#include "audio_engine.h"
#include <cstdio>

namespace SoundShop {

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() { shutdown(); }

void AudioEngine::init() {
    formatManager = std::make_unique<juce::AudioFormatManager>();
    formatManager->registerBasicFormats();

    deviceManager = std::make_unique<juce::AudioDeviceManager>();

    // Try to open default audio device
    // Initialize with both input (for recording, IR capture) and output.
    // Uses the OS default devices.
    auto result = deviceManager->initialiseWithDefaultDevices(1, 2);
    if (result.isNotEmpty()) {
        fprintf(stderr, "Audio device init warning: %s\n", result.toRawUTF8());
        // Continue anyway — might work with different settings
    }

    auto* device = deviceManager->getCurrentAudioDevice();
    if (device) {
        sampleRate = device->getCurrentSampleRate();
        blockSize = device->getCurrentBufferSizeSamples();
        fprintf(stderr, "Audio device: %s (%.0f Hz, %d samples)\n",
                device->getName().toRawUTF8(), sampleRate, blockSize);
    } else {
        fprintf(stderr, "No audio device available\n");
    }

    deviceManager->addAudioCallback(this);

    // Open all available MIDI input devices
    auto midiDevices = juce::MidiInput::getAvailableDevices();
    for (auto& dev : midiDevices) {
        if (deviceManager->isMidiInputDeviceEnabled(dev.identifier))
            continue;
        deviceManager->setMidiInputDeviceEnabled(dev.identifier, true);
        deviceManager->addMidiInputDeviceCallback(dev.identifier, this);
        fprintf(stderr, "MIDI input: %s\n", dev.name.toRawUTF8());
    }
}

void AudioEngine::shutdown() {
    if (deviceManager) {
        deviceManager->removeAudioCallback(this);
        deviceManager->closeAudioDevice();
        deviceManager.reset();
    }
    formatManager.reset();
}

void AudioEngine::setProjectSampleRate(double sr) {
    projectSampleRate = sr;
    double graphRate = getProjectSampleRate();
    if (graph)
        graphProcessor.prepare(*graph, graphRate, blockSize);
    resamplePhase = 0.0;
    fprintf(stderr, "Project sample rate: %.0f Hz (device: %.0f Hz)\n", graphRate, sampleRate);
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    sampleRate = device->getCurrentSampleRate();
    blockSize = device->getCurrentBufferSizeSamples();
    double graphRate = getProjectSampleRate();
    if (graph)
        graphProcessor.prepare(*graph, graphRate, blockSize);
    // Pre-allocate resample buffers (enough for 4x oversampling)
    int maxProjectSamples = blockSize * 4 + 64;
    projectBufL.resize(maxProjectSamples, 0.0f);
    projectBufR.resize(maxProjectSamples, 0.0f);
    resampleBufL.resize(maxProjectSamples, 0.0f);
    resampleBufR.resize(maxProjectSamples, 0.0f);
    resamplePhase = 0.0;
}

void AudioEngine::audioDeviceStopped() {
}

void AudioEngine::audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData,
        int numInputChannels,
        float* const* outputChannelData,
        int numOutputChannels,
        int numSamples,
        const juce::AudioIODeviceCallbackContext& /*context*/) {

    // Clear output
    for (int ch = 0; ch < numOutputChannels; ++ch)
        std::memset(outputChannelData[ch], 0, sizeof(float) * numSamples);

    if (!graph || !transport) return;

    double graphRate = getProjectSampleRate();
    bool needsResample = (std::abs(graphRate - sampleRate) > 1.0);

    // Update transport at project rate
    transport->sampleRate = graphRate;
    transport->positionSamples = positionSamples;

    // Record input if recording (single-track legacy)
    if (recordingManager.isRecording() && numInputChannels > 0)
        recordingManager.processSamples(inputChannelData, numInputChannels, numSamples);

    // Multi-track recording: route each input channel to its assigned track
    if (multitrackRecorder.isRecording() && numInputChannels > 0)
        multitrackRecorder.processSamples(inputChannelData, numInputChannels, numSamples);

    // Grab incoming MIDI from hardware / computer keyboard and route it.
    //
    // Two things happen to each event:
    //  1. processMidiCC applies any user-learned CC mappings to their
    //     target params. That's done first and unconditionally — learned
    //     mappings always fire.
    //  2. The events are forwarded into the currently-active MIDI
    //     timeline (graph.activeEditorNodeId, if it points at one) so
    //     they flow through its MIDI Out pin to whatever synth or plugin
    //     is wired downstream. CC events that are already mapped (step 1)
    //     are excluded from this forward — otherwise the mod wheel would
    //     both move the learned param AND hit the synth's default CC1
    //     vibrato, double-applying. Notes, pitch bend, aftertouch, and
    //     unmapped CCs all pass through.
    {
        juce::MidiBuffer midiCopy;
        {
            const juce::ScopedLock sl(midiLock);
            midiCopy.swapWith(incomingMidi);
        }
        if (!midiCopy.isEmpty()) {
            graphProcessor.getAutomation().processMidiCC(
                midiCopy, *graphProcessor.getGraph(), graphProcessor.getNodeMap());

            // Snapshot mapped (channel, cc) pairs so we can cheaply check
            // each incoming CC event without touching the automation mutex
            // per-event.
            auto ccMappings = graphProcessor.getAutomation().getCCMappings();
            auto isCCMapped = [&](int ch, int cc) {
                for (auto& m : ccMappings)
                    if (m.midiChannel == ch && m.ccNumber == cc) return true;
                return false;
            };

            // Find the active MIDI timeline target.
            Node* activeMidiNode = nullptr;
            if (graph->activeEditorNodeId >= 0) {
                if (auto* n = graph->findNode(graph->activeEditorNodeId))
                    if (n->type == NodeType::MidiTimeline)
                        activeMidiNode = n;
            }

            if (activeMidiNode) {
                std::lock_guard<std::mutex> lock(*activeMidiNode->mpePassthroughMutex);
                for (auto metadata : midiCopy) {
                    auto msg = metadata.getMessage();
                    // Filter already-trained CC events
                    if (msg.isController() &&
                        isCCMapped(msg.getChannel(), msg.getControllerNumber()))
                        continue;
                    activeMidiNode->pendingMpePassthrough.push_back(
                        { metadata.samplePosition, msg });
                }
            }
        }
    }

    if (!needsResample) {
        // Same rate — no resampling needed
        graphProcessor.processBlock(*graph, *transport,
                                     outputChannelData, numOutputChannels, numSamples);
        if (playing.load()) {
            positionSamples += numSamples;
            // Loop: wrap position back to loop start
            if (transport->loopEnabled && transport->loopEndBeat > transport->loopStartBeat) {
                double posBeat = transport->positionBeats();
                if (posBeat >= transport->loopEndBeat) {
                    positionSamples = (int64_t)transport->beatsToSamples(transport->loopStartBeat);
                }
            }
        }
    } else {
        // Process at project rate, then resample to device rate
        double ratio = graphRate / sampleRate;
        int projectSamples = (int)std::ceil(numSamples * ratio) + 2;
        projectSamples = std::min(projectSamples, (int)projectBufL.size());

        // Clear project buffers
        std::memset(projectBufL.data(), 0, projectSamples * sizeof(float));
        std::memset(projectBufR.data(), 0, projectSamples * sizeof(float));

        // Process graph at project rate
        float* projPtrs[2] = {projectBufL.data(), projectBufR.data()};
        graphProcessor.processBlock(*graph, *transport, projPtrs, 2, projectSamples);

        // Resample: linear interpolation from project rate to device rate
        for (int s = 0; s < numSamples; ++s) {
            double srcPos = resamplePhase + s * ratio;
            int idx = (int)srcPos;
            float frac = (float)(srcPos - idx);
            idx = std::min(idx, projectSamples - 2);
            if (idx < 0) idx = 0;

            if (numOutputChannels > 0)
                outputChannelData[0][s] = projectBufL[idx] + frac * (projectBufL[idx + 1] - projectBufL[idx]);
            if (numOutputChannels > 1)
                outputChannelData[1][s] = projectBufR[idx] + frac * (projectBufR[idx + 1] - projectBufR[idx]);
        }

        // Track fractional sample position for seamless continuation
        resamplePhase = std::fmod(resamplePhase + numSamples * ratio, (double)projectSamples);

        if (playing.load())
            positionSamples += projectSamples; // advance at project rate
            // Loop wrap
            if (transport->loopEnabled && transport->loopEndBeat > transport->loopStartBeat) {
                double posBeat = transport->positionBeats();
                if (posBeat >= transport->loopEndBeat)
                    positionSamples = (int64_t)transport->beatsToSamples(transport->loopStartBeat);
            }
    }

    // Input monitoring: mix audio input directly into output (global toggle)
    if (inputMonitoring.load() && numInputChannels > 0) {
        int chToCopy = std::min(numInputChannels, numOutputChannels);
        for (int ch = 0; ch < chToCopy; ++ch)
            for (int s = 0; s < numSamples; ++s)
                outputChannelData[ch][s] += inputChannelData[ch][s];
    }

    // Per-track input monitoring: mix each track's assigned input into output
    if (graph && numInputChannels > 0)
        multitrackRecorder.processMonitoring(inputChannelData, numInputChannels,
                                              outputChannelData, numOutputChannels,
                                              numSamples, *graph);

    // Always-on output capture: every playback is captured so the Output
    // node's cache can be populated on Stop. The buffer auto-clears when a
    // new playback starts (captureL is empty → fresh start). Only appends
    // while the transport is playing, so there's no dead space.
    {
        bool isPlaying = playing.load();
        if (isPlaying) {
            if (captureL.empty()) {
                captureSampleRate = getSampleRate();
                captureStartSample = positionSamples;
                size_t reserve = (size_t)(captureSampleRate * 60.0);
                captureL.reserve(reserve);
                captureR.reserve(reserve);
            }
            for (int s = 0; s < numSamples; ++s) {
                captureL.push_back(numOutputChannels > 0 ? outputChannelData[0][s] : 0.0f);
                captureR.push_back(numOutputChannels > 1 ? outputChannelData[1][s] : 0.0f);
            }
        }
    }
}

void AudioEngine::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& msg) {
    // Hotkey MIDI capture: forward to the hotkey settings dialog if active.
    // Only capture intentional button presses:
    //   - Note On with velocity > 0 (pad/key press)
    //   - CC with value == 127 (standard button-press value)
    // Ignore gradual CC values (knobs, faders, noisy pots that drift).
    if (hotkeyMidiCapture) {
        if (msg.isNoteOn() && msg.getVelocity() > 0)
            hotkeyMidiCapture(0, msg.getChannel() - 1, msg.getNoteNumber());
        else if (msg.isController() && msg.getControllerValue() == 127)
            hotkeyMidiCapture(1, msg.getChannel() - 1, msg.getControllerNumber());
        // Don't consume non-captured MIDI (let knob noise pass through harmlessly)
        if (msg.isNoteOn() || (msg.isController() && msg.getControllerValue() == 127))
            return;
    }

    // Capture for MIDI Learn
    if (msg.isController() && midiLearn.active.load()) {
        midiLearn.lastCC.store(msg.getControllerNumber());
        midiLearn.lastChannel.store(msg.getChannel());
    }

    // MIDI note recording: capture notes into the target MIDI track
    if (midiRecord.targetNodeId >= 0 && transport && playing.load()) {
        std::lock_guard<std::mutex> lock(midiRecord.mutex);
        double currentBeat = transport->positionBeats();

        if (msg.isNoteOn()) {
            midiRecord.activeNotes.push_back({msg.getNoteNumber(), msg.getVelocity(), currentBeat});
        } else if (msg.isNoteOff()) {
            for (int i = (int)midiRecord.activeNotes.size() - 1; i >= 0; --i) {
                auto& an = midiRecord.activeNotes[i];
                if (an.pitch == msg.getNoteNumber()) {
                    if (graph) {
                        auto* node = graph->findNode(midiRecord.targetNodeId);
                        if (node && !node->clips.empty()) {
                            auto& clip = node->clips[0];
                            MidiNote nn;
                            nn.offset = (float)(an.startBeat - clip.startBeat);
                            nn.pitch = an.pitch;
                            nn.velocity = an.velocity;
                            nn.duration = std::max(0.01f, (float)(currentBeat - an.startBeat));
                            nn.octave = an.pitch / 12;
                            clip.notes.push_back(nn);
                            float noteEnd = nn.offset + nn.duration;
                            if (noteEnd > clip.lengthBeats)
                                clip.lengthBeats = std::ceil(noteEnd / 4.0f) * 4.0f;
                            graph->dirty = true;
                        }
                    }
                    midiRecord.activeNotes.erase(midiRecord.activeNotes.begin() + i);
                    break;
                }
            }
        }
    }

    // MPE recording: capture per-note expression on channels 2-16
    int ch = msg.getChannel();
    if (mpeRecordTargetNodeId >= 0 && ch >= 2 && ch <= 16 && transport) {
        std::lock_guard<std::mutex> lock(mpeMutex);
        double currentBeat = transport->positionBeats();

        if (msg.isNoteOn()) {
            MpeRecordNote rn;
            rn.channel = ch;
            rn.pitch = msg.getNoteNumber();
            rn.velocity = msg.getVelocity();
            rn.startBeat = currentBeat;
            mpeActiveNotes.push_back(rn);
        } else if (msg.isNoteOff()) {
            // Finalize the note and add it to the target node
            for (int i = (int)mpeActiveNotes.size() - 1; i >= 0; --i) {
                auto& rn = mpeActiveNotes[i];
                if (rn.channel == ch && rn.pitch == msg.getNoteNumber()) {
                    if (graph) {
                        auto* node = graph->findNode(mpeRecordTargetNodeId);
                        if (node && !node->clips.empty()) {
                            auto& clip = node->clips[0];
                            MidiNote nn;
                            nn.offset = (float)(rn.startBeat - clip.startBeat);
                            nn.pitch = rn.pitch;
                            nn.duration = (float)(currentBeat - rn.startBeat);
                            nn.expression.pitchBend = std::move(rn.pitchBend);
                            nn.expression.slide = std::move(rn.slide);
                            nn.expression.pressure = std::move(rn.pressure);
                            clip.notes.push_back(nn);
                            // Extend clip if needed
                            float noteEnd = nn.offset + nn.duration;
                            if (noteEnd > clip.lengthBeats)
                                clip.lengthBeats = std::ceil(noteEnd / 4.0f) * 4.0f;
                            graph->dirty = true;
                        }
                    }
                    mpeActiveNotes.erase(mpeActiveNotes.begin() + i);
                    break;
                }
            }
        } else if (msg.isPitchWheel()) {
            for (auto& rn : mpeActiveNotes) {
                if (rn.channel == ch) {
                    float val = msg.getPitchWheelValue() / 16383.0f;
                    float time = (float)(currentBeat - rn.startBeat);
                    rn.pitchBend.push_back({time, val});
                }
            }
        } else if (msg.isController() && msg.getControllerNumber() == 74) {
            for (auto& rn : mpeActiveNotes) {
                if (rn.channel == ch) {
                    float val = msg.getControllerValue() / 127.0f;
                    float time = (float)(currentBeat - rn.startBeat);
                    rn.slide.push_back({time, val});
                }
            }
        } else if (msg.isChannelPressure()) {
            for (auto& rn : mpeActiveNotes) {
                if (rn.channel == ch) {
                    float val = msg.getChannelPressureValue() / 127.0f;
                    float time = (float)(currentBeat - rn.startBeat);
                    rn.pressure.push_back({time, val});
                }
            }
        }
    }

    // Buffer for audio thread (pass-through to graph)
    const juce::ScopedLock sl(midiLock);
    incomingMidi.addEvent(msg, 0);
}

void AudioEngine::recordParamChange(int nodeId, int paramIdx, float value) {
    if (!autoRecord.armed || !transport || !playing.load() || !graph) return;

    auto* node = graph->findNode(nodeId);
    if (!node || paramIdx < 0 || paramIdx >= (int)node->params.size()) return;

    float beat = (float)transport->positionBeats();
    auto& lane = node->params[paramIdx].automation;

    // Add point, replacing any nearby existing point (within 0.05 beats)
    for (auto& pt : lane.points) {
        if (std::abs(pt.beat - beat) < 0.05f) {
            pt.value = value;
            return;
        }
    }
    lane.points.push_back({beat, value});

    // Keep sorted
    std::sort(lane.points.begin(), lane.points.end(),
        [](auto& a, auto& b) { return a.beat < b.beat; });
    graph->dirty = true;
}

// ==============================================================================
// Computer Keyboard MIDI ("Musical Typing")
// ==============================================================================

void AudioEngine::keyboardNoteOn(int midiNote, int velocity) {
    if (midiNote < 0 || midiNote > 127) return;
    auto msg = juce::MidiMessage::noteOn(1, midiNote, (juce::uint8)velocity);
    const juce::ScopedLock sl(midiLock);
    incomingMidi.addEvent(msg, 0);
}

void AudioEngine::keyboardNoteOff(int midiNote) {
    if (midiNote < 0 || midiNote > 127) return;
    auto msg = juce::MidiMessage::noteOff(1, midiNote);
    const juce::ScopedLock sl(midiLock);
    incomingMidi.addEvent(msg, 0);
}

// ==============================================================================
// Output Capture — record the final mix to memory
// ==============================================================================

void AudioEngine::stop() {
    playing = false;
    // Flush the captured audio into the Output node's cache so the user
    // (or the Capture button) can access it without re-rendering. This
    // runs on the UI thread after the audio thread has seen playing=false
    // and stopped appending to the capture buffers.
    if (graph && !captureL.empty()) {
        for (auto& n : graph->nodes) {
            if (n.type == NodeType::Output) {
                n.cache.left = std::move(captureL);
                n.cache.right = std::move(captureR);
                n.cache.sampleRate = captureSampleRate;
                n.cache.startSample = captureStartSample;
                n.cache.numSamples = (int64_t)n.cache.left.size();
                n.cache.valid = true;
                break;
            }
        }
    }
    captureL.clear();
    captureR.clear();
    positionSamples = 0;
}

void AudioEngine::startOutputCapture() {
    // Arm capture — actual recording starts when transport plays.
    // Clear previous capture so the next Play→Stop produces a clean buffer.
    captureL.clear();
    captureR.clear();
    // Reserve ~60 seconds to reduce audio-thread reallocs.
    size_t reserve = (size_t)(getSampleRate() * 60.0);
    captureL.reserve(reserve);
    captureR.reserve(reserve);
    outputCaptureEnabled = true;
}

void AudioEngine::stopOutputCapture() {
    outputCaptureEnabled = false;
}

juce::AudioBuffer<float> AudioEngine::getCaptureBuffer() const {
    int n = (int)captureL.size();
    if (n == 0) return {};
    juce::AudioBuffer<float> buf(2, n);
    buf.copyFrom(0, 0, captureL.data(), n);
    if ((int)captureR.size() >= n)
        buf.copyFrom(1, 0, captureR.data(), n);
    else
        buf.clear(1, 0, n);
    return buf;
}

void AudioEngine::clearCapture() {
    captureL.clear();
    captureR.clear();
}

} // namespace SoundShop

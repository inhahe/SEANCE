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

    // Note: we intentionally do NOT auto-enable MIDI input devices here.
    // The node-based input architecture means MIDI devices are enabled only
    // if a matching MidiInput node exists in the current graph. Enablement
    // is driven by syncMidiDeviceEnablement(), which is called after the
    // graph is loaded/created (see AudioEngine::setGraph).
    fprintf(stderr, "Detected MIDI inputs:\n");
    for (auto& dev : juce::MidiInput::getAvailableDevices())
        fprintf(stderr, "  %s\n", dev.name.toRawUTF8());
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

void AudioEngine::syncMidiDeviceEnablement() {
    if (!deviceManager || !graph) return;

    // Collect the set of device identifiers the current graph wants active.
    std::set<juce::String> wanted;
    for (auto& n : graph->nodes) {
        if (n.type != NodeType::MidiInput) continue;
        // "keyboard" is the internal computer-keyboard source; not a real
        // hardware device, so skip it here.
        if (n.midiInputSourceId == "keyboard") continue;
        if (!n.midiInputSourceId.empty())
            wanted.insert(juce::String(n.midiInputSourceId));
    }

    // Enable wanted devices, disable anything else that's currently on.
    for (auto& dev : juce::MidiInput::getAvailableDevices()) {
        bool shouldEnable = wanted.count(dev.identifier) > 0;
        bool isEnabled = deviceManager->isMidiInputDeviceEnabled(dev.identifier);
        if (shouldEnable && !isEnabled) {
            deviceManager->setMidiInputDeviceEnabled(dev.identifier, true);
            deviceManager->addMidiInputDeviceCallback(dev.identifier, this);
            fprintf(stderr, "MIDI input enabled: %s\n", dev.name.toRawUTF8());
        } else if (!shouldEnable && isEnabled) {
            deviceManager->removeMidiInputDeviceCallback(dev.identifier, this);
            deviceManager->setMidiInputDeviceEnabled(dev.identifier, false);
            fprintf(stderr, "MIDI input disabled: %s\n", dev.name.toRawUTF8());
        }
    }
}

std::vector<AudioEngine::MidiDeviceEntry> AudioEngine::listMidiInputDevices() const {
    std::vector<MidiDeviceEntry> out;
    auto devices = juce::MidiInput::getAvailableDevices();
    for (auto& dev : devices) {
        MidiDeviceEntry e;
        e.name = dev.name;
        e.identifier = dev.identifier;
        e.presentInGraph = false;
        if (graph) {
            for (auto& n : graph->nodes) {
                if (n.type == NodeType::MidiInput &&
                    n.midiInputSourceId == dev.identifier.toStdString()) {
                    e.presentInGraph = true;
                    break;
                }
            }
        }
        out.push_back(std::move(e));
    }
    return out;
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

    // Grab incoming hardware MIDI events (with per-event source identifier)
    // and route each one to the matching MidiInput node in the graph.
    //
    //  1. Apply any user-learned CC mappings to their target params — always
    //     runs regardless of routing (via processMidiCC on a MidiBuffer copy).
    //  2. For each event, find the MidiInput node whose midiInputSourceId
    //     matches the source device's identifier and push the event into
    //     its queue. Already-mapped CCs are filtered out so they don't
    //     double-apply (learned target + synth default handler).
    //  3. Events with no matching node are dropped (user hasn't added an
    //     Input node for that device yet).
    {
        std::vector<std::pair<juce::String, juce::MidiMessage>> events;
        {
            const juce::ScopedLock sl(midiLock);
            events.swap(incomingMidiEvents);
        }
        if (!events.empty()) {
            // Apply CC mappings via the existing buffer-based helper.
            juce::MidiBuffer ccBuf;
            for (auto& [id, msg] : events) ccBuf.addEvent(msg, 0);
            graphProcessor.getAutomation().processMidiCC(
                ccBuf, *graphProcessor.getGraph(), graphProcessor.getNodeMap());

            auto ccMappings = graphProcessor.getAutomation().getCCMappings();
            auto isCCMapped = [&](int ch, int cc) {
                for (auto& m : ccMappings)
                    if (m.midiChannel == ch && m.ccNumber == cc) return true;
                return false;
            };

            for (auto& [id, msg] : events) {
                // Skip trained CCs — they're already handled by the CC
                // mapping pass above; forwarding them would double-apply.
                if (msg.isController() &&
                    isCCMapped(msg.getChannel(), msg.getControllerNumber()))
                    continue;

                // Find the MidiInput node matching this source device
                auto sourceIdStd = id.toStdString();
                for (auto& n : graph->nodes) {
                    if (n.type == NodeType::MidiInput &&
                        n.midiInputSourceId == sourceIdStd) {
                        std::lock_guard<std::mutex> lock(*n.mpePassthroughMutex);
                        n.pendingMpePassthrough.push_back({0, msg});
                        break;
                    }
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

void AudioEngine::handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& msg) {
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

    // Buffer the message along with its source device identifier so the
    // audio thread can route it to the matching MidiInput node safely.
    // We can't iterate graph->nodes from the MIDI thread — the graph may
    // be mutated from the UI/audio thread concurrently.
    if (source) {
        const juce::ScopedLock sl(midiLock);
        incomingMidiEvents.emplace_back(source->getIdentifier(), msg);
    }
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

// Find the "Computer Keyboard" MidiInput node in the graph, if any. Used to
// push typing-MIDI events into its per-node queue so they flow out through
// the cable wired from the node — matching the Phase 1 architecture where
// routing is done purely via graph cables, not flags. Returns nullptr if
// no such node exists (e.g. on an old project that predates the MidiInput
// node type — typing events will be silently dropped in that case, and
// loading the project offers an upgrade path by adding the node manually).
static Node* findComputerKeyboardInputNode(NodeGraph* g) {
    if (!g) return nullptr;
    for (auto& n : g->nodes)
        if (n.type == NodeType::MidiInput && n.midiInputSourceId == "keyboard")
            return &n;
    return nullptr;
}

void AudioEngine::keyboardNoteOn(int midiNote, int velocity) {
    if (midiNote < 0 || midiNote > 127) return;
    auto* kb = findComputerKeyboardInputNode(graph);
    if (!kb) return;
    auto msg = juce::MidiMessage::noteOn(1, midiNote, (juce::uint8)velocity);
    std::lock_guard<std::mutex> lock(*kb->mpePassthroughMutex);
    kb->pendingMpePassthrough.push_back({0, msg});
}

void AudioEngine::keyboardNoteOff(int midiNote) {
    if (midiNote < 0 || midiNote > 127) return;
    auto* kb = findComputerKeyboardInputNode(graph);
    if (!kb) return;
    auto msg = juce::MidiMessage::noteOff(1, midiNote);
    std::lock_guard<std::mutex> lock(*kb->mpePassthroughMutex);
    kb->pendingMpePassthrough.push_back({0, msg});
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

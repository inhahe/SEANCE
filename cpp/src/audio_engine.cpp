#include "audio_engine.h"
#include "audio_cache.h"
#include <cstdio>
#include <algorithm>

namespace SoundShop {

// Defined below shutdown(); forward-declared so init() can reference it.
static juce::File getAudioSettingsFile();

double AudioEngine::computeMaxGraphTailSeconds() const {
    if (!graph) return 0.0;
    double maxTail = 0.0;
    auto& gp = const_cast<GraphProcessor&>(graphProcessor);
    for (const auto& n : graph->nodes) {
        if (auto* p = gp.getProcessorForNode(n.id)) {
            double t = p->getTailLengthSeconds();
            if (t > maxTail) maxTail = t;
        }
    }
    return maxTail;
}

AudioEngine* AudioEngine::sInstance = nullptr;

AudioEngine::AudioEngine() {
    // Last-one-wins on the off chance two are ever constructed (shouldn't
    // happen in production - MainWindow holds the only AudioEngine - but
    // this keeps the invariant simple to reason about). Destructor below
    // clears the slot only if we're still the registered instance, so
    // teardown of a transient engine in a test won't null out a real one.
    sInstance = this;
}
AudioEngine::~AudioEngine() {
    shutdown();
    if (sInstance == this) sInstance = nullptr;
}

void AudioEngine::init() {
    formatManager = std::make_unique<juce::AudioFormatManager>();
    formatManager->registerBasicFormats();

    deviceManager = std::make_unique<juce::AudioDeviceManager>();

    // --- Restore the user's saved audio device choice, or pick a sane
    // first-run default. ------------------------------------------------------
    // SEANCE persists the full device setup (driver type, device names, sample
    // rate, buffer size, channel selection) to a settings file next to the exe
    // so the choice survives restarts. Two paths:
    //
    //   (a) Settings file exists  -> hand the saved XML to initialise() so the
    //       exact device/type the user last selected is restored. We never
    //       override an explicit saved choice.
    //
    //   (b) First run (no file)   -> default the driver type to DirectSound.
    //       JUCE's out-of-the-box default is WASAPI, which on Windows welds the
    //       default input (e.g. a USB webcam mic) and the default output (e.g.
    //       HDMI) into a single shared-mode device. When those are different
    //       physical devices with independent clocks, WASAPI's combined device
    //       corrupts the captured input into a square-wave-like garbage signal
    //       (a known JUCE limitation). DirectSound buffers the two endpoints
    //       independently and captures the mic correctly. We only set this as
    //       the *first-run* default - the saved-settings path above takes
    //       precedence, so a user who deliberately switches back to WASAPI in
    //       the Audio Device Settings dialog keeps WASAPI on subsequent runs.
    std::unique_ptr<juce::XmlElement> savedState;
    {
        auto settingsFile = getAudioSettingsFile();
        if (settingsFile.existsAsFile())
            savedState = juce::parseXML(settingsFile);
    }

    if (savedState == nullptr) {
        // First run: prefer the DirectSound driver type if it's available.
        for (auto* type : deviceManager->getAvailableDeviceTypes()) {
            if (type->getTypeName() == "DirectSound") {
                deviceManager->setCurrentAudioDeviceType("DirectSound", true);
                break;
            }
        }
    }

    // Initialize with both input (for recording, IR capture) and output.
    // savedState restores the user's last choice if present; otherwise JUCE
    // opens the default devices for the current (possibly DirectSound) type.
    auto result = deviceManager->initialise(
        /*numInputChannelsNeeded*/  1,
        /*numOutputChannelsNeeded*/ 2,
        /*savedState*/              savedState.get(),
        /*selectDefaultDeviceOnFailure*/ true);
    if (result.isNotEmpty()) {
        fprintf(stderr, "Audio device init warning: %s\n", result.toRawUTF8());
        // Continue anyway - might work with different settings
    }

    // Make sure an input device is actually open with a live channel
    // (Windows default-device gap - see ensureAudioInputEnabled).
    ensureAudioInputEnabled();

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

    // Persist whatever ensureAudioInputEnabled() landed on so the very first
    // run writes a settings file (subsequent runs restore it). Then listen for
    // device changes so any pick the user makes in the Audio Device Settings
    // dialog is re-persisted automatically.
    saveAudioSettings();
    deviceManager->addChangeListener(this);

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
        // Capture the final device state before tearing down so the user's
        // last-used configuration is what gets restored next launch.
        saveAudioSettings();
        deviceManager->removeChangeListener(this);
        deviceManager->removeAudioCallback(this);
        deviceManager->closeAudioDevice();
        deviceManager.reset();
    }
    formatManager.reset();
}

// Settings file lives next to the executable, matching the convention used by
// the other SEANCE config files (soundshop_recent_projects.txt,
// soundshop_prefs.xml, etc.).
static juce::File getAudioSettingsFile() {
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile)
               .getSiblingFile("soundshop_audio_settings.xml");
}

void AudioEngine::saveAudioSettings() {
    if (!deviceManager) return;
    if (auto xml = deviceManager->createStateXml())
        xml->writeTo(getAudioSettingsFile());
    // createStateXml() returns nullptr when the current setup is identical to
    // the default - in that case there's nothing user-specific to persist and
    // leaving any existing file untouched is fine.
}

void AudioEngine::changeListenerCallback(juce::ChangeBroadcaster* source) {
    if (deviceManager && source == deviceManager.get())
        saveAudioSettings();
}

bool AudioEngine::ensureAudioInputEnabled() {
    if (!deviceManager) return false;
    auto setup = deviceManager->getAudioDeviceSetup();

    // Already have a live input channel and a named input device: nothing to
    // do. Bail before setAudioDeviceSetup so opening the mic dialog doesn't
    // needlessly restart the device (which would glitch audio) on the common
    // path where input is fine.
    if (!setup.inputChannels.isZero() && setup.inputDeviceName.isNotEmpty())
        return true;

    if (auto* type = deviceManager->getCurrentDeviceTypeObject()) {
        type->scanForDevices();
        const auto inputs = type->getDeviceNames(true);  // input device names
        if (setup.inputDeviceName.isEmpty() && inputs.size() > 0) {
            const int defIdx = type->getDefaultDeviceIndex(true);
            setup.inputDeviceName = inputs[juce::jlimit(0, inputs.size() - 1, defIdx)];
        }
    }
    if (setup.inputDeviceName.isEmpty()) {
        fprintf(stderr, "No audio input device available to enable\n");
        return false;
    }
    setup.useDefaultInputChannels = true;  // open the device's default inputs
    setup.inputChannels.setBit(0);          // and guarantee at least channel 0
    auto err = deviceManager->setAudioDeviceSetup(setup, true);
    if (err.isNotEmpty()) {
        fprintf(stderr, "Could not enable audio input: %s\n", err.toRawUTF8());
        return false;
    }
    fprintf(stderr, "Audio input enabled: %s\n", setup.inputDeviceName.toRawUTF8());
    return true;
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
    auditionMonL.resize(maxProjectSamples, 0.0f);
    auditionMonR.resize(maxProjectSamples, 0.0f);
    auditionMonLen = 0;
    resamplePhase = 0.0;

    // Allocate the playback-tap ring buffer on the first device start.
    // Stays allocated for the app lifetime - the audio thread only ever
    // writes into a fixed-size, pre-allocated buffer, never reallocates.
    if (playbackTapBuf.size() != (size_t)kPlaybackTapBufSize) {
        playbackTapBuf.assign((size_t)kPlaybackTapBufSize, 0.0f);
        playbackTapWritePos.store(0, std::memory_order_release);
    }
    playbackTapSampleRate.store(sampleRate, std::memory_order_release);

    // Same treatment for the mic-tap ring buffer (read side of the audio
    // callback). Allocated once, reset signal-detected flag so plugging
    // in a different input device re-evaluates whether any signal is
    // actually arriving.
    if (micTapBuf.size() != (size_t)kMicTapBufSize) {
        micTapBuf.assign((size_t)kMicTapBufSize, 0.0f);
        micTapWritePos.store(0, std::memory_order_release);
    }
    micTapSampleRate.store(sampleRate, std::memory_order_release);
    micTapHasSignal.store(false, std::memory_order_release);
}

void AudioEngine::audioDeviceStopped() {
}

// ---- Audition-monitor bus ----
// See audio_engine.h. All three run on the audio thread; clear/mix bracket the
// graph process, addAuditionMonitor runs inside it. No locking by construction.
void AudioEngine::clearAuditionMonitor(int n) {
    auditionMonLen = std::min(n, (int)auditionMonL.size());
    if (auditionMonLen > 0) {
        std::memset(auditionMonL.data(), 0, auditionMonLen * sizeof(float));
        std::memset(auditionMonR.data(), 0, auditionMonLen * sizeof(float));
    }
}

void AudioEngine::addAuditionMonitor(const float* mono, int n) {
    if (!mono) return;
    const int lim = std::min(n, auditionMonLen);
    for (int s = 0; s < lim; ++s) {
        auditionMonL[s] += mono[s];
        auditionMonR[s] += mono[s];
    }
}

void AudioEngine::mixAuditionMonitorInto(float* left, float* right, int n) const {
    const int lim = std::min(n, auditionMonLen);
    for (int s = 0; s < lim; ++s) {
        if (left)  left[s]  += auditionMonL[s];
        if (right) right[s] += auditionMonR[s];
    }
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

    // Take the graph mutation lock for this entire callback. A non-blocking
    // try_lock is used so we don't stall the audio device while another
    // thread is mid-mutation - e.g. MOD import (~100ms) pushing ~10 new
    // nodes into graph.nodes. A previous tracker-import crash dump
    // (SEANCE.exe.63000.dmp) was traced to this race: the audio callback
    // iterating graph.nodes for both per-node MIDI routing and
    // GraphProcessor::rebuildGraph, while mod_import.cpp simultaneously
    // pushed new entries (occasionally triggering vector reallocation),
    // resulted in torn reads of Node::id (observed 0 and 1132382734 in
    // the rebuilt nodeMap) and a crash downstream in the JUCE graph
    // wiring. Holding the lock here pairs with std::lock_guard in
    // batch-mutators (mod_import callsite, project file load, undo
    // restore - to be added at those sites). If we can't acquire
    // immediately the block is silent, which is barely audible against
    // a multi-second import and dramatically better than a crash.
    std::unique_lock<std::mutex> graphLk(graph->mutationLock, std::try_to_lock);
    if (!graphLk.owns_lock()) return;

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
    //  1. Apply any user-learned CC mappings to their target params - always
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
                // Skip trained CCs - they're already handled by the CC
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

    // Apply the project-wide Song Length + Song Repeat policy. The user-
    // region loop above takes precedence (it's an inner A-B cycler - the
    // user's immediate intent to cycle a section), so this branch only
    // runs when no user loop was applied during this block.
    //
    // For SongRepeat::None and the final iteration of ::NTimes, we don't
    // halt immediately - that would cut reverbs, delays, and release
    // tails.  Instead we enter a "tail grace" countdown: keep processing
    // (the playhead stays at the song end, no new clips trigger) until
    // the max per-node tail has elapsed, then truly stop.
    auto applySongRepeat = [&](int blockSamples) {
        if (!graph || !playing.load()) return;
        // effectiveSongLengthBeats() returns the explicit songLengthBeats if
        // set, else auto-derives from the latest clip across all timeline
        // nodes. 0 means "no end" - project has no clips, just play until
        // the user hits Stop.
        double endBeat = graph->effectiveSongLengthBeats();
        if (endBeat <= 0) return;

        // If we're already inside tail grace, just tick the countdown.
        if (endTailSamplesRemaining >= 0) {
            endTailSamplesRemaining -= blockSamples;
            // Hold the playhead at the song end so no new content triggers.
            positionSamples = (int64_t)transport->beatsToSamples(endBeat);
            if (endTailSamplesRemaining <= 0) {
                playing = false;
                endTailSamplesRemaining = -1;
            }
            return;
        }

        double posBeat = transport->positionBeats();
        if (posBeat < endBeat) return;

        auto enterTailGrace = [&]() {
            // Hold the playhead at the song-end beat; start the countdown.
            positionSamples = (int64_t)transport->beatsToSamples(endBeat);
            double tailSec = computeMaxGraphTailSeconds();
            endTailSamplesRemaining = (int64_t)(tailSec * transport->sampleRate);
            if (endTailSamplesRemaining <= 0) {
                // No tail anywhere in the graph - halt now.
                playing = false;
                endTailSamplesRemaining = -1;
            }
        };

        switch (graph->songRepeatMode) {
            case NodeGraph::SongRepeat::None:
                enterTailGrace();
                break;
            case NodeGraph::SongRepeat::Forever:
                positionSamples = 0;
                ++songPlayCount;
                break;
            case NodeGraph::SongRepeat::NTimes:
                ++songPlayCount;
                // songRepeatCount is "play N times total". We just reached
                // the end of iteration songPlayCount; if we've hit the
                // total, enter tail grace; otherwise wrap.
                if (songPlayCount >= std::max(1, graph->songRepeatCount))
                    enterTailGrace();
                else
                    positionSamples = 0;
                break;
        }
    };

    if (!needsResample) {
        // Same rate - no resampling needed. Clear the audition-monitor bus
        // before the graph runs (synth nodes accumulate into it during
        // processBlock), then sum it onto the graph output afterwards.
        clearAuditionMonitor(numSamples);
        graphProcessor.processBlock(*graph, *transport,
                                     outputChannelData, numOutputChannels, numSamples);
        mixAuditionMonitorInto(numOutputChannels > 0 ? outputChannelData[0] : nullptr,
                               numOutputChannels > 1 ? outputChannelData[1] : nullptr,
                               numSamples);
        if (playing.load()) {
            positionSamples += numSamples;
            // Loop: wrap position back to loop start
            bool userLoopApplied = false;
            if (transport->loopEnabled && transport->loopEndBeat > transport->loopStartBeat) {
                double posBeat = transport->positionBeats();
                if (posBeat >= transport->loopEndBeat) {
                    positionSamples = (int64_t)transport->beatsToSamples(transport->loopStartBeat);
                    userLoopApplied = true;
                }
            }
            if (!userLoopApplied) applySongRepeat(numSamples);
        }
    } else {
        // Process at project rate, then resample to device rate
        double ratio = graphRate / sampleRate;
        int projectSamples = (int)std::ceil(numSamples * ratio) + 2;
        projectSamples = std::min(projectSamples, (int)projectBufL.size());

        // Clear project buffers
        std::memset(projectBufL.data(), 0, projectSamples * sizeof(float));
        std::memset(projectBufR.data(), 0, projectSamples * sizeof(float));

        // Process graph at project rate. Clear the audition-monitor bus first
        // (synth nodes accumulate into it during processBlock) and sum it into
        // the project buffers afterwards so it resamples along with the graph
        // output below.
        clearAuditionMonitor(projectSamples);
        float* projPtrs[2] = {projectBufL.data(), projectBufR.data()};
        graphProcessor.processBlock(*graph, *transport, projPtrs, 2, projectSamples);
        mixAuditionMonitorInto(projectBufL.data(), projectBufR.data(), projectSamples);

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

        if (playing.load()) {
            positionSamples += projectSamples; // advance at project rate
            // Loop wrap
            bool userLoopApplied = false;
            if (transport->loopEnabled && transport->loopEndBeat > transport->loopStartBeat) {
                double posBeat = transport->positionBeats();
                if (posBeat >= transport->loopEndBeat) {
                    positionSamples = (int64_t)transport->beatsToSamples(transport->loopStartBeat);
                    userLoopApplied = true;
                }
            }
            if (!userLoopApplied) applySongRepeat(projectSamples);
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

    // Spectrum analyzer ring buffer (#10): write latest output samples.
    for (int s = 0; s < numSamples; ++s) {
        spectrumBufL[spectrumWritePos] = (numOutputChannels > 0) ? outputChannelData[0][s] : 0.0f;
        spectrumBufR[spectrumWritePos] = (numOutputChannels > 1) ? outputChannelData[1][s] : 0.0f;
        spectrumWritePos = (spectrumWritePos + 1) % kSpectrumBufSize;
    }

    // Master Out peak metering. Output nodes don't get a PanProcessor in
    // the graph (graph_processor.cpp ~604 guards on `type != Output`), so
    // the per-node meterPeakL/R never get written for them and their
    // green meter bar stays dark forever. Without a meter on Master Out
    // the user has no way to tell "audio reached the output but the
    // device is silent" apart from "audio never reached the output" -
    // both look like dead silence. Scan the final device buffer here and
    // stamp every Output node so the UI's existing meter-draw path
    // (node_graph_component.cpp) lights them up consistently with every
    // other node. Same lock-free pattern as PanProcessor: torn reads on
    // the UI thread are tolerated (one frame of stale meter).
    {
        float pkL = 0, pkR = 0;
        if (numOutputChannels > 0) {
            auto* d = outputChannelData[0];
            for (int s = 0; s < numSamples; ++s) pkL = std::max(pkL, std::abs(d[s]));
        }
        if (numOutputChannels > 1) {
            auto* d = outputChannelData[1];
            for (int s = 0; s < numSamples; ++s) pkR = std::max(pkR, std::abs(d[s]));
        } else {
            pkR = pkL;
        }
        for (auto& n : graph->nodes) {
            if (n.type == NodeType::Output) {
                n.meterPeakL = pkL;
                n.meterPeakR = pkR;
            }
        }
    }

    // Wavetable capture-from-playback ring buffer: mono-summed final mix,
    // always-on, used by the +Frame -> "From recent playback" dialog. Single
    // writer (this audio callback), single reader (UI thread snapshot).
    // No lock - the reader tolerates torn reads since worst case is a
    // visual glitch on the dialog's waveform display.
    if (playbackTapBuf.size() == (size_t)kPlaybackTapBufSize) {
        int wp = playbackTapWritePos.load(std::memory_order_relaxed);
        for (int s = 0; s < numSamples; ++s) {
            float l = (numOutputChannels > 0) ? outputChannelData[0][s] : 0.0f;
            float r = (numOutputChannels > 1) ? outputChannelData[1][s] : l;
            playbackTapBuf[(size_t)wp] = 0.5f * (l + r);
            wp = (wp + 1) & (kPlaybackTapBufSize - 1);  // pow-2 wrap
        }
        playbackTapWritePos.store(wp, std::memory_order_release);
    }

    // Mic-tap: same shape as the playback-tap above, but it reads from
    // inputChannelData. Mono-sums every input channel (most setups have
    // 1 input mic but stereo inputs e.g. line-in still get summed for
    // a single-cycle frame source - users who need stereo capture
    // should use audio tracks). Drives the +Frame -> "From microphone"
    // capture dialog. Also latches micTapHasSignal so the dialog can
    // tell a silent mic apart from a missing one.
    if (micTapBuf.size() == (size_t)kMicTapBufSize && numInputChannels > 0) {
        int wp = micTapWritePos.load(std::memory_order_relaxed);
        const float invCh = 1.0f / (float)numInputChannels;
        bool sawSignal = false;
        for (int s = 0; s < numSamples; ++s) {
            float sum = 0.0f;
            for (int ch = 0; ch < numInputChannels; ++ch)
                sum += inputChannelData[ch][s];
            const float v = sum * invCh;
            micTapBuf[(size_t)wp] = v;
            wp = (wp + 1) & (kMicTapBufSize - 1);
            if (std::abs(v) > 1.0e-5f) sawSignal = true;
        }
        micTapWritePos.store(wp, std::memory_order_release);
        if (sawSignal && !micTapHasSignal.load(std::memory_order_relaxed))
            micTapHasSignal.store(true, std::memory_order_release);
    }

    // Capture-from-project preview audio (#capV2). Mixes on top of the
    // graph output so the dialog can audition the pre-rendered song or a
    // marker-position granular drone while picking the capture spot.
    // SongPlay reads the rendered project PCM linearly. GrainLoop runs an
    // overlap-add stream of 4 Hann-windowed grains (envelope length N =
    // previewGrainLenSamples) with jittered source-start positions inside
    // the longer source buffer the UI publishes - this is proper granular
    // synthesis, NOT a clip on loop, so the audition is a continuous
    // drone without a visible seam. On mode changes we run a short
    // linear crossfade between the outgoing and incoming modes so
    // transport state transitions don't click.
    {
        const int requestedMode = previewMode.load(std::memory_order_acquire);

        // Observe a mode change once (this block runs on the audio thread
        // only, so currAppliedPreviewMode is single-writer). When we see
        // a new request we move the current mode into "outgoing" and
        // start the fade. SongPlay always restarts reading at the latest
        // UI-published marker; GrainLoop re-staggers voice phases so the
        // OLA stream begins fresh (incoming voices) while the outgoing
        // grain stream's current voice state keeps producing the dying
        // tail through the crossfade.
        if (requestedMode != currAppliedPreviewMode) {
            outgoingPreviewMode = currAppliedPreviewMode;
            prevSongPosForFade  = previewSongPosSamples.load(std::memory_order_acquire);
            previewFadeCounter  = kPreviewCrossfadeSamples;
            currAppliedPreviewMode = requestedMode;
            if (requestedMode == (int)PreviewMode::GrainLoop)
                grainNeedsReinit = true;
        }

        const bool fading = (previewFadeCounter > 0 && outgoingPreviewMode != 0);
        const bool anyActive = (currAppliedPreviewMode != 0) || fading;

        if (anyActive) {
            auto songPtr  = previewSongPcm.load();
            auto grainPtr = previewGrainBuf.load();
            const int grainLen = previewGrainLenSamples.load(std::memory_order_acquire);

            // Snapshot the current song-play position. We advance it
            // locally and publish back at the end of the block.
            int64_t songPos = previewSongPosSamples.load(std::memory_order_acquire);

            // Freeze-mode dispatch. The dialog publishes which "sustain the
            // marker" algorithm to run (CrossfadeLoop / AsyncGranular /
            // PitchSyncGrains / SpectralFreeze). We snapshot it once per block
            // and hand it to the shared GrainFreezeVoice, which is the SAME
            // reader the synth uses for a held granular note - so the marker
            // audition is sample-for-sample what the placed frame will play.
            const int freezeMode = grainFreezeMode.load(std::memory_order_acquire);
            // Crossfade seam length (loop modes), pitch ratio (1.0 = native),
            // and the source's natural pitch (only PitchSyncGrains uses it).
            // The voice clamps xfade to loopLen/2 internally.
            const int   xfadeReq      = previewCrossfadeSamples.load(std::memory_order_acquire);
            const float grainRatio    = previewGrainRatio.load(std::memory_order_acquire);
            const float embeddedPitch = previewEmbeddedPitchHz.load(std::memory_order_acquire);
            // Cheap pre-gate: skip the voice entirely when there's no usable
            // source. The voice applies the same per-mode viability test and
            // returns 0 for too-short sources, so silence here == silence in
            // the synth.
            const bool grainViable = grainPtr && grainLen >= 16
                                     && (int)grainPtr->size() >= grainLen;
            const float* srcPtr = grainViable ? grainPtr->data() : nullptr;
            const int    srcLen = grainViable ? (int)grainPtr->size() : 0;
            // On mode-entry into GrainLoop (or after clearPreview) re-anchor the
            // shared voice so the audition restarts on the fresh marker spot.
            // The voice also re-anchors itself whenever the source pointer,
            // length, grain length or freeze mode changes, so steady-state
            // marker scrubbing needs no manual reinit here.
            if (grainNeedsReinit) { grainFreezeVoice.reset(); grainNeedsReinit = false; }

            // One sample of the selected freeze algorithm via the shared voice.
            // srcRate == deviceRate for the audition (the preview PCM is already
            // at the device sample rate), so the voice's PitchSyncGrains period
            // is rate/embeddedPitch and grainRatio carries the pitch transpose.
            auto granularSample = [&]() -> float {
                if (!grainViable) return 0.0f;
                const double rate = getSampleRate();
                const float out = grainFreezeVoice.process(
                    srcPtr, srcLen, grainLen, xfadeReq, embeddedPitch,
                    rate, rate, grainRatio, (GranularFreezeMode)freezeMode);
                // Attenuate to roughly match a synth note's post-Volume level
                // so marker-scrub / freeze audition isn't louder than playback.
                return out * kFreezePreviewGain;
            };

            auto songSampleAdvance = [&](int64_t& posRef) -> float {
                if (!songPtr || songPtr->empty()) return 0.0f;
                const auto& v = *songPtr;
                const int64_t n = (int64_t)v.size();
                if (posRef < 0 || posRef >= n) return 0.0f;
                float s = v[(size_t)posRef];
                ++posRef;
                return s;
            };

            auto modeSample = [&](int mode, int64_t& songPosRef) -> float {
                if (mode == (int)PreviewMode::SongPlay)
                    return songSampleAdvance(songPosRef);
                if (mode == (int)PreviewMode::GrainLoop)
                    return granularSample();
                return 0.0f;
            };

            // Edge case: the granular state machine has a single shared
            // voice bank, so if we ever needed to read BOTH outgoing AND
            // incoming as GrainLoop in the same block (impossible in
            // practice - the only GrainLoop-related transitions are
            // SongPlay<->GrainLoop and Off<->GrainLoop, never
            // GrainLoop->GrainLoop), the outgoing tail would corrupt the
            // incoming state. Guarded just in case future modes change
            // the matrix: when both are GrainLoop, skip the fade.
            const bool bothGrain = (currAppliedPreviewMode == (int)PreviewMode::GrainLoop
                                    && outgoingPreviewMode == (int)PreviewMode::GrainLoop);

            for (int s = 0; s < numSamples; ++s) {
                float t = 1.0f;
                if (previewFadeCounter > 0)
                    t = 1.0f - (float)previewFadeCounter
                                / (float)kPreviewCrossfadeSamples;

                const float inSamp = modeSample(currAppliedPreviewMode, songPos);
                float outSamp = t * inSamp;

                if (previewFadeCounter > 0 && outgoingPreviewMode != 0 && !bothGrain) {
                    const float oldSamp = modeSample(outgoingPreviewMode, prevSongPosForFade);
                    outSamp += (1.0f - t) * oldSamp;
                    --previewFadeCounter;
                    if (previewFadeCounter == 0) outgoingPreviewMode = 0;
                } else if (previewFadeCounter > 0) {
                    --previewFadeCounter;
                    if (previewFadeCounter == 0) outgoingPreviewMode = 0;
                }

                if (numOutputChannels > 0) outputChannelData[0][s] += outSamp;
                if (numOutputChannels > 1) outputChannelData[1][s] += outSamp;
            }

            // Publish the advanced song-play position so the UI's marker
            // tracks the playhead.
            previewSongPosSamples.store(songPos, std::memory_order_release);

            // Song finished while in SongPlay: auto-stop. UI's timer will
            // notice mode==Off and update the transport buttons.
            if (currAppliedPreviewMode == (int)PreviewMode::SongPlay && songPtr) {
                if (songPos >= (int64_t)songPtr->size()) {
                    previewMode.store(0, std::memory_order_release);
                }
            }
        }
    }

    // Always-on output capture: every playback is captured so the Output
    // node's cache can be populated on Stop. The buffer auto-clears when a
    // new playback starts (captureL is empty -> fresh start). Only appends
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
    // We can't iterate graph->nodes from the MIDI thread - the graph may
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
// the cable wired from the node - matching the Phase 1 architecture where
// routing is done purely via graph cables, not flags. Returns nullptr if
// no such node exists (e.g. on an old project that predates the MidiInput
// node type - typing events will be silently dropped in that case, and
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
// Output Capture - record the final mix to memory
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
                n.cache.useDisk = false;
                n.cache.diskPath.clear();
                // Stamp the cache with the project's input hash so the
                // capture-from-song dialog can recognise this rendering
                // as fresh on next open and skip its offline render.
                // updateDeterminism mutates flags on all nodes but is
                // benign (it just walks ccMappings + upstream); a 0
                // hash means the project isn't cacheable, in which
                // case we leave inputHash at 0 and the dialog will
                // re-render from scratch.
                auto& mgr = graphProcessor.getCacheManager();
                mgr.updateDeterminism(*graph);
                n.cache.inputHash = mgr.computeNodeHash(n, *graph);
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
    // Arm capture - actual recording starts when transport plays.
    // Clear previous capture so the next Play->Stop produces a clean buffer.
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

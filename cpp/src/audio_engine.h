#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "plugin_host.h"
#include "graph_processor.h"
#include "recording.h"
#include "multitrack_recorder.h"
#include "granular_frame.h"
#include "granular_freeze.h"
#include <memory>
#include <set>
#include <atomic>

namespace SoundShop {

class AudioEngine : public juce::AudioIODeviceCallback,
                    public juce::MidiInputCallback,
                    public juce::ChangeListener {
public:
    AudioEngine();
    ~AudioEngine();

    void init();
    void shutdown();

    // Persist the current audio device configuration (driver type, device
    // names, sample rate, buffer size, channel selection) to a settings file
    // next to the executable, so the user's choice survives restarts. Called
    // automatically whenever the AudioDeviceManager broadcasts a change (i.e.
    // the user picks a different device in the Audio Device Settings dialog)
    // and on shutdown. See changeListenerCallback / init for the restore side.
    void saveAudioSettings();

    // ChangeListener: the AudioDeviceManager broadcasts a change whenever the
    // device setup is modified. We use it to re-persist the settings so a
    // device change made in the settings dialog sticks across restarts.
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    // Make sure the audio device actually has a live input channel open.
    // On Windows the default input and output are frequently different
    // physical devices, and JUCE's default-device init can land on an
    // output-only setup (input channels zeroed / input device name empty),
    // which silently disables the mic so capture / IR / monitoring get no
    // signal. This picks the device type's default input device + its first
    // channel if input is currently off, restarting the device only when a
    // change is actually needed (no-op, no glitch, if input is already
    // live). Called at init() and again whenever a mic-consuming UI opens
    // (e.g. the microphone capture dialog) so a mic plugged in mid-session
    // is picked up. Returns true if input is enabled afterwards.
    bool ensureAudioInputEnabled();

    // Process-wide accessor for UI tools that need read access to the
    // engine (playback-tap ring buffer, transport state, etc.) without
    // having to be plumbed an AudioEngine& through every component
    // constructor. There is only ever one AudioEngine in the app
    // (constructed in MainWindow), so a static pointer is unambiguous;
    // the constructor / destructor manage it. Returns nullptr if no
    // engine has been constructed yet (e.g. during early static init
    // or in unit tests that don't spin up audio).
    static AudioEngine* getInstance() { return sInstance; }

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData,
        int numInputChannels,
        float* const* outputChannelData,
        int numOutputChannels,
        int numSamples,
        const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    // MidiInputCallback
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& msg) override;

    // MIDI device enablement - called when MidiInput nodes are added/removed
    // so we only keep device callbacks alive for devices the user asked for.
    // Walks the graph and enables exactly the set of devices with matching
    // MidiInput nodes, disabling any others that are currently enabled.
    void syncMidiDeviceEnablement();

    // Return the list of MIDI input devices currently connected to the OS,
    // used by the device wizard to show the user what's available.
    struct MidiDeviceEntry {
        juce::String name;
        juce::String identifier;
        bool presentInGraph = false;
    };
    std::vector<MidiDeviceEntry> listMidiInputDevices() const;

    // Transport
    bool isPlaying() const { return playing.load(); }
    void play() { playing = true; songPlayCount = 0; endTailSamplesRemaining = -1; }
    void stop();
    void pause() { playing = false; }

    // Move the playhead back to the start of playback - the loop start if a
    // user loop region is active, otherwise beat 0. Used when Play is pressed
    // while the playhead is parked at/past the song end so playback restarts
    // from the top instead of beginning in silence past the last note.
    void rewindToStart() {
        if (transport && transport->loopEnabled
            && transport->loopEndBeat > transport->loopStartBeat)
            positionSamples = (int64_t)transport->beatsToSamples(transport->loopStartBeat);
        else
            positionSamples = 0;
        songPlayCount = 0;
        endTailSamplesRemaining = -1;
    }

    double getBpm() const { return bpm.load(); }
    void setBpm(double b) { bpm = b; }

    double getDeviceSampleRate() const { return sampleRate; }
    double getProjectSampleRate() const { return projectSampleRate > 0 ? projectSampleRate : sampleRate; }
    double getSampleRate() const { return getProjectSampleRate(); } // graph processes at project rate
    int getBlockSize() const { return blockSize; }

    // Project sample rate (independent of device)
    void setProjectSampleRate(double sr);
    double projectSampleRate = 0; // 0 = use device rate

    // Plugin hosting
    PluginHost& getPluginHost() { return pluginHost; }

    // Audio device access
    juce::AudioDeviceManager* getDeviceManager() { return deviceManager.get(); }

    // Graph processing
    GraphProcessor& getGraphProcessor() { return graphProcessor; }

    // Recording
    RecordingManager& getRecordingManager() { return recordingManager; }
    MultitrackRecorder& getMultitrackRecorder() { return multitrackRecorder; }

    // Read access to the live graph / transport that this engine is
    // currently driving. Used by feature dialogs (e.g. the capture-from-
    // project dialog) that need to spin up a parallel offline render of
    // the project. May be nullptr before App has wired the engine up.
    NodeGraph* getGraph() const { return graph; }
    Transport* getTransport() const { return transport; }

    // Set the graph and transport to process (called from App)
    void setGraph(NodeGraph* g, Transport* t) {
        graph = g; transport = t;
        if (graph && sampleRate > 0)
            graphProcessor.prepare(*graph, sampleRate, blockSize);
        // Enable only the devices the loaded graph asks for.
        syncMidiDeviceEnablement();
    }

private:
    static AudioEngine* sInstance;

    std::unique_ptr<juce::AudioDeviceManager> deviceManager;
    std::unique_ptr<juce::AudioFormatManager> formatManager;
    PluginHost pluginHost;
    GraphProcessor graphProcessor;
    RecordingManager recordingManager;
    MultitrackRecorder multitrackRecorder;
    NodeGraph* graph = nullptr;
    Transport* transport = nullptr;

    std::atomic<bool> playing{false};
    std::atomic<double> bpm{120.0};
    int64_t positionSamples = 0;

    // Counts how many times the song-length endpoint has been reached
    // since playback started. The song-repeat policy (None/Forever/
    // NTimes) reads this to decide whether to wrap the playhead or halt.
    // Reset on play() and on every manual seek from the transport UI.
    int songPlayCount = 0;

    // Tail-grace state for SongRepeat::None: once the playhead crosses
    // songLengthBeats, instead of halting immediately we keep pumping
    // silent blocks so per-node tails (reverb, release, delay feedback)
    // can ring out.  `endTailSamplesRemaining` is the countdown in samples.
    // -1 means "not in tail grace" (normal playback).
    int64_t endTailSamplesRemaining = -1;

    // Walk the graph and return max(processor->getTailLengthSeconds())
    // across every audio processor.  Called when entering tail grace.
    double computeMaxGraphTailSeconds() const;
public:
    void resetSongPlayCount() { songPlayCount = 0; endTailSamplesRemaining = -1; }
private:
    double sampleRate = 44100.0;  // device sample rate
    int blockSize = 512;

    // Resampling: project rate -> device rate
    std::vector<float> resampleBufL, resampleBufR;
    std::vector<float> projectBufL, projectBufR;
    double resamplePhase = 0.0;

    // Audition-monitor bus (see addAuditionMonitor in the public section).
    // Sized to the project buffer; cleared each callback before the graph
    // processes and summed into the graph output afterwards. auditionMonLen is
    // the valid sample count for the current block.
    std::vector<float> auditionMonL, auditionMonR;
    int auditionMonLen = 0;
    void clearAuditionMonitor(int n);
    void mixAuditionMonitorInto(float* left, float* right, int n) const;

    // Incoming MIDI from hardware controllers
    // Incoming MIDI from hardware devices, with per-message source device
    // identifier so the audio thread can look up the matching MidiInput
    // node and route the event there. Populated by handleIncomingMidiMessage
    // on the MIDI thread; drained by the audio callback.
    std::vector<std::pair<juce::String, juce::MidiMessage>> incomingMidiEvents;
    juce::CriticalSection midiLock;

    // MIDI Learn state
public:
    struct MidiLearnState {
        std::atomic<bool> active{false};
        std::atomic<int> lastCC{-1};
        std::atomic<int> lastChannel{-1};
    } midiLearn;

    // MIDI note recording: captures notes from hardware MIDI keyboard into a track
    struct MidiNoteRecordState {
        int targetNodeId = -1;           // -1 = not recording
        struct ActiveNote {
            int pitch;
            int velocity;
            double startBeat;
        };
        std::vector<ActiveNote> activeNotes;
        std::mutex mutex;
    } midiRecord;

    void startMidiRecording(int nodeId) { midiRecord.targetNodeId = nodeId; }
    void stopMidiRecording() { midiRecord.targetNodeId = -1; }
    bool isMidiRecording() const { return midiRecord.targetNodeId >= 0; }

    // Automation recording: captures parameter changes during playback
    struct AutomationRecordState {
        bool armed = false;               // when true, param changes are recorded
        std::mutex mutex;
    } autoRecord;

    void armAutomationRecording() { autoRecord.armed = true; }
    void disarmAutomationRecording() { autoRecord.armed = false; }
    bool isAutomationArmed() const { return autoRecord.armed; }

    // Input monitoring: hear audio input through the output
    std::atomic<bool> inputMonitoring{false};

    // Hotkey MIDI capture: when non-null, MIDI CC/note events are forwarded
    // here for hotkey assignment instead of being routed to instruments.
    std::function<void(int type, int channel, int number)> hotkeyMidiCapture;

    // Computer keyboard as MIDI input ("musical typing")
    std::atomic<bool> keyboardMidiEnabled{false};
    int keyboardOctave = 4;  // base octave for the keyboard mapping
    std::set<int> keysDown;  // track which keys are currently held

    // Call from the UI's keyPressed/keyReleased to generate MIDI events
    void keyboardNoteOn(int midiNote, int velocity = 100);
    void keyboardNoteOff(int midiNote);

    // Spectrum analyzer ring buffer (#10). The audio thread writes the last
    // N output samples; the UI reads a snapshot for FFT display. Small
    // enough that a torn read is just a visual glitch, never a crash.
    static constexpr int kSpectrumBufSize = 2048;
    float spectrumBufL[kSpectrumBufSize] = {};
    float spectrumBufR[kSpectrumBufSize] = {};
    int spectrumWritePos = 0;
    // UI calls this to grab a snapshot for FFT analysis.
    void getSpectrumSnapshot(std::vector<float>& out, int channel = 0) const {
        out.resize(kSpectrumBufSize);
        const float* src = (channel == 0) ? spectrumBufL : spectrumBufR;
        int wp = spectrumWritePos;
        for (int i = 0; i < kSpectrumBufSize; ++i)
            out[i] = src[(wp + i) % kSpectrumBufSize];
    }

    // Playback-tap ring buffer for the wavetable capture-from-playback
    // feature. Always-on (~1 MB per channel at the device rate, ~30 s of
    // audio at typical sample rates). The audio thread writes mono-mixed
    // output samples here after the graph processes each block; the UI
    // thread snapshots a window of recent samples for the capture dialog
    // to display and to slice into wavetable frames. Torn reads are
    // acceptable - the worst case is a visual glitch on the dialog's
    // waveform display, and grain capture happens on the UI thread after
    // a complete snapshot is taken.
    static constexpr int kPlaybackTapBufSize = 1 << 21;  // ~2M samples, ~43 s at 48 kHz
    std::vector<float> playbackTapBuf;                   // mono-summed (L+R)*0.5
    std::atomic<int>   playbackTapWritePos{0};
    std::atomic<double> playbackTapSampleRate{44100.0};

    // UI calls this to grab the most recent N samples (in *device* sample
    // rate) into `out`, oldest-first. Returns the device sample rate the
    // samples were captured at (or 0 if the buffer has never been written).
    // out is resized to numSamples (which is clamped to kPlaybackTapBufSize).
    double getPlaybackTapSnapshot(std::vector<float>& out, int numSamples) const {
        if (playbackTapBuf.size() != (size_t)kPlaybackTapBufSize) {
            out.clear();
            return 0.0;
        }
        int n = std::min(numSamples, (int)kPlaybackTapBufSize);
        out.resize((size_t)n);
        int wp = playbackTapWritePos.load(std::memory_order_acquire);
        // wp points to the NEXT slot to be written, so the last written
        // sample is at (wp - 1). Walk back n samples from there.
        for (int i = 0; i < n; ++i) {
            int src = ((wp - n + i) % kPlaybackTapBufSize + kPlaybackTapBufSize) % kPlaybackTapBufSize;
            out[(size_t)i] = playbackTapBuf[(size_t)src];
        }
        return playbackTapSampleRate.load(std::memory_order_acquire);
    }

    // Mic-input tap ring buffer (#captureDialog). Always-on parallel to
    // playbackTapBuf, but reads from inputChannelData in the audio
    // callback instead of outputChannelData. Same size and contract;
    // mono-summed across all input channels. Lets the capture dialog
    // pull a live "what's at the mic right now" snapshot without
    // requiring an explicit Arm/Record gesture (matches the symmetric
    // experience the playback tap offers).
    static constexpr int kMicTapBufSize = kPlaybackTapBufSize;
    std::vector<float> micTapBuf;
    std::atomic<int>   micTapWritePos{0};
    std::atomic<double> micTapSampleRate{44100.0};
    // Latched true the first time any non-zero sample arrives on input,
    // so the dialog can distinguish "no mic plugged in / muted" from
    // "mic exists but the user hasn't made any sound yet". Reset on
    // device start so plugging/unplugging midway through a session
    // re-evaluates the flag.
    std::atomic<bool>  micTapHasSignal{false};

    double getMicTapSnapshot(std::vector<float>& out, int numSamples) const {
        if (micTapBuf.size() != (size_t)kMicTapBufSize) {
            out.clear();
            return 0.0;
        }
        int n = std::min(numSamples, (int)kMicTapBufSize);
        out.resize((size_t)n);
        int wp = micTapWritePos.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i) {
            int src = ((wp - n + i) % kMicTapBufSize + kMicTapBufSize) % kMicTapBufSize;
            out[(size_t)i] = micTapBuf[(size_t)src];
        }
        return micTapSampleRate.load(std::memory_order_acquire);
    }
    bool hasMicSignal() const { return micTapHasSignal.load(std::memory_order_acquire); }

    // Output capture: record the final mix to memory for "Save as Audio Track"
    // or export. Only the audio thread writes to the buffers (while capturing);
    // the UI thread reads them only after capture is stopped. No lock needed.
    std::atomic<bool> outputCaptureEnabled{false};
    std::vector<float> captureL, captureR;
    double captureSampleRate = 44100.0;
    int64_t captureStartSample = 0; // transport position when capture started

    void startOutputCapture();
    void stopOutputCapture();
    bool isCapturingOutput() const { return outputCaptureEnabled.load(); }

    // Retrieve the captured audio as a stereo JUCE buffer. Returns empty if
    // no capture data. Does NOT clear the capture - call clearCapture() for that.
    juce::AudioBuffer<float> getCaptureBuffer() const;
    int64_t getCaptureStartSample() const { return captureStartSample; }
    void clearCapture();

    // Record a param change (called from UI when user moves a slider/knob)
    void recordParamChange(int nodeId, int paramIdx, float value);

    // ============================================================
    // Audition-monitor bus.
    // ============================================================
    // When a synth node is NOT routed to an Output node (Node::reachesOutput
    // == false), its editor "Play" audition voices have nowhere to go in the
    // graph - the rendered audio dead-ends at the unconnected node. To keep the
    // preview audible regardless of wiring, TerrainSynthProcessor diverts those
    // audition voices here instead of into its graph output buffer. The audio
    // callback clears this bus before processing the graph and sums it into the
    // final output afterwards (at project rate, so it resamples with the rest).
    //
    // Thread model: addAuditionMonitor() is only ever called from inside
    // graphProcessor.processBlock(), which the audio callback invokes between
    // clearAuditionMonitor() and mixAuditionMonitorInto() on the SAME thread -
    // so no locking is needed. Synth nodes accumulate (multiple unrouted synths
    // auditioning at once sum together). Mono in, panned dead-center.
    void addAuditionMonitor(const float* mono, int n);

    // ============================================================
    // Preview audio for the "Capture from project" dialog (#capV2).
    // ============================================================
    // The dialog pre-renders the whole song to mono PCM once on open,
    // then drives this engine to either (a) play the rendered song from
    // the marker position at full fidelity (Playing state) or (b) loop
    // a small grain extracted around the marker (Paused / Scrubbing
    // state). The audio thread mixes whichever mode is active on top of
    // the live graph output so the user can audition while picking the
    // capture spot without touching the live transport.
    //
    // Thread safety:
    //   - songPcm and grainBuf are std::shared_ptr published via
    //     std::atomic. The audio thread loads once per block and reads
    //     from the locked-in pointer for the whole block.
    //   - previewMode / previewSongPosSamples are plain atomics.
    //   - On mode change, the audio thread runs a ~10 ms linear
    //     crossfade between the previous and new modes so transport
    //     state transitions don't click.
    enum class PreviewMode { Off = 0, SongPlay = 1, GrainLoop = 2 };

    // GrainFreezeMode picks which "freeze the marker spot" algorithm runs in
    // GrainLoop preview. Each has its own sonic character; see audio_engine.h
    // private section for the full rundown. Aliased to GranularFreezeMode
    // from granular_frame.h so the dialog auditions with the same algorithm
    // it bakes into the saved frame and there's only one wire enum to keep
    // stable.
    using GrainFreezeMode = GranularFreezeMode;
    void setGrainFreezeMode(GrainFreezeMode m) {
        grainFreezeMode.store((int)m, std::memory_order_release);
    }
    GrainFreezeMode getGrainFreezeMode() const {
        return (GrainFreezeMode)grainFreezeMode.load(std::memory_order_acquire);
    }

    // Install / replace the song PCM (mono, at the device sample rate).
    // Pass nullptr to clear. Safe to call from any thread.
    void setPreviewSongPcm(std::shared_ptr<std::vector<float>> pcm) {
        previewSongPcm.store(std::move(pcm));
    }
    // Install / replace the grain source buffer (mono, at the device sample
    // rate). This is the source material the granular voices read from -
    // typically a window of the song several times longer than the grain
    // length, so voices can pick jittered start positions inside it. Pass
    // nullptr to clear. Safe to call from any thread.
    void setPreviewGrainBuffer(std::shared_ptr<std::vector<float>> grain) {
        previewGrainBuf.store(std::move(grain));
    }
    // Set the grain length N (in samples). This is the envelope length of
    // each Hann-windowed grain in the OLA stream, NOT the size of the
    // source buffer (which is typically several times larger). Safe to
    // call from any thread; audio thread re-initialises voice phases on
    // the next block if the value changed.
    void setPreviewGrainLength(int n) {
        previewGrainLenSamples.store(n, std::memory_order_release);
    }
    int  getPreviewGrainLength() const {
        return previewGrainLenSamples.load(std::memory_order_acquire);
    }
    // Set the crossfade length in samples for the CrossfadeLoop freeze
    // mode. The audio thread snapshots this per block and clamps to
    // [0, grainLen/2] -- bigger than L/2 would overlap the two ramps and
    // collapse the loop. 0 disables the crossfade (clicks at every seam,
    // but a useful debug option). Safe to call from any thread.
    void setPreviewCrossfadeLength(int n) {
        previewCrossfadeSamples.store(std::max(0, n), std::memory_order_release);
    }
    int  getPreviewCrossfadeLength() const {
        return previewCrossfadeSamples.load(std::memory_order_acquire);
    }
    // Set the GrainLoop playback pitch ratio. 1.0 = play the marker loop at
    // its native rate (the literal recording). >1 plays it faster/higher,
    // <1 slower/lower. The capture dialog's "As note" picker drives this so
    // the marker audition is pitched to match what the eventual synth voice
    // will produce when the captured frame is played at the editor's
    // reference note (A4 = 440 Hz): ratio = 440 / capturedPitchHz. The audio
    // thread resamples the loop with linear interpolation; the source buffer
    // already reserves an L/2 lookahead tail, so any ratio in the supported
    // pitch range stays inside the buffer. Safe to call from any thread.
    void setPreviewGrainRatio(float r) {
        previewGrainRatio.store(r > 0.0f ? r : 1.0f, std::memory_order_release);
    }
    float getPreviewGrainRatio() const {
        return previewGrainRatio.load(std::memory_order_acquire);
    }
    // The marker recording's natural pitch in Hz, published alongside the
    // grain ratio. Only the PitchSyncGrains freeze mode reads it (to derive
    // the loop period = sampleRate / pitch); the other modes ignore it. Set to
    // the same capturedPitchHz the synth bakes into the frame so the audition
    // pitch-sync loop matches the played note. Safe to call from any thread.
    void setPreviewEmbeddedPitch(float hz) {
        previewEmbeddedPitchHz.store(hz > 0.0f ? hz : 440.0f,
                                     std::memory_order_release);
    }
    void setPreviewMode(PreviewMode m) {
        previewMode.store((int)m, std::memory_order_release);
    }
    PreviewMode getPreviewMode() const {
        return (PreviewMode)previewMode.load(std::memory_order_acquire);
    }
    // Seek the song-play playhead (UI thread). The audio thread advances
    // this on each block while in SongPlay mode.
    void setPreviewSongPosSamples(int64_t s) {
        previewSongPosSamples.store(s, std::memory_order_release);
    }
    int64_t getPreviewSongPosSamples() const {
        return previewSongPosSamples.load(std::memory_order_acquire);
    }
    // Clears all preview state. Use on dialog close. Sets mode=Off, drops
    // both buffers, zeroes the position.
    void clearPreview() {
        setPreviewMode(PreviewMode::Off);
        setPreviewSongPcm(nullptr);
        setPreviewGrainBuffer(nullptr);
        previewGrainLenSamples.store(0, std::memory_order_release);
        previewCrossfadeSamples.store(0, std::memory_order_release);
        previewGrainRatio.store(1.0f, std::memory_order_release);
        previewEmbeddedPitchHz.store(440.0f, std::memory_order_release);
        previewSongPosSamples.store(0, std::memory_order_release);
        grainNeedsReinit = true;  // force voice re-anchor on next GrainLoop entry
    }

private:
    std::atomic<int>      previewMode { 0 };
    std::atomic<int64_t>  previewSongPosSamples { 0 };
    std::atomic<int>      previewGrainLenSamples { 0 };
    std::atomic<int>      previewCrossfadeSamples { 0 };
    std::atomic<float>    previewGrainRatio { 1.0f };
    std::atomic<float>    previewEmbeddedPitchHz { 440.0f };
    std::atomic<std::shared_ptr<std::vector<float>>> previewSongPcm;
    std::atomic<std::shared_ptr<std::vector<float>>> previewGrainBuf;
    // Audio-thread-only crossfade state.
    //   currAppliedPreviewMode  - the mode we are currently bleeding in
    //                             (matches previewMode after the most
    //                             recent transition is observed).
    //   outgoingPreviewMode     - the mode we are bleeding out (0 = none).
    //   previewFadeCounter      - samples remaining in the fade. Reaches 0
    //                             once outgoing is fully muted.
    //   prevSongPosForFade      - song-play read position for the outgoing
    //                             mode (when outgoing=SongPlay). The dying
    //                             stream keeps advancing through the song
    //                             while it fades out.
    //
    // Grain freeze (GrainLoop mode) - the engine continuously plays back the
    // marker spot so the user can audition it before committing to a Capture.
    // The dialog publishes a multi-second source PCM window centered on the
    // marker; the engine picks an L-sample loop inside that window (L =
    // previewGrainLenSamples) and plays it on repeat. There are four
    // algorithmic variants, all reaching for "sustain the marker spot
    // smoothly" but each with its own character:
    //   CrossfadeLoop    - faithful tape loop with a short Hann crossfade
    //                      at the seam (no click). Default / "what does
    //                      this spot literally sound like."
    //   AsyncGranular    - many short grains at randomised positions in a
    //                      small range near the marker. "Frozen blur"
    //                      texture, like GRM Freeze.
    //   PitchSyncGrains  - grain hop locked to the detected pitch period,
    //                      so the loop is one cycle and the output is a
    //                      true sustained tone at that pitch. Requires a
    //                      pitch-detected source.
    //   SpectralFreeze   - FFT a window at the marker, keep magnitudes,
    //                      regenerate phases per frame, IFFT. Ethereal
    //                      pad-like sustain.
    // The mode the dialog wants is in grainFreezeMode (atomic, set by the
    // GUI thread). The audio thread snapshots it once per block and hands it
    // to the shared GrainFreezeVoice below, which holds all per-mode state
    // (anchor, loop phase, grain bank, spectral buffers) and dispatches on the
    // mode. All four algorithms are implemented in granular_freeze.cpp.
    //
    //   grainNeedsReinit - set on mode-entry into GrainLoop (and clearPreview)
    //                      so the next block resets the shared voice and it
    //                      re-anchors on the fresh marker spot. The voice also
    //                      re-anchors itself when the source pointer, length,
    //                      grain length or mode changes, so steady-state marker
    //                      scrubbing needs no manual reinit.
    int     currAppliedPreviewMode = 0;
    int     outgoingPreviewMode = 0;
    int     previewFadeCounter = 0;
    int64_t prevSongPosForFade = 0;
    bool    grainNeedsReinit       = false;
    // Atomic mode pick. 0 = CrossfadeLoop (default), 1 = AsyncGranular,
    // 2 = PitchSyncGrains, 3 = SpectralFreeze. Stored as int so the
    // atomic is trivially copyable and lock-free.
    std::atomic<int> grainFreezeMode { 0 };
    // Shared freeze reader. The same GrainFreezeVoice implementation backs the
    // held synth note (TerrainSynthProcessor) so that "what you audition in the
    // capture dialog == what the synth plays back" is guaranteed by construction
    // rather than by keeping two hand-copied loop readers in lockstep. The voice
    // owns its own per-mode state and re-anchors itself when the source pointer,
    // length, grain length or mode changes.
    GrainFreezeVoice grainFreezeVoice;
    static constexpr int kPreviewCrossfadeSamples = 480;  // ~10 ms @ 48 kHz
    // The GrainLoop freeze preview (used for marker-scrub audition and the
    // capture-dialog Play button when there's no synth node to route through)
    // plays the source PCM directly at full scale. A wired-up synth note, by
    // contrast, is attenuated by env * velocity * Volume before it hits the
    // mix. To keep the freeze audition from being noticeably louder than
    // actual note playback, scale the GrainLoop output down by this factor.
    // It's a perceptual match, not an exact one (a dense full-mix grain and a
    // single synth voice never measure identically), so this is tuned by ear.
    static constexpr float kFreezePreviewGain = 0.5f;
public:

    // MPE recording: captures per-note expression from hardware MPE controllers
    struct MpeRecordNote {
        int channel;
        int pitch;
        int velocity;
        double startBeat;
        std::vector<ExpressionPoint> pitchBend;
        std::vector<ExpressionPoint> slide;
        std::vector<ExpressionPoint> pressure;
    };
    std::vector<MpeRecordNote> mpeActiveNotes;
    std::mutex mpeMutex;
    int mpeRecordTargetNodeId = -1; // which node to add recorded notes to
};

} // namespace SoundShop

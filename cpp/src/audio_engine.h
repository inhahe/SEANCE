#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "plugin_host.h"
#include "graph_processor.h"
#include "recording.h"
#include "multitrack_recorder.h"
#include <memory>
#include <set>
#include <atomic>

namespace SoundShop {

class AudioEngine : public juce::AudioIODeviceCallback,
                    public juce::MidiInputCallback {
public:
    AudioEngine();
    ~AudioEngine();

    void init();
    void shutdown();

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

    // Set the graph and transport to process (called from App)
    void setGraph(NodeGraph* g, Transport* t) {
        graph = g; transport = t;
        if (graph && sampleRate > 0)
            graphProcessor.prepare(*graph, sampleRate, blockSize);
        // Enable only the devices the loaded graph asks for.
        syncMidiDeviceEnablement();
    }

private:
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

#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "plugin_host.h"
#include "graph_processor.h"
#include "recording.h"
#include <memory>
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

    // Transport
    bool isPlaying() const { return playing.load(); }
    void play() { playing = true; }
    void stop() { playing = false; positionSamples = 0; }
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

    // Set the graph and transport to process (called from App)
    void setGraph(NodeGraph* g, Transport* t) {
        graph = g; transport = t;
        if (graph && sampleRate > 0)
            graphProcessor.prepare(*graph, sampleRate, blockSize);
    }

private:
    std::unique_ptr<juce::AudioDeviceManager> deviceManager;
    std::unique_ptr<juce::AudioFormatManager> formatManager;
    PluginHost pluginHost;
    GraphProcessor graphProcessor;
    RecordingManager recordingManager;
    NodeGraph* graph = nullptr;
    Transport* transport = nullptr;

    std::atomic<bool> playing{false};
    std::atomic<double> bpm{120.0};
    int64_t positionSamples = 0;
    double sampleRate = 44100.0;  // device sample rate
    int blockSize = 512;

    // Resampling: project rate -> device rate
    std::vector<float> resampleBufL, resampleBufR;
    std::vector<float> projectBufL, projectBufR;
    double resamplePhase = 0.0;

    // Incoming MIDI from hardware controllers
    juce::MidiBuffer incomingMidi;
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

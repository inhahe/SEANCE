#pragma once
#include "node_graph.h"
#include "transport.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <mutex>

namespace SoundShop {

class RecordingManager {
public:
    RecordingManager();

    // Start recording on a node (creates a new take lane or clip)
    void startRecording(Node& node, int numChannels, double sampleRate, const std::string& outputDir);

    // Called from audio thread — append samples
    void processSamples(const float* const* inputData, int numChannels, int numSamples);

    // Stop recording — finalizes the audio file and adds a clip
    void stopRecording(Node& node, Transport& transport);

    bool isRecording() const { return recording.load(); }
    int getRecordingNodeId() const { return recordingNodeId; }

private:
    std::atomic<bool> recording{false};
    int recordingNodeId = -1;
    double recordingSampleRate = 44100;
    int recordingChannels = 2;

    // Buffer for incoming audio (ring buffer approach)
    std::mutex bufferMutex;
    juce::AudioBuffer<float> recordBuffer;
    int writePos = 0;
    int64_t totalSamplesRecorded = 0;
    double recordStartBeat = 0;

    std::string outputFilePath;
    std::unique_ptr<juce::FileOutputStream> fileStream;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    juce::WavAudioFormat wavFormat;
};

} // namespace SoundShop

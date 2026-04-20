#pragma once
#include "node_graph.h"
#include "transport.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>

namespace SoundShop {

// Per-track recording state. Each armed Audio Track gets one of these
// during a recording session. The audio callback writes input samples
// to the buffer; on stop, the buffer is saved to a WAV file and a clip
// is created on the track.
struct TrackRecordState {
    int nodeId = -1;
    int inputChannel = -1;    // which hardware input channel
    std::vector<float> buffer; // accumulated samples (mono)
    int64_t samplesRecorded = 0;
    double startBeat = 0;
    bool active = false;
};

class MultitrackRecorder {
public:
    // Start recording on all armed Audio Track nodes.
    // Called when the user presses Record + Play.
    void startRecording(NodeGraph& graph, Transport& transport, double sampleRate,
                        const std::string& outputDir);

    // Called from the audio callback each block.
    // Routes each input channel to its assigned track's buffer.
    void processSamples(const float* const* inputData, int numInputChannels,
                        int numSamples);

    // Called from the audio callback to mix monitored inputs into the output.
    // Adds the assigned input channel to the output for each track with
    // monitoring enabled.
    void processMonitoring(const float* const* inputData, int numInputChannels,
                           float* const* outputData, int numOutputChannels,
                           int numSamples, NodeGraph& graph);

    // Stop recording on all tracks — finalize WAV files, create clips.
    void stopRecording(NodeGraph& graph, Transport& transport, double sampleRate);

    bool isRecording() const { return recording.load(); }
    int getActiveTrackCount() const { return (int)tracks.size(); }

private:
    std::atomic<bool> recording{false};
    std::vector<TrackRecordState> tracks;
    std::string outputDirectory;
    double recordSampleRate = 44100;
};

} // namespace SoundShop

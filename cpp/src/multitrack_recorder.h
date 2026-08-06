#pragma once
#include "node_graph.h"
#include "transport.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>
#include <memory>
#include <atomic>
#include <string>

namespace SoundShop {

// Per-track recording state. Each armed Audio Track gets one of these during a
// recording session.
//
// Samples go straight from the audio callback into a juce ThreadedWriter, which
// hands them to a shared background thread that does the actual file I/O. The
// audio thread therefore never allocates, never blocks on the disk, and has no
// length limit -- takes can run as long as the drive has room. (An earlier
// version accumulated the whole take in a std::vector via push_back, which
// reallocated mid-callback -- a guaranteed dropout every time a take passed the
// 60-second reserve, and a multi-hundred-megabyte memcpy on the audio thread for
// long takes.)
struct TrackRecordState {
    int nodeId = -1;
    int inputChannel = -1;    // which hardware input channel
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    juce::File file;
    int64_t samplesRecorded = 0;  // reached the file; audio thread writes only
    int64_t samplesDropped = 0;   // FIFO was full, audio was lost
    double startBeat = 0;
    bool active = false;
};

class MultitrackRecorder {
public:
    MultitrackRecorder();
    ~MultitrackRecorder();

    // Start recording on all armed Audio Track nodes.
    // Called when the user presses Record + Play.
    void startRecording(NodeGraph& graph, Transport& transport, double sampleRate,
                        const std::string& outputDir);

    // Called from the audio callback each block.
    // Routes each input channel to its assigned track's writer.
    void processSamples(const float* const* inputData, int numInputChannels,
                        int numSamples);

    // Called from the audio callback to mix monitored inputs into the output.
    // Adds the assigned input channel to the output for each track with
    // monitoring enabled.
    void processMonitoring(const float* const* inputData, int numInputChannels,
                           float* const* outputData, int numOutputChannels,
                           int numSamples, NodeGraph& graph);

    // Stop recording on all tracks - finalize WAV files, create clips.
    void stopRecording(NodeGraph& graph, Transport& transport, double sampleRate);

    bool isRecording() const { return recording.load(); }
    int getActiveTrackCount() const { return (int) tracks.size(); }

    // Samples the last take lost because the disk could not keep up, summed
    // over every track. Non-zero means the recording has audible gaps, so the
    // user has to be told rather than handed a quietly corrupt take.
    int64_t getDroppedSampleCount() const { return droppedSamples; }

private:
    std::atomic<bool> recording{false};
    std::vector<TrackRecordState> tracks;
    std::string outputDirectory;
    double recordSampleRate = 44100;
    int64_t droppedSamples = 0;   // totalled up by stopRecording

    // Guards `tracks` against the audio callback while start/stop rebuild it.
    // This is the pattern JUCE's own AudioRecordingDemo uses: the lock is
    // uncontended on every normal block, and it is the only way to tear the
    // writers down without racing a callback that is mid-write.
    juce::CriticalSection trackLock;

    // Shared background thread that drains every track's ThreadedWriter.
    juce::TimeSliceThread writerThread{"SEANCE recording"};
};

} // namespace SoundShop

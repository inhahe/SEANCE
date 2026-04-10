#include "recording.h"
#include <cstdio>

namespace SoundShop {

RecordingManager::RecordingManager() {}

void RecordingManager::startRecording(Node& node, int numChannels, double sampleRate,
                                       const std::string& outputDir) {
    if (recording.load()) return;

    recordingNodeId = node.id;
    recordingSampleRate = sampleRate;
    recordingChannels = numChannels;
    totalSamplesRecorded = 0;

    // Create output file
    auto dir = juce::File(outputDir);
    if (!dir.isDirectory())
        dir.createDirectory();

    auto timestamp = juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
    auto fileName = node.name + "_" + timestamp.toStdString() + ".wav";
    auto file = dir.getChildFile(fileName);
    outputFilePath = file.getFullPathName().toStdString();

    // Create WAV writer
    fileStream = std::make_unique<juce::FileOutputStream>(file);
    if (fileStream->failedToOpen()) {
        fprintf(stderr, "Failed to create recording file: %s\n", outputFilePath.c_str());
        return;
    }

    writer.reset(wavFormat.createWriterFor(fileStream.get(), sampleRate, numChannels,
                                            24, {}, 0));
    if (!writer) {
        fprintf(stderr, "Failed to create WAV writer\n");
        fileStream.reset();
        return;
    }

    // FileOutputStream is now owned by the writer
    fileStream.release();

    recording = true;
    fprintf(stderr, "Recording started: %s (%d ch, %.0f Hz)\n",
            outputFilePath.c_str(), numChannels, sampleRate);
}

void RecordingManager::processSamples(const float* const* inputData, int numChannels, int numSamples) {
    if (!recording.load() || !writer) return;

    // Write directly to file (WAV writer is thread-safe for sequential writes)
    juce::AudioBuffer<float> buf(const_cast<float* const*>(inputData), numChannels, numSamples);
    writer->writeFromAudioSampleBuffer(buf, 0, numSamples);
    totalSamplesRecorded += numSamples;
}

void RecordingManager::stopRecording(Node& node, Transport& transport) {
    if (!recording.load()) return;

    recording = false;
    writer.reset(); // flushes and closes the file

    double durationSecs = totalSamplesRecorded / recordingSampleRate;
    double durationBeats = durationSecs * transport.bpm / 60.0;

    fprintf(stderr, "Recording stopped: %.1f seconds (%.1f beats), saved to %s\n",
            durationSecs, durationBeats, outputFilePath.c_str());

    // Create a clip or take lane with the recording
    if (node.takeLanes.empty()) {
        // First recording — create a take lane
        TakeLane lane;
        lane.name = "Take 1";
        Clip clip;
        clip.name = "Take 1";
        clip.startBeat = recordStartBeat;
        clip.lengthBeats = (float)durationBeats;
        clip.audioFilePath = outputFilePath;
        clip.channels = recordingChannels;
        clip.color = 0xFFCC6644; // orange-ish
        lane.clips.push_back(clip);
        node.takeLanes.push_back(std::move(lane));
        node.activeTakeLane = 0;
    } else {
        // Subsequent recording — add a new take lane
        TakeLane lane;
        lane.name = "Take " + std::to_string(node.takeLanes.size() + 1);
        Clip clip;
        clip.name = lane.name;
        clip.startBeat = recordStartBeat;
        clip.lengthBeats = (float)durationBeats;
        clip.audioFilePath = outputFilePath;
        clip.channels = recordingChannels;
        clip.color = 0xFFCC6644;
        lane.clips.push_back(clip);
        node.takeLanes.push_back(std::move(lane));
        node.activeTakeLane = (int)node.takeLanes.size() - 1;
    }

    // Also add the clip to the node's main clip list for immediate playback
    Clip mainClip;
    mainClip.name = "Recording";
    mainClip.startBeat = recordStartBeat;
    mainClip.lengthBeats = (float)durationBeats;
    mainClip.audioFilePath = outputFilePath;
    mainClip.channels = recordingChannels;
    mainClip.color = 0xFFCC6644;
    node.clips.push_back(mainClip);

    recordingNodeId = -1;
}

} // namespace SoundShop

#include "multitrack_recorder.h"
#include "audio_export.h"
#include <cstring>
#include <algorithm>

namespace SoundShop {

void MultitrackRecorder::startRecording(NodeGraph& graph, Transport& transport,
                                         double sampleRate, const std::string& outputDir) {
    tracks.clear();
    outputDirectory = outputDir;
    recordSampleRate = sampleRate;

    double startBeat = transport.positionBeats();

    // Find all armed Audio Track nodes
    for (auto& node : graph.nodes) {
        if (node.type != NodeType::AudioTimeline) continue;
        if (!node.recordArmed) continue;
        if (node.recordInputChannel < 0) continue;

        TrackRecordState state;
        state.nodeId = node.id;
        state.inputChannel = node.recordInputChannel;
        state.startBeat = startBeat;
        state.active = true;
        // Pre-reserve ~60 seconds
        state.buffer.reserve((size_t)(sampleRate * 60.0));
        tracks.push_back(std::move(state));
    }

    if (tracks.empty()) return;
    recording = true;
}

void MultitrackRecorder::processSamples(const float* const* inputData,
                                         int numInputChannels, int numSamples) {
    if (!recording.load()) return;

    for (auto& track : tracks) {
        if (!track.active) continue;
        int ch = track.inputChannel;
        if (ch < 0 || ch >= numInputChannels || !inputData[ch]) continue;

        const float* src = inputData[ch];
        for (int s = 0; s < numSamples; ++s)
            track.buffer.push_back(src[s]);
        track.samplesRecorded += numSamples;
    }
}

void MultitrackRecorder::processMonitoring(const float* const* inputData,
                                            int numInputChannels,
                                            float* const* outputData,
                                            int numOutputChannels,
                                            int numSamples,
                                            NodeGraph& graph) {
    // For each Audio Track with monitoring enabled, mix its input channel
    // into the output so the performer can hear themselves.
    for (auto& node : graph.nodes) {
        if (node.type != NodeType::AudioTimeline) continue;
        if (!node.inputMonitor) continue;
        int ch = node.recordInputChannel;
        if (ch < 0 || ch >= numInputChannels || !inputData[ch]) continue;

        // Read volume/pan from node params
        float volume = 1.0f, pan = 0.0f;
        for (auto& p : node.params) {
            if (p.name == "Volume") volume = p.value;
            if (p.name == "Pan") pan = p.value;
        }

        // Simple pan law
        float gainL = volume, gainR = volume;
        if (pan < 0) gainR *= (1.0f + pan);
        if (pan > 0) gainL *= (1.0f - pan);

        const float* src = inputData[ch];
        for (int s = 0; s < numSamples; ++s) {
            if (numOutputChannels > 0 && outputData[0])
                outputData[0][s] += src[s] * gainL;
            if (numOutputChannels > 1 && outputData[1])
                outputData[1][s] += src[s] * gainR;
        }
    }
}

void MultitrackRecorder::stopRecording(NodeGraph& graph, Transport& transport,
                                        double sampleRate) {
    recording = false;

    juce::File outputDir(outputDirectory);
    if (!outputDir.isDirectory())
        outputDir = juce::File::getCurrentWorkingDirectory().getChildFile("recordings");
    outputDir.createDirectory();

    auto timestamp = juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");

    for (auto& track : tracks) {
        if (!track.active || track.buffer.empty()) continue;

        auto* node = graph.findNode(track.nodeId);
        if (!node) continue;

        // Save to WAV
        juce::String nodeName = juce::String(node->name).replaceCharacters(" /\\:", "____");
        juce::String fileName = nodeName + "_" + timestamp + ".wav";
        juce::File outFile = outputDir.getChildFile(fileName);

        // Build a JUCE buffer from our mono recording
        int numSamples = (int)track.buffer.size();
        juce::AudioBuffer<float> buf(1, numSamples);
        buf.copyFrom(0, 0, track.buffer.data(), numSamples);

        // Export as 24-bit WAV
        ExportOptions opts;
        opts.format = ExportFormat::WAV;
        opts.sampleRate = (int)sampleRate;
        opts.bitsPerSample = 24;
        AudioExporter::exportToFile(outFile, buf, opts);

        // Create a clip on the track pointing to the recorded file
        double durationBeats = (double)numSamples / sampleRate * (transport.bpm / 60.0);
        Clip clip;
        clip.name = fileName.toStdString();
        clip.startBeat = (float)track.startBeat;
        clip.lengthBeats = (float)durationBeats;
        clip.color = juce::Colours::red.withSaturation(0.6f).getARGB();
        clip.audioFilePath = outFile.getFullPathName().toStdString();
        node->clips.push_back(clip);

        // Disarm after recording
        node->recordArmed = false;

        juce::Logger::writeToLog("Recorded: " + outFile.getFullPathName()
            + " (" + juce::String(numSamples) + " samples, "
            + juce::String((double)numSamples / sampleRate, 1) + "s)");
    }

    tracks.clear();
    graph.dirty = true;
}

} // namespace SoundShop

#include "multitrack_recorder.h"
#include <cstring>
#include <algorithm>

namespace SoundShop {

MultitrackRecorder::MultitrackRecorder() {
    writerThread.startThread(juce::Thread::Priority::normal);
}

MultitrackRecorder::~MultitrackRecorder() {
    {
        const juce::ScopedLock sl(trackLock);
        recording = false;
        tracks.clear();
    }
    writerThread.stopThread(2000);
}

void MultitrackRecorder::startRecording(NodeGraph& graph, Transport& transport,
                                         double sampleRate, const std::string& outputDir) {
    {
        const juce::ScopedLock sl(trackLock);
        recording = false;
        tracks.clear();
    }
    outputDirectory = outputDir;
    recordSampleRate = sampleRate;

    double startBeat = transport.positionBeats();

    juce::File dir(outputDir);
    dir.createDirectory();
    auto timestamp = juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");

    // Build every track's file and writer up front, off the audio thread, so
    // the callback only ever has to push samples at an already-open writer.
    std::vector<TrackRecordState> staged;
    juce::WavAudioFormat wavFormat;

    for (auto& node : graph.nodes) {
        if (node.type != NodeType::AudioTimeline) continue;
        if (!node.recordArmed) continue;
        if (node.recordInputChannel < 0) continue;

        juce::String nodeName = juce::String(node.name).replaceCharacters(" /\\:", "____");
        juce::File outFile = dir.getChildFile(nodeName + "_" + timestamp + ".wav");
        outFile.deleteFile();

        auto stream = std::make_unique<juce::FileOutputStream>(outFile);
        if (stream->failedToOpen()) {
            juce::Logger::writeToLog("Recording: could not open " + outFile.getFullPathName());
            continue;
        }

        // 24-bit mono, matching the device rate.
        std::unique_ptr<juce::AudioFormatWriter> writer(
            wavFormat.createWriterFor(stream.get(), sampleRate, 1, 24, {}, 0));
        if (!writer) {
            juce::Logger::writeToLog("Recording: could not create WAV writer for "
                                     + outFile.getFullPathName());
            continue;
        }
        stream.release();  // the writer owns it now

        TrackRecordState state;
        state.nodeId = node.id;
        state.inputChannel = node.recordInputChannel;
        state.startBeat = startBeat;
        state.active = true;
        state.file = outFile;
        // Four seconds of FIFO headroom per track. JUCE's demo uses a fixed
        // 32768 samples, which is only ~0.7 s at 44.1 kHz - short enough that
        // one ordinary disk or antivirus stall mid-take would overrun it and
        // silently lose audio.
        state.writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(
            writer.release(), writerThread, (int) (sampleRate * 4.0));
        staged.push_back(std::move(state));
    }

    if (staged.empty()) return;

    const juce::ScopedLock sl(trackLock);
    tracks = std::move(staged);
    recording = true;
}

void MultitrackRecorder::processSamples(const float* const* inputData,
                                         int numInputChannels, int numSamples) {
    if (!recording.load()) return;

    const juce::ScopedLock sl(trackLock);
    if (!recording.load()) return;

    for (auto& track : tracks) {
        if (!track.active || !track.writer) continue;
        int ch = track.inputChannel;
        if (ch < 0 || ch >= numInputChannels || !inputData[ch]) continue;

        // Mono: hand the writer a one-entry channel array pointing at this
        // track's input. write() only copies into the FIFO; the background
        // thread does the file I/O.
        const float* chans[1] = { inputData[ch] };
        if (track.writer->write(chans, numSamples))
            track.samplesRecorded += numSamples;
        else
            track.samplesDropped += numSamples;   // FIFO full - disk fell behind
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
    // Take the lock first so no callback is mid-write, then drop the writers.
    // Destroying a ThreadedWriter flushes its FIFO and closes the file.
    std::vector<TrackRecordState> finished;
    {
        const juce::ScopedLock sl(trackLock);
        recording = false;
        finished = std::move(tracks);
        tracks.clear();
    }
    for (auto& track : finished)
        track.writer.reset();

    droppedSamples = 0;
    for (auto& track : finished)
        droppedSamples += track.samplesDropped;

    for (auto& track : finished) {
        if (!track.active || track.samplesRecorded <= 0) {
            track.file.deleteFile();  // nothing captured; don't leave a 0-byte wav
            continue;
        }

        auto* node = graph.findNode(track.nodeId);
        if (!node) continue;

        int64_t numSamples = track.samplesRecorded;
        juce::File outFile = track.file;
        juce::String fileName = outFile.getFileName();

        // Create a clip on the track pointing to the recorded file.
        // track.startBeat is an *absolute* transport beat, but Clip::startBeat is
        // node-local: AudioTimelineProcessor adds node.absoluteBeatOffset back on
        // during playback. Subtract it here or a clip recorded onto a nested track
        // would double-count the nesting offset and play late.
        double durationBeats = (double)numSamples / sampleRate * (transport.bpm / 60.0);
        Clip clip;
        clip.name = fileName.toStdString();
        clip.startBeat = (float)(track.startBeat - node->absoluteBeatOffset);
        clip.lengthBeats = (float)durationBeats;
        clip.color = juce::Colours::red.withSaturation(0.6f).getARGB();
        clip.audioFilePath = outFile.getFullPathName().toStdString();
        node->clips.push_back(clip);

        // Disarm after recording
        node->recordArmed = false;

        juce::Logger::writeToLog("Recorded: " + outFile.getFullPathName()
            + " (" + juce::String(numSamples) + " samples, "
            + juce::String((double)numSamples / sampleRate, 1) + "s)");

        if (track.samplesDropped > 0)
            juce::Logger::writeToLog("  WARNING: dropped "
                + juce::String(track.samplesDropped) + " samples ("
                + juce::String((double) track.samplesDropped / sampleRate, 2)
                + "s) - the disk could not keep up, so this take has gaps.");
    }

    graph.dirty = true;
}

} // namespace SoundShop

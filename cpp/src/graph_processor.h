#pragma once
#include "node_graph.h"
#include "transport.h"
#include "automation.h"
#include "melody_player.h"
#include "audio_cache.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <unordered_map>
#include <map>

namespace SoundShop {

// Wraps our MIDI timeline node as a JUCE AudioProcessor
class MidiTimelineProcessor : public juce::AudioProcessor {
public:
    MidiTimelineProcessor(Node& node, Transport& transport);
    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double sr, int bs) override { sampleRate = sr; blockSize = bs; }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    MelodyPlayer melodyPlayer;
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
private:
    Node& node;
    Transport& transport;
    double sampleRate = 44100;
    int blockSize = 512;

    // MPE channel allocation (channels 2-16 = indices 0-14)
    struct MpeChannel {
        int clipIdx = -1, noteIdx = -1, pitch = -1;
        bool active = false;
    };
    static constexpr int kMpeChannels = 15;
    MpeChannel mpeChannels[kMpeChannels];
    int nextMpeChannel = 0;

    bool wasPlaying = false;   // detects playing->stopped transition for note-off

    int allocMpeChannel(int ci, int ni, int pitch);
    void freeMpeChannel(int ci, int ni);
    int findMpeChannel(int ci, int ni) const;
    void emitExpression(juce::MidiBuffer& midi, int mpeIdx,
                        const NoteExpression& expr, float beatInNote, int sampleOffset);
};

// Wraps an audio timeline node - plays audio file clips
class AudioTimelineProcessor : public juce::AudioProcessor {
public:
    AudioTimelineProcessor(Node& node, Transport& transport, NodeGraph& graph);
    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double sr, int bs) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
private:
    Node& node;
    Transport& transport;
    NodeGraph& graph;
    double sampleRate = 44100;
    int blockSize = 512;

    // Loaded audio files cached per clip
    struct LoadedAudio {
        std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
        std::unique_ptr<juce::AudioFormatReader> reader;
        int numChannels = 0;
        double fileSampleRate = 44100;
        int64_t totalSamples = 0;
    };
    std::map<std::string, std::shared_ptr<LoadedAudio>> audioCache;
    juce::AudioFormatManager formatManager;

    std::shared_ptr<LoadedAudio> getAudio(const std::string& path);
};

// Wraps a node that has no plugin (pass-through or test tone)
class PassthroughProcessor : public juce::AudioProcessor {
public:
    PassthroughProcessor(Node& node);
    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double sr, int bs) override { sampleRate = sr; }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
private:
    Node& node;
    double sampleRate = 44100;
    float phase = 0;
};

// Parallel MIDI generator that emits the MPE Configuration Message (the RPN
// "zone" handshake) into a hosted plugin's MIDI input, so MPE-capable plugins
// interpret incoming channels 2..16 as per-note member channels.
//
// Why a separate node wired *alongside* the real MIDI source (rather than
// transforming the note stream): it only ever ADDS the RPN handshake and never
// touches notes, so a non-MPE plugin - which simply ignores the unknown RPN -
// is byte-for-byte unaffected. And because it injects directly at the plugin's
// graph input, the handshake bypasses the cable-level MIDI-Learn CC filter that
// could otherwise strip the RPN's CC 6/38/100/101 bytes.
//
// Lifecycle: emits for the first few blocks after construction (covers plugin
// load, graph rebuild, and the MPE toggle - all of which recreate this node)
// and re-arms on every transport play-start edge (covers plugins that reset
// their zone layout when playback stops).
class MpeConfigProcessor : public juce::AudioProcessor {
public:
    MpeConfigProcessor(Node& n, Transport& t) : node(n), transport(t) {}
    const juce::String getName() const override { return "MPE Config"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override {
        buf.clear();
        const bool playing = transport.playing;
        if (playing && !wasPlaying) emitCountdown = kEmitBlocks;
        wasPlaying = playing;
        if (emitCountdown > 0) {
            --emitCountdown;
            // Lower zone: master channel 1, member channels 2..16 (15 of them).
            // Per-note bend range from the node (default 48 semis); master bend
            // range 2 to match the legacy/global pitch-wheel range used
            // elsewhere (see signal_modulation.h / TerrainSynth).
            const int pnRange = juce::jlimit(0, 96, node.mpePitchBendRange);
            const juce::MidiBuffer mcm = juce::MPEMessages::setLowerZone(15, pnRange, 2);
            for (const auto meta : mcm)
                midi.addEvent(meta.getMessage(), 0);
        }
    }
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return true; }
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
private:
    Node& node;
    Transport& transport;
    bool wasPlaying = false;
    static constexpr int kEmitBlocks = 4;
    int emitCountdown = kEmitBlocks;
};

class GraphProcessor {
public:
    GraphProcessor();

    void prepare(NodeGraph& graph, double sampleRate, int blockSize);
    void processBlock(NodeGraph& graph, Transport& transport,
                      float* const* outputData, int numChannels, int numSamples);

    // Rebuild the JUCE graph from our node graph
    void rebuildGraph(NodeGraph& graph, Transport& transport);

    double getSampleRate() const { return sampleRate; }
    int getBlockSize() const { return blockSize; }

    // Force a rebuild on the next audio callback (thread-safe)
    void requestRebuild() { rebuildRequested = true; }

    // Get the AudioProcessor for a given node ID (returns null if not in graph)
    juce::AudioProcessor* getProcessorForNode(int nodeId);

    // Automation
    AutomationManager& getAutomation() { return automation; }
    void applyAutomation(const std::vector<AutomationValue>& values);
    const std::unordered_map<int, juce::AudioProcessorGraph::NodeID>& getNodeMap() const { return nodeMap; }
    juce::AudioProcessorGraph* getGraph() { return processorGraph.get(); }
    AudioCacheManager& getCacheManager() { return cacheManager; }

private:
    std::unique_ptr<juce::AudioProcessorGraph> processorGraph;
    double sampleRate = 44100.0;
    int blockSize = 512;

    // Map our node IDs to JUCE graph node IDs.
    // nodeMap stores the OUTPUT side: the JUCE node that downstream connections
    // should pull audio FROM. For nodes with a pan inserted after them, this is
    // the pan node - pan is where the chain ends.
    // nodeInputMap stores the INPUT side: the JUCE node that upstream connections
    // should push audio/MIDI INTO. For nodes with a pan, this is the original
    // processor (not the pan), so MIDI events actually reach the synth.
    // For nodes without a pan (Output), both maps point to the same JUCE node.
    std::unordered_map<int, juce::AudioProcessorGraph::NodeID> nodeMap;
    std::unordered_map<int, juce::AudioProcessorGraph::NodeID> nodeInputMap;
    juce::AudioProcessorGraph::NodeID outputNodeId;
    AutomationManager automation;
    AudioCacheManager cacheManager;

    int graphVersion = 0;  // increments when graph needs rebuild
    int lastNodeCount = 0;
    int lastLinkCount = 0;
    std::atomic<bool> rebuildRequested{false};
};

} // namespace SoundShop

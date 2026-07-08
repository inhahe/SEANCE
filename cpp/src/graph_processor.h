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
#include <mutex>

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

// Cable-level MIDI adapter inserted on a MIDI cable whose destination is a
// hosted VST3/AU plugin. It is the ONLY part of the graph that knows BOTH the
// project-wide tuning AND that the destination is a hosted plugin (vs a native
// synth that tunes itself via Transport::noteToFreq), so per the architecture
// this is where note->frequency delivery for plugins is reconciled.
//
// Responsibilities, decided per-destination from dstNode.mpeEnabled:
//   - Plugin in MPE mode  -> ensure each note has its own member channel
//     (2..16), spreading a single-channel source if needed, and add the FULL
//     per-note tuning bend (concert pitch + temperament), summed with any
//     expression pitch-bend already in the stream.
//   - Plugin NOT in MPE   -> collapse everything to channel 1 and apply only
//     the UNIFORM concert-pitch bend (temperament can't be per-note on a shared
//     channel; this is the documented "needs MPE" fallback).
// When the project is at default tuning (12-TET / A440) it is a pure pass-
// through. Native-synth cables never get this node.
class MidiTuningAdapterProcessor : public juce::AudioProcessor {
public:
    MidiTuningAdapterProcessor(Node& dstNode, Transport& transport)
        : dstNode(dstNode), transport(transport) {}
    const juce::String getName() const override { return "MIDI Tuning Adapter"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return true; }
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
    Node& dstNode;
    Transport& transport;

    // Output voice slots for MPE mode: index i -> MIDI member channel i+2.
    // A voice maps an incoming (channel,pitch) note to the member channel we
    // assigned it, so expression/CC/note-off on that input note follow it.
    struct Voice { bool active = false; int inCh = 0; int pitch = -1; float exprSemis = 0.0f; };
    static constexpr int kMembers = 15;
    Voice voices[kMembers];
    int nextVoice = 0;

    bool wasPlaying = false;
    // Collapse mode sends an RPN to pin the plugin's channel-1 bend range to a
    // small, near-universal value so the (sub-semitone) concert-pitch bend is
    // interpreted correctly even by plugins that default to +/-2.
    static constexpr int kCollapseRange = 2; // semitones
    int rpnCountdown = 4;

    int allocVoice(int inCh, int pitch);
    int findVoice(int inCh, int pitch) const;
    static int encodeBend(float semis, int rangeSemis);
};

class GraphProcessor {
public:
    GraphProcessor();

    void prepare(NodeGraph& graph, double sampleRate, int blockSize);
    void processBlock(NodeGraph& graph, Transport& transport,
                      float* const* outputData, int numChannels, int numSamples);

    // Rebuild the JUCE graph from our node graph
    void rebuildGraph(NodeGraph& graph, Transport& transport);

    // Build the built-in AudioProcessor for a "normal" node (any synth, effect,
    // timeline, SignalShape/Script, etc.), returning a freshly-owned processor.
    // This is the graph-agnostic factory: it depends ONLY on the node, the
    // transport, and the graph it belongs to - no GraphProcessor member state -
    // so it can populate an INNER graph (e.g. a per-voice subgraph inside a
    // Voice container) exactly as it populates the main graph. The caller is
    // responsible for the main-graph-only special cases that this does NOT
    // handle: the Output sink, hosted-plugin instance ownership transfer, and
    // cache-playback substitution. Returns a PassthroughProcessor for anything
    // unrecognized (never null). See poly-voice-architecture.md.
    static std::unique_ptr<juce::AudioProcessor> createNodeProcessor(
        Node& node, Transport& transport, NodeGraph& graph);

    double getSampleRate() const { return sampleRate; }
    int getBlockSize() const { return blockSize; }

    // Force a rebuild on the next audio callback (thread-safe)
    void requestRebuild() { rebuildRequested = true; }

    // Request an immediate "panic": on the next audio callback, reset every
    // processor in the graph so all trailing sound (synth release tails,
    // reverb/echo/delay buffers, sustained voices, plugin tails) is silenced
    // at once instead of ringing out. Thread-safe (UI thread sets the flag;
    // the audio thread consumes it). Used by the transport Stop so pressing
    // Stop cuts the sound dead, the way every DAW's Stop does. The graph keeps
    // processing afterwards (musical-typing audition still works while stopped)
    // - this is a one-shot state wipe, not a permanent mute.
    void requestPanic() { panicRequested = true; }

    // Scope filter for rebuildGraph (per-voice polyphony, see
    // poly-voice-architecture.md). -1 (default) = the main/top-level graph:
    // build only nodes whose voiceContainerId == -1 (top level). >= 0 = build
    // only the inner subgraph of that VoiceContainer (nodes whose
    // voiceContainerId == scope). PolyVoiceProcessor sets this to the container
    // id on each per-voice GraphProcessor so the same connection-builder wires
    // the inner clone. The cross-scope link filtering is automatic: a link is
    // only wired if BOTH endpoints were built in this scope (they land in
    // nodeMap/nodeInputMap), so out-of-scope links are skipped by the existing
    // membership guard in the link loop.
    void setBuildScope(int s) { buildScope = s; }

    // Get the AudioProcessor for a given node ID (returns null if not in graph)
    juce::AudioProcessor* getProcessorForNode(int nodeId);

    // Snapshot of every node's own audio latency in samples, keyed by stable
    // node id (settled after the last graph prepare; a missing id means 0 - the
    // node reported no latency, or the graph hasn't been built yet). Thread-safe
    // (locks the latency listener). The UI uses this for the per-node and
    // cumulative ("to here") latency readouts in the node right-click menu.
    std::unordered_map<int, int> snapshotNodeLatencies() { return latencyListener.snapshot(); }

    // A hosted-plugin parameter event captured from a plugin's own editor. kind:
    // 0=gestureBegin 1=gestureEnd 2=parameterChanged; value carries the normalized
    // value for kind 2. Queued by the listener (off the audio/plugin thread),
    // drained by the UI timer via drainParamEvents().
    struct ParamEvent { int nodeId; int paramIdx; int kind; float value; };

    // Drain queued hosted-plugin parameter events (knob-drags the user made in a
    // plugin's own editor). Thread-safe; called by the UI timer to feed
    // automation recording. Returns and clears the pending events.
    std::vector<ParamEvent> drainParamEvents() {
        std::lock_guard<std::mutex> lk(latencyListener.mtx);
        std::vector<ParamEvent> out;
        out.swap(latencyListener.paramEvents);
        return out;
    }

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

    // Plugin-delay-compensation upkeep. juce::AudioProcessorGraph already
    // inserts the compensating delay lines (it builds them from each member's
    // getLatencySamples() when the render sequence is prepared - see
    // RenderSequenceBuilder in juce_AudioProcessorGraph.cpp), so SEANCE gets
    // PDC for free for hosted plugins and any built-in that reports latency.
    // The one thing the graph does NOT do is rebuild itself when a member's
    // latency CHANGES at runtime (e.g. a plugin toggling oversampling/lookahead,
    // or a future built-in switching to a latency-bearing high-quality mode).
    // JUCE notes "if the latency of a node changes, the graph should be rebuilt"
    // and leaves that to the owner. This listener is attached to every graph
    // node; on a latencyChanged notification it flags a rebuild so the render
    // sequence (and thus the delay compensation) is recomputed.
    //
    // CRITICAL: JUCE fires audioProcessorChanged(latencyChanged=true) on every
    // re-prepare, even when the latency value is identical. Acting on that
    // blindly creates an infinite loop: rebuild -> prepareToPlay -> latencyChanged
    // -> flag rebuild -> rebuild... (observed at 1736 rebuilds/session, saturating
    // the CPU and hanging the app). So we only flag a rebuild when the latency
    // VALUE actually changes. Because rebuildGraph recreates every processor (new
    // pointers each time), the value cache is keyed by STABLE node id, not by
    // processor pointer. procNodeId maps the current processors to their node ids
    // (rebuilt each rebuild via beginRebuild/track); lastLatencyByNode persists
    // the last settled latency per node and is refreshed by commitLatencies()
    // AFTER prepareToPlay, so the post-prepare notification converges to a no-op.
    struct LatencyChangeListener : juce::AudioProcessorListener {
        std::atomic<bool>* rebuildFlag = nullptr;
        std::mutex mtx;
        std::unordered_map<juce::AudioProcessor*, int> procNodeId; // rebuilt each rebuild
        std::unordered_map<int, int> lastLatencyByNode;            // persists across rebuilds

        void beginRebuild() {
            std::lock_guard<std::mutex> lk(mtx);
            procNodeId.clear();
        }
        void track(juce::AudioProcessor* p, int nodeId) {
            if (!p) return;
            std::lock_guard<std::mutex> lk(mtx);
            procNodeId[p] = nodeId;
        }
        void commitLatencies() {
            std::lock_guard<std::mutex> lk(mtx);
            for (auto& kv : procNodeId)
                lastLatencyByNode[kv.second] = kv.first->getLatencySamples();
        }
        // Thread-safe copy of the settled per-node latencies for the UI.
        std::unordered_map<int, int> snapshot() {
            std::lock_guard<std::mutex> lk(mtx);
            return lastLatencyByNode;
        }
        // -------- Plugin-param user-gesture capture (automation recording) -----
        // This listener is already attached to every processor, so it doubles as
        // the hook for recording knob-drags the user makes inside a hosted
        // plugin's OWN editor window. We can't touch the NodeGraph from whatever
        // thread the plugin notifies on, so events are queued here and drained by
        // the UI timer (drainParamEvents). kind: 0=gestureBegin 1=gestureEnd
        // 2=parameterChanged. `value` carries the normalized value for kind 2.
        std::vector<ParamEvent> paramEvents;  // GraphProcessor::ParamEvent
        void pushParamEvt(juce::AudioProcessor* p, int idx, int kind, float value) {
            std::lock_guard<std::mutex> lk(mtx);
            auto it = procNodeId.find(p);
            if (it == procNodeId.end()) return; // unknown processor - ignore
            if (paramEvents.size() < 4096)      // bound memory if UI stalls
                paramEvents.push_back({ it->second, idx, kind, value });
        }
        void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor* p, int idx) override {
            pushParamEvt(p, idx, 0, 0.0f);
        }
        void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor* p, int idx) override {
            pushParamEvt(p, idx, 1, 0.0f);
        }
        void audioProcessorParameterChanged(juce::AudioProcessor* p, int idx, float v) override {
            pushParamEvt(p, idx, 2, v);
        }
        void audioProcessorChanged(juce::AudioProcessor* p,
                                   const ChangeDetails& details) override {
            if (!details.latencyChanged || !rebuildFlag || !p) return;
            int now = p->getLatencySamples();
            {
                std::lock_guard<std::mutex> lk(mtx);
                auto it = procNodeId.find(p);
                if (it == procNodeId.end()) return; // unknown processor - ignore
                auto lit = lastLatencyByNode.find(it->second);
                if (lit != lastLatencyByNode.end() && lit->second == now)
                    return; // latency unchanged - no rebuild (breaks the loop)
            }
            rebuildFlag->store(true);
        }
    };
    LatencyChangeListener latencyListener;

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
    std::atomic<bool> panicRequested{false};

    // Which scope rebuildGraph builds (see setBuildScope). -1 = top level.
    int buildScope = -1;
};

} // namespace SoundShop

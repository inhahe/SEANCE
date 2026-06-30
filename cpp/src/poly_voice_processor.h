#pragma once
#include "node_graph.h"
#include "transport.h"
#include "graph_processor.h"
#include "voice_nodes.h"
#include "voice_allocator.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <vector>

namespace SoundShop {

// =============================================================================
// PolyVoiceProcessor  -  the engine behind a VoiceContainer node
// =============================================================================
//
// One juce::AudioProcessor, inserted as a SINGLE node in the main graph (so
// main-graph PDC, mixing and routing treat it like any instrument: MIDI in,
// stereo audio out). Internally it runs N independent realisations - "voices" -
// of the container's inner patch and sums their outputs.
//
// Each voice is its own GraphProcessor built with buildScope = the container's
// id, so GraphProcessor::rebuildGraph wires the inner subgraph (the nodes whose
// voiceContainerId == containerId) using the EXACT same connection-builder the
// main graph uses - control-channel widening, pan, signal cables and all. No
// wiring logic is duplicated here. See poly-voice-architecture.md.
//
// MIDI handling: the container parses its incoming MidiBuffer itself and does
// NOT forward it wholesale into the patches. Instead it allocates a voice per
// note and drives that voice's VoiceInProcessor (the inner "Voice In" puck),
// which emits the note into its clone and exposes Pitch/Gate/Velocity signals.
//
// v1 scope (M1): fixed N, steal-oldest, RMS voice-free, no hosted plugins inside
// a voice (one Node, N processor clones - the plugin instance can only move into
// one), no nested containers.
class PolyVoiceProcessor : public juce::AudioProcessor {
public:
    PolyVoiceProcessor(Node& containerNode, NodeGraph& graph, Transport& transport);

    const juce::String getName() const override { return containerNode.name; }
    void prepareToPlay(double sr, int bs) override;
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
    Node& containerNode;
    NodeGraph& graph;
    Transport& transport;
    int containerId = -1;

    double sampleRate = 44100.0;
    int blockSize = 512;

    // Per-slot audio machinery. The allocation/lifecycle bookkeeping (active,
    // gate, note, age, silence) lives in `alloc` and is indexed by the same slot
    // number, so VoiceAllocator can be unit-tested without the audio graph.
    struct Voice {
        std::unique_ptr<GraphProcessor> gp;
        VoiceInProcessor* voiceIn = nullptr; // owned by gp's inner graph
        juce::AudioBuffer<float> scratch;
        // Per-voice stereo balance gains for its slot in a unison stack (1,1 =
        // centred / no spread). Set when the container assigns the slot to a
        // unison position; applied when the voice's audio is summed into the mix.
        float panGainL = 1.0f, panGainR = 1.0f;
    };
    std::vector<Voice> voices;
    VoiceAllocator alloc;
    bool built = false;

    // A released voice is freed once its output RMS stays below this floor for
    // kFreeMs (envelope-tail done). Tuned conservatively so tails aren't cut.
    static constexpr float kFloorRms = 1.0e-4f;
    static constexpr float kFreeMs   = 250.0f;

    void buildVoices();
    static VoiceInProcessor* findVoiceIn(GraphProcessor& gp);
};

} // namespace SoundShop

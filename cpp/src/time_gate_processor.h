#pragma once
#include "effect_regions.h"
#include "node_graph.h"
#include "transport.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

namespace SoundShop {

// A transparent gain processor inserted on a time-gated link. When the current
// beat falls inside one of its effect regions, audio passes through (wet=1).
// Outside all regions, audio is silenced (wet=0). At region boundaries, a
// crossfade of configurable duration smooths the transition.
//
// The processor reads the Transport for the current beat position each block.
// Effect regions are read from the source Node's effectRegions vector. This is
// technically a race with the UI thread, but benign on x86 for v1 — proper
// thread safety is a follow-up (atomic shared_ptr swap of the regions list).

class TimeGateProcessor : public juce::AudioProcessor {
public:
    // linkId: the Link this gate sits on. Regions matching this linkId (or
    // whose groupId matches a group containing this linkId) control the gate.
    TimeGateProcessor(int linkId, Node& sourceNode, NodeGraph& graph, Transport& transport);

    const juce::String getName() const override { return "TimeGate"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
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
    int linkId;
    Node& sourceNode;
    NodeGraph& graph;
    Transport& transport;
    double sampleRate = 44100;

    // Compute the gate's wet amount (0..1) at a given beat, considering all
    // applicable regions (direct linkId match + group membership).
    float computeWet(float beat) const;
};

} // namespace SoundShop

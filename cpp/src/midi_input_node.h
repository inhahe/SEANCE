#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace SoundShop {

// ============================================================================
// MidiInputProcessor — turns a physical/virtual MIDI input source into a
// first-class graph node. The audio engine pushes incoming MIDI events
// into the node's pendingMpePassthrough queue (which is also used by old
// MPE timelines — same mechanism, repurposed for the new routing model).
// This processor drains the queue at the start of each block and emits
// the events on its own MIDI output, so downstream nodes wired to it
// receive them via normal graph MIDI routing.
//
// One instance per input source: "Computer Keyboard", each hardware MIDI
// device, each network MIDI client, etc. The user wires the node's MIDI
// Out pin to wherever they want the events to go.
// ============================================================================

class MidiInputProcessor : public juce::AudioProcessor {
public:
    MidiInputProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override {
        buf.clear();
        std::lock_guard<std::mutex> lock(*node.mpePassthroughMutex);
        for (auto& [offset, msg] : node.pendingMpePassthrough)
            midi.addEvent(msg, offset);
        node.pendingMpePassthrough.clear();
    }
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return false; } // we generate MIDI, don't receive it
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
};

} // namespace SoundShop

#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <string>

namespace SoundShop {

// ============================================================================
// MIDI Modulator — "signal + MIDI -> MIDI" node.
//
// Takes one MIDI input and N user-configurable Signal inputs; each signal
// modulates some attribute of the passing MIDI stream. Each input has a
// rule: pick a target (Velocity, Pitch Bend, Mod Wheel, Aftertouch, or an
// arbitrary CC number) plus an Amount knob.
//
// - Velocity rules scale the velocity of each passing note-on at the
//   sample-accurate offset of the note.
// - Continuous targets (bend/CC/aftertouch) are emitted once per block
//   based on the signal value at the block midpoint. Good enough for
//   LFO/envelope modulation; not sample-accurate.
//
// The whole config is stored in node.script as a small text blob so
// adding/removing inputs persists across save/load without schema changes.
// ============================================================================

enum class ModTarget {
    Velocity,     // scale note-on velocity
    PitchBend,    // emit pitch-wheel messages
    ModWheel,     // emit CC#1 messages
    Aftertouch,   // emit channel pressure messages
    CC            // emit CC messages with a user-picked number
};

struct ModRule {
    ModTarget target = ModTarget::Velocity;
    float     amount = 1.0f;  // meaning depends on target (usually 0..2)
    int       ccNumber = 74;  // only used when target == CC
};

struct MidiModDoc {
    std::vector<ModRule> rules;

    std::string encode() const;
    bool decode(const std::string& s);
    static MidiModDoc defaultDoc();           // one velocity rule
    static MidiModDoc fromLegacyVelScale();   // backward compat for __velscale__
};

// ============================================================================
// Processor
// ============================================================================

class MidiModulatorProcessor : public juce::AudioProcessor {
public:
    MidiModulatorProcessor(Node& n) : node(n) {
        cachedScript = node.script;
        if (cachedScript == "__velscale__")
            doc = MidiModDoc::fromLegacyVelScale();
        else if (!doc.decode(cachedScript))
            doc = MidiModDoc::defaultDoc();
    }

    const juce::String getName() const override { return "MIDI Mod"; }
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
    Node& node;
    MidiModDoc doc;
    std::string cachedScript;

    // Remember the last MIDI channel we saw a note-on on, so when we
    // emit continuous modulation messages (pitch bend / CC / aftertouch)
    // we target a channel that matches actual activity. Defaults to 1.
    int lastSeenChannel = 1;

    void rereadDocIfChanged();
};

// ============================================================================
// Editor — small dialog for configuring rules + signal input count
// ============================================================================

class MidiModEditorComponent : public juce::Component {
public:
    MidiModEditorComponent(NodeGraph& graph, int nodeId, std::function<void()> onApply);

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    NodeGraph& graph;
    int nodeId;
    std::function<void()> onApply;
    MidiModDoc doc;

    juce::TextButton addInputBtn { "+ Input" };
    juce::TextButton applyBtn    { "Apply" };
    juce::TextButton closeBtn    { "Close" };

    juce::Viewport rulesViewport;
    juce::Component rulesContainer;

    class RuleRow;
    std::vector<std::unique_ptr<RuleRow>> rows;

    void rebuildRows();
    void commitToNode();
    void onDocChanged();
    // Mutates the node's pinsIn to match the current rule count.
    void syncNodePins();
};

} // namespace SoundShop

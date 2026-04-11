#pragma once
#include "node_graph.h"
#include "audio_engine.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace SoundShop {

// Simple dialog shown on new-project creation (and manually via menu) that
// lists all detected MIDI input devices with checkboxes so the user can
// pick which ones to add to the graph. Devices already present in the
// graph are shown as checked-and-disabled ("already added"). Clicking
// Add creates a MidiInput node per newly-checked device and runs a
// callback so the caller can request a graph rebuild.
//
// The wizard is narrow-scope on purpose: just MIDI inputs right now.
// Audio inputs and audio outputs will get their own sections / wizards
// in Phases 2b and 2c. Can easily extend this into a three-section
// dialog later without changing the model.
class MidiDeviceWizardComponent : public juce::Component {
public:
    MidiDeviceWizardComponent(NodeGraph& graph,
                              AudioEngine& engine,
                              std::function<void()> onApply);

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    NodeGraph& graph;
    AudioEngine& engine;
    std::function<void()> onApply;

    juce::Label headerLabel;
    juce::Label hintLabel;
    juce::TextButton addBtn { "Add Selected" };
    juce::TextButton selectAllBtn { "Select All" };
    juce::TextButton skipBtn { "Skip" };

    struct Row {
        std::unique_ptr<juce::ToggleButton> toggle;
        juce::String identifier;
        juce::String name;
        bool alreadyAdded = false;
    };
    std::vector<Row> rows;

    juce::Viewport viewport;
    juce::Component content;

    void rebuildList();
    void closeSelf();
};

} // namespace SoundShop

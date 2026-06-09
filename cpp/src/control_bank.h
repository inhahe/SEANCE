#pragma once
#include "node_graph.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <vector>

namespace SoundShop {

// Control Bank node editor.
//
// A Control Bank is a NodeType::SignalShape node tagged with the
// "__controlbank__" script. It exposes N user-defined sliders, each emitting
// one control-signal output pin (PinKind::Signal, mono 0..1). The slider's
// value IS the output value, written straight onto the pin's control channel
// every block by SignalShapeProcessor's __controlbank__ branch. Wire any
// output to a param-arming (orange) input or a Signal/Mod input to drive it by
// hand - it's a bank of manual macro faders.
//
// The editor window is resizable: stretching it lengthens the sliders, and a
// longer JUCE linear slider maps the same 0..1 range across more pixels, so
// dragging it becomes proportionally more precise. An orientation toggle
// switches between vertical faders (laid out in a row; window height = length)
// and horizontal sliders (stacked; window width = length).
//
// Slider count, names and the orientation flag round-trip through the project
// file: each slider is a Node::Param (name/value/min=0/max=1) paired 1:1 with a
// Signal output Pin of the same name, and the orientation lives in node.script
// ("__controlbank__" = vertical, "__controlbank__:h" = horizontal).
class ControlBankComponent : public juce::Component {
public:
    // graph/nodeId: the Control Bank node. onChanged: invoked after any edit
    // that the audio graph must observe (value move, slider add/remove, rename,
    // orientation flip) - wired to GraphProcessor::requestRebuild via the
    // graph component's onNodeEdited.
    ControlBankComponent(NodeGraph& graph, int nodeId, std::function<void()> onChanged);

    void paint(juce::Graphics& g) override;
    void resized() override;

    // Hard limits on slider count. One slider minimum (a zero-output node is
    // pointless); 32 is a generous ceiling that still lays out legibly.
    static constexpr int kMinSliders = 1;
    static constexpr int kMaxSliders = 32;

private:
    NodeGraph& graph;
    int nodeId;
    std::function<void()> onChanged;

    bool vertical = true; // false = horizontal sliders

    struct Row {
        std::unique_ptr<juce::Slider> slider;
        std::unique_ptr<juce::Label>  name;   // editable label
        std::unique_ptr<juce::TextButton> removeBtn;
        int pinId = -1; // the Signal output pin this slider drives
    };
    std::vector<Row> rows;

    juce::Label    titleLabel;
    juce::TextButton addBtn{"+ Slider"};
    juce::ToggleButton orientationToggle{"Horizontal sliders"};

    Node* node() { return graph.findNode(nodeId); }

    void rebuildRows();        // (re)build slider widgets from node.params/pins
    void addSlider();          // append one slider+pin
    void removeSlider(int idx);// drop one slider+pin (and its cables)
    void renameSlider(int idx, const juce::String& newName);
    void writeOrientationToScript();
    void applyOrientationStyles();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ControlBankComponent)
};

} // namespace SoundShop

#pragma once
#include "node_graph.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace SoundShop {

// A 2D XY pad performance controller. Each axis (X, Y, Z/scroll) directly
// controls a parameter on any node in the graph, selected via dropdowns.
// No graph wiring needed — the pad writes to the target param each frame.
//
// Opened from the menu (not a graph node). Each axis has a dropdown:
//   "Node: Piano → Param: Pan"
// Dragging the handle or scrolling immediately updates the target param.

struct XYAxisBinding {
    int nodeId = -1;
    int paramIdx = -1;
};

class XYPadComponent : public juce::Component, private juce::Timer {
public:
    // nodeId: the XY Pad node in the graph (stores X/Y/Z params and has
    // signal output pins). Also provides dropdown fast-path for direct
    // param control without wiring.
    XYPadComponent(NodeGraph& graph, int nodeId);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    void timerCallback() override;

private:
    NodeGraph& graph;
    int nodeId;  // the XY Pad node in the graph

    float padX = 0.5f;  // 0..1
    float padY = 0.5f;  // 0..1
    float padZ = 0.5f;  // 0..1 (scroll wheel)

    XYAxisBinding xBind, yBind, zBind;

    juce::ComboBox xCombo{"X axis"}, yCombo{"Y axis"}, zCombo{"Z axis (scroll)"};
    juce::Label xLabel, yLabel, zLabel;

    void rebuildCombos();
    void applyBinding(const XYAxisBinding& bind, float value01);
    void updateNodeParams(); // write X/Y/Z to the node's params for signal output
    std::pair<float, float> mouseToXY(juce::Point<float> pos) const;
    juce::Rectangle<float> getPadArea() const;

    // Combo items encode nodeId + paramIdx as a single int:
    //   itemId = nodeId * 1000 + paramIdx + 1  (0 = "None")
    void parseComboId(int itemId, int& nodeId, int& paramIdx);
    int makeComboId(int nodeId, int paramIdx);
};

} // namespace SoundShop

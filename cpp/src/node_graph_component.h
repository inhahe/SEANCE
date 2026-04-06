#pragma once
#include "node_graph.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace SoundShop {

class NodeGraphComponent : public juce::Component {
public:
    NodeGraphComponent(NodeGraph& graph);

    void paint(juce::Graphics& g) override;
    void resized() override;

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& key) override;

    void fitAll();

    // Callbacks
    std::function<void(Node&)> onOpenEditor;
    std::function<void(int)> onShowPluginUI;    // called with node ID
    std::function<void(int)> onShowPluginInfo;   // called with node ID
    std::function<void(int)> onShowPluginPresets; // called with node ID
    std::function<void(int)> onShowMidiMap;       // called with node ID
    std::function<void(int)> onFreezeNode;        // called with node ID
    std::function<void(int)> onRunScript;         // called with node ID

    // Convert between screen and canvas coordinates
    juce::Point<float> screenToCanvas(juce::Point<float> screen) const;
    juce::Point<float> canvasToScreen(juce::Point<float> canvas) const;

private:
    NodeGraph& graph;

    // View transform
    float zoom = 1.0f;
    juce::Point<float> panOffset{0, 0};

    // Interaction state
    enum class DragMode { None, Pan, MoveNode, DragLink, SelectBox };
    DragMode dragMode = DragMode::None;
    int dragNodeId = -1;
    int dragPinId = -1;       // pin we're dragging a link from
    bool dragPinIsOutput = true;
    juce::Point<float> dragStart;
    juce::Point<float> dragCurrent;
    int selectedNodeId = -1;
    int selectedLinkId = -1;

    // Drawing helpers
    void drawGrid(juce::Graphics& g);
    void drawNode(juce::Graphics& g, Node& node);
    void drawLink(juce::Graphics& g, Link& link);
    void drawPendingLink(juce::Graphics& g);
    juce::Colour getNodeColor(const Node& node) const;

    // Hit testing
    Node* nodeAtPoint(juce::Point<float> canvasPos);
    int pinAtPoint(juce::Point<float> canvasPos, bool& isOutput);
    int linkAtPoint(juce::Point<float> canvasPos);
    juce::Rectangle<float> getNodeBounds(const Node& node) const;
    juce::Point<float> getPinPosition(const Node& node, const Pin& pin) const;

    // Context menu
    void showBackgroundMenu(juce::Point<float> canvasPos);
    void showNodeMenu(Node& node);
    void showLinkMenu(int linkId);

    // Helpers
    void deleteSelectedLink();
    void deleteSelectedNode();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NodeGraphComponent)
};

} // namespace SoundShop

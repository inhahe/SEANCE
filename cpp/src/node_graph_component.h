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
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& key) override;

    void fitAll();

    // After a project load (or any external mutation of graph.viewZoom),
    // re-evaluate which view to show: restore the saved pan/zoom if one was
    // persisted (graph.viewZoom > 0), otherwise fit-all. Called by
    // main_window after ProjectFile::load. Sets pendingInitialFit so the
    // decision actually applies on the next paint/resized once we have a
    // real size.
    void notifyProjectLoaded();

    // Callbacks
    std::function<void(Node&)> onOpenEditor;
    std::function<void()> onNodeEdited;         // called when a node's data changed; wire to graphProcessor.requestRebuild()
    std::function<void(int)> onNodeDeleted;     // called with node ID when a node is about to be removed
    std::function<void(int)> onShowPluginUI;    // called with node ID
    std::function<void(int)> onShowPluginInfo;   // called with node ID
    std::function<void(int)> onShowPluginPresets; // called with node ID
    std::function<void(int)> onShowMidiMap;       // called with node ID
    std::function<void(int)> onFreezeNode;        // called with node ID
    std::function<void(int)> onRunScript;         // called with node ID
    std::function<void(juce::String)> onOpenHelpDoc; // called with docs/<file> relative path
    // Fire a one-shot manual trigger on the live SignalShape processor for
    // node `id`. main_window wires this to GraphProcessor::getProcessorForNode
    // + dynamic_cast<SignalShapeProcessor*> + fireManualTrigger(). No-op if
    // the processor isn't a SignalShape (defensive - the editor only enables
    // its Manual Trigger button when the lookup succeeds, but the audio
    // graph may have rebuilt since the dialog opened).
    std::function<void(int)> onSignalShapeManualTrigger;

    // Returns the audio graph's live {sampleRate, blockSize}. Wired by
    // main_window to the audio engine. Used by the cable right-click menu to
    // show the exact Param update rate (sampleRate / blockSize) and the block
    // size, instead of a hard-coded approximation. May be null before wiring,
    // or return sampleRate <= 0 before the audio device has started - callers
    // must fall back to a generic label in that case.
    std::function<std::pair<double, int>()> getAudioFormat;

    // Convert between screen and canvas coordinates
    juce::Point<float> screenToCanvas(juce::Point<float> screen) const;
    juce::Point<float> canvasToScreen(juce::Point<float> canvas) const;

private:
    NodeGraph& graph;

    // View transform
    float zoom = 1.0f;
    juce::Point<float> panOffset{0, 0};

    // True until the first resized()/paint() callback applies the initial
    // view (either restoring the saved pan/zoom from graph.viewZoom/PanX/PanY
    // or running fitAll() as a fallback). Prevents the user from briefly
    // seeing nodes at the default zoom/pan before that decision, which used
    // to look like a tacky zoom-in animation on every project load. Reset
    // to true by notifyProjectLoaded() so a mid-session "Load Project"
    // reapplies the saved view from the newly-loaded graph.
    bool pendingInitialFit = true;

    // Push the live zoom/panOffset back into graph.viewZoom/PanX/PanY so
    // the next project save records the user's current view. Called after
    // any interaction that mutates the view (wheel zoom, pan drag,
    // fitAll). NodeGraph itself owns the saved view; the component just
    // mirrors its own working values into the graph as they change.
    void publishViewState();

    // Interaction state
    enum class DragMode { None, Pan, MoveNode, DragLink, SelectBox, DragParam };
    DragMode dragMode = DragMode::None;
    int dragNodeId = -1;
    int dragPinId = -1;       // pin we're dragging a link from
    bool dragPinIsOutput = true;
    int dragHoverPinId = -1;  // pin currently hovered while dragging a link
                              //   (drop target if released here, -1 if none)
    int dragParamIdx = -1;    // index into node.params when dragMode == DragParam
    float dragParamStartValue = 0.0f;
    float dragParamLeftX = 0.0f;   // canvas-space left edge of the slider's track
    float dragParamWidth = 1.0f;   // canvas-space width of the slider's track
    juce::Point<float> dragStart;
    juce::Point<float> dragCurrent;
    int selectedNodeId = -1;
    int selectedLinkId = -1;
    int hoveredLinkId = -1;   // cable currently within right-click distance of
                              //   the cursor (highlighted so the user can see
                              //   what a right-click / click will target)

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

    // Delete `rootId` along with every descendant if it's a Group
    // container. Walks the group tree so nested groups cascade too,
    // then removes connected links, closes any open editors, fires
    // onNodeDeleted, unlinks the root from its parent group (if any),
    // and removes everything from graph.nodes. Pushes one undo
    // snapshot for the whole operation.
    void deleteNodeAndDescendants(int rootId);

    // Build a TerrainSynth node with the requested name, script, and
    // dimensionality (1..8). Creates MIDI + N Signal input pins, an Audio
    // output pin, and the standard envelope/volume/pan/traversal/grain
    // params plus per-axis Radius/Center pairs. Used by both the
    // synchronous menu paths (sin*cos, noise, image, audio) and the
    // async N-D custom-expression dialog callback.
    Node& makeTerrainNode(const std::string& name,
                          const std::string& script,
                          juce::Point<float> canvasPos,
                          int numDims = 2);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NodeGraphComponent)
};

} // namespace SoundShop

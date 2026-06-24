#pragma once
#include "node_graph.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace SoundShop {

class NodeGraphComponent : public juce::Component,
                           public juce::TooltipClient {
public:
    NodeGraphComponent(NodeGraph& graph);

    void paint(juce::Graphics& g) override;
    void resized() override;

    // TooltipClient: when the mouse rests over a pin that carries a
    // Pin::tooltip (e.g. a synth "Pressure" input or a MIDI Breakout output),
    // return that text so the app's TooltipWindow shows it. Returns "" over
    // empty space, nodes, or pins with no tooltip.
    juce::String getTooltip() override;

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

    // Returns each node's own audio latency in samples, keyed by stable node id
    // (a missing id means 0). Wired by main_window to
    // GraphProcessor::snapshotNodeLatencies(). Used by the node right-click menu
    // to show this node's own latency and the cumulative latency accumulated
    // along the longest path of nodes feeding it. May be null before wiring;
    // returns an empty map before the audio graph has been built.
    std::function<std::unordered_map<int, int>()> getNodeLatencies;

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
    // wantInput: -1 = accept any pin, 0 = only output pins, 1 = only input
    // pins. Returns the CLOSEST matching pin to canvasPos (or -1). Drag/drop
    // pass the opposite of the source pin's direction so a target never
    // resolves to the wrong side / the source pin itself.
    int pinAtPoint(juce::Point<float> canvasPos, bool& isOutput, int wantInput = -1);
    int linkAtPoint(juce::Point<float> canvasPos);
    juce::Rectangle<float> getNodeBounds(const Node& node) const;
    juce::Point<float> getPinPosition(const Node& node, const Pin& pin) const;

    // Context menu
    void showBackgroundMenu(juce::Point<float> canvasPos);
    void showNodeMenu(Node& node);
    void showLinkMenu(int linkId);

    // Cumulative audio latency (in samples) accumulated at the OUTPUT of `nodeId`:
    // the node's own latency plus the largest summed latency along any path of
    // nodes feeding its inputs (the value JUCE's delay compensation aligns to).
    // `ownLatency` is a snapshot from getNodeLatencies() (missing id => 0); `memo`
    // and `visiting` are scratch maps the caller default-constructs (memo caches
    // results, visiting guards against feedback cycles). Pure graph walk over
    // graph.links/pins - no audio-thread access.
    int cumulativeLatencyTo(int nodeId,
                            const std::unordered_map<int, int>& ownLatency,
                            std::unordered_map<int, int>& memo,
                            std::unordered_set<int>& visiting) const;

    // Transient per-paint cache feeding the on-node latency badge: stable node
    // id -> total accumulated latency in samples (only nodes with >0 are stored).
    // Recomputed at the top of paint(), and only when some node actually reports
    // latency, so an all-zero graph does no work and draws no badges.
    std::unordered_map<int, int> latencyBadgeTotals;
    double latencyBadgeSampleRate = 0.0;
    // Right-click menu for a single pin (triggered anywhere across the pin's
    // row, including its label text). For a control-input pin (one bound to a
    // ModPin) this offers Switch Set/Mod and Remove; for any other pin it
    // falls back to showNodeMenu so a plain pin right-click still does
    // something useful.
    void showPinMenu(Node& node, const Pin& pin, bool isInput);

    // Control-input (#88) operations, shared by the param-row menu and the
    // pin menu so both surfaces stay in sync. All look the node up by id and
    // address the binding by stable paramIndex (no dangling references across
    // the async menu callback). Each commits an undo snapshot and requests a
    // graph rebuild.
    void addControlInput(int nodeId, int paramIdx, bool absolute);
    void removeControlInput(int nodeId, int paramIdx);
    void switchControlInputMode(int nodeId, int paramIdx);

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

// Launch the shared AHDSR envelope editor (AHDSREnvelopeComponent) as a
// tool dialog editing `graph.findNode(nodeId)->ahdsrEnvelope`. Shared by the
// node right-click "Envelope (AHDSR)..." menu and the "Envelope..." buttons
// inside the instrument editor dialogs (wavetable / layered / spectral, etc.)
// so there's a single launch path. Marks the graph dirty on each edit and
// commits one "Edit envelope" undo snapshot. `parent` is the component the
// dialog centres around / is owned by (for taskbar parentage). No-op if the
// node no longer exists.
void launchAhdsrEnvelopeDialog(juce::Component* parent, NodeGraph& graph,
                               int nodeId);

// Open a multi-tab AHDSR editor over node.opEnvelopes - one tab per envelope,
// each hosting an AHDSREnvelopeComponent bound to opEnvelopes[i] by reference.
// `tabNames` gives the tab labels and also fixes the expected count: the node's
// opEnvelopes vector is resized to tabNames.size() if needed so the dialog
// always has something to edit. Used by the FM synth (4 operator envelopes) and
// available to any future multi-envelope instrument. Marks the graph dirty on
// each edit and commits one undo snapshot when the (modal) dialog closes. No-op
// if the node no longer exists. `parent` provides taskbar parentage.
void launchOpEnvelopesDialog(juce::Component* parent, NodeGraph& graph,
                             int nodeId,
                             const std::vector<juce::String>& tabNames);

} // namespace SoundShop

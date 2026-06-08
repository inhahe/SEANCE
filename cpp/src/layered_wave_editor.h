#pragma once
#include "node_graph.h"
#include "wavetable_frame.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <map>
#include <functional>
#include <string>
#include <memory>
#include <optional>

namespace SoundShop {

// A single layer in a layered waveform: a basic shape at a harmonic ratio,
// with phase offset and amplitude. Layers are summed into one single-cycle
// wavetable at edit time (bake-once, not per-note).
struct WaveLayer {
    enum Shape { Sine, Saw, Square, Triangle, Noise, Drawn, Formula };
    Shape shape = Sine;
    int   ratio = 1;      // harmonic number: 1 = fundamental, 2 = octave, ...
    float phase = 0.0f;   // 0..1 (one full cycle)
    float amp   = 1.0f;   // 0..1

    // For Drawn shape: two sub-modes.
    //
    // Points mode (freehandMode == false): free control points over one cycle.
    // Each point is (phase 0..1, amplitude -1..1). Waveform is evaluated
    // by Catmull-Rom interpolation through the points, wrapped periodically.
    // Empty list = flat zero; a fresh Drawn layer is seeded with a few
    // points so the user has something to grab.
    //
    // Freehand mode (freehandMode == true): per-sample waveform data stored
    // in drawnSamples (512 floats, each in -1..1, covering one cycle).
    // Linearly interpolated to any output table size during render.
    bool freehandMode = false;
    std::vector<std::pair<float, float>> drawnPoints;
    std::vector<float> drawnSamples;  // 512 samples for freehand mode

    // For Formula shape: a single text expression evaluated over one cycle
    // with `x` ranging across [0, 2*pi) (radians). Reuses the WaveExprParser
    // shared with the frequency-domain editor, so the same vocabulary works
    // both places: sin, cos, tan, exp, log, sqrt, pow, abs, tanh, clamp,
    // saw(x), square(x), triangle(x), noise(), random, pi, e, + - * / ^.
    // Output is clamped to [-1, 1] by the parser.
    std::string formulaExpr = "sin(x)";
};

// Editor component for a single WaveLayer. Used by the wavetable editor (one
// row per layer in a stack) and by the SignalShape editor (one editor per
// "layer" composing the LFO/envelope shape). The editor owns no model state
// itself - it edits the WaveLayer pointed to by `layerPtr`, and notifies the
// owner via callbacks whenever the user mutates anything.
//
// Lifetime contract: the WaveLayer pointed to must outlive the editor, and
// must not be relocated (vector reallocation, erase, etc.) while the editor
// holds a pointer to it. The owner is responsible for calling setLayerPtr()
// whenever the underlying storage moves. The wavetable editor's solution is
// to rebuild every editor whenever the layer vector mutates - that keeps the
// pointer logic trivial at the cost of a UI rebuild per add/delete (cheap
// since the user can't add/delete fast enough to notice).
class WaveLayerEditor : public juce::Component {
public:
    struct Callbacks {
        // Required. Called whenever the user mutates the layer through any
        // control on this editor. The owner typically responds by re-rendering
        // a preview / committing to the model / requesting a graph rebuild.
        std::function<void()> onChanged;
        // Optional. If set, a small "X" delete button appears in the top-right
        // and invokes this when clicked. The owner is responsible for actually
        // removing the layer from its container - this editor just signals
        // the intent. If null, the delete button is hidden (single-layer
        // editors that don't support deletion).
        std::function<void()> onDelete;
        // Optional. If set, the row label reads "Layer N" using the returned
        // 1-based index. Called every syncFromModel(). If null, the label
        // reads "Layer" with no index suffix.
        std::function<int()> indexForLabel;
    };

    WaveLayerEditor(WaveLayer* layerPtr, Callbacks cb);

    // Rebind to a different layer (e.g. after the owner's storage moved).
    // Triggers a full UI sync from the new layer's state.
    void setLayerPtr(WaveLayer* p);
    WaveLayer* getLayerPtr() const { return layer; }

    // Pull every visible control's state from the underlying layer. Call
    // after setLayerPtr or after any external mutation (preset load,
    // project load, ...). Internal mutations don't need it - they update
    // both the model and the controls themselves.
    void syncFromModel();

    // Re-render the mini per-layer preview strip. Cheap (512 samples).
    // Internal callbacks already call this; external code only needs it
    // when the layer was mutated without going through this editor.
    void refreshPreview();

    void resized() override;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent&) override;

    static constexpr int previewHeight = 92;
    // Total minimum row height: label + shape btn row + sub-row +
    // 3 slider rows + padding + preview. Owners use this to lay out
    // a vertical stack of editors at a uniform pitch.
    static int rowHeight() { return 22 + 24 + 24 + 20 * 3 + 12 + previewHeight + 4; }

private:
    void updateShapeButtons();
    juce::Rectangle<float> getPreviewAreaBounds() const;
    bool mouseToPointXY(juce::Point<float> p, float& outX, float& outY) const;
    int findPointNear(float x, float y, float radius = 0.05f) const;
    void sortPointsByX();
    void writeFreehandSample(float x, float y);
    void showPresetMenu();

    WaveLayer* layer = nullptr;
    Callbacks callbacks;

    juce::Label label;
    juce::TextButton sineBtn, sawBtn, squareBtn, triangleBtn, noiseBtn, drawnBtn, formulaBtn;
    juce::TextButton freehandToggle;
    juce::TextEditor formulaEditor;
    juce::Slider ratioSlider, phaseSlider, ampSlider;
    juce::Label  ratioLabel, phaseLabel, ampLabel;
    juce::TextButton presetBtn;
    juce::TextButton deleteBtn;
    std::vector<float> previewSamples;
    int draggingIdx = -1;

    bool freehandDrawing = false;
    int  lastFreehandIdx = -1;
    float lastFreehandY = 0.0f;
};

struct LayeredWaveform : public IWavetableFrame {
    std::vector<WaveLayer> layers;
    int tableSize = 2048;

    // Sum layers into `out` (resized to this->tableSize). Normalized to peak 1.0.
    void render(std::vector<float>& out) const;

    // Encode as a string stored in node.script, prefixed with "__layered__:".
    std::string encode() const;

    // Decode from a string (with or without the prefix). Returns true on success.
    bool decode(const std::string& s);

    // Create a default 1-layer sine.
    static LayeredWaveform defaultSine();

    // ---- IWavetableFrame overrides ----------------------------------------
    // typeId() is "layered" - the wire tag used in __wavetable2__ encoding.
    // render(ts, out) ignores this->tableSize and renders at the requested
    // size; encodeBody/decodeBody handle the body without the __layered__:
    // prefix so a container can length-prefix it inline.
    const char* typeId() const override { return "layered"; }
    void render(int tableSize, std::vector<float>& out) const override;
    std::string encodeBody() const override;
    bool decodeBody(const std::string& body) override;
    std::unique_ptr<IWavetableFrame> clone() const override;
};

// -----------------------------------------------------------------------------
// LayerStackComponent
// -----------------------------------------------------------------------------
//
// Reusable vertical stack of WaveLayerEditor rows + a "+ Layer" button +
// (optionally) a summation preview pane that shows all layers summed into one
// cycle. This is the shared layered-waveform editing surface used by BOTH the
// Wavetable editor's right pane and the Signal Shape (LFO / envelope) editor,
// so the add/delete/edit-a-layer experience is identical in both places.
//
// The component does NOT own its model: it edits the `layers` vector of a
// LayeredWaveform supplied via setTarget() and fires onChanged after every
// mutation. The owner decides what onChanged does (commit to node.script,
// request a graph rebuild, re-render its own preview, ...).
//
// Realloc safety: the WaveLayerEditor rows hold WaveLayer* into
// target->layers. Any add or delete that can reallocate that vector rebuilds
// ALL rows (rebuildRows), so no row ever holds a dangling pointer - the same
// strategy the wavetable editor used before this was extracted.
//
// Options let callers omit the bits that only make sense in one context (e.g.
// the wavetable editor keeps its own multi-frame-type preview, so it turns the
// summation preview OFF; the LFO editor turns it ON). The per-row controls
// (shape buttons, harmonic ratio, phase, amp, draw/formula) are identical in
// both editors and are not configurable - harmonic ratio is kept everywhere
// because an extra-rate layer is a useful capability for an LFO too.
class LayerStackComponent : public juce::Component {
public:
    struct Options {
        // When true, a preview pane at the bottom draws the sum of all layers
        // (peak-normalized, matching what a consumer actually renders).
        bool showSummationPreview = false;
        int  summationPreviewHeight = 140;
        juce::String addLayerButtonText = "+ Layer";
        // Shown centered over the (empty) row area when there are no layers.
        juce::String emptyHint;
        // Seed for a freshly added layer. Arg = current layer count. If null,
        // a default-constructed WaveLayer (sine, ratio 1, amp 1) is used.
        std::function<WaveLayer(int existingCount)> makeNewLayer;
    };

    LayerStackComponent(Options opts, std::function<void()> onChanged);

    // Bind to the LayeredWaveform to edit (nullptr clears the stack). Cheap to
    // call defensively: it only rebuilds rows when the target pointer or its
    // layer count differs from what's currently displayed.
    void setTarget(LayeredWaveform* lw);
    LayeredWaveform* getTarget() const { return target; }

    // Force a full rebuild + re-sync from the model. Use after an external,
    // in-place mutation that changed layer values or count without going
    // through this component (e.g. the owner decoded a new doc into the same
    // LayeredWaveform object).
    void refreshFromModel();

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void rebuildRows();
    void addLayer();
    void renderSummation();
    void layoutRows();

    Options opts;
    std::function<void()> onChanged;
    LayeredWaveform* target = nullptr;
    int shownLayerCount = -1;   // staleness sentinel for setTarget

    juce::TextButton addLayerBtn;
    juce::Viewport   viewport;
    juce::Component  container;
    std::vector<std::unique_ptr<WaveLayerEditor>> rows;
    juce::Label      emptyHintLabel;

    std::vector<float> summationSamples;
    juce::Rectangle<int> summationBounds;
};

// A wavetable can be authored in two modes:
//
//  Grid: an N-dimensional rectilinear grid of LayeredWaveform frames played
//    via an (N+1)-dimensional terrain {tableSize, dim0, dim1, ...}. One
//    Position parameter per dimension morphs through the grid via N-linear
//    interpolation. Fast, easy to author for axis-aligned morph charts.
//
//  Scatter: an arbitrary set of frames placed at points in N-dimensional
//    space (each frame stores its own coordinate). The Position knobs give
//    a query point and the rendered waveform is a weighted blend of nearby
//    frames using a Wendland radial basis function. Lets you arrange e.g.
//    three sounds in a triangle and morph freely between them, which a
//    grid topology cannot express.
// A single entry in the wavetable's shared waveform library. The library
// owns the actual waveform data; cells (grid slots or scatter dots) hold
// only an `id` that points at one of these entries. This decouples the
// lifetime of a waveform from the lifetime of any cell that references
// it - deleting a cell does not destroy the waveform, and editing a
// waveform updates every cell that references it.
//
// IDs are document-scoped, monotonically assigned via WavetableDoc::
// nextLibraryId, and stable across copies / saves / loads. id == -1 is
// the reserved "no entry" sentinel that empty cells use.
struct WaveformLibraryEntry {
    int id = -1;
    std::string name;                              // user-editable label
    // User-picked color for this waveform. -1 = auto (derived from spectral
    // centroid of the waveform's harmonic content, or fall back to a
    // per-index palette rotation). >=0 indexes into the 8-color palette
    // returned by libraryPalette(). Lives on the LIBRARY entry so every
    // placement (grid cell or scatter dot) referencing the same waveform
    // is the same colour - which is what the user expects when "waveform"
    // and "colour" are conceptually one identity.
    int colorIdx = -1;
    std::unique_ptr<IWavetableFrame> wave;

    WaveformLibraryEntry() = default;
    WaveformLibraryEntry(WaveformLibraryEntry&&) noexcept = default;
    WaveformLibraryEntry& operator=(WaveformLibraryEntry&&) noexcept = default;
    // unique_ptr is non-copyable, so we provide deep-copy via clone().
    WaveformLibraryEntry(const WaveformLibraryEntry& o)
        : id(o.id), name(o.name), colorIdx(o.colorIdx),
          wave(o.wave ? o.wave->clone() : nullptr) {}
    WaveformLibraryEntry& operator=(const WaveformLibraryEntry& o) {
        if (&o == this) return *this;
        id = o.id; name = o.name; colorIdx = o.colorIdx;
        wave = o.wave ? o.wave->clone() : nullptr;
        return *this;
    }
};

// 8-colour palette shared by the library list swatch, the arrangement-view
// dots (both Grid and Scatter modes), and the colour picker. Indices are
// stable across save / load via WaveformLibraryEntry::colorIdx; out-of-range
// indices wrap modulo 8.
juce::Colour libraryPalette(int idx);
// Number of named palette colours. (Currently 8.)
int libraryPaletteSize();
// Resolve the colour to actually paint for a library entry. If entry's
// colorIdx >= 0 -> palette[colorIdx]. Otherwise derive from layered spectral
// centroid (warm = low partials, cool = high partials), falling back to
// palette[fallbackIdx] for non-layered / empty-info entries.
juce::Colour libraryEntryDisplayColor(const WaveformLibraryEntry* entry,
                                      int fallbackIdx);

// Small colour-swatch button. Click pops a palette menu (Auto + 8
// preset colours) and fires onPick(colorIdx) with -1 = Auto. Used both
// in the editor's identity row (right pane) and the library list rows
// in the arrangement sidebar.
class LibraryColorSwatch : public juce::Component,
                           public juce::SettableTooltipClient {
public:
    LibraryColorSwatch();
    void paint(juce::Graphics& g) override;
    void mouseUp(const juce::MouseEvent& e) override;
    // Visible swatch colour (resolved via libraryEntryDisplayColor for
    // "Auto", or libraryPalette(idx) for explicit picks). The Auto vs
    // explicit distinction isn't surfaced as a visual badge - the colour
    // resolves to the same palette index either way, so there's nothing
    // for the user to disambiguate by sight. The popup picker shows
    // "Auto" as the ticked menu item when applicable, which is enough.
    void setSwatchColor(juce::Colour c);
    // True if the underlying entry is on Auto. Used only by the picker
    // popup to tick the Auto row; no longer affects the visible swatch.
    void setIsAuto(bool a) { isAuto = a; }
    bool getIsAuto() const { return isAuto; }
    std::function<void(int colorIdx)> onPick; // -1 = Auto, else palette idx
private:
    juce::Colour col { 0xff5fb3ff };
    bool isAuto = true;
    bool hover = false;
    void mouseEnter(const juce::MouseEvent&) override { hover = true;  repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
};

struct ScatterFrame {
    // Reference to the waveform in WavetableDoc::library that this scatter
    // dot displays. -1 = empty cell (no waveform assigned yet). The
    // waveform data itself lives in the library, not here, so editing it
    // anywhere (library sidebar, edit view) updates every scatter dot
    // that references the same id.
    int waveformId = -1;
    std::vector<float> position;   // length = WavetableDoc::scatterDims, each in [0,1]
    std::string label;              // optional short user label shown in viewport
    // Note: there is no per-instance colorIdx here. Colour is a property of
    // the waveform itself (WaveformLibraryEntry::colorIdx), so every dot
    // referencing the same library entry paints the same colour.

    // Trivially copyable now that the waveform data lives elsewhere. The
    // copy/move/assign defaults handle position/label/waveformId correctly
    // without bespoke clone semantics.
    ScatterFrame() = default;
    ScatterFrame(const ScatterFrame&) = default;
    ScatterFrame(ScatterFrame&&) noexcept = default;
    ScatterFrame& operator=(const ScatterFrame&) = default;
    ScatterFrame& operator=(ScatterFrame&&) noexcept = default;
};

enum class WavetableMode { Grid, Scatter };

// Captured at the moment we convert a Grid wavetable to Scatter, so the
// editor can offer a "Convert back to Grid" reverse as long as the user
// hasn't moved any of the resulting scatter dots away from their original
// cell centers. Once the user drags any dot, the reverse button greys out
// (because cell-center alignment is the only way to round-trip without
// losing information). In-memory only - saving and reloading discards the
// snapshot, which is the right default ("commit" on save).
struct ScatterFromGridSnapshot {
    std::vector<int> gridDims;        // grid shape at conversion time
    std::vector<int> originalCellIdx; // one entry per scatterFrame, row-major into gridDims; -1 = no source cell
};

struct WavetableDoc {
    WavetableMode mode = WavetableMode::Grid;
    int tableSize = 2048;

    // ---- Waveform library (shared across cells) ----
    //
    // Single source of truth for waveform data. Cells (grid slots in
    // cellWaveformIds, scatter dots in scatterFrames) reference entries
    // here by id. The library is what gets edited; cells are just
    // placements. Three consequences:
    //
    //   1. Removing a cell does NOT remove a library entry - the
    //      waveform stays available for later assignment.
    //   2. Editing a waveform updates every cell that references it.
    //   3. The user can have library entries that aren't yet assigned
    //      to any cell, sitting in the sidebar ready to drop onto one.
    //
    // ids are document-scoped, monotonically assigned via nextLibraryId.
    // -1 = "no entry" sentinel for empty cells.
    std::vector<WaveformLibraryEntry> library;
    int nextLibraryId = 1;

    // ---- Grid mode ----
    // One library id per cell, flat row-major. -1 = empty cell. Size is
    // always exactly gridCellCount() except during in-progress
    // mode/axis transitions. Sample-time playback treats -1 as silence.
    std::vector<int> cellWaveformIds;
    std::vector<int> gridDims;             // size per dimension (e.g., {4} for 1D, {3,4} for 2D)

    // ---- Scatter mode ----
    int scatterDims = 2;                   // number of N-D coord axes
    float scatterRadius = 0.45f;           // RBF cutoff (in normalized [0,1] units)
    std::vector<ScatterFrame> scatterFrames;

    // Set by convertGridToScatter() and consulted by canRevertScatterToGrid().
    // Cleared whenever a scatter frame moves off its snapshot cell center,
    // or on any mode switch other than the matching reverse.
    std::optional<ScatterFromGridSnapshot> scatterFromGridSnapshot;

    WavetableDoc() = default;
    WavetableDoc(WavetableDoc&&) noexcept = default;
    WavetableDoc& operator=(WavetableDoc&&) noexcept = default;
    // Library entries hold unique_ptr<IWavetableFrame>, so copy semantics
    // are deep (each library entry clones its waveform). Cells (ids and
    // ScatterFrames) are trivially copyable.
    WavetableDoc(const WavetableDoc& o) = default;
    WavetableDoc& operator=(const WavetableDoc& o) = default;

    // Total cell count = product of gridDims
    int totalFrames() const {
        int n = 1;
        for (int d : gridDims) n *= std::max(1, d);
        return n;
    }

    // Number of position dimensions exposed to the synth as Position params.
    // Grid: number of grid axes. Scatter: scatterDims.
    int numDimensions() const {
        return mode == WavetableMode::Grid ? (int)gridDims.size() : scatterDims;
    }

    // Number of editable cells in the active mode.
    int activeFrameCount() const {
        return mode == WavetableMode::Grid ? (int)cellWaveformIds.size()
                                            : (int)scatterFrames.size();
    }

    // ---- Library API ----
    //
    // addLibraryEntry takes ownership of `wave` and returns the freshly
    // assigned id (>= 1). `name` is optional; empty name auto-generates
    // a default like "Waveform N" using the size of the library.
    int addLibraryEntry(std::unique_ptr<IWavetableFrame> wave,
                        std::string name = {});

    // Remove a library entry by id. Every cell that referenced it
    // becomes empty (-1). No-op for unknown ids. Returns true if an
    // entry was actually removed.
    bool removeLibraryEntry(int id);

    // Library lookup. nullptr for id == -1 or unknown id.
    IWavetableFrame* libraryFrameById(int id);
    const IWavetableFrame* libraryFrameById(int id) const;

    // Convenience: the library id assigned to cell `cellIdx` in the
    // current mode (Grid -> cellWaveformIds[idx]; Scatter ->
    // scatterFrames[idx].waveformId). -1 for empty / out of range.
    int libraryIdForCell(int cellIdx) const;

    // Set the cell's library reference. -1 clears it. No-op for out
    // of range cell indices or unknown library ids (other than -1).
    void assignCellToLibrary(int cellIdx, int libraryId);

    // True iff any cell currently references the given library id.
    // Used by the library sidebar to show "in use" badges and to warn
    // before deleting a library entry that's still placed.
    bool isLibraryEntryUsed(int id) const;
    int  countCellsUsingLibrary(int id) const;

    // Find the index of a library entry with a given id, or -1.
    int findLibraryIndexById(int id) const;

    // ---- Per-cell frame access (rerouted through the library) ----

    // Pointer to the waveform of cell `idx` (mode-aware), or nullptr
    // if the cell is out of range / empty / references a missing id.
    IWavetableFrame* frameAt(int idx);
    const IWavetableFrame* frameAt(int idx) const;

    // Convenience: returns the i-th cell's waveform downcast to
    // LayeredWaveform if it is one, else nullptr.
    LayeredWaveform* layeredFrameAt(int idx);
    const LayeredWaveform* layeredFrameAt(int idx) const;

    // Same as libraryFrameById, but downcast to LayeredWaveform. Returns
    // nullptr if the id isn't in the library OR the entry's frame isn't
    // layered. Used by the editor's library-id-addressed read paths
    // (currentEditingLayeredFrame() etc.) so the right-pane editor never
    // has to do its own dynamic_cast at every call site.
    LayeredWaveform* layeredFrameByLibrary(int libId);
    const LayeredWaveform* layeredFrameByLibrary(int libId) const;

    // Convert N-dimensional index to flat index (row-major)
    int gridToFlat(const std::vector<int>& idx) const {
        int flat = 0, stride = 1;
        for (int d = (int)gridDims.size() - 1; d >= 0; --d) {
            flat += idx[d] * stride;
            stride *= gridDims[d];
        }
        return flat;
    }

    // ---- Sparse-grid / mode-conversion helpers ----
    //
    // gridCellCount() is the product of gridDims (or 0 if any axis is <= 0).
    // The frames vector is kept at this size in Grid mode, with null slots
    // representing empty cells.
    int  gridCellCount() const;

    // Row-major coordinate <-> flat-index conversion. Bounds-checked: out of
    // range inputs return {} / -1 respectively, so callers can guard cheaply.
    std::vector<int> cellIdxToGridCoord(int idx) const;
    int  gridCoordToCellIdx(const std::vector<int>& coord) const;

    // Center of cell `idx` in normalized [0,1]^N coordinates, used as the
    // scatter position when converting a grid frame to a scatter frame.
    // Returns {} on out-of-range. For an axis of size 1, the center is 0.5.
    std::vector<float> cellCenterPosition(int cellIdx) const;

    // Grow / shrink one grid axis in-place, preserving every frame's
    // (i, j, k, ...) cell coordinate. Frames whose coord lies outside the
    // new axis size are dropped. Used by the Wavetable view's per-axis
    // steppers.
    void resizeGridAxis(int axisIdx, int newSize);

    // Mode transitions. Both clear scatterFromGridSnapshot on entry so a
    // stale snapshot from an earlier round-trip can't leak into the new
    // state; convertGridToScatter() then writes a fresh one.
    //
    // Grid -> Scatter: every non-null grid cell becomes a scatter frame
    // positioned at its cell center, with originalCellIdx recording the
    // source cell. scatterDims defaults to max(gridDims.size(), 2) so the
    // RBF view always has at least the 2 axes it needs.
    void convertGridToScatter();

    // True iff every scatter frame is still at the cell center recorded
    // in scatterFromGridSnapshot (within a small tolerance) AND the
    // scatter-frame count matches the snapshot's originalCellIdx count.
    // Greyed-out button in the editor reads this each repaint.
    bool canRevertScatterToGrid() const;

    // Scatter -> Grid. Caller must check canRevertScatterToGrid() first;
    // calling otherwise is a no-op (asserted in debug).
    void revertScatterToGrid();

    // Encode/decode
    std::string encode() const;
    bool decode(const std::string& s);

    static WavetableDoc defaultSingleSine();

    // Empty 1D wavetable - no frames at all. Used as the initial state for
    // the "Wavetable" node so the user starts with a blank wavetable view
    // and explicitly picks the first waveform type via "+ Waveform" instead
    // of getting a sine they have to delete.
    static WavetableDoc defaultEmpty();
};

// Editor window contents (paired with a juce::DialogWindow launched by the caller).
// Edits `node.script` directly. onApply() should request a graph rebuild so
// the new waveform takes effect; it is called on a debounce timer (not on
// every slider tick) to avoid racing JUCE's async graph rebuild.
//
// The editor holds a WavetableDoc (one or more frames). Only one frame is
// editable at a time - the current frame - selected via frame-tab buttons.
// Inherits DragAndDropContainer so the arrangement view can accept drops
// from the Library list (dropped library entry -> new cell or scatter
// dot at the cursor) and from cell-drag-to-cell (1D/2D wavetables only).
// The container is parented high enough in the tree that all drag
// sources and targets (Library row buttons, the embedded ScatterView)
// can find it via DragAndDropContainer::findParentDragContainerFor.
class LayeredWaveEditorComponent : public juce::Component,
                                   public juce::DragAndDropContainer,
                                   private juce::Timer {
public:
    LayeredWaveEditorComponent(NodeGraph& graph, int nodeId, std::function<void()> onApply);
    ~LayeredWaveEditorComponent() override;

    void resized() override;
    void paint(juce::Graphics& g) override;
    void timerCallback() override;

    // Layer access goes through these so the WaveLayerEditor rows don't have
    // to know about the current frame selection.
    std::vector<WaveLayer>& currentLayers();
    const std::vector<WaveLayer>& currentLayers() const;

private:
    class ScatterView; // inner component for the N-D scatter viewport
    class WavetableViewWindowContent; // arrangement view + sidebar (embedded)
    friend class ScatterView;
    friend class WavetableViewWindowContent;

    NodeGraph& graph;
    int nodeId;
    std::function<void()> onApply;

    WavetableDoc wave;
    // ---- Editor target vs. arrangement-view selection (two separate things) ----
    //
    // currentLibraryId  = which LIBRARY ENTRY the right-pane editor is
    //                     currently editing (layer rows / spectral editor /
    //                     wavelet painter / preview strip all bind to this).
    //                     The library entry IS the waveform definition, so
    //                     edits flow into every cell that references it.
    //                     -1 = nothing being edited (empty library).
    //
    // currentFrameIdx   = which CELL in the arrangement view is currently
    //                     selected. Used as the destination for "Assign to
    //                     selected cell", as the click-to-select target in
    //                     the Cells list and arrangement view, and as the
    //                     anchor for "place captured waveform HERE". Has
    //                     nothing to do with what the editor edits anymore -
    //                     selecting an empty cell does NOT change the
    //                     editor target.
    //
    // Linkage: clicking a NON-empty cell in the arrangement view or the
    // Cells list updates BOTH (the cell becomes selected AND its library
    // entry becomes the editor target, because that's almost always what
    // the user means). Clicking an empty cell updates only
    // currentFrameIdx. Clicking a row in the Library list or a frame tab
    // updates only currentLibraryId.
    int currentLibraryId = -1;
    int currentFrameIdx = 0;            // Grid mode: row-major cell index. Scatter mode: scatter index.
    std::vector<float> currentPosition; // Internal default placement for new Scatter frames (center of N-D cube).
    std::vector<float> previewSamples;
    // Bounds of the single-cycle preview strip on the right pane, set by
    // resized() so paint() can draw the curve without re-deriving the
    // side-by-side geometry.
    juce::Rectangle<int> previewBounds;
    // Bounds of the "no editor for this frame type yet" placeholder area
    // in the right pane. Non-empty only when the currently-edited library
    // entry's frame is non-layered AND has no embedded editor (currently
    // SampleFrame). paint() draws an explanatory message here so the user
    // isn't staring at a blank rectangle.
    juce::Rectangle<int> placeholderBounds;

    // Editor body is split side-by-side:
    //   - Left half: the WAVETABLE arrangement view (scatter / grid
    //     visualization + sidebar with Library list, Cells list, mode
    //     conversion, axis steppers, RBF radius, per-axis position
    //     controls, and N-D rotation sliders).
    //   - Right half: the per-waveform editor for the currently-selected
    //     cell (currentFrameIdx). Layer rows for layered waveforms; the
    //     embedded spectral / wavelet editor for those frame types. A
    //     Compare panel and the single-cycle preview live at the top
    //     and bottom of the right half respectively.
    //
    // Both halves are visible all the time - selecting a cell on the left
    // (click a dot, click a Cells list row, or click the Library pencil)
    // updates the right half. No mode toggle; no OK button.

private:
    // The single entry point for "add a new waveform to the wavetable".
    // Currently driven by the sidebar's "+ Waveform" button (which lives
    // on WavetableViewWindowContent and calls this via the friend
    // relationship). The anchor component is what the popup menu is
    // positioned against - it must be on-screen and remain alive until
    // the menu is dismissed.
    void showAddWaveformMenu(juce::Component* anchor);

    juce::TextButton applyBtn    { "Apply" };
    juce::TextButton closeBtn    { "Close" };
    juce::TextButton helpBtn     { "?" };
    // Compare (#9): switch the synth between two render modes that apply
    // to a wavetable cycle, so the user can A/B audition the same waveform
    // played two different ways. Direct = the cycle is read by one
    // oscillator (classic wavetable). Additive bank = the cycle is FFT'd
    // and resynthesised as a per-partial sine bank. The third Synth Mode
    // (AM-sine / WaveformPerPoint) isn't a wavetable render mode at all
    // (it's for sonifying 2D / N-D terrains) and is intentionally NOT
    // exposed here - mixing it into the wavetable editor was the source
    // of the original "A: Wavetable / B: Additive" confusion.
    juce::Label      compareLabel { {}, "Render mode:" };
    juce::TextButton compareDirectBtn   { "Direct" };
    juce::TextButton compareAdditiveBtn { "Additive bank" };

    // Discoverability hint shown when the wavetable still has its default
    // single frame. Tells the user this editor isn't just a single waveform
    // editor - it's a wavetable arrangement space. Hides as soon as a second
    // frame is added.
    juce::Label hintLabel;

    // Per-waveform identity row at the top of the right pane: a colour
    // swatch (opens the palette picker) plus a name TextEditor for the
    // library entry the editor is currently bound to. Lives above the
    // editor body (layer rows / spectral / wavelet) so it stays visible
    // for every editor type. Hidden when no library entry is targeted
    // (empty library). LibraryColorSwatch is defined at namespace scope
    // in the cpp file and reused by the library list rows in the
    // arrangement-view sidebar.
    juce::Label      identityLabel { {}, "Waveform:" };
    std::unique_ptr<LibraryColorSwatch> nameColorSwatch;
    juce::TextEditor nameEditor;

    // Push the editor's current colour / name into the library entry the
    // editor is targeting, and reflect any change back into the library
    // list in the arrangement sidebar. Called whenever currentLibraryId
    // changes, the editor is constructed, or the user finishes editing.
    void refreshIdentityRow();

    // Shared layer-stack widget (the "+ Layer" header, the scrolling list of
    // WaveLayerEditor rows, and per-layer add/delete). Identical code is used
    // by the Signal Shape (LFO / envelope) editor. Summation preview is OFF
    // here - the wavetable editor has its own multi-frame-type preview strip
    // (refreshPreview / previewBounds) that handles spectral / wavelet /
    // granular frames too, which the shared component knows nothing about.
    // Bound to the current layered frame via setTarget(currentEditingLayeredFrame()).
    std::unique_ptr<LayerStackComponent> layerStack;

    // When the current frame is non-layered (spectral / wavelet), the
    // matching editor is embedded into the same screen area normally
    // occupied by the layer stack, so the wavetable editor stays a single
    // window regardless of which frame type the user is editing.
    // Recreated whenever the user switches to a frame of a different type
    // (it's bound to a specific frame via the frame-backed ctor).
    std::unique_ptr<juce::Component> embeddedFrameEditor;
    // typeId() of the frame the embed currently targets, so we know when
    // we can keep the embed vs. when we need to tear it down and rebuild.
    std::string embeddedFrameType;

    // Painted red "x" glyph for deleting a frame tab. A bare TextButton frame
    // would create visual noise in a dense row of frame tabs (and JUCE's
    // TextButton ellipsises out single characters at narrow widths). This is
    // the only place in the editor that uses a non-button delete affordance;
    // the layer-row and other deletes elsewhere stay as TextButton("X") since
    // they sit at the far edge of a row and don't have the density issue.
    class FrameDeleteX : public juce::Component,
                         public juce::SettableTooltipClient {
    public:
        std::function<void()> onClick;
        FrameDeleteX() { setMouseCursor(juce::MouseCursor::PointingHandCursor); }
        void paint(juce::Graphics& g) override;
        void mouseEnter(const juce::MouseEvent&) override { hover = true;  repaint(); }
        void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
        void mouseUp(const juce::MouseEvent& e) override;
    private:
        bool hover = false;
    };

    // The wavetable arrangement view (scatter/grid visualization + the
    // sidebar with frames list, mode-conversion, axis steppers, RBF
    // radius, per-axis position controls, and N-D rotation sliders).
    // This used to live in a pop-out window; it's now an embedded child
    // that fills the editor body in arrangement mode and is hidden in
    // per-waveform edit mode.
    std::unique_ptr<WavetableViewWindowContent> arrangementView;

    // N-D rotation of the scatter view. Shared source-of-truth between
    // the embedded ScatterView and the pop-out wavetable-view window so
    // mouse drag in one view and slider drag in the other stay in sync.
    // One angle (degrees) per ordered pair (i, j), i < j, of scatterDims;
    // see ScatterView::rotateNd for the composition order.
    std::vector<float> scatterPlaneAngles;
    static int  scatterPlaneCount(int N) { return (N <= 1) ? 0 : N * (N - 1) / 2; }
    static int  scatterPlaneIndexFor(int i, int j, int N);
    static std::vector<std::pair<int,int>> scatterPlanePairs(int N);
    void  ensureScatterPlaneAngles();
    float getScatterPlaneAngle(int i, int j) const;
    void  setScatterPlaneAngle(int i, int j, float deg);
    // Forwarded from ScatterView mouse-drag handler so the pop-out
    // window's sliders update when the user orbits the embedded view.
    void  notifyScatterViewRotated();
    // Tell the pop-out wavetable view that either the currently-selected
    // frame or the Position cursor changed (frame click, drag, +Frame, etc.).
    // The arrangement view re-pushes both into its per-axis slider strips.
    void  notifyPopoutFrameOrPositionChanged();
    void  notifyPopoutDocMutated();
    // Repaints the arrangement view's scatter visualization, used by all
    // rotation-state changes.
    void  repaintScatterViews();


    // Show the capture flow inline in the right pane (the spectral /
    // wavelet / layered / granular editors live there too). Source:
    // 0=Playback (project song), 1=Mic, 2=File. The capture component is
    // parented into capturePanel and given the right pane's bounds; the
    // per-frame editor is hidden while it's up. On Save the component
    // fires OnCapture.
    //
    // replaceCurrentEntry: when false (default), captured frames are
    // appended to the wavetable along the first dimension (Grid) or
    // stretched along the X axis (Scatter) - the normal "add waveforms"
    // flow. When true, the FIRST captured frame replaces the wave on the
    // library entry the editor is currently bound to (currentLibraryId);
    // any further captured frames are discarded. This is the "Re-capture"
    // path used by GranularFrameEditorComponent, so the user can swap a
    // captured source for a new one without piling up library entries.
    // No-op (defaults to append) if no library entry is currently bound.
    void showCapturePanelInline(int sourceKind,
                                bool replaceCurrentEntry = false);
    void dismissCapturePanel();
    void appendCapturedFramesAlongPosition(std::vector<std::unique_ptr<IWavetableFrame>> frames);
    // Replace the wave on the currently-edited library entry with the
    // first captured frame (drops the rest). Used by re-capture flows.
    // No-op if currentLibraryId is unset, the entry has been removed, or
    // `frames` is empty.
    void replaceCurrentEntryWithCapturedFrame(
        std::vector<std::unique_ptr<IWavetableFrame>> frames);

    // While non-null, occupies the right pane in place of the per-frame
    // editor / Compare panel / preview. Created by showCapturePanelInline
    // and torn down by dismissCapturePanel. Lifetime is tied to the
    // editor, so closing the editor (or destruction) cleans it up.
    std::unique_ptr<juce::Component> capturePanel;

    // ---- Editor-target accessors (read currentLibraryId, NOT a cell) ----
    //
    // Returns the IWavetableFrame the right-pane editor is currently bound
    // to. nullptr means "no editing target" (empty library, deleted entry,
    // or the entry's frame couldn't be resolved). Every code path that
    // edits the current frame goes through one of these.
    IWavetableFrame* currentEditingFrame();
    const IWavetableFrame* currentEditingFrame() const;
    LayeredWaveform* currentEditingLayeredFrame();
    const LayeredWaveform* currentEditingLayeredFrame() const;

    // The normalized wavetable Position [0,1]^N of the frame the editor is
    // currently targeting (currentLibraryId), derived from the first cell /
    // scatter dot that references it. Grid: the cell's per-axis grid coord
    // divided by (dimSize-1). Scatter: the dot's authored position. Empty
    // if the entry isn't placed in any cell (or no editing target). Used to
    // tell the synth which frame to audition so a frame's Play button plays
    // THAT frame, not whatever the live Position knob selects.
    std::vector<float> currentFramePosition() const;

    // Set the right-pane editor's target. Triggers the same UI refresh as
    // switchToFrame() did in the old model: rebuilds the frame tab strip,
    // rebuilds the layer rows / embedded sub-editor, refreshes preview,
    // and pings the pop-out wavetable view. No-op if libId is the current
    // target. libId == -1 explicitly clears the target.
    void setEditingLibraryEntry(int libId);

    void rebuildRows();
    void updateHintText();
    // Bring the embedded frame editor into sync with the current frame.
    // For layered frames: tear down any embed, show the layer rows.
    // For spectral / wavelet frames: hide the layer rows, build (or keep)
    // the matching component and parent it into the layers area. Called
    // from rebuildRows() and switchToFrame(); cheap when the embed
    // already matches the current frame type.
    void updateFrameEditorEmbed();
    void rebuildScatterUI();        // re-evaluates rotation slider count when dim count changes
    void refreshPreview();
    void commitToNode(); // encode `wave` into node.script
    // Push a graph undo snapshot so a settled wavetable edit enters the undo
    // system. Without this the edit lives only in the node's script (updated
    // by commitToNode) and is silently destroyed by any later, unrelated
    // Ctrl+Z that restores an earlier snapshot - which then gets saved,
    // making the waveform/cell "vanish" on reload.
    void commitUndoStep();
    void onLayerChanged();
    void switchToFrame(int idx);
    void syncPositionParams();      // ensure node has the right number of Position params
    // One block-rate modulation pin per Position axis, named by axis (X/Y/Z/W).
    // `pinToAxis` maps each existing Position pin's id to its axis index,
    // captured before syncPositionParams() reshuffles the Position params.
    void syncPositionModPins(Node& nd, const std::map<int, int>& pinToAxis);
};

} // namespace SoundShop

#pragma once
#include "node_graph.h"
#include "wavetable_frame.h"
#include "shape_expr.h"
#include "warp.h"
#include "warp_editor.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <map>
#include <functional>
#include <string>
#include <memory>
#include <optional>

namespace SoundShop {

// Load-scoped collector for factory-waveform references that fail to resolve
// against the current build's WaveformBank. A reference is a stable bank NAME
// (see WaveLayer::factoryRef); resolveFactoryRef() looks it up via
// WaveformBank::indexForName and, when the name is absent (e.g. the project was
// saved by a NEWER SEANCE whose waveforms.bin added that cycle), it silences the
// layer rather than embedding samples. That silent-zeroing would make a loaded
// song untrue to the original with no indication, so we surface a warning.
//
// Usage: construct a FactoryRefResolutionScope on the stack around a project
// load (or import). While one or more scopes are active, resolveFactoryRef()
// reports every unresolved name into the innermost scope (deduplicated, in
// encounter order). After the load completes, inspect `unresolved` and, if
// non-empty, warn the user. Scopes nest (the active one is a thread-unaware
// stack - loads happen on the message thread). When no scope is active,
// resolution failures are simply silent as before (e.g. undo restore, library
// preview decodes), so only user-facing project opens raise the popup.
class FactoryRefResolutionScope {
public:
    FactoryRefResolutionScope();
    ~FactoryRefResolutionScope();
    FactoryRefResolutionScope(const FactoryRefResolutionScope&) = delete;
    FactoryRefResolutionScope& operator=(const FactoryRefResolutionScope&) = delete;

    // Names of factory waveforms that could not be resolved during this scope,
    // unique and in the order first encountered.
    std::vector<std::string> unresolved;

    // Called by WaveLayer::resolveFactoryRef() when a name fails to resolve.
    // No-op when no scope is active.
    static void reportUnresolved(const std::string& name);

private:
    FactoryRefResolutionScope* prev_ = nullptr;
    static FactoryRefResolutionScope* active_;
};

// A single layer in a layered waveform: a basic shape at a harmonic ratio,
// with phase offset and amplitude. Layers are summed into one single-cycle
// wavetable at edit time (bake-once, not per-note).
struct WaveLayer {
    // Shape ids are serialized by NAME (see shapeName/parseShape), never by
    // ordinal, so new values can be appended freely without breaking old files.
    // The last four (Pulse..PhaseDist) are the Bucket B "generator morphs": a
    // built-in oscillator whose timbre is swept by a morph parameter that is
    // part of the wave's DEFINITION (duty, sync ratio, FM index, PD amount),
    // distinct from the Bucket A warp chain that transforms an arbitrary cycle.
    enum Shape { Sine, Saw, Square, Triangle, Noise, Drawn, Formula,
                 Pulse, Sync, FM, PhaseDist };
    Shape shape = Sine;
    int   ratio = 1;      // harmonic number: 1 = fundamental, 2 = octave, ...
    float phase = 0.0f;   // 0..1 (one full cycle)
    float amp   = 1.0f;   // 0..1

    // Bucket B generator-morph parameters. Meaning is per-shape; ignored by the
    // classic shapes (Sine..Formula). Stored as two generic normalised [0,1]
    // knobs so the serializer and UI stay uniform:
    //   Pulse     - shapeParam = duty cycle (0=thin, 1=wide; 0.5 = square).
    //               shapeParam2 unused.
    //   Sync      - shapeParam = hard-sync amount; the slave oscillator runs
    //               (1 + 7*shapeParam)x the master and resets every master cycle
    //               (the classic sync sweep). shapeParam2 unused.
    //   FM        - shapeParam  = modulation index (0..~8 radians of phase mod).
    //               shapeParam2 = modulator : carrier ratio (mapped to 1..8),
    //               quantised to integers so one cycle stays periodic.
    //   PhaseDist - shapeParam = Casio-CZ phase-distortion amount (0 = sine,
    //               1 = hard resonant skew). shapeParam2 unused.
    // Defaults give an immediately-audible morph at the midpoint.
    float shapeParam  = 0.5f;
    float shapeParam2 = 0.5f;

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
    // with `x` ranging across [0, 2*pi) (radians). The same vocabulary works
    // in all three languages: sin, cos, tan, exp, log, sqrt, pow, abs, tanh,
    // clamp, saw(x), square(x), triangle(x), noise(), pi, e, + - * / ^.
    // Output is clamped to [-1, 1].
    std::string formulaExpr = "sin(x)";

    // Authoring language for the Formula expression. Built-in is evaluated
    // live (pure C++, thread-safe). Lua/Python are baked once into
    // formulaSamples at edit/load time on the UI thread (the interpreters are
    // not audio-thread safe) and sampled from there during render.
    ShapeLang formulaLang = ShapeLang::Builtin;

    // Transient (not serialized): one cycle of the Lua/Python-baked formula,
    // and the last bake error (empty when OK). Re-baked by rebakeFormula().
    std::vector<float> formulaSamples;
    std::string        formulaError;

    // Per-layer warp chain (Bucket A, element-scope): shape-bending ops baked
    // into THIS layer's single cycle at render time, before it's summed into
    // the stack. Unlike the frame-scope warp chain (WavetableDoc::warpChain,
    // applied live per voice and modulatable), these are part of the layer's
    // baked definition - they let one layer be folded/clipped/bent while its
    // neighbours stay clean. Empty by default. Serialized as a trailing
    // "warp=" field on the layer (see encodeLayer/parseLayer).
    std::vector<WarpOp> warpChain;

    // When non-empty, this Drawn/Freehand layer's cycle is a REFERENCE to a
    // built-in factory waveform, identified by its stable WaveformBank NAME
    // (resolved via WaveformBank::indexForName). Set by the factory-waveform
    // browser when the user inserts a stock cycle. While the reference holds,
    // the project serializes only the NAME (a "factory=" field), NOT the 512
    // samples - so a session that uses stock waveforms stays small. On load the
    // samples are re-resolved from the bank into drawnSamples (resolveFactoryRef)
    // so the render path is unchanged.
    //
    // The reference survives level/tuning edits (amp, phase, ratio, warp) but is
    // dropped - "forked to a full copy" - the moment the cycle CONTENT itself is
    // edited (drawing over it, switching to Points, changing shape). Forking is
    // belt-and-suspenders: the UI clears factoryRef on those edits, AND the
    // serializer is content-addressed (it only writes the name when drawnSamples
    // still match the bank byte-for-byte), so a missed UI trigger can never
    // silently store the wrong reference. Empty for user-drawn / captured /
    // imported / generator cycles; only meaningful for Drawn shape.
    std::string factoryRef;

    // When >= 0, this layer LIVE-REFERENCES a user Waveform asset in the project
    // library (AssetLibrary id), the per-layer analogue of the frame-scope
    // WaveformLibraryEntry::assetId. While the reference holds, the layer's cycle
    // content is kept in sync with the asset: on load (and after any settled edit
    // elsewhere) resolvePerLayerWaveformReferences() pulls the asset's body into
    // this layer, and a settled edit to THIS layer is pushed back to the asset by
    // writeBackPerLayerWaveforms(), propagating to every layer/frame that shares
    // the id (the "live reference" model). -1 = independent (the layer owns its
    // cycle outright), exactly as before.
    //
    // The shared unit is the layer's SHAPE content, not its mix placement: amp is
    // a per-slot property preserved across resolves (like a frame entry's gain).
    // A per-layer asset is stored as a single-layer LayeredWaveform; if a layer is
    // pointed at a MULTI-layer asset (one saved from frame scope) it is flattened
    // to that frame's summed cycle on resolve, and a write-back would shrink the
    // asset to that single flattened cycle - so multi-layer assets are best used
    // by COPY (sync off) at the layer scope. See layerToWaveformAsset /
    // applyWaveformAssetToLayer. Serialized as a trailing "asset=" field.
    int assetId = -1;

    // Re-bake formulaSamples from formulaExpr for the current formulaLang.
    // No-op (clears the buffer) for Built-in. Call after any change to
    // formulaExpr / formulaLang and after loading from a project.
    void rebakeFormula();

    // Resolve factoryRef -> drawnSamples from the built-in WaveformBank (also
    // forces shape=Drawn, freehandMode=true). No-op when factoryRef is empty.
    // If the name can't be found (e.g. waveforms.bin missing), drawnSamples is
    // left empty - the layer renders silent - and factoryRef is kept so a
    // re-save still references it. Defined in layered_wave_editor.cpp (needs the
    // bank). Call after decoding a layer that carries a factory= reference.
    void resolveFactoryRef();
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
        // Optional. Fired when the row's preferred height changes (the only
        // cause is an op being added to / removed from the per-layer warp
        // chain, which grows/shrinks the embedded warp editor). The owner
        // re-lays-out its row stack so the rows below shift to follow. Null
        // when per-layer warp is disabled.
        std::function<void()> onHeightChanged;
        // Optional. Fired when the user picks "From Library..." in the wave-
        // source picker. The owner opens the waveform library browser and, on
        // a pick, loads the chosen single cycle into THIS layer (the owner
        // re-fetches the layer by its stable index and calls refreshFromModel).
        // If null, the "Use Library..." menu entry is omitted.
        std::function<void()> onPickFromLibrary;
        // Optional. Fired when the user picks "Save to Library..." in the wave-
        // source picker. The owner publishes THIS layer as a reusable Waveform
        // asset (a single-layer LayeredWaveform) and live-links the layer to it.
        // If null, the "Save to Library..." menu entry is omitted.
        std::function<void()> onSaveToLibrary;
        // Optional. Fired when the user picks "Unlink from Library" in the wave-
        // source picker. The owner detaches THIS layer's live link (sets
        // WaveLayer::assetId = -1) while KEEPING the current cycle content, so the
        // layer becomes an independent editable copy that no longer propagates to
        // or from the asset. Only meaningful (and only shown enabled) when the
        // layer currently references an asset. If null, the entry is omitted.
        std::function<void()> onDesyncFromLibrary;

        // Optional. Per-layer warp modulation (#88, item-M). The embedded
        // per-layer warp editor's "Mod" checkbox routes through these so the
        // owner can create/destroy a per-layer warp Param + modulation pin for
        // warp op `opIndex` on THIS layer. The layer identity is resolved by the
        // owner (the LayerStackComponent closure that captures the row index);
        // only the warp slot `opIndex` is passed here. Left unset when per-layer
        // warp is disabled - the "Mod" checkbox then stays hidden.
        std::function<bool(int opIndex)>      isWarpOpModulated;
        std::function<void(int opIndex,bool)> setWarpOpModulated;
        // Optional. Absolute-only amount lock for this layer's warp op (forwarded
        // to WarpChainEditor::Callbacks::isAmountLocked). Unset = lock when pinned.
        std::function<bool(int opIndex)>      isWarpOpAmountLocked;
        // Optional. Mirrors WarpChainEditor::Callbacks::modDisabledReason for the
        // per-layer chain: a non-empty return disables the op's "Mod" checkbox and
        // shows the string as its tooltip. Used to gate per-layer warp modulation
        // to single-frame wavetables (the only shape the synth re-bakes live).
        std::function<juce::String(int opIndex)> warpModDisabledReason;

        // Optional. Per-layer field modulation (#88, item-M). A "Mod" checkbox
        // next to this layer's Phase and Amplitude slider routes through these so
        // the owner can create/destroy a layer-field Param + modulation pin so a
        // cable can drive that value live. field: 0 = Phase, 1 = Amplitude. The
        // layer identity is resolved by the owner (the LayerStackComponent closure
        // capturing the row index); only the field is passed here. Left unset when
        // per-layer field modulation is disabled - the checkboxes stay hidden.
        std::function<bool(int field)>      isFieldModulated;
        std::function<void(int field,bool)> setFieldModulated;
        // Optional. Absolute-only lock for the Phase/Amp/generator slider. Unset =
        // lock whenever the field is pinned (legacy).
        std::function<bool(int field)>      isFieldAmountLocked;
        // Optional. A non-empty return disables the field's "Mod" checkbox and
        // shows the string as its tooltip (gates to single-frame wavetables).
        std::function<juce::String(int field)> fieldModDisabledReason;
    };

    // enableWarp embeds a per-layer warp chain editor (baked shape-bending on
    // THIS layer's cycle, not modulatable - distinct from the doc-level
    // frame-scope warp). Off for the LFO / Signal-Shape editor, on for the
    // wavetable layer stack.
    WaveLayerEditor(WaveLayer* layerPtr, Callbacks cb, bool enableWarp = false);

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
    // Fixed part of a layer row, the same for every wave-source shape: outer
    // margin (reduced(4) top+bottom) + label + wave-source picker + sub-row +
    // the 3 base slider rows (Harmonic / Phase / Amplitude) + the mini preview.
    // The generator-morph slider rows are NOT included here - they exist only
    // for the generator shapes (Pulse / Sync / FM / PhaseDist), so
    // preferredHeight() adds them on demand. Reserving them unconditionally
    // (the old behaviour) left a large empty gap above the Layer Morph strip
    // for the classic shapes, which is exactly the wasted space we're removing.
    static constexpr int baseRowHeight = 8 + 22 + 24 + 24 + 20 * 3 + previewHeight;

    // Actual height this row wants: baseRowHeight, plus one row per VISIBLE
    // generator-morph slider, plus the embedded per-layer warp editor's current
    // height (which tracks its op count) when warp is enabled. Owners lay out
    // the row stack at this per-row pitch; updateSourceControls() fires
    // onHeightChanged when the visible-morph-row count changes so the stack
    // reflows on a shape switch.
    int preferredHeight() const;

private:
    void updateSourceControls();
    // Refresh the Phase/Amp "Mod" checkbox visibility, toggle state, enabled
    // state (+ disabled-reason tooltip), and whether the corresponding slider is
    // signal-locked (greyed) because a modulation cable is driving it.
    void syncFieldModState();
    juce::Rectangle<float> getPreviewAreaBounds() const;
    bool mouseToPointXY(juce::Point<float> p, float& outX, float& outY) const;
    int findPointNear(float x, float y, float radius = 0.05f) const;
    void sortPointsByX();
    void writeFreehandSample(float x, float y);
    void showWaveSourceMenu();

    WaveLayer* layer = nullptr;
    Callbacks callbacks;

    juce::Label label;
    // One unified wave-source picker replaces the old 11 shape buttons + the
    // separate Preset button. Opens a single menu: static shapes -> Draw ->
    // Formula -> wave-defining (Type-1) morphs -> Presets -> From Library.
    juce::TextButton waveSourceBtn;
    juce::TextButton freehandToggle;
    juce::TextEditor formulaEditor;
    juce::ComboBox   formulaLangCombo;   // Built-in / Lua / Python (Formula only)
    juce::Slider ratioSlider, phaseSlider, ampSlider;
    juce::Label  ratioLabel, phaseLabel, ampLabel;
    // Per-parameter modulation "Pin" toggles (#88, item-M). Visible only when
    // callbacks.setFieldModulated is wired (the wavetable layer stack); hidden for
    // the LFO / Signal-Shape editor. field 0 = phase, 1 = amplitude, 2 = generator
    // param (Duty/Amount/Index), 3 = FM ratio. morph/morph2 buttons additionally
    // follow their slider's visibility (only the parameter-bearing generators).
    juce::ToggleButton phaseModBtn, ampModBtn, morphModBtn, morph2ModBtn;
    // Generator-morph parameter sliders (visible only for Pulse/Sync/FM/PD).
    // morphSlider drives shapeParam (duty / sync amount / FM index / PD amount);
    // morph2Slider drives shapeParam2 (FM modulator:carrier ratio only).
    juce::Slider morphSlider, morph2Slider;
    juce::Label  morphLabel, morph2Label;
    juce::TextButton deleteBtn;
    std::vector<float> previewSamples;
    int draggingIdx = -1;

    // Per-layer warp chain editor (baked shape-bending on this layer's cycle).
    // Null unless the owner passed enableWarp=true. Bound to layer->warpChain
    // and rebound in setLayerPtr; its onChanged re-renders this row's preview
    // (warp is shown applied) and forwards to callbacks.onChanged.
    std::unique_ptr<WarpChainEditor> warpEditor;

    bool freehandDrawing = false;
    int  lastFreehandIdx = -1;
    float lastFreehandY = 0.0f;
};

struct LayeredWaveform : public IWavetableFrame {
    std::vector<WaveLayer> layers;
    int tableSize = 2048;

    // Un-hide the base-class render(int, out) wrapper. Declaring the no-arg
    // render(out) below would otherwise hide ALL base `render` overloads by
    // name, so callers that do lw.render(ts, out) wouldn't compile. The base
    // wrapper (renderRaw + gain) is exactly what they want.
    using IWavetableFrame::render;

    // Sum layers into `out` (resized to this->tableSize). Normalized to peak
    // 1.0. This is the gain-FREE primitive: it does NOT apply IWavetableFrame::
    // gain (that's applied by the base-class render(int,out) wrapper, which
    // renderRaw() delegates here through). Callers wanting the gained cycle
    // should go through the IWavetableFrame render() path.
    void render(std::vector<float>& out) const;

    // Render like render(), but with each layer's per-layer warp-op amounts
    // overridden by live values (the unified warp/morph model: a per-layer warp
    // op can opt into a modulation pin, so its amount is driven by a cable). The
    // synth calls this at block rate to re-bake the cycle when any per-layer warp
    // op is modulated; the editor's baked path stays on plain render().
    //   overrides[layerIdx][opSlot] = live amount in [0,1], or < 0 to keep the
    //   op's baked amount. overrides[layerIdx] may be shorter than the layer's
    //   warp chain (trailing ops keep their baked amount) or empty (no override
    //   for that layer). `overrides` itself may be shorter than `layers`.
    // With an empty `overrides` this is byte-for-byte identical to render() -
    // render() delegates here - so there is a single summation/normalization
    // code path.
    void renderWithLiveWarp(const std::vector<std::vector<float>>& overrides,
                            std::vector<float>& out) const;

    // Render like renderWithLiveWarp(), but ALSO override each layer's Phase
    // and/or Amplitude with live (modulated) values. These are the per-layer
    // field modulation pins (#88): a layer's Phase or Amp slider can opt into a
    // modulation cable, so its value is driven at block rate.
    //   warpOverrides  - same convention as renderWithLiveWarp's `overrides`.
    //   phaseOverrides[layerIdx] = live phase offset (any value, including
    //                  negative - phase wraps into the layer's sample lookup), or
    //                  NaN to keep the layer's stored phase. (A < 0 sentinel can't
    //                  be used here: a modulated phase can legitimately be
    //                  negative.) May be shorter than `layers` (trailing layers
    //                  keep stored phase).
    //   ampOverrides[layerIdx]   = live amplitude in [0,1], or NaN to keep the
    //                  layer's stored amp. Same length rules.
    //   shapeOverrides[layerIdx] = live generator parameter (shapeParam: duty /
    //                  sync amount / FM index / phase-dist amount) in [0,1], or NaN
    //                  to keep the layer's stored value. Only affects the
    //                  parameter-bearing generator shapes; ignored otherwise.
    //   shape2Overrides[layerIdx]= live second generator parameter (shapeParam2:
    //                  FM modulator:carrier ratio) in [0,1], or NaN. FM only.
    // renderWithLiveWarp delegates here with empty overrides, so the
    // summation/normalization code path stays unified.
    void renderWithLiveOverrides(const std::vector<std::vector<float>>& warpOverrides,
                                 const std::vector<float>& phaseOverrides,
                                 const std::vector<float>& ampOverrides,
                                 const std::vector<float>& shapeOverrides,
                                 const std::vector<float>& shape2Overrides,
                                 std::vector<float>& out) const;

    // Encode as a string stored in node.script, prefixed with "__layered__:".
    std::string encode() const;

    // Decode from a string (with or without the prefix). Returns true on success.
    bool decode(const std::string& s);

    // Transient (not serialized). Set by decode(): true when at least one
    // non-Built-in (Lua/Python/GLSL) Formula layer arrived WITHOUT an embedded
    // pre-baked cycle (the "bake=" field encodeLayer now writes). Such layers
    // can only be baked on the message thread (the interpreters are not
    // audio-thread safe), so when this is true after a project load the owner
    // should re-encode the script on the message thread - see
    // migrateLayeredScriptEmbedBake - so the audio thread never has to run an
    // interpreter to reconstruct the cycle.
    bool decodedNeedsBakeEmbed = false;

    // Create a default 1-layer sine.
    static LayeredWaveform defaultSine();

    // ---- IWavetableFrame overrides ----------------------------------------
    // typeId() is "layered" - the wire tag used in __wavetable2__ encoding.
    // render(ts, out) ignores this->tableSize and renders at the requested
    // size; encodeBody/decodeBody handle the body without the __layered__:
    // prefix so a container can length-prefix it inline.
    const char* typeId() const override { return "layered"; }
    void renderRaw(int tableSize, std::vector<float>& out) const override;
    std::string encodeBody() const override;
    bool decodeBody(const std::string& body) override;
    std::unique_ptr<IWavetableFrame> clone() const override;
};

// Message-thread migration for old projects. If `script` is a "__layered__:"
// tag whose Lua/Python/GLSL Formula layers carry no embedded baked cycle (saved
// before encodeLayer started embedding "bake=" data), this re-bakes the
// formulas on the CURRENT thread and re-encodes so the audio thread can render
// them without ever invoking an interpreter. Rewrites `script` and returns true
// ONLY when a change was needed; otherwise it's a cheap no-op returning false.
// MUST be called on the message thread (it may run Python/Lua to bake).
bool migrateLayeredScriptEmbedBake(std::string& script);

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
        // When true, each layer row embeds a per-layer warp chain editor
        // (baked shape-bending on that layer's cycle). The wavetable layer
        // stack turns this ON; the LFO / Signal-Shape editor leaves it OFF.
        bool enablePerLayerWarp = false;
        // Optional. When set, each layer row's wave-source picker offers a
        // "From Library..." entry. The arg is the row's (stable) layer index;
        // the owner opens the waveform browser and, on a pick, loads the
        // chosen single cycle into target->layers[index] then calls
        // refreshFromModel(). Null = no library entry on the picker.
        std::function<void(int layerIndex)> onPickFromLibrary;
        // Optional. When set, each layer row's wave-source picker also offers a
        // "Save to Library..." entry. The arg is the row's (stable) layer index;
        // the owner publishes target->layers[index] as a Waveform asset and
        // live-links the layer to it. Null = no save entry on the picker.
        std::function<void(int layerIndex)> onSaveToLibrary;
        // Optional. When set, each layer row's wave-source picker offers an
        // "Unlink from Library" entry (enabled only while that layer references
        // an asset). The arg is the row's (stable) layer index; the owner clears
        // target->layers[index].assetId, keeping the current cycle as an
        // independent copy. Null = no unlink entry on the picker.
        std::function<void(int layerIndex)> onDesyncFromLibrary;

        // Optional. Per-layer warp modulation (#88, item-M). The per-layer warp
        // editor's "Mod" checkbox routes (layerIndex, opIndex) to the owner, which
        // creates/destroys a per-layer warp Param + modulation pin so an LFO /
        // oscillator cable can drive that op's amount live. isLayerWarpOpModulated
        // queries the current pin state; setLayerWarpOpModulated toggles it;
        // layerWarpModDisabledReason returns a non-empty string to disable the
        // checkbox with an explanation (used to gate modulation to single-frame
        // wavetables). All null = the "Mod" checkbox stays hidden (baked-only).
        std::function<bool(int layerIndex,int opIndex)>      isLayerWarpOpModulated;
        std::function<void(int layerIndex,int opIndex,bool)> setLayerWarpOpModulated;
        std::function<juce::String(int layerIndex,int opIndex)> layerWarpModDisabledReason;
        // Optional. Returns whether (layerIndex, opIndex)'s amount slider should be
        // LOCKED - true only under an active Absolute ("Set") cable. Unset = lock
        // whenever pinned (legacy).
        std::function<bool(int layerIndex,int opIndex)>      isLayerWarpOpAmountLocked;

        // Optional. Per-layer field modulation (#88, item-M). A "Mod" checkbox
        // next to each layer's Phase and Amplitude slider routes (layerIndex,
        // field) to the owner, which creates/destroys a layer-field Param +
        // modulation pin so an LFO / oscillator cable can drive that value live.
        // field: 0 = Phase, 1 = Amplitude. isLayerFieldModulated queries the
        // current pin state; setLayerFieldModulated toggles it;
        // layerFieldModDisabledReason returns a non-empty string to disable the
        // checkbox with an explanation (gates modulation to single-frame tables,
        // same as warp). All null = the "Mod" checkboxes stay hidden (baked-only).
        std::function<bool(int layerIndex,int field)>      isLayerFieldModulated;
        std::function<void(int layerIndex,int field,bool)> setLayerFieldModulated;
        std::function<juce::String(int layerIndex,int field)> layerFieldModDisabledReason;
        // Optional. Absolute-only lock for the Phase/Amp/generator slider (twin of
        // isLayerWarpOpAmountLocked). Unset = lock whenever pinned (legacy).
        std::function<bool(int layerIndex,int field)>      isLayerFieldAmountLocked;
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

    // Project asset-library reference. -1 = independent (this entry owns its
    // own `wave` data). >=0 = live reference to a published Waveform asset:
    // `wave` is a resolved copy of that asset's frame, kept in sync by
    // resolveWaveformReferences() at edit/load time. Editing the asset
    // propagates to every library entry (in any node) that references it.
    // Distinct from `id`, which is the document-local entry id that cells
    // reference; assetId points OUT to the project-global store.
    int assetId = -1;

    WaveformLibraryEntry() = default;
    WaveformLibraryEntry(WaveformLibraryEntry&&) noexcept = default;
    WaveformLibraryEntry& operator=(WaveformLibraryEntry&&) noexcept = default;
    // unique_ptr is non-copyable, so we provide deep-copy via clone().
    WaveformLibraryEntry(const WaveformLibraryEntry& o)
        : id(o.id), name(o.name), colorIdx(o.colorIdx),
          wave(o.wave ? o.wave->clone() : nullptr), assetId(o.assetId) {}
    WaveformLibraryEntry& operator=(const WaveformLibraryEntry& o) {
        if (&o == this) return *this;
        id = o.id; name = o.name; colorIdx = o.colorIdx;
        wave = o.wave ? o.wave->clone() : nullptr;
        assetId = o.assetId;
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
    int scatterDims = 1;                   // number of N-D coord axes (0=drop-target, 1=line, 2+=square/cube view)
    float scatterRadius = 0.45f;           // RBF cutoff (in normalized [0,1] units)
    // "Distance fades volume" toggle, shared by both Scatter and Grid modes
    // (the WavetableMode union of state, even though the field lives in the
    // scatter block for historical reasons).
    //
    // When false (default) the blend is volume-normalized: the active weights
    // are divided by their sum so the total output level stays constant as you
    // morph.
    //   - Scatter: a lone frame is always full volume regardless of distance.
    //   - Grid: empty cells don't drain volume - the synth renormalizes the
    //     morph over the *filled* cells, so the output stays full-volume even
    //     when some cells are empty.
    // When true the raw blend weight is used directly as gain:
    //   - Scatter: moving the Position toward a frame makes it louder, away
    //     makes it quieter, and a point outside every frame's radius is silent.
    //     Lets a single scatter dot act as a "loudness island".
    //   - Grid: empty cells fade volume - morphing toward an empty cell ducks
    //     the output toward silence (the pre-renormalization grid behavior).
    // Serialized as the optional last field of the mode spec (4th for scatter,
    // after the dims for grid).
    bool absoluteBlend = false;
    std::vector<ScatterFrame> scatterFrames;

    // Set by convertGridToScatter() and consulted by canRevertScatterToGrid().
    // Cleared whenever a scatter frame moves off its snapshot cell center,
    // or on any mode switch other than the matching reverse.
    std::optional<ScatterFromGridSnapshot> scatterFromGridSnapshot;

    // NOTE: the frame-scope ("Summation Morph") warp chain and its asset
    // reference USED to live here as doc-level fields (warpChain / warpAssetId)
    // shared by every frame. As of 2026-06 they are PER FRAME: see
    // IWavetableFrame::morphChain / morphAssetId. Each library entry's frame
    // carries its own chain, baked into its cycle before the cross-frame morph
    // blend. Old projects that stored the single doc-level chain are migrated
    // into every frame on decode (see WavetableDoc::decode).

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
    //
    // NOTE: this is the *geometric* dimension count - it drives the
    // visualization (rotation planes, projection combo, axis steppers) and
    // must include inert axes. For the count of axes the user can actually
    // *traverse* (which drives the Position params/pins), use
    // effectiveDimCount() instead.
    int numDimensions() const {
        return mode == WavetableMode::Grid ? (int)gridDims.size() : scatterDims;
    }

    // Indices of the axes the user can actually traverse - i.e. the axes for
    // which a Position parameter / modulation pin is worth exposing. Defined
    // in the .cpp. The rule:
    //   Grid:    every axis whose size is >= 2 (a size-1 axis has a single
    //            cell, so a Position along it does nothing), in axis order.
    //   Scatter: all scatterDims axes when there's something for a position
    //            to do - either >= 2 frames to interpolate between, OR the
    //            "distance fades volume" blend is on (so moving toward/away
    //            from even a single frame changes loudness). Otherwise empty
    //            (a lone normalized frame is always full volume regardless of
    //            position, so its axes are inert).
    // The k-th entry maps the k-th Position parameter to its geometry axis;
    // the synth pins all other (inert) grid axes to coordinate 0.
    std::vector<int> effectiveAxes() const;
    int effectiveDimCount() const { return (int)effectiveAxes().size(); }

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

    // Build a unique "<base> (copy N)" name for duplicating the entry named
    // `srcName`. Strips any existing " (copy)" / " (copy N)" suffix so repeated
    // duplication increments rather than stacking ("Saw (copy 1)", "(copy 2)",
    // …) and picks the lowest N not already present in the library.
    std::string makeUniqueCopyName(const std::string& srcName) const;

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

    // TEMP throwaway diagnostic - dumps grid/cell/scatter/library state to file.
    void debugDumpState(const char* tag) const;

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

    // General (lossy) Scatter -> Grid conversion, used when a lossless
    // revertScatterToGrid() isn't possible (the wavetable was authored as
    // Scatter, or axes/dots were edited so the snapshot no longer matches).
    // Flattens every scatter dot into a 1D grid of N cells in frame order
    // (one dot per cell), discarding the free-form scatter positions. The
    // library is untouched; cells reference the same waveform ids. Always
    // succeeds, so the editor can offer a "to Grid" path unconditionally.
    void convertScatterToGrid();

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

// ---- Waveform asset-library bridge -----------------------------------------
//
// A published Waveform asset stores a single IWavetableFrame: AssetEntry.subType
// is the frame typeId() ("layered"/"spectral"/...) and AssetEntry.payload is the
// frame's encodeBody(). These two helpers convert between a frame and that
// asset representation; the resolver below pushes a stored frame into every
// library entry that references it.

// Encode a frame as (subType, payload) for AssetLibrary::add/update. A null
// frame encodes as an empty "layered" body (matching WavetableDoc::encode()).
void waveformAssetFromFrame(const IWavetableFrame* frame,
                            std::string& outSubType, std::string& outPayload);

// Build a fresh frame from a Waveform asset's (subType, payload). Returns
// nullptr on an unknown subType or a body that fails to decode.
std::unique_ptr<IWavetableFrame> frameFromWaveformAsset(const std::string& subType,
                                                        const std::string& payload);

// Per-layer Waveform asset bridge (the per-layer analogue of the above). A layer
// is shared as a single-layer LayeredWaveform; amp is excluded (a per-slot
// property). layerToWaveformAsset encodes one layer into (subType, payload);
// applyWaveformAssetToLayer pulls an asset body back into a layer, preserving the
// layer's amp + assetId and flattening multi-layer/non-layered assets to a
// freehand cycle. See WaveLayer::assetId and resolvePerLayerWaveformReferences().
void layerToWaveformAsset(const WaveLayer& layer,
                          std::string& outSubType, std::string& outPayload);
bool applyWaveformAssetToLayer(const std::string& subType,
                               const std::string& payload, WaveLayer& layer);

// ---- Standalone single-frame instrument ("__framesynth__:") ----------------
//
// A standalone single-frame instrument node (Layered / Frequency Domain /
// Wavelet Space / Inharmonic / Sample / Granular) wraps ONE IWavetableFrame in
// a 1x1-grid WavetableDoc and stores it as "__framesynth__:" + doc.encode().
// The synth strips the prefix and renders it through the normal wavetable path
// (see terrain_synth.h kFrameSynthPrefix / effectiveSynthScript); the editor
// dispatch routes the prefix to LayeredWaveEditorComponent, which detects the
// wrapper and runs in FOCUSED mode (the wavetable-only surfaces hidden). These
// helpers are the single source of truth for building / reading that wrapper so
// the menu-creation code, the editor, and the self-tests all agree.

// Wrap one frame as a 1x1-grid WavetableDoc (the canonical single-frame doc).
WavetableDoc makeSingleFrameWavetable(std::unique_ptr<IWavetableFrame> frame);

// Encode a single frame as a "__framesynth__:" standalone node script.
std::string encodeFrameSynthScript(std::unique_ptr<IWavetableFrame> frame);

// Build a default (factory) "__framesynth__:" script for one of the six frame
// types (typeId is "layered" / "spectral" / "wavelet" / "sample" / "granular" /
// "inharmonic"). Returns an empty string for an unknown id. Used by the
// node-graph Add-node menu so frame-instrument creation lives behind one helper
// that knows how to construct each frame type's default.
std::string defaultFrameSynthScriptForType(const std::string& typeId);

// Decode a "__framesynth__:" script back to its single frame (a clone), or
// nullptr if `script` is not a framesynth script / fails to decode. `outDoc`,
// when non-null, receives the full decoded doc (for the editor, which keeps the
// doc as its edit buffer).
std::unique_ptr<IWavetableFrame> decodeFrameSynthScript(const std::string& script,
                                                        WavetableDoc* outDoc = nullptr);

// resolveWaveformReferences(NodeGraph&) is declared in node_graph.h (so the
// non-GUI serialization layer can call it without pulling in the editor),
// and implemented in layered_wave_editor.cpp alongside WavetableDoc decode.

// Editor window contents (paired with a juce::DialogWindow launched by the caller).
// Edits `node.script` directly. onApply() should request a graph rebuild so
// the new waveform takes effect; it is called on a debounce timer (not on
// every slider tick) to avoid racing JUCE's async graph rebuild.
//
// A horizontal slider whose *draggable* travel spans [0, dragMax] (the
// comfortable range a user normally wants), while its underlying value range
// and text box accept anything up to a much larger safety ceiling. Values
// above dragMax simply pin the thumb at the right end instead of being
// clamped, so the text field is a free numeric entry (type 6, 8, etc.) without
// letting the drag run off to absurd values. Used for the per-waveform gain:
// drag covers 0..4x, but you can still type a higher (or lower) figure.
class FreeEntrySlider : public juce::Slider {
public:
    FreeEntrySlider()
        : juce::Slider(juce::Slider::LinearHorizontal,
                       juce::Slider::TextBoxRight) {}
    double dragMax = 4.0;
    double valueToProportionOfLength(double value) override {
        return juce::jlimit(0.0, 1.0, value / dragMax);
    }
    double proportionOfLengthToValue(double proportion) override {
        return juce::jlimit(0.0, dragMax, proportion * dragMax);
    }
};

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
    // Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y while THIS editor's pop-out window holds
    // keyboard focus. The editor is a separate top-level DialogWindow, so these
    // keystrokes never reach MainContentComponent::keyPressed (the main-window
    // undo handler) - without this override, pressing Ctrl+Z right after a
    // capture (the dialog has focus) does nothing. Routes to the shared
    // graph.undoTree so undo/redo works identically from inside the editor; the
    // snapshot restore then refreshes this editor via reloadFromNode().
    bool keyPressed(const juce::KeyPress& key) override;

    // Re-read this editor's `wave` document from its node's (restored) script
    // and rebuild the UI WITHOUT committing. Called after an external undo/redo
    // rewrites the node out from under the editor: the in-memory `wave` is
    // decoded only at construction, so a snapshot restore would otherwise leave
    // the editor showing stale data - and its debounce timer would re-commit
    // that stale data, silently undoing the undo. Cancels the pending debounce,
    // re-decodes, preserves the selection when it survives, and refreshes every
    // view. Closes the window if the node itself was undone away.
    void reloadFromNode();

    // Refresh every open wavetable editor after an undo/redo snapshot restore.
    // Iterates the live-editor registry and calls reloadFromNode() on each
    // editor bound to `g`. Invoked from the main window's onLoadSnapshot once
    // the graph has been reparsed from the snapshot. Static because the main
    // window doesn't own the editors' (non-modal DialogWindow) lifetimes.
    static void reloadOpenEditorsAfterSnapshot(NodeGraph& g);

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

    // ---- Standalone single-frame "frame synth" focus mode ----
    // When the node's script is a __framesynth__ wrapper (a single-frame
    // WavetableDoc representing one of the six standalone instrument node
    // types - Layered / Spectral / Wavelet / Inharmonic / Sample / Granular),
    // this editor runs in a FOCUSED mode: the wavetable-only surfaces (grid /
    // scatter arrangement view, library list, + Waveform, multi-frame Position
    // morph, mode conversion, the asset-library row) are hidden, leaving just
    // the single frame's editor plus the generic Gain / Preview / Envelope /
    // Morph toolbar. The underlying WavetableDoc still holds exactly one frame
    // and the entire wavetable render path is reused unchanged - the frame
    // synth is a distinct node type to the user, never a "1-frame wavetable in
    // disguise", but shares 100% of the DSP and sub-editor code.
    //   frameSynthMode : true when opened on a __framesynth__ node.
    //   scriptPrefix   : "" for a real wavetable, kFrameSynthPrefix for a frame
    //                    synth. Re-prepended on every commitToNode() so the
    //                    wrapper survives round-trips; decode paths always strip
    //                    it via effectiveSynthScript().
    bool frameSynthMode = false;
    std::string scriptPrefix;

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

    // Which surface the user most recently SELECTED on. The keyboard Delete
    // key needs this because currentLibraryId and currentFrameIdx are both
    // updated when you click a populated cell/dot (see Linkage above), so on
    // its own "is a library entry highlighted?" can't tell whether the user
    // means "delete this waveform definition" or "remove this placement".
    //   Library = last picked a row in the Library list  -> Delete removes the
    //             waveform DEFINITION (and every placement referencing it).
    //   View    = last picked a cell/dot in the arrangement view or Cells list
    //             -> Delete removes only THIS placement; the library waveform
    //             survives so it can be re-placed.
    // Set at the genuine click gestures only (library row onClick, Cells-list
    // row onClick, ScatterView cell/dot mouseDown) - NOT in the programmatic
    // switchToFrame / setEditingLibraryEntry paths - so internal navigation
    // (capture binding, post-delete fallback, drag-swap) never silently
    // changes which thing Delete will hit.
    enum class SelectionSurface { Library, View };
    SelectionSurface activeSelectionSurface = SelectionSurface::Library;
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

    // Open the unified waveform-library browser: the built-in single-cycle
    // library (category tree + search + curated ★) AND the project's user
    // Waveform assets in one picker, with "starred only" and "show user items"
    // filters. Selecting a built-in inserts an editable Drawn/Freehand
    // LayeredWaveform copy (see makeFactoryFrame); selecting a user asset
    // live-references it (edits propagate to every reference).
    //   replaceCurrentFrame == false: the + Waveform flow - ADD a new frame.
    //   replaceCurrentFrame == true:  the identity-row "Use Library..." flow -
    //     REPLACE the current frame's content/reference in place.
    // Anchor centres the dialog.
    void showWaveformLibraryBrowser(juce::Component* anchor,
                                    bool replaceCurrentFrame);

    // Per-layer "From Library..." loader: opens the waveform browser and, on a
    // pick, loads the chosen single cycle into layer `layerIndex` of the frame
    // currently bound to the layer stack (preserving that layer's ratio/phase/
    // amp). Wired into LayerStackComponent::Options::onPickFromLibrary.
    void showWaveformLibraryBrowserForLayer(int layerIndex);

    // Per-layer "Save to Library..." publisher: prompts for a name, publishes
    // layer `layerIndex` of the frame currently bound to the layer stack as a
    // single-layer Waveform asset, and live-links that layer to the new asset
    // (so subsequent edits propagate). Wired into LayerStackComponent::Options::
    // onSaveToLibrary.
    void publishLayerToLibrary(int layerIndex);

    // Per-layer "Unlink from Library" handler: clears layer `layerIndex`'s
    // assetId (detaching the live link) while keeping the current cycle as an
    // independent editable copy. Wired into LayerStackComponent::Options::
    // onDesyncFromLibrary.
    void desyncLayerFromLibrary(int layerIndex);

    // Build a one-layer LayeredWaveform whose single Drawn/Freehand layer holds
    // `cycle` (expected 512 samples in [-1,1], one cycle). This is the import
    // path for both a factory-bank entry and a user-loaded single-cycle wav: the
    // result is a fully editable, serialisable frame (the user can draw over it,
    // stack layers on it, warp it). Shared so both callers stay in sync. Public
    // (and static) so the self-test can exercise it without a live component.
public:
    // `factoryName` (when non-empty) marks the resulting single layer as a live
    // reference to the named built-in factory waveform (sets WaveLayer::factoryRef
    // so the project stores just the name, not the samples - see that field). Pass
    // the bank entry's stable name for a factory pick; leave empty for a user-
    // loaded single-cycle .wav (which must embed its samples).
    static std::unique_ptr<IWavetableFrame> makeFactoryFrame(
        const std::vector<float>& cycle, const std::string& factoryName = "");
private:

    juce::TextButton applyBtn    { "Apply" };
    juce::TextButton closeBtn    { "Close" };
    juce::TextButton helpBtn     { "?" };
    // Preview/Stop: audition the currently-edited frame through the owning
    // synth's voice path (envelope + Volume + downstream chain), exactly like
    // the granular ("from audio file") and inharmonic frame editors and the
    // capture dialogs. Ships the frame's rendered single cycle as a direct
    // audition payload so it's audible even when it isn't placed into the
    // wavetable grid, and refreshes live as the frame is edited (so you hear
    // changes while a note sustains). Labelled "Preview" (toggling to "Stop")
    // and sits in a button row near the bottom of the editor, matching every
    // other audition control in the app.
    juce::TextButton playBtn     { "Preview" };
    bool framePlaying = false;

    // Continuous low-rate poll that watches the LIVE node-param amounts for the
    // current frame's summation-morph (warp) slots and re-ships the preview +
    // held audition whenever they change underneath the editor - e.g. the user
    // drags the node-graph param slider of a PINNED morph op while this editor
    // is open. The preview/audition render at the live node-param amount
    // (renderEditingFrameLiveCycle), but only refresh on editor-originated
    // events; without this poll a node-side drag would appear to "do nothing"
    // (the held audition keeps overriding the synth terrain with the stale
    // cycle). A decoupled poll - rather than storing this editor's pointer in
    // the node graph for push notification - avoids any dangling-pointer risk
    // across graph.nodes reallocations.
    struct CallbackTimer : juce::Timer {
        std::function<void()> fn;
        void timerCallback() override { if (fn) fn(); }
    };
    CallbackTimer paramWatchTimer;
    std::vector<float> lastPolledWarpAmounts;
    void pollNodeParamChanges();
    // Opens the shared AHDSR amplitude-envelope editor for this synth's
    // node in a separate dialog (kept out of this already-dense editor).
    juce::TextButton envelopeBtn { "Envelope..." };
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
    juce::Label      nameFieldLabel { {}, "Name:" };
    juce::TextEditor nameEditor;

    // Per-waveform output gain (IWavetableFrame::gain). A horizontal slider on
    // the identity row whose drag spans 0..4x (1.0 = unity); the text box
    // accepts higher or lower typed values up to a safety ceiling. Because
    // every frame type peak-normalises its cycle, this is the ONLY way to make
    // one waveform louder/quieter than its neighbours; it scales the rendered
    // samples post-normalisation, so it shows in the preview and in the baked
    // synth output and morphs. Drag-end commits one undo step.
    juce::Label     gainLabel { {}, "Gain:" };
    FreeEntrySlider gainSlider;

    // Push the editor's current colour / name into the library entry the
    // editor is targeting, and reflect any change back into the library
    // list in the arrangement sidebar. Called whenever currentLibraryId
    // changes, the editor is constructed, or the user finishes editing.
    void refreshIdentityRow();

    // ---- Project asset-library reference row (below the gain row) ----------
    // Shows whether the currently-edited library entry live-references a
    // published Waveform asset, and lets the user (a) publish the current frame
    // as a new asset ("Save to Library"), or (b) repoint THIS frame to a library
    // waveform via the unified browser ("Use Library...", replaceCurrentFrame).
    // While referenced, edits to this waveform write back to the asset and
    // propagate to every other node referencing it (the live-reference model,
    // mirroring the AHDSR row). Selection uses the same category/starred/show-
    // user browser as the + Waveform flow, never a flat dropdown - the wave-shape
    // library is far too large for a combo (see agent-todo design refinement #2).
    juce::Label      assetLibStatus;
    juce::TextButton useLibraryBtn  { juce::String::fromUTF8("Use Library\xe2\x80\xa6") };
    juce::TextButton saveToLibBtn   { "Save to Library" };
    juce::TextButton desyncFromLibBtn { "Unlink" };
    // Refresh the status label + button enablement from the current entry's
    // assetId (shows the referenced asset name, or "Independent waveform").
    void refreshAssetLibRow();
    // Repoint the current entry to live-reference a user Waveform asset (mirror
    // the asset's frame in, preserving this slot's gain). Used by the identity-
    // row "Use Library..." browser flow when a user asset is chosen.
    void adoptWaveformAsset(int assetId);
    // Publish the current entry's frame as a new Waveform asset and link it.
    void publishCurrentWaveformToLibrary();
    // Detach the current entry's live link to a Waveform asset (set assetId = -1)
    // while keeping the current frame as an independent editable copy. Used by
    // the identity-row "Unlink" button; no-op when already independent.
    void desyncCurrentWaveformFromLibrary();
    // After committing this node, push every asset-referencing entry's frame
    // up to its asset and propagate to other nodes (live write-back). Called
    // from commitToNode(). No-op when no entry references an asset.
    void writeBackReferencedWaveforms();
    // Per-layer analogue: push every layer that live-references a Waveform asset
    // (WaveLayer::assetId >= 0) back up to its asset and propagate to every other
    // layer/frame sharing the id. Called from commitToNode(). No-op when no layer
    // references an asset. A layer linked to a multi-layer asset writes back a
    // single flattened cycle (see WaveLayer::assetId / layerToWaveformAsset).
    void writeBackPerLayerWaveforms();
    // Same for the frame-scope warp chain: if it references a shared
    // MorphAlgorithm asset, push the edited chain back and propagate. Called
    // from commitUndoStep(). No-op when warpAssetId < 0 (independent).
    void writeBackReferencedWarp();

    // Shared layer-stack widget (the "+ Layer" header, the scrolling list of
    // WaveLayerEditor rows, and per-layer add/delete). Identical code is used
    // by the Signal Shape (LFO / envelope) editor. Summation preview is OFF
    // here - the wavetable editor has its own multi-frame-type preview strip
    // (refreshPreview / previewBounds) that handles spectral / wavelet /
    // granular frames too, which the shared component knows nothing about.
    // Bound to the current layered frame via setTarget(currentEditingLayeredFrame()).
    std::unique_ptr<LayerStackComponent> layerStack;

    // Frame-scope warp chain editor (Bucket A shape-bending), bound to
    // wave.warpChain. Lives in the right pane under the layer stack. Its amounts
    // become "Warp N" node params (syncWarpParams) so they can be modulated.
    std::unique_ptr<WarpChainEditor> frameWarpEditor;

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
    // Add captured waveforms to the Library list ONLY - they are not placed
    // into the arrangement, so capturing never grows the grid's cell count
    // or adds a scatter dot. Placement is a separate explicit step via the
    // Library list's "Assign to selected cell". The first new entry becomes
    // the right-pane editor's target; the cell/scatter selection is left
    // untouched.
    //
    // sourceKind (0 = project song, 1 = mic, 2 = file; matches
    // showCapturePanelInline) is used only to build each new library
    // entry's display name - "Mic - Crossfade loop" etc. - reflecting the
    // capture source and the granular freeze method instead of a generic
    // "Waveform N". -1 falls back to the auto-generated default name.
    void addCapturedFramesToLibrary(std::vector<std::unique_ptr<IWavetableFrame>> frames,
                                    int sourceKind = -1);
    // Build the capture panel's metadata write-through sink: commits "As
    // note" pitch / freeze mode / crossfade edits straight to the library
    // frame the editor currently targets (currentLibraryId, re-looked-up
    // live so a doc mutation or a rebinding Save can never dangle). Shared by
    // the capture panel's replace mode (bound when the panel opens) and
    // append mode (bound after the first Save, once a frame exists to write
    // to) so a metadata edit can't be silently lost on Close in either mode.
    std::function<void(double, int, double, int, int)> makeCaptureMetadataSink();
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

    // Pop a modal text-entry dialog to rename the library entry `libId`, then
    // apply it via setLibraryEntryName(). The discoverable rename affordance
    // for the Library list rows and the arrangement-view dots/cells (their
    // right-click menus call this); the right-pane identity-row nameEditor is
    // the inline equivalent for whichever entry the editor is currently on.
    // No-op if libId no longer exists.
    void renameLibraryEntry(int libId);

    // Set library entry `libId`'s display name and run the full settle: commit
    // to the node script, push an undo step, rebuild the Library list, sync the
    // identity row if this is the current target, and ping the pop-out. Shared
    // by renameLibraryEntry() (popup) and the identity-row nameEditor (inline).
    // No-op if the entry is missing or the name is unchanged.
    void setLibraryEntryName(int libId, const std::string& newName);

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
    // ---- Frame audition (Play button) --------------------------------------
    // toggleFramePlay flips between start/stop. startFramePlay holds a
    // level-triggered audition on the owning synth node (Node::heldAudition)
    // carrying the current frame's rendered single cycle, so the synth voice
    // sustains it across the debounced graph rebuild an edit fires. stop clears
    // the held note. refreshHeldFrameAudition re-ships the cycle when the frame
    // is edited mid-play (or the editor target changes) so the sustained note
    // tracks the edit - "what you see = what you hear". All three look the node
    // up fresh via graph.findNode(nodeId) so a graph.nodes reallocation can
    // never leave a stale Node*.
    void toggleFramePlay();
    void startFramePlay();
    void stopFramePlay();
    void refreshHeldFrameAudition();
    // Render the currently-edited frame to a single cycle the way the SYNTH
    // sees it: the frame's summation-morph chain applied at the LIVE node-param
    // amounts (so a PINNED op reflects the value dialled in on the node-graph
    // param slider, not the frozen editor amount). For unpinned ops the param
    // mirrors the editor amount, so the result matches renderMorphed. Used by
    // the preview and the held audition so "what you see / hear" tracks the node
    // param slider for pinned morph stages. Returns false if there's no frame.
    bool renderEditingFrameLiveCycle(std::vector<float>& out);
    void commitToNode(); // encode `wave` into node.script
    // Push a graph undo snapshot so a settled wavetable edit enters the undo
    // system. Without this the edit lives only in the node's script (updated
    // by commitToNode) and is silently destroyed by any later, unrelated
    // Ctrl+Z that restores an earlier snapshot - which then gets saved,
    // making the waveform/cell "vanish" on reload.
    void commitUndoStep();
    void onLayerChanged();
    // Lighter sibling of onLayerChanged() for a continuous frame-scope morph
    // amount-slider drag: refreshes the on-screen preview and restarts the
    // debounce, but does NOT rewrite the node script or synchronously re-ship the
    // held audition every tick (the amount already flows to the synth via its
    // live-read "Warp N" param, and the 20 Hz poll re-ships the audition at a
    // de-clickable rate). The settled script commit happens in timerCallback.
    void onFrameWarpAmountDragged();
    void switchToFrame(int idx);
    void syncPositionParams();      // ensure node has the right number of Position params
    // Ensure the node carries exactly one frame-scope morph param per op across
    // EVERY frame's per-frame morph chain (IWavetableFrame::morphChain), keyed by
    // (warpFrameId, warpSlot), so the on-demand modulation-pin mechanism (#88)
    // can drive each morph amount with an LFO / oscillator. Adds missing params
    // (seeded from the op amount), removes params for deleted ops / frames (with
    // their mod pins / cables), and remaps surviving modPin param indices. Called
    // on a structural morph-chain edit. Delegates to syncWarpParamsForNode(doc).
    void syncWarpParams();
    // Re-point the per-frame summation-morph editor (frameWarpEditor) at the
    // current frame's morphChain + morphAssetId. Called from every flow that
    // changes the editor target (rebuildRows) or moves the storage (undo
    // restore). The chain is per-frame, so the editor must rebind on each switch.
    void rebindFrameWarpEditor();
    // Lightweight: mirror each op's current amount into its matching (un-
    // modulated) "Warp N" param so the synth's live read tracks the editor
    // slider without a full param rebuild. Called on every warp amount edit.
    void pushWarpAmountsToParams();
    // Reorder support: when two warp ops swap slots a<->b, swap the names of
    // their positional "Warp a+1" / "Warp b+1" node params so a wired
    // modulation cable keeps driving its op rather than the slot it vacated.
    // The param objects (and the modPins that reference them by index) stay
    // put in nd->params; only their names swap, so the synth's per-slot
    // getParamByName("Warp k+1") read now resolves each op to its original
    // (possibly modulated) param. No-op if either name is absent.
    void swapWarpParamNames(int a, int b);
    // Map frame-scope warp op `opIndex` to the index of its "Warp opIndex+1" node
    // param (the unified warp/morph model: the op's amount is a node param that
    // can opt into a modulation pin). Returns -1 if the param isn't present yet.
    // Used by the warp editor's per-row "Mod" checkbox callbacks.
    int warpParamIndexForOp(int opIndex) const;

    // ---- Per-layer warp modulation (#88, item-M) --------------------------
    // The layer stack's per-layer "Mod" checkbox routes (layer, opIndex) here.
    // A per-layer warp param is keyed by (Param::warpLayer == layer,
    // Param::warpSlot == opIndex) and exists ONLY while modulated (created on
    // check, destroyed on uncheck) - distinct from the always-present frame-
    // scope "Warp N" params. The synth re-bakes the modulated op's amount live
    // for a SINGLE-frame wavetable only, so the checkbox is gated to that case.
    //
    // perLayerWarpParamIndex: find the (layer, op) param's index, or -1.
    int  perLayerWarpParamIndex(int layer, int op) const;
    // True when per-layer warp modulation is offered (the wavetable holds exactly
    // one frame, the only shape the synth re-bakes live).
    bool perLayerWarpModSupported() const;
    // Query/toggle the (layer, op) per-layer warp modulation pin. Toggling on
    // creates the param (seeded from the op amount) + a mod pin; toggling off
    // drops the pin and the now-orphan param. No-op when unsupported.
    bool isLayerWarpOpModulated(int layer, int op) const;
    void setLayerWarpOpModulated(int layer, int op, bool on);
    // True only when an active Absolute ("Set") cable drives this (layer, op)'s
    // amount - the single condition that locks its slider. A bare pin or a "Mod"
    // cable leaves it editable (drag sets the base).
    bool isLayerWarpOpAmountLocked(int layer, int op) const;
    // Reconcile every per-layer warp param against the current editing frame's
    // chains (remove params for deleted ops + their pins, relabel survivors).
    // Called from onLayerChanged on every edit; early-outs when no per-layer warp
    // param exists. Drops all per-layer params when the table isn't single-frame.
    void reconcilePerLayerWarpParamsNow();
    // Empty when the (layer, op) op can be modulated; otherwise the reason its
    // "Mod" checkbox is disabled (the grayed-control-explains-itself rule).
    juce::String layerWarpModDisabledReason(int layer, int op) const;

    // ---- Per-layer Phase / Amplitude modulation (#88, item-M) -------------
    // Each layer's Phase/Amp "Mod" checkbox routes (layer, field) here, where
    // field 0 = Phase, 1 = Amplitude. A layer-field param is keyed by
    // (Param::warpLayer == layer, Param::layerField == field, Param::warpSlot ==
    // -1) and exists ONLY while modulated. The synth re-bakes the modulated
    // phase/amp live for a SINGLE-frame wavetable only (same gate as warp).
    int  layerFieldParamIndex(int layer, int field) const;
    bool isLayerFieldModulated(int layer, int field) const;
    void setLayerFieldModulated(int layer, int field, bool on);
    // Absolute-only lock for the Phase/Amp/generator slider (see warp-op twin).
    bool isLayerFieldAmountLocked(int layer, int field) const;
    juce::String layerFieldModDisabledReason(int layer, int field) const;
    // Re-sync Position params/pins only when the effective dimension count has
    // actually changed since the params were last built. Cheap to call on every
    // structural mutation (grid axis resize, scatter frame add/remove, cell
    // placement); skips the erase/re-add churn - and the audio-thread param read
    // race it would otherwise expose - whenever the count is unchanged.
    void maybeSyncPositionParams();
    // One block-rate modulation pin per Position axis, named by axis (X/Y/Z/W).
    // `pinToAxis` maps each existing Position pin's id to its axis index,
    // captured before syncPositionParams() reshuffles the Position params.
    void syncPositionModPins(Node& nd, const std::map<int, int>& pinToAxis);
};

} // namespace SoundShop

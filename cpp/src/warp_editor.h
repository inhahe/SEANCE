#pragma once

// =============================================================================
// WarpChainEditor - a reusable JUCE component that edits a chain of WarpOps
// (waveform shape-bending). The same widget drives:
//   * the frame-scope warp chain   (WavetableDoc::warpChain)   - modulatable
//   * a per-layer warp chain        (WaveLayer::warpChain)      - baked
//   * later: per spectral/wavelet/granular element chains (Bucket C)
//
// It does NOT own the chain: the host points it at a std::vector<WarpOp> via
// setChain() and rebinds whenever that storage moves (frame/layer selection
// change). Every mutation fires Callbacks::onChanged; structural changes (an op
// added or removed) also fire onStructureChanged so the host can re-layout and
// re-sync any "Warp N" modulation params.
//
// The method picker is a juce::PopupMenu grouped by WarpDomain, with a small
// star badge on `recommended` (higher-quality) methods - the quality cue the
// user asked for, kept inside the existing domain grouping rather than a
// separate category. Pure JUCE + warp.h; no audio-engine dependencies.
// =============================================================================

#include "warp.h"
#include "asset_library.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <memory>
#include <functional>

namespace SoundShop {

class WarpChainEditor : public juce::Component {
public:
    struct Callbacks {
        // Required. Fired on every mutation (op added/removed/enabled/method),
        // EXCEPT a bare amount-slider drag when onAmountChanged is also set (see
        // below).
        std::function<void()> onChanged;
        // Optional. Fired specifically when an amount SLIDER moves, instead of
        // onChanged. An amount is a continuous, high-frequency gesture and (for
        // hosts that back the chain with live-read node params) flows to the
        // synth through the modulatable param without a script rewrite - so those
        // hosts route it here to run a lighter path (update the param + visual
        // preview only) and avoid re-committing / re-shipping the audio preview on
        // every drag tick, which clicks. A method/enable/structure change still
        // goes through onChanged (those ARE baked into the script). When unset,
        // the amount slider falls back to onChanged (the baked-chain default).
        std::function<void()> onAmountChanged;
        // Optional. Fired only when the op LIST changes (add/remove). Hosts use
        // this to re-sync node "Warp N" params + re-layout the surrounding view.
        std::function<void()> onStructureChanged;
        // Optional. Fired when two ops swap position (reorder via the up/down
        // arrows), with the two affected slot indices (low first). The op COUNT
        // is unchanged, so this is distinct from onStructureChanged. Hosts that
        // map each slot to a positional "Warp N" modulation param use this to
        // keep a wired LFO/oscillator following its op rather than its slot
        // (see LayeredWaveEditorComponent::swapWarpParamNames). Baked-chain hosts
        // (per-layer / spectral / wavelet / granular) leave it unset; the
        // always-firing onChanged re-bakes them in the new order. onChanged
        // fires after this on every reorder.
        std::function<void(int, int)> onReorder;

        // Optional. The unified warp/morph model: each op's amount param can opt
        // into an on-demand modulation pin (#88) so an LFO/oscillator can drive
        // the morph live. The per-row "Mod" checkbox reflects/toggles that pin.
        // `isModulated(opIndex)` returns whether op `opIndex` currently has a
        // modulation pin; `setModulated(opIndex, on)` adds/removes it. Hosts that
        // back the chain with node params (the frame-scope warp editor) implement
        // both, mapping op i -> its "Warp N" param -> add/removeParamModPin. Baked
        // chains (per-layer / spectral / wavelet / granular) leave these unset and
        // the "Mod" checkbox is hidden for those rows. Toggling fires onChanged
        // after setModulated so the host can re-commit.
        std::function<bool(int)>      isModulated;
        std::function<void(int,bool)> setModulated;

        // Optional. When set and it returns a NON-empty string for op `opIndex`,
        // the per-row "Mod" checkbox is shown but DISABLED, with that string as
        // its tooltip (the grayed-control-explains-itself rule). Returns empty
        // when the op can be modulated (checkbox enabled). Hosts that only allow
        // modulation in some states - e.g. per-layer warp, which the synth only
        // re-bakes live for a single-frame wavetable - use this to explain why
        // the checkbox is unavailable instead of silently doing nothing.
        std::function<juce::String(int)> modDisabledReason;

        // Optional. Returns whether op `opIndex`'s amount slider should be LOCKED
        // (non-draggable) right now. Pinning an op alone does NOT lock its slider:
        // a pinned-but-uncabled op, or one driven by a *Modulate* ("Mod") cable,
        // stays editable - dragging sets the base/resting amount the modulation
        // swings around (mirrors the node-graph slider semantics). Only an active
        // *Absolute* ("Set") cable, which fully owns the value, locks the slider.
        // When unset, the editor falls back to locking whenever the op is pinned
        // (legacy behaviour) - so hosts that want the looser rule must provide it.
        std::function<bool(int)> isAmountLocked;
    };

    explicit WarpChainEditor(Callbacks cb);

    // Override the default "Warp  (shape-bending)" header title (and, optionally,
    // its tooltip). Hosts use this to give the editor a context-appropriate
    // user-facing name - e.g. the frame-scope summation editor reads "Summation
    // Morph" because it reshapes the combined output of every layer, not one
    // frame. "Morph" is the user-facing word for a shaping op (see the unified
    // warp/morph model); "warp" is the mechanism term. Pass an empty tooltip to
    // leave the existing one. Safe to call before or after setChain().
    void setHeaderText(juce::String title, juce::String tooltip = {});

    // Point the editor at the chain it edits. nullptr = nothing to edit (the
    // Add button is disabled). Triggers a full rebuild of the row widgets.
    void setChain(std::vector<WarpOp>* chain);
    std::vector<WarpOp>* getChain() const { return chain; }

    // Restrict the method picker to a subset of warp domains. Empty (the
    // default) = every domain is offered. Hosts whose representation only
    // supports some domains call this so the picker can't offer an inert
    // method - e.g. the granular element chain, where a continuous OLA grain
    // stream has no periodic phase axis, so only amplitude-domain
    // (waveshaping) methods are meaningful. Both the "+ Add" default method
    // and the popup menu respect the filter. `emptyHint` (optional) replaces
    // the default "press + Add..." prompt so the host can explain the
    // restriction. Call before setChain() / rebuild().
    void setAllowedDomains(std::vector<WarpDomain> domains,
                           juce::String emptyHint = {});

    // Optional integration with the project asset library's MorphAlgorithm
    // (warp-chain) store. When set, the editor shows a "Morph:" row: a picker to
    // reference a stored warp chain (live) and a "Save to Library" button to
    // publish the current chain. While a stored chain is referenced, the host's
    // settled-edit write-back pushes edits to the asset and propagates to every
    // frame sharing the id (the "live reference" model). "(Independent)" =
    // the editor edits the frame's own local chain, exactly as before. Only the
    // frame-scope warp editor opts in; the baked per-layer / spectral / wavelet
    // chains leave this unset and behave as before. Mirrors
    // AHDSREnvelopeComponent::LibraryContext.
    struct LibraryContext {
        AssetLibrary* lib = nullptr;
        std::function<int()>     getAssetId;   // current frame warpAssetId
        std::function<void(int)> setAssetId;   // set frame warpAssetId
        std::function<void()>    propagate;    // re-resolve all live references
    };
    void setLibraryContext(LibraryContext ctx);
    // Re-read the picker selection + Save button state from the bound asset id.
    // Call after an external change to the referenced id (undo, frame switch).
    void refreshLibraryRow();

    // Rebuild the per-op rows from the bound chain. Call after an external
    // mutation (preset load, undo restore) that changed the chain behind us.
    void rebuild();

    // Lightweight re-sync of every row's amount slider value + lock/enable state
    // from the bound chain, WITHOUT recreating the row widgets (so it won't
    // interrupt an in-flight interaction the way rebuild() would). Hosts call
    // this when the chain's amounts were changed underneath the editor by an
    // outside agent - e.g. the frame-scope editor mirrors a node-graph param
    // slider drag / a "Mod" modulation cable back into chain.amount and needs
    // the editor slider to follow. Slider updates use dontSendNotification so
    // this does not re-fire onChanged.
    void refreshAmounts();

    // Height this editor wants for the current op count - hosts use it to size
    // the slot they place the editor in (a vertical stack / scroll viewport).
    int preferredHeight() const;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void showMethodMenu(int opIndex);
    // Shared method-picker popup. Builds the domain-grouped menu (respecting the
    // allowed-domain filter), ticks `current`, and calls `onPick` with the chosen
    // method. Used both by the per-row method button (change an existing op) and
    // by "+ Add" (append a new op of the chosen method). Guarded by a SafePointer
    // so a closed-late menu can't call back into a destroyed editor.
    void showMethodPicker(juce::Component* target, WarpMethod current,
                          std::function<void(WarpMethod)> onPick);
    // Open the morph-library picker dialog ("Use Library..."), and apply the
    // user's choice. assetId -1 = detach to Independent; a built-in id = copy its
    // ops in (stays Independent); a user id = adopt the chain, live-linking it
    // when `sync` is true (edits propagate) or copying it when `sync` is false.
    void showMorphLibraryBrowser();
    void onMorphPicked(int assetId, bool sync);
    // "Unlink from Library": detach the live link to a saved morph (set the
    // bound warpAssetId to -1) while keeping the current chain as an independent
    // editable copy. No-op (and the button stays disabled) when already
    // independent. Mirrors the per-layer / frame Unlink affordances.
    void detachFromLibrary();
    void openAddToLibraryDialog();
    void addOp();
    void removeOp(int opIndex);
    // Swap op `idx` with its neighbour `idx + delta` (delta = -1 up / +1 down).
    // No-op if the target slot is out of range. Order is what defines the
    // signal-chain processing order, so this is a real edit (fires onReorder +
    // onChanged). The arrows that drive it are disabled at the ends.
    void moveOp(int idx, int delta);
    void refreshRowVisuals(int opIndex);

    struct Row {
        std::unique_ptr<juce::ToggleButton> enable;  // op on/off
        std::unique_ptr<juce::TextButton>   method;   // name + badge, opens picker
        std::unique_ptr<juce::Slider>       amount;   // 0..1 morph amount
        std::unique_ptr<juce::ToggleButton> mod;      // opt into a modulation pin
        std::unique_ptr<juce::TextButton>   up;       // move this stage earlier
        std::unique_ptr<juce::TextButton>   down;     // move this stage later
        std::unique_ptr<juce::TextButton>   del;      // remove this op
    };

    static constexpr int kHeaderH = 22;
    static constexpr int kRowH    = 24;

    bool domainAllowed(WarpDomain d) const;

    Callbacks cb;
    std::vector<WarpOp>* chain = nullptr;

    // Empty = all domains allowed. See setAllowedDomains().
    std::vector<WarpDomain> allowedDomains;
    juce::String            emptyHint;

    juce::Label      header;
    juce::TextButton addBtn;
    std::vector<Row> rows;

    // Library row (MorphAlgorithm store). Hidden until setLibraryContext.
    // A status label showing the current reference plus a "Use Library..." /
    // "Save to Library" button pair (the load/save halves), replacing the old
    // combo so the picker dialog can host a "Sync to library" choice.
    LibraryContext   libCtx;
    bool             libraryRowVisible = false;
    juce::Label      libraryLbl;
    juce::TextButton useLibBtn   { juce::String::fromUTF8("Use Library\xe2\x80\xa6") };
    juce::TextButton addToLibBtn { "Save to Library" };
    juce::TextButton unlinkLibBtn{ "Unlink" };
};

} // namespace SoundShop

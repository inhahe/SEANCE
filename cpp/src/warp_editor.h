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
        // Required. Fired on every mutation (op added/removed/enabled/amount).
        std::function<void()> onChanged;
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
    };

    explicit WarpChainEditor(Callbacks cb);

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
    void rebuildLibraryCombo();
    void onLibrarySelected(int comboId);
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
    LibraryContext   libCtx;
    bool             libraryRowVisible = false;
    juce::Label      libraryLbl  { {}, "Morph:" };
    juce::ComboBox   libraryCombo;
    juce::TextButton addToLibBtn { "Save to Library" };
};

} // namespace SoundShop

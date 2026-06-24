#include "warp_editor.h"
#include "dialog_helpers.h"

#include <map>

namespace SoundShop {

namespace {
const char* domainLabel(WarpDomain d) {
    switch (d) {
        case WarpDomain::Phase:      return "Phase  -  remap read position";
        case WarpDomain::Amplitude:  return "Amplitude  -  shape the value";
        case WarpDomain::Modulation: return "Modulation";
        case WarpDomain::Spectral:   return "Spectral";
        case WarpDomain::Wavelet:    return "Wavelet";
        case WarpDomain::Granular:   return "Granular";
    }
    return "Other";
}

// "Soft Clip  *" for recommended methods; the star is the quality badge.
juce::String methodButtonText(WarpMethod m) {
    const auto* info = warpMethodInfo(m);
    if (!info) return "Pick warp...";
    juce::String t = info->name;
    if (info->recommended) t += "   " + juce::String(juce::CharPointer_UTF8("\xe2\x98\x85")); // U+2605 star
    return t;
}

// =============================================================================
// MorphLibraryBrowser - modal picker for the summation-morph "Use Library..."
// button. Replaces the old library combo. Lists three groups:
//   * Independent (this frame's own local chain) - assetId -1
//   * Built-in presets (immutable templates; load a COPY, stay Independent)
//   * Saved morphs    (user MorphAlgorithm assets; can live-link when "Sync")
// A "Sync to library" checkbox is enabled only when a Saved entry is selected:
// built-ins are immutable starting points and Independent has nothing to sync.
// Fires onPick(assetId, sync); onCancel on dismissal. Mirrors the waveform
// picker's sync model so all three scopes (frame / layer / morph) read alike.
// =============================================================================
class MorphLibraryBrowser : public juce::Component,
                            public juce::ListBoxModel {
public:
    std::function<void(int assetId, bool sync)> onPick;

    MorphLibraryBrowser(AssetLibrary& lib, int currentId) {
        // No "Independent" row: this picker exists to LOAD a morph, so every
        // selectable row must populate the chain. Detaching to an independent
        // copy is done by the dedicated "Unlink from Library" button on the
        // editor, not by a no-op pick here (which used to silently load nothing).
        auto all = lib.list(AssetKind::MorphAlgorithm);
        std::vector<const AssetEntry*> builtins, users;
        for (auto* e : all)
            (isBuiltinMorphAssetId(e->id) ? builtins : users).push_back(e);
        if (!builtins.empty()) {
            rows.push_back({ -2, "Built-in presets", {}, true, false });
            for (auto* e : builtins)
                rows.push_back({ e->id, juce::String(e->name), {}, false, false });
        }
        if (!users.empty()) {
            rows.push_back({ -2, "Saved morphs", {}, true, false });
            for (auto* e : users)
                rows.push_back({ e->id, juce::String(e->name)
                                    + (e->archived ? "  [archived]" : ""),
                                {}, false, true });
        }

        list.setModel(this);
        list.setRowHeight(24);
        list.setColour(juce::ListBox::backgroundColourId,
                       juce::Colour(0xff202024));
        addAndMakeVisible(list);

        syncToggle.setButtonText("Sync to library");
        syncToggle.setTooltip(
            "Live-link this frame to the picked saved morph: editing the morph "
            "here updates every frame that references it, and vice versa. Off = "
            "load a one-time independent copy.");
        addAndMakeVisible(syncToggle);

        useBtn.setButtonText("Use");
        useBtn.onClick = [this] { commit(); };
        addAndMakeVisible(useBtn);
        cancelBtn.setButtonText("Cancel");
        cancelBtn.onClick = [this] { close(); };
        addAndMakeVisible(cancelBtn);

        // Pre-select the current reference if it's in the list; otherwise the
        // first selectable (non-header) row, so "Use" always loads something.
        int sel = -1;
        for (int i = 0; i < (int)rows.size(); ++i)
            if (!rows[i].isHeader && rows[i].assetId == currentId) { sel = i; break; }
        if (sel < 0)
            for (int i = 0; i < (int)rows.size(); ++i)
                if (!rows[i].isHeader) { sel = i; break; }
        if (sel >= 0) list.selectRow(sel);
        updateSyncEnabled();

        setSize(440, 380);
    }

    // ListBoxModel -----------------------------------------------------------
    int getNumRows() override { return (int)rows.size(); }

    void paintListBoxItem(int row, juce::Graphics& g, int w, int h,
                          bool selected) override {
        if (row < 0 || row >= (int)rows.size()) return;
        const auto& r = rows[row];
        if (r.isHeader) {
            g.setColour(juce::Colours::white.withAlpha(0.45f));
            g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            g.drawText(r.name.toUpperCase(),
                       juce::Rectangle<int>(8, 0, w - 12, h),
                       juce::Justification::centredLeft, true);
            return;
        }
        if (selected) {
            g.setColour(juce::Colour(0xff3a6ea5));
            g.fillRect(0, 0, w, h);
        }
        g.setColour(juce::Colours::white.withAlpha(r.isUser ? 0.95f : 0.85f));
        g.setFont(juce::FontOptions(13.0f));
        g.drawText("    " + r.name,
                   juce::Rectangle<int>(8, 0, w - 12, h),
                   juce::Justification::centredLeft, true);
    }

    void selectedRowsChanged(int) override { updateSyncEnabled(); }
    void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override { commit(); }

    void resized() override {
        auto a = getLocalBounds().reduced(10);
        auto bottom = a.removeFromBottom(30);
        cancelBtn.setBounds(bottom.removeFromRight(90));
        bottom.removeFromRight(8);
        useBtn.setBounds(bottom.removeFromRight(90));
        syncToggle.setBounds(bottom.removeFromLeft(160));
        a.removeFromBottom(8);
        list.setBounds(a);
    }

private:
    struct MorphRow {
        int          assetId;   // -1 Independent; -2 header; builtin/user id
        juce::String name;
        juce::String desc;
        bool         isHeader = false;
        bool         isUser   = false;
    };

    void updateSyncEnabled() {
        int sel = list.getSelectedRow();
        const bool isUser = (sel >= 0 && sel < (int)rows.size()
                             && rows[sel].isUser);
        syncToggle.setEnabled(isUser);
        if (!isUser) {
            syncToggle.setToggleState(false, juce::dontSendNotification);
            int sel2 = list.getSelectedRow();
            const bool builtin = (sel2 >= 0 && sel2 < (int)rows.size()
                                  && !rows[sel2].isHeader
                                  && rows[sel2].assetId >= 0);
            syncToggle.setTooltip(builtin
                ? "Built-in presets are immutable templates - they always load as "
                  "an independent copy, so there is nothing to sync."
                : "Pick a saved morph to enable live-linking.");
        } else {
            syncToggle.setTooltip(
                "Live-link this frame to the picked saved morph: editing the morph "
                "here updates every frame that references it, and vice versa. Off = "
                "load a one-time independent copy.");
        }
    }

    void commit() {
        int sel = list.getSelectedRow();
        if (sel < 0 || sel >= (int)rows.size() || rows[sel].isHeader) return;
        const int id = rows[sel].assetId;
        const bool sync = syncToggle.isEnabled() && syncToggle.getToggleState();
        if (onPick) onPick(id, sync);
        close();
    }

    void close() {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState(0);
    }

    juce::ListBox        list;
    juce::ToggleButton   syncToggle;
    juce::TextButton     useBtn, cancelBtn;
    std::vector<MorphRow> rows;
};
} // namespace

WarpChainEditor::WarpChainEditor(Callbacks callbacks) : cb(std::move(callbacks)) {
    header.setText("Warp  (shape-bending)", juce::dontSendNotification);
    header.setTooltip("Shape-bending stages applied in order. Phase warps remap "
                      "the read position; amplitude warps reshape the value. "
                      "Methods marked with a star are the higher-quality picks.");
    header.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    addAndMakeVisible(header);

    addBtn.setButtonText("+ Add");
    addBtn.setTooltip("Add a shape-bending stage: pick a method from the list and "
                      "it's appended as a new stage. Each stage has its own amount "
                      "and can be reordered or removed.");
    addBtn.onClick = [this] { addOp(); };
    addAndMakeVisible(addBtn);

    // Library row (hidden until setLibraryContext provides a library).
    libraryLbl.setJustificationType(juce::Justification::centredLeft);
    addChildComponent(libraryLbl);
    addChildComponent(useLibBtn);
    addChildComponent(addToLibBtn);
    addChildComponent(unlinkLibBtn);
    useLibBtn.setTooltip(
        "Load a ready-made morph from this project's library into the stack above. "
        "Built-in presets load a COPY you can freely tweak. A morph you Saved can "
        "load as a live link - check \"Sync to library\" in the picker and editing "
        "it here updates every frame that uses it. This is the load half of the "
        "Load/Save pair; \"+ Add\" instead builds a stack stage by stage.");
    addToLibBtn.setTooltip("Save the current morph stack to this project's library as "
                           "a reusable preset, so you can load it on other frames from "
                           "\"Use Library...\". (That picker is the matching Load.)");
    unlinkLibBtn.onClick = [this]() { detachFromLibrary(); };
    useLibBtn.onClick   = [this]() { showMorphLibraryBrowser(); };
    addToLibBtn.onClick = [this]() { openAddToLibraryDialog(); };
}

void WarpChainEditor::setLibraryContext(LibraryContext ctx) {
    libCtx = std::move(ctx);
    libraryRowVisible = (libCtx.lib != nullptr);
    libraryLbl.setVisible(libraryRowVisible);
    useLibBtn.setVisible(libraryRowVisible);
    addToLibBtn.setVisible(libraryRowVisible);
    unlinkLibBtn.setVisible(libraryRowVisible);
    if (libraryRowVisible) refreshLibraryRow();
    resized();
}

void WarpChainEditor::refreshLibraryRow() {
    if (!libraryRowVisible) return;
    // Status label: show whether this frame's morph is Independent or live-linked
    // to a named library morph (mirrors the waveform asset-row status text).
    int cur = libCtx.getAssetId ? libCtx.getAssetId() : -1;
    if (cur >= 0 && libCtx.lib) {
        const AssetEntry* e = libCtx.lib->find(cur);
        juce::String nm = (e && !e->name.empty()) ? juce::String(e->name)
                          : ("#" + juce::String(cur));
        if (e && e->archived) nm += "  [archived]";
        libraryLbl.setText(juce::String::fromUTF8("Morph: \xe2\x86\x92 ") + nm,
                           juce::dontSendNotification);
        libraryLbl.setColour(juce::Label::textColourId,
                             juce::Colour(0xffffcf4d).withAlpha(0.95f));
    } else {
        libraryLbl.setText("Morph: Independent (this frame's own)",
                           juce::dontSendNotification);
        libraryLbl.setColour(juce::Label::textColourId,
                             juce::Colours::white.withAlpha(0.6f));
    }
    // A morph algorithm with no stages is meaningless, so only allow publishing
    // a non-empty chain.
    const bool haveOps = chain && !chain->empty();
    addToLibBtn.setEnabled(haveOps);
    addToLibBtn.setTooltip(haveOps
        ? "Publish the current warp chain to the project library as a reusable "
          "morph algorithm, then reference it here."
        : "Add at least one warp stage before saving this as a shared morph "
          "algorithm.");
    // Unlink only does something while live-linked to a saved morph (cur >= 0).
    // Disabled-but-explained when already independent (grayed-control rule).
    const bool linked = (cur >= 0);
    unlinkLibBtn.setEnabled(linked);
    unlinkLibBtn.setTooltip(linked
        ? "Detach this frame's live link to the saved morph, keeping the current "
          "stack as an independent editable copy. Edits stop propagating to/from "
          "the library morph."
        : "This frame's morph is already independent - there's no library link to "
          "unlink. Use \"Use Library...\" and Sync to live-link it.");
}

void WarpChainEditor::onMorphPicked(int assetId, bool sync) {
    if (!libCtx.lib || !libCtx.setAssetId || !chain) return;
    if (assetId < 0) {
        // Detach to independent: keep the current chain as this frame's own
        // local copy (no payload change), just stop referencing.
        libCtx.setAssetId(-1);
        refreshLibraryRow();
        if (cb.onChanged) cb.onChanged();
        return;
    }
    if (const BuiltinMorphChain* b = builtinMorphChain(assetId)) {
        // Built-in template: COPY its ops in and stay Independent (built-ins are
        // immutable starting points, not live-reference assets - the same model
        // as copying a factory waveform into a layer). The op count changes, so
        // this is a structural edit: fire onStructureChanged so the host re-syncs
        // its "Warp N" params.
        *chain = b->ops;
        libCtx.setAssetId(-1);
        rebuild();
        refreshLibraryRow();
        if (cb.onChanged) cb.onChanged();
        if (cb.onStructureChanged) cb.onStructureChanged();
        return;
    }
    // User-saved chain. Mirror its ops into the bound chain. When `sync` is set,
    // also live-reference it (edits propagate to every frame sharing the id);
    // otherwise load a one-time independent copy. Either way the op count can
    // change, so fire onStructureChanged so the host re-syncs its "Warp N" params.
    libCtx.setAssetId(sync ? assetId : -1);
    if (const AssetEntry* e = libCtx.lib->find(assetId)) {
        *chain = decodeWarpChain(e->payload);
        rebuild();
    }
    refreshLibraryRow();
    if (cb.onChanged) cb.onChanged();
    if (cb.onStructureChanged) cb.onStructureChanged();
}

void WarpChainEditor::detachFromLibrary() {
    if (!libCtx.lib || !libCtx.setAssetId) return;
    int cur = libCtx.getAssetId ? libCtx.getAssetId() : -1;
    if (cur < 0) return; // already independent
    // Keep the current chain exactly as-is; just stop referencing the asset.
    // No structural change (op count is unchanged), so no onStructureChanged.
    libCtx.setAssetId(-1);
    refreshLibraryRow();
    if (cb.onChanged) cb.onChanged();
}

void WarpChainEditor::showMorphLibraryBrowser() {
    if (!libCtx.lib || !libCtx.setAssetId) return;
    int cur = libCtx.getAssetId ? libCtx.getAssetId() : -1;
    auto* browser = new MorphLibraryBrowser(*libCtx.lib, cur);
    juce::Component::SafePointer<WarpChainEditor> safe(this);
    browser->onPick = [safe](int assetId, bool sync) {
        if (safe != nullptr) safe->onMorphPicked(assetId, sync);
    };
    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(browser);
    opts.dialogTitle = "Use morph from Library";
    opts.dialogBackgroundColour = juce::Colour(0xff202024);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = true;
    opts.resizable = false;
    opts.componentToCentreAround = this;
    launchToolDialog(opts);
}

void WarpChainEditor::openAddToLibraryDialog() {
    if (!libCtx.lib || !libCtx.setAssetId || !chain || chain->empty()) return;
    auto* aw = new juce::AlertWindow("Add morph algorithm to Library",
        "Name for the shared warp chain:", juce::MessageBoxIconType::NoIcon, this);
    aw->addTextEditor("name", "Morph");
    aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    aw->enterModalState(true, juce::ModalCallbackFunction::create(
        [this, aw](int res) {
            if (res == 1 && chain) {
                auto name = aw->getTextEditorContents("name").trim().toStdString();
                if (name.empty()) name = "Morph";
                int id = libCtx.lib->add(AssetKind::MorphAlgorithm, name, "",
                                         encodeWarpChain(*chain));
                libCtx.setAssetId(id);
                refreshLibraryRow();
                if (cb.onChanged) cb.onChanged();
            }
            delete aw;
        }), true);
}

void WarpChainEditor::setHeaderText(juce::String title, juce::String tooltip) {
    header.setText(title, juce::dontSendNotification);
    if (tooltip.isNotEmpty()) header.setTooltip(tooltip);
}

void WarpChainEditor::setChain(std::vector<WarpOp>* c) {
    chain = c;
    rebuild();
}

void WarpChainEditor::setAllowedDomains(std::vector<WarpDomain> domains,
                                        juce::String hint) {
    allowedDomains = std::move(domains);
    emptyHint = std::move(hint);
}

bool WarpChainEditor::domainAllowed(WarpDomain d) const {
    if (allowedDomains.empty()) return true;  // unrestricted
    for (WarpDomain a : allowedDomains) if (a == d) return true;
    return false;
}

int WarpChainEditor::preferredHeight() const {
    const int n = chain ? (int)chain->size() : 0;
    return kHeaderH + (libraryRowVisible ? kRowH : 0) + n * kRowH + 4;
}

void WarpChainEditor::addOp() {
    if (!chain) return;
    // Open the method picker straight away so each "+ Add" lets the user choose
    // WHICH shape-bender to append. (Previously Add silently appended an op
    // pre-set to the first recommended method, so clicking Add again just added
    // another copy of that same default with no obvious way to pick a different
    // one - you had to know to click the row's method button afterwards.) The op
    // is created only once a method is chosen; cancelling adds nothing.
    showMethodPicker(&addBtn, WarpMethod::None, [this](WarpMethod chosen) {
        if (!chain || chosen == WarpMethod::None) return;
        WarpOp op;
        op.method  = chosen;
        op.amount  = 0.5f;
        op.enabled = true;
        chain->push_back(op);
        rebuild();
        if (cb.onChanged) cb.onChanged();
        if (cb.onStructureChanged) cb.onStructureChanged();
    });
}

void WarpChainEditor::removeOp(int idx) {
    if (!chain || idx < 0 || idx >= (int)chain->size()) return;
    chain->erase(chain->begin() + idx);
    rebuild();
    if (cb.onChanged) cb.onChanged();
    if (cb.onStructureChanged) cb.onStructureChanged();
}

void WarpChainEditor::moveOp(int idx, int delta) {
    if (!chain) return;
    const int j = idx + delta;
    if (idx < 0 || idx >= (int)chain->size()) return;
    if (j   < 0 || j   >= (int)chain->size()) return;
    std::swap((*chain)[idx], (*chain)[j]);
    rebuild();
    // onReorder first so positional bindings (a host's "Warp N" modulation
    // params) are fixed up BEFORE onChanged mirrors amounts / re-bakes. Not a
    // structure change (count unchanged) so onStructureChanged does NOT fire.
    if (cb.onReorder) cb.onReorder(std::min(idx, j), std::max(idx, j));
    if (cb.onChanged) cb.onChanged();
}

void WarpChainEditor::refreshRowVisuals(int idx) {
    if (!chain || idx < 0 || idx >= (int)rows.size() || idx >= (int)chain->size())
        return;
    const WarpOp& op = (*chain)[idx];
    auto& row = rows[idx];
    row.method->setButtonText(methodButtonText(op.method));
    if (const auto* info = warpMethodInfo(op.method))
        row.method->setTooltip(info->tooltip);
    row.enable->setToggleState(op.enabled, juce::dontSendNotification);
    // Reflect whether this op's amount is being driven by a modulation pin. When
    // modulated, the manual amount slider is signal-locked (greyed) so the user
    // knows the cable is in control - matching the node-graph "Mod:" pin behaviour.
    bool modulated = false;
    if (row.mod && cb.isModulated) {
        modulated = cb.isModulated(idx);
        row.mod->setToggleState(modulated, juce::dontSendNotification);
        // Grayed-control-explains-itself: when the host reports a reason this op
        // can't be modulated right now (e.g. per-layer warp only re-bakes a
        // single-frame wavetable), disable the checkbox and surface the reason as
        // its tooltip instead of the generic "add a pin" copy. Empty reason (the
        // common case) = enabled.
        juce::String reason =
            cb.modDisabledReason ? cb.modDisabledReason(idx) : juce::String();
        const bool modAvail = reason.isEmpty();
        row.mod->setEnabled(modAvail);
        row.mod->setTooltip(modAvail
            ? "Add an input pin for this stage's amount, so a cable (LFO, "
              "oscillator, automation) can drive the morph live. The pin can run "
              "in Mod or Set mode. Uncheck to remove the pin and edit the amount "
              "by hand."
            : reason);
    }
    // Disabled op = greyed amount slider so the bypass is visible. The amount is
    // ALSO locked when an active *Absolute* ("Set") cable owns the value - but a
    // mere pin (no cable, or a "Mod" cable) leaves the slider editable: dragging
    // then sets the base amount the modulation swings around, exactly like the
    // node-graph slider. Hosts that don't supply isAmountLocked fall back to the
    // legacy "locked whenever pinned" rule.
    bool amountLocked = cb.isAmountLocked ? cb.isAmountLocked(idx) : modulated;
    row.amount->setEnabled(op.enabled && !amountLocked);
    if (row.amount)
        row.amount->setTooltip(amountLocked
            ? "Signal-locked - this stage's amount is driven by an incoming "
              "Set (absolute) cable. Disconnect it (or switch the pin to Mod) to "
              "edit the amount by hand."
            : modulated
            ? "Morph amount base (0 = no effect, 1 = full). A modulation cable "
              "swings the live amount around this resting value - drag to set the "
              "centre it modulates around."
            : "Morph amount (0 = no effect, 1 = full). Check Pin to drive this "
              "with a cable (LFO / oscillator / automation) instead.");
}

void WarpChainEditor::refreshAmounts() {
    if (!chain) return;
    const int n = juce::jmin((int)rows.size(), (int)chain->size());
    for (int i = 0; i < n; ++i) {
        if (rows[i].amount)
            rows[i].amount->setValue((*chain)[i].amount, juce::dontSendNotification);
        refreshRowVisuals(i);
    }
}

void WarpChainEditor::rebuild() {
    rows.clear();
    if (chain) {
        rows.reserve(chain->size());
        for (int i = 0; i < (int)chain->size(); ++i) {
            Row row;

            row.enable = std::make_unique<juce::ToggleButton>();
            row.enable->setTooltip("Enable / bypass this warp stage.");
            row.enable->onClick = [this, i] {
                if (!chain || i >= (int)chain->size()) return;
                (*chain)[i].enabled = rows[i].enable->getToggleState();
                refreshRowVisuals(i);
                if (cb.onChanged) cb.onChanged();
            };
            addAndMakeVisible(*row.enable);

            row.method = std::make_unique<juce::TextButton>();
            row.method->onClick = [this, i] { showMethodMenu(i); };
            addAndMakeVisible(*row.method);

            row.amount = std::make_unique<juce::Slider>(
                juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
            row.amount->setRange(0.0, 1.0, 0.001);
            row.amount->setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 18);
            row.amount->setTooltip("Morph amount (0 = no effect, 1 = full). "
                                   "Right-click the node param to drive this with "
                                   "an LFO / oscillator.");
            row.amount->setValue((*chain)[i].amount, juce::dontSendNotification);
            row.amount->onValueChange = [this, i] {
                if (!chain || i >= (int)chain->size()) return;
                (*chain)[i].amount = (float)rows[i].amount->getValue();
                // Amount is a continuous gesture: prefer the lighter
                // onAmountChanged path when the host provides it, so a drag
                // doesn't re-commit / re-ship the audio preview every tick.
                if (cb.onAmountChanged)   cb.onAmountChanged();
                else if (cb.onChanged)    cb.onChanged();
            };
            addAndMakeVisible(*row.amount);

            // "Pin" checkbox - opt this op's amount into a live modulation input
            // pin (#88), the unified warp/morph model. Labelled "Pin" rather than
            // "Mod" because the pin it creates can run in either Mod or Set mode
            // (those are the node-graph pin types) - the checkbox just exposes the
            // input. Only meaningful when the host backs the chain with node params
            // (frame-scope editor implements the callbacks); hidden on baked chains
            // that leave the callbacks unset.
            row.mod = std::make_unique<juce::ToggleButton>("Pin");
            row.mod->setTooltip("Add an input pin for this stage's amount, so a "
                                "cable (LFO, oscillator, automation) can drive the "
                                "morph live. The pin can run in Mod or Set mode. "
                                "Uncheck to remove the pin and edit the amount by "
                                "hand.");
            row.mod->setVisible((bool)cb.isModulated && (bool)cb.setModulated);
            row.mod->onClick = [this, i] {
                if (!cb.setModulated || i >= (int)rows.size()) return;
                cb.setModulated(i, rows[i].mod->getToggleState());
                refreshRowVisuals(i);
                if (cb.onChanged) cb.onChanged();
            };
            addAndMakeVisible(*row.mod);

            // Reorder arrows. Shape-bending stages are applied in list order, so
            // the order is part of the sound (fold-then-clip != clip-then-fold).
            const int last = (int)chain->size() - 1;
            row.up = std::make_unique<juce::TextButton>(
                juce::String(juce::CharPointer_UTF8("\xe2\x96\xb2")));  // U+25B2
            row.up->setTooltip(i == 0
                ? "Already the first stage - nothing to move it before."
                : "Move this stage earlier in the chain (it shapes the wave "
                  "sooner). Order matters: fold-then-clip sounds different from "
                  "clip-then-fold.");
            row.up->setEnabled(i > 0);
            row.up->onClick = [this, i] { moveOp(i, -1); };
            addAndMakeVisible(*row.up);

            row.down = std::make_unique<juce::TextButton>(
                juce::String(juce::CharPointer_UTF8("\xe2\x96\xbc")));  // U+25BC
            row.down->setTooltip(i == last
                ? "Already the last stage - nothing to move it after."
                : "Move this stage later in the chain (it shapes the wave "
                  "after the stages above it).");
            row.down->setEnabled(i < last);
            row.down->onClick = [this, i] { moveOp(i, +1); };
            addAndMakeVisible(*row.down);

            row.del = std::make_unique<juce::TextButton>("X");
            row.del->setTooltip("Remove this warp stage.");
            row.del->onClick = [this, i] { removeOp(i); };
            addAndMakeVisible(*row.del);

            rows.push_back(std::move(row));
            refreshRowVisuals(i);
        }
    }
    addBtn.setEnabled(chain != nullptr);
    if (libraryRowVisible) refreshLibraryRow();
    resized();
    repaint();
}

void WarpChainEditor::showMethodMenu(int idx) {
    if (!chain || idx < 0 || idx >= (int)chain->size()) return;
    showMethodPicker(rows[idx].method.get(), (*chain)[idx].method,
        [this, idx](WarpMethod chosen) {
            if (!chain || idx >= (int)chain->size()) return;
            (*chain)[idx].method = chosen;
            refreshRowVisuals(idx);
            if (cb.onChanged) cb.onChanged();
        });
}

void WarpChainEditor::showMethodPicker(juce::Component* target, WarpMethod current,
                                       std::function<void(WarpMethod)> onPick) {
    // Group methods by domain, preserving registry order within each group.
    std::map<WarpDomain, std::vector<const WarpMethodInfo*>> byDomain;
    std::vector<WarpDomain> domainOrder;
    for (const auto& info : warpMethodRegistry()) {
        if (info.method == WarpMethod::None) continue;
        if (!domainAllowed(info.domain)) continue;  // host restricts the picker
        if (byDomain.find(info.domain) == byDomain.end())
            domainOrder.push_back(info.domain);
        byDomain[info.domain].push_back(&info);
    }

    juce::PopupMenu menu;
    for (WarpDomain d : domainOrder) {
        menu.addSectionHeader(domainLabel(d));
        for (const auto* info : byDomain[d]) {
            const bool ticked = (info->method == current);
            juce::String txt = info->name;
            if (info->recommended)
                txt += "   " + juce::String(juce::CharPointer_UTF8("\xe2\x98\x85"));
            menu.addItem((int)info->method + 1, txt, true, ticked);
        }
    }

    // SafePointer-guard so a menu that closes after this editor is destroyed (a
    // frame/layer switch tearing the chain down) can't call back into a dead
    // object. While `safe` is alive, the onPick lambda's captured `this` is valid.
    juce::Component::SafePointer<WarpChainEditor> safe(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(target),
        [safe, onPick = std::move(onPick)](int chosen) {
            if (chosen <= 0 || safe == nullptr) return;
            onPick((WarpMethod)(chosen - 1));
        });
}

void WarpChainEditor::resized() {
    auto a = getLocalBounds().reduced(2);

    auto top = a.removeFromTop(kHeaderH - 2);
    addBtn.setBounds(top.removeFromRight(60));
    header.setBounds(top);

    if (libraryRowVisible) {
        auto lib = a.removeFromTop(kRowH).reduced(0, 2);
        // Three buttons on the right (Save | Use Library | Unlink), status label
        // fills the rest on the left. Mirrors the waveform asset-row layout.
        addToLibBtn.setBounds(lib.removeFromRight(110));
        lib.removeFromRight(4);
        useLibBtn.setBounds(lib.removeFromRight(110));
        lib.removeFromRight(4);
        unlinkLibBtn.setBounds(lib.removeFromRight(64));
        lib.removeFromRight(8);
        libraryLbl.setBounds(lib);
    }

    for (int i = 0; i < (int)rows.size(); ++i) {
        auto r = a.removeFromTop(kRowH).reduced(0, 2);
        rows[i].enable->setBounds(r.removeFromLeft(22));
        rows[i].del->setBounds(r.removeFromRight(22));
        rows[i].down->setBounds(r.removeFromRight(20));
        rows[i].up->setBounds(r.removeFromRight(20));
        r.removeFromRight(4);
        // "Mod" checkbox sits between the arrows and the amount slider. Only
        // takes layout space when the host wired up the modulation callbacks.
        if (rows[i].mod && rows[i].mod->isVisible()) {
            rows[i].mod->setBounds(r.removeFromRight(52));
            r.removeFromRight(4);
        }
        rows[i].amount->setBounds(r.removeFromRight(150));
        r.removeFromRight(4);
        rows[i].method->setBounds(r);
    }
}

void WarpChainEditor::paint(juce::Graphics& g) {
    // Faint separator under the header so the warp section reads as its own
    // group inside whatever host panel embeds it.
    g.setColour(juce::Colours::white.withAlpha(0.10f));
    g.drawHorizontalLine(kHeaderH - 1, 2.0f, (float)getWidth() - 2.0f);

    if (chain && chain->empty()) {
        g.setColour(juce::Colours::white.withAlpha(0.45f));
        g.setFont(juce::FontOptions(12.0f));
        juce::String hint = emptyHint.isNotEmpty()
            ? emptyHint
            : "No shape-bending - press + Add to fold / clip / bend this wave.";
        const int hintTop = kHeaderH + (libraryRowVisible ? kRowH : 0);
        g.drawText(hint,
                   getLocalBounds().withTop(hintTop).reduced(6, 2),
                   juce::Justification::centredLeft, true);
    }
}

} // namespace SoundShop

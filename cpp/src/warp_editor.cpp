#include "warp_editor.h"

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
    libraryLbl.setJustificationType(juce::Justification::centredRight);
    addChildComponent(libraryLbl);
    addChildComponent(libraryCombo);
    addChildComponent(addToLibBtn);
    libraryCombo.setTooltip("Reference a shared morph algorithm (warp chain) from "
                            "this project's library. While referenced, editing the "
                            "chain here updates every frame that uses it. Pick "
                            "(Independent) to give this frame its own copy.");
    addToLibBtn.setTooltip("Publish the current warp chain to the project library "
                           "as a reusable morph algorithm, then reference it here.");
    libraryCombo.onChange = [this]() { onLibrarySelected(libraryCombo.getSelectedId()); };
    addToLibBtn.onClick   = [this]() { openAddToLibraryDialog(); };
}

void WarpChainEditor::setLibraryContext(LibraryContext ctx) {
    libCtx = std::move(ctx);
    libraryRowVisible = (libCtx.lib != nullptr);
    libraryLbl.setVisible(libraryRowVisible);
    libraryCombo.setVisible(libraryRowVisible);
    addToLibBtn.setVisible(libraryRowVisible);
    if (libraryRowVisible) refreshLibraryRow();
    resized();
}

void WarpChainEditor::refreshLibraryRow() {
    if (!libraryRowVisible) return;
    rebuildLibraryCombo();
    // A morph algorithm with no stages is meaningless, so only allow publishing
    // a non-empty chain.
    const bool haveOps = chain && !chain->empty();
    addToLibBtn.setEnabled(haveOps);
    addToLibBtn.setTooltip(haveOps
        ? "Publish the current warp chain to the project library as a reusable "
          "morph algorithm, then reference it here."
        : "Add at least one warp stage before saving this as a shared morph "
          "algorithm.");
}

void WarpChainEditor::rebuildLibraryCombo() {
    if (!libCtx.lib) return;
    libraryCombo.clear(juce::dontSendNotification);
    libraryCombo.addItem("(Independent)", 1);   // reserved id 1 (user ids >= 1e6)
    int cur = libCtx.getAssetId ? libCtx.getAssetId() : -1;
    bool curListed = false;

    // Every morph chain now lives in the project library: the curated built-ins
    // are seeded there (seedBuiltinMorphLibrary), and the user's saved chains are
    // published there. Source the picker from that ONE list and partition it into
    // "Built-in" (reserved id range) and "Saved" (user ids) so the two groups
    // still read distinctly. Selecting a built-in COPIES its ops into an
    // Independent chain (a template, not a live reference - see onLibrarySelected),
    // so a built-in id is never stored as a frame's assetId; it only appears as a
    // transient combo pick. Listing built-ins first keeps a fresh project's picker
    // from being just "(Independent)".
    auto allMorphs = libCtx.lib->list(AssetKind::MorphAlgorithm);
    std::vector<const AssetEntry*> builtins, userEntries;
    for (const AssetEntry* e : allMorphs)
        (isBuiltinMorphAssetId(e->id) ? builtins : userEntries).push_back(e);

    if (!builtins.empty()) {
        libraryCombo.addSectionHeading("Built-in");
        for (const AssetEntry* e : builtins) {
            libraryCombo.addItem(juce::String(e->name), e->id);
            if (e->id == cur) curListed = true;
        }
    }

    if (!userEntries.empty()) libraryCombo.addSectionHeading("Saved");
    for (const AssetEntry* e : userEntries) {
        juce::String nm = e->name.empty() ? ("#" + juce::String(e->id))
                                          : juce::String(e->name);
        libraryCombo.addItem(nm, e->id);
        if (e->id == cur) curListed = true;
    }
    // If the referenced algorithm is archived (hidden from the normal list),
    // still show it so the user sees what they're referencing.
    if (cur >= 0 && !curListed) {
        const AssetEntry* e = libCtx.lib->find(cur);
        if (e) libraryCombo.addItem(juce::String(e->name) + "  [archived]", e->id);
    }
    libraryCombo.setSelectedId(cur >= 0 ? cur : 1, juce::dontSendNotification);
}

void WarpChainEditor::onLibrarySelected(int comboId) {
    if (!libCtx.lib || !libCtx.setAssetId || !chain) return;
    if (comboId == 1) {
        // Detach to independent: keep the current chain as this frame's own
        // local copy (no payload change), just stop referencing.
        libCtx.setAssetId(-1);
        refreshLibraryRow();
        if (cb.onChanged) cb.onChanged();
        return;
    }
    if (const BuiltinMorphChain* b = builtinMorphChain(comboId)) {
        // Built-in template: COPY its ops in and stay Independent (built-ins are
        // immutable starting points, not live-reference assets - the same model
        // as copying a factory waveform into a layer). The op count changes, so
        // this is a structural edit: fire onStructureChanged so the host re-syncs
        // its "Warp N" params. refreshLibraryRow resets the combo to "(Independent)"
        // since the assetId is now -1.
        *chain = b->ops;
        libCtx.setAssetId(-1);
        rebuild();
        refreshLibraryRow();
        if (cb.onChanged) cb.onChanged();
        if (cb.onStructureChanged) cb.onStructureChanged();
        return;
    }
    // Adopt the chosen library chain: mirror it into the bound chain and
    // reference it. The op count can change, so this is a structural edit -
    // fire onStructureChanged so the host re-syncs its "Warp N" params.
    libCtx.setAssetId(comboId);
    if (const AssetEntry* e = libCtx.lib->find(comboId)) {
        *chain = decodeWarpChain(e->payload);
        rebuild();
    }
    refreshLibraryRow();
    if (cb.onChanged) cb.onChanged();
    if (cb.onStructureChanged) cb.onStructureChanged();
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
                rebuildLibraryCombo();
                libraryCombo.setSelectedId(id, juce::dontSendNotification);
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
    // Disabled op = greyed amount slider so the bypass is visible. A modulated
    // amount is also greyed (driven by the incoming cable, not the slider).
    row.amount->setEnabled(op.enabled && !modulated);
    if (row.amount)
        row.amount->setTooltip(modulated
            ? "Signal-locked - this stage's amount is driven by an incoming "
              "modulation cable. Uncheck Pin to edit it by hand."
            : "Morph amount (0 = no effect, 1 = full). Check Pin to drive this "
              "with a cable (LFO / oscillator / automation) instead.");
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
                if (cb.onChanged) cb.onChanged();
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
        libraryLbl.setBounds(lib.removeFromLeft(48));
        lib.removeFromLeft(4);
        addToLibBtn.setBounds(lib.removeFromRight(120));
        lib.removeFromRight(6);
        libraryCombo.setBounds(lib);
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

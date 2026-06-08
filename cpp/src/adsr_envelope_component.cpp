#include "adsr_envelope_component.h"
#include <algorithm>
#include <cmath>

namespace SoundShop {

// ==============================================================================
// AHDSREnvelopeComponent
// ==============================================================================
AHDSREnvelopeComponent::AHDSREnvelopeComponent(AHDSREnvelope& e,
                                               std::function<void()> oc)
    : env(e), onChanged(std::move(oc))
{
    auto setupSlider = [this](juce::Slider& s, double lo, double hi,
                              double step, const juce::String& suffix,
                              double initial) {
        s.setSliderStyle(juce::Slider::LinearVertical);
        s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 18);
        s.setRange(lo, hi, step);
        s.setValue(initial, juce::dontSendNotification);
        s.setTextValueSuffix(suffix);
        addAndMakeVisible(s);
    };

    setupSlider(attackS,  0.0, 10000.0, 0.1, " ms", env.attackMs);
    setupSlider(holdS,    0.0, 10000.0, 0.1, " ms", env.holdMs);
    setupSlider(decayS,   0.0, 10000.0, 0.1, " ms", env.decayMs);
    setupSlider(sustainS, 0.0, 1.0,     0.01, "",   env.sustain);
    setupSlider(releaseS, 0.0, 10000.0, 0.1, " ms", env.releaseMs);
    setupSlider(velS,     0.0, 1.0,     0.01, "",   env.velocitySensitivity);

    // Skewed time sliders so the bottom half of the throw maps to the
    // 0-100ms range musicians actually want fine control over; a linear
    // 0-10000 slider would shove all useful values into a few pixels.
    attackS.setSkewFactorFromMidPoint(100.0);
    holdS  .setSkewFactorFromMidPoint(100.0);
    decayS .setSkewFactorFromMidPoint(300.0);
    releaseS.setSkewFactorFromMidPoint(500.0);

    auto bind = [this](juce::Slider& s, float& field) {
        s.onValueChange = [this, &s, &field]() {
            field = (float)s.getValue();
            commitChange();
        };
        s.onDragEnd = [this]() { commitChange(); };
    };
    bind(attackS,  env.attackMs);
    bind(holdS,    env.holdMs);
    bind(decayS,   env.decayMs);
    bind(sustainS, env.sustain);
    bind(releaseS, env.releaseMs);
    bind(velS,     env.velocitySensitivity);

    // Labels above each slider.
    for (auto* l : { &attackLbl, &holdLbl, &decayLbl, &sustainLbl,
                     &releaseLbl, &velLbl }) {
        l->setJustificationType(juce::Justification::centred);
        l->setFont(juce::Font(11.0f, juce::Font::bold));
        addAndMakeVisible(*l);
    }

    // Tooltips. CLAUDE.md flags ADSR jargon as unsuitable for non-
    // musicians, so we add a plain-language note next to each technical
    // label.
    attackLbl .setTooltip("Time (ms) for the note to fade IN from silence after a key press. Short = snappy; long = swelling.");
    holdLbl   .setTooltip("Time (ms) the note stays at full volume after the attack finishes, before starting to fade toward the sustain level. Use 0 for the classic ADSR shape.");
    decayLbl  .setTooltip("Time (ms) for the volume to fall from full to the Sustain level while the key is still held.");
    sustainLbl.setTooltip("Volume the note holds at while the key stays pressed. 0 = silent (note dies); 1 = full volume forever.");
    releaseLbl.setTooltip("Time (ms) for the volume to fade OUT after the key is released.");
    velLbl    .setTooltip("How much the key strike strength affects volume. 0 = ignore velocity (every note plays at full volume, organ-like). 1 = full velocity scaling (soft key press = soft note, piano-like).");
    sustainS  .setTooltip("Volume the note holds at while the key stays pressed (0..1).");

    // Top row controls.
    presetLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(presetLabel);
    addAndMakeVisible(presetCombo);
    addAndMakeVisible(saveAsBtn);
    addAndMakeVisible(manageBtn);
    presetCombo.setTooltip("Recall a saved envelope preset. Factory presets are marked with a star. Editing afterwards is fine - the preset isn't modified until you Save as.");
    saveAsBtn  .setTooltip("Save the current A/H/D/S/R values and curves as a named preset in the global library. Using the name of a factory preset overwrites it and removes its built-in marker. To restore the original later, delete the edited entry and click Restore Built-ins.");
    manageBtn  .setTooltip("Open the preset manager to rename, delete, or restore factory presets.");

    presetCombo.onChange = [this]() {
        int id = presetCombo.getSelectedId();
        if (id > 0) onPresetChosen(id);
    };
    saveAsBtn.onClick  = [this]() { openSaveAsDialog(); };
    manageBtn.onClick  = [this]() { openManageDialog(); };

    rebuildPresetCombo();
    // Subscribe to preset list changes so a Save-as in this component
    // (or in another instance) refreshes the dropdown.
    EnvelopePresetManager::instance().addListener([safeThis = juce::Component::SafePointer<AHDSREnvelopeComponent>(this)]() {
        if (safeThis) juce::MessageManager::callAsync([safeThis]() {
            if (safeThis) safeThis->rebuildPresetCombo();
        });
    });

    // Bottom buttons.
    addAndMakeVisible(editAttackBtn);
    addAndMakeVisible(editDecayBtn);
    addAndMakeVisible(editReleaseBtn);
    editAttackBtn .setTooltip("Edit the shape of the attack ramp (linear / exponential / freehand) using the same three-mode curve editor used elsewhere in the app.");
    editDecayBtn  .setTooltip("Edit the shape of the decay ramp.");
    editReleaseBtn.setTooltip("Edit the shape of the release tail.");
    editAttackBtn .onClick = [this]() {
        openCurveEditor("Attack Curve", env.attackCurve,
                        juce::Colour(120, 200, 120));
    };
    editDecayBtn.onClick = [this]() {
        openCurveEditor("Decay Curve", env.decayCurve,
                        juce::Colour(200, 200, 120));
    };
    editReleaseBtn.onClick = [this]() {
        openCurveEditor("Release Curve", env.releaseCurve,
                        juce::Colour(200, 140, 100));
    };
}

AHDSREnvelopeComponent::~AHDSREnvelopeComponent() = default;

void AHDSREnvelopeComponent::syncFromModel() {
    attackS .setValue(env.attackMs,  juce::dontSendNotification);
    holdS   .setValue(env.holdMs,    juce::dontSendNotification);
    decayS  .setValue(env.decayMs,   juce::dontSendNotification);
    sustainS.setValue(env.sustain,   juce::dontSendNotification);
    releaseS.setValue(env.releaseMs, juce::dontSendNotification);
    velS    .setValue(env.velocitySensitivity, juce::dontSendNotification);
    repaint();
}

void AHDSREnvelopeComponent::commitChange() {
    if (onChanged) onChanged();
    repaint();
}

// ==============================================================================
// Preset list
// ==============================================================================
void AHDSREnvelopeComponent::rebuildPresetCombo() {
    presetCombo.clear(juce::dontSendNotification);
    presetCombo.addItem("(choose preset...)", 1000);
    presetCombo.setSelectedId(1000, juce::dontSendNotification);
    auto presets = EnvelopePresetManager::instance().getAll();
    // Built-ins are listed first, then a divider, then user presets.
    // We have to walk the list twice because preset insertion order in
    // the library isn't guaranteed to be built-ins-then-users any more
    // (renames, demotions and restores can shuffle the order). The id
    // (1-based) still maps to the source index in `presets` so
    // onPresetChosen can look up the right entry.
    int id = 1;
    bool anyBuiltIn = false;
    for (auto& p : presets) {
        if (!p.builtIn) { ++id; continue; }
        anyBuiltIn = true;
        // The leading ★ glyph is the visible "factory preset" badge.
        // Using a real unicode star instead of "*" so it's distinct
        // from anything a user is plausibly going to type in a preset
        // name. PopupMenu / ComboBox don't expose per-item colour so
        // the glyph is what carries the distinction in the dropdown
        // (the manage dialog adds colour on top).
        juce::String label = juce::String::fromUTF8(u8"\u2605 ") + juce::String(p.name);
        presetCombo.addItem(label, id++);
    }
    if (anyBuiltIn) presetCombo.addSeparator();
    id = 1;
    for (auto& p : presets) {
        if (p.builtIn) { ++id; continue; }
        presetCombo.addItem(juce::String(p.name), id++);
    }
}

void AHDSREnvelopeComponent::onPresetChosen(int id) {
    if (id <= 0 || id == 1000) return;
    auto presets = EnvelopePresetManager::instance().getAll();
    int idx = id - 1;
    if (idx < 0 || idx >= (int)presets.size()) return;
    env = presets[idx].envelope;
    syncFromModel();
    commitChange();
    // Leave the dropdown showing the chosen preset name (don't reset to
    // the placeholder) so the user can see which one they recalled.
}

// ==============================================================================
// Save as / Manage dialogs
// ==============================================================================
void AHDSREnvelopeComponent::openSaveAsDialog() {
    auto* aw = new juce::AlertWindow("Save Envelope Preset",
                                     "Name for this envelope:",
                                     juce::MessageBoxIconType::NoIcon, this);
    aw->addTextEditor("name", "My Envelope");
    aw->addButton("Save",   1, juce::KeyPress(juce::KeyPress::returnKey));
    aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    aw->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [this, aw](int r) {
                if (r == 1) {
                    auto name = aw->getTextEditorContents("name").toStdString();
                    if (!name.empty()) {
                        EnvelopePresetManager::instance().addOrReplace(name, env);
                        EnvelopePresetManager::instance().save();
                    }
                }
                delete aw;
            }), false);
}

// Tiny modal list view for renaming / deleting user presets.
namespace {
class PresetManageDialog : public juce::Component,
                          private juce::ListBoxModel {
public:
    PresetManageDialog() {
        list.setModel(this);
        addAndMakeVisible(list);
        addAndMakeVisible(duplicateBtn);
        addAndMakeVisible(renameBtn);
        addAndMakeVisible(deleteBtn);
        addAndMakeVisible(restoreBtn);
        addAndMakeVisible(closeBtn);
        addAndMakeVisible(infoLabel);
        infoLabel.setText(juce::String::fromUTF8(
            u8"Factory presets are marked \u2605 in gold. Editing or renaming "
            u8"one removes the marker but keeps the entry. Restore Built-ins "
            u8"re-adds only factory presets that have been deleted outright; "
            u8"it never overwrites anything still in the library."),
                          juce::dontSendNotification);
        infoLabel.setJustificationType(juce::Justification::centredLeft);
        infoLabel.setFont(juce::Font(11.0f, juce::Font::italic));

        duplicateBtn.setTooltip("Make a copy of the selected preset under a new name. The original is left untouched. Useful when you want to tweak a factory preset's values without losing the original - duplicate it first, then edit the duplicate.");
        renameBtn   .setTooltip("Rename the selected preset. Renaming a factory preset removes its built-in marker.");
        deleteBtn   .setTooltip("Delete the selected preset. Built-ins can be brought back via Restore Built-ins.");
        restoreBtn  .setTooltip("Re-add any factory presets that have been deleted outright. Existing entries (including ones you've edited or renamed) are never overwritten.");

        duplicateBtn.onClick = [this]() { duplicateSelected(); };
        renameBtn   .onClick = [this]() { renameSelected(); };
        deleteBtn   .onClick = [this]() { deleteSelected(); };
        restoreBtn  .onClick = [this]() { restoreBuiltIns(); };
        closeBtn    .onClick = [this]() {
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState(0);
        };
        refresh();
        setSize(560, 380);
    }

    void resized() override {
        auto r = getLocalBounds().reduced(8);
        // Info label sits at the top; wraps to two lines.
        infoLabel.setBounds(r.removeFromTop(32));
        r.removeFromTop(4);
        auto btns = r.removeFromBottom(28);
        // Close on the far right; the four action buttons left-aligned.
        // Grouping makes the destructive (Delete) / constructive
        // (Duplicate, Restore) split visible at a glance instead of
        // burying Close among the actions.
        closeBtn    .setBounds(btns.removeFromRight(80));
        btns.removeFromRight(8);
        duplicateBtn.setBounds(btns.removeFromLeft(90));
        btns.removeFromLeft(6);
        renameBtn   .setBounds(btns.removeFromLeft(80));
        btns.removeFromLeft(6);
        deleteBtn   .setBounds(btns.removeFromLeft(80));
        btns.removeFromLeft(6);
        restoreBtn  .setBounds(btns.removeFromLeft(140));
        r.removeFromBottom(6);
        list.setBounds(r);
    }

    int getNumRows() override {
        return (int)EnvelopePresetManager::instance().size();
    }

    void paintListBoxItem(int row, juce::Graphics& g, int w, int h,
                          bool sel) override {
        auto presets = EnvelopePresetManager::instance().getAll();
        if (row < 0 || row >= (int)presets.size()) return;
        if (sel) g.fillAll(juce::Colour(80, 100, 140));
        else if (row & 1) g.fillAll(juce::Colour(40, 44, 56));
        else g.fillAll(juce::Colour(46, 50, 62));
        bool isBuiltIn = presets[row].builtIn;
        // Factory presets get a gold ★ + gold-tinted name; user
        // presets get the default white. The gold is bright enough
        // to stand out at a glance even in the alternating-stripe
        // background.
        const juce::Colour goldBadge { 0xff, 0xd1, 0x3b };
        const juce::Colour goldName  { 0xff, 0xe1, 0x88 };
        juce::String name = juce::String(presets[row].name);
        juce::String tail = isBuiltIn ? " (built-in)" : juce::String();
        if (isBuiltIn) {
            // Draw the star glyph separately so we can paint it in a
            // brighter accent colour than the name itself.
            g.setColour(goldBadge);
            g.setFont(juce::Font(13.0f, juce::Font::bold));
            g.drawText(juce::String::fromUTF8(u8"\u2605"),
                       6, 0, 18, h, juce::Justification::centredLeft);
        }
        g.setColour(isBuiltIn ? goldName : juce::Colours::white);
        g.setFont(juce::Font(13.0f, juce::Font::plain));
        int textX = isBuiltIn ? 26 : 8;
        g.drawText(name + tail, textX, 0, w - textX - 8, h,
                   juce::Justification::centredLeft);
    }

    void refresh() { list.updateContent(); list.repaint(); }

private:
    juce::ListBox    list;
    juce::TextButton duplicateBtn { "Duplicate" };
    juce::TextButton renameBtn    { "Rename" };
    juce::TextButton deleteBtn    { "Delete" };
    juce::TextButton restoreBtn   { "Restore Built-ins" };
    juce::TextButton closeBtn     { "Close" };
    juce::Label      infoLabel;

    int  selectedRow() const { return list.getSelectedRow(); }

    void duplicateSelected() {
        int row = selectedRow(); if (row < 0) return;
        auto presets = EnvelopePresetManager::instance().getAll();
        if (row >= (int)presets.size()) return;
        const auto& src = presets[row];
        // Auto-generated default name: "<source> copy", disambiguated
        // against the current library so the user can just hit Return
        // without thinking about collisions. They can still edit the
        // name in the text field before committing.
        std::string defaultName = src.name + " copy";
        int n = 2;
        auto isTaken = [&presets](const std::string& candidate) {
            for (auto& p : presets) if (p.name == candidate) return true;
            return false;
        };
        while (isTaken(defaultName))
            defaultName = src.name + " copy " + std::to_string(n++);

        auto* aw = new juce::AlertWindow("Duplicate Preset",
                                         "Name for the new preset:",
                                         juce::MessageBoxIconType::NoIcon, this);
        aw->addTextEditor("name", defaultName);
        aw->addButton("Duplicate", 1, juce::KeyPress(juce::KeyPress::returnKey));
        aw->addButton("Cancel",    0, juce::KeyPress(juce::KeyPress::escapeKey));
        // Capture the source envelope by value: the underlying preset
        // list could shift before the modal returns (e.g. another
        // component triggered a refresh), so resolving by row index in
        // the callback would be unsafe.
        AHDSREnvelope srcEnv = src.envelope;
        aw->enterModalState(true,
            juce::ModalCallbackFunction::create(
                [this, aw, srcEnv](int r) {
                    if (r == 1) {
                        auto name = aw->getTextEditorContents("name").toStdString();
                        if (!name.empty()) {
                            // addNew never overwrites: a typed-in name
                            // that happens to collide with another
                            // entry will be auto-suffixed " (2)" etc.
                            // The duplicate is always a user preset
                            // (builtIn=false, empty origin id), so it
                            // doesn't claim any factory lineage.
                            EnvelopePresetManager::instance().addNew(name, srcEnv);
                            EnvelopePresetManager::instance().save();
                            refresh();
                        }
                    }
                    delete aw;
                }), false);
    }

    void renameSelected() {
        int row = selectedRow(); if (row < 0) return;
        auto presets = EnvelopePresetManager::instance().getAll();
        if (row >= (int)presets.size()) return;
        std::string oldName = presets[row].name;
        auto* aw = new juce::AlertWindow("Rename Preset",
                                         "New name:",
                                         juce::MessageBoxIconType::NoIcon, this);
        aw->addTextEditor("name", oldName);
        aw->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
        aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        aw->enterModalState(true,
            juce::ModalCallbackFunction::create(
                [this, aw, oldName](int r) {
                    if (r == 1) {
                        auto n = aw->getTextEditorContents("name").toStdString();
                        if (!n.empty()) {
                            EnvelopePresetManager::instance().rename(oldName, n);
                            EnvelopePresetManager::instance().save();
                            refresh();
                        }
                    }
                    delete aw;
                }), false);
    }

    void deleteSelected() {
        int row = selectedRow(); if (row < 0) return;
        auto presets = EnvelopePresetManager::instance().getAll();
        if (row >= (int)presets.size()) return;
        std::string name = presets[row].name;
        bool wasBuiltIn = presets[row].builtIn;
        juce::String msg = "Delete preset \"" + juce::String(name) + "\"?";
        if (wasBuiltIn)
            msg += "\n\nThis is a factory preset. You can bring it back later "
                   "with Restore Built-ins.";
        juce::NativeMessageBox::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::QuestionIcon)
                .withTitle("Delete Preset")
                .withMessage(msg)
                .withButton("Delete")
                .withButton("Cancel")
                .withAssociatedComponent(this),
            [this, name](int r) {
                if (r == 1) {
                    EnvelopePresetManager::instance().remove(name);
                    EnvelopePresetManager::instance().save();
                    refresh();
                }
            });
    }

    void restoreBuiltIns() {
        // Mirror the manager's add-only logic to count what would
        // actually happen, so we can give an honest preview before the
        // user commits. We never replace existing entries:
        //   - origin id already represented -> skip (factory entry is
        //     still in the library, even if renamed or edited);
        //   - origin missing AND name free -> would restore;
        //   - origin missing BUT name taken by an unrelated user
        //     preset -> would skip (don't clobber).
        auto& mgr = EnvelopePresetManager::instance();
        auto defaults = EnvelopePresetManager::getBuiltInDefaults();
        auto current  = mgr.getAll();
        int wouldRestore = 0;
        int blockedByName = 0;
        for (auto& def : defaults) {
            bool represented = false;
            for (auto& p : current)
                if (p.builtInOriginId == def.name) { represented = true; break; }
            if (represented) continue;
            bool nameTaken = false;
            for (auto& p : current)
                if (p.name == def.name) { nameTaken = true; break; }
            if (nameTaken) { ++blockedByName; continue; }
            ++wouldRestore;
        }
        if (wouldRestore == 0 && blockedByName == 0) {
            juce::NativeMessageBox::showAsync(
                juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::InfoIcon)
                    .withTitle("Restore Built-ins")
                    .withMessage("All factory presets are still represented "
                                 "in the library (under their original names "
                                 "or as renamed / edited entries). Nothing "
                                 "to restore.\n\nTip: to bring back the "
                                 "original version of a factory preset you "
                                 "have edited, delete the edited entry first "
                                 "and then click Restore Built-ins again.")
                    .withButton("OK")
                    .withAssociatedComponent(this),
                nullptr);
            return;
        }
        juce::String msg;
        if (wouldRestore > 0) {
            msg << wouldRestore
                << (wouldRestore == 1 ? " factory preset" : " factory presets")
                << " will be added back to the library. ";
        }
        if (blockedByName > 0) {
            msg << blockedByName
                << (blockedByName == 1 ? " factory preset" : " factory presets")
                << " can't be restored because the name is currently used by "
                   "an unrelated entry; rename or delete that entry to free "
                   "the name. ";
        }
        msg << "\n\nNothing currently in the library will be modified or "
               "replaced.\n\nContinue?";
        juce::NativeMessageBox::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::QuestionIcon)
                .withTitle("Restore Built-ins")
                .withMessage(msg)
                .withButton(wouldRestore > 0 ? "Restore" : "OK")
                .withButton("Cancel")
                .withAssociatedComponent(this),
            [this](int r) {
                if (r == 1) {
                    EnvelopePresetManager::instance().restoreBuiltIns();
                    EnvelopePresetManager::instance().save();
                    refresh();
                }
            });
    }
};
} // namespace

void AHDSREnvelopeComponent::openManageDialog() {
    auto* content = new PresetManageDialog();
    juce::DialogWindow::LaunchOptions opt;
    opt.dialogTitle = "Manage Envelope Presets";
    opt.content.setOwned(content);
    opt.escapeKeyTriggersCloseButton = true;
    opt.useNativeTitleBar = true;
    opt.resizable = false;
    opt.componentToCentreAround = this;
    opt.launchAsync();
}

// ==============================================================================
// Curve editor dialog (per-segment)
// ==============================================================================
namespace {
class CurveEditDialog : public juce::Component {
public:
    CurveEditDialog(SpectralCurve& curve, const juce::String& title,
                    juce::Colour col, std::function<void()> onChanged)
        : panel(curve, title, 0.0f, 1.0f, col, onChanged)
    {
        addAndMakeVisible(panel);
        addAndMakeVisible(closeBtn);
        addAndMakeVisible(linearBtn);
        addAndMakeVisible(expBtn);
        addAndMakeVisible(invExpBtn);
        addAndMakeVisible(sCurveBtn);

        // Quick-set shapes - one-click access to the most common ramps so
        // users don't have to type "x^0.5" themselves.
        linearBtn.setTooltip("Set to a linear ramp.");
        expBtn   .setTooltip("Set to a fast-then-slow ramp (\"exponential\").");
        invExpBtn.setTooltip("Set to a slow-then-fast ramp (\"inverse exponential\").");
        sCurveBtn.setTooltip("Set to an S-shaped ramp.");

        auto setShape = [this, &curve, onChanged](const juce::String& expr) {
            curve.mode = SpectralCurve::Equation;
            curve.expression = expr.toStdString();
            curve.freehandMode = false;
            panel.syncFromModel();
            if (onChanged) onChanged();
        };
        // The dialog is reused for attack (rising 0->1) and decay/release
        // (falling 1->0), so we expose both directions here and rely on
        // the host to label them. To keep this simple, we provide
        // "rising" shapes; for a falling segment the user can flip in
        // the editor or just edit the expression.
        linearBtn.onClick = [setShape]() { setShape("x"); };
        expBtn   .onClick = [setShape]() { setShape("x^2"); };
        invExpBtn.onClick = [setShape]() { setShape("x^0.5"); };
        sCurveBtn.onClick = [setShape]() { setShape("0.5 - 0.5*cos(3.14159*x)"); };
        closeBtn .onClick = [this]() {
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState(0);
        };
        setSize(520, 360);
    }

    void resized() override {
        auto r = getLocalBounds().reduced(8);
        auto btns = r.removeFromBottom(28);
        closeBtn  .setBounds(btns.removeFromRight(80));
        btns.removeFromRight(8);
        sCurveBtn .setBounds(btns.removeFromRight(70));
        invExpBtn .setBounds(btns.removeFromRight(90));
        expBtn    .setBounds(btns.removeFromRight(70));
        linearBtn .setBounds(btns.removeFromRight(70));
        r.removeFromBottom(6);
        panel.setBounds(r);
    }

private:
    SpectralCurvePanel panel;
    juce::TextButton closeBtn   { "Close" };
    juce::TextButton linearBtn  { "Linear" };
    juce::TextButton expBtn     { "Fast->slow" };
    juce::TextButton invExpBtn  { "Slow->fast" };
    juce::TextButton sCurveBtn  { "S-curve" };
};
} // namespace

void AHDSREnvelopeComponent::openCurveEditor(const juce::String& title,
                                             SpectralCurve& curve,
                                             juce::Colour colour) {
    auto* content = new CurveEditDialog(curve, title, colour,
                                         [this]() { commitChange(); });
    juce::DialogWindow::LaunchOptions opt;
    opt.dialogTitle = title;
    opt.content.setOwned(content);
    opt.escapeKeyTriggersCloseButton = true;
    opt.useNativeTitleBar = true;
    opt.resizable = true;
    opt.componentToCentreAround = this;
    opt.launchAsync();
}

// ==============================================================================
// Layout + paint
// ==============================================================================
void AHDSREnvelopeComponent::resized() {
    auto r = getLocalBounds().reduced(6);

    // Top row.
    auto top = r.removeFromTop(26);
    presetLabel.setBounds(top.removeFromLeft(60));
    presetCombo.setBounds(top.removeFromLeft(220));
    top.removeFromLeft(8);
    saveAsBtn .setBounds(top.removeFromLeft(90));
    top.removeFromLeft(4);
    manageBtn .setBounds(top.removeFromLeft(90));
    r.removeFromTop(6);

    // Slider row (~120-160px tall depending on container).
    int sliderH = juce::jmax(90, r.getHeight() / 2);
    auto sliders = r.removeFromTop(sliderH);
    int n = 6;
    int w = sliders.getWidth() / n;
    auto laySlider = [&](juce::Slider& s, juce::Label& l, int i) {
        auto col = sliders.withX(sliders.getX() + i * w).withWidth(w);
        auto lb  = col.removeFromTop(16);
        l.setBounds(lb);
        s.setBounds(col.reduced(2, 0));
    };
    laySlider(attackS,  attackLbl,  0);
    laySlider(holdS,    holdLbl,    1);
    laySlider(decayS,   decayLbl,   2);
    laySlider(sustainS, sustainLbl, 3);
    laySlider(releaseS, releaseLbl, 4);
    laySlider(velS,     velLbl,     5);

    r.removeFromTop(4);

    // Preview + edit buttons.
    auto btnRow = r.removeFromBottom(26);
    int bw = btnRow.getWidth() / 3;
    editAttackBtn .setBounds(btnRow.withX(btnRow.getX() + 0 * bw).withWidth(bw - 4));
    editDecayBtn  .setBounds(btnRow.withX(btnRow.getX() + 1 * bw).withWidth(bw - 4));
    editReleaseBtn.setBounds(btnRow.withX(btnRow.getX() + 2 * bw).withWidth(bw - 4));
    r.removeFromBottom(4);
    previewBounds = r;
}

void AHDSREnvelopeComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(28, 32, 40));
    g.setColour(juce::Colour(60, 65, 80));
    g.drawRect(getLocalBounds(), 1);

    paintPreview(g, previewBounds);
}

void AHDSREnvelopeComponent::paintPreview(juce::Graphics& g,
                                          juce::Rectangle<int> r) {
    if (r.isEmpty()) return;
    g.setColour(juce::Colour(18, 20, 28));
    g.fillRect(r);
    g.setColour(juce::Colour(45, 50, 60));
    g.drawRect(r, 1);

    // Bake the envelope to a polyline. We allocate samples to stages
    // proportionally to their durations, capped so a tiny / huge value
    // doesn't drown the preview.
    auto A = env.attackCurve.evaluate(64);
    auto D = env.decayCurve.evaluate(64);
    auto R = env.releaseCurve.evaluate(64);

    float total = env.attackMs + env.holdMs + env.decayMs
                + std::max(50.0f, env.releaseMs); // pretend a tail
    if (total <= 0.0f) total = 1.0f;
    // Add a "sustain hold" segment for context so the user sees the
    // shape stay flat at the sustain level.
    float sustainShare = 0.18f;     // 18% of preview width
    float remaining = 1.0f - sustainShare;
    float aFrac = remaining * (env.attackMs  / total);
    float hFrac = remaining * (env.holdMs    / total);
    float dFrac = remaining * (env.decayMs   / total);
    float rFrac = remaining * (std::max(50.0f, env.releaseMs) / total);

    int W = r.getWidth();
    int H = r.getHeight();
    auto yAt = [&](float lvl) {
        return r.getY() + H - (int)(juce::jlimit(0.0f, 1.0f, lvl) * (H - 2)) - 1;
    };
    int x = r.getX();
    int xA = x + (int)(aFrac * W);
    int xH = xA + (int)(hFrac * W);
    int xD = xH + (int)(dFrac * W);
    int xS = xD + (int)(sustainShare * W);
    int xR = xS + (int)(rFrac * W);
    xR = std::min(xR, r.getRight());

    juce::Path path;
    path.startNewSubPath((float)x, (float)yAt(0));
    for (int i = 0; i < (int)A.size(); ++i) {
        float t = (float)i / (float)(A.size() - 1);
        path.lineTo((float)(x + t * (xA - x)), (float)yAt(A[i]));
    }
    path.lineTo((float)xA, (float)yAt(1));
    path.lineTo((float)xH, (float)yAt(1));
    for (int i = 0; i < (int)D.size(); ++i) {
        float t = (float)i / (float)(D.size() - 1);
        float shape = D[i]; // goes 1 -> 0
        float lvl = env.sustain + (1.0f - env.sustain) * shape;
        path.lineTo((float)(xH + t * (xD - xH)), (float)yAt(lvl));
    }
    path.lineTo((float)xS, (float)yAt(env.sustain));
    for (int i = 0; i < (int)R.size(); ++i) {
        float t = (float)i / (float)(R.size() - 1);
        float shape = R[i];
        float lvl = env.sustain * shape;
        path.lineTo((float)(xS + t * (xR - xS)), (float)yAt(lvl));
    }
    path.lineTo((float)xR, (float)yAt(0));

    g.setColour(juce::Colour(120, 200, 255));
    g.strokePath(path, juce::PathStrokeType(1.6f));

    // Stage dividers.
    g.setColour(juce::Colour(70, 80, 100));
    for (int xv : { xA, xH, xD, xS })
        g.drawVerticalLine(xv, (float)r.getY(), (float)r.getBottom());

    // Stage labels along the top.
    g.setFont(10.0f);
    g.setColour(juce::Colour(140, 150, 170));
    auto drawTag = [&](int x1, int x2, const char* s) {
        if (x2 - x1 < 18) return;
        g.drawText(s, juce::Rectangle<int>(x1, r.getY() + 1, x2 - x1, 12),
                   juce::Justification::centred, false);
    };
    drawTag(r.getX(), xA, "A");
    drawTag(xA, xH, "H");
    drawTag(xH, xD, "D");
    drawTag(xD, xS, "S");
    drawTag(xS, xR, "R");
}

} // namespace SoundShop

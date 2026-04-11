#include "piano_roll_component.h"
#include "music_theory.h"
#include "undo.h"
#include <cmath>
#include <set>

namespace SoundShop {

// Static clipboards shared across all piano roll instances
std::vector<PianoRollComponent::ClipboardNote> PianoRollComponent::clipboard;
std::unique_ptr<Clip> PianoRollComponent::clipClipboard;

PianoRollComponent::PianoRollComponent(NodeGraph& g, Node& n, Transport* t)
    : graph(g), nodeId(n.id), node(&n), transport(t) {
    setWantsKeyboardFocus(true);

    bool isMidi = node->type == NodeType::MidiTimeline;
    {
        juce::String title = node->name + (isMidi ? " [MIDI]" : " [Audio]");
        if (node->parentGroupId >= 0) {
            auto* parent = graph.findNode(node->parentGroupId);
            if (parent)
                title += " in " + juce::String(parent->name);
        }
        titleLabel.setText(title, juce::dontSendNotification);
    }
    titleLabel.setColour(juce::Label::textColourId, isMidi ? juce::Colours::limegreen : juce::Colours::cornflowerblue);
    titleLabel.setFont(juce::Font(13.0f, juce::Font::bold));
    addAndMakeVisible(titleLabel);

    helpLabel.setText("Click=place  Drag=select  Edges=resize  Alt=no snap  Scroll=pitch  Shift+scroll=pan  Ctrl+scroll=zoom  Right-click=menu",
                      juce::dontSendNotification);
    helpLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
    helpLabel.setFont(juce::Font(11.0f));
    addAndMakeVisible(helpLabel);

    auto apply = [this](auto fn) {
        for (auto& [ci, ni] : state.selected)
            if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size())
                fn(node->clips[ci].notes[ni]);
        repaint();
    };

    // All toolbar buttons
    auto addBtn = [this](juce::TextButton& b) { addAndMakeVisible(b); };
    addBtn(compactBtn); addBtn(closeBtn);
    addBtn(transpDownOctBtn); addBtn(transpDownSemiBtn); addBtn(transpUpSemiBtn); addBtn(transpUpOctBtn);
    addBtn(timeLeftBtn); addBtn(timeRightBtn);
    addBtn(selectAllBtn); addBtn(deselectBtn);
    addBtn(dblDurBtn); addBtn(halfDurBtn); addBtn(reverseBtn);
    addBtn(detuneResetBtn);
    addBtn(snap14Btn); addBtn(snap12Btn); addBtn(snap1Btn); addBtn(snapOffBtn);
    addBtn(snapScaleBtn); addBtn(detectKeyBtn);

    // Tooltips: cover the controls whose labels are abbreviated or use
    // music terminology a non-musician wouldn't necessarily know.
    // Skip the genuinely self-explanatory ones (Select All, Deselect,
    // Reverse, X close).
    compactBtn.setTooltip("Toggle compact mode — hides most toolbar buttons to maximize the note-editing area");
    transpUpOctBtn.setTooltip("Move every selected note up by one octave (12 semitones)");
    transpDownOctBtn.setTooltip("Move every selected note down by one octave (12 semitones)");
    transpUpSemiBtn.setTooltip("Move every selected note up by one semitone (one piano key)");
    transpDownSemiBtn.setTooltip("Move every selected note down by one semitone (one piano key)");
    timeLeftBtn.setTooltip("Nudge selected notes earlier in time by one snap unit");
    timeRightBtn.setTooltip("Nudge selected notes later in time by one snap unit");
    dblDurBtn.setTooltip("Double the length of every selected note (makes them last twice as long)");
    halfDurBtn.setTooltip("Halve the length of every selected note (makes them last half as long)");
    reverseBtn.setTooltip("Reverse the order of selected notes in time, so the last becomes the first");
    detuneResetBtn.setTooltip("Reset the detune of selected notes back to 0 cents (perfectly in tune)");
    snap14Btn.setTooltip("Snap notes to quarter-beat positions (1/16th of a 4/4 bar)");
    snap12Btn.setTooltip("Snap notes to half-beat positions (1/8th of a 4/4 bar)");
    snap1Btn.setTooltip("Snap notes to whole-beat positions (1/4 of a 4/4 bar)");
    snapOffBtn.setTooltip("Disable snapping — notes can be placed at any position. Hold Alt while dragging for the same effect.");
    snapScaleBtn.setTooltip("Snap notes to the chosen Key/Scale, so dragging a note up or down only lands on \"in key\" pitches");
    detectKeyBtn.setTooltip("Analyze the notes in this clip and guess the key/scale, then set the dropdowns to match");

    // Mute / Solo / Pan
    addBtn(muteBtn); addBtn(soloBtn);
    // "Mute" is universally understood — skip the tooltip. "Solo" is a
    // DAW term that non-musicians might not know.
    soloBtn.setTooltip("Solo this track — when any track is soloed, all non-soloed tracks are silenced");
    // Pan slider only for nodes that produce audio. MIDI Timelines only
    // output MIDI events — panning them does nothing.
    bool showPan = (n.type != NodeType::MidiTimeline);
    if (showPan) {
        addAndMakeVisible(panSlider);
        addAndMakeVisible(panLbl);
    }
    panLbl.setText("Pan:", juce::dontSendNotification);
    panSlider.setTooltip("Pan this track left or right in the stereo image. Center = both speakers, "
                         "left = only left speaker, right = only right speaker.");
    panSlider.setRange(-1.0, 1.0, 0.01);
    // Read initial pan from the named param (if it exists) or node->pan
    {
        float initPan = node->pan;
        for (auto& p : node->params)
            if (p.name == "Pan") { initPan = p.value; break; }
        panSlider.setValue(initPan);
    }
    panSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    panSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    panSlider.onValueChange = [this]() {
        float val = (float)panSlider.getValue();
        node->pan = val; // legacy field
        // Update the named param
        for (auto& p : node->params)
            if (p.name == "Pan") { p.value = val; break; }
        graph.dirty = true;
    };
    muteBtn.onClick = [this]() {
        node->muted = !node->muted;
        muteBtn.setColour(juce::TextButton::buttonColourId,
            node->muted ? juce::Colour(180, 50, 50) : juce::Colour(55, 55, 60));
        graph.dirty = true;
    };
    soloBtn.onClick = [this]() {
        node->soloed = !node->soloed;
        soloBtn.setColour(juce::TextButton::buttonColourId,
            node->soloed ? juce::Colour(180, 180, 50) : juce::Colour(55, 55, 60));
        graph.dirty = true;
    };
    // Set initial colors
    muteBtn.setColour(juce::TextButton::buttonColourId,
        node->muted ? juce::Colour(180, 50, 50) : juce::Colour(55, 55, 60));
    soloBtn.setColour(juce::TextButton::buttonColourId,
        node->soloed ? juce::Colour(180, 180, 50) : juce::Colour(55, 55, 60));

    // Expression / velocity lane buttons.
    // Velocity lane is now fully working; MPE lanes are still hidden until
    // their interaction code is audited.
    addBtn(exprOffBtn);
    addBtn(exprVelBtn);
    exprOffBtn.setTooltip("Hide the expression/automation lane below the piano roll");
    exprVelBtn.setTooltip("Show the velocity lane — drag the bars to change how loud each note plays");
    exprOffBtn.onClick   = [this]() { exprLane = ExprNone; repaint(); };
    exprVelBtn.onClick   = [this]() { exprLane = ExprVelocity; repaint(); };
    exprPBBtn.onClick    = [this]() { exprLane = ExprPitchBend; repaint(); };
    exprSlideBtn.onClick = [this]() { exprLane = ExprSlide; repaint(); };
    exprPressBtn.onClick = [this]() { exprLane = ExprPressure; repaint(); };

    // Automation lane parameter selector
    addAndMakeVisible(autoParamCombo);
    autoParamCombo.setTooltip("Pick a parameter to automate in the lane below the piano roll. "
                              "Once shown, click in the lane to add points and drag them to draw a curve "
                              "that controls the parameter over time.");
    autoParamCombo.addItem("Automate Param", 1);
    for (int i = 0; i < (int)node->params.size(); ++i)
        autoParamCombo.addItem(node->params[i].name, i + 2);
    autoParamCombo.setSelectedItemIndex(0);
    autoParamCombo.onChange = [this]() {
        int idx = autoParamCombo.getSelectedItemIndex();
        if (idx <= 0) {
            exprLane = ExprNone;
            autoParamIndex = -1;
        } else {
            exprLane = ExprAutomation;
            autoParamIndex = idx - 1;
        }
        repaint();
    };

    // Root/Key/Mode/Scale combo boxes
    auto addCombo = [this](juce::ComboBox& cb, juce::Label& lbl, const char* text) {
        lbl.setText(text, juce::dontSendNotification);
        lbl.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        lbl.setFont(juce::Font(11.0f));
        addAndMakeVisible(lbl);
        addAndMakeVisible(cb);
    };
    addCombo(rootCombo, rootLbl, "Root:");
    addCombo(keyCombo, keyLbl, "Key:");
    addCombo(modeCombo, modeLbl, "Mode:");
    addCombo(scaleCombo, scaleLbl, "Scale:");
    rootCombo.setTooltip("Root note — the home pitch of the key/scale (e.g. C for C Major)");
    keyCombo.setTooltip("Key family — Major sounds happy/bright, Minor sounds sad/dark");
    modeCombo.setTooltip("Mode — variants of the major scale that change the mood (Dorian, Phrygian, Lydian, etc.)");
    scaleCombo.setTooltip("Scale — broader categories like Pentatonic, Blues, Whole-Tone, Chromatic. "
                          "Affects which notes are highlighted as 'in key' on the piano roll.");

    // Populate root
    for (int i = 0; i < 12; ++i)
        rootCombo.addItem(MusicTheory::NOTE_NAMES[i], i + 1);
    rootCombo.setSelectedId(state.keyRoot + 1, juce::dontSendNotification);
    rootCombo.onChange = [this]() { state.keyRoot = rootCombo.getSelectedId() - 1; repaint(); };

    // Helper: find combo index by name
    auto findComboIndex = [](juce::ComboBox& cb, const std::string& name) -> int {
        for (int i = 0; i < cb.getNumItems(); ++i)
            if (cb.getItemText(i).toStdString() == name) return i;
        return 0;
    };

    // Populate key
    { int id = 1; for (auto& [name, _] : MusicTheory::keys()) keyCombo.addItem(name, id++); }
    keyCombo.setSelectedItemIndex(findComboIndex(keyCombo, "Major"), juce::dontSendNotification);
    keyCombo.onChange = [this]() {
        state.activeCategory = "key";
        state.keyName = keyCombo.getText().toStdString();
        repaint();
    };

    // Populate mode
    { int id = 1; for (auto& [name, _] : MusicTheory::modes()) modeCombo.addItem(name, id++); }
    modeCombo.setSelectedItemIndex(findComboIndex(modeCombo, "Ionian"), juce::dontSendNotification);
    modeCombo.onChange = [this]() {
        state.activeCategory = "mode";
        state.modeName = modeCombo.getText().toStdString();
        repaint();
    };

    // Populate scale
    { int id = 1; for (auto& [name, _] : MusicTheory::scales()) scaleCombo.addItem(name, id++); }
    scaleCombo.setSelectedItemIndex(findComboIndex(scaleCombo, "Chromatic"), juce::dontSendNotification);
    scaleCombo.onChange = [this]() {
        state.activeCategory = "scale";
        state.scaleName = scaleCombo.getText().toStdString();
        repaint();
    };

    // Set consistent small font on all combos
    rootCombo.setLookAndFeel(&smallComboLF);
    keyCombo.setLookAndFeel(&smallComboLF);
    modeCombo.setLookAndFeel(&smallComboLF);
    scaleCombo.setLookAndFeel(&smallComboLF);
    rootCombo.setJustificationType(juce::Justification::centredLeft);
    keyCombo.setJustificationType(juce::Justification::centredLeft);
    modeCombo.setJustificationType(juce::Justification::centredLeft);
    scaleCombo.setJustificationType(juce::Justification::centredLeft);

    // Set detune label to match other labels
    detuneLbl.setFont(juce::Font(11.0f));

    // Scrollbars
    addAndMakeVisible(hScrollBar);
    addAndMakeVisible(vScrollBar);
    addAndMakeVisible(hZoomSlider);

    hScrollBar.setRangeLimits(0, 1);
    hScrollBar.setAutoHide(false);
    hScrollBar.addListener(this);

    vScrollBar.setRangeLimits(0, 127);
    vScrollBar.setAutoHide(false);
    vScrollBar.addListener(this);

    hZoomSlider.setRange(0.2, 10.0, 0.1);
    hZoomSlider.setValue(state.hZoom, juce::dontSendNotification);
    hZoomSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    hZoomSlider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    hZoomSlider.setTooltip("Horizontal zoom");
    hZoomSlider.onValueChange = [this]() {
        state.hZoom = (float)hZoomSlider.getValue();
        updateScrollBars();
        repaint();
    };

    detuneLbl.setText("Detune:", juce::dontSendNotification);
    detuneLbl.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible(detuneLbl);
    detuneSlider.setRange(-100, 100, 1);
    detuneSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 50, 20);
    detuneSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    detuneSlider.setTooltip("Fine-tune the pitch of selected notes by cents (-100 to +100). "
                            "100 cents = 1 semitone. Useful for slightly out-of-tune effects "
                            "or matching another instrument's tuning.");
    addAndMakeVisible(detuneSlider);

    compactBtn.onClick = [this]() {
        compactMode = !compactMode;
        compactBtn.setButtonText(compactMode ? "++" : "--");
        resized(); repaint();
    };
    closeBtn.onClick = [this]() { if (onClose) onClose(node->id); };

    transpDownOctBtn.onClick = [this, apply]() { apply([](MidiNote& n) { n.pitch = std::max(0, n.pitch - 12); }); };
    transpDownSemiBtn.onClick = [this, apply]() { apply([](MidiNote& n) { n.pitch = std::max(0, n.pitch - 1); }); };
    transpUpSemiBtn.onClick = [this, apply]() { apply([](MidiNote& n) { n.pitch = std::min(127, n.pitch + 1); }); };
    transpUpOctBtn.onClick = [this, apply]() { apply([](MidiNote& n) { n.pitch = std::min(127, n.pitch + 12); }); };

    timeLeftBtn.onClick = [this, apply]() {
        float s = state.snap > 0 ? state.snap : 0.25f;
        apply([s](MidiNote& n) { n.offset = std::max(0.0f, n.offset - s); });
    };
    timeRightBtn.onClick = [this, apply]() {
        float s = state.snap > 0 ? state.snap : 0.25f;
        apply([s](MidiNote& n) { n.offset += s; });
    };

    selectAllBtn.onClick = [this]() {
        state.selected.clear();
        for (int ci = 0; ci < (int)node->clips.size(); ++ci)
            for (int ni = 0; ni < (int)node->clips[ci].notes.size(); ++ni)
                state.selected.insert({ci, ni});
        repaint();
    };
    deselectBtn.onClick = [this]() { state.selected.clear(); repaint(); };

    dblDurBtn.onClick = [this, apply]() { apply([](MidiNote& n) { n.duration *= 2; }); };
    halfDurBtn.onClick = [this, apply]() { apply([](MidiNote& n) { n.duration = std::max(0.125f, n.duration / 2); }); };
    reverseBtn.onClick = [this]() {
        if (state.selected.empty()) return;
        float minOff = 1e9f, maxEnd = 0;
        for (auto& [ci, ni] : state.selected) {
            if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size()) {
                auto& n2 = node->clips[ci].notes[ni];
                float ab = node->clips[ci].startBeat + n2.offset;
                minOff = std::min(minOff, ab); maxEnd = std::max(maxEnd, ab + n2.duration);
            }
        }
        for (auto& [ci, ni] : state.selected) {
            if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size()) {
                auto& n2 = node->clips[ci].notes[ni];
                float ab = node->clips[ci].startBeat + n2.offset;
                n2.offset = std::max(0.0f, maxEnd - (ab - minOff) - n2.duration - node->clips[ci].startBeat);
            }
        }
        repaint();
    };

    detuneSlider.onDragStart = [this]() {
        captureSelectedSnapshot(dragBeforeSnapshot);
    };
    detuneSlider.onValueChange = [this, apply]() {
        float val = (float)detuneSlider.getValue();
        apply([val](MidiNote& n) { n.detune = val; });
    };
    detuneSlider.onDragEnd = [this]() {
        if (!dragBeforeSnapshot.empty()) {
            pushDragUndo("Detune", dragBeforeSnapshot);
            dragBeforeSnapshot.clear();
        }
    };
    detuneResetBtn.onClick = [this, apply]() {
        std::vector<NoteSnapshot> before;
        captureSelectedSnapshot(before);
        apply([](MidiNote& n) { n.detune = 0; });
        pushDragUndo("Reset detune", before);
        detuneSlider.setValue(0);
    };

    auto setSnap = [this](float v) { state.snap = v; repaint(); };
    snap14Btn.onClick = [this, setSnap]() { setSnap(0.25f); };
    snap12Btn.onClick = [this, setSnap]() { setSnap(0.5f); };
    snap1Btn.onClick = [this, setSnap]() { setSnap(1.0f); };
    snapOffBtn.onClick = [this, setSnap]() { setSnap(0.0f); };

    snapScaleBtn.onClick = [this, apply]() {
        auto getIntervals = [&]() -> std::vector<int> {
            const ScaleMap* t = nullptr;
            if (state.activeCategory == "key") t = &MusicTheory::keys();
            else if (state.activeCategory == "mode") t = &MusicTheory::modes();
            else if (state.activeCategory == "scale") t = &MusicTheory::scales();
            if (t) { auto* v = findScale(*t, state.activeName()); if (v) return *v; }
            return {0,2,4,5,7,9,11};
        };
        auto intervals = getIntervals();
        int root = state.keyRoot;
        apply([&](MidiNote& n) { n.pitch = MusicTheory::snapToScale(n.pitch, root, intervals); });
    };
    detectKeyBtn.onClick = [this]() {
        std::vector<int> pitches;
        if (!state.selected.empty()) {
            for (auto& [ci, ni] : state.selected)
                if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size())
                    pitches.push_back(node->clips[ci].notes[ni].pitch);
        } else {
            for (auto& clip : node->clips)
                for (auto& n2 : clip.notes) pitches.push_back(n2.pitch);
        }
        auto results = MusicTheory::detectKeys(pitches);
        juce::PopupMenu rm;
        for (int i = 0; i < std::min((int)results.size(), 20); ++i) {
            auto& m = results[i];
            rm.addItem(i + 1, juce::String(MusicTheory::NOTE_NAMES[m.root]) + " " + m.scaleName
                + " [" + m.category + "] " + juce::String((int)(m.coverage * 100)) + "%");
        }
        rm.showMenuAsync(juce::PopupMenu::Options(), [this, results](int r) {
            if (r >= 1 && r <= (int)results.size()) {
                auto& m = results[r - 1];
                state.keyRoot = m.root;
                state.activeCategory = m.category;
                if (m.category == "key") state.keyName = m.scaleName;
                else if (m.category == "mode") state.modeName = m.scaleName;
                else state.scaleName = m.scaleName;
                repaint();
            }
        });
    };
}

void PianoRollComponent::resized() {
    refreshNode(); if (!node) return;
    auto area = getLocalBounds();
    int th = toolbarHeight();
    int rowH = 26;

    // Row 0: title + M/S/Pan + compact + close
    auto row0 = area.removeFromTop(rowH);
    titleLabel.setBounds(row0.removeFromLeft(150));
    closeBtn.setBounds(row0.removeFromRight(24).reduced(0, 2));
    compactBtn.setBounds(row0.removeFromRight(24).reduced(0, 2));
    muteBtn.setBounds(row0.removeFromRight(42).reduced(1, 2));
    soloBtn.setBounds(row0.removeFromRight(42).reduced(1, 2));
    if (panSlider.isVisible()) {
        panSlider.setBounds(row0.removeFromRight(70).reduced(0, 2));
        panLbl.setBounds(row0.removeFromRight(32).reduced(0, 2));
    }
    helpLabel.setBounds(row0.reduced(4, 0));

    auto allToolbarBtns = {&transpDownOctBtn, &transpDownSemiBtn, &transpUpSemiBtn, &transpUpOctBtn,
        &timeLeftBtn, &timeRightBtn, &selectAllBtn, &deselectBtn,
        &dblDurBtn, &halfDurBtn, &reverseBtn, &detuneResetBtn,
        &snap14Btn, &snap12Btn, &snap1Btn, &snapOffBtn,
        &snapScaleBtn, &detectKeyBtn};

    if (compactMode) {
        for (auto* b : allToolbarBtns) b->setVisible(false);
        detuneLbl.setVisible(false);
        detuneSlider.setVisible(false);
        helpLabel.setVisible(false);
        rootCombo.setVisible(false); rootLbl.setVisible(false);
        keyCombo.setVisible(false); keyLbl.setVisible(false);
        modeCombo.setVisible(false); modeLbl.setVisible(false);
        scaleCombo.setVisible(false); scaleLbl.setVisible(false);
    } else {
        for (auto* b : allToolbarBtns) b->setVisible(true);
        detuneLbl.setVisible(true);
        detuneSlider.setVisible(true);
        helpLabel.setVisible(true);
        rootCombo.setVisible(true); rootLbl.setVisible(true);
        keyCombo.setVisible(true); keyLbl.setVisible(true);
        modeCombo.setVisible(true); modeLbl.setVisible(true);
        scaleCombo.setVisible(true); scaleLbl.setVisible(true);

        // Row 1: transpose + time shift + select
        auto row1 = area.removeFromTop(rowH);
        int x = 4;
        auto place = [&](juce::Component& c, int w) {
            c.setBounds(row1.getX() + x, row1.getY() + 1, w, rowH - 2);
            x += w + 2;
        };
        place(transpDownOctBtn, 65);
        place(transpDownSemiBtn, 72);
        place(transpUpSemiBtn, 72);
        place(transpUpOctBtn, 65);
        x += 6;
        place(timeLeftBtn, 70);
        place(timeRightBtn, 75);
        x += 6;
        place(selectAllBtn, 65);
        place(deselectBtn, 60);
        x += 6;
        place(dblDurBtn, 80);
        place(halfDurBtn, 80);
        place(reverseBtn, 55);
        x += 6;
        detuneLbl.setBounds(row1.getX() + x, row1.getY() + 1, 48, rowH - 2);
        x += 50;
        detuneSlider.setBounds(row1.getX() + x, row1.getY() + 1, 100, rowH - 2);
        x += 102;
        place(detuneResetBtn, 42);

        // Row 2: snap + Root/Key/Mode/Scale + scale operations
        auto row2 = area.removeFromTop(rowH);
        x = 4;
        auto place2 = [&](juce::Component& c, int w) {
            c.setBounds(row2.getX() + x, row2.getY() + 1, w, rowH - 2);
            x += w + 2;
        };
        auto placeLblCombo = [&](juce::Label& lbl, int lw, juce::ComboBox& cb, int cw) {
            lbl.setBounds(row2.getX() + x, row2.getY() + 1, lw, rowH - 2);
            x += lw;
            cb.setBounds(row2.getX() + x, row2.getY() + 1, cw, rowH - 2);
            x += cw + 4;
        };

        // Snap buttons - highlight current
        auto styleSnap = [&](juce::TextButton& btn, float val) {
            btn.setColour(juce::TextButton::buttonColourId,
                std::abs(state.snap - val) < 0.01f ? juce::Colour(50, 90, 140) : juce::Colour(55, 55, 60));
        };
        styleSnap(snap14Btn, 0.25f);
        styleSnap(snap12Btn, 0.5f);
        styleSnap(snap1Btn, 1.0f);
        styleSnap(snapOffBtn, 0.0f);
        place2(snap14Btn, 35);
        place2(snap12Btn, 35);
        place2(snap1Btn, 30);
        place2(snapOffBtn, 35);
        x += 6;

        placeLblCombo(rootLbl, 32, rootCombo, 50);
        placeLblCombo(keyLbl, 28, keyCombo, 130);
        placeLblCombo(modeLbl, 38, modeCombo, 115);
        placeLblCombo(scaleLbl, 38, scaleCombo, 130);
        x += 4;
        place2(snapScaleBtn, 85);
        place2(detectKeyBtn, 70);
        // Velocity lane + automation are always available; MPE lanes only
        // when MPE is enabled on the node.
        x += 8;
        place2(exprOffBtn, 35);
        place2(exprVelBtn, 35);
        // MPE expression lane buttons still hidden until their interaction
        // code is audited.
        exprPBBtn.setVisible(false);
        exprSlideBtn.setVisible(false);
        exprPressBtn.setVisible(false);

        auto styleLane = [&](juce::TextButton& btn, ExprLane lane) {
            btn.setColour(juce::TextButton::buttonColourId,
                exprLane == lane ? juce::Colour(50, 90, 140) : juce::Colour(55, 55, 60));
        };
        styleLane(exprOffBtn, ExprNone);
        styleLane(exprVelBtn, ExprVelocity);
        x += 4;
        autoParamCombo.setBounds(row2.getX() + x, row2.getY() + 1, 120, rowH - 2);
    }

    // Visibility
    if (compactMode) {
        exprOffBtn.setVisible(false); exprVelBtn.setVisible(false);
        exprPBBtn.setVisible(false); exprSlideBtn.setVisible(false); exprPressBtn.setVisible(false);
        autoParamCombo.setVisible(false);
    } else {
        exprOffBtn.setVisible(true); exprVelBtn.setVisible(true);
        exprPBBtn.setVisible(node && node->mpeEnabled);
        exprSlideBtn.setVisible(node && node->mpeEnabled);
        exprPressBtn.setVisible(node && node->mpeEnabled);
        autoParamCombo.setVisible(node && !node->params.empty());
    }

    // Scrollbars — bottom and right edges of the piano roll area
    auto pianoArea = getLocalBounds();
    pianoArea.removeFromTop(toolbarHeight());
    vScrollBar.setBounds(pianoArea.removeFromRight(SCROLLBAR_SIZE));
    auto bottomBar = pianoArea.removeFromBottom(SCROLLBAR_SIZE);
    // Zoom slider gets a generous width (40% of the bar, min 150px) so it's
    // easy to grab. The horizontal scrollbar takes the rest.
    int zoomW = std::max(150, (int)(bottomBar.getWidth() * 0.4f));
    hZoomSlider.setBounds(bottomBar.removeFromRight(zoomW));
    hScrollBar.setBounds(bottomBar);

    updateScrollBars();
}

void PianoRollComponent::paint(juce::Graphics& g) {
    refreshNode(); if (!node) return;
    // Sync pan slider with current param value (tracks automation/signal changes).
    // Only for nodes that produce audio (pan slider hidden for MIDI timelines).
    if (panSlider.isVisible() && node) {
        bool signalLocked = graph.hasSignalInput(node->id);
        panSlider.setEnabled(!signalLocked);
        panSlider.setAlpha(signalLocked ? 0.4f : 1.0f);
        for (auto& p : node->params)
            if (p.name == "Pan") {
                panSlider.setValue(p.value, juce::dontSendNotification);
                break;
            }
    }

    // Draw toolbar background
    g.setColour(juce::Colour(35, 35, 40));
    g.fillRect(0, 0, getWidth(), toolbarHeight());

    auto area = getLocalBounds().toFloat();
    area.removeFromTop(toolbarHeight());
    area.removeFromRight(SCROLLBAR_SIZE);  // vertical scrollbar
    area.removeFromBottom(SCROLLBAR_SIZE); // horizontal scrollbar + zoom
    float gridX = KEY_WIDTH;
    float gridW = area.getWidth() - KEY_WIDTH;
    float gridH = area.getHeight();

    // Translate everything below toolbar
    g.saveState();
    g.setOrigin(0, toolbarHeight());

    int visRange = state.visibleRange;
    int pitchHi = state.scrollPitch + visRange / 2;
    float rowH = gridH / std::max(visRange, 1);
    float totalBeats = graph.getTimelineBeats(*node);
    float absOffset = node->absoluteBeatOffset; // cascading parent offset
    float absTotalBeats = totalBeats + absOffset;

    // Horizontal zoom/scroll — guard against zero visible beats
    float visibleBeats = std::max(1.0f, absTotalBeats / std::max(state.hZoom, 0.1f));
    float scrollBeat = juce::jlimit(0.0f, std::max(0.0f, absTotalBeats - visibleBeats), state.hScroll);
    state.hScroll = scrollBeat;

    auto beatToX = [&](float b) { return gridX + ((b + absOffset - scrollBeat) / visibleBeats) * gridW; };
    auto pitchToY = [&](int p) { return (pitchHi - p) * rowH; };

    // Build scale highlight set
    auto getIntervals = [&]() -> std::vector<int> {
        const ScaleMap* table = nullptr;
        if (state.activeCategory == "key") table = &MusicTheory::keys();
        else if (state.activeCategory == "mode") table = &MusicTheory::modes();
        else if (state.activeCategory == "scale") table = &MusicTheory::scales();
        if (table) {
            auto* v = findScale(*table, state.activeName());
            if (v) return *v;
        }
        return {0,2,4,5,7,9,11};
    };
    auto intervals = getIntervals();
    std::set<int> scaleNotes;
    for (int s : intervals) scaleNotes.insert((s + state.keyRoot) % 12);
    bool isChromatic = intervals.size() >= 12;

    // Background
    g.fillAll(juce::Colour(20, 20, 30));

    // Piano keys + row backgrounds
    for (int i = 0; i <= visRange; ++i) {
        int pitch = pitchHi - i;
        float y = i * rowH;
        if (pitch < 0 || pitch > 127) continue;

        bool isBlack = MusicTheory::isBlackKey(pitch);
        bool inScale = isChromatic || scaleNotes.count(pitch % 12) > 0;

        // Row background
        if (i < visRange) {
            juce::Colour rowCol;
            if (inScale)
                rowCol = isBlack ? juce::Colour(30, 30, 50) : juce::Colour(38, 38, 55);
            else
                rowCol = juce::Colour(18, 18, 22);
            g.setColour(rowCol);
            g.fillRect(gridX, y, gridW, rowH);
        }

        // Horizontal line
        int alpha = (pitch % 12 == 0) ? 60 : (inScale ? 25 : 10);
        g.setColour(juce::Colour(255, 255, 255).withAlpha((uint8_t)alpha));
        g.drawHorizontalLine((int)y, gridX, gridX + gridW);

        // Key label
        if (i < visRange) {
            bool isAuditioned = auditionKeys.count(pitch) > 0;
            juce::Colour keyCol;
            if (isAuditioned)
                keyCol = isBlack ? juce::Colour(80, 100, 180) : juce::Colour(100, 130, 220);
            else if (!inScale && !isChromatic)
                keyCol = juce::Colour(25, 25, 28);
            else
                keyCol = isBlack ? juce::Colour(40, 40, 50) : juce::Colour(60, 60, 70);
            g.setColour(keyCol);
            g.fillRect(0.0f, y, (float)KEY_WIDTH - 2, rowH);

            if (rowH > 7) {
                auto name = MusicTheory::noteName(pitch);
                float fontSize = juce::jlimit(7.0f, 14.0f, rowH * 0.75f);
                g.setFont(juce::Font(fontSize));
                juce::Colour textCol = (!inScale && !isChromatic) ? juce::Colour(60, 60, 60)
                    : (pitch % 12 == 0) ? juce::Colour(180, 180, 180)
                    : juce::Colour(120, 120, 120);
                g.setColour(textCol);
                g.drawText(name, juce::Rectangle<float>(3, y, KEY_WIDTH - 5, rowH),
                           juce::Justification::centredLeft, false);
            }
        }
    }

    // Beat grid with time-signature-aware bar lines
    {
        double bpb = transport ? transport->timeSigMap.beatsPerBar(0) : 4.0;
        int startBeatInt = (int)std::floor(scrollBeat);
        int endBeatInt = (int)std::ceil(scrollBeat + visibleBeats) + 1;
        for (int beat = startBeatInt; beat <= endBeatInt; ++beat) {
            // Grid beats are absolute; beatToX adds absOffset, so subtract it for grid
            float x = gridX + ((float)beat - scrollBeat) / visibleBeats * gridW;
            // Check if this beat is a bar boundary
            bool isBar = (std::abs(std::fmod((double)beat, bpb)) < 0.01 ||
                           std::abs(std::fmod((double)beat, bpb) - bpb) < 0.01);
            g.setColour(juce::Colour(255, 255, 255).withAlpha(isBar ? 0.4f : 0.1f));
            g.drawVerticalLine((int)x, 0, gridH);
            if (isBar) {
                int barNum = (int)((double)beat / bpb) + 1;
                g.setFont(10.0f);
                g.setColour(juce::Colour(200, 200, 200).withAlpha(0.5f));
                g.drawText(juce::String(barNum), x + 2, 2, 30, 12,
                           juce::Justification::centredLeft);
            }
        }
    }

    // Clip boundaries
    for (auto& clip : node->clips) {
        float cx1 = beatToX(clip.startBeat);
        float cx2 = beatToX(clip.startBeat + clip.lengthBeats);
        uint8_t cr = (clip.color >> 16) & 0xFF, cg2 = (clip.color >> 8) & 0xFF, cb = clip.color & 0xFF;
        g.setColour(juce::Colour(cr, cg2, cb).withAlpha(0.06f));
        g.fillRect(cx1, 0.0f, cx2 - cx1, gridH);
        g.setColour(juce::Colour(cr, cg2, cb).withAlpha(0.3f));
        g.drawVerticalLine((int)cx1, 0, gridH);
    }

    // Notes
    for (int ci = 0; ci < (int)node->clips.size(); ++ci) {
        auto& clip = node->clips[ci];
        uint8_t cr = (clip.color >> 16) & 0xFF, cg2 = (clip.color >> 8) & 0xFF, cb = clip.color & 0xFF;
        for (int ni = 0; ni < (int)clip.notes.size(); ++ni) {
            auto& note = clip.notes[ni];
            float absBeat = clip.startBeat + note.offset;
            float nx1 = beatToX(absBeat);
            float nx2 = beatToX(absBeat + note.duration);
            float ny = pitchToY(note.pitch);
            float detuneOff = -(note.detune / 100.0f) * rowH;
            ny += detuneOff;

            if (ny + rowH < 0 || ny > gridH) continue;

            bool isSel = state.selected.count({ci, ni}) > 0;
            bool isChrom = note.chromaticOffset != 0;

            // Note body
            if (isSel) {
                g.setColour(juce::Colours::white.withAlpha(0.9f));
                g.fillRoundedRectangle(nx1 + 1, ny + 1, nx2 - nx1 - 2, rowH - 2, 2);
                g.setColour(juce::Colours::yellow);
                g.drawRoundedRectangle(nx1, ny, nx2 - nx1, rowH, 2, 2);
            } else if (isChrom) {
                g.setColour(juce::Colour(200, 130, 60).withAlpha(0.8f));
                g.fillRoundedRectangle(nx1 + 1, ny + 1, nx2 - nx1 - 2, rowH - 2, 2);
            } else {
                g.setColour(juce::Colour(cr, cg2, cb).withAlpha(0.85f));
                g.fillRoundedRectangle(nx1 + 1, ny + 1, nx2 - nx1 - 2, rowH - 2, 2);
                g.setColour(juce::Colour(cr, cg2, cb).brighter(0.2f));
                g.drawRoundedRectangle(nx1 + 1, ny + 1, nx2 - nx1 - 2, rowH - 2, 2, 1);
            }

            // Note label
            float noteW = nx2 - nx1;
            if (noteW > 14 && rowH > 7) {
                auto name = MusicTheory::noteName(note.pitch);
                float fontSize = juce::jlimit(8.0f, 14.0f, rowH * 0.75f);
                g.setFont(juce::Font(fontSize));
                g.setColour(isSel ? juce::Colours::black : juce::Colours::white.withAlpha(0.8f));
                g.drawText(name,
                    juce::Rectangle<float>(nx1 + 3, ny, noteW - 6, rowH),
                    juce::Justification::centredLeft, false);
            }

            // Resize handles
            g.setColour(juce::Colours::white.withAlpha(0.15f));
            g.fillRect(nx1 + 1, ny + 1, 4.0f, rowH - 2);
            g.fillRect(nx2 - 5, ny + 1, 4.0f, rowH - 2);
        }
    }

    // Loop region highlight
    if (transport && transport->loopEnabled && transport->loopEndBeat > transport->loopStartBeat) {
        float lx1 = gridX + ((float)transport->loopStartBeat - scrollBeat) / visibleBeats * gridW;
        float lx2 = gridX + ((float)transport->loopEndBeat - scrollBeat) / visibleBeats * gridW;
        lx1 = std::max(lx1, gridX);
        lx2 = std::min(lx2, gridX + gridW);
        if (lx2 > lx1) {
            g.setColour(juce::Colour(60, 60, 150).withAlpha(0.15f));
            g.fillRect(lx1, 0.0f, lx2 - lx1, gridH);
            g.setColour(juce::Colour(100, 100, 255).withAlpha(0.5f));
            g.drawVerticalLine((int)lx1, 0, gridH);
            g.drawVerticalLine((int)lx2, 0, gridH);
        }
    }

    // Markers
    for (auto& marker : graph.markers) {
        float mx = gridX + ((float)marker.beat - scrollBeat) / visibleBeats * gridW;
        if (mx >= gridX - 5 && mx <= gridX + gridW + 5) {
            auto col = juce::Colour(marker.color);
            // Vertical line
            g.setColour(col.withAlpha(0.5f));
            g.drawVerticalLine((int)mx, 0, gridH);
            // Flag at top
            g.setColour(col.withAlpha(0.8f));
            juce::Path flag;
            flag.addTriangle(mx, 0, mx + 8, 4, mx, 8);
            g.fillPath(flag);
            g.fillRect(mx, 0.0f, 2.0f, 10.0f);
            // Label
            g.setFont(9.0f);
            g.setColour(col);
            g.drawText(marker.name, (int)mx + 4, 0, 80, 12, juce::Justification::centredLeft);
        }
    }

    // Insert chain layers: flat-edged bars stacked flush at the top of the
    // grid, representing the track's serial effect chain. Bottom layer
    // processes first, top layer last. No rounded corners, no gaps — they
    // look like stackable blocks.
    if (!node->effectRegions.empty()) {
        const float barH = 12.0f;
        const float barY0 = 0.0f; // flush with top

        // Assign each unique (linkId,groupId) pair its own layer row.
        std::vector<std::pair<int,int>> seenPairs;
        auto getRow = [&](int lid, int gid) -> int {
            for (int i = 0; i < (int)seenPairs.size(); ++i)
                if (seenPairs[i].first == lid && seenPairs[i].second == gid) return i;
            seenPairs.push_back({lid, gid});
            return (int)seenPairs.size() - 1;
        };

        for (const auto& region : node->effectRegions) {
            float rx1 = beatToX(region.startBeat);
            float rx2 = beatToX(region.endBeat);
            if (rx2 < gridX || rx1 > gridX + gridW) continue;
            rx1 = std::max(rx1, gridX);
            rx2 = std::min(rx2, gridX + gridW);

            int row = getRow(region.linkId, region.groupId);
            float ry = barY0 + row * barH; // flush stacking, no gaps

            uint32_t col = region.color;
            if (col == 0) {
                if (region.groupId >= 0) {
                    if (auto* grp = graph.findEffectGroup(region.groupId))
                        col = grp->color;
                }
                if (col == 0 && region.linkId >= 0)
                    col = getDistinctColor(region.linkId);
                if (col == 0)
                    col = 0xFF808080;
            }

            auto barColor = juce::Colour((uint8_t)((col >> 16) & 0xFF),
                                         (uint8_t)((col >> 8) & 0xFF),
                                         (uint8_t)(col & 0xFF));

            // Flat bar body — no rounded corners, stackable
            g.setColour(barColor.withAlpha(0.70f));
            g.fillRect(rx1, ry, rx2 - rx1, barH);
            // Thin top/bottom edge lines for separation
            g.setColour(barColor.brighter(0.4f));
            g.drawHorizontalLine((int)ry, rx1, rx2);
            g.drawHorizontalLine((int)(ry + barH), rx1, rx2);

            // Label: effect name
            if (rx2 - rx1 > 40.0f) {
                juce::String label;
                if (region.groupId >= 0) {
                    if (auto* grp = graph.findEffectGroup(region.groupId))
                        label = grp->name.empty() ? "" : juce::String(grp->name);
                }
                if (label.isEmpty() && region.linkId >= 0) {
                    for (auto& link : graph.links) {
                        if (link.id == region.linkId) {
                            for (auto& n : graph.nodes)
                                for (auto& pin : n.pinsIn)
                                    if (pin.id == link.endPin) { label = n.name; break; }
                            break;
                        }
                    }
                }
                if (label.isNotEmpty()) {
                    g.setColour(juce::Colours::white.withAlpha(0.9f));
                    g.setFont(juce::Font(std::min(10.0f, barH - 2.0f)));
                    g.drawText(label, rx1 + 4, (int)ry, (int)(rx2 - rx1 - 8), (int)barH,
                               juce::Justification::centredLeft, false);
                }
            }
        }
    }

    // Playback cursor (absolute beat, no double-offset)
    if (transport && transport->playing) {
        float cursorBeat = (float)transport->positionBeats();
        float cx = gridX + ((cursorBeat - scrollBeat) / visibleBeats) * gridW;
        if (cx >= gridX && cx <= gridX + gridW) {
            g.setColour(juce::Colours::white);
            g.drawVerticalLine((int)cx, 0, gridH);
        }
    }

    // Expression / velocity / automation lane
    if (exprLane != ExprNone && (exprLane == ExprVelocity || exprLane == ExprAutomation || node->mpeEnabled)) {
        float exprY = gridH - EXPR_LANE_HEIGHT;
        float exprH = EXPR_LANE_HEIGHT;

        // Background
        g.setColour(juce::Colour(15, 15, 25));
        g.fillRect(gridX, exprY, gridW, exprH);

        // Lane label: explain what this section shows so it's not mysterious.
        {
            juce::String laneLabel;
            juce::String laneHint;
            switch (exprLane) {
                case ExprVelocity:
                    laneLabel = "Velocity (note loudness)";
                    laneHint = "Drag bars up/down to change how loud each note plays";
                    break;
                case ExprAutomation:
                    if (autoParamIndex >= 0 && autoParamIndex < (int)node->params.size())
                        laneLabel = "Automation: " + juce::String(node->params[autoParamIndex].name);
                    else
                        laneLabel = "Automation";
                    laneHint = "Click to add points, drag to move, right-click to delete";
                    break;
                case ExprPitchBend:  laneLabel = "Pitch Bend"; laneHint = "Per-note pitch curve"; break;
                case ExprSlide:      laneLabel = "Slide";      laneHint = "Per-note slide (CC74)"; break;
                case ExprPressure:   laneLabel = "Pressure";   laneHint = "Per-note aftertouch"; break;
                default: break;
            }
            if (laneLabel.isNotEmpty()) {
                g.setColour(juce::Colours::white.withAlpha(0.7f));
                g.setFont(juce::Font(11.0f, juce::Font::bold));
                g.drawText(laneLabel, gridX + 6, (int)exprY + 2, 200, 14,
                           juce::Justification::centredLeft, false);
                g.setColour(juce::Colours::grey.withAlpha(0.5f));
                g.setFont(juce::Font(9.0f));
                g.drawText(laneHint, gridX + 210, (int)exprY + 2, 350, 14,
                           juce::Justification::centredLeft, false);
            }
        }

        // Divider at top of lane
        g.setColour(juce::Colour(60, 60, 80));
        g.drawHorizontalLine((int)exprY, gridX, gridX + gridW);

        // Center line for pitch bend
        if (exprLane == ExprPitchBend) {
            g.setColour(juce::Colours::grey.withAlpha(0.3f));
            g.drawHorizontalLine((int)(exprY + exprH * 0.5f), gridX, gridX + gridW);
        }

        // Draw per-note data
        for (auto& clip : node->clips) {
            int ciIdx = (int)(&clip - &node->clips[0]);
            for (int ni = 0; ni < (int)clip.notes.size(); ++ni) {
                auto& note = clip.notes[ni];
                float noteStartBeat = clip.startBeat + note.getOffset();
                float nx1 = beatToX(noteStartBeat);

                bool isSelected = state.selected.count({ciIdx, ni}) > 0;
                auto noteColor = juce::Colour(clip.color).brighter(0.3f);

                if (exprLane == ExprVelocity) {
                    // Velocity: one vertical bar per note, spanning the
                    // note's actual duration so it visually lines up with
                    // the note above. Min 3px wide so very short notes are
                    // still clickable.
                    float noteEndBeat = noteStartBeat + note.getDuration();
                    float nx2 = beatToX(noteEndBeat);
                    float barWidth = std::max(3.0f, nx2 - nx1);
                    float barHeight = (note.velocity / 127.0f) * exprH;
                    float barY = exprY + exprH - barHeight;

                    // Off-screen cull
                    if (nx2 < gridX || nx1 > gridX + gridW) continue;

                    g.setColour(isSelected ? noteColor : noteColor.withAlpha(0.7f));
                    g.fillRect(nx1, barY, barWidth, barHeight);
                    g.setColour(juce::Colours::white.withAlpha(isSelected ? 0.6f : 0.3f));
                    g.drawRect(juce::Rectangle<float>(nx1, barY, barWidth, barHeight), 1.0f);
                } else {
                    // MPE expression curves
                    auto& curve = exprLane == ExprPitchBend ? note.expression.pitchBend
                                : exprLane == ExprSlide     ? note.expression.slide
                                : note.expression.pressure;
                    if (curve.empty()) continue;

                    float noteEndBeat = noteStartBeat + note.getDuration();
                    float nx2 = beatToX(noteEndBeat);
                    if (nx2 < gridX || nx1 > gridX + gridW) continue;

                    g.setColour(isSelected ? noteColor : noteColor.withAlpha(0.6f));

                    juce::Path path;
                    bool first = true;
                    for (auto& pt : curve) {
                        float px = beatToX(noteStartBeat + pt.time);
                        float py = exprY + exprH * (1.0f - pt.value);
                        if (first) { path.startNewSubPath(px, py); first = false; }
                        else path.lineTo(px, py);
                    }
                    g.strokePath(path, juce::PathStrokeType(isSelected ? 2.0f : 1.5f));

                    for (auto& pt : curve) {
                        float px = beatToX(noteStartBeat + pt.time);
                        float py = exprY + exprH * (1.0f - pt.value);
                        g.fillEllipse(px - 3, py - 3, 6, 6);
                    }
                }
            }
        }

        // Automation lane: draw automation curve for selected parameter
        if (exprLane == ExprAutomation && autoParamIndex >= 0 &&
            autoParamIndex < (int)node->params.size()) {
            auto& param = node->params[autoParamIndex];
            auto& lane = param.automation;
            float pMin = param.minVal, pMax = param.maxVal;
            float pRange = std::max(0.001f, pMax - pMin);

            // Draw param name and range
            g.setColour(juce::Colours::orange.withAlpha(0.7f));
            g.setFont(10.0f);
            g.drawText(juce::String(param.name) + " [" + juce::String(pMin, 1) + ".." + juce::String(pMax, 1) + "]",
                        gridX + 4, (int)exprY + 2, 200, 12, juce::Justification::centredLeft);

            // Draw current value as horizontal line
            float curNorm = (param.value - pMin) / pRange;
            float curY = exprY + exprH * (1.0f - curNorm);
            g.setColour(juce::Colours::orange.withAlpha(0.2f));
            g.drawHorizontalLine((int)curY, gridX, gridX + gridW);

            if (!lane.points.empty()) {
                // Draw automation curve using Catmull-Rom interpolation.
                // Sample the curve at sub-beat resolution for smoothness.
                g.setColour(juce::Colours::orange);
                juce::Path autoPath;
                float beatStart = scrollBeat;
                float beatEnd = scrollBeat + visibleBeats;
                float step = visibleBeats / std::max(1.0f, gridW * 0.5f); // ~2 px per sample
                bool first = true;
                for (float b = beatStart; b <= beatEnd; b += step) {
                    float val = lane.evaluate(b);
                    if (val < -0.5f) continue; // sentinel = no data
                    float px = gridX + ((b - scrollBeat) / visibleBeats) * gridW;
                    float norm = (val - pMin) / pRange;
                    float py = exprY + exprH * (1.0f - juce::jlimit(0.0f, 1.0f, norm));
                    if (first) { autoPath.startNewSubPath(px, py); first = false; }
                    else autoPath.lineTo(px, py);
                }
                g.strokePath(autoPath, juce::PathStrokeType(2.0f));

                // Draw control point dots (white fill, orange border)
                for (int pi = 0; pi < (int)lane.points.size(); ++pi) {
                    auto& pt = lane.points[pi];
                    float px = gridX + ((pt.beat - scrollBeat) / visibleBeats) * gridW;
                    float norm = (pt.value - pMin) / pRange;
                    float py = exprY + exprH * (1.0f - juce::jlimit(0.0f, 1.0f, norm));
                    if (px >= gridX - 5 && px <= gridX + gridW + 5) {
                        bool isDragged = (exprDragPtIdx == pi && exprDragNI == -2 && dragMode == DragExprPoint);
                        g.setColour(isDragged ? juce::Colours::yellow : juce::Colours::white);
                        g.fillEllipse(px - 4, py - 4, 8, 8);
                        g.setColour(juce::Colours::orange);
                        g.drawEllipse(px - 4, py - 4, 8, 8, 1.5f);
                    }
                }
            }
        }

        // Divider line
        g.setColour(juce::Colours::grey.withAlpha(0.5f));
        g.drawHorizontalLine((int)exprY, gridX, gridX + gridW);
    }

    // Selection box (offset by toolbar height since graphics origin is shifted)
    if (dragMode == DragBox) {
        float tbh = (float)toolbarHeight();
        auto r = juce::Rectangle<float>::leftTopRightBottom(
            std::min(dragStartScreen.x, dragCurrentScreen.x),
            std::min(dragStartScreen.y, dragCurrentScreen.y) - tbh,
            std::max(dragStartScreen.x, dragCurrentScreen.x),
            std::max(dragStartScreen.y, dragCurrentScreen.y) - tbh);
        g.setColour(juce::Colours::yellow.withAlpha(0.1f));
        g.fillRect(r);
        g.setColour(juce::Colours::yellow.withAlpha(0.5f));
        g.drawRect(r, 1);
    }

    g.restoreState();
}

// Helper: convert screen pos to beat/pitch
std::pair<float, int> PianoRollComponent::screenToBeatPitch(juce::Point<float> pos) const {
    float gridX = KEY_WIDTH;
    float gridW = getWidth() - KEY_WIDTH - SCROLLBAR_SIZE;
    float gridH = getHeight() - toolbarHeight() - SCROLLBAR_SIZE;
    pos.y -= toolbarHeight();
    int visRange = state.visibleRange;
    int pitchHi = state.scrollPitch + visRange / 2;
    float totalBeats = graph.getTimelineBeats(*node);
    float absOffset = node->absoluteBeatOffset;
    float absTotalBeats = totalBeats + absOffset;
    float rowH = gridH / std::max(visRange, 1);

    float visibleBeats = absTotalBeats / std::max(state.hZoom, 0.1f);
    // Screen shows absolute beats; convert to node-local beat
    float absBeat = state.hScroll + ((pos.x - gridX) / gridW) * visibleBeats;
    float beat = absBeat - absOffset;
    int pitch = pitchHi - (int)std::floor(pos.y / rowH);
    // Clamp to valid MIDI range and visible pitch range so clicking
    // below the grid doesn't create notes at inaudible pitches.
    int pitchLo = pitchHi - visRange;
    pitch = juce::jlimit(std::max(0, pitchLo), std::min(127, pitchHi), pitch);
    return {beat, pitch};
}

PianoRollComponent::NoteHit PianoRollComponent::findNoteAt(juce::Point<float> screenPos) const {
    auto [beat, pitch] = screenToBeatPitch(screenPos);
    float gridX = KEY_WIDTH;
    float gridW = getWidth() - KEY_WIDTH - SCROLLBAR_SIZE;
    float totalBeats = graph.getTimelineBeats(*node);
    float absOffset = node->absoluteBeatOffset;
    float absTotalBeats = totalBeats + absOffset;
    float visibleBeats = absTotalBeats / std::max(state.hZoom, 0.1f);
    float scrollBeat = state.hScroll;
    auto beatToX = [&](float b) { return gridX + ((b + absOffset - scrollBeat) / visibleBeats) * gridW; };

    for (int ci = 0; ci < (int)node->clips.size(); ++ci) {
        auto& clip = node->clips[ci];
        for (int ni = 0; ni < (int)clip.notes.size(); ++ni) {
            auto& n = clip.notes[ni];
            float absBeat = clip.startBeat + n.offset;
            float nx1 = beatToX(absBeat);
            float nx2 = beatToX(absBeat + n.duration);
            // Expand hit zone by 4px on each side for edge detection
            if (screenPos.x >= nx1 - 4 && screenPos.x <= nx2 + 4 && n.pitch == pitch) {
                NoteHit::Edge edge = NoteHit::Body;
                if (screenPos.x < nx1 + 6) edge = NoteHit::Left;
                else if (screenPos.x > nx2 - 6) edge = NoteHit::Right;
                return {ci, ni, edge};
            }
        }
    }
    return {};
}

void PianoRollComponent::mouseDown(const juce::MouseEvent& e) {
    refreshNode(); if (!node) return;
    auto [beat, pitch] = screenToBeatPitch(e.position);

    // Expression lane interaction
    if (isInExprLane(e.position)) {
        auto [exBeat, exVal] = screenToExprBeatValue(e.position);

        // Automation lane: click to add/drag/delete points
        if (exprLane == ExprAutomation && autoParamIndex >= 0 &&
            autoParamIndex < (int)node->params.size()) {
            auto& param = node->params[autoParamIndex];
            auto& lane = param.automation;
            float pMin = param.minVal, pMax = param.maxVal;
            // Convert screen to beat and normalized value
            float gridW2 = (float)(getWidth() - KEY_WIDTH - SCROLLBAR_SIZE);
            float gridH2 = (float)(getHeight() - toolbarHeight() - SCROLLBAR_SIZE);
            float exprY2 = (float)toolbarHeight() + gridH2 - EXPR_LANE_HEIGHT;
            float absTotalBeats2 = graph.getTimelineBeats(*node) + node->absoluteBeatOffset;
            float visBeats2 = absTotalBeats2 / std::max(0.1f, state.hZoom);
            float absBeat = state.hScroll + ((e.position.x - KEY_WIDTH) / gridW2) * visBeats2;
            float normVal = 1.0f - (e.position.y - exprY2) / EXPR_LANE_HEIGHT;
            normVal = juce::jlimit(0.0f, 1.0f, normVal);
            float paramVal = pMin + normVal * (pMax - pMin);

            if (e.mods.isRightButtonDown()) {
                // Delete nearest point
                float bestDist = 15.0f;
                int bestIdx = -1;
                for (int i = 0; i < (int)lane.points.size(); ++i) {
                    float px = KEY_WIDTH + ((lane.points[i].beat - state.hScroll) / visBeats2) * gridW2;
                    float dist = std::abs(px - e.position.x);
                    if (dist < bestDist) { bestDist = dist; bestIdx = i; }
                }
                if (bestIdx >= 0) {
                    lane.points.erase(lane.points.begin() + bestIdx);
                    graph.dirty = true;
                }
            } else {
                // Find nearest existing point to drag
                float bestDist = 10.0f;
                int bestIdx = -1;
                for (int i = 0; i < (int)lane.points.size(); ++i) {
                    float px = KEY_WIDTH + ((lane.points[i].beat - state.hScroll) / visBeats2) * gridW2;
                    float dist = std::abs(px - e.position.x);
                    if (dist < bestDist) { bestDist = dist; bestIdx = i; }
                }
                if (bestIdx >= 0) {
                    // Drag existing point
                    dragMode = DragExprPoint;
                    exprDragPtIdx = bestIdx;
                    exprDragCI = autoParamIndex; // reuse for param index
                    exprDragNI = -2; // sentinel: automation mode
                } else {
                    // Add new point
                    lane.points.push_back({absBeat, paramVal});
                    std::sort(lane.points.begin(), lane.points.end(),
                        [](auto& a, auto& b) { return a.beat < b.beat; });
                    graph.dirty = true;
                    // Find and drag the new point
                    for (int i = 0; i < (int)lane.points.size(); ++i) {
                        if (std::abs(lane.points[i].beat - absBeat) < 0.01f) {
                            dragMode = DragExprPoint;
                            exprDragPtIdx = i;
                            exprDragCI = autoParamIndex;
                            exprDragNI = -2;
                            break;
                        }
                    }
                }
            }
            repaint();
            return;
        }

        // Velocity lane: click a note's bar to set its velocity, then drag
        // vertically to continue tweaking. Hit-test by the note's actual
        // time span so every note corresponds to exactly one bar. If two
        // notes overlap in time (polyphony), pick the one whose bar top is
        // closest to the click y — that way the user can still target the
        // bar they can actually see.
        if (exprLane == ExprVelocity) {
            // Compute the click's Y within the expr lane in pixels so we can
            // compare to each bar's top edge for disambiguation.
            float gridH = getHeight() - toolbarHeight() - SCROLLBAR_SIZE;
            float exprY = toolbarHeight() + gridH - EXPR_LANE_HEIGHT;
            float clickY = e.position.y;

            int bestCI = -1, bestNI = -1;
            float bestDistY = 1e9f;

            for (int ci = 0; ci < (int)node->clips.size(); ++ci) {
                auto& clip = node->clips[ci];
                for (int ni = 0; ni < (int)clip.notes.size(); ++ni) {
                    auto& note = clip.notes[ni];
                    float noteStart = clip.startBeat + note.getOffset();
                    float noteEnd   = noteStart + note.getDuration();
                    // Allow a small slop in beat-space so very short notes
                    // are still clickable (must be at least ~5px wide).
                    float minDurBeats = 0.01f;
                    if (noteEnd < noteStart + minDurBeats)
                        noteEnd = noteStart + minDurBeats;
                    if (exBeat < noteStart || exBeat > noteEnd) continue;

                    // Pick the note whose bar-top is closest to click y
                    // (but always accept any match if nothing better).
                    float barHeight = (note.velocity / 127.0f) * EXPR_LANE_HEIGHT;
                    float barTop = exprY + EXPR_LANE_HEIGHT - barHeight;
                    float d = std::abs(barTop - clickY);
                    if (d < bestDistY) {
                        bestDistY = d;
                        bestCI = ci;
                        bestNI = ni;
                    }
                }
            }

            if (bestCI >= 0) {
                auto& clip = node->clips[bestCI];
                auto& note = clip.notes[bestNI];
                // Snapshot for undo
                dragBeforeSnapshot = {{bestCI, bestNI, note.offset, note.duration,
                                       note.detune, note.pitch, note.velocity}};
                note.velocity = juce::jlimit(1, 127, (int)(exVal * 127.0f));
                dragMode = DragExprPoint;
                exprDragCI = bestCI;
                exprDragNI = bestNI;
                graph.dirty = true;
                repaint();
            }
            return;
        }

        if (e.mods.isRightButtonDown()) {
            // Right-click: delete nearest breakpoint
            float bestDist = 10.0f;
            int bestCI = -1, bestNI = -1, bestPt = -1;
            for (int ci = 0; ci < (int)node->clips.size(); ++ci) {
                auto& clip = node->clips[ci];
                for (int ni = 0; ni < (int)clip.notes.size(); ++ni) {
                    auto& note = clip.notes[ni];
                    auto* curve = const_cast<PianoRollComponent*>(this)->getExprCurve(note);
                    if (!curve) continue;
                    float noteStart = clip.startBeat + note.getOffset();
                    for (int pi = 0; pi < (int)curve->size(); ++pi) {
                        float ptBeat = noteStart + (*curve)[pi].time;
                        float db = std::abs(ptBeat - exBeat);
                        float dv = std::abs((*curve)[pi].value - exVal);
                        float dist = db * 20.0f + dv * EXPR_LANE_HEIGHT;
                        if (dist < bestDist) {
                            bestDist = dist; bestCI = ci; bestNI = ni; bestPt = pi;
                        }
                    }
                }
            }
            if (bestPt >= 0) {
                auto* curve = getExprCurve(node->clips[bestCI].notes[bestNI]);
                if (curve) curve->erase(curve->begin() + bestPt);
                graph.dirty = true;
                repaint();
            }
            return;
        }

        // Left-click: find nearest breakpoint to drag, or add new one
        float bestDist = 10.0f;
        int bestCI = -1, bestNI = -1, bestPt = -1;
        for (int ci = 0; ci < (int)node->clips.size(); ++ci) {
            auto& clip = node->clips[ci];
            for (int ni = 0; ni < (int)clip.notes.size(); ++ni) {
                auto& note = clip.notes[ni];
                auto* curve = getExprCurve(note);
                if (!curve) continue;
                float noteStart = clip.startBeat + note.getOffset();
                for (int pi = 0; pi < (int)curve->size(); ++pi) {
                    float ptBeat = noteStart + (*curve)[pi].time;
                    float db = std::abs(ptBeat - exBeat);
                    float dv = std::abs((*curve)[pi].value - exVal);
                    float dist = db * 20.0f + dv * EXPR_LANE_HEIGHT;
                    if (dist < bestDist) {
                        bestDist = dist; bestCI = ci; bestNI = ni; bestPt = pi;
                    }
                }
            }
        }

        if (bestPt >= 0) {
            // Drag existing point
            dragMode = DragExprPoint;
            exprDragCI = bestCI; exprDragNI = bestNI; exprDragPtIdx = bestPt;
        } else {
            // Add new point to the note under the cursor
            // Find which note's time span contains exBeat
            for (int ci = 0; ci < (int)node->clips.size(); ++ci) {
                auto& clip = node->clips[ci];
                for (int ni = 0; ni < (int)clip.notes.size(); ++ni) {
                    auto& note = clip.notes[ni];
                    float noteStart = clip.startBeat + note.getOffset();
                    float noteEnd = noteStart + note.getDuration();
                    if (exBeat >= noteStart && exBeat <= noteEnd) {
                        auto* curve = getExprCurve(note);
                        if (!curve) continue;
                        float timeInNote = exBeat - noteStart;
                        // Insert sorted by time
                        int insertIdx = 0;
                        for (int pi = 0; pi < (int)curve->size(); ++pi)
                            if ((*curve)[pi].time < timeInNote) insertIdx = pi + 1;
                        curve->insert(curve->begin() + insertIdx, {timeInNote, exVal});
                        dragMode = DragExprPoint;
                        exprDragCI = ci; exprDragNI = ni; exprDragPtIdx = insertIdx;
                        graph.dirty = true;
                        repaint();
                        return;
                    }
                }
            }
        }
        repaint();
        return;
    }

    if (e.mods.isRightButtonDown()) {
        auto hit = findNoteAt(e.position);
        if (hit.valid()) {
            if (!state.selected.count({hit.ci, hit.ni}))
                state.selected = {{hit.ci, hit.ni}};
            showNoteMenu();
        } else {
            lastClickBeat = beat;
            lastClickPitch = pitch;
            showEmptyMenu();
        }
        return;
    }

    auto hit = findNoteAt(e.position);
    if (hit.valid()) {
        if (e.mods.isShiftDown()) {
            auto k = std::make_pair(hit.ci, hit.ni);
            if (state.selected.count(k)) state.selected.erase(k);
            else state.selected.insert(k);
        } else {
            if (!state.selected.count({hit.ci, hit.ni}))
                state.selected = {{hit.ci, hit.ni}};

            if (hit.edge == NoteHit::Left)
                dragMode = DragResizeLeft;
            else if (hit.edge == NoteHit::Right)
                dragMode = DragResizeRight;
            else
                dragMode = DragNote;

            dragNoteCI = hit.ci;
            dragNoteNI = hit.ni;
            dragStartBeat = beat;
            dragStartPitch = pitch;

            // Capture before-snapshot for undo
            captureSelectedSnapshot(dragBeforeSnapshot);
        }
    } else if (e.position.x < KEY_WIDTH && e.position.y > toolbarHeight()) {
        // Audition
        {
            std::lock_guard<std::mutex> lock(*node->auditionMutex);
            node->pendingAudition.push_back({true, pitch, 100});
        }
        auditionKeys[pitch] = juce::Time::getMillisecondCounterHiRes();
        repaint();
        // Schedule note-off and visual clear
        juce::Timer::callAfterDelay(500, [this, pitch]() {
            {
                std::lock_guard<std::mutex> lock(*node->auditionMutex);
                node->pendingAudition.push_back({false, pitch, 0});
            }
            auditionKeys.erase(pitch);
            repaint();
        });
    } else {
        if (!e.mods.isShiftDown()) state.selected.clear();
        dragMode = DragBox;
        dragStartScreen = e.position;
        dragCurrentScreen = e.position;
        lastClickBeat = beat;
        lastClickPitch = pitch;
    }
    repaint();
}

void PianoRollComponent::mouseDrag(const juce::MouseEvent& e) {
    refreshNode(); if (!node) return;
    auto [beat, pitch] = screenToBeatPitch(e.position);
    float snap = state.snap > 0 ? state.snap : 0.0625f;
    bool altHeld = e.mods.isAltDown();

    auto snapBeat = [&](float b) -> float {
        return altHeld ? b : std::round(b / snap) * snap;
    };

    if (dragMode == DragNote) {
        float snappedHover = snapBeat(beat);
        float snappedStart = snapBeat(dragStartBeat);
        float db = snappedHover - snappedStart;
        int dp = pitch - dragStartPitch;
        if (db != 0 || dp != 0) {
            for (auto& [ci, ni] : state.selected) {
                if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size()) {
                    auto& n = node->clips[ci].notes[ni];
                    n.offset = std::max(0.0f, n.offset + db);
                    n.pitch = juce::jlimit(0, 127, n.pitch + dp);
                }
            }
            dragStartBeat = beat;
            dragStartPitch = pitch;
        }
    } else if (dragMode == DragResizeRight) {
        if (dragNoteCI >= 0 && dragNoteCI < (int)node->clips.size()
            && dragNoteNI >= 0 && dragNoteNI < (int)node->clips[dragNoteCI].notes.size()) {
            auto& n = node->clips[dragNoteCI].notes[dragNoteNI];
            float absStart = node->clips[dragNoteCI].startBeat + n.offset;
            float newEnd = altHeld ? beat : snapBeat(beat);
            n.duration = std::max(0.03125f, newEnd - absStart);
        }
    } else if (dragMode == DragResizeLeft) {
        if (dragNoteCI >= 0 && dragNoteCI < (int)node->clips.size()
            && dragNoteNI >= 0 && dragNoteNI < (int)node->clips[dragNoteCI].notes.size()) {
            auto& n = node->clips[dragNoteCI].notes[dragNoteNI];
            float absEnd = node->clips[dragNoteCI].startBeat + n.offset + n.duration;
            float newStart = altHeld ? beat : snapBeat(beat);
            newStart = std::min(newStart, absEnd - 0.03125f);
            newStart = std::max(node->clips[dragNoteCI].startBeat, newStart);
            n.duration = absEnd - newStart;
            n.offset = newStart - node->clips[dragNoteCI].startBeat;
        }
    } else if (dragMode == DragBox) {
        dragCurrentScreen = e.position;
    } else if (dragMode == DragExprPoint) {
        // Automation point dragging
        if (exprDragNI == -2 && exprDragCI >= 0 && exprDragCI < (int)node->params.size()) {
            auto& param = node->params[exprDragCI];
            auto& lane = param.automation;
            if (exprDragPtIdx >= 0 && exprDragPtIdx < (int)lane.points.size()) {
                float gridW2 = (float)(getWidth() - KEY_WIDTH - SCROLLBAR_SIZE);
                float gridH2 = (float)(getHeight() - toolbarHeight() - SCROLLBAR_SIZE);
                float exprY2 = (float)toolbarHeight() + gridH2 - EXPR_LANE_HEIGHT;
                float absTotalBeats2 = graph.getTimelineBeats(*node) + node->absoluteBeatOffset;
                float visBeats2 = absTotalBeats2 / std::max(0.1f, state.hZoom);
                float absBeat = state.hScroll + ((e.position.x - KEY_WIDTH) / gridW2) * visBeats2;
                float normVal = 1.0f - (e.position.y - exprY2) / EXPR_LANE_HEIGHT;
                normVal = juce::jlimit(0.0f, 1.0f, normVal);
                lane.points[exprDragPtIdx].beat = std::max(0.0f, absBeat);
                lane.points[exprDragPtIdx].value = param.minVal + normVal * (param.maxVal - param.minVal);
                graph.dirty = true;
            }
        }
        else if (exprDragCI >= 0 && exprDragCI < (int)node->clips.size()
            && exprDragNI >= 0 && exprDragNI < (int)node->clips[exprDragCI].notes.size()) {
            auto& note = node->clips[exprDragCI].notes[exprDragNI];
            if (exprLane == ExprVelocity) {
                auto [exBeat, exVal] = screenToExprBeatValue(e.position);
                note.velocity = juce::jlimit(1, 127, (int)(exVal * 127.0f));
                graph.dirty = true;
            } else {
                auto* curve = getExprCurve(note);
                if (curve && exprDragPtIdx >= 0 && exprDragPtIdx < (int)curve->size()) {
                    auto [exBeat, exVal] = screenToExprBeatValue(e.position);
                    float noteStart = node->clips[exprDragCI].startBeat + note.getOffset();
                    float timeInNote = juce::jlimit(0.0f, note.getDuration(), exBeat - noteStart);
                    (*curve)[exprDragPtIdx].time = timeInNote;
                    (*curve)[exprDragPtIdx].value = exVal;
                    graph.dirty = true;
                }
            }
        }
    }

    // Set cursor based on hover
    if (dragMode == DragNone || dragMode == DragBox) {
        auto probe = findNoteAt(e.position);
        if (probe.valid() && (probe.edge == NoteHit::Left || probe.edge == NoteHit::Right))
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        else
            setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    repaint();
}

void PianoRollComponent::mouseUp(const juce::MouseEvent& e) {
    refreshNode(); if (!node) return;
    if (dragMode == DragBox) {
        auto [b1, p1] = screenToBeatPitch(dragStartScreen);
        auto [b2, p2] = screenToBeatPitch(dragCurrentScreen);
        float dist = std::abs(b2 - b1) + std::abs((float)(p2 - p1));

        if (dist < 0.3f) {
            // Place a note — find or extend a clip to fit
            float snap = state.snap > 0 ? state.snap : 0.25f;
            float sb = std::round(lastClickBeat / snap) * snap;
            if (sb < 0) sb = 0;
            float noteDur = snap * 4;

            // Remember the current visible beat range so we can preserve it
            float oldTotalBeats = graph.getTimelineBeats(*node);
            float oldVisibleBeats = oldTotalBeats / std::max(0.1f, state.hZoom);

            // Find a clip that contains this beat
            Clip* targetClip = nullptr;
            for (auto& clip : node->clips) {
                if (clip.startBeat <= sb && sb < clip.startBeat + clip.lengthBeats) {
                    targetClip = &clip;
                    break;
                }
            }

            // If no clip found, extend the nearest clip or create one
            if (!targetClip) {
                if (!node->clips.empty()) {
                    Clip* best = &node->clips.back();
                    for (auto& clip : node->clips) {
                        if (clip.startBeat <= sb)
                            best = &clip;
                    }
                    float needed = sb + noteDur - best->startBeat;
                    best->lengthBeats = std::max(best->lengthBeats, std::ceil(needed / 4.0f) * 4.0f);
                    targetClip = best;
                } else {
                    float len = std::max(4.0f, std::ceil((sb + noteDur) / 4.0f) * 4.0f);
                    node->clips.push_back({"Clip 1", 0, len, 0xFF4488CC});
                    targetClip = &node->clips.back();
                }
            }

            if (targetClip) {
                MidiNote nn;
                nn.offset = sb - targetClip->startBeat;
                nn.pitch = lastClickPitch;
                nn.duration = noteDur;
                targetClip->notes.push_back(nn);

                // Push undo for note placement (already done, use pushDone)
                int ci = (int)(targetClip - &node->clips[0]);
                MidiNote nnCopy = nn;
                auto* nodePtr = node;
                graph.undoTree.pushDone(std::make_unique<LambdaCommand>(
                    "Place note",
                    [nodePtr, ci, nnCopy]() {
                        if (ci < (int)nodePtr->clips.size())
                            nodePtr->clips[ci].notes.push_back(nnCopy);
                    },
                    [nodePtr, ci]() {
                        if (ci < (int)nodePtr->clips.size() && !nodePtr->clips[ci].notes.empty())
                            nodePtr->clips[ci].notes.pop_back();
                    }
                ));
            }

            // Preserve the visible beat range after clip extension
            float newTotalBeats = graph.getTimelineBeats(*node);
            if (newTotalBeats > oldTotalBeats && oldVisibleBeats > 0) {
                state.hZoom = newTotalBeats / oldVisibleBeats;
            }
        } else {
            // Select notes in box
            float beatMin = std::min(b1, b2), beatMax = std::max(b1, b2);
            int pitchMin = std::min(p1, p2), pitchMax = std::max(p1, p2);
            for (int ci = 0; ci < (int)node->clips.size(); ++ci) {
                for (int ni = 0; ni < (int)node->clips[ci].notes.size(); ++ni) {
                    auto& n = node->clips[ci].notes[ni];
                    float ab = node->clips[ci].startBeat + n.offset;
                    if (ab + n.duration >= beatMin && ab <= beatMax
                        && n.pitch >= pitchMin && n.pitch <= pitchMax)
                        state.selected.insert({ci, ni});
                }
            }
        }
    }
    if (dragMode == DragExprPoint) {
        // Sort automation points after drag
        if (exprDragNI == -2 && exprDragCI >= 0 && exprDragCI < (int)node->params.size()) {
            auto& lane = node->params[exprDragCI].automation;
            std::sort(lane.points.begin(), lane.points.end(),
                [](auto& a, auto& b) { return a.beat < b.beat; });
        }
        // Sort expression points by time after drag
        if (exprDragCI >= 0 && exprDragCI < (int)node->clips.size()
            && exprDragNI >= 0 && exprDragNI < (int)node->clips[exprDragCI].notes.size()) {
            auto* curve = getExprCurve(node->clips[exprDragCI].notes[exprDragNI]);
            if (curve)
                std::sort(curve->begin(), curve->end(),
                    [](auto& a, auto& b) { return a.time < b.time; });
        }
    }

    // Push undo for drag operations
    if (!dragBeforeSnapshot.empty()) {
        if (dragMode == DragNote || dragMode == DragResizeLeft || dragMode == DragResizeRight) {
            std::string desc = (dragMode == DragNote) ? "Move notes"
                             : (dragMode == DragResizeLeft) ? "Resize note (left)"
                             : "Resize note (right)";
            pushDragUndo(desc, dragBeforeSnapshot);
        } else if (dragMode == DragExprPoint && exprLane == ExprVelocity) {
            // Velocity drag undo
            if (exprDragCI >= 0 && exprDragNI >= 0) {
                state.selected = {{exprDragCI, exprDragNI}};
                pushDragUndo("Change velocity", dragBeforeSnapshot);
            }
        }
        dragBeforeSnapshot.clear();
    }

    dragMode = DragNone;
    repaint();
}

void PianoRollComponent::scrollBarMoved(juce::ScrollBar* bar, double newRangeStart) {
    if (bar == &hScrollBar) {
        float totalBeats = graph.getTimelineBeats(*node);
        state.hScroll = (float)(newRangeStart * totalBeats);
    } else if (bar == &vScrollBar) {
        state.scrollPitch = 127 - (int)newRangeStart - state.visibleRange / 2;
        state.scrollPitch = juce::jlimit(12, 115, state.scrollPitch);
    }
    repaint();
}

void PianoRollComponent::updateScrollBars() {
    float totalBeats = graph.getTimelineBeats(*node);
    float visibleBeats = totalBeats / std::max(state.hZoom, 0.1f);

    hScrollBar.setRangeLimits(0, 1.0);
    double thumbSize = juce::jlimit(0.01, 1.0, (double)(visibleBeats / totalBeats));
    hScrollBar.setCurrentRange(state.hScroll / totalBeats, thumbSize, juce::dontSendNotification);

    vScrollBar.setRangeLimits(0, 127);
    int pitchHi = state.scrollPitch + state.visibleRange / 2;
    vScrollBar.setCurrentRange(127 - pitchHi, state.visibleRange, juce::dontSendNotification);
}

void PianoRollComponent::mouseMove(const juce::MouseEvent& e) {
    refreshNode(); if (!node) return;
    auto hit = findNoteAt(e.position);
    if (hit.valid() && (hit.edge == NoteHit::Left || hit.edge == NoteHit::Right))
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    else
        setMouseCursor(juce::MouseCursor::NormalCursor);
}

void PianoRollComponent::mouseWheelMove(const juce::MouseEvent& e,
                                         const juce::MouseWheelDetails& wheel) {
    refreshNode(); if (!node) return;
    if (e.mods.isCtrlDown()) {
        // Ctrl+scroll: horizontal zoom
        float zoomDelta = wheel.deltaY * 0.3f;
        state.hZoom = juce::jlimit(0.2f, 20.0f, state.hZoom * (1.0f + zoomDelta));
        hZoomSlider.setValue(state.hZoom, juce::dontSendNotification);
    } else if (e.mods.isShiftDown()) {
        // Shift+scroll: horizontal pan
        float totalBeats = graph.getTimelineBeats(*node);
        float visibleBeats = totalBeats / std::max(state.hZoom, 0.1f);
        state.hScroll -= wheel.deltaY * visibleBeats * 0.1f;
        state.hScroll = juce::jlimit(0.0f, std::max(0.0f, totalBeats - visibleBeats), state.hScroll);
    } else {
        // Plain scroll: pitch
        int delta = (int)(wheel.deltaY * 10);
        if (delta == 0) delta = wheel.deltaY > 0 ? 1 : -1;
        state.scrollPitch = juce::jlimit(12, 115, state.scrollPitch + delta);
    }
    updateScrollBars();
    repaint();
}

void PianoRollComponent::showNoteMenu() {
    refreshNode(); if (!node) return;
    juce::PopupMenu menu;
    int numSel = (int)state.selected.size();
    menu.addSectionHeader(juce::String(numSel) + " note" + (numSel != 1 ? "s" : ""));
    menu.addItem(1, "Delete");
    menu.addItem(2, "Duplicate");
    menu.addItem(60, "Copy");
    menu.addItem(61, "Cut");
    menu.addItem(62, "Paste", !clipboard.empty());
    menu.addItem(63, "Zoom to Selection");
    menu.addSeparator();

    juce::PopupMenu transpose;
    transpose.addItem(10, "+1 Semitone");
    transpose.addItem(11, "-1 Semitone");
    transpose.addItem(12, "+1 Octave");
    transpose.addItem(13, "-1 Octave");
    menu.addSubMenu("Transpose", transpose);

    juce::PopupMenu dur;
    dur.addItem(20, "Double");
    dur.addItem(21, "Halve");
    dur.addSeparator();
    dur.addItem(22, "1/16");
    dur.addItem(23, "1/8");
    dur.addItem(24, "1/4");
    dur.addItem(25, "1/2");
    dur.addItem(26, "1");
    menu.addSubMenu("Duration", dur);

    juce::PopupMenu timeShift;
    timeShift.addItem(30, "Shift Left");
    timeShift.addItem(31, "Shift Right");
    menu.addSubMenu("Time Shift", timeShift);

    menu.addItem(32, "Reverse");

    juce::PopupMenu detune;
    detune.addItem(40, "Reset to 0");
    detune.addItem(41, "-50 cents");
    detune.addItem(42, "-25 cents");
    detune.addItem(43, "+25 cents");
    detune.addItem(44, "+50 cents");
    menu.addSubMenu("Detune", detune);

    menu.addSeparator();

    // Root/Key/Mode/Scale submenus
    juce::PopupMenu rootMenu;
    for (int i = 0; i < 12; ++i)
        rootMenu.addItem(100 + i, MusicTheory::NOTE_NAMES[i], true, state.keyRoot == i);
    menu.addSubMenu("Root", rootMenu);

    juce::PopupMenu keyMenu;
    int ki = 0;
    for (auto& [name, _] : MusicTheory::keys())
        keyMenu.addItem(200 + ki++, name, true, state.activeCategory == "key" && state.keyName == name);
    menu.addSubMenu("Key", keyMenu);

    juce::PopupMenu modeMenu;
    int mi = 0;
    for (auto& [name, _] : MusicTheory::modes())
        modeMenu.addItem(300 + mi++, name, true, state.activeCategory == "mode" && state.modeName == name);
    menu.addSubMenu("Mode", modeMenu);

    juce::PopupMenu scaleMenu;
    int si = 0;
    for (auto& [name, _] : MusicTheory::scales())
        scaleMenu.addItem(400 + si++, name, true, state.activeCategory == "scale" && state.scaleName == name);
    menu.addSubMenu("Scale", scaleMenu);

    juce::PopupMenu quantMenu;
    quantMenu.addItem(70, "1/4 beat (100%)");
    quantMenu.addItem(71, "1/2 beat (100%)");
    quantMenu.addItem(72, "1 beat (100%)");
    quantMenu.addItem(73, "1/4 beat (50%)");
    quantMenu.addItem(74, "1/2 beat (50%)");
    quantMenu.addItem(75, "1 beat (50%)");
    quantMenu.addItem(76, "1/8 beat (100%)");
    quantMenu.addItem(77, "1/3 beat (100%) [triplet]");
    menu.addSubMenu("Quantize", quantMenu);

    menu.addItem(50, "Snap to Scale");
    menu.addItem(51, "Change Key");
    menu.addItem(52, "Detect Key");

    menu.addSeparator();
    menu.addItem(3, "Select All");
    menu.addItem(4, "Deselect All");

    menu.showMenuAsync(juce::PopupMenu::Options(), [this](int result) {
        auto apply = [&](auto fn) {
            for (auto& [ci, ni] : state.selected)
                if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size())
                    fn(node->clips[ci].notes[ni]);
        };
        // Undo-aware apply: captures before/after snapshots
        auto applyUndo = [&](const std::string& desc, auto fn) {
            std::vector<NoteSnapshot> before;
            captureSelectedSnapshot(before);
            apply(fn);
            pushDragUndo(desc, before);
        };
        auto selectAll = [&]() {
            state.selected.clear();
            for (int ci = 0; ci < (int)node->clips.size(); ++ci)
                for (int ni = 0; ni < (int)node->clips[ci].notes.size(); ++ni)
                    state.selected.insert({ci, ni});
        };

        float snapVal = state.snap > 0 ? state.snap : 0.25f;

        switch (result) {
            case 1: // Delete
                for (auto it = state.selected.rbegin(); it != state.selected.rend(); ++it) {
                    auto [ci, ni] = *it;
                    if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size())
                        node->clips[ci].notes.erase(node->clips[ci].notes.begin() + ni);
                }
                state.selected.clear();
                break;
            case 2: // Duplicate
                for (auto& [ci, ni] : state.selected) {
                    if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size()) {
                        auto dup = node->clips[ci].notes[ni];
                        dup.offset += 0.25f;
                        node->clips[ci].notes.push_back(dup);
                    }
                }
                break;
            case 3: selectAll(); break;
            case 4: state.selected.clear(); break;
            case 10: applyUndo("+1 semi", [](MidiNote& n) { n.pitch = std::min(127, n.pitch + 1); }); break;
            case 11: applyUndo("-1 semi", [](MidiNote& n) { n.pitch = std::max(0, n.pitch - 1); }); break;
            case 12: applyUndo("+1 octave", [](MidiNote& n) { n.pitch = std::min(127, n.pitch + 12); }); break;
            case 13: applyUndo("-1 octave", [](MidiNote& n) { n.pitch = std::max(0, n.pitch - 12); }); break;
            case 20: applyUndo("x2 duration", [](MidiNote& n) { n.duration *= 2; }); break;
            case 21: applyUndo("/2 duration", [](MidiNote& n) { n.duration = std::max(0.125f, n.duration / 2); }); break;
            case 22: applyUndo("Set 1/16", [](MidiNote& n) { n.duration = 0.25f; }); break;
            case 23: applyUndo("Set 1/8", [](MidiNote& n) { n.duration = 0.5f; }); break;
            case 24: applyUndo("Set 1/4", [](MidiNote& n) { n.duration = 1.0f; }); break;
            case 25: applyUndo("Set 1/2", [](MidiNote& n) { n.duration = 2.0f; }); break;
            case 26: applyUndo("Set 1", [](MidiNote& n) { n.duration = 4.0f; }); break;
            case 30: applyUndo("Shift left", [snapVal](MidiNote& n) { n.offset = std::max(0.0f, n.offset - snapVal); }); break;
            case 31: applyUndo("Shift right", [snapVal](MidiNote& n) { n.offset += snapVal; }); break;
            case 32: { // Reverse
                if (!state.selected.empty()) {
                    float minOff = 1e9f, maxEnd = 0;
                    for (auto& [ci, ni] : state.selected) {
                        if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size()) {
                            auto& n = node->clips[ci].notes[ni];
                            float ab = node->clips[ci].startBeat + n.offset;
                            minOff = std::min(minOff, ab);
                            maxEnd = std::max(maxEnd, ab + n.duration);
                        }
                    }
                    for (auto& [ci, ni] : state.selected) {
                        if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size()) {
                            auto& n = node->clips[ci].notes[ni];
                            float ab = node->clips[ci].startBeat + n.offset;
                            n.offset = std::max(0.0f, maxEnd - (ab - minOff) - n.duration - node->clips[ci].startBeat);
                        }
                    }
                }
                break;
            }
            case 40: applyUndo("Reset detune", [](MidiNote& n) { n.detune = 0; }); break;
            case 41: applyUndo("Detune -50c", [](MidiNote& n) { n.detune = -50; }); break;
            case 42: applyUndo("Detune -25c", [](MidiNote& n) { n.detune = -25; }); break;
            case 43: applyUndo("Detune +25c", [](MidiNote& n) { n.detune = 25; }); break;
            case 44: applyUndo("Detune +50c", [](MidiNote& n) { n.detune = 50; }); break;
            case 50: { // Snap to Scale
                auto getIntervals = [&]() -> std::vector<int> {
                    const ScaleMap* table = nullptr;
                    if (state.activeCategory == "key") table = &MusicTheory::keys();
                    else if (state.activeCategory == "mode") table = &MusicTheory::modes();
                    else if (state.activeCategory == "scale") table = &MusicTheory::scales();
                    if (table) { auto* v = findScale(*table, state.activeName()); if (v) return *v; }
                    return {0,2,4,5,7,9,11};
                };
                auto intervals = getIntervals();
                int root = state.keyRoot;
                apply([&](MidiNote& n) { n.pitch = MusicTheory::snapToScale(n.pitch, root, intervals); });
                break;
            }
            case 51: { // Change Key
                auto getIntervals = [&]() -> std::vector<int> {
                    const ScaleMap* table = nullptr;
                    if (state.activeCategory == "key") table = &MusicTheory::keys();
                    else if (state.activeCategory == "mode") table = &MusicTheory::modes();
                    else if (state.activeCategory == "scale") table = &MusicTheory::scales();
                    if (table) { auto* v = findScale(*table, state.activeName()); if (v) return *v; }
                    return {0,2,4,5,7,9,11};
                };
                auto intervals = getIntervals();
                int root = state.keyRoot;
                apply([&](MidiNote& n) {
                    n.pitch = juce::jlimit(0, 127,
                        MusicTheory::degreeToPitch(n.degree, n.octave, n.chromaticOffset, root, intervals));
                });
                break;
            }
            case 52: { // Detect Key
                std::vector<int> pitches;
                if (!state.selected.empty()) {
                    for (auto& [ci, ni] : state.selected)
                        if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size())
                            pitches.push_back(node->clips[ci].notes[ni].pitch);
                } else {
                    for (auto& clip : node->clips)
                        for (auto& n : clip.notes) pitches.push_back(n.pitch);
                }
                auto results = MusicTheory::detectKeys(pitches);
                // Show results in a popup
                juce::PopupMenu resultMenu;
                for (int i = 0; i < std::min((int)results.size(), 20); ++i) {
                    auto& m = results[i];
                    auto label = juce::String(MusicTheory::NOTE_NAMES[m.root]) + " " + m.scaleName
                        + " [" + m.category + "] " + juce::String((int)(m.coverage * 100)) + "%";
                    resultMenu.addItem(1000 + i, label);
                }
                resultMenu.showMenuAsync(juce::PopupMenu::Options(), [this, results](int r) {
                    if (r >= 1000 && r < 1000 + (int)results.size()) {
                        auto& m = results[r - 1000];
                        state.keyRoot = m.root;
                        state.activeCategory = m.category;
                        if (m.category == "key") state.keyName = m.scaleName;
                        else if (m.category == "mode") state.modeName = m.scaleName;
                        else state.scaleName = m.scaleName;
                        repaint();
                    }
                });
                break;
            }
            case 60: copySelected(); break;
            case 61: cutSelected(); break;
            case 62: pasteAtCursor(); break;
            case 63: zoomToSelection(); break;
            case 70: case 71: case 72: case 73: case 74: case 75: case 76: case 77: {
                // Quantize selected notes
                float grid = 0.25f;
                float strength = 1.0f;
                switch (result) {
                    case 70: grid = 0.25f; strength = 1.0f; break;
                    case 71: grid = 0.5f;  strength = 1.0f; break;
                    case 72: grid = 1.0f;  strength = 1.0f; break;
                    case 73: grid = 0.25f; strength = 0.5f; break;
                    case 74: grid = 0.5f;  strength = 0.5f; break;
                    case 75: grid = 1.0f;  strength = 0.5f; break;
                    case 76: grid = 0.125f; strength = 1.0f; break;
                    case 77: grid = 1.0f / 3.0f; strength = 1.0f; break;
                }
                // If nothing selected, quantize all notes
                auto targets = state.selected;
                if (targets.empty()) {
                    for (int ci2 = 0; ci2 < (int)node->clips.size(); ++ci2)
                        for (int ni2 = 0; ni2 < (int)node->clips[ci2].notes.size(); ++ni2)
                            targets.insert({ci2, ni2});
                }
                for (auto& [ci2, ni2] : targets) {
                    if (ci2 < (int)node->clips.size() && ni2 < (int)node->clips[ci2].notes.size()) {
                        auto& n2 = node->clips[ci2].notes[ni2];
                        float snapped = std::round(n2.offset / grid) * grid;
                        n2.offset += (snapped - n2.offset) * strength;
                        n2.offset = std::max(0.0f, n2.offset);
                    }
                }
                break;
            }
            default:
                if (result >= 100 && result < 112) state.keyRoot = result - 100;
                else if (result >= 200 && result < 300) {
                    int i = 0; for (auto& [name, _] : MusicTheory::keys()) {
                        if (i++ == result - 200) { state.activeCategory = "key"; state.keyName = name; break; }
                    }
                } else if (result >= 300 && result < 400) {
                    int i = 0; for (auto& [name, _] : MusicTheory::modes()) {
                        if (i++ == result - 300) { state.activeCategory = "mode"; state.modeName = name; break; }
                    }
                } else if (result >= 400 && result < 500) {
                    int i = 0; for (auto& [name, _] : MusicTheory::scales()) {
                        if (i++ == result - 400) { state.activeCategory = "scale"; state.scaleName = name; break; }
                    }
                }
                break;
        }
        repaint();
    });
}

// ==============================================================================
// Clipboard operations
// ==============================================================================

bool PianoRollComponent::keyPressed(const juce::KeyPress& key) {
    refreshNode(); if (!node) return false;
    if (key.getModifiers().isCtrlDown()) {
        switch (key.getKeyCode()) {
            case 'C': copySelected(); return true;
            case 'X': cutSelected(); return true;
            case 'V': pasteAtCursor(); return true;
            case 'A': selectAll(); return true;
            case 'F': zoomToSelection(); return true;
        }
    }
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) {
        deleteSelected();
        return true;
    }
    return false;
}

void PianoRollComponent::copySelected() {
    clipboard.clear();
    if (state.selected.empty()) return;

    // Find the earliest selected note's absolute beat position
    float minBeat = 1e9f;
    for (auto& [ci, ni] : state.selected) {
        if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size()) {
            float absBeat = node->clips[ci].startBeat + node->clips[ci].notes[ni].getOffset();
            minBeat = std::min(minBeat, absBeat);
        }
    }

    // Copy notes relative to the earliest
    for (auto& [ci, ni] : state.selected) {
        if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size()) {
            auto& n = node->clips[ci].notes[ni];
            float absBeat = node->clips[ci].startBeat + n.getOffset();
            ClipboardNote cn;
            cn.offsetFromFirst = absBeat - minBeat;
            cn.pitch = n.pitch;
            cn.duration = n.getDuration();
            cn.velocity = n.velocity;
            cn.detune = n.detune;
            cn.expression = n.expression;
            clipboard.push_back(cn);
        }
    }
}

void PianoRollComponent::cutSelected() {
    copySelected();
    deleteSelected();
}

void PianoRollComponent::deleteSelected() {
    if (state.selected.empty()) return;

    // Capture deleted notes for undo
    struct DeletedNote { int ci; int ni; MidiNote note; };
    std::vector<DeletedNote> deleted;

    // Sort in reverse order so indices stay valid as we remove
    std::vector<std::pair<int, int>> sorted(state.selected.begin(), state.selected.end());
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) {
        return a.first > b.first || (a.first == b.first && a.second > b.second);
    });

    for (auto& [ci, ni] : sorted) {
        if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size()) {
            deleted.push_back({ci, ni, node->clips[ci].notes[ni]});
            node->clips[ci].notes.erase(node->clips[ci].notes.begin() + ni);
        }
    }

    auto* nodePtr = node;
    // Reverse deleted so undo re-inserts in forward order
    std::reverse(deleted.begin(), deleted.end());
    graph.undoTree.pushDone(std::make_unique<LambdaCommand>(
        "Delete " + std::to_string(deleted.size()) + " notes",
        [nodePtr, deleted]() {
            // redo: delete again (reverse order)
            for (int i = (int)deleted.size() - 1; i >= 0; --i) {
                auto& d = deleted[i];
                if (d.ci < (int)nodePtr->clips.size() && d.ni < (int)nodePtr->clips[d.ci].notes.size())
                    nodePtr->clips[d.ci].notes.erase(nodePtr->clips[d.ci].notes.begin() + d.ni);
            }
        },
        [nodePtr, deleted]() {
            // undo: re-insert notes
            for (auto& d : deleted) {
                if (d.ci < (int)nodePtr->clips.size())
                    nodePtr->clips[d.ci].notes.insert(nodePtr->clips[d.ci].notes.begin() + d.ni, d.note);
            }
        }
    ));

    state.selected.clear();
    graph.dirty = true;
    repaint();
}

void PianoRollComponent::pasteAtCursor() {
    if (clipboard.empty()) return;

    // Paste at lastClickBeat position (where the user last clicked)
    float pasteBeat = lastClickBeat;
    float snap = state.snap > 0 ? state.snap : 0.25f;
    pasteBeat = std::round(pasteBeat / snap) * snap;

    // Find or create a clip that covers the paste range
    Clip* targetClip = nullptr;
    for (auto& clip : node->clips) {
        if (clip.startBeat <= pasteBeat && pasteBeat < clip.startBeat + clip.lengthBeats) {
            targetClip = &clip;
            break;
        }
    }
    if (!targetClip && !node->clips.empty())
        targetClip = &node->clips[0];
    if (!targetClip) return;

    state.selected.clear();
    int ci = (int)(targetClip - &node->clips[0]);

    for (auto& cn : clipboard) {
        MidiNote nn;
        nn.offset = pasteBeat + cn.offsetFromFirst - targetClip->startBeat;
        nn.pitch = cn.pitch;
        nn.duration = cn.duration;
        nn.velocity = cn.velocity;
        nn.detune = cn.detune;
        nn.expression = cn.expression;
        targetClip->notes.push_back(nn);

        // Select the pasted note
        int ni = (int)targetClip->notes.size() - 1;
        state.selected.insert({ci, ni});

        // Extend clip if needed
        float noteEnd = nn.offset + nn.duration;
        if (noteEnd > targetClip->lengthBeats)
            targetClip->lengthBeats = std::ceil(noteEnd / 4.0f) * 4.0f;
    }

    graph.dirty = true;
    repaint();
}

void PianoRollComponent::selectAll() {
    state.selected.clear();
    for (int ci = 0; ci < (int)node->clips.size(); ++ci)
        for (int ni = 0; ni < (int)node->clips[ci].notes.size(); ++ni)
            state.selected.insert({ci, ni});
    repaint();
}

void PianoRollComponent::zoomToSelection() {
    if (state.selected.empty()) return;

    float minBeat = 1e9f, maxBeat = -1e9f;
    int minPitch = 127, maxPitch = 0;

    for (auto& [ci, ni] : state.selected) {
        if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size()) {
            auto& n = node->clips[ci].notes[ni];
            float ab = node->clips[ci].startBeat + n.getOffset();
            minBeat = std::min(minBeat, ab);
            maxBeat = std::max(maxBeat, ab + n.getDuration());
            minPitch = std::min(minPitch, n.pitch);
            maxPitch = std::max(maxPitch, n.pitch);
        }
    }

    if (minBeat >= maxBeat) return;

    // Add padding
    float beatPad = std::max(1.0f, (maxBeat - minBeat) * 0.1f);
    int pitchPad = std::max(2, (maxPitch - minPitch) / 5);
    minBeat -= beatPad;
    maxBeat += beatPad;
    minPitch = std::max(0, minPitch - pitchPad);
    maxPitch = std::min(127, maxPitch + pitchPad);

    // Set horizontal zoom and scroll
    float totalBeats = graph.getTimelineBeats(*node);
    float visibleBeats = maxBeat - minBeat;
    state.hZoom = std::max(0.1f, totalBeats / visibleBeats);
    state.hScroll = std::max(0.0f, minBeat);

    // Set vertical scroll and range
    state.scrollPitch = (minPitch + maxPitch) / 2;
    state.visibleRange = std::max(12, maxPitch - minPitch + pitchPad * 2);

    updateScrollBars();
    repaint();
}

void PianoRollComponent::copyClipAtCursor() {
    for (auto& clip : node->clips) {
        if (lastClickBeat >= clip.startBeat && lastClickBeat < clip.startBeat + clip.lengthBeats) {
            clipClipboard = std::make_unique<Clip>(clip);
            return;
        }
    }
}

void PianoRollComponent::pasteClipAtCursor() {
    if (!clipClipboard) return;
    float snap = state.snap > 0 ? state.snap : 1.0f;
    float pasteBeat = std::round(lastClickBeat / snap) * snap;

    Clip pasted = *clipClipboard;
    pasted.startBeat = pasteBeat;
    pasted.name += " (copy)";
    node->clips.push_back(pasted);
    graph.dirty = true;
    repaint();
}

// ==============================================================================
// Undo support for drag operations
// ==============================================================================

void PianoRollComponent::captureSelectedSnapshot(std::vector<NoteSnapshot>& snap) {
    snap.clear();
    for (auto& [ci, ni] : state.selected) {
        if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size()) {
            auto& n = node->clips[ci].notes[ni];
            snap.push_back({ci, ni, n.offset, n.duration, n.detune, n.pitch, n.velocity});
        }
    }
}

void PianoRollComponent::pushDragUndo(const std::string& desc,
                                       const std::vector<NoteSnapshot>& before) {
    // Capture "after" state
    std::vector<NoteSnapshot> after;
    captureSelectedSnapshot(after);

    // Check if anything actually changed
    bool changed = false;
    if (before.size() != after.size()) changed = true;
    else {
        for (size_t i = 0; i < before.size(); ++i) {
            auto& b = before[i]; auto& a = after[i];
            if (b.offset != a.offset || b.duration != a.duration || b.pitch != a.pitch ||
                b.detune != a.detune || b.velocity != a.velocity) {
                changed = true; break;
            }
        }
    }
    if (!changed) return;

    auto beforeCopy = before;
    auto afterCopy = after;
    auto* nodePtr = node;

    graph.undoTree.execute(std::make_unique<LambdaCommand>(
        desc,
        [afterCopy, nodePtr]() {
            for (auto& s : afterCopy) {
                if (s.ci < (int)nodePtr->clips.size() && s.ni < (int)nodePtr->clips[s.ci].notes.size()) {
                    auto& n = nodePtr->clips[s.ci].notes[s.ni];
                    n.offset = s.offset; n.duration = s.duration; n.pitch = s.pitch;
                    n.detune = s.detune; n.velocity = s.velocity;
                }
            }
        },
        [beforeCopy, nodePtr]() {
            for (auto& s : beforeCopy) {
                if (s.ci < (int)nodePtr->clips.size() && s.ni < (int)nodePtr->clips[s.ci].notes.size()) {
                    auto& n = nodePtr->clips[s.ci].notes[s.ni];
                    n.offset = s.offset; n.duration = s.duration; n.pitch = s.pitch;
                    n.detune = s.detune; n.velocity = s.velocity;
                }
            }
        }
    ));
}

// ==============================================================================
// Expression lane helpers
// ==============================================================================

void PianoRollComponent::triggerAction(const std::string& action) {
    if (action == "nudge_left")       timeLeftBtn.triggerClick();
    else if (action == "nudge_right") timeRightBtn.triggerClick();
    else if (action == "transpose_up_semi")   transpUpSemiBtn.triggerClick();
    else if (action == "transpose_down_semi") transpDownSemiBtn.triggerClick();
    else if (action == "transpose_up_oct")    transpUpOctBtn.triggerClick();
    else if (action == "transpose_down_oct")  transpDownOctBtn.triggerClick();
    else if (action == "double_duration")     dblDurBtn.triggerClick();
    else if (action == "halve_duration")      halfDurBtn.triggerClick();
    else if (action == "reverse")             reverseBtn.triggerClick();
    else if (action == "select_all")          selectAllBtn.triggerClick();
    else if (action == "deselect")            deselectBtn.triggerClick();
}

bool PianoRollComponent::isInExprLane(juce::Point<float> pos) const {
    // Any active expression lane (velocity, automation, MPE) should catch
    // clicks so they don't fall through to note placement below the grid.
    if (exprLane == ExprNone) return false;
    float gridH = getHeight() - toolbarHeight() - SCROLLBAR_SIZE;
    float exprY = toolbarHeight() + gridH - EXPR_LANE_HEIGHT;
    return pos.y >= exprY && pos.y < exprY + EXPR_LANE_HEIGHT && pos.x >= KEY_WIDTH;
}

std::pair<float, float> PianoRollComponent::screenToExprBeatValue(juce::Point<float> pos) const {
    float gridX = KEY_WIDTH;
    float gridW = getWidth() - KEY_WIDTH - SCROLLBAR_SIZE;
    float gridH = getHeight() - toolbarHeight() - SCROLLBAR_SIZE;
    float exprY = toolbarHeight() + gridH - EXPR_LANE_HEIGHT;
    float totalBeats = graph.getTimelineBeats(*node);
    float visibleBeats = totalBeats / std::max(state.hZoom, 0.1f);

    float beat = state.hScroll + ((pos.x - gridX) / gridW) * visibleBeats;
    float value = 1.0f - (pos.y - exprY) / EXPR_LANE_HEIGHT;
    value = juce::jlimit(0.0f, 1.0f, value);
    return {beat, value};
}

std::vector<ExpressionPoint>* PianoRollComponent::getExprCurve(MidiNote& note) {
    switch (exprLane) {
        case ExprPitchBend: return &note.expression.pitchBend;
        case ExprSlide:     return &note.expression.slide;
        case ExprPressure:  return &note.expression.pressure;
        default: return nullptr;
    }
}

const std::vector<ExpressionPoint>* PianoRollComponent::getExprCurveConst(const MidiNote& note) const {
    switch (exprLane) {
        case ExprPitchBend: return &note.expression.pitchBend;
        case ExprSlide:     return &note.expression.slide;
        case ExprPressure:  return &note.expression.pressure;
        default: return nullptr;
    }
}

void PianoRollComponent::showEmptyMenu() {
    refreshNode(); if (!node) return;
    juce::PopupMenu menu;
    menu.addItem(1, "Place Note Here");
    menu.addSeparator();

    juce::PopupMenu snapMenu;
    snapMenu.addItem(10, "1/4 beat", true, std::abs(state.snap - 0.25f) < 0.01f);
    snapMenu.addItem(11, "1/2 beat", true, std::abs(state.snap - 0.5f) < 0.01f);
    snapMenu.addItem(12, "1 beat", true, std::abs(state.snap - 1.0f) < 0.01f);
    snapMenu.addItem(13, "Off", true, state.snap < 0.01f);
    menu.addSubMenu("Snap Grid", snapMenu);

    // Root/Key/Mode/Scale
    juce::PopupMenu rootMenu;
    for (int i = 0; i < 12; ++i)
        rootMenu.addItem(100 + i, MusicTheory::NOTE_NAMES[i], true, state.keyRoot == i);
    menu.addSubMenu("Root", rootMenu);

    juce::PopupMenu keyMenu;
    int ki = 0;
    for (auto& [name, _] : MusicTheory::keys())
        keyMenu.addItem(200 + ki++, name, true, state.activeCategory == "key" && state.keyName == name);
    menu.addSubMenu("Key", keyMenu);

    juce::PopupMenu modeMenu;
    int mi = 0;
    for (auto& [name, _] : MusicTheory::modes())
        modeMenu.addItem(300 + mi++, name, true, state.activeCategory == "mode" && state.modeName == name);
    menu.addSubMenu("Mode", modeMenu);

    juce::PopupMenu scaleMenu;
    int si = 0;
    for (auto& [name, _] : MusicTheory::scales())
        scaleMenu.addItem(400 + si++, name, true, state.activeCategory == "scale" && state.scaleName == name);
    menu.addSubMenu("Scale", scaleMenu);

    menu.addItem(14, "Detect Key");

    juce::PopupMenu quantMenu2;
    quantMenu2.addItem(70, "1/4 beat (100%)");
    quantMenu2.addItem(71, "1/2 beat (100%)");
    quantMenu2.addItem(72, "1 beat (100%)");
    quantMenu2.addItem(73, "1/4 beat (50%)");
    quantMenu2.addItem(74, "1/2 beat (50%)");
    quantMenu2.addItem(75, "1 beat (50%)");
    quantMenu2.addItem(76, "1/8 beat (100%)");
    quantMenu2.addItem(77, "1/3 beat (triplet, 100%)");
    menu.addSubMenu("Quantize All", quantMenu2);

    // Clip operations
    menu.addSeparator();
    juce::PopupMenu clipMenu;
    clipMenu.addItem(80, "Split Clip at Cursor");
    clipMenu.addItem(81, "Trim Clip Start to Cursor");
    clipMenu.addItem(82, "Trim Clip End to Cursor");
    clipMenu.addSeparator();
    clipMenu.addItem(85, "Copy Clip");
    clipMenu.addItem(86, "Paste Clip", clipClipboard != nullptr);
    clipMenu.addItem(87, "Duplicate Clip");
    clipMenu.addSeparator();
    clipMenu.addItem(83, "Delete Clip at Cursor");
    clipMenu.addItem(84, "Add New Clip Here");
    clipMenu.addSeparator();
    clipMenu.addItem(88, "Insert Time Here (1 bar)");
    clipMenu.addItem(89, "Delete Time (1 bar at cursor)");
    clipMenu.addSeparator();
    clipMenu.addItem(95, "Set Loop Start Here");
    clipMenu.addItem(96, "Set Loop End Here");
    clipMenu.addSeparator();
    clipMenu.addItem(97, "Add Marker Here...");
    menu.addSubMenu("Clip", clipMenu);

    // Effect regions: add/delete time-gated effect routing
    {
        juce::PopupMenu fxMenu;

        // List groups first
        for (auto& grp : graph.effectGroups) {
            auto col = juce::Colour((uint8_t)((grp.color >> 16) & 0xFF),
                                    (uint8_t)((grp.color >> 8) & 0xFF),
                                    (uint8_t)(grp.color & 0xFF));
            juce::String label = grp.name.empty()
                ? "Group #" + juce::String(grp.id)
                : juce::String(grp.name);
            fxMenu.addItem(5000 + grp.id, "Group: " + label);
        }

        // Then list individual links (showing From → To)
        if (!graph.effectGroups.empty() && !graph.links.empty())
            fxMenu.addSeparator();
        for (auto& link : graph.links) {
            juce::String src, dst;
            for (auto& n : graph.nodes) {
                for (auto& pin : n.pinsOut)
                    if (pin.id == link.startPin) src = n.name;
                for (auto& pin : n.pinsIn)
                    if (pin.id == link.endPin) dst = n.name;
            }
            if (src.isEmpty() || dst.isEmpty()) continue;
            fxMenu.addItem(6000 + link.id, src + " > " + dst);
        }

        // Delete region option (if cursor is on a region)
        bool hasRegionAtCursor = false;
        int regionIdxAtCursor = -1;
        for (int i = 0; i < (int)node->effectRegions.size(); ++i) {
            auto& r = node->effectRegions[i];
            if (lastClickBeat >= r.startBeat && lastClickBeat <= r.endBeat) {
                hasRegionAtCursor = true;
                regionIdxAtCursor = i;
                break;
            }
        }
        if (hasRegionAtCursor) {
            fxMenu.addSeparator();
            fxMenu.addItem(5999, "Delete Effect Region at Cursor");
        }

        if (fxMenu.getNumItems() > 0)
            menu.addSubMenu("Effect Regions", fxMenu);
    }

    menu.addSeparator();
    menu.addItem(15, "Paste Notes", !clipboard.empty());
    menu.addItem(86, "Paste Clip", clipClipboard != nullptr);
    menu.addItem(2, "Select All");
    menu.addItem(3, "Deselect All");
    if (onClose)
        menu.addItem(4, "Close Editor");

    menu.showMenuAsync(juce::PopupMenu::Options(), [this](int result) {
        switch (result) {
            case 1: {
                float snap = state.snap > 0 ? state.snap : 0.25f;
                float sb = std::round(lastClickBeat / snap) * snap;
                if (sb < 0) sb = 0;
                float noteDur = snap * 4;

                Clip* target = nullptr;
                for (auto& clip : node->clips)
                    if (clip.startBeat <= sb && sb < clip.startBeat + clip.lengthBeats)
                        { target = &clip; break; }

                if (!target && !node->clips.empty()) {
                    target = &node->clips.back();
                    float needed = sb + noteDur - target->startBeat;
                    target->lengthBeats = std::max(target->lengthBeats, std::ceil(needed / 4.0f) * 4.0f);
                } else if (!target) {
                    float len = std::max(4.0f, std::ceil((sb + noteDur) / 4.0f) * 4.0f);
                    node->clips.push_back({"Clip 1", 0, len, 0xFF4488CC});
                    target = &node->clips.back();
                }

                if (target) {
                    MidiNote nn;
                    nn.offset = sb - target->startBeat;
                    nn.pitch = lastClickPitch;
                    nn.duration = noteDur;
                    target->notes.push_back(nn);
                }
                break;
            }
            case 2:
                state.selected.clear();
                for (int ci = 0; ci < (int)node->clips.size(); ++ci)
                    for (int ni = 0; ni < (int)node->clips[ci].notes.size(); ++ni)
                        state.selected.insert({ci, ni});
                break;
            case 3: state.selected.clear(); break;
            case 4: if (onClose) onClose(node->id); break;
            case 15: pasteAtCursor(); break;
            case 80: { // Split clip at cursor
                float splitBeat = lastClickBeat;
                for (int ci = 0; ci < (int)node->clips.size(); ++ci) {
                    auto& clip = node->clips[ci];
                    if (splitBeat > clip.startBeat && splitBeat < clip.startBeat + clip.lengthBeats) {
                        // Create new clip for the right half
                        Clip rightClip;
                        rightClip.name = clip.name + " (R)";
                        rightClip.startBeat = splitBeat;
                        rightClip.lengthBeats = clip.startBeat + clip.lengthBeats - splitBeat;
                        rightClip.color = clip.color;
                        rightClip.channels = clip.channels;
                        rightClip.audioFilePath = clip.audioFilePath;
                        rightClip.slipOffset = clip.slipOffset + (splitBeat - clip.startBeat) * 60.0f / std::max(1.0f, graph.bpm);
                        rightClip.gainDb = clip.gainDb;
                        rightClip.fadeInBeats = 0;
                        rightClip.fadeOutBeats = clip.fadeOutBeats;

                        // Move notes to the right clip if they start after split point
                        for (auto it = clip.notes.begin(); it != clip.notes.end(); ) {
                            float noteAbsBeat = clip.startBeat + it->getOffset();
                            if (noteAbsBeat >= splitBeat) {
                                MidiNote moved = *it;
                                moved.offset = noteAbsBeat - rightClip.startBeat;
                                rightClip.notes.push_back(moved);
                                it = clip.notes.erase(it);
                            } else {
                                // Trim note if it straddles the split
                                float noteEnd = noteAbsBeat + it->getDuration();
                                if (noteEnd > splitBeat)
                                    it->duration = splitBeat - noteAbsBeat;
                                ++it;
                            }
                        }

                        // Move CC events
                        for (auto it = clip.ccEvents.begin(); it != clip.ccEvents.end(); ) {
                            float ccAbsBeat = clip.startBeat + it->offset;
                            if (ccAbsBeat >= splitBeat) {
                                MidiCCEvent moved = *it;
                                moved.offset = ccAbsBeat - rightClip.startBeat;
                                rightClip.ccEvents.push_back(moved);
                                it = clip.ccEvents.erase(it);
                            } else {
                                ++it;
                            }
                        }

                        // Trim left clip
                        clip.lengthBeats = splitBeat - clip.startBeat;
                        clip.fadeOutBeats = 0;
                        clip.name = clip.name.find(" (R)") == std::string::npos
                            ? clip.name + " (L)" : clip.name;

                        node->clips.push_back(rightClip);
                        graph.dirty = true;
                        break; // only split first matching clip
                    }
                }
                break;
            }
            case 81: { // Trim clip start to cursor
                float trimBeat = lastClickBeat;
                for (auto& clip : node->clips) {
                    if (trimBeat > clip.startBeat && trimBeat < clip.startBeat + clip.lengthBeats) {
                        float offset = trimBeat - clip.startBeat;
                        // Adjust note offsets
                        for (auto it = clip.notes.begin(); it != clip.notes.end(); ) {
                            it->offset -= offset;
                            if (it->offset + it->duration <= 0)
                                it = clip.notes.erase(it);
                            else {
                                if (it->offset < 0) {
                                    it->duration += it->offset;
                                    it->offset = 0;
                                }
                                ++it;
                            }
                        }
                        for (auto it = clip.ccEvents.begin(); it != clip.ccEvents.end(); ) {
                            it->offset -= offset;
                            if (it->offset < 0) it = clip.ccEvents.erase(it);
                            else ++it;
                        }
                        clip.slipOffset += offset * 60.0f / std::max(1.0f, graph.bpm);
                        clip.lengthBeats -= offset;
                        clip.startBeat = trimBeat;
                        graph.dirty = true;
                        break;
                    }
                }
                break;
            }
            case 82: { // Trim clip end to cursor
                float trimBeat = lastClickBeat;
                for (auto& clip : node->clips) {
                    if (trimBeat > clip.startBeat && trimBeat < clip.startBeat + clip.lengthBeats) {
                        clip.lengthBeats = trimBeat - clip.startBeat;
                        // Remove notes past the new end
                        for (auto it = clip.notes.begin(); it != clip.notes.end(); ) {
                            if (it->offset >= clip.lengthBeats)
                                it = clip.notes.erase(it);
                            else {
                                if (it->offset + it->duration > clip.lengthBeats)
                                    it->duration = clip.lengthBeats - it->offset;
                                ++it;
                            }
                        }
                        clip.ccEvents.erase(
                            std::remove_if(clip.ccEvents.begin(), clip.ccEvents.end(),
                                [&](auto& cc) { return cc.offset >= clip.lengthBeats; }),
                            clip.ccEvents.end());
                        graph.dirty = true;
                        break;
                    }
                }
                break;
            }
            case 83: { // Delete clip at cursor
                float beat = lastClickBeat;
                for (auto it = node->clips.begin(); it != node->clips.end(); ++it) {
                    if (beat >= it->startBeat && beat < it->startBeat + it->lengthBeats) {
                        node->clips.erase(it);
                        state.selected.clear();
                        graph.dirty = true;
                        break;
                    }
                }
                break;
            }
            case 85: copyClipAtCursor(); break;
            case 86: pasteClipAtCursor(); break;
            case 87: { // Duplicate clip
                copyClipAtCursor();
                if (clipClipboard) {
                    Clip dup = *clipClipboard;
                    dup.startBeat += dup.lengthBeats; // place right after original
                    dup.name += " (dup)";
                    node->clips.push_back(dup);
                    graph.dirty = true;
                }
                break;
            }
            case 88: { // Insert time
                double bpb = transport ? transport->timeSigMap.beatsPerBar(lastClickBeat) : 4.0;
                // Show dialog for amount
                auto* aw = new juce::AlertWindow("Insert Time",
                    "Insert empty time at beat " + juce::String(lastClickBeat, 1),
                    juce::MessageBoxIconType::NoIcon);
                aw->addComboBox("scope", {"This track only", "All tracks"});
                aw->addComboBox("amount", {"1 beat", "1 bar (" + juce::String(bpb, 0) + " beats)",
                                            "2 bars", "4 bars", "Custom..."});
                aw->getComboBoxComponent("amount")->setSelectedItemIndex(1);
                aw->addButton("Insert", 1); aw->addButton("Cancel", 0);
                float clickBeat = lastClickBeat;
                int nid = node->id;
                float bpbf = (float)bpb;
                aw->enterModalState(true, juce::ModalCallbackFunction::create(
                    [this, aw, clickBeat, nid, bpbf](int res) {
                        if (res == 1) {
                            float amounts[] = {1.0f, bpbf, bpbf * 2, bpbf * 4, 0};
                            int ai = aw->getComboBoxComponent("amount")->getSelectedItemIndex();
                            float dur = amounts[juce::jlimit(0, 4, ai)];
                            if (dur <= 0) dur = bpbf; // fallback
                            bool allTracks = aw->getComboBoxComponent("scope")->getSelectedItemIndex() == 1;
                            graph.insertTime(clickBeat, dur, allTracks ? -1 : nid);
                        }
                        delete aw; repaint();
                    }), true);
                break;
            }
            case 89: { // Delete time
                double bpb = transport ? transport->timeSigMap.beatsPerBar(lastClickBeat) : 4.0;
                auto* aw = new juce::AlertWindow("Delete Time",
                    "Delete time starting at beat " + juce::String(lastClickBeat, 1),
                    juce::MessageBoxIconType::NoIcon);
                aw->addComboBox("scope", {"This track only", "All tracks"});
                aw->addComboBox("amount", {"1 beat", "1 bar (" + juce::String(bpb, 0) + " beats)",
                                            "2 bars", "4 bars"});
                aw->getComboBoxComponent("amount")->setSelectedItemIndex(1);
                aw->addButton("Delete", 1); aw->addButton("Cancel", 0);
                float clickBeat = lastClickBeat;
                int nid = node->id;
                float bpbf = (float)bpb;
                aw->enterModalState(true, juce::ModalCallbackFunction::create(
                    [this, aw, clickBeat, nid, bpbf](int res) {
                        if (res == 1) {
                            float amounts[] = {1.0f, bpbf, bpbf * 2, bpbf * 4};
                            int ai = aw->getComboBoxComponent("amount")->getSelectedItemIndex();
                            float dur = amounts[juce::jlimit(0, 3, ai)];
                            bool allTracks = aw->getComboBoxComponent("scope")->getSelectedItemIndex() == 1;
                            graph.deleteTime(clickBeat, clickBeat + dur, allTracks ? -1 : nid);
                        }
                        delete aw; repaint();
                    }), true);
                break;
            }
            case 97: { // Add marker
                float markerBeat = lastClickBeat + node->absoluteBeatOffset;
                auto* aw = new juce::AlertWindow("Add Marker",
                    "At beat " + juce::String(markerBeat, 1),
                    juce::MessageBoxIconType::NoIcon);
                aw->addTextEditor("name", "", "Name:");
                aw->addButton("Add", 1, juce::KeyPress(juce::KeyPress::returnKey));
                aw->addButton("Cancel", 0);
                aw->enterModalState(true, juce::ModalCallbackFunction::create(
                    [this, aw, markerBeat](int res) {
                        if (res == 1) {
                            auto name = aw->getTextEditorContents("name").toStdString();
                            if (!name.empty()) {
                                Marker m;
                                m.id = (int)graph.markers.size() + 1000;
                                m.name = name;
                                m.beat = markerBeat;
                                graph.markers.push_back(m);
                                graph.dirty = true;
                            }
                        }
                        delete aw;
                        repaint();
                    }), true);
                break;
            }
            case 95: // Set loop start
                graph.loopStartBeat = lastClickBeat + node->absoluteBeatOffset;
                graph.loopEnabled = true;
                break;
            case 96: // Set loop end
                graph.loopEndBeat = lastClickBeat + node->absoluteBeatOffset;
                graph.loopEnabled = true;
                break;
            case 84: { // Add new clip
                float snap = state.snap > 0 ? state.snap : 1.0f;
                float start = std::floor(lastClickBeat / snap) * snap;
                Clip c;
                c.name = "Clip " + std::to_string(node->clips.size() + 1);
                c.startBeat = start;
                c.lengthBeats = 4.0f;
                c.color = juce::Colours::cornflowerblue.getARGB();
                node->clips.push_back(c);
                graph.dirty = true;
                break;
            }
            case 70: case 71: case 72: case 73: case 74: case 75: case 76: case 77: {
                float grid = 0.25f, strength = 1.0f;
                switch (result) {
                    case 70: grid = 0.25f; strength = 1.0f; break;
                    case 71: grid = 0.5f;  strength = 1.0f; break;
                    case 72: grid = 1.0f;  strength = 1.0f; break;
                    case 73: grid = 0.25f; strength = 0.5f; break;
                    case 74: grid = 0.5f;  strength = 0.5f; break;
                    case 75: grid = 1.0f;  strength = 0.5f; break;
                    case 76: grid = 0.125f; strength = 1.0f; break;
                    case 77: grid = 1.0f / 3.0f; strength = 1.0f; break;
                }
                for (auto& clip : node->clips)
                    for (auto& n : clip.notes) {
                        float snapped = std::round(n.offset / grid) * grid;
                        n.offset += (snapped - n.offset) * strength;
                        n.offset = std::max(0.0f, n.offset);
                    }
                break;
            }
            case 10: state.snap = 0.25f; break;
            case 11: state.snap = 0.5f; break;
            case 12: state.snap = 1.0f; break;
            case 13: state.snap = 0.0f; break;
            case 14: {
                std::vector<int> pitches;
                for (auto& clip : node->clips)
                    for (auto& n : clip.notes) pitches.push_back(n.pitch);
                auto results = MusicTheory::detectKeys(pitches);
                juce::PopupMenu rm;
                for (int i = 0; i < std::min((int)results.size(), 20); ++i) {
                    auto& m = results[i];
                    rm.addItem(1000 + i, juce::String(MusicTheory::NOTE_NAMES[m.root]) + " " + m.scaleName
                        + " [" + m.category + "] " + juce::String((int)(m.coverage * 100)) + "%");
                }
                rm.showMenuAsync(juce::PopupMenu::Options(), [this, results](int r) {
                    if (r >= 1000 && r < 1000 + (int)results.size()) {
                        auto& m = results[r - 1000];
                        state.keyRoot = m.root;
                        state.activeCategory = m.category;
                        if (m.category == "key") state.keyName = m.scaleName;
                        else if (m.category == "mode") state.modeName = m.scaleName;
                        else state.scaleName = m.scaleName;
                        repaint();
                    }
                });
                break;
            }
            default:
                if (result >= 100 && result < 112) state.keyRoot = result - 100;
                else if (result >= 200 && result < 300) {
                    int i = 0; for (auto& [name, _] : MusicTheory::keys()) {
                        if (i++ == result - 200) { state.activeCategory = "key"; state.keyName = name; break; }
                    }
                } else if (result >= 300 && result < 400) {
                    int i = 0; for (auto& [name, _] : MusicTheory::modes()) {
                        if (i++ == result - 300) { state.activeCategory = "mode"; state.modeName = name; break; }
                    }
                } else if (result >= 400 && result < 500) {
                    int i = 0; for (auto& [name, _] : MusicTheory::scales()) {
                        if (i++ == result - 400) { state.activeCategory = "scale"; state.scaleName = name; break; }
                    }
                } else if (result == 5999) {
                    // Delete effect region at cursor
                    for (int i = 0; i < (int)node->effectRegions.size(); ++i) {
                        auto& r = node->effectRegions[i];
                        if (lastClickBeat >= r.startBeat && lastClickBeat <= r.endBeat) {
                            node->effectRegions.erase(node->effectRegions.begin() + i);
                            graph.dirty = true;
                            break;
                        }
                    }
                } else if (result >= 5000 && result < 5999) {
                    // Add effect region for a group
                    int groupId = result - 5000;
                    float snap = state.snap > 0 ? state.snap : 1.0f;
                    float beat = std::floor(lastClickBeat / snap) * snap;
                    EffectRegion region;
                    region.groupId = groupId;
                    region.startBeat = beat;
                    region.endBeat = beat + 4.0f;
                    if (auto* grp = graph.findEffectGroup(groupId))
                        region.color = grp->color;
                    node->effectRegions.push_back(region);
                    graph.dirty = true;
                } else if (result >= 6000) {
                    // Add effect region for an individual link
                    int linkId = result - 6000;
                    float snap = state.snap > 0 ? state.snap : 1.0f;
                    float beat = std::floor(lastClickBeat / snap) * snap;
                    EffectRegion region;
                    region.linkId = linkId;
                    region.startBeat = beat;
                    region.endBeat = beat + 4.0f;
                    region.color = getDistinctColor(linkId);
                    node->effectRegions.push_back(region);
                    graph.dirty = true;
                }
                break;
        }
        repaint();
    });
}

} // namespace SoundShop

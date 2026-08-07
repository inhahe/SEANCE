#include "piano_roll_component.h"
#include "music_theory.h"
#include "track_nesting_menu.h"
#include "dialog_helpers.h"
#include "undo.h"
#include <cmath>
#include <set>

namespace SoundShop {

// ---------------------------------------------------------------------------
// SliderMenuItem - horizontal slider + preset buttons for use inside a
// PopupMenu.  Used by the note right-click menu for Velocity and Detune.
// ---------------------------------------------------------------------------
class SliderMenuItem : public juce::PopupMenu::CustomComponent {
public:
    SliderMenuItem(const juce::String& label,
                   double minVal, double maxVal, double currentVal, double step,
                   const juce::String& suffix,
                   const std::vector<std::pair<double, juce::String>>& presets,
                   std::function<void(double)> onChangeCallback)
        : juce::PopupMenu::CustomComponent(false),
          onChange(std::move(onChangeCallback))
    {
        titleLabel.setText(label, juce::dontSendNotification);
        titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
        titleLabel.setFont(juce::Font(13.0f));
        addAndMakeVisible(titleLabel);

        slider.setRange(minVal, maxVal, step);
        slider.setValue(currentVal, juce::dontSendNotification);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 45, 20);
        slider.setTextValueSuffix(suffix);
        slider.setColour(juce::Slider::textBoxTextColourId, juce::Colours::white);
        slider.setColour(juce::Slider::thumbColourId, juce::Colour(80, 140, 210));
        slider.setColour(juce::Slider::trackColourId, juce::Colour(50, 90, 140));
        slider.setColour(juce::Slider::backgroundColourId, juce::Colour(35, 35, 45));
        slider.onValueChange = [this]() {
            if (onChange) onChange(slider.getValue());
        };
        addAndMakeVisible(slider);

        for (auto& [val, name] : presets) {
            auto* btn = presetButtons.add(new juce::TextButton(name));
            btn->setColour(juce::TextButton::buttonColourId, juce::Colour(45, 45, 55));
            btn->setColour(juce::TextButton::textColourOnId, juce::Colours::white);
            btn->setColour(juce::TextButton::textColourOffId, juce::Colour(200, 200, 210));
            btn->onClick = [this, val]() {
                slider.setValue(val, juce::sendNotification);
            };
            addAndMakeVisible(btn);
        }
    }

    void getIdealSize(int& idealWidth, int& idealHeight) override {
        idealWidth = 280;
        idealHeight = 66;
    }

    void resized() override {
        auto area = getLocalBounds().reduced(8, 4);
        auto topRow = area.removeFromTop(22);
        titleLabel.setBounds(topRow.removeFromLeft(60));
        slider.setBounds(topRow);
        area.removeFromTop(4);
        auto btnRow = area.removeFromTop(20);
        int numBtns = presetButtons.size();
        if (numBtns > 0) {
            int btnW = btnRow.getWidth() / numBtns;
            for (int i = 0; i < numBtns; ++i)
                presetButtons[i]->setBounds(btnRow.removeFromLeft(btnW).reduced(1, 0));
        }
    }

private:
    juce::Label titleLabel;
    juce::Slider slider;
    juce::OwnedArray<juce::TextButton> presetButtons;
    std::function<void(double)> onChange;
};

// Static clipboards shared across all piano roll instances
std::vector<PianoRollComponent::ClipboardNote> PianoRollComponent::clipboard;
std::unique_ptr<Clip> PianoRollComponent::clipClipboard;

PianoRollComponent::PianoRollComponent(NodeGraph& g, Node& n, Transport* t)
    : graph(g), nodeId(n.id), node(&n), transport(t) {
    setWantsKeyboardFocus(true);

    // Auto-fit the vertical view to the notes that already exist. PianoRollState
    // isn't persisted, so without this every open resets to a fixed C4-centred
    // window (scrollPitch 60) - any notes above ~pitch 69 would land above the
    // top interactive row, where they render at the very top edge but can't be
    // clicked or marquee-selected. Centre the window on the notes' pitch span
    // (with a little headroom) so they open fully visible and editable.
    {
        int minPitch = 127, maxPitch = 0;
        bool any = false;
        for (auto& clip : node->clips)
            for (auto& nt : clip.notes) {
                minPitch = std::min(minPitch, nt.pitch);
                maxPitch = std::max(maxPitch, nt.pitch);
                any = true;
            }
        if (any) {
            int span = maxPitch - minPitch;
            // Keep the default zoom for ordinary clusters; only widen the window
            // when the notes span more than it can show (plus a little margin).
            // Always centre on the notes so they open inside the interactive
            // window rather than stranded above/below the default C4 view.
            state.visibleRange = juce::jlimit(state.visibleRange, 120, span + 4);
            int center = (minPitch + maxPitch) / 2;
            state.scrollPitch = juce::jlimit(state.visibleRange / 2,
                                             127 - state.visibleRange / 2,
                                             center);
        }
    }

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
    addBtn(quantizeBtn);
    addBtn(snap14Btn); addBtn(snap12Btn); addBtn(snap1Btn); addBtn(snapOffBtn);
    addBtn(snapScaleBtn); addBtn(detectKeyBtn);

    // Tooltips: cover the controls whose labels are abbreviated or use
    // music terminology a non-musician wouldn't necessarily know.
    // Skip the genuinely self-explanatory ones (Select All, Deselect,
    // Reverse, X close).
    compactBtn.setTooltip("Toggle compact mode - hides most toolbar buttons to maximize the note-editing area");
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
    snapOffBtn.setTooltip("Disable snapping - notes can be placed at any position. Hold Alt while dragging for the same effect.");
    snapScaleBtn.setTooltip("Snap notes to the chosen Key/Scale, so dragging a note up or down only lands on \"in key\" pitches");
    detectKeyBtn.setTooltip("Analyze the notes in this clip and guess the key/scale, then set the dropdowns to match");

    // Quantize button + strength slider
    quantizeBtn.setTooltip("Quantize selected notes (or all notes if none are selected) toward the nearest "
                           "snap-grid position by the percentage shown on the slider. 100% = perfectly on grid, "
                           "50% = halfway between original and grid, etc.");
    addAndMakeVisible(quantizeStrSlider);
    addAndMakeVisible(quantizeStrLbl);
    quantizeStrLbl.setText("Q:", juce::dontSendNotification);
    quantizeStrLbl.setFont(juce::Font(11.0f));
    quantizeStrLbl.setJustificationType(juce::Justification::centredRight);
    quantizeStrSlider.setRange(1.0, 100.0, 1.0);
    quantizeStrSlider.setValue(100.0, juce::dontSendNotification);
    quantizeStrSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 38, 18);
    quantizeStrSlider.setTextValueSuffix("%");
    quantizeStrSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    quantizeStrSlider.setTooltip("Quantize strength - 100% snaps notes exactly to the grid, lower values "
                                 "move notes only partway toward the grid (sounds more human)");
    quantizeBtn.onClick = [this, apply]() {
        float grid = state.snap > 0.0f ? state.snap : 0.25f;
        float strength = (float)quantizeStrSlider.getValue() / 100.0f;
        // Quantize selected, or all if nothing selected
        auto targets = state.selected;
        if (targets.empty()) {
            for (int ci2 = 0; ci2 < (int)node->clips.size(); ++ci2)
                for (int ni2 = 0; ni2 < (int)node->clips[ci2].notes.size(); ++ni2)
                    targets.insert({ci2, ni2});
        }
        graph.commitSnapshot("Quantize notes");
        for (auto& [ci2, ni2] : targets) {
            if (ci2 < (int)node->clips.size() && ni2 < (int)node->clips[ci2].notes.size()) {
                auto& n2 = node->clips[ci2].notes[ni2];
                float snapped = std::round(n2.offset / grid) * grid;
                n2.offset += (snapped - n2.offset) * strength;
                n2.offset = std::max(0.0f, n2.offset);
            }
        }
        repaint();
    };

    // Mute / Solo / Pan
    addBtn(muteBtn); addBtn(soloBtn);
    // "Mute" is universally understood - skip the tooltip. "Solo" is a
    // DAW term that non-musicians might not know.
    soloBtn.setTooltip("Solo this track - when any track is soloed, all non-soloed tracks are silenced");
    // Pan slider only for nodes that produce audio. MIDI Timelines only
    // output MIDI events - panning them does nothing.
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

    // MPE expression lane selector. Only meaningful (and only shown) when the
    // node has MPE enabled - it picks which per-note expression curve the lane
    // below the piano roll displays and edits. Mutually exclusive with the
    // automation-param selector: choosing one resets the other, since the lane
    // can only show one thing at a time.
    addAndMakeVisible(exprLaneCombo);
    exprLaneCombo.setTooltip("MPE per-note expression to view/edit in the lane below the piano roll. "
                             "Pitch Bend = per-note pitch curve, Slide = timbre (CC74), Pressure = "
                             "per-note aftertouch. Click in the lane over a note to add a point, drag "
                             "to shape the curve, right-click a point to delete. See Help -> MIDI Input "
                             "for recording these from an MPE controller.");
    exprLaneCombo.addItem("MPE Lane: off", 1);
    exprLaneCombo.addItem("Pitch Bend", 2);
    exprLaneCombo.addItem("Slide (timbre)", 3);
    exprLaneCombo.addItem("Pressure", 4);
    exprLaneCombo.setSelectedItemIndex(0, juce::dontSendNotification);
    exprLaneCombo.onChange = [this]() {
        switch (exprLaneCombo.getSelectedId()) {
            case 2: exprLane = ExprPitchBend; break;
            case 3: exprLane = ExprSlide;     break;
            case 4: exprLane = ExprPressure;  break;
            default: exprLane = ExprNone;     break;
        }
        if (exprLane != ExprNone) {
            // Selecting an MPE lane takes over the bottom lane from automation.
            autoParamIndex = -1;
            autoParamCombo.setSelectedItemIndex(0, juce::dontSendNotification);
        }
        repaint();
    };

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
            // Selecting an automation param takes over from any MPE lane.
            exprLaneCombo.setSelectedItemIndex(0, juce::dontSendNotification);
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
    rootCombo.setTooltip("Root note - the home pitch (tonic) of the key/scale, e.g. C for C Major. "
                         "Combined with Key + Mode (or a fixed Scale) to decide which notes are 'in key'.");
    // Key + Mode tooltips are (re)set by syncScaleUI() so they can explain the
    // greyed-out state too; these initial values match the enabled case.
    keyCombo.setTooltip("Key (parent scale) - the 7 notes to use. Major sounds happy/bright; "
                        "the Minor variants sound darker. Works together with Root and Mode.");
    modeCombo.setTooltip("Mode - which note of the Key becomes 'home', rotating the same 7 notes to "
                         "change the mood. Mode 1 'Ionian (Major)' = the Key itself; Dorian, Phrygian, "
                         "Lydian, etc. start on a different degree.");
    scaleCombo.setTooltip("Scale - a fixed non-diatonic note set (Pentatonic, Blues, Whole-Tone, "
                          "Chromatic, ...). Overrides Key + Mode; pick a Key or Mode to switch back. "
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
        state.activeCategory = "keymode";
        state.keyName = keyCombo.getText().toStdString();
        syncScaleUI();   // un-grey if we were in scale mode
        repaint();
    };

    // Populate mode
    { int id = 1; for (auto& [name, _] : MusicTheory::modes()) modeCombo.addItem(name, id++); }
    modeCombo.setSelectedItemIndex(findComboIndex(modeCombo, "Ionian (Major)"), juce::dontSendNotification);
    modeCombo.onChange = [this]() {
        state.activeCategory = "keymode";
        state.modeName = modeCombo.getText().toStdString();
        syncScaleUI();
        repaint();
    };

    // Populate scale
    { int id = 1; for (auto& [name, _] : MusicTheory::scales()) scaleCombo.addItem(name, id++); }
    scaleCombo.setSelectedItemIndex(findComboIndex(scaleCombo, "Chromatic"), juce::dontSendNotification);
    scaleCombo.onChange = [this]() {
        state.activeCategory = "scale";
        state.scaleName = scaleCombo.getText().toStdString();
        syncScaleUI();   // grey out Key + Mode
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
    hZoomSlider.setTooltip("Horizontal zoom - drag to stretch or shrink the timeline. "
                           "Also: Ctrl + mouse wheel.");
    hZoomSlider.onValueChange = [this]() {
        state.hZoom = (float)hZoomSlider.getValue();
        updateScrollBars();
        repaint();
    };

    // Vertical zoom: backs state.visibleRange (semitones-on-screen). The
    // slider value IS a "zoom level" running 1..10, where 10 = most
    // zoomed in (12 semitones / one octave visible) and 1 = most zoomed
    // out (120 semitones, nearly the full MIDI keyboard). Mapping is
    // linear in semitones-visible to keep the drag feel uniform across
    // the range. Slider direction matches hZoom: drag right = zoom in =
    // thicker note lanes.
    addAndMakeVisible(vZoomSlider);
    vZoomSlider.setRange(1.0, 10.0, 0.1);
    vZoomSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    vZoomSlider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    vZoomSlider.setTooltip("Vertical zoom - drag right to thicken the "
                           "note lanes (fewer semitones visible), left "
                           "to thin them (more visible). Also: "
                           "Ctrl+Shift + mouse wheel on the grid.");
    // Helpers: visibleRange <-> slider value. visibleRange in [12, 120];
    // slider in [1, 10] inverted so right = zoomed in.
    auto rangeToSlider = [](int range) {
        return 10.0 - (juce::jlimit(12, 120, range) - 12) / 12.0;
    };
    auto sliderToRange = [](double v) {
        return (int)std::round(12.0 + (10.0 - v) * 12.0);
    };
    vZoomSlider.setValue(rangeToSlider(state.visibleRange),
                         juce::dontSendNotification);
    vZoomSlider.onValueChange = [this, sliderToRange]() {
        int newRange = sliderToRange(vZoomSlider.getValue());
        if (newRange == state.visibleRange) return;
        state.visibleRange = newRange;
        // Re-clamp scrollPitch so the new range still fits inside MIDI
        // 0..127 - without this, zooming out at the top or bottom of the
        // keyboard would clip the visible range against the MIDI ceiling.
        state.scrollPitch = juce::jlimit(newRange / 2,
                                          127 - newRange / 2,
                                          state.scrollPitch);
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
        auto intervals = MusicTheory::activeIntervals(
            state.activeCategory, state.keyName, state.modeName, state.scaleName);
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
                // Map the detected match into the Key+Mode model. A "key" match
                // sets the parent and resets Mode to Ionian (identity rotation);
                // a "mode" match keeps Major as the parent and sets the Mode; a
                // "scale" match switches to the fixed-scale regime.
                if (m.category == "scale") {
                    state.activeCategory = "scale"; state.scaleName = m.scaleName;
                } else if (m.category == "mode") {
                    state.activeCategory = "keymode"; state.keyName = "Major"; state.modeName = m.scaleName;
                } else {
                    state.activeCategory = "keymode"; state.keyName = m.scaleName; state.modeName = "Ionian (Major)";
                }
                syncScaleUI();
                repaint();
            }
        });
    };

    // Reflect the initial scale selection into the dropdowns (selection,
    // enabled state, tooltips) so Key/Mode start correctly greyed if the
    // project opened in a fixed-scale regime.
    syncScaleUI();
}

void PianoRollComponent::syncScaleUI() {
    auto selByText = [](juce::ComboBox& cb, const std::string& name) {
        for (int i = 0; i < cb.getNumItems(); ++i)
            if (cb.getItemText(i).toStdString() == name) {
                cb.setSelectedItemIndex(i, juce::dontSendNotification);
                return;
            }
    };
    rootCombo.setSelectedId(state.keyRoot + 1, juce::dontSendNotification);
    selByText(keyCombo, state.keyName);
    selByText(modeCombo, state.modeName);
    selByText(scaleCombo, state.scaleName);

    // Key + Mode only drive the highlight when NOT in the fixed-scale regime.
    // Grey them out otherwise, and explain why (per the "disabled controls must
    // explain themselves" rule) - choosing a Key or Mode switches back.
    const bool keymode = (state.activeCategory != "scale");
    keyCombo.setEnabled(keymode);
    modeCombo.setEnabled(keymode);
    if (keymode) {
        keyCombo.setTooltip("Key (parent scale) - the 7 notes to use. Major sounds happy/bright; "
                            "the Minor variants sound darker. Works together with Root and Mode.");
        modeCombo.setTooltip("Mode - which note of the Key becomes 'home', rotating the same 7 notes to "
                             "change the mood. Mode 1 'Ionian (Major)' = the Key itself; Dorian, Phrygian, "
                             "Lydian, etc. start on a different degree.");
    } else {
        const juce::String why =
            "Disabled because a fixed Scale (" + juce::String(state.scaleName) + ") is selected. "
            "Pick a Key or Mode here to switch back to the rotational key/mode system.";
        keyCombo.setTooltip(why);
        modeCombo.setTooltip(why);
    }
}

void PianoRollComponent::resized() {
    refreshNode(); if (!node) return;
    auto area = getLocalBounds();
    int rowH = 26;

    // Reserve the resize-handle strip at the very top. The handle is
    // painted directly by paint() and handled by mouseDown - no child
    // component - but the layout has to skip its height so the toolbar
    // sits below it instead of underneath. toolbarHeight() already
    // includes RESIZE_HANDLE_H so every grid offset elsewhere in the
    // file (paint, mouseDown, etc.) accounts for the strip without
    // touching them individually.
    area.removeFromTop(RESIZE_HANDLE_H);

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
        &quantizeBtn,
        &snap14Btn, &snap12Btn, &snap1Btn, &snapOffBtn,
        &snapScaleBtn, &detectKeyBtn};

    if (compactMode) {
        for (auto* b : allToolbarBtns) b->setVisible(false);
        detuneLbl.setVisible(false);
        detuneSlider.setVisible(false);
        quantizeStrSlider.setVisible(false);
        quantizeStrLbl.setVisible(false);
        helpLabel.setVisible(false);
        rootCombo.setVisible(false); rootLbl.setVisible(false);
        keyCombo.setVisible(false); keyLbl.setVisible(false);
        modeCombo.setVisible(false); modeLbl.setVisible(false);
        scaleCombo.setVisible(false); scaleLbl.setVisible(false);
    } else {
        for (auto* b : allToolbarBtns) b->setVisible(true);
        detuneLbl.setVisible(true);
        detuneSlider.setVisible(true);
        quantizeStrSlider.setVisible(true);
        quantizeStrLbl.setVisible(true);
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
        x += 6;
        quantizeStrLbl.setBounds(row1.getX() + x, row1.getY() + 1, 20, rowH - 2);
        x += 22;
        quantizeStrSlider.setBounds(row1.getX() + x, row1.getY() + 1, 80, rowH - 2);
        x += 82;
        place(quantizeBtn, 58);

        // Row 2: snap + Root/Key/Mode/Scale + scale operations
        auto row2 = area.removeFromTop(rowH);
        x = 4;
        auto place2 = [&](juce::Component& c, int w) {
            c.setBounds(row2.getX() + x, row2.getY() + 1, w, rowH - 2);
            x += w + 2;
        };
        // Like place2 but measures the button text to determine width.
        // NB: Font::getStringWidth was deprecated in JUCE 8 and returns 0
        // (silently making every button collapse to just `pad`). Use
        // GlyphArrangement::getStringWidthInt instead.
        auto placeBtn = [&](juce::TextButton& btn, int pad = 16) {
            auto font = btn.getLookAndFeel().getTextButtonFont(btn, rowH - 2);
            int w = juce::GlyphArrangement::getStringWidthInt(font, btn.getButtonText()) + pad;
            place2(btn, w);
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
        x += 8;
        // MPE expression lane selector sits just left of the automation
        // selector; it's only shown when the node has MPE enabled (see
        // visibility block below).
        exprLaneCombo.setBounds(row2.getX() + x, row2.getY() + 1, 120, rowH - 2);
        if (node && node->mpeEnabled) x += 124;
        autoParamCombo.setBounds(row2.getX() + x, row2.getY() + 1, 120, rowH - 2);
    }

    // Visibility
    if (compactMode) {
        exprLaneCombo.setVisible(false);
        autoParamCombo.setVisible(false);
    } else {
        exprLaneCombo.setVisible(node && node->mpeEnabled);
        autoParamCombo.setVisible(node && !node->params.empty());
    }

    // Scrollbars - bottom and right edges of the piano roll area
    auto pianoArea = getLocalBounds();
    pianoArea.removeFromTop(toolbarHeight());
    vScrollBar.setBounds(pianoArea.removeFromRight(SCROLLBAR_SIZE));
    auto bottomBar = pianoArea.removeFromBottom(SCROLLBAR_SIZE);
    // Bottom bar holds (from right) the vertical-zoom slider, the
    // horizontal-zoom slider, and the horizontal scrollbar in the
    // remaining left space. Each zoom slider gets ~25% of the bar with a
    // 110-px floor so they stay grabbable on narrow editor panels.
    int vZoomW = std::max(110, (int)(bottomBar.getWidth() * 0.22f));
    int hZoomW = std::max(110, (int)(bottomBar.getWidth() * 0.22f));
    vZoomSlider.setBounds(bottomBar.removeFromRight(vZoomW));
    hZoomSlider.setBounds(bottomBar.removeFromRight(hZoomW));
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

    // Draw toolbar background (includes the resize handle area at the
    // very top; the handle strip is overpainted with its own colour and
    // grip dots immediately after).
    g.setColour(juce::Colour(35, 35, 40));
    g.fillRect(0, 0, getWidth(), toolbarHeight());

    // Resize handle strip - thin grip at the very top of the panel.
    // Click+drag here resizes this panel's height; panels above just
    // shift position. Drawn slightly darker than the toolbar so the
    // boundary is obvious, with three centred grip dots as the affordance.
    {
        juce::Rectangle<int> hb(0, 0, getWidth(), RESIZE_HANDLE_H);
        g.setColour(juce::Colour(22, 22, 28));
        g.fillRect(hb);
        g.setColour(juce::Colour(95, 95, 115));
        int cx = getWidth() / 2;
        int cy = RESIZE_HANDLE_H / 2;
        for (int i = -1; i <= 1; ++i)
            g.fillRect(cx + i * 10 - 1, cy - 1, 2, 2);
        // Subtle bottom hairline to separate the handle from the toolbar.
        g.setColour(juce::Colour(0, 0, 0).withAlpha(0.4f));
        g.drawHorizontalLine(RESIZE_HANDLE_H - 1, 0.0f, (float)getWidth());
    }

    // Effect Regions lane, then the per-track header strip (track name +
    // parent/offset, drag to retime). Both sit inside toolbarHeight(), the lane
    // directly above the header so the header keeps its existing adjacency to
    // the note grid.
    paintFxLane(g);
    paintTrackHeader(g);

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

    // Clip everything to the grid rectangle. Without this, a note one row above
    // the top visible pitch draws at negative y and bleeds over the toolbar's
    // bottom edge, looking like it sits "on the top line" even though it's above
    // the highest interactive row - which is exactly why such notes can't be
    // clicked or marquee-selected. Clipping keeps off-grid notes off-screen.
    g.reduceClipRegion(0, 0, getWidth(), (int)area.getHeight());

    int visRange = state.visibleRange;
    int pitchHi = state.scrollPitch + visRange / 2;
    float rowH = gridH / std::max(visRange, 1);
    float totalBeats = graph.getTimelineBeats(*node);
    float absOffset = node->absoluteBeatOffset; // cascading parent offset
    float absTotalBeats = totalBeats + absOffset;

    // Horizontal zoom/scroll - guard against zero visible beats
    float visibleBeats = std::max(1.0f, absTotalBeats / std::max(state.hZoom, 0.1f));
    float scrollBeat = juce::jlimit(0.0f, std::max(0.0f, absTotalBeats - visibleBeats), state.hScroll);
    state.hScroll = scrollBeat;

    auto beatToX = [&](float b) { return gridX + ((b + absOffset - scrollBeat) / visibleBeats) * gridW; };
    auto pitchToY = [&](int p) { return (pitchHi - p) * rowH; };

    // Build scale highlight set
    auto intervals = MusicTheory::activeIntervals(
        state.activeCategory, state.keyName, state.modeName, state.scaleName);
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
        }
    }

    // Note-name labels (second pass). Drawn AFTER all row backgrounds /
    // key fills so a label is never overdrawn by a later row fill. Goal:
    // EVERY row carries its own note name so the user can read which note
    // each row is, not just the C-of-octave anchors. Three regimes:
    //  - "Per-row" (rowH >= 4.0): label every key. The font is shrunk to
    //    fit the row (floored at 6.5 px - the smallest size that still
    //    rasterises legibly here). At the default zoom/panel height rows are
    //    ~7.5 px so each name sits cleanly on its own row; even a manually
    //    shortened panel (~4-5 px rows) still shows a name on every row.
    //  - "Sparse" (rowH < 4.0): rows are now too short to fit even a 6.5 px
    //    glyph without illegible overlap, so fall back to one C-per-octave
    //    anchor label (using up to a full octave of vertical space) so
    //    C3/C4/C5/... stay readable at extreme vertical zoom-out.
    {
        // Per-row threshold of 4.0 px: below this even a 6.5 px font's
        // glyphs would stack on top of each other into mush, so we drop to
        // the C-per-octave fallback. At/above it we label every row.
        const bool perRow = rowH >= 4.0f;
        // In per-row mode the label rect is exactly one row tall and the
        // font is shrunk to fit (floored at 6.5 so glyphs still rasterise);
        // a slight font>rowH overflow is fine because the text is centred
        // and only thin glyph extremes clip. In sparse mode the anchor
        // label spans up to ~12 rows so the C name stays large and legible.
        float labelH = perRow ? rowH : juce::jmin(rowH * 12.0f, 16.0f);
        float fontSize = perRow ? juce::jlimit(6.5f, 12.0f, rowH * 1.05f)
                                : juce::jlimit(7.0f, 14.0f, labelH * 0.85f);
        // JUCE 8: the deprecated Font(float) constructor renders as a
        // zero-glyph font in this codebase (same root cause as
        // Font::getStringWidth returning 0 - see notes in
        // layered_wave_editor.cpp's intrinsicComboWidth helper). Going
        // through FontOptions is the supported path and produces actual
        // glyphs.
        g.setFont(juce::Font(juce::FontOptions(fontSize)));
        // Per-row: centre the name in its row. Sparse: top-align so each C
        // label sits at its anchor row rather than sliding down an octave.
        auto just = perRow ? juce::Justification::centredLeft
                           : juce::Justification::topLeft;

        for (int i = 0; i < visRange; ++i) {
            int pitch = pitchHi - i;
            if (pitch < 0 || pitch > 127) continue;
            const bool isOctaveAnchor = (pitch % 12 == 0); // C
            if (!perRow && !isOctaveAnchor) continue;
            if (rowH <= 2.5f) continue;

            float y = i * rowH;
            bool inScale = isChromatic || scaleNotes.count(pitch % 12) > 0;
            // Out-of-scale notes used to render at (60,60,60) on a
            // (25,25,28) key fill - contrast ratio ~1.3:1, basically
            // invisible. Bumped to (120,120,120) so the labels are
            // legible while still being dimmer than in-scale (170) and
            // root (220) via the row-background contrast.
            juce::Colour textCol = (!inScale && !isChromatic) ? juce::Colour(120, 120, 120)
                : isOctaveAnchor ? juce::Colour(220, 220, 220)
                : juce::Colour(170, 170, 170);
            g.setColour(textCol);
            g.drawText(MusicTheory::noteName(pitch),
                       juce::Rectangle<float>(3, y, KEY_WIDTH - 5, labelH),
                       just, false);
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
                g.setFont(juce::Font(juce::FontOptions(10.0f)));
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
                juce::Colour base(cr, cg2, cb);
                // When this timeline emits MPE, tint notes by their recorded
                // mean pressure toward a hot orange so heavily-pressed notes
                // stand out at a glance.
                if (node->mpeEnabled && !note.expression.pressure.empty()) {
                    float sum = 0.0f;
                    for (auto& pt : note.expression.pressure) sum += pt.value;
                    float meanP = sum / (float)note.expression.pressure.size();
                    base = base.interpolatedWith(juce::Colour(255, 90, 40),
                                                 juce::jlimit(0.0f, 0.85f, meanP));
                }
                g.setColour(base.withAlpha(0.85f));
                g.fillRoundedRectangle(nx1 + 1, ny + 1, nx2 - nx1 - 2, rowH - 2, 2);
                g.setColour(base.brighter(0.2f));
                g.drawRoundedRectangle(nx1 + 1, ny + 1, nx2 - nx1 - 2, rowH - 2, 2, 1);
            }

            // Note label
            float noteW = nx2 - nx1;
            if (noteW > 14 && rowH > 7) {
                auto name = MusicTheory::noteName(note.pitch);
                float fontSize = juce::jlimit(8.0f, 14.0f, rowH * 0.75f);
                // JUCE 8: route through FontOptions so Graphics::drawText
                // actually rasterises glyphs - the deprecated Font(float)
                // produces empty output in 8.0.12.
                g.setFont(juce::Font(juce::FontOptions(fontSize)));
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

    // Loop region: drawn later (after the notes, just before the song-end
    // handle) as an explicit A-B loop overlay - a top loop-bar with inward
    // bracket end-caps, dashed boundary lines, and a "LOOP" label - rather
    // than a flat full-height wash. The wash read as a brighter "more active"
    // stretch of grid and got mistaken for a third beat-activation level; the
    // bracketed bar reads unmistakably as a loop. See REFERENCE.md
    // "Grid regions and song length".

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

    // Time-gated effect layers used to be drawn here, as flat bars flush with
    // the top of the note grid. They now live in their own always-visible
    // "Effect Regions" lane above the track-header strip (paintFxLane) - which
    // makes the feature discoverable, gives it room to be edited by dragging,
    // and stops the bars overpainting marker flags/labels, which are drawn at
    // y = 0..12 in this same pass.

    // Playback cursor (absolute beat, no double-offset)
    if (transport && transport->playing) {
        float cursorBeat = (float)transport->positionBeats();
        float cx = gridX + ((cursorBeat - scrollBeat) / visibleBeats) * gridW;
        if (cx >= gridX && cx <= gridX + gridW) {
            g.setColour(juce::Colours::white);
            g.drawVerticalLine((int)cx, 0, gridH);
        }
    }

    // Expression / automation lane
    if (exprLane != ExprNone && (exprLane == ExprAutomation || node->mpeEnabled)) {
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
                g.setFont(juce::Font(juce::FontOptions(11.0f).withStyle("Bold")));
                g.drawText(laneLabel, gridX + 6, (int)exprY + 2, 200, 14,
                           juce::Justification::centredLeft, false);
                g.setColour(juce::Colours::grey.withAlpha(0.5f));
                g.setFont(juce::Font(juce::FontOptions(9.0f)));
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

                {
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

    // Loop region overlay (drawn on top of the notes so it's always visible,
    // matching the END handle). Drawn as a thin outline that frames the looped
    // span - NOT a filled wash or solid bar, both of which hid the notes inside
    // the region and read as a brightness "level". The two vertical edge lines
    // are the drag handles (grab to move loopStart / loopEnd); thin top & bottom
    // hairlines tie them into a frame, and a small "LOOP" tag marks the start.
    if (transport && transport->loopEnabled &&
        transport->loopEndBeat > transport->loopStartBeat) {
        float lxFull1 = beatToX((float)transport->loopStartBeat - node->absoluteBeatOffset);
        float lxFull2 = beatToX((float)transport->loopEndBeat   - node->absoluteBeatOffset);
        float lx1 = std::max(lxFull1, gridX);
        float lx2 = std::min(lxFull2, gridX + gridW);
        if (lx2 > lx1) {
            juce::Colour loopCol(90, 140, 255);
            bool edge1On = (lxFull1 >= gridX && lxFull1 <= gridX + gridW);
            bool edge2On = (lxFull2 >= gridX && lxFull2 <= gridX + gridW);
            bool draggingThis = (dragMode == DragLoopStart || dragMode == DragLoopEnd);

            // Thin top & bottom hairlines frame the span without covering cells.
            g.setColour(loopCol.withAlpha(0.55f));
            g.drawHorizontalLine(0, lx1, lx2);
            g.drawHorizontalLine((int)gridH - 1, lx1, lx2);

            // Vertical boundary lines = the drag handles. A touch brighter and
            // 2px so they're easy to grab; the active edge brightens further.
            auto drawEdge = [&](float x, bool active) {
                g.setColour(loopCol.withAlpha(active ? 1.0f : 0.85f));
                g.fillRect(x - 1.0f, 0.0f, 2.0f, gridH);
                // small inward ticks at top so it reads as a loop boundary
                g.fillRect(x - 1.0f, 0.0f, 6.0f, 2.0f);
                g.fillRect(x - 1.0f, gridH - 2.0f, 6.0f, 2.0f);
            };
            if (edge1On) drawEdge(lxFull1, draggingThis && dragMode == DragLoopStart);
            if (edge2On) drawEdge(lxFull2, draggingThis && dragMode == DragLoopEnd);

            // "LOOP" tag at the start edge (clamped on-screen), small and high
            // so it doesn't sit over the note rows people actually read.
            g.setColour(loopCol.brighter(0.4f));
            g.setFont(juce::Font(juce::FontOptions(9.0f)));
            g.drawText("LOOP", (int)lx1 + 4, 1, 46, 10,
                       juce::Justification::centredLeft);
        }
    }

    // Song-end resize handle. A draggable orange border at the end of this
    // timeline's content. Drag left to shorten the song (trims trailing notes),
    // right to add empty beats. Drawn on top of the notes so it's always
    // grabbable, with a grip tab + label at the top for discoverability.
    if (!node->clips.empty()) {
        int endClip = -1;
        float endBeat = songEndBeatLocal(&endClip);
        float ex = beatToX(endBeat);
        if (ex >= gridX - 4 && ex <= gridX + gridW + 12) {
            bool active = (dragMode == DragSongEnd);
            auto col = juce::Colours::orange.withAlpha(active ? 0.95f : 0.6f);
            g.setColour(col);
            g.fillRect(ex - 1.5f, 0.0f, 3.0f, gridH);
            // Grip tab at the top edge of the grid.
            juce::Rectangle<float> tab(ex - 5.0f, 0.0f, 10.0f, 20.0f);
            g.fillRoundedRectangle(tab, 2.0f);
            g.setColour(juce::Colours::black.withAlpha(0.7f));
            g.drawText("END", (int)ex + 4, 2, 40, 14,
                       juce::Justification::centredLeft);
        }
    }

    // Paste ghost preview. When the clipboard has notes and the cursor is over
    // the grid, draw a translucent outline of where Ctrl+V would drop the block,
    // anchored top-left to the hovered cell - exactly the placement pasteAtCursor
    // computes. Gives the persistent, visible paste target the marquee can't.
    if (!clipboard.empty() && hoverValid && dragMode == DragNone) {
        float snap = state.snap > 0 ? state.snap : 0.25f;
        float pasteBeat = std::round(hoverBeat / snap) * snap;
        g.setColour(juce::Colours::aqua.withAlpha(0.35f));
        for (auto& cn : clipboard) {
            float nb = pasteBeat + cn.offsetFromFirst;        // node-local beat
            int   np = juce::jlimit(0, 127, hoverPitch - cn.pitchBelowTop);
            float gx1 = beatToX(nb);
            float gx2 = beatToX(nb + cn.duration);
            float gy = pitchToY(np);
            if (gx2 < gridX || gx1 > gridX + gridW || gy + rowH < 0 || gy > gridH)
                continue;
            g.fillRoundedRectangle(gx1 + 1, gy + 1, gx2 - gx1 - 2, rowH - 2, 2);
        }
        g.setColour(juce::Colours::aqua.withAlpha(0.8f));
        for (auto& cn : clipboard) {
            float nb = pasteBeat + cn.offsetFromFirst;
            int   np = juce::jlimit(0, 127, hoverPitch - cn.pitchBelowTop);
            float gx1 = beatToX(nb);
            float gx2 = beatToX(nb + cn.duration);
            float gy = pitchToY(np);
            if (gx2 < gridX || gx1 > gridX + gridW || gy + rowH < 0 || gy > gridH)
                continue;
            g.drawRoundedRectangle(gx1 + 1, gy + 1, gx2 - gx1 - 2, rowH - 2, 2, 1);
        }
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

float PianoRollComponent::songEndBeatLocal(int* clipIdxOut) const {
    float end = 0;
    int idx = -1;
    for (int i = 0; i < (int)node->clips.size(); ++i) {
        float e = node->clips[i].startBeat + node->clips[i].lengthBeats;
        if (e > end) { end = e; idx = i; }
    }
    if (clipIdxOut) *clipIdxOut = idx;
    return end;
}

float PianoRollComponent::beatToScreenX(float beatLocal) const {
    float gridX = KEY_WIDTH;
    float gridW = getWidth() - KEY_WIDTH - SCROLLBAR_SIZE;
    float totalBeats = graph.getTimelineBeats(*node);
    float absOffset = node->absoluteBeatOffset;
    float absTotalBeats = totalBeats + absOffset;
    float visibleBeats = absTotalBeats / std::max(state.hZoom, 0.1f);
    return gridX + ((beatLocal + absOffset - state.hScroll) / visibleBeats) * gridW;
}

float PianoRollComponent::panelXToAbsBeat(float x, float capturedVisBeats, float capturedScroll) const {
    float gridX = KEY_WIDTH;
    float gridW = std::max(1.0f, (float)getWidth() - KEY_WIDTH - SCROLLBAR_SIZE);
    float visibleBeats, scrollBeat;
    if (capturedVisBeats > 0.0f) {
        visibleBeats = capturedVisBeats;
        scrollBeat = capturedScroll;
    } else {
        float absTotalBeats = graph.getTimelineBeats(*node) + node->absoluteBeatOffset;
        visibleBeats = std::max(1.0f, absTotalBeats / std::max(state.hZoom, 0.1f));
        scrollBeat = state.hScroll;
    }
    return scrollBeat + ((x - gridX) / gridW) * visibleBeats;
}

bool PianoRollComponent::isInTrackHeader(juce::Point<float> pos) const {
    return pos.y >= (float)trackHeaderTop() && pos.y < (float)toolbarHeight()
        && pos.x >= KEY_WIDTH && pos.x < (float)getWidth() - SCROLLBAR_SIZE;
}

// ===========================================================================
// Effect Regions lane
//
// A band above the "Start" track-header strip listing this track's time-gated
// effect layers as stacked shaded tubes. See the block comment on
// FX_ROW_H_COLLAPSED in piano_roll_component.h for the design rationale.
// ===========================================================================

int PianoRollComponent::fxRowOf(const EffectRegion& r) const {
    // First-appearance order over distinct (linkId, groupId) pairs. Two regions
    // gating the same wire therefore share a row - they're the same layer, just
    // active at different times - and can never overlap vertically.
    if (!node) return 0;
    int row = 0;
    for (const auto& other : node->effectRegions) {
        if (&other == &r) return row;
        bool seenBefore = false;
        for (const auto& earlier : node->effectRegions) {
            if (&earlier == &other) break;
            if (earlier.linkId == other.linkId && earlier.groupId == other.groupId) {
                seenBefore = true;
                break;
            }
        }
        if (!seenBefore) {
            if (other.linkId == r.linkId && other.groupId == r.groupId) return row;
            ++row;
        }
    }
    return row;
}

int PianoRollComponent::fxRowCount() const {
    if (!node) return 1;
    int distinct = 0;
    for (size_t i = 0; i < node->effectRegions.size(); ++i) {
        bool seenBefore = false;
        for (size_t j = 0; j < i; ++j)
            if (node->effectRegions[j].linkId  == node->effectRegions[i].linkId
             && node->effectRegions[j].groupId == node->effectRegions[i].groupId) {
                seenBefore = true;
                break;
            }
        if (!seenBefore) ++distinct;
    }
    // Never zero: an empty lane is the discoverability affordance, and while
    // expanded there's always one spare row to right-click into to add a layer.
    return std::max(1, distinct);
}

int PianoRollComponent::fxLaneHeight() const {
    int rows = fxRowCount();
    if (fxLaneExpanded) rows += 1;      // spare row: right-click here to add
    return rows * fxRowHeight() + FX_LANE_PAD * 2;
}

void PianoRollComponent::setFxLaneExpanded(bool shouldExpand) {
    if (fxLaneExpanded == shouldExpand) return;
    fxLaneExpanded = shouldExpand;
    // The lane's height is part of toolbarHeight(), so the whole grid below it
    // shifts. Re-lay-out (scrollbars are positioned from toolbarHeight()) and
    // repaint the lot.
    resized();
    repaint();
}

bool PianoRollComponent::isInFxLane(juce::Point<float> pos) const {
    return pos.y >= (float)fxLaneTop()
        && pos.y < (float)(fxLaneTop() + fxLaneHeight())
        && pos.x < (float)getWidth() - SCROLLBAR_SIZE;
}

float PianoRollComponent::fxBeatToX(float localBeat) const {
    if (!node) return KEY_WIDTH;
    float gridX = KEY_WIDTH;
    float gridW = std::max(1.0f, (float)getWidth() - KEY_WIDTH - SCROLLBAR_SIZE);
    float absOffset = node->absoluteBeatOffset;
    float absTotalBeats = graph.getTimelineBeats(*node) + absOffset;
    float visibleBeats = std::max(1.0f, absTotalBeats / std::max(state.hZoom, 0.1f));
    float scrollBeat = juce::jlimit(0.0f, std::max(0.0f, absTotalBeats - visibleBeats),
                                    state.hScroll);
    // `+ absOffset` is what makes the lane slide with the track: regions are
    // stored in node-local beats, exactly like clips and notes.
    return gridX + ((localBeat + absOffset - scrollBeat) / visibleBeats) * gridW;
}

float PianoRollComponent::fxXToBeat(float x) const {
    if (!node) return 0.0f;
    float gridX = KEY_WIDTH;
    float gridW = std::max(1.0f, (float)getWidth() - KEY_WIDTH - SCROLLBAR_SIZE);
    float absOffset = node->absoluteBeatOffset;
    float absTotalBeats = graph.getTimelineBeats(*node) + absOffset;
    float visibleBeats = std::max(1.0f, absTotalBeats / std::max(state.hZoom, 0.1f));
    float scrollBeat = juce::jlimit(0.0f, std::max(0.0f, absTotalBeats - visibleBeats),
                                    state.hScroll);
    return scrollBeat + ((x - gridX) / gridW) * visibleBeats - absOffset;
}

float PianoRollComponent::fxBeatsPerPixel() const {
    if (!node) return 1.0f;
    float gridW = std::max(1.0f, (float)getWidth() - KEY_WIDTH - SCROLLBAR_SIZE);
    float absTotalBeats = graph.getTimelineBeats(*node) + node->absoluteBeatOffset;
    float visibleBeats = std::max(1.0f, absTotalBeats / std::max(state.hZoom, 0.1f));
    return visibleBeats / gridW;
}

float PianoRollComponent::fxMagneticSnap(float localBeat, int dir, bool* snapped) const {
    if (snapped) *snapped = false;
    if (!node) return localBeat;
    // Snap in ABSOLUTE beats: that's where the vertical grid lines the user is
    // aiming at are drawn (see the beat-grid loop in paint()). A track whose
    // offset isn't a whole grid step would otherwise snap to positions with no
    // line under them.
    const float absOffset = node->absoluteBeatOffset;
    const float pull = FX_SNAP_PULL_PX * fxBeatsPerPixel();
    return SoundShop::magneticSnapBeat(localBeat + absOffset, dir, state.snap, pull, snapped)
           - absOffset;
}

// Resolve a region's display colour: explicit colour, else its group's, else a
// palette entry keyed on the link id, else grey.
static juce::Colour fxRegionColour(const NodeGraph& graph, const EffectRegion& r) {
    uint32_t col = r.color;
    if (col == 0 && r.groupId >= 0)
        for (const auto& grp : graph.effectGroups)
            if (grp.id == r.groupId) { col = grp.color; break; }
    if (col == 0 && r.linkId >= 0) col = getDistinctColor(r.linkId);
    if (col == 0) col = 0xFF808080;
    return juce::Colour((uint8_t)((col >> 16) & 0xFF),
                        (uint8_t)((col >> 8) & 0xFF),
                        (uint8_t)(col & 0xFF));
}

// Human-readable name for what a region gates: the group name, else the
// destination node of the wire, else a fallback.
static juce::String fxRegionLabel(NodeGraph& graph, const EffectRegion& r) {
    if (r.groupId >= 0) {
        if (auto* grp = graph.findEffectGroup(r.groupId))
            return grp->name.empty() ? ("Group #" + juce::String(grp->id))
                                     : juce::String(grp->name);
        return "(missing group)";
    }
    if (r.linkId >= 0) {
        for (auto& link : graph.links)
            if (link.id == r.linkId)
                for (auto& n : graph.nodes)
                    for (auto& pin : n.pinsIn)
                        if (pin.id == link.endPin) return juce::String(n.name);
        return "(missing wire)";
    }
    return "(unassigned)";
}

int PianoRollComponent::fxRegionAt(juce::Point<float> pos, int* edgeOut) const {
    if (edgeOut) *edgeOut = 0;
    if (!node || !isInFxLane(pos)) return -1;
    const float rowH = (float)fxRowHeight();
    const float top = (float)fxLaneTop() + FX_LANE_PAD;
    // Iterate backwards so the topmost-drawn region wins an overlap.
    for (int i = (int)node->effectRegions.size() - 1; i >= 0; --i) {
        const auto& r = node->effectRegions[(size_t)i];
        float y = top + (float)fxRowOf(r) * rowH;
        if (pos.y < y || pos.y >= y + rowH) continue;
        float x1 = fxBeatToX(r.startBeat), x2 = fxBeatToX(r.endBeat);
        if (pos.x < x1 - FX_EDGE_GRAB || pos.x > x2 + FX_EDGE_GRAB) continue;
        if (edgeOut) {
            // Edge zones are only offered while expanded - at 7 px tall a
            // collapsed bar is too small to aim at, and the lane is read-only
            // in that state anyway.
            if (!fxLaneExpanded)                    *edgeOut = 0;
            else if (pos.x <= x1 + FX_EDGE_GRAB)    *edgeOut = -1;
            else if (pos.x >= x2 - FX_EDGE_GRAB)    *edgeOut = +1;
            else                                    *edgeOut = 0;
        }
        return i;
    }
    return -1;
}

void PianoRollComponent::paintFxLane(juce::Graphics& g) {
    if (!node) return;
    const int top = fxLaneTop();
    const int h = fxLaneHeight();
    const float gridX = KEY_WIDTH;
    const float gridW = std::max(1.0f, (float)getWidth() - KEY_WIDTH - SCROLLBAR_SIZE);
    const float rowH = (float)fxRowHeight();

    // Band background - a touch lighter when expanded so "I am in edit mode" is
    // visible at a glance, not just inferable from the extra height.
    g.setColour(fxLaneExpanded ? juce::Colour(34, 32, 44) : juce::Colour(23, 23, 30));
    g.fillRect(0, top, getWidth(), h);

    // Left-gutter caption over the keyboard column, mirroring "Start" below.
    // The chevron is the collapse affordance and the whole gutter cell is its
    // hit target (see mouseDown).
    // Non-ASCII must go through fromUTF8: a bare narrow literal is decoded as
    // Latin-1 here, which renders the chevron as three mojibake glyphs and eats
    // the gutter width. Chevron and word are drawn as separate cells because
    // "> Effects" as one string does not fit in KEY_WIDTH and JUCE silently
    // ellipsises it to "Eff...".
    g.setColour(fxLaneExpanded ? juce::Colour(190, 180, 220) : juce::Colour(120, 120, 140));
    g.setFont(juce::Font(9.0f));
    g.drawText(juce::String::fromUTF8(fxLaneExpanded ? "\xe2\x96\xbe" : "\xe2\x96\xb8"),
               1, top, 8, h, juce::Justification::centredLeft);
    g.drawText("Effects", 9, top, (int)gridX - 11, h, juce::Justification::centredRight);

    // Row guides while expanded, so empty rows read as droppable slots rather
    // than dead space.
    if (fxLaneExpanded) {
        g.setColour(juce::Colours::white.withAlpha(0.05f));
        for (int rowIdx = 0; rowIdx < fxRowCount() + 1; ++rowIdx) {
            float y = (float)(top + FX_LANE_PAD) + (float)rowIdx * rowH;
            g.drawHorizontalLine((int)(y + rowH - 1), gridX, gridX + gridW);
        }
    }

    for (const auto& region : node->effectRegions) {
        float x1 = fxBeatToX(region.startBeat);
        float x2 = fxBeatToX(region.endBeat);
        if (x2 < gridX || x1 > gridX + gridW) continue;
        const bool clipL = x1 < gridX, clipR = x2 > gridX + gridW;
        x1 = std::max(x1, gridX);
        x2 = std::min(x2, gridX + gridW);
        if (x2 - x1 < 1.5f) x2 = x1 + 1.5f;

        // Collapsed rows are flush - the tubes stack directly on top of each
        // other so the ribbon reads as one solid bundle of cables, which is the
        // whole job of the collapsed state. Expanded keeps a 1 px inset per row
        // so each tube reads as a separate object you can grab and drag.
        const float inset = fxLaneExpanded ? 1.0f : 0.0f;
        float y = (float)(top + FX_LANE_PAD) + (float)fxRowOf(region) * rowH;
        juce::Rectangle<float> bar(x1, y + inset, x2 - x1, rowH - inset * 2.0f);
        auto col = fxRegionColour(graph, region);

        // Tube shading: a vertical three-stop ramp (dark rim -> bright specular
        // band in the upper third -> dark rim) reads as a cylinder lying on its
        // side, which distinguishes a layer bar from the flat rectangles used
        // everywhere else in the piano roll.
        juce::ColourGradient tube(col.darker(0.75f), 0.0f, bar.getY(),
                                  col.darker(0.55f), 0.0f, bar.getBottom(), false);
        tube.addColour(0.32, col.brighter(0.55f));
        tube.addColour(0.55, col);
        g.setGradientFill(tube);
        const float radius = std::min(bar.getHeight() * 0.5f, 5.0f);
        g.fillRoundedRectangle(bar, radius);

        // Rim light along the top edge completes the cylinder read.
        g.setColour(juce::Colours::white.withAlpha(0.30f));
        g.drawLine(bar.getX() + radius, bar.getY() + 0.75f,
                   bar.getRight() - radius, bar.getY() + 0.75f, 1.0f);

        // End caps. A clipped end gets a flat bright edge instead, signalling
        // "this continues off-screen" rather than "it ends here".
        g.setColour(col.brighter(0.7f).withAlpha(clipL ? 0.9f : 0.75f));
        g.fillRect(bar.getX(), bar.getY(), clipL ? 2.0f : 1.5f, bar.getHeight());
        g.setColour(col.brighter(0.7f).withAlpha(clipR ? 0.9f : 0.75f));
        g.fillRect(bar.getRight() - (clipR ? 2.0f : 1.5f), bar.getY(),
                   clipR ? 2.0f : 1.5f, bar.getHeight());

        // Name only when expanded - a 7 px collapsed tube has no room, and the
        // colour plus the gutter caption already say what the ribbon is.
        if (fxLaneExpanded && bar.getWidth() > 34.0f) {
            g.setColour(juce::Colours::white.withAlpha(0.95f));
            g.setFont(juce::Font(std::min(10.0f, rowH - 6.0f)));
            g.drawText(fxRegionLabel(graph, region),
                       (int)bar.getX() + 5, (int)bar.getY(),
                       (int)bar.getWidth() - 10, (int)bar.getHeight(),
                       juce::Justification::centredLeft, false);
        }
    }

    // Empty state. This is the entire reason the lane is always visible: with
    // no layers there is otherwise nothing anywhere in the UI to tell you the
    // feature exists.
    if (node->effectRegions.empty()) {
        g.setColour(juce::Colour(110, 105, 130));
        g.setFont(juce::Font(std::min(10.0f, rowH - 1.0f)));
        g.drawText(fxLaneExpanded
                       ? juce::String("right-click to add an effect layer")
                       : juce::String::fromUTF8("no effect layers \xe2\x80\x94 click to add one"),
                   (int)gridX + 6, top, (int)gridW - 12, h,
                   juce::Justification::centredLeft, false);
    }

    // Bottom hairline separating the lane from the track-header strip.
    g.setColour(juce::Colour(0, 0, 0).withAlpha(0.5f));
    g.drawHorizontalLine(top + h - 1, 0.0f, (float)getWidth());
}

// Scrollable "which wires are in this group?" checklist for the Effects lane's
// "New group of wires..." dialog. Each row carries the wire's palette colour as
// a solid swatch, matching the tube the group will draw and the tag on the wire
// in the graph - picking wires by colour is how the user navigates a graph with
// more cables than distinct names.
namespace {

class WireCheckRow : public juce::Component {
public:
    WireCheckRow(int linkId_, const juce::String& label, juce::Colour c)
        : linkId(linkId_), colour(c) {
        button.setButtonText(label);
        button.onClick = [this] { if (onToggle) onToggle(); };
        addAndMakeVisible(button);
    }
    std::function<void()> onToggle;
    void resized() override { button.setBounds(18, 0, getWidth() - 18, getHeight()); }
    void paint(juce::Graphics& g) override {
        g.setColour(colour);
        g.fillRoundedRectangle(2.0f, (float)getHeight() * 0.5f - 5.0f, 11.0f, 10.0f, 2.0f);
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.drawRoundedRectangle(2.0f, (float)getHeight() * 0.5f - 5.0f, 11.0f, 10.0f, 2.0f, 1.0f);
    }
    bool isChecked() const { return button.getToggleState(); }
    int  getLinkId() const { return linkId; }

private:
    int linkId;
    juce::Colour colour;
    juce::ToggleButton button;
};

// Contents of the "New Group of Wires" dialog. A DialogWindow rather than an
// AlertWindow: AlertWindow puts WS_EX_APPWINDOW on its peer with no owner, so it
// earns a second SEANCE button on the Windows taskbar (see CLAUDE.md's dialog
// rule) - launchToolDialog() is the project's fix for that.
class NewGroupContent : public juce::Component {
public:
    using Callback = std::function<void(std::vector<int>, juce::String)>;

    NewGroupContent(const std::vector<std::pair<int, juce::String>>& wires,
                    float beat, Callback cb)
        : onCreate(std::move(cb)) {
        blurb.setText("Tick every wire that should switch on and off together. "
                      "A layer for the group is added at beat "
                          + juce::String(beat, 2) + " of this track.",
                      juce::dontSendNotification);
        blurb.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(blurb);

        for (const auto& [linkId, label] : wires) {
            auto row = std::make_unique<WireCheckRow>(linkId, label,
                                                      juce::Colour(getDistinctColor(linkId)));
            list.addAndMakeVisible(row.get());
            rows.push_back(std::move(row));
        }
        viewport.setViewedComponent(&list, false);
        viewport.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport);

        nameLabel.setText("Group name:", juce::dontSendNotification);
        addAndMakeVisible(nameLabel);
        nameEditor.setTextToShowWhenEmpty("optional", juce::Colours::grey);
        addAndMakeVisible(nameEditor);

        // Nothing ticked means nothing to group, so the button stays off until
        // it would actually do something - with a tooltip saying why, per the
        // "grayed-out controls must explain themselves" rule.
        createBtn.setEnabled(false);
        createBtn.setTooltip("Tick at least one wire first.");
        createBtn.onClick = [this] {
            std::vector<int> picked;
            for (const auto& r : rows)
                if (r->isChecked()) picked.push_back(r->getLinkId());
            auto name = nameEditor.getText();
            close();
            if (onCreate) onCreate(std::move(picked), name);
        };
        cancelBtn.onClick = [this] { close(); };
        addAndMakeVisible(createBtn);
        addAndMakeVisible(cancelBtn);

        for (const auto& r : rows)
            r->onToggle = [this] {
                bool any = false;
                for (const auto& x : rows) any = any || x->isChecked();
                createBtn.setEnabled(any);
                createBtn.setTooltip(any ? juce::String()
                                         : juce::String("Tick at least one wire first."));
            };

        setSize(420, juce::jlimit(160, 420, 150 + (int)rows.size() * rowH));
    }

    void resized() override {
        auto r = getLocalBounds().reduced(10);
        blurb.setBounds(r.removeFromTop(44));
        auto buttons = r.removeFromBottom(30);
        cancelBtn.setBounds(buttons.removeFromRight(90));
        buttons.removeFromRight(8);
        createBtn.setBounds(buttons.removeFromRight(90));
        r.removeFromBottom(8);
        auto nameRow = r.removeFromBottom(26);
        nameLabel.setBounds(nameRow.removeFromLeft(90));
        nameEditor.setBounds(nameRow);
        r.removeFromBottom(8);
        viewport.setBounds(r);
        list.setSize(std::max(0, r.getWidth() - 12), (int)rows.size() * rowH);
        for (int i = 0; i < (int)rows.size(); ++i)
            rows[(size_t)i]->setBounds(0, i * rowH, list.getWidth(), rowH);
    }

private:
    void close() {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState(0);
    }

    static constexpr int rowH = 22;
    Callback onCreate;
    juce::Label blurb, nameLabel;
    juce::TextEditor nameEditor;
    juce::Viewport viewport;
    juce::Component list;
    std::vector<std::unique_ptr<WireCheckRow>> rows;
    juce::TextButton createBtn{"Create"}, cancelBtn{"Cancel"};
};

} // namespace

void PianoRollComponent::showFxLaneMenu(juce::Point<float> pos) {
    if (!node) return;
    const int myId = node->id;
    // Snap the click beat the same way note placement does, so a layer created
    // here lines up with the grid the user can see.
    const float snap = state.snap > 0 ? state.snap : 1.0f;
    const float clickBeat = std::floor(fxXToBeat(pos.x) / snap) * snap;

    int edge = 0;
    const int hitIdx = fxRegionAt(pos, &edge);

    juce::PopupMenu menu;
    // "Layers" everywhere the user can read it - the struct is EffectRegion but
    // README/REFERENCE/docs and every item below say "layer", and a menu that
    // calls the same thing by two names reads like two different features.
    menu.addSectionHeader("Effect Layers");

    if (hitIdx >= 0) {
        const auto& r = node->effectRegions[(size_t)hitIdx];
        menu.addItem(1, "Delete layer \"" + fxRegionLabel(graph, r) + "\"");
        menu.addSeparator();
    }

    // Add submenu: groups first, then individual wires, each with a colour swatch
    // so the menu entry looks like the tube it will create - the user is picking
    // a colour here as much as a name, and the tube carries no other identity.
    auto swatch = [](uint32_t argb) {
        auto d = std::make_unique<juce::DrawableRectangle>();
        d->setRectangle(juce::Parallelogram<float>(juce::Rectangle<float>(0.0f, 0.0f, 12.0f, 12.0f)));
        d->setFill(juce::FillType(juce::Colour((juce::uint8)((argb >> 16) & 0xFF),
                                               (juce::uint8)((argb >> 8) & 0xFF),
                                               (juce::uint8)(argb & 0xFF))));
        d->setStrokeFill(juce::FillType(juce::Colours::black.withAlpha(0.6f)));
        d->setStrokeThickness(1.0f);
        return d;
    };
    auto addSwatchItem = [&swatch](juce::PopupMenu& m, int id, const juce::String& text,
                                   uint32_t argb) {
        juce::PopupMenu::Item item(text);
        item.itemID = id;
        item.image = swatch(argb);
        m.addItem(std::move(item));
    };

    // Wire names come from graph.wireLabel() so this menu, the layer legend and
    // the graph's own wire menus all spell a wire the same way - including the
    // rule that two wires between the same pair of nodes get qualified with
    // their plug names so the user isn't picking blind.
    std::vector<std::pair<int, juce::String>> wires;   // linkId, label
    for (auto& link : graph.links) {
        auto label = graph.wireLabel(link.id);
        if (label.isNotEmpty()) wires.emplace_back(link.id, label);
    }

    juce::PopupMenu addMenu;
    // Creating a group lives HERE, at the top of the list of things you can
    // gate, because this menu is where the need for one is felt. It also exists
    // on the wire's own right-click menu in the graph, but a user shaping layers
    // in a piano roll shouldn't have to go find a cable to discover that groups
    // are a thing.
    if (!wires.empty()) {
        addMenu.addItem(4, juce::String::fromUTF8("New group of wires\xe2\x80\xa6"));
        addMenu.addSeparator();
    }
    for (auto& grp : graph.effectGroups) {
        juce::String label = grp.name.empty() ? ("Group #" + juce::String(grp.id))
                                              : juce::String(grp.name);
        addSwatchItem(addMenu, 5000 + grp.id, "Group: " + label, grp.color);
    }
    if (!graph.effectGroups.empty()) addMenu.addSeparator();

    for (auto& [linkId, label] : wires)
        addSwatchItem(addMenu, 6000 + linkId, label, getDistinctColor(linkId));

    if (!wires.empty())
        menu.addSubMenu("Add layer at beat " + juce::String(clickBeat, 2)
                            + juce::String::fromUTF8("\xe2\x80\xa6"), addMenu);
    else
        menu.addItem(-1, "Add layer (no wires in this project yet)", false);

    menu.addSeparator();
    menu.addItem(2, fxLaneExpanded ? "Done editing (collapse)" : "Expand to edit");
    menu.addItem(3, juce::String::fromUTF8("Help: Effect Layers\xe2\x80\xa6"));

    // withMousePosition, NOT withTargetComponent(this): the target component is
    // the whole piano roll, so JUCE parks the menu against that giant rectangle's
    // edge - right-clicking a layer at bar 30 popped the menu at the far left of
    // the window, nowhere near the thing being right-clicked.
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(),
                       [this, myId, clickBeat, hitIdx, wires](int result) {
        refreshNode();
        if (!node || node->id != myId || result == 0) return;

        if (result == 1) {
            if (hitIdx >= 0 && hitIdx < (int)node->effectRegions.size()) {
                node->effectRegions.erase(node->effectRegions.begin() + hitIdx);
                graph.dirty = true;
                graph.commitSnapshot("Delete effect layer");
                if (onTimingChanged) onTimingChanged();
            }
        } else if (result == 2) {
            setFxLaneExpanded(!fxLaneExpanded);
            return;
        } else if (result == 3) {
            if (onOpenHelpDoc) onOpenHelpDoc("layers-and-groups.html");
            return;
        } else if (result == 4) {
            // Make a group and gate it here in one step. Splitting it ("group
            // made - now go add a layer for it") is what made groups feel like a
            // separate feature you had to already know about.
            auto* content = new NewGroupContent(wires, clickBeat,
                [this, myId, clickBeat](std::vector<int> picked, juce::String name) {
                    refreshNode();
                    if (picked.empty() || !node || node->id != myId) return;
                    auto& grp = graph.addEffectGroup(name.toStdString());
                    grp.linkIds = std::move(picked);
                    EffectRegion region;
                    region.groupId = grp.id;
                    region.startBeat = clickBeat;
                    region.endBeat = clickBeat + 4.0f;
                    region.color = grp.color;
                    node->effectRegions.push_back(region);
                    graph.dirty = true;
                    graph.commitSnapshot("New effect group");
                    setFxLaneExpanded(true);   // land straight in edit mode
                    if (onTimingChanged) onTimingChanged();
                    resized();
                    repaint();
                });
            juce::DialogWindow::LaunchOptions opt;
            opt.dialogTitle = "New Group of Wires";
            opt.content.setOwned(content);
            opt.escapeKeyTriggersCloseButton = true;
            opt.useNativeTitleBar = true;
            opt.resizable = true;
            opt.componentToCentreAround = this;
            launchToolDialog(opt);   // modal, no separate taskbar entry
            return;
        } else if (result >= 5000 && result < 6000) {
            EffectRegion region;
            region.groupId = result - 5000;
            region.startBeat = clickBeat;
            region.endBeat = clickBeat + 4.0f;
            if (auto* grp = graph.findEffectGroup(region.groupId)) region.color = grp->color;
            node->effectRegions.push_back(region);
            graph.dirty = true;
            graph.commitSnapshot("Add effect layer");
            setFxLaneExpanded(true);   // land straight in edit mode
            if (onTimingChanged) onTimingChanged();
        } else if (result >= 6000) {
            EffectRegion region;
            region.linkId = result - 6000;
            region.startBeat = clickBeat;
            region.endBeat = clickBeat + 4.0f;
            region.color = getDistinctColor(region.linkId);
            node->effectRegions.push_back(region);
            graph.dirty = true;
            graph.commitSnapshot("Add effect layer");
            setFxLaneExpanded(true);
            if (onTimingChanged) onTimingChanged();
        }
        // Adding or deleting can change the row count, hence the lane height.
        resized();
        repaint();
    });
}

void PianoRollComponent::paintTrackHeader(juce::Graphics& g) {
    if (!node) return;
    float gridX = KEY_WIDTH;
    float gridW = std::max(1.0f, (float)getWidth() - KEY_WIDTH - SCROLLBAR_SIZE);
    int top = trackHeaderTop();
    int h = TRACK_HEADER_H;

    // Band background.
    g.setColour(juce::Colour(26, 26, 34));
    g.fillRect(0, top, getWidth(), h);

    // Left-gutter caption over the keyboard column.
    g.setColour(juce::Colour(120, 120, 140));
    g.setFont(juce::Font(10.0f));
    g.drawText("Start", 2, top, (int)gridX - 4, h, juce::Justification::centredRight);

    // Horizontal mapping (mirrors the grid below).
    float totalBeats = graph.getTimelineBeats(*node);
    float absOffset = node->absoluteBeatOffset;
    float absTotalBeats = totalBeats + absOffset;
    float visibleBeats = std::max(1.0f, absTotalBeats / std::max(state.hZoom, 0.1f));
    float scrollBeat = juce::jlimit(0.0f, std::max(0.0f, absTotalBeats - visibleBeats), state.hScroll);
    auto beatToX = [&](float b) { return gridX + ((b + absOffset - scrollBeat) / visibleBeats) * gridW; };

    // The track as a clip-block from its start beat to its content end.
    float x0 = beatToX(0.0f);
    float x1 = beatToX(totalBeats);
    float bx0 = juce::jlimit(gridX, gridX + gridW, x0);
    float bx1 = juce::jlimit(gridX, gridX + gridW, x1);
    juce::Rectangle<float> block(bx0, (float)top + 2.0f, std::max(3.0f, bx1 - bx0), (float)h - 4.0f);
    bool isChild = node->parentGroupId >= 0;
    juce::Colour blockCol = isChild ? juce::Colour(70, 115, 90) : juce::Colour(60, 85, 125);
    g.setColour(blockCol);
    g.fillRoundedRectangle(block, 3.0f);
    g.setColour(blockCol.brighter(0.4f));
    g.drawRoundedRectangle(block, 3.0f, 1.0f);
    // Bright left edge = the start-beat grab affordance (only if the block's
    // true start is in view, not clamped to the left edge).
    if (x0 >= gridX - 0.5f) {
        g.setColour(juce::Colours::white.withAlpha(0.75f));
        g.fillRect(block.getX(), block.getY(), 2.0f, block.getHeight());
    }

    // Label: track name, parent, and own start offset.
    juce::String label = juce::String(node->name);
    if (isChild) {
        auto* parent = graph.findNode(node->parentGroupId);
        label += juce::String::fromUTF8("  \xe2\x97\x82 child of ")
               + juce::String(parent ? parent->name : "?");
    }
    if (node->groupBeatOffset != 0.0f || isChild)
        label += "   @" + juce::String(node->groupBeatOffset, 2) + " beats";
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(11.0f));
    int labX = (int)block.getX() + 6;
    g.drawText(label, labX, top, getWidth() - labX - SCROLLBAR_SIZE - 4, h,
               juce::Justification::centredLeft);

    // Bottom hairline separating the strip from the grid.
    g.setColour(juce::Colour(0, 0, 0).withAlpha(0.5f));
    g.drawHorizontalLine(toolbarHeight() - 1, 0.0f, (float)getWidth());
}

void PianoRollComponent::showTrackHeaderMenu() {
    if (!node) return;
    const int myId = node->id;

    juce::PopupMenu menu;
    menu.addSectionHeader(juce::String(node->name));
    // The parent/child + start-beat items are shared with the node-graph node
    // menu (see track_nesting_menu.h) so the two entry points never drift.
    TrackNestingMenu::addItems(menu, graph, *node);

    // withMousePosition for the same reason as the Effects-lane menu above.
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(),
        [this, myId](int result) {
            if (result == 0) return;
            TrackNestingMenu::handle(result, graph, myId, this,
                [this](const char* desc) {
                    graph.resolveAnchors();
                    repaint();
                    if (onTimingChanged) onTimingChanged();
                    graph.commitSnapshot(desc);
                });
        });
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

juce::String PianoRollComponent::getTooltip() {
    // The TooltipWindow polls this whenever the mouse is at rest. Refresh the
    // node pointer first (graph.nodes can reallocate between hovers, which
    // would invalidate a stale node pointer) before touching clip data.
    refreshNode();
    if (!node) return {};

    auto p = getMouseXYRelative().toFloat();

    // Effect Regions lane. Explained in full here because the lane is the only
    // signpost the feature has - see the block comment in the header.
    if (isInFxLane(p)) {
        int edge = 0;
        const int idx = fxRegionAt(p, &edge);
        if (idx >= 0) {
            const auto& r = node->effectRegions[(size_t)idx];
            juce::String t;
            t << "Effect layer: " << fxRegionLabel(graph, r) << "\n"
              << "Active from beat " << juce::String(r.startBeat, 2)
              << " to " << juce::String(r.endBeat, 2)
              << " (" << juce::String(r.endBeat - r.startBeat, 2) << " beats).";
            if (fxLaneExpanded)
                t << "\nDrag the middle to move it, an end to resize. "
                     "Right-click to delete.";
            else
                t << "\nClick the lane to expand it for editing.";
            return t;
        }
        if (fxLaneExpanded)
            return "Effect Regions - time ranges in which a cable (or effect group) "
                   "is switched on.\nRight-click to add a layer. Click the caption, "
                   "or press Escape, when you're done editing.";
        return "Effect Regions - time ranges in which a cable (or effect group) is "
               "switched on, so an effect only applies to part of the track.\n"
               "Click to expand this lane and edit them.";
    }

    // Restrict to the note grid - skip the keyboard column, toolbar and the
    // two scrollbars so hovering chrome never shows a note tooltip.
    if (p.x < KEY_WIDTH || p.x > getWidth() - SCROLLBAR_SIZE) return {};
    if (p.y < toolbarHeight() || p.y > getHeight() - SCROLLBAR_SIZE) return {};
    if (isInExprLane(p)) return {};

    // Same beat<->pixel mapping as findNoteAt / paint.
    float gridX = KEY_WIDTH;
    float gridW = getWidth() - KEY_WIDTH - SCROLLBAR_SIZE;
    float totalBeats = graph.getTimelineBeats(*node);
    float absOffset = node->absoluteBeatOffset;
    float absTotalBeats = totalBeats + absOffset;
    float visibleBeats = std::max(1.0f, absTotalBeats / std::max(state.hZoom, 0.1f));
    float scrollBeat = state.hScroll;
    auto beatToX = [&](float b) { return gridX + ((b + absOffset - scrollBeat) / visibleBeats) * gridW; };

    auto [hoverBeat, hoverPitch] = screenToBeatPitch(p);
    juce::ignoreUnused(hoverBeat);

    juce::StringArray lines;
    for (int ci = 0; ci < (int)node->clips.size(); ++ci) {
        auto& clip = node->clips[ci];
        for (int ni = 0; ni < (int)clip.notes.size(); ++ni) {
            auto& n = clip.notes[ni];
            if (n.pitch != hoverPitch) continue;
            float absBeat = clip.startBeat + n.getOffset();
            float nx1 = beatToX(absBeat);
            float nx2 = beatToX(absBeat + n.getDuration());
            if (p.x < nx1 || p.x > nx2) continue;

            juce::String line = MusicTheory::noteName(n.pitch);
            if (n.degree >= 0 && n.degree < 7)
                line << " (" << MusicTheory::DEGREE_NAMES[n.degree] << ")";
            line << "  vel " << n.velocity;
            line << "  start " << juce::String(absBeat, 2)
                 << "  len " << juce::String(n.getDuration(), 2);
            if (std::abs(n.detune) > 0.01f)
                line << "  " << (n.detune > 0 ? "+" : "")
                     << juce::String(n.detune, 0) << "c";
            if (node->clips.size() > 1 && !clip.name.empty())
                line << "  [" << clip.name << "]";
            lines.add(line);
        }
    }
    if (lines.isEmpty()) return {};
    if (lines.size() > 1)
        lines.insert(0, juce::String(lines.size()) + " notes:");
    return lines.joinIntoString("\n");
}

void PianoRollComponent::mouseDown(const juce::MouseEvent& e) {
    refreshNode(); if (!node) return;

    // Resize-handle gesture: clicks in the top RESIZE_HANDLE_H strip
    // start a panel-resize drag and don't reach anything else. Tracked
    // through mouseDrag in screen-y coordinates so the drag survives the
    // panel shifting under the cursor mid-gesture.
    if (e.y < RESIZE_HANDLE_H && onResizeDrag) {
        resizingHeight = true;
        resizeLastY = e.getScreenY();
        return;
    }

    // Effect Regions lane. Sits above the track-header strip, so it must be
    // tested first (both are inside toolbarHeight()).
    //
    //   collapsed  - left-click anywhere expands into edit mode; right-click
    //                opens the menu (and expands if you add something).
    //   expanded   - left-drag a tube's middle to move it, an end to resize;
    //                right-click a tube to delete it or empty space to add.
    //                Clicking the left-gutter caption collapses again, as does
    //                Escape and the menu's "Done editing" item.
    if (isInFxLane(e.position)) {
        if (e.mods.isRightButtonDown()) {
            showFxLaneMenu(e.position);
            return;
        }
        // Left-gutter caption cell is the expand/collapse toggle in both states.
        if (e.position.x < KEY_WIDTH) {
            setFxLaneExpanded(!fxLaneExpanded);
            return;
        }
        if (!fxLaneExpanded) {
            setFxLaneExpanded(true);
            return;
        }
        int edge = 0;
        const int idx = fxRegionAt(e.position, &edge);
        if (idx >= 0) {
            const auto& r = node->effectRegions[(size_t)idx];
            fxDragIdx = idx;
            fxDragMode = edge < 0 ? FxDrag::ResizeL
                       : edge > 0 ? FxDrag::ResizeR
                                  : FxDrag::Move;
            fxDragGrabBeat = fxXToBeat(e.position.x);
            fxDragStart0 = r.startBeat;
            fxDragEnd0 = r.endBeat;
            fxDragChanged = false;
            // No direction yet, so the first few pixels of travel are unsnapped.
            fxDragDir = 0;
            fxDragDirLastX = e.position.x;
        }
        return;
    }

    // Track-header strip: right-click opens the parent/offset menu; left-drag
    // retimes the track (changes its own groupBeatOffset, which cascades to
    // children). Capture the beat<->x mapping at drag start so it stays stable
    // even as the derived timeline length shifts while dragging.
    if (isInTrackHeader(e.position)) {
        if (e.mods.isRightButtonDown()) {
            showTrackHeaderMenu();
            return;
        }
        float absTotalBeats = graph.getTimelineBeats(*node) + node->absoluteBeatOffset;
        trackOffsetDragVisBeats = std::max(1.0f, absTotalBeats / std::max(state.hZoom, 0.1f));
        trackOffsetDragScroll = state.hScroll;
        trackOffsetStartOffset = node->groupBeatOffset;
        trackOffsetDownAbsBeat = panelXToAbsBeat(e.position.x, trackOffsetDragVisBeats,
                                                 trackOffsetDragScroll);
        dragMode = DragTrackOffset;
        return;
    }

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
                    graph.commitSnapshot("Delete automation point");
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
                graph.commitSnapshot("Delete MPE expression point");
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
        // Defer the context menu to mouseUp (see rightClickArmed in the header).
        // Showing it here, while the right button is held, lets the release
        // auto-select the first item ("Place Note Here") and drop a stray note.
        rightClickArmed = true;
        rightClickDownPos = e.position;
        return;
    }

    auto hit = findNoteAt(e.position);

    // Song-end resize handle: grab the orange end border when the cursor is near
    // it and not over a note. Captures the gesture-start horizontal mapping so
    // the live drag stays smooth even as the derived timeline length changes.
    if (!hit.valid() && !node->clips.empty()
        && e.position.x >= KEY_WIDTH && e.position.y > toolbarHeight()) {
        int endClip = -1;
        float endBeat = songEndBeatLocal(&endClip);
        if (endClip >= 0
            && std::abs(e.position.x - beatToScreenX(endBeat)) <= SONG_END_GRAB_PX) {
            dragMode = DragSongEnd;
            songEndDragClipIdx = endClip;
            float absTotalBeats = graph.getTimelineBeats(*node) + node->absoluteBeatOffset;
            songEndDragVisBeats = absTotalBeats / std::max(state.hZoom, 0.1f);
            songEndDragScroll = state.hScroll;
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            repaint();
            return;
        }
    }

    // A-B loop boundary handles: grab the loop start/end vertical line when the
    // cursor is near it (and not over a note). loopStart/End are absolute beats;
    // beatToScreenX takes node-local, so subtract the node's absolute offset.
    if (!hit.valid() && transport && transport->loopEnabled
        && transport->loopEndBeat > transport->loopStartBeat
        && e.position.x >= KEY_WIDTH && e.position.y > toolbarHeight()) {
        float sx1 = beatToScreenX((float)transport->loopStartBeat - node->absoluteBeatOffset);
        float sx2 = beatToScreenX((float)transport->loopEndBeat   - node->absoluteBeatOffset);
        float d1 = std::abs(e.position.x - sx1);
        float d2 = std::abs(e.position.x - sx2);
        // Prefer whichever edge is closer when both are within grab range.
        if (d1 <= LOOP_EDGE_GRAB_PX && d1 <= d2) {
            dragMode = DragLoopStart;
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            repaint();
            return;
        }
        if (d2 <= LOOP_EDGE_GRAB_PX) {
            dragMode = DragLoopEnd;
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            repaint();
            return;
        }
    }

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
        // Remember the pre-drag selection so live marquee updates can rebuild
        // selection = base + notes-in-box on every tick (Shift+drag keeps the
        // old selection as the base; a plain drag starts from empty).
        marqueeBase = state.selected;
    }
    repaint();
}

void PianoRollComponent::mouseDrag(const juce::MouseEvent& e) {
    refreshNode(); if (!node) return;

    // Resize-handle drag - fire deltaY in screen coords up to the host
    // so the panel's heightPx tracks the cursor. Doing this in screen-y
    // (rather than e.y / e.position.y) is important because the panel's
    // bounds shift under the cursor as the host re-lays-out the stack
    // mid-gesture; local coords would oscillate.
    if (resizingHeight && onResizeDrag) {
        int y = e.getScreenY();
        int dy = y - resizeLastY;
        if (dy != 0) {
            onResizeDrag(dy);
            resizeLastY = y;
        }
        return;
    }

    // Effect-layer move/resize. Live-updates the region; one undo snapshot is
    // pushed on release (mouseUp), matching the track-retime gesture below.
    // Positions are pixel-precise, with a directional magnetic pull toward grid
    // markers on the departing side only (fxMagneticSnap); Alt turns it off.
    if (fxDragMode != FxDrag::None) {
        if (fxDragIdx < 0 || fxDragIdx >= (int)node->effectRegions.size()) {
            fxDragMode = FxDrag::None;
            return;
        }
        auto& r = node->effectRegions[(size_t)fxDragIdx];

        // Track travel direction with hysteresis - see fxDragDir in the header
        // for why it's derived from the live cursor, not the total delta.
        if (e.position.x > fxDragDirLastX + FX_DIR_HYSTERESIS_PX) {
            fxDragDir = +1; fxDragDirLastX = e.position.x;
        } else if (e.position.x < fxDragDirLastX - FX_DIR_HYSTERESIS_PX) {
            fxDragDir = -1; fxDragDirLastX = e.position.x;
        }
        // Alt = fully free, matching every other snap-bearing gesture here.
        const int dir = e.mods.isAltDown() ? 0 : fxDragDir;
        auto magnet = [this, dir](float b, bool* hit = nullptr) {
            return fxMagneticSnap(b, dir, hit);
        };

        const float delta = fxXToBeat(e.position.x) - fxDragGrabBeat;
        // A layer must keep a positive length, so each resize edge is clamped
        // against the other one rather than being allowed to cross it. The
        // floor is one pixel's worth of beats rather than a grid step: edges are
        // pixel-precise now, so a grid-sized minimum would be an arbitrary wall
        // in the middle of the range the user can otherwise reach.
        const float minLen = std::max(fxBeatsPerPixel(), 1.0e-4f);

        float newStart = r.startBeat, newEnd = r.endBeat;
        if (fxDragMode == FxDrag::Move) {
            const float len = fxDragEnd0 - fxDragStart0;
            const float rawStart = fxDragStart0 + delta;
            // Both ends of a moving layer are magnetic; whichever actually
            // caught a marker wins, and if both did, the smaller correction.
            // Snapping only the left edge would make the right edge unalignable
            // without arithmetic, and a layer's end is just as musically
            // meaningful as its start. Note the hitL/hitR flags rather than
            // comparing the results to rawStart: a candidate that did NOT snap
            // has a correction of exactly zero, so a nearest-correction test
            // alone would let it beat the one that did.
            bool hitL = false, hitR = false;
            const float snapL = magnet(rawStart, &hitL);
            const float snapR = magnet(rawStart + len, &hitR) - len;
            if (hitL && hitR)
                newStart = std::abs(snapL - rawStart) <= std::abs(snapR - rawStart)
                         ? snapL : snapR;
            else if (hitL) newStart = snapL;
            else if (hitR) newStart = snapR;
            else           newStart = rawStart;
            newStart = std::max(0.0f, newStart);
            newEnd = newStart + len;
        } else if (fxDragMode == FxDrag::ResizeL) {
            newStart = juce::jlimit(0.0f, fxDragEnd0 - minLen, magnet(fxDragStart0 + delta));
            newEnd = fxDragEnd0;
        } else { // ResizeR
            newStart = fxDragStart0;
            newEnd = std::max(fxDragStart0 + minLen, magnet(fxDragEnd0 + delta));
        }
        if (newStart != r.startBeat || newEnd != r.endBeat) {
            r.startBeat = newStart;
            r.endBeat = newEnd;
            fxDragChanged = true;
            graph.dirty = true;
            repaint();
        }
        return;
    }

    // Track-header retime drag: move the track's own start offset by however
    // many beats the cursor has travelled since mouseDown. Snap unless Alt is
    // held. No undo step here - one snapshot is pushed on mouseUp.
    if (dragMode == DragTrackOffset) {
        float curAbs = panelXToAbsBeat(e.position.x, trackOffsetDragVisBeats,
                                       trackOffsetDragScroll);
        float newOffset = trackOffsetStartOffset + (curAbs - trackOffsetDownAbsBeat);
        if (!e.mods.isAltDown()) {
            float snap = state.snap > 0 ? state.snap : 0.0625f;
            newOffset = std::round(newOffset / snap) * snap;
        }
        newOffset = std::max(0.0f, newOffset);
        if (newOffset != node->groupBeatOffset) {
            node->groupBeatOffset = newOffset;
            node->anchorMarker.clear();   // explicit drag overrides any anchor
            graph.resolveAnchors();
            graph.dirty = true;
            repaint();
            if (onTimingChanged) onTimingChanged();
        }
        return;
    }

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
        // Live highlight: recompute which notes the rectangle currently covers
        // so they light up as it sweeps over them and drop out when it leaves.
        updateMarqueeSelection();
    } else if (dragMode == DragSongEnd) {
        if (songEndDragClipIdx >= 0 && songEndDragClipIdx < (int)node->clips.size()) {
            // Convert cursor x to a node-local beat using the mapping captured at
            // gesture start (decoupled from the live, changing timeline length).
            float gridX = KEY_WIDTH;
            float gridW = (float)(getWidth() - KEY_WIDTH - SCROLLBAR_SIZE);
            float absBeat = songEndDragScroll
                          + ((e.position.x - gridX) / std::max(1.0f, gridW)) * songEndDragVisBeats;
            float beatLocal = absBeat - node->absoluteBeatOffset;
            float target = altHeld ? beatLocal : std::round(beatLocal / snap) * snap;
            auto& clip = node->clips[songEndDragClipIdx];
            float minEnd = clip.startBeat + std::max(snap, 0.25f);
            target = std::max(target, minEnd);
            clip.lengthBeats = target - clip.startBeat;
            graph.dirty = true;
        }
    } else if (dragMode == DragLoopStart || dragMode == DragLoopEnd) {
        // Loop boundaries are absolute beats; `beat` is node-local, so add the
        // node's absolute offset. Snap unless Alt is held. Keep start < end with
        // at least one snap unit between them.
        float absTarget = std::max(0.0f, snapBeat(beat)) + node->absoluteBeatOffset;
        float gap = std::max(snap, 0.25f);
        if (dragMode == DragLoopStart)
            graph.loopStartBeat = std::min((double)absTarget, graph.loopEndBeat - gap);
        else
            graph.loopEndBeat = std::max((double)absTarget, graph.loopStartBeat + gap);
        if (transport) {
            transport->loopStartBeat = graph.loopStartBeat;
            transport->loopEndBeat = graph.loopEndBeat;
        }
        graph.dirty = true;
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
            {
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
    // Clear panel-resize gesture before anything else - the handle drag
    // never sets dragMode and never has a clip/note to operate on, so the
    // rest of mouseUp would just be a no-op for it anyway.
    if (resizingHeight) {
        resizingHeight = false;
        return;
    }

    // Commit an effect-layer move/resize: one undo step for the whole gesture,
    // and only if the range actually changed (a plain click on a tube must not
    // litter the undo tree with no-op steps).
    if (fxDragMode != FxDrag::None) {
        const bool wasResize = fxDragMode != FxDrag::Move;
        fxDragMode = FxDrag::None;
        fxDragIdx = -1;
        if (fxDragChanged) {
            fxDragChanged = false;
            graph.commitSnapshot(wasResize ? "Resize effect layer" : "Move effect layer");
            if (onTimingChanged) onTimingChanged();
        }
        return;
    }

    // Commit a track-header retime: the offset was updated live during the
    // drag; push one undo step for the whole gesture on release.
    if (dragMode == DragTrackOffset) {
        dragMode = DragNone;
        graph.resolveAnchors();
        if (onTimingChanged) onTimingChanged();
        graph.commitSnapshot("Move track in time");
        return;
    }

    // Deferred right-click context menu: armed in mouseDown, shown here on
    // release so no held button can auto-pick a menu item. Treat it as a menu
    // click only if the cursor didn't travel far (a right-drag isn't a menu
    // gesture). Recompute the hit/anchor at the release point.
    if (rightClickArmed) {
        rightClickArmed = false;
        if (e.position.getDistanceFrom(rightClickDownPos) < 6.0f) {
            auto [rbBeat, rbPitch] = screenToBeatPitch(e.position);
            auto rbHit = findNoteAt(e.position);
            if (rbHit.valid()) {
                if (!state.selected.count({rbHit.ci, rbHit.ni}))
                    state.selected = {{rbHit.ci, rbHit.ni}};
                showNoteMenu();
            } else {
                lastClickBeat = rbBeat;
                lastClickPitch = rbPitch;
                showEmptyMenu();
            }
        }
        return;
    }

    // Commit a song-end resize: the clip length was adjusted live during the
    // drag; now trim any notes left beyond the new end (and clamp ones that
    // straddle it), then snapshot the whole gesture as one undo step.
    if (dragMode == DragSongEnd) {
        if (songEndDragClipIdx >= 0 && songEndDragClipIdx < (int)node->clips.size()) {
            auto& clip = node->clips[songEndDragClipIdx];
            auto& notes = clip.notes;
            notes.erase(std::remove_if(notes.begin(), notes.end(),
                [&](const MidiNote& n) { return n.offset >= clip.lengthBeats; }),
                notes.end());
            for (auto& n : notes)
                if (n.offset + n.duration > clip.lengthBeats)
                    n.duration = std::max(0.03125f, clip.lengthBeats - n.offset);
            // Note indices shifted as notes were removed; drop the selection
            // rather than leave it pointing at moved/removed entries.
            state.selected.clear();
            // Dragging END right extends the clip past an explicit song-length
            // override; grow the override so the added beats play. The snapshot
            // below captures the new value, so undo restores it. No-op in auto
            // mode (where shortening/extending follows the clip directly).
            graph.growSongLengthToContent();
            graph.commitSnapshot("Resize song end");
            // Preserve the visible beat span if the timeline length changed
            // (mirrors the note-placement path).
            updateScrollBars();
        }
        songEndDragClipIdx = -1;
        dragMode = DragNone;
        setMouseCursor(juce::MouseCursor::NormalCursor);
        repaint();
        return;
    }

    // Commit a loop-edge drag as a single undo step. The loop fields live on the
    // graph and are captured by commitSnapshot (serializeForUndo writes them).
    if (dragMode == DragLoopStart || dragMode == DragLoopEnd) {
        graph.commitSnapshot("Move loop edge");
        dragMode = DragNone;
        setMouseCursor(juce::MouseCursor::NormalCursor);
        repaint();
        return;
    }

    if (dragMode == DragBox) {
        auto [b1, p1] = screenToBeatPitch(dragStartScreen);
        auto [b2, p2] = screenToBeatPitch(dragCurrentScreen);
        float dist = std::abs(b2 - b1) + std::abs((float)(p2 - p1));

        if (dist < 0.3f) {
            // A negligible drag is really a click to place a note, not a
            // marquee. Undo any live highlighting the drag may have applied by
            // restoring the pre-drag selection (empty for a plain click).
            state.selected = marqueeBase;
            // Place a note - find or extend a clip to fit
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
                // Grow an explicit song-length override so a note placed past
                // the song end actually plays (no-op in auto mode). Capture
                // prior/new for undo.
                float newClipLen = targetClip->lengthBeats;
                double priorSongLen = graph.growSongLengthToContent();
                double newSongLen = graph.songLengthBeats;
                auto* graphPtr = &graph;
                graph.undoTree.pushDone(std::make_unique<LambdaCommand>(
                    "Place note",
                    [nodePtr, graphPtr, ci, nnCopy, newClipLen, newSongLen]() {
                        if (ci < (int)nodePtr->clips.size()) {
                            nodePtr->clips[ci].notes.push_back(nnCopy);
                            nodePtr->clips[ci].lengthBeats = newClipLen;
                        }
                        graphPtr->songLengthBeats = newSongLen;
                    },
                    [nodePtr, graphPtr, ci, priorSongLen]() {
                        if (ci < (int)nodePtr->clips.size() && !nodePtr->clips[ci].notes.empty())
                            nodePtr->clips[ci].notes.pop_back();
                        graphPtr->songLengthBeats = priorSongLen;
                    }
                ));
            }

            // Preserve the visible beat range after clip extension
            float newTotalBeats = graph.getTimelineBeats(*node);
            if (newTotalBeats > oldTotalBeats && oldVisibleBeats > 0) {
                state.hZoom = newTotalBeats / oldVisibleBeats;
            }
        } else {
            // Finalize the marquee selection (same computation that ran live on
            // every drag tick).
            updateMarqueeSelection();
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
        // Commit the gesture (point add or drag) as one undo step. Expression
        // and automation curves have no trivial in-place inverse and are part
        // of the serialized graph, so the snapshot path is the correct one
        // (matches the policy in CLAUDE.md). commitSnapshot no-ops if nothing
        // actually changed.
        graph.commitSnapshot(exprDragNI == -2 ? "Edit automation" : "Edit MPE expression");
    }

    // Push undo for drag operations
    if (!dragBeforeSnapshot.empty()) {
        if (dragMode == DragNote || dragMode == DragResizeLeft || dragMode == DragResizeRight) {
            std::string desc = (dragMode == DragNote) ? "Move notes"
                             : (dragMode == DragResizeLeft) ? "Resize note (left)"
                             : "Resize note (right)";
            pushDragUndo(desc, dragBeforeSnapshot);
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
        // Clamp so the visible window stays inside MIDI 0..127. Using the same
        // bounds as the zoom slider (visibleRange/2 .. 127-visibleRange/2) lets
        // the scrollbar reach the very top (pitch 127) and bottom (pitch 0) - a
        // fixed 12..115 clamp would strand notes near the extremes one row out
        // of the interactive area.
        state.scrollPitch = juce::jlimit(state.visibleRange / 2,
                                         127 - state.visibleRange / 2,
                                         state.scrollPitch);
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
    // Top resize-handle strip wins over note-edge cursor changes - it's
    // physically above the grid and getting a left-right arrow there
    // would be misleading.
    if (e.y < RESIZE_HANDLE_H && onResizeDrag) {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        return;
    }
    // Effect Regions lane: a pointing hand says "this whole band is clickable"
    // (the single most important cue for a feature nobody was finding), and the
    // horizontal-resize arrow marks the drag-to-retime edges while expanded.
    if (isInFxLane(e.position)) {
        int edge = 0;
        const bool overRegion = fxRegionAt(e.position, &edge) >= 0;
        setMouseCursor(overRegion && edge != 0 ? juce::MouseCursor::LeftRightResizeCursor
                                               : juce::MouseCursor::PointingHandCursor);
        return;
    }
    auto hit = findNoteAt(e.position);
    bool nearLoopEdge = false;
    if (!hit.valid() && transport && transport->loopEnabled
        && transport->loopEndBeat > transport->loopStartBeat
        && e.position.x >= KEY_WIDTH && e.position.y > toolbarHeight()) {
        float sx1 = beatToScreenX((float)transport->loopStartBeat - node->absoluteBeatOffset);
        float sx2 = beatToScreenX((float)transport->loopEndBeat   - node->absoluteBeatOffset);
        nearLoopEdge = std::abs(e.position.x - sx1) <= LOOP_EDGE_GRAB_PX
                    || std::abs(e.position.x - sx2) <= LOOP_EDGE_GRAB_PX;
    }
    if ((hit.valid() && (hit.edge == NoteHit::Left || hit.edge == NoteHit::Right)) || nearLoopEdge)
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    else
        setMouseCursor(juce::MouseCursor::NormalCursor);

    // Track the grid cell under the cursor as the live paste target. Only the
    // editable grid counts (right of the keys, below the toolbar, left of the
    // vertical scrollbar) so the ghost preview never appears over chrome.
    float gridRight = (float)getWidth() - SCROLLBAR_SIZE;
    float gridBottom = (float)getHeight() - SCROLLBAR_SIZE;
    bool onGrid = e.position.x >= KEY_WIDTH && e.position.x < gridRight
               && e.position.y >= (float)toolbarHeight() && e.position.y < gridBottom;
    bool wasValid = hoverValid;
    if (onGrid) {
        auto [hb, hp] = screenToBeatPitch(e.position);
        hoverBeat = hb;
        hoverPitch = hp;
        hoverValid = true;
        // Repaint to move the ghost preview while there's something to paste.
        if (!clipboard.empty()) repaint();
    } else if (wasValid) {
        hoverValid = false;
        if (!clipboard.empty()) repaint();
    }
}

void PianoRollComponent::mouseEnter(const juce::MouseEvent&) {
    // Focus-follows-mouse: grab keyboard focus when the cursor enters the panel
    // so Ctrl+C / Ctrl+V / Delete work while hovering, without first having to
    // click (which would place a stray note). Required for hover+Ctrl+V paste to
    // land where the ghost preview shows.
    //
    // ...but ONLY when SEANCE is already the foreground application. On Windows,
    // calling grabKeyboardFocus() inside a window that isn't the active window
    // forces that window to the foreground (Win32 SetFocus activates the owning
    // top-level window), which would yank SEANCE in front of whatever app the
    // user is actually working in just because the mouse passed over our window.
    // Guarding on isForegroundProcess() keeps focus-follows-mouse working when
    // SEANCE is the active app while never stealing focus from another app.
    if (juce::Process::isForegroundProcess() && !hasKeyboardFocus(true))
        grabKeyboardFocus();
}

void PianoRollComponent::mouseExit(const juce::MouseEvent&) {
    // Cursor left the panel - drop the ghost paste preview.
    if (hoverValid) {
        hoverValid = false;
        if (!clipboard.empty()) repaint();
    }
}

void PianoRollComponent::mouseWheelMove(const juce::MouseEvent& e,
                                         const juce::MouseWheelDetails& wheel) {
    refreshNode(); if (!node) return;
    if (e.mods.isCtrlDown() && e.mods.isShiftDown()) {
        // Ctrl+Shift+scroll: vertical zoom (rows-per-octave). Up = zoom
        // in (fewer semitones, thicker note lanes). Step in semitones so
        // a single wheel notch makes a perceptible row-height change at
        // any zoom level - using a multiplicative factor felt too slow
        // near the zoomed-in end where visibleRange is already small.
        int step = wheel.deltaY > 0 ? -2 : 2;
        int newRange = juce::jlimit(12, 120, state.visibleRange + step);
        if (newRange != state.visibleRange) {
            state.visibleRange = newRange;
            state.scrollPitch = juce::jlimit(newRange / 2,
                                              127 - newRange / 2,
                                              state.scrollPitch);
            // Keep the vZoom slider in sync. Reuse the same mapping the
            // slider's onValueChange uses (inverted, linear-in-semitones).
            double sliderValue = 10.0 - (newRange - 12) / 12.0;
            vZoomSlider.setValue(sliderValue, juce::dontSendNotification);
        }
    } else if (e.mods.isCtrlDown()) {
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

// Ramer-Douglas-Peucker simplification of a recorded expression curve.
// Measures each point's deviation in value from the straight line between
// the current segment's endpoints (time is the x-axis), and drops every
// point whose deviation stays below `tol` (in normalized 0..1 value units).
// The first and last points are always kept. Used to thin the dense streams
// captured from an MPE controller into a handful of editable control points.
static void simplifyExprCurve(std::vector<ExpressionPoint>& pts, float tol) {
    int n = (int)pts.size();
    if (n <= 2) return;
    std::vector<bool> keep(n, false);
    keep[0] = keep[n - 1] = true;
    std::vector<std::pair<int, int>> stack;
    stack.push_back({0, n - 1});
    while (!stack.empty()) {
        auto [a, b] = stack.back();
        stack.pop_back();
        if (b <= a + 1) continue;
        float t0 = pts[a].time, v0 = pts[a].value;
        float t1 = pts[b].time, v1 = pts[b].value;
        float dt = t1 - t0;
        float maxDev = -1.0f;
        int maxIdx = -1;
        for (int i = a + 1; i < b; ++i) {
            float interp = (std::abs(dt) > 1e-9f)
                ? v0 + (v1 - v0) * ((pts[i].time - t0) / dt)
                : v0;
            float dev = std::abs(pts[i].value - interp);
            if (dev > maxDev) { maxDev = dev; maxIdx = i; }
        }
        if (maxDev > tol && maxIdx > 0) {
            keep[maxIdx] = true;
            stack.push_back({a, maxIdx});
            stack.push_back({maxIdx, b});
        }
    }
    std::vector<ExpressionPoint> out;
    out.reserve(n);
    for (int i = 0; i < n; ++i) if (keep[i]) out.push_back(pts[i]);
    pts = std::move(out);
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

    menu.addItem(32, "Reverse", numSel > 1);
    menu.addSeparator();

    // Shared undo state - the slider callbacks modify notes directly while
    // the menu is open; we push a single undo step when the menu closes.
    struct SliderUndo {
        bool changed = false;
        std::vector<NoteSnapshot> before;
    };
    auto sliderUndo = std::make_shared<SliderUndo>();
    captureSelectedSnapshot(sliderUndo->before);

    // Current value from first selected note (for slider initial position)
    int initVel = 100;
    float initDetune = 0;
    if (!state.selected.empty()) {
        auto [ci, ni] = *state.selected.begin();
        if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size()) {
            initVel = node->clips[ci].notes[ni].velocity;
            initDetune = node->clips[ci].notes[ni].detune;
        }
    }
    int initVelPct = (int)(initVel / 127.0 * 100.0 + 0.5);

    // Velocity submenu (slider + presets)
    {
        juce::PopupMenu velSub;
        velSub.addCustomItem(-1, std::make_unique<SliderMenuItem>(
            "Velocity", 0, 100, initVelPct, 1, "%",
            std::vector<std::pair<double, juce::String>>{
                {0, "0%"}, {25, "25%"}, {50, "50%"}, {75, "75%"}, {100, "100%"}},
            [this, sliderUndo](double pct) {
                int vel = std::max(0, (int)(pct / 100.0 * 127.0 + 0.5));
                for (auto& [ci, ni] : state.selected)
                    if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size())
                        node->clips[ci].notes[ni].velocity = vel;
                sliderUndo->changed = true;
                graph.dirty = true;
                repaint();
            }));
        menu.addSubMenu("Velocity", velSub);
    }

    // Detune submenu (slider + presets)
    {
        juce::PopupMenu detSub;
        detSub.addCustomItem(-2, std::make_unique<SliderMenuItem>(
            "Detune", -100, 100, initDetune, 1, " ct",
            std::vector<std::pair<double, juce::String>>{
                {-75, "-75"}, {-50, "-50"}, {-25, "-25"}, {0, "0"},
                {25, "+25"}, {50, "+50"}, {75, "+75"}},
            [this, sliderUndo](double cents) {
                float d = (float)cents;
                for (auto& [ci, ni] : state.selected)
                    if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size())
                        node->clips[ci].notes[ni].detune = d;
                sliderUndo->changed = true;
                graph.dirty = true;
                repaint();
            }));
        menu.addSubMenu("Detune", detSub);
    }

    menu.addSeparator();

    // Root/Key/Mode/Scale submenus
    juce::PopupMenu rootMenu;
    for (int i = 0; i < 12; ++i)
        rootMenu.addItem(100 + i, MusicTheory::NOTE_NAMES[i], true, state.keyRoot == i);
    menu.addSubMenu("Root", rootMenu);

    juce::PopupMenu keyMenu;
    int ki = 0;
    for (auto& [name, _] : MusicTheory::keys())
        keyMenu.addItem(200 + ki++, name, true, state.activeCategory != "scale" && state.keyName == name);
    menu.addSubMenu("Key", keyMenu);

    juce::PopupMenu modeMenu;
    int mi = 0;
    for (auto& [name, _] : MusicTheory::modes())
        modeMenu.addItem(300 + mi++, name, true, state.activeCategory != "scale" && state.modeName == name);
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

    // MPE expression actions — only meaningful when this timeline emits MPE,
    // i.e. the recorded per-note pitch-bend/slide/pressure curves are live.
    if (node->mpeEnabled && !state.selected.empty()) {
        menu.addSeparator();
        juce::PopupMenu mpeMenu;
        mpeMenu.addItem(80, "Smooth / Thin Curves");
        mpeMenu.addItem(81, "Clear Expression");
        if (!node->params.empty()) {
            juce::PopupMenu bakeMenu;
            for (int i = 0; i < (int)node->params.size(); ++i)
                bakeMenu.addItem(800 + i, node->params[i].name);
            mpeMenu.addSubMenu("Bake Pressure to Automation", bakeMenu);
        }
        menu.addSubMenu("MPE Expression", mpeMenu);
    }

    menu.addSeparator();
    menu.addItem(3, "Select All");
    menu.addItem(4, "Deselect All");

    menu.showMenuAsync(juce::PopupMenu::Options(), [this, sliderUndo](int result) {
        // Commit slider changes (velocity/detune) as an undo step before
        // handling any regular menu item, so the undo order is correct.
        if (sliderUndo->changed) {
            pushDragUndo("Change velocity/detune", sliderUndo->before);
            sliderUndo->changed = false;
        }

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
            // Detune and velocity are now handled by inline slider
            // components - no case IDs needed.
            case 50: { // Snap to Scale
                auto intervals = MusicTheory::activeIntervals(
                    state.activeCategory, state.keyName, state.modeName, state.scaleName);
                int root = state.keyRoot;
                apply([&](MidiNote& n) { n.pitch = MusicTheory::snapToScale(n.pitch, root, intervals); });
                break;
            }
            case 51: { // Change Key
                auto intervals = MusicTheory::activeIntervals(
                    state.activeCategory, state.keyName, state.modeName, state.scaleName);
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
                        if (m.category == "scale") {
                            state.activeCategory = "scale"; state.scaleName = m.scaleName;
                        } else if (m.category == "mode") {
                            state.activeCategory = "keymode"; state.keyName = "Major"; state.modeName = m.scaleName;
                        } else {
                            state.activeCategory = "keymode"; state.keyName = m.scaleName; state.modeName = "Ionian (Major)";
                        }
                        syncScaleUI();
                        repaint();
                    }
                });
                break;
            }
            case 80: { // Smooth / thin recorded MPE curves
                for (auto& [ci, ni] : state.selected) {
                    if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size()) {
                        auto& ex = node->clips[ci].notes[ni].expression;
                        simplifyExprCurve(ex.pitchBend, 0.02f);
                        simplifyExprCurve(ex.slide, 0.02f);
                        simplifyExprCurve(ex.pressure, 0.02f);
                    }
                }
                graph.commitSnapshot("Smooth MPE expression");
                break;
            }
            case 81: { // Clear all recorded MPE expression on selection
                for (auto& [ci, ni] : state.selected) {
                    if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size()) {
                        auto& ex = node->clips[ci].notes[ni].expression;
                        ex.pitchBend.clear();
                        ex.slide.clear();
                        ex.pressure.clear();
                    }
                }
                graph.commitSnapshot("Clear MPE expression");
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
                if (result >= 100 && result < 112) { state.keyRoot = result - 100; syncScaleUI(); }
                else if (result >= 200 && result < 300) {
                    int i = 0; for (auto& [name, _] : MusicTheory::keys()) {
                        if (i++ == result - 200) { state.activeCategory = "keymode"; state.keyName = name; break; }
                    }
                    syncScaleUI();
                } else if (result >= 300 && result < 400) {
                    int i = 0; for (auto& [name, _] : MusicTheory::modes()) {
                        if (i++ == result - 300) { state.activeCategory = "keymode"; state.modeName = name; break; }
                    }
                    syncScaleUI();
                } else if (result >= 400 && result < 500) {
                    int i = 0; for (auto& [name, _] : MusicTheory::scales()) {
                        if (i++ == result - 400) { state.activeCategory = "scale"; state.scaleName = name; break; }
                    }
                    syncScaleUI();
                } else if (result >= 800 && result < 900) {
                    // Bake recorded per-note pressure onto a param's automation lane.
                    int pi = result - 800;
                    if (pi < (int)node->params.size()) {
                        auto& param = node->params[pi];
                        for (auto& [ci, ni] : state.selected) {
                            if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size()) {
                                auto& clip = node->clips[ci];
                                auto& note = clip.notes[ni];
                                float noteStart = clip.startBeat + note.getOffset();
                                for (auto& pt : note.expression.pressure) {
                                    float beat = noteStart + pt.time;
                                    float val = param.minVal + pt.value * (param.maxVal - param.minVal);
                                    param.automation.points.push_back({beat, val});
                                }
                            }
                        }
                        std::sort(param.automation.points.begin(), param.automation.points.end(),
                                  [](const AutomationPoint& a, const AutomationPoint& b) { return a.beat < b.beat; });
                        graph.commitSnapshot("Bake pressure to automation");
                        // Reveal the lane we just wrote into.
                        exprLane = ExprAutomation;
                        autoParamIndex = pi;
                        autoParamCombo.setSelectedItemIndex(pi + 1, juce::dontSendNotification);
                        exprLaneCombo.setSelectedItemIndex(0, juce::dontSendNotification);
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
            case 'V':
                // Paste at the cursor's grid cell, exactly where the ghost
                // preview is drawn (pasteAtCursor reads hoverBeat/hoverPitch).
                pasteAtCursor();
                return true;
            case 'A': selectAll(); return true;
            case 'F': zoomToSelection(); return true;
        }
    }
    // Escape leaves Effect Regions edit mode. This is the "I'm done" exit the
    // lane needs; the caption chevron and the context menu's Done item are the
    // other two. Only consumed when the lane is actually expanded, so Escape
    // keeps its normal meaning otherwise.
    if (key == juce::KeyPress::escapeKey && fxLaneExpanded) {
        setFxLaneExpanded(false);
        return true;
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

    // Find the copied block's top-left: earliest beat and highest pitch. Notes
    // are stored relative to this corner so paste can re-anchor the whole block.
    float minBeat = 1e9f;
    int   maxPitch = 0;
    for (auto& [ci, ni] : state.selected) {
        if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size()) {
            auto& n = node->clips[ci].notes[ni];
            float absBeat = node->clips[ci].startBeat + n.getOffset();
            minBeat  = std::min(minBeat, absBeat);
            maxPitch = std::max(maxPitch, n.pitch);
        }
    }

    // Copy notes relative to that top-left corner.
    for (auto& [ci, ni] : state.selected) {
        if (ci < (int)node->clips.size() && ni < (int)node->clips[ci].notes.size()) {
            auto& n = node->clips[ci].notes[ni];
            float absBeat = node->clips[ci].startBeat + n.getOffset();
            ClipboardNote cn;
            cn.offsetFromFirst = absBeat - minBeat;
            cn.pitchBelowTop = maxPitch - n.pitch;
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

    // Anchor the paste to the SAME point the ghost preview draws: the cursor's
    // grid cell while hovering (hoverValid), falling back to the last click when
    // the cursor isn't over the grid - e.g. a right-click -> Paste Notes, where
    // the popup steals the mouse (mouseExit clears hoverValid) so lastClick holds
    // the right-click location. This guarantees notes land exactly where the
    // ghost showed instead of at a stale, divergent lastClick.
    float anchorBeat  = hoverValid ? hoverBeat  : lastClickBeat;
    int   anchorPitch = hoverValid ? hoverPitch : lastClickPitch;

    float pasteBeat = anchorBeat;
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

    // Record the inserted notes (ci, ni, note) and the clip's prior length so
    // the whole paste is a single undoable LambdaCommand.
    struct PastedNote { int ci; int ni; MidiNote note; };
    std::vector<PastedNote> pasted;
    float priorLength = targetClip->lengthBeats;

    for (auto& cn : clipboard) {
        MidiNote nn;
        nn.offset = pasteBeat + cn.offsetFromFirst - targetClip->startBeat;
        // Re-anchor pitch: the clipboard stores each note as semitones below the
        // highest copied note, so anchorPitch (the paste target row) becomes
        // the top of the pasted block.
        nn.pitch = juce::jlimit(0, 127, anchorPitch - cn.pitchBelowTop);
        nn.duration = cn.duration;
        nn.velocity = cn.velocity;
        nn.detune = cn.detune;
        nn.expression = cn.expression;
        targetClip->notes.push_back(nn);

        // Select the pasted note
        int ni = (int)targetClip->notes.size() - 1;
        state.selected.insert({ci, ni});
        pasted.push_back({ci, ni, nn});

        // Extend clip if needed
        float noteEnd = nn.offset + nn.duration;
        if (noteEnd > targetClip->lengthBeats)
            targetClip->lengthBeats = std::ceil(noteEnd / 4.0f) * 4.0f;
    }

    // If an explicit song-length override is clamping playback, grow it so the
    // freshly-pasted notes actually play. No-op in auto mode. Capture the prior
    // value so undo restores it.
    double priorSongLen = graph.growSongLengthToContent();
    double newSongLen   = graph.songLengthBeats;

    auto* nodePtr = node;
    auto* graphPtr = &graph;
    float newLength = targetClip->lengthBeats;
    graph.undoTree.pushDone(std::make_unique<LambdaCommand>(
        "Paste " + std::to_string(pasted.size()) + " notes",
        [nodePtr, graphPtr, pasted, ci, newLength, newSongLen]() {
            // redo: re-append the pasted notes (they were the tail of the clip)
            if (ci < (int)nodePtr->clips.size()) {
                for (auto& p : pasted)
                    nodePtr->clips[ci].notes.push_back(p.note);
                nodePtr->clips[ci].lengthBeats = newLength;
            }
            graphPtr->songLengthBeats = newSongLen;
        },
        [nodePtr, graphPtr, pasted, ci, priorLength, priorSongLen]() {
            // undo: remove the pasted notes (highest index first) and restore length
            if (ci < (int)nodePtr->clips.size()) {
                for (int i = (int)pasted.size() - 1; i >= 0; --i) {
                    int ni = pasted[i].ni;
                    if (ni < (int)nodePtr->clips[ci].notes.size())
                        nodePtr->clips[ci].notes.erase(nodePtr->clips[ci].notes.begin() + ni);
                }
                nodePtr->clips[ci].lengthBeats = priorLength;
            }
            graphPtr->songLengthBeats = priorSongLen;
        }
    ));

    graph.dirty = true;
    repaint();
}

void PianoRollComponent::updateMarqueeSelection() {
    refreshNode(); if (!node) return;
    auto [b1, p1] = screenToBeatPitch(dragStartScreen);
    auto [b2, p2] = screenToBeatPitch(dragCurrentScreen);
    juce::ignoreUnused(p1, p2);

    // Use the RAW (unclamped) pitch of the box corners rather than the
    // view-clamped p1/p2: this lets the marquee spill past the top/bottom of
    // the visible window so notes sitting just above or below the view (or
    // dragged-to-edge selections) are still caught. screenToBeatPitch clamps to
    // [pitchLo, pitchHi], which would otherwise drop any note above the top row.
    auto rawPitch = [&](float y) {
        float gh = getHeight() - toolbarHeight() - SCROLLBAR_SIZE;
        int vr = state.visibleRange;
        int pHi = state.scrollPitch + vr / 2;
        float rH = gh / std::max(vr, 1);
        return pHi - (int)std::floor((y - toolbarHeight()) / rH);
    };
    int rp1 = rawPitch(dragStartScreen.y);
    int rp2 = rawPitch(dragCurrentScreen.y);
    float beatMin = std::min(b1, b2), beatMax = std::max(b1, b2);
    int pitchMin = juce::jlimit(0, 127, std::min(rp1, rp2));
    int pitchMax = juce::jlimit(0, 127, std::max(rp1, rp2));

    // Rebuild from the pre-drag base so notes that leave the rectangle
    // unhighlight, and Shift+drag keeps adding to the original selection.
    state.selected = marqueeBase;
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
    state.visibleRange = juce::jlimit(12, 120,
                                       maxPitch - minPitch + pitchPad * 2);

    // Sync zoom sliders to the post-fit state so the slider thumbs
    // reflect the new zoom instead of lagging on the old value.
    hZoomSlider.setValue(state.hZoom, juce::dontSendNotification);
    vZoomSlider.setValue(10.0 - (state.visibleRange - 12) / 12.0,
                         juce::dontSendNotification);

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
    // A pasted clip can land past the song end; grow an explicit override so
    // it plays (no-op in auto mode).
    graph.growSongLengthToContent();
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

    // When the right-click lands inside (or on the edge of) the A-B loop region,
    // surface a one-click way to remove it right at the top level. Loops imported
    // from MOD/tracker files surprise users who never set one, so make turning it
    // off obvious. (Also available in the Clip submenu and by dragging the edges.)
    if (transport && transport->loopEnabled
        && transport->loopEndBeat > transport->loopStartBeat) {
        double absClick = (double)lastClickBeat + node->absoluteBeatOffset;
        if (absClick >= transport->loopStartBeat - 0.5
            && absClick <= transport->loopEndBeat + 0.5) {
            menu.addItem(98, "Disable Loop Region");
            menu.addSeparator();
        }
    }

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
        keyMenu.addItem(200 + ki++, name, true, state.activeCategory != "scale" && state.keyName == name);
    menu.addSubMenu("Key", keyMenu);

    juce::PopupMenu modeMenu;
    int mi = 0;
    for (auto& [name, _] : MusicTheory::modes())
        modeMenu.addItem(300 + mi++, name, true, state.activeCategory != "scale" && state.modeName == name);
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

    // Loop region — define / adjust the A-B playback loop directly from the
    // grid. The convenience items (selection / clip / whole song) let the user
    // define a loop in one click instead of placing start and end separately.
    {
        juce::PopupMenu loopMenu;
        bool haveSel = !state.selected.empty();
        bool clipHere = false;
        for (auto& c : node->clips)
            if (lastClickBeat >= c.startBeat && lastClickBeat < c.startBeat + c.lengthBeats)
                { clipHere = true; break; }
        loopMenu.addItem(91, "Loop Selected Notes", haveSel);
        loopMenu.addItem(92, "Loop Clip Under Cursor", clipHere);
        loopMenu.addItem(93, "Loop Whole Song");
        loopMenu.addSeparator();
        loopMenu.addItem(95, "Set Loop Start Here");
        loopMenu.addItem(96, "Set Loop End Here");
        loopMenu.addSeparator();
        loopMenu.addItem(98, "Clear Loop Region", transport && transport->loopEnabled);
        menu.addSubMenu("Loop Region", loopMenu);
    }

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
    clipMenu.addItem(97, "Add Marker Here...");
    menu.addSubMenu("Clip", clipMenu);

    // Time-gated effect layers used to be created from a submenu here. That was
    // the feature's only entry point and, buried mid-menu with no visual trace
    // anywhere else, nobody found it. They now live in the always-visible
    // Effect Regions lane above the track-header strip, which owns both the
    // display and the add/delete/move/resize interactions (see paintFxLane /
    // showFxLaneMenu). This item just points at it so anyone who learned the
    // old location isn't stranded.
    menu.addItem(4999, juce::String::fromUTF8(
        "Edit Effect Layers\xe2\x80\xa6 (opens the Effects lane above the grid)"));

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
                    // Grow an explicit song-length override so a note added
                    // past the song end plays (no-op in auto mode).
                    graph.growSongLengthToContent();
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
            case 91: { // Loop selected notes (span the selection's beat extent)
                float minB = 1e9f, maxB = -1e9f;
                for (auto& [ci, ni] : state.selected) {
                    if (ci < 0 || ci >= (int)node->clips.size()) continue;
                    auto& clip = node->clips[ci];
                    if (ni < 0 || ni >= (int)clip.notes.size()) continue;
                    auto& nt = clip.notes[ni];
                    float s = clip.startBeat + nt.offset;
                    minB = std::min(minB, s);
                    maxB = std::max(maxB, s + nt.duration);
                }
                if (maxB > minB) {
                    graph.loopStartBeat = minB + node->absoluteBeatOffset;
                    graph.loopEndBeat   = maxB + node->absoluteBeatOffset;
                    graph.loopEnabled = true;
                    if (transport) {
                        transport->loopEnabled = true;
                        transport->loopStartBeat = graph.loopStartBeat;
                        transport->loopEndBeat = graph.loopEndBeat;
                    }
                    graph.commitSnapshot("Loop selected notes");
                }
                break;
            }
            case 92: { // Loop the clip under the cursor
                for (auto& c : node->clips)
                    if (lastClickBeat >= c.startBeat && lastClickBeat < c.startBeat + c.lengthBeats) {
                        graph.loopStartBeat = c.startBeat + node->absoluteBeatOffset;
                        graph.loopEndBeat   = c.startBeat + c.lengthBeats + node->absoluteBeatOffset;
                        graph.loopEnabled = true;
                        if (transport) {
                            transport->loopEnabled = true;
                            transport->loopStartBeat = graph.loopStartBeat;
                            transport->loopEndBeat = graph.loopEndBeat;
                        }
                        graph.commitSnapshot("Loop clip");
                        break;
                    }
                break;
            }
            case 93: { // Loop the whole song (full project length, mirrors the Loop button)
                float maxBeat = 0;
                for (auto& n : graph.nodes)
                    for (auto& c : n.clips)
                        maxBeat = std::max(maxBeat, c.startBeat + c.lengthBeats);
                if (maxBeat <= 0) maxBeat = 4;
                graph.loopStartBeat = 0;
                graph.loopEndBeat = maxBeat;
                graph.loopEnabled = true;
                if (transport) {
                    transport->loopEnabled = true;
                    transport->loopStartBeat = graph.loopStartBeat;
                    transport->loopEndBeat = graph.loopEndBeat;
                }
                graph.commitSnapshot("Loop whole song");
                break;
            }
            case 95: // Set loop start
                graph.loopStartBeat = lastClickBeat + node->absoluteBeatOffset;
                // Keep end strictly after start so the region stays valid.
                if (graph.loopEndBeat <= graph.loopStartBeat)
                    graph.loopEndBeat = graph.loopStartBeat + std::max(state.snap, 0.25f);
                graph.loopEnabled = true;
                if (transport) {
                    transport->loopEnabled = true;
                    transport->loopStartBeat = graph.loopStartBeat;
                    transport->loopEndBeat = graph.loopEndBeat;
                }
                graph.commitSnapshot("Set loop start");
                break;
            case 96: // Set loop end
                graph.loopEndBeat = lastClickBeat + node->absoluteBeatOffset;
                if (graph.loopEndBeat <= graph.loopStartBeat)
                    graph.loopStartBeat = std::max(0.0, graph.loopEndBeat - std::max((double)state.snap, 0.25));
                graph.loopEnabled = true;
                if (transport) {
                    transport->loopEnabled = true;
                    transport->loopStartBeat = graph.loopStartBeat;
                    transport->loopEndBeat = graph.loopEndBeat;
                }
                graph.commitSnapshot("Set loop end");
                break;
            case 98: // Disable / clear the loop region
                graph.loopEnabled = false;
                if (transport) transport->loopEnabled = false;
                graph.commitSnapshot("Clear loop region");
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
                        if (m.category == "scale") {
                            state.activeCategory = "scale"; state.scaleName = m.scaleName;
                        } else if (m.category == "mode") {
                            state.activeCategory = "keymode"; state.keyName = "Major"; state.modeName = m.scaleName;
                        } else {
                            state.activeCategory = "keymode"; state.keyName = m.scaleName; state.modeName = "Ionian (Major)";
                        }
                        syncScaleUI();
                        repaint();
                    }
                });
                break;
            }
            default:
                if (result >= 100 && result < 112) { state.keyRoot = result - 100; syncScaleUI(); }
                else if (result >= 200 && result < 300) {
                    int i = 0; for (auto& [name, _] : MusicTheory::keys()) {
                        if (i++ == result - 200) { state.activeCategory = "keymode"; state.keyName = name; break; }
                    }
                    syncScaleUI();
                } else if (result >= 300 && result < 400) {
                    int i = 0; for (auto& [name, _] : MusicTheory::modes()) {
                        if (i++ == result - 300) { state.activeCategory = "keymode"; state.modeName = name; break; }
                    }
                    syncScaleUI();
                } else if (result >= 400 && result < 500) {
                    int i = 0; for (auto& [name, _] : MusicTheory::scales()) {
                        if (i++ == result - 400) { state.activeCategory = "scale"; state.scaleName = name; break; }
                    }
                    syncScaleUI();
                } else if (result == 4999) {
                    // Breadcrumb from the old "Effect Regions" submenu: expand
                    // the lane, which is now where layers are created and edited.
                    setFxLaneExpanded(true);
                }
                break;
        }
        repaint();
    });
}

} // namespace SoundShop

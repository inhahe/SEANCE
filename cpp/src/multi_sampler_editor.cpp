#include "multi_sampler_editor.h"
#include "pitch_detect.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>

namespace SoundShop {

MultiSamplerEditorComponent::MultiSamplerEditorComponent(NodeGraph& g, int nid, AudioEngine& ae)
    : graph(g), nodeId(nid), audioEngine(ae)
{
    reloadFromNode();

    // ---- top row ----
    addAndMakeVisible(titleLabel);
    titleLabel.setFont(juce::Font(14.0f, juce::Font::bold));
    if (auto* nd = graph.findNode(nodeId))
        titleLabel.setText(juce::String(nd->name), juce::dontSendNotification);

    addAndMakeVisible(closeBtn);
    closeBtn.setTooltip("Close the sampler editor. Changes are committed on every field edit - "
                        "there's no separate 'Apply' step.");
    closeBtn.onClick = [this]() {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
            juce::Component::SafePointer<juce::DialogWindow> safe(dw);
            juce::MessageManager::callAsync([safe]() {
                if (safe) delete safe.getComponent();
            });
        }
    };

    auto setupSlider = [this](juce::Slider& s, juce::Label& l, const juce::String& name,
                               double lo, double hi, double val, const juce::String& suf,
                               const juce::String& tip) {
        addAndMakeVisible(s);
        addAndMakeVisible(l);
        l.setText(name, juce::dontSendNotification);
        l.setFont(11.0f);
        s.setRange(lo, hi);
        s.setValue(val, juce::dontSendNotification);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 18);
        if (suf.isNotEmpty()) s.setTextValueSuffix(suf);
        s.setTooltip(tip);
    };

    // Volume and Pan are stored as real node.params (not in the doc)
    // so automation lanes and signal cables can target them. Read/write
    // the current node via helper lambdas.
    auto readParam = [this](const char* name, float def) -> float {
        if (auto* nd = graph.findNode(nodeId))
            for (auto& p : nd->params) if (p.name == name) return p.value;
        return def;
    };
    auto writeParam = [this](const char* name, float val, float lo, float hi) {
        if (auto* nd = graph.findNode(nodeId)) {
            for (auto& p : nd->params) {
                if (p.name == name) { p.value = val; return; }
            }
            nd->params.push_back({name, val, lo, hi});
        }
    };

    setupSlider(volumeSlider, volumeLabel, "Volume", 0.0, 1.0,
                readParam("Volume", 0.5f), "",
                "Global output gain for this instrument (0..1). Exposed as a "
                "real param so automation lanes and Signal cables can drive it.");
    volumeSlider.onValueChange = [this, writeParam]() {
        writeParam("Volume", (float)volumeSlider.getValue(), 0.0f, 1.0f);
        graph.dirty = true;
    };

    setupSlider(panSlider, panLabel, "Pan", -1.0, 1.0,
                readParam("Pan", 0.0f), "",
                "Global stereo position. -1 = hard left, 0 = center, 1 = hard right. "
                "Exposed as a real param so automation lanes can drive it.");
    panSlider.onValueChange = [this, writeParam]() {
        writeParam("Pan", (float)panSlider.getValue(), -1.0f, 1.0f);
        graph.dirty = true;
    };

    setupSlider(attackSlider, attackLabel, "Attack",  0.001, 2.0, doc.attack, " s",
                "Volume envelope attack time in seconds. Used only when no explicit "
                "Volume Envelope breakpoint list is set.");
    attackSlider.onValueChange = [this]() { doc.attack = (float)attackSlider.getValue(); commit(); };

    setupSlider(decaySlider, decayLabel, "Decay", 0.001, 5.0, doc.decay, " s",
                "Volume envelope decay time in seconds.");
    decaySlider.onValueChange = [this]() { doc.decay = (float)decaySlider.getValue(); commit(); };

    setupSlider(sustainSlider, sustainLabel, "Sustain", 0.0, 1.0, doc.sustain, "",
                "Volume envelope sustain level, 0..1. Note holds at this level "
                "until the note-off event.");
    sustainSlider.onValueChange = [this]() { doc.sustain = (float)sustainSlider.getValue(); commit(); };

    setupSlider(releaseSlider, releaseLabel, "Release", 0.001, 10.0, doc.release, " s",
                "Volume envelope release time in seconds. After note-off the level "
                "ramps from its current value to zero over this duration.");
    releaseSlider.onValueChange = [this]() { doc.release = (float)releaseSlider.getValue(); commit(); };

    // Filter.
    setupSlider(filterCutoffSlider, filterCutoffLabel, "Cutoff", 20.0, 20000.0,
                doc.filterCutoff, " Hz",
                "Filter cutoff frequency in Hz. Only active when Filter Mode is not Off.");
    filterCutoffSlider.setSkewFactorFromMidPoint(1000.0);
    filterCutoffSlider.onValueChange = [this]() {
        doc.filterCutoff = (float)filterCutoffSlider.getValue(); commit();
    };

    setupSlider(filterResSlider, filterResLabel, "Res", 0.0, 0.99, doc.filterResonance, "",
                "Filter resonance / Q. Higher values emphasize frequencies near the cutoff.");
    filterResSlider.onValueChange = [this]() {
        doc.filterResonance = (float)filterResSlider.getValue(); commit();
    };

    addAndMakeVisible(filterModeCombo);
    addAndMakeVisible(filterModeLabel);
    filterModeLabel.setText("Mode", juce::dontSendNotification);
    filterModeLabel.setFont(11.0f);
    filterModeCombo.addItem("Low Pass",  1);
    filterModeCombo.addItem("High Pass", 2);
    filterModeCombo.addItem("Band Pass", 3);
    filterModeCombo.addItem("Off",       4);
    filterModeCombo.setSelectedId(doc.filterMode + 1, juce::dontSendNotification);
    filterModeCombo.setTooltip("Filter type. Off disables filtering entirely (cheaper).");
    filterModeCombo.onChange = [this]() {
        doc.filterMode = filterModeCombo.getSelectedId() - 1;
        commit();
    };

    // ---- zone list ----
    addAndMakeVisible(zoneList);
    zoneList.setModel(this);
    zoneList.setColour(juce::ListBox::backgroundColourId, juce::Colour(18, 20, 28));

    addAndMakeVisible(addZoneBtn);
    addZoneBtn.setTooltip("Add a new zone covering the full note/velocity range. "
                          "Use Load... to assign a sample to it.");
    addZoneBtn.onClick = [this]() { addZone(); };

    addAndMakeVisible(removeZoneBtn);
    removeZoneBtn.setTooltip("Delete the currently selected zone.");
    removeZoneBtn.onClick = [this]() { removeSelectedZone(); };

    // ---- per-zone panel ----
    addAndMakeVisible(zonePathLabel);
    zonePathLabel.setText("Sample:", juce::dontSendNotification);
    zonePathLabel.setFont(11.0f);
    addAndMakeVisible(zonePathValue);
    zonePathValue.setFont(10.0f);
    zonePathValue.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible(loadSampleBtn);
    loadSampleBtn.setTooltip("Load an audio file (WAV, MP3, AIFF, FLAC, OGG) as the sample "
                             "for the currently selected zone.");
    loadSampleBtn.onClick = [this]() { loadSampleForSelectedZone(); };

    addAndMakeVisible(baseNoteLabel);
    baseNoteLabel.setText("Base Note:", juce::dontSendNotification);
    baseNoteLabel.setFont(11.0f);
    addAndMakeVisible(baseNoteCombo);
    populateNoteCombo(baseNoteCombo);
    baseNoteCombo.setTooltip("The MIDI note at which the sample plays at its original pitch. "
                             "When you trigger another note, the sample is pitch-shifted up or "
                             "down relative to this base.");
    baseNoteCombo.onChange = [this]() {
        if (selectedZone < 0 || selectedZone >= (int)doc.zones.size()) return;
        doc.zones[selectedZone].baseNote = baseNoteCombo.getSelectedId() - 1;
        commit();
    };

    setupSlider(fineTuneSlider, fineTuneLabel, "Fine Tune", -100.0, 100.0, 0, " cents",
                "Pitch correction in cents (1/100 of a semitone). Useful when the "
                "recorded sample is slightly flat or sharp.");
    fineTuneSlider.onValueChange = [this]() {
        if (selectedZone < 0 || selectedZone >= (int)doc.zones.size()) return;
        doc.zones[selectedZone].fineTuneCents = (float)fineTuneSlider.getValue();
        commit();
    };

    addAndMakeVisible(loNoteLabel);
    loNoteLabel.setText("Lo Note:", juce::dontSendNotification);
    loNoteLabel.setFont(11.0f);
    addAndMakeVisible(loNoteCombo);
    populateNoteCombo(loNoteCombo);
    loNoteCombo.setTooltip("Lowest MIDI note that triggers this zone. Notes below this play "
                           "a different zone (if any) or fall silent.");
    loNoteCombo.onChange = [this]() {
        if (selectedZone < 0 || selectedZone >= (int)doc.zones.size()) return;
        doc.zones[selectedZone].loNote = loNoteCombo.getSelectedId() - 1;
        commit();
        zoneList.updateContent();
        zoneList.repaint();
    };

    addAndMakeVisible(hiNoteLabel);
    hiNoteLabel.setText("Hi Note:", juce::dontSendNotification);
    hiNoteLabel.setFont(11.0f);
    addAndMakeVisible(hiNoteCombo);
    populateNoteCombo(hiNoteCombo);
    hiNoteCombo.setTooltip("Highest MIDI note that triggers this zone.");
    hiNoteCombo.onChange = [this]() {
        if (selectedZone < 0 || selectedZone >= (int)doc.zones.size()) return;
        doc.zones[selectedZone].hiNote = hiNoteCombo.getSelectedId() - 1;
        commit();
        zoneList.updateContent();
        zoneList.repaint();
    };

    setupSlider(loVelSlider, loVelLabel, "Lo Vel", 1.0, 127.0, 1, "",
                "Lowest MIDI velocity that triggers this zone. Use velocity ranges "
                "to layer soft vs. loud samples.");
    loVelSlider.onValueChange = [this]() {
        if (selectedZone < 0 || selectedZone >= (int)doc.zones.size()) return;
        doc.zones[selectedZone].loVel = (int)loVelSlider.getValue();
        commit();
    };

    setupSlider(hiVelSlider, hiVelLabel, "Hi Vel", 1.0, 127.0, 127, "",
                "Highest MIDI velocity that triggers this zone.");
    hiVelSlider.onValueChange = [this]() {
        if (selectedZone < 0 || selectedZone >= (int)doc.zones.size()) return;
        doc.zones[selectedZone].hiVel = (int)hiVelSlider.getValue();
        commit();
    };

    setupSlider(zoneGainSlider, zoneGainLabel, "Gain", -24.0, 12.0, 0, " dB",
                "Per-zone gain in decibels. Use to balance loudness between zones "
                "that come from different source recordings.");
    zoneGainSlider.onValueChange = [this]() {
        if (selectedZone < 0 || selectedZone >= (int)doc.zones.size()) return;
        doc.zones[selectedZone].gainDb = (float)zoneGainSlider.getValue();
        commit();
    };

    setupSlider(zonePanSlider, zonePanLabel, "Zone Pan", -1.0, 1.0, 0, "",
                "Per-zone stereo position. Adds to the global pan at playback time.");
    zonePanSlider.onValueChange = [this]() {
        if (selectedZone < 0 || selectedZone >= (int)doc.zones.size()) return;
        doc.zones[selectedZone].pan = (float)zonePanSlider.getValue();
        commit();
    };

    refreshZonePanel();
    setSize(780, 540);
}

void MultiSamplerEditorComponent::populateNoteCombo(juce::ComboBox& c) const {
    for (int i = 0; i < 128; ++i)
        c.addItem(midiNoteFullName(i), i + 1);
}

void MultiSamplerEditorComponent::reloadFromNode() {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;
    doc.decode(nd->script);
    if (doc.zones.empty()) {
        doc.zones.emplace_back(); // at least one zone so the UI has something to show
    }
    selectedZone = 0;
}

void MultiSamplerEditorComponent::commit() {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;
    nd->script = doc.encode();
    graph.dirty = true;
}

int MultiSamplerEditorComponent::getNumRows() {
    return (int)doc.zones.size();
}

void MultiSamplerEditorComponent::paintListBoxItem(int row, juce::Graphics& g,
                                                    int width, int height, bool selected) {
    if (row < 0 || row >= (int)doc.zones.size()) return;
    if (selected) g.fillAll(juce::Colour(60, 80, 140));
    auto& z = doc.zones[row];
    juce::String leaf = juce::File(juce::String(z.samplePath)).getFileName();
    if (leaf.isEmpty()) leaf = "(no sample)";
    juce::String label = juce::String(midiNoteFullName(z.loNote)) + "-"
                        + juce::String(midiNoteFullName(z.hiNote)) + " : " + leaf;
    g.setColour(juce::Colours::white);
    g.setFont(11.5f);
    g.drawText(label, 6, 0, width - 10, height, juce::Justification::centredLeft);
}

void MultiSamplerEditorComponent::selectedRowsChanged(int lastRow) {
    selectedZone = lastRow;
    refreshZonePanel();
}

void MultiSamplerEditorComponent::refreshZonePanel() {
    bool valid = selectedZone >= 0 && selectedZone < (int)doc.zones.size();
    zonePathValue.setText(valid ? juce::String(doc.zones[selectedZone].samplePath) : juce::String{},
                          juce::dontSendNotification);
    if (valid) {
        auto& z = doc.zones[selectedZone];
        baseNoteCombo.setSelectedId(z.baseNote + 1, juce::dontSendNotification);
        fineTuneSlider.setValue(z.fineTuneCents,  juce::dontSendNotification);
        loNoteCombo.setSelectedId(z.loNote + 1,   juce::dontSendNotification);
        hiNoteCombo.setSelectedId(z.hiNote + 1,   juce::dontSendNotification);
        loVelSlider.setValue(z.loVel,             juce::dontSendNotification);
        hiVelSlider.setValue(z.hiVel,             juce::dontSendNotification);
        zoneGainSlider.setValue(z.gainDb,         juce::dontSendNotification);
        zonePanSlider.setValue(z.pan,             juce::dontSendNotification);
    }
}

void MultiSamplerEditorComponent::loadSampleForSelectedZone() {
    if (selectedZone < 0 || selectedZone >= (int)doc.zones.size()) return;
    auto chooser = std::make_shared<juce::FileChooser>(
        "Load Sample for Zone", juce::File(), "*.wav;*.mp3;*.aiff;*.flac;*.ogg");
    chooser->launchAsync(juce::FileBrowserComponent::openMode,
        [this, chooser](const juce::FileChooser& fc) {
            auto file = fc.getResult();
            if (!file.existsAsFile()) return;
            if (selectedZone < 0 || selectedZone >= (int)doc.zones.size()) return;
            doc.zones[selectedZone].samplePath = file.getFullPathName().toStdString();
            commit();
            refreshZonePanel();
            zoneList.updateContent();
            zoneList.repaint();
        });
}

void MultiSamplerEditorComponent::addZone() {
    MultiSamplerZone z;
    z.loNote = 0; z.hiNote = 127;
    z.loVel = 1; z.hiVel = 127;
    z.baseNote = 60;
    doc.zones.push_back(z);
    selectedZone = (int)doc.zones.size() - 1;
    commit();
    zoneList.updateContent();
    zoneList.selectRow(selectedZone);
    refreshZonePanel();
}

void MultiSamplerEditorComponent::removeSelectedZone() {
    if (selectedZone < 0 || selectedZone >= (int)doc.zones.size()) return;
    doc.zones.erase(doc.zones.begin() + selectedZone);
    if (doc.zones.empty()) doc.zones.emplace_back(); // keep at least one
    selectedZone = std::min(selectedZone, (int)doc.zones.size() - 1);
    commit();
    zoneList.updateContent();
    zoneList.selectRow(selectedZone);
    refreshZonePanel();
}

void MultiSamplerEditorComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(22, 22, 28));
    g.setColour(juce::Colour(40, 42, 52));
    g.drawHorizontalLine(40, 0.0f, (float)getWidth());
    g.drawHorizontalLine(100, 0.0f, (float)getWidth());
}

void MultiSamplerEditorComponent::resized() {
    auto area = getLocalBounds().reduced(8);

    // Title row
    auto titleRow = area.removeFromTop(28);
    titleLabel.setBounds(titleRow.removeFromLeft(300));
    closeBtn.setBounds(titleRow.removeFromRight(70).reduced(0, 2));
    area.removeFromTop(4);

    // Global params (two rows: volume/pan + ADSR, then filter)
    auto row1 = area.removeFromTop(26);
    auto place = [&](juce::Label& l, juce::Slider& s, int wLbl, int wSlider,
                     juce::Rectangle<int>& r) {
        l.setBounds(r.removeFromLeft(wLbl));
        s.setBounds(r.removeFromLeft(wSlider));
        r.removeFromLeft(4);
    };
    place(volumeLabel, volumeSlider,    46, 110, row1);
    place(panLabel,    panSlider,       26, 110, row1);
    place(attackLabel, attackSlider,    42, 110, row1);
    place(decayLabel,  decaySlider,     38, 110, row1);
    place(sustainLabel, sustainSlider,  52, 110, row1);
    place(releaseLabel, releaseSlider,  48, 110, row1);

    auto row2 = area.removeFromTop(26);
    place(filterCutoffLabel, filterCutoffSlider, 42, 140, row2);
    place(filterResLabel,    filterResSlider,    26, 110, row2);
    filterModeLabel.setBounds(row2.removeFromLeft(34));
    filterModeCombo.setBounds(row2.removeFromLeft(100).reduced(0, 2));

    area.removeFromTop(8);

    // Zone list + per-zone panel side by side
    auto listArea = area.removeFromLeft(300);
    auto zoneBtns = listArea.removeFromBottom(26);
    addZoneBtn.setBounds(zoneBtns.removeFromLeft(74).reduced(0, 2));
    zoneBtns.removeFromLeft(4);
    removeZoneBtn.setBounds(zoneBtns.removeFromLeft(74).reduced(0, 2));
    // Trim a bit of right/bottom padding so the list doesn't butt up
    // against the per-zone panel next to it.
    listArea.removeFromRight(8);
    listArea.removeFromBottom(4);
    zoneList.setBounds(listArea);

    auto panel = area;
    auto panelRow = [&](int h) { return panel.removeFromTop(h); };

    // Sample path + Load button
    {
        auto r = panelRow(26);
        zonePathLabel.setBounds(r.removeFromLeft(60));
        loadSampleBtn.setBounds(r.removeFromLeft(80).reduced(0, 2));
        r.removeFromLeft(6);
        zonePathValue.setBounds(r);
    }
    panel.removeFromTop(4);
    // Base Note + Fine Tune
    {
        auto r = panelRow(26);
        baseNoteLabel.setBounds(r.removeFromLeft(70));
        baseNoteCombo.setBounds(r.removeFromLeft(120).reduced(0, 2));
        r.removeFromLeft(10);
        fineTuneLabel.setBounds(r.removeFromLeft(68));
        fineTuneSlider.setBounds(r.removeFromLeft(140));
    }
    panel.removeFromTop(4);
    // Lo Note + Hi Note
    {
        auto r = panelRow(26);
        loNoteLabel.setBounds(r.removeFromLeft(56));
        loNoteCombo.setBounds(r.removeFromLeft(100).reduced(0, 2));
        r.removeFromLeft(10);
        hiNoteLabel.setBounds(r.removeFromLeft(56));
        hiNoteCombo.setBounds(r.removeFromLeft(100).reduced(0, 2));
    }
    panel.removeFromTop(4);
    // Lo Vel + Hi Vel
    {
        auto r = panelRow(26);
        loVelLabel.setBounds(r.removeFromLeft(46));
        loVelSlider.setBounds(r.removeFromLeft(140));
        r.removeFromLeft(10);
        hiVelLabel.setBounds(r.removeFromLeft(46));
        hiVelSlider.setBounds(r.removeFromLeft(140));
    }
    panel.removeFromTop(4);
    // Zone Gain + Zone Pan
    {
        auto r = panelRow(26);
        zoneGainLabel.setBounds(r.removeFromLeft(44));
        zoneGainSlider.setBounds(r.removeFromLeft(140));
        r.removeFromLeft(10);
        zonePanLabel.setBounds(r.removeFromLeft(64));
        zonePanSlider.setBounds(r.removeFromLeft(140));
    }
}

} // namespace SoundShop

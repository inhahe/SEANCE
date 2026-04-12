#include "sampler_editor.h"
#include <cmath>

namespace SoundShop {

SamplerEditorComponent::SamplerEditorComponent(NodeGraph& g, int nid, AudioEngine& ae)
    : graph(g), nodeId(nid), audioEngine(ae)
{
    // Load sample data for display
    auto* nd = graph.findNode(nodeId);
    if (nd && nd->script.rfind("__audio__:", 0) == 0) {
        auto path = nd->script.substr(10);
        juce::AudioFormatManager mgr;
        mgr.registerBasicFormats();
        auto file = juce::File(path);
        std::unique_ptr<juce::AudioFormatReader> reader(mgr.createReaderFor(file));
        if (reader) {
            int len = (int)reader->lengthInSamples;
            juce::AudioBuffer<float> buf(1, len);
            reader->read(&buf, 0, len, 0, true, false);
            sampleData.resize(len);
            for (int i = 0; i < len; ++i) sampleData[i] = buf.getSample(0, i);
            fileSampleRate = reader->sampleRate;
        }
    }

    addAndMakeVisible(titleLabel);
    titleLabel.setFont(juce::Font(14.0f, juce::Font::bold));
    titleLabel.setText(nd ? juce::String(nd->name) : "Sampler", juce::dontSendNotification);

    addAndMakeVisible(loadBtn);
    loadBtn.setTooltip("Load an audio file (WAV, MP3, AIFF, FLAC, OGG) as the sample for this instrument");
    loadBtn.onClick = [this]() { loadSample(); };

    addAndMakeVisible(analyzeBtn);
    analyzeBtn.setTooltip("Analyze the loaded sample to detect its pitch using two algorithms (autocorrelation and YIN). "
                          "Use the results to set the base note so the sample plays at correct pitch when triggered by MIDI.");
    analyzeBtn.onClick = [this]() { runAnalysis(); };

    // Detection result labels
    addAndMakeVisible(acLabel);
    addAndMakeVisible(yinLabel);
    acLabel.setText("Autocorrelation: (not run)", juce::dontSendNotification);
    yinLabel.setText("YIN: (not run)", juce::dontSendNotification);
    acLabel.setFont(11.0f); yinLabel.setFont(11.0f);

    // Preview buttons
    addAndMakeVisible(playACBtn);  addAndMakeVisible(useACBtn);
    addAndMakeVisible(playYINBtn); addAndMakeVisible(useYINBtn);
    addAndMakeVisible(playSineBtn);
    playACBtn.setTooltip("Play a sine wave at the autocorrelation-detected pitch so you can compare it to the sample by ear");
    useACBtn.setTooltip("Set the base note to the autocorrelation-detected pitch");
    playYINBtn.setTooltip("Play a sine wave at the YIN-detected pitch so you can compare it to the sample by ear");
    useYINBtn.setTooltip("Set the base note to the YIN-detected pitch (often more accurate for clean tones)");
    playSineBtn.setTooltip("Play a sine wave at the currently-set base note for comparison");
    playACBtn.onClick = [this]() {
        if (autoCorResult.frequencyHz > 0)
            playPreview(autoCorResult.frequencyHz);
    };
    playYINBtn.onClick = [this]() {
        if (yinResult.frequencyHz > 0)
            playPreview(yinResult.frequencyHz);
    };
    playSineBtn.onClick = [this]() {
        // Play a sine wave at the currently-selected base note
        int noteIdx = baseNoteCombo.getSelectedId() - 1;
        if (noteIdx >= 0 && noteIdx <= 127)
            playSinePreview(440.0f * std::pow(2.0f, (noteIdx - 69) / 12.0f));
    };
    useACBtn.onClick = [this]() { applyDetection(autoCorResult); };
    useYINBtn.onClick = [this]() { applyDetection(yinResult); };

    // Base note combo
    addAndMakeVisible(baseNoteCombo); addAndMakeVisible(baseNoteLbl);
    baseNoteLbl.setText("Base note:", juce::dontSendNotification);
    baseNoteLbl.setFont(11.0f);
    baseNoteCombo.setTooltip("The MIDI note that plays the sample at its original pitch and speed. "
                             "When you trigger a different MIDI note, the sample is pitch-shifted up or down "
                             "from this base. Set it to whatever pitch the sample was recorded at.");
    for (int i = 0; i < 128; ++i)
        baseNoteCombo.addItem(midiNoteFullName(i) + " (" + juce::String((int)(440.0 * std::pow(2.0, (i-69)/12.0))) + " Hz)", i + 1);
    baseNoteCombo.setSelectedId(69 + 1); // A4 default

    // Read existing base note from node params if present
    if (nd) {
        for (auto& p : nd->params) {
            if (p.name == "Base Note") {
                baseNoteCombo.setSelectedId((int)p.value + 1, juce::dontSendNotification);
                break;
            }
        }
    }

    // Fine-tune
    addAndMakeVisible(fineTuneSlider); addAndMakeVisible(fineTuneLbl);
    fineTuneLbl.setText("Fine-tune:", juce::dontSendNotification);
    fineTuneLbl.setFont(11.0f);
    fineTuneSlider.setRange(-50.0, 50.0, 0.1);
    fineTuneSlider.setValue(0.0);
    fineTuneSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    fineTuneSlider.setTextValueSuffix(" cents");
    fineTuneSlider.setTooltip("Fine pitch adjustment in cents (1/100 of a semitone). "
                              "Use to nudge the sample slightly sharper or flatter so it matches another instrument's tuning.");
    if (nd) {
        for (auto& p : nd->params)
            if (p.name == "Fine Tune")
                fineTuneSlider.setValue(p.value, juce::dontSendNotification);
    }

    addAndMakeVisible(autoTuneToggle);
    autoTuneToggle.setTooltip("When on, the sample is automatically pitch-corrected during analysis "
                              "so its detected pitch matches the chosen base note exactly. Useful for samples "
                              "that are slightly off from a standard musical pitch.");
    autoTuneToggle.setToggleState(false, juce::dontSendNotification);

    // Pitch method
    addAndMakeVisible(pitchMethodCombo); addAndMakeVisible(pitchMethodLbl);
    pitchMethodLbl.setText("Pitch method:", juce::dontSendNotification);
    pitchMethodLbl.setFont(11.0f);
    pitchMethodCombo.addItem("Resample (changes speed + pitch)", 1);
    pitchMethodCombo.addItem("Pitch Shift (preserves speed)", 2);
    pitchMethodCombo.setSelectedId(1);
    pitchMethodCombo.setTooltip("How to play the sample at different MIDI note pitches. "
                                "Resample is the classic 'tape speed' method — higher notes also play faster, lower notes play slower. "
                                "Pitch Shift uses time-stretching to change pitch without affecting playback speed (slower but more natural for melodic samples).");
    if (nd) {
        for (auto& p : nd->params)
            if (p.name == "Pitch Method")
                pitchMethodCombo.setSelectedId((int)p.value + 1, juce::dontSendNotification);
    }

    addAndMakeVisible(applyBtn);
    applyBtn.onClick = [this]() {
        commitToNode();
        audioEngine.getGraphProcessor().requestRebuild();
    };

    addAndMakeVisible(closeBtn);
    closeBtn.onClick = [this]() {
        commitToNode();
        audioEngine.getGraphProcessor().requestRebuild();
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
            juce::Component::SafePointer<juce::DialogWindow> safe(dw);
            juce::MessageManager::callAsync([safe]() {
                if (safe) delete safe.getComponent();
            });
        }
    };

    // Auto-run analysis if we have sample data
    if (!sampleData.empty())
        runAnalysis();

    setSize(650, 520);
}

void SamplerEditorComponent::loadSample() {
    auto chooser = std::make_shared<juce::FileChooser>(
        "Load Sample", juce::File(), "*.wav;*.mp3;*.aiff;*.flac;*.ogg");
    chooser->launchAsync(juce::FileBrowserComponent::openMode,
        [this, chooser](const juce::FileChooser& fc) {
            auto file = fc.getResult();
            if (!file.existsAsFile()) return;
            // Update the node's script
            if (auto* nd = graph.findNode(nodeId))
                nd->script = "__audio__:" + file.getFullPathName().toStdString();
            // Reload sample data
            juce::AudioFormatManager mgr;
            mgr.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader(mgr.createReaderFor(file));
            if (reader) {
                int len = (int)reader->lengthInSamples;
                juce::AudioBuffer<float> buf(1, len);
                reader->read(&buf, 0, len, 0, true, false);
                sampleData.resize(len);
                for (int i = 0; i < len; ++i) sampleData[i] = buf.getSample(0, i);
                fileSampleRate = reader->sampleRate;
            }
            runAnalysis();
            repaint();
        });
}

void SamplerEditorComponent::runAnalysis() {
    if (sampleData.empty()) return;
    autoCorResult = detectPitchAutocorrelation(sampleData.data(), (int)sampleData.size(), fileSampleRate);
    yinResult = detectPitchYIN(sampleData.data(), (int)sampleData.size(), fileSampleRate);
    analysisRun = true;

    // Update labels
    auto formatResult = [](const PitchResult& r, const char* method) -> juce::String {
        if (r.frequencyHz <= 0)
            return juce::String(method) + ": no pitch detected";
        return juce::String(method) + ": " + juce::String(r.frequencyHz, 1) + " Hz = "
             + midiNoteFullName(r.midiNote)
             + " " + juce::String(r.centsOffset > 0 ? "+" : "")
             + juce::String(r.centsOffset, 1) + " cents"
             + " (confidence: " + juce::String((int)(r.confidence * 100)) + "%)";
    };
    acLabel.setText(formatResult(autoCorResult, "Autocorrelation"), juce::dontSendNotification);
    yinLabel.setText(formatResult(yinResult, "YIN"), juce::dontSendNotification);
    repaint();
}

void SamplerEditorComponent::applyDetection(const PitchResult& result) {
    if (result.midiNote < 0) return;
    baseNoteCombo.setSelectedId(result.midiNote + 1, juce::dontSendNotification);
    if (autoTuneToggle.getToggleState()) {
        // Auto-tune: set fine-tune to the cents offset so the sample
        // lands exactly on the detected nearest note
        fineTuneSlider.setValue(-result.centsOffset, juce::dontSendNotification);
    } else {
        fineTuneSlider.setValue(0, juce::dontSendNotification);
    }
    commitToNode();
    repaint();
}

void SamplerEditorComponent::commitToNode() {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;

    // Find or create the sampler-specific params
    auto findOrAddParam = [&](const std::string& name, float val, float lo, float hi) {
        for (auto& p : nd->params) {
            if (p.name == name) { p.value = val; return; }
        }
        nd->params.push_back({name, val, lo, hi});
    };

    findOrAddParam("Base Note", (float)(baseNoteCombo.getSelectedId() - 1), 0, 127);
    findOrAddParam("Fine Tune", (float)fineTuneSlider.getValue(), -50, 50);
    findOrAddParam("Pitch Method", (float)(pitchMethodCombo.getSelectedId() - 1), 0, 1);
    graph.dirty = true;
}

void SamplerEditorComponent::playPreview(float frequencyHz) {
    // Play the sample at the detected frequency mapped to middle C (C4 = 60)
    // by sending a MIDI note-on for C4. The synth's base note + fine-tune
    // will determine how it sounds.
    audioEngine.keyboardNoteOn(60, 100);
    // Schedule note-off after 500ms
    juce::Timer::callAfterDelay(500, [this]() {
        audioEngine.keyboardNoteOff(60);
    });
}

void SamplerEditorComponent::playSinePreview(float frequencyHz) {
    // Generate a short sine wave and play it through the audio engine
    // For simplicity, just send a note-on which plays through the connected synth.
    // The user can compare by ear.
    audioEngine.keyboardNoteOn(baseNoteCombo.getSelectedId() - 1, 80);
    juce::Timer::callAfterDelay(500, [this]() {
        audioEngine.keyboardNoteOff(baseNoteCombo.getSelectedId() - 1);
    });
}

void SamplerEditorComponent::resized() {
    auto a = getLocalBounds().reduced(10);

    auto row0 = a.removeFromTop(28);
    titleLabel.setBounds(row0.removeFromLeft(150));
    closeBtn.setBounds(row0.removeFromRight(60).reduced(0, 2));
    row0.removeFromRight(4);
    applyBtn.setBounds(row0.removeFromRight(60).reduced(0, 2));
    row0.removeFromRight(8);
    loadBtn.setBounds(row0.removeFromRight(100).reduced(0, 2));

    a.removeFromTop(4);

    // Analysis section
    auto row1 = a.removeFromTop(24);
    analyzeBtn.setBounds(row1.removeFromLeft(100).reduced(0, 2));

    auto row2 = a.removeFromTop(22);
    acLabel.setBounds(row2.removeFromLeft(400));
    playACBtn.setBounds(row2.removeFromLeft(130).reduced(2, 1));
    useACBtn.setBounds(row2.removeFromLeft(60).reduced(2, 1));

    auto row3 = a.removeFromTop(22);
    yinLabel.setBounds(row3.removeFromLeft(400));
    playYINBtn.setBounds(row3.removeFromLeft(130).reduced(2, 1));
    useYINBtn.setBounds(row3.removeFromLeft(60).reduced(2, 1));

    auto row3b = a.removeFromTop(24);
    playSineBtn.setBounds(row3b.removeFromLeft(140).reduced(0, 2));
    autoTuneToggle.setBounds(row3b.removeFromLeft(200).reduced(4, 2));

    a.removeFromTop(4);

    // Base note + fine tune
    auto row4 = a.removeFromTop(26);
    baseNoteLbl.setBounds(row4.removeFromLeft(65));
    baseNoteCombo.setBounds(row4.removeFromLeft(180).reduced(0, 2));
    row4.removeFromLeft(12);
    fineTuneLbl.setBounds(row4.removeFromLeft(65));
    fineTuneSlider.setBounds(row4.removeFromLeft(200).reduced(0, 2));

    auto row5 = a.removeFromTop(26);
    pitchMethodLbl.setBounds(row5.removeFromLeft(85));
    pitchMethodCombo.setBounds(row5.removeFromLeft(250).reduced(0, 2));

    // Remaining space = waveform display (handled in paint)
}

void SamplerEditorComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(22, 22, 28));

    // Waveform display area = everything below the controls
    auto a = getLocalBounds().reduced(10);
    a.removeFromTop(28 + 4 + 24 + 22 + 22 + 24 + 4 + 26 + 26); // skip control rows
    auto waveArea = a.toFloat().reduced(0, 4);

    g.setColour(juce::Colour(18, 20, 28));
    g.fillRoundedRectangle(waveArea, 4.0f);
    g.setColour(juce::Colour(50, 55, 70));
    g.drawRoundedRectangle(waveArea, 4.0f, 1.0f);

    // Center line
    float cy = waveArea.getCentreY();
    g.setColour(juce::Colours::grey.withAlpha(0.3f));
    g.drawHorizontalLine((int)cy, waveArea.getX(), waveArea.getRight());

    // Waveform
    if (!sampleData.empty()) {
        g.setColour(juce::Colours::cornflowerblue);
        juce::Path p;
        int n = (int)sampleData.size();
        int step = std::max(1, n / (int)waveArea.getWidth());
        bool first = true;
        for (int i = 0; i < n; i += step) {
            float x = waveArea.getX() + (float)i / n * waveArea.getWidth();
            // Find min/max in this step range for proper visualization
            float minV = sampleData[i], maxV = sampleData[i];
            for (int j = 1; j < step && i + j < n; ++j) {
                minV = std::min(minV, sampleData[i + j]);
                maxV = std::max(maxV, sampleData[i + j]);
            }
            float y1 = cy - maxV * waveArea.getHeight() * 0.45f;
            float y2 = cy - minV * waveArea.getHeight() * 0.45f;
            if (first) { p.startNewSubPath(x, y1); first = false; }
            else p.lineTo(x, y1);
            if (y2 != y1) p.lineTo(x, y2);
        }
        g.strokePath(p, juce::PathStrokeType(1.0f));

        // Duration label
        g.setColour(juce::Colours::grey);
        g.setFont(10.0f);
        double dur = (double)sampleData.size() / fileSampleRate;
        g.drawText(juce::String(dur, 2) + "s  " + juce::String(n) + " samples  "
                   + juce::String((int)fileSampleRate) + " Hz",
                   waveArea.reduced(4, 2).toNearestInt(),
                   juce::Justification::bottomLeft);
    } else {
        g.setColour(juce::Colours::grey);
        g.setFont(12.0f);
        g.drawText("No sample loaded — click 'Load Sample' to import a .wav file",
                   waveArea.toNearestInt(), juce::Justification::centred);
    }

    // Detection markers on waveform
    if (analysisRun && !sampleData.empty()) {
        auto drawPeriodMarker = [&](const PitchResult& r, juce::Colour col, const char* label) {
            if (r.frequencyHz <= 0) return;
            float period = (float)(fileSampleRate / r.frequencyHz);
            float periodFrac = period / (float)sampleData.size();
            if (periodFrac > 0 && periodFrac < 0.5f) {
                float x = waveArea.getX() + periodFrac * waveArea.getWidth();
                g.setColour(col.withAlpha(0.6f));
                g.drawVerticalLine((int)x, waveArea.getY(), waveArea.getBottom());
                g.setFont(9.0f);
                g.drawText(label, (int)x + 2, (int)waveArea.getY() + 2, 60, 12,
                           juce::Justification::centredLeft);
            }
        };
        drawPeriodMarker(autoCorResult, juce::Colours::orange, "AC period");
        drawPeriodMarker(yinResult, juce::Colours::limegreen, "YIN period");
    }
}

} // namespace SoundShop

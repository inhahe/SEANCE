#define _USE_MATH_DEFINES
#include "convolution_editor.h"
#include "fft_util.h"
#include <cmath>
#include <algorithm>
#include <complex>

namespace SoundShop {

ConvolutionEditorComponent::ConvolutionEditorComponent(NodeGraph& g, int nid,
                                                        std::function<void()> apply)
    : graph(g), nodeId(nid), onApply(std::move(apply))
{
    // Decode existing IR from node
    if (auto* nd = graph.findNode(nodeId))
        ir = ConvolutionProcessor::decodeIR(nd->script);
    if (ir.empty()) ir = {1.0f}; // identity

    // Presets
    addAndMakeVisible(presetCombo);
    presetCombo.addItem("Custom", 1);
    presetCombo.addItem("Lowpass", 2);
    presetCombo.addItem("Highpass", 3);
    presetCombo.addItem("Bandpass", 4);
    presetCombo.addItem("Echo / Delay", 5);
    presetCombo.setSelectedId(1);
    presetCombo.onChange = [this]() { resized(); repaint(); };

    // Preset parameter sliders
    auto setupSlider = [this](juce::Slider& s, juce::Label& l, const char* name,
                              double lo, double hi, double def, const char* suffix) {
        addAndMakeVisible(s); addAndMakeVisible(l);
        s.setRange(lo, hi);
        s.setValue(def);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
        s.setTextValueSuffix(suffix);
        l.setText(name, juce::dontSendNotification);
        l.setFont(11.0f);
        l.setJustificationType(juce::Justification::centredRight);
    };
    setupSlider(cutoffSlider,    cutoffLbl,    "Cutoff:",      20, 20000, 2000, " Hz");
    setupSlider(orderSlider,     orderLbl,     "Steepness:",   1,  200,   32,   "");
    setupSlider(bandwidthSlider, bwLbl,        "Bandwidth:",   10, 10000, 500,  " Hz");
    setupSlider(delaySlider,     delayLbl,     "Delay:",       1,  2000,  200,  " ms");
    setupSlider(feedbackSlider,  fbLbl,        "Feedback:",    0,  0.99,  0.5,  "");
    setupSlider(echoCountSlider, echoLbl,      "Echoes:",      1,  20,    4,    "");

    addAndMakeVisible(applyPresetBtn);
    applyPresetBtn.onClick = [this]() { generateFromPreset(); };

    addAndMakeVisible(loadFileBtn);
    loadFileBtn.onClick = [this]() { loadFromFile(); };

    addAndMakeVisible(lengthSlider); addAndMakeVisible(lengthLbl);
    lengthLbl.setText("IR length:", juce::dontSendNotification);
    lengthLbl.setFont(11.0f);
    lengthSlider.setRange(1, 4096, 1);
    lengthSlider.setValue(ir.size());
    lengthSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);

    addAndMakeVisible(applyBtn);
    applyBtn.onClick = [this]() { commitIR(); if (onApply) onApply(); };

    addAndMakeVisible(closeBtn);
    closeBtn.onClick = [this]() {
        commitIR();
        if (onApply) onApply();
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
            juce::Component::SafePointer<juce::DialogWindow> safe(dw);
            juce::MessageManager::callAsync([safe]() {
                if (safe) delete safe.getComponent();
            });
        }
    };

    // Initialize control points from IR
    controlPoints.clear();
    int step = std::max(1, (int)ir.size() / 16);
    for (int i = 0; i < (int)ir.size(); i += step) {
        float x = (float)i / std::max(1.0f, (float)(ir.size() - 1));
        controlPoints.push_back({x, ir[i]});
    }

    updateFreqResponse();
    setSize(700, 500);
}

void ConvolutionEditorComponent::commitIR() {
    if (auto* nd = graph.findNode(nodeId))
        nd->script = ConvolutionProcessor::encodeIR(ir);
}

void ConvolutionEditorComponent::updateFreqResponse() {
    // FFT the IR to show the magnitude spectrum
    int n = 1;
    while (n < std::max(512, (int)ir.size())) n <<= 1;
    FFT fft(n);
    std::vector<float> padded(n, 0.0f);
    for (int i = 0; i < (int)ir.size() && i < n; ++i) padded[i] = ir[i];
    std::vector<std::complex<float>> freq;
    fft.forwardReal(padded, freq);
    freqResponse.resize(freq.size());
    for (int i = 0; i < (int)freq.size(); ++i)
        freqResponse[i] = std::abs(freq[i]);
    // Normalize
    float peak = 0;
    for (float v : freqResponse) peak = std::max(peak, v);
    if (peak > 1e-6f)
        for (float& v : freqResponse) v /= peak;
}

juce::Rectangle<float> ConvolutionEditorComponent::getIRArea() const {
    auto a = getLocalBounds().toFloat().reduced(10);
    a.removeFromTop(130); // preset controls
    auto bottom = a.removeFromBottom(a.getHeight() * 0.4f); // freq response
    a.removeFromBottom(6);
    a.removeFromBottom(28); // length slider + buttons
    return a;
}

juce::Rectangle<float> ConvolutionEditorComponent::getFreqArea() const {
    auto a = getLocalBounds().toFloat().reduced(10);
    a.removeFromTop(130);
    return a.removeFromBottom(a.getHeight() * 0.4f);
}

void ConvolutionEditorComponent::resized() {
    auto a = getLocalBounds().reduced(10);
    auto top = a.removeFromTop(28);
    presetCombo.setBounds(top.removeFromLeft(150).reduced(0, 2));
    top.removeFromLeft(8);
    applyPresetBtn.setBounds(top.removeFromLeft(70).reduced(0, 2));
    top.removeFromLeft(8);
    loadFileBtn.setBounds(top.removeFromLeft(100).reduced(0, 2));
    closeBtn.setBounds(top.removeFromRight(60).reduced(0, 2));
    top.removeFromRight(4);
    applyBtn.setBounds(top.removeFromRight(60).reduced(0, 2));

    // Preset-specific controls (second/third rows)
    int presetId = presetCombo.getSelectedId();
    auto hideAll = [&]() {
        cutoffSlider.setVisible(false); cutoffLbl.setVisible(false);
        orderSlider.setVisible(false);  orderLbl.setVisible(false);
        bandwidthSlider.setVisible(false); bwLbl.setVisible(false);
        delaySlider.setVisible(false);  delayLbl.setVisible(false);
        feedbackSlider.setVisible(false); fbLbl.setVisible(false);
        echoCountSlider.setVisible(false); echoLbl.setVisible(false);
    };
    hideAll();

    auto row2 = a.removeFromTop(26);
    auto row3 = a.removeFromTop(26);
    auto placeCtrl = [](juce::Label& l, juce::Slider& s, juce::Rectangle<int>& row, int lw, int sw) {
        l.setVisible(true); s.setVisible(true);
        l.setBounds(row.removeFromLeft(lw).reduced(0, 1));
        s.setBounds(row.removeFromLeft(sw).reduced(0, 1));
        row.removeFromLeft(8);
    };

    if (presetId == 2 || presetId == 3) { // Lowpass / Highpass
        placeCtrl(cutoffLbl, cutoffSlider, row2, 50, 200);
        placeCtrl(orderLbl, orderSlider, row2, 80, 150);
    } else if (presetId == 4) { // Bandpass
        placeCtrl(cutoffLbl, cutoffSlider, row2, 50, 180);
        placeCtrl(bwLbl, bandwidthSlider, row2, 75, 180);
        placeCtrl(orderLbl, orderSlider, row3, 80, 150);
    } else if (presetId == 5) { // Echo
        placeCtrl(delayLbl, delaySlider, row2, 45, 180);
        placeCtrl(fbLbl, feedbackSlider, row2, 65, 150);
        placeCtrl(echoLbl, echoCountSlider, row3, 55, 120);
    }

    // Length + buttons below the IR area
    a.removeFromTop(4);
    auto irArea = getIRArea(); // just for reference
    (void)irArea;
    auto btnRow = a.removeFromTop(26);
    // Place at absolute position to avoid accumulation issues
    int bx = getLocalBounds().getX() + 10;
    int by = getLocalBounds().getBottom() - 10 - (int)(getLocalBounds().getHeight() * 0.4f) - 32;
    lengthLbl.setBounds(bx, by, 60, 24);
    lengthSlider.setBounds(bx + 62, by, 200, 24);
}

void ConvolutionEditorComponent::generateFromPreset() {
    int presetId = presetCombo.getSelectedId();
    switch (presetId) {
        case 2: ir = ConvolutionProcessor::generateLowpass(
                    (float)cutoffSlider.getValue(), (int)orderSlider.getValue(), sampleRate); break;
        case 3: ir = ConvolutionProcessor::generateHighpass(
                    (float)cutoffSlider.getValue(), (int)orderSlider.getValue(), sampleRate); break;
        case 4: ir = ConvolutionProcessor::generateBandpass(
                    (float)cutoffSlider.getValue(), (float)bandwidthSlider.getValue(),
                    (int)orderSlider.getValue(), sampleRate); break;
        case 5: ir = ConvolutionProcessor::generateEcho(
                    (float)delaySlider.getValue(), (float)feedbackSlider.getValue(),
                    (int)echoCountSlider.getValue(), sampleRate); break;
        default: return;
    }
    lengthSlider.setValue(ir.size(), juce::dontSendNotification);
    updateFreqResponse();
    commitIR();
    if (onApply) onApply();
    repaint();
}

void ConvolutionEditorComponent::loadFromFile() {
    auto chooser = std::make_shared<juce::FileChooser>(
        "Load Impulse Response", juce::File(), "*.wav;*.aiff;*.flac");
    chooser->launchAsync(juce::FileBrowserComponent::openMode,
        [this, chooser](const juce::FileChooser& fc) {
            auto file = fc.getResult();
            if (!file.existsAsFile()) return;
            juce::AudioFormatManager mgr;
            mgr.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader(mgr.createReaderFor(file));
            if (!reader) return;
            int len = (int)reader->lengthInSamples;
            juce::AudioBuffer<float> buf(1, len);
            reader->read(&buf, 0, len, 0, true, false);
            ir.resize(len);
            for (int i = 0; i < len; ++i) ir[i] = buf.getSample(0, i);
            sampleRate = reader->sampleRate;
            lengthSlider.setValue(len, juce::dontSendNotification);
            updateFreqResponse();
            commitIR();
            if (onApply) onApply();
            repaint();
        });
}

void ConvolutionEditorComponent::mouseDown(const juce::MouseEvent& e) {
    auto area = getIRArea();
    if (!area.contains(e.position)) return;

    float x = (e.position.x - area.getX()) / area.getWidth();
    float y = 1.0f - 2.0f * (e.position.y - area.getY()) / area.getHeight(); // -1..1

    if (e.mods.isRightButtonDown()) {
        // Delete nearest point (keep minimum 2)
        if (controlPoints.size() <= 2) return;
        int best = -1; float bestD = 0.05f;
        for (int i = 0; i < (int)controlPoints.size(); ++i) {
            float dx = controlPoints[i].first - x;
            float dist = std::abs(dx);
            if (dist < bestD) { bestD = dist; best = i; }
        }
        if (best >= 0) {
            controlPoints.erase(controlPoints.begin() + best);
            renderFromControlPoints();
            repaint();
        }
        return;
    }

    // Find near existing point to drag
    int best = -1; float bestD = 0.03f;
    for (int i = 0; i < (int)controlPoints.size(); ++i) {
        float dist = std::abs(controlPoints[i].first - x);
        if (dist < bestD) { bestD = dist; best = i; }
    }
    if (best >= 0) {
        dragPointIdx = best;
    } else {
        // Add new point
        controlPoints.push_back({x, juce::jlimit(-1.0f, 1.0f, y)});
        std::sort(controlPoints.begin(), controlPoints.end());
        for (int i = 0; i < (int)controlPoints.size(); ++i)
            if (std::abs(controlPoints[i].first - x) < 0.001f) { dragPointIdx = i; break; }
        renderFromControlPoints();
        repaint();
    }
}

void ConvolutionEditorComponent::mouseDrag(const juce::MouseEvent& e) {
    if (dragPointIdx < 0 || dragPointIdx >= (int)controlPoints.size()) return;
    auto area = getIRArea();
    float x = juce::jlimit(0.0f, 1.0f, (e.position.x - area.getX()) / area.getWidth());
    float y = juce::jlimit(-1.0f, 1.0f, 1.0f - 2.0f * (e.position.y - area.getY()) / area.getHeight());
    controlPoints[dragPointIdx] = {x, y};
    std::sort(controlPoints.begin(), controlPoints.end());
    // Re-find after sort
    for (int i = 0; i < (int)controlPoints.size(); ++i)
        if (std::abs(controlPoints[i].first - x) < 0.001f &&
            std::abs(controlPoints[i].second - y) < 0.001f) { dragPointIdx = i; break; }
    renderFromControlPoints();
    repaint();
}

void ConvolutionEditorComponent::mouseUp(const juce::MouseEvent&) {
    if (dragPointIdx >= 0) {
        commitIR();
        if (onApply) onApply();
    }
    dragPointIdx = -1;
}

void ConvolutionEditorComponent::renderFromControlPoints() {
    int len = std::max(4, (int)lengthSlider.getValue());
    ir.resize(len, 0.0f);
    if (controlPoints.empty()) return;
    // Catmull-Rom through control points
    auto sorted = controlPoints;
    std::sort(sorted.begin(), sorted.end());
    for (int i = 0; i < len; ++i) {
        float x = (float)i / (float)(len - 1);
        // Find segment
        int i1 = 0;
        for (int j = 0; j < (int)sorted.size() - 1; ++j)
            if (sorted[j].first <= x) i1 = j;
        int i0 = std::max(0, i1 - 1);
        int i2 = std::min((int)sorted.size() - 1, i1 + 1);
        int i3 = std::min((int)sorted.size() - 1, i1 + 2);
        float x1 = sorted[i1].first, x2 = sorted[i2].first;
        float t = (x2 - x1 > 1e-6f) ? (x - x1) / (x2 - x1) : 0.0f;
        t = juce::jlimit(0.0f, 1.0f, t);
        float y0 = sorted[i0].second, y1 = sorted[i1].second;
        float y2 = sorted[i2].second, y3 = sorted[i3].second;
        float t2 = t * t, t3 = t2 * t;
        ir[i] = 0.5f * ((2*y1) + (-y0+y2)*t + (2*y0-5*y1+4*y2-y3)*t2 + (-y0+3*y1-3*y2+y3)*t3);
    }
    updateFreqResponse();
}

void ConvolutionEditorComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(22, 22, 28));

    // IR waveform area
    auto irArea = getIRArea();
    g.setColour(juce::Colour(18, 20, 28));
    g.fillRoundedRectangle(irArea, 4.0f);
    g.setColour(juce::Colour(50, 55, 70));
    g.drawRoundedRectangle(irArea, 4.0f, 1.0f);

    // Center line
    float cy = irArea.getCentreY();
    g.setColour(juce::Colours::grey.withAlpha(0.3f));
    g.drawHorizontalLine((int)cy, irArea.getX(), irArea.getRight());

    // Label
    g.setColour(juce::Colours::white.withAlpha(0.6f));
    g.setFont(10.0f);
    g.drawText("Impulse Response (" + juce::String((int)ir.size()) + " samples)"
               + "  —  click to add points, drag to move, right-click to delete",
               irArea.reduced(4, 2).toNearestInt(), juce::Justification::topLeft);

    // Draw IR curve
    if (!ir.empty()) {
        juce::Path p;
        for (int i = 0; i < (int)ir.size(); ++i) {
            float x = irArea.getX() + (float)i / (float)(ir.size() - 1) * irArea.getWidth();
            float y = cy - ir[i] * irArea.getHeight() * 0.45f;
            if (i == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
        }
        g.setColour(juce::Colours::cornflowerblue);
        g.strokePath(p, juce::PathStrokeType(1.5f));
    }

    // Control points
    for (int i = 0; i < (int)controlPoints.size(); ++i) {
        float x = irArea.getX() + controlPoints[i].first * irArea.getWidth();
        float y = cy - controlPoints[i].second * irArea.getHeight() * 0.45f;
        g.setColour(i == dragPointIdx ? juce::Colours::yellow : juce::Colours::white);
        g.fillEllipse(x - 4, y - 4, 8, 8);
        g.setColour(juce::Colours::cornflowerblue);
        g.drawEllipse(x - 4, y - 4, 8, 8, 1.0f);
    }

    // Frequency response area
    auto freqArea = getFreqArea();
    g.setColour(juce::Colour(18, 20, 28));
    g.fillRoundedRectangle(freqArea, 4.0f);
    g.setColour(juce::Colour(50, 55, 70));
    g.drawRoundedRectangle(freqArea, 4.0f, 1.0f);

    g.setColour(juce::Colours::white.withAlpha(0.6f));
    g.setFont(10.0f);
    g.drawText("Frequency Response (magnitude)",
               freqArea.reduced(4, 2).toNearestInt(), juce::Justification::topLeft);

    if (!freqResponse.empty()) {
        juce::Path fp;
        for (int i = 0; i < (int)freqResponse.size(); ++i) {
            // Log-scale x axis
            float frac = (float)i / (float)(freqResponse.size() - 1);
            float x = freqArea.getX() + frac * freqArea.getWidth();
            float mag = freqResponse[i];
            float y = freqArea.getBottom() - mag * freqArea.getHeight() * 0.9f;
            if (i == 0) fp.startNewSubPath(x, y); else fp.lineTo(x, y);
        }
        g.setColour(juce::Colours::orange);
        g.strokePath(fp, juce::PathStrokeType(1.5f));
    }
}

} // namespace SoundShop

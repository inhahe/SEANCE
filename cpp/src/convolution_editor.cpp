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
    presetCombo.setTooltip("Choose a preset filter type. Lowpass keeps low frequencies, Highpass keeps high frequencies, "
                           "Bandpass keeps a frequency range. Echo creates a delay effect. "
                           "Click Apply Preset to generate the impulse response.");
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
    cutoffSlider.setTooltip("Cutoff frequency in Hz — for Lowpass: keeps frequencies below this; "
                            "for Highpass: keeps frequencies above this; for Bandpass: center frequency");
    orderSlider.setTooltip("Filter steepness — higher values make the filter cut frequencies more sharply at the cutoff. "
                           "Low values give a gentle slope, high values give a brick-wall.");
    bandwidthSlider.setTooltip("Bandwidth in Hz around the center frequency (Bandpass only). Wider = lets through more of the spectrum.");
    delaySlider.setTooltip("Time between echo repeats in milliseconds (Echo preset only)");
    feedbackSlider.setTooltip("Strength of each echo relative to the previous one. 0 = single echo, 0.99 = many slowly-fading echoes (Echo preset only)");
    echoCountSlider.setTooltip("Number of echo repeats to generate (Echo preset only)");

    addAndMakeVisible(applyPresetBtn);
    applyPresetBtn.setTooltip("Generate the impulse response from the preset settings above. Replaces any custom drawing.");
    applyPresetBtn.onClick = [this]() { generateFromPreset(); };

    addAndMakeVisible(loadFileBtn);
    loadFileBtn.setTooltip("Load an audio file as an impulse response. Use a recording of a real space "
                           "(room, hall, cabinet) to capture its acoustic character and apply it to any audio.");
    loadFileBtn.onClick = [this]() { loadFromFile(); };

    // Drawing mode toggle: control points (Catmull-Rom smoothed) vs
    // freehand (per-sample direct drawing). Control points is the default.
    addAndMakeVisible(modePointsBtn);
    addAndMakeVisible(modeFreehandBtn);
    modePointsBtn.setTooltip("Control points mode — drag a few smooth control points to shape the impulse response. "
                             "Easier for clean curves; the editor smooths between points.");
    modeFreehandBtn.setTooltip("Freehand mode — draw the impulse response sample by sample with the mouse. "
                               "Useful for sharp transients or non-smooth shapes.");
    modePointsBtn.setClickingTogglesState(true);
    modeFreehandBtn.setClickingTogglesState(true);
    modePointsBtn.setToggleState(true, juce::dontSendNotification);
    auto updateModeToggles = [this]() {
        modePointsBtn.setToggleState(drawMode == DrawMode::ControlPoints,
                                     juce::dontSendNotification);
        modeFreehandBtn.setToggleState(drawMode == DrawMode::Freehand,
                                       juce::dontSendNotification);
    };
    modePointsBtn.onClick = [this, updateModeToggles]() {
        drawMode = DrawMode::ControlPoints;
        updateModeToggles();
        repaint();
    };
    modeFreehandBtn.onClick = [this, updateModeToggles]() {
        drawMode = DrawMode::Freehand;
        updateModeToggles();
        repaint();
    };

    addAndMakeVisible(lengthSlider); addAndMakeVisible(lengthLbl);
    lengthLbl.setText("IR length:", juce::dontSendNotification);
    lengthLbl.setFont(11.0f);
    lengthSlider.setRange(1, 4096, 1);
    lengthSlider.setValue(ir.size());
    lengthSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    lengthSlider.setTooltip("Impulse response length in samples. Longer IRs allow longer reverbs / "
                            "delays but cost more CPU. ~512 samples = ~12 ms at 44.1 kHz.");

    addAndMakeVisible(applyBtn);
    applyBtn.setTooltip("Save the current impulse response to the convolution node without closing this editor");
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

// Map a (possibly fractional) sample index to its screen x inside the IR
// area, taking current zoom/scroll into account. Samples outside the
// visible window land outside `irArea` horizontally.
float ConvolutionEditorComponent::sampleIdxToScreenX(float idx,
                                                     const juce::Rectangle<float>& irArea) const {
    int n = (int)ir.size();
    if (n <= 1) return irArea.getX();
    float visibleSpan = std::max(1.0f, (float)n / std::max(1.0f, zoomX));
    float firstVisible = scrollFrac * (n - visibleSpan);
    float frac = (idx - firstVisible) / visibleSpan; // 0..1 across visible
    return irArea.getX() + frac * irArea.getWidth();
}

// Inverse: screen x -> sample index.
float ConvolutionEditorComponent::screenXToSampleIdx(float x,
                                                     const juce::Rectangle<float>& irArea) const {
    int n = (int)ir.size();
    if (n <= 1) return 0.0f;
    float visibleSpan = std::max(1.0f, (float)n / std::max(1.0f, zoomX));
    float firstVisible = scrollFrac * (n - visibleSpan);
    float frac = (x - irArea.getX()) / std::max(1.0f, irArea.getWidth());
    return firstVisible + frac * visibleSpan;
}

void ConvolutionEditorComponent::mouseWheelMove(const juce::MouseEvent& e,
                                                const juce::MouseWheelDetails& w) {
    auto area = getIRArea();
    if (!area.contains(e.position)) return;

    if (e.mods.isCtrlDown() || e.mods.isCommandDown()) {
        // Zoom centered on the mouse x
        float sampleAtMouse = screenXToSampleIdx(e.position.x, area);
        float factor = 1.0f + w.deltaY * 0.5f;
        zoomX = juce::jlimit(1.0f, 128.0f, zoomX * factor);
        // Re-anchor so the sample under the mouse stays put
        int n = (int)ir.size();
        if (n > 1) {
            float visibleSpan = std::max(1.0f, (float)n / zoomX);
            float newFirstVisible = sampleAtMouse -
                ((e.position.x - area.getX()) / area.getWidth()) * visibleSpan;
            float maxFirst = std::max(0.0f, (float)n - visibleSpan);
            scrollFrac = juce::jlimit(0.0f, 1.0f,
                (maxFirst > 0) ? newFirstVisible / maxFirst : 0.0f);
        }
    } else {
        // Pan
        scrollFrac = juce::jlimit(0.0f, 1.0f, scrollFrac - w.deltaY * 0.1f);
    }
    repaint();
}

void ConvolutionEditorComponent::resized() {
    auto a = getLocalBounds().reduced(10);
    auto top = a.removeFromTop(28);
    presetCombo.setBounds(top.removeFromLeft(150).reduced(0, 2));
    top.removeFromLeft(8);
    applyPresetBtn.setBounds(top.removeFromLeft(70).reduced(0, 2));
    top.removeFromLeft(8);
    loadFileBtn.setBounds(top.removeFromLeft(100).reduced(0, 2));
    top.removeFromLeft(12);
    modePointsBtn.setBounds(top.removeFromLeft(70).reduced(0, 2));
    modeFreehandBtn.setBounds(top.removeFromLeft(80).reduced(0, 2));
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

    // Screen Y -> amplitude -1..1 (used by both modes)
    float y = 1.0f - 2.0f * (e.position.y - area.getY()) / area.getHeight();
    y = juce::jlimit(-1.0f, 1.0f, y);

    if (drawMode == DrawMode::Freehand) {
        // Write directly to the IR sample under the cursor.
        int n = (int)ir.size();
        int idx = (int)std::round(screenXToSampleIdx(e.position.x, area));
        if (idx >= 0 && idx < n) {
            ir[idx] = y;
            lastDrawSample = idx;
            lastDrawValue  = y;
            updateFreqResponse();
            repaint();
        }
        return;
    }

    // ControlPoints mode: use a fractional x in [0..1] for the control list.
    float x = screenXToSampleIdx(e.position.x, area) /
              std::max(1.0f, (float)(ir.size() - 1));
    x = juce::jlimit(0.0f, 1.0f, x);

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

    // Find near existing point to drag (tolerance scales with zoom so it
    // stays roughly the same pixel distance regardless of zoom level).
    float pointPickTol = 0.03f / std::max(1.0f, zoomX);
    int best = -1; float bestD = pointPickTol;
    for (int i = 0; i < (int)controlPoints.size(); ++i) {
        float dist = std::abs(controlPoints[i].first - x);
        if (dist < bestD) { bestD = dist; best = i; }
    }
    if (best >= 0) {
        dragPointIdx = best;
    } else {
        controlPoints.push_back({x, y});
        std::sort(controlPoints.begin(), controlPoints.end());
        for (int i = 0; i < (int)controlPoints.size(); ++i)
            if (std::abs(controlPoints[i].first - x) < 0.001f) { dragPointIdx = i; break; }
        renderFromControlPoints();
        repaint();
    }
}

void ConvolutionEditorComponent::mouseDrag(const juce::MouseEvent& e) {
    auto area = getIRArea();
    float y = juce::jlimit(-1.0f, 1.0f,
        1.0f - 2.0f * (e.position.y - area.getY()) / area.getHeight());

    if (drawMode == DrawMode::Freehand) {
        // Draw a continuous line by filling every sample between the last
        // drawn position and the current one — otherwise fast mouse motion
        // leaves gaps.
        int n = (int)ir.size();
        int idx = (int)std::round(screenXToSampleIdx(e.position.x, area));
        if (idx < 0) idx = 0;
        if (idx >= n) idx = n - 1;
        if (n > 0) {
            if (lastDrawSample < 0) { lastDrawSample = idx; lastDrawValue = y; }
            int lo = std::min(lastDrawSample, idx);
            int hi = std::max(lastDrawSample, idx);
            float v0 = (lo == lastDrawSample) ? lastDrawValue : y;
            float v1 = (hi == idx)            ? y            : lastDrawValue;
            for (int i = lo; i <= hi; ++i) {
                float t = (hi > lo) ? (float)(i - lo) / (float)(hi - lo) : 0.0f;
                ir[i] = v0 + (v1 - v0) * t;
            }
            lastDrawSample = idx;
            lastDrawValue  = y;
            updateFreqResponse();
            repaint();
        }
        return;
    }

    if (dragPointIdx < 0 || dragPointIdx >= (int)controlPoints.size()) return;
    float x = juce::jlimit(0.0f, 1.0f,
        screenXToSampleIdx(e.position.x, area) /
        std::max(1.0f, (float)(ir.size() - 1)));
    controlPoints[dragPointIdx] = {x, y};
    std::sort(controlPoints.begin(), controlPoints.end());
    for (int i = 0; i < (int)controlPoints.size(); ++i)
        if (std::abs(controlPoints[i].first - x) < 0.001f &&
            std::abs(controlPoints[i].second - y) < 0.001f) { dragPointIdx = i; break; }
    renderFromControlPoints();
    repaint();
}

void ConvolutionEditorComponent::mouseUp(const juce::MouseEvent&) {
    if (drawMode == DrawMode::Freehand && lastDrawSample >= 0) {
        commitIR();
        if (onApply) onApply();
    } else if (dragPointIdx >= 0) {
        commitIR();
        if (onApply) onApply();
    }
    dragPointIdx = -1;
    lastDrawSample = -1;
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

    // Compute the visible sample range under current zoom/scroll
    int nSamples = (int)ir.size();
    float visibleSpan = std::max(1.0f, (float)nSamples / std::max(1.0f, zoomX));
    float firstVisible = scrollFrac * std::max(0.0f, (float)nSamples - visibleSpan);
    int visLo = (int)std::floor(firstVisible);
    int visHi = (int)std::ceil(firstVisible + visibleSpan) + 1;
    visLo = juce::jlimit(0, nSamples - 1, visLo);
    visHi = juce::jlimit(0, nSamples,     visHi);
    float pxPerSample = (nSamples > 0) ?
        irArea.getWidth() / std::max(1.0f, visibleSpan) : 1.0f;

    // Label — shows current zoom + freehand hint when zoomed in enough
    g.setColour(juce::Colours::white.withAlpha(0.6f));
    g.setFont(10.0f);
    juce::String modeStr = (drawMode == DrawMode::Freehand) ? "Freehand" : "Points";
    juce::String label = "IR: " + juce::String(nSamples) + " samples"
                       + "  —  " + modeStr
                       + "  —  Ctrl+wheel to zoom (" + juce::String(zoomX, 1) + "x), wheel to scroll";
    g.drawText(label, irArea.reduced(4, 2).toNearestInt(), juce::Justification::topLeft);

    // Draw sample-boundary grid when zoomed in enough that each sample is
    // visually distinguishable (≥5 px). Gives the user precise targeting
    // for freehand per-sample edits.
    if (pxPerSample >= 5.0f) {
        g.setColour(juce::Colours::white.withAlpha(0.08f));
        for (int i = visLo; i < visHi; ++i) {
            float x = sampleIdxToScreenX((float)i, irArea);
            g.drawVerticalLine((int)x, irArea.getY(), irArea.getBottom());
        }
    }

    // Draw IR curve — only the visible portion, and as discrete stems +
    // dots when zoomed in enough so individual samples are legible.
    if (!ir.empty()) {
        bool showSamples = pxPerSample >= 5.0f;
        juce::Path p;
        bool first = true;
        for (int i = visLo; i < visHi; ++i) {
            float x = sampleIdxToScreenX((float)i, irArea);
            float y = cy - ir[i] * irArea.getHeight() * 0.45f;
            if (first) { p.startNewSubPath(x, y); first = false; }
            else p.lineTo(x, y);
        }
        g.setColour(juce::Colours::cornflowerblue);
        g.strokePath(p, juce::PathStrokeType(1.5f));

        if (showSamples) {
            g.setColour(juce::Colours::cornflowerblue.brighter(0.3f));
            for (int i = visLo; i < visHi; ++i) {
                float x = sampleIdxToScreenX((float)i, irArea);
                float y = cy - ir[i] * irArea.getHeight() * 0.45f;
                // Stem from center line to sample value
                g.drawVerticalLine((int)x, std::min(y, cy), std::max(y, cy));
                // Sample dot
                g.fillEllipse(x - 2, y - 2, 4, 4);
            }
        }
    }

    // Control points (only visible in ControlPoints mode)
    if (drawMode == DrawMode::ControlPoints) {
        for (int i = 0; i < (int)controlPoints.size(); ++i) {
            float sampleIdx = controlPoints[i].first * (float)(nSamples - 1);
            float x = sampleIdxToScreenX(sampleIdx, irArea);
            if (x < irArea.getX() - 6 || x > irArea.getRight() + 6) continue;
            float y = cy - controlPoints[i].second * irArea.getHeight() * 0.45f;
            g.setColour(i == dragPointIdx ? juce::Colours::yellow : juce::Colours::white);
            g.fillEllipse(x - 4, y - 4, 8, 8);
            g.setColour(juce::Colours::cornflowerblue);
            g.drawEllipse(x - 4, y - 4, 8, 8, 1.0f);
        }
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

#include "signal_eq_editor.h"
#include "builtin_effects.h"   // SignalEQProcessor::countPoints / kMaxPoints / kMinPoints
#include <cmath>
#include <complex>
#include <algorithm>

namespace SoundShop {

// Sample rate assumed for drawing the response curve. The actual processing uses
// the real device rate; the curve's shape in log-frequency is essentially rate-
// independent across 20 Hz..20 kHz, so a fixed display rate is fine for the plot.
static constexpr double kDisplaySR = 48000.0;

SignalEQEditorComponent::SignalEQEditorComponent(NodeGraph& g, int nid,
                                                 std::function<void()> apply)
    : graph(g), nodeId(nid), onApply(std::move(apply)) {
    titleLabel.setText("Signal EQ", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(16.0f, juce::Font::bold));
    titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(titleLabel);

    modeBox.addItem("Zero-latency (biquad)", 1);
    modeBox.addItem("FFT exact (phase-clean)", 2);
    modeBox.setTooltip(
        "How the curve is applied to the audio.\n"
        "Zero-latency: a bank of resonant filters, processed sample-by-sample. "
        "No added delay; introduces the gentle phase shift of an analog EQ.\n"
        "FFT exact: the same magnitude curve but phase-clean, via a short-time "
        "Fourier transform. Costs more CPU and softens sharp transients slightly, "
        "but the response is mathematically exact. Still adds no latency.");
    modeBox.onChange = [this] {
        if (auto* n = node()) {
            for (auto& p : n->params)
                if (p.name == "Mode") {
                    float v = (float)(modeBox.getSelectedId() - 1);
                    p.value = v; p.baseValue = v;
                }
            commitNow("Signal EQ mode");
            scheduleApply();
        }
    };
    addAndMakeVisible(modeBox);

    auto setupValueSlider = [this](juce::Slider& s, juce::Label& lbl,
                                   const juce::String& text) {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 18);
        lbl.setText(text, juce::dontSendNotification);
        lbl.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        lbl.setFont(juce::Font(12.0f));
        addAndMakeVisible(s);
        addAndMakeVisible(lbl);
    };

    setupValueSlider(widthSlider, widthLabel, "Width (Q)");
    widthSlider.setRange(0.1, 24.0, 0.0);
    widthSlider.setSkewFactorFromMidPoint(2.0);
    widthSlider.setTooltip(
        "Bandwidth of every point's bell, shared across all points. Low = wide, "
        "gentle bells; high = narrow, surgical peaks/notches. This is the filter "
        "Q: bigger numbers are narrower.");
    widthSlider.onValueChange = [this] {
        if (auto* n = node())
            for (auto& p : n->params)
                if (p.name == "Width") {
                    float v = (float)widthSlider.getValue();
                    p.value = v; p.baseValue = v;
                }
        repaint();
    };
    widthSlider.onDragEnd = [this] { commitNow("Signal EQ width"); };

    setupValueSlider(mixSlider, mixLabel, "Mix");
    mixSlider.setRange(0.0, 1.0, 0.0);
    mixSlider.setTooltip("Dry/wet blend. 0 = bypass (dry only), 1 = fully filtered.");
    mixSlider.onValueChange = [this] {
        if (auto* n = node())
            for (auto& p : n->params)
                if (p.name == "Mix") {
                    float v = (float)mixSlider.getValue();
                    p.value = v; p.baseValue = v;
                }
        repaint();
    };
    mixSlider.onDragEnd = [this] { commitNow("Signal EQ mix"); };

    addPointBtn.setTooltip("Add a new flat point at 1 kHz. You can also double-click "
                           "empty space in the graph to add a point there.");
    addPointBtn.onClick = [this] {
        addPointAt(1000.0f, 0.0f);
    };
    addAndMakeVisible(addPointBtn);

    closeBtn.onClick = [this] {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->closeButtonPressed();
    };
    addAndMakeVisible(closeBtn);

    hintLabel.setText("Drag a point to move it - double-click empty space to add - "
                      "right-click a point to delete or add a control input.",
                      juce::dontSendNotification);
    hintLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
    hintLabel.setFont(juce::Font(11.0f));
    hintLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(hintLabel);

    syncControlsFromNode();
    setSize(620, 440);
    startTimerHz(8);
}

SignalEQEditorComponent::~SignalEQEditorComponent() { stopTimer(); }

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
void SignalEQEditorComponent::resized() {
    auto r = getLocalBounds().reduced(10);
    titleLabel.setBounds(r.removeFromTop(24));
    r.removeFromTop(4);

    auto bottom = r.removeFromBottom(96);
    hintLabel.setBounds(r.removeFromBottom(18));
    canvasArea = r.reduced(2);

    // Bottom control strip: two rows.
    auto row1 = bottom.removeFromTop(28);
    auto modeBoxArea = row1.removeFromLeft(220);
    modeBox.setBounds(modeBoxArea.reduced(0, 2));
    addPointBtn.setBounds(row1.removeFromRight(110).reduced(2));
    closeBtn.setBounds(row1.removeFromRight(90).reduced(2));

    bottom.removeFromTop(6);
    auto row2 = bottom.removeFromTop(26);
    auto wArea = row2.removeFromLeft(row2.getWidth() / 2).reduced(2, 0);
    widthLabel.setBounds(wArea.removeFromLeft(64));
    widthSlider.setBounds(wArea);
    auto mArea = row2.reduced(2, 0);
    mixLabel.setBounds(mArea.removeFromLeft(32));
    mixSlider.setBounds(mArea);
}

// ---------------------------------------------------------------------------
// Mapping helpers
// ---------------------------------------------------------------------------
float SignalEQEditorComponent::freqToX(float hz) const {
    hz = juce::jlimit(kMinHz, kMaxHz, hz);
    float t = std::log10(hz / kMinHz) / std::log10(kMaxHz / kMinHz);
    return canvasArea.getX() + t * canvasArea.getWidth();
}
float SignalEQEditorComponent::xToFreq(float x) const {
    float t = (x - canvasArea.getX()) / juce::jmax(1, canvasArea.getWidth());
    t = juce::jlimit(0.0f, 1.0f, t);
    return kMinHz * std::pow(kMaxHz / kMinHz, t);
}
float SignalEQEditorComponent::gainToY(float db) const {
    db = juce::jlimit(kMinDb, kMaxDb, db);
    float t = (db - kMinDb) / (kMaxDb - kMinDb);  // 0 at bottom, 1 at top
    return canvasArea.getBottom() - t * canvasArea.getHeight();
}
float SignalEQEditorComponent::yToGain(float y) const {
    float t = (canvasArea.getBottom() - y) / juce::jmax(1, canvasArea.getHeight());
    t = juce::jlimit(0.0f, 1.0f, t);
    return kMinDb + t * (kMaxDb - kMinDb);
}

// ---------------------------------------------------------------------------
// Node / param helpers
// ---------------------------------------------------------------------------
Node* SignalEQEditorComponent::node() { return graph.findNode(nodeId); }

int SignalEQEditorComponent::pointCount() {
    if (auto* n = node()) return SignalEQProcessor::countPoints(*n);
    return 0;
}

int SignalEQEditorComponent::freqParamIndex(int idx1) {
    if (auto* n = node()) {
        std::string key = "P" + std::to_string(idx1) + " Freq";
        for (int i = 0; i < (int)n->params.size(); ++i)
            if (n->params[i].name == key) return i;
    }
    return -1;
}
int SignalEQEditorComponent::gainParamIndex(int idx1) {
    if (auto* n = node()) {
        std::string key = "P" + std::to_string(idx1) + " Gain";
        for (int i = 0; i < (int)n->params.size(); ++i)
            if (n->params[i].name == key) return i;
    }
    return -1;
}

bool SignalEQEditorComponent::getPoint(int idx1, float& f, float& gdb) {
    int fi = freqParamIndex(idx1), gi = gainParamIndex(idx1);
    if (fi < 0 || gi < 0) return false;
    auto* n = node();
    f   = n->params[fi].modulated ? n->params[fi].baseValue : n->params[fi].value;
    gdb = n->params[gi].modulated ? n->params[gi].baseValue : n->params[gi].value;
    return true;
}

void SignalEQEditorComponent::setPointLive(int idx1, float f, float gdb) {
    int fi = freqParamIndex(idx1), gi = gainParamIndex(idx1);
    if (fi < 0 || gi < 0) return;
    auto* n = node();
    f   = juce::jlimit(kMinHz, kMaxHz, f);
    gdb = juce::jlimit(kMinDb, kMaxDb, gdb);
    n->params[fi].value = f;   n->params[fi].baseValue = f;
    n->params[gi].value = gdb; n->params[gi].baseValue = gdb;
}

int SignalEQEditorComponent::pointNearby(juce::Point<float> p) {
    int np = pointCount();
    int best = -1; float bestDist = 14.0f; // hit radius (px)
    for (int i = 1; i <= np; ++i) {
        float f, g;
        if (!getPoint(i, f, g)) continue;
        juce::Point<float> handle(freqToX(f), gainToY(g));
        float d = handle.getDistanceFrom(p);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

// Combined response in dB at frequency hz: product of every point's digital
// peaking-biquad magnitude (the same coefficients SignalEQProcessor builds).
float SignalEQEditorComponent::responseDbAt(float hz) {
    auto* n = node();
    if (!n) return 0.0f;
    float Q = 1.0f;
    for (auto& p : n->params) if (p.name == "Width") Q = p.value;
    Q = juce::jlimit(0.1f, 24.0f, Q);

    const double sr = kDisplaySR;
    double w = 2.0 * 3.14159265358979323846 * juce::jlimit(kMinHz, kMaxHz, hz) / sr;
    std::complex<double> z1(std::cos(-w), std::sin(-w));
    std::complex<double> z2 = z1 * z1;

    double mag = 1.0;
    int np = pointCount();
    for (int i = 1; i <= np; ++i) {
        float f, gdb;
        if (!getPoint(i, f, gdb)) continue;
        f = juce::jlimit(20.0f, (float)(sr * 0.49), f);
        double w0 = 2.0 * 3.14159265358979323846 * f / sr;
        double cosw0 = std::cos(w0), sinw0 = std::sin(w0);
        double alpha = sinw0 / (2.0 * Q);
        double A = std::pow(10.0, gdb / 40.0);
        double b0 = 1.0 + alpha * A, b1 = -2.0 * cosw0, b2 = 1.0 - alpha * A;
        double a0 = 1.0 + alpha / A, a1 = -2.0 * cosw0, a2 = 1.0 - alpha / A;
        std::complex<double> num = b0 + b1 * z1 + b2 * z2;
        std::complex<double> den = a0 + a1 * z1 + a2 * z2;
        double d = std::abs(den);
        mag *= (d > 1e-12) ? std::abs(num) / d : 1.0;
    }
    return (float)(20.0 * std::log10(juce::jmax(1e-6, mag)));
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------
void SignalEQEditorComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(22, 22, 28));

    // Canvas background.
    g.setColour(juce::Colour(14, 14, 18));
    g.fillRect(canvasArea);
    g.setColour(juce::Colour(60, 60, 70));
    g.drawRect(canvasArea, 1);

    // dB grid lines + labels.
    g.setFont(10.0f);
    for (int db = -24; db <= 24; db += 12) {
        float y = gainToY((float)db);
        g.setColour(db == 0 ? juce::Colour(90, 90, 105) : juce::Colour(40, 40, 50));
        g.drawHorizontalLine((int)y, (float)canvasArea.getX(), (float)canvasArea.getRight());
        g.setColour(juce::Colour(120, 120, 130));
        g.drawText(juce::String(db) + " dB", canvasArea.getX() + 2, (int)y - 12, 48, 12,
                   juce::Justification::left);
    }
    // Frequency decade grid + labels.
    const float decades[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    for (float f : decades) {
        float x = freqToX(f);
        g.setColour(juce::Colour(40, 40, 50));
        g.drawVerticalLine((int)x, (float)canvasArea.getY(), (float)canvasArea.getBottom());
        g.setColour(juce::Colour(120, 120, 130));
        juce::String lbl = f >= 1000 ? juce::String(f / 1000, (f == 1000 ? 0 : 0)) + "k"
                                     : juce::String((int)f);
        g.drawText(lbl, (int)x - 16, canvasArea.getBottom() - 12, 32, 12,
                   juce::Justification::centred);
    }

    // Combined response curve.
    juce::Path curve;
    bool first = true;
    for (int px = 0; px <= canvasArea.getWidth(); ++px) {
        float x = canvasArea.getX() + px;
        float f = xToFreq(x);
        float y = gainToY(responseDbAt(f));
        if (first) { curve.startNewSubPath(x, y); first = false; }
        else        curve.lineTo(x, y);
    }
    g.setColour(juce::Colour(120, 200, 255));
    g.strokePath(curve, juce::PathStrokeType(2.0f));

    // Point handles.
    int np = pointCount();
    auto* n = node();
    for (int i = 1; i <= np; ++i) {
        float f, gdb;
        if (!getPoint(i, f, gdb)) continue;
        juce::Point<float> h(freqToX(f), gainToY(gdb));
        bool driven = false;
        if (n) {
            int fi = freqParamIndex(i), gi = gainParamIndex(i);
            if (fi >= 0 && n->params[fi].modulated) driven = true;
            if (gi >= 0 && n->params[gi].modulated) driven = true;
        }
        g.setColour(i == draggingPoint ? juce::Colours::white
                    : driven ? juce::Colour(255, 180, 80)   // orange when modulated
                             : juce::Colour(120, 200, 255));
        g.fillEllipse(h.x - 5, h.y - 5, 10, 10);
        g.setColour(juce::Colours::black);
        g.drawEllipse(h.x - 5, h.y - 5, 10, 10, 1.0f);
        g.setColour(juce::Colours::white);
        g.setFont(10.0f);
        g.drawText("P" + juce::String(i), (int)h.x - 10, (int)h.y - 20, 20, 12,
                   juce::Justification::centred);
    }
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------
void SignalEQEditorComponent::mouseDown(const juce::MouseEvent& e) {
    if (!canvasArea.contains(e.getPosition())) return;
    int hit = pointNearby(e.position);

    if (e.mods.isPopupMenu()) {
        if (hit < 0) return;
        int fi = freqParamIndex(hit), gi = gainParamIndex(hit);
        juce::PopupMenu m;
        bool freqMod = (fi >= 0 && hasParamModPin(graph, nodeId, fi));
        bool gainMod = (gi >= 0 && hasParamModPin(graph, nodeId, gi));
        m.addItem(1, "Add frequency (X) control input", !freqMod);
        m.addItem(2, "Add gain (Y) control input", !gainMod);
        m.addSeparator();
        m.addItem(3, "Delete point P" + juce::String(hit),
                  pointCount() > SignalEQProcessor::kMinPoints);
        int point = hit;
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
            [this, point](int r) {
                if (r == 1) { int idx = freqParamIndex(point); if (idx >= 0) addControlInput(idx); }
                else if (r == 2) { int idx = gainParamIndex(point); if (idx >= 0) addControlInput(idx); }
                else if (r == 3) deletePoint(point);
            });
        return;
    }

    if (hit > 0) {
        draggingPoint = hit;
        dragMoved = false;
    }
}

void SignalEQEditorComponent::mouseDrag(const juce::MouseEvent& e) {
    if (draggingPoint < 0) return;
    float f = xToFreq(e.position.x);
    float gdb = yToGain(e.position.y);
    setPointLive(draggingPoint, f, gdb);
    dragMoved = true;
    repaint();
}

void SignalEQEditorComponent::mouseUp(const juce::MouseEvent&) {
    if (draggingPoint > 0 && dragMoved)
        commitNow("Move EQ point");
    draggingPoint = -1;
    dragMoved = false;
}

void SignalEQEditorComponent::mouseDoubleClick(const juce::MouseEvent& e) {
    if (!canvasArea.contains(e.getPosition())) return;
    if (pointNearby(e.position) > 0) return;   // double-click on a handle: ignore
    addPointAt(xToFreq(e.position.x), yToGain(e.position.y));
}

// ---------------------------------------------------------------------------
// Structural edits
// ---------------------------------------------------------------------------
void SignalEQEditorComponent::addPointAt(float hz, float db) {
    auto* n = node();
    if (!n) return;
    int np = SignalEQProcessor::countPoints(*n);
    if (np >= SignalEQProcessor::kMaxPoints) return;
    hz = juce::jlimit(kMinHz, kMaxHz, hz);
    db = juce::jlimit(kMinDb, kMaxDb, db);
    std::string pfx = "P" + std::to_string(np + 1) + " ";
    n->params.push_back({pfx + "Freq", hz, 20.0f, 20000.0f});
    n->params.push_back({pfx + "Gain", db, -24.0f, 24.0f});
    commitNow("Add EQ point");
    scheduleApply();
}

// Delete an arbitrary point and keep the P<n> numbering contiguous, fixing up
// every modPin's paramIndex so no modulation binding dangles or points at the
// wrong param after the vector shrinks. (The two erased params are adjacent in
// creation order, so removal shifts later indices down by two.)
void SignalEQEditorComponent::deletePoint(int idx1) {
    auto* n = node();
    if (!n) return;
    int np = SignalEQProcessor::countPoints(*n);
    if (np <= SignalEQProcessor::kMinPoints) return;

    int fi = freqParamIndex(idx1), gi = gainParamIndex(idx1);
    if (fi < 0 || gi < 0) return;

    // Drop any modulation pins on this point's two params first (valid indices).
    removeParamModPin(graph, nodeId, fi);
    removeParamModPin(graph, nodeId, gi);

    int lo = std::min(fi, gi), hi = std::max(fi, gi);
    n->params.erase(n->params.begin() + hi);
    n->params.erase(n->params.begin() + lo);

    // Shift surviving modPins' paramIndex past the erased slots.
    for (auto& mp : n->modPins) {
        int shift = 0;
        if (mp.paramIndex > hi) ++shift;
        if (mp.paramIndex > lo) ++shift;
        mp.paramIndex -= shift;
    }

    // Renumber points above idx1 down by one to stay contiguous.
    for (int j = idx1 + 1; j <= np; ++j) {
        std::string oldF = "P" + std::to_string(j) + " Freq";
        std::string oldG = "P" + std::to_string(j) + " Gain";
        std::string newF = "P" + std::to_string(j - 1) + " Freq";
        std::string newG = "P" + std::to_string(j - 1) + " Gain";
        for (auto& p : n->params) {
            if (p.name == oldF) p.name = newF;
            else if (p.name == oldG) p.name = newG;
        }
    }
    pruneOrphanModPins(graph, nodeId);
    commitNow("Delete EQ point");
    scheduleApply();
}

void SignalEQEditorComponent::addControlInput(int paramIndex) {
    if (paramIndex < 0) return;
    addParamModPin(graph, nodeId, paramIndex, /*absolute=*/false);
    commitNow("Add EQ control input");
    scheduleApply();
}

// ---------------------------------------------------------------------------
// Commit / sync / apply
// ---------------------------------------------------------------------------
void SignalEQEditorComponent::commitNow(const juce::String& desc) {
    graph.commitSnapshot(desc.toStdString());
    syncControlsFromNode();
    repaint();
}

void SignalEQEditorComponent::syncControlsFromNode() {
    auto* n = node();
    if (!n) return;
    for (auto& p : n->params) {
        if (p.name == "Mode")  modeBox.setSelectedId((int)p.value + 1, juce::dontSendNotification);
        else if (p.name == "Width") widthSlider.setValue(p.value, juce::dontSendNotification);
        else if (p.name == "Mix")   mixSlider.setValue(p.value, juce::dontSendNotification);
    }
}

void SignalEQEditorComponent::scheduleApply() {
    structuralPending = true;
}

void SignalEQEditorComponent::timerCallback() {
    if (structuralPending) {
        structuralPending = false;
        if (onApply) onApply();
    }
    // Keep the plot live so signal-driven points animate as they move.
    repaint();
}

} // namespace SoundShop

#define _USE_MATH_DEFINES
#include "curve_editor.h"
#include "builtin_synth.h"   // WaveExprParser
#include <algorithm>
#include <cmath>
#include <sstream>

namespace SoundShop {

// ==============================================================================
// Static helpers (file-scope, shared between SpectralCurve::evaluate
// and SpectralCurvePanel rendering)
// ==============================================================================
static constexpr int kFreehandSampleCount = 512;

static std::vector<std::pair<float, float>> defaultMagPoints() {
    return {
        {0.00f, 1.00f},
        {0.10f, 0.55f},
        {0.25f, 0.25f},
        {0.50f, 0.08f},
        {0.80f, 0.02f},
    };
}

static std::vector<std::pair<float, float>> defaultPhasePoints() {
    return {
        {0.00f, 0.0f},
        {0.25f, 0.0f},
        {0.50f, 0.0f},
        {0.75f, 0.0f},
    };
}

static std::vector<float> defaultFreehandSamples() {
    return std::vector<float>(kFreehandSampleCount, 0.0f);
}

static float sampleDrawnPointsNonPeriodic(
    const std::vector<std::pair<float, float>>& pts, float x)
{
    int n = (int)pts.size();
    if (n == 0) return 0.0f;
    if (n == 1) return pts[0].second;
    if (x <= pts[0].first)      return pts[0].second;
    if (x >= pts[n - 1].first)  return pts[n - 1].second;

    int i1 = 0;
    for (int i = 0; i < n - 1; ++i)
        if (x >= pts[i].first && x < pts[i + 1].first) { i1 = i; break; }
    int i2 = i1 + 1;
    int i0 = std::max(0, i1 - 1);
    int i3 = std::min(n - 1, i2 + 1);

    float x1 = pts[i1].first, x2 = pts[i2].first;
    float t = (x2 - x1 > 1e-6f) ? (x - x1) / (x2 - x1) : 0.0f;
    t = juce::jlimit(0.0f, 1.0f, t);

    float y0 = pts[i0].second, y1 = pts[i1].second;
    float y2 = pts[i2].second, y3 = pts[i3].second;
    float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * y1)
                 + (-y0 + y2) * t
                 + (2.0f*y0 - 5.0f*y1 + 4.0f*y2 - y3) * t2
                 + (-y0 + 3.0f*y1 - 3.0f*y2 + y3) * t3);
}

static float sampleDrawnSamplesAt(const std::vector<float>& s, float x) {
    int n = (int)s.size();
    if (n == 0) return 0.0f;
    x = juce::jlimit(0.0f, 1.0f, x);
    float idx = x * (float)(n - 1);
    int i0 = juce::jlimit(0, n - 1, (int)idx);
    int i1 = std::min(n - 1, i0 + 1);
    float frac = idx - (float)i0;
    return s[i0] * (1.0f - frac) + s[i1] * frac;
}

// Escape helpers so a SpectralCurve's encoded form is safe to embed in a
// '|'-delimited container. Within a curve's field list (comma-separated)
// formula expressions can contain commas and pipes, so we substitute
// `,` -> `;` and `|` -> `\x1F` on encode and reverse on decode. Neither
// substitute is part of the WaveExprParser grammar.
static std::string escapeFormula(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (char c : s) {
        if      (c == ',')  out += ';';
        else if (c == '|')  out += '\x1F';
        else                out += c;
    }
    return out;
}
static std::string unescapeFormula(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (char c : s) {
        if      (c == ';')    out += ',';
        else if (c == '\x1F') out += '|';
        else                  out += c;
    }
    return out;
}

static std::vector<std::string> splitChar(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= s.size()) {
        size_t n = s.find(sep, p);
        if (n == std::string::npos) n = s.size();
        out.push_back(s.substr(p, n - p));
        p = n + 1;
    }
    return out;
}

// ==============================================================================
// SpectralCurve - evaluation and serialization
// ==============================================================================
// Resolution of the normalized buffer baked from a Lua/Python equation.
static constexpr int kScriptBakeRes = 1024;

void SpectralCurve::rebake() {
    if (mode != Equation || lang == ShapeLang::Builtin) {
        scriptSamples.clear();
        scriptError.clear();
        return;
    }
    std::string err;
    bakeShapeExpr(lang, expression, /*domainRadians=*/false, kScriptBakeRes,
                  scriptSamples, err);
    scriptError = err;
}

std::vector<float> SpectralCurve::evaluate(int N) const {
    std::vector<float> out(N, 0.0f);
    if (N <= 0) return out;

    if (mode == Equation) {
        if (lang == ShapeLang::Builtin) {
            // Built-in: `f` is the integer bin index over [0, N), evaluated
            // live. Pure C++, safe from any thread.
            out = WaveExprParser::evaluateOverBins(expression, N);
            return out;
        }
        // Lua/Python: resample the pre-baked normalized [0,1] buffer. Never
        // calls the interpreter (which isn't audio-thread safe).
        for (int k = 0; k < N; ++k) {
            float x = (N > 1) ? (float)k / (float)(N - 1) : 0.0f;
            out[k] = scriptSamples.empty() ? 0.0f
                                           : sampleDrawnSamplesAt(scriptSamples, x);
        }
        return out;
    }

    if (freehandMode) {
        for (int k = 0; k < N; ++k) {
            float x = (N > 1) ? (float)k / (float)(N - 1) : 0.0f;
            out[k] = sampleDrawnSamplesAt(drawnSamples, x);
        }
    } else {
        for (int k = 0; k < N; ++k) {
            float x = (N > 1) ? (float)k / (float)(N - 1) : 0.0f;
            out[k] = sampleDrawnPointsNonPeriodic(drawnPoints, x);
        }
    }
    return out;
}

std::string SpectralCurve::encode() const {
    std::ostringstream o;
    if (mode == Equation) {
        // Field 2 = authoring language ("builtin"/"lua"/"python"); absent in
        // pre-language saves, which decode as Built-in.
        o << "eq," << escapeFormula(expression) << "," << shapeLangKey(lang);
    } else if (freehandMode) {
        o << "drawnfh," << drawnSamples.size();
        for (float v : drawnSamples) o << "," << v;
    } else {
        o << "drawnpt," << drawnPoints.size();
        for (auto& p : drawnPoints) o << "," << p.first << "," << p.second;
    }
    return o.str();
}

bool SpectralCurve::decode(const std::string& body, SpectralCurve& out) {
    auto f = splitChar(body, ',');
    if (f.empty()) return false;
    const std::string& kind = f[0];
    out = SpectralCurve{};
    if (kind == "eq") {
        out.mode = SpectralCurve::Equation;
        if (f.size() > 1) out.expression = unescapeFormula(f[1]);
        if (f.size() > 2) out.lang = shapeLangFromKey(f[2]);
        out.rebake();   // populate scriptSamples for Lua/Python equations
        return true;
    }
    if (kind == "drawnfh") {
        out.mode = SpectralCurve::Drawn;
        out.freehandMode = true;
        if (f.size() < 2) return false;
        int n = 0; try { n = std::stoi(f[1]); } catch (...) {}
        for (int k = 0; k < n && (size_t)(2 + k) < f.size(); ++k) {
            float v = 0; try { v = std::stof(f[2 + k]); } catch (...) {}
            out.drawnSamples.push_back(v);
        }
        return true;
    }
    if (kind == "drawnpt") {
        out.mode = SpectralCurve::Drawn;
        out.freehandMode = false;
        if (f.size() < 2) return false;
        int n = 0; try { n = std::stoi(f[1]); } catch (...) {}
        for (int k = 0; k < n; ++k) {
            size_t xi = 2 + (size_t)k * 2;
            size_t yi = xi + 1;
            if (yi >= f.size()) break;
            float x = 0, y = 0;
            try { x = std::stof(f[xi]); } catch (...) {}
            try { y = std::stof(f[yi]); } catch (...) {}
            out.drawnPoints.emplace_back(x, y);
        }
        return true;
    }
    return false;
}

// ==============================================================================
// SpectralCurvePanel
// ==============================================================================
SpectralCurvePanel::SpectralCurvePanel(SpectralCurve& curve_,
                                       const juce::String& title_,
                                       float yMin_, float yMax_,
                                       juce::Colour curveColour_,
                                       std::function<void()> onChanged_)
    : curve(curve_), title(title_),
      yMin(yMin_), yMax(yMax_), curveColour(curveColour_),
      onChanged(std::move(onChanged_))
{
    addAndMakeVisible(titleLabel);
    titleLabel.setText(title, juce::dontSendNotification);
    titleLabel.setFont(juce::Font(14.0f, juce::Font::bold));

    addAndMakeVisible(equationBtn);
    equationBtn.setButtonText("Equation");
    equationBtn.setClickingTogglesState(true);
    equationBtn.setTooltip("Author this curve as a formula in `f` (0..N-1 across the curve's x range). "
                           "Vocabulary: sin, cos, tan, exp, log, sqrt, pow, abs, tanh, clamp, "
                           "saw(f), square(f), triangle(f), noise(), random, pi, e.");
    equationBtn.onClick = [this]() {
        curve.mode = SpectralCurve::Equation;
        updateModeUI();
        if (onChanged) onChanged();
    };

    addAndMakeVisible(drawBtn);
    drawBtn.setButtonText("Draw");
    drawBtn.setClickingTogglesState(true);
    drawBtn.setTooltip("Author this curve graphically. In Points mode, click to add control points "
                       "(shift-click to delete, drag to move) and the curve is interpolated through them. "
                       "In Freehand mode, click and drag to paint the curve directly.");
    drawBtn.onClick = [this]() {
        if (curve.mode == SpectralCurve::Equation) {
            seedDrawnFromEquation();
        } else if (curve.drawnPoints.empty()) {
            curve.drawnPoints = (yMin < 0.0f) ? defaultPhasePoints()
                                              : defaultMagPoints();
        }
        curve.mode = SpectralCurve::Drawn;
        updateModeUI();
        if (onChanged) onChanged();
    };

    addAndMakeVisible(freehandToggle);
    freehandToggle.setButtonText("Points");
    freehandToggle.setTooltip("Toggle between Points mode (click to place control points with smooth interpolation) "
                              "and Freehand mode (click and drag to draw the curve directly).");
    freehandToggle.onClick = [this]() {
        curve.freehandMode = !curve.freehandMode;
        freehandToggle.setButtonText(curve.freehandMode ? "Freehand" : "Points");
        if (curve.freehandMode && curve.drawnSamples.empty())
            curve.drawnSamples = defaultFreehandSamples();
        if (onChanged) onChanged();
        repaint();
    };

    addAndMakeVisible(exprEditor);
    exprEditor.setMultiLine(false);
    exprEditor.setReturnKeyStartsNewLine(false);
    exprEditor.setText(curve.expression, juce::dontSendNotification);
    exprEditor.onTextChange = [this]() {
        curve.expression = exprEditor.getText().toStdString();
        curve.rebake();           // refresh Lua/Python bake (no-op for Built-in)
        if (onChanged) onChanged();
        repaint();
    };

    // Language selector (Built-in / Lua / Python) for the Equation expression.
    addAndMakeVisible(langCombo);
    langCombo.addItem("Built-in", 1);
    langCombo.addItem("Lua",      2);
    langCombo.addItem("Python",   3);
    langCombo.setItemEnabled(2, shapeLangAvailable(ShapeLang::Lua));
    langCombo.setItemEnabled(3, shapeLangAvailable(ShapeLang::Python));
    langCombo.setSelectedId((int)curve.lang + 1, juce::dontSendNotification);
    langCombo.setTooltip("Language for the Equation. Built-in: `f` is the bin index "
                         "(0..N-1) over the curve. Lua / Python: `f` is the normalized "
                         "position 0..1, with full loops/variables (e.g. sum many "
                         "harmonics). Lua/Python are baked into the curve when you edit, "
                         "not run live; multi-line bodies must end with `return`.");
    langCombo.onChange = [this]() {
        curve.lang = (ShapeLang)(langCombo.getSelectedId() - 1);
        exprEditor.setMultiLine(curve.lang != ShapeLang::Builtin);
        exprEditor.setReturnKeyStartsNewLine(curve.lang != ShapeLang::Builtin);
        curve.rebake();
        updateModeUI();
        if (onChanged) onChanged();
        repaint();
    };

    updateModeUI();
}

void SpectralCurvePanel::syncFromModel() {
    if (curve.mode == SpectralCurve::Equation
        && exprEditor.getText().toStdString() != curve.expression)
    {
        exprEditor.setText(curve.expression, juce::dontSendNotification);
    }
    curve.rebake();   // ensure scriptSamples reflect the loaded expression/lang
    freehandToggle.setButtonText(curve.freehandMode ? "Freehand" : "Points");
    updateModeUI();
}

void SpectralCurvePanel::seedDrawnFromEquation() {
    auto highRes = curve.evaluate(kFreehandSampleCount);
    if ((int)highRes.size() != kFreehandSampleCount)
        highRes.assign(kFreehandSampleCount, 0.0f);
    curve.drawnSamples = highRes;

    constexpr int kSeedPoints = 32;
    curve.drawnPoints.clear();
    curve.drawnPoints.reserve((size_t)kSeedPoints);
    for (int i = 0; i < kSeedPoints; ++i) {
        const float x = (kSeedPoints > 1)
            ? (float)i / (float)(kSeedPoints - 1)
            : 0.0f;
        const int srcIdx = juce::jlimit(0, kFreehandSampleCount - 1,
            (int)std::round(x * (kFreehandSampleCount - 1)));
        curve.drawnPoints.emplace_back(x, highRes[(size_t)srcIdx]);
    }

    double sumSq = 0.0, sumOrigSq = 0.0;
    for (int i = 0; i < kFreehandSampleCount; ++i) {
        float x = (kFreehandSampleCount > 1)
            ? (float)i / (float)(kFreehandSampleCount - 1)
            : 0.0f;
        float pred = sampleDrawnPointsNonPeriodic(curve.drawnPoints, x);
        float orig = highRes[(size_t)i];
        float d = pred - orig;
        sumSq     += (double)d * d;
        sumOrigSq += (double)orig * orig;
    }
    double sigEnergy = std::max(sumOrigSq, 1e-9);
    double relErr = std::sqrt(sumSq / sigEnergy);
    if (relErr > 0.20)
        curve.freehandMode = true;
}

void SpectralCurvePanel::updateModeUI() {
    bool eq = (curve.mode == SpectralCurve::Equation);
    equationBtn.setToggleState(eq, juce::dontSendNotification);
    drawBtn.setToggleState(!eq, juce::dontSendNotification);
    exprEditor.setVisible(eq);
    langCombo.setVisible(eq);
    langCombo.setSelectedId((int)curve.lang + 1, juce::dontSendNotification);
    bool scripted = eq && curve.lang != ShapeLang::Builtin;
    exprEditor.setMultiLine(scripted);
    exprEditor.setReturnKeyStartsNewLine(scripted);
    freehandToggle.setVisible(!eq);
    freehandToggle.setButtonText(curve.freehandMode ? "Freehand" : "Points");
    resized();
    repaint();
}

void SpectralCurvePanel::resized() {
    auto a = getLocalBounds().reduced(4);
    auto top = a.removeFromTop(22);
    titleLabel.setBounds(top.removeFromLeft(120));

    freehandToggle.setBounds(top.removeFromRight(80));
    top.removeFromRight(4);
    drawBtn.setBounds(top.removeFromRight(60));
    equationBtn.setBounds(top.removeFromRight(72));

    if (curve.mode == SpectralCurve::Equation) {
        // Language combo on its own narrow row, then the expression editor -
        // taller and multi-line for Lua/Python so loops are practical to write.
        auto comboRow = a.removeFromTop(22);
        langCombo.setBounds(comboRow.removeFromLeft(90));
        a.removeFromTop(2);
        int exprH = (curve.lang == ShapeLang::Builtin) ? 24 : 84;
        exprEditor.setBounds(a.removeFromTop(exprH));
        a.removeFromTop(2);
    }
    canvasBounds = a;
}

juce::Rectangle<float> SpectralCurvePanel::getCanvasBoundsF() const {
    return canvasBounds.toFloat().reduced(2.0f);
}

void SpectralCurvePanel::paint(juce::Graphics& g) {
    g.setColour(juce::Colour(28, 28, 36));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
    g.setColour(juce::Colour(70, 70, 90));
    g.drawRoundedRectangle(getLocalBounds().toFloat(), 4.0f, 1.0f);

    auto cb = getCanvasBoundsF();
    g.setColour(juce::Colour(18, 18, 24));
    g.fillRoundedRectangle(cb, 3.0f);

    float cy;
    if (yMin < 0.0f) {
        cy = cb.getCentreY();
    } else {
        cy = cb.getBottom() - 4;
    }
    g.setColour(juce::Colours::grey.withAlpha(0.25f));
    g.drawHorizontalLine((int)cy, cb.getX(), cb.getRight());

    int nPreview = 256;
    std::vector<float> samples;
    if (curve.mode == SpectralCurve::Equation) {
        samples = curve.evaluate(nPreview);
        if (yMin >= 0.0f)
            for (auto& v : samples) if (v < 0.0f) v = 0.0f;
    } else {
        samples.assign(nPreview, 0.0f);
        for (int i = 0; i < nPreview; ++i) {
            float x = (float)i / (float)(nPreview - 1);
            if (curve.freehandMode)
                samples[i] = sampleDrawnSamplesAt(curve.drawnSamples, x);
            else
                samples[i] = sampleDrawnPointsNonPeriodic(curve.drawnPoints, x);
        }
    }

    juce::Path p;
    for (int i = 0; i < nPreview; ++i) {
        float x = cb.getX() + (float)i / (float)(nPreview - 1) * cb.getWidth();
        float y = yToPixel(samples[i], cb);
        if (i == 0) p.startNewSubPath(x, y);
        else p.lineTo(x, y);
    }
    g.setColour(curveColour);
    g.strokePath(p, juce::PathStrokeType(1.4f));

    if (curve.mode == SpectralCurve::Drawn && !curve.freehandMode) {
        for (int i = 0; i < (int)curve.drawnPoints.size(); ++i) {
            const auto& pt = curve.drawnPoints[i];
            float x = cb.getX() + pt.first * cb.getWidth();
            float y = yToPixel(pt.second, cb);
            bool isDragged = (i == draggingIdx);
            g.setColour(isDragged ? juce::Colours::yellow : juce::Colours::white);
            g.fillEllipse(x - 3.0f, y - 3.0f, 6.0f, 6.0f);
            g.setColour(juce::Colour(60, 90, 140));
            g.drawEllipse(x - 3.0f, y - 3.0f, 6.0f, 6.0f, 1.0f);
        }
    }

    // Surface a Lua/Python bake error over the canvas.
    if (curve.mode == SpectralCurve::Equation && !curve.scriptError.empty()) {
        g.setColour(juce::Colours::red.withAlpha(0.9f));
        g.setFont(11.0f);
        g.drawFittedText("Script error: " + juce::String(curve.scriptError),
                         cb.toNearestInt().reduced(4),
                         juce::Justification::topLeft, 3);
    }
}

float SpectralCurvePanel::yToPixel(float v, const juce::Rectangle<float>& cb) const {
    float t = (v - yMin) / (yMax - yMin);
    t = juce::jlimit(0.0f, 1.0f, t);
    return cb.getBottom() - t * cb.getHeight();
}

bool SpectralCurvePanel::mouseToCurveXY(juce::Point<float> p, float& outX, float& outY) const {
    auto cb = getCanvasBoundsF();
    if (!cb.contains(p)) return false;
    outX = (p.x - cb.getX()) / juce::jmax(1.0f, cb.getWidth());
    float t = 1.0f - (p.y - cb.getY()) / juce::jmax(1.0f, cb.getHeight());
    outY = yMin + t * (yMax - yMin);
    outX = juce::jlimit(0.0f, 1.0f, outX);
    outY = juce::jlimit(yMin, yMax, outY);
    return true;
}

int SpectralCurvePanel::findPointNear(float x, float y, float radius) const {
    int best = -1;
    float bestD2 = radius * radius;
    float yScale = 1.0f / juce::jmax(0.001f, yMax - yMin);
    for (int i = 0; i < (int)curve.drawnPoints.size(); ++i) {
        float dx = curve.drawnPoints[i].first - x;
        float dy = (curve.drawnPoints[i].second - y) * yScale;
        float d2 = dx * dx + dy * dy;
        if (d2 < bestD2) { bestD2 = d2; best = i; }
    }
    return best;
}

void SpectralCurvePanel::sortPointsByX() {
    std::sort(curve.drawnPoints.begin(), curve.drawnPoints.end(),
              [](auto& a, auto& b) { return a.first < b.first; });
}

void SpectralCurvePanel::writeFreehandSample(float x, float y) {
    auto& samples = curve.drawnSamples;
    if (samples.empty()) samples = defaultFreehandSamples();
    int n = (int)samples.size();
    int idx = juce::jlimit(0, n - 1, (int)(x * (float)(n - 1)));
    if (lastFreehandIdx >= 0 && lastFreehandIdx != idx) {
        int from = lastFreehandIdx, to = idx;
        float fromY = lastFreehandY, toY = y;
        int steps = std::abs(to - from);
        int dir = (to > from) ? 1 : -1;
        for (int s = 0; s <= steps; ++s) {
            int si = from + s * dir;
            if (si < 0 || si >= n) continue;
            float t = (steps > 0) ? (float)s / (float)steps : 1.0f;
            samples[si] = fromY + (toY - fromY) * t;
        }
    } else {
        samples[idx] = y;
    }
    lastFreehandIdx = idx;
    lastFreehandY = y;
}

void SpectralCurvePanel::mouseDown(const juce::MouseEvent& e) {
    if (curve.mode != SpectralCurve::Drawn) return;
    float x, y;
    if (!mouseToCurveXY(e.position, x, y)) return;

    if (curve.freehandMode) {
        freehandDrawing = true;
        lastFreehandIdx = -1;
        writeFreehandSample(x, y);
        if (onChanged) onChanged();
        repaint();
        return;
    }

    auto& pts = curve.drawnPoints;
    int hit = findPointNear(x, y);
    if (e.mods.isShiftDown() && hit >= 0) {
        if ((int)pts.size() > 2) {
            pts.erase(pts.begin() + hit);
            draggingIdx = -1;
            if (onChanged) onChanged();
            repaint();
        }
        return;
    }
    if (hit >= 0) {
        draggingIdx = hit;
    } else {
        pts.emplace_back(x, y);
        sortPointsByX();
        draggingIdx = -1;
        for (int i = 0; i < (int)pts.size(); ++i)
            if (std::abs(pts[i].first - x) < 1e-5f
                && std::abs(pts[i].second - y) < 1e-5f)
                { draggingIdx = i; break; }
        if (onChanged) onChanged();
        repaint();
    }
}

void SpectralCurvePanel::mouseDrag(const juce::MouseEvent& e) {
    if (curve.mode != SpectralCurve::Drawn) return;
    auto cb = getCanvasBoundsF();
    auto cp = e.position;
    cp.x = juce::jlimit(cb.getX(), cb.getRight() - 1.0f, cp.x);
    cp.y = juce::jlimit(cb.getY(), cb.getBottom(), cp.y);
    float x, y;
    mouseToCurveXY(cp, x, y);

    if (curve.freehandMode && freehandDrawing) {
        writeFreehandSample(x, y);
        if (onChanged) onChanged();
        repaint();
        return;
    }
    if (draggingIdx < 0) return;
    auto& pts = curve.drawnPoints;
    if (draggingIdx >= (int)pts.size()) { draggingIdx = -1; return; }
    pts[draggingIdx] = { x, y };
    float ox = x, oy = y;
    sortPointsByX();
    draggingIdx = -1;
    for (int i = 0; i < (int)pts.size(); ++i)
        if (std::abs(pts[i].first - ox) < 1e-5f
            && std::abs(pts[i].second - oy) < 1e-5f)
            { draggingIdx = i; break; }
    if (onChanged) onChanged();
    repaint();
}

void SpectralCurvePanel::mouseUp(const juce::MouseEvent&) {
    draggingIdx = -1;
    freehandDrawing = false;
    lastFreehandIdx = -1;
}

} // namespace SoundShop

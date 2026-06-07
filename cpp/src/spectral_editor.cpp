#define _USE_MATH_DEFINES
#include "spectral_editor.h"
#include "builtin_synth.h"   // WaveExprParser
#include "fft_util.h"        // FFT for the preview
#include "help_utils.h"
#include <cmath>
#include <sstream>
#include <algorithm>
#include <complex>

namespace SoundShop {

// ==============================================================================
// SpectralCurve - evaluation
// ==============================================================================

static constexpr int kFreehandSampleCount = 512;

// Default points for a fresh Drawn magnitude curve: a gentle decay so the
// user has something visible/grabable, matching the default exp(-f/20).
static std::vector<std::pair<float, float>> defaultMagPoints() {
    return {
        {0.00f, 1.00f},
        {0.10f, 0.55f},
        {0.25f, 0.25f},
        {0.50f, 0.08f},
        {0.80f, 0.02f},
    };
}

// Default points for a fresh Drawn phase curve: a flat zero phase, with
// a few points so the user can grab and reshape.
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

// Catmull-Rom through a non-periodic, sorted-by-x sequence.
// x is in [0, 1] (clamped). Outside the point range we hold the
// endpoint value rather than wrap (unlike the waveform editor, where
// the curve is periodic).
static float sampleDrawnPointsNonPeriodic(
    const std::vector<std::pair<float, float>>& pts, float x)
{
    int n = (int)pts.size();
    if (n == 0) return 0.0f;
    if (n == 1) return pts[0].second;
    if (x <= pts[0].first)      return pts[0].second;
    if (x >= pts[n - 1].first)  return pts[n - 1].second;

    // Find segment [pts[i1], pts[i2]] containing x
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

std::vector<float> SpectralCurve::evaluate(int halfBins) const {
    std::vector<float> out(halfBins, 0.0f);
    if (halfBins <= 0) return out;

    if (mode == Equation) {
        out = WaveExprParser::evaluateOverBins(expression, halfBins);
        return out;
    }

    // Drawn mode
    if (freehandMode) {
        for (int k = 0; k < halfBins; ++k) {
            float x = (halfBins > 1) ? (float)k / (float)(halfBins - 1) : 0.0f;
            out[k] = sampleDrawnSamplesAt(drawnSamples, x);
        }
    } else {
        for (int k = 0; k < halfBins; ++k) {
            float x = (halfBins > 1) ? (float)k / (float)(halfBins - 1) : 0.0f;
            out[k] = sampleDrawnPointsNonPeriodic(drawnPoints, x);
        }
    }
    return out;
}

// ==============================================================================
// SpectralDoc - encode / decode
// ==============================================================================

// The same escape strategy as layered_wave_editor.cpp: within a curve's
// field list (comma-separated), formula expressions can contain commas
// and pipes, so we substitute `,` -> `;` and `|` -> `\x1F` on encode and
// reverse on decode. Neither character is part of WaveExprParser grammar.
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

static void encodeCurve(std::ostringstream& o, const SpectralCurve& c) {
    if (c.mode == SpectralCurve::Equation) {
        o << "eq," << escapeFormula(c.expression);
    } else if (c.freehandMode) {
        o << "drawnfh," << c.drawnSamples.size();
        for (float v : c.drawnSamples) o << "," << v;
    } else {
        o << "drawnpt," << c.drawnPoints.size();
        for (auto& p : c.drawnPoints) o << "," << p.first << "," << p.second;
    }
}

static bool parseCurve(const std::string& body, SpectralCurve& out) {
    auto f = splitChar(body, ',');
    if (f.empty()) return false;
    const std::string& kind = f[0];
    out = SpectralCurve{};
    if (kind == "eq") {
        out.mode = SpectralCurve::Equation;
        if (f.size() > 1) out.expression = unescapeFormula(f[1]);
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

std::string SpectralDoc::encode() const {
    std::ostringstream o;
    o << "__spectral2__:" << fftSize << "|";
    encodeCurve(o, mag);
    o << "|";
    encodeCurve(o, phase);
    return o.str();
}

bool SpectralDoc::decode(const std::string& s) {
    // ---- New format ----
    if (s.rfind("__spectral2__:", 0) == 0) {
        std::string rest = s.substr(std::string("__spectral2__:").size());
        auto parts = splitChar(rest, '|');
        if (parts.size() < 3) return false;
        try { fftSize = std::stoi(parts[0]); } catch (...) { fftSize = 2048; }
        if (!parseCurve(parts[1], mag))   return false;
        if (!parseCurve(parts[2], phase)) return false;
        return true;
    }

    // ---- Legacy format: __spectral__:<fftSize>:<phaseMode>:<magExpr>|<phaseExpr> ----
    if (s.rfind("__spectral__:", 0) == 0) {
        std::string rest = s.substr(std::string("__spectral__:").size());
        auto c1 = rest.find(':');
        auto c2 = (c1 != std::string::npos) ? rest.find(':', c1 + 1)
                                            : std::string::npos;
        if (c1 == std::string::npos || c2 == std::string::npos) return false;
        try { fftSize = std::stoi(rest.substr(0, c1)); } catch (...) { fftSize = 2048; }
        int phaseMode = 1;
        try { phaseMode = std::stoi(rest.substr(c1 + 1, c2 - c1 - 1)); } catch (...) {}
        std::string body = rest.substr(c2 + 1);
        std::string magExpr, phaseExpr;
        auto bar = body.find('|');
        if (bar != std::string::npos) {
            magExpr   = body.substr(0, bar);
            phaseExpr = body.substr(bar + 1);
        } else {
            magExpr = body;
        }
        mag.mode = SpectralCurve::Equation;
        mag.expression = magExpr;
        phase.mode = SpectralCurve::Equation;
        switch (phaseMode) {
            case 0: phase.expression = phaseExpr.empty() ? "0" : phaseExpr; break;
            case 1: phase.expression = "noise()*pi"; break;
            case 2: phase.expression = "0"; break;
            case 3: phase.expression = "-pi*f"; break;
            default: phase.expression = "0"; break;
        }
        return true;
    }
    return false;
}

SpectralDoc SpectralDoc::defaultBuiltin() {
    SpectralDoc d;
    d.fftSize = 2048;
    d.mag.mode = SpectralCurve::Equation;
    d.mag.expression = "exp(-f/20)";
    d.phase.mode = SpectralCurve::Equation;
    d.phase.expression = "noise()*pi";
    return d;
}

// =============================================================================
// SpectralDoc -> time-domain waveform
// =============================================================================
//
// Shared by Terrain::fillFromSpectralDoc and SpectralFrame::render so the
// rendering rules (power-of-two rounding, DC kill, peak normalisation) live
// in one place. The caller picks the table size; the SpectralCurves are
// evaluated at the matching halfBins regardless of doc.fftSize.
void renderSpectralToWaveform(const SpectralDoc& doc,
                              int tableSize,
                              std::vector<float>& out)
{
    int n = 2;
    while (n < tableSize && n < 16384) n <<= 1;
    if (n < 2) n = 2;

    int halfBins = n / 2 + 1;
    auto mags   = doc.mag.evaluate(halfBins);
    auto phases = doc.phase.evaluate(halfBins);
    for (auto& m : mags) if (m < 0.0f) m = 0.0f;

    std::vector<std::complex<float>> spectrum(halfBins);
    for (int k = 0; k < halfBins; ++k) {
        float m = mags[k];
        float p = phases[k];
        if (k == 0 || k == halfBins - 1)
            spectrum[k] = std::complex<float>(m, 0.0f);
        else
            spectrum[k] = std::complex<float>(m * std::cos(p),
                                              m * std::sin(p));
    }
    spectrum[0] = 0.0f; // kill DC

    FFT fft(n);
    fft.inverseReal(spectrum, out);

    float peak = 0.0f;
    for (float v : out) peak = std::max(peak, std::abs(v));
    if (peak > 1e-9f) {
        float inv = 1.0f / peak;
        for (float& v : out) v *= inv;
    }
}

// =============================================================================
// SpectralFrame - IWavetableFrame adapter
// =============================================================================

void SpectralFrame::render(int tableSize, std::vector<float>& out) const {
    renderSpectralToWaveform(doc, tableSize, out);
}

std::string SpectralFrame::encodeBody() const {
    // Strip the "__spectral2__:" prefix; the container length-prefixes the
    // body so embedded ':' and '|' are safe.
    auto full = doc.encode();
    const std::string prefix = "__spectral2__:";
    if (full.rfind(prefix, 0) == 0) return full.substr(prefix.size());
    return full;
}

bool SpectralFrame::decodeBody(const std::string& body) {
    // SpectralDoc::decode accepts only prefixed input; reattach the prefix
    // so the existing parser is happy.
    return doc.decode("__spectral2__:" + body);
}

std::unique_ptr<IWavetableFrame> SpectralFrame::clone() const {
    return std::make_unique<SpectralFrame>(*this);
}

// ==============================================================================
// CurvePanel - one mag-or-phase panel (mode toggle + editor area)
// ==============================================================================
//
// Layout (top-down):
//   [Title]                                   [Equation | Draw] [Points/Freehand]
//   [editor area: text field OR draw canvas]
//
// The draw canvas supports clicking to add/move points (Points mode) or
// click-drag to draw samples (Freehand mode), same UX as the waveform
// editor's Drawn shape - so it's instantly familiar to anyone who's used
// either editor.
class SpectralEditorComponent::CurvePanel : public juce::Component {
public:
    CurvePanel(SpectralEditorComponent& owner_,
               SpectralCurve& curve_,
               const juce::String& title_,
               float yMin_, float yMax_,
               juce::Colour curveColour_)
        : owner(owner_), curve(curve_), title(title_),
          yMin(yMin_), yMax(yMax_), curveColour(curveColour_)
    {
        addAndMakeVisible(titleLabel);
        titleLabel.setText(title, juce::dontSendNotification);
        titleLabel.setFont(juce::Font(14.0f, juce::Font::bold));

        // Mode buttons
        addAndMakeVisible(equationBtn);
        equationBtn.setButtonText("Equation");
        equationBtn.setClickingTogglesState(true);
        equationBtn.setTooltip("Author this curve as a formula in `f` (the FFT bin index, 0..halfBins-1). "
                               "Vocabulary: sin, cos, tan, exp, log, sqrt, pow, abs, tanh, clamp, "
                               "saw(f), square(f), triangle(f), noise(), random, pi, e.");
        equationBtn.onClick = [this]() {
            curve.mode = SpectralCurve::Equation;
            updateModeUI();
            owner.onCurveChanged();
        };

        addAndMakeVisible(drawBtn);
        drawBtn.setButtonText("Draw");
        drawBtn.setClickingTogglesState(true);
        drawBtn.setTooltip("Author this curve graphically. In Points mode, click to add control points "
                           "(shift-click to delete, drag to move) and the curve is interpolated through them. "
                           "In Freehand mode, click and drag to paint the curve directly.");
        drawBtn.onClick = [this]() {
            // Equation -> Draw: seed the drawn data from the equation's
            // current evaluation so the displayed shape doesn't jump when
            // the user clicks Draw. Without this, the canvas snaps to a
            // generic default (gentle decay for magnitude, flat zeros for
            // phase) regardless of what the equation was producing - so
            // a noise-textured phase or a custom magnitude shape both
            // visibly disappear at the moment the user expects to start
            // editing what they were already seeing. We evaluate BEFORE
            // flipping mode because evaluate() routes through whatever
            // mode is currently set.
            if (curve.mode == SpectralCurve::Equation) {
                seedDrawnFromEquation();
            } else if (curve.drawnPoints.empty()) {
                // Already in Drawn mode with no prior data - fall back
                // to the generic defaults so the user has something to
                // grab.
                curve.drawnPoints = (yMin < 0.0f) ? defaultPhasePoints()
                                                  : defaultMagPoints();
            }
            curve.mode = SpectralCurve::Drawn;
            updateModeUI();
            owner.onCurveChanged();
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
            owner.onCurveChanged();
            repaint();
        };

        // Equation editor
        addAndMakeVisible(exprEditor);
        exprEditor.setMultiLine(false);
        exprEditor.setReturnKeyStartsNewLine(false);
        exprEditor.setText(curve.expression, juce::dontSendNotification);
        exprEditor.onTextChange = [this]() {
            curve.expression = exprEditor.getText().toStdString();
            owner.onCurveChanged();
        };

        updateModeUI();
    }

    void syncFromModel() {
        if (curve.mode == SpectralCurve::Equation
            && exprEditor.getText().toStdString() != curve.expression)
        {
            exprEditor.setText(curve.expression, juce::dontSendNotification);
        }
        freehandToggle.setButtonText(curve.freehandMode ? "Freehand" : "Points");
        updateModeUI();
    }

    // Take a snapshot of the equation's current evaluation and seed both
    // drawnSamples (for Freehand mode) and drawnPoints (for Points mode)
    // from it. Called on Equation -> Draw transitions so the canvas
    // continues showing the same shape the user was already looking at.
    //
    // We evaluate the equation once at kFreehandSampleCount resolution
    // and use that single sample set for both representations: the
    // freehand storage stores it directly, and the points storage takes
    // a downsampled subset. Sharing the source matters for noise() and
    // random expressions - re-evaluating would give different texture
    // each time, and Points / Freehand would visibly disagree if the
    // user toggled the freehandMode switch right after seeding.
    void seedDrawnFromEquation() {
        // Evaluate at high resolution so noise/textured equations keep
        // their detail in Freehand mode.
        auto highRes = curve.evaluate(kFreehandSampleCount);
        if ((int)highRes.size() != kFreehandSampleCount)
            highRes.assign(kFreehandSampleCount, 0.0f);
        curve.drawnSamples = highRes;

        // Points mode: 32 evenly-spaced samples from the same evaluation,
        // enough for the user to grab and reshape without being overwhelmed
        // by control points. Higher counts make a noise curve unusable for
        // editing; lower counts lose the equation's shape entirely.
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

        // Decide between Points mode (32 Catmull-Rom anchors) and Freehand
        // mode (512 stored samples) based on whether the 32-anchor curve
        // can actually reproduce what the equation was showing. For smooth
        // shapes (gentle decays, low-frequency sines) the anchors fit fine
        // and Points mode gives the user the simpler editing experience.
        // For high-frequency content (noise(), random, fast saw(f)/square(f)
        // / triangle(f), large coefficients on f inside sin/cos), 32 anchors
        // smooth out the detail to mush - so we auto-pick Freehand instead
        // and the user keeps drawing on top of the equation's actual shape.
        //
        // Heuristic: compare the Catmull-Rom resampling of the 32 anchors
        // back to the true equation samples and compute relative RMS error
        // (error energy / signal energy). If reconstruction is off by more
        // than ~20% of the signal's own RMS, the equation has more detail
        // than 32 anchors can hold and Freehand is the honest choice. Pure
        // noise() produces relErr near 1.0; smooth curves are well under
        // 0.05; the 0.20 threshold sits comfortably between those regimes.
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

    void updateModeUI() {
        bool eq = (curve.mode == SpectralCurve::Equation);
        equationBtn.setToggleState(eq, juce::dontSendNotification);
        drawBtn.setToggleState(!eq, juce::dontSendNotification);
        exprEditor.setVisible(eq);
        freehandToggle.setVisible(!eq);
        // Keep the Points/Freehand button label in sync with curve.freehandMode.
        // seedDrawnFromEquation() may auto-flip freehandMode when the equation
        // has too much detail for 32 Catmull-Rom anchors to represent; without
        // updating the label here the button would still read "Points" while
        // the editor is actually in Freehand mode.
        freehandToggle.setButtonText(curve.freehandMode ? "Freehand" : "Points");
        // Equation vs Drawn changes whether the expression text field is
        // visible, which moves the canvas top edge - re-run our layout so
        // canvasBounds reflects the new free area before the next paint.
        resized();
        repaint();
    }

    void resized() override {
        auto a = getLocalBounds().reduced(4);
        auto top = a.removeFromTop(22);
        titleLabel.setBounds(top.removeFromLeft(120));

        // Mode controls on the right of the title row
        freehandToggle.setBounds(top.removeFromRight(80));
        top.removeFromRight(4);
        drawBtn.setBounds(top.removeFromRight(60));
        equationBtn.setBounds(top.removeFromRight(72));

        // Below the title row: either the expression editor or the canvas.
        if (curve.mode == SpectralCurve::Equation) {
            exprEditor.setBounds(a.removeFromTop(24));
        }
        canvasBounds = a;
    }

    juce::Rectangle<float> getCanvasBoundsF() const {
        return canvasBounds.toFloat().reduced(2.0f);
    }

    void paint(juce::Graphics& g) override {
        // Background
        g.setColour(juce::Colour(28, 28, 36));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
        g.setColour(juce::Colour(70, 70, 90));
        g.drawRoundedRectangle(getLocalBounds().toFloat(), 4.0f, 1.0f);

        auto cb = getCanvasBoundsF();
        g.setColour(juce::Colour(18, 18, 24));
        g.fillRoundedRectangle(cb, 3.0f);

        // Center / baseline line
        float cy;
        if (yMin < 0.0f) {
            cy = cb.getCentreY();   // phase: zero in the middle
        } else {
            cy = cb.getBottom() - 4; // mag: zero at bottom
        }
        g.setColour(juce::Colours::grey.withAlpha(0.25f));
        g.drawHorizontalLine((int)cy, cb.getX(), cb.getRight());

        // Sample the curve at nPreview points across the bin range so we
        // can draw it. Drawn modes sample directly out of the stored
        // curve; Equation mode evaluates the expression once per paint
        // (same parser as the synth-side renderer, so what you see is
        // what you get).
        int nPreview = 256;
        std::vector<float> samples;
        if (curve.mode == SpectralCurve::Equation) {
            samples = curve.evaluate(nPreview);
            // Magnitude is non-negative on the synth side; mirror that
            // so the plot doesn't mislead about what will be heard.
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

        // Overlay control points in Points mode (Drawn only; Equation
        // mode has no points to grab).
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
    }

    // Convert a sample value to pixel y inside the canvas.
    float yToPixel(float v, const juce::Rectangle<float>& cb) const {
        float t = (v - yMin) / (yMax - yMin); // 0..1
        t = juce::jlimit(0.0f, 1.0f, t);
        return cb.getBottom() - t * cb.getHeight();
    }

    bool mouseToCurveXY(juce::Point<float> p, float& outX, float& outY) const {
        auto cb = getCanvasBoundsF();
        if (!cb.contains(p)) return false;
        outX = (p.x - cb.getX()) / juce::jmax(1.0f, cb.getWidth());
        float t = 1.0f - (p.y - cb.getY()) / juce::jmax(1.0f, cb.getHeight());
        outY = yMin + t * (yMax - yMin);
        outX = juce::jlimit(0.0f, 1.0f, outX);
        outY = juce::jlimit(yMin, yMax, outY);
        return true;
    }

    int findPointNear(float x, float y, float radius = 0.04f) const {
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

    void sortPointsByX() {
        std::sort(curve.drawnPoints.begin(), curve.drawnPoints.end(),
                  [](auto& a, auto& b) { return a.first < b.first; });
    }

    void writeFreehandSample(float x, float y) {
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

    void mouseDown(const juce::MouseEvent& e) override {
        if (curve.mode != SpectralCurve::Drawn) return;
        float x, y;
        if (!mouseToCurveXY(e.position, x, y)) return;

        if (curve.freehandMode) {
            freehandDrawing = true;
            lastFreehandIdx = -1;
            writeFreehandSample(x, y);
            owner.onCurveChanged();
            repaint();
            return;
        }

        auto& pts = curve.drawnPoints;
        int hit = findPointNear(x, y);
        if (e.mods.isShiftDown() && hit >= 0) {
            if ((int)pts.size() > 2) {
                pts.erase(pts.begin() + hit);
                draggingIdx = -1;
                owner.onCurveChanged();
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
            owner.onCurveChanged();
            repaint();
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        if (curve.mode != SpectralCurve::Drawn) return;
        auto cb = getCanvasBoundsF();
        auto cp = e.position;
        cp.x = juce::jlimit(cb.getX(), cb.getRight() - 1.0f, cp.x);
        cp.y = juce::jlimit(cb.getY(), cb.getBottom(), cp.y);
        float x, y;
        mouseToCurveXY(cp, x, y);

        if (curve.freehandMode && freehandDrawing) {
            writeFreehandSample(x, y);
            owner.onCurveChanged();
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
        owner.onCurveChanged();
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override {
        draggingIdx = -1;
        freehandDrawing = false;
        lastFreehandIdx = -1;
    }

private:
    SpectralEditorComponent& owner;
    SpectralCurve& curve;
    juce::String title;
    float yMin, yMax;
    juce::Colour curveColour;

    juce::Label      titleLabel;
    juce::TextButton equationBtn, drawBtn;
    juce::TextButton freehandToggle;
    juce::TextEditor exprEditor;

    juce::Rectangle<int> canvasBounds;
    int draggingIdx = -1;
    bool freehandDrawing = false;
    int  lastFreehandIdx = -1;
    float lastFreehandY = 0.0f;
};

// ==============================================================================
// SpectralEditorComponent
// ==============================================================================

SpectralEditorComponent::SpectralEditorComponent(NodeGraph& g, int id,
                                                  std::function<void()> apply)
    : graph(&g), nodeId(id), externalFrame(nullptr), onApply(std::move(apply))
{
    if (auto* nd = graph->findNode(nodeId)) {
        if (!doc.decode(nd->script))
            doc = SpectralDoc::defaultBuiltin();
    } else {
        doc = SpectralDoc::defaultBuiltin();
    }
    initUI();
}

SpectralEditorComponent::SpectralEditorComponent(SpectralFrame& frame,
                                                  std::function<void()> apply)
    : graph(nullptr), nodeId(0), externalFrame(&frame), onApply(std::move(apply))
{
    // Seed the editor from the frame's existing doc. No NodeGraph: commits
    // mirror straight back into externalFrame->doc.
    doc = frame.doc;
    initUI();
}

void SpectralEditorComponent::initUI() {
    // FFT size combo
    addAndMakeVisible(fftSizeLabel);
    fftSizeLabel.setText("FFT size:", juce::dontSendNotification);
    fftSizeLabel.setFont(13.0f);
    addAndMakeVisible(fftSizeCombo);
    fftSizeCombo.addItem("512",  1);
    fftSizeCombo.addItem("1024", 2);
    fftSizeCombo.addItem("2048", 3);
    fftSizeCombo.addItem("4096", 4);
    int sel = (doc.fftSize <= 512) ? 1 : (doc.fftSize <= 1024) ? 2
            : (doc.fftSize <= 2048) ? 3 : 4;
    fftSizeCombo.setSelectedId(sel, juce::dontSendNotification);
    fftSizeCombo.setTooltip("How many FFT bins to use to render this wavetable. "
                            "More bins = finer spectral detail and a longer single-cycle waveform. "
                            "Higher values cost more memory but the synth's anti-aliased mip pyramid keeps CPU cost similar.");
    fftSizeCombo.onChange = [this]() {
        int s = fftSizeCombo.getSelectedId();
        doc.fftSize = (s == 1) ? 512 : (s == 2) ? 1024 : (s == 3) ? 2048 : 4096;
        onCurveChanged();
    };

    // Curve panels
    phasePanel = std::make_unique<CurvePanel>(*this, doc.phase, "Phase",
        -3.14159265f, 3.14159265f, juce::Colour(255, 180, 100));
    magPanel   = std::make_unique<CurvePanel>(*this, doc.mag,   "Magnitude",
        0.0f, 1.0f, juce::Colour(150, 200, 255));
    addAndMakeVisible(phasePanel.get());
    addAndMakeVisible(magPanel.get());

    // Apply/Close belong to the standalone-dialog mode where this editor
    // owns its own DialogWindow. When the editor is frame-backed (embedded
    // into the wavetable editor as the per-frame editing slot), commits
    // happen continuously through the onApply callback and there's no
    // owning dialog to close, so both buttons are hidden.
    const bool standaloneDialogMode = (externalFrame == nullptr);
    if (standaloneDialogMode) {
        addAndMakeVisible(applyBtn);
        applyBtn.setTooltip("Push the current spectrum to the node now (it is also auto-applied a half-second after edits stop).");
        applyBtn.onClick = [this]() { commitToNode(); if (onApply) onApply(); };
        addAndMakeVisible(closeBtn);
        closeBtn.setTooltip("Close this editor. Unapplied edits are committed first.");
        closeBtn.onClick = [this]() {
            commitToNode(); if (onApply) onApply();
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState(0);
        };
    }
    addAndMakeVisible(helpBtn);
    helpBtn.setTooltip("Open the help page for the frequency-domain editor.");
    helpBtn.onClick = []() { openHelpDocFile("wavetables.html"); };

    refreshPreview();
    setSize(720, 520);
}

SpectralEditorComponent::~SpectralEditorComponent() {
    stopTimer();
}

void SpectralEditorComponent::resized() {
    auto a = getLocalBounds().reduced(8);

    // Top row: FFT size + buttons. Apply/Close only appear in standalone
    // dialog mode; in embedded mode they're not parented at all, so we
    // skip their slots to give the FFT-size combo more breathing room.
    auto top = a.removeFromTop(28);
    fftSizeLabel.setBounds(top.removeFromLeft(70));
    fftSizeCombo.setBounds(top.removeFromLeft(80));
    if (externalFrame == nullptr) {
        closeBtn.setBounds(top.removeFromRight(72));
        top.removeFromRight(4);
        applyBtn.setBounds(top.removeFromRight(72));
        top.removeFromRight(4);
    }
    helpBtn.setBounds(top.removeFromRight(24));

    a.removeFromTop(4);

    // Bottom: time-domain preview strip. Only shown in standalone mode -
    // when this editor is embedded inside the wavetable shell as the
    // per-frame editor for a Spectral frame, the shell already paints a
    // larger single-cycle preview at the bottom of its own area, so we'd
    // just be duplicating it (and at a smaller size). Skipping the strip
    // here also gives the mag / phase panels the full vertical space.
    if (externalFrame == nullptr) {
        auto previewArea = a.removeFromBottom(80);
        (void)previewArea; // drawn in paint()
        a.removeFromBottom(4);
    }

    // Two stacked curve panels: phase on top, mag on bottom.
    int half = a.getHeight() / 2;
    phasePanel->setBounds(a.removeFromTop(half - 2));
    a.removeFromTop(4);
    magPanel->setBounds(a);
}

void SpectralEditorComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(22, 22, 28));

    // Embedded mode: the wavetable shell already paints a "Single-cycle
    // waveform" strip below us, so we skip our own to avoid duplicating it.
    if (externalFrame != nullptr) return;

    // Time-domain preview strip at the bottom
    auto bounds = getLocalBounds().reduced(8);
    bounds.removeFromTop(28 + 4);
    auto previewArea = bounds.removeFromBottom(80).toFloat().reduced(2.0f);

    g.setColour(juce::Colour(18, 18, 24));
    g.fillRoundedRectangle(previewArea, 3.0f);
    g.setColour(juce::Colour(70, 70, 90));
    g.drawRoundedRectangle(previewArea, 3.0f, 1.0f);

    float cy = previewArea.getCentreY();
    g.setColour(juce::Colours::grey.withAlpha(0.25f));
    g.drawHorizontalLine((int)cy, previewArea.getX(), previewArea.getRight());

    if (!previewSamples.empty()) {
        juce::Path p;
        int n = (int)previewSamples.size();
        float w = previewArea.getWidth() - 4;
        float h = previewArea.getHeight() - 4;
        float cx = previewArea.getX() + 2;
        for (int i = 0; i < n; ++i) {
            float x = cx + (float)i / (float)(n - 1) * w;
            float y = cy - previewSamples[i] * h * 0.45f;
            if (i == 0) p.startNewSubPath(x, y);
            else p.lineTo(x, y);
        }
        g.setColour(juce::Colour(180, 220, 255));
        g.strokePath(p, juce::PathStrokeType(1.3f));
    }

    // Label sits *inside* the preview rectangle's top-left corner. Drawing
    // it above (the old behaviour) put the label 14 px up into the
    // magnitude panel, which painted over it and made the strip look
    // unlabeled.
    g.setColour(juce::Colours::lightgrey);
    g.setFont(11.0f);
    g.drawText("Resulting waveform (one cycle, IFFT of magnitude * phase above)",
               previewArea.reduced(6.0f, 3.0f).toNearestInt(),
               juce::Justification::topLeft);
}

void SpectralEditorComponent::timerCallback() {
    stopTimer();
    commitToNode();
    if (onApply) onApply();
}

void SpectralEditorComponent::refreshPreview() {
    // Round fftSize up to power of two, matching the synth-side logic.
    int n = 2;
    while (n < doc.fftSize && n < 16384) n <<= 1;
    int halfBins = n / 2 + 1;

    auto mags   = doc.mag.evaluate(halfBins);
    auto phases = doc.phase.evaluate(halfBins);
    // Clamp magnitudes to >= 0 (mag must be non-negative; freehand could
    // produce a slight negative from drawing below the baseline).
    for (auto& m : mags) if (m < 0.0f) m = 0.0f;

    std::vector<std::complex<float>> spectrum(halfBins);
    for (int k = 0; k < halfBins; ++k) {
        float m = mags[k];
        float ph = phases[k];
        if (k == 0 || k == halfBins - 1)
            spectrum[k] = std::complex<float>(m, 0.0f);
        else
            spectrum[k] = std::complex<float>(m * std::cos(ph), m * std::sin(ph));
    }
    spectrum[0] = 0.0f;  // kill DC

    FFT fft(n);
    fft.inverseReal(spectrum, previewSamples);

    float peak = 0.0f;
    for (float v : previewSamples) peak = std::max(peak, std::abs(v));
    if (peak > 1e-9f) {
        float inv = 1.0f / peak;
        for (float& v : previewSamples) v *= inv;
    }
    repaint();
}

void SpectralEditorComponent::commitToNode() {
    if (externalFrame) {
        // Frame-backed: mirror the working doc into the owned SpectralFrame.
        // The hosting wavetable editor will re-encode the parent wavetable
        // and push it through via onApply().
        externalFrame->doc = doc;
    } else if (graph) {
        if (auto* nd = graph->findNode(nodeId)) {
            nd->script = doc.encode();
            // Mark the project dirty so quit-without-save prompts and
            // autosave both notice these edits. (The externalFrame branch
            // above is inside a layered-wave parent that bumps the flag
            // through its own commitToNode.)
            graph->dirty = true;
        }
    }
}

void SpectralEditorComponent::onCurveChanged() {
    refreshPreview();
    if (externalFrame != nullptr) {
        // Embedded inside the layered-wave editor: commit immediately so the
        // parent's f->render() sees fresh data, and call onApply right away
        // so the parent re-paints its single-cycle preview every drag tick.
        // The parent already debounces its own audio-graph rebuild (150ms),
        // so we don't add extra latency by doing this. Without this the
        // parent preview only refreshed 500ms after the user stopped editing,
        // which looked like "phase edits don't affect the resulting wave".
        commitToNode();
        if (onApply) onApply();
        return;
    }
    // Standalone (node-graph) mode: onApply triggers an audio-graph rebuild
    // (requestRebuild()), which is expensive — keep the 500ms debounce so
    // typing in the equation field doesn't fight the audio thread.
    startTimer(500);
}

} // namespace SoundShop

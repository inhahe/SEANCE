#pragma once
#include "shape_expr.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <string>
#include <functional>

namespace SoundShop {

// ==============================================================================
// SpectralCurve - a reusable 1D curve over a [0, 1] x-axis with three
// authoring modes:
//   - Equation:  text expression in `f` (e.g. "exp(-f/20)") evaluated via
//                WaveExprParser, with `f` ranging across [0, halfBins).
//   - Drawn / Points:    Catmull-Rom interpolation through user-placed
//                        (x, y) control points.
//   - Drawn / Freehand:  per-sample painted values (fixed 512-sample
//                        buffer, linearly resampled on evaluate).
//
// The same struct is used in multiple places:
//   - SpectralDoc magnitude + phase curves (frequency-domain wavetables)
//   - SpectrumTap per-bin custom frequency response curves
// and is intended to be reusable anywhere a user-authored 1D shape is
// needed.
// ==============================================================================
struct SpectralCurve {
    enum Mode { Equation, Drawn };
    Mode mode = Equation;
    std::string expression = "exp(-f/20)";
    bool freehandMode = false;
    std::vector<std::pair<float, float>> drawnPoints;
    std::vector<float> drawnSamples;

    // Authoring language for the Equation expression.
    //   Built-in - WaveExprParser, `f` is the integer bin index over [0, N);
    //              evaluated live at the requested N.
    //   Lua / Python - `f` is the normalized position [0, 1]; baked once into
    //              scriptSamples (UI thread) and resampled on evaluate(), since
    //              the interpreters aren't safe to call from the audio thread.
    ShapeLang lang = ShapeLang::Builtin;

    // Transient (not serialized): the Lua/Python bake over normalized [0,1] and
    // the last bake error (empty when OK). Re-baked by rebake().
    std::vector<float> scriptSamples;
    std::string        scriptError;

    // Re-bake scriptSamples from `expression` for the current language. No-op
    // (clears the buffer) for Built-in. Call after editing the expression /
    // language and after decoding from a project.
    void rebake();

    // Evaluate this curve at N evenly-spaced samples spanning [0, 1].
    std::vector<float> evaluate(int N) const;

    // Compact serialization safe to embed inside a '|'-delimited blob.
    // The encoded form contains no '|' characters (formula expressions
    // have '|' escaped to \x1F, and the drawn modes use commas only).
    std::string encode() const;
    static bool decode(const std::string& s, SpectralCurve& out);
};

// ==============================================================================
// SpectralCurvePanel - reusable editor component for one SpectralCurve.
//
// Layout:
//   [Title]                          [Equation | Draw] [Points/Freehand]
//   [editor area: text field OR draw canvas]
//
// In Drawn mode the canvas supports:
//   - Points sub-mode: click to add a point, drag to move, shift-click to
//                      delete (minimum 2 points retained).
//   - Freehand sub-mode: click-drag to paint per-sample values.
//
// onChanged() fires whenever the user edits anything. Use it to refresh
// any preview / commit to the underlying model.
// ==============================================================================
class SpectralCurvePanel : public juce::Component {
public:
    SpectralCurvePanel(SpectralCurve& curve,
                       const juce::String& title,
                       float yMin, float yMax,
                       juce::Colour curveColour,
                       std::function<void()> onChanged);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

    // Call when the underlying SpectralCurve was changed externally (e.g.
    // loaded from a file or restored by undo) so the panel's text editor
    // and mode toggles reflect the new state.
    void syncFromModel();

private:
    SpectralCurve& curve;
    juce::String title;
    float yMin, yMax;
    juce::Colour curveColour;
    std::function<void()> onChanged;

    juce::Label      titleLabel;
    juce::TextButton equationBtn, drawBtn;
    juce::TextButton freehandToggle;
    juce::TextEditor exprEditor;
    juce::ComboBox   langCombo;   // Built-in / Lua / Python (Equation mode)

    juce::Rectangle<int> canvasBounds;
    int draggingIdx = -1;
    bool freehandDrawing = false;
    int  lastFreehandIdx = -1;
    float lastFreehandY = 0.0f;

    void updateModeUI();
    void seedDrawnFromEquation();

    juce::Rectangle<float> getCanvasBoundsF() const;
    float yToPixel(float v, const juce::Rectangle<float>& cb) const;
    bool  mouseToCurveXY(juce::Point<float> p, float& outX, float& outY) const;
    int   findPointNear(float x, float y, float radius = 0.04f) const;
    void  sortPointsByX();
    void  writeFreehandSample(float x, float y);
};

} // namespace SoundShop

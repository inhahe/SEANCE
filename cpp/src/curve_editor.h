#pragma once
#include "shape_expr.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <string>
#include <functional>
#include <cstdlib>

namespace SoundShop {

struct NodeGraph;  // for showFrequencyGraphLibraryMenu (defined in curve_editor.cpp)

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

    // Lock/unlock all editing. When read-only, the expression field, mode
    // toggles and language combo are disabled and the canvas ignores mouse
    // edits; a "linked - read only" badge is drawn. Used while the curve is a
    // live link to a library asset: the only way to edit a linked curve is to
    // edit it in the library, or to break the link and fork a copy. See the
    // FrequencyGraph library model in REFERENCE.md.
    void setReadOnly(bool ro);
    bool isReadOnly() const { return readOnly; }

    // Optional unlink affordance. When set AND the panel is read-only, the
    // "linked" badge becomes a button reading "linked - click to edit a copy";
    // clicking it invokes this callback (which the consumer wires to break the
    // library link and fork an independent copy). This is the ONLY unlink path -
    // unlinking is deliberately NOT in the library popup menu (it's a node-state
    // action, not a library operation). When null the badge is a plain,
    // non-interactive "linked - read only" label.
    std::function<void()> onUnlink;

private:
    SpectralCurve& curve;
    juce::String title;
    float yMin, yMax;
    juce::Colour curveColour;
    std::function<void()> onChanged;
    bool readOnly = false;
    juce::Rectangle<float> badgeBounds;  // hit area for the unlink badge (read-only)

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

// ==============================================================================
// showFrequencyGraphLibraryMenu - the shared publish / load-copy / sync popup
// for any SpectralCurve that can be backed by a project FrequencyGraph asset.
//
// Library model (see REFERENCE.md "FrequencyGraph library"): loading a library
// curve FORKS by default (independent copy); syncing is opt-in and makes the
// curve a live, READ-ONLY mirror of the asset. The only way to change a shared
// library item is to edit it in the library; a synced consumer never writes back.
// To diverge from a sync, click the "linked - click to edit a copy" badge on the
// curve panel (SpectralCurvePanel::onUnlink) - unlinking is intentionally NOT a
// library-menu item, since it's a node-state action, not a library operation.
//
// Menu items:
//   - "Add this curve to library"          : publishes a copy; node stays independent.
//   - "Load a copy from library"           : picks an asset, copies it in (independent).
//   - "Sync with library curve (read-only)": picks an asset, mirrors it in as a live link.
//
// Every consumer stores its asset id in a different place, so the menu is
// parameterised by callbacks rather than owning the id:
//   - anchor      : component the popup is positioned against.
//   - graph       : the project graph whose `assets` store is the library.
//   - curve       : the live curve; on "publish" its encode() seeds the new
//                   asset, on "load copy"/"sync" the picked asset's payload is
//                   decoded INTO it (so the caller can re-encode / refresh UI).
//   - currentId   : the curve's current link (-1 = independent). Retained for API
//                   compatibility; no longer consulted (unlink moved to the badge).
//   - defaultName : name for a newly-published asset.
//   - onChanged   : invoked after any change with the new asset id: the linked id
//                   for "sync", or -1 for publish / load-copy. The caller stores
//                   the id, re-encodes its node script, refreshes its panel, and
//                   sets the panel read-only iff the id >= 0. NOT called if the
//                   user dismisses the menu.
// ==============================================================================
void showFrequencyGraphLibraryMenu(juce::Component* anchor,
                                   NodeGraph& graph,
                                   SpectralCurve& curve,
                                   int currentId,
                                   const juce::String& defaultName,
                                   std::function<void(int)> onChanged);

// ==============================================================================
// Curve EQ node script (`__curveeq__:`) - a single magnitude-response
// SpectralCurve (gain multiplier vs. frequency, evaluated per FFT bin) with an
// optional FrequencyGraph asset link. One source of truth for the format,
// shared by the processor (builtin_effects.h), the editor (spectral_editor.cpp)
// and the resolver below. Format:
//   __curveeq__:<curve.encode()>[|refs:<assetId>]
// curve.encode() never emits '|', so the optional "|refs:" tail is unambiguous.
// ==============================================================================
namespace CurveEq {
    inline const char* kPrefix() { return "__curveeq__:"; }

    inline std::string encode(const SpectralCurve& curve, int assetId) {
        std::string s = std::string(kPrefix()) + curve.encode();
        if (assetId >= 0) s += "|refs:" + std::to_string(assetId);
        return s;
    }

    // Returns false (and leaves outCurve/outAssetId at defaults) if `script` is
    // not a Curve EQ script. outAssetId is -1 when there is no link.
    inline bool decode(const std::string& script, SpectralCurve& outCurve,
                       int& outAssetId) {
        outAssetId = -1;
        const std::string pfx = kPrefix();
        if (script.rfind(pfx, 0) != 0) return false;
        std::string rest = script.substr(pfx.size());
        std::string curveEnc = rest;
        auto bar = rest.find("|refs:");
        if (bar != std::string::npos) {
            curveEnc = rest.substr(0, bar);
            outAssetId = std::atoi(rest.substr(bar + 6).c_str());
        }
        return SpectralCurve::decode(curveEnc, outCurve);
    }
}

// Re-mirror every Curve EQ node's linked FrequencyGraph asset into its live
// curve (and re-encode the node script). Detaches (assetId -> -1, keeping the
// last cached curve) when the asset is gone. Returns the number of curves
// refreshed from a live asset. Call after readProject() and after edits.
int resolveCurveEqReferences(NodeGraph& graph);

} // namespace SoundShop

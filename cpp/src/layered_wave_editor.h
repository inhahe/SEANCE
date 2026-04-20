#pragma once
#include "node_graph.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <functional>
#include <string>

namespace SoundShop {

// A single layer in a layered waveform: a basic shape at a harmonic ratio,
// with phase offset and amplitude. Layers are summed into one single-cycle
// wavetable at edit time (bake-once, not per-note).
struct WaveLayer {
    enum Shape { Sine, Saw, Square, Triangle, Noise, Drawn };
    Shape shape = Sine;
    int   ratio = 1;      // harmonic number: 1 = fundamental, 2 = octave, ...
    float phase = 0.0f;   // 0..1 (one full cycle)
    float amp   = 1.0f;   // 0..1

    // For Drawn shape: free control points over one cycle.
    // Each point is (phase 0..1, amplitude -1..1). Waveform is evaluated
    // by Catmull-Rom interpolation through the points, wrapped periodically.
    // Empty list = flat zero; a fresh Drawn layer is seeded with a few
    // points so the user has something to grab.
    std::vector<std::pair<float, float>> drawnPoints;
};

struct LayeredWaveform {
    std::vector<WaveLayer> layers;
    int tableSize = 2048;

    // Sum layers into `out` (resized to tableSize). Normalized to peak 1.0.
    void render(std::vector<float>& out) const;

    // Encode as a string stored in node.script, prefixed with "__layered__:".
    std::string encode() const;

    // Decode from a string (with or without the prefix). Returns true on success.
    bool decode(const std::string& s);

    // Create a default 1-layer sine.
    static LayeredWaveform defaultSine();
};

// A wavetable can be authored in two modes:
//
//  Grid: an N-dimensional rectilinear grid of LayeredWaveform frames played
//    via an (N+1)-dimensional terrain {tableSize, dim0, dim1, ...}. One
//    Position parameter per dimension morphs through the grid via N-linear
//    interpolation. Fast, easy to author for axis-aligned morph charts.
//
//  Scatter: an arbitrary set of frames placed at points in N-dimensional
//    space (each frame stores its own coordinate). The Position knobs give
//    a query point and the rendered waveform is a weighted blend of nearby
//    frames using a Wendland radial basis function. Lets you arrange e.g.
//    three sounds in a triangle and morph freely between them, which a
//    grid topology cannot express.
struct ScatterFrame {
    LayeredWaveform wave;
    std::vector<float> position;   // length = WavetableDoc::scatterDims, each in [0,1]
    std::string label;              // optional short user label shown in viewport
    int colorIdx = -1;              // -1 = auto from spectral centroid
};

enum class WavetableMode { Grid, Scatter };

struct WavetableDoc {
    WavetableMode mode = WavetableMode::Grid;
    int tableSize = 2048;

    // ---- Grid mode ----
    std::vector<LayeredWaveform> frames;  // flat storage, indexed row-major
    std::vector<int> gridDims;             // size per dimension (e.g., {4} for 1D, {3,4} for 2D)

    // ---- Scatter mode ----
    int scatterDims = 2;                   // number of N-D coord axes
    float scatterRadius = 0.45f;           // RBF cutoff (in normalized [0,1] units)
    std::vector<ScatterFrame> scatterFrames;

    // Total frame count = product of gridDims
    int totalFrames() const {
        int n = 1;
        for (int d : gridDims) n *= std::max(1, d);
        return n;
    }

    // Number of position dimensions exposed to the synth as Position params.
    // Grid: number of grid axes. Scatter: scatterDims.
    int numDimensions() const {
        return mode == WavetableMode::Grid ? (int)gridDims.size() : scatterDims;
    }

    // Number of editable frames in the active mode.
    int activeFrameCount() const {
        return mode == WavetableMode::Grid ? (int)frames.size()
                                            : (int)scatterFrames.size();
    }

    // Pointer to the layers of the i-th frame (mode-aware), or nullptr if
    // out of range. Used by LayerRow so it doesn't need to know which mode
    // is active.
    LayeredWaveform* frameAt(int idx);
    const LayeredWaveform* frameAt(int idx) const;

    // Convert N-dimensional index to flat index (row-major)
    int gridToFlat(const std::vector<int>& idx) const {
        int flat = 0, stride = 1;
        for (int d = (int)gridDims.size() - 1; d >= 0; --d) {
            flat += idx[d] * stride;
            stride *= gridDims[d];
        }
        return flat;
    }

    // Encode/decode
    std::string encode() const;
    bool decode(const std::string& s);

    static WavetableDoc defaultSingleSine();
};

// Editor window contents (paired with a juce::DialogWindow launched by the caller).
// Edits `node.script` directly. onApply() should request a graph rebuild so
// the new waveform takes effect; it is called on a debounce timer (not on
// every slider tick) to avoid racing JUCE's async graph rebuild.
//
// The editor holds a WavetableDoc (one or more frames). Only one frame is
// editable at a time — the current frame — selected via frame-tab buttons.
class LayeredWaveEditorComponent : public juce::Component, private juce::Timer {
public:
    LayeredWaveEditorComponent(NodeGraph& graph, int nodeId, std::function<void()> onApply);
    ~LayeredWaveEditorComponent() override;

    void resized() override;
    void paint(juce::Graphics& g) override;
    void timerCallback() override;

    // Layer access goes through these so LayerRow doesn't have to know about
    // the current frame selection.
    std::vector<WaveLayer>& currentLayers();
    const std::vector<WaveLayer>& currentLayers() const;

private:
    class LayerRow; // inner component for a single layer's controls
    class ScatterView; // inner component for the N-D scatter viewport
    friend class LayerRow;
    friend class ScatterView;

    NodeGraph& graph;
    int nodeId;
    std::function<void()> onApply;

    WavetableDoc wave;
    int currentFrameIdx = 0;            // Grid mode: row-major frame index. Scatter mode: scatter index.
    std::vector<float> currentPosition; // Scatter mode: current Position cursor in [0,1]^scatterDims
    std::vector<float> previewSamples;

    juce::TextButton addLayerBtn { "+ Add Layer" };
    juce::TextButton addFrameBtn { "+ Frame" };
    juce::TextButton modeToggleBtn{ "Mode: Grid" };
    juce::TextButton addDimBtn   { "+ Dim" };
    juce::TextButton removeDimBtn{ "- Dim" };
    juce::TextButton anaglyph3DBtn { "3D" };
    juce::ComboBox   projectionCombo;
    juce::TextButton applyBtn    { "Apply" };
    juce::TextButton closeBtn    { "Close" };
    juce::TextButton helpBtn     { "?" };
    juce::Viewport   layersViewport;
    juce::Component  layersContainer;
    std::vector<std::unique_ptr<LayerRow>> rows;

    // Frame tab buttons — one per frame in Grid mode. Each has an X to delete.
    struct FrameTab {
        std::unique_ptr<juce::TextButton> selectBtn;
        std::unique_ptr<juce::TextButton> deleteBtn;
    };
    std::vector<FrameTab> frameTabs;
    juce::Component frameTabsRow;

    // Scatter mode UI
    std::unique_ptr<ScatterView> scatterView;
    // One slider per non-projected scatter axis, so the user can move the
    // Position cursor along axes the current 2D/3D projection isn't showing.
    std::vector<std::unique_ptr<juce::Slider>> nonProjAxisSliders;
    std::vector<std::unique_ptr<juce::Label>>  nonProjAxisLabels;
    juce::Component nonProjAxisRow;

    // Numeric coordinate entry for the selected scatter frame.
    // One slider per scatter dimension; rebuilt whenever scatterDims changes.
    std::vector<std::unique_ptr<juce::Slider>> coordSliders;
    std::vector<std::unique_ptr<juce::Label>>  coordLabels;
    juce::Component coordRow;
    juce::Label     coordRowTitle;

    // Stereo (3D anaglyph) settings — only visible when the 3D toggle is on.
    // Match real-world geometry so the parallax math comes out correct for
    // the user's actual eyes/screen.
    juce::Slider ipdSlider;       // interpupillary distance, mm (50..75)
    juce::Slider distSlider;      // viewing distance, cm        (30..120)
    juce::Slider depthSlider;     // depth of the scene cube, mm (10..200)
    juce::Label  ipdLabel, distLabel, depthLabel, dpiLabel;
    juce::Component stereoRow;

    void rebuildRows();
    void rebuildFrameTabs();
    void rebuildScatterUI();        // builds projection combo entries + non-proj sliders
    void rebuildCoordRow();         // rebuild coord-entry sliders to match scatterDims
    void syncCoordRowFromSelection();
    void refreshPreview();
    void commitToNode(); // encode `wave` into node.script
    void onLayerChanged();
    void switchToFrame(int idx);
    void toggleMode();              // Grid <-> Scatter
    void syncPositionParams();      // ensure node has the right number of Position params
    void onProjectionChanged();
    void pushStereoSettingsToView();
};

} // namespace SoundShop

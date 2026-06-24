#pragma once
#include "node_graph.h"
#include "wavelet_paint.h"
#include "wavelet_frame.h"
#include "warp_editor.h"     // WarpChainEditor (per-coefficient element warp)
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <string>
#include <functional>

namespace SoundShop {

// ==============================================================================
// Wavelet Space Painter (#65): draw directly on a scales x time grid
// of DWT coefficients, hear the result in real time via IDWT.
//
// The canvas shows wavelet decomposition levels as rows (coarse at top,
// fine at bottom) and time positions as columns. The user paints
// coefficient values with the mouse (click/drag), and the node's
// waveform is updated in real time by inverse-DWT'ing the edited
// coefficients back to the time domain.
//
// Intended as a creative sound-design tool: instead of drawing a
// waveform directly, you draw in the time-frequency plane, getting
// intuitive control over which frequencies exist at which moments.
//
// Controls:
//   Left-click / drag  - paint at Amp slider value
//   Shift + left-click - paint at -Amp (flip sign)
//   Right-click / drag - erase (set to zero)
//   Amp slider         - paint amplitude in [-1, +1]
//   Filter combo       - db1 / db2 / db4 / sym4 wavelet family
//   Levels combo       - number of decomposition levels (rows in the grid)
//   Clear button       - zero all coefficients
//
// The encoded script (stored in node.script) is decoded by
// terrain_synth.cpp to produce the synth's one-cycle waveform.
// ==============================================================================
class WaveletPainterComponent : public juce::Component, private juce::Timer {
public:
    // Node-backed mode: read/write node.script directly. Used when the
    // painter opens on a Wavelet Space node.
    WaveletPainterComponent(NodeGraph& graph, int nodeId,
                            std::function<void()> onApply);

    // Frame-backed mode: read/write an external WaveletFrame's coefficient
    // grid, no NodeGraph involved. Used when the painter is launched as a
    // sub-dialog from the wavetable shell to edit a single WaveletFrame
    // inside a mixed-type wavetable. onApply is invoked after every commit
    // so the owning wavetable editor can re-render its preview.
    WaveletPainterComponent(WaveletFrame& externalFrame,
                            std::function<void()> onApply);

    ~WaveletPainterComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp  (const juce::MouseEvent& e) override;

    void timerCallback() override;

private:
    // Either (graph, nodeId) or externalFrame is active, never both.
    NodeGraph* graph = nullptr;
    int nodeId = 0;
    WaveletFrame* externalFrame = nullptr;
    std::function<void()> onApply;

    void initUI();

    // Coefficient grid in DWT-packed layout: [approx | D1 | D2 | ... | DnumLevels]
    int numLevels = kWaveletPaintDefaultLevels;
    int totalSize = kWaveletPaintDefaultSize;
    std::string filterName = kWaveletPaintDefaultFilter;
    std::vector<float> coefficients;
    std::vector<float> waveform;

    // Toolbar
    juce::Slider     ampSlider;
    juce::Label      ampLabel;
    juce::ComboBox   filterCombo;
    juce::Label      filterLabel;
    juce::ComboBox   levelsCombo;
    juce::Label      levelsLabel;
    juce::TextButton clearBtn { "Clear" };
    juce::TextButton helpBtn  { "?" };
    // Opens the shared AHDSR amplitude-envelope editor for this node in a
    // separate dialog. Node-backed mode only (graph != nullptr); in frame-backed
    // sub-editor mode the wavetable shell owns the envelope button instead.
    juce::TextButton envelopeBtn { "Envelope..." };
    // Sustained held audition (the generic editor "Preview" button - same role
    // as the wavetable shell's playBtn). Node-backed mode only.
    juce::TextButton playBtn { "Preview" };
    bool framePlaying = false;

    // Bucket C element warp (frame-backed mode only). Edits the owning
    // WaveletFrame's warpChain, baked into the coefficient grid before the IDWT
    // (matching WaveletFrame::renderRaw) so the preview reflects the audio. Null
    // in node-backed mode, where the standalone wavelet node stores only a
    // __waveletpaint__ script with no warp section. Both warp domains apply:
    // phase-domain ops stretch/shift along the coefficient index, amplitude-
    // domain ops reshape the coefficient magnitudes.
    std::unique_ptr<WarpChainEditor> warpEditor;

    // Layout caches updated in resized()
    juce::Rectangle<int> toolbarBounds;
    juce::Rectangle<int> gridBounds;
    juce::Rectangle<int> waveBounds;

    void updateWaveform();
    void commitToNode();
    void paintCoeff(const juce::MouseEvent& e);
    void seedDefaultCoefficients();

    // Held-audition (Preview) control. togglePreview starts/stops; ship re-sends
    // the freshly-rendered cycle whenever an edit changes it (no-op unless a
    // Preview is active). Both no-op in frame-backed mode (graph == nullptr).
    void togglePreview();
    void refreshPreviewAudition();
};

} // namespace SoundShop

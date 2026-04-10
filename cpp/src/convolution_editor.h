#pragma once
#include "node_graph.h"
#include "convolution_processor.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace SoundShop {

// Editor for the convolution filter's impulse response. Supports:
// - Drawing the IR via control points (Catmull-Rom) or freehand
// - Parameterized presets (lowpass, highpass, bandpass, echo)
// - Loading IR from a .wav file
// - Live frequency response preview

class ConvolutionEditorComponent : public juce::Component {
public:
    ConvolutionEditorComponent(NodeGraph& graph, int nodeId, std::function<void()> onApply);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e,
                        const juce::MouseWheelDetails& w) override;

private:
    // Drawing modes
    enum class DrawMode { ControlPoints, Freehand };
    DrawMode drawMode = DrawMode::ControlPoints;
    juce::TextButton modePointsBtn { "Points" };
    juce::TextButton modeFreehandBtn { "Freehand" };

    // Horizontal zoom / scroll into the IR. zoomX = 1 shows the whole IR,
    // zoomX > 1 shows a subrange. scrollFrac in [0..1] positions the left
    // edge of the visible window within the IR.
    float zoomX = 1.0f;
    float scrollFrac = 0.0f;

    // Freehand drag state
    int lastDrawSample = -1;
    float lastDrawValue = 0.0f;

    // Convert a screen x inside irArea back to a (possibly fractional) sample
    // index, accounting for current zoom/scroll.
    float screenXToSampleIdx(float x, const juce::Rectangle<float>& irArea) const;
    // Inverse: sample index -> screen x inside irArea.
    float sampleIdxToScreenX(float idx, const juce::Rectangle<float>& irArea) const;

    NodeGraph& graph;
    int nodeId;
    std::function<void()> onApply;

    std::vector<float> ir;           // current IR being edited
    std::vector<float> freqResponse; // magnitude spectrum for preview
    double sampleRate = 48000;

    // Preset controls
    juce::ComboBox presetCombo;
    juce::Slider cutoffSlider, orderSlider, bandwidthSlider;
    juce::Slider delaySlider, feedbackSlider, echoCountSlider;
    juce::Label cutoffLbl, orderLbl, bwLbl, delayLbl, fbLbl, echoLbl;
    juce::TextButton applyPresetBtn{"Generate"};
    juce::TextButton loadFileBtn{"Load IR File..."};
    juce::TextButton applyBtn{"Apply"};
    juce::TextButton closeBtn{"Close"};

    // IR length
    juce::Slider lengthSlider;
    juce::Label lengthLbl;

    // Control points for drawing
    std::vector<std::pair<float, float>> controlPoints; // (x 0..1, y -1..1)
    int dragPointIdx = -1;

    void commitIR();
    void updateFreqResponse();
    void generateFromPreset();
    void renderFromControlPoints();
    void loadFromFile();

    juce::Rectangle<float> getIRArea() const;
    juce::Rectangle<float> getFreqArea() const;
};

} // namespace SoundShop

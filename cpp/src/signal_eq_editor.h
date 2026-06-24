#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "node_graph.h"

namespace SoundShop {

// =============================================================================
// SignalEQEditorComponent - editor for a Signal EQ node ("__signaleq__").
//
// The node's frequency response is a set of "curve points", each a peaking bell
// whose centre frequency (P<n> Freq) and gain (P<n> Gain) are individual node
// params - and therefore individually signal-modulatable via the on-demand
// Param-pin mechanism (#88). This editor is sugar over those params:
//
//   * A log-frequency / dB canvas draws the combined response and one draggable
//     handle per point. Dragging a handle writes that point's Freq + Gain params
//     (committed to the undo tree on mouse-up).
//   * Double-click empty canvas adds a point there; right-click a handle to
//     delete it or to add a Freq/Gain control input (a "Mod:" Param pin so a
//     signal can drive that coordinate).
//   * Mode (Zero-latency biquad vs FFT exact), Width (shared bell Q) and Mix are
//     edited with the controls along the bottom; they map to the node's global
//     params.
//
// All state lives in node params, so save/load, undo and automation come for
// free. onApply() is called (debounced) whenever something changes so the audio
// graph rebuilds to pick up structural edits (added/removed points or pins).
// =============================================================================
class SignalEQEditorComponent : public juce::Component, private juce::Timer {
public:
    SignalEQEditorComponent(NodeGraph& graph, int nodeId,
                            std::function<void()> onApply);
    ~SignalEQEditorComponent() override;

    void resized() override;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void timerCallback() override;

private:
    NodeGraph& graph;
    int nodeId;
    std::function<void()> onApply;

    juce::Label      titleLabel;
    juce::ComboBox   modeBox;
    juce::Slider     widthSlider;
    juce::Label      widthLabel;
    juce::Slider     mixSlider;
    juce::Label      mixLabel;
    juce::TextButton addPointBtn  { "Add Point" };
    juce::TextButton closeBtn     { "Close" };
    juce::Label      hintLabel;

    juce::Rectangle<int> canvasArea;   // the response-plot rectangle

    int  draggingPoint = -1;           // 1-based point index being dragged, or -1
    bool dragMoved = false;            // did the drag actually change anything
    bool needsCommit = false;          // pending snapshot on mouse-up
    bool structuralPending = false;    // a rebuild-worthy change happened

    // --- frequency / gain <-> pixel mapping (log freq, linear dB) ---
    static constexpr float kMinHz = 20.0f, kMaxHz = 20000.0f;
    static constexpr float kMinDb = -24.0f, kMaxDb = 24.0f;
    float freqToX(float hz) const;
    float xToFreq(float x) const;
    float gainToY(float db) const;
    float yToGain(float y) const;

    // --- node/param helpers ---
    Node* node();
    int   pointCount();
    bool  getPoint(int idx1, float& freqOut, float& gainOut); // idx1 1-based
    int   freqParamIndex(int idx1);  // param index of "P<idx1> Freq", or -1
    int   gainParamIndex(int idx1);
    void  setPointLive(int idx1, float freq, float gain);     // no snapshot
    int   pointNearby(juce::Point<float> p);                  // nearest handle, or -1
    float responseDbAt(float hz);                             // combined curve, dB

    void  addPointAt(float hz, float db);
    void  deletePoint(int idx1);
    void  addControlInput(int paramIndex);
    void  commitNow(const juce::String& desc);
    void  syncControlsFromNode();
    void  scheduleApply();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SignalEQEditorComponent)
};

} // namespace SoundShop

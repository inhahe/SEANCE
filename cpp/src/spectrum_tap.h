#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <cmath>

namespace SoundShop {

// A frequency bin: bandpass filter + envelope follower extracting amplitude
// at a chosen frequency range. Updated per sample, output is a smooth 0..1
// amplitude envelope usable as a control signal.
struct FrequencyBin {
    float centerFreq = 1000.0f;  // Hz
    float bandwidth  = 200.0f;   // Hz (width of the band)
    uint32_t color   = 0xFFFF8800;

    // Biquad bandpass state (2nd-order IIR)
    float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0; // coefficients
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;           // state

    // Envelope follower state (one-pole lowpass on rectified signal)
    float envelope = 0.0f;
    float smoothCoeff = 0.0f; // computed from sample rate

    // Recompute coefficients for the current freq/bandwidth/sampleRate
    void computeCoeffs(double sampleRate) {
        double Q = std::max(0.1, (double)centerFreq / std::max(1.0, (double)bandwidth));
        double w0 = 2.0 * 3.14159265358979 * centerFreq / sampleRate;
        double sinW0 = std::sin(w0);
        double cosW0 = std::cos(w0);
        double alpha = sinW0 / (2.0 * Q);

        double a0 = 1.0 + alpha;
        b0 = (float)(alpha / a0);
        b1 = 0.0f;
        b2 = (float)(-alpha / a0);
        a1 = (float)(-2.0 * cosW0 / a0);
        a2 = (float)((1.0 - alpha) / a0);

        // Envelope smoothing: ~10ms attack, ~50ms release
        double smoothTime = 0.02; // 20ms average
        smoothCoeff = (float)(1.0 - std::exp(-1.0 / (smoothTime * sampleRate)));
    }

    // Process one sample through the bandpass + envelope follower.
    // Returns the current envelope amplitude (0..~1).
    float process(float input) {
        // Biquad bandpass
        float filtered = b0 * input + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = input;
        y2 = y1; y1 = filtered;

        // Rectify + smooth
        float rectified = std::abs(filtered);
        envelope += smoothCoeff * (rectified - envelope);
        return envelope;
    }

    void reset() {
        x1 = x2 = y1 = y2 = 0;
        envelope = 0;
    }
};

// Audio processor inserted inline on a wire. Passes audio through unchanged
// while running frequency bins on it. Each bin's amplitude is available as
// a named param on the node (readable by downstream signal connections).
class SpectrumTapProcessor : public juce::AudioProcessor {
public:
    SpectrumTapProcessor(Node& node);

    const juce::String getName() const override { return "SpectrumTap"; }
    void prepareToPlay(double sr, int bs) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    Node& node;
    double sampleRate = 44100;
    std::vector<FrequencyBin> bins;

    void rebuildBins();
};

// UI for the spectrum tap: shows bins on a frequency axis, lets user
// add/remove/resize them, displays live amplitude levels.
class SpectrumTapComponent : public juce::Component, private juce::Timer {
public:
    SpectrumTapComponent(NodeGraph& graph, int nodeId);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void timerCallback() override { repaint(); }

private:
    NodeGraph& graph;
    int nodeId;
    int dragBinIdx = -1;
    enum DragWhat { DragCenter, DragLeft, DragRight };
    DragWhat dragWhat = DragCenter;

    juce::TextButton addBinBtn{"+ Add Bin"};

    // Convert between Hz and screen x (log scale)
    float hzToX(float hz) const;
    float xToHz(float x) const;
    juce::Rectangle<float> getSpectrumArea() const;
};

} // namespace SoundShop

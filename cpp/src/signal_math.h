#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>

namespace SoundShop {

// =============================================================================
// SignalMathProcessor  -  per-sample binary arithmetic on two control signals
// =============================================================================
//
// The most foundational module in the per-voice modular kit (M3). It reads two
// control SIGNALS and combines them sample-by-sample with a selectable
// operation, writing the result to a single Signal output:
//
//   Signal in 0: "A"   (channel 2)
//   Signal in 1: "B"   (channel 3)
//   Signal out 0: "Out" (channel 2)
//
// Operation (node param "Operation", 0..5):
//   0 Add        Out = A + B
//   1 Subtract   Out = A - B
//   2 Multiply   Out = A * B      (VCA / ring-mod / scaling)
//   3 Divide     Out = A / B      (B==0 -> 0, no NaN/inf)
//   4 Min        Out = min(A, B)
//   5 Max        Out = max(A, B)
//
// Inputs are pure signals: an unwired input reads as 0 (the honest modular
// convention). So Subtract with A unwired negates B (Out = -B); Multiply with
// either input unwired is silent. A constant operand is provided by wiring a
// dedicated source, not baked in here, to keep the op semantics clean.
//
// Like every control node, signals live on buffer channels 2+ (channels 0/1 are
// the always-present stereo audio bus, unused here). Stateless and per-sample,
// so it works identically standalone or cloned inside a Voice container.
class SignalMathProcessor : public juce::AudioProcessor {
public:
    explicit SignalMathProcessor(Node& n) : node(n) {}

    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        const int n  = buf.getNumSamples();
        const int ch = buf.getNumChannels();
        const int op = (int) std::lround(paramValue("Operation", 0.0f));

        // Need at least the two input channels (2,3) and the output channel (2)
        // to do anything. If the layout is too narrow just emit silence.
        if (ch <= 2) { for (int c = 0; c < ch; ++c) buf.clear(c, 0, n); return; }

        const float* a = buf.getReadPointer(2);
        const float* b = (ch > 3) ? buf.getReadPointer(3) : nullptr;

        // Compute into a scratch first because the output channel (2) aliases
        // input A's channel - we must not clobber A while B reads from a later
        // channel (they're independent here, but writing-through keeps it safe
        // and obviously correct).
        scratch.resize((size_t) n);
        for (int i = 0; i < n; ++i) {
            const float av = a[i];
            const float bv = b ? b[i] : 0.0f;
            float out;
            switch (op) {
                case 1:  out = av - bv; break;
                case 2:  out = av * bv; break;
                case 3:  out = (bv != 0.0f) ? av / bv : 0.0f; break;
                case 4:  out = juce::jmin(av, bv); break;
                case 5:  out = juce::jmax(av, bv); break;
                default: out = av + bv; break; // 0 = Add
            }
            scratch[(size_t) i] = std::isfinite(out) ? out : 0.0f;
        }

        // Write the result to the single Signal output (channel 2) and silence
        // the audio bus (0/1) plus any leftover control channel so nothing
        // leaks the raw inputs downstream.
        buf.copyFrom(2, 0, scratch.data(), n);
        buf.clear(0, 0, n);
        if (ch > 1) buf.clear(1, 0, n);
        for (int c = 3; c < ch; ++c) buf.clear(c, 0, n);
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
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
    float paramValue(const char* name, float def) const {
        for (auto& p : node.params)
            if (p.name == name) return p.value;
        return def;
    }

    Node& node;
    std::vector<float> scratch;
};

} // namespace SoundShop

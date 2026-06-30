#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>

namespace SoundShop {

// =============================================================================
// SignalLogicProcessor  -  comparison + boolean logic on control signals
// =============================================================================
//
// Turns two control SIGNALS into a clean 0/1 GATE every sample - the module for
// thresholding a signal or combining gates. A boolean input is read as "true
// when >= 0.5".
//
//   Signal in 0:  "A"   (channel 2)
//   Signal in 1:  "B"   (channel 3)   (threshold / second operand; B unwired = 0)
//   Signal out 0: "Out" (channel 2)   1.0 or 0.0
//
// Param "Operation" (enum 0..5):
//   0 A > B       comparator: gate while A rises above B (threshold detect)
//   1 A < B       comparator: gate while A is below B
//   2 A AND B     both true (>= 0.5)
//   3 A OR  B     either true
//   4 A XOR B     exactly one true
//   5 NOT A       inverts A (ignores B)
//
// Output is always a hard 0/1, so it composes cleanly with anything that wants a
// gate (a Voice gate, a Sample & Hold Trigger, an envelope's gate). Stateless.
class SignalLogicProcessor : public juce::AudioProcessor {
public:
    explicit SignalLogicProcessor(Node& n) : node(n) {}

    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        const int n  = buf.getNumSamples();
        const int ch = buf.getNumChannels();
        if (ch <= 2) { for (int c = 0; c < ch; ++c) buf.clear(c, 0, n); return; }

        const int op = (int) std::lround(paramValue("Operation", 0.0f));
        const float* a = buf.getReadPointer(2);
        const float* b = (ch > 3) ? buf.getReadPointer(3) : nullptr;

        scratch.resize((size_t) n);
        for (int i = 0; i < n; ++i) {
            const float av = a[i];
            const float bv = b ? b[i] : 0.0f;
            const bool ab = av >= 0.5f;
            const bool bb = bv >= 0.5f;
            bool out;
            switch (op) {
                case 1:  out = av < bv;        break; // A < B
                case 2:  out = ab && bb;       break; // AND
                case 3:  out = ab || bb;       break; // OR
                case 4:  out = ab != bb;       break; // XOR
                case 5:  out = !ab;            break; // NOT A
                default: out = av > bv;        break; // A > B
            }
            scratch[(size_t) i] = out ? 1.0f : 0.0f;
        }

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

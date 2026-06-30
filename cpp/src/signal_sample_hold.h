#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>

namespace SoundShop {

// =============================================================================
// SampleHoldProcessor  -  sample & hold (modular kit)
// =============================================================================
//
// On the rising edge of a Trigger, latches a value and holds it steady until the
// next trigger - the classic source of stepped / random-per-note modulation.
//
//   Signal in 0:  "In"      (channel 2) - the value sampled in "Input" mode.
//   Signal in 1:  "Trigger" (channel 3) - a rising edge (>= 0.5) takes a sample.
//                 Wire a Voice container's VoiceIn Gate here for one fresh value
//                 per note.
//   Signal out 0: "Out"     (channel 2) - the most recently held value.
//
// Param "Source" (enum 0..2):
//   0 Input         hold the "In" signal sampled at the trigger.
//   1 Random ±1     hold a fresh random value in [-1, +1] (ignores In).
//   2 Random 0..1   hold a fresh random value in [0, 1]   (ignores In).
//
// The internal random generator means "random per note" works with nothing wired
// to In - just feed the gate to Trigger. Each voice clone owns its own instance
// (and its own RNG state), so per-voice randomness varies between voices.
class SampleHoldProcessor : public juce::AudioProcessor {
public:
    explicit SampleHoldProcessor(Node& n) : node(n) {}

    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double, int) override {
        held = 0.0f;
        prevTrigHigh = false;
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        const int n  = buf.getNumSamples();
        const int ch = buf.getNumChannels();
        if (ch <= 2) { for (int c = 0; c < ch; ++c) buf.clear(c, 0, n); return; }

        const int source = (int) std::lround(paramValue("Source", 0.0f));
        const float* in   = buf.getReadPointer(2);
        const float* trig = (ch > 3) ? buf.getReadPointer(3) : nullptr;

        scratch.resize((size_t) n);
        for (int i = 0; i < n; ++i) {
            if (trig) {
                const bool high = trig[i] >= 0.5f;
                if (high && !prevTrigHigh) {
                    switch (source) {
                        case 1:  held = rng.nextFloat() * 2.0f - 1.0f; break; // -1..1
                        case 2:  held = rng.nextFloat();               break; // 0..1
                        default: held = in[i];                         break; // Input
                    }
                }
                prevTrigHigh = high;
            }
            scratch[(size_t) i] = held;
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
    float held = 0.0f;
    bool  prevTrigHigh = false;
    juce::Random rng;
    std::vector<float> scratch;
};

} // namespace SoundShop

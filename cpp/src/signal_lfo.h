#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>

namespace SoundShop {

// =============================================================================
// SignalLFOProcessor  -  a control-rate low-frequency oscillator (modular kit)
// =============================================================================
//
// Generates a periodic control SIGNAL for modulation - the classic LFO. Per
// sample it advances a phase and emits the selected waveform:
//
//   Signal in 0:  "Sync" (channel 2) - optional. A rising edge (>= 0.5) resets
//                 the phase to 0, so wiring a Voice container's VoiceIn Gate here
//                 retriggers the LFO at the start of every note. Unwired (reads
//                 as 0) -> the LFO free-runs.
//   Signal out 0: "Out"  (channel 2) - the waveform.
//
// Params:
//   "Rate"     (Hz, 0.1..20)   cycles per second.
//   "Shape"    (enum 0..3)     0 sine, 1 triangle, 2 saw, 3 square.
//   "Polarity" (enum 0..1)     0 bipolar (-1..1), 1 unipolar (0..1).
//
// Stateful (phase persists across blocks). Each voice clone inside a container
// owns its own instance, so per-voice LFOs are independent and can sync to that
// voice's own gate. The audio bus (channels 0/1) is always silent.
class SignalLFOProcessor : public juce::AudioProcessor {
public:
    explicit SignalLFOProcessor(Node& n) : node(n) {}

    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double sr, int) override {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        phase = 0.0;
        prevSyncHigh = false;
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        const int n  = buf.getNumSamples();
        const int ch = buf.getNumChannels();
        if (ch <= 2) { for (int c = 0; c < ch; ++c) buf.clear(c, 0, n); return; }

        const float rateHz   = juce::jmax(0.0f, paramValue("Rate", 2.0f));
        const int   shape     = (int) std::lround(paramValue("Shape", 0.0f));
        const bool  unipolar  = paramValue("Polarity", 0.0f) >= 0.5f;
        const double inc = (double) rateHz / sampleRate;

        const float* sync = (ch > 2) ? buf.getReadPointer(2) : nullptr;

        scratch.resize((size_t) n);
        for (int i = 0; i < n; ++i) {
            // Sync edge: reset phase on the rising edge of the Sync input.
            if (sync) {
                const bool high = sync[i] >= 0.5f;
                if (high && !prevSyncHigh) phase = 0.0;
                prevSyncHigh = high;
            }

            float bip = shapeSample(shape, (float) phase); // -1..1
            scratch[(size_t) i] = unipolar ? 0.5f * (bip + 1.0f) : bip;

            phase += inc;
            if (phase >= 1.0) phase -= std::floor(phase);
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

    static float shapeSample(int shape, float ph) {
        switch (shape) {
            case 1:  return 4.0f * std::abs(ph - 0.5f) - 1.0f;   // triangle
            case 2:  return 2.0f * ph - 1.0f;                    // saw (rising)
            case 3:  return ph < 0.5f ? 1.0f : -1.0f;            // square
            default: return std::sin(ph * juce::MathConstants<float>::twoPi); // sine
        }
    }

    Node& node;
    double sampleRate = 44100.0;
    double phase = 0.0;
    bool   prevSyncHigh = false;
    std::vector<float> scratch;
};

} // namespace SoundShop

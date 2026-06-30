#pragma once
#include "node_graph.h"
#include "signal_modulation.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>

namespace SoundShop {

// ==============================================================================
// SIGNAL FILTER - resonant multi-mode filter (modular kit, M3)
// Script: "__signalfilter__"
//
// A hand-rolled TPT (topology-preserving transform) state-variable filter
// (Andrew Simper / Cytomic form). One audio in, one audio out, blue Effect node.
// Params:
//   Type       0 = Low-pass, 1 = High-pass, 2 = Band-pass   (enum picker)
//   Cutoff     20..20000 Hz   (modulatable via #88 - right-click -> Add Mod input)
//   Resonance  0..1            (maps to Q 0.5..10; modulatable via #88)
//
// Cutoff/Resonance are ordinary node params, so the standard on-demand
// modulation-pin mechanism (#88, applySignalModulations) drives them with NO
// hardcoded control pin: a cable wired to a "Mod: Cutoff" pin swings the cutoff
// per block. This deliberately mirrors Signal EQ instead of adding a bespoke
// cutoff signal pin (CLAUDE.md #88 rule).
//
// The coefficients are recomputed once per block from the (possibly modulated)
// param values - cheap (one tan) and gives clean block-rate cutoff sweeps.
// State (ic1eq/ic2eq) is per audio channel and persists across blocks; a
// per-voice clone in a VoiceContainer therefore has its own independent filter
// state, exactly like the other stateful modules.
// ==============================================================================
class SignalFilterProcessor : public juce::AudioProcessor {
public:
    SignalFilterProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Signal Filter"; }

    void prepareToPlay(double sr, int) override {
        sampleRate = sr > 0 ? sr : 44100.0;
        reset();
    }
    void releaseResources() override {}

    void reset() {
        for (int c = 0; c < 2; ++c) { ic1eq[c] = 0.0; ic2eq[c] = 0.0; }
    }

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);

        float cutoff = paramByName(node, "Cutoff", 1000.0f);
        float res    = paramByName(node, "Resonance", 0.2f);
        int   type   = (int)std::round(paramByName(node, "Type", 0.0f));

        // Keep cutoff inside a stable range (below Nyquist - tan() blows up at
        // exactly Nyquist, and sub-20 Hz is below the audible band anyway).
        double nyqLimit = 0.45 * sampleRate;
        cutoff = (float)std::clamp((double)cutoff, 20.0, nyqLimit);
        res    = std::clamp(res, 0.0f, 1.0f);

        // Resonance 0..1 -> Q 0.5..10 (0.5 = gently damped, 10 = sharp peak).
        double Q = 0.5 + (double)res * 9.5;
        double g = std::tan(juce::MathConstants<double>::pi * cutoff / sampleRate);
        double k = 1.0 / Q;
        double a1 = 1.0 / (1.0 + g * (g + k));
        double a2 = g * a1;
        double a3 = g * a2;

        // Only the main stereo audio bus (channels 0/1) is filtered. Control
        // channels (2+, the #88 modulation inputs) are read block-rate by
        // applySignalModulations and must pass through untouched.
        const int nCh = juce::jmin(2, buf.getNumChannels());
        const int n   = buf.getNumSamples();
        for (int c = 0; c < nCh; ++c) {
            float* d = buf.getWritePointer(c);
            double s1 = ic1eq[c];
            double s2 = ic2eq[c];
            for (int i = 0; i < n; ++i) {
                double v0 = (double)d[i];
                double v3 = v0 - s2;
                double v1 = a1 * s1 + a2 * v3;
                double v2 = s2 + a2 * s1 + a3 * v3;
                s1 = 2.0 * v1 - s1;
                s2 = 2.0 * v2 - s2;
                double low  = v2;
                double band = v1;
                double high = v0 - k * v1 - v2;
                double out = (type == 1) ? high : (type == 2) ? band : low;
                if (!std::isfinite(out)) out = 0.0;
                d[i] = (float)out;
            }
            // Flush denormals/NaNs in the state to keep the filter stable.
            if (!std::isfinite(s1)) s1 = 0.0;
            if (!std::isfinite(s2)) s2 = 0.0;
            ic1eq[c] = s1;
            ic2eq[c] = s2;
        }
    }

    // Boilerplate
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
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
    double sampleRate = 44100.0;
    double ic1eq[2] = { 0.0, 0.0 };
    double ic2eq[2] = { 0.0, 0.0 };
};

} // namespace SoundShop

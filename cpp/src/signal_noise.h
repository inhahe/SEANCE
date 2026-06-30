#pragma once
#include "node_graph.h"
#include "adsr_envelope.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

namespace SoundShop {

// =============================================================================
// SignalNoiseProcessor  -  a gated, enveloped noise generator
// =============================================================================
//
// The noise counterpart to the Signal Oscillator: a Signal-driven monophonic
// source for the modular voice kit. It has no pitch (noise is broadband) - it
// reads two control signals and turns them into an enveloped burst of noise:
//
//   Signal in 0: "Gate"     (0/1)  - note on while >= 0.5, release on the
//                                    falling edge; drives the AHDSR envelope.
//   Signal in 1: "Velocity" (0..1) - latched on the gate's rising edge and fed
//                                    to the envelope's velocity scaling.
//
// Wired from a VoiceIn puck's Gate/Velocity outputs inside a Voice container,
// one instance lives in each voice clone (the container provides polyphony), so
// it makes snares / hats / wind / breath / percussion voices. Run the output
// through a Signal Filter for tuned-noise and resonant-sweep timbres. Outside a
// container it still works as a standalone gated noise source.
//
// Control signals arrive on buffer channels 2,3 (2 audio + N signal; see
// widenForControl in graph_processor.cpp). The stereo noise is written to
// channels 0/1, with DECORRELATED left/right noise for natural stereo width.
//
// The amplitude envelope is the shared node AHDSR (node.ahdsrEnvelope), so the
// existing envelope editor works on this node unchanged - identical to the
// Signal Oscillator.
class SignalNoiseProcessor : public juce::AudioProcessor {
public:
    explicit SignalNoiseProcessor(Node& n) : node(n) {}

    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double sr, int) override {
        sampleRate = sr;
        tables.prepare(node.ahdsrEnvelope);
        env.hardReset();
        gateHigh = false;
        for (int c = 0; c < 2; ++c) { pink[c] = PinkState{}; brown[c] = 0.0f; }
    }
    void releaseResources() override {}
    // Transport panic (Stop): hard-reset the amp envelope so any release tail
    // is cut immediately instead of fading out over the Release time.
    void reset() override { env.hardReset(); gateHigh = false; }

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        const int n  = buf.getNumSamples();
        const int ch = buf.getNumChannels();
        tables.prepare(node.ahdsrEnvelope);

        const float volume = paramValue("Volume", 0.5f);
        const int   type   = (int)std::lround(paramValue("Type", 0.0f));

        float* d0 = ch > 0 ? buf.getWritePointer(0) : nullptr;
        float* d1 = ch > 1 ? buf.getWritePointer(1) : nullptr;

        for (int i = 0; i < n; ++i) {
            const float gate = (ch > 2) ? buf.getSample(2, i) : 0.0f;
            const float vel  = (ch > 3) ? buf.getSample(3, i) : 1.0f;

            const bool wantHigh = gate >= 0.5f;
            if (wantHigh && !gateHigh) env.noteOn(juce::jlimit(0.0f, 1.0f, vel));
            else if (!wantHigh && gateHigh) env.noteOff();
            gateHigh = wantHigh;

            const float amp = env.tick((float)sampleRate, node.ahdsrEnvelope, tables);
            const float g = amp * volume;

            if (d0) d0[i] = noiseSample(type, 0) * g;
            if (d1) d1[i] = noiseSample(type, 1) * g;
        }

        // Silence control channels so they don't leak downstream as audio.
        for (int c = 2; c < ch; ++c) buf.clear(c, 0, n);
    }

    double getTailLengthSeconds() const override {
        return node.ahdsrEnvelope.releaseMs / 1000.0;
    }
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

    // One noise sample on audio channel `c` (0 or 1) of the requested colour.
    // White is decorrelated per channel; Pink/Brown keep per-channel filter
    // state so the two channels stay independent (natural stereo width).
    float noiseSample(int type, int c) {
        const float white = rng.nextFloat() * 2.0f - 1.0f;
        if (type == 1) {
            // Pink: Paul Kellet's economical 7-pole filter, scaled to ~ -1..1.
            PinkState& p = pink[c];
            p.b0 = 0.99886f * p.b0 + white * 0.0555179f;
            p.b1 = 0.99332f * p.b1 + white * 0.0750759f;
            p.b2 = 0.96900f * p.b2 + white * 0.1538520f;
            p.b3 = 0.86650f * p.b3 + white * 0.3104856f;
            p.b4 = 0.55000f * p.b4 + white * 0.5329522f;
            p.b5 = -0.7616f * p.b5 - white * 0.0168980f;
            float out = p.b0 + p.b1 + p.b2 + p.b3 + p.b4 + p.b5 + p.b6
                        + white * 0.5362f;
            p.b6 = white * 0.115926f;
            out *= 0.11f;
            return juce::jlimit(-1.0f, 1.0f, out);
        }
        if (type == 2) {
            // Brown(ian): leaky integral of white, scaled up to a usable level.
            float& b = brown[c];
            b = 0.997f * b + white * 0.025f;
            return juce::jlimit(-1.0f, 1.0f, b * 8.0f);
        }
        return white; // 0 = White
    }

    struct PinkState {
        float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    };

    Node& node;
    double sampleRate = 44100.0;
    bool   gateHigh = false;
    juce::Random rng;
    PinkState pink[2];
    float     brown[2] = { 0.0f, 0.0f };

    AHDSRCurveTables     tables;
    AHDSREnvelopeRuntime env;
};

} // namespace SoundShop

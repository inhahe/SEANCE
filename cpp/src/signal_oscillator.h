#pragma once
#include "node_graph.h"
#include "adsr_envelope.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>

namespace SoundShop {

// =============================================================================
// SignalOscillatorProcessor  -  a Signal-driven monophonic oscillator
// =============================================================================
//
// This is the "modular payoff" half of per-voice polyphony (fork b). Unlike a
// MIDI synth, it takes no note stream: it reads three control SIGNALS and turns
// them into a tone:
//
//   Signal in 0: "Pitch"    (Hz)   - oscillator frequency, sample by sample.
//   Signal in 1: "Gate"     (0/1)  - note on while >= 0.5, release on the
//                                    falling edge; drives the AHDSR envelope.
//   Signal in 2: "Velocity" (0..1) - latched on the gate's rising edge and
//                                    fed to the envelope's velocity scaling.
//
// Wired from a VoiceIn puck's Pitch/Gate/Velocity outputs inside a Voice
// container, one instance lives in each voice clone, so it only ever needs to
// be MONOPHONIC - the container's cloning provides the polyphony. Outside a
// container it still works as a standalone signal-controlled oscillator.
//
// Control signals arrive on buffer channels 2,3,4 (2 audio + N signal; see
// widenForControl in graph_processor.cpp). The stereo tone is written to
// channels 0/1.
//
// The amplitude envelope is the shared node AHDSR (node.ahdsrEnvelope), so the
// existing envelope editor works on this node unchanged.
class SignalOscillatorProcessor : public juce::AudioProcessor {
public:
    explicit SignalOscillatorProcessor(Node& n) : node(n) {}

    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double sr, int) override {
        sampleRate = sr;
        tables.prepare(node.ahdsrEnvelope);
        env.hardReset();
        phase = 0.0;
        gateHigh = false;
    }
    void releaseResources() override {}
    // Transport panic (Stop): hard-reset the amp envelope so any release tail
    // is cut immediately instead of fading out over the Release time.
    void reset() override { env.hardReset(); phase = 0.0; gateHigh = false; }

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        const int n  = buf.getNumSamples();
        const int ch = buf.getNumChannels();
        // Keep the baked envelope tables current with any live edits (cheap
        // no-op when the curves are unchanged).
        tables.prepare(node.ahdsrEnvelope);

        const float volume   = paramValue("Volume", 0.5f);
        const int   waveform = (int)std::lround(paramValue("Waveform", 0.0f));
        // Pulse duty cycle (only used by the Pulse waveform). Clamp away from
        // 0/1 so the pulse never degenerates to DC silence. Modulatable via #88.
        const float pulseW   = juce::jlimit(0.02f, 0.98f, paramValue("Pulse Width", 0.5f));

        float* d0 = ch > 0 ? buf.getWritePointer(0) : nullptr;
        float* d1 = ch > 1 ? buf.getWritePointer(1) : nullptr;

        for (int i = 0; i < n; ++i) {
            // Read the control signals for this sample (default to a sensible
            // resting value when the pin isn't wired / channel is absent).
            const float pitchHz = (ch > 2) ? buf.getSample(2, i) : lastPitchHz;
            const float gate    = (ch > 3) ? buf.getSample(3, i) : 0.0f;
            const float vel     = (ch > 4) ? buf.getSample(4, i) : 1.0f;
            lastPitchHz = pitchHz > 0.0f ? pitchHz : lastPitchHz;

            // Gate edge detection -> envelope note-on / note-off.
            const bool wantHigh = gate >= 0.5f;
            if (wantHigh && !gateHigh) env.noteOn(juce::jlimit(0.0f, 1.0f, vel));
            else if (!wantHigh && gateHigh) env.noteOff();
            gateHigh = wantHigh;

            const float amp = env.tick((float)sampleRate, node.ahdsrEnvelope, tables);

            // Advance phase and synth the selected waveform.
            const double freq = (double)juce::jmax(0.0f, lastPitchHz);
            phase += freq / (sampleRate > 0.0 ? sampleRate : 44100.0);
            if (phase >= 1.0) phase -= std::floor(phase);
            const float s = oscSample(waveform, (float)phase, pulseW) * amp * volume;

            if (d0) d0[i] = s;
            if (d1) d1[i] = s;
        }

        // Silence any control channels in the output so they don't leak the
        // input signals back into a downstream that treats them as audio.
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

    static float oscSample(int waveform, float ph, float pulseW = 0.5f) {
        switch (waveform) {
            case 1:  return 2.0f * ph - 1.0f;                  // saw
            case 2:  return ph < 0.5f ? 1.0f : -1.0f;          // square
            case 3:  return 4.0f * std::abs(ph - 0.5f) - 1.0f; // triangle
            case 4:  return ph < pulseW ? 1.0f : -1.0f;        // pulse (variable width)
            default: return std::sin(ph * juce::MathConstants<float>::twoPi); // sine
        }
    }

    Node& node;
    double sampleRate = 44100.0;
    double phase = 0.0;
    float  lastPitchHz = 440.0f;
    bool   gateHigh = false;

    AHDSRCurveTables   tables;
    AHDSREnvelopeRuntime env;
};

} // namespace SoundShop

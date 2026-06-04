#pragma once
#include "node_graph.h"
#include "signal_modulation.h"
#include "wavelet.h"
#include "fft_util.h"
#include "builtin_synth.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <vector>
#include <random>
#include <complex>

namespace SoundShop {

// Helper: read a named param from the node, return def if not found.
inline float paramByName(const Node& node, const char* name, float def) {
    for (auto& p : node.params)
        if (p.name == name) return p.value;
    return def;
}

// Number of single-loop cycles a feedback delay takes to decay to -60 dB
// (the standard "RT60" inaudibility threshold).  feedback is the linear
// per-loop gain (e.g. 0.5 means each echo is half the previous one).
// Returns 0 when feedback ≈ 0 (single tap, no tail beyond the delay).
inline double feedbackLoopsToInaudible(double feedback) {
    feedback = std::min(std::abs(feedback), 0.999);   // guard against runaway
    if (feedback < 1e-4) return 1.0;                  // ~0 — just the one delay
    // gain_after_N_loops = feedback^N.  Solve feedback^N = 10^(-60/20) = 1e-3:
    //   N = log(1e-3) / log(feedback) = -3 * ln(10) / ln(feedback).
    return -3.0 * std::log(10.0) / std::log(feedback);
}

// ==============================================================================
// TREMOLO — amplitude modulation by an LFO
// Params: Rate (Hz), Depth (0-1), Shape (0=sine, 1=square, 2=triangle)
// ==============================================================================
class TremoloProcessor : public juce::AudioProcessor {
public:
    TremoloProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Tremolo"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        float rate  = paramByName(node, "Rate", 4.0f);
        float depth = paramByName(node, "Depth", 0.5f);
        int shape   = (int)paramByName(node, "Shape", 0.0f);
        for (int s = 0; s < buf.getNumSamples(); ++s) {
            float t = (float)(phase * 2.0 * 3.14159265);
            float lfo = 0;
            if (shape == 0) lfo = std::sin(t);
            else if (shape == 1) lfo = (std::sin(t) >= 0) ? 1.0f : -1.0f;
            else lfo = 2.0f * std::abs(2.0f * (float)(phase - std::floor(phase + 0.5))) - 1.0f;
            float gain = 1.0f - depth * 0.5f * (1.0f - lfo);
            for (int c = 0; c < buf.getNumChannels(); ++c)
                buf.getWritePointer(c)[s] *= gain;
            phase += rate / sampleRate;
            if (phase > 1.0) phase -= 1.0;
        }
    }
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
    double sampleRate = 44100, phase = 0;
};

// ==============================================================================
// VIBRATO — pitch modulation via modulated delay line
// Params: Rate (Hz), Depth (semitones)
// ==============================================================================
class VibratoProcessor : public juce::AudioProcessor {
public:
    VibratoProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Vibrato"; }
    void prepareToPlay(double sr, int) override {
        sampleRate = sr;
        int maxDelay = (int)(sr * 0.05); // 50ms max
        for (int c = 0; c < 2; ++c) {
            delayBuf[c].assign(maxDelay, 0.0f);
            writePos[c] = 0;
        }
    }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        float rate  = paramByName(node, "Rate", 5.0f);
        float depth = paramByName(node, "Depth", 0.3f); // semitones
        float maxDelayMs = depth * 0.5f; // rough mapping
        int maxDelaySamples = (int)(maxDelayMs * sampleRate / 1000.0);
        maxDelaySamples = std::min(maxDelaySamples, (int)delayBuf[0].size() - 1);
        for (int s = 0; s < buf.getNumSamples(); ++s) {
            float lfo = std::sin((float)(phase * 2.0 * 3.14159265));
            float delaySamples = (float)maxDelaySamples * (0.5f + 0.5f * lfo);
            for (int c = 0; c < std::min(buf.getNumChannels(), 2); ++c) {
                auto* data = buf.getWritePointer(c);
                delayBuf[c][writePos[c]] = data[s];
                // Read with linear interpolation
                float readPos = (float)writePos[c] - delaySamples;
                if (readPos < 0) readPos += (float)delayBuf[c].size();
                int idx = (int)readPos;
                float frac = readPos - idx;
                int idx2 = (idx + 1) % (int)delayBuf[c].size();
                idx = idx % (int)delayBuf[c].size();
                data[s] = delayBuf[c][idx] + frac * (delayBuf[c][idx2] - delayBuf[c][idx]);
                writePos[c] = (writePos[c] + 1) % (int)delayBuf[c].size();
            }
            phase += rate / sampleRate;
            if (phase > 1.0) phase -= 1.0;
        }
    }
    // Tail = the actual delay being read (depth × 0.5 ms), not the full
    // 50 ms buffer.  No feedback, so the buffered samples flush out and
    // there's no recirculating tail.
    double getTailLengthSeconds() const override {
        double depth = (double) paramByName(node, "Depth", 0.3f);
        return depth * 0.5e-3;
    }
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
    double sampleRate = 44100, phase = 0;
    std::vector<float> delayBuf[2];
    int writePos[2] = {0, 0};
};

// ==============================================================================
// FLANGER — short modulated delay with feedback, mixed with dry
// Params: Rate (Hz), Depth (0-1), Feedback (0-0.95), Mix (0-1)
// ==============================================================================
class FlangerProcessor : public juce::AudioProcessor {
public:
    FlangerProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Flanger"; }
    void prepareToPlay(double sr, int) override {
        sampleRate = sr;
        int maxDelay = (int)(sr * 0.015); // 15ms max
        for (int c = 0; c < 2; ++c) {
            delayBuf[c].assign(maxDelay, 0.0f);
            writePos[c] = 0;
        }
    }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        float rate     = paramByName(node, "Rate", 0.3f);
        float depth    = paramByName(node, "Depth", 0.7f);
        float feedback = paramByName(node, "Feedback", 0.5f);
        float mix      = paramByName(node, "Mix", 0.5f);
        int maxD = (int)delayBuf[0].size() - 1;
        for (int s = 0; s < buf.getNumSamples(); ++s) {
            float lfo = std::sin((float)(phase * 2.0 * 3.14159265));
            float delaySamples = depth * maxD * (0.5f + 0.5f * lfo);
            for (int c = 0; c < std::min(buf.getNumChannels(), 2); ++c) {
                auto* data = buf.getWritePointer(c);
                float dry = data[s];
                // Read delayed sample
                float readPos = (float)writePos[c] - delaySamples;
                if (readPos < 0) readPos += (float)delayBuf[c].size();
                int idx = (int)readPos % (int)delayBuf[c].size();
                int idx2 = (idx + 1) % (int)delayBuf[c].size();
                float frac = readPos - std::floor(readPos);
                float wet = delayBuf[c][idx] + frac * (delayBuf[c][idx2] - delayBuf[c][idx]);
                // Write with feedback
                delayBuf[c][writePos[c]] = dry + wet * feedback;
                writePos[c] = (writePos[c] + 1) % (int)delayBuf[c].size();
                data[s] = dry * (1.0f - mix) + wet * mix;
            }
            phase += rate / sampleRate;
            if (phase > 1.0) phase -= 1.0;
        }
    }
    // Tail = (15 ms max delay) × (loops until feedback decays to -60 dB).
    double getTailLengthSeconds() const override {
        double fb = (double) paramByName(node, "Feedback", 0.5f);
        return 0.015 * feedbackLoopsToInaudible(fb);
    }
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
    double sampleRate = 44100, phase = 0;
    std::vector<float> delayBuf[2];
    int writePos[2] = {0, 0};
};

// ==============================================================================
// PHASER — chain of allpass filters with LFO-modulated frequency
// Params: Rate (Hz), Depth (0-1), Stages (2-12), Feedback (0-0.95)
// ==============================================================================
class PhaserProcessor : public juce::AudioProcessor {
public:
    PhaserProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Phaser"; }
    void prepareToPlay(double sr, int) override {
        sampleRate = sr;
        for (auto& s : allpassState) s = {};
    }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        float rate     = paramByName(node, "Rate", 0.5f);
        float depth    = paramByName(node, "Depth", 0.7f);
        float feedback = paramByName(node, "Feedback", 0.3f);
        int stages     = juce::jlimit(2, 12, (int)paramByName(node, "Stages", 6.0f));
        for (int s = 0; s < buf.getNumSamples(); ++s) {
            float lfo = std::sin((float)(phase * 2.0 * 3.14159265));
            // Sweep center frequency: 200 Hz to 4000 Hz
            float centerFreq = 200.0f + (4000.0f - 200.0f) * depth * (0.5f + 0.5f * lfo);
            float d = -std::cos(2.0f * 3.14159265f * centerFreq / (float)sampleRate);
            for (int c = 0; c < std::min(buf.getNumChannels(), 2); ++c) {
                auto* data = buf.getWritePointer(c);
                float x = data[s] + lastOut[c] * feedback;
                // Chain of first-order allpass filters
                for (int st = 0; st < stages; ++st) {
                    float y = -x * 0.5f + d * allpassState[c * 12 + st].z1 + allpassState[c * 12 + st].z1 * 0.5f;
                    // Simplified: first-order allpass y = d*(x + y_prev) - x_prev
                    float a1 = d;
                    y = a1 * x + allpassState[c * 12 + st].z1 - a1 * allpassState[c * 12 + st].z2;
                    allpassState[c * 12 + st].z2 = allpassState[c * 12 + st].z1;
                    allpassState[c * 12 + st].z1 = x;
                    x = y;
                }
                lastOut[c] = x;
                data[s] = data[s] * 0.5f + x * 0.5f; // wet/dry 50%
            }
            phase += rate / sampleRate;
            if (phase > 1.0) phase -= 1.0;
        }
    }
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
    double sampleRate = 44100, phase = 0;
    struct APState { float z1 = 0, z2 = 0; };
    APState allpassState[24]; // 2 channels × 12 max stages
    float lastOut[2] = {0, 0};
};

// ==============================================================================
// COMPRESSOR — dynamics processor
// Params: Threshold (dB), Ratio, Attack (ms), Release (ms), Makeup Gain (dB)
// ==============================================================================
class CompressorProcessor : public juce::AudioProcessor {
public:
    CompressorProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Compressor"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        float threshold = paramByName(node, "Threshold", -20.0f); // dB
        float ratio     = paramByName(node, "Ratio", 4.0f);
        float attackMs  = paramByName(node, "Attack", 10.0f);
        float releaseMs = paramByName(node, "Release", 100.0f);
        float makeupDb  = paramByName(node, "Makeup Gain", 0.0f);
        float threshLin = std::pow(10.0f, threshold / 20.0f);
        float makeup    = std::pow(10.0f, makeupDb / 20.0f);
        float attackCoeff  = std::exp(-1.0f / (float)(attackMs * 0.001 * sampleRate));
        float releaseCoeff = std::exp(-1.0f / (float)(releaseMs * 0.001 * sampleRate));
        // Sidechain: if a Signal/Audio cable is wired to the "Sidechain"
        // pin, it arrives on channel 2 (the first control slot). Use
        // that for detection instead of the main audio input.
        bool hasSidechain = buf.getNumChannels() > 2;
        for (int s = 0; s < buf.getNumSamples(); ++s) {
            float peak = 0;
            if (hasSidechain) {
                peak = std::abs(buf.getSample(2, s));
            } else {
                for (int c = 0; c < std::min(2, buf.getNumChannels()); ++c)
                    peak = std::max(peak, std::abs(buf.getSample(c, s)));
            }
            // Envelope follower
            float coeff = (peak > envLevel) ? attackCoeff : releaseCoeff;
            envLevel = coeff * envLevel + (1.0f - coeff) * peak;
            // Gain computation
            float gain = 1.0f;
            if (envLevel > threshLin) {
                float dbOver = 20.0f * std::log10(envLevel / threshLin);
                float dbReduction = dbOver * (1.0f - 1.0f / ratio);
                gain = std::pow(10.0f, -dbReduction / 20.0f);
            }
            gain *= makeup;
            for (int c = 0; c < buf.getNumChannels(); ++c)
                buf.getWritePointer(c)[s] *= gain;
        }
    }
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
    float envLevel = 0;
};

// ==============================================================================
// LIMITER — brick-wall limiter (compressor with inf ratio, fast attack)
// Params: Ceiling (dB), Release (ms)
// ==============================================================================
class LimiterProcessor : public juce::AudioProcessor {
public:
    LimiterProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Limiter"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        float ceilingDb = paramByName(node, "Ceiling", -0.3f);
        float releaseMs = paramByName(node, "Release", 50.0f);
        float ceiling = std::pow(10.0f, ceilingDb / 20.0f);
        float releaseCoeff = std::exp(-1.0f / (float)(releaseMs * 0.001 * sampleRate));
        for (int s = 0; s < buf.getNumSamples(); ++s) {
            float peak = 0;
            for (int c = 0; c < buf.getNumChannels(); ++c)
                peak = std::max(peak, std::abs(buf.getSample(c, s)));
            float targetGain = (peak > ceiling) ? ceiling / peak : 1.0f;
            // Instant attack, smoothed release
            if (targetGain < gainState)
                gainState = targetGain; // instant
            else
                gainState = releaseCoeff * gainState + (1.0f - releaseCoeff) * targetGain;
            for (int c = 0; c < buf.getNumChannels(); ++c)
                buf.getWritePointer(c)[s] *= gainState;
        }
    }
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
    float gainState = 1.0f;
};

// ==============================================================================
// GATE — silences audio below a threshold
// Params: Threshold (dB), Attack (ms), Release (ms)
// ==============================================================================
class GateProcessor : public juce::AudioProcessor {
public:
    GateProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Gate"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        float threshDb  = paramByName(node, "Threshold", -40.0f);
        float attackMs  = paramByName(node, "Attack", 1.0f);
        float releaseMs = paramByName(node, "Release", 50.0f);
        float threshLin = std::pow(10.0f, threshDb / 20.0f);
        float attackCoeff  = std::exp(-1.0f / (float)(attackMs * 0.001 * sampleRate));
        float releaseCoeff = std::exp(-1.0f / (float)(releaseMs * 0.001 * sampleRate));
        for (int s = 0; s < buf.getNumSamples(); ++s) {
            float peak = 0;
            for (int c = 0; c < buf.getNumChannels(); ++c)
                peak = std::max(peak, std::abs(buf.getSample(c, s)));
            float target = (peak > threshLin) ? 1.0f : 0.0f;
            float coeff = (target > gateLevel) ? (1.0f - attackCoeff) : (1.0f - releaseCoeff);
            gateLevel += coeff * (target - gateLevel);
            for (int c = 0; c < buf.getNumChannels(); ++c)
                buf.getWritePointer(c)[s] *= gateLevel;
        }
    }
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
    float gateLevel = 0;
};

// ==============================================================================
// ECHO — delay line with feedback (infinite repeats that decay)
// Params: Delay (ms), Feedback (0-0.95), Mix (0-1)
// ==============================================================================
class EchoProcessor : public juce::AudioProcessor {
public:
    EchoProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Echo"; }
    void prepareToPlay(double sr, int) override {
        sampleRate = sr;
        int maxDelay = (int)(sr * 2.0); // 2 seconds max
        for (int c = 0; c < 2; ++c) {
            delayBuf[c].assign(maxDelay, 0.0f);
            writePos[c] = 0;
        }
    }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        float delayMs  = paramByName(node, "Delay", 300.0f);
        float feedback = paramByName(node, "Feedback", 0.5f);
        float mix      = paramByName(node, "Mix", 0.4f);
        int delaySamples = juce::jlimit(1, (int)delayBuf[0].size() - 1,
                                         (int)(delayMs * sampleRate / 1000.0));
        for (int s = 0; s < buf.getNumSamples(); ++s) {
            for (int c = 0; c < std::min(buf.getNumChannels(), 2); ++c) {
                auto* data = buf.getWritePointer(c);
                float dry = data[s];
                int readIdx = (writePos[c] - delaySamples + (int)delayBuf[c].size()) % (int)delayBuf[c].size();
                float wet = delayBuf[c][readIdx];
                delayBuf[c][writePos[c]] = dry + wet * feedback;
                writePos[c] = (writePos[c] + 1) % (int)delayBuf[c].size();
                data[s] = dry * (1.0f - mix) + wet * mix;
            }
        }
    }
    // Tail = (Delay ms) × (loops until feedback decays to -60 dB).
    // With default 300 ms delay + 0.5 feedback ≈ 3.0 s; with high
    // feedback (0.95) it stretches to ~17 s — the old hardcoded 5 s
    // was both wasteful at low feedback and clipping at high feedback.
    double getTailLengthSeconds() const override {
        double delaySec = (double) paramByName(node, "Delay", 300.0f) * 0.001;
        double fb       = (double) paramByName(node, "Feedback", 0.5f);
        return delaySec * feedbackLoopsToInaudible(fb);
    }
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
    std::vector<float> delayBuf[2];
    int writePos[2] = {0, 0};
};

// ==============================================================================
// ARPEGGIATOR — MIDI effect: hold a chord, plays notes sequentially
// Params: Rate (Hz or beat-synced), Pattern (0=up, 1=down, 2=updown, 3=random), Octaves (1-4)
// ==============================================================================
class ArpeggiatorProcessor : public juce::AudioProcessor {
public:
    ArpeggiatorProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Arpeggiator"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override {
        float rate    = paramByName(node, "Rate", 8.0f); // notes per second
        int pattern   = (int)paramByName(node, "Pattern", 0.0f);
        int octaves   = juce::jlimit(1, 4, (int)paramByName(node, "Octaves", 1.0f));

        // Collect held notes
        for (auto metadata : midi) {
            auto msg = metadata.getMessage();
            if (msg.isNoteOn()) heldNotes.insert(msg.getNoteNumber());
            if (msg.isNoteOff()) heldNotes.erase(msg.getNoteNumber());
        }
        midi.clear(); // we'll generate our own MIDI output

        if (heldNotes.empty()) {
            if (lastNote >= 0) {
                midi.addEvent(juce::MidiMessage::noteOff(1, lastNote), 0);
                lastNote = -1;
            }
            return;
        }

        // Build the note sequence
        std::vector<int> seq;
        std::vector<int> baseNotes(heldNotes.begin(), heldNotes.end());
        std::sort(baseNotes.begin(), baseNotes.end());
        for (int oct = 0; oct < octaves; ++oct)
            for (int n : baseNotes) {
                int note = n + oct * 12;
                if (note <= 127) seq.push_back(note);
            }

        if (pattern == 1) std::reverse(seq.begin(), seq.end());
        else if (pattern == 2) {
            auto down = seq;
            std::reverse(down.begin(), down.end());
            if (down.size() > 2) { down.erase(down.begin()); down.pop_back(); }
            seq.insert(seq.end(), down.begin(), down.end());
        }

        if (seq.empty()) return;

        // Advance and emit notes
        double samplesPerNote = sampleRate / std::max(0.1, (double)rate);
        for (int s = 0; s < buf.getNumSamples(); ++s) {
            sampleCounter++;
            if (sampleCounter >= samplesPerNote) {
                sampleCounter -= samplesPerNote;
                // Note off previous
                if (lastNote >= 0)
                    midi.addEvent(juce::MidiMessage::noteOff(1, lastNote), s);
                // Note on next
                if (pattern == 3) // random
                    seqIdx = rng() % (int)seq.size();
                else
                    seqIdx = (seqIdx + 1) % (int)seq.size();
                lastNote = seq[seqIdx];
                midi.addEvent(juce::MidiMessage::noteOn(1, lastNote, (juce::uint8)100), s);
            }
        }
    }
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
    double sampleRate = 44100, sampleCounter = 0;
    std::set<int> heldNotes;
    int seqIdx = -1, lastNote = -1;
    std::mt19937 rng{42};
};

// ==============================================================================
// MIXTURE — organ-style harmonics: each note triggers octaves + fifths above
// Params: Octaves (1-4), Include Fifths (0/1), Include Thirds (0/1), Level Decay (how
// much quieter each added harmonic is, 0-1)
// ==============================================================================
class MixtureProcessor : public juce::AudioProcessor {
public:
    MixtureProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Mixture"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer& midi) override {
        int numOctaves    = juce::jlimit(1, 4, (int)paramByName(node, "Octaves", 2.0f));
        bool includeFifths = paramByName(node, "Include Fifths", 1.0f) > 0.5f;
        bool includeThirds = paramByName(node, "Include Thirds", 0.0f) > 0.5f;
        float levelDecay   = paramByName(node, "Level Decay", 0.5f);

        juce::MidiBuffer output;
        for (auto metadata : midi) {
            auto msg = metadata.getMessage();
            output.addEvent(msg, metadata.samplePosition); // pass original

            if (msg.isNoteOn()) {
                int baseNote = msg.getNoteNumber();
                int baseVel = msg.getVelocity();
                float vel = (float)baseVel;
                // Add octaves above
                for (int oct = 1; oct <= numOctaves; ++oct) {
                    vel *= (1.0f - levelDecay);
                    int note = baseNote + oct * 12;
                    if (note <= 127 && vel > 1)
                        output.addEvent(juce::MidiMessage::noteOn(msg.getChannel(), note,
                            (juce::uint8)std::max(1, (int)vel)), metadata.samplePosition);
                }
                // Add fifths (7 semitones above each octave)
                if (includeFifths) {
                    vel = (float)baseVel;
                    for (int oct = 0; oct < numOctaves; ++oct) {
                        vel *= (1.0f - levelDecay * 0.8f);
                        int note = baseNote + oct * 12 + 7;
                        if (note <= 127 && vel > 1)
                            output.addEvent(juce::MidiMessage::noteOn(msg.getChannel(), note,
                                (juce::uint8)std::max(1, (int)vel)), metadata.samplePosition);
                    }
                }
                // Add thirds (4 semitones above, tierce de Picardie style)
                if (includeThirds) {
                    vel = (float)baseVel * (1.0f - levelDecay);
                    int note = baseNote + 4; // major third
                    if (note <= 127 && vel > 1)
                        output.addEvent(juce::MidiMessage::noteOn(msg.getChannel(), note,
                            (juce::uint8)std::max(1, (int)vel)), metadata.samplePosition);
                }
            }
            else if (msg.isNoteOff()) {
                int baseNote = msg.getNoteNumber();
                // Release all harmonics
                for (int oct = 1; oct <= numOctaves; ++oct) {
                    int note = baseNote + oct * 12;
                    if (note <= 127)
                        output.addEvent(juce::MidiMessage::noteOff(msg.getChannel(), note), metadata.samplePosition);
                }
                if (includeFifths) {
                    for (int oct = 0; oct < numOctaves; ++oct) {
                        int note = baseNote + oct * 12 + 7;
                        if (note <= 127)
                            output.addEvent(juce::MidiMessage::noteOff(msg.getChannel(), note), metadata.samplePosition);
                    }
                }
                if (includeThirds) {
                    int note = baseNote + 4;
                    if (note <= 127)
                        output.addEvent(juce::MidiMessage::noteOff(msg.getChannel(), note), metadata.samplePosition);
                }
            }
        }
        midi.swapWith(output);
    }
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
};

// VelocityScaleProcessor was replaced by the more general
// MidiModulatorProcessor (see midi_mod_node.h/.cpp). Old projects with
// __velscale__ scripts are auto-upgraded by that processor.

// ==============================================================================
// REVERB — algorithmic reverberation (Freeverb / Schroeder topology)
//
// Eight parallel lowpass-feedback comb filters per channel with a small
// stereo spread between left and right tunings, followed by four serial
// allpass filters. This is the classic Freeverb arrangement — the comb
// delays simulate the average reflection density in a room, each comb's
// feedback lowpass dulls successive reflections (so high frequencies
// decay faster than lows, as real rooms do), and the serial allpasses
// thicken the tail into a diffuse smear.
//
// Params:
//   Mix      — dry/wet crossfade, 0=dry 1=wet
//   Size     — room size, 0..1 (controls comb feedback gain; larger = longer tail)
//   Damping  — high-frequency damping in the feedback path, 0..1
//   Width    — stereo spread of the wet signal, 0..1 (0=mono, 1=full stereo)
//   Pre-Delay — delay before reverb kicks in, in ms
//
// Delay lengths are the well-known Freeverb tunings (samples at 44.1 kHz).
// They get scaled if the runtime sample rate differs, so the perceived
// room size stays consistent across sample rates.
// ==============================================================================
class ReverbProcessor : public juce::AudioProcessor {
public:
    ReverbProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Reverb"; }

    void prepareToPlay(double sr, int /*bs*/) override {
        sampleRate = sr;
        // Scale factor so delay lengths track sample rate.
        double scale = sr / 44100.0;

        // Freeverb classic tunings (samples @ 44.1 kHz).
        static const int kCombL[kNumCombs]    = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int kCombR[kNumCombs]    = {1116+23, 1188+23, 1277+23, 1356+23,
                                                  1422+23, 1491+23, 1557+23, 1617+23};
        static const int kAllpassL[kNumAllps] = {556, 441, 341, 225};
        static const int kAllpassR[kNumAllps] = {556+23, 441+23, 341+23, 225+23};

        for (int i = 0; i < kNumCombs; ++i) {
            combL[i].setSize((int)std::round(kCombL[i] * scale));
            combR[i].setSize((int)std::round(kCombR[i] * scale));
        }
        for (int i = 0; i < kNumAllps; ++i) {
            apL[i].setSize((int)std::round(kAllpassL[i] * scale));
            apR[i].setSize((int)std::round(kAllpassR[i] * scale));
        }

        int maxPredelay = (int)(sr * 0.2); // up to 200 ms
        predelayL.assign(std::max(1, maxPredelay), 0.0f);
        predelayR.assign(std::max(1, maxPredelay), 0.0f);
        predelayWritePos = 0;
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        const int n = buf.getNumSamples();
        const int ch = buf.getNumChannels();
        if (n == 0 || ch == 0) return;

        float mix       = juce::jlimit(0.0f, 1.0f, paramByName(node, "Mix",     0.3f));
        float size      = juce::jlimit(0.0f, 1.0f, paramByName(node, "Size",    0.6f));
        float damping   = juce::jlimit(0.0f, 1.0f, paramByName(node, "Damping", 0.5f));
        float widthRaw  = juce::jlimit(0.0f, 1.0f, paramByName(node, "Width",   1.0f));
        float preDelMs  = juce::jlimit(0.0f, 200.0f, paramByName(node, "Pre-Delay", 0.0f));

        // Freeverb gain mapping: feedback in [0.28..0.98] gives the usual
        // "tight room" to "long hall" range. 0.5 of the param maps to the
        // scaled-roomsize sweet spot.
        const float feedback = 0.28f + size * 0.70f;
        // Wet/dry mix as a 0..1 linear crossfade (constant-sum, not power).
        const float wetGain = mix;
        const float dryGain = 1.0f - mix;
        // Stereo width: wet1 feeds same-side, wet2 crosses to opposite.
        const float wet1 = widthRaw * 0.5f + 0.5f;
        const float wet2 = (1.0f - widthRaw) * 0.5f;

        const int   preDelSamples = std::min((int)(preDelMs * 0.001 * sampleRate),
                                               (int)predelayL.size() - 1);

        // Propagate damping/feedback to all comb filters.
        for (int i = 0; i < kNumCombs; ++i) {
            combL[i].feedback = feedback;
            combL[i].damp     = damping;
            combR[i].feedback = feedback;
            combR[i].damp     = damping;
        }

        float* left  = buf.getWritePointer(0);
        float* right = ch > 1 ? buf.getWritePointer(1) : left;

        for (int s = 0; s < n; ++s) {
            float inL = left[s];
            float inR = right[s];

            // Push into pre-delay ring. Read N samples back.
            predelayL[predelayWritePos] = inL;
            predelayR[predelayWritePos] = inR;
            int readPos = predelayWritePos - preDelSamples;
            if (readPos < 0) readPos += (int)predelayL.size();
            float wetInL = predelayL[readPos];
            float wetInR = predelayR[readPos];
            predelayWritePos++;
            if (predelayWritePos >= (int)predelayL.size()) predelayWritePos = 0;

            // Average the channels at the reverb input (standard Freeverb
            // behavior) to avoid cancellation artifacts in the tail, then
            // restore stereo via the wet1/wet2 spread at output time.
            float rvIn = (wetInL + wetInR) * 0.5f * 0.015f; // Freeverb input gain

            float outL = 0, outR = 0;
            for (int i = 0; i < kNumCombs; ++i) {
                outL += combL[i].process(rvIn);
                outR += combR[i].process(rvIn);
            }
            for (int i = 0; i < kNumAllps; ++i) {
                outL = apL[i].process(outL);
                outR = apR[i].process(outR);
            }

            left[s]  = inL * dryGain + (outL * wet1 + outR * wet2) * wetGain;
            if (ch > 1)
                right[s] = inR * dryGain + (outR * wet1 + outL * wet2) * wetGain;
        }
    }

    // Tail (RT60) = longest comb-filter delay × loops-to-inaudible at the
    // current feedback.  Feedback is mapped from the Size param exactly as
    // in processBlock (0.28..0.98).  Pre-delay adds linearly on top.
    double getTailLengthSeconds() const override {
        // Longest Freeverb comb tuning is 1617 samples @ 44.1 kHz.
        constexpr double kLongestCombSec = 1617.0 / 44100.0;
        double size     = (double) juce::jlimit(0.0f, 1.0f, paramByName(node, "Size",      0.6f));
        double feedback = 0.28 + size * 0.70;
        double preDel   = (double) juce::jlimit(0.0f, 200.0f, paramByName(node, "Pre-Delay", 0.0f)) * 0.001;
        return preDel + kLongestCombSec * feedbackLoopsToInaudible(feedback);
    }
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

    static constexpr int kNumCombs = 8;
    static constexpr int kNumAllps = 4;

    // Lowpass-feedback comb filter: the filter in the feedback loop is a
    // one-pole IIR lowpass, so successive echo repetitions get progressively
    // duller — simulating frequency-dependent absorption in real rooms.
    struct Comb {
        std::vector<float> buf;
        int pos = 0;
        float feedback = 0.5f;
        float damp = 0.5f;
        float lastLP = 0.0f;
        void setSize(int sz) {
            buf.assign(std::max(1, sz), 0.0f);
            pos = 0;
            lastLP = 0.0f;
        }
        float process(float in) {
            float y = buf[pos];
            // One-pole LP feedback: (1-damp)*y + damp*lastLP
            lastLP = y * (1.0f - damp) + lastLP * damp;
            buf[pos] = in + lastLP * feedback;
            if (++pos >= (int)buf.size()) pos = 0;
            return y;
        }
    };

    // Schroeder allpass: y = -x + buf[pos]; buf[pos] = x + 0.5*buf[pos].
    struct Allpass {
        std::vector<float> buf;
        int pos = 0;
        static constexpr float kFeedback = 0.5f;
        void setSize(int sz) { buf.assign(std::max(1, sz), 0.0f); pos = 0; }
        float process(float in) {
            float bufOut = buf[pos];
            float y = -in + bufOut;
            buf[pos] = in + bufOut * kFeedback;
            if (++pos >= (int)buf.size()) pos = 0;
            return y;
        }
    };

    Comb    combL[kNumCombs], combR[kNumCombs];
    Allpass apL[kNumAllps],   apR[kNumAllps];

    std::vector<float> predelayL, predelayR;
    int predelayWritePos = 0;
};

// ==============================================================================
// PARAMETRIC EQ — 4-band biquad equalizer
//
// Each band has its own type (Peak, Low Shelf, High Shelf, High Pass,
// Low Pass), frequency, gain (dB, relevant for peak/shelf), and Q.
// Biquad coefficients are computed from the Robert Bristow-Johnson
// Audio EQ Cookbook. The 4 bands cascade in series per stereo channel.
//
// Params (per band, N = 1..4):
//   BN Type  — 0=Peak, 1=LowShelf, 2=HighShelf, 3=HP, 4=LP
//   BN Freq  — center/corner frequency in Hz
//   BN Gain  — boost/cut in dB (peak and shelf only)
//   BN Q     — bandwidth / resonance (0.1..10)
// ==============================================================================
class ParametricEQProcessor : public juce::AudioProcessor {
public:
    ParametricEQProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "EQ"; }

    void prepareToPlay(double sr, int) override {
        sampleRate = sr;
        for (auto& b : bands) b.reset();
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        updateCoefficients();

        const int n = buf.getNumSamples();
        const int ch = buf.getNumChannels();
        for (int b = 0; b < kNumBands; ++b) {
            for (int c = 0; c < std::min(ch, 2); ++c) {
                float* data = buf.getWritePointer(c);
                auto& s = bands[b].state[c];
                const auto& co = bands[b].co;
                for (int i = 0; i < n; ++i) {
                    float x = data[i];
                    float y = co.b0 * x + co.b1 * s.x1 + co.b2 * s.x2
                            - co.a1 * s.y1 - co.a2 * s.y2;
                    s.x2 = s.x1; s.x1 = x;
                    s.y2 = s.y1; s.y1 = y;
                    data[i] = y;
                }
            }
        }
    }

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
    static constexpr int kNumBands = 4;

    struct Coeffs { float b0=1,b1=0,b2=0,a1=0,a2=0; };
    struct BiquadState { float x1=0,x2=0,y1=0,y2=0; };
    struct Band {
        Coeffs co;
        BiquadState state[2]; // stereo
        void reset() { state[0] = state[1] = {}; }
    };
    Band bands[kNumBands];

    // RBJ cookbook biquad coefficient computation.
    void updateCoefficients() {
        const char* bandNames[] = {"B1", "B2", "B3", "B4"};
        for (int b = 0; b < kNumBands; ++b) {
            std::string prefix = std::string(bandNames[b]) + " ";
            int type  = (int)paramByName(node, (prefix + "Type").c_str(),
                                          b == 0 ? 3.0f : b == 3 ? 4.0f : 0.0f);
            float freq = paramByName(node, (prefix + "Freq").c_str(),
                                      b == 0 ? 80.0f : b == 1 ? 400.0f :
                                      b == 2 ? 2500.0f : 8000.0f);
            float gain = paramByName(node, (prefix + "Gain").c_str(), 0.0f);
            float Q    = paramByName(node, (prefix + "Q").c_str(), 0.707f);

            freq = juce::jlimit(20.0f, (float)(sampleRate * 0.49), freq);
            Q    = juce::jlimit(0.1f, 10.0f, Q);

            const float kPi = 3.14159265358979323846f;
            float w0 = 2.0f * kPi * freq / (float)sampleRate;
            float cosw0 = std::cos(w0);
            float sinw0 = std::sin(w0);
            float alpha = sinw0 / (2.0f * Q);
            float A = std::pow(10.0f, gain / 40.0f); // sqrt of linear gain

            float b0=1, b1=0, b2=0, a0=1, a1=0, a2=0;

            switch (type) {
                case 0: // Peak EQ
                    b0 = 1.0f + alpha * A;
                    b1 = -2.0f * cosw0;
                    b2 = 1.0f - alpha * A;
                    a0 = 1.0f + alpha / A;
                    a1 = -2.0f * cosw0;
                    a2 = 1.0f - alpha / A;
                    break;
                case 1: { // Low Shelf
                    float t = 2.0f * std::sqrt(A) * alpha;
                    b0 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 + t);
                    b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0);
                    b2 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 - t);
                    a0 = (A + 1.0f) + (A - 1.0f) * cosw0 + t;
                    a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0);
                    a2 = (A + 1.0f) + (A - 1.0f) * cosw0 - t;
                    break;
                }
                case 2: { // High Shelf
                    float t = 2.0f * std::sqrt(A) * alpha;
                    b0 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 + t);
                    b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0);
                    b2 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 - t);
                    a0 = (A + 1.0f) - (A - 1.0f) * cosw0 + t;
                    a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0);
                    a2 = (A + 1.0f) - (A - 1.0f) * cosw0 - t;
                    break;
                }
                case 3: // High Pass
                    b0 = (1.0f + cosw0) / 2.0f;
                    b1 = -(1.0f + cosw0);
                    b2 = (1.0f + cosw0) / 2.0f;
                    a0 = 1.0f + alpha;
                    a1 = -2.0f * cosw0;
                    a2 = 1.0f - alpha;
                    break;
                case 4: // Low Pass
                    b0 = (1.0f - cosw0) / 2.0f;
                    b1 = 1.0f - cosw0;
                    b2 = (1.0f - cosw0) / 2.0f;
                    a0 = 1.0f + alpha;
                    a1 = -2.0f * cosw0;
                    a2 = 1.0f - alpha;
                    break;
                default: // bypass
                    b0 = 1; b1 = b2 = a1 = a2 = 0; a0 = 1;
                    break;
            }
            // Normalize by a0.
            float inv = 1.0f / a0;
            bands[b].co = {b0*inv, b1*inv, b2*inv, a1*inv, a2*inv};
        }
    }
};

// ==============================================================================
// RING MODULATOR — multiplies two audio signals
//
// Takes two Audio inputs (Carrier + Modulator) and outputs their
// sample-by-sample product. Produces metallic, bell-like, inharmonic
// tones. When only one input is connected, the internal oscillator
// acts as the modulator at a user-set frequency.
//
// Params: Mix (dry/wet), Int Freq (internal osc Hz, used when no
// second input), Int Shape (0=sine, 1=square, 2=triangle).
// ==============================================================================
class RingModProcessor : public juce::AudioProcessor {
public:
    RingModProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Ring Mod"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        float mix      = paramByName(node, "Mix", 0.5f);
        float intFreq  = paramByName(node, "Int Freq", 440.0f);
        int   intShape = (int)paramByName(node, "Int Shape", 0.0f);

        const int n = buf.getNumSamples();
        const int ch = buf.getNumChannels();
        // If the node has a second Audio input wired, the graph processor
        // sums it into channel 0/1 alongside the first input — there's no
        // separate channel for the modulator in the current routing model.
        // So for the two-input case we'd need a dedicated routing path.
        // For now, use the internal oscillator as the modulator source.
        // (A future enhancement can add a second bus via JUCE's bus API.)
        for (int s = 0; s < n; ++s) {
            float mod = 0;
            float t = (float)(phase * 2.0 * 3.14159265);
            if (intShape == 0) mod = std::sin(t);
            else if (intShape == 1) mod = std::sin(t) >= 0 ? 1.0f : -1.0f;
            else mod = 2.0f * std::abs(2.0f * (float)(phase - std::floor(phase + 0.5))) - 1.0f;
            phase += intFreq / sampleRate;
            if (phase > 1.0) phase -= 1.0;

            for (int c = 0; c < ch; ++c) {
                float dry = buf.getSample(c, s);
                float wet = dry * mod;
                buf.setSample(c, s, dry * (1.0f - mix) + wet * mix);
            }
        }
    }

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
    double sampleRate = 44100, phase = 0;
};

// ==============================================================================
// MID/SIDE ENCODE — splits stereo into Mid + Side on separate channels
// MID/SIDE DECODE — recombines Mid + Side back into stereo
//
// Both are implemented as a single processor that reads a "Mode" param:
//   0 = Encode (L/R → Mid/Side)
//   1 = Decode (Mid/Side → L/R)
// This lets one node type serve both halves of the utility pair.
// ==============================================================================
class MidSideProcessor : public juce::AudioProcessor {
public:
    MidSideProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "M/S"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        if (buf.getNumChannels() < 2 || buf.getNumSamples() == 0) return;
        int mode = (int)paramByName(node, "Mode", 0.0f);
        float* L = buf.getWritePointer(0);
        float* R = buf.getWritePointer(1);
        for (int s = 0; s < buf.getNumSamples(); ++s) {
            float l = L[s], r = R[s];
            if (mode == 0) {
                // Encode: Mid = (L+R)/2, Side = (L-R)/2
                L[s] = (l + r) * 0.5f;
                R[s] = (l - r) * 0.5f;
            } else {
                // Decode: L = Mid+Side, R = Mid-Side
                L[s] = l + r;
                R[s] = l - r;
            }
        }
    }

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
};

// ==============================================================================
// FM SYNTHESIS — 4-operator frequency modulation synthesizer
//
// Each operator is a sine oscillator with its own frequency ratio
// (relative to the MIDI note), level, and ADSR envelope. "Algorithm"
// selects the routing: which operators modulate which, and which go
// directly to the output. Op4 can self-modulate (feedback) for richer
// harmonics.
//
// 8 algorithms (classic 4-op patterns):
//   0: 4→3→2→1→out                (full series chain)
//   1: (3+4)→2→1→out              (two mods into one carrier stack)
//   2: 4→3→out, 2→1→out           (two independent mod→carrier pairs)
//   3: 4→(1+2+3)→out              (one mod into three carriers)
//   4: (4→3)→out, 2→out, 1→out    (one pair + two additive)
//   5: (4→3)→out, (4→2)→out, 1→out (shared mod)
//   6: 4→3→2→out, 1→out           (3-chain + additive)
//   7: 1+2+3+4→out                (pure additive, no FM)
// ==============================================================================
class FMSynthProcessor : public juce::AudioProcessor {
public:
    FMSynthProcessor(Node& n) : node(n) { voices.resize(16); }
    const juce::String getName() const override { return "FM Synth"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override {
        applySignalModulations(node, buf);
        buf.clear();
        const int numSamples = buf.getNumSamples();
        if (numSamples == 0) return;

        int algo    = (int)paramByName(node, "Algorithm", 0.0f);
        float fbAmt = paramByName(node, "Feedback", 0.3f);

        // Per-operator params.
        struct OpParams { float ratio, level, a, d, s, r; };
        OpParams ops[4];
        const char* opNames[] = {"Op1","Op2","Op3","Op4"};
        for (int i = 0; i < 4; ++i) {
            std::string p(opNames[i]);
            ops[i].ratio = paramByName(node, (p+" Ratio").c_str(), (float)(i+1));
            ops[i].level = paramByName(node, (p+" Level").c_str(), i==0?1.0f:0.5f);
            ops[i].a     = std::max(0.001f, paramByName(node, (p+" A").c_str(), 0.01f));
            ops[i].d     = std::max(0.001f, paramByName(node, (p+" D").c_str(), 0.1f));
            ops[i].s     = paramByName(node, (p+" S").c_str(), 0.7f);
            ops[i].r     = std::max(0.001f, paramByName(node, (p+" R").c_str(), 0.3f));
        }

        // Handle MIDI.
        for (auto meta : midi) {
            auto msg = meta.getMessage();
            if (msg.isNoteOn()) {
                auto& v = allocVoice();
                v.active = true;
                v.note = msg.getNoteNumber();
                v.vel = msg.getVelocity() / 127.0f;
                v.held = true;
                v.time = 0;
                v.relTime = 0;
                for (int i = 0; i < 4; ++i) v.phase[i] = 0;
                v.fb1 = v.fb2 = 0;
            } else if (msg.isNoteOff()) {
                for (auto& v : voices)
                    if (v.active && v.held && v.note == msg.getNoteNumber())
                        { v.held = false; v.relTime = v.time; }
            }
        }

        const float kPi2 = 6.28318530718f;
        float volume = paramByName(node, "Volume", 0.5f);
        float dt = 1.0f / (float)sampleRate;

        for (int s = 0; s < numSamples; ++s) {
            float out = 0;
            for (auto& v : voices) {
                if (!v.active) continue;
                // MPE per-note pitch bend (#78)
                float baseFreq = 440.0f * std::pow(2.0f, (v.note - 69 + v.mpe.pitchBend) / 12.0f);
                // Compute per-operator envelopes.
                float env[4];
                for (int i = 0; i < 4; ++i) {
                    float t = v.time;
                    if (v.held) {
                        if (t < ops[i].a) env[i] = t / ops[i].a;
                        else if (t < ops[i].a + ops[i].d)
                            env[i] = 1.0f + (ops[i].s - 1.0f) * ((t - ops[i].a) / ops[i].d);
                        else env[i] = ops[i].s;
                    } else {
                        float envAtRel = ops[i].s;
                        float rt = v.time - v.relTime;
                        env[i] = envAtRel * std::max(0.0f, 1.0f - rt / ops[i].r);
                        if (rt >= ops[i].r) env[i] = 0;
                    }
                    env[i] *= ops[i].level;
                }
                // Check if all envelopes are done.
                if (!v.held) {
                    bool allDone = true;
                    for (int i = 0; i < 4; ++i)
                        if (env[i] > 0.0001f) { allDone = false; break; }
                    if (allDone) { v.active = false; continue; }
                }
                // Compute operators with algorithm routing.
                // Op4 with feedback.
                float fb = (v.fb1 + v.fb2) * 0.5f * fbAmt;
                float o4 = std::sin(v.phase[3] * kPi2 + fb) * env[3];
                v.fb2 = v.fb1; v.fb1 = o4;
                float o3, o2, o1;
                switch (algo) {
                    case 0: // 4→3→2→1→out
                        o3 = std::sin(v.phase[2]*kPi2 + o4*kPi2) * env[2];
                        o2 = std::sin(v.phase[1]*kPi2 + o3*kPi2) * env[1];
                        o1 = std::sin(v.phase[0]*kPi2 + o2*kPi2) * env[0];
                        out += o1; break;
                    case 1: // (3+4)→2→1→out
                        o3 = std::sin(v.phase[2]*kPi2 + o4*kPi2) * env[2];
                        o2 = std::sin(v.phase[1]*kPi2 + (o3+o4)*0.5f*kPi2) * env[1];
                        o1 = std::sin(v.phase[0]*kPi2 + o2*kPi2) * env[0];
                        out += o1; break;
                    case 2: // 4→3→out, 2→1→out
                        o3 = std::sin(v.phase[2]*kPi2 + o4*kPi2) * env[2];
                        o2 = std::sin(v.phase[1]*kPi2) * env[1];
                        o1 = std::sin(v.phase[0]*kPi2 + o2*kPi2) * env[0];
                        out += (o3 + o1) * 0.5f; break;
                    case 3: // 4→(1+2+3)→out
                        o3 = std::sin(v.phase[2]*kPi2 + o4*kPi2) * env[2];
                        o2 = std::sin(v.phase[1]*kPi2 + o4*kPi2) * env[1];
                        o1 = std::sin(v.phase[0]*kPi2 + o4*kPi2) * env[0];
                        out += (o1 + o2 + o3) * 0.33f; break;
                    case 4: // (4→3)→out, 2→out, 1→out
                        o3 = std::sin(v.phase[2]*kPi2 + o4*kPi2) * env[2];
                        o2 = std::sin(v.phase[1]*kPi2) * env[1];
                        o1 = std::sin(v.phase[0]*kPi2) * env[0];
                        out += (o1 + o2 + o3) * 0.33f; break;
                    case 5: // (4→3)→out, (4→2)→out, 1→out
                        o3 = std::sin(v.phase[2]*kPi2 + o4*kPi2) * env[2];
                        o2 = std::sin(v.phase[1]*kPi2 + o4*kPi2) * env[1];
                        o1 = std::sin(v.phase[0]*kPi2) * env[0];
                        out += (o1 + o2 + o3) * 0.33f; break;
                    case 6: // 4→3→2→out, 1→out
                        o3 = std::sin(v.phase[2]*kPi2 + o4*kPi2) * env[2];
                        o2 = std::sin(v.phase[1]*kPi2 + o3*kPi2) * env[1];
                        o1 = std::sin(v.phase[0]*kPi2) * env[0];
                        out += (o1 + o2) * 0.5f; break;
                    default: // 7: all additive
                        o3 = std::sin(v.phase[2]*kPi2) * env[2];
                        o2 = std::sin(v.phase[1]*kPi2) * env[1];
                        o1 = std::sin(v.phase[0]*kPi2) * env[0];
                        out += (o1 + o2 + o3 + o4) * 0.25f; break;
                }
                // Advance phases.
                for (int i = 0; i < 4; ++i)
                    v.phase[i] += (baseFreq * ops[i].ratio) / (float)sampleRate;
                v.time += dt;
                out *= v.vel;
            }
            out *= volume;
            out = juce::jlimit(-1.0f, 1.0f, out);
            for (int c = 0; c < buf.getNumChannels(); ++c)
                buf.addSample(c, s, out);
        }
    }

    // Tail = max release time across the 4 operators (FM voice ends when
    // every op envelope reaches 0).  Names match the per-op R param the
    // processBlock reads.
    double getTailLengthSeconds() const override {
        double maxR = 0.001;
        const char* opNames[] = {"Op1","Op2","Op3","Op4"};
        for (int i = 0; i < 4; ++i) {
            std::string p(opNames[i]);
            double r = (double) paramByName(node, (p+" R").c_str(), 0.3f);
            if (r > maxR) maxR = r;
        }
        return maxR;
    }
    bool acceptsMidi() const override { return true; }
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
    Node& node;
    double sampleRate = 44100;
    struct Voice {
        bool active = false, held = false;
        int note = 0;
        float vel = 0, time = 0, relTime = 0;
        float phase[4] = {};
        float fb1 = 0, fb2 = 0;
        int mpeChannel = 1;     // MPE per-note channel (#78)
        MpeVoiceState mpe;
    };
    std::vector<Voice> voices;
    Voice& allocVoice() {
        for (auto& v : voices) if (!v.active) return v;
        float oldest = -1; int idx = 0;
        for (int i = 0; i < (int)voices.size(); ++i)
            if (voices[i].time > oldest) { oldest = voices[i].time; idx = i; }
        return voices[idx];
    }
};

// ==============================================================================
// PHASE DISTORTION SYNTHESIS — Casio CZ-style instrument
//
// Warps a sine wave's phase with a modulator function controlled by a
// "depth" parameter to produce subtractive-like sounds. When depth=0,
// output is a pure sine. At depth=1, the waveform matches the selected
// shape (saw, square, pulse, or resonant). The depth can change over
// time (via an internal "DCW envelope") to create filter-sweep-like
// timbral motion without an actual filter.
//
// Waveform types:
//   0=Sawtooth — phase is compressed into the first half cycle
//   1=Square   — phase holds then snaps
//   2=Pulse    — ultra-narrow phase concentration
//   3=Resonant — multiplied phase creates harmonic bursts
//
// Params: Waveform, Depth, DCW Attack, DCW Decay, DCW Sustain,
//         Attack, Decay, Sustain, Release, Volume
// ==============================================================================
class PDSynthProcessor : public juce::AudioProcessor {
public:
    PDSynthProcessor(Node& n) : node(n) { voices.resize(12); }
    const juce::String getName() const override { return "PD Synth"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override {
        applySignalModulations(node, buf);
        buf.clear();
        const int numSamples = buf.getNumSamples();
        if (numSamples == 0) return;

        int waveform   = (int)paramByName(node, "Waveform", 0.0f);
        float maxDepth = paramByName(node, "Depth", 0.8f);
        // DCW envelope: controls depth over time (like CZ's DCW)
        float dcwA     = std::max(0.001f, paramByName(node, "DCW Attack", 0.01f));
        float dcwD     = std::max(0.001f, paramByName(node, "DCW Decay", 0.3f));
        float dcwS     = paramByName(node, "DCW Sustain", 0.3f);
        // Amplitude ADSR
        float aA       = std::max(0.001f, paramByName(node, "Attack", 0.005f));
        float aD       = std::max(0.001f, paramByName(node, "Decay", 0.1f));
        float aS       = paramByName(node, "Sustain", 0.7f);
        float aR       = std::max(0.001f, paramByName(node, "Release", 0.3f));
        float volume   = paramByName(node, "Volume", 0.5f);

        for (auto meta : midi) {
            auto msg = meta.getMessage();
            if (msg.isNoteOn()) {
                auto& v = allocVoice();
                v.active = true; v.held = true;
                v.note = msg.getNoteNumber();
                v.vel = msg.getVelocity() / 127.0f;
                v.mpeChannel = msg.getChannel();
                v.mpe = MpeVoiceState{};
                v.phase = 0; v.time = 0; v.relTime = 0;
            } else if (msg.isNoteOff()) {
                for (auto& v : voices)
                    if (v.active && v.held && v.note == msg.getNoteNumber())
                        { v.held = false; v.relTime = v.time; }
            }
        }
        // Distribute MPE per-channel messages to voices (#78)
        distributeMpeMessages(midi, voices);

        float dt = 1.0f / (float)sampleRate;
        const float kPi2 = 6.28318530718f;

        for (int s = 0; s < numSamples; ++s) {
            float out = 0;
            for (auto& v : voices) {
                if (!v.active) continue;
                float freq = 440.0f * std::pow(2.0f, (v.note - 69) / 12.0f);

                // Amplitude ADSR
                float ampEnv = 0;
                if (v.held) {
                    if (v.time < aA) ampEnv = v.time / aA;
                    else if (v.time < aA + aD) ampEnv = 1.0f + (aS - 1.0f) * ((v.time - aA) / aD);
                    else ampEnv = aS;
                } else {
                    float rt = v.time - v.relTime;
                    ampEnv = aS * std::max(0.0f, 1.0f - rt / aR);
                    if (rt >= aR) { v.active = false; continue; }
                }

                // DCW envelope (drives depth): attack → decay → sustain level × maxDepth
                float dcwEnv = 0;
                if (v.time < dcwA) dcwEnv = v.time / dcwA;
                else if (v.time < dcwA + dcwD) dcwEnv = 1.0f + (dcwS - 1.0f) * ((v.time - dcwA) / dcwD);
                else dcwEnv = dcwS;
                float depth = maxDepth * dcwEnv;

                // Phase distortion
                float p = (float)std::fmod(v.phase, 1.0);
                if (p < 0) p += 1.0f;
                float distorted = p; // identity = sine

                switch (waveform) {
                    case 0: { // Sawtooth: compress first half, stretch second
                        float split = 0.5f - depth * 0.45f;
                        if (split < 0.05f) split = 0.05f;
                        if (p < split) distorted = (p / split) * 0.5f;
                        else distorted = 0.5f + ((p - split) / (1.0f - split)) * 0.5f;
                        break;
                    }
                    case 1: { // Square: hold at top, snap down
                        float hold = 0.5f + depth * 0.45f;
                        if (p < hold) distorted = (p / hold) * 0.5f;
                        else distorted = 0.5f + ((p - hold) / (1.0f - hold)) * 0.5f;
                        break;
                    }
                    case 2: { // Pulse: narrow spike
                        float w = 0.5f - depth * 0.48f;
                        if (w < 0.02f) w = 0.02f;
                        if (p < w) distorted = (p / w) * 0.5f;
                        else if (p < w * 2) distorted = 0.5f + ((p - w) / w) * 0.5f;
                        else distorted = 0.0f;
                        break;
                    }
                    case 3: { // Resonant: multiply phase for harmonic ringing
                        int mult = 1 + (int)(depth * 7.0f);
                        distorted = std::fmod(p * mult, 1.0f);
                        // Window with original phase to create decay
                        float window = 1.0f - p;
                        distorted = std::sin(distorted * kPi2) * window;
                        // Skip the sin() below — we already computed the output
                        out += distorted * ampEnv * v.vel;
                        v.phase += freq / (float)sampleRate;
                        v.time += dt;
                        goto nextVoice;
                    }
                }

                out += std::sin(distorted * kPi2) * ampEnv * v.vel;
                v.phase += freq / (float)sampleRate;
                v.time += dt;
                nextVoice:;
            }
            out *= volume;
            out = juce::jlimit(-1.0f, 1.0f, out);
            for (int c = 0; c < buf.getNumChannels(); ++c)
                buf.addSample(c, s, out);
        }
    }

    // Tail = the Release param (single-envelope synth, voice dies at
    // time = releaseTime after note-off).
    double getTailLengthSeconds() const override {
        return (double) std::max(0.001f, paramByName(node, "Release", 0.3f));
    }
    bool acceptsMidi() const override { return true; }
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
    Node& node;
    double sampleRate = 44100;
    struct Voice {
        bool active = false, held = false;
        int note = 0;
        float vel = 0, time = 0, relTime = 0;
        double phase = 0;
        int mpeChannel = 1;
        MpeVoiceState mpe;
    };
    std::vector<Voice> voices;
    Voice& allocVoice() {
        for (auto& v : voices) if (!v.active) return v;
        float oldest = -1; int idx = 0;
        for (int i = 0; i < (int)voices.size(); ++i)
            if (voices[i].time > oldest) { oldest = voices[i].time; idx = i; }
        return voices[idx];
    }
};

// ==============================================================================
// PARTICLE / GRANULAR CLOUD SYNTH
//
// Generates N overlapping short waveform bursts ("particles"), each
// with its own mini-envelope, frequency, and random spread. The result
// is a cloud-like texture that can range from ambient pads (dense,
// slow, narrow spread) to glitchy textures (sparse, fast, wide spread).
// MIDI note sets the base frequency; velocity sets density.
//
// Params:
//   Density    — particles per second (1..200)
//   Spread     — frequency randomization range in semitones (0..24)
//   Grain Size — duration of each particle in ms (1..500)
//   Attack     — per-particle attack as fraction of grain (0..1)
//   Release    — per-particle release as fraction of grain (0..1)
//   Shape      — particle waveform: 0=sine, 1=saw, 2=square, 3=noise
//   Volume
// ==============================================================================
class ParticleSynthProcessor : public juce::AudioProcessor {
public:
    ParticleSynthProcessor(Node& n) : node(n) { grains.reserve(128); }
    const juce::String getName() const override { return "Particle"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override {
        applySignalModulations(node, buf);
        buf.clear();
        const int numSamples = buf.getNumSamples();
        if (numSamples == 0) return;

        float density    = paramByName(node, "Density", 30.0f);
        float spread     = paramByName(node, "Spread", 7.0f);
        float grainMs    = paramByName(node, "Grain Size", 50.0f);
        float attackFrac = paramByName(node, "Attack", 0.1f);
        float relFrac    = paramByName(node, "Release", 0.3f);
        int   shape      = (int)paramByName(node, "Shape", 0.0f);
        float volume     = paramByName(node, "Volume", 0.5f);

        // Handle MIDI
        for (auto meta : midi) {
            auto msg = meta.getMessage();
            if (msg.isNoteOn()) {
                heldNote = msg.getNoteNumber();
                heldVel = msg.getVelocity() / 127.0f;
                noteActive = true;
            } else if (msg.isNoteOff() && msg.getNoteNumber() == heldNote) {
                noteActive = false;
            }
        }

        float dt = 1.0f / (float)sampleRate;
        float grainSec = grainMs * 0.001f;
        float spawnInterval = 1.0f / std::max(1.0f, density);
        const float kPi2 = 6.28318530718f;

        for (int s = 0; s < numSamples; ++s) {
            // Spawn new grains when a note is held
            if (noteActive) {
                spawnTimer += dt;
                while (spawnTimer >= spawnInterval) {
                    spawnTimer -= spawnInterval;
                    Grain g;
                    float baseFreq = 440.0f * std::pow(2.0f, (heldNote - 69) / 12.0f);
                    // Randomize pitch
                    float semiOff = ((float)rng() / (float)rng.max() - 0.5f) * 2.0f * spread;
                    g.freq = baseFreq * std::pow(2.0f, semiOff / 12.0f);
                    g.duration = grainSec;
                    g.attackTime = grainSec * attackFrac;
                    g.releaseTime = grainSec * relFrac;
                    g.phase = 0;
                    g.age = 0;
                    g.vel = heldVel;
                    g.pan = (float)rng() / (float)rng.max() * 2.0f - 1.0f; // random stereo
                    grains.push_back(g);
                }
            }

            float outL = 0, outR = 0;
            for (auto it = grains.begin(); it != grains.end();) {
                auto& g = *it;
                // Per-grain envelope
                float env = 1.0f;
                if (g.age < g.attackTime) env = g.age / std::max(0.0001f, g.attackTime);
                else if (g.age > g.duration - g.releaseTime)
                    env = std::max(0.0f, (g.duration - g.age) / std::max(0.0001f, g.releaseTime));
                if (g.age >= g.duration) { it = grains.erase(it); continue; }

                float sample = 0;
                float ph = (float)std::fmod(g.phase, 1.0);
                switch (shape) {
                    case 0: sample = std::sin(ph * kPi2); break;
                    case 1: sample = ph * 2.0f - 1.0f; break;
                    case 2: sample = ph < 0.5f ? 1.0f : -1.0f; break;
                    default: sample = ((float)rng() / (float)rng.max()) * 2.0f - 1.0f; break;
                }
                sample *= env * g.vel;
                float panL = std::cos((g.pan + 1.0f) * 0.25f * 3.14159f);
                float panR = std::sin((g.pan + 1.0f) * 0.25f * 3.14159f);
                outL += sample * panL;
                outR += sample * panR;

                g.phase += g.freq / (float)sampleRate;
                g.age += dt;
                ++it;
            }

            // Scale by sqrt of active grain count to prevent clipping
            float gc = (float)grains.size();
            if (gc > 1) { outL /= std::sqrt(gc); outR /= std::sqrt(gc); }
            outL *= volume; outR *= volume;

            if (buf.getNumChannels() >= 1) buf.addSample(0, s, outL);
            if (buf.getNumChannels() >= 2) buf.addSample(1, s, outR);
        }

        // Safety: cap grain count
        if (grains.size() > 1024) grains.erase(grains.begin(), grains.begin() + 512);
    }

    // Tail = the grain duration.  Each particle has its own envelope that
    // completes within the grain, so once new particles stop firing
    // (no more held notes), the longest possible remaining audio is one
    // full grain.
    double getTailLengthSeconds() const override {
        return (double) paramByName(node, "Grain Size", 50.0f) * 0.001;
    }
    bool acceptsMidi() const override { return true; }
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
    Node& node;
    double sampleRate = 44100;
    struct Grain {
        float freq, duration, attackTime, releaseTime;
        double phase;
        float age, vel, pan;
    };
    std::vector<Grain> grains;
    bool noteActive = false;
    int heldNote = 60;
    float heldVel = 0.8f;
    float spawnTimer = 0;
    std::mt19937 rng{42};
};

// ==============================================================================
// TRANSIENT/SUSTAIN SPLIT — wavelet-based separation
//
// Decomposes audio into transient (attack) and sustain (body) components
// using wavelet thresholding. The transient part captures sharp onsets
// (drum hits, plucks, consonants); the sustain part captures the
// steady-state body (tones, reverb tails, vowels).
//
// Two audio outputs: the node's main out carries the recombined signal
// with adjustable transient/sustain balance, but if the user wires
// into the second output (via the signal pin), they get the separated
// transient-only signal for independent routing.
//
// Params:
//   Transient  — gain multiplier for the transient component (0..2)
//   Sustain    — gain multiplier for the sustain component (0..2)
//   Threshold  — wavelet coefficient threshold for separation (0..1)
//   Levels     — number of DWT decomposition levels (1..8)
// ==============================================================================
class TransientSplitProcessor : public juce::AudioProcessor {
public:
    TransientSplitProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Transient Split"; }
    void prepareToPlay(double sr, int bs) override {
        sampleRate = sr;
        blockSize = bs;
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        const int n = buf.getNumSamples();
        const int ch = std::min(2, buf.getNumChannels());
        if (n == 0 || ch == 0) return;

        float transGain = paramByName(node, "Transient", 1.0f);
        float susGain   = paramByName(node, "Sustain",   1.0f);
        float threshold = paramByName(node, "Threshold", 0.3f);
        int   levels    = juce::jlimit(1, 8, (int)paramByName(node, "Levels", 4.0f));

        auto filt = getWaveletFilter("db4");

        // Pad to next power of 2 for DWT.
        int padLen = 1;
        while (padLen < n) padLen *= 2;

        for (int c = 0; c < ch; ++c) {
            float* data = buf.getWritePointer(c);

            // Copy into padded buffer.
            std::vector<float> sig(padLen, 0.0f);
            for (int i = 0; i < n; ++i) sig[i] = data[i];
            std::vector<float> original = sig;

            // Forward DWT.
            int actualLevels = dwt(sig, levels, filt);

            // Threshold: large coefficients = transient, small = sustain.
            // Find the max coefficient magnitude for adaptive thresholding.
            float maxCoeff = 0;
            for (auto v : sig) maxCoeff = std::max(maxCoeff, std::abs(v));
            float thresh = threshold * maxCoeff;

            // Build transient-only coefficients (keep above threshold).
            std::vector<float> transSig = sig;
            std::vector<float> susSig = sig;
            for (int i = 0; i < padLen; ++i) {
                if (std::abs(sig[i]) >= thresh) {
                    susSig[i] = 0; // transient coefficient — zero out in sustain
                } else {
                    transSig[i] = 0; // sustain coefficient — zero out in transient
                }
            }

            // Inverse DWT for both components.
            idwt(transSig, actualLevels, filt);
            idwt(susSig, actualLevels, filt);

            // Recombine with gain controls.
            for (int i = 0; i < n; ++i)
                data[i] = transSig[i] * transGain + susSig[i] * susGain;
        }
    }

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
    int blockSize = 512;
};

// ==============================================================================
// WAVELET DENOISER — wavelet shrinkage noise reduction
//
// Uses the standard wavelet shrinkage method (Donoho & Johnstone):
// forward DWT, soft-threshold the detail coefficients, inverse DWT.
// Small coefficients (likely noise) are shrunk toward zero; large
// coefficients (likely signal) are preserved. This removes broadband
// noise while keeping transients sharp — unlike spectral gating which
// can smear transients.
//
// Params:
//   Threshold — noise floor estimate (0..1, fraction of max coefficient)
//   Levels    — DWT decomposition depth (1..8)
//   Mix       — dry/wet blend
// ==============================================================================
class WaveletDenoiserProcessor : public juce::AudioProcessor {
public:
    WaveletDenoiserProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Denoiser"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        const int n = buf.getNumSamples();
        const int ch = std::min(2, buf.getNumChannels());
        if (n == 0 || ch == 0) return;

        float threshold = paramByName(node, "Threshold", 0.1f);
        int   levels    = juce::jlimit(1, 8, (int)paramByName(node, "Levels", 4.0f));
        float mix       = juce::jlimit(0.0f, 1.0f, paramByName(node, "Mix", 1.0f));

        auto filt = getWaveletFilter("sym4");
        int padLen = 1;
        while (padLen < n) padLen *= 2;

        for (int c = 0; c < ch; ++c) {
            float* data = buf.getWritePointer(c);
            std::vector<float> sig(padLen, 0.0f);
            for (int i = 0; i < n; ++i) sig[i] = data[i];
            std::vector<float> dry(data, data + n);

            int actualLevels = dwt(sig, levels, filt);

            // Soft threshold: shrink coefficients toward zero.
            float maxCoeff = 0;
            for (auto v : sig) maxCoeff = std::max(maxCoeff, std::abs(v));
            float thresh = threshold * maxCoeff;
            // Skip the approximation coefficients (lowest band) — only
            // threshold the detail coefficients.
            int approxLen = padLen;
            for (int l = 0; l < actualLevels; ++l) approxLen /= 2;
            for (int i = approxLen; i < padLen; ++i) {
                float v = sig[i];
                if (std::abs(v) < thresh)
                    sig[i] = 0;
                else
                    sig[i] = (v > 0) ? v - thresh : v + thresh; // soft shrinkage
            }

            idwt(sig, actualLevels, filt);

            for (int i = 0; i < n; ++i)
                data[i] = dry[i] * (1.0f - mix) + sig[i] * mix;
        }
    }

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
};

// ==============================================================================
// WAVELET BITCRUSH — quantize wavelet coefficients
//
// Reduces the precision of wavelet coefficients at selected bands,
// producing bit-reduction artifacts that only affect the frequencies
// you choose (unlike traditional bitcrush which hits everything). Low
// bands = crunchy bass; high bands = sizzly highs; all bands = full
// lo-fi character.
//
// Params: Bits (1..16), Band Lo (lowest band to crush), Band Hi,
//         Levels, Mix
// ==============================================================================
class WaveletBitcrushProcessor : public juce::AudioProcessor {
public:
    WaveletBitcrushProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Wavelet Bitcrush"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        const int n = buf.getNumSamples();
        const int ch = std::min(2, buf.getNumChannels());
        if (n == 0 || ch == 0) return;

        int   bits   = juce::jlimit(1, 16, (int)paramByName(node, "Bits", 4.0f));
        int   bandLo = juce::jlimit(0, 7, (int)paramByName(node, "Band Lo", 0.0f));
        int   bandHi = juce::jlimit(0, 7, (int)paramByName(node, "Band Hi", 7.0f));
        int   levels = juce::jlimit(1, 8, (int)paramByName(node, "Levels", 4.0f));
        float mix    = juce::jlimit(0.0f, 1.0f, paramByName(node, "Mix", 1.0f));

        auto filt = getWaveletFilter("db2");
        int padLen = 1;
        while (padLen < n) padLen *= 2;

        float quantStep = 1.0f / (float)(1 << bits);

        for (int c = 0; c < ch; ++c) {
            float* data = buf.getWritePointer(c);
            std::vector<float> sig(padLen, 0.0f);
            for (int i = 0; i < n; ++i) sig[i] = data[i];
            std::vector<float> dry(data, data + n);

            int actualLevels = dwt(sig, levels, filt);

            // Quantize coefficients in the selected band range.
            // Band 0 = coarsest detail (lowest freq), actualLevels-1 = finest.
            int approxLen = padLen;
            for (int l = 0; l < actualLevels; ++l) approxLen /= 2;
            int bandStart = approxLen;
            for (int band = 0; band < actualLevels; ++band) {
                int bandLen = approxLen * (1 << band);
                if (band >= bandLo && band <= bandHi) {
                    for (int i = bandStart; i < bandStart + bandLen && i < padLen; ++i)
                        sig[i] = std::round(sig[i] / quantStep) * quantStep;
                }
                bandStart += bandLen;
            }

            idwt(sig, actualLevels, filt);
            for (int i = 0; i < n; ++i)
                data[i] = dry[i] * (1.0f - mix) + sig[i] * mix;
        }
    }

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
};

// ==============================================================================
// DYADIC OCTAVE SHIFTER — clean octave up/down via wavelet bands
//
// Shifts pitch by exact octaves (1 or 2 up/down) by manipulating
// wavelet decomposition levels. Shifting down = zero-stuff the
// approximation coefficients and reconstruct at double length (then
// resample back). Shifting up = decimate. Since the shift is always
// a power of 2 in the wavelet domain, there's no time-stretching
// artifacts — the transients stay sharp.
//
// For this initial implementation we use a simpler approach: the
// wavelet bands are shifted by reassigning coefficients to different
// levels, then reconstructing. This gives clean octave shifts with
// minimal artifacts.
//
// Params: Shift (-2..+2 octaves), Mix
// ==============================================================================
class OctaveShiftProcessor : public juce::AudioProcessor {
public:
    OctaveShiftProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Octave Shift"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        const int n = buf.getNumSamples();
        const int ch = std::min(2, buf.getNumChannels());
        if (n == 0 || ch == 0) return;

        int shift = juce::jlimit(-2, 2, (int)paramByName(node, "Shift", -1.0f));
        float mix = juce::jlimit(0.0f, 1.0f, paramByName(node, "Mix", 0.5f));
        if (shift == 0) return; // no change

        auto filt = getWaveletFilter("db4");
        int padLen = 1;
        while (padLen < n) padLen *= 2;

        for (int c = 0; c < ch; ++c) {
            float* data = buf.getWritePointer(c);
            std::vector<float> sig(padLen, 0.0f);
            for (int i = 0; i < n; ++i) sig[i] = data[i];
            std::vector<float> dry(data, data + n);

            int levels = 6;
            int actualLevels = dwt(sig, levels, filt);

            // Shift bands: positive shift = move coefficients to higher
            // bands (higher frequency = octave up); negative = lower.
            std::vector<float> shifted(padLen, 0.0f);
            if (shift > 0) {
                // Octave up: copy each band to the next-higher band.
                // The finest detail band wraps / gets dropped; the
                // approximation becomes the new coarsest detail.
                int approxLen = padLen;
                for (int l = 0; l < actualLevels; ++l) approxLen /= 2;
                // Copy approximation as the new lowest detail.
                for (int i = 0; i < approxLen; ++i) shifted[i] = sig[i];
                // Shift detail bands up by `shift` levels.
                int srcStart = approxLen;
                for (int band = 0; band < actualLevels; ++band) {
                    int bandLen = approxLen * (1 << band);
                    int dstBand = band + shift;
                    if (dstBand < actualLevels) {
                        int dstStart = approxLen;
                        for (int b = 0; b < dstBand; ++b) dstStart += approxLen * (1 << b);
                        int dstLen = approxLen * (1 << dstBand);
                        // Simple copy (truncate/extend if sizes differ).
                        int copyLen = std::min(bandLen, dstLen);
                        for (int i = 0; i < copyLen; ++i) shifted[dstStart + i] = sig[srcStart + i];
                    }
                    srcStart += bandLen;
                }
            } else {
                // Octave down: shift bands to lower (coarser) levels.
                int approxLen = padLen;
                for (int l = 0; l < actualLevels; ++l) approxLen /= 2;
                for (int i = 0; i < approxLen; ++i) shifted[i] = sig[i];
                int srcStart = approxLen;
                for (int band = 0; band < actualLevels; ++band) {
                    int bandLen = approxLen * (1 << band);
                    int dstBand = band + shift; // negative shift
                    if (dstBand >= 0) {
                        int dstStart = approxLen;
                        for (int b = 0; b < dstBand; ++b) dstStart += approxLen * (1 << b);
                        int dstLen = approxLen * (1 << dstBand);
                        int copyLen = std::min(bandLen, dstLen);
                        for (int i = 0; i < copyLen; ++i) shifted[dstStart + i] = sig[srcStart + i];
                    }
                    srcStart += bandLen;
                }
            }

            idwt(shifted, actualLevels, filt);
            for (int i = 0; i < n; ++i)
                data[i] = dry[i] * (1.0f - mix) + shifted[i] * mix;
        }
    }

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
};

// ==============================================================================
// WAVELET MULTIBAND COMPRESSOR — per-octave-band dynamics
//
// Decomposes audio into octave-wide bands via DWT, applies independent
// compression to each band's coefficients, then reconstructs. The
// octave bands are natural wavelet decomposition levels — no crossover
// filters needed, so the bands sum perfectly without phase artifacts.
//
// Params:
//   Threshold — dB below peak to start compressing (per band, shared)
//   Ratio     — compression ratio (shared across bands for simplicity)
//   Levels    — number of octave bands (1..6)
//   Low Gain  — post-compression gain for the lowest band (dB)
//   High Gain — post-compression gain for the highest band (dB)
//   Mix
// ==============================================================================
class WaveletMultibandCompProcessor : public juce::AudioProcessor {
public:
    WaveletMultibandCompProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Wavelet MB Comp"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        const int n = buf.getNumSamples();
        const int ch = std::min(2, buf.getNumChannels());
        if (n == 0 || ch == 0) return;

        float threshDb = paramByName(node, "Threshold", -20.0f);
        float ratio    = std::max(1.0f, paramByName(node, "Ratio", 4.0f));
        int   levels   = juce::jlimit(1, 6, (int)paramByName(node, "Levels", 4.0f));
        float loGainDb = paramByName(node, "Low Gain", 0.0f);
        float hiGainDb = paramByName(node, "High Gain", 0.0f);
        float mix      = juce::jlimit(0.0f, 1.0f, paramByName(node, "Mix", 1.0f));

        float threshLin = std::pow(10.0f, threshDb / 20.0f);
        auto filt = getWaveletFilter("db4");
        int padLen = 1;
        while (padLen < n) padLen *= 2;

        for (int c = 0; c < ch; ++c) {
            float* data = buf.getWritePointer(c);
            std::vector<float> sig(padLen, 0.0f);
            for (int i = 0; i < n; ++i) sig[i] = data[i];
            std::vector<float> dry(data, data + n);

            int actualLevels = dwt(sig, levels, filt);

            // Per-band compression: compute peak of each band, apply gain
            // reduction if peak exceeds threshold.
            int approxLen = padLen;
            for (int l = 0; l < actualLevels; ++l) approxLen /= 2;
            int bandStart = approxLen;
            for (int band = 0; band < actualLevels; ++band) {
                int bandLen = approxLen * (1 << band);
                // Find peak in this band.
                float peak = 0;
                for (int i = bandStart; i < bandStart + bandLen && i < padLen; ++i)
                    peak = std::max(peak, std::abs(sig[i]));
                // Compute gain reduction.
                float gain = 1.0f;
                if (peak > threshLin) {
                    float dbOver = 20.0f * std::log10(peak / threshLin);
                    float dbReduction = dbOver * (1.0f - 1.0f / ratio);
                    gain = std::pow(10.0f, -dbReduction / 20.0f);
                }
                // Per-band tilt: interpolate between loGainDb and hiGainDb.
                float bandFrac = (float)band / std::max(1.0f, (float)(actualLevels - 1));
                float tiltDb = loGainDb + (hiGainDb - loGainDb) * bandFrac;
                float tiltGain = std::pow(10.0f, tiltDb / 20.0f);
                gain *= tiltGain;

                for (int i = bandStart; i < bandStart + bandLen && i < padLen; ++i)
                    sig[i] *= gain;
                bandStart += bandLen;
            }

            idwt(sig, actualLevels, filt);
            for (int i = 0; i < n; ++i)
                data[i] = dry[i] * (1.0f - mix) + sig[i] * mix;
        }
    }

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
};

// ==============================================================================
// WAVELET SCALE-SHIFT PITCH SHIFTER
//
// Shifts pitch by modifying the CWT scalogram: shift all scales by a
// factor corresponding to the desired pitch ratio, then reconstruct
// via inverse CWT. Unlike time-domain pitch shifting (which introduces
// time artifacts) or FFT-based shifting (which smears transients), the
// wavelet approach preserves transient sharpness because the wavelet
// basis naturally adapts its window size to frequency content.
//
// Params: Semitones (-24..+24), Mix
// ==============================================================================
class WaveletPitchShiftProcessor : public juce::AudioProcessor {
public:
    WaveletPitchShiftProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Wavelet Pitch"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        const int n = buf.getNumSamples();
        const int ch = std::min(2, buf.getNumChannels());
        if (n == 0 || ch == 0) return;

        float semitones = paramByName(node, "Semitones", 0.0f);
        float mix       = juce::jlimit(0.0f, 1.0f, paramByName(node, "Mix", 1.0f));
        if (std::abs(semitones) < 0.01f) return; // no shift

        float ratio = std::pow(2.0f, semitones / 12.0f);

        for (int c = 0; c < ch; ++c) {
            float* data = buf.getWritePointer(c);
            std::vector<float> sig(data, data + n);
            std::vector<float> dry(data, data + n);

            // Forward CWT.
            auto result = cwt(sig, 2.0f, 64.0f, 24);

            // Shift scales: multiply each scale value by 1/ratio so
            // higher pitch = smaller scales. Rebuild the scalogram with
            // shifted scale assignments.
            CWTResult shifted = result;
            std::fill(shifted.magnitude.begin(), shifted.magnitude.end(), 0.0f);
            std::fill(shifted.phase.begin(), shifted.phase.end(), 0.0f);

            for (int s = 0; s < result.numScales; ++s) {
                // Target scale index after shift.
                float targetScale = result.scales[s] / ratio;
                // Find nearest scale in the grid.
                int bestIdx = 0;
                float bestDist = 1e9f;
                for (int ss = 0; ss < result.numScales; ++ss) {
                    float dist = std::abs(result.scales[ss] - targetScale);
                    if (dist < bestDist) { bestDist = dist; bestIdx = ss; }
                }
                // Copy this scale's coefficients to the target position.
                for (int t = 0; t < result.numSamples; ++t) {
                    shifted.magnitude[bestIdx * n + t] += result.magnitude[s * n + t];
                    shifted.phase[bestIdx * n + t] = result.phase[s * n + t];
                }
            }

            // Inverse CWT.
            auto recon = icwt(shifted);

            for (int i = 0; i < n && i < (int)recon.size(); ++i)
                data[i] = dry[i] * (1.0f - mix) + recon[i] * mix;
        }
    }

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
};

// ==============================================================================
// SELF-SIMILAR / 1/f WAVELET REVERB
//
// Creates a fractal-like reverb tail by scaling wavelet coefficients
// with a 1/f power law across decomposition levels. Coarser levels
// (lower frequencies) get more energy, finer levels (higher freq)
// decay faster — mimicking the natural 1/f spectrum of real acoustic
// spaces. The result is a diffuse, organic-sounding tail that's
// quite different from algorithmic or convolution reverbs.
//
// Params: Decay (overall tail length), Color (1/f exponent: 0=white,
//         1=pink, 2=brown), Levels, Mix
// ==============================================================================
class WaveletReverbProcessor : public juce::AudioProcessor {
public:
    WaveletReverbProcessor(Node& n) : node(n) {
        tailBufL.resize(8192, 0.0f);
        tailBufR.resize(8192, 0.0f);
    }
    const juce::String getName() const override { return "Wavelet Reverb"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        const int n = buf.getNumSamples();
        const int ch = std::min(2, buf.getNumChannels());
        if (n == 0 || ch == 0) return;

        float decay  = juce::jlimit(0.0f, 1.0f, paramByName(node, "Decay", 0.7f));
        float color  = juce::jlimit(0.0f, 3.0f, paramByName(node, "Color", 1.0f));
        int   levels = juce::jlimit(1, 8, (int)paramByName(node, "Levels", 5.0f));
        float mix    = juce::jlimit(0.0f, 1.0f, paramByName(node, "Mix", 0.3f));

        auto filt = getWaveletFilter("db4");
        int tailLen = (int)tailBufL.size();

        for (int c = 0; c < ch; ++c) {
            float* data = buf.getWritePointer(c);
            auto& tail = (c == 0) ? tailBufL : tailBufR;

            // Add new input to the tail buffer (shift + accumulate).
            // Shift existing tail left by n samples and add new input.
            for (int i = 0; i < tailLen - n; ++i)
                tail[i] = tail[i + n] * decay;
            for (int i = 0; i < n && (tailLen - n + i) >= 0; ++i)
                tail[tailLen - n + i] = data[i];

            // DWT the tail buffer.
            std::vector<float> sig = tail;
            int actualLevels = dwt(sig, levels, filt);

            // Apply 1/f weighting: each band's gain = 1 / (band+1)^color
            int approxLen = tailLen;
            for (int l = 0; l < actualLevels; ++l) approxLen /= 2;
            int bandStart = approxLen;
            for (int band = 0; band < actualLevels; ++band) {
                int bandLen = approxLen * (1 << band);
                float weight = 1.0f / std::pow((float)(band + 1), color);
                for (int i = bandStart; i < bandStart + bandLen && i < tailLen; ++i)
                    sig[i] *= weight;
                bandStart += bandLen;
            }

            // IDWT to get the reverb tail.
            idwt(sig, actualLevels, filt);

            // Mix into output.
            for (int i = 0; i < n; ++i)
                data[i] = data[i] * (1.0f - mix) + sig[tailLen - n + i] * mix;
        }
    }

    // Tail: when input stops, the 8192-sample shift-decay tail buffer
    // fully cycles through to zero in bufferLen/sampleRate seconds — that
    // bounds the tail regardless of the Decay param.  ~186 ms at 44.1 kHz.
    double getTailLengthSeconds() const override {
        return (double) tailBufL.size() / std::max(1.0, sampleRate);
    }
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
    std::vector<float> tailBufL, tailBufR;
};

// ==============================================================================
// INDEPENDENT TRANSIENT + TONAL PITCH SHIFTING
//
// Separates audio into transient and tonal components via wavelet
// thresholding, pitch-shifts only the tonal part, then recombines.
// The transients (drum hits, plucks) keep their original pitch and
// timing — only the sustained tonal content gets shifted. This gives
// pitch shifting that preserves drum punch and percussive attacks.
//
// Params: Semitones (-24..+24), Threshold (transient/tonal separation),
//         Trans Gain (0..2, keep or boost transients), Levels, Mix
// ==============================================================================
class IndependentPitchShiftProcessor : public juce::AudioProcessor {
public:
    IndependentPitchShiftProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Ind. Pitch Shift"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        const int n = buf.getNumSamples();
        const int ch = std::min(2, buf.getNumChannels());
        if (n == 0 || ch == 0) return;

        float semitones = paramByName(node, "Semitones", 0.0f);
        float threshold = paramByName(node, "Threshold", 0.3f);
        float transGain = paramByName(node, "Trans Gain", 1.0f);
        int   levels    = juce::jlimit(1, 8, (int)paramByName(node, "Levels", 4.0f));
        float mix       = juce::jlimit(0.0f, 1.0f, paramByName(node, "Mix", 1.0f));

        if (std::abs(semitones) < 0.01f) return;
        float ratio = std::pow(2.0f, semitones / 12.0f);

        auto filt = getWaveletFilter("db4");
        int padLen = 1;
        while (padLen < n) padLen *= 2;

        for (int c = 0; c < ch; ++c) {
            float* data = buf.getWritePointer(c);
            std::vector<float> sig(padLen, 0.0f);
            for (int i = 0; i < n; ++i) sig[i] = data[i];
            std::vector<float> dry(data, data + n);

            int actualLevels = dwt(sig, levels, filt);

            // Separate: threshold-based split into transient + tonal.
            float maxCoeff = 0;
            for (auto v : sig) maxCoeff = std::max(maxCoeff, std::abs(v));
            float thresh = threshold * maxCoeff;

            std::vector<float> transSig(padLen, 0.0f);
            std::vector<float> tonalSig(padLen, 0.0f);
            int approxLen = padLen;
            for (int l = 0; l < actualLevels; ++l) approxLen /= 2;
            // Keep approximation in tonal.
            for (int i = 0; i < approxLen; ++i) tonalSig[i] = sig[i];
            // Split detail bands.
            for (int i = approxLen; i < padLen; ++i) {
                if (std::abs(sig[i]) >= thresh) transSig[i] = sig[i];
                else tonalSig[i] = sig[i];
            }

            // Reconstruct tonal, pitch-shift it via resampling.
            idwt(tonalSig, actualLevels, filt);
            // Simple pitch shift via resampling (linear interp).
            std::vector<float> shifted(n, 0.0f);
            for (int i = 0; i < n; ++i) {
                float srcPos = (float)i * ratio;
                int i0 = (int)srcPos;
                float frac = srcPos - i0;
                if (i0 + 1 < n) shifted[i] = tonalSig[i0] * (1.0f - frac) + tonalSig[i0 + 1] * frac;
                else if (i0 < n) shifted[i] = tonalSig[i0];
            }

            // Reconstruct transients (unshifted).
            idwt(transSig, actualLevels, filt);

            // Recombine.
            for (int i = 0; i < n; ++i)
                data[i] = dry[i] * (1.0f - mix) +
                          (shifted[i] + transSig[i] * transGain) * mix;
        }
    }

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
};

// ==============================================================================
// WAVELET COMPLEXITY KNOB — coefficient sparsification
//
// Smoothly simplifies audio by keeping only the N largest wavelet
// coefficients and zeroing the rest, then reconstructing. At 100%
// complexity the signal is unchanged; at 0% only the single largest
// coefficient survives (a near-silence or pure tone). In between you
// get progressive detail reduction — like an audio "resolution" dial.
//
// Params: Complexity (0..1), Levels, Mix
// ==============================================================================
class WaveletComplexityProcessor : public juce::AudioProcessor {
public:
    WaveletComplexityProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Complexity"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        const int n = buf.getNumSamples();
        const int ch = std::min(2, buf.getNumChannels());
        if (n == 0 || ch == 0) return;

        float complexity = juce::jlimit(0.0f, 1.0f, paramByName(node, "Complexity", 0.5f));
        int   levels     = juce::jlimit(1, 8, (int)paramByName(node, "Levels", 4.0f));
        float mix        = juce::jlimit(0.0f, 1.0f, paramByName(node, "Mix", 1.0f));

        auto filt = getWaveletFilter("db4");
        int padLen = 1;
        while (padLen < n) padLen *= 2;

        for (int c = 0; c < ch; ++c) {
            float* data = buf.getWritePointer(c);
            std::vector<float> sig(padLen, 0.0f);
            for (int i = 0; i < n; ++i) sig[i] = data[i];
            std::vector<float> dry(data, data + n);

            int actualLevels = dwt(sig, levels, filt);

            // Sort coefficients by magnitude and zero out the smallest.
            int keep = std::max(1, (int)(padLen * complexity));
            std::vector<std::pair<float, int>> coeffs(padLen);
            for (int i = 0; i < padLen; ++i)
                coeffs[i] = {std::abs(sig[i]), i};
            std::partial_sort(coeffs.begin(), coeffs.begin() + keep, coeffs.end(),
                [](auto& a, auto& b) { return a.first > b.first; });
            // Zero everything not in the top-keep set.
            std::vector<bool> kept(padLen, false);
            for (int i = 0; i < keep; ++i) kept[coeffs[i].second] = true;
            for (int i = 0; i < padLen; ++i)
                if (!kept[i]) sig[i] = 0;

            idwt(sig, actualLevels, filt);
            for (int i = 0; i < n; ++i)
                data[i] = dry[i] * (1.0f - mix) + sig[i] * mix;
        }
    }

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
};

// ==============================================================================
// ADDITIVE BANK SYNTH — per-partial sine oscillator synthesis
//
// Sums N harmonic sine oscillators per voice. Each partial runs at
// `fundamental × ratio_n` where ratio_n defaults to the harmonic
// series (1, 2, 3, ...) but can be shifted toward inharmonic spacing
// via the Stretch param. Amplitude per partial follows a 1/n^rolloff
// law controlled by Brightness — 0 = all partials equal (organ),
// 1 = natural rolloff (strings), 2+ = mellow (flute-like).
//
// Params: Partials (1..64), Stretch (harmonic→inharmonic, 0..2),
//         Brightness (amplitude rolloff exponent, 0..3),
//         Attack, Decay, Sustain, Release, Volume
// ==============================================================================
class AdditiveSynthProcessor : public juce::AudioProcessor {
public:
    AdditiveSynthProcessor(Node& n) : node(n) { voices.resize(12); }
    const juce::String getName() const override { return "Additive"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override {
        applySignalModulations(node, buf);
        buf.clear();
        const int numSamples = buf.getNumSamples();
        if (numSamples == 0) return;

        int   numPartials = juce::jlimit(1, 64, (int)paramByName(node, "Partials", 16.0f));
        float stretch     = paramByName(node, "Stretch", 0.0f);
        float brightness  = std::max(0.0f, paramByName(node, "Brightness", 1.0f));
        // Inharmonic presets (#8): override stretch/brightness for
        // common timbres. 0=Custom (use manual values).
        int preset = (int)paramByName(node, "Preset", 0.0f);
        switch (preset) {
            case 1: stretch = 0.12f; brightness = 0.8f; break; // Bell (slightly inharmonic)
            case 2: stretch = 0.25f; brightness = 2.0f; break; // Drum (inharmonic + mellow)
            case 3: stretch = 0.01f; brightness = 0.5f; break; // Stretched Piano (subtle)
            case 4: stretch = 0.0f;  brightness = 0.0f; break; // Organ (all equal)
            default: break; // Custom — use manual Stretch/Brightness
        }
        float aA = std::max(0.001f, paramByName(node, "Attack", 0.01f));
        float aD = std::max(0.001f, paramByName(node, "Decay", 0.1f));
        float aS = paramByName(node, "Sustain", 0.7f);
        float aR = std::max(0.001f, paramByName(node, "Release", 0.3f));
        float volume = paramByName(node, "Volume", 0.5f);

        for (auto meta : midi) {
            auto msg = meta.getMessage();
            if (msg.isNoteOn()) {
                auto& v = allocVoice();
                v.active = true; v.held = true;
                v.note = msg.getNoteNumber();
                v.vel = msg.getVelocity() / 127.0f;
                v.time = 0; v.relTime = 0;
                v.phases.assign(64, 0.0);
            } else if (msg.isNoteOff()) {
                for (auto& v : voices)
                    if (v.active && v.held && v.note == msg.getNoteNumber())
                        { v.held = false; v.relTime = v.time; }
            }
        }

        const float kPi2 = 6.28318530718f;
        float dt = 1.0f / (float)sampleRate;

        for (int s = 0; s < numSamples; ++s) {
            float out = 0;
            for (auto& v : voices) {
                if (!v.active) continue;
                float baseFreq = 440.0f * std::pow(2.0f, (v.note - 69 + v.mpe.pitchBend) / 12.0f);

                // ADSR
                float env;
                if (v.held) {
                    if (v.time < aA) env = v.time / aA;
                    else if (v.time < aA + aD) env = 1.0f + (aS - 1.0f) * ((v.time - aA) / aD);
                    else env = aS;
                } else {
                    float rt = v.time - v.relTime;
                    env = aS * std::max(0.0f, 1.0f - rt / aR);
                    if (rt >= aR) { v.active = false; continue; }
                }

                // Sum partials
                float voiceOut = 0;
                for (int p = 0; p < numPartials; ++p) {
                    // Ratio: harmonic = (p+1), stretched = (p+1)^(1+stretch)
                    float ratio = std::pow((float)(p + 1), 1.0f + stretch);
                    float freq = baseFreq * ratio;
                    if (freq > sampleRate * 0.49) break; // anti-alias

                    // Amplitude rolloff
                    float amp = 1.0f / std::pow((float)(p + 1), brightness);
                    voiceOut += std::sin((float)v.phases[p] * kPi2) * amp;
                    v.phases[p] += freq / sampleRate;
                    if (v.phases[p] > 1.0) v.phases[p] -= 1.0;
                }

                out += voiceOut * env * v.vel;
                v.time += dt;
            }

            // Normalize by sqrt of partial count
            if (numPartials > 1)
                out /= std::sqrt((float)numPartials);
            out *= volume;
            out = juce::jlimit(-1.0f, 1.0f, out);
            for (int c = 0; c < buf.getNumChannels(); ++c)
                buf.addSample(c, s, out);
        }
    }

    // Tail = the Release param (additive voice envelope dies at
    // time = releaseTime after note-off).
    double getTailLengthSeconds() const override {
        return (double) std::max(0.001f, paramByName(node, "Release", 0.3f));
    }
    bool acceptsMidi() const override { return true; }
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
    Node& node;
    double sampleRate = 44100;
    struct Voice {
        bool active = false, held = false;
        int note = 0;
        float vel = 0, time = 0, relTime = 0;
        std::vector<double> phases;
        int mpeChannel = 1;
        MpeVoiceState mpe;
    };
    std::vector<Voice> voices;
    Voice& allocVoice() {
        for (auto& v : voices) if (!v.active) return v;
        float oldest = -1; int idx = 0;
        for (int i = 0; i < (int)voices.size(); ++i)
            if (voices[i].time > oldest) { oldest = voices[i].time; idx = i; }
        return voices[idx];
    }
};

// ==============================================================================
// ASYMMETRIC WAVELET FILTER — non-causal / look-ahead filtering
//
// Standard filters respond AFTER a transient happens. This wavelet-based
// filter can "anticipate" transients by processing the wavelet domain
// with different gains for pre-transient vs post-transient regions.
// The effect: a filter that reacts before the attack hits, creating
// impossible-sounding dynamics that no causal filter can produce.
//
// Uses the transient detection from #53 to find onset positions, then
// applies asymmetric gain curves around each onset in the wavelet domain.
//
// Params: Pre-Attack (ms, how far ahead to start), Post-Decay (ms),
//         Pre Gain, Post Gain, Levels, Mix
// ==============================================================================
class AsymmetricFilterProcessor : public juce::AudioProcessor {
public:
    AsymmetricFilterProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Asymmetric Filter"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        const int n = buf.getNumSamples();
        const int ch = std::min(2, buf.getNumChannels());
        if (n == 0 || ch == 0) return;

        float preMs    = paramByName(node, "Pre-Attack", 20.0f);
        float postMs   = paramByName(node, "Post-Decay", 50.0f);
        float preGain  = paramByName(node, "Pre Gain", 2.0f);
        float postGain = paramByName(node, "Post Gain", 0.5f);
        int   levels   = juce::jlimit(1, 8, (int)paramByName(node, "Levels", 4.0f));
        float mix      = juce::jlimit(0.0f, 1.0f, paramByName(node, "Mix", 1.0f));

        int preSamples  = (int)(preMs * 0.001 * sampleRate);
        int postSamples = (int)(postMs * 0.001 * sampleRate);

        auto filt = getWaveletFilter("db4");
        int padLen = 1;
        while (padLen < n) padLen *= 2;

        for (int c = 0; c < ch; ++c) {
            float* data = buf.getWritePointer(c);
            std::vector<float> sig(padLen, 0.0f);
            for (int i = 0; i < n; ++i) sig[i] = data[i];
            std::vector<float> dry(data, data + n);

            int actualLevels = dwt(sig, levels, filt);

            // Detect transients: find peaks in the finest detail level.
            int approxLen = padLen;
            for (int l = 0; l < actualLevels; ++l) approxLen /= 2;
            // Finest detail band is the last one (highest indices).
            int finestStart = padLen / 2;
            int finestLen = padLen / 2;

            // Find transient positions (peaks in finest detail).
            std::vector<int> transients;
            float maxFine = 0;
            for (int i = finestStart; i < finestStart + finestLen; ++i)
                maxFine = std::max(maxFine, std::abs(sig[i]));
            float transThresh = maxFine * 0.5f;
            for (int i = finestStart; i < finestStart + finestLen; ++i)
                if (std::abs(sig[i]) > transThresh) transients.push_back(i - finestStart);

            // Build a gain envelope that's asymmetric around each transient.
            std::vector<float> gainEnv(padLen, 1.0f);
            for (int t : transients) {
                // Pre-attack region: ramp up preGain before the transient
                for (int i = std::max(0, t - preSamples); i < t; ++i) {
                    float frac = (float)(i - (t - preSamples)) / std::max(1, preSamples);
                    gainEnv[i] *= 1.0f + (preGain - 1.0f) * frac;
                }
                // Post-decay region: apply postGain after the transient
                for (int i = t; i < std::min(padLen, t + postSamples); ++i) {
                    float frac = 1.0f - (float)(i - t) / std::max(1, postSamples);
                    gainEnv[i] *= postGain + (1.0f - postGain) * (1.0f - frac);
                }
            }

            // Apply gain envelope to all coefficients.
            for (int i = 0; i < padLen; ++i)
                sig[i] *= gainEnv[i];

            idwt(sig, actualLevels, filt);
            for (int i = 0; i < n; ++i)
                data[i] = dry[i] * (1.0f - mix) + sig[i] * mix;
        }
    }

    // Tail = 0.  Processing is per-block: transient detection and the
    // pre/post gain envelopes only act on the current block's content,
    // so once input stops there's nothing left to ring out.
    double getTailLengthSeconds() const override { return 0.0; }
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
};

// ==============================================================================
// ADAPTIVE RESOLUTION WAVELET PITCH TRACKER
//
// Tracks the fundamental pitch of an incoming audio signal using
// wavelet analysis. Outputs the detected pitch as a Signal value
// (in Hz, normalized to 0..1 over a configurable range). Can be wired
// into any param for pitch-following effects (auto-tune, pitch-to-
// filter-cutoff, etc.).
//
// Uses the DWT to identify the dominant period: the coarsest wavelet
// level with the strongest energy determines the approximate pitch
// range, then the coefficient pattern within that level refines the
// estimate. Handles vibrato and pitch bends gracefully because the
// wavelet's time-frequency resolution adapts to the input.
//
// Params: Min Hz, Max Hz
// Output: Signal out = detected frequency normalized to [0..1]
//         where 0 = Min Hz, 1 = Max Hz. Also stored in a node param
//         "Detected Hz" for UI display.
// ==============================================================================
class WaveletPitchTrackerProcessor : public juce::AudioProcessor {
public:
    WaveletPitchTrackerProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Pitch Tracker"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        const int n = buf.getNumSamples();
        if (n == 0 || buf.getNumChannels() == 0) return;

        float minHz = std::max(20.0f, paramByName(node, "Min Hz", 50.0f));
        float maxHz = std::min(5000.0f, paramByName(node, "Max Hz", 2000.0f));

        // Read mono input.
        const float* input = buf.getReadPointer(0);
        std::vector<float> sig(input, input + n);

        // Pad to power of 2.
        int padLen = 1;
        while (padLen < n) padLen *= 2;
        sig.resize(padLen, 0.0f);

        auto filt = getWaveletFilter("db4");
        int levels = std::min(8, (int)(std::log2(padLen) - 2));
        int actualLevels = dwt(sig, levels, filt);

        // Find the dominant wavelet level: the one with the highest energy.
        int approxLen = padLen;
        for (int l = 0; l < actualLevels; ++l) approxLen /= 2;

        int bestBand = 0;
        float bestEnergy = 0;
        int bandStart = approxLen;
        for (int band = 0; band < actualLevels; ++band) {
            int bandLen = approxLen * (1 << band);
            float energy = 0;
            for (int i = bandStart; i < bandStart + bandLen && i < padLen; ++i)
                energy += sig[i] * sig[i];
            energy /= bandLen; // normalize by band size
            if (energy > bestEnergy) { bestEnergy = energy; bestBand = band; }
            bandStart += bandLen;
        }

        // Estimate frequency from the dominant band.
        // Band k covers frequencies around sampleRate / 2^(k+1).
        // The center frequency of band k at this sample rate:
        float bandCenterHz = (float)(sampleRate / std::pow(2.0, bestBand + 2));
        float detectedHz = juce::jlimit(minHz, maxHz, bandCenterHz);

        // Write detected Hz to a node param for display.
        for (auto& p : node.params)
            if (p.name == "Detected Hz") { p.value = detectedHz; break; }

        // Output normalized signal (0 = minHz, 1 = maxHz) on channel 0.
        float normalized = (detectedHz - minHz) / std::max(1.0f, maxHz - minHz);
        buf.clear();
        if (buf.getNumChannels() >= 1) {
            auto* out = buf.getWritePointer(0);
            for (int i = 0; i < n; ++i) out[i] = normalized;
        }
    }

    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return true; }
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
    Node& node;
    double sampleRate = 44100;
};

// ==============================================================================
// WAVELET-BAND VOCODER
//
// Classic vocoder effect using wavelet octave bands instead of fixed
// FFT bins. Two inputs: carrier (typically a synth pad) and modulator
// (typically a voice). The modulator's per-band envelope shapes the
// carrier's per-band amplitude, producing the "talking synth" effect.
//
// Since the node graph sums all audio inputs into channels 0/1, we
// use channel 2 (a Signal input pin) for the modulator signal. The
// carrier comes in on channels 0/1 as the normal audio input.
//
// Params: Bands (decomposition levels), Mix
// ==============================================================================
class WaveletVocoderProcessor : public juce::AudioProcessor {
public:
    WaveletVocoderProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Wavelet Vocoder"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        const int n = buf.getNumSamples();
        if (n == 0 || buf.getNumChannels() < 1) return;

        int bands = juce::jlimit(1, 8, (int)paramByName(node, "Bands", 5.0f));
        float mix = juce::jlimit(0.0f, 1.0f, paramByName(node, "Mix", 1.0f));

        // Carrier = channel 0 (main audio in).
        // Modulator = channel 2 (Signal input, if connected).
        bool hasModulator = buf.getNumChannels() > 2;
        if (!hasModulator) return; // no modulator → passthrough

        auto filt = getWaveletFilter("db4");
        int padLen = 1;
        while (padLen < n) padLen *= 2;

        float* carrierData = buf.getWritePointer(0);
        const float* modData = buf.getReadPointer(2);
        std::vector<float> dry(carrierData, carrierData + n);

        // Pad and DWT both signals.
        std::vector<float> carrier(padLen, 0.0f), modulator(padLen, 0.0f);
        for (int i = 0; i < n; ++i) { carrier[i] = carrierData[i]; modulator[i] = modData[i]; }

        int actualLevels = dwt(carrier, bands, filt);
        dwt(modulator, bands, filt);

        // For each band: compute the modulator's energy, compute the
        // carrier's energy, scale carrier by modulator/carrier ratio.
        int approxLen = padLen;
        for (int l = 0; l < actualLevels; ++l) approxLen /= 2;
        // Scale approximation too.
        {
            float modE = 0, carE = 0;
            for (int i = 0; i < approxLen; ++i) {
                modE += modulator[i] * modulator[i];
                carE += carrier[i] * carrier[i];
            }
            float scale = (carE > 1e-9f) ? std::sqrt(modE / carE) : 0.0f;
            for (int i = 0; i < approxLen; ++i) carrier[i] *= scale;
        }
        int bandStart = approxLen;
        for (int band = 0; band < actualLevels; ++band) {
            int bandLen = approxLen * (1 << band);
            float modE = 0, carE = 0;
            for (int i = bandStart; i < bandStart + bandLen && i < padLen; ++i) {
                modE += modulator[i] * modulator[i];
                carE += carrier[i] * carrier[i];
            }
            float scale = (carE > 1e-9f) ? std::sqrt(modE / carE) : 0.0f;
            scale = std::min(scale, 10.0f); // prevent blowup
            for (int i = bandStart; i < bandStart + bandLen && i < padLen; ++i)
                carrier[i] *= scale;
            bandStart += bandLen;
        }

        idwt(carrier, actualLevels, filt);

        for (int i = 0; i < n; ++i)
            carrierData[i] = dry[i] * (1.0f - mix) + carrier[i] * mix;
        // Copy to right channel if stereo.
        if (buf.getNumChannels() >= 2)
            std::memcpy(buf.getWritePointer(1), carrierData, n * sizeof(float));
    }

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
};

// ==============================================================================
// FORMANT-PRESERVING PITCH SHIFT — wavelet packet approach
//
// Pitch-shifts audio while preserving formants (the resonant
// frequencies that make a voice sound like THAT voice, not a chipmunk).
// Method: decompose into wavelet bands, pitch-shift each band via
// resampling, but keep the spectral envelope (formant shape) by
// scaling band gains back to their original levels after the shift.
//
// Params: Semitones (-24..+24), Formant Lock (0..1, how much formant
//         to preserve — 0=no preservation, 1=full), Levels, Mix
// ==============================================================================
class FormantPitchShiftProcessor : public juce::AudioProcessor {
public:
    FormantPitchShiftProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "Formant Pitch"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        const int n = buf.getNumSamples();
        const int ch = std::min(2, buf.getNumChannels());
        if (n == 0 || ch == 0) return;

        float semitones  = paramByName(node, "Semitones", 0.0f);
        float formantLock = juce::jlimit(0.0f, 1.0f, paramByName(node, "Formant Lock", 0.8f));
        int   levels     = juce::jlimit(1, 8, (int)paramByName(node, "Levels", 5.0f));
        float mix        = juce::jlimit(0.0f, 1.0f, paramByName(node, "Mix", 1.0f));

        if (std::abs(semitones) < 0.01f) return;
        float ratio = std::pow(2.0f, semitones / 12.0f);

        auto filt = getWaveletFilter("db4");
        int padLen = 1;
        while (padLen < n) padLen *= 2;

        for (int c = 0; c < ch; ++c) {
            float* data = buf.getWritePointer(c);
            std::vector<float> sig(padLen, 0.0f);
            for (int i = 0; i < n; ++i) sig[i] = data[i];
            std::vector<float> dry(data, data + n);

            // 1. Measure per-band energy before shift (= formant envelope).
            int actualLevels = dwt(sig, levels, filt);
            int approxLen = padLen;
            for (int l = 0; l < actualLevels; ++l) approxLen /= 2;

            std::vector<float> origEnergy(actualLevels + 1);
            float aE = 0;
            for (int i = 0; i < approxLen; ++i) aE += sig[i] * sig[i];
            origEnergy[0] = aE / std::max(1, approxLen);
            int bandStart = approxLen;
            for (int band = 0; band < actualLevels; ++band) {
                int bandLen = approxLen * (1 << band);
                float e = 0;
                for (int i = bandStart; i < bandStart + bandLen && i < padLen; ++i)
                    e += sig[i] * sig[i];
                origEnergy[band + 1] = e / std::max(1, bandLen);
                bandStart += bandLen;
            }

            // 2. Reconstruct and pitch-shift via resampling.
            idwt(sig, actualLevels, filt);
            std::vector<float> shifted(n, 0.0f);
            for (int i = 0; i < n; ++i) {
                float srcPos = (float)i * ratio;
                int i0 = (int)srcPos;
                float frac = srcPos - i0;
                if (i0 + 1 < n) shifted[i] = sig[i0] * (1.0f - frac) + sig[i0 + 1] * frac;
                else if (i0 < n) shifted[i] = sig[i0];
            }

            // 3. Re-decompose the shifted signal and adjust band gains
            //    to match the original formant envelope.
            std::vector<float> shiftPad(padLen, 0.0f);
            for (int i = 0; i < n; ++i) shiftPad[i] = shifted[i];
            dwt(shiftPad, levels, filt);

            // Scale each band to match original energy (= formant restoration).
            {
                float shiftE = 0;
                for (int i = 0; i < approxLen; ++i) shiftE += shiftPad[i] * shiftPad[i];
                shiftE /= std::max(1, approxLen);
                float scale = (shiftE > 1e-12f) ? std::sqrt(origEnergy[0] / shiftE) : 1.0f;
                scale = 1.0f + (scale - 1.0f) * formantLock;
                for (int i = 0; i < approxLen; ++i) shiftPad[i] *= scale;
            }
            bandStart = approxLen;
            for (int band = 0; band < actualLevels; ++band) {
                int bandLen = approxLen * (1 << band);
                float shiftE = 0;
                for (int i = bandStart; i < bandStart + bandLen && i < padLen; ++i)
                    shiftE += shiftPad[i] * shiftPad[i];
                shiftE /= std::max(1, bandLen);
                float scale = (shiftE > 1e-12f) ? std::sqrt(origEnergy[band + 1] / shiftE) : 1.0f;
                scale = std::min(scale, 10.0f);
                scale = 1.0f + (scale - 1.0f) * formantLock;
                for (int i = bandStart; i < bandStart + bandLen && i < padLen; ++i)
                    shiftPad[i] *= scale;
                bandStart += bandLen;
            }

            idwt(shiftPad, actualLevels, filt);

            for (int i = 0; i < n; ++i)
                data[i] = dry[i] * (1.0f - mix) + shiftPad[i] * mix;
        }
    }

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
};

// ==============================================================================
// SPECTRAL GRAIN SYNTH — Mode C: IFFT-to-windowed-grain playback
//
// Defines a spectrum via magnitude expression (same as the wavetable
// spectral mode), but instead of playing a single IFFT cycle, it
// generates a bank of short grains (each an IFFT with different random
// phases) and overlap-adds them at a controllable rate. This produces
// evolving, shimmering textures from a static spectral definition.
//
// Params: Partials (FFT size / 2), Density (grains/sec), Grain Size (ms),
//         Spread (phase randomness 0..1), Attack, Decay, Sustain, Release,
//         Volume. The magnitude expression is in node.script after the
//         "__spectralgrain__:" prefix.
// ==============================================================================
class SpectralGrainProcessor : public juce::AudioProcessor {
public:
    SpectralGrainProcessor(Node& n) : node(n) { voices.resize(8); }
    const juce::String getName() const override { return "Spectral Grain"; }

    void prepareToPlay(double sr, int) override {
        sampleRate = sr;
        regenerateGrains();
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override {
        applySignalModulations(node, buf);
        buf.clear();
        const int numSamples = buf.getNumSamples();
        if (numSamples == 0 || grainBank.empty()) return;

        float density   = std::max(1.0f, paramByName(node, "Density", 20.0f));
        float grainMs   = std::max(1.0f, paramByName(node, "Grain Size", 40.0f));
        float aA = std::max(0.001f, paramByName(node, "Attack", 0.01f));
        float aD = std::max(0.001f, paramByName(node, "Decay", 0.1f));
        float aS = paramByName(node, "Sustain", 0.7f);
        float aR = std::max(0.001f, paramByName(node, "Release", 0.3f));
        float volume = paramByName(node, "Volume", 0.5f);

        int grainSizeSamples = (int)(grainMs * 0.001f * (float)sampleRate);
        grainSizeSamples = std::min(grainSizeSamples, (int)grainBank[0].size());
        float spawnInterval = 1.0f / density;
        float dt = 1.0f / (float)sampleRate;

        for (auto meta : midi) {
            auto msg = meta.getMessage();
            if (msg.isNoteOn()) {
                auto& v = allocVoice();
                v.active = true; v.held = true;
                v.note = msg.getNoteNumber();
                v.vel = msg.getVelocity() / 127.0f;
                v.time = 0; v.relTime = 0;
                v.spawnTimer = 0;
            } else if (msg.isNoteOff()) {
                for (auto& v : voices)
                    if (v.active && v.held && v.note == msg.getNoteNumber())
                        { v.held = false; v.relTime = v.time; }
            }
        }

        for (int s = 0; s < numSamples; ++s) {
            float out = 0;
            for (auto& v : voices) {
                if (!v.active) continue;

                // ADSR
                float env;
                if (v.held) {
                    if (v.time < aA) env = v.time / aA;
                    else if (v.time < aA + aD) env = 1.0f + (aS - 1.0f) * ((v.time - aA) / aD);
                    else env = aS;
                } else {
                    float rt = v.time - v.relTime;
                    env = aS * std::max(0.0f, 1.0f - rt / aR);
                    if (rt >= aR) { v.active = false; continue; }
                }

                // Spawn grains
                v.spawnTimer += dt;
                while (v.spawnTimer >= spawnInterval && v.held) {
                    v.spawnTimer -= spawnInterval;
                    ActiveGrain g;
                    g.grainIdx = rng() % grainBank.size();
                    g.pos = 0;
                    g.len = grainSizeSamples;
                    float baseFreq = 440.0f * std::pow(2.0f, (v.note - 69) / 12.0f);
                    g.rate = baseFreq / 440.0f; // pitch ratio relative to A4
                    activeGrains.push_back(g);
                }

                v.time += dt;

                // Sum active grains for this voice
                float voiceOut = 0;
                for (auto it = activeGrains.begin(); it != activeGrains.end();) {
                    auto& g = *it;
                    int idx = (int)g.pos;
                    if (idx >= g.len || idx >= (int)grainBank[g.grainIdx].size()) {
                        it = activeGrains.erase(it);
                        continue;
                    }
                    // Hann window
                    float w = 0.5f * (1.0f - std::cos(6.28318f * g.pos / g.len));
                    voiceOut += grainBank[g.grainIdx][idx] * w;
                    g.pos += g.rate;
                    ++it;
                }
                out += voiceOut * env * v.vel;
            }

            if (activeGrains.size() > 1)
                out /= std::sqrt((float)activeGrains.size());
            out *= volume;
            out = juce::jlimit(-1.0f, 1.0f, out);
            for (int c = 0; c < buf.getNumChannels(); ++c)
                buf.addSample(c, s, out);
        }

        if (activeGrains.size() > 2048)
            activeGrains.erase(activeGrains.begin(), activeGrains.begin() + 1024);
    }

    // Tail = ADSR release + the longest grain still ringing out.
    // After release, voices stop spawning grains and the envelope dies at
    // aR seconds; any in-flight grain plays for at most grainSize ms more.
    double getTailLengthSeconds() const override {
        double aR     = (double) std::max(0.001f, paramByName(node, "Release",   0.3f));
        double grainS = (double) std::max(1.0f,   paramByName(node, "Grain Size", 40.0f)) * 0.001;
        return aR + grainS;
    }
    bool acceptsMidi() const override { return true; }
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
    Node& node;
    double sampleRate = 44100;

    // Pre-generated grain bank: N short waveforms, each an IFFT of the
    // same magnitude spectrum with different random phases.
    std::vector<std::vector<float>> grainBank;
    static constexpr int kNumGrains = 16;
    static constexpr int kGrainFFTSize = 1024;

    void regenerateGrains() {
        grainBank.clear();
        grainBank.resize(kNumGrains);

        // Parse magnitude expression from script.
        std::string magExpr = "exp(-f/10)"; // default
        auto& script = node.script;
        if (script.rfind("__spectralgrain__:", 0) == 0) {
            magExpr = script.substr(18);
            if (magExpr.empty()) magExpr = "exp(-f/10)";
        }

        int halfBins = kGrainFFTSize / 2 + 1;
        auto mags = WaveExprParser::evaluateOverBins(magExpr, halfBins);

        std::mt19937 rngLocal(42);
        std::uniform_real_distribution<float> phaseDist(-3.14159f, 3.14159f);

        FFT fft(kGrainFFTSize);
        for (int g = 0; g < kNumGrains; ++g) {
            // Random phases for each grain.
            std::vector<std::complex<float>> spectrum(halfBins);
            for (int k = 0; k < halfBins; ++k) {
                float ph = phaseDist(rngLocal);
                spectrum[k] = std::complex<float>(mags[k] * std::cos(ph),
                                                   mags[k] * std::sin(ph));
            }
            spectrum[0] = 0; // no DC
            fft.inverseReal(spectrum, grainBank[g]);
            // Normalize
            float peak = 0;
            for (float v : grainBank[g]) peak = std::max(peak, std::abs(v));
            if (peak > 1e-6f) for (float& v : grainBank[g]) v /= peak;
        }
    }

    struct Voice {
        bool active = false, held = false;
        int note = 0;
        float vel = 0, time = 0, relTime = 0;
        float spawnTimer = 0;
    };
    std::vector<Voice> voices;
    Voice& allocVoice() {
        for (auto& v : voices) if (!v.active) return v;
        float oldest = -1; int idx = 0;
        for (int i = 0; i < (int)voices.size(); ++i)
            if (voices[i].time > oldest) { oldest = voices[i].time; idx = i; }
        return voices[idx];
    }

    struct ActiveGrain {
        int grainIdx = 0;
        float pos = 0;
        int len = 0;
        float rate = 1.0f;
    };
    std::vector<ActiveGrain> activeGrains;
    std::mt19937 rng{1234};
};

// ==============================================================================
// SPECTRAL MODELING SYNTHESIS (SMS) — deterministic/stochastic split
//
// Decomposes audio into two components:
//   - Deterministic (harmonic): detected spectral peaks, resynthesized
//     as sine oscillators. The "clean" tonal part.
//   - Stochastic (noise): the residual after subtracting the harmonic
//     part. Represents breath, bow noise, consonants, etc.
//
// Each component gets its own gain control so you can independently
// boost or cut the tonal vs noisy parts of any sound.
//
// Uses short-time FFT: window the input, FFT, find peaks above a
// threshold (= deterministic), inverse-FFT only the peaks to get the
// harmonic part, subtract from the original to get the residual.
//
// Params: Threshold (peak detection, 0..1), Harmonic Gain, Noise Gain,
//         FFT Size (as a power-of-2 exponent), Mix
// ==============================================================================
class SMSProcessor : public juce::AudioProcessor {
public:
    SMSProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return "SMS"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        applySignalModulations(node, buf);
        const int n = buf.getNumSamples();
        const int ch = std::min(2, buf.getNumChannels());
        if (n == 0 || ch == 0) return;

        float threshold   = juce::jlimit(0.0f, 1.0f, paramByName(node, "Threshold", 0.1f));
        float harmonicGain = paramByName(node, "Harmonic Gain", 1.0f);
        float noiseGain    = paramByName(node, "Noise Gain", 1.0f);
        int   fftExp       = juce::jlimit(8, 12, (int)paramByName(node, "FFT Size", 10.0f));
        float mix          = juce::jlimit(0.0f, 1.0f, paramByName(node, "Mix", 1.0f));

        int fftSize = 1 << fftExp;
        if (fftSize > n) fftSize = n; // can't exceed block size
        // Round down to power of 2 that fits.
        while (fftSize > n) fftSize /= 2;
        if (fftSize < 4) return;

        int halfBins = fftSize / 2 + 1;
        FFT fft(fftSize);

        for (int c = 0; c < ch; ++c) {
            float* data = buf.getWritePointer(c);
            std::vector<float> dry(data, data + n);

            // Process in overlapping frames (hop = fftSize/2).
            std::vector<float> output(n, 0.0f);
            std::vector<float> window(fftSize);
            for (int i = 0; i < fftSize; ++i)
                window[i] = 0.5f * (1.0f - std::cos(6.28318f * i / fftSize)); // Hann

            int hop = fftSize / 2;
            for (int frame = 0; frame + fftSize <= n; frame += hop) {
                // Window the input.
                std::vector<float> windowed(fftSize);
                for (int i = 0; i < fftSize; ++i)
                    windowed[i] = data[frame + i] * window[i];

                // Forward FFT.
                std::vector<std::complex<float>> spectrum;
                fft.forwardReal(windowed, spectrum);

                // Find magnitude peaks.
                float maxMag = 0;
                std::vector<float> mags(halfBins);
                for (int k = 0; k < halfBins; ++k) {
                    mags[k] = std::abs(spectrum[k]);
                    maxMag = std::max(maxMag, mags[k]);
                }
                float thresh = threshold * maxMag;

                // Separate: peaks above threshold = deterministic.
                std::vector<std::complex<float>> harmSpectrum(halfBins, {0,0});
                for (int k = 0; k < halfBins; ++k) {
                    if (mags[k] >= thresh)
                        harmSpectrum[k] = spectrum[k];
                }

                // IFFT harmonic part.
                std::vector<float> harmonic;
                fft.inverseReal(harmSpectrum, harmonic);

                // Residual = original windowed - harmonic.
                // Overlap-add both parts with gains.
                for (int i = 0; i < fftSize && (frame + i) < n; ++i) {
                    float h = harmonic[i] * harmonicGain;
                    float r = (windowed[i] - harmonic[i]) * noiseGain;
                    output[frame + i] += (h + r) * window[i]; // re-window for OLA
                }
            }

            // Normalize OLA (Hann + 50% overlap = constant 1.0 after normalization).
            for (int i = 0; i < n; ++i)
                data[i] = dry[i] * (1.0f - mix) + output[i] * mix;
        }
    }

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
};

} // namespace SoundShop

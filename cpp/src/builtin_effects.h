#pragma once
#include "node_graph.h"
#include "signal_modulation.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <vector>
#include <random>

namespace SoundShop {

// Helper: read a named param from the node, return def if not found.
inline float paramByName(const Node& node, const char* name, float def) {
    for (auto& p : node.params)
        if (p.name == name) return p.value;
    return def;
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
    double getTailLengthSeconds() const override { return 0.05; }
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
    double getTailLengthSeconds() const override { return 0.1; }
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
    double getTailLengthSeconds() const override { return 5.0; }
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

    double getTailLengthSeconds() const override { return 6.0; }
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
                float baseFreq = 440.0f * std::pow(2.0f, (v.note - 69) / 12.0f);
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

    double getTailLengthSeconds() const override { return 5.0; }
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
        float fb1 = 0, fb2 = 0; // op4 feedback history
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

} // namespace SoundShop

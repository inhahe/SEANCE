#pragma once
#include "node_graph.h"
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
        float threshold = paramByName(node, "Threshold", -20.0f); // dB
        float ratio     = paramByName(node, "Ratio", 4.0f);
        float attackMs  = paramByName(node, "Attack", 10.0f);
        float releaseMs = paramByName(node, "Release", 100.0f);
        float makeupDb  = paramByName(node, "Makeup Gain", 0.0f);
        float threshLin = std::pow(10.0f, threshold / 20.0f);
        float makeup    = std::pow(10.0f, makeupDb / 20.0f);
        float attackCoeff  = std::exp(-1.0f / (float)(attackMs * 0.001 * sampleRate));
        float releaseCoeff = std::exp(-1.0f / (float)(releaseMs * 0.001 * sampleRate));
        for (int s = 0; s < buf.getNumSamples(); ++s) {
            // Detect peak across all channels
            float peak = 0;
            for (int c = 0; c < buf.getNumChannels(); ++c)
                peak = std::max(peak, std::abs(buf.getSample(c, s)));
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

} // namespace SoundShop

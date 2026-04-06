#include "signal_shape_node.h"
#include <cmath>

namespace SoundShop {

SignalShapeProcessor::SignalShapeProcessor(Node& n, Transport& t) : node(n), transport(t) {
    if (!node.script.empty())
        wavetable.generateFromExpression(node.script);
    else
        wavetable.generateSine();
}

float SignalShapeProcessor::getParam(int idx, float def) const {
    if (idx >= 0 && idx < (int)node.params.size())
        return node.params[idx].value;
    return def;
}

void SignalShapeProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) {
    buf.clear();
    int numSamples = buf.getNumSamples();

    // Params:
    // 0 = Mode (0=LFO, 1=Envelope)
    // 1 = Rate (Hz for LFO, duration in beats for Envelope)
    // 2 = Min output value
    // 3 = Max output value
    // 4 = Beat sync (0=free Hz, 1=beat-synced)
    // 5 = Phase offset (0-1)

    int modeInt = (int)getParam(0, 0.0f);
    SignalMode mode = (modeInt == 1) ? SignalMode::Envelope : SignalMode::LFO;
    float rate = getParam(1, 1.0f);
    float minVal = getParam(2, 0.0f);
    float maxVal = getParam(3, 1.0f);
    bool beatSync = (int)getParam(4, 0.0f) != 0;
    float phaseOffset = getParam(5, 0.0f);

    // Check for envelope trigger (MIDI note-on)
    if (mode == SignalMode::Envelope) {
        for (auto metadata : midi) {
            auto msg = metadata.getMessage();
            if (msg.isNoteOn()) {
                envTriggered = true;
                envPhase = 0.0f;
            }
        }
    }

    // Generate signal
    for (int s = 0; s < numSamples; ++s) {
        float rawValue = 0.0f;

        if (mode == SignalMode::LFO) {
            float currentPhase;
            if (beatSync) {
                // Phase from transport beat position
                double beat = transport.positionBeats() +
                    transport.bpm / (60.0 * sampleRate) * s;
                currentPhase = std::fmod((float)(beat * rate) + phaseOffset, 1.0f);
                if (currentPhase < 0) currentPhase += 1.0f;
            } else {
                currentPhase = std::fmod(phase + phaseOffset, 1.0f);
                phase += rate / (float)sampleRate;
                if (phase > 1.0f) phase -= 1.0f;
            }
            rawValue = wavetable.sample(currentPhase, rate, (float)sampleRate);
        } else {
            // Envelope mode: play through shape once on trigger
            if (envTriggered) {
                rawValue = wavetable.sample(envPhase, 1.0f, (float)sampleRate);
                // Advance: rate = duration in beats
                float durationBeats = std::max(0.01f, rate);
                float beatsPerSample = (float)(transport.bpm / (60.0 * sampleRate));
                envPhase += beatsPerSample / durationBeats;
                if (envPhase >= 1.0f) {
                    envPhase = 1.0f;
                    envTriggered = false;
                    // Hold at last value
                    rawValue = wavetable.sample(1.0f, 1.0f, (float)sampleRate);
                }
            }
        }

        // Map from [-1, 1] to [minVal, maxVal]
        float normalized = (rawValue + 1.0f) * 0.5f; // 0..1
        lastOutputValue = minVal + normalized * (maxVal - minVal);
    }

    // Write the signal value to the param output for UI-rate reading
    if (!node.params.empty() && node.params.size() > 6)
        node.params[6].value = lastOutputValue;

    // Also write to audio channel 0 for audio-rate Signal pin output
    if (buf.getNumChannels() > 0) {
        // Fill the entire buffer with the signal (per-sample for fidelity)
        // Re-evaluate per sample for smooth output
        auto* out = buf.getWritePointer(0);
        float ph = phase;
        for (int i = 0; i < numSamples; ++i) {
            float val = 0;
            if (mode == SignalMode::LFO) {
                float curPhase;
                if (beatSync) {
                    double beat2 = transport.positionBeats() +
                        transport.bpm / (60.0 * sampleRate) * i;
                    curPhase = std::fmod((float)(beat2 * rate) + phaseOffset, 1.0f);
                    if (curPhase < 0) curPhase += 1.0f;
                } else {
                    curPhase = std::fmod(ph + phaseOffset, 1.0f);
                    ph += rate / (float)sampleRate;
                    if (ph > 1.0f) ph -= 1.0f;
                }
                val = wavetable.sample(curPhase, rate, (float)sampleRate);
            } else if (envTriggered) {
                val = wavetable.sample(envPhase, 1.0f, (float)sampleRate);
            }
            // Map to -1..1 output range (Signal pins use -1..1)
            out[i] = juce::jlimit(-1.0f, 1.0f, val);
        }
    } // "Output" param
}

} // namespace SoundShop

#pragma once
#include "node_graph.h"
#include "transport.h"
#include "builtin_synth.h" // reuse Wavetable and WaveExprParser
#include <juce_audio_processors/juce_audio_processors.h>

namespace SoundShop {

// Signal Shape Node — outputs a modulation signal via Param pins.
// The shape is defined by an expression or control points (same as waveform editor).
// Modes:
//   LFO: repeating at a given rate (Hz or beat-synced)
//   Envelope: one-shot triggered by MIDI note-on, plays through once

enum class SignalMode { LFO, Envelope };

class SignalShapeProcessor : public juce::AudioProcessor {
public:
    SignalShapeProcessor(Node& node, Transport& transport);

    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double sr, int bs) override { sampleRate = sr; }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return true; } // for envelope trigger
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

    Wavetable& getWavetable() { return wavetable; }
    float getCurrentValue() const { return lastOutputValue; }

private:
    Node& node;
    Transport& transport;
    double sampleRate = 44100;

    Wavetable wavetable;
    float phase = 0.0f;
    float lastOutputValue = 0.0f;

    // Envelope state
    bool envTriggered = false;
    float envPhase = 0.0f; // 0..1 through the shape, then stops

    float getParam(int idx, float def) const;
};

} // namespace SoundShop

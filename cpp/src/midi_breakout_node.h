#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace SoundShop {

// ============================================================================
// MidiBreakoutProcessor - taps a live MIDI stream and exposes its expression
// controllers as block-rate control signals on four Signal output pins, in
// this fixed order (matching the pins created in node_graph_component.cpp):
//
//   0: Velocity    0..1   last note-on velocity (held until the next note-on)
//   1: Pressure    0..1   channel aftertouch, or the latest poly key-pressure
//   2: Mod Wheel   0..1   CC 1
//   3: Pitch Bend  0..1   normalized 14-bit wheel, 0.5 = centre
//
// Control outputs live on audio-buffer channels 2.. (channels 0/1 are the
// unused audio pair); widenForControl() in graph_processor sizes the buffer to
// 2 + (number of control out pins). Values are held across blocks so an
// unchanging controller keeps emitting its last value rather than snapping to
// zero between events. The node does not forward MIDI - it is a pure
// MIDI-to-control tap.
//
// IMPORTANT: feeding any of these outputs back into the SAME synth that already
// receives this MIDI double-applies the controller, because every tonal synth
// reads pressure / pitch-bend / mod-wheel from its own MIDI input internally.
// The intended use is routing a controller somewhere it would not otherwise
// reach - a filter cutoff, a wavetable position, a *different* synth's Pressure
// input, an effect parameter, etc. The pin tooltips spell this out.
// ============================================================================
class MidiBreakoutProcessor : public juce::AudioProcessor {
public:
    explicit MidiBreakoutProcessor(Node& n) : node(n) {}
    const juce::String getName() const override { return node.name; }

    void prepareToPlay(double, int) override {
        velocity = 0.0f; pressure = 0.0f; modWheel = 0.0f; pitchBend = 0.5f;
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override {
        // Fold all incoming MIDI into the held controller values. We keep the
        // final value (block-rate) rather than tracking sub-block timing - the
        // consumers of these control signals sample per block, and expression
        // controllers don't need sample-accurate edges.
        for (const auto meta : midi) {
            const auto msg = meta.getMessage();
            if (msg.isNoteOn())
                velocity = (float) msg.getVelocity() / 127.0f;
            else if (msg.isChannelPressure())
                pressure = (float) msg.getChannelPressureValue() / 127.0f;
            else if (msg.isAftertouch())   // polyphonic key pressure
                pressure = (float) msg.getAfterTouchValue() / 127.0f;
            else if (msg.isController() && msg.getControllerNumber() == 1)
                modWheel = (float) msg.getControllerValue() / 127.0f;
            else if (msg.isPitchWheel())
                pitchBend = juce::jlimit(0.0f, 1.0f,
                                         (float) msg.getPitchWheelValue() / 16383.0f);
        }
        // Pure control tap: don't pass MIDI downstream.
        midi.clear();

        const int n  = buf.getNumSamples();
        const int nc = buf.getNumChannels();
        // Audio pair (0/1) carries nothing.
        for (int ch = 0; ch < juce::jmin(2, nc); ++ch)
            buf.clear(ch, 0, n);
        // Control outputs on channels 2.. in pin order.
        const float vals[4] = { velocity, pressure, modWheel, pitchBend };
        for (int i = 0; i < 4; ++i) {
            const int ch = 2 + i;
            if (ch < nc) {
                float* d = buf.getWritePointer(ch);
                for (int s = 0; s < n; ++s) d[s] = vals[i];
            }
        }
    }

    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return true; }   // we consume MIDI
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
    float velocity  = 0.0f;
    float pressure  = 0.0f;
    float modWheel  = 0.0f;
    float pitchBend = 0.5f;   // centre
};

} // namespace SoundShop

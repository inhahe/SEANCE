#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>
#include <cmath>

namespace SoundShop {

// =============================================================================
// VoiceInProcessor  -  the per-note context source ("Voice In" puck)
// =============================================================================
//
// Lives INSIDE a VoiceContainer's inner graph. There is one VoiceIn node per
// container, and PolyVoiceProcessor builds N independent clones of the inner
// graph (one per voice). Each clone has its OWN VoiceInProcessor instance; the
// container drives clone v's instance with voice v's per-note state before
// rendering that voice's block.
//
// Outputs (pin order fixed - see node creation in node_graph.cpp):
//   pin 0: "MIDI"     (Midi)   - raw per-voice note stream (fork (a): drives an
//                                ordinary MIDI synth inside the patch).
//   pin 1: "Pitch"    (Signal) - note frequency in Hz on control channel 2.
//   pin 2: "Gate"     (Signal) - 1.0 while held, 0.0 after note-off, channel 3.
//   pin 3: "Velocity" (Signal) - note-on velocity 0..1 on control channel 4.
//
// The container and the inner graph run on the SAME audio thread, sequentially
// (drive VoiceIn, then run the voice's graph), so the plain members below need
// no atomics - they are written and read within one synchronous call chain.
class VoiceInProcessor : public juce::AudioProcessor {
public:
    explicit VoiceInProcessor(Node& n) : node(n) {}

    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override {
        const int n = buf.getNumSamples();
        // Channels 0+1 carry no audio from a source puck.
        if (buf.getNumChannels() > 0) buf.clear(0, 0, n);
        if (buf.getNumChannels() > 1) buf.clear(1, 0, n);

        // Emit this block's queued note events (already at their within-block
        // sample offsets) into the inner graph's MIDI stream, then clear.
        for (auto& e : pending)
            midi.addEvent(e.msg, juce::jlimit(0, n > 0 ? n - 1 : 0, e.sampleOffset));
        pending.clear();

        // Write the per-voice context signals. Pitch/Gate/Velocity are held
        // constant across the block in v1 (sample-accurate ramps are M2). The
        // control channels are 2,3,4 (see widenForControl: 2 audio + 3 signal).
        writeConst(buf, 2, pitchHz);
        writeConst(buf, 3, gate);
        writeConst(buf, 4, velocity);
    }

    // ---- Driven by PolyVoiceProcessor (same audio thread) -------------------

    // Queue a note-on for this voice at the given within-block sample offset.
    void noteOn(int sampleOffset, int midiNote, float vel) {
        velocity = juce::jlimit(0.0f, 1.0f, vel);
        pitchHz  = midiToHz(midiNote);
        gate     = 1.0f;
        if (pending.size() < kMaxPending)
            pending.push_back({ juce::MidiMessage::noteOn(1, midiNote,
                                 (juce::uint8)juce::jlimit(1, 127, (int)std::lround(vel * 127.0f))),
                                 sampleOffset });
    }

    // Queue a note-off (release). The voice keeps sounding through its tail.
    void noteOff(int sampleOffset, int midiNote) {
        gate = 0.0f;
        if (pending.size() < kMaxPending)
            pending.push_back({ juce::MidiMessage::noteOff(1, midiNote), sampleOffset });
    }

    // Hard reset (voice stolen / panic): drop gate and flush an all-notes-off.
    void reset() {
        gate = 0.0f;
        pending.clear();
        if (pending.size() < kMaxPending)
            pending.push_back({ juce::MidiMessage::allNotesOff(1), 0 });
    }

    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return false; }
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

    static float midiToHz(int note) {
        return 440.0f * std::pow(2.0f, (note - 69) / 12.0f);
    }

private:
    void writeConst(juce::AudioBuffer<float>& buf, int ch, float v) {
        if (ch < buf.getNumChannels()) {
            float* d = buf.getWritePointer(ch);
            for (int i = 0, n = buf.getNumSamples(); i < n; ++i) d[i] = v;
        }
    }

    Node& node;
    double sampleRate = 44100.0;

    struct Ev { juce::MidiMessage msg; int sampleOffset; };
    static constexpr size_t kMaxPending = 64;
    std::vector<Ev> pending;

    float pitchHz  = 440.0f;
    float gate     = 0.0f;
    float velocity = 0.0f;
};

} // namespace SoundShop

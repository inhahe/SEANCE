#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <algorithm>
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

        // Write the per-voice context signals as PIECEWISE-CONSTANT ramps so a
        // note-on/off lands on its exact within-block sample offset (M2:
        // sample-accurate gates). Each segment in `segs` is a value change at an
        // offset; before the first segment the signal holds the value carried
        // from the previous block. The control channels are 2,3,4 (see
        // widenForControl: 2 audio + 3 signal -> Pitch ch2, Gate ch3, Vel ch4).
        writeSignals(buf, n);
    }

    // ---- Driven by PolyVoiceProcessor (same audio thread) -------------------

    // Queue a note-on for this voice at the given within-block sample offset.
    // Schedules the Pitch/Gate/Velocity step at that offset AND the MIDI note.
    void noteOn(int sampleOffset, int midiNote, float vel) {
        const float v  = juce::jlimit(0.0f, 1.0f, vel);
        const float hz = midiToHz(midiNote);
        pushSeg({ sampleOffset, 1.0f, hz, v });
        if (pending.size() < kMaxPending)
            pending.push_back({ juce::MidiMessage::noteOn(1, midiNote,
                                 (juce::uint8)juce::jlimit(1, 127, (int)std::lround(vel * 127.0f))),
                                 sampleOffset });
    }

    // Queue a note-off (release) at the given offset. The voice keeps sounding
    // through its tail, so Pitch/Velocity are held at their current values and
    // only the Gate falls to 0.
    void noteOff(int sampleOffset, int midiNote) {
        pushSeg({ sampleOffset, 0.0f, lastPitch(), lastVel() });
        if (pending.size() < kMaxPending)
            pending.push_back({ juce::MidiMessage::noteOff(1, midiNote), sampleOffset });
    }

    // Hard reset (voice stolen / panic): drop the gate at the block start and
    // flush an all-notes-off. Clears any segments already queued this block so
    // a stale note-on can't survive the steal (and keeps `segs` ordered).
    void reset() {
        const float p = lastPitch();
        const float v = lastVel();
        segs.clear();
        pushSeg({ 0, 0.0f, p, v });
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
    // One scheduled value-change of the Pitch/Gate/Velocity signals, to take
    // effect from `offset` samples into the current block.
    struct Seg { int offset; float gate, pitch, vel; };

    // The latest scheduled Pitch/Velocity this block (or the carried value if
    // nothing is queued yet) - what a note-off should hold the tone at.
    float lastPitch() const { return segs.empty() ? curPitch : segs.back().pitch; }
    float lastVel()   const { return segs.empty() ? curVel   : segs.back().vel; }

    void pushSeg(const Seg& s) {
        if (segs.size() < kMaxSegs) segs.push_back(s);
    }

    // Render the three control signals as piecewise-constant ramps across the
    // block, then carry the final values into the next block. Channels: Pitch
    // = 2, Gate = 3, Velocity = 4 (matches widenForControl's layout). Segments
    // are stable-sorted by offset so an out-of-order reset (offset 0) injected
    // mid-stream still produces a monotonic ramp.
    void writeSignals(juce::AudioBuffer<float>& buf, int n) {
        std::stable_sort(segs.begin(), segs.end(),
                         [](const Seg& a, const Seg& b) { return a.offset < b.offset; });
        const int ch = buf.getNumChannels();
        float* pp = (2 < ch) ? buf.getWritePointer(2) : nullptr; // Pitch
        float* gp = (3 < ch) ? buf.getWritePointer(3) : nullptr; // Gate
        float* vp = (4 < ch) ? buf.getWritePointer(4) : nullptr; // Velocity

        size_t idx = 0;
        float g = curGate, p = curPitch, v = curVel;
        for (int i = 0; i < n; ++i) {
            while (idx < segs.size() && segs[idx].offset <= i) {
                g = segs[idx].gate; p = segs[idx].pitch; v = segs[idx].vel; ++idx;
            }
            if (pp) pp[i] = p;
            if (gp) gp[i] = g;
            if (vp) vp[i] = v;
        }
        // Any segments past the block end still update the carried value so they
        // take effect at the very start of the next block.
        while (idx < segs.size()) {
            g = segs[idx].gate; p = segs[idx].pitch; v = segs[idx].vel; ++idx;
        }
        curGate = g; curPitch = p; curVel = v;
        segs.clear();
    }

    Node& node;
    double sampleRate = 44100.0;

    struct Ev { juce::MidiMessage msg; int sampleOffset; };
    static constexpr size_t kMaxPending = 64;
    static constexpr size_t kMaxSegs    = 128;
    std::vector<Ev> pending;
    std::vector<Seg> segs;

    // Carried signal values: the level at the START of the current block (= the
    // level at the end of the previous one). Updated by writeSignals each block.
    float curPitch = 440.0f;
    float curGate  = 0.0f;
    float curVel   = 0.0f;
};

} // namespace SoundShop

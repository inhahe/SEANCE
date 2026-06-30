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
// Outputs (pin order fixed - see node creation in node_graph_component.cpp):
//   pin 0: "MIDI"     (Midi)   - raw per-voice note stream (fork (a): drives an
//                                ordinary MIDI synth inside the patch).
//   pin 1: "Pitch"    (Signal) - note frequency in Hz on control channel 2.
//                                INCLUDES per-note pitch bend (MPE / channel
//                                pitch wheel folded in multiplicatively).
//   pin 2: "Gate"     (Signal) - 1.0 while held, 0.0 after note-off, channel 3.
//   pin 3: "Velocity" (Signal) - note-on velocity 0..1 on control channel 4.
//   pin 4: "Pressure" (Signal) - per-note pressure 0..1 on channel 5 (MPE
//                                channel pressure / poly aftertouch; 0 at rest).
//   pin 5: "Timbre"   (Signal) - per-note timbre 0..1 on channel 6 (MPE CC74 /
//                                "slide"; 0.5 at rest = centre).
//
// The three expression dimensions (bend, pressure, timbre) are continuous
// controllers: PolyVoiceProcessor sets a per-block TARGET from the matching MPE
// message and VoiceIn ramps toward it with a short one-pole smoother (~5 ms) so
// there is no zipper noise. They reset to neutral on every fresh note-on so one
// note's expression never bleeds into the next note that reuses the voice.
//
// The container and the inner graph run on the SAME audio thread, sequentially
// (drive VoiceIn, then run the voice's graph), so the plain members below need
// no atomics - they are written and read within one synchronous call chain.
class VoiceInProcessor : public juce::AudioProcessor {
public:
    explicit VoiceInProcessor(Node& n) : node(n) {}

    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double sr, int) override {
        sampleRate = sr;
        // One-pole smoothing coefficient for the expression signals (~5 ms time
        // constant). coef = 1 - exp(-1/(tau*sr)); guarded for absurd sample rates.
        const double tau = 0.005;
        exprSmooth = (sr > 0.0) ? (float) (1.0 - std::exp(-1.0 / (tau * sr))) : 1.0f;
    }
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
    // glideMs > 0 makes the Pitch signal SLIDE (portamento) from its current
    // value to the new note over glideMs instead of stepping - used when this
    // voice was stolen mid-note (see PolyVoiceProcessor). The Gate still steps
    // instantly at the offset so the envelope retriggers on time.
    void noteOn(int sampleOffset, int midiNote, float vel, float glideMs = 0.0f) {
        const float v  = juce::jlimit(0.0f, 1.0f, vel);
        const float hz = midiToHz(midiNote);
        // Fresh note: snap every expression dimension back to neutral so the
        // previous note's bend/pressure/timbre can't bleed into this one when
        // the voice is reused. Subsequent MPE messages this block re-target them.
        curBend = tgtBend = 0.0f;
        curPressure = tgtPressure = 0.0f;
        curTimbre = tgtTimbre = 0.5f;
        int glideSamples = 0;
        if (glideMs > 0.0f && sampleRate > 0.0)
            glideSamples = (int) std::lround(glideMs * 0.001 * sampleRate);
        pushSeg({ sampleOffset, 1.0f, hz, v, glideSamples });
        if (pending.size() < kMaxPending)
            pending.push_back({ juce::MidiMessage::noteOn(1, midiNote,
                                 (juce::uint8)juce::jlimit(1, 127, (int)std::lround(vel * 127.0f))),
                                 sampleOffset });
    }

    // Queue a note-off (release) at the given offset. The voice keeps sounding
    // through its tail, so Pitch/Velocity are held at their current values and
    // only the Gate falls to 0.
    void noteOff(int sampleOffset, int midiNote) {
        pushSeg({ sampleOffset, 0.0f, lastPitch(), lastVel(), 0 });
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
        pushSeg({ 0, 0.0f, p, v, 0 });
        pending.clear();
        if (pending.size() < kMaxPending)
            pending.push_back({ juce::MidiMessage::allNotesOff(1), 0 });
        // A stolen/panicked voice also drops its expression to neutral.
        curBend = tgtBend = 0.0f;
        curPressure = tgtPressure = 0.0f;
        curTimbre = tgtTimbre = 0.5f;
    }

    // ---- Per-note MPE expression (set per block by PolyVoiceProcessor) -------
    // These set the TARGET; writeSignals ramps the live value toward it each
    // sample. setPitchBend is in semitones (folded into the Pitch signal);
    // pressure/timbre are 0..1 (timbre centre = 0.5).
    void setPitchBend(float semitones) { tgtBend = semitones; }
    void setPressure (float v01)       { tgtPressure = juce::jlimit(0.0f, 1.0f, v01); }
    void setTimbre   (float v01)       { tgtTimbre   = juce::jlimit(0.0f, 1.0f, v01); }

    // Unison detune for this voice's slot in cents, folded into the Pitch signal
    // alongside pitch bend. A structural per-slot constant (NOT reset on note-on,
    // unlike the MPE expression dimensions): the container sets it when it assigns
    // the slot to a position in a unison stack. 0 = no detune (default).
    void setUnisonDetune(float cents) { unisonDetuneCents = cents; }

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
    // effect from `offset` samples into the current block. glideSamples > 0
    // turns the pitch change into a portamento ramp of that many samples
    // (linear in log-frequency, i.e. constant semitones/sec); 0 = instant step.
    struct Seg { int offset; float gate, pitch, vel; int glideSamples; };

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
        float* zp = (5 < ch) ? buf.getWritePointer(5) : nullptr; // Pressure
        float* yp = (6 < ch) ? buf.getWritePointer(6) : nullptr; // Timbre

        size_t idx = 0;
        float g = curGate, p = curPitch, v = curVel;
        for (int i = 0; i < n; ++i) {
            while (idx < segs.size() && segs[idx].offset <= i) {
                const Seg& s = segs[idx];
                g = s.gate; v = s.vel;
                if (s.glideSamples > 0 && p > 0.0f && s.pitch > 0.0f) {
                    // Begin a portamento ramp from the current pitch toward the
                    // target, advancing one step per sample in log-frequency.
                    glideTargetLog = std::log((double) s.pitch);
                    glideRemaining = s.glideSamples;
                    glideStepLog   = (glideTargetLog - std::log((double) p))
                                     / (double) s.glideSamples;
                } else {
                    p = s.pitch;          // instant step
                    glideRemaining = 0;
                }
                ++idx;
            }
            // Advance an in-flight glide toward its target (may span blocks).
            if (glideRemaining > 0) {
                double lp = std::log((double) p) + glideStepLog;
                if (--glideRemaining == 0) lp = glideTargetLog; // snap to exact
                p = (float) std::exp(lp);
            }
            // Smooth the expression signals toward their per-block targets so
            // continuous controllers (bend/pressure/timbre) don't zipper.
            curBend     += (tgtBend     - curBend)     * exprSmooth;
            curPressure += (tgtPressure - curPressure) * exprSmooth;
            curTimbre   += (tgtTimbre   - curTimbre)   * exprSmooth;
            // Fold pitch bend AND unison detune into the Pitch signal
            // multiplicatively (both expressed in semitones; cents/100).
            const float semis = curBend + unisonDetuneCents * 0.01f;
            const float hzOut = (semis != 0.0f)
                ? p * std::exp2(semis * (1.0f / 12.0f)) : p;
            if (pp) pp[i] = hzOut;
            if (gp) gp[i] = g;
            if (vp) vp[i] = v;
            if (zp) zp[i] = curPressure;
            if (yp) yp[i] = curTimbre;
        }
        // Any segments past the block end still update the carried value so they
        // take effect at the very start of the next block.
        while (idx < segs.size()) {
            const Seg& s = segs[idx];
            g = s.gate; v = s.vel;
            if (s.glideSamples > 0 && p > 0.0f && s.pitch > 0.0f) {
                glideTargetLog = std::log((double) s.pitch);
                glideRemaining = s.glideSamples;
                glideStepLog   = (glideTargetLog - std::log((double) p)) / (double) s.glideSamples;
            } else {
                p = s.pitch;
                glideRemaining = 0;
            }
            ++idx;
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

    // In-flight portamento ramp state, persisted across blocks so a glide longer
    // than one block keeps sliding. glideRemaining == 0 means no active glide.
    int    glideRemaining = 0;
    double glideStepLog   = 0.0;
    double glideTargetLog = 0.0;

    // Per-note MPE expression. `cur` is the smoothed live value, `tgt` the per-
    // block target set by setPitchBend/setPressure/setTimbre. Defaults are the
    // neutral resting points (bend 0 semis, pressure 0, timbre 0.5 = centre).
    float curBend = 0.0f, tgtBend = 0.0f;          // semitones
    float curPressure = 0.0f, tgtPressure = 0.0f;  // 0..1
    float curTimbre = 0.5f, tgtTimbre = 0.5f;      // 0..1 (centre 0.5)
    float exprSmooth = 1.0f;                        // per-sample one-pole coef

    // Unison detune for this voice's slot, in cents (structural, not reset on
    // note-on). Folded into the Pitch signal alongside pitch bend.
    float unisonDetuneCents = 0.0f;
};

} // namespace SoundShop

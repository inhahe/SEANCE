#pragma once
#include "node_graph.h"
#include "transport.h"
#include "builtin_synth.h"        // WaveExprParser::runProgram + IExprEmitSink
#include "script_runtime.h"       // IScriptRuntime, ScriptLang/Rate
#include "layered_wave_editor.h"  // LayeredWaveform for the optional shape() table
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace SoundShop {

// -----------------------------------------------------------------------------
// MIDI Script (algorithmic MIDI generator)
// -----------------------------------------------------------------------------
//
// A MIDI Script node runs a small program (the SEANCE mini-language - the same
// grammar and built-in math as the wavetable / Signal Shape expression editor)
// once per sample and emits MIDI events. Unlike a one-shot Python authoring
// pass, it runs live and forever, so it can generate arpeggios, euclidean
// rhythms, generative melodies, LFO-style CC sweeps, etc., reacting to the
// transport and its inputs in real time.
//
// The program differs from a plain expression in three ways (all added to
// WaveExprParser as "program mode"):
//
//   1. Statements - separated by ';' or newlines. A statement is either an
//      assignment `name = expr` or a bare expression run for side effects.
//   2. Persistent state - any assigned variable survives across samples and
//      across audio blocks (it lives in the processor, not the stack), so a
//      program can keep a running phase, step counter, RNG seed, etc.
//   3. MIDI emit functions - note(p,v,d) noteon(p,v) noteoff(p) cc(n,v)
//      bend(v). They push events into the node's MIDI output at the current
//      sample offset. The reserved variable `out` selects which MIDI output
//      pin they go to (0-based; default 0).
//
// Per-sample variables the program can read:
//   t       seconds since transport start (0 while stopped at start)
//   beat    transport beat position
//   bpm     current tempo
//   bar     beat / 4 (whole bars in 4/4; just beat/4 otherwise)
//   playing 1 while the transport is rolling, 0 when stopped
//   sr      sample rate
//   dt      seconds per sample (1 / sr)
//   note    MIDI note number of the most-recent note-on across ALL MIDI inputs (-1 if none)
//   vel     velocity 0..1 of that note-on
//   gate    1 while any MIDI-input note is held, 0 otherwise
//           (per-input identity is available via pollmidi()/midievent(), which
//            report a 1-based input index for each event)
//   s1..sN  the Signal input pin values at this sample
//   out     reserved output selector (read/write; default 0)
//   shape(pos)  samples the optional drawn shape table at pos (0..1, wraps)
//
// Determinism: like Signal Shape, the program is driven from the transport
// position, so a given song position always produces the same MIDI. The RNG
// (`noise()` / `random`) is seeded once and advances per call, so generative
// patterns are reproducible within a single play-through but evolve over time.
struct MidiScriptDoc {
    // The program text. Empty = emits nothing (a fresh node is silent). For the
    // Builtin and Lua languages this IS the source; for Wasm it is ignored (the
    // binary comes from wasmPath instead).
    std::string program;

    // Which scripting language runs `program`:
    //   Builtin - the SEANCE mini-expression language (per-sample only).
    //   Lua     - embedded Lua 5.4 (per-sample OR per-block, see `rate`).
    //   Wasm    - a user-supplied .wasm binary (per-block only; from wasmPath).
    ScriptLang language = ScriptLang::Builtin;

    // Execution rate. Builtin is always PerSample; Wasm is always PerBlock; only
    // Lua honours this choice. PerBlock calls the script once per audio block
    // (cheap, scales to many instances); PerSample calls it every sample
    // (sample-accurate but heavy Lua can stutter the audio thread).
    ScriptRate rate = ScriptRate::PerSample;

    // Filesystem path to the .wasm binary (Wasm language only). Chosen via the
    // editor's native file dialog. Empty until the user picks a file.
    std::string wasmPath;

    // Number of Signal input pins (0..16), exposed as s1..sN.
    int signalInputCount = 0;

    // Number of MIDI input pins (0..16). 0 = no MIDI input (note/vel/gate stay
    // idle); 1 = a single "MIDI In" pin; >1 exposes "MIDI In 1..N". With more
    // than one, pollmidi() / midievent() report which input each event arrived
    // on (1-based) so the program can react to several MIDI sources separately.
    // The note/vel/gate convenience variables track the most recent note-on
    // across ALL inputs.
    int midiInputCount = 1;

    // Number of MIDI output pins (1..16). Each is an independent MIDI cable;
    // the program routes to one via the `out` variable. Implemented by tagging
    // emitted events with channel = out+1 and inserting a per-cable channel
    // filter in the graph (see graph_processor.cpp), so downstream nodes see a
    // clean single-stream cable, not muxed channels.
    int outputCount = 1;

    // Optional drawn shape, sampled by shape(pos) in the program. A fresh node
    // has zero layers (shape() returns 0 until the user adds one).
    LayeredWaveform shapeLayers;

    // Round-trip via node.script. Prefix "__midiscript__:".
    std::string encode() const;
    bool decode(const std::string& s);

    static MidiScriptDoc defaultDoc();
    static bool isMidiScriptScript(const std::string& s);
};

class MidiScriptProcessor : public juce::AudioProcessor {
public:
    MidiScriptProcessor(Node& node, Transport& transport);

    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double sr, int bs) override { sampleRate = sr; }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;
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

    // How many MIDI output pins this script declares (>=1). graph_processor
    // reads this to decide whether to insert per-cable channel filters.
    int getOutputCount() const { return doc.outputCount; }

    // True when the most recent script (re)load failed to compile/link (Lua
    // syntax error, Wasm load/link error, ...). Set on the AUDIO thread in
    // reloadIfScriptChanged(); read on the MESSAGE thread by the node-graph
    // editor to draw a red error badge on the node. atomic<bool> so the
    // cross-thread read is a clean data-race-free load. Builtin never errors
    // here (it parses lazily per sample), so this stays false for Builtin.
    bool hasScriptError() const { return scriptHasError.load(std::memory_order_relaxed); }

private:
    Node& node;
    Transport& transport;
    double sampleRate = 44100;

    MidiScriptDoc doc;
    std::string lastScriptSeen;
    void reloadIfScriptChanged();

    // The active language runtime (Builtin / Lua / Wasm). Owns the program /
    // binary and all persistent script state (the init/start/loop sectioning for
    // the Builtin language now lives inside BuiltinExprRuntime). Re-created
    // whenever the script text or language changes.
    std::unique_ptr<IScriptRuntime> runtime;
    // True when the runtime runs once per block (Wasm always; Lua when its rate
    // is PerBlock). False = the host drives a per-sample loop (Builtin always;
    // Lua when PerSample).
    bool runBlockMode = false;
    bool wasPlaying = false;   // tracks the transport play rising edge

    // Set true after a failed runtime->load() (Lua/Wasm compile/link error);
    // cleared on a successful load. Written on the audio thread, read on the
    // message thread (see hasScriptError()).
    std::atomic<bool> scriptHasError{false};

    // Fill `vars` with the standard per-sample bindings for sample offset `s`
    // within the current block. Used by the per-sample dispatch loop and the
    // transport-start hook.
    void buildVars(ScriptVars& vars, int s,
                   double sr, double secPerSample, float gateVal, float freqHz,
                   const std::vector<const float*>& sigChans, int sigCount) const;
    // Bind the shape(pos) sampler on the current runtime (after a shape rebuild).
    void bindShape();

    // Optional pre-rendered shape table for shape(pos).
    static constexpr int kShapeTableSize = 1024;
    std::vector<float> shapeSamples;
    void rebuildShapeSamples();

    // MIDI input state tracked across blocks (note / vel / gate vars).
    int   notesHeld = 0;
    int   lastNoteOn = -1;
    float lastVelocity = 0.0f;

    // Event-driven MIDI-input scratch for block mode (ScriptBlockCtx::midiIn).
    // A member so it isn't reallocated on the audio thread every block.
    std::vector<ScriptMidiEvent> midiInEvents;

    // A note scheduled by note(p,v,d): its note-off is due `samplesRemaining`
    // samples from the start of the CURRENT block. Each block we subtract the
    // block length; when it reaches <= 0 the note-off is emitted at that offset.
    // Tracked across blocks so durations longer than one block still end
    // correctly. Works identically for per-sample and per-block dispatch.
    struct PendingNoteOff {
        long long samplesRemaining; // samples from current block start until off
        int channel;                // 1..16 (out+1)
        int pitch;                  // 0..127
    };
    std::vector<PendingNoteOff> pendingOffs;
    // Hard cap so a runaway program can't grow the vector without bound on the
    // audio thread. New notes beyond this are dropped (their note-off would be
    // unmanaged anyway).
    static constexpr size_t kMaxPendingOffs = 512;

    float getParam(int idx, float def) const;

    // Emit sink implementation (see builtin_synth.h IExprEmitSink). Holds the
    // current MIDI buffer + sample offset so emit calls land at the right time.
    struct Sink;
    friend struct Sink;
};

// -----------------------------------------------------------------------------
// MidiChannelFilterProcessor
// -----------------------------------------------------------------------------
//
// A MidiScript node with >1 MIDI output emits all events into its single output
// MIDI buffer, tagging each with channel = output-index + 1. JUCE's audio graph
// only models ONE MIDI bus per node, so to give each output pin a genuinely
// independent cable we splice one of these filters onto every outgoing MIDI
// cable: it passes only events on its target channel and rewrites them to
// channel 1, so the downstream node sees a clean single-stream cable (the
// channel tag is purely an internal routing detail, never user-visible).
//
// Audio passes through untouched (it's a pure MIDI router). Created and wired
// by graph_processor during graph rebuild; one filter is shared by all cables
// leaving the same output pin.
class MidiChannelFilterProcessor : public juce::AudioProcessor {
public:
    explicit MidiChannelFilterProcessor(int targetChannel1to16)
        : targetChannel(targetChannel1to16) {}

    const juce::String getName() const override { return "MIDI Out Filter"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;
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
    int targetChannel = 1;
};

// -----------------------------------------------------------------------------
// MidiChannelStampProcessor
// -----------------------------------------------------------------------------
//
// The input-side mirror of MidiChannelFilterProcessor. A SignalShape / MidiScript
// / Script node with >1 MIDI input pin needs to know which input pin each event
// arrived on, but JUCE merges all incoming MIDI cables into a node's single MIDI
// bus. So graph_processor splices one of these onto every cable feeding a given
// input pin: it rewrites EVERY event's channel to the pin's index + 1 (regardless
// of the event's original channel), so after the merge the receiving processor
// can recover the source pin from the channel nibble. The channel tag is purely
// an internal routing detail (the script API never exposes raw MIDI channels).
//
// Audio passes through untouched. One stamper is shared by all cables into the
// same input pin (they all stamp to the same channel and merge cleanly).
class MidiChannelStampProcessor : public juce::AudioProcessor {
public:
    explicit MidiChannelStampProcessor(int channel1to16)
        : channel(channel1to16) {}

    const juce::String getName() const override { return "MIDI In Stamp"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;
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
    int channel = 1;
};

} // namespace SoundShop

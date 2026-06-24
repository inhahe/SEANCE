#pragma once
#include "builtin_synth.h"   // IExprEmitSink, WaveExprParser
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <cmath>

namespace SoundShop {

// -----------------------------------------------------------------------------
// Script runtime abstraction
// -----------------------------------------------------------------------------
//
// Both the MIDI Script node (algorithmic MIDI generator) and the Signal Shape
// node (LFO / envelope) can run their program in one of three languages:
//
//   * Builtin  - the SEANCE mini-expression language (WaveExprParser).
//   * Lua      - an embedded Lua 5.4 interpreter.
//   * Wasm     - a user-supplied WebAssembly binary (loaded from a .wasm file).
//
// A node owns one IScriptRuntime chosen from its (language, rate, role). The
// runtime is the ONLY place that knows the language; the processors build a
// uniform per-sample variable map / per-block context and hand it over.
//
// Execution rate
// --------------
// There are two execution paths, picked per node:
//
//   PerSample - the host runs the per-sample loop and calls the runtime once
//               per sample (evalSignal / runMidi). Cheap and sample-accurate
//               for Builtin (an allocation-free expression walker). For Lua it
//               is convenient (same mental model as Builtin) but calls the
//               interpreter 44,100+ times a second, so a heavy script can cause
//               audio stutter - the editor warns about this.
//
//   PerBlock  - the host calls the runtime once per audio block (runBlock); the
//               runtime fills the whole output buffer (Signal) or emits all the
//               block's MIDI with per-event sample offsets (MIDI). One
//               interpreter call per block, so it scales to many instances.
//               Sample-accurate output is still possible - the script writes
//               each sample / stamps each event itself (see the docs' block-mode
//               loop example). This is the only mode Wasm supports (its ABI is
//               inherently block-at-a-time).
//
// Capability matrix:
//   Builtin -> PerSample only
//   Lua     -> PerSample and PerBlock
//   Wasm    -> PerBlock only
//
enum class ScriptLang { Builtin = 0, Lua = 1, Wasm = 2 };
enum class ScriptRate { PerSample = 0, PerBlock = 1 };
// Midi    - pure MIDI generator (emits via sink, no continuous output).
// Signal  - pure continuous generator (one value per sample via evalSignal).
// Unified - the general scriptable node: ONE program that both emits MIDI via
//           the sink AND assigns continuous outputs o1..oP (read back after the
//           run). Behaves like Midi for program sectioning (init:/start:/loop:)
//           and persistent state, but also exposes runUnified() to harvest the
//           o1..oP values. A node with 0 MIDI outputs is a pure signal source;
//           with 0 continuous outputs it is a pure MIDI generator; both > 0 is a
//           hybrid. See runUnified().
enum class ScriptRole { Midi, Signal, Unified };

using ScriptVars = std::unordered_map<std::string, float>;

// One incoming MIDI-input event delivered to a per-block script, so a streaming
// (PerBlock) program can react to notes/CC/bend EVENT-DRIVEN at sample accuracy
// instead of only polling the block-constant note/vel/gate/freq snapshot below.
// `kind`: 0 = note-off, 1 = note-on, 2 = control-change, 3 = pitch-bend.
//   note-on : a = note number,  b = velocity 0..1
//   note-off: a = note number,  b = 0
//   cc      : a = controller#,  b = value 0..1
//   bend    : a = 0,            b = -1..1 (0 = centre)
// `offset` is the sample position within the block. The list is in arrival order
// (non-decreasing offset). Only populated for the PerBlock path.
struct ScriptMidiEvent {
    int   offset  = 0;
    int   kind    = 0;
    int   a       = 0;
    float b       = 0.0f;
    // Which MIDI-INPUT pin this event arrived on (0-based). With a single MIDI
    // input it is always 0; it carries the source pin once a node exposes more
    // than one MIDI input, so a streaming script can tell its inputs apart
    // (pollmidi() returns it as the leading value). Part of the unified
    // event format shared with param inputs.
    int   inIndex = 0;
};

// Per-block execution context for the PerBlock path. The host fills the timing /
// IO fields, then calls runBlock(). Signal-role runtimes write `out`; MIDI-role
// runtimes emit through `sink` (stamping each event with a sample offset).
struct ScriptBlockCtx {
    double sampleRate = 44100.0;
    int    numSamples = 0;
    double bpm        = 120.0;
    bool   playing    = false;

    // Block-start transport position.
    long long posSamples = 0;
    double    posBeats   = 0.0;

    // Most-recent MIDI-input note state (constant across the block).
    int   note = -1;
    float vel  = 0.0f;
    float gate = 0.0f;
    float freq = 0.0f;

    // Signal-role only: the Rate node-param (cycles/sec, or /beat when synced).
    float rate = 1.0f;

    // Signal input pins s1..sN: each pointer is `numSamples` long, or null.
    const std::vector<const float*>* sig = nullptr;
    int sigCount = 0;

    // Event-driven MIDI INPUT for this block (note-on/off, CC, bend), each
    // stamped with a within-block sample offset. Null/empty when the node has no
    // MIDI input or nothing arrived. Scripts iterate this (midiin()/midievent()
    // in Lua) instead of, or alongside, the block-constant note/vel/gate/freq.
    const std::vector<ScriptMidiEvent>* midiIn = nullptr;
    int midiInCount = 0;

    // Signal role: output buffer to fill (length numSamples). Null for MIDI.
    // For a SINGLE continuous output this is the buffer; for the multi-output
    // streaming path it aliases outs[0] (the first output pin). Block-mode
    // loop() programs only ever see this one buffer (out(i,v)).
    float* out = nullptr;

    // Multi-output streaming: one buffer per continuous output pin o1..oP, each
    // `numSamples` long. The streaming out(pin, v) writes outs[pin]; a script
    // with one output just uses out(v) (pin 0). Null/empty for the single-output
    // and block-loop() paths (which use `out` above). outCount == P.
    const std::vector<float*>* outs = nullptr;
    int outCount = 0;

    // MIDI role: emit sink. Null for Signal.
    IExprEmitSink* sink = nullptr;
};

// -----------------------------------------------------------------------------
// IScriptRuntime
// -----------------------------------------------------------------------------
struct IScriptRuntime {
    virtual ~IScriptRuntime() = default;

    // Compile / parse `source`. Returns false and fills `error` on failure.
    // Builtin never fails here (it parses lazily per sample); Lua reports
    // syntax errors; Wasm reports load/link errors.
    virtual bool load(const std::string& source, std::string& error) = 0;

    // Bind the shape(pos) sampler (the node's drawn-waveform table). Stable
    // across a block; re-set whenever the table is re-rendered.
    virtual void setShape(std::function<float(float)> shapeFn) { shape = std::move(shapeFn); }

    // Bind the note->frequency mapper, which applies the PROJECT tuning system
    // (Equal12 / Pythagorean / Just / Meantone + concert pitch) rather than a
    // hardcoded 12-TET 440. Scripts reach it via notefreq(). The host binds this
    // capturing its Transport (transport.noteToFreq); the default fallback below
    // is plain 12-TET so a runtime with no host binding still returns something
    // sensible.
    virtual void setNoteToFreq(std::function<float(int)> fn) { noteToFreq = std::move(fn); }

    // Helper for trampolines / evaluators: frequency of a MIDI note using the
    // bound mapper, or 12-TET A440 if none was bound.
    float noteFreq(int note) const {
        if (noteToFreq) return noteToFreq(note);
        return 440.0f * std::pow(2.0f, (note - 69) / 12.0f);
    }

    // Reset persistent state. Called on reload / when the program text changes.
    // For languages with an init hook (Builtin `init:`, Lua `init()`), this also
    // runs that hook once.
    virtual void reset() {}

    // Transport-start rising edge (stopped -> rolling). Runs the start hook
    // (Builtin `start:`, Lua `start()`). `vars` are the standard bindings at the
    // block start; `sink` is non-null for the MIDI role so a one-shot can emit.
    virtual void onStart(const ScriptVars& vars, IExprEmitSink* sink) { (void)vars; (void)sink; }

    // --- PerSample path -------------------------------------------------------
    // Signal role: evaluate and return the output value for one sample.
    virtual float evalSignal(const ScriptVars& vars) { (void)vars; return 0.0f; }
    // MIDI role: run the program for one sample, emitting via `sink`
    // (sink->sampleOffset has already been set by the host).
    virtual void  runMidi(const ScriptVars& vars, IExprEmitSink* sink) { (void)vars; (void)sink; }
    // Unified role: run the program for one sample. It may emit MIDI via `sink`
    // (may be null when the node has no MIDI outputs) AND assign continuous
    // outputs, which the host harvests into outs[0..outCount-1] from the
    // program variables o1..oP. outs[0] (o1) defaults to the program's return
    // value (the last bare expression) when not explicitly assigned, so the
    // common single-output program `curve` still works. The default
    // implementation falls back to evalSignal() for outs[0] and ignores the
    // sink, so runtimes that haven't implemented the unified path still produce
    // a continuous signal (just no MIDI).
    virtual void runUnified(const ScriptVars& vars, IExprEmitSink* sink,
                            float* outs, int outCount) {
        (void)sink;
        float v = evalSignal(vars);
        for (int i = 0; i < outCount; ++i) outs[i] = (i == 0) ? v : 0.0f;
    }

    // --- PerBlock path --------------------------------------------------------
    virtual void runBlock(const ScriptBlockCtx& ctx) { (void)ctx; }

    // --- Whole-grid generation path (offline terrain authoring) ---------------
    // Fill `data` (length = product(dims), row-major) with one value per cell in
    // [0,1]. Unlike the per-cell path (evalSignal called once per coordinate),
    // the program runs ONCE and owns the whole array, so it can do cross-cell
    // work the per-cell path can't: blur/convolution, cellular automata, FFT,
    // iterative relaxation, global normalisation, etc. The runtime exposes the
    // grid shape and a get/set API to the script (e.g. Lua globals `dims`, `nd`,
    // `total` plus `set(i,v)` / `get(i)` and a `generate()` entry). Returns false
    // and fills `error` when the language can't generate whole-grid (the default)
    // or the program fails. Mirrors supportsPerBlock() in the capability matrix.
    virtual bool runGenerate(const std::vector<int>& dims, float* data,
                             std::string& error) {
        (void)dims; (void)data;
        error = "this language can't generate a whole grid at once";
        return false;
    }

    // Which rates this runtime instance supports.
    virtual bool supportsPerSample() const = 0;
    virtual bool supportsPerBlock()  const = 0;

    // True when the loaded program OWNS ITS OWN SAMPLE LOOP and drives every
    // output pin independently (vs. a classic loop() that produces one buffer the
    // host fans to all pins). Two cases qualify:
    //   * a Lua `stream()` coroutine (pulls input / pushes output one sample or
    //     one block at a time, state persisting across blocks), and
    //   * any WASM module (the ABI is inherently a program-owns-the-loop model:
    //     ss_process() fills the whole block and linear memory persists between
    //     calls, so it is morally a streaming program too).
    // The host uses this to (a) run the program through the per-block path
    // regardless of the per-sample/per-block Run setting - it is inherently
    // sample-accurate so the rate choice only matters for classic loop()
    // programs - and (b) hand it one output buffer per pin (ScriptBlockCtx::outs)
    // and route each back to its own cable instead of fanning a single buffer.
    // Default false (Builtin); Lua (when a stream() is loaded) and Wasm override.
    virtual bool isStreaming() const { return false; }

    // Last load/run error (empty if none). Builtin never errors; Lua/Wasm report
    // syntax / load / runtime failures here for the editor to surface.
    virtual std::string getError() const { return {}; }

protected:
    std::function<float(float)> shape;       // shape(pos) sampler (may be empty)
    std::function<float(int)>   noteToFreq;   // note->Hz via project tuning (may be empty)
};

// Convert a block's MIDI input buffer into the ScriptMidiEvent list a per-block
// script consumes via ScriptBlockCtx::midiIn. Clears `out`, then appends one
// entry per note-on / note-off / control-change / pitch-bend message (other
// message types are ignored), offsets clamped to [0, numSamples). The buffer is
// already time-ordered, so the result is too.
//
// `midiInputCount` is how many MIDI input pins the node exposes. When > 1 the
// graph has stamped each event's channel with (input-pin index + 1) (see
// MidiChannelStampProcessor), so we recover ScriptMidiEvent::inIndex from the
// channel nibble (clamped to 0..midiInputCount-1). With a single input pin the
// channel carries no routing meaning and inIndex is always 0.
void buildScriptMidiIn(const juce::MidiBuffer& midi, int numSamples,
                       std::vector<ScriptMidiEvent>& out, int midiInputCount = 1);

// Returns true if `lang` can run at `rate` (mirrors the capability matrix).
bool scriptLangSupportsRate(ScriptLang lang, ScriptRate rate);

// Factory. `role` and `rate` let a runtime pick the right entry points / codegen.
// May return nullptr if the language is unavailable in this build (e.g. Lua not
// vendored, or Wasm without wasm3) - callers fall back to Builtin / silence.
std::unique_ptr<IScriptRuntime> makeScriptRuntime(ScriptLang lang,
                                                  ScriptRole role,
                                                  ScriptRate rate);

// Whether a language backend is compiled into this build.
bool scriptLangAvailable(ScriptLang lang);

} // namespace SoundShop

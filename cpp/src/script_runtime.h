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

    // Signal role: output buffer to fill (length numSamples). Null for MIDI.
    float* out = nullptr;

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

    // Which rates this runtime instance supports.
    virtual bool supportsPerSample() const = 0;
    virtual bool supportsPerBlock()  const = 0;

    // Last load/run error (empty if none). Builtin never errors; Lua/Wasm report
    // syntax / load / runtime failures here for the editor to surface.
    virtual std::string getError() const { return {}; }

protected:
    std::function<float(float)> shape;       // shape(pos) sampler (may be empty)
    std::function<float(int)>   noteToFreq;   // note->Hz via project tuning (may be empty)
};

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

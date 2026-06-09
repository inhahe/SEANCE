#pragma once
#include <string>
#include <vector>

namespace SoundShop {

// =============================================================================
// Static-shape expression languages.
//
// Several places in SEANCE let the user author a static 1-D shape with a text
// expression that is *baked once* into a sample buffer at edit time (NOT run
// per audio sample): wavetable Formula layers and spectral magnitude/phase
// curves. Unlike the live MIDI-Script / Signal-Shape nodes (which run on the
// audio thread and offer Built-in / Lua / WebAssembly), these shapes are baked
// off the audio thread, so the trade-offs differ:
//   * Built-in - the pure-C++ expression evaluator (WaveExprParser). Always
//                available, evaluable from any thread.
//   * Lua      - a sandboxed lua_State, created locally per bake. Lets the user
//                use loops/locals (e.g. additive synthesis over many harmonics).
//   * Python   - the shared embedded CPython interpreter. UI-thread only.
// WebAssembly is intentionally NOT offered here: the open-a-binary flow buys
// nothing for a one-shot offline bake.
//
// This is a deliberately separate axis from the live nodes' ScriptLang
// (Built-in / Lua / Wasm): static shapes can run Python (offline) but never
// Wasm; live nodes can run Wasm (real-time) but never Python.
// =============================================================================
enum class ShapeLang { Builtin = 0, Lua = 1, Python = 2 };

// Stable lowercase token used in project-file serialization.
const char* shapeLangKey(ShapeLang lang);
ShapeLang   shapeLangFromKey(const std::string& key);

// Human-readable name for combo boxes ("Built-in", "Lua", "Python").
const char* shapeLangDisplayName(ShapeLang lang);

// Whether the backend is usable in this build/run. Built-in is always true;
// Lua depends on HAS_LUA; Python depends on the interpreter initialising.
bool shapeLangAvailable(ShapeLang lang);

// Bake a user expression/program into N float samples.
//
//   domainRadians == true   -> the loop variable is `x`, swept across one cycle
//                              [0, 2*pi). The result is clamped to [-1, 1].
//                              (wavetable Formula layers)
//   domainRadians == false  -> the loop variable is `f`, swept across the
//                              normalized range [0, 1]. The result is not
//                              clamped. (spectral curves)
//
// Lua/Python sources may be a bare expression in the loop variable
// (e.g. "sin(x)") or a multi-statement body that ends in `return <value>`
// (e.g. an additive-synthesis loop). The same math vocabulary as the built-in
// language is provided (sin, cos, saw, square, triangle, clamp, noise, ...).
//
// Returns true on success. On any failure (syntax error, runtime error,
// interpreter unavailable) returns false, fills `out` with N zeros, and sets
// `error` to a human-readable message.
bool bakeShapeExpr(ShapeLang lang, const std::string& src,
                   bool domainRadians, int N,
                   std::vector<float>& out, std::string& error);

} // namespace SoundShop

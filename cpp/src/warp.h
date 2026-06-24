#pragma once

// =============================================================================
// Waveform shape-bending ("warp") framework.
//
// A *warp* is a real-time, modulatable shaping of a waveform. Warps split into
// two operating domains that matter for how they're applied per sample:
//
//   * Phase-domain  - remap the read position BEFORE the table lookup.
//                     out(p) = cycle( warpPhaseValue(method, p, amount) )
//                     (bend, asym, PWM-skew, sync, phase distortion, remap, VPS)
//
//   * Amplitude-domain - a nonlinear transfer applied to the sample AFTER the
//                     lookup. out = warpAmpValue(method, cycle(p), amount)
//                     (clip, fold, wrap, rectify, quantize, saturate, mirror,
//                      flip, Chebyshev)
//
// Both forms are cheap per-sample functions, so the synth voice can apply them
// live every sample with a *modulated* amount - this is what makes "morph the
// waveform with an oscillator" work (the amount is a node param driven, on
// demand, by a Signal/Param cable via the #88 modulation-pin mechanism).
//
// This file is the registry ("the array of methods to choose from") plus the
// per-sample primitives. Buffer helpers (applyWarpChain) compose the primitives
// for editor preview and per-element baking (Bucket C/B). Pure std, no JUCE, so
// it is trivially unit-testable from the self-test harness.
//
// Buckets (see design notes / REFERENCE.md):
//   A - Transform warps: a function of an arbitrary cycle. Live in this file as
//       per-sample primitives; work on every frame type's output.
//   B - Generator morphs (PWM pulse, sync, FM, CZ-PD): the morph param is part
//       of the wave's *definition* - implemented as layer-shape primitives, not
//       here.
//   C - Representation/element-bound (spectral per-bin, wavelet per-coeff,
//       granular per-grain): applied inside each frame's render.
// =============================================================================

#include <vector>
#include <string>
#include <cstdint>

#include "asset_library.h"   // seed the built-in morph chains into the project store

enum class WarpDomain {
    Phase,        // remap read position
    Amplitude,    // nonlinear transfer on the sample value
    Modulation,   // combine with another signal (Bucket B - placeholder ids)
    Spectral,     // operate on FFT bins (Bucket C)
    Wavelet,      // operate on DWT coefficients (Bucket C)
    Granular      // operate per grain (Bucket C)
};

// Stable ids - serialized in project files, so NEVER renumber existing values;
// append new methods at the end of their group / the enum.
enum class WarpMethod : int {
    None = 0,

    // --- Bucket A : Amplitude-domain (transfer on sample value) ---
    SoftClip      = 1,
    HardClip      = 2,
    Wavefold      = 3,
    Wavewrap      = 4,
    Rectify       = 5,
    Quantize      = 6,   // amplitude quantize / bitcrush
    TubeSat       = 7,   // asymmetric (even-harmonic) saturation
    TapeSat       = 8,   // symmetric (odd-harmonic) saturation
    // 9 reserved (was a planned "Mirror" amplitude method - never shipped /
    // never serialized; left as a hole so the surviving ids stay stable).
    Flip          = 10,
    Chebyshev     = 11,

    // --- Bucket A : Phase-domain (remap read position) ---
    BendPlus      = 20,
    BendMinus     = 21,
    AsymPlus      = 22,
    AsymMinus     = 23,
    PwmSkew       = 24,
    PhaseQuantize = 25,
    PhaseDistortion = 26, // Casio CZ style (generic remap form)
    VectorPhaseShaping = 27,
    Remap         = 28,  // parameterized S-curve (full curve editor: Milestone 5)
    SelfSync      = 29   // read-restart formant sync
};

struct WarpMethodInfo {
    WarpMethod  method;
    WarpDomain  domain;
    const char* name;        // display name in the picker
    bool        recommended; // quality badge (high-quality / popular methods)
    const char* tooltip;     // 1-line description for the picker / node tooltip
    const char* paramName;   // human label for this method's amount param (the
                             // "named morph parameter": "Drive", "Fold", "Width"
                             // ...). Drives the node param / modulation-pin label
                             // and the library picker instead of a bare "Warp N".
                             // Falls back to "Amount" when null/empty.
};

// The full registry - "the array of shapeshifting methods to choose from".
// Grouped by domain; `recommended` drives the quality badge in the UI.
const std::vector<WarpMethodInfo>& warpMethodRegistry();

// Look up a method's static info (or nullptr if unknown).
const WarpMethodInfo* warpMethodInfo(WarpMethod m);

// Convenience.
WarpDomain warpDomainOf(WarpMethod m);
const char* warpMethodName(WarpMethod m);

// Human label for a method's amount param ("Drive", "Fold", "Width", ...) - the
// named morph parameter. Returns "Amount" for None / unknown methods. Used to
// name the node's modulation param + pin and the library picker, so a modulated
// warp reads as "Soft Clip Drive" rather than an opaque "Warp 1".
const char* warpParamLabel(WarpMethod m);

// Resolve a method NAME to its enum (the inverse of warpMethodName), tolerant of
// spelling: case-insensitive, spaces / underscores / parenthetical qualifiers
// ignored, and '+'/'-' read as "plus"/"minus" - so "Soft Clip", "softclip",
// "SoftClip" all map to SoftClip, and "Bend +" / "bend+" / "bendplus" all map to
// BendPlus. Common short aliases are accepted too (fold->Wavefold, wrap->
// Wavewrap, bitcrush->Quantize, vps->VectorPhaseShaping, sync->SelfSync, ...).
// Returns WarpMethod::None for an unknown name (callers treat None as identity,
// so a typo degrades to "no shaping" rather than erroring). This is the bridge
// the scripting languages use so a script can pass a readable name instead of a
// magic integer; the integer enum value is still accepted directly everywhere.
WarpMethod warpMethodFromName(const char* name);

// ---- Standalone Waveshaper effect-node identity ----------------------------
//
// A Waveshaper is a built-in audio Effect node that applies ONE amplitude-
// domain (Bucket A) warp transfer to its input, with a single modulatable
// "Amount" param. Its identity lives in node.script as
//     "__waveshaper:" <token> "__"      e.g. "__waveshaper:softclip__"
// where <token> is a stable, lowercase, separator-free spelling that the
// tolerant warpMethodFromName() parses. ONE node type, ten menu entries (one
// per amplitude method). Phase-domain morphs stay inside the synth (they need
// the read position before the table lookup), so only amplitude methods - pure
// per-sample transfers that work on any incoming audio - get an effect node.
constexpr const char* kWaveshaperScriptPrefix = "__waveshaper:";
constexpr const char* kWaveshaperScriptSuffix = "__";

// The amplitude-domain (Bucket A) methods exposed as Waveshaper nodes, in menu
// order. Mirrors the WarpMethod amplitude group; keep in sync if methods are
// added (id 9 stays a hole - see WarpMethod).
inline const std::vector<WarpMethod>& waveshaperMethods() {
    static const std::vector<WarpMethod> v = {
        WarpMethod::SoftClip, WarpMethod::HardClip, WarpMethod::Wavefold,
        WarpMethod::Wavewrap, WarpMethod::Rectify,  WarpMethod::Quantize,
        WarpMethod::TubeSat,  WarpMethod::TapeSat,  WarpMethod::Flip,
        WarpMethod::Chebyshev
    };
    return v;
}

// Stable serialization token for a Waveshaper method (lowercase, no spaces).
// These are exact aliases warpMethodFromName() already understands, so the
// round-trip is robust. NEVER change an existing token (it's in project files).
inline const char* waveshaperToken(WarpMethod m) {
    switch (m) {
        case WarpMethod::SoftClip:  return "softclip";
        case WarpMethod::HardClip:  return "hardclip";
        case WarpMethod::Wavefold:  return "wavefold";
        case WarpMethod::Wavewrap:  return "wavewrap";
        case WarpMethod::Rectify:   return "rectify";
        case WarpMethod::Quantize:  return "quantize";
        case WarpMethod::TubeSat:   return "tubesat";
        case WarpMethod::TapeSat:   return "tapesat";
        case WarpMethod::Flip:      return "flip";
        case WarpMethod::Chebyshev: return "chebyshev";
        default:                    return "";
    }
}

inline std::string waveshaperScriptFor(WarpMethod m) {
    return std::string(kWaveshaperScriptPrefix) + waveshaperToken(m)
         + kWaveshaperScriptSuffix;
}

inline bool isWaveshaperScript(const std::string& script) {
    return script.rfind(kWaveshaperScriptPrefix, 0) == 0;
}

// Parse the method from a Waveshaper node.script; returns None if it isn't a
// waveshaper script or the token is unknown (callers treat None as an identity
// passthrough, so an unrecognized token degrades to "no shaping" not a crash).
inline WarpMethod waveshaperMethodFromScript(const std::string& script) {
    if (!isWaveshaperScript(script)) return WarpMethod::None;
    std::string body = script.substr(std::string(kWaveshaperScriptPrefix).size());
    const std::string suf = kWaveshaperScriptSuffix;
    if (body.size() >= suf.size()
        && body.compare(body.size() - suf.size(), suf.size(), suf) == 0)
        body.erase(body.size() - suf.size());
    return warpMethodFromName(body.c_str());
}

// ---- Per-sample primitives (the modulatable foundation) --------------------
//
// amount is normalized 0..1 (0 = identity / no effect). Methods that are
// naturally bipolar (bend, asym) are split into +/- variants so every amount
// stays a simple 0..1 knob that the modulation system can drive.

// Phase-domain: map an input phase in [0,1) to a warped read phase in [0,1).
// For non-phase methods this returns p unchanged.
float warpPhaseValue(WarpMethod m, float phase, float amount);

// Amplitude-domain: map an input sample (nominally [-1,1]) to a warped sample.
// For non-amplitude methods this returns x unchanged.
float warpAmpValue(WarpMethod m, float x, float amount);

// ---- Buffer helpers (preview / bake) ---------------------------------------

struct WarpOp {
    WarpMethod method = WarpMethod::None;
    float amount = 0.0f;   // 0..1 morph amount (a node param when modulatable)
    float aux    = 0.0f;   // secondary param (VPS height, etc.); 0 if unused
    bool  enabled = true;
};

// Apply a chain of transform warps to a single-cycle buffer, in order, in place.
// Phase-domain ops resample the cycle through warpPhaseValue (linear interp,
// periodic); amplitude-domain ops map each sample through warpAmpValue. Used by
// the editor live preview and by per-element baking. The live synth path uses
// the per-sample primitives directly instead (so amount can be modulated).
void applyWarpChain(const std::vector<WarpOp>& ops, std::vector<float>& cycle);

// Read a periodic single-cycle buffer at a fractional phase in [0,1) with
// linear interpolation and wraparound. Exposed for the synth voice loop.
float warpReadCycle(const std::vector<float>& cycle, float phase);

// ---- Compact warp-chain (de)serialization ----------------------------------
//
// Shared by every doc that stores a warp chain (layered per-layer "warp=" field
// and doc-level ":warp:" section, spectral / wavelet / granular element chains).
// Grammar:  <count>:<op>:<op>...  where an op is  <method>;<amount>;<aux>;<en> .
// Uses ':' between ops and ';' within an op, so it never collides with the
// ','/'|' field separators those docs use. The leading <count> is advisory -
// decode trusts the actual op tokens, not the count. An empty chain encodes as
// "0"; callers omit the section entirely when the chain is empty (so old
// decoders never see it).
std::string encodeWarpChain(const std::vector<WarpOp>& chain);
std::vector<WarpOp> decodeWarpChain(const std::string& s);

// ---- Built-in morph presets (curated Type-2 / Bucket A chains) --------------
//
// Code-defined starting points for the morph (warp-chain) picker - the Type-2
// analogue of the built-in factory waveform bank. A handful of musically-useful
// chains (warm saturation, wavefold, lo-fi crush, ...) so the morph library
// picker is never empty in a fresh project. These are TEMPLATES, not
// live-reference assets: choosing one COPIES its ops into the editor's chain (the
// frame then owns an independent copy), exactly as a factory waveform is copied
// into a layer. User-saved chains (AssetLibrary MorphAlgorithm) are the
// live-reference assets. All ops are Bucket A (Type-2), so a built-in can never
// introduce a Type-1 wave-defining generator at a summation/transfer stage.
struct BuiltinMorphChain {
    int                 id;          // stable built-in id (kBuiltinMorphIdBase + n)
    const char*         name;        // display name in the morph picker
    const char*         description; // one-line tooltip
    std::vector<WarpOp> ops;         // the Type-2 chain (all Bucket A methods)
};

// Reserved built-in id base - far below AssetLibrary::kUserIdBase (1e6) and the
// morph combo's reserved id 1 (Independent), so a built-in id never collides
// with a user MorphAlgorithm id or the Independent sentinel. (See the "DISJOINT
// ID SPACE" note in asset_library.h.)
constexpr int kBuiltinMorphIdBase = 200000;

// The curated built-in chains, in display order. Stable for the process; the ops
// are assembled once on first call.
const std::vector<BuiltinMorphChain>& builtinMorphChains();

// Look up a built-in chain by id, or nullptr if `id` is not a built-in morph id.
const BuiltinMorphChain* builtinMorphChain(int id);

// True when `id` is in the reserved built-in morph id range (kBuiltinMorphIdBase
// .. AssetLibrary::kUserIdBase). Distinguishes a seeded built-in morph asset from
// a user-published one without consulting the library. Used by the picker (to
// group "Built-in" vs "Saved") and by serialization (built-ins are code-owned and
// must NOT be written to project files - see seedBuiltinMorphLibrary).
bool isBuiltinMorphAssetId(int id);

// Seed the curated built-in morph chains (builtinMorphChains) into a project's
// AssetLibrary as MorphAlgorithm entries at their fixed built-in ids, so the morph
// picker and the Asset Library panel both source them from ONE place: the library.
//
// CODE-OWNED, NOT SERIALIZED. Built-ins are inserted at ids in the reserved
// built-in range (kBuiltinMorphIdBase, below AssetLibrary::kUserIdBase), are
// re-seeded on every new project and every project load, and are skipped by
// project-file serialization. This keeps them improvable across app versions,
// keeps project files free of boilerplate, and makes them effectively
// undeletable (a deletion is undone by the next re-seed) - matching the asset
// library's "DISJOINT ID SPACE" + "divergence = duplicate" design. Idempotent:
// an entry is only inserted if its id is not already present, so calling this
// repeatedly (new project, load, undo restore) never duplicates.
void seedBuiltinMorphLibrary(SoundShop::AssetLibrary& lib);

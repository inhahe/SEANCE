#pragma once
#include <string>

// ---------------------------------------------------------------------------
// glsl_waveform — shared bridge between the factory waveform bank and GLSL
// compute bakes.
//
// Several offline GLSL bake paths (terrain generation, wavetable/curve Formula
// layers) let the user call waveform(<id>, phase) inside their shader to read
// the factory waveform bank on the GPU. GLSL can't take strings, so the helper
// is INTEGER-ONLY: <id> is a waveform's stable entry index (the same integer
// shown in the factory-waveform browser and returned by Lua/Python
// `waveforms["name"]`). The ENTIRE bank is uploaded once per session (globally
// indexed, layout id*512 + sampleIndex) to a cached read-only SSBO at binding 2
// (see glslSetCachedBuffer); the emitted GLSL waveform() function indexes
// straight into it with no source rewriting and no parser. An out-of-range id
// reads as silence (0).
//
// This logic used to live privately in terrain_synth.cpp; it is shared here so
// the curve/wavetable bake path (shape_expr.cpp) gets identical semantics
// without duplicating the upload + codegen.
// ---------------------------------------------------------------------------

namespace SoundShop {

// If `userBody` mentions "waveform" (a cheap substring check, not a parse),
// ensure the factory waveform bank is resident in the cached binding-2 SSBO and
// return the GLSL source for the `float waveform(int id, float phase)` function
// indexing it (caller prepends it to the shader). If the body doesn't use
// waveform(), `outFn` is set empty and the caller emits nothing — the common
// no-waveform bake pays nothing. On a GPU upload failure returns false and sets
// *err. Must run on the GL thread (the same thread all glsl_compute calls use).
bool glslWaveformFn(const std::string& userBody, std::string& outFn,
                    std::string* err = nullptr);

} // namespace SoundShop

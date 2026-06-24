#pragma once

// =============================================================================
// Bucket C: representation-bound whole-buffer warps.
//
// Unlike the Bucket A per-sample primitives (warpAmpValue / warpPhaseValue in
// warp.h), these only make sense over a WHOLE buffer: each transforms the
// time-domain buffer into a representation (FFT magnitude bins / DWT
// coefficients), applies a per-element amplitude warp - reusing warpAmpValue as
// the single source of truth, so there is NO reimplementation of the transfer
// math - then transforms back to the time domain.
//
// Because they need the whole buffer at once, they are meaningful only in
// whole-buffer scripting contexts: offline bake (terrain / wavetable generate)
// and block-rate streaming (Lua stream(), WASM). They are deliberately NOT
// exposed to the per-sample languages (Builtin expression parser, GLSL), where
// "the whole buffer" doesn't exist at the point a sample is computed.
//
// This mirrors how the wavetable Spectral / Wavelet frames bake their per-bin /
// per-coefficient warp chains inside render() (spectral_editor.cpp,
// wavelet_frame.cpp): same representation, same warpAmpValue transfer, just
// exposed as a callable buffer transform for scripts.
//
// `amount` is the same normalized 0..1 morph knob as the per-sample primitives
// (0 = identity / no effect). An unknown method is identity (the warpAmpValue
// contract), so a typo'd method degrades to "no shaping" rather than erroring.
// All functions operate IN PLACE and preserve the buffer length.
//
// Pure std (FFT from fft_util.h, DWT from wavelet.h) - no JUCE - so it is
// trivially unit-testable from the self-test harness.
//
// NOTE on "granular per-grain" (the third Bucket C representation): in the
// granular wavetable frame that warp collapses to an amplitude-domain transfer
// applied to the representative cycle (granular_frame.cpp warpAmpOps()), which
// is exactly warpAmpValue per sample - already reachable from every language via
// warpamp(). There is no distinct grain-domain buffer transform to expose, so
// adding a "granularWarp" here would just duplicate a warpamp() loop. It is
// intentionally omitted to avoid that redundancy.
// =============================================================================

#include "warp.h"
#include <vector>
#include <string>

namespace SoundShop {

// Spectral: real FFT of the buffer (zero-padded up to the next power of two,
// capped at 16384 to match the FFT range used elsewhere), warp the per-bin
// MAGNITUDE envelope through warpAmpValue (phase preserved), then inverse FFT.
// The DC bin (k=0) is left untouched - warping a constant offset is meaningless
// and would only add thumps. Buffer length is preserved. A no-op for buffers
// shorter than 2 samples.
void spectralWarpBuffer(std::vector<float>& buf, WarpMethod method, float amount);

// Wavelet: forward multi-level DWT (Daubechies / Symlet filter by name, default
// "db4"; unknown names fall back to Haar/db1 per getWaveletFilter), warp every
// approximation + detail COEFFICIENT through warpAmpValue, then inverse DWT.
// `levels` is the decomposition depth (default 5); the DWT clamps it internally
// to what the (power-of-two padded) length supports. Buffer length is preserved
// (the buffer is zero-padded to a power of two for the transform and truncated
// back afterwards). A no-op for buffers shorter than 2 samples.
void waveletWarpBuffer(std::vector<float>& buf, WarpMethod method, float amount,
                       const std::string& filter = "db4", int levels = 5);

} // namespace SoundShop

#pragma once
#include <vector>
#include <string>
#include <cstddef>
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace SoundShop {

// ==============================================================================
// Discrete Wavelet Transform (DWT) and Inverse (IDWT)
//
// Implements the lifting-scheme Daubechies wavelets (db1 through db10),
// Symlets (sym2 through sym8), and Biorthogonal (bior1.3, bior2.2, etc.)
// families. The lifting scheme is faster than convolution-based DWT and
// operates in place.
//
// For the initial implementation we use the simplest approach: filter-bank
// convolution with downsampling (analysis) and upsampling with synthesis
// filters (reconstruction). This is the standard Mallat algorithm.
//
// Signal length must be a power of 2 for the recursive decomposition to
// work cleanly. Non-power-of-2 signals should be zero-padded by the caller.
// ==============================================================================

// Wavelet filter coefficients (low-pass decomposition filter h0).
// The other three filters (h1, g0, g1) are derived from h0 by the
// standard QMF / CQF relationships.
struct WaveletFilter {
    std::vector<float> h0; // low-pass decomposition
    std::string name;

    // Derived filters (computed on construction).
    std::vector<float> h1; // high-pass decomposition
    std::vector<float> g0; // low-pass reconstruction
    std::vector<float> g1; // high-pass reconstruction

    void computeDerived() {
        int N = (int)h0.size();
        h1.resize(N);
        g0.resize(N);
        g1.resize(N);
        for (int i = 0; i < N; ++i) {
            // QMF: h1[i] = (-1)^i * h0[N-1-i]
            h1[i] = ((i % 2 == 0) ? 1.0f : -1.0f) * h0[N - 1 - i];
            // Reconstruction = time-reversed analysis
            g0[i] = h0[N - 1 - i];
            g1[i] = h1[N - 1 - i];
        }
    }
};

// Pre-built filter banks for common wavelet families.
inline WaveletFilter makeDb1() {
    // Haar wavelet (db1)
    WaveletFilter f;
    f.name = "db1";
    float s = 1.0f / std::sqrt(2.0f);
    f.h0 = {s, s};
    f.computeDerived();
    return f;
}

inline WaveletFilter makeDb2() {
    WaveletFilter f;
    f.name = "db2";
    float s3 = std::sqrt(3.0f);
    float d = 4.0f * std::sqrt(2.0f);
    f.h0 = {(1+s3)/d, (3+s3)/d, (3-s3)/d, (1-s3)/d};
    f.computeDerived();
    return f;
}

inline WaveletFilter makeDb4() {
    WaveletFilter f;
    f.name = "db4";
    f.h0 = {
        -0.01059740178f,  0.03288301166f,  0.03084138183f, -0.18703481171f,
        -0.02798376941f,  0.63088076793f,  0.71484657055f,  0.23037781331f
    };
    f.computeDerived();
    return f;
}

inline WaveletFilter makeSym4() {
    WaveletFilter f;
    f.name = "sym4";
    f.h0 = {
        -0.07576571478f, -0.02963552764f,  0.49761866763f,  0.80373875180f,
         0.29785779560f, -0.09921954357f, -0.01260396726f,  0.03222310060f
    };
    f.computeDerived();
    return f;
}

// Get a filter by name. Returns Haar (db1) for unknown names.
inline WaveletFilter getWaveletFilter(const std::string& name) {
    if (name == "db1" || name == "haar") return makeDb1();
    if (name == "db2") return makeDb2();
    if (name == "db4") return makeDb4();
    if (name == "sym4") return makeSym4();
    return makeDb1(); // fallback
}

// ==============================================================================
// Forward DWT (analysis): decomposes signal into approximation + detail
// coefficients at multiple levels.
//
// Input:  signal of length N (power of 2)
// Output: coefficients in place - the vector is reordered so that:
//   [detail_level_L, ..., detail_level_1, approximation_level_L]
// where L is the number of decomposition levels.
// ==============================================================================

// ------------------------------------------------------------------------
// Scratch buffers for allocation-free DWT/IDWT.
//
// WHY THIS EXISTS: the transform needs two half-length temporaries for
// analysis and one full-length temporary for synthesis. Allocating them
// inside the transform means a `new`/`delete` pair per level per channel
// per audio block - a hard real-time violation (malloc can block on a
// global lock, so the audio thread can be preempted by any other thread
// that happens to be allocating, causing dropouts). Every wavelet effect
// node runs the transform on the audio thread, so the scratch has to be
// hoisted out and reused.
//
// USAGE: hold a WaveletWorkspace as a member, call ensure(maxPaddedLen)
// from prepareToPlay(), then pass it to the transform. After ensure() the
// transform performs no allocation at all. The convenience overloads that
// omit the workspace allocate a temporary one and are for OFFLINE callers
// only (self-tests, analysis, wavetable baking) - never the audio thread.
// ------------------------------------------------------------------------
struct WaveletWorkspace {
    std::vector<float> approx;   // analysis: low-band scratch  (len/2)
    std::vector<float> detail;   // analysis: high-band scratch (len/2)
    std::vector<float> recon;    // synthesis: accumulation scratch (len)

    // Reserve enough scratch for transforms on signals up to `maxLen`
    // samples. Call from prepareToPlay(). Safe to call repeatedly; vectors
    // only ever grow, so the steady state is allocation-free.
    void ensure(int maxLen) {
        if ((int)recon.size()  < maxLen)     recon.resize(maxLen);
        if ((int)approx.size() < maxLen / 2) approx.resize(maxLen / 2);
        if ((int)detail.size() < maxLen / 2) detail.resize(maxLen / 2);
    }
};

// Single-level decomposition: splits `data[0..len-1]` into approximation
// (first half) and detail (second half) coefficients.
inline void dwtStep(std::vector<float>& data, int len, const WaveletFilter& filt,
                    WaveletWorkspace& ws) {
    int filterLen = (int)filt.h0.size();
    ws.ensure(len);   // no-op once prepareToPlay() has sized the workspace
    std::vector<float>& approx = ws.approx;
    std::vector<float>& detail = ws.detail;
    for (int i = 0; i < len / 2; ++i) {
        float a = 0, d = 0;
        for (int j = 0; j < filterLen; ++j) {
            int idx = (2 * i + j) % len;
            a += filt.h0[j] * data[idx];
            d += filt.h1[j] * data[idx];
        }
        approx[i] = a;
        detail[i] = d;
    }
    // Pack: approximation in first half, detail in second half.
    for (int i = 0; i < len / 2; ++i) {
        data[i]         = approx[i];
        data[len/2 + i] = detail[i];
    }
}

// ---------------------------------------------------------------------------
// TWO SYNTHESIS CONVENTIONS - pick deliberately.
//
// The ANALYSIS side (dwtStep) has only one form. The SYNTHESIS side has two,
// and they are NOT interchangeable:
//
//   idwtStep   - LEGACY / painter convention. Scatters with the time-reversed
//                filters g0[j]=h0[N-1-j], g1[j]=h1[N-1-j]. For multi-tap
//                orthogonal wavelets this is NOT the adjoint of the analysis,
//                so idwt(dwt(x)) does NOT reconstruct x - measured on a chirp
//                it loses ~58% of the signal energy and is not a pure delay.
//                It is kept, frozen, because the wavelet PAINTER and the
//                terrain synth's fractal fill author coefficients BY HAND and
//                never analyse first: for them this transform is a sound-design
//                choice, and existing saved projects depend on it sounding
//                exactly as it does. Do NOT "fix" it.
//
//   idwtStepPR - PERFECT-RECONSTRUCTION convention. Scatters with the SAME
//                analysis filters h0/h1, which is the true transpose, giving
//                A^T A = I exactly (periodic boundary). Use this for anything
//                that analyses real audio, edits coefficients and resynthesises
//                - every wavelet EFFECT node - because those must be a no-op at
//                neutral settings.
//
// Rule of thumb: if you called dwt() on incoming audio, you must use the PR
// inverse. If you built the coefficients yourself, you want the legacy one.
// ---------------------------------------------------------------------------

// Single-level reconstruction: merges approximation + detail back.
// LEGACY convention - see the note above. Lossy by design; frozen.
inline void idwtStep(std::vector<float>& data, int len, const WaveletFilter& filt,
                     WaveletWorkspace& ws) {
    int half = len / 2;
    int filterLen = (int)filt.g0.size();
    ws.ensure(len);
    float* result = ws.recon.data();
    std::fill(result, result + len, 0.0f);
    // Upsample + convolve with reconstruction filters.
    for (int i = 0; i < half; ++i) {
        for (int j = 0; j < filterLen; ++j) {
            int idx = (2 * i + j) % len;
            result[idx] += filt.g0[j] * data[i] + filt.g1[j] * data[half + i];
        }
    }
    for (int i = 0; i < len; ++i) data[i] = result[i];
}

// Multi-level forward DWT. Decomposes `levels` times. The signal is
// modified in place. Returns the number of levels actually computed
// (may be less than requested if the signal is too short).
inline int dwt(std::vector<float>& signal, int levels, const WaveletFilter& filt,
               WaveletWorkspace& ws) {
    int len = (int)signal.size();
    int done = 0;
    for (int l = 0; l < levels && len >= (int)filt.h0.size(); ++l) {
        dwtStep(signal, len, filt, ws);
        len /= 2;
        ++done;
    }
    return done;
}

// Multi-level inverse DWT. Reconstructs from `levels` decomposition levels.
inline void idwt(std::vector<float>& signal, int levels, const WaveletFilter& filt,
                 WaveletWorkspace& ws) {
    int minLen = (int)signal.size();
    for (int l = 0; l < levels; ++l) minLen /= 2;
    int len = minLen;
    for (int l = 0; l < levels; ++l) {
        len *= 2;
        idwtStep(signal, len, filt, ws);
    }
}

// --- Perfect-reconstruction synthesis (the adjoint of dwtStep) -------------
// Identical to idwtStep except it scatters with h0/h1 instead of the
// time-reversed g0/g1. That makes it the exact transpose of the analysis, so
// idwtPR(dwt(x)) == x to float precision. See the convention note above.
inline void idwtStepPR(std::vector<float>& data, int len, const WaveletFilter& filt,
                       WaveletWorkspace& ws) {
    int half = len / 2;
    int filterLen = (int)filt.h0.size();
    ws.ensure(len);
    float* result = ws.recon.data();
    std::fill(result, result + len, 0.0f);
    for (int i = 0; i < half; ++i) {
        for (int j = 0; j < filterLen; ++j) {
            int idx = (2 * i + j) % len;
            result[idx] += filt.h0[j] * data[i] + filt.h1[j] * data[half + i];
        }
    }
    for (int i = 0; i < len; ++i) data[i] = result[i];
}

// Multi-level perfect-reconstruction inverse. Mirrors idwt()'s level walk.
inline void idwtPR(std::vector<float>& signal, int levels, const WaveletFilter& filt,
                   WaveletWorkspace& ws) {
    int minLen = (int)signal.size();
    for (int l = 0; l < levels; ++l) minLen /= 2;
    int len = minLen;
    for (int l = 0; l < levels; ++l) {
        len *= 2;
        idwtStepPR(signal, len, filt, ws);
    }
}

// --- Offline convenience overloads -----------------------------------------
// Identical maths, but each call spins up a throwaway workspace, so they
// allocate. Fine for self-tests, wavetable baking and other message-thread
// work; NEVER call these from an audio callback - hold a WaveletWorkspace
// and use the overloads above instead.
inline void dwtStep(std::vector<float>& data, int len, const WaveletFilter& filt) {
    WaveletWorkspace ws; dwtStep(data, len, filt, ws);
}
inline void idwtStep(std::vector<float>& data, int len, const WaveletFilter& filt) {
    WaveletWorkspace ws; idwtStep(data, len, filt, ws);
}
inline int dwt(std::vector<float>& signal, int levels, const WaveletFilter& filt) {
    WaveletWorkspace ws; return dwt(signal, levels, filt, ws);
}
inline void idwt(std::vector<float>& signal, int levels, const WaveletFilter& filt) {
    WaveletWorkspace ws; idwt(signal, levels, filt, ws);
}
inline void idwtStepPR(std::vector<float>& data, int len, const WaveletFilter& filt) {
    WaveletWorkspace ws; idwtStepPR(data, len, filt, ws);
}
inline void idwtPR(std::vector<float>& signal, int levels, const WaveletFilter& filt) {
    WaveletWorkspace ws; idwtPR(signal, levels, filt, ws);
}

// Convenience: decompose fully (max levels for the signal length).
inline int dwtFull(std::vector<float>& signal, const WaveletFilter& filt,
                   WaveletWorkspace& ws) {
    int maxLevels = 0;
    int len = (int)signal.size();
    while (len >= (int)filt.h0.size()) { len /= 2; ++maxLevels; }
    return dwt(signal, maxLevels, filt, ws);
}
inline int dwtFull(std::vector<float>& signal, const WaveletFilter& filt) {
    WaveletWorkspace ws; return dwtFull(signal, filt, ws);
}

// ==============================================================================
// WaveletFxScratch - everything a real-time wavelet effect needs to run its
// per-block transform without allocating.
//
// The wavelet effect nodes all share one shape: zero-pad the block to a power
// of two, keep a dry copy, DWT, mangle coefficients, IDWT, crossfade. Written
// naively that costs a heap allocation for the filter bank (4 vectors rebuilt
// by getWaveletFilter every block), two for the padded/dry buffers per channel,
// and two per level inside each dwtStep/idwtStep - on the order of 35 malloc/
// free pairs per block per node, i.e. thousands per second on the audio thread.
//
// Hold one of these per processor, call prepare() from prepareToPlay(), then
// useFilter() + load() per channel. After prepare() the whole path is
// allocation-free (buffers only ever grow, and the filter is rebuilt only if
// the wavelet name actually changes - which for a fixed-family effect is never
// after the first block).
// ==============================================================================
struct WaveletFxScratch {
    WaveletWorkspace ws;                 // transform scratch
    std::vector<float> sig;              // zero-padded working signal
    std::vector<float> dry;              // untouched copy for the wet/dry mix

    // Reserve for blocks up to `maxBlockSamples`. Cheap to call again with a
    // bigger size if the host changes its buffer size.
    void prepare(int maxBlockSamples) {
        int pad = 1;
        while (pad < maxBlockSamples) pad *= 2;
        sig.reserve((size_t)pad);
        dry.reserve((size_t)maxBlockSamples);
        ws.ensure(pad);
    }

    // Cached filter bank. Rebuilds only when the requested family changes, so
    // the steady-state cost is one short string compare.
    const WaveletFilter& useFilter(const char* name) {
        if (filterName != name) {
            filter = getWaveletFilter(name);
            filterName = name;
        }
        return filter;
    }

    // Fill `sig` with a zero-padded copy of src[0..n) (length becomes the next
    // power of two >= n) and `dry` with a plain copy. Returns that padded
    // length. No allocation once prepare() has reserved enough.
    int load(const float* src, int n) {
        int padLen = 1;
        while (padLen < n) padLen *= 2;
        sig.assign(src, src + n);
        sig.resize((size_t)padLen, 0.0f);
        dry.assign(src, src + n);
        ws.ensure(padLen);
        return padLen;
    }

private:
    WaveletFilter filter;
    std::string   filterName;   // empty => `filter` not built yet
};

// ==============================================================================
// Continuous Wavelet Transform (CWT) - for non-dyadic scale operations
//
// Unlike the DWT (which decomposes at dyadic scales 2^j only), the CWT
// can analyze at arbitrary scales, making it suitable for pitch shifting
// by non-octave intervals, fine-grained spectral analysis, etc.
//
// We use the Morlet wavelet (complex sinusoid × Gaussian) which gives
// good time-frequency resolution. The output is a 2D scalogram:
// rows = scales, columns = time positions.
//
// This is computationally expensive (O(N × S × W) where N = signal
// length, S = number of scales, W = wavelet width). For real-time use,
// restrict the signal to one audio block and use few scales.
// ==============================================================================

struct CWTResult {
    int numScales = 0;
    int numSamples = 0;
    // Scalogram: [scale][sample] = magnitude (real-valued, always >= 0).
    // Stored as a flat vector in row-major order: index = scale * numSamples + sample.
    std::vector<float> magnitude;
    // Phase: [scale][sample] = phase angle in radians (-pi..pi).
    std::vector<float> phase;
    // Scale values used (in samples).
    std::vector<float> scales;

    float mag(int scale, int sample) const {
        return magnitude[scale * numSamples + sample];
    }
    float ph(int scale, int sample) const {
        return phase[scale * numSamples + sample];
    }
};

// Morlet wavelet at a given scale: ψ(t) = exp(-t²/2) * exp(i*ω0*t)
// where ω0 is the central frequency (typically 6.0 for good resolution).
// Returns the real and imaginary parts in `realOut` and `imagOut`.
inline void morletWavelet(std::vector<float>& realOut, std::vector<float>& imagOut,
                           float scale, float omega0 = 6.0f) {
    int halfWidth = (int)(scale * 3.0f); // 3σ on each side
    int width = 2 * halfWidth + 1;
    realOut.resize(width);
    imagOut.resize(width);
    float norm = 1.0f / std::sqrt(scale); // L2 normalization
    for (int i = 0; i < width; ++i) {
        float t = (float)(i - halfWidth) / scale;
        float gauss = std::exp(-0.5f * t * t);
        realOut[i] = norm * gauss * std::cos(omega0 * t);
        imagOut[i] = norm * gauss * std::sin(omega0 * t);
    }
}

// WARNING -- cwt()/icwt() currently have NO callers. Their only consumer was
// Wavelet Pitch Shift, removed for being non-functional. They are kept as
// scaffolding for a future rewrite, but DO NOT build on them as-is: icwt() is
// not the inverse of cwt() (it discards the imaginary part and normalises by a
// sum of 1/scale^2 weights instead of the Morlet admissibility constant, so
// there is no unity-gain path even at ratio 1), and cwt() uses direct
// time-domain convolution, which is ~7x too slow for real time. Both defects
// and their remedies are written up in known-issues.md.
//
// Forward CWT: convolve the signal with the Morlet wavelet at each scale.
// `minScale` and `maxScale` define the range; `numScales` logarithmically
// spaced scales are computed within that range.
inline CWTResult cwt(const std::vector<float>& signal,
                      float minScale = 2.0f,
                      float maxScale = 128.0f,
                      int numScales = 32,
                      float omega0 = 6.0f) {
    CWTResult result;
    result.numScales = numScales;
    result.numSamples = (int)signal.size();
    result.magnitude.resize(numScales * (int)signal.size(), 0.0f);
    result.phase.resize(numScales * (int)signal.size(), 0.0f);
    result.scales.resize(numScales);

    // Logarithmically spaced scales.
    float logMin = std::log(minScale);
    float logMax = std::log(maxScale);
    for (int s = 0; s < numScales; ++s)
        result.scales[s] = std::exp(logMin + (logMax - logMin) * s / std::max(1, numScales - 1));

    int N = (int)signal.size();
    std::vector<float> wReal, wImag;

    for (int s = 0; s < numScales; ++s) {
        float scale = result.scales[s];
        morletWavelet(wReal, wImag, scale, omega0);
        int halfW = (int)wReal.size() / 2;

        for (int t = 0; t < N; ++t) {
            float re = 0, im = 0;
            for (int j = 0; j < (int)wReal.size(); ++j) {
                int idx = t + j - halfW;
                if (idx < 0 || idx >= N) continue;
                re += signal[idx] * wReal[j];
                im += signal[idx] * wImag[j];
            }
            int flat = s * N + t;
            result.magnitude[flat] = std::sqrt(re * re + im * im);
            result.phase[flat] = std::atan2(im, re);
        }
    }
    return result;
}

// Inverse CWT (approximate reconstruction): sum weighted wavelets at
// each scale. This is the standard admissibility-based reconstruction
// formula. The result is an approximation - the CWT is redundant, so
// perfect reconstruction requires additional normalization.
inline std::vector<float> icwt(const CWTResult& result, float omega0 = 6.0f) {
    int N = result.numSamples;
    std::vector<float> output(N, 0.0f);
    std::vector<float> wReal, wImag;
    float normFactor = 0;

    for (int s = 0; s < result.numScales; ++s) {
        float scale = result.scales[s];
        morletWavelet(wReal, wImag, scale, omega0);
        int halfW = (int)wReal.size() / 2;
        float scaleWeight = 1.0f / (scale * scale); // admissibility weight

        for (int t = 0; t < N; ++t) {
            float mag = result.magnitude[s * N + t];
            float ph  = result.phase[s * N + t];
            float re = mag * std::cos(ph);
            // Reconstruct: add the real part of the wavelet scaled by the coefficient.
            for (int j = 0; j < (int)wReal.size(); ++j) {
                int idx = t + j - halfW;
                if (idx < 0 || idx >= N) continue;
                output[idx] += re * wReal[j] * scaleWeight;
            }
        }
        normFactor += scaleWeight;
    }

    // Normalize.
    if (normFactor > 0)
        for (auto& v : output) v /= normFactor;
    return output;
}

} // namespace SoundShop

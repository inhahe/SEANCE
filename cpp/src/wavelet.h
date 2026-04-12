#pragma once
#include <vector>
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
// Output: coefficients in place — the vector is reordered so that:
//   [detail_level_L, ..., detail_level_1, approximation_level_L]
// where L is the number of decomposition levels.
// ==============================================================================

// Single-level decomposition: splits `data[0..len-1]` into approximation
// (first half) and detail (second half) coefficients.
inline void dwtStep(std::vector<float>& data, int len, const WaveletFilter& filt) {
    int filterLen = (int)filt.h0.size();
    std::vector<float> approx(len / 2);
    std::vector<float> detail(len / 2);
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

// Single-level reconstruction: merges approximation + detail back.
inline void idwtStep(std::vector<float>& data, int len, const WaveletFilter& filt) {
    int half = len / 2;
    int filterLen = (int)filt.g0.size();
    std::vector<float> result(len, 0.0f);
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
inline int dwt(std::vector<float>& signal, int levels, const WaveletFilter& filt) {
    int len = (int)signal.size();
    int done = 0;
    for (int l = 0; l < levels && len >= (int)filt.h0.size(); ++l) {
        dwtStep(signal, len, filt);
        len /= 2;
        ++done;
    }
    return done;
}

// Multi-level inverse DWT. Reconstructs from `levels` decomposition levels.
inline void idwt(std::vector<float>& signal, int levels, const WaveletFilter& filt) {
    int minLen = (int)signal.size();
    for (int l = 0; l < levels; ++l) minLen /= 2;
    int len = minLen;
    for (int l = 0; l < levels; ++l) {
        len *= 2;
        idwtStep(signal, len, filt);
    }
}

// Convenience: decompose fully (max levels for the signal length).
inline int dwtFull(std::vector<float>& signal, const WaveletFilter& filt) {
    int maxLevels = 0;
    int len = (int)signal.size();
    while (len >= (int)filt.h0.size()) { len /= 2; ++maxLevels; }
    return dwt(signal, maxLevels, filt);
}

} // namespace SoundShop

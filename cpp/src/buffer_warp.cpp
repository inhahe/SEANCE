#include "buffer_warp.h"
#include "fft_util.h"
#include "wavelet.h"
#include <cmath>
#include <complex>

namespace SoundShop {

// Round n up to the next power of two, floored at 2 and capped at 16384 (the
// FFT range used by the granular / spectral frame renderers).
static int ceilPow2Capped(int n) {
    int p = 2;
    while (p < n && p < 16384) p <<= 1;
    return p;
}

void spectralWarpBuffer(std::vector<float>& buf, WarpMethod method, float amount) {
    const int size = (int)buf.size();
    if (size < 2) return;

    int n = ceilPow2Capped(size);

    // Zero-pad the time-domain buffer up to the FFT length.
    std::vector<float> padded((size_t)n, 0.0f);
    for (int i = 0; i < size && i < n; ++i) padded[(size_t)i] = buf[(size_t)i];

    FFT fft(n);
    std::vector<FFT::cplx> spectrum;
    fft.forwardReal(padded, spectrum);

    // Warp the per-bin magnitude envelope; preserve phase. DC bin (0) left
    // untouched. warpAmpValue is the single source of truth for the transfer.
    for (size_t k = 1; k < spectrum.size(); ++k) {
        float mag = std::abs(spectrum[k]);
        if (mag <= 1e-20f) continue;          // nothing to reshape
        float warped = warpAmpValue(method, mag, amount);
        if (warped < 0.0f) warped = -warped;  // a magnitude is non-negative
        float scale = warped / mag;
        spectrum[k] *= scale;                 // keep the phase, scale the radius
    }

    std::vector<float> timeOut;
    fft.inverseReal(spectrum, timeOut);

    // Truncate back to the original length (drops the zero-pad tail).
    for (int i = 0; i < size; ++i)
        buf[(size_t)i] = (i < (int)timeOut.size()) ? timeOut[(size_t)i] : 0.0f;
}

// ---- Perfect-reconstruction DWT round trip (local to buffer_warp) ----------
//
// We deliberately do NOT use wavelet.h's dwt()/idwt() here. That pair's synthesis
// step convolves with the TIME-REVERSED filters (g0[j]=h0[N-1-j]); for multi-tap
// orthogonal wavelets that is not the adjoint of the analysis, so idwt(dwt(x)) is
// lossy (it does not reconstruct x). That has never mattered for the wavelet
// PAINTER, which only ever idwt's hand-authored coefficients - the only consumer
// of wavelet.h's idwt - so its convention must stay frozen for project
// compatibility. A forward->warp->inverse round trip, however, requires true
// perfect reconstruction. The correct orthonormal synthesis is the transpose of
// the analysis: scatter with the SAME analysis filters h0/h1 (not reversed),
// which gives A^T A = I exactly (periodic boundary) - so amount 0 is an exact
// identity with no shift. We reuse getWaveletFilter() so the filter COEFFICIENTS
// remain a single source of truth; only the (correct) synthesis convolution
// lives here.
static void dwtStepPR(std::vector<float>& data, int len, const WaveletFilter& f) {
    int N = (int)f.h0.size();
    std::vector<float> approx(len / 2), detail(len / 2);
    for (int i = 0; i < len / 2; ++i) {
        float a = 0.0f, d = 0.0f;
        for (int j = 0; j < N; ++j) {
            int idx = (2 * i + j) % len;
            a += f.h0[j] * data[(size_t)idx];
            d += f.h1[j] * data[(size_t)idx];
        }
        approx[(size_t)i] = a;
        detail[(size_t)i] = d;
    }
    for (int i = 0; i < len / 2; ++i) {
        data[(size_t)i]              = approx[(size_t)i];
        data[(size_t)(len / 2 + i)]  = detail[(size_t)i];
    }
}
static void idwtStepPR(std::vector<float>& data, int len, const WaveletFilter& f) {
    int half = len / 2;
    int N = (int)f.h0.size();
    std::vector<float> result((size_t)len, 0.0f);
    for (int i = 0; i < half; ++i) {
        for (int j = 0; j < N; ++j) {
            int idx = (2 * i + j) % len;            // adjoint: same filters, scatter
            result[(size_t)idx] += f.h0[j] * data[(size_t)i]
                                 + f.h1[j] * data[(size_t)(half + i)];
        }
    }
    for (int i = 0; i < len; ++i) data[(size_t)i] = result[(size_t)i];
}

void waveletWarpBuffer(std::vector<float>& buf, WarpMethod method, float amount,
                       const std::string& filter, int levels) {
    const int size = (int)buf.size();
    if (size < 2) return;
    if (levels < 1) levels = 1;

    int n = ceilPow2Capped(size);

    // Zero-pad to a power of two so the recursive decomposition is clean.
    std::vector<float> coeffs((size_t)n, 0.0f);
    for (int i = 0; i < size && i < n; ++i) coeffs[(size_t)i] = buf[(size_t)i];

    WaveletFilter filt = getWaveletFilter(filter);

    // Forward multi-level analysis (PR convention). Stop early if the length can
    // no longer be halved below the filter length, mirroring wavelet.h's dwt().
    int filterLen = (int)filt.h0.size();
    int len = n, done = 0;
    for (int l = 0; l < levels && len >= filterLen; ++l) {
        dwtStepPR(coeffs, len, filt);
        len /= 2;
        ++done;
    }

    // Warp every coefficient through the amplitude transfer (single source of
    // truth). Coefficients are signed, so warpAmpValue's bipolar shaping applies
    // directly - no |.|-clamp here (unlike the spectral magnitude case).
    for (auto& c : coeffs)
        c = warpAmpValue(method, c, amount);

    // Inverse multi-level synthesis (PR convention), exactly undoing the analysis.
    int minLen = n;
    for (int l = 0; l < done; ++l) minLen /= 2;
    len = minLen;
    for (int l = 0; l < done; ++l) {
        len *= 2;
        idwtStepPR(coeffs, len, filt);
    }

    // Truncate back to the original length (drops the zero-pad tail).
    for (int i = 0; i < size; ++i)
        buf[(size_t)i] = coeffs[(size_t)i];
}

} // namespace SoundShop

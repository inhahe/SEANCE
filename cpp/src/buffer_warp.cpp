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

// ---- Perfect-reconstruction DWT round trip ---------------------------------
//
// A forward->warp->inverse round trip requires TRUE perfect reconstruction, so
// this uses wavelet.h's dwtStep() paired with idwtStepPR() - the PR (adjoint)
// synthesis, which scatters with the same analysis filters h0/h1 and therefore
// satisfies A^T A = I exactly on a periodic boundary. Amount 0 is then an exact
// identity with no shift.
//
// It must NOT use wavelet.h's plain idwtStep(): that is the frozen LEGACY
// painter convention, which convolves with the time-reversed filters and is
// lossy. See the convention note in wavelet.h for which to use when.
//
// (Both PR helpers used to be duplicated here as file-local statics. They now
// live in wavelet.h so the effect nodes can share them, and so there is only
// one copy of the maths to keep correct.)

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
        dwtStep(coeffs, len, filt);
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

#define _USE_MATH_DEFINES
#include "fft_util.h"
#include <cmath>
#include <cassert>

namespace SoundShop {

static bool isPow2(int n) { return n > 0 && (n & (n - 1)) == 0; }

static int intLog2(int n) {
    int k = 0;
    while ((1 << k) < n) ++k;
    return k;
}

FFT::FFT(int nIn) : n(nIn), logN(intLog2(nIn)) {
    assert(isPow2(n) && "FFT size must be a power of two");

    // Precompute forward twiddles: W_n^k = exp(-2*pi*i*k/n) for k=0..n/2-1
    twiddles.resize(n / 2);
    for (int k = 0; k < n / 2; ++k) {
        float theta = -2.0f * (float)M_PI * (float)k / (float)n;
        twiddles[k] = cplx(std::cos(theta), std::sin(theta));
    }

    // Precompute bit-reversal permutation indices.
    bitRev.resize(n);
    for (int i = 0; i < n; ++i) {
        int rev = 0;
        int x = i;
        for (int b = 0; b < logN; ++b) {
            rev = (rev << 1) | (x & 1);
            x >>= 1;
        }
        bitRev[i] = rev;
    }
}

void FFT::transform(cplx* data, bool inverse) const {
    // Bit-reversal reorder (in place).
    for (int i = 0; i < n; ++i) {
        int j = bitRev[i];
        if (j > i) std::swap(data[i], data[j]);
    }

    // Cooley-Tukey butterflies.
    // At stage s, butterfly length = 2^s, half = 2^(s-1).
    for (int s = 1; s <= logN; ++s) {
        int m = 1 << s;         // butterfly size
        int mh = m >> 1;        // half size
        int twiddleStep = n / m;
        for (int k = 0; k < n; k += m) {
            for (int j = 0; j < mh; ++j) {
                cplx w = twiddles[j * twiddleStep];
                if (inverse) w = std::conj(w);
                cplx t = w * data[k + j + mh];
                cplx u = data[k + j];
                data[k + j]      = u + t;
                data[k + j + mh] = u - t;
            }
        }
    }

    if (inverse) {
        float inv = 1.0f / (float)n;
        for (int i = 0; i < n; ++i) data[i] *= inv;
    }
}

// ---- Allocation-free pointer API -------------------------------------------

void FFT::forward(cplx* data) const { transform(data, false); }
void FFT::inverse(cplx* data) const { transform(data, true); }

void FFT::forwardReal(const float* in, cplx* work) const {
    for (int i = 0; i < n; ++i) work[i] = cplx(in[i], 0.0f);
    transform(work, false);
}

void FFT::inverseReal(cplx* work, float* out) const {
    // Mirror the caller-written half-spectrum into the conjugate half. Bins 0
    // and n/2 are their own mirrors, so they are left as the caller set them.
    for (int k = 1; k < n / 2; ++k)
        work[n - k] = std::conj(work[k]);
    transform(work, true);
    for (int i = 0; i < n; ++i) out[i] = work[i].real();
}

// ---- Allocating convenience API --------------------------------------------

void FFT::forward(std::vector<cplx>& data) const {
    assert((int)data.size() == n);
    transform(data.data(), false);
}

void FFT::inverse(std::vector<cplx>& data) const {
    assert((int)data.size() == n);
    transform(data.data(), true);
}

void FFT::forwardReal(const std::vector<float>& in, std::vector<cplx>& out) const {
    assert((int)in.size() == n);
    std::vector<cplx> tmp(n);
    forwardReal(in.data(), tmp.data());
    out.assign(tmp.begin(), tmp.begin() + (n / 2 + 1));
}

void FFT::inverseReal(const std::vector<cplx>& halfSpectrum, std::vector<float>& out) const {
    assert((int)halfSpectrum.size() == n / 2 + 1);
    std::vector<cplx> full(n);
    for (int k = 0; k <= n / 2; ++k) full[k] = halfSpectrum[k];
    out.resize(n);
    inverseReal(full.data(), out.data());
}

// ---------------------------------------------------------------------------
// FFTLadder
// ---------------------------------------------------------------------------

void FFTLadder::prepare(int minSize, int maxSize) {
    if (minSize < 2) minSize = 2;
    if (maxSize < minSize) maxSize = minSize;
    // Idempotent: prepareToPlay can run repeatedly with unchanged bounds (a
    // device change that keeps the same block size), and rebuilding the whole
    // ladder each time would be pure waste.
    if (minSize == preparedMin && maxSize == preparedMax && !bySizeLog2.empty())
        return;

    const int maxExp = intLog2(maxSize);   // rounds up for non-powers of two
    bySizeLog2.clear();
    bySizeLog2.resize((size_t) maxExp + 1);
    for (int e = 1; e <= maxExp; ++e) {
        const int sz = 1 << e;
        if (sz < minSize || sz > maxSize) continue;
        bySizeLog2[(size_t) e] = std::make_unique<FFT>(sz);
    }
    preparedMin = minSize;
    preparedMax = maxSize;
}

const FFT* FFTLadder::forSize(int n) const {
    if (!isPow2(n)) return nullptr;
    const int e = intLog2(n);
    if (e < 0 || e >= (int) bySizeLog2.size()) return nullptr;
    return bySizeLog2[(size_t) e].get();
}

} // namespace SoundShop

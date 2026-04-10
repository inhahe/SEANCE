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

void FFT::transform(std::vector<cplx>& data, bool inverse) const {
    assert((int)data.size() == n);

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
        for (auto& c : data) c *= inv;
    }
}

void FFT::forward(std::vector<cplx>& data) const { transform(data, false); }
void FFT::inverse(std::vector<cplx>& data) const { transform(data, true); }

void FFT::forwardReal(const std::vector<float>& in, std::vector<cplx>& out) const {
    assert((int)in.size() == n);
    std::vector<cplx> tmp(n);
    for (int i = 0; i < n; ++i) tmp[i] = cplx(in[i], 0.0f);
    forward(tmp);
    out.assign(tmp.begin(), tmp.begin() + (n / 2 + 1));
}

void FFT::inverseReal(const std::vector<cplx>& halfSpectrum, std::vector<float>& out) const {
    assert((int)halfSpectrum.size() == n / 2 + 1);
    std::vector<cplx> full(n);
    full[0] = halfSpectrum[0];
    full[n / 2] = halfSpectrum[n / 2];
    for (int k = 1; k < n / 2; ++k) {
        full[k] = halfSpectrum[k];
        full[n - k] = std::conj(halfSpectrum[k]);
    }
    inverse(full);
    out.resize(n);
    for (int i = 0; i < n; ++i) out[i] = full[i].real();
}

} // namespace SoundShop

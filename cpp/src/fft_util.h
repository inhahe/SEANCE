#pragma once
// Small radix-2 Cooley-Tukey FFT/IFFT for power-of-two sizes.
// Self-contained, no dependencies. Used by the spectral synth for
// note-on-time grain/wavetable generation (not audio-thread hot path).
//
// Sizes supported: any power of two from 2 up to 65536.
// Accuracy: IFFT(FFT(x)) == x within ~1e-6 for N <= 4096 in float.

#include <vector>
#include <complex>
#include <cstddef>

namespace SoundShop {

class FFT {
public:
    using cplx = std::complex<float>;

    // Construct for a fixed size. Precomputes twiddle factors and bit-reversal.
    // n must be a power of two.
    explicit FFT(int n);

    int size() const { return n; }

    // In-place forward FFT. data.size() must equal n.
    void forward(std::vector<cplx>& data) const;

    // In-place inverse FFT. Scaled by 1/n so that IFFT(FFT(x)) == x.
    void inverse(std::vector<cplx>& data) const;

    // Convenience: real-input forward. Output has n/2+1 meaningful bins.
    void forwardReal(const std::vector<float>& in, std::vector<cplx>& out) const;

    // Convenience: real-output inverse from half-spectrum (n/2+1 bins).
    // Reconstructs the conjugate half internally.
    void inverseReal(const std::vector<cplx>& halfSpectrum, std::vector<float>& out) const;

private:
    int n = 0;
    int logN = 0;
    std::vector<cplx> twiddles;     // forward twiddles, length n/2
    std::vector<int>  bitRev;       // bit-reversal permutation, length n

    void transform(std::vector<cplx>& data, bool inverse) const;
};

} // namespace SoundShop

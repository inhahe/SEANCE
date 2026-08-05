#pragma once
// Small radix-2 Cooley-Tukey FFT/IFFT for power-of-two sizes.
// Self-contained, no dependencies.
//
// Sizes supported: any power of two from 2 up to 65536.
// Accuracy: IFFT(FFT(x)) == x within ~1e-6 for N <= 4096 in float.
//
// TWO API LAYERS, same maths underneath:
//
//   * Pointer overloads (forward/inverse/forwardReal/inverseReal taking raw
//     pointers) allocate NOTHING. The caller owns every buffer. These are safe
//     on the audio thread and are what the phase-vocoder pitch shifter uses.
//
//   * std::vector overloads are thin convenience wrappers that allocate their
//     output (and, for the real ones, a temporary). They are the original API,
//     used by the spectral synth's note-on-time grain/wavetable generation and
//     by the whole-buffer warps -- NOT audio-thread safe. Kept unchanged so
//     existing callers are unaffected.

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

    // ---- Allocation-free pointer API (audio-thread safe) -------------------
    //
    // Every one of these operates entirely within buffers the caller supplies.
    // `data` / `work` must point at n writable cplx; `in` / `out` at n float.

    // In-place forward FFT over n elements.
    void forward(cplx* data) const;

    // In-place inverse FFT over n elements. Scaled by 1/n, so IFFT(FFT(x))==x.
    void inverse(cplx* data) const;

    // Real-input forward, in a caller-owned n-element cplx buffer. On return
    // work[0 .. n/2] is the half-spectrum; work[n/2+1 .. n-1] holds the
    // (redundant) conjugate half and can be ignored.
    void forwardReal(const float* in, cplx* work) const;

    // Real-output inverse. On entry work[0 .. n/2] is the half-spectrum; the
    // conjugate half is filled in for you, so callers need only write the
    // n/2+1 meaningful bins. work is clobbered; out receives n real samples.
    void inverseReal(cplx* work, float* out) const;

    // ---- Allocating convenience API (NOT audio-thread safe) ---------------

    // In-place forward FFT. data.size() must equal n.
    void forward(std::vector<cplx>& data) const;

    // In-place inverse FFT. Scaled by 1/n so that IFFT(FFT(x)) == x.
    void inverse(std::vector<cplx>& data) const;

    // Real-input forward. Output has n/2+1 meaningful bins.
    void forwardReal(const std::vector<float>& in, std::vector<cplx>& out) const;

    // Real-output inverse from half-spectrum (n/2+1 bins).
    // Reconstructs the conjugate half internally.
    void inverseReal(const std::vector<cplx>& halfSpectrum, std::vector<float>& out) const;

private:
    int n = 0;
    int logN = 0;
    std::vector<cplx> twiddles;     // forward twiddles, length n/2
    std::vector<int>  bitRev;       // bit-reversal permutation, length n

    void transform(cplx* data, bool inverse) const;
};

} // namespace SoundShop

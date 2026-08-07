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
#include <memory>

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

// ---------------------------------------------------------------------------
// FFTLadder - one ready-built FFT per power-of-two size an effect might pick.
//
// Constructing an FFT allocates its twiddle and bit-reversal tables, so an
// effect whose transform size is chosen inside processBlock (from a "FFT Size"
// param, or clamped to the host block size) cannot construct one there without
// allocating on the audio thread.
//
// Rebuilding lazily "only when the size changes" is not good enough either:
// the size changes exactly when the user drags the FFT Size knob, so the
// allocation would land in the middle of the gesture - precisely when a
// dropout is most audible.
//
// So build the whole ladder up front in prepareToPlay and let processBlock
// pick a size with a pointer lookup. The memory is trivial: every size from
// 16 to 4096 together is well under 100 KB.
// ---------------------------------------------------------------------------
class FFTLadder {
public:
    // Build an FFT for each power of two in [minSize, maxSize]. Call from
    // prepareToPlay (or any other non-audio-thread setup path). Re-preparing
    // with the same bounds is a no-op, so calling it every prepareToPlay is free.
    void prepare(int minSize, int maxSize);

    // The FFT for exactly `n`, or nullptr if `n` is outside the prepared range
    // or not a power of two. Never allocates - safe on the audio thread.
    const FFT* forSize(int n) const;

private:
    // Indexed by log2(size); entries outside the prepared range stay null.
    std::vector<std::unique_ptr<FFT>> bySizeLog2;
    int preparedMin = 0, preparedMax = 0;
};

} // namespace SoundShop

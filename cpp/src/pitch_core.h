#pragma once
// ============================================================================
// PhaseVocoderShifter -- in-house, allocation-free, constant-duration pitch
// shifting for the audio thread.
//
// WHY THIS EXISTS
// ---------------
// SEANCE previously had three pitch shifters and all three were broken:
//
//   * Independent Pitch Shift and Formant Pitch Shift resampled each block
//     independently. Resampling changes DURATION, so shifting up by an octave
//     consumed the whole block but produced only half a block of output -- the
//     rest was silence. Measured as a hard 50%-duty-cycle gate at the block
//     rate (~94 Hz at 512/48k): the energy in the last quarter of every block
//     was 0.00000 where a healthy signal measures ~0.25. This is not fixable by
//     adjusting bounds; constant-duration pitch shifting fundamentally needs
//     state that outlives the block, which is exactly what this class holds.
//
//   * Wavelet Pitch Shift snapped to CWT scale indices ~2.5 semitones apart,
//     so small intervals did nothing and large ones landed hundreds of cents
//     off. Tracked separately -- it is not a consumer of this class.
//
// The obvious alternative was to re-base everything on Rubber Band, which was
// vendored at the time. That was the wrong direction: Rubber Band is GPL v2 and
// was linked statically, so it made the whole binary GPL-encumbered. Growing
// that dependency would have deepened a licensing blocker instead of removing
// it. This class is the replacement, and all of the consumers have since been
// moved over -- the Rubber Band dependency is gone from the build entirely.
//
// HOW IT WORKS
// ------------
// Bin-mapping phase vocoder (the classic Bernsee `smbPitchShift` structure),
// not stretch-then-resample. Bin mapping keeps the hop constant on both the
// analysis and synthesis side, which makes the buffering trivial, keeps the
// whole thing allocation-free, and makes it testable in isolation. Consumers
// layer their own transient/band handling on top, so a plain spectral shifter
// is the right primitive to hand them -- with the one exception of formant
// preservation (setFormantPreserve), which is built in because it has to
// happen between the shift and the inverse FFT and cannot be bolted on from
// outside.
//
//   analysis  : window -> FFT -> per-bin magnitude and TRUE frequency, recovered
//               from the phase advance between frames rather than assuming each
//               bin sits at its centre frequency.
//   mapping   : bin k moves to round(k*ratio); magnitudes accumulate where
//               several source bins land on the same destination.
//   synthesis : integrate each destination bin's phase at its new frequency,
//               inverse FFT, window again, overlap-add.
//
// Because the true frequency is carried through the mapping, resolution is NOT
// limited to whole bins -- a one-semitone shift is genuinely a one-semitone
// shift, which is the specific thing the old implementations could not do.
//
// REAL-TIME CONTRACT
// ------------------
// prepare() allocates. reset(), setPitchRatio(), triggerTransient() and
// process() allocate nothing and take no locks. process() may be called with
// any block size, including sizes that vary call to call; it is a sample-driven
// FIFO and has no opinion about block boundaries.
//
// LATENCY
// -------
// latencySamples() == fftSize - hop. Callers that care about phase alignment
// against a dry path must delay the dry path by the same amount.
//
// CHANNELS
// --------
// Mono. Each channel needs its own instance -- sharing one across channels
// would cross-contaminate the phase accumulators and collapse the stereo image.
// ============================================================================

#include "fft_util.h"

#include <vector>
#include <complex>
#include <cmath>
#include <memory>
#include <algorithm>

namespace SoundShop {

// ============================================================================
// LatencyDelay -- a plain N-sample delay line.
//
// Every consumer of PhaseVocoderShifter needs one. The shifter delays its
// output by latencySamples(), and the graph's PDC compensates for that against
// OTHER paths in the node graph -- but it knows nothing about paths INSIDE a
// node. So a node's dry signal, or any parallel unshifted band, must be delayed
// by the same amount by hand or it will be misaligned against the wet path by
// the full latency (32 ms at the default settings, which is a slapback echo,
// not a mix).
// ============================================================================
class LatencyDelay {
public:
    // Allocates. `samples` may be 0, in which case process() is a pass-through.
    void prepare(int samples) {
        delay = std::max(0, samples);
        buf.assign((size_t)std::max(1, delay), 0.0f);
        pos = 0;
    }

    void reset() {
        std::fill(buf.begin(), buf.end(), 0.0f);
        pos = 0;
    }

    int delaySamples() const { return delay; }

    // Allocation-free. in and out may alias (the stored sample is read out
    // before the incoming one overwrites it).
    void process(const float* in, float* out, int n) {
        if (delay <= 0) {
            if (out != in) std::copy(in, in + n, out);
            return;
        }
        for (int i = 0; i < n; ++i) {
            const float held = buf[(size_t)pos];
            buf[(size_t)pos] = in[i];
            out[i] = held;
            if (++pos >= delay) pos = 0;
        }
    }

    size_t capacityBytes() const { return buf.capacity() * sizeof(float); }

private:
    std::vector<float> buf;
    int delay = 0, pos = 0;
};

class PhaseVocoderShifter {
public:
    using cplx = std::complex<float>;

    // Allocates. Call from prepareToPlay, never from processBlock.
    //   fftOrder    -- fftSize = 1 << fftOrder. 11 (2048) is the default: long
    //                  enough to resolve low fundamentals, short enough that
    //                  latency stays around 32 ms and transients stay crisp.
    //   overlapIn   -- frames per fftSize. 4 (75% overlap) is the standard
    //                  Hann choice and the one the normalisation below assumes.
    //   sampleRateIn -- only used to size the cepstral lifter for formant
    //                  preservation (see setFormantPreserve). Everything else
    //                  in this class works in bins and is rate-agnostic.
    void prepare(int fftOrder = 11, int overlapIn = 4, double sampleRateIn = 48000.0) {
        fftOrder = std::clamp(fftOrder, 6, 15);
        overlap  = std::clamp(overlapIn, 2, 8);
        sampleRate = sampleRateIn > 0.0 ? sampleRateIn : 48000.0;

        fftSize = 1 << fftOrder;
        hop     = fftSize / overlap;
        latency = fftSize - hop;

        fft = std::make_unique<FFT>(fftSize);

        window.resize((size_t)fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[(size_t)i] = 0.5f * (1.0f - std::cos(6.28318530718f * (float)i / (float)fftSize));

        // Overlap-add normalisation. The frame is windowed on analysis AND on
        // synthesis, so what overlap-adds is w^2. Sum of Hann^2 across `overlap`
        // evenly spaced hops is a constant; divide it out so unity in is unity
        // out. Computed rather than hardcoded so changing `overlap` stays correct.
        float wsum = 0.0f;
        for (int i = 0; i < overlap; ++i) {
            float w = window[(size_t)(i * hop)];
            wsum += w * w;
        }
        olaScale = (wsum > 1e-9f) ? 1.0f / wsum : 1.0f;

        inFifo.resize((size_t)fftSize);
        outFifo.resize((size_t)fftSize);
        outAccum.resize((size_t)(fftSize * 2));
        work.resize((size_t)fftSize);
        frame.resize((size_t)fftSize);
        const size_t nBins = (size_t)(fftSize / 2 + 1);
        lastPhase.resize(nBins);
        sumPhase.resize(nBins);
        anaMag.resize(nBins);
        anaPhase.resize(nBins);
        anaFreq.resize(nBins);
        synMag.resize(nBins);
        synPhase.resize(nBins);
        // Worst case is a peak every third bin (the peak test looks two bins
        // out on each side), but reserve one slot per bin so no pathological
        // spectrum can ever make push_back reallocate on the audio thread.
        peaks.reserve(nBins);

        // Formant-preservation scratch (see setFormantPreserve). Sized
        // unconditionally so toggling the flag at runtime never allocates.
        env.resize(nBins);
        cepWork.resize((size_t)fftSize);
        // Quefrency cutoff for the cepstral lifter, in samples. Everything
        // below this quefrency is "slowly varying across frequency" and is
        // taken to be the vocal-tract/instrument-body envelope; everything
        // above it is the harmonic comb of the excitation. 1.2 ms sits above
        // the period of the highest pitch we care about separating (~830 Hz)
        // and well below the shortest formant structure, which is the standard
        // compromise. Expressed in seconds so it does not silently change
        // meaning when the sample rate or FFT size changes.
        cepCut = std::clamp((int)(sampleRate * 0.0012), 8, fftSize / 8);

        reset();
    }

    // Formant preservation. Off by default.
    //
    // Plain pitch shifting scales the WHOLE spectrum, envelope included, so a
    // voice shifted up an octave gets its formants dragged up an octave too --
    // the "chipmunk" sound. Preservation puts the original spectral envelope
    // back afterwards, so the pitch moves but the timbre (which vowel it is,
    // which instrument it sounds like) stays put.
    //
    // Method: cepstral liftering. Take the log magnitude spectrum, transform to
    // the cepstrum, keep only the low-quefrency part (see cepCut above), and
    // transform back -- that gives a smooth envelope with the harmonic comb
    // removed. After shifting, bin k holds what came from bin k/ratio, so its
    // envelope is wrong by env(k)/env(k/ratio); multiplying that ratio back in
    // restores the original envelope.
    //
    // Costs two extra FFTs per frame, so it is gated on the flag AND skipped
    // entirely at ratio 1 (where the correction is identically 1). Callers that
    // never enable it pay nothing.
    void setFormantPreserve(bool on) { formantPreserve = on; }
    bool formantPreserveEnabled() const { return formantPreserve; }

    bool isPrepared() const { return fftSize > 0; }

    // Allocation-free. Clears all history; the next latencySamples() of output
    // will be the ramp-up and should be treated as garbage by callers that care.
    void reset() {
        std::fill(inFifo.begin(),    inFifo.end(),    0.0f);
        std::fill(outFifo.begin(),   outFifo.end(),   0.0f);
        std::fill(outAccum.begin(),  outAccum.end(),  0.0f);
        std::fill(lastPhase.begin(), lastPhase.end(), 0.0f);
        std::fill(sumPhase.begin(),  sumPhase.end(),  0.0f);
        rover = latency;
        // The first frame after a reset has no previous phase to advance from,
        // so it is treated as a transient: synthesis phases are seeded from the
        // analysis phases. Without this the partials of the very first frame
        // start at arbitrary relative phases and partially cancel, which shows
        // up as a level dip at the top of every note.
        pendingTransient = true;
    }

    // 1.0 = no shift, 2.0 = up an octave, 0.5 = down an octave.
    // Clamped to +/- two octaves: beyond that the bin mapping folds so much of
    // the spectrum onto a handful of destination bins that the result is noise
    // rather than a pitch, so silently producing garbage would be worse than
    // clamping.
    void setPitchRatio(float r) { ratio = std::clamp(r, 0.25f, 4.0f); }
    float pitchRatio() const { return ratio; }

    static float ratioForSemitones(float semis) { return std::pow(2.0f, semis / 12.0f); }

    // Tell the shifter a transient is arriving. On the next analysis frame the
    // synthesis phases are re-initialised from the analysis phases instead of
    // being integrated forward. Phase integration is what smears attacks in a
    // phase vocoder -- resetting at the attack restores the click. Callers with
    // a transient detector (SEANCE's wavelet detector is exactly this) should
    // drive it; callers without one simply never call it.
    void triggerTransient() { pendingTransient = true; }

    int latencySamples() const { return latency; }
    int fftLength() const { return fftSize; }
    int hopSize() const { return hop; }

    // Total capacity of every internal buffer, in bytes. This exists so the
    // self-test can assert allocation-freedom: capacity is the only externally
    // observable trace of a reallocation, so a test that drives process() hard
    // and watches this number stay constant is a real regression guard against
    // someone reintroducing a per-block resize. Not used by production code.
    size_t capacityBytes() const {
        return (window.capacity() + inFifo.capacity() + outFifo.capacity()
                + outAccum.capacity() + frame.capacity() + lastPhase.capacity()
                + sumPhase.capacity() + anaMag.capacity() + anaPhase.capacity()
                + anaFreq.capacity() + synMag.capacity() + synPhase.capacity()
                + env.capacity()) * sizeof(float)
               + (work.capacity() + cepWork.capacity()) * sizeof(cplx)
               + peaks.capacity() * sizeof(int);
    }

    // Allocation-free. in and out may alias.
    void process(const float* in, float* out, int numSamples) {
        if (!isPrepared()) {
            if (out != in) std::copy(in, in + numSamples, out);
            return;
        }

        for (int i = 0; i < numSamples; ++i) {
            inFifo[(size_t)rover]  = in[i];
            out[i]                 = outFifo[(size_t)(rover - latency)];
            ++rover;

            if (rover >= fftSize) {
                rover = latency;
                processFrame();

                // Publish one hop of finished output and slide the accumulator
                // down by the same amount, so outAccum[0] always corresponds to
                // the start of the next frame to be emitted.
                for (int k = 0; k < hop; ++k)
                    outFifo[(size_t)k] = outAccum[(size_t)k];
                std::copy(outAccum.begin() + hop, outAccum.begin() + hop + fftSize,
                          outAccum.begin());
                std::fill(outAccum.begin() + fftSize, outAccum.begin() + fftSize + hop, 0.0f);

                // Slide the input FIFO down by a hop; rover resumes at `latency`
                // so the next hop of input lands directly after what we kept.
                std::copy(inFifo.begin() + hop, inFifo.begin() + fftSize, inFifo.begin());
            }
        }
    }

private:
    // ---- One analysis/synthesis frame ---------------------------------------
    //
    // This is PEAK-based, not per-bin. The obvious implementation maps each bin
    // independently to round(k*ratio), and it is wrong in a way that is easy to
    // miss: a windowed sinusoid is not one bin, it is a mainlobe spread over
    // three or four adjacent bins with a specific magnitude and phase profile.
    // Mapping each of those bins separately by round(k*ratio) SPREADS them --
    // at ratio 2 neighbours land two bins apart with holes between -- so the
    // reconstructed frame is no longer a windowed sinusoid, and successive
    // frames stop overlap-adding coherently. Measured cost of getting this
    // wrong: -4.5 dB on an octave-up shift, with audible warble.
    //
    // Instead: find the spectral peaks, and translate each peak TOGETHER WITH
    // ITS WHOLE REGION OF INFLUENCE by one integer bin offset. The mainlobe
    // arrives at its new home with its shape intact. Within a region the bins'
    // phases are locked to the peak's, preserving their original relative phase
    // (Laroche-Dolson "identity phase locking") -- which is also what stops the
    // per-bin phase drift that makes plain phase vocoders sound smeared.
    void processFrame() {
        const int half = fftSize / 2;
        const float twoPi = 6.28318530718f;
        // Phase advance a bin sitting exactly at its centre frequency accrues
        // over one hop. Whatever a bin's measured advance exceeds this by is
        // its true deviation from centre.
        const float expected = twoPi * (float)hop / (float)fftSize;
        // Deviation in radians-per-hop -> deviation in bins.
        const float devToBins = (float)fftSize / (twoPi * (float)hop);

        for (int i = 0; i < fftSize; ++i)
            frame[(size_t)i] = inFifo[(size_t)i] * window[(size_t)i];

        fft->forwardReal(frame.data(), work.data());

        const bool resetPhase = pendingTransient;
        pendingTransient = false;

        // ---- Analysis: magnitude, phase, and true frequency per bin --------
        for (int k = 0; k <= half; ++k) {
            const float mag   = std::abs(work[(size_t)k]);
            const float phase = std::arg(work[(size_t)k]);

            float delta = phase - lastPhase[(size_t)k];
            lastPhase[(size_t)k] = phase;
            delta -= (float)k * expected;
            delta = wrapPi(delta);

            anaMag[(size_t)k]   = mag;
            anaPhase[(size_t)k] = phase;
            anaFreq[(size_t)k]  = (float)k + delta * devToBins;   // in bins
        }

        // ---- Peak picking ---------------------------------------------------
        // A bin is a peak if it dominates both neighbours on each side. Looking
        // two bins out rather than one rejects the ripple on the shoulders of a
        // mainlobe, which would otherwise register as its own peak and get
        // translated away from the partial it belongs to.
        peaks.clear();
        for (int k = 2; k <= half - 2; ++k) {
            const float m = anaMag[(size_t)k];
            if (m > anaMag[(size_t)(k - 1)] && m > anaMag[(size_t)(k - 2)]
                && m > anaMag[(size_t)(k + 1)] && m > anaMag[(size_t)(k + 2)])
                peaks.push_back(k);
        }
        if (peaks.empty()) {
            // Degenerate frame (silence, DC, or energy only at the very edges).
            // Treat the loudest bin as the single peak so the whole spectrum
            // still has a region to belong to and nothing is silently dropped.
            int best = 0;
            for (int k = 1; k <= half; ++k)
                if (anaMag[(size_t)k] > anaMag[(size_t)best]) best = k;
            peaks.push_back(best);
        }

        // Envelope must be measured from the ANALYSIS spectrum, before synMag
        // is built, because it is the input's envelope we are restoring.
        const bool doFormant = formantPreserve && std::abs(ratio - 1.0f) > 1e-4f;
        if (doFormant) computeEnvelope();

        std::fill(synMag.begin(),   synMag.end(),   0.0f);
        std::fill(synPhase.begin(), synPhase.end(), 0.0f);

        // ---- Translate each peak's region -----------------------------------
        const int numPeaks = (int)peaks.size();
        for (int i = 0; i < numPeaks; ++i) {
            const int p = peaks[(size_t)i];

            // Region of influence: from the midpoint to the previous peak to the
            // midpoint to the next. Midpoints rather than spectral valleys --
            // valley-finding costs another sweep and, at 75% overlap, makes no
            // audible difference because adjacent partials that close together
            // are already unresolved by the window.
            const int lo = (i == 0)             ? 0    : (peaks[(size_t)(i - 1)] + p) / 2 + 1;
            const int hi = (i == numPeaks - 1)  ? half : (p + peaks[(size_t)(i + 1)]) / 2;

            // Where the peak lands. Rounding to a whole bin is what lets the
            // region move rigidly; the sub-bin remainder is NOT lost, because
            // the synthesis frequency below comes from the peak's true frequency
            // rather than from its bin index.
            const int destPeak = (int)std::lround((float)p * ratio);
            if (destPeak < 0 || destPeak > half) continue;   // shifted past Nyquist
            const int shift = destPeak - p;

            // Advance the peak's synthesis phase at its NEW frequency.
            if (resetPhase)
                sumPhase[(size_t)destPeak] = anaPhase[(size_t)p];
            else
                sumPhase[(size_t)destPeak] +=
                    twoPi * (float)hop * (anaFreq[(size_t)p] * ratio) / (float)fftSize;
            const float peakPhase = sumPhase[(size_t)destPeak];
            const float peakAnaPhase = anaPhase[(size_t)p];

            for (int k = lo; k <= hi; ++k) {
                const int d = k + shift;
                if (d < 0 || d > half) continue;
                synMag[(size_t)d] += anaMag[(size_t)k];
                // Identity phase locking: keep each bin at the same phase
                // offset from its peak that it had in the source.
                synPhase[(size_t)d] = peakPhase + (anaPhase[(size_t)k] - peakAnaPhase);
            }
        }

        // ---- Restore the original spectral envelope -------------------------
        // Bin k of the shifted spectrum carries what was at bin k/ratio, so it
        // arrives wearing env(k/ratio) when it should be wearing env(k).
        if (doFormant) {
            for (int k = 0; k <= half; ++k) {
                if (synMag[(size_t)k] <= 0.0f) continue;
                const float srcEnv = envAt((float)k / ratio, half);
                if (srcEnv <= 1e-9f) continue;
                // Clamp the correction to +/-20 dB. Where the source envelope is
                // near zero (above the shifted signal's Nyquist, or in a deep
                // spectral valley) the raw ratio explodes and would amplify
                // nothing but numerical noise into a loud artefact.
                synMag[(size_t)k] *= std::clamp(env[(size_t)k] / srcEnv, 0.1f, 10.0f);
            }
        }

        for (int k = 0; k <= half; ++k) {
            const float p = synPhase[(size_t)k];
            work[(size_t)k] = cplx(synMag[(size_t)k] * std::cos(p),
                                   synMag[(size_t)k] * std::sin(p));
        }

        fft->inverseReal(work.data(), frame.data());

        for (int i = 0; i < fftSize; ++i)
            outAccum[(size_t)i] += frame[(size_t)i] * window[(size_t)i] * olaScale;
    }

    // Smooth spectral envelope of the current analysis frame, into env[].
    // Cepstral liftering; see setFormantPreserve for why. Allocation-free.
    void computeEnvelope() {
        const int half = fftSize / 2;

        // Log magnitude, floored. The floor matters: log(0) is -inf, and a
        // single -inf anywhere makes the whole cepstrum NaN.
        for (int k = 0; k <= half; ++k)
            cepWork[(size_t)k] = cplx(std::log(std::max(anaMag[(size_t)k], 1e-6f)), 0.0f);
        for (int k = 1; k < half; ++k)
            cepWork[(size_t)(fftSize - k)] = cepWork[(size_t)k];

        // Real, even input -> real, even cepstrum. `inverse` scales by 1/n and
        // `forward` does not, so the pair below is an exact round trip and the
        // only thing that changes the data is the lifter between them.
        fft->inverse(cepWork.data());

        for (int q = cepCut; q <= fftSize - cepCut; ++q)
            cepWork[(size_t)q] = cplx(0.0f, 0.0f);

        fft->forward(cepWork.data());

        for (int k = 0; k <= half; ++k)
            env[(size_t)k] = std::exp(cepWork[(size_t)k].real());
    }

    // env[] sampled at a fractional bin index, linearly interpolated.
    float envAt(float x, int half) const {
        if (x <= 0.0f)            return env[0];
        if (x >= (float)half)     return env[(size_t)half];
        const int   i = (int)x;
        const float f = x - (float)i;
        return env[(size_t)i] * (1.0f - f) + env[(size_t)(i + 1)] * f;
    }

    static float wrapPi(float x) {
        const float twoPi = 6.28318530718f;
        // Phase deltas here are bounded by the bin index times the expected
        // advance, so a subtractive wrap terminates quickly; fmod would be
        // slower and no more accurate.
        while (x >  3.14159265359f) x -= twoPi;
        while (x < -3.14159265359f) x += twoPi;
        return x;
    }

    std::unique_ptr<FFT> fft;

    int fftSize = 0, hop = 0, latency = 0, overlap = 4, rover = 0;
    float ratio = 1.0f;
    float olaScale = 1.0f;
    bool pendingTransient = false;
    bool formantPreserve = false;
    double sampleRate = 48000.0;
    int cepCut = 0;

    std::vector<float> window, inFifo, outFifo, outAccum, frame;
    std::vector<float> lastPhase, sumPhase, anaMag, anaPhase, anaFreq, synMag, synPhase;
    std::vector<float> env;
    std::vector<cplx>  work, cepWork;
    // Peak bin indices for the current frame. clear() + push_back() into a
    // capacity reserved in prepare() -- never grows on the audio thread.
    std::vector<int>   peaks;
};

} // namespace SoundShop

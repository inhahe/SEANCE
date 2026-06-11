#define _USE_MATH_DEFINES
#include "granular_freeze.h"
#include <cmath>
#include <algorithm>

namespace SoundShop {

namespace {
constexpr float kTwoPi = 6.28318530717958648f;
constexpr float kPi    = 3.14159265358979324f;
}

// ---------------------------------------------------------------------------
// (re)initialisation - runs on the first call and whenever the source window,
// its length, the grain length, or the mode changes. Computes the anchor and
// per-mode setup. This is the only place that allocates (FFT twiddles + the
// spectral buffers), mirroring the codebase's lazy-on-note-on pattern; steady
// state is allocation-free.
// ---------------------------------------------------------------------------
void GrainFreezeVoice::initialise(const float* src, int srcLen, int grainLen,
                                  int xfade, float embeddedPitchHz,
                                  double srcRate, double deviceRate,
                                  GranularFreezeMode mode) {
    (void)src; (void)xfade;
    lastSrc = src; lastLen = srcLen; lastGrain = grainLen; lastMode = mode;
    inited = true;

    switch (mode) {
        case GranularFreezeMode::PitchSyncGrains: {
            // Loop exactly one pitch period -> a single repeating cycle =
            // a clean sustained tone at the source's pitch.
            const double rate = (srcRate > 0.0) ? srcRate : deviceRate;
            const double period = (embeddedPitchHz > 0.0f)
                                      ? rate / (double)embeddedPitchHz
                                      : (double)grainLen;
            loopLen = std::max(16, (int)std::lround(period));
            const int needLen = loopLen + loopLen / 2;
            if (srcLen < needLen) { loopLen = 0; }      // unviable -> silence
            else { anchor = (srcLen - needLen) / 2; }
            loopPhase = 0.0f;
            break;
        }
        case GranularFreezeMode::AsyncGranular: {
            // Overlap-add bank of jittered grains near the marker.
            aGrain = std::max(16, grainLen);
            if (srcLen < aGrain + 4) { aGrain = 0; break; }   // unviable
            anchor  = srcLen / 2;                              // roam centre
            aJitter = std::min(aGrain, std::max(0, (srcLen - aGrain) / 2 - 1));
            for (int i = 0; i < kAsync; ++i) {
                aEnv[i]  = (float)i * (float)aGrain / (float)kAsync;  // staggered
                aRead[i] = aEnv[i];                                    // ratio~1 init
                int off  = (int)((frand01() * 2.0f - 1.0f) * (float)aJitter);
                aStart[i] = std::clamp(anchor + off, 0, std::max(0, srcLen - 1));
            }
            break;
        }
        case GranularFreezeMode::SpectralFreeze: {
            // Largest power-of-two FFT size that fits the source, capped at
            // 2048 and floored at 256. Sources shorter than 256 can't be
            // spectrally frozen -> silence.
            specN = 0;
            if (srcLen >= 256) {
                int want = std::min(srcLen, 2048);
                int N = 256;
                while (N * 2 <= want) N *= 2;
                specN = N;
            }
            if (specN <= 0) break;
            specHop = specN / 4;
            anchor  = std::max(0, (srcLen - specN) / 2);

            hann.resize((size_t)specN);
            for (int n = 0; n < specN; ++n)
                hann[(size_t)n] = 0.5f * (1.0f - std::cos(kTwoPi * (float)n / (float)specN));

            // Analysis: one Hann-windowed FFT at the anchor -> magnitudes.
            std::vector<float> ana((size_t)specN);
            for (int n = 0; n < specN; ++n) {
                const int idx = anchor + n;
                const float xv = (idx >= 0 && idx < srcLen) ? src[(size_t)idx] : 0.0f;
                ana[(size_t)n] = xv * hann[(size_t)n];
            }
            if (!fft || fft->size() != specN)
                fft = std::make_unique<FFT>(specN);
            std::vector<FFT::cplx> spec;
            fft->forwardReal(ana, spec);
            specMag.resize((size_t)(specN / 2 + 1));
            for (int k = 0; k <= specN / 2; ++k)
                specMag[(size_t)k] = std::abs(spec[(size_t)k]);

            specFull.assign((size_t)specN, FFT::cplx(0.0f, 0.0f));
            specOverlap.assign((size_t)specN, 0.0f);
            olaSize = specN * 2;
            olaBuf.assign((size_t)olaSize, 0.0f);
            produced = 0;
            readPos  = 0.0;
            break;
        }
        case GranularFreezeMode::CrossfadeLoop:
        default: {
            loopLen = std::max(16, grainLen);
            const int needLen = loopLen + loopLen / 2;
            if (srcLen < needLen) { loopLen = 0; }
            else { anchor = (srcLen - needLen) / 2; }
            loopPhase = 0.0f;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// CrossfadeLoop / PitchSyncGrains: a single fractional playhead loops
// [anchor, anchor+loopLen) and Hann-crossfades the seam against the lookahead
// tail [anchor+loopLen, ...). Resampled by `ratio` for pitch tracking. This is
// byte-for-byte the algorithm both call sites previously duplicated.
// ---------------------------------------------------------------------------
float GrainFreezeVoice::loopSample(const float* src, int srcLen, int xfade, float ratio) {
    if (loopLen <= 0 || srcLen <= 0) return 0.0f;
    const float p = loopPhase;

    const float mainF = (float)anchor + p;
    int   mi0 = (int)mainF;
    float mfr = mainF - (float)mi0;
    if (mi0 < 0)               { mi0 = 0; mfr = 0.0f; }
    else if (mi0 > srcLen - 1) { mi0 = srcLen - 1; mfr = 0.0f; }
    const int mi1 = std::min(mi0 + 1, srcLen - 1);
    float out = src[(size_t)mi0] * (1.0f - mfr) + src[(size_t)mi1] * mfr;

    const int xf = std::max(0, std::min(xfade, loopLen / 2));
    if (xf > 0 && p < (float)xf) {
        const float alpha = 0.5f * (1.0f - std::cos(kPi * p / (float)xf));
        const float tailF = (float)(anchor + loopLen) + p;
        int   ti0 = (int)tailF;
        float tfr = tailF - (float)ti0;
        if (ti0 < 0)               { ti0 = 0; tfr = 0.0f; }
        else if (ti0 > srcLen - 1) { ti0 = srcLen - 1; tfr = 0.0f; }
        const int ti1 = std::min(ti0 + 1, srcLen - 1);
        const float tail = src[(size_t)ti0] * (1.0f - tfr) + src[(size_t)ti1] * tfr;
        out = alpha * out + (1.0f - alpha) * tail;
    }

    float np = p + ratio;
    while (np >= (float)loopLen) np -= (float)loopLen;
    while (np < 0.0f)            np += (float)loopLen;
    loopPhase = np;
    return out;
}

// ---------------------------------------------------------------------------
// AsyncGranular: kAsync overlapping Hann grains. Each grain reads forward
// through the source (advancing by `ratio`) from a randomised start near the
// marker; when its envelope completes it re-triggers at a fresh jittered
// start. The decorrelated grain bank produces a frozen blur / GRM-Freeze
// texture rather than an exact loop.
// ---------------------------------------------------------------------------
float GrainFreezeVoice::asyncSample(const float* src, int srcLen, float ratio) {
    if (aGrain <= 0 || srcLen <= 0) return 0.0f;
    float out = 0.0f;
    for (int i = 0; i < kAsync; ++i) {
        const float env = 0.5f * (1.0f - std::cos(kTwoPi * aEnv[i] / (float)aGrain));
        const float f = (float)aStart[i] + aRead[i];
        int   i0 = (int)f;
        float fr = f - (float)i0;
        if (i0 < 0)               { i0 = 0; fr = 0.0f; }
        else if (i0 > srcLen - 1) { i0 = srcLen - 1; fr = 0.0f; }
        const int i1 = std::min(i0 + 1, srcLen - 1);
        const float s = src[(size_t)i0] * (1.0f - fr) + src[(size_t)i1] * fr;
        out += env * s;

        aEnv[i]  += 1.0f;
        aRead[i] += ratio;
        if (aEnv[i] >= (float)aGrain) {
            aEnv[i] -= (float)aGrain;          // keep fractional remainder
            aRead[i] = aEnv[i] * ratio;        // read consistent with leftover env
            const int off = (int)((frand01() * 2.0f - 1.0f) * (float)aJitter);
            aStart[i] = std::clamp(anchor + off, 0, std::max(0, srcLen - 1));
        }
    }
    // kAsync=4 Hann grains at 75% overlap sum to ~2.0 -> halve for unity.
    return out * 0.5f;
}

// ---------------------------------------------------------------------------
// SpectralFreeze synthesis: build a random-phase spectrum from the frozen
// magnitudes, IFFT in place, window, and overlap-add specHop finished samples
// into the output ring. No allocation (all buffers reused).
// ---------------------------------------------------------------------------
void GrainFreezeVoice::spectralSynthHop() {
    const int half = specN / 2;
    specFull[0]            = FFT::cplx(specMag[0], 0.0f);
    specFull[(size_t)half] = FFT::cplx(specMag[(size_t)half], 0.0f);
    for (int k = 1; k < half; ++k) {
        const float ph = frand01() * kTwoPi;
        const float m  = specMag[(size_t)k];
        const FFT::cplx c(m * std::cos(ph), m * std::sin(ph));
        specFull[(size_t)k]          = c;
        specFull[(size_t)(specN - k)] = std::conj(c);
    }
    fft->inverse(specFull);   // in place, scaled by 1/specN

    for (int n = 0; n < specN; ++n)
        specOverlap[(size_t)n] += specFull[(size_t)n].real() * hann[(size_t)n];

    // Emit specHop finished samples into the ring. specGain compensates the
    // analysis+synth Hann windows and the random-phase incoherent OLA sum so
    // the freeze sits at a musical, non-clipping level.
    constexpr float specGain = 0.8f;
    for (int k = 0; k < specHop; ++k) {
        const long long abs = produced + k;
        olaBuf[(size_t)(((abs % olaSize) + olaSize) % olaSize)] =
            specOverlap[(size_t)k] * specGain;
    }
    produced += specHop;

    // Slide the overlap accumulator left by one hop, zeroing the new tail.
    for (int n = 0; n < specN - specHop; ++n)
        specOverlap[(size_t)n] = specOverlap[(size_t)(n + specHop)];
    for (int n = specN - specHop; n < specN; ++n)
        specOverlap[(size_t)n] = 0.0f;
}

float GrainFreezeVoice::spectralSample(float ratio) {
    if (specN <= 0 || !fft) return 0.0f;
    const long long need = (long long)std::floor(readPos) + 2;
    int guard = 0;
    while (produced < need && guard++ < 256) spectralSynthHop();

    const long long i0 = (long long)std::floor(readPos);
    const float fr = (float)(readPos - (double)i0);
    const size_t a = (size_t)(((i0 % olaSize) + olaSize) % olaSize);
    const size_t b = (size_t)((((i0 + 1) % olaSize) + olaSize) % olaSize);
    const float out = olaBuf[a] * (1.0f - fr) + olaBuf[b] * fr;
    readPos += (double)ratio;
    return out;
}

// ---------------------------------------------------------------------------
// Public entry: (re)initialise if anything changed, then dispatch to the mode.
// ---------------------------------------------------------------------------
float GrainFreezeVoice::process(const float* src, int srcLen, int grainLen,
                                int xfade, float embeddedPitchHz, double srcRate,
                                double deviceRate, float ratio,
                                GranularFreezeMode mode) {
    grainLen = std::max(16, grainLen);
    if (!src || srcLen <= 0) return 0.0f;

    if (!inited || src != lastSrc || srcLen != lastLen
        || grainLen != lastGrain || mode != lastMode) {
        initialise(src, srcLen, grainLen, xfade, embeddedPitchHz,
                   srcRate, deviceRate, mode);
    }

    switch (mode) {
        case GranularFreezeMode::AsyncGranular:   return asyncSample(src, srcLen, ratio);
        case GranularFreezeMode::PitchSyncGrains: return loopSample(src, srcLen, xfade, ratio);
        case GranularFreezeMode::SpectralFreeze:  return spectralSample(ratio);
        case GranularFreezeMode::CrossfadeLoop:
        default:                                  return loopSample(src, srcLen, xfade, ratio);
    }
}

} // namespace SoundShop

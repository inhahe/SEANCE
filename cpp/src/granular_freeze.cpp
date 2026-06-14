#define _USE_MATH_DEFINES
#include "granular_freeze.h"
#include <cmath>
#include <algorithm>

namespace SoundShop {

namespace {
constexpr float kTwoPi = 6.28318530717958648f;
constexpr float kPi    = 3.14159265358979324f;

// Lightweight autocorrelation pitch-period detector for PitchSyncGrains.
// Returns the fundamental period in SOURCE samples, or 0 if no confident
// pitch is found (the caller then falls back to the labelled embeddedPitch).
// Mirrors pitch_detect.h's autocorrelation method but returns the period
// directly and avoids pulling juce_core onto this DSP path. PitchSyncGrains
// snaps its grain origins to this period, so it MUST use the source's real
// pitch - the embedded label (default A4 = 440 Hz) is usually wrong and made
// the mode buzz at 440 Hz instead of tracking the captured tone.
int detectPeriodSamples(const float* src, int srcLen, double rate) {
    if (srcLen < 64 || rate <= 0.0) return 0;
    const int analyzeLen = std::min(srcLen, (int)(rate * 0.1));      // <=100 ms
    const int offset     = std::max(0, (srcLen - analyzeLen) / 2);   // middle
    const float* d       = src + offset;
    const int minLag = std::max(1, (int)(rate / 5000.0));            // <=5 kHz
    // Cap the longest lag at 50 Hz so analyzeLen stays >> maxLag (stable
    // autocorrelation) and the inner loop stays cheap on note-on.
    const int maxLag = std::min(analyzeLen / 2, (int)(rate / 50.0)); // >=50 Hz
    if (maxLag <= minLag) return 0;
    const int span = analyzeLen - maxLag;
    if (span <= 0) return 0;
    double energy = 0.0;
    for (int i = 0; i < span; ++i) energy += (double)d[i] * (double)d[i];
    if (energy < 1e-8) return 0;
    int   bestLag  = 0;
    float bestCorr = 0.0f;
    bool  pastDip  = false;
    for (int lag = minLag; lag <= maxLag; ++lag) {
        double sum = 0.0;
        for (int i = 0; i < span; ++i) sum += (double)d[i] * (double)d[i + lag];
        const float c = (float)(sum / energy);
        if (!pastDip && c < 0.0f) pastDip = true;
        if (pastDip && c > bestCorr) { bestCorr = c; bestLag = lag; }
    }
    if (bestCorr < 0.2f || bestLag < 16) return 0;   // too weak / too short
    return bestLag;
}
}

// ---------------------------------------------------------------------------
// (re)initialisation - runs on the first call and whenever the source window,
// its length, the grain length, or the mode changes. Computes the anchor and
// per-mode setup. This is the only place that allocates (FFT twiddles + the
// spectral buffers), mirroring the codebase's lazy-on-note-on pattern; steady
// state is allocation-free.
// ---------------------------------------------------------------------------
void GrainFreezeVoice::initialise(const float* src, int srcLen, int grainLen,
                                  int windowStart, int windowLen, int grainCount,
                                  int fftSize, int xfade,
                                  float embeddedPitchHz, double srcRate,
                                  double deviceRate, GranularFreezeMode mode) {
    (void)xfade;
    lastSrc = src; lastLen = srcLen; lastGrain = grainLen;
    lastWindow = windowStart; lastWindowLen = windowLen;
    lastGrainCount = grainCount; lastFftSize = fftSize; lastMode = mode;
    inited = true;

    // Resolve the active grain count (<=0 => the historical 4-grain bank) and
    // size the OLA grain banks. resize() only reallocates when the count
    // actually changes, so steady state stays allocation-free. The cloud modes
    // seed these vectors below; the loop/spectral modes leave them untouched.
    numGrains = (grainCount <= 0) ? 4 : std::clamp(grainCount, 2, kMaxGrains);
    aEnv.assign((size_t)numGrains, 0.0f);
    aRead.assign((size_t)numGrains, 0.0f);
    aStart.assign((size_t)numGrains, 0);
    psEnv.assign((size_t)numGrains, 0.0f);
    psRead.assign((size_t)numGrains, 0.0f);
    psStart.assign((size_t)numGrains, 0);

    // Resolve the freeze window (band). bandLen is the window WIDTH, via the
    // shared resolveAutoWindowLen():
    //   * explicit windowLen (>=0)        -> that width.
    //   * kWindowLegacyAuto (-1)          -> grainLen (the original one-grain-wide
    //                                        auto window; keeps pre-band frames
    //                                        byte-for-byte identical).
    //   * kWindowAutoPerMethod (-2)       -> autoWindowMultiplier(mode) x grainLen
    //                                        (cloud modes get roam room, loop/FFT
    //                                        modes stay 1x). New frames default to
    //                                        this.
    // Async / PitchSync floor the width at the grain length (a grain must fit
    // inside the window); CrossfadeLoop (the loop IS the whole window) and
    // SpectralFreeze (FFT of the window) don't granulate, so they only need a
    // handful of samples and let the window shrink below the (now inert) grain
    // length. When the user has placed an explicit band (windowStart >= 0)
    // bandStart is clamped so [bandStart, bandStart+bandLen) stays inside the
    // source. windowStart == -1 leaves `banded` false and every mode below uses
    // its historical centring math, so pre-band frames sound byte-for-byte the
    // same.
    banded    = (windowStart >= 0);
    {
        const bool grainCloud = (mode == GranularFreezeMode::AsyncGranular ||
                                 mode == GranularFreezeMode::PitchSyncGrains);
        const int  rawLen  = resolveAutoWindowLen(windowLen, mode, grainLen);
        const int  loFloor = grainCloud
                                 ? std::min(std::max(16, grainLen), srcLen)
                                 : std::min(16, srcLen);
        bandLen = std::clamp(rawLen, loFloor, std::max(loFloor, srcLen));
    }
    bandStart = banded ? std::clamp(windowStart, 0, std::max(0, srcLen - bandLen))
                       : 0;

    switch (mode) {
        case GranularFreezeMode::PitchSyncGrains: {
            // Pitch-synchronous granular freeze: the pitch-coherent twin of
            // AsyncGranular. Build the SAME stationary scatter of overlapping
            // Hann grains over the window (a freeze, not a forward sweep), but
            // snap every grain origin to the source's pitch-period grid. Because
            // overlapping origins differ by an integer number of periods, the
            // grains read the same waveform phase and sum coherently => a clean,
            // stable pitch instead of async's comb-filtered blur. The many
            // period-aligned grains are the "grains" (plural) the mode is named
            // for; the slight timbral variation between grains drawn from
            // different parts of the note keeps it a living "grain cloud" rather
            // than a dead single cycle.
            //
            // The period is DETECTED from the source (autocorrelation); the
            // embedded pitch is a user-typed label defaulting to A4 = 440 Hz and
            // is usually wrong (trusting it made the mode buzz at 440 Hz). Fall
            // back to the label only when detection finds no confident pitch.
            const double rate = (srcRate > 0.0) ? srcRate : deviceRate;
            int period = detectPeriodSamples(src, srcLen, rate);
            if (period < 16) {
                period = (embeddedPitchHz > 0.0f)
                             ? (int)std::lround(rate / (double)embeddedPitchHz)
                             : grainLen;
            }
            period = std::max(16, period);
            // The grains roam inside the freeze window. When banded, that window
            // is the user's [bandStart, bandStart+bandLen) selection; otherwise
            // it is the whole source (legacy behaviour).
            const int winLo  = banded ? bandStart : 0;
            const int winLen = banded ? bandLen   : srcLen;
            // Grain length = whole number of periods nearest the target, at least
            // two periods so the Hann seam is smooth. Integer-period length keeps
            // the snap grid clean. Banded => the user's grainLen is the grain (the
            // window width is separate roam room - that decoupling is what lets
            // this mode diverge from AsyncGranular). Non-banded (legacy) => ~80 ms.
            const int target = banded
                ? grainLen
                : ((rate > 0.0) ? (int)std::lround(0.08 * rate) : 4 * period);
            int nP = std::max(2, (int)std::lround((double)target / (double)period));
            int grain = nP * period;
            if (grain > winLen) {              // window can't hold the target
                nP = std::max(1, winLen / period);
                grain = nP * period;
            }
            if (grain < 16 || winLen < grain + 2) {
                // Window too short to hold whole-period grains: fall back to the
                // single-cycle crossfade loop (still pitch-locked, static
                // timbre). loopSample handles this path (psGrain == 0).
                psGrain = 0;
                loopLen = period;
                if (banded) {
                    anchor = std::clamp(winLo, 0, std::max(0, srcLen - loopLen));
                } else {
                    const int needLen = loopLen + loopLen / 2;
                    if (srcLen < needLen) { loopLen = 0; }   // unviable -> silence
                    else { anchor = (srcLen - needLen) / 2; }
                }
                loopPhase = 0.0f;
                break;
            }
            psPeriod = period;
            psGrain  = grain;
            psBase   = winLo;
            psRoamHi = winLo + std::max(0, winLen - grain);
            // numGrains staggered voices at (1 - 1/numGrains) overlap (4 grains
            // == the historical 75% OLA), each seeded at a random period-snapped
            // origin.
            for (int i = 0; i < numGrains; ++i) {
                psEnv[(size_t)i]  = (float)i * (float)grain / (float)numGrains;
                psRead[(size_t)i] = psEnv[(size_t)i];      // ratio~1 init
                psStart[(size_t)i] = pitchSyncSnapStart();
            }
            break;
        }
        case GranularFreezeMode::AsyncGranular: {
            // Overlap-add bank of short jittered grains roaming the source.
            // The grain is a SHORT (~80 ms) "blur" grain, not the whole loop:
            // the capture path passes grainLen == the crossfade loop length
            // (up to the entire selection), which is far too long to overlap
            // into a granular blur. We also cap it at srcLen/2 so every grain
            // read stays inside [0, srcLen). The old code used a grainLen-long
            // grain centred at srcLen/2, so each grain read [0.75L, 1.75L) of a
            // 1.5L-sample source - ~25% of every grain ran off the end and
            // clamped to the DC tail, gutting the level (the audition came out
            // ~20 dB below the other modes).
            const double rate = (srcRate > 0.0) ? srcRate : deviceRate;
            // Banded => the grain is the user's grainLen (fit inside the band);
            // the band's extra width is the roam room. Non-banded (legacy) => the
            // grain is a short ~80 ms blur grain because grainLen there doubled
            // as the whole loop length and is far too long to overlap.
            if (banded) {
                aGrain = std::max(16, std::min(grainLen, bandLen));
            } else {
                const int musical = (rate > 0.0) ? (int)std::lround(0.08 * rate)
                                                 : grainLen;
                aGrain = std::max(16, std::min(grainLen,
                                  std::min(musical, srcLen / 2)));
            }
            if (srcLen < aGrain + 4) { aGrain = 0; break; }   // unviable
            // Grains roam inside the freeze window. Banded => the user's
            // [bandStart, bandStart+bandLen) selection; otherwise the whole
            // source (legacy). A grain starting in [aRoamLo, aRoamHi] reads up
            // to aStart + aGrain, kept inside the window so no read clamps to a
            // DC tail.
            if (banded) {
                aRoamLo = std::clamp(bandStart, 0, std::max(0, srcLen - aGrain));
                const int bandHi = std::min(srcLen, bandStart + bandLen);
                aWindowHi = bandHi;
                aRoamHi = std::max(aRoamLo, bandHi - aGrain);
            } else {
                aRoamLo = 0;
                aWindowHi = srcLen;
                aRoamHi = std::max(0, srcLen - aGrain);
            }
            anchor  = (aRoamLo + aRoamHi) / 2;                // roam centre
            aJitter = (aRoamHi - aRoamLo) / 2;                // full in-window roam
            for (int i = 0; i < numGrains; ++i) {
                aEnv[(size_t)i]  = (float)i * (float)aGrain / (float)numGrains; // staggered
                aRead[(size_t)i] = aEnv[(size_t)i];                             // ratio~1 init
                int off  = (int)((frand01() * 2.0f - 1.0f) * (float)aJitter);
                aStart[(size_t)i] = std::clamp(anchor + off, aRoamLo, aRoamHi);
            }
            break;
        }
        case GranularFreezeMode::SpectralFreeze: {
            // FFT size. The window (band when banded, else whole source) bounds
            // it. fftSize == 0 is "auto": largest power of two that fits, capped
            // at 2048 (the historical behaviour, kept byte-identical). A non-zero
            // fftSize is an explicit request: the largest power of two <=
            // min(fftSize, window, 8192), floored at 256. Windows shorter than
            // 256 can't be spectrally frozen -> silence.
            const int winCap = banded ? bandLen : srcLen;
            specN = 0;
            if (winCap >= 256) {
                const int cap = (fftSize > 0)
                                    ? std::min(std::min(fftSize, 8192), winCap)
                                    : std::min(winCap, 2048);
                if (cap >= 256) {
                    int N = 256;
                    while (N * 2 <= cap) N *= 2;
                    specN = N;
                }
            }
            if (specN <= 0) break;
            specHop = specN / 4;
            // Anchor the analysis window at the centre of the freeze region.
            // Banded => centre of the user's band. Otherwise (legacy) the centre
            // of the LOOP region (grainLen), NOT the centre of the whole source:
            // the capture path hands us a source ~1.5x grainLen whose extra half
            // is a decayed lookahead tail; centring on srcLen/2 froze that tail
            // and came out far quieter than the CrossfadeLoop reference.
            if (banded) {
                anchor = std::clamp(bandStart + (bandLen - specN) / 2,
                                    0, std::max(0, srcLen - specN));
            } else {
                anchor = std::min(std::max(0, grainLen / 2 - specN / 2),
                                  std::max(0, srcLen - specN));
            }

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
            // The loop IS the whole freeze window, so the loop length is the
            // band width (windowLen), not the grain. An auto window (windowLen
            // == -1) resolved bandLen to grainLen above, so old frames loop one
            // grain exactly as before.
            loopLen = std::max(16, bandLen);
            if (banded) {
                // Loop region == the user's band. Any crossfade lookahead reads
                // past the band into the source; loopSample clamps the seam to
                // whatever lookahead remains before srcLen.
                if (srcLen < loopLen) { loopLen = 0; }
                else { anchor = std::clamp(bandStart, 0,
                                           std::max(0, srcLen - loopLen)); }
            } else {
                const int needLen = loopLen + loopLen / 2;
                if (srcLen < needLen) { loopLen = 0; }
                else { anchor = (srcLen - needLen) / 2; }
            }
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

    // Clamp the seam to half the loop AND to whatever lookahead remains past
    // the loop end before srcLen. When the band sits at the source's far edge
    // (banded mode) there may be little or no tail; without this clamp the
    // crossfade would read off the end and clamp to a DC sample, thumping the
    // seam. A zero-lookahead band simply gets no crossfade (hard loop).
    const int lookahead = std::max(0, srcLen - (anchor + loopLen));
    const int xf = std::max(0, std::min(std::min(xfade, loopLen / 2), lookahead));
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

// Pick a random grain origin snapped to the pitch-period grid: a multiple of
// psPeriod within [0, psRoamHi]. Snapping every origin to the same grid is what
// makes overlapping grains phase-coherent (they differ by whole periods).
int GrainFreezeVoice::pitchSyncSnapStart() {
    const int span       = std::max(0, psRoamHi - psBase);
    const int periodsMax = (psPeriod > 0) ? span / psPeriod : 0;
    int m = (periodsMax > 0) ? (int)(frand01() * (float)(periodsMax + 1)) : 0;
    if (m > periodsMax) m = periodsMax;            // frand01 can round up to 1.0
    // origin = psBase + m*period keeps every origin on one period grid (offset
    // by psBase), so overlapping grains still differ by a whole number of
    // periods and stay phase-coherent.
    return psBase + m * psPeriod;
}

// ---------------------------------------------------------------------------
// PitchSyncGrains: kPS overlapping Hann grains forming a stationary scatter
// over the window (a freeze), but with every grain origin snapped to the pitch-
// period grid so overlapping grains stay phase-coherent => a clean, stable
// pitch. Identical envelope/overlap machinery to AsyncGranular (hence the same
// *0.5 unity-gain trim for four 75%-overlap Hann grains); the only difference
// is pitchSyncSnapStart() vs. async's free jitter. Resampled by `ratio` for
// MIDI tracking.
// ---------------------------------------------------------------------------
float GrainFreezeVoice::pitchSyncSample(const float* src, int srcLen, float ratio) {
    if (psGrain <= 0 || srcLen <= 0) return 0.0f;
    float out = 0.0f;
    for (int v = 0; v < numGrains; ++v) {
        const float env = 0.5f * (1.0f - std::cos(kTwoPi * psEnv[(size_t)v] / (float)psGrain));
        const float f = (float)psStart[(size_t)v] + psRead[(size_t)v];
        int   i0 = (int)f;
        float fr = f - (float)i0;
        if (i0 < 0)               { i0 = 0; fr = 0.0f; }
        else if (i0 > srcLen - 1) { i0 = srcLen - 1; fr = 0.0f; }
        const int i1 = std::min(i0 + 1, srcLen - 1);
        const float s = src[(size_t)i0] * (1.0f - fr) + src[(size_t)i1] * fr;
        out += env * s;

        psEnv[(size_t)v]  += 1.0f;
        psRead[(size_t)v] += ratio;
        if (psEnv[(size_t)v] >= (float)psGrain) {
            psEnv[(size_t)v] -= (float)psGrain;          // keep fractional remainder
            psRead[(size_t)v] = psEnv[(size_t)v] * ratio;// read consistent with leftover env
            psStart[(size_t)v] = pitchSyncSnapStart();   // fresh period-snapped origin
        }
    }
    // numGrains Hann grains at (1 - 1/numGrains) overlap sum to numGrains/2;
    // 2/numGrains normalises to unity (== 0.5 at the historical 4 grains).
    return out * (2.0f / (float)numGrains);
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
    // A grain advances its read by `ratio` per envelope sample, so over its
    // aGrain-sample life it sweeps aGrain*ratio source samples. When pitched up
    // (ratio > 1) a grain starting at the init-time aRoamHi (= aWindowHi-aGrain)
    // would read aGrain*(ratio-1) samples PAST the window's upper edge, into the
    // half-window lookahead tail (or the source's DC end) - decorrelated content
    // the user never selected, which smears the freeze and was a contributor to
    // "doesn't sound constant". Tighten the re-trigger high bound so the whole
    // sped-up sweep stays inside [aRoamLo, aWindowHi). At ratio <= 1 this equals
    // the original aRoamHi (max(1,ratio) factor), so native-pitch playback is
    // byte-for-byte unchanged.
    const int sweep   = (int)std::ceil((float)aGrain * std::max(1.0f, ratio));
    const int roamHi  = std::min(aRoamHi, std::max(aRoamLo, aWindowHi - sweep));
    float out = 0.0f;
    for (int i = 0; i < numGrains; ++i) {
        const float env = 0.5f * (1.0f - std::cos(kTwoPi * aEnv[(size_t)i] / (float)aGrain));
        const float f = (float)aStart[(size_t)i] + aRead[(size_t)i];
        int   i0 = (int)f;
        float fr = f - (float)i0;
        if (i0 < 0)               { i0 = 0; fr = 0.0f; }
        else if (i0 > srcLen - 1) { i0 = srcLen - 1; fr = 0.0f; }
        const int i1 = std::min(i0 + 1, srcLen - 1);
        const float s = src[(size_t)i0] * (1.0f - fr) + src[(size_t)i1] * fr;
        out += env * s;

        aEnv[(size_t)i]  += 1.0f;
        aRead[(size_t)i] += ratio;
        if (aEnv[(size_t)i] >= (float)aGrain) {
            aEnv[(size_t)i] -= (float)aGrain;          // keep fractional remainder
            aRead[(size_t)i] = aEnv[(size_t)i] * ratio;// read consistent with leftover env
            const int off = (int)((frand01() * 2.0f - 1.0f) * (float)aJitter);
            aStart[(size_t)i] = std::clamp(anchor + off, aRoamLo, roamHi);  // stay in-window (ratio-aware)
        }
    }
    // numGrains Hann grains at (1 - 1/numGrains) overlap sum to ~numGrains/2;
    // 2/numGrains normalises to unity (== 0.5 at the historical 4 grains).
    return out * (2.0f / (float)numGrains);
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
    // the freeze sits at a musical, non-clipping level. 1.0 brings the freeze
    // up to roughly the CrossfadeLoop reference level for a steady tone (0.8
    // left it audibly quiet) while keeping headroom against random-phase peaks.
    constexpr float specGain = 1.0f;
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
                                int windowStart, int windowLen, int grainCount,
                                int fftSize, int xfade,
                                float embeddedPitchHz, double srcRate,
                                double deviceRate, float ratio,
                                GranularFreezeMode mode) {
    grainLen = std::max(16, grainLen);
    if (!src || srcLen <= 0) return 0.0f;

    if (!inited || src != lastSrc || srcLen != lastLen
        || grainLen != lastGrain || windowStart != lastWindow
        || windowLen != lastWindowLen || grainCount != lastGrainCount
        || fftSize != lastFftSize || mode != lastMode) {
        initialise(src, srcLen, grainLen, windowStart, windowLen, grainCount,
                   fftSize, xfade, embeddedPitchHz, srcRate, deviceRate, mode);
    }

    switch (mode) {
        case GranularFreezeMode::AsyncGranular:   return asyncSample(src, srcLen, ratio);
        case GranularFreezeMode::PitchSyncGrains:
            // PSOLA when the window is long enough; single-cycle loop fallback
            // (psGrain == 0) for very short captures.
            return (psGrain > 0) ? pitchSyncSample(src, srcLen, ratio)
                                 : loopSample(src, srcLen, xfade, ratio);
        case GranularFreezeMode::SpectralFreeze:  return spectralSample(ratio);
        case GranularFreezeMode::CrossfadeLoop:
        default:                                  return loopSample(src, srcLen, xfade, ratio);
    }
}

} // namespace SoundShop

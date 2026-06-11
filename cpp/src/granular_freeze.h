#pragma once
#include "granular_frame.h"   // GranularFreezeMode
#include "fft_util.h"         // FFT (self-contained radix-2)
#include <vector>
#include <memory>
#include <cstdint>

namespace SoundShop {

// ===========================================================================
// GrainFreezeVoice - the single shared implementation of all four granular
// "freeze" algorithms.
//
// WHY THIS EXISTS: the capture/freeze dialog auditions a marker spot
// (AudioEngine, audio_engine.cpp) and a held synth note sustains a placed
// granular frame (TerrainSynthProcessor, terrain_synth.cpp). These two MUST
// sound identical - "what you audition is what you play". Historically each
// site carried its own copy of the CrossfadeLoop reader, with comments
// begging future maintainers to keep them in lockstep. This class collapses
// both into one reader so they cannot diverge: add a mode here and both sites
// get it for free.
//
// One GrainFreezeVoice instance == one independent playback stream:
//   * the audition engine owns a single instance (one marker preview at a
//     time);
//   * the synth owns one instance per voice per granular frame (so a 3-way
//     morph across three granular cells runs three streams on each voice).
//
// All four modes sustain the captured window and resample their output by
// `ratio` so a held note tracks MIDI pitch (ratio == 1 plays at the captured
// pitch, exactly what the audition defaults to):
//   CrossfadeLoop   - tape-loop an L-sample window, Hann-crossfade the seam.
//                     Faithful "what does this spot literally sound like".
//   PitchSyncGrains - loop exactly one detected pitch period, so the output
//                     is a single repeating cycle = a clean sustained tone at
//                     the source's pitch.
//   AsyncGranular   - overlap-add bank of short grains at jittered start
//                     positions near the marker. Frozen blur / GRM-Freeze.
//   SpectralFreeze  - FFT a window at the marker, keep the magnitudes,
//                     resynthesise with randomised phases via overlap-add
//                     IFFT. Ethereal pad sustain, decoupled from time-domain
//                     identity.
//
// REAL-TIME NOTES: process() is the audio-thread hot path and allocates
// nothing on the steady-state path. Buffers (and the FFT twiddles for
// SpectralFreeze) are (re)allocated only on (re)initialisation - i.e. the
// first call, or when the source pointer / length / grain length / mode
// changes. That mirrors the codebase's existing lazy-on-note-on allocation
// pattern (granStreams.resize on note-on). Steady state is allocation-free.
// ===========================================================================
struct GrainFreezeVoice {
    // Forget all state so the next process() re-initialises from scratch
    // (used on voice (re)allocation / note-on).
    void reset() { inited = false; }

    // Produce ONE output sample for the current stream.
    //   src/srcLen        - source PCM window (mono) and its length
    //   grainLen          - loop / grain length in samples (>= 16 enforced)
    //   xfade             - crossfade seam length in samples (loop modes)
    //   embeddedPitchHz   - source's natural pitch (for PitchSyncGrains)
    //   srcRate           - source sample rate (0 => assume deviceRate)
    //   deviceRate        - current device sample rate
    //   ratio             - per-output-sample source-read advance (pitch
    //                       tracking); 1.0 == native pitch
    //   mode              - which freeze algorithm to run
    // Returns 0 when the source is too short to sustain (same viability gate
    // across all sites, so silence here == silence in the audition).
    float process(const float* src, int srcLen, int grainLen, int xfade,
                  float embeddedPitchHz, double srcRate, double deviceRate,
                  float ratio, GranularFreezeMode mode);

private:
    // ---- (re)init detection ----
    bool               inited    = false;
    const float*       lastSrc   = nullptr;
    int                lastLen   = -1;
    int                lastGrain = -1;
    GranularFreezeMode lastMode  = GranularFreezeMode::CrossfadeLoop;

    int anchor = 0;          // window start in the source

    // ---- Loop modes (CrossfadeLoop, PitchSyncGrains) ----
    float loopPhase = 0.0f;  // fractional playhead [0, loopLen)
    int   loopLen   = 0;     // resolved loop length (grainLen or pitch period)

    // ---- AsyncGranular ----
    static constexpr int kAsync = 4;        // overlapping grain voices
    float    aEnv[kAsync]  = {0,0,0,0};      // envelope phase [0, grainLen)
    float    aRead[kAsync] = {0,0,0,0};      // source read offset within grain
    int      aStart[kAsync]= {0,0,0,0};      // grain start sample in source
    int      aGrain = 0;                     // grain length in samples
    int      aJitter = 0;                    // +/- start jitter radius
    uint32_t rngState = 0x9E3779B9u;

    // ---- SpectralFreeze ----
    // Phase-vocoder freeze: one analysis FFT at the marker stores the
    // magnitude spectrum; synthesis re-randomises phases each hop, IFFTs, and
    // overlap-adds into a streaming output ring that the per-sample reader
    // resamples by `ratio` for pitch tracking. All buffers are reused across
    // hops so steady-state synthesis allocates nothing.
    std::unique_ptr<FFT>        fft;          // sized specN, lazily built
    int                         specN = 0;    // FFT size (power of two)
    int                         specHop = 0;  // synthesis hop (specN/4)
    std::vector<float>          specMag;      // magnitudes, specN/2+1 bins
    std::vector<FFT::cplx>      specFull;     // specN, reused IFFT scratch
    std::vector<float>          specOverlap;  // specN OLA accumulator
    std::vector<float>          hann;         // specN-sample Hann window
    std::vector<float>          olaBuf;       // output ring (native rate)
    int                         olaSize = 0;  // ring length (2*specN)
    long long                   produced = 0; // abs samples pushed to olaBuf
    double                      readPos  = 0.0;  // fractional native-rate read

    // xorshift32 PRNG - cheap, deterministic, RT-safe (no <random> locks).
    uint32_t nextRand() {
        rngState ^= rngState << 13;
        rngState ^= rngState >> 17;
        rngState ^= rngState << 5;
        return rngState;
    }
    float frand01() { return (float)(nextRand() & 0xFFFFFF) / (float)0x1000000; }

    void initialise(const float* src, int srcLen, int grainLen, int xfade,
                    float embeddedPitchHz, double srcRate, double deviceRate,
                    GranularFreezeMode mode);

    float loopSample(const float* src, int srcLen, int xfade, float ratio);
    float asyncSample(const float* src, int srcLen, float ratio);
    float spectralSample(float ratio);
    void  spectralSynthHop();
};

} // namespace SoundShop

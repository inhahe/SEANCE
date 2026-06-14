#pragma once
#include "wavetable_frame.h"
#include "warp.h"            // WarpOp (amplitude-domain element warp)
#include <vector>
#include <string>
#include <memory>

namespace SoundShop {

// Maximum number of partials an InharmonicFrame can hold. The live voice runs
// one sine oscillator per partial per voice, so this bounds the per-voice cost
// (and matches the additive-bank partial cap so the two additive paths stay
// comparable). The editor caps "+ Partial" at this.
inline constexpr int kInharmonicMaxPartials = 64;

// =============================================================================
// InharmonicFrame  (Milestone 9: the inharmonic stack)
// =============================================================================
//
// A wavetable frame whose sound is an additive stack of sine partials at
// arbitrary, user-authored frequency RATIOS relative to the played note's
// fundamental. Unlike the synth-wide AdditiveBank mode - which DERIVES its
// partials by FFT-ing a single cycle and so is harmonic-by-construction (it can
// only stretch the existing harmonic series with one global knob) - an
// InharmonicFrame lets you place each partial's ratio directly. Ratios need not
// be integers, so the frame can voice genuinely inharmonic spectra: bells,
// mallets, metallic / clangorous tones, detuned stacks, stretched pianos.
//
// Because inharmonic partials do NOT share a common period, the frame can't be
// baked into a single repeating cycle the way Layered / Spectral / Wavelet
// frames are. It is instead played as a LIVE multi-oscillator voice: the synth
// advances one phase accumulator per partial per voice at
// (noteHz * ratio_k), sums amp_k * sin(...), and mixes the result into the
// terrain morph exactly like a granular frame's live grain stream (see
// TerrainSynthProcessor::wtInharmonicFrames / Voice::InhStream). This means an
// inharmonic frame can be morphed against cycle frames and granular frames in
// the same wavetable.
//
// renderRaw(tableSize, out) fallback: sums the partials over ONE fundamental
// period [0,1) into a tableSize cycle (peak-normalised). Because the partials
// are generally inharmonic this snapshot is not perfectly loop-clean, but it is
// a faithful single-period thumbnail of the spectrum and is what the wavetable
// editor draws and what the synth's terrain uses if the live additive voice is
// ever unavailable. The true timbre comes from the live oscillator bank.
//
// Wire format (inside a length-prefixed __wavetable2__ body):
//   <numPartials>;<r0>,<a0>,<p0>;<r1>,<a1>,<p1>;...[;warp:<chain>]
// where each partial is ratio,amp,phase (floats). The optional trailing
// ";warp:<chain>" section carries the Bucket C amplitude-domain element warp
// (omitted when empty, so warp-free frames round-trip byte-identical). The
// sample tokens never contain ';' or the literal "warp", so the section splits
// off unambiguously.
struct InharmonicFrame : public IWavetableFrame {
    struct Partial {
        float ratio = 1.0f;   // frequency multiple of the note fundamental (>0)
        float amp   = 1.0f;   // linear amplitude (0..1 typical; normalised at render)
        float phase = 0.0f;   // initial phase, 0..1 of a cycle
    };
    std::vector<Partial> partials;

    // Bucket C element warp: AMPLITUDE-domain shape-bending ops (clip / fold /
    // saturate / ...) applied to the additive voice's output, in chain order.
    // Like granular, an inharmonic live voice has no periodic phase axis to
    // remap, so only amplitude-domain waveshaping is meaningful here (the
    // editor's picker is restricted to it). The same ops are baked into the
    // representative cycle in renderRaw AND applied to the live oscillator-bank
    // sample in the synth, so the on-screen preview matches the audio.
    std::vector<WarpOp> warpChain;

    // Enabled amplitude-domain ops only - the subset meaningful for an additive
    // stream. Shared by renderRaw and the synth so both apply an identical
    // transfer. A hand-edited file could carry phase-domain ops; they're dropped
    // here rather than applied where they'd have no defined meaning.
    std::vector<WarpOp> warpAmpOps() const {
        std::vector<WarpOp> out;
        for (const auto& op : warpChain)
            if (op.enabled && op.method != WarpMethod::None
                && warpDomainOf(op.method) == WarpDomain::Amplitude)
                out.push_back(op);
        return out;
    }

    InharmonicFrame() = default;

    const char* typeId() const override { return "inharmonic"; }
    void renderRaw(int tableSize, std::vector<float>& out) const override;
    std::string encodeBody() const override;
    bool decodeBody(const std::string& body) override;
    std::unique_ptr<IWavetableFrame> clone() const override;

    // A musically useful starting stack: the classic struck-bar inharmonic
    // ratios (1, 2.76, 5.40, 8.93, 13.34) with rolled-off amplitudes. Gives an
    // immediately-recognisable bell/metallophone tone out of the box.
    static InharmonicFrame defaultBell();

    // Peak-normalisation gain for a partial set: 1 / peak of the summed
    // partials over one fundamental period. renderRaw() applies this same factor
    // inline so the on-screen thumbnail peaks at 1.0; the LIVE oscillator-bank
    // voice in the synth has no normalisation of its own, so it multiplies its
    // summed output by this so a placed/auditioned inharmonic frame is as loud
    // as its editor preview ("what you see = what you hear"). Returns 1.0 for an
    // empty / silent stack. tableSize controls the resolution of the peak scan.
    static float normGainFor(const std::vector<Partial>& partials,
                             int tableSize = 2048);
};

} // namespace SoundShop

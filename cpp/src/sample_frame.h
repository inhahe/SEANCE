#pragma once
#include "wavetable_frame.h"
#include <vector>
#include <string>
#include <memory>

namespace SoundShop {

// SampleFrame holds one cycle of PCM audio, captured from project playback
// (or any other arbitrary source) and conditioned to be loop-perfect at
// its stored cycle length.
//
// "Loop-perfect" here means the spectrum has been forced to integer-bin
// amplitudes via an FFT round-trip at capture time, so reading the cycle
// repeatedly produces no boundary discontinuity. The synth's terrain
// renderer treats the cycle exactly like a Layered / Spectral / Wavelet
// frame's render() output - same tableSize contract, same playback path -
// so a SampleFrame can sit alongside other frame types in the same
// wavetable and morph with them freely.
//
// To get "alive" timbres from project audio, the capture dialog produces
// many SampleFrames (one per cycle of the source) and lays them out along
// the wavetable's Position dimension. The user then modulates Position
// (LFO / envelope) to sweep through the captured cycles - which gives
// back the cycle-to-cycle evolution of the source. This is the same
// technique modern wavetable synths (Serum, Vital, etc.) use for their
// sample-derived "noise" frames.
//
// Wire format used in __wavetable2__ frames is:
//   <storedSize>;<embeddedPitchHz>;<c0,c1,...,c{N-1}>
// where storedSize == N (number of float samples) and embeddedPitchHz is
// the detected source pitch at capture time, kept for future features
// (e.g. note-rate auto-sweep). The container length-prefixes the body so
// the ',' and ';' inside the sample list are safe.
struct SampleFrame : public IWavetableFrame {
    // The one cycle. Length is the canonical stored size - typically the
    // wavetable's tableSize at capture time (2048 by default). Values in
    // roughly [-1, 1]; render() peak-normalises before handing off.
    std::vector<float> cycle;

    // Source pitch at capture time, in Hz. 0 means "not measured / not
    // applicable". Not used by render() in v1 - reserved for future use
    // (note-rate auto-sweep, "transpose to root" feature).
    float embeddedPitchHz = 0.0f;

    SampleFrame() = default;
    explicit SampleFrame(std::vector<float> cycleData, float pitchHz = 0.0f)
        : cycle(std::move(cycleData)), embeddedPitchHz(pitchHz) {}

    const char* typeId() const override { return "sample"; }
    void render(int tableSize, std::vector<float>& out) const override;
    std::string encodeBody() const override;
    bool decodeBody(const std::string& body) override;
    std::unique_ptr<IWavetableFrame> clone() const override;

    // Capture-time conditioning: round-trip `cycle` through its own
    // spectrum (integer-bin amplitudes only), forcing it to be perfectly
    // periodic at its current length. After this call, looping the cycle
    // produces no boundary click. Safe to call on any non-empty cycle;
    // a no-op on empty input. Used by the capture dialog before storing
    // the frame so the loop is clean regardless of how the cycle was
    // sliced out of the source.
    void cleanLoopBoundaries();

    // Default empty cycle of the canonical size, filled with a single
    // sine so it doesn't render as silence if instantiated by the
    // type-id registry on decode failure.
    static SampleFrame defaultEmpty(int storedSize = 2048);
};

} // namespace SoundShop

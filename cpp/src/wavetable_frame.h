#pragma once
#include <vector>
#include <string>
#include <memory>

namespace SoundShop {

// =============================================================================
// IWavetableFrame
// =============================================================================
//
// Abstract single-cycle waveform source. Each wavetable frame produces one
// cycle of audio (length = tableSize), which the synth then plays as one
// slice of an N-D wavetable.
//
// The interface deliberately stays small: render the cycle, identify the
// concrete type, serialise to/from a body string, and clone. Everything else
// is concrete-type-specific (LayeredWaveform has layers; future SpectralFrame
// will have FFT bin curves; future WaveletFrame will have DWT coefficients).
//
// Phase 1: only LayeredWaveform implements this. WavetableDoc still holds
// vector<LayeredWaveform> directly.
//
// Phase 2: WavetableDoc will move to vector<unique_ptr<IWavetableFrame>> so
// a single wavetable can mix layered, spectral and wavelet frames. At that
// point the script format becomes __wavetable2__: with length-prefixed
// per-frame bodies (length-prefixing is required because some body
// encodings - e.g. waveletpaint - use ':' internally).
//
// typeId() values are stable wire-format tags used by __wavetable2__:
//   "layered"  - LayeredWaveform (current)
//   "spectral" - SpectralCurve pair (planned)
//   "wavelet"  - DWT coefficients (planned)
class IWavetableFrame {
public:
    virtual ~IWavetableFrame() = default;

    // Stable identifier of the concrete subclass, used as a tag in the
    // __wavetable2__ encoding. Must be a short ASCII token with no ':',
    // '|', ';' or '@' (those are the wavetable-format delimiters).
    virtual const char* typeId() const = 0;

    // Per-frame output gain, applied on TOP of whatever the concrete type
    // renders (see render() below). Default 1.0 = unchanged. Every frame
    // type peak-normalises its raw cycle (LayeredWaveform to peak 1.0,
    // SampleFrame/SpectralFrame/... likewise), which means without this knob
    // every waveform in a wavetable plays back at the SAME loudness - there
    // is no way to make one frame quieter or louder than its neighbours, and
    // morphs between frames carry no volume contour. `gain` is the post-
    // normalisation scalar that fixes that: it actually changes the rendered
    // samples, so it shows up in the editor preview, in the baked synth
    // terrain, and in every cross-frame morph. Lives on the base class so it
    // applies uniformly to every frame type and is serialised once by the
    // wavetable container (WavetableDoc), not per-subclass.
    float gain = 1.0f;

    // Render one cycle of the waveform into `out`, WITHOUT the per-frame
    // `gain` applied. Implementations resize `out` to `tableSize` and fill
    // it with samples in [-1, 1]. Peak-normalisation policy is up to the
    // concrete type; the synth does not re-normalise. External callers should
    // use render() (below), not this - this is the gain-free primitive.
    virtual void renderRaw(int tableSize, std::vector<float>& out) const = 0;

    // Public render entry point: renderRaw() followed by the per-frame gain.
    // ALL consumers (synth bake, editor preview, scatter/grid morph blend)
    // call this, so a non-unity gain is reflected everywhere the frame is
    // heard or drawn. Non-virtual on purpose - the gain rule is identical for
    // every frame type, so it lives here once instead of in each subclass.
    void render(int tableSize, std::vector<float>& out) const {
        renderRaw(tableSize, out);
        if (gain != 1.0f)
            for (auto& v : out) v *= gain;
    }

    // Serialise the frame body (no type tag, no length prefix - just the
    // raw body string). The container is responsible for wrapping this
    // with the typeId and a length so the body can be safely embedded
    // even when it contains delimiter characters.
    virtual std::string encodeBody() const = 0;

    // Parse a body string previously produced by encodeBody() of the same
    // concrete type. Returns false on malformed input.
    virtual bool decodeBody(const std::string& body) = 0;

    // Deep copy. Used when frames are duplicated (e.g. + Frame button
    // copies the current frame, scatter <-> grid conversion moves frames
    // between containers).
    virtual std::unique_ptr<IWavetableFrame> clone() const = 0;
};

} // namespace SoundShop

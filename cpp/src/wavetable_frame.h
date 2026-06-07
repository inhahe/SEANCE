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

    // Render one cycle of the waveform into `out`. Implementations should
    // resize `out` to `tableSize` and fill it with samples in [-1, 1].
    // Peak normalisation policy is up to the concrete type; the synth
    // does not re-normalise.
    virtual void render(int tableSize, std::vector<float>& out) const = 0;

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

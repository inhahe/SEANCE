#pragma once
#include "wavetable_frame.h"
#include "wavelet_paint.h"   // encodeWaveletPaint / decodeWaveletPaint / waveletPaintToWaveform
#include <vector>
#include <string>
#include <memory>

namespace SoundShop {

// WaveletFrame wraps a DWT coefficient grid as an IWavetableFrame so a
// wavelet-painted shape can sit alongside layered and spectral frames in a
// mixed-type wavetable.
//
// The wire body used in __wavetable2__ is everything after the
// "__waveletpaint__:" prefix of the legacy script format, i.e.
// "<filter>:<numLevels>:<totalSize>:<c0,...,cN-1>". That body uses ':'
// internally, which is why the wavetable container has to length-prefix it
// instead of splitting on ':' to find frame boundaries.
struct WaveletFrame : public IWavetableFrame {
    std::vector<float> coefficients;
    int         numLevels = kWaveletPaintDefaultLevels;
    int         totalSize = kWaveletPaintDefaultSize;
    std::string filterName = kWaveletPaintDefaultFilter;

    WaveletFrame() = default;

    const char* typeId() const override { return "wavelet"; }
    void render(int tableSize, std::vector<float>& out) const override;
    std::string encodeBody() const override;
    bool decodeBody(const std::string& body) override;
    std::unique_ptr<IWavetableFrame> clone() const override;

    // Create a default frame: all zeros at the default level/filter/size.
    static WaveletFrame defaultEmpty();
};

} // namespace SoundShop

#include "wavelet_frame.h"
#include <algorithm>

namespace SoundShop {

void WaveletFrame::render(int tableSize, std::vector<float>& out) const {
    // The wavelet coefficient grid always IDWTs to exactly totalSize
    // samples; we resample (nearest-neighbour) to the requested tableSize
    // so a wavelet frame fits into a wavetable whose other frames may use
    // a different tableSize. In practice the editor pins the doc's
    // tableSize to match all frames, so this resample is usually identity.
    auto wave = waveletPaintToWaveform(coefficients, numLevels, filterName);
    if (tableSize <= 0) { out.clear(); return; }
    if ((int)wave.size() == tableSize) { out = std::move(wave); return; }

    out.assign((size_t)tableSize, 0.0f);
    if (wave.empty()) return;

    const float scale = (float)wave.size() / (float)tableSize;
    for (int i = 0; i < tableSize; ++i) {
        int src = std::min((int)wave.size() - 1, (int)(i * scale));
        out[i] = wave[(size_t)src];
    }
}

std::string WaveletFrame::encodeBody() const {
    // encodeWaveletPaint emits the full "__waveletpaint__:..." form. Strip
    // the prefix so the wavetable container can length-prefix the body.
    auto full = encodeWaveletPaint(coefficients, numLevels, filterName);
    const std::string prefix = "__waveletpaint__:";
    if (full.rfind(prefix, 0) == 0) return full.substr(prefix.size());
    return full;
}

bool WaveletFrame::decodeBody(const std::string& body) {
    return decodeWaveletPaint("__waveletpaint__:" + body,
                              coefficients, numLevels, totalSize, filterName);
}

std::unique_ptr<IWavetableFrame> WaveletFrame::clone() const {
    return std::make_unique<WaveletFrame>(*this);
}

WaveletFrame WaveletFrame::defaultEmpty() {
    WaveletFrame f;
    f.coefficients.assign((size_t)f.totalSize, 0.0f);
    return f;
}

} // namespace SoundShop

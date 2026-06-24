#include "wavelet_frame.h"
#include <algorithm>

namespace SoundShop {

void WaveletFrame::renderRaw(int tableSize, std::vector<float>& out) const {
    // The wavelet coefficient grid always IDWTs to exactly totalSize
    // samples; we resample (nearest-neighbour) to the requested tableSize
    // so a wavelet frame fits into a wavetable whose other frames may use
    // a different tableSize. In practice the editor pins the doc's
    // tableSize to match all frames, so this resample is usually identity.
    // Per-coefficient warp (Bucket C) is baked here: warp the coefficient grid
    // before the IDWT so the shaping happens in the wavelet domain.
    std::vector<float> coeffs = coefficients;
    if (!warpChain.empty()) applyWarpChain(warpChain, coeffs);
    auto wave = waveletPaintToWaveform(coeffs, numLevels, filterName);
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
    std::string b = (full.rfind(prefix, 0) == 0) ? full.substr(prefix.size())
                                                 : full;
    // Optional per-coefficient warp chain, appended as a ":warp:<chain>"
    // section. The coefficient list is comma-separated and never contains the
    // literal "warp", so decodeBody can split this back off unambiguously.
    // Omitted when empty so pre-warp frames round-trip byte-identical.
    if (!warpChain.empty()) b += ":warp:" + encodeWarpChain(warpChain);
    return b;
}

bool WaveletFrame::decodeBody(const std::string& body) {
    // Split off the optional ":warp:<chain>" section before handing the rest
    // to the (warp-unaware) wavelet-paint decoder.
    std::string wpBody = body;
    std::vector<WarpOp> wc;
    auto wpos = body.find(":warp:");
    if (wpos != std::string::npos) {
        wc = decodeWarpChain(body.substr(wpos + 6));
        wpBody = body.substr(0, wpos);
    }
    bool ok = decodeWaveletPaint("__waveletpaint__:" + wpBody,
                                 coefficients, numLevels, totalSize, filterName);
    if (ok) warpChain = std::move(wc);
    return ok;
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

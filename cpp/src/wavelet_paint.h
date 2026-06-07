#pragma once
#include "wavelet.h"
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

namespace SoundShop {

// ==============================================================================
// Wavelet-painter script format (#65)
//
// Header-only helpers used by both the painter UI (cpp/src/wavelet_painter.h)
// and the synth side (cpp/src/terrain_synth.cpp). Splitting the format and
// IDWT-to-waveform logic out from wavelet_painter.h keeps the synth path
// free of any JUCE GUI includes.
//
// Encoded form (stored verbatim in `node.script`):
//
//   __waveletpaint__:<filter>:<numLevels>:<totalSize>:<c0,c1,...,cN-1>
//
//   <filter>     wavelet filter name (db1, db2, db4, sym4, ...)
//   <numLevels>  DWT decomposition levels
//   <totalSize>  N = total coefficient count (must be 2^k * 2^numLevels-aligned)
//   <c0..cN-1>   comma-separated %.4f coefficient values
//
// The format encodes its own parameters so the painter can later support
// different filter families / level counts / table sizes without breaking
// existing projects.
// ==============================================================================

// Encoding constants - the painter currently uses these defaults, but the
// decoder accepts any values from the saved script.
inline constexpr int         kWaveletPaintDefaultLevels = 5;
inline constexpr int         kWaveletPaintDefaultSize   = 1024;
inline constexpr const char* kWaveletPaintDefaultFilter = "db4";

// Build the `__waveletpaint__:...` script from coefficients + params.
inline std::string encodeWaveletPaint(const std::vector<float>& coefficients,
                                      int numLevels,
                                      const std::string& filterName)
{
    std::string s = "__waveletpaint__:" + filterName + ":"
                  + std::to_string(numLevels) + ":"
                  + std::to_string((int)coefficients.size()) + ":";
    s.reserve(s.size() + coefficients.size() * 8);
    char buf[24];
    for (size_t i = 0; i < coefficients.size(); ++i) {
        if (i > 0) s += ',';
        std::snprintf(buf, sizeof(buf), "%.4f", coefficients[i]);
        s += buf;
    }
    return s;
}

// Decode a `__waveletpaint__:...` script. Returns true on success. On
// failure, output params are left untouched.
inline bool decodeWaveletPaint(const std::string& script,
                               std::vector<float>& coefficients,
                               int& numLevels,
                               int& totalSize,
                               std::string& filterName)
{
    const std::string prefix = "__waveletpaint__:";
    if (script.rfind(prefix, 0) != 0) return false;
    std::string rest = script.substr(prefix.size());

    auto c1 = rest.find(':');
    if (c1 == std::string::npos) return false;
    auto c2 = rest.find(':', c1 + 1);
    if (c2 == std::string::npos) return false;
    auto c3 = rest.find(':', c2 + 1);
    if (c3 == std::string::npos) return false;

    filterName = rest.substr(0, c1);
    try { numLevels = std::stoi(rest.substr(c1 + 1, c2 - c1 - 1)); }
        catch (...) { return false; }
    try { totalSize = std::stoi(rest.substr(c2 + 1, c3 - c2 - 1)); }
        catch (...) { return false; }
    if (numLevels < 1 || totalSize < (1 << numLevels)) return false;

    std::string body = rest.substr(c3 + 1);
    coefficients.clear();
    coefficients.reserve(totalSize);
    size_t p = 0;
    while (p <= body.size()) {
        size_t n = body.find(',', p);
        if (n == std::string::npos) n = body.size();
        std::string tok = body.substr(p, n - p);
        if (!tok.empty()) {
            try { coefficients.push_back(std::stof(tok)); }
                catch (...) { coefficients.push_back(0.0f); }
        }
        p = n + 1;
    }
    // Pad / truncate to declared totalSize so the IDWT sees the expected
    // shape even if the file was hand-edited.
    coefficients.resize((size_t)totalSize, 0.0f);
    return true;
}

// IDWT the coefficient grid back to a normalised one-cycle waveform.
// Output size == totalSize, peak normalised to 1.0. Used by both the
// painter (for the live preview) and the synth (to fill the terrain).
inline std::vector<float> waveletPaintToWaveform(
    const std::vector<float>& coefficients,
    int numLevels,
    const std::string& filterName)
{
    std::vector<float> w = coefficients;
    if (w.empty()) return w;
    auto filt = getWaveletFilter(filterName);
    idwt(w, numLevels, filt);

    float peak = 0.0f;
    for (float v : w) { float a = std::abs(v); if (a > peak) peak = a; }
    if (peak > 1e-9f) {
        float inv = 1.0f / peak;
        for (float& v : w) v *= inv;
    }
    return w;
}

} // namespace SoundShop

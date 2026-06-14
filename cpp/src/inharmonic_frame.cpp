#include "inharmonic_frame.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace SoundShop {

void InharmonicFrame::renderRaw(int tableSize, std::vector<float>& out) const {
    out.assign((size_t)tableSize, 0.0f);
    if (tableSize <= 0 || partials.empty()) return;

    // Representative single-period thumbnail: sum the partials over one
    // fundamental period [0,1). Inharmonic partials don't share this period, so
    // the snapshot is not perfectly loop-clean - that's inherent to an
    // inharmonic spectrum and faithful to what the live voice produces. The
    // editor draws this; the synth only uses it as a terrain fallback if the
    // live oscillator bank is unavailable.
    const float TWO_PI = 6.28318530718f;
    for (int i = 0; i < tableSize; ++i) {
        float t = (float)i / (float)tableSize;
        float v = 0.0f;
        for (const auto& p : partials)
            v += p.amp * std::sin(TWO_PI * (p.ratio * t + p.phase));
        out[(size_t)i] = v;
    }

    // Peak-normalise to the cycle-frame convention (peak 1.0). The same factor
    // is what the live oscillator-bank voice multiplies by (normGainFor), so the
    // synth audio matches this thumbnail's loudness.
    float inv = normGainFor(partials, tableSize);
    if (inv != 1.0f)
        for (float& v : out) v *= inv;

    // Bucket C element warp (amplitude-domain waveshaping), applied to the
    // normalised cycle so the thumbnail matches the transfer the synth applies
    // to the live additive sample. warpAmpOps() drops any non-amplitude ops.
    auto ampOps = warpAmpOps();
    if (!ampOps.empty())
        for (auto& v : out)
            for (const auto& op : ampOps)
                v = warpAmpValue(op.method, v, op.amount);
}

std::string InharmonicFrame::encodeBody() const {
    // <numPartials>;<r>,<a>,<p>;<r>,<a>,<p>;...[;warp:<chain>]
    std::ostringstream ss;
    ss << partials.size();
    char buf[64];
    for (const auto& p : partials) {
        std::snprintf(buf, sizeof(buf), ";%.6f,%.6f,%.6f", p.ratio, p.amp, p.phase);
        ss << buf;
    }
    // Optional amplitude-domain element warp, appended as ";warp:<chain>". The
    // partial tokens never contain ';' or the literal "warp", and
    // encodeWarpChain never emits "warp", so the delimiter is unambiguous.
    // Omitted when empty so warp-free frames round-trip byte-identical.
    if (!warpChain.empty())
        ss << ";warp:" << encodeWarpChain(warpChain);
    return ss.str();
}

bool InharmonicFrame::decodeBody(const std::string& body) {
    partials.clear();
    warpChain.clear();
    if (body.empty()) return false;

    // Split off the optional ";warp:<chain>" section first so the partial loop
    // never sees it.
    size_t end = body.size();
    size_t wpos = body.find(";warp:");
    if (wpos != std::string::npos) {
        warpChain = decodeWarpChain(body.substr(wpos + 6));
        end = wpos;
    }

    // First token: partial count (used only to reserve / sanity-check; the
    // actual partials are parsed from the following tokens).
    size_t s0 = body.find(';');
    if (s0 == std::string::npos || s0 > end) s0 = end;
    int declared = 0;
    try { declared = std::stoi(body.substr(0, s0)); } catch (...) { declared = 0; }
    if (declared < 0 || declared > 100000) return false;

    // Remaining ';'-separated tokens are "ratio,amp,phase" triples.
    size_t p = (s0 < end) ? s0 + 1 : end;
    while (p < end) {
        size_t semi = body.find(';', p);
        if (semi == std::string::npos || semi > end) semi = end;
        std::string tok = body.substr(p, semi - p);
        p = semi + 1;
        if (tok.empty()) continue;
        // Parse "ratio,amp,phase".
        size_t c1 = tok.find(',');
        size_t c2 = (c1 == std::string::npos) ? std::string::npos : tok.find(',', c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) continue;
        Partial pt;
        try {
            pt.ratio = std::stof(tok.substr(0, c1));
            pt.amp   = std::stof(tok.substr(c1 + 1, c2 - c1 - 1));
            pt.phase = std::stof(tok.substr(c2 + 1));
        } catch (...) { continue; }
        if (!(pt.ratio > 0.0f)) pt.ratio = 1.0f;
        partials.push_back(pt);
        if ((int)partials.size() >= kInharmonicMaxPartials) break;
    }
    return true;
}

std::unique_ptr<IWavetableFrame> InharmonicFrame::clone() const {
    return std::make_unique<InharmonicFrame>(*this);
}

float InharmonicFrame::normGainFor(const std::vector<Partial>& partials,
                                   int tableSize) {
    if (partials.empty() || tableSize <= 0) return 1.0f;
    const float TWO_PI = 6.28318530718f;
    float peak = 0.0f;
    for (int i = 0; i < tableSize; ++i) {
        float t = (float)i / (float)tableSize;
        float v = 0.0f;
        for (const auto& p : partials)
            v += p.amp * std::sin(TWO_PI * (p.ratio * t + p.phase));
        peak = std::max(peak, std::abs(v));
    }
    return (peak > 1e-9f) ? (1.0f / peak) : 1.0f;
}

InharmonicFrame InharmonicFrame::defaultBell() {
    InharmonicFrame f;
    // Classic struck-bar / tubular-bell inharmonic ratios with a natural
    // amplitude roll-off. Phases left at 0 (a struck onset is roughly in
    // phase). These give an immediately bell-like tone the user can reshape.
    const float ratios[] = { 1.0f, 2.76f, 5.40f, 8.93f, 13.34f };
    const float amps[]   = { 1.0f, 0.6f,  0.4f,  0.25f, 0.15f  };
    for (int i = 0; i < 5; ++i)
        f.partials.push_back({ ratios[i], amps[i], 0.0f });
    return f;
}

} // namespace SoundShop

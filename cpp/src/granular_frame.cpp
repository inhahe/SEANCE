#include "granular_frame.h"
#include "fft_util.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace SoundShop {

// Round `n` up to the next power of two, with a small floor so we don't
// FFT trivially small buffers. Cap at 16384 matching FFT::FFT's range.
static int ceilPow2(int n) {
    int p = 2;
    while (p < n && p < 16384) p <<= 1;
    return p;
}

void GranularFrame::renderRaw(int tableSize, std::vector<float>& out) const {
    out.assign((size_t)tableSize, 0.0f);
    if (source.empty() || tableSize <= 0) return;

    // Fallback representative cycle: extract a tableSize-sample slice
    // centered in the source, then run an FFT round-trip with DC removed
    // to make it loop-clean. This cycle is what the wavetable editor
    // draws and what the terrain renderer uses if granular playback is
    // not available for this voice (e.g., the granular layer is disabled
    // or all slots are taken). The actual playable timbre still comes
    // from the granular OLA stream the synth runs against `source`.
    const int srcLen = (int)source.size();
    int sliceLen = std::min(tableSize, srcLen);
    int startIdx = std::max(0, (srcLen - sliceLen) / 2);

    std::vector<float> raw((size_t)tableSize, 0.0f);
    for (int i = 0; i < sliceLen; ++i)
        raw[(size_t)i] = source[(size_t)(startIdx + i)];

    // FFT round-trip at the next power of two so the integer-bin
    // reconstruction is perfectly periodic at the FFT length. Kill DC
    // to avoid thumps from constant offsets.
    int n = ceilPow2(tableSize);
    std::vector<float> padded(n, 0.0f);
    for (int i = 0; i < tableSize; ++i) padded[(size_t)i] = raw[(size_t)i];

    FFT fft(n);
    std::vector<FFT::cplx> spectrum;
    fft.forwardReal(padded, spectrum);
    if (!spectrum.empty()) spectrum[0] = 0.0f;

    std::vector<float> ifftOut;
    fft.inverseReal(spectrum, ifftOut);
    ifftOut.resize((size_t)tableSize, 0.0f);

    // Peak-normalise to match the cycle-frame convention.
    float peak = 0.0f;
    for (float v : ifftOut) peak = std::max(peak, std::abs(v));
    if (peak > 1e-9f) {
        float inv = 1.0f / peak;
        for (float& v : ifftOut) v *= inv;
    }
    out = std::move(ifftOut);
}

std::string GranularFrame::encodeBody() const {
    // <srcLen>;<srcRate>;<grainLen>;<pitchHz>;<freezeMode>;<xfadeSamples>;<s0,s1,...>
    // %.4f gives 4 decimals - same density convention as SampleFrame.
    // A 1-second 48 kHz source encodes to ~280 KB of text. Big but
    // tolerable inside the length-prefixed wavetable2 body; a future
    // base64-binary path can shrink this ~1.6x.
    std::ostringstream ss;
    // captureSourceKind is written biased by +1 so the value is always a
    // non-negative all-digit token (the decode header run only accepts pure
    // digits): -1 (unknown) -> 0, 0 (song) -> 1, 1 (mic) -> 2, 2 (file) -> 3.
    // It is always emitted (never omitted) so this optional-int slot keeps a
    // stable position if more header ints are appended after it later.
    ss << source.size() << ';'
       << sourceSampleRate << ';'
       << grainLength << ';'
       << embeddedPitchHz << ';'
       << (int)freezeMode << ';'
       << crossfadeSamples << ';'
       << (captureSourceKind + 1) << ';';
    char buf[24];
    for (size_t i = 0; i < source.size(); ++i) {
        if (i > 0) ss << ',';
        std::snprintf(buf, sizeof(buf), "%.4f", source[i]);
        ss << buf;
    }
    return ss.str();
}

bool GranularFrame::decodeBody(const std::string& body) {
    // Header (current): <srcLen>;<srcRate>;<grainLen>;<pitchHz>;<freezeMode>;<xfadeSamples>;
    // Header (older):   <srcLen>;<srcRate>;<grainLen>;<pitchHz>;<freezeMode>;
    // Header (oldest):  <srcLen>;<srcRate>;<grainLen>;<pitchHz>;
    //
    // The first four header fields (srcLen, srcRate, grainLen, pitchHz)
    // are always present. After that, we read a run of optional all-digit
    // integer tokens delimited by ';'. They populate, in order:
    //   freezeMode (default 0 = CrossfadeLoop)
    //   crossfadeSamples (default 480 ~ original hardcoded 10 ms)
    // We stop the moment we encounter a token that contains a non-digit
    // character (the float list has '.' / 'e' / sign chars and the first
    // sample begins with one of those). This keeps the format extensible:
    // newer writers can append more int fields and older readers will
    // silently default the ones they don't know.
    size_t s1 = body.find(';');
    if (s1 == std::string::npos) return false;
    size_t s2 = body.find(';', s1 + 1);
    if (s2 == std::string::npos) return false;
    size_t s3 = body.find(';', s2 + 1);
    if (s3 == std::string::npos) return false;
    size_t s4 = body.find(';', s3 + 1);
    if (s4 == std::string::npos) return false;

    int srcLen = 0;
    try { srcLen = std::stoi(body.substr(0, s1)); }
    catch (...) { return false; }
    if (srcLen <= 0 || srcLen > 8'000'000) return false;  // ~167s @ 48 kHz cap

    try { sourceSampleRate = std::stod(body.substr(s1 + 1, s2 - s1 - 1)); }
    catch (...) { sourceSampleRate = 0.0; }

    try { grainLength = std::stoi(body.substr(s2 + 1, s3 - s2 - 1)); }
    catch (...) { grainLength = 4800; }
    if (grainLength < 16) grainLength = 16;

    try { embeddedPitchHz = std::stof(body.substr(s3 + 1, s4 - s3 - 1)); }
    catch (...) { embeddedPitchHz = 440.0f; }
    if (embeddedPitchHz <= 0.0f) embeddedPitchHz = 440.0f;

    // Walk the optional-int header run. Each iteration peeks at the
    // next token; if it's pure digits and ends with ';' before the
    // first ',', consume it as the next optional int and advance.
    freezeMode = GranularFreezeMode::CrossfadeLoop;
    crossfadeSamples = 480;  // legacy fallback matches the pre-slider hardcoded 10 ms
    size_t pos = s4 + 1;
    size_t firstComma = body.find(',', pos);
    auto tryOptInt = [&](int& out) -> bool {
        size_t semi = body.find(';', pos);
        if (semi == std::string::npos) return false;
        if (firstComma != std::string::npos && semi >= firstComma) return false;
        if (semi == pos) return false;  // empty field, treat as end
        for (size_t i = pos; i < semi; ++i) {
            char c = body[i];
            if (c < '0' || c > '9') return false;
        }
        try { out = std::stoi(body.substr(pos, semi - pos)); }
        catch (...) { return false; }
        pos = semi + 1;
        return true;
    };
    captureSourceKind = -1;  // unknown unless the (newer) header field is present
    int tmp;
    if (tryOptInt(tmp)) {
        if (tmp >= 0 && tmp <= 3) freezeMode = (GranularFreezeMode)tmp;
        // else: unknown value, keep default
    }
    if (tryOptInt(tmp)) {
        crossfadeSamples = std::max(0, tmp);
    }
    // Third optional int: captureSourceKind, biased by +1 on write (see
    // encodeBody). 0 -> unknown (-1), 1 -> song (0), 2 -> mic (1), 3 -> file
    // (2). Absent in pre-field projects, which correctly stay "unknown".
    if (tryOptInt(tmp)) {
        captureSourceKind = (tmp >= 1 && tmp <= 3) ? (tmp - 1) : -1;
    }
    size_t sampleStart = pos;

    // Body: comma-separated floats.
    source.clear();
    source.reserve((size_t)srcLen);
    size_t p = sampleStart;
    while (p <= body.size()) {
        size_t n = body.find(',', p);
        if (n == std::string::npos) n = body.size();
        if (n > p) {
            try { source.push_back(std::stof(body.substr(p, n - p))); }
            catch (...) { source.push_back(0.0f); }
        }
        p = n + 1;
    }
    // Pad / truncate to declared srcLen so the synth sees the expected
    // shape even if the file was hand-edited or truncated.
    source.resize((size_t)srcLen, 0.0f);
    return true;
}

std::unique_ptr<IWavetableFrame> GranularFrame::clone() const {
    return std::make_unique<GranularFrame>(*this);
}

GranularFrame GranularFrame::defaultEmpty() {
    GranularFrame f;
    // 1-second A4 sine at 48 kHz, ~100 ms grains. Audible if the synth
    // accidentally instantiates this on decode failure - which beats
    // returning silence and the user thinking the synth is broken.
    const int n = 48000;
    f.source.assign((size_t)n, 0.0f);
    const float twoPi = 6.28318530718f;
    for (int i = 0; i < n; ++i)
        f.source[(size_t)i] = std::sin(twoPi * 440.0f * (float)i / 48000.0f);
    f.sourceSampleRate = 48000.0;
    f.grainLength = 4800;
    f.embeddedPitchHz = 440.0f;
    return f;
}

} // namespace SoundShop

#include "sample_frame.h"
#include "fft_util.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <sstream>

namespace SoundShop {

// Round `n` up to the next power of two, with a small floor so we don't
// FFT trivially small buffers. Cap at 16384 matching FFT::FFT's range.
static int ceilPow2(int n) {
    int p = 2;
    while (p < n && p < 16384) p <<= 1;
    return p;
}

void SampleFrame::cleanLoopBoundaries() {
    if (cycle.empty()) return;

    // Round-trip through a power-of-two FFT. The FFT only sees integer-bin
    // amplitudes by construction (the spectrum at integer bins is what
    // gets reconstructed), so the IFFT output is perfectly periodic at
    // the FFT length. After this the loop has no boundary discontinuity.
    int n = ceilPow2((int)cycle.size());
    std::vector<float> padded(n, 0.0f);
    for (size_t i = 0; i < cycle.size() && (int)i < n; ++i) padded[i] = cycle[i];

    FFT fft(n);
    std::vector<FFT::cplx> spectrum;
    fft.forwardReal(padded, spectrum);

    // Kill DC - a constant offset adds nothing useful at synth time and
    // can produce thumps when the wavetable's voice envelope opens.
    if (!spectrum.empty()) spectrum[0] = 0.0f;

    std::vector<float> out;
    fft.inverseReal(spectrum, out);

    // Trim back to the original requested length and normalise.
    out.resize(cycle.size());
    float peak = 0.0f;
    for (float v : out) peak = std::max(peak, std::abs(v));
    if (peak > 1e-9f) {
        float inv = 1.0f / peak;
        for (float& v : out) v *= inv;
    }
    cycle = std::move(out);
}

void SampleFrame::renderRaw(int tableSize, std::vector<float>& out) const {
    out.assign((size_t)tableSize, 0.0f);
    if (cycle.empty() || tableSize <= 0) return;

    const int m = (int)cycle.size();

    // Fast path: requested size matches stored size.
    if (m == tableSize) {
        out = cycle;
        return;
    }

    // FFT-based resample (band-limited) when both sizes are powers of two
    // and reasonable. Take the spectrum at the stored size, keep the bins
    // that fit at the output size, IFFT to produce a band-limited output
    // that's loop-perfect at tableSize.
    int mp = ceilPow2(m);
    int np = ceilPow2(tableSize);

    std::vector<float> srcPadded(mp, 0.0f);
    for (int i = 0; i < m && i < mp; ++i) srcPadded[i] = cycle[i];

    FFT fftIn(mp);
    std::vector<FFT::cplx> srcSpec;
    fftIn.forwardReal(srcPadded, srcSpec);

    int outHalf = np / 2 + 1;
    int copy    = std::min((int)srcSpec.size(), outHalf);

    std::vector<FFT::cplx> dstSpec(outHalf, FFT::cplx(0.0f, 0.0f));
    // Scale so that the IFFT preserves time-domain amplitude across the
    // size change (parseval relation gives the np/mp factor here).
    float scale = (float)np / (float)mp;
    for (int k = 0; k < copy; ++k) dstSpec[k] = srcSpec[k] * scale;
    // Zero DC for the same reason as cleanLoopBoundaries above.
    dstSpec[0] = 0.0f;
    // The Nyquist bin must be real; if we copied a complex value from
    // a non-Nyquist position into it, force its imaginary part to zero.
    if (outHalf > 0) dstSpec[outHalf - 1] = FFT::cplx(dstSpec[outHalf - 1].real(), 0.0f);

    FFT fftOut(np);
    std::vector<float> resampled;
    fftOut.inverseReal(dstSpec, resampled);

    // Trim/pad to the exact requested tableSize. Most of the time np
    // already equals tableSize, but tableSize doesn't have to be a power
    // of two so we may have an oversized buffer to truncate.
    if ((int)resampled.size() >= tableSize) {
        out.assign(resampled.begin(), resampled.begin() + tableSize);
    } else {
        out = resampled;
        out.resize(tableSize, 0.0f);
    }

    // Peak-normalise. Other frame types follow the same convention - the
    // terrain renderer doesn't re-normalise.
    float peak = 0.0f;
    for (float v : out) peak = std::max(peak, std::abs(v));
    if (peak > 1e-9f) {
        float inv = 1.0f / peak;
        for (float& v : out) v *= inv;
    }
}

std::string SampleFrame::encodeBody() const {
    // <storedSize>;<embeddedPitchHz>;<c0,c1,...>
    // %.4f gives 4 decimals - enough audible resolution for cycle data
    // and consistent with WaveletFrame's encoding density. A 2048-sample
    // cycle encodes to ~16 KB of text, which is acceptable inside the
    // length-prefixed wavetable2 body.
    std::ostringstream ss;
    ss << cycle.size() << ';' << embeddedPitchHz << ';';
    char buf[24];
    for (size_t i = 0; i < cycle.size(); ++i) {
        if (i > 0) ss << ',';
        std::snprintf(buf, sizeof(buf), "%.4f", cycle[i]);
        ss << buf;
    }
    return ss.str();
}

bool SampleFrame::decodeBody(const std::string& body) {
    // Header: <storedSize>;<embeddedPitchHz>;
    size_t s1 = body.find(';');
    if (s1 == std::string::npos) return false;
    size_t s2 = body.find(';', s1 + 1);
    if (s2 == std::string::npos) return false;

    int storedSize = 0;
    try { storedSize = std::stoi(body.substr(0, s1)); }
    catch (...) { return false; }
    if (storedSize <= 0 || storedSize > 65536) return false;

    try { embeddedPitchHz = std::stof(body.substr(s1 + 1, s2 - s1 - 1)); }
    catch (...) { embeddedPitchHz = 0.0f; }

    // Body: comma-separated floats.
    cycle.clear();
    cycle.reserve((size_t)storedSize);
    size_t p = s2 + 1;
    while (p <= body.size()) {
        size_t n = body.find(',', p);
        if (n == std::string::npos) n = body.size();
        if (n > p) {
            try { cycle.push_back(std::stof(body.substr(p, n - p))); }
            catch (...) { cycle.push_back(0.0f); }
        }
        p = n + 1;
    }
    // Pad / truncate to declared storedSize so the synth sees the
    // expected shape even if the file was hand-edited or truncated.
    cycle.resize((size_t)storedSize, 0.0f);
    return true;
}

std::unique_ptr<IWavetableFrame> SampleFrame::clone() const {
    return std::make_unique<SampleFrame>(*this);
}

SampleFrame SampleFrame::defaultEmpty(int storedSize) {
    SampleFrame f;
    f.cycle.assign((size_t)std::max(2, storedSize), 0.0f);
    const float twoPi = 6.28318530718f;
    for (size_t i = 0; i < f.cycle.size(); ++i)
        f.cycle[i] = std::sin(twoPi * (float)i / (float)f.cycle.size());
    f.embeddedPitchHz = 0.0f;
    return f;
}

} // namespace SoundShop

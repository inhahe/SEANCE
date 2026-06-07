#define _USE_MATH_DEFINES
#include "layered_wave_editor.h"
#include "spectral_editor.h"     // SpectralFrame for __wavetable2__ decode + sub-editor
#include "wavelet_frame.h"        // WaveletFrame for __wavetable2__ decode
#include "wavelet_painter.h"      // WaveletPainterComponent for sub-editor
#include "sample_frame.h"         // SampleFrame for capture-from-playback frames
#include "granular_frame.h"       // GranularFrame for granular capture frames
#include "builtin_synth.h"   // WaveExprParser for Formula shape
#include "help_utils.h"
#include "dialog_helpers.h"       // launchToolDialog for the pop-out wavetable view
#include "capture_from_playback.h" // CaptureFromPlaybackDialog / CaptureFromSongDialog for the "from playback" frame source
#include "audio_engine.h"          // AudioEngine::getInstance() to fetch the live graph/transport for the song-render dialog
#include <cmath>
#include <sstream>
#include <algorithm>
#include <random>
#include <limits>

namespace SoundShop {

// ==============================================================================
// LayeredWaveform - data model
// ==============================================================================

// Seed a fresh Drawn layer with a handful of points arranged like a sine so
// the user has something visible to grab and move.
static constexpr int kFreehandSampleCount = 512;

static std::vector<float> defaultFreehandSamples() {
    std::vector<float> out(kFreehandSampleCount, 0.0f);
    for (int i = 0; i < kFreehandSampleCount; ++i) {
        float phase = (float)i / (float)kFreehandSampleCount;
        out[i] = std::sin(2.0f * (float)M_PI * phase);
    }
    return out;
}

static std::vector<std::pair<float, float>> defaultDrawnPoints() {
    return {
        {0.00f,  0.0f},
        {0.25f,  1.0f},
        {0.50f,  0.0f},
        {0.75f, -1.0f}
    };
}

static float evalShape(WaveLayer::Shape s, float x /*phase 0..1*/, std::mt19937& rng) {
    const float TWOPI = 2.0f * (float)M_PI;
    switch (s) {
        case WaveLayer::Sine:     return std::sin(TWOPI * x);
        case WaveLayer::Saw:      return 2.0f * (x - std::floor(x + 0.5f));
        case WaveLayer::Square:   return (x - std::floor(x) < 0.5f) ? 1.0f : -1.0f;
        case WaveLayer::Triangle: {
            float t = x - std::floor(x);
            return 4.0f * std::abs(t - 0.5f) - 1.0f;
        }
        case WaveLayer::Noise: {
            std::uniform_real_distribution<float> d(-1.0f, 1.0f);
            return d(rng);
        }
        case WaveLayer::Drawn:    return 0.0f; // handled by sampleLayer below
        case WaveLayer::Formula:  return 0.0f; // handled by sampleLayer below
    }
    return 0.0f;
}

// Catmull-Rom interpolation through a periodic sequence of (x, y) points.
// `pts` must be sorted by x. x values in [0, 1); we treat the list as
// cyclic, so p[-1] == p[n-1] and p[n] == p[0] (x-shifted by +/-1 accordingly).
static float sampleDrawnPoints(const std::vector<std::pair<float, float>>& pts,
                               float x)
{
    int n = (int)pts.size();
    if (n == 0) return 0.0f;
    if (n == 1) return pts[0].second;

    // Wrap x into [0, 1)
    x = x - std::floor(x);

    // Find the segment [p1, p2] such that p1.x <= x < p2.x (handling wrap).
    int i1 = -1;
    for (int i = 0; i < n; ++i) {
        float a = pts[i].first;
        float b = (i + 1 < n) ? pts[i + 1].first : pts[0].first + 1.0f;
        float xx = x;
        if (a > b) { // shouldn't happen if sorted, but just in case
            if (xx < a) xx += 1.0f;
        }
        if (xx >= a && xx < b) { i1 = i; break; }
    }
    if (i1 < 0) i1 = n - 1;
    int i0 = (i1 - 1 + n) % n;
    int i2 = (i1 + 1) % n;
    int i3 = (i1 + 2) % n;

    float x1 = pts[i1].first;
    float x2 = (i1 + 1 < n) ? pts[i2].first : pts[i2].first + 1.0f;
    float xx = x;
    if (xx < x1) xx += 1.0f;
    float t = (x2 - x1 > 1e-6f) ? (xx - x1) / (x2 - x1) : 0.0f;
    t = juce::jlimit(0.0f, 1.0f, t);

    float y0 = pts[i0].second, y1 = pts[i1].second;
    float y2 = pts[i2].second, y3 = pts[i3].second;

    // Catmull-Rom
    float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * y1)
                 + (-y0 + y2) * t
                 + (2.0f*y0 - 5.0f*y1 + 4.0f*y2 - y3) * t2
                 + (-y0 + 3.0f*y1 - 3.0f*y2 + y3) * t3);
}

// Linear interpolation through a periodic array of per-sample waveform data.
static float sampleDrawnSamples(const std::vector<float>& samples, float x) {
    int n = (int)samples.size();
    if (n == 0) return 0.0f;
    x = x - std::floor(x);          // wrap to [0, 1)
    float idx = x * (float)n;
    int i0 = (int)idx % n;
    int i1 = (i0 + 1) % n;
    float frac = idx - std::floor(idx);
    return samples[i0] * (1.0f - frac) + samples[i1] * frac;
}

static float sampleLayer(const WaveLayer& layer, float x, std::mt19937& rng) {
    if (layer.shape == WaveLayer::Drawn) {
        if (layer.freehandMode && !layer.drawnSamples.empty())
            return sampleDrawnSamples(layer.drawnSamples, x);
        return sampleDrawnPoints(layer.drawnPoints, x);
    }
    if (layer.shape == WaveLayer::Formula) {
        // Evaluate the expression at x in radians, like the freq-domain editor's
        // time-domain mode. WaveExprParser::evaluateAt does not clamp, so cap
        // here to [-1, 1] to match the other shapes' output range.
        if (layer.formulaExpr.empty()) return 0.0f;
        float v = WaveExprParser::evaluateAt(layer.formulaExpr,
                                             2.0f * (float)M_PI * x,
                                             0.0f);
        return juce::jlimit(-1.0f, 1.0f, v);
    }
    return evalShape(layer.shape, x, rng);
}

void LayeredWaveform::render(std::vector<float>& out) const {
    out.assign(tableSize, 0.0f);
    if (layers.empty()) return;

    for (const auto& layer : layers) {
        // Use a layer-specific deterministic seed so noise layers are stable
        // across renders (not changing on every edit).
        std::mt19937 rng(1234u + (unsigned)layer.ratio * 31u + (unsigned)layer.shape * 7u);
        int r = std::max(1, layer.ratio);
        for (int i = 0; i < tableSize; ++i) {
            float phase = (float)i / (float)tableSize;    // 0..1 over base period
            float x = phase * (float)r + layer.phase;     // ratio + phase offset
            out[i] += layer.amp * sampleLayer(layer, x, rng);
        }
    }

    // Normalize to peak 1.0
    float peak = 0.0f;
    for (float v : out) peak = std::max(peak, std::abs(v));
    if (peak > 1e-9f) {
        float inv = 1.0f / peak;
        for (float& v : out) v *= inv;
    }
}

static const char* shapeName(WaveLayer::Shape s) {
    switch (s) {
        case WaveLayer::Sine:     return "sine";
        case WaveLayer::Saw:      return "saw";
        case WaveLayer::Square:   return "square";
        case WaveLayer::Triangle: return "triangle";
        case WaveLayer::Noise:    return "noise";
        case WaveLayer::Drawn:    return "drawn";
        case WaveLayer::Formula:  return "formula";
    }
    return "sine";
}

static WaveLayer::Shape parseShape(const std::string& s) {
    if (s == "saw")      return WaveLayer::Saw;
    if (s == "square")   return WaveLayer::Square;
    if (s == "triangle") return WaveLayer::Triangle;
    if (s == "noise")    return WaveLayer::Noise;
    if (s == "drawn")    return WaveLayer::Drawn;
    if (s == "formula")  return WaveLayer::Formula;
    return WaveLayer::Sine;
}

// The layer-field separator is `,` and the layer separator is `|`, so a
// Formula expression containing those characters has to be escaped. We
// substitute commas with `;` and pipes with `\x1F` (Unit Separator, ASCII 31)
// on encode and reverse it on decode. Neither character is part of the
// WaveExprParser grammar, so this round-trips losslessly for any valid input.
static std::string escapeFormula(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if      (c == ',')  out += ';';
        else if (c == '|')  out += '\x1F';
        else                out += c;
    }
    return out;
}

static std::string unescapeFormula(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if      (c == ';')   out += ',';
        else if (c == '\x1F') out += '|';
        else                 out += c;
    }
    return out;
}

// Emit one layer as a comma-separated field list (no trailing `|`).
static void encodeLayer(std::ostringstream& o, const WaveLayer& l) {
    o << shapeName(l.shape)
      << "," << l.ratio
      << "," << l.phase
      << "," << l.amp;
    if (l.shape == WaveLayer::Drawn) {
        o << "," << (l.freehandMode ? 1 : 0);
        if (l.freehandMode) {
            o << "," << l.drawnSamples.size();
            for (float s : l.drawnSamples)
                o << "," << s;
        } else {
            o << "," << l.drawnPoints.size();
            for (auto& p : l.drawnPoints)
                o << "," << p.first << "," << p.second;
        }
    } else if (l.shape == WaveLayer::Formula) {
        // Field 4 = escaped expression. Commas inside the formula are mapped
        // to `;` so the comma-split parser still sees a single field.
        o << "," << escapeFormula(l.formulaExpr);
    }
}

// Parse a single layer from a comma-separated string. Returns true on success.
static bool parseLayer(const std::string& lp, WaveLayer& out) {
    std::vector<std::string> f;
    size_t p = 0;
    while (p <= lp.size()) {
        size_t n = lp.find(',', p);
        if (n == std::string::npos) n = lp.size();
        f.push_back(lp.substr(p, n - p));
        p = n + 1;
    }
    if (f.size() < 4) return false;
    out = WaveLayer{};
    out.shape = parseShape(f[0]);
    try { out.ratio = std::stoi(f[1]); } catch (...) { out.ratio = 1; }
    try { out.phase = std::stof(f[2]); } catch (...) { out.phase = 0.0f; }
    try { out.amp   = std::stof(f[3]); } catch (...) { out.amp   = 1.0f; }
    if (out.shape == WaveLayer::Formula) {
        if (f.size() > 4)
            out.formulaExpr = unescapeFormula(f[4]);
        if (out.formulaExpr.empty())
            out.formulaExpr = "sin(x)";
        return true;
    }
    if (out.shape == WaveLayer::Drawn && f.size() > 4) {
        // Field 4: freehandMode flag (0 or 1). Legacy data without this flag
        // will have a point count here instead; detect by checking whether the
        // total field count matches point-mode layout.
        int freehandFlag = 0;
        try { freehandFlag = std::stoi(f[4]); } catch (...) {}

        // Heuristic for legacy data: if f[4] > 1, it must be a legacy point
        // count (old format had no freehand flag).  New format always has 0 or 1.
        bool isLegacy = (freehandFlag > 1);
        if (isLegacy) {
            out.freehandMode = false;
            int count = freehandFlag;
            for (int k = 0; k < count; ++k) {
                size_t xi = 5 + (size_t)k * 2;
                size_t yi = xi + 1;
                if (yi >= f.size()) break;
                float x = 0, y = 0;
                try { x = std::stof(f[xi]); } catch (...) {}
                try { y = std::stof(f[yi]); } catch (...) {}
                out.drawnPoints.emplace_back(x, y);
            }
        } else {
            out.freehandMode = (freehandFlag != 0);
            if (f.size() > 5) {
                int count = 0;
                try { count = std::stoi(f[5]); } catch (...) {}
                if (out.freehandMode) {
                    for (int k = 0; k < count && (size_t)(6 + k) < f.size(); ++k) {
                        float v = 0.0f;
                        try { v = std::stof(f[6 + k]); } catch (...) {}
                        out.drawnSamples.push_back(v);
                    }
                } else {
                    for (int k = 0; k < count; ++k) {
                        size_t xi = 6 + (size_t)k * 2;
                        size_t yi = xi + 1;
                        if (yi >= f.size()) break;
                        float x = 0, y = 0;
                        try { x = std::stof(f[xi]); } catch (...) {}
                        try { y = std::stof(f[yi]); } catch (...) {}
                        out.drawnPoints.emplace_back(x, y);
                    }
                }
            }
        }
    }
    return true;
}

std::string LayeredWaveform::encode() const {
    return "__layered__:" + encodeBody();
}

// Body form (no "__layered__:" prefix): tableSize|layer1|layer2|...
// Used by the IWavetableFrame interface so containers can embed the body
// with their own length prefix in the __wavetable2__ format (Phase 2).
std::string LayeredWaveform::encodeBody() const {
    std::ostringstream o;
    o << tableSize;
    for (const auto& l : layers) {
        o << "|";
        encodeLayer(o, l);
    }
    return o.str();
}

bool LayeredWaveform::decodeBody(const std::string& body) {
    // decode() already strips an optional __layered__: prefix and parses
    // the body, so we can just delegate.
    return decode(body);
}

// IWavetableFrame::render(int ts, out) override - render at the caller's
// requested table size instead of the member tableSize. Implemented via a
// shallow copy so the existing const render(out) can stay untouched and the
// member tableSize doesn't have to be mutated through const.
void LayeredWaveform::render(int ts, std::vector<float>& out) const {
    if (ts <= 0) { out.clear(); return; }
    LayeredWaveform tmp = *this;
    tmp.tableSize = ts;
    tmp.render(out);
}

std::unique_ptr<IWavetableFrame> LayeredWaveform::clone() const {
    return std::make_unique<LayeredWaveform>(*this);
}

bool LayeredWaveform::decode(const std::string& s) {
    layers.clear();
    std::string body = s;
    if (body.rfind("__layered__:", 0) == 0) body = body.substr(12);
    if (body.empty()) return false;

    // split by '|'
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t next = body.find('|', pos);
        if (next == std::string::npos) next = body.size();
        parts.push_back(body.substr(pos, next - pos));
        pos = next + 1;
    }
    if (parts.empty()) return false;
    try {
        tableSize = std::stoi(parts[0]);
    } catch (...) { tableSize = 2048; }

    for (size_t i = 1; i < parts.size(); ++i) {
        WaveLayer layer;
        if (parseLayer(parts[i], layer))
            layers.push_back(layer);
    }
    return !layers.empty();
}

// ==============================================================================
// WavetableDoc
// ==============================================================================

// Format reference:
//   Grid: __wavetable__:<tableSize>:g;<numDims>;<dim0>;<dim1>;...:<frameCount>:<gridFrame0>:<gridFrame1>:...
//     where gridFrameN = layer1|layer2|...
//   Scatter: __wavetable__:<tableSize>:s;<scatterDims>;<radius>:<frameCount>:<scatterFrame0>:...
//     where scatterFrameN = pos0;pos1;...;posN@<label>@<layer1|layer2|...>
//   Legacy (no g/s prefix): treated as grid format with parts[1] containing dims OR a frame count.
// =============================================================================
// __wavetable2__ format (current)
// =============================================================================
//
// Grammar:
//   __wavetable2__:<ts>:<modeSpec>:<count>[<frame>]*
//
//   modeSpec (Grid):    g;<numDims>;<dim0>;<dim1>;...
//   modeSpec (Scatter): s;<scatterDims>;<radius>
//
//   frame (Grid):    :<typeId>:<bodyLen>:<body of exactly bodyLen chars>
//   frame (Scatter): :<pos0>;<pos1>;...@<label>@<typeId>:<bodyLen>:<body>
//
// The body length prefix lets the body contain any character (including
// the structural delimiters ':' '|' ';' '@') without escaping. This is
// essential because the wavelet body uses ':' internally and the layered
// body uses '|' to separate layers.
//
// Label sanitisation: '@', ':' and '|' are replaced with '_' on encode so
// the position@label@typeId framing stays unambiguous.

static std::string sanitizeLabel(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (c == '@' || c == ':' || c == '|') o.push_back('_');
        else                                   o.push_back(c);
    }
    return o;
}

// =============================================================================
// __wavetable3__ format (current) - library + cell-by-reference model
// =============================================================================
//
// Top-level grammar:
//   __wavetable3__:<ts>:<modeSpec>:<libCount>[<libEntry>]*:<cellCount>[<cell>]*
//
//   modeSpec (Grid):    g;<numDims>;<dim0>;<dim1>;...
//   modeSpec (Scatter): s;<scatterDims>;<radius>
//
//   libEntry: :<id>:<nameLen>:<name><typeId>:<bodyLen>:<body>
//     <name> is exactly <nameLen> raw chars (no escaping). <body> is
//     exactly <bodyLen> raw chars, same length-prefix trick as v2.
//
//   cell (Grid):    :<libraryId>
//     -1 = empty cell; any other id must resolve to a library entry.
//   cell (Scatter): :<libraryId>:<pos0>;<pos1>;...@<label>
//     Label is sanitized for '@' ':' '|' (see sanitizeLabel).
//
// Why a third version: v2 conflated cells with waveforms (each cell
// owned its data). v3 separates them so the same waveform can sit in
// many cells, can be edited once and updated everywhere, and survives
// being removed from a cell. v2/v1 decoders below auto-migrate by
// promoting each non-null cell to a fresh library entry.

std::string WavetableDoc::encode() const {
    std::ostringstream o;
    o << "__wavetable3__:" << tableSize << ":";
    if (mode == WavetableMode::Grid) {
        o << "g;" << gridDims.size();
        for (int d : gridDims) o << ";" << d;
    } else {
        o << "s;" << scatterDims << ";" << scatterRadius;
    }

    // Library section.
    o << ":" << library.size();
    for (const auto& e : library) {
        o << ":" << e.id << ":" << e.name.size() << ":" << e.name;
        const char* tid = e.wave ? e.wave->typeId() : "layered";
        std::string body = e.wave ? e.wave->encodeBody() : "";
        o << tid << ":" << body.size() << ":" << body;
    }

    // Cell section.
    if (mode == WavetableMode::Grid) {
        o << ":" << cellWaveformIds.size();
        for (int id : cellWaveformIds) o << ":" << id;
    } else {
        o << ":" << scatterFrames.size();
        for (const auto& sf : scatterFrames) {
            o << ":" << sf.waveformId << ":";
            for (int i = 0; i < (int)sf.position.size(); ++i) {
                if (i > 0) o << ";";
                o << sf.position[i];
            }
            o << "@" << sanitizeLabel(sf.label);
        }
    }
    return o.str();
}

// Helper: parse the layer string `layer1|layer2|...` into a LayeredWaveform.
static LayeredWaveform parseLayerString(const std::string& fb, int tableSize) {
    LayeredWaveform lw;
    lw.tableSize = tableSize;
    size_t lp = 0;
    while (lp <= fb.size()) {
        size_t ln = fb.find('|', lp);
        if (ln == std::string::npos) ln = fb.size();
        std::string layerStr = fb.substr(lp, ln - lp);
        if (!layerStr.empty()) {
            WaveLayer layer;
            if (parseLayer(layerStr, layer))
                lw.layers.push_back(layer);
        }
        lp = ln + 1;
    }
    return lw;
}

// Construct a concrete IWavetableFrame by typeId. Hardcoded list of known
// types - swap to a registry if/when this grows. Returns nullptr on
// unknown tag (the caller treats the slot as "frame skipped").
static std::unique_ptr<IWavetableFrame> createFrameByTypeId(const std::string& tid) {
    if (tid == "layered")  return std::make_unique<LayeredWaveform>();
    if (tid == "spectral") return std::make_unique<SpectralFrame>();
    if (tid == "wavelet")  return std::make_unique<WaveletFrame>();
    if (tid == "sample")   return std::make_unique<SampleFrame>();
    if (tid == "granular") return std::make_unique<GranularFrame>();
    return nullptr;
}

// Single-pass cursor over the v2 body. Read* methods advance `p` and
// return false on overrun so the parser fails cleanly.
struct V2Reader {
    const std::string& s;
    size_t p = 0;
    explicit V2Reader(const std::string& src) : s(src) {}

    bool eof() const { return p >= s.size(); }

    // Read up to (and consume) the next occurrence of `delim`. Returns the
    // text before the delimiter. If `delim` isn't found, consumes to EOS
    // and returns the remainder (caller treats this as the final token).
    std::string readUntil(char delim) {
        size_t n = s.find(delim, p);
        if (n == std::string::npos) {
            std::string out = s.substr(p);
            p = s.size();
            return out;
        }
        std::string out = s.substr(p, n - p);
        p = n + 1;
        return out;
    }

    // Read exactly N raw chars without delimiter scanning. Returns ""
    // (and leaves p unchanged) on overrun so the caller can detect failure.
    std::string readN(size_t n) {
        if (p + n > s.size()) return {};
        std::string out = s.substr(p, n);
        p += n;
        return out;
    }
};

// Shared helpers used by every decoder variant.

static std::vector<std::string> splitSemi(const std::string& src) {
    std::vector<std::string> out;
    size_t dp = 0;
    while (dp <= src.size()) {
        size_t dn = src.find(';', dp);
        if (dn == std::string::npos) dn = src.size();
        out.push_back(src.substr(dp, dn - dp));
        dp = dn + 1;
    }
    return out;
}

// Parse the leading "g;..." / "s;..." mode header. Writes into `doc.mode`,
// `doc.gridDims`, `doc.scatterDims`, `doc.scatterRadius` and returns true if
// `modeSpec` looked well-formed enough to commit to that mode.
static bool parseModeSpec(WavetableDoc& doc, const std::string& modeSpec) {
    if (modeSpec.empty()) return false;
    const bool isScatter = (modeSpec[0] == 's');
    size_t mp = (modeSpec.size() > 1 && modeSpec[1] == ';') ? 2 : 1;
    const std::string modeBody = modeSpec.substr(mp);

    if (isScatter) {
        doc.mode = WavetableMode::Scatter;
        auto dp = splitSemi(modeBody);
        if (!dp.empty())   try { doc.scatterDims   = std::stoi(dp[0]); } catch (...) { doc.scatterDims = 2; }
        if (dp.size() > 1) try { doc.scatterRadius = std::stof(dp[1]); } catch (...) { doc.scatterRadius = 0.45f; }
        // Scatter mode must have at least 2 axes. Legacy files with
        // scatterDims=1 would otherwise lock all dots onto y=0.5.
        if (doc.scatterDims < 2) doc.scatterDims = 2;
    } else {
        doc.mode = WavetableMode::Grid;
        auto dp = splitSemi(modeBody);
        int numDims = 0;
        if (!dp.empty()) try { numDims = std::stoi(dp[0]); } catch (...) {}
        for (int d = 0; d < numDims && d + 1 < (int)dp.size(); ++d) {
            try { doc.gridDims.push_back(std::stoi(dp[d + 1])); }
            catch (...) { doc.gridDims.push_back(1); }
        }
    }
    return true;
}

// =============================================================================
// __wavetable3__ decoder (current format)
// =============================================================================
//
// Reads the library + cell sections produced by WavetableDoc::encode().
// See the format-grammar comment above encode() for the wire layout.

static bool decodeWavetableV3(WavetableDoc& doc, const std::string& body) {
    V2Reader r(body);
    try { doc.tableSize = std::stoi(r.readUntil(':')); }
    catch (...) { doc.tableSize = 2048; }

    const std::string modeSpec = r.readUntil(':');
    if (!parseModeSpec(doc, modeSpec)) return false;
    const bool isScatter = (doc.mode == WavetableMode::Scatter);

    // ---- Library section ----
    int libCount = 0;
    try { libCount = std::stoi(r.readUntil(':')); } catch (...) {}

    int maxIdSeen = 0;
    for (int e = 0; e < libCount && !r.eof(); ++e) {
        int id = -1;
        try { id = std::stoi(r.readUntil(':')); } catch (...) { return false; }
        size_t nameLen = 0;
        try { nameLen = (size_t)std::stoul(r.readUntil(':')); } catch (...) { return false; }
        std::string name = r.readN(nameLen);
        if (name.size() != nameLen) return false;
        std::string typeId = r.readUntil(':');
        size_t bodyLen = 0;
        try { bodyLen = (size_t)std::stoul(r.readUntil(':')); } catch (...) { return false; }
        std::string fbody = r.readN(bodyLen);
        if (fbody.size() != bodyLen) return false;

        std::unique_ptr<IWavetableFrame> frame = createFrameByTypeId(typeId);
        if (frame && !frame->decodeBody(fbody)) frame.reset();
        if (!frame) {
            auto lw = std::make_unique<LayeredWaveform>();
            lw->tableSize = doc.tableSize;
            frame = std::move(lw);
        }

        WaveformLibraryEntry entry;
        entry.id = id;
        entry.name = std::move(name);
        entry.wave = std::move(frame);
        doc.library.push_back(std::move(entry));
        if (id > maxIdSeen) maxIdSeen = id;
    }
    doc.nextLibraryId = std::max(doc.nextLibraryId, maxIdSeen + 1);

    // ---- Cell section ----
    int cellCount = 0;
    try { cellCount = std::stoi(r.readUntil(':')); } catch (...) {}

    if (isScatter) {
        for (int c = 0; c < cellCount && !r.eof(); ++c) {
            int libId = -1;
            try { libId = std::stoi(r.readUntil(':')); } catch (...) {}
            const std::string posStr = r.readUntil('@');
            const std::string label  = r.readUntil(':');
            ScatterFrame sf;
            sf.waveformId = libId;
            sf.label = label;
            auto pp = splitSemi(posStr);
            for (auto& pstr : pp) {
                try { sf.position.push_back(std::stof(pstr)); }
                catch (...) { sf.position.push_back(0.5f); }
            }
            while ((int)sf.position.size() < doc.scatterDims) sf.position.push_back(0.5f);
            if ((int)sf.position.size() > doc.scatterDims)    sf.position.resize(doc.scatterDims);
            doc.scatterFrames.push_back(std::move(sf));
        }
    } else {
        for (int c = 0; c < cellCount && !r.eof(); ++c) {
            int libId = -1;
            try { libId = std::stoi(r.readUntil(':')); } catch (...) {}
            doc.cellWaveformIds.push_back(libId);
        }
        if (doc.gridDims.empty()) doc.gridDims = { (int)doc.cellWaveformIds.size() };
    }
    return true;
}

// =============================================================================
// __wavetable2__ decoder (legacy; auto-migrates to library + cell ids)
// =============================================================================
//
// v2 stored each cell's waveform inline. To load into the v3 data model, each
// non-null cell is promoted to a fresh library entry and the cell stores that
// id. Sparse-grid empty cells become id = -1 (no library entry created).

static bool decodeWavetableV2(WavetableDoc& doc, const std::string& body) {
    V2Reader r(body);
    try { doc.tableSize = std::stoi(r.readUntil(':')); }
    catch (...) { doc.tableSize = 2048; }

    const std::string modeSpec = r.readUntil(':');
    if (!parseModeSpec(doc, modeSpec)) return false;
    const bool isScatter = (doc.mode == WavetableMode::Scatter);

    int frameCount = 0;
    try { frameCount = std::stoi(r.readUntil(':')); } catch (...) {}

    for (int f = 0; f < frameCount && !r.eof(); ++f) {
        std::string positions, label;
        if (isScatter) {
            positions = r.readUntil('@');
            label     = r.readUntil('@');
        }
        std::string typeId = r.readUntil(':');
        size_t bodyLen = 0;
        try { bodyLen = (size_t)std::stoul(r.readUntil(':')); } catch (...) { return false; }
        std::string fbody = r.readN(bodyLen);
        if (fbody.size() != bodyLen) return false;

        // "null" is the explicit sparse-cell marker for Grid mode and is the
        // one typeId where nullptr is the correct decoded result. For every
        // other typeId, decode failure or unknown type falls back to an empty
        // layered frame so the wavetable still loads with the right frame count.
        const bool isExplicitNull = (typeId == "null");
        std::unique_ptr<IWavetableFrame> frame;
        if (!isExplicitNull) {
            frame = createFrameByTypeId(typeId);
            if (frame && !frame->decodeBody(fbody)) frame.reset();
            if (!frame) {
                auto lw = std::make_unique<LayeredWaveform>();
                lw->tableSize = doc.tableSize;
                frame = std::move(lw);
            }
        }

        if (isScatter) {
            // A null scatter frame would have no waveform at all; coerce to
            // empty layered so the scatter list stays well-formed.
            if (!frame) {
                auto lw = std::make_unique<LayeredWaveform>();
                lw->tableSize = doc.tableSize;
                frame = std::move(lw);
            }
            const int libId = doc.addLibraryEntry(std::move(frame));
            ScatterFrame sf;
            sf.label = label;
            sf.waveformId = libId;
            auto pp = splitSemi(positions);
            for (auto& pstr : pp) {
                try { sf.position.push_back(std::stof(pstr)); }
                catch (...) { sf.position.push_back(0.5f); }
            }
            while ((int)sf.position.size() < doc.scatterDims) sf.position.push_back(0.5f);
            if ((int)sf.position.size() > doc.scatterDims)    sf.position.resize(doc.scatterDims);
            doc.scatterFrames.push_back(std::move(sf));
        } else {
            if (!frame) {
                // Sparse / empty grid cell.
                doc.cellWaveformIds.push_back(-1);
            } else {
                const int libId = doc.addLibraryEntry(std::move(frame));
                doc.cellWaveformIds.push_back(libId);
            }
        }
    }

    if (!isScatter && doc.gridDims.empty())
        doc.gridDims = { (int)doc.cellWaveformIds.size() };

    return isScatter ? !doc.scatterFrames.empty() : !doc.cellWaveformIds.empty();
}

// =============================================================================
// Legacy __wavetable__ decoder (Phase 1; layered-only, auto-migrates to v3)
// =============================================================================

static bool decodeWavetableLegacy(WavetableDoc& doc, const std::string& body) {
    // Split body on ':' -> [tableSize, modeAndDims, frameCount, frame0, frame1, ...]
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t next = body.find(':', pos);
        if (next == std::string::npos) next = body.size();
        parts.push_back(body.substr(pos, next - pos));
        pos = next + 1;
    }
    if (parts.size() < 2) return false;
    try { doc.tableSize = std::stoi(parts[0]); } catch (...) { doc.tableSize = 2048; }

    // Detect mode: parts[1] starts with "g;" (grid v2-style), "s;" (scatter), or
    // is bare digits (grid v1 with semicolons OR very-old single frame count).
    int frameStartIdx = 2;
    int frameCount = 0;
    bool isScatter = false;

    std::string dimsField = parts[1];
    if (!dimsField.empty() && (dimsField[0] == 'g' || dimsField[0] == 's')) {
        isScatter = (dimsField[0] == 's');
        dimsField = dimsField.substr(1);
        if (!dimsField.empty() && dimsField[0] == ';') dimsField.erase(0, 1);
    }

    if (isScatter) {
        // Scatter: dimsField = "scatterDims;radius"
        auto dp = splitSemi(dimsField);
        doc.mode = WavetableMode::Scatter;
        if (!dp.empty())     try { doc.scatterDims   = std::stoi(dp[0]); } catch (...) { doc.scatterDims = 2; }
        if (dp.size() > 1)   try { doc.scatterRadius = std::stof(dp[1]); } catch (...) { doc.scatterRadius = 0.45f; }
        if (doc.scatterDims < 2) doc.scatterDims = 2;
        if (parts.size() > 2) try { frameCount = std::stoi(parts[2]); } catch (...) {}
        frameStartIdx = 3;

        for (int f = 0; f < frameCount && (size_t)(frameStartIdx + f) < parts.size(); ++f) {
            const std::string& fb = parts[frameStartIdx + f];
            // Split on '@': posList @ label @ layers
            size_t a1 = fb.find('@');
            size_t a2 = (a1 != std::string::npos) ? fb.find('@', a1 + 1) : std::string::npos;
            std::string posStr = (a1 != std::string::npos) ? fb.substr(0, a1) : "";
            std::string label  = (a1 != std::string::npos && a2 != std::string::npos)
                                  ? fb.substr(a1 + 1, a2 - a1 - 1) : "";
            std::string layerStr = (a2 != std::string::npos) ? fb.substr(a2 + 1) : "";

            ScatterFrame sf;
            sf.label = std::move(label);
            auto pp = splitSemi(posStr);
            for (auto& p : pp) {
                try { sf.position.push_back(std::stof(p)); }
                catch (...) { sf.position.push_back(0.5f); }
            }
            while ((int)sf.position.size() < doc.scatterDims) sf.position.push_back(0.5f);
            if ((int)sf.position.size() > doc.scatterDims) sf.position.resize(doc.scatterDims);

            auto lw = std::make_unique<LayeredWaveform>(parseLayerString(layerStr, doc.tableSize));
            sf.waveformId = doc.addLibraryEntry(std::move(lw));
            doc.scatterFrames.push_back(std::move(sf));
        }
        return !doc.scatterFrames.empty();
    }

    // Grid path (v1 with bare numDims;dims, or very-old with bare frameCount).
    doc.mode = WavetableMode::Grid;
    if (dimsField.find(';') != std::string::npos) {
        auto dp = splitSemi(dimsField);
        int numDims = 0;
        if (!dp.empty()) try { numDims = std::stoi(dp[0]); } catch (...) {}
        for (int d = 0; d < numDims && d + 1 < (int)dp.size(); ++d) {
            try { doc.gridDims.push_back(std::stoi(dp[d + 1])); } catch (...) { doc.gridDims.push_back(1); }
        }
        if (parts.size() > 2) try { frameCount = std::stoi(parts[2]); } catch (...) {}
        frameStartIdx = 3;
    } else {
        // Very old: parts[1] is just a frame count, default 1D
        try { frameCount = std::stoi(dimsField); } catch (...) {}
        doc.gridDims = { frameCount };
        frameStartIdx = 2;
    }

    for (int f = 0; f < frameCount && (size_t)(frameStartIdx + f) < parts.size(); ++f) {
        auto lw = std::make_unique<LayeredWaveform>(
            parseLayerString(parts[frameStartIdx + f], doc.tableSize));
        const int libId = doc.addLibraryEntry(std::move(lw));
        doc.cellWaveformIds.push_back(libId);
    }
    if (doc.gridDims.empty()) doc.gridDims = { (int)doc.cellWaveformIds.size() };

    return !doc.cellWaveformIds.empty();
}

bool WavetableDoc::decode(const std::string& s) {
    library.clear();
    nextLibraryId = 1;
    cellWaveformIds.clear();
    scatterFrames.clear();
    gridDims.clear();
    mode = WavetableMode::Grid;
    scatterFromGridSnapshot.reset();

    {
        const std::string prefix = "__wavetable3__:";
        if (s.rfind(prefix, 0) == 0)
            return decodeWavetableV3(*this, s.substr(prefix.size()));
    }
    {
        const std::string prefix = "__wavetable2__:";
        if (s.rfind(prefix, 0) == 0)
            return decodeWavetableV2(*this, s.substr(prefix.size()));
    }
    {
        const std::string prefix = "__wavetable__:";
        if (s.rfind(prefix, 0) == 0)
            return decodeWavetableLegacy(*this, s.substr(prefix.size()));
    }
    return false;
}

// ---- Library API ----------------------------------------------------------

int WavetableDoc::addLibraryEntry(std::unique_ptr<IWavetableFrame> wave,
                                  std::string name) {
    WaveformLibraryEntry e;
    e.id = nextLibraryId++;
    if (name.empty()) {
        std::ostringstream o;
        o << "Waveform " << (int)library.size() + 1;
        name = o.str();
    }
    e.name = std::move(name);
    e.wave = std::move(wave);
    library.push_back(std::move(e));
    return library.back().id;
}

bool WavetableDoc::removeLibraryEntry(int id) {
    if (id < 0) return false;
    const int idx = findLibraryIndexById(id);
    if (idx < 0) return false;
    library.erase(library.begin() + idx);
    // Clear cell references to the removed id.
    for (int& c : cellWaveformIds) if (c == id) c = -1;
    for (auto& sf : scatterFrames)  if (sf.waveformId == id) sf.waveformId = -1;
    return true;
}

int WavetableDoc::findLibraryIndexById(int id) const {
    if (id < 0) return -1;
    for (size_t i = 0; i < library.size(); ++i)
        if (library[i].id == id) return (int)i;
    return -1;
}

IWavetableFrame* WavetableDoc::libraryFrameById(int id) {
    const int idx = findLibraryIndexById(id);
    return idx < 0 ? nullptr : library[(size_t)idx].wave.get();
}

const IWavetableFrame* WavetableDoc::libraryFrameById(int id) const {
    const int idx = findLibraryIndexById(id);
    return idx < 0 ? nullptr : library[(size_t)idx].wave.get();
}

int WavetableDoc::libraryIdForCell(int cellIdx) const {
    if (mode == WavetableMode::Grid) {
        if (cellIdx < 0 || cellIdx >= (int)cellWaveformIds.size()) return -1;
        return cellWaveformIds[(size_t)cellIdx];
    }
    if (cellIdx < 0 || cellIdx >= (int)scatterFrames.size()) return -1;
    return scatterFrames[(size_t)cellIdx].waveformId;
}

void WavetableDoc::assignCellToLibrary(int cellIdx, int libraryId) {
    // -1 is the "clear" sentinel; any other id must exist in the library
    // or the call is a no-op (preserves the invariant that every non-(-1)
    // cell id is resolvable).
    if (libraryId != -1 && findLibraryIndexById(libraryId) < 0) return;
    if (mode == WavetableMode::Grid) {
        if (cellIdx < 0 || cellIdx >= (int)cellWaveformIds.size()) return;
        cellWaveformIds[(size_t)cellIdx] = libraryId;
    } else {
        if (cellIdx < 0 || cellIdx >= (int)scatterFrames.size()) return;
        scatterFrames[(size_t)cellIdx].waveformId = libraryId;
    }
}

bool WavetableDoc::isLibraryEntryUsed(int id) const {
    return countCellsUsingLibrary(id) > 0;
}

int WavetableDoc::countCellsUsingLibrary(int id) const {
    if (id < 0) return 0;
    int n = 0;
    for (int c : cellWaveformIds)  if (c == id) ++n;
    for (const auto& sf : scatterFrames) if (sf.waveformId == id) ++n;
    return n;
}

// ---- Per-cell frame access (rerouted through the library) ----------------

IWavetableFrame* WavetableDoc::frameAt(int idx) {
    return libraryFrameById(libraryIdForCell(idx));
}

const IWavetableFrame* WavetableDoc::frameAt(int idx) const {
    return libraryFrameById(libraryIdForCell(idx));
}

LayeredWaveform* WavetableDoc::layeredFrameAt(int idx) {
    return dynamic_cast<LayeredWaveform*>(frameAt(idx));
}

LayeredWaveform* WavetableDoc::layeredFrameByLibrary(int libId) {
    return dynamic_cast<LayeredWaveform*>(libraryFrameById(libId));
}

const LayeredWaveform* WavetableDoc::layeredFrameByLibrary(int libId) const {
    return dynamic_cast<const LayeredWaveform*>(libraryFrameById(libId));
}

const LayeredWaveform* WavetableDoc::layeredFrameAt(int idx) const {
    return dynamic_cast<const LayeredWaveform*>(frameAt(idx));
}

// ---- Sparse-grid / mode-conversion helpers -----------------------------------

int WavetableDoc::gridCellCount() const {
    if (gridDims.empty()) return 0;
    int n = 1;
    for (int d : gridDims) {
        if (d <= 0) return 0;
        n *= d;
    }
    return n;
}

std::vector<int> WavetableDoc::cellIdxToGridCoord(int idx) const {
    const int total = gridCellCount();
    if (idx < 0 || idx >= total) return {};
    std::vector<int> coord(gridDims.size(), 0);
    int rem = idx;
    // Row-major: last axis varies fastest. Matches gridToFlat() inverse.
    for (int d = (int)gridDims.size() - 1; d >= 0; --d) {
        const int sz = gridDims[d];
        coord[d] = rem % sz;
        rem /= sz;
    }
    return coord;
}

int WavetableDoc::gridCoordToCellIdx(const std::vector<int>& coord) const {
    if (coord.size() != gridDims.size()) return -1;
    int flat = 0, stride = 1;
    for (int d = (int)gridDims.size() - 1; d >= 0; --d) {
        if (coord[d] < 0 || coord[d] >= gridDims[d]) return -1;
        flat += coord[d] * stride;
        stride *= gridDims[d];
    }
    return flat;
}

std::vector<float> WavetableDoc::cellCenterPosition(int cellIdx) const {
    auto coord = cellIdxToGridCoord(cellIdx);
    if (coord.empty()) return {};
    std::vector<float> pos(coord.size(), 0.5f);
    for (size_t d = 0; d < coord.size(); ++d) {
        const int sz = gridDims[d];
        pos[d] = (sz > 0) ? ((float)coord[d] + 0.5f) / (float)sz : 0.5f;
    }
    return pos;
}

void WavetableDoc::resizeGridAxis(int axisIdx, int newSize) {
    if (axisIdx < 0 || axisIdx >= (int)gridDims.size()) return;
    if (newSize < 1) newSize = 1;
    if (newSize == gridDims[axisIdx]) return;

    // Walk every old cell and copy its library id into the new flat layout if
    // its coordinate is still in range on the resized axis. We can't do this
    // in place because the row-major strides change with the axis size. Cells
    // that fall outside the new size are dropped (their library entries
    // remain in the library, just unreferenced).
    std::vector<int> newDims = gridDims;
    newDims[axisIdx] = newSize;

    auto coordToFlat = [](const std::vector<int>& coord, const std::vector<int>& dims) {
        int flat = 0, stride = 1;
        for (int d = (int)dims.size() - 1; d >= 0; --d) {
            flat += coord[d] * stride;
            stride *= dims[d];
        }
        return flat;
    };

    int newTotal = 1;
    for (int d : newDims) newTotal *= d;

    std::vector<int> newCells(newTotal, -1);
    const int oldTotal = (int)cellWaveformIds.size();
    for (int oldIdx = 0; oldIdx < oldTotal; ++oldIdx) {
        auto coord = cellIdxToGridCoord(oldIdx);
        if (coord.empty()) continue;
        if (coord[axisIdx] >= newSize) continue; // dropped by shrink
        const int newIdx = coordToFlat(coord, newDims);
        newCells[newIdx] = cellWaveformIds[oldIdx];
    }
    cellWaveformIds = std::move(newCells);
    gridDims = std::move(newDims);

    // Any prior scatter-revert snapshot is invalidated by a topology change.
    scatterFromGridSnapshot.reset();
}

void WavetableDoc::convertGridToScatter() {
    // Build a fresh scatterFrames list from every non-empty grid cell.
    // Library entries are NOT moved - cells reference them by id, and the
    // library is shared across both modes. Record the source cell index so
    // canRevertScatterToGrid() can detect an unedited round-trip.
    ScatterFromGridSnapshot snap;
    snap.gridDims = gridDims;

    const int total = (int)cellWaveformIds.size();
    std::vector<ScatterFrame> newScatter;
    newScatter.reserve(total);

    const int outDims = std::max((int)gridDims.size(), 2);

    for (int cellIdx = 0; cellIdx < total; ++cellIdx) {
        const int libId = cellWaveformIds[cellIdx];
        if (libId < 0) continue;
        ScatterFrame sf;
        sf.waveformId = libId;
        sf.position = cellCenterPosition(cellIdx);
        // Pad to outDims (e.g., a 1D grid becomes 2D with y=0.5).
        while ((int)sf.position.size() < outDims) sf.position.push_back(0.5f);
        sf.colorIdx = -1;
        newScatter.push_back(std::move(sf));
        snap.originalCellIdx.push_back(cellIdx);
    }

    scatterFrames = std::move(newScatter);
    scatterDims = outDims;
    mode = WavetableMode::Scatter;
    cellWaveformIds.clear();
    scatterFromGridSnapshot = std::move(snap);
}

bool WavetableDoc::canRevertScatterToGrid() const {
    if (mode != WavetableMode::Scatter) return false;
    if (!scatterFromGridSnapshot.has_value()) return false;
    const auto& snap = *scatterFromGridSnapshot;
    if (snap.originalCellIdx.size() != scatterFrames.size()) return false;
    if (snap.gridDims.empty()) return false;

    // Compute cell centers from the snapshot's grid shape (not the current
    // gridDims, which may be empty in scatter mode) and compare componentwise.
    auto snapCellCenter = [&](int idx) -> std::vector<float> {
        std::vector<float> pos(snap.gridDims.size(), 0.5f);
        int rem = idx;
        for (int d = (int)snap.gridDims.size() - 1; d >= 0; --d) {
            const int sz = snap.gridDims[d];
            if (sz <= 0) return {};
            const int c = rem % sz;
            rem /= sz;
            pos[d] = ((float)c + 0.5f) / (float)sz;
        }
        return pos;
    };

    constexpr float tol = 1e-4f;
    for (size_t i = 0; i < scatterFrames.size(); ++i) {
        const int srcCell = snap.originalCellIdx[i];
        auto expected = snapCellCenter(srcCell);
        if (expected.empty()) return false;
        const auto& got = scatterFrames[i].position;
        // The snapshot only constrains the first snap.gridDims.size() axes;
        // any extra padding axes (e.g., y when reverting a 1D-from-grid view)
        // must still sit at 0.5 to count as "unmoved".
        for (size_t d = 0; d < got.size(); ++d) {
            const float exp = (d < expected.size()) ? expected[d] : 0.5f;
            if (std::abs(got[d] - exp) > tol) return false;
        }
    }
    return true;
}

void WavetableDoc::revertScatterToGrid() {
    if (!canRevertScatterToGrid()) {
        jassertfalse;
        return;
    }
    const auto snap = *scatterFromGridSnapshot;  // copy before we mutate
    const int total = [&]{ int n = 1; for (int d : snap.gridDims) n *= d; return n; }();

    std::vector<int> newCells(total, -1);
    for (size_t i = 0; i < scatterFrames.size(); ++i) {
        const int cell = snap.originalCellIdx[i];
        if (cell < 0 || cell >= total) continue;
        newCells[cell] = scatterFrames[i].waveformId;
    }

    cellWaveformIds = std::move(newCells);
    gridDims = snap.gridDims;
    mode = WavetableMode::Grid;
    scatterFrames.clear();
    // The snapshot is consumed by a successful revert; a fresh
    // convertGridToScatter() will install a new one if the user converts again.
    scatterFromGridSnapshot.reset();
}

WavetableDoc WavetableDoc::defaultSingleSine() {
    WavetableDoc d;
    d.tableSize = 2048;
    d.gridDims = {1}; // 1D with 1 cell
    const int libId = d.addLibraryEntry(
        std::make_unique<LayeredWaveform>(LayeredWaveform::defaultSine()),
        "Sine");
    d.cellWaveformIds.push_back(libId);
    return d;
}

WavetableDoc WavetableDoc::defaultEmpty() {
    // 1D wavetable with a single empty cell. gridDims = {1} keeps the
    // gridDims-product invariant in sync with cellWaveformIds.size(), and
    // "+ Waveform" sparse-aware placement (see LayeredWaveEditorComponent's
    // add-frame handler) finds the empty cell and slots the first real
    // waveform in without growing the grid. The library starts empty.
    WavetableDoc d;
    d.tableSize = 2048;
    d.gridDims = {1};
    d.cellWaveformIds.push_back(-1);
    return d;
}

LayeredWaveform LayeredWaveform::defaultSine() {
    LayeredWaveform w;
    WaveLayer l;
    l.shape = WaveLayer::Sine;
    l.ratio = 1;
    l.phase = 0.0f;
    l.amp   = 1.0f;
    w.layers.push_back(l);
    return w;
}

// ==============================================================================
// LayerRow - one row per layer
// ==============================================================================

// Sample one layer's contribution over one cycle (amp-scaled, not normalized).
// Used for the per-layer mini preview.
static void renderSingleLayer(const WaveLayer& layer, int tableSize,
                              std::vector<float>& out)
{
    out.assign(tableSize, 0.0f);
    std::mt19937 rng(1234u + (unsigned)layer.ratio * 31u + (unsigned)layer.shape * 7u);
    // For Drawn shapes, show one cycle in the preview so the control
    // points line up with the curve. The harmonic ratio still applies
    // in the final multi-layer render (LayeredWaveform::render).
    int r = (layer.shape == WaveLayer::Drawn) ? 1 : std::max(1, layer.ratio);
    for (int i = 0; i < tableSize; ++i) {
        float phase = (float)i / (float)tableSize;
        float x = phase * (float)r + layer.phase;
        out[i] = layer.amp * sampleLayer(layer, x, rng);
    }
}

class LayeredWaveEditorComponent::LayerRow : public juce::Component {
public:
    LayerRow(LayeredWaveEditorComponent& owner_, int index_)
        : owner(owner_), index(index_)
    {
        addAndMakeVisible(label);
        label.setJustificationType(juce::Justification::centredLeft);
        label.setFont(13.0f);

        auto addShapeBtn = [this](juce::TextButton& b, const char* name, WaveLayer::Shape s) {
            addAndMakeVisible(b);
            b.setButtonText(name);
            b.setClickingTogglesState(true);
            b.setRadioGroupId(0); // we'll handle toggling manually
            b.onClick = [this, s]() {
                auto& l = owner.currentLayers()[index];
                l.shape = s;
                // Seed a fresh Drawn layer with a few points so the user has
                // something grabable instead of an empty canvas.
                if (s == WaveLayer::Drawn && l.drawnPoints.empty())
                    l.drawnPoints = defaultDrawnPoints();
                updateShapeButtons();
                refreshPreview();
                owner.onLayerChanged();
            };
        };
        addShapeBtn(sineBtn,     "Sine",     WaveLayer::Sine);
        addShapeBtn(sawBtn,      "Saw",      WaveLayer::Saw);
        addShapeBtn(squareBtn,   "Square",   WaveLayer::Square);
        addShapeBtn(triangleBtn, "Triangle", WaveLayer::Triangle);
        addShapeBtn(noiseBtn,    "Noise",    WaveLayer::Noise);
        addShapeBtn(drawnBtn,    "Draw",     WaveLayer::Drawn);
        addShapeBtn(formulaBtn,  "Formula",  WaveLayer::Formula);

        addAndMakeVisible(freehandToggle);
        freehandToggle.setButtonText("Points");
        freehandToggle.setTooltip("Toggle between Points mode (click to place control points with smooth interpolation) "
                                  "and Freehand mode (click and drag to draw the waveform shape directly).");
        freehandToggle.onClick = [this]() {
            auto& l = owner.currentLayers()[index];
            l.freehandMode = !l.freehandMode;
            freehandToggle.setButtonText(l.freehandMode ? "Freehand" : "Points");
            // Seed freehand samples if switching to freehand for the first time.
            if (l.freehandMode && l.drawnSamples.empty())
                l.drawnSamples = defaultFreehandSamples();
            refreshPreview();
            owner.onLayerChanged();
        };
        freehandToggle.setVisible(false); // only visible when shape == Drawn

        // Formula expression editor - only visible when shape == Formula.
        // Uses the same WaveExprParser vocabulary as the freq-domain editor:
        // sin, cos, tan, exp, log, sqrt, pow, abs, tanh, clamp,
        // saw(x), square(x), triangle(x), noise(), random, pi, e, + - * / ^.
        // `x` ranges over [0, 2*pi) for one cycle.
        addAndMakeVisible(formulaEditor);
        formulaEditor.setMultiLine(false);
        formulaEditor.setReturnKeyStartsNewLine(false);
        formulaEditor.setTooltip("Expression in `x` (radians, 0 to 2*pi over one cycle). "
                                 "Vocabulary: sin, cos, tan, exp, log, sqrt, pow, abs, tanh, clamp, "
                                 "saw(x), square(x), triangle(x), noise(), random, pi, e. "
                                 "Output is clamped to -1..1.");
        formulaEditor.setText("sin(x)", juce::dontSendNotification);
        formulaEditor.onTextChange = [this]() {
            auto& l = owner.currentLayers()[index];
            l.formulaExpr = formulaEditor.getText().toStdString();
            refreshPreview();
            owner.onLayerChanged();
        };
        formulaEditor.setVisible(false);

        auto setupSlider = [this](juce::Slider& sl, double lo, double hi, double step, const char* suffix) {
            addAndMakeVisible(sl);
            sl.setSliderStyle(juce::Slider::LinearHorizontal);
            sl.setTextBoxStyle(juce::Slider::TextBoxRight, false, 55, 18);
            sl.setRange(lo, hi, step);
            sl.setTextValueSuffix(suffix);
            sl.onValueChange = [this]() {
                auto& l = owner.currentLayers()[index];
                l.ratio = (int)ratioSlider.getValue();
                l.phase = (float)phaseSlider.getValue();
                l.amp   = (float)ampSlider.getValue();
                refreshPreview();
                owner.onLayerChanged();
            };
        };
        setupSlider(ratioSlider, 1.0, 16.0, 1.0, "x");
        setupSlider(phaseSlider, 0.0, 1.0, 0.01, "");
        setupSlider(ampSlider,   0.0, 1.0, 0.01, "");
        ratioSlider.setTooltip("Harmonic ratio: how many times faster this layer cycles than the fundamental. "
                               "1 = root pitch, 2 = one octave up, 3 = one octave + a fifth, etc. Higher numbers add brighter overtones.");
        phaseSlider.setTooltip("Phase offset (0 to 1): shifts where in its cycle this layer starts. "
                               "Affects how layers add up when summed - different phases give different timbres.");
        ampSlider.setTooltip("Amplitude (0 to 1): how loud this layer is in the final sum. 0 = silent, 1 = full volume. "
                             "Use to balance layers against each other.");

        addAndMakeVisible(ratioLabel);
        addAndMakeVisible(phaseLabel);
        addAndMakeVisible(ampLabel);
        ratioLabel.setText("Harmonic",  juce::dontSendNotification);
        phaseLabel.setText("Phase",     juce::dontSendNotification);
        ampLabel  .setText("Amplitude", juce::dontSendNotification);
        for (auto* l : { &ratioLabel, &phaseLabel, &ampLabel }) {
            l->setFont(11.0f);
            l->setJustificationType(juce::Justification::centredLeft);
        }

        addAndMakeVisible(deleteBtn);
        deleteBtn.setButtonText("X");
        deleteBtn.onClick = [this]() {
            owner.currentLayers().erase(owner.currentLayers().begin() + index);
            owner.rebuildRows();
            owner.onLayerChanged();
        };
    }

    void syncFromModel() {
        auto& l = owner.currentLayers()[index];
        label.setText("Layer " + juce::String(index + 1), juce::dontSendNotification);
        ratioSlider.setValue(l.ratio, juce::dontSendNotification);
        phaseSlider.setValue(l.phase, juce::dontSendNotification);
        ampSlider  .setValue(l.amp,   juce::dontSendNotification);
        updateShapeButtons();
        freehandToggle.setVisible(l.shape == WaveLayer::Drawn);
        freehandToggle.setButtonText(l.freehandMode ? "Freehand" : "Points");
        formulaEditor.setVisible(l.shape == WaveLayer::Formula);
        if (l.shape == WaveLayer::Formula)
            formulaEditor.setText(l.formulaExpr, juce::dontSendNotification);
        refreshPreview();
    }

    void refreshPreview() {
        if (index < 0 || index >= (int)owner.currentLayers().size()) return;
        renderSingleLayer(owner.currentLayers()[index], 512, previewSamples);
        repaint();
    }

    void updateShapeButtons() {
        auto& l = owner.currentLayers()[index];
        sineBtn    .setToggleState(l.shape == WaveLayer::Sine,     juce::dontSendNotification);
        sawBtn     .setToggleState(l.shape == WaveLayer::Saw,      juce::dontSendNotification);
        squareBtn  .setToggleState(l.shape == WaveLayer::Square,   juce::dontSendNotification);
        triangleBtn.setToggleState(l.shape == WaveLayer::Triangle, juce::dontSendNotification);
        noiseBtn   .setToggleState(l.shape == WaveLayer::Noise,    juce::dontSendNotification);
        drawnBtn   .setToggleState(l.shape == WaveLayer::Drawn,    juce::dontSendNotification);
        formulaBtn .setToggleState(l.shape == WaveLayer::Formula,  juce::dontSendNotification);
        freehandToggle.setVisible(l.shape == WaveLayer::Drawn);
        freehandToggle.setButtonText(l.freehandMode ? "Freehand" : "Points");
        formulaEditor.setVisible(l.shape == WaveLayer::Formula);
        if (l.shape == WaveLayer::Formula
            && formulaEditor.getText().toStdString() != l.formulaExpr)
        {
            formulaEditor.setText(l.formulaExpr, juce::dontSendNotification);
        }
    }

    void resized() override {
        auto a = getLocalBounds().reduced(4);
        auto top = a.removeFromTop(22);
        label.setBounds(top.removeFromLeft(70));
        deleteBtn.setBounds(top.removeFromRight(22));

        // Shape button row - 7 buttons (sine/saw/square/triangle/noise/draw/formula)
        auto btnRow = a.removeFromTop(24);
        int bw = btnRow.getWidth() / 7;
        sineBtn    .setBounds(btnRow.removeFromLeft(bw));
        sawBtn     .setBounds(btnRow.removeFromLeft(bw));
        squareBtn  .setBounds(btnRow.removeFromLeft(bw));
        triangleBtn.setBounds(btnRow.removeFromLeft(bw));
        noiseBtn   .setBounds(btnRow.removeFromLeft(bw));
        drawnBtn   .setBounds(btnRow.removeFromLeft(bw));
        formulaBtn .setBounds(btnRow);

        // Sub-row: Freehand/Points toggle (Drawn) or Formula text editor (Formula).
        // Always reserve the height so the slider rows below don't jump when
        // toggling shape.
        auto subRow = a.removeFromTop(24);
        if (freehandToggle.isVisible()) {
            freehandToggle.setBounds(subRow.removeFromLeft(100));
        } else if (formulaEditor.isVisible()) {
            formulaEditor.setBounds(subRow);
        }

        // Reserve space for the mini preview (bottom of row)
        a.removeFromBottom(previewHeight);

        // Slider rows
        auto sliderRow = [&](juce::Label& lab, juce::Slider& sl) {
            auto r = a.removeFromTop(20);
            lab.setBounds(r.removeFromLeft(70));
            sl.setBounds(r);
        };
        sliderRow(ratioLabel, ratioSlider);
        sliderRow(phaseLabel, phaseSlider);
        sliderRow(ampLabel,   ampSlider);
    }

    void paint(juce::Graphics& g) override {
        g.setColour(juce::Colour(40, 40, 50));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
        g.setColour(juce::Colour(70, 70, 90));
        g.drawRoundedRectangle(getLocalBounds().toFloat(), 4.0f, 1.0f);

        // Mini waveform preview for just this layer's contribution
        auto bounds = getLocalBounds().reduced(6).toFloat();
        auto previewArea = bounds.removeFromBottom((float)previewHeight).reduced(2.0f);

        g.setColour(juce::Colour(24, 24, 30));
        g.fillRoundedRectangle(previewArea, 3.0f);

        // Center line
        float cy = previewArea.getCentreY();
        g.setColour(juce::Colours::grey.withAlpha(0.25f));
        g.drawHorizontalLine((int)cy, previewArea.getX(), previewArea.getRight());

        if (!previewSamples.empty()) {
            float cx = previewArea.getX() + 2;
            float w  = previewArea.getWidth() - 4;
            float h  = previewArea.getHeight() - 4;

            juce::Path p;
            int n = (int)previewSamples.size();
            for (int i = 0; i < n; ++i) {
                float x = cx + (float)i / (float)(n - 1) * w;
                float y = cy - previewSamples[i] * h * 0.45f;
                if (i == 0) p.startNewSubPath(x, y);
                else p.lineTo(x, y);
            }
            g.setColour(juce::Colour(150, 200, 255));
            g.strokePath(p, juce::PathStrokeType(1.3f));

            // For Drawn layers in Points mode, overlay the control points so
            // the user can see and grab them.
            const auto& layer = owner.currentLayers()[index];
            if (layer.shape == WaveLayer::Drawn && !layer.freehandMode) {
                for (int i = 0; i < (int)layer.drawnPoints.size(); ++i) {
                    const auto& pt = layer.drawnPoints[i];
                    // Scale y by amp because the preview renders amp*shape,
                    // so the visible curve is also amp-scaled.
                    float x = cx + pt.first * w;
                    float y = cy - (pt.second * layer.amp) * h * 0.45f;
                    bool isDragged = (i == draggingIdx);
                    g.setColour(isDragged ? juce::Colours::yellow : juce::Colours::white);
                    g.fillEllipse(x - 3.0f, y - 3.0f, 6.0f, 6.0f);
                    g.setColour(juce::Colour(60, 90, 140));
                    g.drawEllipse(x - 3.0f, y - 3.0f, 6.0f, 6.0f, 1.0f);
                }
            }
        }
    }

    static constexpr int previewHeight = 92;
    static int rowHeight() { return 22 + 24 + 24 + 20 * 3 + 12 + previewHeight + 4; }

    juce::Rectangle<float> getPreviewAreaBounds() const {
        auto bounds = getLocalBounds().reduced(6).toFloat();
        return bounds.removeFromBottom((float)previewHeight).reduced(2.0f);
    }

    // Convert a mouse position in component coordinates to (x, y) in the
    // normalized space used by drawnPoints: x in [0, 1), y in [-1, 1].
    // Returns true if p is inside the preview area.
    bool mouseToPointXY(juce::Point<float> p, float& outX, float& outY) const {
        auto area = getPreviewAreaBounds();
        if (!area.contains(p)) return false;
        outX = (p.x - area.getX()) / juce::jmax(1.0f, area.getWidth());
        outY = 1.0f - 2.0f * (p.y - area.getY()) / juce::jmax(1.0f, area.getHeight());
        outX = juce::jlimit(0.0f, 0.999f, outX);
        outY = juce::jlimit(-1.0f, 1.0f, outY);
        return true;
    }

    // Find the closest point to (x, y), returning its index, or -1 if none
    // is within `radius` (in normalized coordinates, where x spans 1 unit
    // and y spans 2 units).
    int findPointNear(float x, float y, float radius = 0.05f) const {
        auto& pts = owner.currentLayers()[index].drawnPoints;
        int best = -1;
        float bestD2 = radius * radius;
        for (int i = 0; i < (int)pts.size(); ++i) {
            float dx = pts[i].first - x;
            float dy = (pts[i].second - y) * 0.5f; // compress y to match x scale
            float d2 = dx * dx + dy * dy;
            if (d2 < bestD2) { bestD2 = d2; best = i; }
        }
        return best;
    }

    void sortPointsByX() {
        auto& pts = owner.currentLayers()[index].drawnPoints;
        std::sort(pts.begin(), pts.end(),
                  [](const std::pair<float,float>& a, const std::pair<float,float>& b) {
                      return a.first < b.first;
                  });
    }

    // Write freehand sample data at normalized position x with value y,
    // interpolating between the previous write position and the current one
    // so there are no gaps when dragging quickly.
    void writeFreehandSample(float x, float y) {
        auto& samples = owner.currentLayers()[index].drawnSamples;
        if (samples.empty()) samples = defaultFreehandSamples();
        int n = (int)samples.size();
        int idx = juce::jlimit(0, n - 1, (int)(x * (float)n));
        if (lastFreehandIdx >= 0 && lastFreehandIdx != idx) {
            // Interpolate between last and current to avoid gaps.
            int from = lastFreehandIdx;
            int to = idx;
            float fromY = lastFreehandY;
            float toY = y;
            int steps = std::abs(to - from);
            int dir = (to > from) ? 1 : -1;
            for (int s = 0; s <= steps; ++s) {
                int si = from + s * dir;
                if (si < 0 || si >= n) continue;
                float t = (steps > 0) ? (float)s / (float)steps : 1.0f;
                samples[si] = fromY + (toY - fromY) * t;
            }
        } else {
            samples[idx] = y;
        }
        lastFreehandIdx = idx;
        lastFreehandY = y;
    }

    void mouseDown(const juce::MouseEvent& e) override {
        auto& l = owner.currentLayers()[index];
        if (l.shape != WaveLayer::Drawn) return;
        float x, y;
        if (!mouseToPointXY(e.position, x, y)) return;

        if (l.freehandMode) {
            // Freehand: start drawing samples
            freehandDrawing = true;
            lastFreehandIdx = -1;
            writeFreehandSample(x, y);
            refreshPreview();
            owner.onLayerChanged();
            return;
        }

        // Points mode (original behavior)
        auto& pts = l.drawnPoints;
        int hit = findPointNear(x, y);
        if (e.mods.isShiftDown() && hit >= 0) {
            // Shift-click a point to delete it (keep at least 2 points so
            // interpolation has something to work with).
            if ((int)pts.size() > 2) {
                pts.erase(pts.begin() + hit);
                draggingIdx = -1;
                refreshPreview();
                owner.onLayerChanged();
            }
            return;
        }
        if (hit >= 0) {
            draggingIdx = hit;
        } else {
            // Add a new point at the cursor, then sort by x so interpolation stays valid.
            pts.emplace_back(x, y);
            sortPointsByX();
            // After sorting, re-find the point we just added so we can continue
            // dragging it.
            draggingIdx = -1;
            for (int i = 0; i < (int)pts.size(); ++i)
                if (std::abs(pts[i].first - x) < 1e-5f && std::abs(pts[i].second - y) < 1e-5f)
                    { draggingIdx = i; break; }
            refreshPreview();
            owner.onLayerChanged();
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        auto& l = owner.currentLayers()[index];
        if (l.shape != WaveLayer::Drawn) return;

        if (l.freehandMode && freehandDrawing) {
            auto area = getPreviewAreaBounds();
            auto cp = e.position;
            cp.x = juce::jlimit(area.getX(), area.getRight() - 1.0f, cp.x);
            cp.y = juce::jlimit(area.getY(), area.getBottom(), cp.y);
            float x, y;
            mouseToPointXY(cp, x, y);
            writeFreehandSample(x, y);
            refreshPreview();
            owner.onLayerChanged();
            return;
        }

        // Points mode
        if (draggingIdx < 0) return;
        auto& pts = l.drawnPoints;
        if (draggingIdx >= (int)pts.size()) { draggingIdx = -1; return; }
        float x, y;
        // Use clamped conversion so dragging outside the area still moves the point.
        auto area = getPreviewAreaBounds();
        auto cp = e.position;
        cp.x = juce::jlimit(area.getX(), area.getRight() - 1.0f, cp.x);
        cp.y = juce::jlimit(area.getY(), area.getBottom(), cp.y);
        mouseToPointXY(cp, x, y);
        pts[draggingIdx] = { x, y };
        // Re-sort after movement since x may have changed order.
        // Remember old position so we can re-find after sort.
        float ox = x, oy = y;
        sortPointsByX();
        draggingIdx = -1;
        for (int i = 0; i < (int)pts.size(); ++i)
            if (std::abs(pts[i].first - ox) < 1e-5f && std::abs(pts[i].second - oy) < 1e-5f)
                { draggingIdx = i; break; }
        refreshPreview();
        owner.onLayerChanged();
    }

    void mouseUp(const juce::MouseEvent&) override {
        draggingIdx = -1;
        freehandDrawing = false;
        lastFreehandIdx = -1;
    }

private:
    LayeredWaveEditorComponent& owner;
    int index;

    juce::Label label;
    juce::TextButton sineBtn, sawBtn, squareBtn, triangleBtn, noiseBtn, drawnBtn, formulaBtn;
    juce::TextButton freehandToggle;
    juce::TextEditor formulaEditor;
    juce::Slider ratioSlider, phaseSlider, ampSlider;
    juce::Label  ratioLabel, phaseLabel, ampLabel;
    juce::TextButton deleteBtn;
    std::vector<float> previewSamples;
    int draggingIdx = -1;

    // Freehand drawing state
    bool freehandDrawing = false;
    int  lastFreehandIdx = -1;
    float lastFreehandY = 0.0f;
};

// ==============================================================================
// ScatterView - N-D scatter wavetable viewport
// ==============================================================================
//
// Renders frames as labeled colored dots projected to 2D, with the current
// Position cursor as a crosshair. The user can:
//   - Click empty space to move the Position cursor (the playhead) there.
//   - Click a frame dot to select it (so its layers become editable below).
//   - Drag a frame dot to move it.
//   - Right-button drag to orbit the camera (yaw/pitch) in any 3D mode.
//   - Pick which axis pair (or triple, in 3D) to view via the parent's
//     projection combo.
//   - Pick a 3D viewing mode via the parent's stereo-mode combo:
//       * Off            - flat 2D projection.
//       * Anaglyph       - one composite image with red/cyan eye channels;
//                          requires red/cyan paper glasses.
//       * Cross-eyed     - side-by-side stereoscope; the viewer crosses
//                          their eyes so the LEFT half is seen by the RIGHT
//                          eye and vice versa.
//       * Parallel       - side-by-side stereoscope viewed "wall-eyed" -
//                          the LEFT half is seen by the LEFT eye and the
//                          RIGHT half by the RIGHT eye (the opposite of
//                          cross-eyed; this is how a Holmes stereoscope or
//                          a VR headset presents the pair).
class LayeredWaveEditorComponent::ScatterView : public juce::Component,
                                                public juce::SettableTooltipClient,
                                                public juce::DragAndDropTarget {
public:
    explicit ScatterView(LayeredWaveEditorComponent& o) : owner(o) {}

    enum class StereoMode { Off, Anaglyph, CrossEyed, Parallel };

    // Which axes are projected to screen X / Y / Z (Z only used in 3D modes).
    int axisX = 0, axisY = 1, axisZ = 2;
    StereoMode stereoMode = StereoMode::Off;

    // Wireframe corner-count cap. The hypercube grows as 2^N edges and
    // becomes visually meaningless past about 6 dims (64 corners, 192
    // edges) — taller wavetables still render and snap to the cube, the
    // wireframe just stops adding further axes.
    static constexpr int kMaxRenderDims = 6;

    // Paint-scoped view transform. Each paintScene() begins by calling
    // computeViewTransform(area), which projects every N-hypercube corner
    // through the current rotation, finds the bbox, and picks a scale +
    // centre so the projected hypercube fills `area` with a small margin
    // regardless of rotation. projectPoint() / screenToPosition() then
    // read these cached values instead of the old fixed-0.48 *min(W,H)
    // formula. Without this, a rotated cube's projection shrinks below
    // the available view (the old code computed a scale that fit only the
    // un-rotated [-0.5..+0.5] axis range), and edges of the wireframe
    // wouldn't extend to where dots near the corners actually drew.
    // Mutable: paint state, not user-visible state — modified from const
    // helpers like screenToPosition / projectPoint indirectly.
    mutable float renderScale = 1.0f;
    mutable float renderCx = 0.0f;
    mutable float renderCy = 0.0f;

    // Apply the owner's N-D rotation to a position vector. The owner
    // owns the rotation state (`scatterPlaneAngles`) so the embedded
    // view and the pop-out wavetable-view window stay in lock-step -
    // dragging the embedded view or moving a slider in the pop-out both
    // update the same vector, and both views repaint together.
    // Rotations are composed in lexicographic plane order (see
    // LayeredWaveEditorComponent::planePairs). Centred at 0.5 because
    // the unit cube lives in [0, 1]^N.
    std::vector<float> rotateNd(std::vector<float> p) const;

    // Any of the stereo modes count as "3D" for projection / pin / hit-test
    // purposes - they all need yaw/pitch rotation, the Z axis read, etc.
    bool is3D() const { return stereoMode != StereoMode::Off; }
    bool isSplitView() const {
        return stereoMode == StereoMode::CrossEyed
            || stereoMode == StereoMode::Parallel;
    }

    // Real-world stereo geometry. The parent editor pushes these in from
    // its sliders so the parallax matches the user's actual eyes / screen.
    float ipdMm = 63.0f;          // interpupillary distance
    float viewingDistMm = 600.0f; // distance from eyes to screen
    float sceneDepthMm  = 60.0f;  // physical depth of the unit cube
    float dpi = 96.0f;            // screen DPI (auto-detected)

    void setProjection(int ax, int ay, int az) { axisX = ax; axisY = ay; axisZ = az; repaint(); }

    // A monoscopic projection (one camera, no parallax) plus the
    // post-rotation depth so applyParallax can compute disparity.
    struct Projected { float x, y; float worldZ; };

    // Project an N-D position to a 2D screen point - without stereo offset.
    // In any 3D mode this includes yaw/pitch rotation and perspective scaling
    // (size only - both eyes get the same shape).
    Projected projectPoint(const std::vector<float>& pos,
                           const juce::Rectangle<float>& area) const
    {
        (void)area;  // transform is cached in renderCx/Cy/Scale - see
                     // computeViewTransform(); area is read there, not here.
        // Apply the full N-D rotation first, then pick the projected
        // axes out of the rotated vector. Plane angles drive everything
        // - the screen-X/Y axes pull from rotated[axisX]/rotated[axisY],
        // and the screen-Z (used for perspective + parallax in any 3D
        // mode) pulls from rotated[axisZ]. With all plane angles at zero
        // the rotation is identity and the projection matches the
        // original "axis pick" behaviour exactly.
        auto rotated = rotateNd(pos);
        int nd = (int)rotated.size();
        float px = (axisX >= 0 && axisX < nd) ? rotated[axisX] : 0.5f;
        float py = (axisY >= 0 && axisY < nd) ? rotated[axisY] : 0.5f;
        float pz = (is3D() && axisZ >= 0 && axisZ < nd) ? rotated[axisZ] : 0.5f;

        float x = px - 0.5f, y = py - 0.5f, z = pz - 0.5f;

        // No perspective size-scaling in 3D modes - earlier revisions
        // applied pers = fov / (fov - z) with fov = 1.6, which produced a
        // very pronounced near/far warp (~1.45x at the near corners, 0.76x
        // at the far ones). For a small abstract wireframe this read as
        // distorting the geometry rather than adding depth. Parallax (in
        // applyParallax) still uses the raw z to drive stereo disparity,
        // so stereo modes still fuse with depth - we just keep the
        // monoscopic projection orthographic.

        // Axonometric contribution from every dim that isn't one of the
        // explicitly projected ones (axisX, axisY, and in 3D modes axisZ).
        // Without this, a rotation in a plane whose BOTH axes are non-
        // projected (e.g. Z-W in Flat 2D view with N=4) is mathematically
        // invisible: rotateNd modifies rotated[Z] and rotated[W], neither
        // of which feeds the projection. The fix gives each non-projected
        // dim a small screen-space direction, so any change to its rotated
        // value visibly nudges the projection. Directions are spread around
        // a half-circle so successive non-projected dims pull in distinct
        // diagonal directions; weight is small (0.22) to keep the view
        // feeling 2D rather than pseudo-3D.
        addAxonometric(rotated, x, y);

        // Map the centred coordinate via the fixed scale transform set up
        // by computeViewTransform(). renderCx/Cy are just the area centre,
        // so this is a single multiply-add to scale + recentre.
        return { renderCx + x * renderScale, renderCy + y * renderScale, z };
    }

    // See projectPoint comment for the rationale. Pulled out so
    // computeViewTransform's worst-case extent calculation can share the
    // exact same transform math.
    void addAxonometric(const std::vector<float>& rotated,
                        float& x, float& y) const
    {
        const int nd = (int)rotated.size();
        int kth = 0;
        for (int d = 0; d < nd; ++d) {
            if (d == axisX) continue;
            if (d == axisY) continue;
            if (is3D() && d == axisZ) continue;
            // Spread non-projected dims around a half-circle, starting at
            // -60deg (upper-right) and stepping -45deg per dim. These are
            // never axis-aligned, so they never coincide with axisX/axisY
            // direction and the axonometric contribution always has a
            // distinct visual signature.
            const float base = -juce::MathConstants<float>::pi / 3.0f;
            const float step = -juce::MathConstants<float>::pi / 4.0f;
            float angle = base + (float)kth * step;
            const float w = 0.22f;
            float c = std::cos(angle), s = std::sin(angle);
            float r = rotated[(size_t)d] - 0.5f;
            x += w * r * c;
            y += w * r * s;
            ++kth;
        }
    }

    // Compute the paint-scoped view transform (renderScale, renderCx,
    // renderCy). Picks a single constant scale that fits the worst-case
    // projected extent of the unit N-hypercube across ALL possible
    // rotations, instead of fitting the current bbox.
    //
    // Why constant instead of per-rotation fit: the old fit-bbox approach
    // made the wireframe "breathe" as you rotated - a square rotated 45deg
    // has a smaller axis-aligned bbox than the un-rotated square, so scale
    // grew, then shrank again as you kept rotating. The visual result was
    // a wireframe that resized constantly. Locking the scale to the
    // worst-case extent means the cube can't grow under any rotation; it
    // shrinks slightly into the visible area at non-worst-case angles but
    // never gets cropped.
    //
    // Worst-case extent: every corner of [0,1]^N is at distance sqrt(N)/2
    // from the centre, so under ANY rotation the projected coord of any
    // corner lies in [-sqrt(N)/2, +sqrt(N)/2] on each screen axis. Max
    // span = sqrt(N). On top of that:
    //   - axonometric: each non-projected dim adds up to weight*0.5 per
    //     axis (worst when its rotated value hits 0 or 1). Padded as
    //     0.5 * 0.22 * numNonProj per side, doubled for both sides.
    //   - perspective: removed - projectPoint is now purely orthographic
    //     in both 2D and 3D modes (parallax is still applied via z, so
    //     stereo still fuses with depth).
    void computeViewTransform(juce::Rectangle<float> area) const {
        int N = std::max(2, std::min(kMaxRenderDims, owner.wave.numDimensions()));

        // Number of dims that don't feed directly into screen X/Y(/Z).
        int projDims = 2 + (is3D() ? 1 : 0);
        int nonProj  = std::max(0, N - projDims);

        // Cube diagonal in projected screen coords (worst-case under any rotation)
        float worst = std::sqrt((float)N);
        // Axonometric padding: each non-projected dim contributes up to
        // weight*1.0 across its [0,1] range, half on each side; sum across dims.
        worst += 0.22f * (float)nonProj;

        // Margin: leave ~3% on each side so dot outlines and the cell-coord
        // labels printed below dots don't clip at the edge.
        constexpr float marginFactor = 0.94f;
        float fit = std::min(area.getWidth(), area.getHeight()) * marginFactor;
        renderScale = fit / std::max(1e-3f, worst);
        renderCx = area.getCentreX();
        renderCy = area.getCentreY();
    }

    // Compute the stereoscopic disparity in screen pixels for a point at
    // post-rotation depth `worldZ` (in [-0.5..+0.5] world units).
    //
    // Geometry: place the screen plane at z=0, viewer's eyes at -D (mm in
    // front), eye separation = IPD. A point at depth z_mm behind the screen
    // appears at parallax (IPD/2) * z_mm/(D+z_mm) per eye (positive = right
    // eye image moves right, left eye image moves left -> uncrossed disparity
    // -> object fuses behind the screen). For a point in front (z_mm<0) the
    // formula naturally yields crossed disparity.
    juce::Point<float> applyParallax(const Projected& pp, int eyeSign) const {
        if (!is3D() || eyeSign == 0) return { pp.x, pp.y };
        float z_mm = pp.worldZ * sceneDepthMm; // post-rotation z mapped to physical mm
        float denom = viewingDistMm + z_mm;
        if (std::abs(denom) < 1e-3f) return { pp.x, pp.y };
        float disparity_mm = (ipdMm * 0.5f) * z_mm / denom;
        float disparity_px = disparity_mm * dpi / 25.4f;
        return { pp.x + (float)eyeSign * disparity_px, pp.y };
    }

    // Backward-compat wrapper used by drawSquare2D / drawCube3D etc.
    juce::Point<float> projectToScreen(const std::vector<float>& pos,
                                       const juce::Rectangle<float>& area) const
    {
        auto p = projectPoint(pos, area);
        return { p.x, p.y };
    }

    // In side-by-side stereoscope modes the viewport is split into two
    // half-rectangles. Pick which half a mouse position lives in (and
    // return the corresponding sub-rectangle). For Off / Anaglyph this
    // returns the full rectangle.
    juce::Rectangle<float> sceneRectForPoint(juce::Point<float> p,
                                             juce::Rectangle<float> full) const
    {
        if (!isSplitView()) return full;
        float halfW = full.getWidth() * 0.5f;
        if (p.x < full.getCentreX())
            return full.withWidth(halfW);
        return full.withTrimmedLeft(halfW);
    }

    static juce::Colour autoColorForFrame(const ScatterFrame& sf,
                                          const IWavetableFrame* resolvedWave,
                                          int idx) {
        const juce::Colour palette[] = {
            juce::Colour(0xff5fb3ff), juce::Colour(0xffff7373),
            juce::Colour(0xff8aff80), juce::Colour(0xffffd24c),
            juce::Colour(0xffd084ff), juce::Colour(0xff6effe0),
            juce::Colour(0xffff9ad1), juce::Colour(0xffffb86b),
        };
        if (sf.colorIdx >= 0) {
            // user-picked palette index
            return palette[sf.colorIdx % 8];
        }
        // For layered frames we can derive a spectral centroid -> hue map
        // (low = warm red, high = cool blue). For non-layered frames
        // (spectral / wavelet / empty) we fall back to a per-index palette
        // rotation so multiple new dots are still visually distinct from
        // each other rather than all collapsing to one default colour.
        float centroid = 0.0f, mass = 0.0f;
        if (auto* lw = dynamic_cast<const LayeredWaveform*>(resolvedWave)) {
            for (auto& l : lw->layers) {
                float w = std::abs(l.amp);
                centroid += (float)std::max(1, l.ratio) * w;
                mass += w;
            }
        }
        if (mass < 1e-9f) {
            // No usable layer info -> palette by frame index.
            return palette[((idx % 8) + 8) % 8];
        }
        centroid /= mass;
        // Map centroid 1..16 -> hue 0.05 (warm orange) .. 0.66 (blue)
        float t = juce::jlimit(0.0f, 1.0f, (centroid - 1.0f) / 15.0f);
        float hue = 0.05f + t * 0.61f;
        return juce::Colour::fromHSV(hue, 0.65f, 0.95f, 1.0f);
    }

    void paint(juce::Graphics& g) override {
        auto full = getLocalBounds().toFloat().reduced(2.0f);
        g.setColour(juce::Colour(18, 18, 24));
        g.fillRoundedRectangle(full, 4.0f);
        g.setColour(juce::Colour(60, 60, 78));
        g.drawRoundedRectangle(full, 4.0f, 1.0f);

        // Inline legend painted once over the full viewport (not per-pane in
        // stereoscope mode - it'd just duplicate noise).
        drawLegend(g, full);

        if (isSplitView()) {
            // Two physical panes side-by-side. Conventions:
            //   Cross-eyed: viewer crosses eyes, so LEFT pane = RIGHT eye image.
            //   Parallel:   viewer's eyes stay parallel (wall-eyed / VR style),
            //               so LEFT pane = LEFT eye image.
            float halfW = full.getWidth() * 0.5f;
            auto leftPane  = full.withWidth(halfW);
            auto rightPane = full.withTrimmedLeft(halfW);
            // Subtle separator line so the eye knows where the split is.
            g.setColour(juce::Colour(40, 40, 56));
            g.drawLine(full.getCentreX(), full.getY() + 4.0f,
                       full.getCentreX(), full.getBottom() - 4.0f, 1.0f);
            int leftEyeSign  = (stereoMode == StereoMode::Parallel) ? -1 : +1;
            int rightEyeSign = -leftEyeSign;
            paintScene(g, leftPane,  leftEyeSign);
            paintScene(g, rightPane, rightEyeSign);
        } else {
            // Off (eyeSign 0) and Anaglyph (handled internally by paintScene)
            // both render once over the whole viewport.
            paintScene(g, full, 0);
        }

        // Drop / cell-drag indicators painted on top so they sit above
        // the dots and wireframe.
        drawDragOverlays(g, full);
    }

    // Highlight rings for in-progress drags. Two cases:
    //   * Grid cell-drag in progress: paint a green ring around the
    //     destination cell so the user sees where the swap will land.
    //   * Library drop in progress: paint a green ring at the drop
    //     target (a grid cell in Grid mode, the cursor position in
    //     Scatter mode).
    // Both use the same green for visual consistency - drop = drop,
    // regardless of source.
    void drawDragOverlays(juce::Graphics& g, juce::Rectangle<float> full) {
        auto ringAtScreen = [&g](juce::Point<float> p, float radius) {
            g.setColour(juce::Colour(0xff5be36e).withAlpha(0.95f));
            g.drawEllipse(p.x - radius, p.y - radius,
                          radius * 2.0f, radius * 2.0f, 2.0f);
            g.setColour(juce::Colour(0xff5be36e).withAlpha(0.18f));
            g.fillEllipse(p.x - radius, p.y - radius,
                          radius * 2.0f, radius * 2.0f);
        };

        auto ringAtCell = [&](int cellIdx) {
            if (cellIdx < 0 || cellIdx >= owner.wave.gridCellCount()) return;
            auto area = sceneRectForPoint(full.getCentre(), full);
            computeViewTransform(area);
            auto cellPos = owner.wave.cellCenterPosition(cellIdx);
            int needN = std::max(2, owner.wave.numDimensions());
            while ((int)cellPos.size() < needN) cellPos.push_back(0.5f);
            auto sp = projectToScreen(cellPos, area);
            ringAtScreen(sp, 12.0f);
        };

        // 1. Grid cell-drag preview (destination cell).
        if (dragCellSrcIdx >= 0 && dragCellDstIdx >= 0
            && dragCellDstIdx != dragCellSrcIdx
            && owner.wave.mode == WavetableMode::Grid) {
            ringAtCell(dragCellDstIdx);
        }

        // 2. Library-row drop hover.
        if (hoverDropActive) {
            if (owner.wave.mode == WavetableMode::Grid) {
                ringAtCell(hoverDropCellIdx);
            } else {
                ringAtScreen(hoverDropScreenPt, 10.0f);
            }
        }
    }

    // Render one stereoscopic eye-view (or the flat monoscopic / anaglyph
    // composite view) into a sub-rectangle of the viewport.
    //   eyeSign  -1 = left eye image, +1 = right eye image, 0 = no parallax.
    //   In Anaglyph mode the eyeSign argument is ignored and both red+cyan
    //   passes are drawn into the same `area`.
    void paintScene(juce::Graphics& g, juce::Rectangle<float> area, int eyeSign) {
        // Compute the fit-scale transform first so the wireframe + dots +
        // gridlines all share the same per-paint scale & centre. Must
        // happen before any projectPoint/projectToScreen call inside this
        // pane.
        computeViewTransform(area);

        // Frame / unit-hypercube wireframe. Draws every corner of the
        // wavetable's actual N-dim cube (capped at kMaxRenderDims) and
        // every 1-bit-differ edge. Unrotated, a high-N cube projects
        // many edges on top of each other in 2D so it looks like a flat
        // square — rotate via the orbit drag or sidebar sliders to see
        // the extra dims pull apart.
        drawHypercube(g, area, eyeSign);

        // Axis labels (top of pane)
        g.setColour(juce::Colours::grey);
        g.setFont(11.0f);
        auto axName = [](int i) {
            const char* n[] = { "X", "Y", "Z", "W", "V", "U", "T", "S" };
            return juce::String((i >= 0 && i < 8) ? n[i] : "?");
        };
        if (is3D()) {
            g.drawText("axes: " + axName(axisX) + " x " + axName(axisY) + " x " + axName(axisZ),
                       area.reduced(6, 4).toNearestInt(), juce::Justification::topLeft);
            const char* modeTag = nullptr;
            switch (stereoMode) {
                case StereoMode::Anaglyph:  modeTag = "(red-cyan glasses)"; break;
                case StereoMode::CrossEyed: modeTag = (eyeSign > 0 ? "(left eye)" : "(right eye)"); break;
                case StereoMode::Parallel:  modeTag = (eyeSign < 0 ? "(left eye)" : "(right eye)"); break;
                default: break;
            }
            if (modeTag)
                g.drawText(modeTag, area.reduced(6, 4).toNearestInt(),
                           juce::Justification::topRight);
        } else {
            g.drawText("axes: " + axName(axisX) + " x " + axName(axisY),
                       area.reduced(6, 4).toNearestInt(), juce::Justification::topLeft);
        }

        bool useAnaglyph = (stereoMode == StereoMode::Anaglyph);

        // ---- Grid mode rendering -------------------------------------------------
        // Show one dot per non-null cell at the cell center. Cell centers come
        // from WavetableDoc::cellCenterPosition, padded to the rendering axis
        // count so projectPoint can read axisX/axisY/axisZ out of it.
        if (owner.wave.mode == WavetableMode::Grid) {
            const int viewDims = std::max(2, std::max((int)owner.wave.gridDims.size(),
                                                       owner.wave.scatterDims));

            // Faint cell-boundary grid: for each dim d with more than one
            // cell, draw every interior cell-boundary slab as the wireframe
            // of an (N-1)-cube fixed at coord = i/dims[d] along d.
            //
            // Why "(N-1)-cube wireframe" not "lines on the front+back face":
            //   The previous implementation drew gridlines parallel to axisY
            //   on the Z=0 / Z=1 / W=0 / W=1 faces. Those lines stay parallel
            //   to the projected Y axis no matter how the cube is rotated
            //   (because we never vary axisZ/axisW along them), so they look
            //   "stuck" - rotation around X-Z visibly tilts the cube but the
            //   gridlines remain vertical. They also bunch onto the two outer
            //   faces of the cube with empty space in between.
            //   Drawing the full perpendicular slab wireframe instead:
            //     - spreads gridlines across the interior of the projection
            //       (each slab cuts THROUGH the cube, not along its faces),
            //     - includes edges parallel to every non-d axis, so rotation
            //       around any plane visibly tilts at least one set of edges,
            //     - is geometrically what "cell boundary" actually means in
            //       N-D: a hyperplane perpendicular to d, drawn at the
            //       interior cell partitions.
            //
            // The `!is3D()` gate stays in place because in stereo modes the
            // per-slab wireframes on top of the cube wireframe get noisy;
            // mono users can still read cell structure via dots.
            if (!is3D() && (int)owner.wave.gridDims.size() >= 1) {
                g.setColour(juce::Colour(60, 60, 80).withAlpha(0.55f));

                for (int d = 0; d < viewDims; ++d) {
                    const int dd = (d < (int)owner.wave.gridDims.size())
                                   ? owner.wave.gridDims[d] : 1;
                    if (dd <= 1) continue;  // no interior boundaries

                    // The (N-1) dims perpendicular to d. The slab at d=t
                    // is the unit (N-1)-cube spanning these dims, with d
                    // fixed at t. Wireframe = corners 0..2^(N-1)-1, edges
                    // connecting corner-pairs that differ in exactly 1 bit.
                    std::vector<int> otherDims;
                    otherDims.reserve((size_t)viewDims - 1);
                    for (int o = 0; o < viewDims; ++o)
                        if (o != d) otherDims.push_back(o);
                    const int nOther = (int)otherDims.size();
                    const int nCorners = (nOther > 0) ? (1 << nOther) : 1;

                    auto cornerPos = [&](int cornerMask, float t) {
                        std::vector<float> p(viewDims, 0.0f);
                        p[(size_t)d] = t;
                        for (int k = 0; k < nOther; ++k)
                            p[(size_t)otherDims[(size_t)k]]
                                = ((cornerMask >> k) & 1) ? 1.0f : 0.0f;
                        return p;
                    };

                    for (int i = 1; i < dd; ++i) {
                        const float t = (float)i / (float)dd;
                        if (nOther == 0) {
                            // Degenerate (1-D grid): the "slab" is a point;
                            // nothing meaningful to draw. The outer wireframe
                            // already shows the [0..1] extent.
                            continue;
                        }
                        // Iterate unique edges of the (N-1)-cube: for each
                        // corner c0 and each bit, the edge goes to c0 ^ (1<<bit).
                        // Only emit when c0 < c1 to avoid drawing each edge twice.
                        for (int c0 = 0; c0 < nCorners; ++c0) {
                            for (int bit = 0; bit < nOther; ++bit) {
                                const int c1 = c0 ^ (1 << bit);
                                if (c1 <= c0) continue;
                                auto p0 = projectToScreen(cornerPos(c0, t), area);
                                auto p1 = projectToScreen(cornerPos(c1, t), area);
                                g.drawLine(p0.x, p0.y, p1.x, p1.y, 0.8f);
                            }
                        }
                    }
                }
            }

            // Cell dots
            for (int idx = 0; idx < (int)owner.wave.cellWaveformIds.size(); ++idx) {
                if (owner.wave.cellWaveformIds[idx] < 0) continue;
                auto cellPos = owner.wave.cellCenterPosition(idx);
                if (cellPos.empty()) continue;
                while ((int)cellPos.size() < viewDims) cellPos.push_back(0.5f);

                bool sel = (idx == owner.currentFrameIdx);
                // "Lib-target" = this cell references the library entry
                // currently in the right-pane editor. Drawing a warm outer
                // ring around every such cell makes the link from the
                // editor (data) to its placements (cells) visible at a
                // glance - especially important when one entry is placed
                // in multiple cells.
                const bool isLibTarget = (owner.currentLibraryId >= 0
                    && owner.wave.cellWaveformIds[idx] == owner.currentLibraryId);
                auto pp = projectPoint(cellPos, area);

                if (useAnaglyph) {
                    auto pL = applyParallax(pp, -1);
                    auto pR = applyParallax(pp, +1);
                    float rr = sel ? 7.0f : 5.0f;
                    g.setColour(juce::Colour::fromRGBA(255, 0, 0, sel ? 230 : 180));
                    g.fillEllipse(pL.x - rr, pL.y - rr, 2*rr, 2*rr);
                    g.setColour(juce::Colour::fromRGBA(0, 255, 255, sel ? 230 : 180));
                    g.fillEllipse(pR.x - rr, pR.y - rr, 2*rr, 2*rr);
                } else {
                    auto p = applyParallax(pp, eyeSign);
                    float rr = sel ? 7.0f : 5.0f;
                    juce::Colour base(0xff5fb3ff);
                    g.setColour(base.withAlpha(sel ? 1.0f : 0.85f));
                    g.fillEllipse(p.x - rr, p.y - rr, 2*rr, 2*rr);
                    g.setColour(juce::Colours::white.withAlpha(sel ? 1.0f : 0.5f));
                    g.drawEllipse(p.x - rr, p.y - rr, 2*rr, 2*rr, sel ? 1.6f : 1.0f);
                    if (isLibTarget) {
                        // Amber outer ring. Slightly thicker for the
                        // selected cell so the existing white ring is
                        // still visible inside it.
                        g.setColour(juce::Colour(0xffffc34a).withAlpha(0.9f));
                        const float pad = sel ? 4.0f : 3.0f;
                        g.drawEllipse(p.x - rr - pad, p.y - rr - pad,
                                      2 * (rr + pad), 2 * (rr + pad),
                                      sel ? 1.8f : 1.4f);
                    }

                    // Cell coord label e.g. "(0,1)"
                    auto coord = owner.wave.cellIdxToGridCoord(idx);
                    juce::String txt;
                    if (coord.size() == 1) {
                        txt = "[" + juce::String(coord[0]) + "]";
                    } else if (!coord.empty()) {
                        juce::String inner;
                        for (size_t d = 0; d < coord.size(); ++d) {
                            if (d) inner << ",";
                            inner << coord[d];
                        }
                        txt = "(" + inner + ")";
                    }
                    if (!txt.isEmpty()) {
                        g.setColour(juce::Colours::white.withAlpha(0.85f));
                        g.setFont(10.0f);
                        g.drawText(txt, (int)p.x - 50, (int)p.y + (int)rr + 1,
                                   100, 12, juce::Justification::centred);
                    }
                }
            }

            return;
        }

        // Draw frames
        auto& frames = owner.wave.scatterFrames;
        for (int i = 0; i < (int)frames.size(); ++i) {
            auto* resolved = owner.wave.libraryFrameById(frames[i].waveformId);
            auto base = autoColorForFrame(frames[i], resolved, i);
            bool sel = (i == owner.currentFrameIdx);
            // "Lib-target" = this scatter dot references the library entry
            // currently in the right-pane editor. Amber outer ring marks
            // every such dot so edits in the right pane are visibly
            // anchored to their placements.
            const bool isLibTarget = (owner.currentLibraryId >= 0
                && frames[i].waveformId == owner.currentLibraryId);
            auto pp = projectPoint(frames[i].position, area);

            if (useAnaglyph) {
                auto pL = applyParallax(pp, -1);
                auto pR = applyParallax(pp, +1);
                float r = sel ? 7.0f : 5.0f;
                // Red channel for left eye
                g.setColour(juce::Colour::fromRGBA(255, 0, 0, sel ? 230 : 180));
                g.fillEllipse(pL.x - r, pL.y - r, 2*r, 2*r);
                // Cyan channel for right eye
                g.setColour(juce::Colour::fromRGBA(0, 255, 255, sel ? 230 : 180));
                g.fillEllipse(pR.x - r, pR.y - r, 2*r, 2*r);

                if (sel) {
                    g.setColour(juce::Colours::white.withAlpha(0.7f));
                    g.drawEllipse(pL.x - r - 2, pL.y - r - 2, 2*r + 4, 2*r + 4, 1.2f);
                    g.drawEllipse(pR.x - r - 2, pR.y - r - 2, 2*r + 4, 2*r + 4, 1.2f);
                }
                if (!frames[i].label.empty() || sel) {
                    g.setColour(juce::Colours::white.withAlpha(0.85f));
                    g.setFont(10.0f);
                    juce::String txt = frames[i].label.empty()
                                       ? juce::String("Waveform " + juce::String(i + 1))
                                       : juce::String(frames[i].label);
                    auto mid = (pL + pR) * 0.5f;
                    g.drawText(txt, (int)mid.x - 40, (int)mid.y + (int)r + 2,
                               80, 12, juce::Justification::centred);
                }
            } else {
                // Off (eyeSign==0 -> applyParallax no-ops), CrossEyed or
                // Parallel (eyeSign==-1 or +1 applies horizontal disparity).
                auto p = applyParallax(pp, eyeSign);
                float r = sel ? 8.0f : 6.0f;
                g.setColour(base.withAlpha(sel ? 1.0f : 0.85f));
                g.fillEllipse(p.x - r, p.y - r, 2*r, 2*r);
                g.setColour(juce::Colours::white.withAlpha(sel ? 1.0f : 0.5f));
                g.drawEllipse(p.x - r, p.y - r, 2*r, 2*r, sel ? 1.6f : 1.0f);
                if (isLibTarget) {
                    g.setColour(juce::Colour(0xffffc34a).withAlpha(0.9f));
                    const float pad = sel ? 4.0f : 3.0f;
                    g.drawEllipse(p.x - r - pad, p.y - r - pad,
                                  2 * (r + pad), 2 * (r + pad),
                                  sel ? 1.8f : 1.4f);
                }

                g.setColour(juce::Colours::white.withAlpha(0.9f));
                g.setFont(10.5f);
                juce::String txt = frames[i].label.empty()
                                   ? juce::String("Waveform " + juce::String(i + 1))
                                   : juce::String(frames[i].label);
                g.drawText(txt, (int)p.x - 50, (int)p.y + (int)r + 1,
                           100, 12, juce::Justification::centred);
            }
        }

    }

    // Inline legend explaining what frame dots are. (The Position-cursor
    // crosshair entry was removed when the cursor concept went away - the
    // synth's playback point is driven entirely by the node's Position pins
    // / params at runtime, so there's no in-editor playhead to label.)
    void drawLegend(juce::Graphics& g, juce::Rectangle<float> area) {
        auto legend = area.reduced(8.0f);
        float lx = legend.getRight() - 200.0f;
        float ly = legend.getBottom() - 14.0f;
        // Row 1: blue dot = "waveform". (Traditional wavetable-synth jargon
        // calls each cycle a "frame", but SoundShop targets non-musicians,
        // so the plain term "waveform" reads better here.)
        g.setColour(juce::Colour(0xff5fb3ff).withAlpha(0.9f));
        g.fillEllipse(lx, ly, 8.0f, 8.0f);
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.drawEllipse(lx, ly, 8.0f, 8.0f, 1.0f);
        g.setColour(juce::Colours::white.withAlpha(0.75f));
        g.setFont(10.0f);
        g.drawText("= waveform",
                   (int)(lx + 12), (int)(ly - 2), 140, 12,
                   juce::Justification::left);

        // Row 2: amber ring = "shown in editor". Only meaningful when an
        // editor target is set (it always is once the library has at
        // least one entry).
        if (owner.currentLibraryId >= 0) {
            float ly2 = ly - 14.0f;
            g.setColour(juce::Colour(0xffffc34a).withAlpha(0.9f));
            g.drawEllipse(lx - 1, ly2 - 1, 10.0f, 10.0f, 1.4f);
            g.setColour(juce::Colours::white.withAlpha(0.75f));
            g.drawText("= shown in editor",
                       (int)(lx + 12), (int)(ly2 - 2), 140, 12,
                       juce::Justification::left);
        }
    }

    static void drawCrosshair(juce::Graphics& g, juce::Point<float> p, float r) {
        g.drawEllipse(p.x - r, p.y - r, 2*r, 2*r, 1.5f);
        g.drawLine(p.x - r - 3, p.y, p.x + r + 3, p.y, 1.0f);
        g.drawLine(p.x, p.y - r - 3, p.x, p.y + r + 3, 1.0f);
    }

    // Unified N-cube wireframe. Replaces the old drawSquare2D (only ever
    // a 2D face) and drawCube3D (only ever a 3D face) - both ignored
    // wavetable dims past their own depth, so a 4D wavetable's dots
    // could project to spots well outside the rendered outline once any
    // higher-dim rotation kicked in. Now we enumerate every corner of
    // the actual N-dim cube (capped at kMaxRenderDims) and connect
    // 1-bit-differ pairs, so:
    //   * dots are always inside the rendered hull (they live in
    //     [0,1]^N and the hull is the projection of [0,1]^N);
    //   * the hull always reaches the available view bounds because
    //     computeViewTransform() picks the scale from the same projected
    //     corner set;
    //   * the user sees the actual dim count once they rotate a higher
    //     plane (un-rotated, dims > 2 collapse on top of each other,
    //     which is mathematically correct).
    void drawHypercube(juce::Graphics& g, juce::Rectangle<float> area, int eyeSign) {
        int N = std::max(2, std::min(kMaxRenderDims, owner.wave.numDimensions()));
        int nVerts = 1 << N;
        std::vector<Projected> pp((size_t)nVerts);
        for (int c = 0; c < nVerts; ++c) {
            std::vector<float> v((size_t)N, 0.0f);
            for (int b = 0; b < N; ++b)
                v[(size_t)b] = ((c >> b) & 1) ? 1.0f : 0.0f;
            pp[(size_t)c] = projectPoint(v, area);
        }

        // For an N-cube, edges connect corners that differ in exactly
        // one bit. Powers-of-two are the bit-pattern test: diff != 0 and
        // diff & (diff - 1) == 0 means a single bit set.
        auto drawEdges = [&](int eye, juce::Colour col, float thickness) {
            g.setColour(col);
            for (int a = 0; a < nVerts; ++a) {
                for (int b = a + 1; b < nVerts; ++b) {
                    int diff = a ^ b;
                    if (diff != 0 && (diff & (diff - 1)) == 0) {
                        auto p0 = applyParallax(pp[(size_t)a], eye);
                        auto p1 = applyParallax(pp[(size_t)b], eye);
                        g.drawLine(p0.x, p0.y, p1.x, p1.y, thickness);
                    }
                }
            }
        };

        bool useAnaglyph = (stereoMode == StereoMode::Anaglyph);
        if (useAnaglyph) {
            drawEdges(-1, juce::Colour::fromRGBA(180,   0,   0, 110), 1.0f);
            drawEdges(+1, juce::Colour::fromRGBA(  0, 180, 180, 110), 1.0f);
        } else {
            // Slightly dim higher-dim wireframes so the edge density
            // doesn't fight the dots. 2D is brighter because there's
            // only 4 edges to see.
            juce::Colour col = (N <= 2) ? juce::Colour(80, 80, 100)
                                        : juce::Colour(70, 70, 90);
            drawEdges(eyeSign, col, 1.0f);
        }
    }

    // ---- Mouse interaction ----
    int dragFrameIdx = -1;
    bool dragCursor = false;
    bool dragOrbit = false;             // left- or right-button camera orbit
    juce::Point<float> orbitStart;       // mouse position when orbit drag began

    // Grid-mode "drag this cell to another cell" state. Set on mouseDown
    // when a populated cell is left-clicked AND the wavetable has 1 or 2
    // grid dims (higher dims have axes that the view doesn't show, so the
    // drag would be ambiguous). dragCellDstIdx is the closest cell under
    // the cursor right now; on mouseUp, if src != dst, the two cells'
    // library refs are swapped (matching the axis-stepper drag path used
    // by handleSelFrameGridChange). -1 means no drag active / no target.
    int dragCellSrcIdx = -1;
    int dragCellDstIdx = -1;

    // Library-drag-over state. While a Library row is being dragged over
    // the view, this is the index of the cell (Grid) or -2 (Scatter, drop
    // at cursor) that the drop will land on. Used to paint a hover ring
    // so the user sees where the drop will go. -1 = no drag active.
    int hoverDropCellIdx = -1;
    bool hoverDropActive = false;
    juce::Point<float> hoverDropScreenPt;
    // Snapshots of the (axisX,axisZ) and (axisY,axisZ) plane angles when
    // an orbit drag begins. Horizontal mouse delta adds onto the
    // (axisX, axisZ) plane angle (visually "yaw" - swings the scene left
    // and right around the vertical axis); vertical mouse delta adds onto
    // the (axisY, axisZ) plane angle ("pitch"). All other plane angles
    // are left untouched - they're only adjustable from the pop-out
    // wavetable view's slider sidebar.
    float orbitStartYaw = 0.0f, orbitStartPitch = 0.0f;

    int hitTestFrame(juce::Point<float> p, juce::Rectangle<float> area) const {
        if (owner.wave.mode != WavetableMode::Scatter) return -1;
        // Refresh the cached transform so projection in the hit-test matches
        // the latest paint (mouse events may arrive between paints or in a
        // different sub-pane than the last paint, e.g. cross-eyed view).
        computeViewTransform(area);
        const auto& frames = owner.wave.scatterFrames;
        int best = -1; float bestD2 = 14.0f * 14.0f;
        for (int i = 0; i < (int)frames.size(); ++i) {
            auto sp = projectToScreen(frames[i].position, area);
            float dx = sp.x - p.x, dy = sp.y - p.y;
            float d2 = dx*dx + dy*dy;
            if (d2 < bestD2) { bestD2 = d2; best = i; }
        }
        return best;
    }

    // Grid-mode hit test: walk every populated cell, project its centre
    // position into the view, return the cell index (flat row-major) of the
    // closest non-null cell within the click threshold, or -1.
    int hitTestGridCell(juce::Point<float> p, juce::Rectangle<float> area) const {
        if (owner.wave.mode != WavetableMode::Grid) return -1;
        // Same reason as hitTestFrame - keep the inverse projection in sync
        // with whatever was painted last.
        computeViewTransform(area);
        const int total = owner.wave.gridCellCount();
        if (total <= 0) return -1;
        int best = -1; float bestD2 = 14.0f * 14.0f;
        for (int i = 0; i < total; ++i) {
            if (i >= (int)owner.wave.cellWaveformIds.size()
                || owner.wave.cellWaveformIds[i] < 0)
                continue; // empty cell - nothing to click on
            auto cellPos = owner.wave.cellCenterPosition(i);
            int needN = std::max(2, owner.wave.numDimensions());
            while ((int)cellPos.size() < needN) cellPos.push_back(0.5f);
            auto sp = projectToScreen(cellPos, area);
            float dx = sp.x - p.x, dy = sp.y - p.y;
            float d2 = dx*dx + dy*dy;
            if (d2 < bestD2) { bestD2 = d2; best = i; }
        }
        return best;
    }

    // Like hitTestGridCell, but considers EMPTY cells too and has no
    // distance cap - returns the closest cell regardless of how far away
    // the cursor is. Used as a drop target locator (cell-drag swap, and
    // Library-row drop into an empty cell). Returns -1 only when there
    // are no cells at all.
    int nearestGridCellAny(juce::Point<float> p, juce::Rectangle<float> area) const {
        if (owner.wave.mode != WavetableMode::Grid) return -1;
        computeViewTransform(area);
        const int total = owner.wave.gridCellCount();
        if (total <= 0) return -1;
        int best = -1;
        float bestD2 = std::numeric_limits<float>::max();
        for (int i = 0; i < total; ++i) {
            auto cellPos = owner.wave.cellCenterPosition(i);
            int needN = std::max(2, owner.wave.numDimensions());
            while ((int)cellPos.size() < needN) cellPos.push_back(0.5f);
            auto sp = projectToScreen(cellPos, area);
            float dx = sp.x - p.x, dy = sp.y - p.y;
            float d2 = dx*dx + dy*dy;
            if (d2 < bestD2) { bestD2 = d2; best = i; }
        }
        return best;
    }

    // Inverse projection: convert a screen point back into the projected
    // axes (axisX, axisY) of the N-D position. Other axes are left at the
    // current value of `current`. Only meaningful in 2D mode (any 3D mode
    // can't be unprojected unambiguously, so dragging is locked to the 2D
    // projected axes there too - Z stays at the current cursor value).
    std::vector<float> screenToPosition(juce::Point<float> p,
                                        juce::Rectangle<float> area,
                                        const std::vector<float>& current) const
    {
        // Refresh the cached transform so the inverse map matches the
        // currently displayed projection. Without this, a mousedrag that
        // arrives between two paints (or in a different sub-pane than the
        // last paint) would use stale renderScale/centre and feel like
        // the dot snapped wrong.
        computeViewTransform(area);

        std::vector<float> out = current;
        // Use numDimensions() so the cursor position vector is sized for the
        // active mode's view dimensions (Grid: gridDims.size(); Scatter:
        // scatterDims) - otherwise dragging in Grid mode with more grid axes
        // than scatterDims would leave high axes truncated.
        while ((int)out.size() < owner.wave.numDimensions()) out.push_back(0.5f);
        if (renderScale < 1e-3f) return out;
        // Inverse of projectPoint's centre+scale (we ignore the perspective
        // term and the rotation when mapping back - dragging is locked to
        // the projected axisX/axisY only, which matches the old behaviour).
        float nx = (p.x - renderCx) / renderScale + 0.5f;
        float ny = (p.y - renderCy) / renderScale + 0.5f;
        nx = juce::jlimit(0.0f, 1.0f, nx);
        ny = juce::jlimit(0.0f, 1.0f, ny);
        if (axisX >= 0 && axisX < (int)out.size()) out[axisX] = nx;
        if (axisY >= 0 && axisY < (int)out.size()) out[axisY] = ny;
        return out;
    }

    // Begin an orbit drag from the given start point. Used by both the
    // right-button-on-empty path (legacy gesture) and the left-button-on-
    // empty path. Operates in both 2D and 3D views - in a flat 2D view
    // the rotation still tips the wavetable's hidden axes into the
    // projection, so the user sees dots move and can keep tipping until
    // axes that were edge-on come into view.
    void beginOrbit(juce::Point<float> startPos) {
        dragOrbit = true;
        orbitStart = startPos;
        orbitStartYaw   = owner.getScatterPlaneAngle(axisX, axisZ);
        orbitStartPitch = owner.getScatterPlaneAngle(axisY, axisZ);
    }

    void mouseDown(const juce::MouseEvent& e) override {
        // Right-button (or any mod-popup): if it lands on a dot, show that
        // frame's context menu (Remove from wavetable). On empty space it
        // starts a camera orbit. Used to be 3D-only, but the X-Z / Y-Z
        // rotation it drives is also meaningful in a "flat" 2D view (it
        // tips hidden dims into the projection), so 2D gets it too now.
        if (e.mods.isRightButtonDown() || e.mods.isPopupMenu()) {
            auto fullR = getLocalBounds().toFloat().reduced(2.0f);
            auto areaR = sceneRectForPoint(e.position, fullR);
            int hit = (owner.wave.mode == WavetableMode::Scatter)
                          ? hitTestFrame(e.position, areaR)
                          : hitTestGridCell(e.position, areaR);
            if (hit >= 0) {
                showFrameContextMenu(hit, e.getScreenPosition());
                return;
            }
            beginOrbit(e.position);
            return;
        }

        auto full = getLocalBounds().toFloat().reduced(2.0f);
        auto area = sceneRectForPoint(e.position, full);

        // Grid mode left-click:
        //  - On a populated cell: select it (switchToFrame); if the
        //    wavetable has 1 or 2 grid dims, ALSO arm a cell-drag so the
        //    user can drag the populated cell onto another cell and the
        //    two get swapped on release.
        //  - On empty cell / empty space: start an orbit drag instead of
        //    no-opping. This is what makes the view rotatable by dragging
        //    anywhere in 2D / 3D.
        if (owner.wave.mode != WavetableMode::Scatter) {
            int cell = hitTestGridCell(e.position, area);
            if (cell >= 0) {
                // Route through switchToFrame() so the editor target
                // (currentLibraryId) is synced when the clicked cell holds
                // a library entry, matching the Cells list behaviour.
                owner.switchToFrame(cell);
                const bool dimsOK = ((int)owner.wave.gridDims.size() <= 2);
                if (dimsOK
                    && owner.wave.libraryIdForCell(cell) >= 0) {
                    dragCellSrcIdx = cell;
                    dragCellDstIdx = cell;
                }
                repaint();
            } else {
                // Empty cell or background - start orbit.
                beginOrbit(e.position);
            }
            return;
        }

        int hit = hitTestFrame(e.position, area);

        if (e.mods.isShiftDown() && hit >= 0) {
            // Shift-click frame to delete (keep at least one). Routes through
            // the same removal helper as the right-click "Remove from
            // wavetable" menu so the pop-out's frames list / tabs refresh.
            removeFrameFromWavetable(hit);
            return;
        }

        if (hit >= 0) {
            // Select + (conditionally) start drag. Route through
            // switchToFrame() so the editor target (currentLibraryId)
            // follows the click to the scatter dot's library entry. Only
            // arm the drag-to-move path when the wavetable has 1 or 2
            // scatter dims - higher dim counts would only update axisX /
            // axisY anyway (the inverse projection is ambiguous), which
            // tends to feel like the dot snaps unexpectedly.
            owner.switchToFrame(hit);
            const bool dimsOK = (owner.wave.scatterDims <= 2);
            dragFrameIdx = dimsOK ? hit : -1;
            dragCursor = false;
            repaint();
            return;
        }

        // No frame was hit and we're in Scatter mode: start an orbit drag.
        // The old behaviour was to move a yellow Position cursor here, but
        // the cursor concept has been removed - playback position is driven
        // by the synth node's Position params (cables / sliders) instead.
        // Use + Waveform in the sidebar to add a new dot, or drag a Library
        // row onto the view to drop one at the cursor.
        dragCursor = false;
        dragFrameIdx = -1;
        beginOrbit(e.position);
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        // Helper: apply an orbit step from current mouse position. Shared
        // by Grid and Scatter so the rotation feel is identical.
        auto applyOrbit = [this, &e]() {
            float dx = e.position.x - orbitStart.x;
            float dy = e.position.y - orbitStart.y;
            float newYaw   = orbitStartYaw + dx * 0.5f;
            float newPitch = juce::jlimit(-85.0f, 85.0f,
                                           orbitStartPitch + dy * 0.5f);
            // Orbit speed: ~0.5 degree per pixel. The pitch axis is
            // clamped to +/-85 degrees to avoid the gimbal-flip jolt at
            // the poles - that clamp doesn't generalise to the full N-D
            // rotation set, so we only apply it to the (axisY, axisZ)
            // angle that the drag actually writes (other planes from
            // the slider sidebar are unrestricted).
            owner.setScatterPlaneAngle(axisX, axisZ, newYaw);
            owner.setScatterPlaneAngle(axisY, axisZ, newPitch);
            owner.notifyScatterViewRotated();
        };

        // Grid mode: orbit OR cell-drag. Both are mutually exclusive (set
        // up in mouseDown), so a single if/else chain is enough.
        if (owner.wave.mode != WavetableMode::Scatter) {
            if (dragOrbit) { applyOrbit(); return; }

            if (dragCellSrcIdx >= 0) {
                auto full = getLocalBounds().toFloat().reduced(2.0f);
                auto area = sceneRectForPoint(e.position, full);
                int dst = nearestGridCellAny(e.position, area);
                if (dst != dragCellDstIdx) {
                    dragCellDstIdx = dst;
                    repaint();
                }
            }
            return;
        }

        if (dragOrbit) { applyOrbit(); return; }

        auto full = getLocalBounds().toFloat().reduced(2.0f);
        auto area = sceneRectForPoint(e.position, full);
        if (dragFrameIdx >= 0 && dragFrameIdx < (int)owner.wave.scatterFrames.size()) {
            // mouseDown already gated this on scatterDims <= 2.
            auto& sf = owner.wave.scatterFrames[dragFrameIdx];
            sf.position = screenToPosition(e.position, area, sf.position);
            owner.notifyPopoutFrameOrPositionChanged();
            owner.onLayerChanged();
            repaint();
        }
    }

    void mouseUp(const juce::MouseEvent&) override {
        // Commit any pending Grid cell-swap drag. If the user dropped
        // back onto the source cell (or never moved off it), this is a
        // no-op - the click already selected the cell in mouseDown.
        if (dragCellSrcIdx >= 0 && dragCellDstIdx >= 0
            && dragCellSrcIdx != dragCellDstIdx) {
            commitGridCellDrag(dragCellSrcIdx, dragCellDstIdx);
        }
        dragFrameIdx = -1;
        dragCursor = false;
        dragOrbit = false;
        dragCellSrcIdx = -1;
        dragCellDstIdx = -1;
    }

    // Swap the library refs of two grid cells. Mirrors the swap done by
    // handleSelFrameGridChange (axis-stepper drag): src lib id moves to
    // dst, dst lib id moves to src (so a move-onto-empty leaves the
    // source empty, and a move-onto-populated displaces the existing dot
    // back to the source cell). Selection follows the user's intent and
    // stays with the moved waveform at dst.
    void commitGridCellDrag(int srcIdx, int dstIdx) {
        if (owner.wave.mode != WavetableMode::Grid) return;
        if (srcIdx < 0 || dstIdx < 0) return;
        if (srcIdx >= (int)owner.wave.cellWaveformIds.size()
            || dstIdx >= (int)owner.wave.cellWaveformIds.size()) return;
        std::swap(owner.wave.cellWaveformIds[(size_t)srcIdx],
                  owner.wave.cellWaveformIds[(size_t)dstIdx]);
        owner.currentFrameIdx = dstIdx;
        const int destLibId = owner.wave.libraryIdForCell(dstIdx);
        if (destLibId >= 0) owner.currentLibraryId = destLibId;
        owner.wave.scatterFromGridSnapshot.reset();
        owner.updateHintText();
        owner.rebuildRows();
        owner.onLayerChanged();
        owner.notifyPopoutDocMutated();
    }

    // ---- DragAndDropTarget: accept Library-row drops --------------------
    //
    // A Library row in the sidebar can be dragged onto the view and
    // released to place that library entry into the wavetable. In Grid
    // mode the drop assigns the entry to whichever cell is nearest the
    // cursor. In Scatter mode it creates a new ScatterFrame at the drop
    // position (using screenToPosition's inverse projection - axisX /
    // axisY get the cursor's coordinates, any extra dims default to 0.5).
    // Description format is "libdrag:<entryId>"; see LibraryDragButton
    // in WavetableViewWindowContent.
    static int parseLibraryDragId(const juce::var& description) {
        const juce::String s = description.toString();
        const juce::String prefix("libdrag:");
        if (!s.startsWith(prefix)) return -1;
        return s.substring(prefix.length()).getIntValue();
    }

    bool isInterestedInDragSource(const SourceDetails& d) override {
        return parseLibraryDragId(d.description) >= 0;
    }

    void itemDragEnter(const SourceDetails& d) override {
        if (parseLibraryDragId(d.description) < 0) return;
        hoverDropActive = true;
        itemDragMove(d);
    }

    void itemDragMove(const SourceDetails& d) override {
        if (parseLibraryDragId(d.description) < 0) return;
        hoverDropActive = true;
        hoverDropScreenPt = d.localPosition.toFloat();
        if (owner.wave.mode == WavetableMode::Grid) {
            auto full = getLocalBounds().toFloat().reduced(2.0f);
            auto area = sceneRectForPoint(hoverDropScreenPt, full);
            hoverDropCellIdx = nearestGridCellAny(hoverDropScreenPt, area);
        } else {
            hoverDropCellIdx = -1;
        }
        repaint();
    }

    void itemDragExit(const SourceDetails&) override {
        hoverDropActive = false;
        hoverDropCellIdx = -1;
        repaint();
    }

    void itemDropped(const SourceDetails& d) override {
        const int entryId = parseLibraryDragId(d.description);
        hoverDropActive = false;
        hoverDropCellIdx = -1;
        if (entryId < 0) { repaint(); return; }
        // Verify the entry still exists - drag-and-drop is async, so the
        // user could in principle have deleted the source row mid-drag.
        if (owner.wave.findLibraryIndexById(entryId) < 0) { repaint(); return; }

        auto full = getLocalBounds().toFloat().reduced(2.0f);
        const juce::Point<float> pos = d.localPosition.toFloat();
        auto area = sceneRectForPoint(pos, full);

        if (owner.wave.mode == WavetableMode::Grid) {
            const int cell = nearestGridCellAny(pos, area);
            if (cell < 0) { repaint(); return; }
            owner.wave.assignCellToLibrary(cell, entryId);
            owner.currentFrameIdx = cell;
            owner.currentLibraryId = entryId;
        } else {
            // Scatter: place a new dot at the drop position. screenToPosition
            // pads the position vector to wave.numDimensions() with 0.5 for
            // any dims the view doesn't show, which is what the user spec
            // calls "automatically set upon release" for >2D wavetables.
            std::vector<float> seedPos(owner.wave.numDimensions(), 0.5f);
            std::vector<float> newPos = screenToPosition(pos, area, seedPos);
            ScatterFrame sf;
            sf.waveformId = entryId;
            sf.position = std::move(newPos);
            owner.wave.scatterFrames.push_back(std::move(sf));
            owner.currentFrameIdx = (int)owner.wave.scatterFrames.size() - 1;
            owner.currentLibraryId = entryId;
        }
        owner.wave.scatterFromGridSnapshot.reset();
        owner.updateHintText();
        owner.rebuildRows();
        owner.onLayerChanged();
        owner.notifyPopoutDocMutated();
        repaint();
    }

    // Double-click a frame dot (Scatter) or grid cell (Grid) to jump
    // straight into the per-waveform editor for that frame. The wavetable
    // view is the default landing; double-click is the discoverable
    // shortcut for "edit this one" without going through the frame-tabs
    // row above. (Single click still selects + starts a drag in scatter
    // mode, so the existing arrange-by-dragging gesture isn't broken.)
    void mouseDoubleClick(const juce::MouseEvent& e) override {
        if (e.mods.isRightButtonDown() || e.mods.isPopupMenu()) return;
        if (e.mods.isShiftDown()) return; // shift-double-click is delete-ish; let single handle it
        auto full = getLocalBounds().toFloat().reduced(2.0f);
        auto area = sceneRectForPoint(e.position, full);
        int hit = (owner.wave.mode == WavetableMode::Scatter)
                      ? hitTestFrame(e.position, area)
                      : hitTestGridCell(e.position, area);
        if (hit < 0) return;
        // Side-by-side layout: the per-waveform editor is always visible
        // on the right. Double-click is just an alternative way to select
        // the cell (in case the single-click selection was less precise).
        owner.switchToFrame(hit);
    }

    // Right-click on a dot pops this up. Grid mode: "Remove from wavetable"
    // clears the cell (keeping at least one cell filled). Scatter mode: it
    // erases the scatter entry (keeping at least one frame). Either way the
    // pop-out window's frames list and frame tabs refresh through
    // notifyPopoutDocMutated.
    void showFrameContextMenu(int frameIdx, juce::Point<int> screenPos) {
        const bool isGrid = (owner.wave.mode == WavetableMode::Grid);
        const int filled = (int)(isGrid
            ? std::count_if(owner.wave.cellWaveformIds.begin(),
                            owner.wave.cellWaveformIds.end(),
                            [](int id) { return id >= 0; })
            : (int)owner.wave.scatterFrames.size());
        juce::PopupMenu m;
        m.addItem(1, "Remove from wavetable", filled > 1);
        // Capture the frame index by value so it survives the async menu.
        juce::Component::SafePointer<ScatterView> self(this);
        // withTargetComponent(this) anchored the menu to the entire
        // ScatterView, so it landed at the bottom of the whole pane
        // instead of next to the clicked dot. Use a 1x1 target screen
        // rect at the actual cursor position so the menu pops up where
        // the user clicked.
        const juce::Rectangle<int> targetArea(screenPos, screenPos + juce::Point<int>(1, 1));
        m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(targetArea),
            [self, frameIdx](int r) {
                if (!self || r != 1) return;
                self->removeFrameFromWavetable(frameIdx);
            });
    }

    void removeFrameFromWavetable(int frameIdx) {
        if (owner.wave.mode == WavetableMode::Scatter) {
            if ((int)owner.wave.scatterFrames.size() <= 1) return;
            if (frameIdx < 0 || frameIdx >= (int)owner.wave.scatterFrames.size()) return;
            owner.wave.scatterFrames.erase(owner.wave.scatterFrames.begin() + frameIdx);
            if (owner.currentFrameIdx >= (int)owner.wave.scatterFrames.size())
                owner.currentFrameIdx = (int)owner.wave.scatterFrames.size() - 1;
        } else {
            if (frameIdx < 0 || frameIdx >= (int)owner.wave.cellWaveformIds.size()) return;
            int filled = 0;
            for (int id : owner.wave.cellWaveformIds) if (id >= 0) ++filled;
            if (filled <= 1) return;
            // Clear the cell reference; the library entry itself stays so the
            // user can re-place the waveform later via the library sidebar.
            // The selection deliberately stays on this (now empty) cell so
            // the user can immediately Assign a different library entry to
            // the same slot - matches the X button in the Cells list.
            owner.wave.cellWaveformIds[frameIdx] = -1;
        }
        owner.wave.scatterFromGridSnapshot.reset();
        owner.updateHintText();
        owner.rebuildRows();
        owner.onLayerChanged();
        owner.notifyPopoutDocMutated();
        repaint();
    }

    // Show a "grab" hand cursor when hovering over a frame dot (a discrete
    // selectable / draggable target). Empty space and the Position crosshair
    // both behave the same on click (move Position) - no per-pixel cursor
    // hint is needed there since the click-anywhere semantic is consistent.
    juce::MouseCursor getMouseCursor() override {
        if (owner.wave.mode != WavetableMode::Scatter)
            return juce::MouseCursor::NormalCursor;
        auto p = getMouseXYRelative().toFloat();
        auto full = getLocalBounds().toFloat().reduced(2.0f);
        auto area = sceneRectForPoint(p, full);
        if (hitTestFrame(p, area) >= 0) return juce::MouseCursor::PointingHandCursor;
        return juce::MouseCursor::NormalCursor;
    }

private:
    LayeredWaveEditorComponent& owner;
};

// Apply the owner's N-D plane-rotation stack to a position vector. The
// rotations are composed in lexicographic plane order: (0,1), (0,2), ...,
// (0,N-1), (1,2), ... With every plane angle at zero this is the identity,
// so the projection collapses to the original axis-pick behaviour. The
// angle for plane (i, j) rotates the components p[i] and p[j] of the
// centred vector around the origin (centre = 0.5 because the unit cube
// lives in [0, 1]^N).
std::vector<float> LayeredWaveEditorComponent::ScatterView::rotateNd(std::vector<float> p) const {
    int N = (int)p.size();
    if (N < 2) return p;
    auto pairs = LayeredWaveEditorComponent::scatterPlanePairs(N);
    for (const auto& ij : pairs) {
        int i = ij.first, j = ij.second;
        if (i < 0 || j < 0 || i >= N || j >= N) continue;
        float deg = owner.getScatterPlaneAngle(i, j);
        if (deg == 0.0f) continue;
        float rad = deg * (float)M_PI / 180.0f;
        float c = std::cos(rad), s = std::sin(rad);
        float a = p[i] - 0.5f;
        float b = p[j] - 0.5f;
        p[i] = 0.5f + c * a - s * b;
        p[j] = 0.5f + s * a + c * b;
    }
    return p;
}

// =========================================================================
// Pop-out wavetable view window. Hosts a second ScatterView that shares the
// owner's rotation/position state, plus one slider per N-D plane (i, j) so
// the user can rotate the view in planes the mouse drag doesn't cover. The
// embedded ScatterView in the main editor and the pop-out view are kept in
// lock-step by routing all rotation writes through
// LayeredWaveEditorComponent::setScatterPlaneAngle, and by calling
// notifyScatterViewRotated() from the embedded view's drag handler so the
// sliders here track the live drag.
// =========================================================================

class LayeredWaveEditorComponent::WavetableViewWindowContent
    : public juce::Component
{
public:
    // A Library row that doubles as a drag source: the user can grab the
    // row and drop it onto the arrangement view to place that waveform
    // into a cell (Grid) or as a new dot (Scatter). The button's onClick
    // (set in rebuildLibraryList) keeps working for plain clicks because
    // JUCE only routes mouseUp to the button when no drag was started.
    class LibraryDragButton : public juce::TextButton {
    public:
        LibraryDragButton(int libId_) : libId(libId_) {}
        void mouseDrag(const juce::MouseEvent& e) override {
            // Only kick off a drag once the user has moved a few pixels;
            // a tiny jitter on click shouldn't suddenly become a drop.
            if (e.getDistanceFromDragStart() < 5) {
                juce::TextButton::mouseDrag(e);
                return;
            }
            if (auto* dnd = juce::DragAndDropContainer::findParentDragContainerFor(this)) {
                if (!dnd->isDragAndDropActive()) {
                    juce::var desc(juce::String("libdrag:") + juce::String(libId));
                    dnd->startDragging(desc, this);
                }
            }
        }
    private:
        int libId;
    };

    explicit WavetableViewWindowContent(LayeredWaveEditorComponent& o)
        : owner(o)
    {
        view = std::make_unique<ScatterView>(owner);
        addAndMakeVisible(view.get());
        view->setTooltip(
            "Wavetable arrangement view. Click empty space to move the Position cursor; "
            "click a waveform dot to select it; Shift+click a dot to delete it. "
            "In any 3D viewing mode, right-drag orbits the camera around the (X,Z) "
            "and (Y,Z) planes - other plane angles are adjustable from the rotation "
            "sliders on the right.");

        // ---- View / stereo mode (top of sidebar) ----
        addAndMakeVisible(stereoModeCombo);
        stereoModeCombo.addItem("View: Flat 2D",                          1);
        stereoModeCombo.addItem("View: 3D Anaglyph (red-cyan glasses)",   2);
        stereoModeCombo.addItem("View: 3D Cross-eyed stereoscope",        3);
        stereoModeCombo.addItem("View: 3D Parallel stereoscope",          4);
        stereoModeCombo.setSelectedId(1, juce::dontSendNotification);
        stereoModeCombo.setTooltip(
            "Switch between 2D and 3D viewing modes for this pop-out window.");
        stereoModeCombo.onChange = [this]() {
            int id = stereoModeCombo.getSelectedId();
            ScatterView::StereoMode m = ScatterView::StereoMode::Off;
            if (id == 2) m = ScatterView::StereoMode::Anaglyph;
            if (id == 3) m = ScatterView::StereoMode::CrossEyed;
            if (id == 4) m = ScatterView::StereoMode::Parallel;
            view->stereoMode = m;
            view->repaint();
        };

        // ---- Library list (scrolling) ----
        // Lists every waveform that exists in this wavetable's library,
        // whether or not it's currently placed in a cell. This is where
        // the user creates / renames / deletes the actual waveform data;
        // placement on the arrangement view is a separate concern (Cells
        // list below).
        librarySectionLabel.setText("Library", juce::dontSendNotification);
        librarySectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        librarySectionLabel.setJustificationType(juce::Justification::centredLeft);
        librarySectionLabel.setTooltip(
            "All waveforms in this wavetable's library. Cells in the arrangement view "
            "reference these by id. Deleting a cell does NOT delete its library entry - "
            "use the X button here to remove an entry from the library (which clears any "
            "cells that referenced it).");
        addAndMakeVisible(librarySectionLabel);

        addAndMakeVisible(libraryListViewport);
        libraryListViewport.setViewedComponent(&libraryListContainer, false);
        libraryListViewport.setScrollBarsShown(true, false);

        addAndMakeVisible(addLibraryEntryBtn);
        addLibraryEntryBtn.setTooltip(
            "Add a new waveform to the wavetable. Click to choose the waveform type: "
            "Layered (time-domain layers), Frequency Domain (FFT magnitude+phase), "
            "Wavelet Space (DWT coefficient grid), or capture from audio (project song, "
            "microphone, or audio file). If a waveform is currently selected you can "
            "also duplicate it. The synth crossfades between waveforms as you sweep the "
            "Position parameter on the synth node in the graph, letting you morph "
            "between different waveform shapes during playback.");
        addLibraryEntryBtn.onClick = [this]() {
            // Defer to the editor's central add-waveform flow so this sidebar
            // button offers exactly the same 6+1 choices (Layered / Spectral /
            // Wavelet / capture from project / capture from mic / capture from
            // file, plus Duplicate when something is selected) as the editor
            // used to expose via its top-bar "+ Waveform" button. That top-bar
            // button has been removed - this is the single entry point now.
            owner.showAddWaveformMenu(&addLibraryEntryBtn);
        };

        addAndMakeVisible(assignToCellBtn);
        assignToCellBtn.setTooltip(
            "Place the waveform currently being edited (the highlighted row in the "
            "Library list) into the cell currently selected in the arrangement view "
            "(replaces whatever was there). Greyed out when no waveform is being "
            "edited or no cell is selected.");
        assignToCellBtn.onClick = [this]() {
            const int libId = owner.currentLibraryId;
            if (libId < 0) return;
            owner.wave.assignCellToLibrary(owner.currentFrameIdx, libId);
            owner.wave.scatterFromGridSnapshot.reset();
            owner.updateHintText();
            owner.rebuildRows();
            owner.onLayerChanged();
            refreshAfterDocMutation();
        };

        // ---- Cells list (scrolling) ----
        // Shows occupied placements. Click selects the cell; X clears it.
        // Library entries survive cell deletion.
        framesListLabel.setText("Cells", juce::dontSendNotification);
        framesListLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        framesListLabel.setJustificationType(juce::Justification::centredLeft);
        framesListLabel.setTooltip(
            "Cells in the arrangement view that currently hold a waveform. Each row "
            "shows the cell's coordinate (Grid) or its frame number (Scatter). Click to "
            "select the cell; X clears the cell (library entry survives).");
        addAndMakeVisible(framesListLabel);

        addAndMakeVisible(framesListViewport);
        framesListViewport.setViewedComponent(&framesListContainer, false);
        framesListViewport.setScrollBarsShown(true, false);

        // ---- Convert button (mode-dependent) ----
        addAndMakeVisible(convertBtn);
        convertBtn.onClick = [this]() { onConvertClicked(); };

        // ---- Scrollable settings container ----
        // Everything below has to be parented to settingsContainer (not
        // `this`) so it scrolls with the viewport when content overflows.
        addAndMakeVisible(settingsViewport);
        settingsViewport.setViewedComponent(&settingsContainer, false);
        settingsViewport.setScrollBarsShown(true, false);

        // ---- Grid axes section (Grid mode only) ----
        gridAxesLabel.setText("Grid axes (cells per axis):", juce::dontSendNotification);
        gridAxesLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        gridAxesLabel.setJustificationType(juce::Justification::centredLeft);
        settingsContainer.addAndMakeVisible(gridAxesLabel);

        settingsContainer.addAndMakeVisible(addAxisBtn);
        addAxisBtn.setTooltip("Add a new grid axis (dimension). Each axis is exposed "
                              "to the synth as one Position knob. Max 8 axes.");
        addAxisBtn.onClick = [this]() {
            if ((int)owner.wave.gridDims.size() >= 8) return;
            owner.wave.gridDims.push_back(1);
            // Adding a 1-size axis keeps the frame count constant (every
            // existing cell stays at coord[newAxis]=0). Reset any stale
            // revert snapshot; the topology changed.
            owner.wave.scatterFromGridSnapshot.reset();
            rebuildAxisSteppers();
            owner.syncPositionParams();
            owner.updateHintText();
            // The view N now grew by one - regrow the rotation-angle vector,
            // rebuild this window's rotation slider strip + the selected-
            // frame position strip, and refresh the main editor's
            // projection combo.
            owner.rebuildScatterUI();
            rebuildSliders();
            rebuildSelFrameControls();
            owner.onLayerChanged();
            resized();
        };
        settingsContainer.addAndMakeVisible(removeAxisBtn);
        removeAxisBtn.setTooltip("Remove the last grid axis. Waveforms whose coord on "
                                 "the dropped axis is non-zero are deleted.");
        removeAxisBtn.onClick = [this]() {
            if (owner.wave.gridDims.size() <= 1) return;
            // Drop frames whose coord on the last axis is > 0, then shrink.
            const int lastAxis = (int)owner.wave.gridDims.size() - 1;
            owner.wave.resizeGridAxis(lastAxis, 1);  // collapse last axis to 1
            owner.wave.gridDims.pop_back();
            // After popping, the surviving frames are still in the right
            // slots (their coord on the dropped axis was 0).
            owner.wave.scatterFromGridSnapshot.reset();
            if (owner.currentFrameIdx >= (int)owner.wave.cellWaveformIds.size())
                owner.currentFrameIdx = 0;
            rebuildAxisSteppers();
            owner.syncPositionParams();
            owner.updateHintText();
            // View N shrank - shrink the rotation-angle vector and rebuild
            // the slider strip + selected-frame strip / projection combo
            // accordingly.
            owner.rebuildScatterUI();
            rebuildSliders();
            rebuildSelFrameControls();
            owner.onLayerChanged();
            resized();
        };

        // ---- RBF radius (Scatter mode only) ----
        radiusLabel.setText("RBF radius:", juce::dontSendNotification);
        radiusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        radiusLabel.setJustificationType(juce::Justification::centredLeft);
        settingsContainer.addAndMakeVisible(radiusLabel);

        settingsContainer.addAndMakeVisible(radiusSlider);
        radiusSlider.setRange(0.05, 1.5, 0.0);
        radiusSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        radiusSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 18);
        radiusSlider.setValue(owner.wave.scatterRadius, juce::dontSendNotification);
        radiusSlider.setTooltip("How far each scatter waveform's influence reaches into "
                                "N-D space (Wendland radial basis function cutoff, in "
                                "normalized [0,1] coordinates). Smaller = sharper "
                                "transitions between waveforms; larger = smoother blends.");
        radiusSlider.onValueChange = [this]() {
            owner.wave.scatterRadius = (float)radiusSlider.getValue();
            owner.onLayerChanged();
            view->repaint();
        };

        // ---- Selected-frame position section header ----
        selFrameSectionLabel.setText("Selected waveform position:", juce::dontSendNotification);
        selFrameSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        selFrameSectionLabel.setJustificationType(juce::Justification::centredLeft);
        selFrameSectionLabel.setTooltip("Per-axis position of the currently selected waveform. "
                                        "In Scatter mode each slider sets that waveform's "
                                        "coordinate on the axis (0..1). In Grid mode each "
                                        "stepper picks which cell along the axis - moving "
                                        "onto an occupied cell swaps the two frames.");
        settingsContainer.addAndMakeVisible(selFrameSectionLabel);

        // ---- Rotation section header ----
        rotationSectionLabel.setText("Rotation (per N-D plane):", juce::dontSendNotification);
        rotationSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        rotationSectionLabel.setJustificationType(juce::Justification::centredLeft);
        settingsContainer.addAndMakeVisible(rotationSectionLabel);

        // ---- Reset rotation (pinned bottom, NOT in the scroll area) ----
        addAndMakeVisible(resetRotBtn);
        resetRotBtn.setTooltip("Reset all rotation sliders to 0 (identity rotation). "
                               "The view returns to a pure axis-aligned projection.");
        resetRotBtn.onClick = [this]() {
            int N = std::max(2, owner.wave.numDimensions());
            auto pairs = LayeredWaveEditorComponent::scatterPlanePairs(N);
            for (const auto& ij : pairs)
                owner.setScatterPlaneAngle(ij.first, ij.second, 0.0f);
            refreshSliderValues();
        };

        // Top-bar buttons (Apply / Close / ?) and the discoverability
        // hint label live on LayeredWaveEditorComponent itself; this
        // panel is an embedded child of that editor, so duplicating them
        // here would just give the user two sets of controls that do the
        // same thing. The exception is "+ Waveform" - it used to be a
        // top-bar button on the owner but now lives here in the sidebar
        // (next to the library list it adds to), and delegates to the
        // owner's showAddWaveformMenu via the friend relationship.
        // updateHintText() is still called from refreshAfterDocMutation()
        // so the OWNER's hint stays accurate as the doc changes.

        rebuildSliders();
        rebuildAxisSteppers();
        rebuildSelFrameControls();
        rebuildLibraryList();
        rebuildFramesList();
        updateConvertButton();
        updateAssignButtonEnabled();

        // Default reasonable size; the owner overrides this on resize.
        setSize(940, 620);
    }

    void resized() override {
        auto r = getLocalBounds().reduced(0);

        // No top-bar or hint here - the owning LayeredWaveEditorComponent
        // owns those, and we're embedded into the area below them. We
        // fill our bounds with the sidebar (right) + scatter view (rest).

        const int sidebarW = 280;
        auto sidebar = r.removeFromRight(sidebarW);
        r.removeFromRight(8);

        // Sidebar layout, top to bottom:
        //   View combo (stereo / 3D mode)
        //   Library label + list (~120h) + "+ Waveform" button
        //   "Assign to selected cell" button
        //   Cells label + list (~110h)
        //   Convert button (mode-dependent)
        //   <scrollable settings viewport> (grid/radius + cursor pos +
        //                                   selected-frame pos + rotation)
        //   Reset rotation button (pinned bottom)
        stereoModeCombo.setBounds(sidebar.removeFromTop(28));
        sidebar.removeFromTop(8);

        // ----- Library section -----
        librarySectionLabel.setBounds(sidebar.removeFromTop(18));
        sidebar.removeFromTop(2);
        const int libListH = 120;
        libraryListViewport.setBounds(sidebar.removeFromTop(libListH));
        layoutLibraryListEntries();
        sidebar.removeFromTop(4);
        addLibraryEntryBtn.setBounds(sidebar.removeFromTop(24));
        sidebar.removeFromTop(4);
        assignToCellBtn.setBounds(sidebar.removeFromTop(24));
        sidebar.removeFromTop(10);

        // ----- Cells section -----
        framesListLabel.setBounds(sidebar.removeFromTop(18));
        sidebar.removeFromTop(2);
        const int listH = 110;
        framesListViewport.setBounds(sidebar.removeFromTop(listH));
        // Re-bound the rows to the new viewport width. At construction time
        // rebuildFramesList runs with viewport width 0, so the rows would
        // otherwise be invisibly narrow until something rebuilt them.
        layoutFramesListEntries();
        sidebar.removeFromTop(6);

        // Convert button row (Grid <-> Scatter mode toggle).
        convertBtn.setBounds(sidebar.removeFromTop(26));
        sidebar.removeFromTop(8);

        // Reset rotation pinned to the bottom of the sidebar (outside the
        // scroll area so it's always reachable).
        resetRotBtn.setBounds(sidebar.removeFromBottom(28));
        sidebar.removeFromBottom(6);

        // Everything else is in the scrollable viewport.
        settingsViewport.setBounds(sidebar);
        layoutSettingsContainer();

        view->setBounds(r);
    }

    // Discoverability hint is owned by LayeredWaveEditorComponent and
    // refreshed by its updateHintText(). Nothing to do here - this used
    // to update an embedded hint label that no longer exists.

    ~WavetableViewWindowContent() override = default;

    // (placeLibraryEntryIntoCell was used by the old pencil "focus on
    // orphan entry" flow, which forced auto-placement as a side-effect of
    // focusing the editor. With the library-id-addressed editor the
    // focus step doesn't need a cell anymore, so the helper is gone.
    // The +Waveform flow inlines its own placement logic.)

    // Lay out the per-axis / per-plane controls inside settingsContainer and
    // size the container so it scrolls when content overflows. Mode-dependent
    // sections (grid axes vs RBF radius) and Scatter-only sections (selected
    // frame editable sliders) are shown/hidden here.
    void layoutSettingsContainer() {
        const int innerW = std::max(0, settingsViewport.getWidth()
                                         - settingsViewport.getScrollBarThickness() - 2);
        const int contentX = 0;
        int contentY = 0;
        const int sectionGap = 10;
        const int rowH = 22;
        const int rowGap = 2;

        const bool isGrid = (owner.wave.mode == WavetableMode::Grid);

        // -- Grid axes / RBF radius (mode dependent) --
        if (isGrid) {
            gridAxesLabel.setVisible(true);
            radiusLabel.setVisible(false);
            radiusSlider.setVisible(false);
            addAxisBtn.setVisible(true);
            removeAxisBtn.setVisible(true);

            gridAxesLabel.setBounds(contentX, contentY, innerW, 18);
            contentY += 18 + 2;
            for (size_t k = 0; k < axisSizeSliders.size(); ++k) {
                if (k < axisSizeLabels.size())
                    axisSizeLabels[k]->setBounds(contentX, contentY, 48, rowH);
                axisSizeSliders[k]->setBounds(contentX + 48, contentY, innerW - 48, rowH);
                contentY += rowH + rowGap;
            }
            addAxisBtn.setBounds(contentX, contentY, 70, 24);
            removeAxisBtn.setBounds(contentX + 76, contentY, 70, 24);
            contentY += 24;
        } else {
            gridAxesLabel.setVisible(false);
            addAxisBtn.setVisible(false);
            removeAxisBtn.setVisible(false);
            radiusLabel.setVisible(true);
            radiusSlider.setVisible(true);

            radiusLabel.setBounds(contentX, contentY, 80, rowH);
            radiusSlider.setBounds(contentX + 80, contentY, innerW - 80, rowH);
            contentY += rowH;
        }
        contentY += sectionGap;

        // -- Selected-frame position controls (sliders or steppers) --
        selFrameSectionLabel.setBounds(contentX, contentY, innerW, 18);
        contentY += 18 + 2;
        for (size_t k = 0; k < selFrameSliders.size(); ++k) {
            if (k < selFrameLabels.size())
                selFrameLabels[k]->setBounds(contentX, contentY, 32, rowH);
            selFrameSliders[k]->setBounds(contentX + 32, contentY, innerW - 32, rowH);
            contentY += rowH + rowGap;
        }
        contentY += sectionGap;

        // -- Rotation sliders (one per N-D plane) --
        rotationSectionLabel.setBounds(contentX, contentY, innerW, 18);
        contentY += 18 + 2;
        const int rotRowH = 24;
        for (size_t k = 0; k < planeSliders.size(); ++k) {
            if (k < planeLabels.size())
                planeLabels[k]->setBounds(contentX, contentY, 72, rotRowH);
            planeSliders[k]->setBounds(contentX + 72, contentY, innerW - 72, rotRowH);
            contentY += rotRowH + rowGap;
        }

        settingsContainer.setSize(innerW, std::max(contentY, 10));
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(20, 20, 26));
    }

    // Called by owner when something changed the angle outside this window
    // (mouse drag in the embedded ScatterView, dim count change, etc.).
    // Pushes the current owner angles into the slider widgets WITHOUT
    // firing onValueChange (would otherwise re-write the owner and could
    // cascade).
    void refreshSliderValues() {
        int N = std::max(2, owner.wave.numDimensions());
        if ((int)planeSliders.size() != LayeredWaveEditorComponent::scatterPlaneCount(N)) {
            rebuildSliders();
            resized();
            return;
        }
        auto pairs = LayeredWaveEditorComponent::scatterPlanePairs(N);
        for (size_t k = 0; k < pairs.size() && k < planeSliders.size(); ++k) {
            float deg = owner.getScatterPlaneAngle(pairs[k].first, pairs[k].second);
            planeSliders[k]->setValue(deg, juce::dontSendNotification);
        }
        view->repaint();
    }

    // Called by owner whenever the frame set / mode / grid topology
    // changes (add/remove frame, convert mode, cell placement). Rebuilds
    // every list/stepper/button that depends on the doc's structure.
    void refreshAfterDocMutation() {
        rebuildLibraryList();
        rebuildFramesList();
        rebuildAxisSteppers();
        // Mode may have flipped (Grid<->Scatter) - rebuild the per-axis
        // selected-frame controls so they show the right widget kind.
        rebuildSelFrameControls();
        updateConvertButton();
        updateAssignButtonEnabled();
        // Mode may have flipped; the radius/axes sections swap visibility
        // inside layoutSettingsContainer().
        resized();
        view->repaint();
    }

    // Called by owner whenever currentFrameIdx changes OR a scatter
    // position changes (frame click, drag, switchToFrame, +Frame, etc.).
    // Pushes the new selection's position into the per-axis sliders /
    // steppers, re-evaluates "Back to Grid" eligibility (any drag off a
    // snapshotted cell center should disable it), and repaints the view.
    void refreshFrameAndPositionValues() {
        refreshSelFrameValues();
        updateConvertButton();
        // Cell selection moved - the Assign button's "cell selected" half
        // may have flipped.
        updateAssignButtonEnabled();
        // The Library row highlight tracks currentLibraryId. switchToFrame
        // syncs currentLibraryId to whichever entry the clicked cell holds,
        // so we need to refresh the row toggle states for the highlight to
        // follow dot clicks / drags (not just direct Library row clicks).
        refreshLibraryHighlight();
        view->repaint();
    }

    // Refresh the on/off state of each Library row button so the amber
    // highlight tracks owner.currentLibraryId. Lightweight: no rebuild,
    // just toggleState updates. Also scrolls the selected row into view
    // in the library viewport so the user can see which entry is now
    // the editor target after a click on a dot or cell.
    //
    // Falls back to a full rebuildLibraryList if the row vector got out
    // of sync with wave.library (only happens if something mutated the
    // library without going through refreshAfterDocMutation - shouldn't
    // happen, but the fallback keeps the UI from going stale silently).
    void refreshLibraryHighlight() {
        if (libraryRowButtons.size() != owner.wave.library.size()) {
            rebuildLibraryList();
            return;
        }
        juce::Component* selRow = nullptr;
        for (size_t i = 0; i < libraryRowButtons.size(); ++i) {
            const int entryId = owner.wave.library[i].id;
            const bool selected = (entryId == owner.currentLibraryId);
            libraryRowButtons[i]->setToggleState(selected,
                                                 juce::dontSendNotification);
            if (selected) selRow = libraryRowButtons[i].get();
        }
        if (selRow) {
            // Scroll the selected row into view if it isn't already. Use
            // getBoundsInParent because the row lives inside
            // libraryListContainer, which is what the viewport scrolls.
            auto rowR = selRow->getBoundsInParent();
            auto vis  = libraryListViewport.getViewArea();
            if (!vis.contains(rowR.getCentre())) {
                int targetY = std::max(0, rowR.getY() - 4);
                libraryListViewport.setViewPosition(0, targetY);
            }
        }
    }

    // The embedded ScatterView is the editor's one source of truth for
    // arrangement-view rendering. Public so the owning
    // LayeredWaveEditorComponent can wire repaints / projection / stereo
    // settings through it. The owner does NOT own a separate ScatterView
    // - this one is the one.
    std::unique_ptr<ScatterView> view;

private:
    void onConvertClicked() {
        if (owner.wave.mode == WavetableMode::Grid) {
            owner.wave.convertGridToScatter();
            owner.currentPosition.assign(owner.wave.numDimensions(), 0.5f);
            owner.currentFrameIdx = 0;
        } else {
            // Reverse path: only succeeds if every dot is still at its
            // snapshotted cell center. Button is disabled otherwise.
            if (!owner.wave.canRevertScatterToGrid()) return;
            owner.wave.revertScatterToGrid();
            owner.currentFrameIdx = 0;
        }
        owner.ensureScatterPlaneAngles();
        owner.syncPositionParams();
        owner.updateHintText();
        owner.rebuildRows();
        owner.onLayerChanged();
        refreshAfterDocMutation();
        rebuildSliders();
        resized();
    }

    void updateConvertButton() {
        if (owner.wave.mode == WavetableMode::Grid) {
            convertBtn.setButtonText(juce::String::fromUTF8("Convert \xE2\x86\x92 Scatter"));
            convertBtn.setEnabled(true);
            convertBtn.setTooltip("Convert this Grid wavetable to Scatter mode. Each "
                                  "non-empty cell becomes a scatter dot at the cell center. "
                                  "You can revert back to Grid as long as you don't drag "
                                  "any dot off its original cell center.");
        } else {
            const bool canRevert = owner.wave.canRevertScatterToGrid();
            convertBtn.setButtonText(juce::String::fromUTF8("\xE2\x86\xA9 Back to Grid"));
            convertBtn.setEnabled(canRevert);
            convertBtn.setTooltip(canRevert
                ? juce::String("Revert to Grid mode. Every dot is still at its original "
                               "cell center, so the conversion is lossless.")
                : juce::String("Greyed out: one or more dots has been moved off its "
                               "original cell center (or the wavetable was authored as "
                               "Scatter from the start). To get back to Grid, drag every "
                               "dot exactly onto a cell center - or accept Scatter mode "
                               "as final."));
        }
    }

    // Walk the existing frames-list buttons / delete-X widgets and re-bound
    // them based on the current viewport width. This is split out from
    // rebuildFramesList so resized() can apply correct widths after the
    // viewport finally has a non-zero size (at construction time the
    // viewport width is 0, so positions calculated there would clip).
    void layoutFramesListEntries() {
        const int rowH = 24;
        const int delW = 18;
        int contentW = std::max(0, framesListViewport.getWidth() - 14);
        int y = 0;
        for (size_t i = 0; i < frameListButtons.size(); ++i) {
            if (frameListButtons[i])
                frameListButtons[i]->setBounds(0, y, std::max(0, contentW - delW - 4), rowH - 2);
            if (i < frameListDeletes.size() && frameListDeletes[i])
                frameListDeletes[i]->setBounds(contentW - delW, y, delW, rowH - 2);
            y += rowH;
        }
        framesListContainer.setSize(contentW, std::max(y, 10));
    }

    // Re-bound the rows of the Library list to the current viewport width.
    // Same pattern as layoutFramesListEntries: at construction the viewport
    // width is 0, so rebuildLibraryList's positions are clipped until a
    // resized() pass re-runs this with the real width.
    //
    // Two-column layout: [label button stretches to fill] [red X delete].
    // The old three-column layout had a pencil column for "focus the editor
    // on this entry" - that's now done by clicking the row itself
    // (selection IS the editor target now), so the pencil is gone.
    void layoutLibraryListEntries() {
        const int rowH = 24;
        const int delW = 18;
        int contentW = std::max(0, libraryListViewport.getWidth() - 14);
        int y = 0;
        for (size_t i = 0; i < libraryRowButtons.size(); ++i) {
            int x = contentW;
            if (i < libraryRowDeletes.size() && libraryRowDeletes[i]) {
                x -= delW;
                libraryRowDeletes[i]->setBounds(x, y, delW, rowH - 2);
                x -= 4;
            }
            if (libraryRowButtons[i])
                libraryRowButtons[i]->setBounds(0, y, std::max(0, x), rowH - 2);
            y += rowH;
        }
        libraryListContainer.setSize(contentW, std::max(y, 10));
    }

    // Rebuild the Library list from owner.wave.library. One row per entry,
    // even if the entry is currently orphaned (no cell references it).
    // Selection IS the editor target now: clicking a row focuses the
    // right-pane editor on that library entry. The selection highlight
    // tracks owner.currentLibraryId (the single source of truth - no
    // separate sidebar-selected state).
    void rebuildLibraryList() {
        libraryListContainer.removeAllChildren();
        libraryRowButtons.clear();
        libraryRowDeletes.clear();

        for (size_t k = 0; k < owner.wave.library.size(); ++k) {
            const auto& entry = owner.wave.library[k];
            const int entryId = entry.id;
            int useCount = owner.wave.countCellsUsingLibrary(entryId);

            juce::String label = entry.name.empty()
                ? juce::String("Waveform ") + juce::String((int)k + 1)
                : juce::String(entry.name);
            if (useCount > 0)
                label += juce::String(" (used ") + juce::String(useCount)
                       + juce::String::fromUTF8("\xC3\x97)");

            auto btn = std::make_unique<LibraryDragButton>(entryId);
            btn->setButtonText(label);
            btn->setClickingTogglesState(true);
            btn->setToggleState(entryId == owner.currentLibraryId,
                                juce::dontSendNotification);
            // Explicit toggle-on color. JUCE's default buttonOnColourId is
            // visually almost identical to buttonColourId, so on/off
            // toggling reads as no change at all - the user clicks a row,
            // it never looks selected, and they assume the click did
            // nothing. We use the same amber as the arrangement-view
            // "shown in editor" ring so both views agree visually on
            // which library entry the right pane is editing.
            btn->setColour(juce::TextButton::buttonOnColourId,
                           juce::Colour(0xffffc34a).withAlpha(0.55f));
            btn->setColour(juce::TextButton::textColourOnId,
                           juce::Colours::black);
            btn->setTooltip(
                "Click to edit this waveform in the right pane. Edits flow into "
                "every cell that references it. Use 'Assign to selected cell' "
                "below to place this waveform in the cell currently selected "
                "in the arrangement view. Drag this row onto the wavetable view "
                "to drop it as a new waveform at the cursor (or into a cell in "
                "Grid mode).");
            btn->onClick = [this, entryId]() {
                owner.setEditingLibraryEntry(entryId);
                rebuildLibraryList();
                updateAssignButtonEnabled();
            };
            libraryListContainer.addAndMakeVisible(btn.get());
            libraryRowButtons.push_back(std::move(btn));

            auto del = std::make_unique<FrameDeleteX>();
            del->setTooltip(
                useCount > 0
                    ? juce::String("Remove this entry from the library. The ")
                          + juce::String(useCount)
                          + juce::String(useCount == 1 ? " cell" : " cells")
                          + juce::String(" referencing it will become empty.")
                    : juce::String("Remove this unplaced library entry."));
            del->onClick = [this, entryId]() {
                // Removing a library entry also clears every cell that
                // referenced it (handled inside removeLibraryEntry).
                owner.wave.removeLibraryEntry(entryId);
                // If the editor was focused on this entry, fall back to the
                // first surviving entry (or -1 if the library is empty).
                if (owner.currentLibraryId == entryId) {
                    owner.currentLibraryId = owner.wave.library.empty()
                        ? -1
                        : owner.wave.library.front().id;
                }
                // currentFrameIdx (cell selection) may now point at a
                // cleared cell - leave it; the user can pick a new one.
                owner.wave.scatterFromGridSnapshot.reset();
                owner.updateHintText();
                owner.rebuildRows();
                owner.onLayerChanged();
                refreshAfterDocMutation();
            };
            libraryListContainer.addAndMakeVisible(del.get());
            libraryRowDeletes.push_back(std::move(del));
        }
        layoutLibraryListEntries();
    }

    // Assign button is enabled only when both ends of the operation are
    // present: an editor target (currentLibraryId, which IS the library
    // selection) AND a cell in the arrangement view (currentFrameIdx
    // valid and in range for the active mode). In Grid mode the cell can
    // be empty - that's the whole point of assign-to-empty-cell.
    void updateAssignButtonEnabled() {
        bool haveLib = (owner.currentLibraryId >= 0);
        bool haveCell = false;
        const int idx = owner.currentFrameIdx;
        if (owner.wave.mode == WavetableMode::Grid) {
            haveCell = idx >= 0 && idx < (int)owner.wave.cellWaveformIds.size();
        } else {
            haveCell = idx >= 0 && idx < (int)owner.wave.scatterFrames.size();
        }
        assignToCellBtn.setEnabled(haveLib && haveCell);
    }

    void rebuildFramesList() {
        framesListContainer.removeAllChildren();
        frameListButtons.clear();
        frameListDeletes.clear();

        // Grid mode lists EVERY cell (including empties) so a freshly-laid-out
        // grid is fully visible and "Assign to selected cell" can target an
        // empty slot. Scatter mode lists only its existing dots - scatter
        // points don't exist until placed, so there's no "empty scatter slot".
        auto addEntry = [&](const juce::String& label, int frameIdx,
                            bool isGridSparse, bool isEmpty) {
            auto btn = std::make_unique<juce::TextButton>();
            btn->setButtonText(label);
            btn->setClickingTogglesState(true);
            btn->setToggleState(frameIdx == owner.currentFrameIdx,
                                juce::dontSendNotification);
            // Explicit toggle-on background. Same reason as the Library
            // list: JUCE's default buttonOnColourId reads as no change.
            // We use the arrangement-view dot blue so the Cells-list
            // highlight and the (selected) blue dot ring agree on which
            // cell is selected. Distinct from the Library list's amber
            // (= "shown in editor") on purpose - selection and editor
            // target are now two separate states.
            btn->setColour(juce::TextButton::buttonOnColourId,
                           juce::Colour(0xff5fb3ff).withAlpha(0.55f));
            if (isEmpty) {
                // Dim the empty-cell label so the list reads as a sparse
                // sequence rather than a wall of indistinguishable rows.
                btn->setColour(juce::TextButton::textColourOffId,
                               juce::Colours::grey);
                btn->setColour(juce::TextButton::textColourOnId,
                               juce::Colours::black);
                btn->setTooltip("Empty cell. Click to select it, then pick a "
                                "Library entry and press \"Assign to selected "
                                "cell\" - or use +Waveform to create a new "
                                "entry placed here.");
            } else {
                btn->setColour(juce::TextButton::textColourOnId,
                               juce::Colours::black);
            }
            btn->onClick = [this, frameIdx]() {
                owner.switchToFrame(frameIdx);
                rebuildFramesList();
            };
            framesListContainer.addAndMakeVisible(btn.get());
            frameListButtons.push_back(std::move(btn));

            auto del = std::make_unique<FrameDeleteX>();
            if (isEmpty) {
                // Nothing to clear on an already-empty cell. Keep the slot in
                // the layout so columns line up, but make the X inert + faded.
                del->setEnabled(false);
                del->setAlpha(0.0f);
            } else {
                del->setTooltip(isGridSparse ? juce::String("Clear this cell")
                                              : juce::String("Delete this waveform"));
                del->onClick = [this, frameIdx, isGridSparse]() {
                    if (isGridSparse) {
                        if (frameIdx < 0 || frameIdx >= (int)owner.wave.cellWaveformIds.size()) return;
                        int filled = 0;
                        for (int id : owner.wave.cellWaveformIds) if (id >= 0) ++filled;
                        if (filled <= 1) return;
                        // Library entry survives - this only clears the cell ref.
                        owner.wave.cellWaveformIds[frameIdx] = -1;
                        // Don't auto-jump selection: the user just clicked
                        // X on this specific cell, so leaving the cell
                        // selected (now empty) lets them immediately Assign
                        // a different library entry into the same slot.
                    } else {
                        if ((int)owner.wave.scatterFrames.size() <= 1) return;
                        if (frameIdx < 0 || frameIdx >= (int)owner.wave.scatterFrames.size()) return;
                        owner.wave.scatterFrames.erase(owner.wave.scatterFrames.begin() + frameIdx);
                        owner.currentFrameIdx = std::min(owner.currentFrameIdx,
                                                         (int)owner.wave.scatterFrames.size() - 1);
                    }
                    owner.wave.scatterFromGridSnapshot.reset();
                    owner.updateHintText();
                    owner.rebuildRows();
                    owner.onLayerChanged();
                    refreshAfterDocMutation();
                };
            }
            framesListContainer.addAndMakeVisible(del.get());
            frameListDeletes.push_back(std::move(del));
        };

        auto coordLabel = [](const std::vector<int>& coord, int fallback1Based) -> juce::String {
            if (coord.empty())        return juce::String(fallback1Based);
            if (coord.size() == 1)    return "[" + juce::String(coord[0]) + "]";
            juce::String inner;
            for (size_t d = 0; d < coord.size(); ++d) {
                if (d) inner << ",";
                inner << coord[d];
            }
            return "(" + inner + ")";
        };

        if (owner.wave.mode == WavetableMode::Grid) {
            for (int i = 0; i < (int)owner.wave.cellWaveformIds.size(); ++i) {
                auto coord = owner.wave.cellIdxToGridCoord(i);
                juce::String label = coordLabel(coord, i + 1);
                const bool isEmpty = (owner.wave.cellWaveformIds[i] < 0);
                if (isEmpty) label += "  - empty -";
                addEntry(label, i, true, isEmpty);
            }
        } else {
            for (int i = 0; i < (int)owner.wave.scatterFrames.size(); ++i) {
                juce::String label = "Waveform " + juce::String(i + 1);
                if (!owner.wave.scatterFrames[i].label.empty())
                    label += "  " + juce::String(owner.wave.scatterFrames[i].label);
                addEntry(label, i, false, false);
            }
        }
        layoutFramesListEntries();
    }

    void rebuildAxisSteppers() {
        for (auto& s : axisSizeSliders) settingsContainer.removeChildComponent(s.get());
        for (auto& l : axisSizeLabels)  settingsContainer.removeChildComponent(l.get());
        axisSizeSliders.clear();
        axisSizeLabels.clear();

        if (owner.wave.mode != WavetableMode::Grid) return;

        auto axName = [](int i) -> juce::String {
            const char* n[] = {"X","Y","Z","W","V","U","T","S"};
            if (i >= 0 && i < (int)(sizeof(n)/sizeof(n[0]))) return n[i];
            return "Axis " + juce::String(i);
        };

        for (size_t d = 0; d < owner.wave.gridDims.size(); ++d) {
            auto lab = std::make_unique<juce::Label>();
            lab->setText(axName((int)d) + ":", juce::dontSendNotification);
            lab->setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            lab->setJustificationType(juce::Justification::centredLeft);
            settingsContainer.addAndMakeVisible(lab.get());

            auto sl = std::make_unique<juce::Slider>(juce::Slider::IncDecButtons,
                                                     juce::Slider::TextBoxLeft);
            sl->setRange(1.0, 64.0, 1.0);
            sl->setIncDecButtonsMode(juce::Slider::incDecButtonsDraggable_AutoDirection);
            sl->setValue((double)owner.wave.gridDims[d], juce::dontSendNotification);
            sl->setTooltip("Number of cells along axis " + axName((int)d) + " (1..64). "
                           "Grow to add more positions along this axis; shrink to drop "
                           "frames whose coord on this axis is beyond the new size.");
            int axisIdx = (int)d;
            sl->onValueChange = [this, sl_raw = sl.get(), axisIdx]() {
                int newSize = (int)sl_raw->getValue();
                owner.wave.resizeGridAxis(axisIdx, newSize);
                if (owner.currentFrameIdx >= (int)owner.wave.cellWaveformIds.size()
                    || (owner.currentFrameIdx >= 0
                        && owner.wave.cellWaveformIds[owner.currentFrameIdx] < 0))
                {
                    int next = -1;
                    for (int k = 0; k < (int)owner.wave.cellWaveformIds.size(); ++k)
                        if (owner.wave.cellWaveformIds[k] >= 0) { next = k; break; }
                    owner.currentFrameIdx = std::max(0, next);
                }
                owner.updateHintText();
                owner.rebuildRows();
                owner.onLayerChanged();
                refreshAfterDocMutation();
            };

            settingsContainer.addAndMakeVisible(sl.get());
            axisSizeLabels.push_back(std::move(lab));
            axisSizeSliders.push_back(std::move(sl));
        }
    }

    // Build (or rebuild after a dimension change) one slider per plane (i, j),
    // i < j, of the current view N-D space. Labels show the human-readable
    // axis names so a 4-D wavetable's "X-W" plane is identifiable at a glance.
    // Uses numDimensions() so Grid-mode axis adds produce the right plane set.
    void rebuildSliders() {
        for (auto& s : planeSliders) settingsContainer.removeChildComponent(s.get());
        for (auto& l : planeLabels)  settingsContainer.removeChildComponent(l.get());
        planeSliders.clear();
        planeLabels.clear();

        int N = std::max(2, owner.wave.numDimensions());
        auto pairs = LayeredWaveEditorComponent::scatterPlanePairs(N);

        auto axName = [](int i) -> juce::String {
            const char* n[] = {"X","Y","Z","W","V","U","T","S"};
            if (i >= 0 && i < (int)(sizeof(n)/sizeof(n[0]))) return n[i];
            return "Axis " + juce::String(i);
        };

        for (const auto& ij : pairs) {
            auto lab = std::make_unique<juce::Label>();
            lab->setText(axName(ij.first) + "-" + axName(ij.second), juce::dontSendNotification);
            lab->setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            lab->setJustificationType(juce::Justification::centredLeft);
            settingsContainer.addAndMakeVisible(lab.get());

            auto sl = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal,
                                                     juce::Slider::TextBoxRight);
            sl->setRange(-180.0, 180.0, 0.0);
            sl->setTextValueSuffix(juce::String::fromUTF8("\xC2\xB0")); // degrees
            sl->setNumDecimalPlacesToDisplay(1);
            sl->setValue(owner.getScatterPlaneAngle(ij.first, ij.second),
                         juce::dontSendNotification);
            int i = ij.first, j = ij.second;
            sl->setTooltip("Rotation angle in the " + axName(i) + "-" + axName(j) +
                           " plane (degrees). Drag to orbit the N-D view in this "
                           "plane. Visible regardless of which axes are currently "
                           "shown in the projection, so you can rotate axes out "
                           "of the projection plane and back in.");
            // Route slider drags through the owner's setter so the embedded
            // ScatterView in the main editor repaints too. We intentionally
            // do NOT call notifyScatterViewRotated() here - that refreshes
            // our own sliders, which would be redundant (we're the source
            // of this change) and could fight the live drag state.
            sl->onValueChange = [this, sl_raw = sl.get(), i, j]() {
                owner.setScatterPlaneAngle(i, j, (float)sl_raw->getValue());
            };

            settingsContainer.addAndMakeVisible(sl.get());
            planeLabels.push_back(std::move(lab));
            planeSliders.push_back(std::move(sl));
        }
    }

    // -- Per-axis position controls for the currently selected frame.
    //    Scatter mode: continuous 0..1 sliders, one per dimension.
    //    Grid mode: IncDec steppers, one per axis, picking the cell coord.
    //    Moving onto an occupied Grid cell swaps the two frames. --
    void rebuildSelFrameControls() {
        for (auto& s : selFrameSliders) settingsContainer.removeChildComponent(s.get());
        for (auto& l : selFrameLabels)  settingsContainer.removeChildComponent(l.get());
        selFrameSliders.clear();
        selFrameLabels.clear();

        const int N = std::max(1, owner.wave.numDimensions());
        const bool isGrid = (owner.wave.mode == WavetableMode::Grid);

        auto axName = [](int i) -> juce::String {
            const char* n[] = {"X","Y","Z","W","V","U","T","S"};
            if (i >= 0 && i < (int)(sizeof(n)/sizeof(n[0]))) return n[i];
            return juce::String(i);
        };

        for (int d = 0; d < N; ++d) {
            auto lab = std::make_unique<juce::Label>();
            lab->setText(axName(d) + ":", juce::dontSendNotification);
            lab->setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            lab->setJustificationType(juce::Justification::centredLeft);
            settingsContainer.addAndMakeVisible(lab.get());
            selFrameLabels.push_back(std::move(lab));

            auto sl = std::make_unique<juce::Slider>();
            if (isGrid) {
                int axisSize = (d < (int)owner.wave.gridDims.size())
                                  ? owner.wave.gridDims[(size_t)d] : 1;
                axisSize = std::max(1, axisSize);
                // Allow stepping ONE past the current end of the axis: the
                // value-change handler grows the axis to fit. This is what
                // makes "I added an axis (size 1), now press + to use it"
                // work without the user having to discover the separate
                // axis-size stepper first.
                int stepperMax = std::min(64, axisSize + 1);
                sl->setSliderStyle(juce::Slider::IncDecButtons);
                sl->setIncDecButtonsMode(juce::Slider::incDecButtonsDraggable_Vertical);
                sl->setTextBoxStyle(juce::Slider::TextBoxLeft, false, 50, 18);
                sl->setRange(1.0, (double)stepperMax, 1.0);
                sl->setValue(1.0, juce::dontSendNotification);
                sl->setTooltip("Cell position of the selected waveform on axis " + axName(d) +
                               " (1.." + juce::String(axisSize) + "). Press + at the end "
                               "to grow the axis by one cell. Moving onto an occupied cell "
                               "swaps the two waveforms.");
                int axisIdx = d;
                sl->onValueChange = [this, sl_raw = sl.get(), axisIdx]() {
                    handleSelFrameGridChange(axisIdx, (int)sl_raw->getValue() - 1);
                };
            } else {
                sl->setSliderStyle(juce::Slider::LinearHorizontal);
                sl->setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 18);
                sl->setRange(0.0, 1.0, 0.001);
                sl->setValue(0.5, juce::dontSendNotification);
                sl->setTooltip("Selected waveform's coordinate on axis " + axName(d) +
                               " (0..1). Equivalent to dragging the dot in the view, "
                               "but works on axes that aren't currently projected.");
                int axisIdx = d;
                sl->onValueChange = [this, sl_raw = sl.get(), axisIdx]() {
                    handleSelFrameScatterChange(axisIdx, (float)sl_raw->getValue());
                };
            }
            settingsContainer.addAndMakeVisible(sl.get());
            selFrameSliders.push_back(std::move(sl));
        }
        refreshSelFrameValues();
    }

    // Push the selected frame's position into the slider widgets without
    // firing onValueChange (we'd just write back what we read).
    void refreshSelFrameValues() {
        const int N = std::max(1, owner.wave.numDimensions());
        if ((int)selFrameSliders.size() != N) {
            rebuildSelFrameControls();
            layoutSettingsContainer();
            return;
        }
        const int idx = owner.currentFrameIdx;
        selFrameSectionLabel.setText("Selected waveform position (#" +
                                     juce::String(idx + 1) + "):",
                                     juce::dontSendNotification);
        if (owner.wave.mode == WavetableMode::Scatter) {
            std::vector<float> pos(N, 0.5f);
            if (idx >= 0 && idx < (int)owner.wave.scatterFrames.size()) {
                const auto& sf = owner.wave.scatterFrames[(size_t)idx];
                for (int d = 0; d < N && d < (int)sf.position.size(); ++d)
                    pos[(size_t)d] = sf.position[(size_t)d];
            }
            for (int d = 0; d < N; ++d)
                selFrameSliders[(size_t)d]->setValue((double)pos[(size_t)d],
                                                     juce::dontSendNotification);
        } else {
            std::vector<int> coord;
            if (idx >= 0 && idx < owner.wave.gridCellCount())
                coord = owner.wave.cellIdxToGridCoord(idx);
            for (int d = 0; d < N; ++d) {
                int sz = (d < (int)owner.wave.gridDims.size())
                            ? owner.wave.gridDims[(size_t)d] : 1;
                sz = std::max(1, sz);
                int c = (d < (int)coord.size()) ? coord[(size_t)d] : 0;
                // Update range too: the axis size may have changed since
                // the last build via the per-axis steppers. Same +1 trick
                // as in rebuildSelFrameControls so + at the end grows.
                int stepperMax = std::min(64, sz + 1);
                selFrameSliders[(size_t)d]->setRange(1.0, (double)stepperMax, 1.0);
                selFrameSliders[(size_t)d]->setValue((double)(c + 1),
                                                     juce::dontSendNotification);
            }
        }
    }

    void handleSelFrameScatterChange(int axisIdx, float v) {
        const int idx = owner.currentFrameIdx;
        if (idx < 0 || idx >= (int)owner.wave.scatterFrames.size()) return;
        auto& sf = owner.wave.scatterFrames[(size_t)idx];
        while ((int)sf.position.size() <= axisIdx) sf.position.push_back(0.5f);
        sf.position[(size_t)axisIdx] = juce::jlimit(0.0f, 1.0f, v);
        owner.onLayerChanged();
        // Moving a frame off its snapshotted cell center disqualifies the
        // lossless "Back to Grid" path - re-evaluate so the button greys
        // out the moment the user drags this slider.
        updateConvertButton();
        view->repaint();
    }

    void handleSelFrameGridChange(int axisIdx, int newCoord) {
        const int idx = owner.currentFrameIdx;
        if (idx < 0 || idx >= (int)owner.wave.cellWaveformIds.size()) return;
        if (axisIdx < 0 || axisIdx >= (int)owner.wave.gridDims.size()) return;
        if (newCoord < 0) return;

        auto srcCoord = owner.wave.cellIdxToGridCoord(idx);
        if ((int)srcCoord.size() <= axisIdx) return;
        if (srcCoord[(size_t)axisIdx] == newCoord) return;

        // If the user stepped past the current end of this axis, grow it
        // just enough so the requested coord becomes valid. This is what
        // makes a freshly-added (size-1) axis immediately usable - "+" at
        // the end means "extend the axis", matching how + Frame grows axis
        // 0 when every cell is full.
        if (newCoord >= owner.wave.gridDims[(size_t)axisIdx]) {
            int newSize = newCoord + 1;
            if (newSize > 64) return;  // hard cap matches axis-size stepper
            owner.wave.resizeGridAxis(axisIdx, newSize);
            // resizeGridAxis preserves frames by coord but row-major
            // indices change, so the size sliders need to refresh too.
            rebuildAxisSteppers();
        }

        // Recompute source / dest cell indices in the (possibly resized)
        // grid. Source coord didn't change; dest coord differs only on
        // axisIdx (set to newCoord).
        int srcIdx = owner.wave.gridCoordToCellIdx(srcCoord);
        auto destCoord = srcCoord;
        destCoord[(size_t)axisIdx] = newCoord;
        int destIdx = owner.wave.gridCoordToCellIdx(destCoord);
        if (srcIdx < 0 || destIdx < 0
            || srcIdx >= (int)owner.wave.cellWaveformIds.size()
            || destIdx >= (int)owner.wave.cellWaveformIds.size()) return;

        // Swap source and destination cell ids. This handles both move-to-
        // empty (dest was -1) and move-to-occupied (the displaced cell ref
        // ends up where the source used to be) uniformly. The selection
        // follows the user's intent and goes with the moved frame.
        std::swap(owner.wave.cellWaveformIds[(size_t)srcIdx],
                  owner.wave.cellWaveformIds[(size_t)destIdx]);
        owner.currentFrameIdx = destIdx;
        // Selection moved to the destination cell. If it now holds a
        // library entry, sync the editor target so the right pane stays
        // pointed at the waveform the user just dragged.
        {
            const int destLibId = owner.wave.libraryIdForCell(destIdx);
            if (destLibId >= 0) owner.currentLibraryId = destLibId;
        }
        owner.wave.scatterFromGridSnapshot.reset();
        owner.updateHintText();
        owner.rebuildRows();
        owner.onLayerChanged();
        refreshAfterDocMutation();
    }

    LayeredWaveEditorComponent& owner;
    juce::ComboBox stereoModeCombo;

    // Library list (scroll). Shows ALL library entries (used and unused),
    // each with a "(used N\u00D7)" suffix when placed. This is the canonical
    // list of waveform DATA in the wavetable - cells are placements that
    // reference these entries by id. Add / edit / delete operate here at
    // the data layer; placement on the arrangement view operates at the
    // cell layer (existing Cells list below).
    juce::Label     librarySectionLabel;
    juce::Viewport  libraryListViewport;
    juce::Component libraryListContainer;
    juce::TextButton addLibraryEntryBtn { "+ Waveform" };
    juce::TextButton assignToCellBtn { "Assign to selected cell" };
    std::vector<std::unique_ptr<juce::TextButton>> libraryRowButtons;
    std::vector<std::unique_ptr<FrameDeleteX>>     libraryRowDeletes;
    // The Library row selection IS the editor target (owner.currentLibraryId).
    // No separate sidebar-only state - clicking a row focuses the editor on
    // that entry, and the highlighted row is always the one being edited.

    // Frames list (scroll) - the placements list (cells in Grid mode,
    // scatter dots in Scatter mode). Distinct from the library: a row
    // here represents a cell, not a waveform.
    juce::Label     framesListLabel;
    juce::Viewport  framesListViewport;
    juce::Component framesListContainer;
    std::vector<std::unique_ptr<juce::TextButton>> frameListButtons;
    std::vector<std::unique_ptr<FrameDeleteX>>     frameListDeletes;

    juce::TextButton convertBtn;

    // Scrollable settings area. Everything below the Convert button row and
    // above the Reset rotation button lives inside settingsContainer so a
    // tall N-D wavetable (many axes, many rotation planes) can scroll
    // instead of being clipped.
    juce::Viewport  settingsViewport;
    juce::Component settingsContainer;

    // Grid axes section (lives inside settingsContainer)
    juce::Label gridAxesLabel;
    juce::TextButton addAxisBtn { "+ Axis" };
    juce::TextButton removeAxisBtn { "- Axis" };
    std::vector<std::unique_ptr<juce::Slider>> axisSizeSliders;
    std::vector<std::unique_ptr<juce::Label>>  axisSizeLabels;

    // Scatter section (lives inside settingsContainer)
    juce::Label  radiusLabel;
    juce::Slider radiusSlider;

    // Selected-frame position section (lives inside settingsContainer).
    // In Scatter mode the sliders edit scatterFrames[currentFrameIdx].position
    // directly; in Grid mode they're IncDec steppers that move the selected
    // frame between cells (swapping with whatever is in the destination).
    juce::Label selFrameSectionLabel;
    std::vector<std::unique_ptr<juce::Slider>> selFrameSliders;
    std::vector<std::unique_ptr<juce::Label>>  selFrameLabels;

    // Rotation section (lives inside settingsContainer)
    juce::Label rotationSectionLabel;
    std::vector<std::unique_ptr<juce::Slider>> planeSliders;
    std::vector<std::unique_ptr<juce::Label>>  planeLabels;

    // Reset rotation pinned to the bottom of the sidebar (NOT in the
    // scrollable area, so it's always reachable).
    juce::TextButton resetRotBtn { "Reset rotation" };
};

// ==============================================================================
// LayeredWaveEditorComponent
// ==============================================================================

LayeredWaveEditorComponent::LayeredWaveEditorComponent(NodeGraph& g, int nid, std::function<void()> apply)
    : graph(g), nodeId(nid), onApply(std::move(apply))
{
    // Decode existing state. Try wavetable first, then fall back to single
    // layered waveform (wrapped as a 1-frame wavetable), then default sine.
    auto* nd = graph.findNode(nodeId);
    std::string script = nd ? nd->script : "";
    if (!wave.decode(script)) {
        LayeredWaveform single;
        if (single.decode(script)) {
            wave.tableSize = single.tableSize;
            const int libId = wave.addLibraryEntry(
                std::make_unique<LayeredWaveform>(std::move(single)));
            wave.gridDims = {1};
            wave.cellWaveformIds.push_back(libId);
        } else {
            wave = WavetableDoc::defaultSingleSine();
        }
    }
    // Note: we no longer auto-seed a default sine into an empty Grid
    // wavetable. The "Wavetable" menu entry on the node graph opens the
    // editor with a deliberately empty WavetableDoc (one null cell), and
    // the user picks the first waveform's type via "+ Waveform". The
    // Scatter seed below is still kept for now because Scatter mode is
    // only reached by explicit user conversion, and an empty scatter
    // wavetable has no visible affordances yet.
    if (wave.mode == WavetableMode::Scatter && wave.scatterFrames.empty()) {
        // Seed with two frames so the user can immediately see the morph.
        // Both reference fresh library entries.
        const int idA = wave.addLibraryEntry(
            std::make_unique<LayeredWaveform>(LayeredWaveform::defaultSine()));
        const int idB = wave.addLibraryEntry(
            std::make_unique<LayeredWaveform>(LayeredWaveform::defaultSine()));
        ScatterFrame a; a.waveformId = idA;
        a.position.assign(wave.scatterDims, 0.5f);
        a.position[0] = 0.25f;
        ScatterFrame b; b.waveformId = idB;
        b.position.assign(wave.scatterDims, 0.5f);
        b.position[0] = 0.75f;
        wave.scatterFrames.push_back(std::move(a));
        wave.scatterFrames.push_back(std::move(b));
    }
    // Default to the first non-empty cell so currentFrameIdx points at a
    // real frame whenever one exists. For a fully-empty wavetable
    // (defaultEmpty()), leave currentFrameIdx at 0 - it points at the
    // single empty cell, which updateHintText / currentLayers /
    // updateFrameEditorEmbed all handle as "no current frame".
    currentFrameIdx = 0;
    if (wave.mode == WavetableMode::Grid) {
        for (int k = 0; k < (int)wave.cellWaveformIds.size(); ++k)
            if (wave.cellWaveformIds[k] >= 0) { currentFrameIdx = k; break; }
    }
    // Seed the editor target: prefer the library entry referenced by the
    // initially-selected cell. If the doc is mid-load with empty cells but
    // a populated library (e.g. orphaned entries), fall back to the first
    // library entry so the editor has SOMETHING to edit. -1 means "no
    // entry yet" - paint() shows a placeholder in that case.
    currentLibraryId = -1;
    {
        const int seedFromCell = wave.libraryIdForCell(currentFrameIdx);
        if (seedFromCell >= 0) currentLibraryId = seedFromCell;
        else if (!wave.library.empty()) currentLibraryId = wave.library.front().id;
    }
    currentPosition.assign(std::max(1, wave.numDimensions()), 0.5f);

    addAndMakeVisible(addLayerBtn);
    addLayerBtn.setTooltip("Add a new harmonic layer to the current waveform. "
                           "Each layer is a sine, saw, square, triangle, noise, or drawn shape "
                           "that gets summed into the final waveform.");
    addLayerBtn.onClick = [this]() {
        auto& layers = currentLayers();
        WaveLayer l;
        l.shape = WaveLayer::Sine;
        l.ratio = (int)layers.size() + 1; // each new layer defaults to next harmonic
        l.phase = 0.0f;
        l.amp = 0.5f;
        layers.push_back(l);
        rebuildRows();
        onLayerChanged();
    };

    // The "+ Waveform" button used to live up here on the top toolbar.
    // It now lives in the arrangement-view sidebar (below the Library
    // list), so the user finds it next to the library it adds to instead
    // of detached up in the title bar. The popup-menu flow is identical;
    // see WavetableViewWindowContent::addLibraryEntryBtn and the call
    // to showAddWaveformMenu it makes.

    // Build the embedded arrangement view. It owns the ScatterView and
    // the sidebar (Library list, Cells list, mode-conversion, axis
    // steppers, RBF radius, per-axis position controls, and N-D rotation
    // sliders). Always visible - it occupies the left half of the dialog
    // body in the side-by-side layout.
    arrangementView = std::make_unique<WavetableViewWindowContent>(*this);
    addAndMakeVisible(arrangementView.get());

    // Auto-detect screen DPI from JUCE so the parallax math is calibrated
    // to the user's actual display.
    if (arrangementView && arrangementView->view) {
        auto& displays = juce::Desktop::getInstance().getDisplays();
        const auto* main = displays.getPrimaryDisplay();
        if (main && main->dpi > 1.0)
            arrangementView->view->dpi = (float)main->dpi;
    }

    addAndMakeVisible(helpBtn);
    helpBtn.setTooltip("Open the wavetable / layered waveform docs");
    helpBtn.onClick = []() { openHelpDocFile("wavetables.html"); };

    // Discoverability hint: appears below the toolbar when the wavetable
    // still has just one frame, since the "Waveform / Wavetable" menu name
    // doesn't make the multi-frame N-D arrangement capability obvious.
    // Wording aims to make clear that the Position control lives on the
    // synth node OUT in the graph, NOT inside this editor. Earlier
    // revisions buried "on the synth node in the graph" mid-sentence and
    // it kept reading as if Position were one of this editor's controls,
    // so this version puts the location up front ("Back in the node
    // graph, ...") on its own sentence.
    addAndMakeVisible(hintLabel);
    hintLabel.setText("Tip: this is a wavetable editor - click  + Waveform  to add more waveforms, "
                      "then arrange them in the grid / scatter view below. Use  + Axis  in the "
                      "sidebar to add more arrangement axes. "
                      "Back out in the node graph, the synth node itself has a Position knob "
                      "(or Position 1, 2, ... once you add more axes) that morphs between waveforms.",
                      juce::dontSendNotification);
    hintLabel.setJustificationType(juce::Justification::centredLeft);
    hintLabel.setColour(juce::Label::textColourId,
                        juce::Colours::white.withAlpha(0.55f));
    hintLabel.setFont(juce::Font(juce::FontOptions(12.0f).withStyle("italic")));
    hintLabel.setInterceptsMouseClicks(false, false);
    // Visibility is reapplied whenever frames change (in updateHintText)
    // and on first layout (in resized).
    hintLabel.setVisible(wave.cellWaveformIds.size() <= 1);

    // Compare (#9): flip the synth between two render modes that both
    // apply to a wavetable cycle, so the same waveform can be A/B
    // auditioned. Sets the Synth Mode param: 0 = Direct (SamplePerPoint),
    // 2 = Additive bank. The third Synth Mode (1 = AM-sine) is not exposed
    // here because it's a terrain-sonification mode for 2D/N-D terrains,
    // not a wavetable render mode.
    addAndMakeVisible(compareLabel);
    compareLabel.setFont(11.0f);
    compareLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.75f));
    compareLabel.setTooltip(
        "How this wavetable cycle is rendered to audio. Direct plays the "
        "cycle straight through one oscillator (classic wavetable - fast "
        "and faithful to what you authored). Additive bank FFTs the cycle "
        "into its harmonic series and re-synthesises it as a sum of "
        "independent sine partials - more CPU, but it doesn't alias when "
        "pitched up and lets each partial be modulated separately (planned).");

    addAndMakeVisible(compareDirectBtn);
    addAndMakeVisible(compareAdditiveBtn);
    compareDirectBtn.setTooltip(
        "Direct (classic wavetable): the cycle is read by one oscillator "
        "and the table value IS the audio sample. Fast and faithful to "
        "what you authored.");
    compareAdditiveBtn.setTooltip(
        "Additive bank: the cycle is FFT'd to extract up to 64 harmonic "
        "amplitudes and phases, and each partial is then synthesised as "
        "its own sine oscillator running at fundamental x harmonic number. "
        "For a static cycle this sounds nearly identical to Direct mode, "
        "but it doesn't alias when you pitch up, partials can be modulated "
        "individually (future feature), and it costs ~N times more CPU "
        "where N is the partial count.");

    auto setSynthMode = [this](float v) {
        if (auto* nd = graph.findNode(nodeId))
            for (auto& p : nd->params)
                if (p.name == "Synth Mode") { p.value = v; break; }
    };
    auto refreshCompareTint = [this]() {
        // Read the Synth Mode param and highlight whichever of the two
        // compare buttons currently matches. If it's AM-sine (1), neither
        // highlights - the buttons just look idle. That's fine: AM-sine is
        // selected from the node's main param row, not from this editor.
        float mv = 0.0f;
        if (auto* nd = graph.findNode(nodeId))
            for (auto& p : nd->params)
                if (p.name == "Synth Mode") { mv = p.value; break; }
        int m = juce::jlimit(0, 2, (int)std::round(mv));
        juce::Colour green = juce::Colour(60, 100, 60);
        juce::Colour blue  = juce::Colour(60, 60, 100);
        juce::Colour idle  = juce::Colour(55, 55, 60);
        compareDirectBtn  .setColour(juce::TextButton::buttonColourId,
                                     (m == 0) ? green : idle);
        compareAdditiveBtn.setColour(juce::TextButton::buttonColourId,
                                     (m == 2) ? blue  : idle);
    };
    compareDirectBtn.onClick = [setSynthMode, refreshCompareTint]() {
        setSynthMode(0.0f); // Direct (SamplePerPoint)
        refreshCompareTint();
    };
    compareAdditiveBtn.onClick = [setSynthMode, refreshCompareTint]() {
        setSynthMode(2.0f); // Additive bank
        refreshCompareTint();
    };
    refreshCompareTint();

    addAndMakeVisible(applyBtn);
    applyBtn.setButtonText("Apply");
    applyBtn.setTooltip("Save the current waveform edits to the synth without closing this editor");
    applyBtn.onClick = [this]() {
        commitToNode();
        if (onApply) onApply();
    };

    addAndMakeVisible(closeBtn);
    closeBtn.setButtonText("Close");
    closeBtn.onClick = [this]() {
        // Commit on close too so work isn't lost by accident.
        commitToNode();
        if (onApply) onApply();
        // Single-window editor: just delete our parent dialog. The
        // arrangement view is an embedded child of this component and
        // dies with us.
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
            juce::Component::SafePointer<juce::DialogWindow> safe(dw);
            juce::MessageManager::callAsync([safe]() {
                if (safe) delete safe.getComponent();
            });
        }
    };

    addAndMakeVisible(layersViewport);
    layersViewport.setViewedComponent(&layersContainer, false);
    layersViewport.setScrollBarsShown(true, false);

    updateHintText();
    rebuildScatterUI();
    rebuildRows();
    refreshPreview();
    syncPositionParams();
    // Sized to fit a 1080p display with room for the OS taskbar and the
    // dialog's own non-native title bar (~30px). Side-by-side layout:
    // arrangement view (~960 px) on the left, per-waveform editor on
    // the right (~440 px), with margins and a gap.
    setSize(1500, 780);
}

void LayeredWaveEditorComponent::showAddWaveformMenu(juce::Component* anchor) {
    // Anchor must be on-screen - the popup menu attaches itself to its
    // bounds. The normal caller is the sidebar's "+ Waveform" button on
    // WavetableViewWindowContent; if the main editor is hidden (acting as
    // the state owner behind a pop-out wavetable view), the pop-out's own
    // "+ Waveform" button passes itself instead. There is no longer an
    // editor-owned fallback button to use when anchor is null - the
    // top-bar "+ Waveform" was removed in favour of the sidebar version.
    // Bail rather than crash if a future caller forgets to supply one.
    if (anchor == nullptr) {
        jassertfalse;
        return;
    }

    // Popup menu - pick the type of the new waveform. Item 1 is the
    // legacy duplicate-current behavior; items 2/3/4 insert a fresh
    // default of each editor-authored type; items 5/6/7 capture from
    // an audio source. Mixed-type wavetables are the whole point: you
    // can sit a spectral, wavelet or captured waveform alongside
    // layered waveforms inside one wavetable.
    juce::PopupMenu m;
    m.addSectionHeader("Edit from scratch");
    m.addItem(2, "Layered (time domain)");
    m.addItem(3, "Frequency Domain (FFT)");
    m.addItem(4, "Wavelet Space (DWT)");
    m.addSeparator();
    m.addSectionHeader("Capture from audio");
    // Items 5/6/7: open the capture dialog with one of three audio
    // sources. Each produces N captured waveforms (one per equally-
    // spaced position in a region) and lays them along Position so
    // the synth morphs through them.
    m.addItem(5, "From project song...");
    m.addItem(6, "From microphone / audio input...");
    m.addItem(7, "From audio file...");
    // Duplicate only makes sense when there's a current waveform to
    // duplicate - hide it on an empty wavetable.
    const bool haveCurrent = (wave.mode == WavetableMode::Grid)
        ? (currentFrameIdx >= 0
            && currentFrameIdx < (int)wave.cellWaveformIds.size()
            && wave.cellWaveformIds[currentFrameIdx] >= 0)
        : (currentFrameIdx >= 0
            && currentFrameIdx < (int)wave.scatterFrames.size());
    if (haveCurrent) {
        m.addSeparator();
        m.addItem(1, "Duplicate current waveform");
    }

    auto makeFreshFrame = [](int r) -> std::unique_ptr<IWavetableFrame> {
        if (r == 2) return std::make_unique<LayeredWaveform>(LayeredWaveform::defaultSine());
        if (r == 3) return std::make_unique<SpectralFrame>(SpectralDoc::defaultBuiltin());
        if (r == 4) return std::make_unique<WaveletFrame>(WaveletFrame::defaultEmpty());
        return nullptr;
    };

    m.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(anchor),
        [this, makeFreshFrame](int r) {
            if (r == 0) return;
            if (r == 5 || r == 6 || r == 7) {
                // Capture-from-audio entries. The capture UI lives inline
                // in the right pane (same screen real estate as the
                // layered / spectral / wavelet editors) rather than in a
                // separate window. It produces N frames on Save and we
                // then lay them out along Position; Close just tears the
                // panel down.
                showCapturePanelInline(r - 5);
                return;
            }
            if (wave.mode == WavetableMode::Grid) {
                std::unique_ptr<IWavetableFrame> nf;
                if (r == 1) {
                    // Duplicate current: clone the waveform the editor is
                    // currently focused on (the library entry, not the
                    // selected cell). The clone becomes its own library
                    // entry so the two can be edited independently.
                    if (auto* f = currentEditingFrame())
                        nf = f->clone();
                    else
                        nf = std::make_unique<LayeredWaveform>(LayeredWaveform::defaultSine());
                } else {
                    nf = makeFreshFrame(r);
                }
                if (!nf) return;

                // Sparse-aware placement: drop the new waveform's id in the
                // first empty cell. If every cell is already occupied, grow
                // axis 0 by 1 (the conventional "primary" axis) and place
                // it at the start of the new slice. This keeps the
                // gridDims-product invariant aligned with cellWaveformIds.
                int placeAt = -1;
                for (int k = 0; k < (int)wave.cellWaveformIds.size(); ++k) {
                    if (wave.cellWaveformIds[k] < 0) { placeAt = k; break; }
                }
                if (placeAt < 0) {
                    if (wave.gridDims.empty()) wave.gridDims.push_back(1);
                    const int axis = 0;
                    const int oldSize = wave.gridDims[axis];
                    wave.resizeGridAxis(axis, oldSize + 1);
                    // After growing axis 0, the brand-new slice is at
                    // coord[axis] = oldSize, and (since the other axes
                    // already had their full extent and were preserved
                    // by resizeGridAxis) every cell in that slice is
                    // empty. Pick the first one.
                    std::vector<int> coord(wave.gridDims.size(), 0);
                    coord[axis] = oldSize;
                    placeAt = wave.gridCoordToCellIdx(coord);
                    if (placeAt < 0) placeAt = (int)wave.cellWaveformIds.size() - 1;
                }
                const int libId = wave.addLibraryEntry(std::move(nf));
                wave.cellWaveformIds[placeAt] = libId;
                currentFrameIdx = placeAt;
                // Sync the editor target to the freshly-added entry so the
                // user can immediately edit what they just created.
                currentLibraryId = libId;
                // Adding or replacing a cell invalidates any pending
                // scatter-revert snapshot.
                wave.scatterFromGridSnapshot.reset();
                updateHintText();
            } else {
                ScatterFrame sf;
                if (r == 1
                    && currentFrameIdx >= 0
                    && currentFrameIdx < (int)wave.scatterFrames.size()) {
                    // Duplicate: copy the currently-selected scatter dot.
                    // Two paths: (a) share the same library entry (cheap,
                    // edits propagate), or (b) clone the waveform so each
                    // dot is independently editable. We pick (b) to match
                    // the user's mental model of "+ Waveform / Duplicate
                    // current waveform" - they expect a separate waveform
                    // they can edit without disturbing the source.
                    sf = wave.scatterFrames[currentFrameIdx];
                    if (auto* src = wave.libraryFrameById(sf.waveformId)) {
                        sf.waveformId = wave.addLibraryEntry(src->clone());
                    } else {
                        sf.waveformId = wave.addLibraryEntry(
                            std::make_unique<LayeredWaveform>(LayeredWaveform::defaultSine()));
                    }
                } else {
                    std::unique_ptr<IWavetableFrame> nf = (r == 1)
                        ? std::unique_ptr<IWavetableFrame>(std::make_unique<LayeredWaveform>(LayeredWaveform::defaultSine()))
                        : makeFreshFrame(r);
                    if (!nf) return;
                    sf.waveformId = wave.addLibraryEntry(std::move(nf));
                }
                // Place the new frame at the center of the N-D cube by
                // default. If a frame already sits exactly at the center,
                // the new one gets a small offset so it isn't hidden.
                // The user can drag it to its final spot afterwards.
                sf.position.assign(wave.scatterDims, 0.5f);
                const float eps2 = 0.02f * 0.02f;
                for (const auto& other : wave.scatterFrames) {
                    float d2 = 0.0f;
                    for (size_t k = 0; k < sf.position.size() && k < other.position.size(); ++k) {
                        float dv = sf.position[k] - other.position[k];
                        d2 += dv * dv;
                    }
                    if (d2 < eps2) {
                        for (auto& v : sf.position) v = juce::jlimit(0.0f, 1.0f, v + 0.05f);
                        break;
                    }
                }
                const int newLibId = sf.waveformId;
                wave.scatterFrames.push_back(std::move(sf));
                currentFrameIdx = (int)wave.scatterFrames.size() - 1;
                // Sync the editor target to the freshly-added entry so the
                // user can immediately edit what they just created.
                currentLibraryId = newLibId;
                repaintScatterViews();
            }
            rebuildRows();
            onLayerChanged();
            // In the side-by-side layout the per-waveform editor on the
            // right is always visible, so we just need to make sure the
            // selection points at the freshly-added cell. switchToFrame
            // is a no-op when idx == currentFrameIdx (already true here
            // since the add code set it), so an explicit refresh is
            // enough - the right pane is already bound to it.
            updateHintText();
            refreshPreview();
            notifyPopoutFrameOrPositionChanged();
        });
}

void LayeredWaveEditorComponent::syncPositionParams() {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;
    int n = wave.numDimensions();

    // Remove existing Position params (any param starting with "Position").
    auto isPosName = [](const std::string& s) {
        return s.rfind("Position", 0) == 0;
    };
    nd->params.erase(std::remove_if(nd->params.begin(), nd->params.end(),
        [&](const Param& p) { return isPosName(p.name); }), nd->params.end());

    // Add fresh Position params (1D = "Position", N-D = "Position 1".."Position N")
    for (int i = 0; i < n; ++i) {
        Param p;
        p.name = (n == 1) ? "Position" : ("Position " + std::to_string(i + 1));
        p.value = 0.5f;
        p.minVal = 0.0f;
        p.maxVal = 1.0f;
        nd->params.push_back(std::move(p));
    }
}

void LayeredWaveEditorComponent::rebuildScatterUI() {
    // Make sure the shared rotation-state vector has the right plane count
    // for the current dim count, then push the new count out to the
    // arrangement view (which rebuilds its slider strip if it changed).
    ensureScatterPlaneAngles();
    notifyScatterViewRotated();

    // Default to projecting axes 0 / 1 / 2 (the arrangement view's
    // rotation sliders handle viewing angles for higher-N wavetables).
    if (arrangementView && arrangementView->view)
        arrangementView->view->setProjection(0, 1, 2);

    resized();
}

// ---- Editor-target accessors (read currentLibraryId, not a cell) ----

IWavetableFrame* LayeredWaveEditorComponent::currentEditingFrame() {
    return wave.libraryFrameById(currentLibraryId);
}

const IWavetableFrame* LayeredWaveEditorComponent::currentEditingFrame() const {
    return wave.libraryFrameById(currentLibraryId);
}

LayeredWaveform* LayeredWaveEditorComponent::currentEditingLayeredFrame() {
    return wave.layeredFrameByLibrary(currentLibraryId);
}

const LayeredWaveform* LayeredWaveEditorComponent::currentEditingLayeredFrame() const {
    return wave.layeredFrameByLibrary(currentLibraryId);
}

std::vector<WaveLayer>& LayeredWaveEditorComponent::currentLayers() {
    if (auto* f = currentEditingLayeredFrame()) return f->layers;
    // Non-layered frames (spectral, wavelet, granular) have no layers. The
    // layered editor shows them as an empty layer list; their contents are
    // edited via the embedded sub-editor (SpectralEditorComponent etc.).
    static std::vector<WaveLayer> empty;
    return empty;
}

const std::vector<WaveLayer>& LayeredWaveEditorComponent::currentLayers() const {
    if (auto* f = currentEditingLayeredFrame()) return f->layers;
    static const std::vector<WaveLayer> empty;
    return empty;
}

void LayeredWaveEditorComponent::setEditingLibraryEntry(int libId) {
    if (libId == currentLibraryId) return;
    // -1 is the valid "no editing target" state (empty library). Any other
    // id must exist in the library; otherwise we silently keep the current
    // target rather than dropping to nothing (avoids accidental clears from
    // stale ids in deferred callbacks).
    if (libId != -1 && wave.findLibraryIndexById(libId) < 0) return;
    currentLibraryId = libId;
    updateHintText();
    rebuildRows();
    refreshPreview();
    notifyPopoutFrameOrPositionChanged();
}

void LayeredWaveEditorComponent::switchToFrame(int idx) {
    // Now means: "user picked a cell in the arrangement view / Cells list /
    // (old) frame tab". The cell becomes the selected one (currentFrameIdx),
    // and if the cell holds a library entry we ALSO sync the editor target
    // (currentLibraryId) to it - selecting a non-empty cell almost always
    // means "I want to edit this one". Selecting an empty cell leaves the
    // editor target alone.
    //
    // Acceptable cell ranges are mode-dependent: Grid cells include empty
    // ones (so users can pick an empty cell as an Assign destination);
    // Scatter cells are always populated (a dot only exists if it holds a
    // waveform).
    int max = (wave.mode == WavetableMode::Grid)
        ? (int)wave.cellWaveformIds.size()
        : (int)wave.scatterFrames.size();
    if (idx < 0 || idx >= max) return;

    const bool cellChanged    = (idx != currentFrameIdx);
    currentFrameIdx = idx;

    // If this cell holds a library entry, sync the editor target to it.
    const int cellLibId = wave.libraryIdForCell(idx);
    bool editorChanged = false;
    if (cellLibId >= 0 && cellLibId != currentLibraryId) {
        currentLibraryId = cellLibId;
        editorChanged = true;
    }

    if (!cellChanged && !editorChanged) return;

    updateHintText();
    rebuildRows();
    refreshPreview();
    // Pop-out's per-axis "Selected frame position" strip tracks the
    // editor's selection in both modes.
    notifyPopoutFrameOrPositionChanged();
}

void LayeredWaveEditorComponent::FrameDeleteX::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat().reduced(2.0f);
    const float cx = r.getCentreX();
    const float cy = r.getCentreY();
    // Arm length: 35% of the shorter axis, so the X stays compact even in
    // taller tab rows.
    const float s = std::min(r.getWidth(), r.getHeight()) * 0.30f;
    const juce::Colour col = hover
        ? juce::Colour(0xffff5050)                       // bright red on hover
        : juce::Colour(0xffd04040).withAlpha(0.75f);     // muted red at rest
    const float thick = hover ? 2.2f : 1.6f;
    g.setColour(col);
    g.drawLine(cx - s, cy - s, cx + s, cy + s, thick);
    g.drawLine(cx - s, cy + s, cx + s, cy - s, thick);
}

void LayeredWaveEditorComponent::FrameDeleteX::mouseUp(const juce::MouseEvent& e) {
    if (getLocalBounds().contains(e.getPosition()) && onClick) onClick();
}

void LayeredWaveEditorComponent::updateHintText() {
    // Discoverability hint - shown while the library has 0 or 1 entries
    // so a fresh user sees the basic interactions (add a waveform, click
    // a cell, click a row). Hides once the library has a couple of
    // entries (the multi-waveform nature is visible at that point).
    const bool showHint = (wave.library.size() <= 1);
    if (wave.mode == WavetableMode::Grid) {
        hintLabel.setText(
            "Tip: this is a wavetable. Click  + Waveform  to add a waveform to the "
            "library, then place it in cells on the left. Click a Library row "
            "to edit a waveform; edits flow into every cell that uses it. "
            "Back out in the node graph, the synth node itself has a Position knob "
            "(or Position 1, 2, ... once you add more axes) that morphs between waveforms.",
            juce::dontSendNotification);
    } else {
        hintLabel.setText(
            "Tip: click  + Waveform  to add a waveform; click a dot to select it, "
            "or click a Library row to edit its waveform. "
            "Back out in the node graph, the synth node itself has a Position knob "
            "(or Position 1, 2, ... once you add more axes) that morphs between waveforms.",
            juce::dontSendNotification);
    }
    if (hintLabel.isVisible() != showHint) {
        hintLabel.setVisible(showHint);
        resized();
    }
}

LayeredWaveEditorComponent::~LayeredWaveEditorComponent() {
    // If there's a pending debounced apply, flush it now so the audio
    // engine picks up the last edits even if the editor closes quickly.
    if (isTimerRunning()) {
        stopTimer();
        if (onApply) onApply();
    }
    // arrangementView's unique_ptr destructs here after the member-clean-up
    // chain runs - it holds a ScatterView that references *this, but JUCE
    // tears children down before the parent's members, so by the time we
    // hit this dtor body the ScatterView is already gone.
}

// =========================================================================
// N-D rotation state (shared between the embedded ScatterView and the
// pop-out wavetable view's ScatterView). The owner is the source of truth
// so both views always show the same rotation - dragging in one updates
// the other, sliders in the pop-out update the embedded view.
// =========================================================================

int LayeredWaveEditorComponent::scatterPlaneIndexFor(int i, int j, int N) {
    // Lexicographic enumeration of (i, j) with i < j. The k-th plane is
    // the k-th such pair. The (i, j) -> k formula sums up the plane counts
    // for rows 0..i-1 (each row r contributes (N-1-r) planes) plus the
    // offset within row i.
    if (i > j) std::swap(i, j);
    if (i < 0 || j >= N || i == j) return -1;
    int base = i * N - i - i * (i - 1) / 2;
    return base + (j - i - 1);
}

std::vector<std::pair<int,int>> LayeredWaveEditorComponent::scatterPlanePairs(int N) {
    std::vector<std::pair<int,int>> out;
    out.reserve((size_t)scatterPlaneCount(N));
    for (int i = 0; i < N; ++i)
        for (int j = i + 1; j < N; ++j)
            out.emplace_back(i, j);
    return out;
}

void LayeredWaveEditorComponent::ensureScatterPlaneAngles() {
    // Rotation lives in the "view" N-D space, which is gridDims.size() in
    // Grid mode and scatterDims in Scatter mode. numDimensions() returns
    // exactly that, so we use it instead of scatterDims directly - otherwise
    // adding a Grid axis wouldn't grow the plane-angle vector.
    int N = std::max(2, wave.numDimensions());
    int want = scatterPlaneCount(N);
    if ((int)scatterPlaneAngles.size() != want)
        scatterPlaneAngles.resize((size_t)want, 0.0f);
}

float LayeredWaveEditorComponent::getScatterPlaneAngle(int i, int j) const {
    int N = std::max(2, wave.numDimensions());
    int k = scatterPlaneIndexFor(i, j, N);
    if (k < 0 || k >= (int)scatterPlaneAngles.size()) return 0.0f;
    return scatterPlaneAngles[(size_t)k];
}

void LayeredWaveEditorComponent::setScatterPlaneAngle(int i, int j, float deg) {
    ensureScatterPlaneAngles();
    int N = std::max(2, wave.numDimensions());
    int k = scatterPlaneIndexFor(i, j, N);
    if (k >= 0 && k < (int)scatterPlaneAngles.size()) {
        scatterPlaneAngles[(size_t)k] = deg;
        repaintScatterViews();
    }
}

void LayeredWaveEditorComponent::repaintScatterViews() {
    if (arrangementView && arrangementView->view) arrangementView->view->repaint();
}


void LayeredWaveEditorComponent::showCapturePanelInline(int sourceKind) {
    // sourceKind: 0=Playback (project song), 1=Mic, 2=File.
    //
    // Playback uses CaptureFromSongDialog (#capV2): pre-renders the
    // whole project to PCM offline, shows a song-length timeline with
    // play / pause / stop transport and a draggable marker. While
    // Playing the engine plays back the rendered song at full
    // fidelity; while Paused or Scrubbing it loops a short grain
    // centered on the marker so the user can audition before saving.
    //
    // Mic / File still use the original CaptureFromPlaybackDialog -
    // they don't need offline rendering since their data is either a
    // live ring buffer (mic) or a one-shot file load (file).
    //
    // The same Component classes that used to be hosted in a JUCE
    // DialogWindow are now embedded directly into the right pane via
    // capturePanel. They detect the inline mode by virtue of having
    // their onDismiss callback set: the Capture / Save and Close
    // buttons call onDismiss instead of walking up to a DialogWindow.
    // Save still fires onCapture (which appends frames to the
    // wavetable); Close just clears the panel.
    auto onCaptured = [this](std::vector<std::unique_ptr<IWavetableFrame>> frames) {
        appendCapturedFramesAlongPosition(std::move(frames));
    };

    std::unique_ptr<juce::Component> panel;
    if (sourceKind == 0) {
        auto* eng = AudioEngine::getInstance();
        NodeGraph* gp = eng ? eng->getGraph() : nullptr;
        Transport* tp = eng ? eng->getTransport() : nullptr;
        if (!gp || !tp) {
            // Engine not wired up yet (shouldn't happen once App has
            // run). Fall back to the legacy live-tap component so the
            // user at least gets *some* path through.
            auto p = std::make_unique<CaptureFromPlaybackDialog>(
                CaptureSource::Playback, wave.tableSize, onCaptured);
            p->onDismiss = [this]() { dismissCapturePanel(); };
            panel = std::move(p);
        } else {
            auto p = std::make_unique<CaptureFromSongDialog>(
                *gp, *tp, wave.tableSize, onCaptured);
            p->onDismiss = [this]() { dismissCapturePanel(); };
            panel = std::move(p);
        }
    } else {
        CaptureSource src = (sourceKind == 1) ? CaptureSource::Mic
                                              : CaptureSource::File;
        auto p = std::make_unique<CaptureFromPlaybackDialog>(
            src, wave.tableSize, onCaptured);
        p->onDismiss = [this]() { dismissCapturePanel(); };
        panel = std::move(p);
    }

    // Tear down any existing panel first (re-entering capture while one
    // is already up). Lifetimes are clean since this method only runs on
    // the message thread.
    if (capturePanel) {
        removeChildComponent(capturePanel.get());
        capturePanel.reset();
    }
    capturePanel = std::move(panel);
    addAndMakeVisible(capturePanel.get());
    resized();
}

void LayeredWaveEditorComponent::dismissCapturePanel() {
    if (!capturePanel) return;
    removeChildComponent(capturePanel.get());
    capturePanel.reset();
    // Bring the per-frame editor / Compare panel / preview back. resized()
    // handles the visibility / bounds toggle now that capturePanel is null.
    resized();
    repaint();
}

void LayeredWaveEditorComponent::appendCapturedFramesAlongPosition(
    std::vector<std::unique_ptr<IWavetableFrame>> frames)
{
    if (frames.empty()) return;
    const int N = (int)frames.size();

    // Single-frame capture (e.g. project-song "Save waveform at marker",
    // single-slice mic/file capture) targets the currently SELECTED cell
    // if one is selected. This is what the user means when they pick a
    // moment in the song, select a specific cell in the arrangement view,
    // and click Save - "put this captured waveform HERE", not "make a new
    // cell at the end". Multi-frame captures (mic / file with N>1 slices)
    // still append along axis 0, since N>1 frames can't all fit in one
    // cell.
    if (N == 1) {
        const int idx = currentFrameIdx;
        if (wave.mode == WavetableMode::Grid
            && idx >= 0 && idx < (int)wave.cellWaveformIds.size())
        {
            const int libId = wave.addLibraryEntry(std::move(frames[0]));
            // assignCellToLibrary replaces the cell's reference; the old
            // library entry survives (orphaned), so the user can re-place
            // or delete it via the Library list. Library entries are now
            // independent of cells.
            wave.assignCellToLibrary(idx, libId);
            // Sync the editor target to the freshly-captured entry so the
            // user sees it on the right pane immediately.
            currentLibraryId = libId;
            wave.scatterFromGridSnapshot.reset();
            updateHintText();
            rebuildRows();
            onLayerChanged();
            notifyPopoutFrameOrPositionChanged();
            notifyPopoutDocMutated();
            return;
        }
        if (wave.mode == WavetableMode::Scatter
            && idx >= 0 && idx < (int)wave.scatterFrames.size())
        {
            const int libId = wave.addLibraryEntry(std::move(frames[0]));
            wave.scatterFrames[(size_t)idx].waveformId = libId;
            currentLibraryId = libId;
            wave.scatterFromGridSnapshot.reset();
            updateHintText();
            rebuildRows();
            onLayerChanged();
            notifyPopoutFrameOrPositionChanged();
            notifyPopoutDocMutated();
            repaintScatterViews();
            return;
        }
        // No valid selection - fall through to the append path below so
        // the captured frame doesn't get dropped on the floor.
    }

    if (wave.mode == WavetableMode::Grid) {
        // Append along axis 0. Grow gridDims[0] by N (or set it to N
        // if the wavetable was empty), and fill the new slice with the
        // captured frames at coord[other] = 0. If the editor's initial
        // state is the default single-sine in a 1-cell grid we still
        // append rather than replace - the user can delete the seed
        // sine afterwards if they want a pure-captured wavetable. This
        // matches the behavior of the existing per-type "insert new"
        // entries which never delete the current frame.
        if (wave.gridDims.empty()) {
            wave.gridDims.push_back(0);
            wave.cellWaveformIds.clear();
        }
        const int axis = 0;
        const int oldSize = wave.gridDims[axis];
        wave.resizeGridAxis(axis, oldSize + N);

        // Fill the freshly-created slice positions. Each captured waveform
        // is promoted to a fresh library entry; the cell stores the id.
        int firstNewLibId = -1;
        for (int i = 0; i < N; ++i) {
            std::vector<int> coord(wave.gridDims.size(), 0);
            coord[axis] = oldSize + i;
            int flat = wave.gridCoordToCellIdx(coord);
            if (flat >= 0 && flat < (int)wave.cellWaveformIds.size()) {
                const int libId = wave.addLibraryEntry(std::move(frames[(size_t)i]));
                wave.cellWaveformIds[flat] = libId;
                if (i == 0) firstNewLibId = libId;
            }
        }

        // Select the first newly-inserted frame so the user can see it
        // in the editor body.
        {
            std::vector<int> coord(wave.gridDims.size(), 0);
            coord[axis] = oldSize;
            int flat = wave.gridCoordToCellIdx(coord);
            if (flat >= 0) currentFrameIdx = flat;
        }
        // Sync the editor target to match the new selection.
        if (firstNewLibId >= 0) currentLibraryId = firstNewLibId;
        wave.scatterFromGridSnapshot.reset();
        updateHintText();
    } else {
        // Scatter mode: lay the frames out along the X axis (dim 0),
        // equally spaced from 0.1 .. 0.9 so they sit visibly inside the
        // unit cube without being pinned to the corners. Other axes get
        // the center value 0.5.
        const float x0 = 0.1f, x1 = 0.9f;
        int firstNewLibId = -1;
        for (int i = 0; i < N; ++i) {
            ScatterFrame sf;
            sf.waveformId = wave.addLibraryEntry(std::move(frames[(size_t)i]));
            if (i == 0) firstNewLibId = sf.waveformId;
            sf.position.assign(wave.scatterDims, 0.5f);
            if (wave.scatterDims > 0) {
                const float t = (N == 1) ? 0.5f
                                          : (float)i / (float)(N - 1);
                sf.position[0] = x0 + t * (x1 - x0);
            }
            wave.scatterFrames.push_back(std::move(sf));
        }
        currentFrameIdx = (int)wave.scatterFrames.size() - N;
        if (currentFrameIdx < 0) currentFrameIdx = 0;
        if (firstNewLibId >= 0) currentLibraryId = firstNewLibId;
        repaintScatterViews();
    }

    rebuildRows();
    onLayerChanged();
    notifyPopoutFrameOrPositionChanged();
    notifyPopoutDocMutated();
}

void LayeredWaveEditorComponent::notifyScatterViewRotated() {
    if (arrangementView) arrangementView->refreshSliderValues();
}

void LayeredWaveEditorComponent::notifyPopoutFrameOrPositionChanged() {
    if (arrangementView) arrangementView->refreshFrameAndPositionValues();
}

void LayeredWaveEditorComponent::notifyPopoutDocMutated() {
    if (arrangementView) arrangementView->refreshAfterDocMutation();
}

void LayeredWaveEditorComponent::rebuildRows() {
    rows.clear();
    layersContainer.removeAllChildren();

    int y = 0;
    int rh = LayerRow::rowHeight();
    int vw = std::max(layersViewport.getWidth(), 500);
    const auto& layers = currentLayers();
    for (int i = 0; i < (int)layers.size(); ++i) {
        auto row = std::make_unique<LayerRow>(*this, i);
        row->setBounds(0, y, vw, rh);
        row->syncFromModel();
        layersContainer.addAndMakeVisible(row.get());
        rows.push_back(std::move(row));
        y += rh + 4;
    }
    layersContainer.setSize(vw, std::max(y, 10));

    // For spectral / wavelet frames, the layers area becomes the seat for
    // the matching frame editor instead. Doing this after sizing the
    // (empty) layersContainer means we don't show stale rows under the
    // embedded editor for a flicker frame.
    updateFrameEditorEmbed();
}

void LayeredWaveEditorComponent::updateFrameEditorEmbed() {
    // Bind by LIBRARY ENTRY now, not by cell. Editing the entry
    // automatically flows into every cell that references it.
    IWavetableFrame* f = currentEditingFrame();
    const std::string tid = f ? f->typeId() : std::string();

    // Layered frames use the inline LayerRow widgets - tear down any embed
    // and surface the viewport.
    if (tid != "spectral" && tid != "wavelet") {
        if (embeddedFrameEditor) {
            removeChildComponent(embeddedFrameEditor.get());
            embeddedFrameEditor.reset();
            embeddedFrameType.clear();
        }
        layersViewport.setVisible(true);
        return;
    }

    // Non-layered frame. Rebuild the embed whenever it points at a
    // different frame than we want (different type OR same type but a
    // different concrete frame instance, since each sub-editor binds to a
    // specific frame in its ctor). Frame switching isn't a hot path, so
    // unconditionally rebuild on type mismatch is fine.
    if (embeddedFrameEditor) {
        removeChildComponent(embeddedFrameEditor.get());
        embeddedFrameEditor.reset();
        embeddedFrameType.clear();
    }

    auto onSubApply = [this]() { onLayerChanged(); };

    if (tid == "spectral") {
        auto* sf = dynamic_cast<SpectralFrame*>(f);
        if (!sf) { layersViewport.setVisible(true); return; }
        embeddedFrameEditor = std::make_unique<SpectralEditorComponent>(*sf, onSubApply);
    } else if (tid == "wavelet") {
        auto* wf = dynamic_cast<WaveletFrame*>(f);
        if (!wf) { layersViewport.setVisible(true); return; }
        embeddedFrameEditor = std::make_unique<WaveletPainterComponent>(*wf, onSubApply);
    }

    if (!embeddedFrameEditor) {
        layersViewport.setVisible(true);
        return;
    }

    embeddedFrameType = tid;
    addAndMakeVisible(embeddedFrameEditor.get());
    layersViewport.setVisible(false);

    // Re-lay-out so the embed gets the layersViewport's rectangle. resized()
    // checks for the embed and prefers it over the viewport when present.
    resized();
}

void LayeredWaveEditorComponent::refreshPreview() {
    // Render the LIBRARY ENTRY the editor is currently targeting (NOT the
    // selected cell - they can be different now). Every concrete frame type
    // knows how to produce a tableSize-sample cycle.
    if (auto* f = currentEditingFrame()) {
        f->render(wave.tableSize, previewSamples);
    } else {
        previewSamples.clear();
    }
    repaint();
}

void LayeredWaveEditorComponent::commitToNode() {
    if (auto* nd = graph.findNode(nodeId))
        nd->script = wave.encode();
}

void LayeredWaveEditorComponent::onLayerChanged() {
    // Live visual preview every tick; audio rebuild is debounced so we don't
    // race JUCE's AudioProcessorGraph async rebuild (which crashes on rapid
    // concurrent rebuilds).
    refreshPreview();
    commitToNode();
    // (Re)start the debounce: fire 150ms after the last change.
    startTimer(150);
}

void LayeredWaveEditorComponent::timerCallback() {
    stopTimer();
    if (onApply) onApply();
}

void LayeredWaveEditorComponent::resized() {
    auto a = getLocalBounds().reduced(8);

    // Discoverability hint - shown only when the wavetable has just one
    // frame (so people who picked "Waveform / Wavetable" see immediately
    // that this editor is more than a single-waveform editor). Sits
    // ABOVE the toolbar so it reads as a header rather than getting
    // sandwiched between the top buttons and the frame-tabs row.
    if (hintLabel.isVisible()) {
        hintLabel.setBounds(a.removeFromTop(18));
        a.removeFromTop(4);
    }

    // Top button row spans the whole window:
    //   [Render mode: Direct] [Additive bank]              [?] [Apply] [Close]
    // "+ Waveform" used to sit on the left here; it has been moved into
    // the arrangement-view sidebar (next to the Library list) so it lives
    // beside the data structure it mutates. "Render mode" sets the synth
    // node's Synth Mode param, which applies to the WHOLE wavetable (not
    // per-waveform), so it belongs in the global toolbar rather than the
    // right-pane per-waveform editor where it used to sit.
    auto top = a.removeFromTop(28);
    closeBtn.setBounds(top.removeFromRight(60));
    top.removeFromRight(4);
    applyBtn.setBounds(top.removeFromRight(60));
    top.removeFromRight(4);
    helpBtn.setBounds(top.removeFromRight(26));
    top.removeFromRight(12); // separator gap from the right-side cluster

    compareLabel.setVisible(true);
    compareDirectBtn.setVisible(true);
    compareAdditiveBtn.setVisible(true);
    compareLabel.setBounds(top.removeFromLeft(80));
    top.removeFromLeft(4);
    compareDirectBtn.setBounds(top.removeFromLeft(60));
    top.removeFromLeft(2);
    compareAdditiveBtn.setBounds(top.removeFromLeft(100));

    a.removeFromTop(6);

    // (The old "Frame tabs" row that used to sit here has been removed:
    // it listed one tab per library entry, which exactly duplicated the
    // Library list in the arrangement-view sidebar. With the sidebar
    // already always visible, the tab strip was redundant and ate ~32 px
    // of vertical space that the editor body now reclaims.)

    // ---- Body split: LEFT = arrangement view, RIGHT = per-waveform editor.
    // Both are always visible. Right pane is sized to give the editor enough
    // room (~440 px) without starving the arrangement view; on narrow dialogs
    // it falls back to half-and-half.
    const int rightPaneW = juce::jlimit(360, 560, a.getWidth() / 2);
    auto right = a.removeFromRight(rightPaneW);
    a.removeFromRight(8); // gap between the two halves

    if (arrangementView) {
        arrangementView->setVisible(true);
        arrangementView->setBounds(a);
    }

    // ---- Right pane: capture flow OR (editor body + preview) ----
    // While a capture is in progress, the capture component occupies the
    // entire right pane in place of the normal per-frame editor stack.
    // Hide the per-frame widgets so they don't poke through, and skip the
    // rest of the right-pane layout. The Render mode buttons stay visible
    // - they live in the top toolbar now and apply to the whole node.
    if (capturePanel) {
        if (embeddedFrameEditor) embeddedFrameEditor->setVisible(false);
        layersViewport.setVisible(false);
        addLayerBtn.setVisible(false);
        previewBounds = juce::Rectangle<int>();  // suppresses preview paint
        capturePanel->setBounds(right);
        return;
    }

    // Preview strip at the bottom of the right pane. Bounds are stashed for
    // paint() so the geometry stays in one place.
    int previewH = 150;
    previewBounds = right.removeFromBottom(previewH);
    right.removeFromBottom(6);

    // Middle: either the layered-frame layer rows + viewport, or the
    // embedded spectral / wavelet sub-editor. If the editor has no library
    // entry targeted (empty library), hide both - paint() draws a
    // "no waveform" placeholder over the area.
    const bool haveFrame = (currentEditingFrame() != nullptr);
    if (!haveFrame) {
        if (embeddedFrameEditor) embeddedFrameEditor->setVisible(false);
        layersViewport.setVisible(false);
        addLayerBtn.setVisible(false);
    } else if (embeddedFrameEditor) {
        embeddedFrameEditor->setVisible(true);
        addLayerBtn.setVisible(false);
        layersViewport.setVisible(false);
        embeddedFrameEditor->setBounds(right);
    } else {
        // +Layer button sits in a small header strip above the layer rows
        // so it's clearly part of the layered-frame section.
        addLayerBtn.setVisible(true);
        layersViewport.setVisible(true);
        auto layerHeader = right.removeFromTop(24);
        addLayerBtn.setBounds(layerHeader.removeFromLeft(100));
        right.removeFromTop(4);

        layersViewport.setBounds(right);
        int vw = layersViewport.getWidth();
        int rh = LayerRow::rowHeight();
        int y = 0;
        for (auto& row : rows) {
            row->setBounds(0, y, vw, rh);
            y += rh + 4;
        }
        layersContainer.setSize(vw, std::max(y, 10));
    }
}

void LayeredWaveEditorComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(22, 22, 28));

    // Side-by-side layout: the preview strip lives at the bottom of the
    // RIGHT pane. resized() stashes its bounds in previewBounds so we
    // can draw without re-deriving the geometry here.
    if (previewBounds.isEmpty()) return;
    auto previewArea = previewBounds.toFloat();

    g.setColour(juce::Colour(30, 30, 38));
    g.fillRoundedRectangle(previewArea, 4.0f);
    g.setColour(juce::Colour(70, 70, 90));
    g.drawRoundedRectangle(previewArea, 4.0f, 1.0f);

    // If the editor has no library entry targeted (empty library), show a
    // placeholder over the right pane editor area (and skip the curve).
    const bool haveFrame = (currentEditingFrame() != nullptr);
    if (!haveFrame) {
        // Editor body area is everything in the right pane above the preview.
        // We can't easily reconstruct it here without re-running the resized()
        // geometry, but the layersViewport/embed are hidden anyway, so the
        // placeholder just goes in the preview rect.
        g.setColour(juce::Colours::grey.withAlpha(0.75f));
        g.setFont(12.0f);
        g.drawText("No waveform to edit - add one with + Waveform, or "
                   "click an existing waveform in the Library list.",
                   previewArea.reduced(8.0f).toNearestInt(),
                   juce::Justification::centred, true);
        return;
    }

    // Title. We deliberately don't put the current frame index in this
    // label - the frame tabs above already show which frame is selected,
    // and the old "Grid frame N/M" wording read like a frame identifier
    // rather than a section heading for the preview strip. Calling it
    // "Single-cycle waveform" matches what it actually shows (one cycle
    // of the resulting sample at the currently-selected wavetable frame).
    g.setColour(juce::Colours::grey);
    g.setFont(11.0f);
    juce::String title = "Single-cycle waveform"
                         "  -  baked on edit; Position morphs the active blend";
    g.drawText(title,
               previewArea.reduced(6, 2).toNearestInt(),
               juce::Justification::topLeft, true);

    // Reserve a strip at the bottom of the preview for the X-axis labels
    // and duration anchor.  Without this, the curve sat on a 0..1 phase
    // axis with no indication of what time it actually represents - a
    // wavetable synth reads one cycle per played-note period, so one
    // cycle's duration is literally 1/note_freq seconds.  We show the
    // duration at common reference pitches so the user can see "this
    // sub-cycle wiggle = ~0.5 ms".
    const float axisStripH = 28.0f;
    auto curveArea = previewArea.reduced(4.0f, 14.0f);
    curveArea.removeFromBottom(axisStripH - 4.0f);

    // Waveform curve
    if (!previewSamples.empty()) {
        float cx = curveArea.getX();
        float cy = curveArea.getCentreY();
        float w  = curveArea.getWidth();
        float h  = curveArea.getHeight();

        g.setColour(juce::Colours::grey.withAlpha(0.3f));
        g.drawHorizontalLine((int)cy, cx, cx + w);

        juce::Path p;
        int n = (int)previewSamples.size();
        for (int i = 0; i < n; ++i) {
            float x = cx + (float)i / (float)(n - 1) * w;
            float y = cy - previewSamples[i] * h * 0.45f;
            if (i == 0) p.startNewSubPath(x, y);
            else p.lineTo(x, y);
        }
        g.setColour(juce::Colours::cornflowerblue);
        g.strokePath(p, juce::PathStrokeType(1.5f));

        // Phase tick marks at 0, 1/4, 1/2, 3/4, 1.  Anchors the curve to
        // a clear "one full cycle" mental model - without these, the
        // user has no way to gauge how wide a feature is relative to the
        // whole cycle.
        float tickTop    = cy + h * 0.45f + 2.0f;
        float tickBot    = curveArea.getBottom() + 2.0f;
        float labelY     = curveArea.getBottom() + 4.0f;
        float labelH     = 11.0f;
        g.setColour(juce::Colours::grey.withAlpha(0.5f));
        g.setFont(9.5f);
        const char* phaseLabels[] = {"0", "1/4", "1/2", "3/4", "1 cycle"};
        for (int i = 0; i < 5; ++i) {
            float t = (float)i / 4.0f;
            float x = cx + t * w;
            g.drawVerticalLine((int)x, tickTop, tickBot);
            auto justification = (i == 0)              ? juce::Justification::centredLeft
                              : (i == 4)              ? juce::Justification::centredRight
                                                       : juce::Justification::centred;
            juce::Rectangle<float> lbl(x - 30, labelY, 60, labelH);
            if (i == 0)      lbl.setX(x);
            else if (i == 4) lbl.setX(x - 60);
            g.drawText(phaseLabels[i], lbl.toNearestInt(), justification, false);
        }

        // Duration anchor: a wavetable synth's cycle plays at the played
        // note's frequency, so 1 cycle = 1/freq seconds.  We anchor with
        // two common reference pitches so the user can ballpark how
        // wide their features are in real time.  We don't tie this to
        // the actual playing note (which can change per keystroke) -
        // fixed anchors are more legible.
        const float a4Hz = 440.0f;
        const float c4Hz = 261.626f;
        auto fmtMs = [](float ms) {
            return ms >= 10.0f ? juce::String(ms, 1) + " ms"
                              : juce::String(ms, 2) + " ms";
        };
        juce::String anchor = "1 cycle = " + fmtMs(1000.0f / a4Hz)
                            + " at A4 (440 Hz)   |   "
                            + fmtMs(1000.0f / c4Hz)
                            + " at C4 (262 Hz)";
        g.setColour(juce::Colours::grey.withAlpha(0.85f));
        g.setFont(10.0f);
        juce::Rectangle<float> anchorR(
            curveArea.getX(),
            labelY + labelH + 1.0f,
            curveArea.getWidth(),
            12.0f);
        g.drawText(anchor, anchorR.toNearestInt(),
                   juce::Justification::centred, false);
    }
}

} // namespace SoundShop

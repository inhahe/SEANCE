#include "builtin_synth.h"
#include "fft_util.h"
#include "music_theory.h"
#include "waveform_bank.h"
#include "warp.h"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <random>
#include <sstream>
#include <stack>
#include <cctype>
#include <complex>
#include <unordered_map>
#include <string>

namespace SoundShop {

// ==============================================================================
// Wavetable
// ==============================================================================

void Wavetable::initTables() {
    for (int i = 0; i < NUM_MIPMAPS; ++i)
        tables[i].resize(tableSize, 0.0f);
}

void Wavetable::setTableSize(int size) {
    size = juce::jlimit(4, 65536, size);
    if (size == tableSize) return;
    tableSize = size;
    // Regenerate from stored expression if we have one
    if (!lastExpression.empty())
        generateFromExpression(lastExpression);
    else
        generateSine();
}

void Wavetable::generateSine() {
    initTables();
    for (int i = 0; i < tableSize; ++i)
        tables[0][i] = std::sin(2.0f * juce::MathConstants<float>::pi * i / tableSize);
    buildMipmaps();
}

void Wavetable::generateSaw() {
    initTables();
    for (int i = 0; i < tableSize; ++i)
        tables[0][i] = 2.0f * i / tableSize - 1.0f;
    buildMipmaps();
}

void Wavetable::generateSquare() {
    initTables();
    for (int i = 0; i < tableSize; ++i)
        tables[0][i] = (i < tableSize / 2) ? 1.0f : -1.0f;
    buildMipmaps();
}

void Wavetable::generateTriangle() {
    initTables();
    for (int i = 0; i < tableSize; ++i) {
        float t = (float)i / tableSize;
        tables[0][i] = (t < 0.5f) ? (4.0f * t - 1.0f) : (3.0f - 4.0f * t);
    }
    buildMipmaps();
}

void Wavetable::generateFromExpression(const std::string& expr) {
    lastExpression = expr;
    initTables();
    tables[0] = WaveExprParser::evaluate(expr, tableSize);
    buildMipmaps();
}

void Wavetable::generateFromSpectralExpression(const std::string& magExpr,
                                               const std::string& phaseExpr,
                                               int fftSize,
                                               int phaseMode)
{
    // Force fftSize to a power of two within sane limits.
    int n = 2;
    while (n < fftSize && n < 16384) n <<= 1;
    setTableSize(n);
    initTables();

    int halfBins = n / 2 + 1;
    auto mags = WaveExprParser::evaluateOverBins(magExpr, halfBins);

    std::vector<float> phases(halfBins, 0.0f);
    std::mt19937 rng(1337);
    std::uniform_real_distribution<float> udist(-3.14159265f, 3.14159265f);
    switch (phaseMode) {
        case 0: // expression
            phases = WaveExprParser::evaluateOverBins(phaseExpr, halfBins);
            break;
        case 1: // random
            for (auto& p : phases) p = udist(rng);
            break;
        case 2: // zero - impulsive, clicky at t=0
            // already zero
            break;
        case 3: // linear - gives a chirp-like shift
            for (int k = 0; k < halfBins; ++k)
                phases[k] = -2.0f * 3.14159265f * k * 0.5f;
            break;
        default: break;
    }

    // DC and Nyquist bins must be real for a real-valued output.
    // Force their imaginary parts to zero.
    std::vector<std::complex<float>> spectrum(halfBins);
    for (int k = 0; k < halfBins; ++k) {
        float m = mags[k];
        float p = phases[k];
        if (k == 0 || k == halfBins - 1)
            spectrum[k] = std::complex<float>(m, 0.0f);
        else
            spectrum[k] = std::complex<float>(m * std::cos(p), m * std::sin(p));
    }
    // Kill DC - it's almost never wanted and produces a silent offset.
    spectrum[0] = 0.0f;

    FFT fft(n);
    std::vector<float> out;
    fft.inverseReal(spectrum, out);

    // Normalize to peak 1.0
    float peak = 0.0f;
    for (float v : out) peak = std::max(peak, std::abs(v));
    if (peak > 1e-9f) {
        float inv = 1.0f / peak;
        for (float& v : out) v *= inv;
    }

    tables[0] = out;
    buildMipmaps();

    // Remember this so setTableSize can regenerate. We encode the spectral
    // flavor by prefixing; a cleaner approach later would be a dedicated field.
    lastExpression.clear();
}

void Wavetable::generateFromPoints(const std::vector<std::pair<float, float>>& points) {
    initTables();
    if (points.empty()) { generateSine(); return; }

    // Cubic Hermite interpolation between control points
    // Points are (phase 0-1, amplitude -1..1)
    auto pts = points;
    // Ensure periodicity: add wrap-around points
    if (pts.front().first > 0)
        pts.insert(pts.begin(), {pts.back().first - 1.0f, pts.back().second});
    if (pts.back().first < 1.0f)
        pts.push_back({pts.front().first + 1.0f, pts.front().second});

    std::sort(pts.begin(), pts.end());

    for (int i = 0; i < tableSize; ++i) {
        float phase = (float)i / tableSize;

        // Find surrounding points
        int idx = 0;
        for (int j = 0; j < (int)pts.size() - 1; ++j)
            if (pts[j].first <= phase) idx = j;

        int i0 = std::max(0, idx - 1);
        int i1 = idx;
        int i2 = std::min((int)pts.size() - 1, idx + 1);
        int i3 = std::min((int)pts.size() - 1, idx + 2);

        float t = 0;
        if (pts[i2].first != pts[i1].first)
            t = (phase - pts[i1].first) / (pts[i2].first - pts[i1].first);
        t = juce::jlimit(0.0f, 1.0f, t);

        // Catmull-Rom
        float t2 = t * t, t3 = t2 * t;
        float y0 = pts[i0].second, y1 = pts[i1].second;
        float y2 = pts[i2].second, y3 = pts[i3].second;
        tables[0][i] = 0.5f * ((2 * y1) + (-y0 + y2) * t +
            (2 * y0 - 5 * y1 + 4 * y2 - y3) * t2 +
            (-y0 + 3 * y1 - 3 * y2 + y3) * t3);
    }
    buildMipmaps();
}

void Wavetable::setTable(const std::vector<float>& samples) {
    initTables();
    int n = std::min((int)samples.size(), tableSize);
    for (int i = 0; i < n; ++i)
        tables[0][i] = samples[i];
    // Stretch or pad if needed
    if (n < tableSize) {
        for (int i = n; i < tableSize; ++i)
            tables[0][i] = 0.0f;
    }
    buildMipmaps();
}

void Wavetable::buildMipmaps() {
    // Build progressively band-limited versions using simple averaging
    // Each mipmap halves the bandwidth
    for (int level = 1; level < NUM_MIPMAPS; ++level) {
        tables[level].resize(tableSize);
        auto& prev = tables[level - 1];
        auto& cur = tables[level];

        // Low-pass by averaging adjacent samples (simple box filter, repeated)
        cur = prev;
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<float> tmp(tableSize);
            for (int i = 0; i < tableSize; ++i) {
                int prev_i = (i - 1 + tableSize) % tableSize;
                int next_i = (i + 1) % tableSize;
                tmp[i] = (cur[prev_i] + cur[i] * 2.0f + cur[next_i]) * 0.25f;
            }
            cur = tmp;
        }
    }
}

float Wavetable::sample(float phase, float frequencyHz, float sr) const {
    if (tables[0].empty()) return 0.0f;

    // Select mipmap level based on frequency
    // At Nyquist/2, we want the most filtered version
    float samplesPerCycle = (float)(sr / std::max(1.0f, frequencyHz));
    int level = 0;
    float threshold = tableSize;
    while (level < NUM_MIPMAPS - 1 && samplesPerCycle < threshold) {
        level++;
        threshold *= 0.5f;
    }

    // Linear interpolation within the selected mipmap
    float pos = phase * tableSize;
    pos = pos - std::floor(pos / tableSize) * tableSize; // wrap
    int idx = (int)pos;
    float frac = pos - idx;
    idx = idx % tableSize;
    int next = (idx + 1) % tableSize;

    auto& tbl = tables[level];
    return tbl[idx] + frac * (tbl[next] - tbl[idx]);
}

// ==============================================================================
// WaveExprParser - expression evaluator
// ==============================================================================
//
// Recursive-descent parser. Grammar (low to high precedence):
//
//   ternary   = or        ( '?' ternary ':' ternary )?      (right-assoc)
//   or        = and       ( '||' and )*
//   and       = compare   ( '&&' compare )*
//   compare   = expr      ( ('<'|'>'|'<='|'>='|'=='|'!=') expr )?
//   expr      = muldiv    ( ('+'|'-') muldiv )*
//   muldiv    = pow       ( ('*'|'/') pow )*
//   pow       = unary     ( '^' pow )?                       (right-assoc)
//   unary     = ('!'|'-') unary | atom
//   atom      = '(' ternary ')' | function | identifier | number
//
// Built-in identifiers:   x, f, pi, e
// Custom identifiers:     looked up in `extraVars` (caller-supplied map)
// Built-in functions:     sin, cos, tan, abs, sqrt, exp, log, pow(a,b),
//                         saw, square, triangle, noise, tanh, clamp(v,lo,hi),
//                         floor, ceil, min(a,b), max(a,b), if(c,a,b)
// Built-in zero-arg:      random  (uniform [-pi, pi])
//
// Notes:
// - Comparisons and booleans return 1.0f for true, 0.0f for false.
// - `&&` / `||` are NOT short-circuit (both sides evaluate). Same for the
//   ternary's true and false branches and if()'s second / third arg. This
//   keeps the evaluator simple (no AST yet); side-effecting calls like
//   noise() and random execute in both branches.
// - Unknown identifiers (not in extraVars, not a built-in) evaluate to 0.

namespace {
struct ExprParser {
    const char* str = nullptr;
    const char* pos = nullptr;
    float x = 0.0f;
    float f = 0.0f;
    std::mt19937 rng{42};
    const std::unordered_map<std::string, float>* extraVars = nullptr;
    // Optional sampler for the shape(pos) function. Null/empty -> shape()
    // returns 0. Set by the evaluateWithVars overload that takes a sampler.
    const std::function<float(float)>* shapeFn = nullptr;
    // Optional note->frequency mapper for notefreq(). Null/empty -> 12-TET A440
    // fallback. Set by the runProgram / evaluateWithVars overloads that take it.
    const std::function<float(int)>* noteToFreqFn = nullptr;
    // Program-mode extras (see runProgram). stateVars is the persistent,
    // assignable variable store; sink receives MIDI emit calls. Both null
    // for plain expression evaluation, in which case assignments are
    // discarded and emit functions are no-ops.
    std::unordered_map<std::string, float>* stateVars = nullptr;
    IExprEmitSink* sink = nullptr;

    // The MIDI output index that emit calls route to. Read from the
    // reserved `out` variable (stateVars first, then extraVars), default 0.
    int currentOut() const {
        if (stateVars) {
            auto it = stateVars->find("out");
            if (it != stateVars->end()) return std::max(0, (int)std::lround(it->second));
        }
        if (extraVars) {
            auto it = extraVars->find("out");
            if (it != extraVars->end()) return std::max(0, (int)std::lround(it->second));
        }
        return 0;
    }

    void skipWS() { while (*pos == ' ' || *pos == '\t') pos++; }

    static bool isIdentStart(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }
    static bool isIdentCont(char c) {
        return isIdentStart(c) || (c >= '0' && c <= '9');
    }

    float parseNumber() {
        skipWS();
        const char* start = pos;
        // Unary sign is handled by parseUnary; parseNumber accepts only digits/dot.
        while (std::isdigit((unsigned char)*pos) || *pos == '.') pos++;
        if (pos == start) return 0;
        return std::strtof(start, nullptr);
    }

    // Read an identifier (alphanumeric + underscore, starts with letter/_).
    // Returns empty string if no identifier at current position; advances pos
    // past the identifier on success.
    std::string parseIdentifier() {
        skipWS();
        if (!isIdentStart(*pos)) return {};
        const char* start = pos;
        while (isIdentCont(*pos)) pos++;
        return std::string(start, pos - start);
    }

    // Helper: match a function name followed by '('. If matched, advance past
    // the '(' and return true. Otherwise rewind and return false.
    bool matchFunc(const char* name) {
        skipWS();
        size_t n = std::strlen(name);
        if (std::strncmp(pos, name, n) != 0) return false;
        // Must be followed by '(' with no intervening identifier chars
        if (isIdentCont(pos[n])) return false;
        const char* save = pos;
        pos += n;
        skipWS();
        if (*pos != '(') { pos = save; return false; }
        pos++; // consume '('
        return true;
    }

    // Parse the first argument of a warp*() call: either a quoted method NAME
    // literal ("softclip", "bend+", ...) resolved via warpMethodFromName, or a
    // numeric WarpMethod id. Leaves pos just after the argument (before the
    // comma). Mirrors the waveform() id/name parsing.
    WarpMethod parseWarpMethodArg() {
        skipWS();
        if (*pos == '"' || *pos == '\'') {
            char quote = *pos++;
            const char* start = pos;
            while (*pos != '\0' && *pos != quote) pos++;
            std::string nm(start, pos - start);
            if (*pos == quote) pos++;            // consume closing quote
            return warpMethodFromName(nm.c_str());
        }
        return (WarpMethod)(int)std::lround(parseTernary());
    }

    // Match a bare keyword (no trailing '('): identifier that exactly equals
    // `name` and is followed by a non-identifier character. Advances pos on
    // success.
    bool matchKeyword(const char* name) {
        skipWS();
        size_t n = std::strlen(name);
        if (std::strncmp(pos, name, n) != 0) return false;
        if (isIdentCont(pos[n])) return false;
        pos += n;
        return true;
    }

    // Match a punctuation operator like "<=" or "&&". Advances pos on match.
    bool matchOp(const char* op) {
        size_t n = std::strlen(op);
        if (std::strncmp(pos, op, n) == 0) { pos += n; return true; }
        return false;
    }

    float parseAtom() {
        skipWS();
        if (*pos == '(') {
            pos++;
            float val = parseTernary();
            skipWS();
            if (*pos == ')') pos++;
            return val;
        }

        // String literal -> MIDI note number. A quoted note name like "C4",
        // "c#4" or "Bb3" evaluates to its MIDI pitch, so a user can write
        // note("C4", 100, 0.5) anywhere a pitch number is accepted. Accepts
        // single or double quotes. A name with no octave uses octave 4. A
        // parse failure yields -1 (out of range -> clamped/dropped downstream).
        if (*pos == '"' || *pos == '\'') {
            char quote = *pos++;
            const char* start = pos;
            while (*pos != '\0' && *pos != quote) pos++;
            std::string lit(start, pos - start);
            if (*pos == quote) pos++; // consume closing quote
            return (float)MusicTheory::parseNoteName(lit);
        }

        // Single-arg math functions
        if (matchFunc("sin"))      { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::sin(v); }
        if (matchFunc("cos"))      { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::cos(v); }
        if (matchFunc("tan"))      { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::tan(v); }
        if (matchFunc("abs"))      { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::abs(v); }
        if (matchFunc("sqrt"))     { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::sqrt(std::abs(v)); }
        if (matchFunc("exp"))      { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::exp(v); }
        if (matchFunc("log"))      { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return v > 0 ? std::log(v) : 0.0f; }
        if (matchFunc("tanh"))     { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::tanh(v); }
        if (matchFunc("floor"))    { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::floor(v); }
        if (matchFunc("ceil"))     { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::ceil(v); }

        // GLSL-parity single-arg scalar math (added so the Builtin language shares
        // a vocabulary with the GLSL shape/terrain dialect). All pure and
        // allocation-free, so they're real-time safe in the Script node too.
        if (matchFunc("asin"))        { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::asin(std::clamp(v, -1.0f, 1.0f)); }
        if (matchFunc("acos"))        { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::acos(std::clamp(v, -1.0f, 1.0f)); }
        if (matchFunc("sinh"))        { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::sinh(v); }
        if (matchFunc("cosh"))        { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::cosh(v); }
        if (matchFunc("sign"))        { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return float((v > 0.0f) - (v < 0.0f)); }
        if (matchFunc("fract"))       { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return v - std::floor(v); }
        if (matchFunc("round"))       { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::round(v); }
        if (matchFunc("trunc"))       { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::trunc(v); }
        if (matchFunc("radians"))     { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return v * 0.01745329252f; }
        if (matchFunc("degrees"))     { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return v * 57.2957795131f; }
        if (matchFunc("inversesqrt")) { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return v > 0.0f ? 1.0f / std::sqrt(v) : 0.0f; }
        if (matchFunc("exp2"))        { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::exp2(v); }
        if (matchFunc("log2"))        { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return v > 0.0f ? std::log2(v) : 0.0f; }
        if (matchFunc("asinh"))       { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::asinh(v); }
        if (matchFunc("acosh"))       { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::acosh(std::max(1.0f, v)); }       // domain [1,inf)
        if (matchFunc("atanh"))       { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::atanh(std::clamp(v, -0.999999f, 0.999999f)); } // domain (-1,1)
        if (matchFunc("roundEven"))   { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::nearbyint(v); } // round half to even
        // atan is overloaded: atan(y) or atan(y, x) (=atan2), matching GLSL.
        if (matchFunc("atan")) {
            float y = parseTernary(); skipWS();
            if (*pos == ',') { pos++; float xx = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::atan2(y, xx); }
            if (*pos == ')') pos++;
            return std::atan(y);
        }

        // Range converters and unipolar trig aliases. Control signals on the
        // wire are unipolar 0..1, but sin/cos/tan (and saw/square/triangle/
        // noise) return bipolar -1..1, so a bare sin() clips its negative half
        // on the wire. unipolar(x) maps -1..1 -> 0..1 (wrap a whole bipolar
        // expression in it); bipolar(x) maps 0..1 -> -1..1. usin/ucos/utan are
        // shorthands for unipolar(sin(x)) etc.
        if (matchFunc("unipolar")) { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return v * 0.5f + 0.5f; }
        if (matchFunc("bipolar"))  { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return v * 2.0f - 1.0f; }
        if (matchFunc("usin"))     { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::sin(v) * 0.5f + 0.5f; }
        if (matchFunc("ucos"))     { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::cos(v) * 0.5f + 0.5f; }
        if (matchFunc("utan"))     { float v = parseTernary(); skipWS(); if (*pos == ')') pos++; return std::tan(v) * 0.5f + 0.5f; }

        // Shape-helper functions (wrap to 2-pi domain like the time-domain x)
        if (matchFunc("saw")) {
            float v = parseTernary(); skipWS(); if (*pos == ')') pos++;
            float t = v / (2.0f * 3.14159265f);
            return 2.0f * (t - std::floor(t + 0.5f));
        }
        if (matchFunc("square")) {
            float v = parseTernary(); skipWS(); if (*pos == ')') pos++;
            return std::sin(v) >= 0 ? 1.0f : -1.0f;
        }
        if (matchFunc("triangle")) {
            float v = parseTernary(); skipWS(); if (*pos == ')') pos++;
            float t = v / (2.0f * 3.14159265f);
            return 4.0f * std::abs(t - std::floor(t + 0.5f)) - 1.0f;
        }

        // Unipolar (0..1) shape helpers - same waves mapped onto the wire range.
        if (matchFunc("usaw")) {
            float v = parseTernary(); skipWS(); if (*pos == ')') pos++;
            float t = v / (2.0f * 3.14159265f);
            return (2.0f * (t - std::floor(t + 0.5f))) * 0.5f + 0.5f;
        }
        if (matchFunc("usquare")) {
            float v = parseTernary(); skipWS(); if (*pos == ')') pos++;
            return std::sin(v) >= 0 ? 1.0f : 0.0f;
        }
        if (matchFunc("utriangle")) {
            float v = parseTernary(); skipWS(); if (*pos == ')') pos++;
            float t = v / (2.0f * 3.14159265f);
            return (4.0f * std::abs(t - std::floor(t + 0.5f)) - 1.0f) * 0.5f + 0.5f;
        }

        // shape(pos) — sample the caller-supplied shape table at `pos`.
        // The sampler decides the domain (SignalShape wraps pos as a 0..1
        // phase). Without a sampler bound, evaluates to 0.
        if (matchFunc("shape")) {
            float p = parseTernary(); skipWS(); if (*pos == ')') pos++;
            return (shapeFn && *shapeFn) ? (*shapeFn)(p) : 0.0f;
        }

        // waveform("name", phase) — read one of the ~4000 built-in factory
        // single-cycle waveforms at a normalised phase in [0,1) (wraps), linearly
        // interpolated, returning the raw sample in [-1,1]. The first argument is
        // a quoted waveform NAME (resolved against the WaveformBank by name, NOT
        // a note-name pitch like the bare string literal would be), or a numeric
        // entry index for power users. An unknown name / out-of-range index reads
        // as silence (0). The name lookup is case-insensitive. This is the same
        // waveform() helper every generator language exposes, so a Builtin terrain
        // and a Lua/Python/GLSL one read the bank identically.
        if (matchFunc("waveform")) {
            int id = -1;
            skipWS();
            if (*pos == '"' || *pos == '\'') {
                char quote = *pos++;
                const char* start = pos;
                while (*pos != '\0' && *pos != quote) pos++;
                std::string nm(start, pos - start);
                if (*pos == quote) pos++;          // consume closing quote
                auto& bank = WaveformBank::get();
                bank.ensureLoaded();
                id = bank.indexForName(nm);
            } else {
                id = (int)std::lround(parseTernary());  // numeric entry index
            }
            skipWS(); if (*pos == ',') pos++;
            float ph = parseTernary();
            skipWS(); if (*pos == ')') pos++;
            return WaveformBank::get().sampleAtPhase(id, ph);
        }

        // warpamp(method, x, amount) / warpphase(method, phase, amount) — the
        // wavetable shape-bending ("warp") algorithms exposed as pure scalar
        // functions, routed to the SAME warpAmpValue/warpPhaseValue primitives the
        // Layered Wave editor and synth voice use (so an in-formula clip/fold/bend
        // matches the node bit-for-bit). The first arg is a method NAME literal
        // ("softclip", "wavefold", "bend+", ...) or a numeric WarpMethod id;
        // `amount` is the 0..1 morph knob (0 = identity). amp shapes a sample value
        // in [-1,1]; phase remaps a read position in [0,1).
        if (matchFunc("warpamp")) {
            WarpMethod m = parseWarpMethodArg();
            skipWS(); if (*pos == ',') pos++;
            float xx = parseTernary();
            skipWS(); if (*pos == ',') pos++;
            float amt = parseTernary();
            skipWS(); if (*pos == ')') pos++;
            return warpAmpValue(m, xx, amt);
        }
        if (matchFunc("warpphase")) {
            WarpMethod m = parseWarpMethodArg();
            skipWS(); if (*pos == ',') pos++;
            float ph = parseTernary();
            skipWS(); if (*pos == ',') pos++;
            float amt = parseTernary();
            skipWS(); if (*pos == ')') pos++;
            return warpPhaseValue(m, ph, amt);
        }

        // notefreq(note) — frequency in Hz of a MIDI note, using the PROJECT
        // tuning system (Equal12 / Pythagorean / Just / Meantone + concert
        // pitch) when a mapper is bound, else 12-TET A440. The argument may be a
        // number or a quoted note name (the string literal converts to its
        // number at the atom level), so notefreq("C4") and notefreq(60) match.
        if (matchFunc("notefreq")) {
            float n = parseTernary(); skipWS(); if (*pos == ')') pos++;
            int note = (int)std::lround(n);
            if (note < 0 || note > 127) return 0.0f;
            if (noteToFreqFn && *noteToFreqFn) return (*noteToFreqFn)(note);
            return 440.0f * std::pow(2.0f, (note - 69) / 12.0f);
        }
        // notenum(x) — identity for numbers; for a quoted note name the string
        // literal already converted to its MIDI number at the atom level, so
        // this is mainly a readable way to spell an explicit conversion.
        if (matchFunc("notenum")) {
            float v = parseTernary(); skipWS(); if (*pos == ')') pos++;
            return v;
        }

        // noise() — ignores any argument, returns uniform [-1, 1]
        if (matchFunc("noise")) {
            parseTernary(); skipWS(); if (*pos == ')') pos++;
            std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
            return dist(rng);
        }
        // Unipolar noise: uniform 0..1, ready to drive a control wire directly.
        if (matchFunc("unoise")) {
            parseTernary(); skipWS(); if (*pos == ')') pos++;
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);
            return dist(rng);
        }

        // Two-arg functions
        if (matchFunc("pow")) {
            float base = parseTernary();
            skipWS(); if (*pos == ',') pos++;
            float exp_ = parseTernary();
            skipWS(); if (*pos == ')') pos++;
            return std::pow(base, exp_);
        }
        if (matchFunc("min")) {
            float a = parseTernary();
            skipWS(); if (*pos == ',') pos++;
            float b = parseTernary();
            skipWS(); if (*pos == ')') pos++;
            return std::min(a, b);
        }
        if (matchFunc("max")) {
            float a = parseTernary();
            skipWS(); if (*pos == ',') pos++;
            float b = parseTernary();
            skipWS(); if (*pos == ')') pos++;
            return std::max(a, b);
        }

        // Three-arg functions
        if (matchFunc("clamp")) {
            float v = parseTernary();
            skipWS(); if (*pos == ',') pos++;
            float lo = parseTernary();
            skipWS(); if (*pos == ',') pos++;
            float hi = parseTernary();
            skipWS(); if (*pos == ')') pos++;
            return std::clamp(v, lo, hi);
        }

        // GLSL-parity multi-arg scalar math (mod/step/mix/smoothstep), same
        // semantics as the GLSL dialect. Pure and allocation-free.
        if (matchFunc("mod")) {                // mod(a, b) = a - b*floor(a/b)
            float a = parseTernary(); skipWS(); if (*pos == ',') pos++;
            float b = parseTernary(); skipWS(); if (*pos == ')') pos++;
            return b != 0.0f ? a - b * std::floor(a / b) : 0.0f;
        }
        if (matchFunc("step")) {               // step(edge, x) = x < edge ? 0 : 1
            float edge = parseTernary(); skipWS(); if (*pos == ',') pos++;
            float xx = parseTernary(); skipWS(); if (*pos == ')') pos++;
            return xx < edge ? 0.0f : 1.0f;
        }
        if (matchFunc("mix")) {                // mix(a, b, t) = a + (b-a)*t (lerp)
            float a = parseTernary(); skipWS(); if (*pos == ',') pos++;
            float b = parseTernary(); skipWS(); if (*pos == ',') pos++;
            float t = parseTernary(); skipWS(); if (*pos == ')') pos++;
            return a + (b - a) * t;
        }
        if (matchFunc("smoothstep")) {         // smoothstep(e0, e1, x)
            float e0 = parseTernary(); skipWS(); if (*pos == ',') pos++;
            float e1 = parseTernary(); skipWS(); if (*pos == ',') pos++;
            float xx = parseTernary(); skipWS(); if (*pos == ')') pos++;
            float t = (e1 != e0) ? std::clamp((xx - e0) / (e1 - e0), 0.0f, 1.0f) : 0.0f;
            return t * t * (3.0f - 2.0f * t);
        }
        if (matchFunc("fma")) {                // fma(a, b, c) = a*b + c
            float a = parseTernary(); skipWS(); if (*pos == ',') pos++;
            float b = parseTernary(); skipWS(); if (*pos == ',') pos++;
            float c = parseTernary(); skipWS(); if (*pos == ')') pos++;
            return std::fma(a, b, c);
        }
        if (matchFunc("if")) {
            float cond = parseTernary();
            skipWS(); if (*pos == ',') pos++;
            float thenV = parseTernary();
            skipWS(); if (*pos == ',') pos++;
            float elseV = parseTernary();
            skipWS(); if (*pos == ')') pos++;
            return cond != 0.0f ? thenV : elseV;
        }

        // -------------------------------------------------------------------
        // Program-mode MIDI emit functions. These have a SIDE EFFECT (push a
        // MIDI event into `sink`) and return 1.0 (so they can be used in a
        // ternary, e.g. `(beat<0.01) ? note(60,100,0.5) : 0`). With no sink
        // bound they parse their args (so the program still type-checks) and
        // return 0. The destination output is read from the `out` variable.
        // -------------------------------------------------------------------
        if (matchFunc("note")) {           // note(pitch, vel, durSec)
            float p = parseTernary(); skipWS(); if (*pos == ',') pos++;
            float v = parseTernary(); skipWS(); if (*pos == ',') pos++;
            float d = parseTernary(); skipWS(); if (*pos == ')') pos++;
            if (sink) { sink->emitNote(currentOut(), p, v, d); return 1.0f; }
            return 0.0f;
        }
        if (matchFunc("noteon")) {         // noteon(pitch, vel)
            float p = parseTernary(); skipWS(); if (*pos == ',') pos++;
            float v = parseTernary(); skipWS(); if (*pos == ')') pos++;
            if (sink) { sink->emitNoteOn(currentOut(), p, v); return 1.0f; }
            return 0.0f;
        }
        if (matchFunc("noteoff")) {        // noteoff(pitch)
            float p = parseTernary(); skipWS(); if (*pos == ')') pos++;
            if (sink) { sink->emitNoteOff(currentOut(), p); return 1.0f; }
            return 0.0f;
        }
        if (matchFunc("cc")) {             // cc(number, value 0..1)
            float n = parseTernary(); skipWS(); if (*pos == ',') pos++;
            float v = parseTernary(); skipWS(); if (*pos == ')') pos++;
            if (sink) { sink->emitCC(currentOut(), n, v); return 1.0f; }
            return 0.0f;
        }
        if (matchFunc("bend")) {           // bend(value -1..1)
            float v = parseTernary(); skipWS(); if (*pos == ')') pos++;
            if (sink) { sink->emitBend(currentOut(), v); return 1.0f; }
            return 0.0f;
        }

        // Bare keywords (no parens)
        if (matchKeyword("random")) {
            std::uniform_real_distribution<float> dist(-3.14159265f, 3.14159265f);
            return dist(rng);
        }
        if (matchKeyword("pi")) return 3.14159265f;
        // `e` is a built-in identifier; if the user wants Euler's e they can
        // use `e`, but anything starting with `e` like `exp` was already
        // matched above as a function. We check single-letter `e` after all
        // multi-char tokens, via matchKeyword.

        // Identifier: built-in single-letter (x, f, e) or custom variable.
        skipWS();
        if (isIdentStart(*pos)) {
            const char* save = pos;
            std::string ident = parseIdentifier();
            // Built-in single-letter vars
            if (ident == "x") return x;
            if (ident == "f") return f;
            if (ident == "e") return 2.71828183f;
            // Custom variable lookup. Read-only per-call vars take precedence
            // over the persistent state store, so a transport/input binding
            // can't be shadowed by a stale state value of the same name.
            if (extraVars) {
                auto it = extraVars->find(ident);
                if (it != extraVars->end()) return it->second;
            }
            if (stateVars) {
                auto it = stateVars->find(ident);
                if (it != stateVars->end()) return it->second;
            }
            // Unknown identifier: rewind and let parseNumber try (which will
            // return 0 since no digits). This means unknown idents silently
            // evaluate to 0 rather than parse-error - matches the old
            // parser's "fall through to parseNumber returning 0" behavior.
            pos = save;
            // Consume the identifier so we don't loop forever
            while (isIdentCont(*pos)) pos++;
            return 0.0f;
        }

        return parseNumber();
    }

    float parseUnary() {
        skipWS();
        if (*pos == '!') { pos++; float v = parseUnary(); return (v == 0.0f) ? 1.0f : 0.0f; }
        if (*pos == '-') { pos++; return -parseUnary(); }
        if (*pos == '+') { pos++; return parseUnary(); }
        return parseAtom();
    }

    float parsePow() {
        float val = parseUnary();
        skipWS();
        if (*pos == '^') { pos++; val = std::pow(val, parsePow()); }
        return val;
    }

    float parseMulDiv() {
        float val = parsePow();
        while (true) {
            skipWS();
            if (*pos == '*') { pos++; val *= parsePow(); }
            else if (*pos == '/') { pos++; float d = parsePow(); val = d != 0 ? val / d : 0; }
            else break;
        }
        return val;
    }

    float parseExpr() {
        float val = parseMulDiv();
        while (true) {
            skipWS();
            if (*pos == '+') { pos++; val += parseMulDiv(); }
            else if (*pos == '-') { pos++; val -= parseMulDiv(); }
            else break;
        }
        return val;
    }

    float parseCompare() {
        float lhs = parseExpr();
        skipWS();
        // Order matters: try two-char operators before single-char so '<='
        // doesn't get partially matched as '<' first.
        if (matchOp("<=")) { float r = parseExpr(); return (lhs <= r) ? 1.0f : 0.0f; }
        if (matchOp(">=")) { float r = parseExpr(); return (lhs >= r) ? 1.0f : 0.0f; }
        if (matchOp("==")) { float r = parseExpr(); return (lhs == r) ? 1.0f : 0.0f; }
        if (matchOp("!=")) { float r = parseExpr(); return (lhs != r) ? 1.0f : 0.0f; }
        if (matchOp("<"))  { float r = parseExpr(); return (lhs <  r) ? 1.0f : 0.0f; }
        if (matchOp(">"))  { float r = parseExpr(); return (lhs >  r) ? 1.0f : 0.0f; }
        return lhs;
    }

    float parseAnd() {
        float val = parseCompare();
        while (true) {
            skipWS();
            if (matchOp("&&")) {
                float rhs = parseCompare();
                val = (val != 0.0f && rhs != 0.0f) ? 1.0f : 0.0f;
            } else break;
        }
        return val;
    }

    float parseOr() {
        float val = parseAnd();
        while (true) {
            skipWS();
            if (matchOp("||")) {
                float rhs = parseAnd();
                val = (val != 0.0f || rhs != 0.0f) ? 1.0f : 0.0f;
            } else break;
        }
        return val;
    }

    float parseTernary() {
        float cond = parseOr();
        skipWS();
        if (*pos == '?') {
            pos++;
            float thenV = parseTernary();
            skipWS();
            if (*pos == ':') pos++;
            float elseV = parseTernary();
            return cond != 0.0f ? thenV : elseV;
        }
        return cond;
    }

    float eval(float xVal, float fVal) {
        x = xVal;
        f = fVal;
        pos = str;
        return parseTernary();
    }

    // ---- Program mode -----------------------------------------------------
    // Skip whitespace AND statement separators (';' and newlines) between
    // statements. Newlines aren't skipped by skipWS (which only eats spaces /
    // tabs) precisely so a newline terminates a statement; here we step over
    // the run of separators that sit between two statements.
    void skipSeparators() {
        while (*pos == ' ' || *pos == '\t' || *pos == '\r'
               || *pos == '\n' || *pos == ';')
            pos++;
    }

    // Parse one statement: either `ident = expr` (assignment into stateVars,
    // persistent) or a bare expression evaluated for side effects. Returns
    // the statement's value.
    float parseStatement() {
        skipWS();
        // Look-ahead for an assignment: an identifier immediately followed by
        // a single '=' that is NOT part of '==' (equality). On no match we
        // rewind and treat the whole thing as a bare expression.
        const char* save = pos;
        std::string ident = parseIdentifier();
        if (!ident.empty()) {
            skipWS();
            if (*pos == '=' && pos[1] != '=') {
                pos++; // consume '='
                float v = parseTernary();
                if (stateVars) (*stateVars)[ident] = v;
                return v;
            }
            pos = save; // not an assignment after all
        }
        return parseTernary();
    }

    // Run the whole program. Statements separated by ';' / newlines. Returns
    // the value of the last statement (0 for an empty program).
    float runProgram() {
        pos = str;
        // Seed the reserved `x`/`f` free variables from the bound vars if the
        // caller supplied them (shape/curve bakes pass the sweep position as
        // `x`, so `sin(x)` works in a multi-statement program exactly as it
        // does in a single expression). MIDI programs that don't bind `x`/`f`
        // get the historical default of 0.
        auto seed = [this](const char* name, float def) -> float {
            if (extraVars) { auto it = extraVars->find(name); if (it != extraVars->end()) return it->second; }
            if (stateVars) { auto it = stateVars->find(name); if (it != stateVars->end()) return it->second; }
            return def;
        };
        x = seed("x", 0.0f);
        f = seed("f", 0.0f);
        float last = 0.0f;
        skipSeparators();
        while (*pos != '\0') {
            const char* before = pos;
            last = parseStatement();
            skipSeparators();
            // Safety: a token the grammar can't consume (e.g. a stray '#' or
            // '@') leaves pos un-advanced; without this guard the loop would
            // spin forever on the audio thread. Step over it so the program
            // terminates. WaveExprParser::validate() reports such characters
            // up front, but this keeps the evaluator hang-proof regardless.
            if (pos == before) ++pos;
        }
        return last;
    }
};
} // anonymous namespace

std::vector<float> WaveExprParser::evaluate(const std::string& expr, int tableSize) {
    std::vector<float> result(tableSize);
    ExprParser parser;
    parser.str = expr.c_str();

    for (int i = 0; i < tableSize; ++i) {
        float xVal = 2.0f * 3.14159265f * i / tableSize; // 0 to 2*pi
        result[i] = juce::jlimit(-1.0f, 1.0f, parser.eval(xVal, 0.0f));
    }
    return result;
}

std::vector<float> WaveExprParser::evaluateOverBins(const std::string& expr, int numBins) {
    std::vector<float> result(numBins);
    if (expr.empty()) return result; // all zeros
    ExprParser parser;
    parser.str = expr.c_str();
    // `f` is the integer index over [0, numBins) (frequency bin for spectral
    // curves). `x` is the same position normalized to [0, 1] - this is what
    // non-periodic [0,1] curves (e.g. ADSR segment shapes "x", "1-x") are
    // written in terms of. Binding both lets one evaluator serve both the
    // spectral (bin-index) and normalized-position curve domains.
    for (int i = 0; i < numBins; ++i) {
        float xn = (numBins > 1) ? (float)i / (float)(numBins - 1) : 0.0f;
        result[i] = parser.eval(xn, (float)i);
    }
    return result;
}

float WaveExprParser::evaluateAt(const std::string& expr, float x, float f) {
    if (expr.empty()) return 0.0f;
    ExprParser parser;
    parser.str = expr.c_str();
    return parser.eval(x, f);
}

float WaveExprParser::evaluateWithVars(const std::string& expr,
                                       const std::unordered_map<std::string, float>& vars) {
    if (expr.empty()) return 0.0f;
    ExprParser parser;
    parser.str = expr.c_str();
    parser.extraVars = &vars;
    // x and f default to 0 — the caller usually passes phase via vars["x"]
    // or by including "x" in the map. For SignalShape we use phase as a
    // var named "x" (mapped to 0..2pi) so the existing shape expressions
    // like sin(x) keep working.
    auto itX = vars.find("x"); if (itX != vars.end()) parser.x = itX->second;
    auto itF = vars.find("f"); if (itF != vars.end()) parser.f = itF->second;
    return parser.eval(parser.x, parser.f);
}

float WaveExprParser::evaluateWithVars(const std::string& expr,
                                       const std::unordered_map<std::string, float>& vars,
                                       const std::function<float(float)>& shapeFn,
                                       const std::function<float(int)>& noteToFreqFn) {
    if (expr.empty()) return 0.0f;
    ExprParser parser;
    parser.str = expr.c_str();
    parser.extraVars = &vars;
    parser.shapeFn = &shapeFn;
    parser.noteToFreqFn = &noteToFreqFn;
    auto itX = vars.find("x"); if (itX != vars.end()) parser.x = itX->second;
    auto itF = vars.find("f"); if (itF != vars.end()) parser.f = itF->second;
    return parser.eval(parser.x, parser.f);
}

float WaveExprParser::runProgram(const std::string& program,
                                 const std::unordered_map<std::string, float>& vars,
                                 std::unordered_map<std::string, float>& stateVars,
                                 IExprEmitSink* sink,
                                 const std::function<float(float)>& shapeFn,
                                 const std::function<float(int)>& noteToFreqFn) {
    if (program.empty()) return 0.0f;
    ExprParser parser;
    parser.str          = program.c_str();
    parser.extraVars    = &vars;
    parser.stateVars    = &stateVars;
    parser.sink         = sink;
    parser.shapeFn      = &shapeFn;
    parser.noteToFreqFn = &noteToFreqFn;
    return parser.runProgram();
}

bool WaveExprParser::validate(const std::string& program, std::string& error) {
    error.clear();

    // Characters that are legal *outside* of a quoted literal. Everything the
    // grammar can consume: identifiers (letters/digits/_), numbers ('.'),
    // operators and punctuation. Parens are handled separately (balance), and
    // any char is allowed inside "..."/'...'. Anything not here is out of the
    // language entirely.
    auto isAllowedBare = [](unsigned char c) -> bool {
        if (std::isalnum(c)) return true;
        switch (c) {
            case '_': case '.':                       // identifiers / numbers
            case ',': case ';':                       // arg / statement separators
            case '?': case ':':                       // ternary, section headers
            case '<': case '>': case '=': case '!':   // comparisons
            case '&': case '|':                       // boolean
            case '+': case '-': case '*': case '/': case '^': // arithmetic
            case ' ': case '\t': case '\r': case '\n':         // whitespace
                return true;
            default:
                return false;
        }
    };

    int  parenDepth = 0;
    int  line       = 1;
    bool inQuote    = false;
    char quoteChar  = 0;

    for (size_t i = 0; i < program.size(); ++i) {
        char c = program[i];
        if (inQuote) {
            if (c == '\n') ++line;
            else if (c == quoteChar) inQuote = false;
            continue;
        }
        switch (c) {
            case '\n': ++line; break;
            case '"':
            case '\'': inQuote = true; quoteChar = c; break;
            case '(':  ++parenDepth; break;
            case ')':
                if (--parenDepth < 0) {
                    error = "Unexpected ')' with no matching '(' (line "
                          + std::to_string(line) + ")";
                    return false;
                }
                break;
            default:
                if (!isAllowedBare((unsigned char) c)) {
                    error = std::string("Unexpected character '") + c
                          + "' (line " + std::to_string(line) + ")";
                    return false;
                }
                break;
        }
    }

    if (inQuote) {
        error = std::string("Unterminated string literal (missing closing ")
              + quoteChar + ")";
        return false;
    }
    if (parenDepth > 0) {
        error = "Unbalanced parentheses: " + std::to_string(parenDepth)
              + (parenDepth == 1 ? " '(' is never closed" : " '(' are never closed");
        return false;
    }
    return true;
}

// ==============================================================================
// SynthVoice
// ==============================================================================

float SynthVoice::advance(float sr, float attack, float decay, float sustain, float release) {
    if (envStage == Off) return 0.0f;

    float dt = 1.0f / sr;
    envTime += dt;

    switch (envStage) {
        case Attack:
            envLevel += dt / std::max(0.001f, attack);
            if (envLevel >= 1.0f) { envLevel = 1.0f; envStage = Decay; envTime = 0; }
            break;
        case Decay:
            envLevel -= dt / std::max(0.001f, decay) * (1.0f - sustain);
            if (envLevel <= sustain) { envLevel = sustain; envStage = Sustain; envTime = 0; }
            break;
        case Sustain:
            envLevel = sustain;
            break;
        case Release:
            envLevel -= dt / std::max(0.001f, release);
            if (envLevel <= 0.0f) { envLevel = 0.0f; envStage = Off; active = false; }
            break;
        default: break;
    }

    return juce::jlimit(0.0f, 1.0f, envLevel);
}

// ==============================================================================
// BuiltinSynthProcessor
// ==============================================================================

BuiltinSynthProcessor::BuiltinSynthProcessor(Node& n, Transport& t) : node(n), transport(t) {
    // Initialize wavetable from node's script field (expression) or default sine
    if (!node.script.empty())
        wavetable.generateFromExpression(node.script);
    else
        wavetable.generateSine();
}

void BuiltinSynthProcessor::prepareToPlay(double sr, int bs) {
    sampleRate = sr;
}

float BuiltinSynthProcessor::getParam(int idx, float def) const {
    if (idx >= 0 && idx < (int)node.params.size())
        return node.params[idx].value;
    return def;
}

void BuiltinSynthProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) {
    buf.clear();
    int numSamples = buf.getNumSamples();
    int numChannels = buf.getNumChannels();

    // Read ADSR and filter params from node
    float attack  = getParam(0, 0.01f);
    float decay   = getParam(1, 0.1f);
    float sustain = getParam(2, 0.7f);
    float release = getParam(3, 0.3f);
    float volume  = getParam(4, 0.5f);
    float cutoff  = getParam(5, 1.0f); // 0-1, mapped to 200-20000 Hz
    float reso    = getParam(6, 0.0f); // 0-1

    // Table size parameter (index 7): value is treated as sample count
    int newTableSize = (int)getParam(7, 2048.0f);
    if (newTableSize != wavetable.getTableSize())
        wavetable.setTableSize(newTableSize);

    // Process MIDI events
    for (auto metadata : midi) {
        auto msg = metadata.getMessage();
        if (msg.isNoteOn()) {
            // Find free voice or steal quietest
            int vi = -1;
            float minLevel = 999;
            for (int i = 0; i < MAX_VOICES; ++i) {
                if (!voices[i].active) { vi = i; break; }
                if (voices[i].envLevel < minLevel) { minLevel = voices[i].envLevel; vi = i; }
            }
            if (vi >= 0) {
                voices[vi].active = true;
                voices[vi].note = msg.getNoteNumber();
                voices[vi].mpeChannel = msg.getChannel();
                voices[vi].frequency = (float)juce::MidiMessage::getMidiNoteInHertz(msg.getNoteNumber());
                voices[vi].velocity = msg.getVelocity() / 127.0f;
                voices[vi].mpe = MpeVoiceState{};
                voices[vi].envStage = SynthVoice::Attack;
                voices[vi].envLevel = 0.0f;
                voices[vi].envTime = 0;
                // Don't reset phase for smoother note transitions
            }
        } else if (msg.isNoteOff()) {
            for (int i = 0; i < MAX_VOICES; ++i) {
                if (voices[i].active && voices[i].note == msg.getNoteNumber()
                    && voices[i].envStage != SynthVoice::Release) {
                    voices[i].envStage = SynthVoice::Release;
                    voices[i].envTime = 0;
                }
            }
        } else if (msg.isAllNotesOff()) {
            for (int i = 0; i < MAX_VOICES; ++i) {
                if (voices[i].active && voices[i].envStage != SynthVoice::Release) {
                    voices[i].envStage = SynthVoice::Release;
                    voices[i].envTime = 0;
                }
            }
        } else if (msg.isAllSoundOff()) {
            for (int i = 0; i < MAX_VOICES; ++i)
                voices[i] = {};
        }
    }
    // Distribute per-note expression (pitch bend, channel + poly key
    // pressure, timbre) to the matching voices. No-op until a controller
    // actually sends these messages.
    distributeMpeMessages(midi, voices);

    // Render voices
    float phaseInc;
    for (int i = 0; i < MAX_VOICES; ++i) {
        if (!voices[i].active) continue;
        auto& v = voices[i];
        // Apply per-note pitch bend (semitones) and aftertouch swell.
        float freq = v.frequency * std::pow(2.0f, v.mpe.pitchBend / 12.0f);
        float pMul = 1.0f + node.aftertouchSensitivity * effectivePressure(v.mpe);
        phaseInc = freq / (float)sampleRate;

        for (int s = 0; s < numSamples; ++s) {
            float env = v.advance((float)sampleRate, attack, decay, sustain, release);
            if (!v.active) break;

            float sample = wavetable.sample(v.phase, freq, (float)sampleRate);
            sample *= env * v.velocity * volume * pMul;

            for (int c = 0; c < numChannels; ++c)
                buf.addSample(c, s, sample);

            v.phase += phaseInc;
            if (v.phase >= 1.0f) v.phase -= 1.0f;
        }
    }

    // Track first active voice's phase for visualization
    for (int i = 0; i < MAX_VOICES; ++i) {
        if (voices[i].active) { lastPhase = voices[i].phase; break; }
    }

    // Simple one-pole low-pass filter
    if (cutoff < 0.99f) {
        float freq = 200.0f * std::pow(100.0f, cutoff); // 200 Hz to 20000 Hz
        float rc = 1.0f / (2.0f * 3.14159265f * freq);
        float alpha = (1.0f / (float)sampleRate) / (rc + 1.0f / (float)sampleRate);

        for (int c = 0; c < numChannels; ++c) {
            auto* data = buf.getWritePointer(c);
            float prev = data[0];
            for (int s = 1; s < numSamples; ++s) {
                data[s] = prev + alpha * (data[s] - prev);
                prev = data[s];
            }
        }
    }
}

} // namespace SoundShop

#include "builtin_synth.h"
#include "fft_util.h"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <random>
#include <sstream>
#include <stack>
#include <cctype>
#include <complex>

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
        case 2: // zero — impulsive, clicky at t=0
            // already zero
            break;
        case 3: // linear — gives a chirp-like shift
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
    // Kill DC — it's almost never wanted and produces a silent offset.
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
// WaveExprParser — simple math expression evaluator
// ==============================================================================

// Tokenizer + recursive descent parser for waveform expressions
// Supports: sin, cos, abs, sqrt, pow, saw, square, triangle, noise, x, pi, e
// Operators: + - * / ^ ( )

// File-local reusable parser. Binds both `x` and `f` as free variables so the
// same expression language works for time-domain ("x in [0, 2pi)") and
// frequency-domain ("f = bin index") evaluation. Other wave-editor callers
// pass f=0 and vice versa.
namespace {
struct ExprParser {
    const char* str;
    const char* pos;
    float x = 0.0f;
    float f = 0.0f;
    std::mt19937 rng{42};

    void skipWS() { while (*pos == ' ' || *pos == '\t') pos++; }

    float parseNumber() {
        skipWS();
        const char* start = pos;
        if (*pos == '-' || *pos == '+') pos++;
        while (std::isdigit(*pos) || *pos == '.') pos++;
        if (pos == start) return 0;
        return std::strtof(start, nullptr);
    }

    float parseAtom() {
        skipWS();
        if (*pos == '(') {
            pos++;
            float val = parseExpr();
            skipWS();
            if (*pos == ')') pos++;
            return val;
        }
        if (*pos == '-') { pos++; return -parseAtom(); }

        if (strncmp(pos, "sin(", 4) == 0)  { pos += 3; return std::sin(parseAtom()); }
        if (strncmp(pos, "cos(", 4) == 0)  { pos += 3; return std::cos(parseAtom()); }
        if (strncmp(pos, "tan(", 4) == 0)  { pos += 3; return std::tan(parseAtom()); }
        if (strncmp(pos, "abs(", 4) == 0)  { pos += 3; return std::abs(parseAtom()); }
        if (strncmp(pos, "sqrt(", 5) == 0) { pos += 4; return std::sqrt(std::abs(parseAtom())); }
        if (strncmp(pos, "exp(", 4) == 0)  { pos += 3; return std::exp(parseAtom()); }
        if (strncmp(pos, "log(", 4) == 0)  { pos += 3; float v = parseAtom(); return v > 0 ? std::log(v) : 0.0f; }
        if (strncmp(pos, "pow(", 4) == 0) {
            pos += 4;
            float base = parseExpr();
            skipWS(); if (*pos == ',') pos++;
            float exp = parseExpr();
            skipWS(); if (*pos == ')') pos++;
            return std::pow(base, exp);
        }
        if (strncmp(pos, "saw(", 4) == 0) {
            pos += 3;
            float v = parseAtom();
            float t = v / (2.0f * 3.14159265f);
            return 2.0f * (t - std::floor(t + 0.5f));
        }
        if (strncmp(pos, "square(", 7) == 0) {
            pos += 6;
            float v = parseAtom();
            return std::sin(v) >= 0 ? 1.0f : -1.0f;
        }
        if (strncmp(pos, "triangle(", 9) == 0) {
            pos += 8;
            float v = parseAtom();
            float t = v / (2.0f * 3.14159265f);
            return 4.0f * std::abs(t - std::floor(t + 0.5f)) - 1.0f;
        }
        if (strncmp(pos, "noise(", 6) == 0) {
            pos += 5;
            parseAtom();
            std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
            return dist(rng);
        }
        if (strncmp(pos, "random", 6) == 0 && pos[6] != '(') {
            // bare `random` => uniform [-pi, pi] (useful for phase expressions)
            pos += 6;
            std::uniform_real_distribution<float> dist(-3.14159265f, 3.14159265f);
            return dist(rng);
        }
        if (strncmp(pos, "tanh(", 5) == 0) { pos += 4; return std::tanh(parseAtom()); }
        if (strncmp(pos, "clamp(", 6) == 0) {
            pos += 6;
            float v = parseExpr();
            skipWS(); if (*pos == ',') pos++;
            float lo = parseExpr();
            skipWS(); if (*pos == ',') pos++;
            float hi = parseExpr();
            skipWS(); if (*pos == ')') pos++;
            return std::clamp(v, lo, hi);
        }
        if (*pos == 'x' && !std::isalnum(*(pos + 1))) { pos++; return x; }
        if (*pos == 'f' && !std::isalnum(*(pos + 1))) { pos++; return f; }
        if (strncmp(pos, "pi", 2) == 0 && !std::isalnum(*(pos + 2))) { pos += 2; return 3.14159265f; }
        if (*pos == 'e' && !std::isalpha(*(pos + 1))) { pos++; return 2.71828183f; }

        return parseNumber();
    }

    float parsePow() {
        float val = parseAtom();
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

    float eval(float xVal, float fVal) {
        x = xVal;
        f = fVal;
        pos = str;
        return parseExpr();
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
    for (int i = 0; i < numBins; ++i) {
        result[i] = parser.eval(0.0f, (float)i);
    }
    return result;
}

float WaveExprParser::evaluateAt(const std::string& expr, float x, float f) {
    if (expr.empty()) return 0.0f;
    ExprParser parser;
    parser.str = expr.c_str();
    return parser.eval(x, f);
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
                voices[vi].noteNumber = msg.getNoteNumber();
                voices[vi].midiChannel = msg.getChannel();
                voices[vi].frequency = (float)juce::MidiMessage::getMidiNoteInHertz(msg.getNoteNumber());
                voices[vi].velocity = msg.getVelocity() / 127.0f;
                voices[vi].envStage = SynthVoice::Attack;
                voices[vi].envLevel = 0.0f;
                voices[vi].envTime = 0;
                // Don't reset phase for smoother note transitions
            }
        } else if (msg.isNoteOff()) {
            for (int i = 0; i < MAX_VOICES; ++i) {
                if (voices[i].active && voices[i].noteNumber == msg.getNoteNumber()
                    && voices[i].envStage != SynthVoice::Release) {
                    voices[i].envStage = SynthVoice::Release;
                    voices[i].envTime = 0;
                }
            }
        }
    }

    // Render voices
    float phaseInc;
    for (int i = 0; i < MAX_VOICES; ++i) {
        if (!voices[i].active) continue;
        auto& v = voices[i];
        phaseInc = v.frequency / (float)sampleRate;

        for (int s = 0; s < numSamples; ++s) {
            float env = v.advance((float)sampleRate, attack, decay, sustain, release);
            if (!v.active) break;

            float sample = wavetable.sample(v.phase, v.frequency, (float)sampleRate);
            sample *= env * v.velocity * volume;

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

#include "terrain_synth.h"
#include "builtin_synth.h" // for WaveExprParser
#include "fft_util.h"
#include "layered_wave_editor.h" // for LayeredWaveform decode/render
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <random>
#include <numeric>
#include <fstream>
#include <complex>
#include <cmath>

namespace SoundShop {

// ==============================================================================
// Terrain — N-dimensional sample data
// ==============================================================================

void Terrain::init(const std::vector<int>& dimensions) {
    dims = dimensions;
    int total = 1;
    for (int d : dims) total *= std::max(1, d);
    data.resize(total, 0.0f);
}

int Terrain::coordToFlatIndex(const std::vector<int>& indices) const {
    int flat = 0;
    int stride = 1;
    for (int d = (int)dims.size() - 1; d >= 0; --d) {
        flat += juce::jlimit(0, dims[d] - 1, indices[d]) * stride;
        stride *= dims[d];
    }
    return flat;
}

float Terrain::sample(const std::vector<float>& coord) const {
    if (data.empty() || dims.empty()) return 0.0f;
    int nd = (int)dims.size();

    // N-linear interpolation
    // For each dimension, compute the two neighboring indices and the fraction
    int numCorners = 1 << nd; // 2^N corners of the interpolation hypercube
    float result = 0.0f;

    for (int corner = 0; corner < numCorners; ++corner) {
        float weight = 1.0f;
        std::vector<int> indices(nd);
        for (int d = 0; d < nd; ++d) {
            float pos = coord[d] * (dims[d] - 1);
            pos = juce::jlimit(0.0f, (float)(dims[d] - 1), pos);
            int lo = (int)pos;
            int hi = std::min(lo + 1, dims[d] - 1);
            float frac = pos - lo;

            if (corner & (1 << d)) {
                indices[d] = hi;
                weight *= frac;
            } else {
                indices[d] = lo;
                weight *= (1.0f - frac);
            }
        }
        result += at(coordToFlatIndex(indices)) * weight;
    }
    return result;
}

void Terrain::fillConstant(float value) {
    std::fill(data.begin(), data.end(), value);
}

void Terrain::fillNoise(unsigned int seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (auto& s : data) s = dist(rng);
}

void Terrain::fillFromExpression(const std::string& expr) {
    if (dims.empty() || data.empty()) return;
    int nd = (int)dims.size();

    // Simple expression evaluator — reuse WaveExprParser approach
    // Variables: x (dim 0), y (dim 1), z (dim 2), w (dim 3), all in [0, 2*pi]
    // For higher dims, use numbered vars via the parser

    struct Parser {
        const char* str;
        const char* pos;
        float vars[8] = {};
        std::mt19937 rng{42};

        void skipWS() { while (*pos == ' ' || *pos == '\t') pos++; }
        float parseNumber() {
            skipWS();
            const char* start = pos;
            if (*pos == '-' || *pos == '+') pos++;
            while (std::isdigit(*pos) || *pos == '.') pos++;
            return (pos > start) ? std::strtof(start, nullptr) : 0;
        }
        float parseAtom() {
            skipWS();
            if (*pos == '(') { pos++; float v = parseExpr(); skipWS(); if (*pos == ')') pos++; return v; }
            if (*pos == '-') { pos++; return -parseAtom(); }
            if (strncmp(pos, "sin(", 4) == 0) { pos += 3; return std::sin(parseAtom()); }
            if (strncmp(pos, "cos(", 4) == 0) { pos += 3; return std::cos(parseAtom()); }
            if (strncmp(pos, "abs(", 4) == 0) { pos += 3; return std::abs(parseAtom()); }
            if (strncmp(pos, "sqrt(", 5) == 0) { pos += 4; return std::sqrt(std::abs(parseAtom())); }
            if (strncmp(pos, "tanh(", 5) == 0) { pos += 4; return std::tanh(parseAtom()); }
            if (strncmp(pos, "noise(", 6) == 0) {
                pos += 5; parseAtom();
                return std::uniform_real_distribution<float>(-1, 1)(rng);
            }
            if (strncmp(pos, "pow(", 4) == 0) {
                pos += 4; float b = parseExpr(); skipWS();
                if (*pos == ',') pos++; float e = parseExpr();
                skipWS(); if (*pos == ')') pos++; return std::pow(b, e);
            }
            if (*pos == 'x') { pos++; return vars[0]; }
            if (*pos == 'y') { pos++; return vars[1]; }
            if (*pos == 'z') { pos++; return vars[2]; }
            if (*pos == 'w') { pos++; return vars[3]; }
            if (strncmp(pos, "pi", 2) == 0) { pos += 2; return 3.14159265f; }
            if (*pos == 'e' && !std::isalpha(*(pos+1))) { pos++; return 2.71828183f; }
            return parseNumber();
        }
        float parsePow() { float v = parseAtom(); skipWS(); if (*pos == '^') { pos++; v = std::pow(v, parsePow()); } return v; }
        float parseMulDiv() {
            float v = parsePow();
            while (true) { skipWS();
                if (*pos == '*') { pos++; v *= parsePow(); }
                else if (*pos == '/') { pos++; float d = parsePow(); v = d != 0 ? v / d : 0; }
                else break; } return v; }
        float parseExpr() {
            float v = parseMulDiv();
            while (true) { skipWS();
                if (*pos == '+') { pos++; v += parseMulDiv(); }
                else if (*pos == '-') { pos++; v -= parseMulDiv(); }
                else break; } return v; }
        float eval() { pos = str; return parseExpr(); }
    };

    Parser parser;
    parser.str = expr.c_str();

    // Iterate over all points in the N-dimensional grid
    std::vector<int> indices(nd, 0);
    for (int flat = 0; flat < (int)data.size(); ++flat) {
        // Compute indices from flat
        int tmp = flat;
        for (int d = nd - 1; d >= 0; --d) {
            indices[d] = tmp % dims[d];
            tmp /= dims[d];
        }
        // Set vars: each dimension maps to [0, 2*pi]
        for (int d = 0; d < std::min(nd, 8); ++d)
            parser.vars[d] = 2.0f * 3.14159265f * indices[d] / std::max(1, dims[d] - 1);

        data[flat] = juce::jlimit(-1.0f, 1.0f, parser.eval());
    }
}

void Terrain::fillFromImage(const std::string& path) {
    // Load image using JUCE
    auto file = juce::File(path);
    if (!file.existsAsFile()) { fprintf(stderr, "Image not found: %s\n", path.c_str()); return; }

    auto img = juce::ImageFileFormat::loadFrom(file);
    if (!img.isValid()) { fprintf(stderr, "Failed to load image: %s\n", path.c_str()); return; }

    int w = img.getWidth(), h = img.getHeight();
    init({h, w}); // dims = [rows, cols]

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            auto pixel = img.getPixelAt(x, y);
            float brightness = pixel.getBrightness();
            data[y * w + x] = brightness * 2.0f - 1.0f;
        }
    }

    fprintf(stderr, "Terrain loaded from image: %dx%d\n", w, h);
}

void Terrain::smooth(int passes) {
    if (dims.size() != 2 || data.empty()) return;
    int h = dims[0], w = dims[1];
    for (int pass = 0; pass < passes; ++pass) {
        std::vector<float> smoothed(data.size());
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float sum = 0, weight = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        int ny = juce::jlimit(0, h - 1, y + dy);
                        int nx = juce::jlimit(0, w - 1, x + dx);
                        float k = (dx == 0 && dy == 0) ? 4.0f
                                : (dx == 0 || dy == 0) ? 2.0f : 1.0f;
                        sum += data[ny * w + nx] * k;
                        weight += k;
                    }
                }
                smoothed[y * w + x] = sum / weight;
            }
        }
        data = smoothed;
    }
}

void Terrain::fillFromSpectralExpression(const std::string& magExpr,
                                          const std::string& phaseExpr,
                                          int fftSize,
                                          int phaseMode)
{
    // Round fftSize up to a power of two.
    int n = 2;
    while (n < fftSize && n < 16384) n <<= 1;

    // Ensure the terrain is 1D of size n.
    init({n});

    int halfBins = n / 2 + 1;
    auto mags = WaveExprParser::evaluateOverBins(magExpr, halfBins);

    std::vector<float> phases(halfBins, 0.0f);
    std::mt19937 rng(1337);
    std::uniform_real_distribution<float> udist(-3.14159265f, 3.14159265f);
    switch (phaseMode) {
        case 0: phases = WaveExprParser::evaluateOverBins(phaseExpr, halfBins); break;
        case 1: for (auto& p : phases) p = udist(rng); break;
        case 2: break; // already zero
        case 3:
            for (int k = 0; k < halfBins; ++k)
                phases[k] = -3.14159265f * (float)k;
            break;
        default: break;
    }

    std::vector<std::complex<float>> spectrum(halfBins);
    for (int k = 0; k < halfBins; ++k) {
        float m = mags[k];
        float p = phases[k];
        if (k == 0 || k == halfBins - 1)
            spectrum[k] = std::complex<float>(m, 0.0f);
        else
            spectrum[k] = std::complex<float>(m * std::cos(p), m * std::sin(p));
    }
    // Kill DC offset — it would produce a silent bias on playback.
    spectrum[0] = 0.0f;

    FFT fft(n);
    std::vector<float> timeDomain;
    fft.inverseReal(spectrum, timeDomain);

    // Normalize to peak 1.0
    float peak = 0.0f;
    for (float v : timeDomain) peak = std::max(peak, std::abs(v));
    if (peak > 1e-9f) {
        float inv = 1.0f / peak;
        for (float& v : timeDomain) v *= inv;
    }

    // Copy into terrain data.
    if ((int)data.size() == n) {
        data = std::move(timeDomain);
    }
}

void Terrain::fillFromAudioFile(const std::string& path) {
    auto file = juce::File(path);
    if (!file.existsAsFile()) return;

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    auto* reader = fm.createReaderFor(file);
    if (!reader) return;

    int numSamples = (int)reader->lengthInSamples;
    init({numSamples});

    juce::AudioBuffer<float> buf(1, numSamples);
    reader->read(&buf, 0, numSamples, 0, true, false);
    std::memcpy(data.data(), buf.getReadPointer(0), numSamples * sizeof(float));

    delete reader;
    fprintf(stderr, "Terrain loaded from audio: %d samples\n", numSamples);
}

// ==============================================================================
// Traversal — maps time to N-dimensional coordinate
// ==============================================================================

std::vector<float> Traversal::evaluate(const TraversalParams& params, int numDims,
                                        double beatTime, double bpm, double sr) const {
    std::vector<float> coord(numDims, 0.5f);

    switch (params.mode) {
    case TraversalMode::Linear: {
        int axis = juce::jlimit(0, numDims - 1, params.linearAxis);
        float pos = std::fmod((float)(beatTime * params.linearSpeed), 1.0f);
        if (pos < 0) pos += 1.0f;
        coord[axis] = pos;
        break;
    }

    case TraversalMode::Orbit: {
        float t = (float)(beatTime * params.speed) * 2.0f * 3.14159265f;
        float rx = params.radiusX + params.radiusModAmount *
            std::sin((float)(beatTime * params.radiusModSpeed) * 2.0f * 3.14159265f);
        float ry = params.radiusY + params.radiusModAmount *
            std::sin((float)(beatTime * params.radiusModSpeed) * 2.0f * 3.14159265f);
        rx = juce::jlimit(0.0f, 0.5f, rx);
        ry = juce::jlimit(0.0f, 0.5f, ry);

        if (numDims >= 1) coord[0] = juce::jlimit(0.0f, 1.0f, params.centerX + rx * std::cos(t));
        if (numDims >= 2) coord[1] = juce::jlimit(0.0f, 1.0f, params.centerY + ry * std::sin(t));
        // Higher dims: orbit in pairs
        for (int d = 2; d < numDims; d += 2) {
            coord[d] = juce::jlimit(0.0f, 1.0f, 0.5f + 0.3f * std::cos(t * (1.0f + d * 0.1f)));
            if (d + 1 < numDims)
                coord[d + 1] = juce::jlimit(0.0f, 1.0f, 0.5f + 0.3f * std::sin(t * (1.0f + d * 0.1f)));
        }
        break;
    }

    case TraversalMode::Lissajous: {
        for (int d = 0; d < numDims && d < 8; ++d) {
            auto& ax = params.axes[d];
            float t = (float)(beatTime * ax.frequency) + ax.phase;
            coord[d] = juce::jlimit(0.0f, 1.0f,
                ax.center + ax.amplitude * std::sin(t * 2.0f * 3.14159265f));
        }
        break;
    }

    case TraversalMode::Path: {
        if (params.pathPoints.empty()) break;
        if (params.pathPoints.size() == 1) {
            coord = params.pathPoints[0].coord;
            coord.resize(numDims, 0.5f);
            break;
        }
        // Find position along path based on beat time (loop)
        float totalTime = params.pathPoints.back().time;
        if (totalTime <= 0) break;
        float t = std::fmod((float)beatTime, totalTime);
        if (t < 0) t += totalTime;

        // Find surrounding points
        int idx = 0;
        for (int i = 0; i < (int)params.pathPoints.size() - 1; ++i)
            if (params.pathPoints[i].time <= t) idx = i;

        auto& p0 = params.pathPoints[idx];
        auto& p1 = params.pathPoints[std::min(idx + 1, (int)params.pathPoints.size() - 1)];
        float frac = (p1.time > p0.time) ? (t - p0.time) / (p1.time - p0.time) : 0;

        for (int d = 0; d < numDims; ++d) {
            float v0 = d < (int)p0.coord.size() ? p0.coord[d] : 0.5f;
            float v1 = d < (int)p1.coord.size() ? p1.coord[d] : 0.5f;
            coord[d] = juce::jlimit(0.0f, 1.0f, v0 + frac * (v1 - v0));
        }
        break;
    }

    case TraversalMode::Physics: {
        // Simple Verlet-style physics
        if (physPos.size() != (size_t)numDims || lastPhysBeat < 0) {
            physPos.resize(numDims, 0.5f);
            physVel.resize(numDims, 0.0f);
            lastPhysBeat = beatTime;
        }
        double dt = beatTime - lastPhysBeat;
        if (dt > 0 && dt < 1.0) { // guard against huge jumps
            for (int d = 0; d < numDims; ++d) {
                // Gravity toward attractors
                float accel = 0;
                for (auto& att : params.attractors) {
                    float target = (d == 0) ? att.x : (d == 1) ? att.y : 0.5f;
                    float diff = target - physPos[d];
                    accel += diff * att.strength;
                }
                physVel[d] += (float)(accel * dt);
                physVel[d] *= (1.0f - params.friction);
                physPos[d] += (float)(physVel[d] * dt);
                // Bounce off walls
                if (physPos[d] < 0) { physPos[d] = -physPos[d]; physVel[d] = std::abs(physVel[d]); }
                if (physPos[d] > 1) { physPos[d] = 2.0f - physPos[d]; physVel[d] = -std::abs(physVel[d]); }
                physPos[d] = juce::jlimit(0.0f, 1.0f, physPos[d]);
            }
        }
        lastPhysBeat = beatTime;
        coord = physPos;
        break;
    }

    case TraversalMode::Custom:
        // TODO: expression-based traversal
        break;
    }

    return coord;
}

// ==============================================================================
// EnvCurve — cached envelope shape table
// ==============================================================================

static constexpr int ENV_TABLE_SIZE = 256;

void TerrainSynthProcessor::EnvCurve::buildFromExpression(const std::string& expr) {
    table = WaveExprParser::evaluate(expr, ENV_TABLE_SIZE);
    // Clamp to 0..1
    for (auto& v : table) v = juce::jlimit(0.0f, 1.0f, (v + 1.0f) * 0.5f); // map -1..1 to 0..1
    valid = true;
}

void TerrainSynthProcessor::EnvCurve::buildFromPoints(const std::vector<std::pair<float, float>>& points) {
    table.resize(ENV_TABLE_SIZE);
    if (points.empty()) { valid = false; return; }
    for (int i = 0; i < ENV_TABLE_SIZE; ++i) {
        float t = (float)i / (ENV_TABLE_SIZE - 1);
        // Linear interpolation between points
        float val = points.back().second;
        for (int j = 1; j < (int)points.size(); ++j) {
            if (t <= points[j].first) {
                float frac = (points[j].first > points[j-1].first)
                    ? (t - points[j-1].first) / (points[j].first - points[j-1].first) : 0;
                val = points[j-1].second + frac * (points[j].second - points[j-1].second);
                break;
            }
        }
        table[i] = juce::jlimit(0.0f, 1.0f, val);
    }
    valid = true;
}

float TerrainSynthProcessor::EnvCurve::evaluate(float t) const {
    if (!valid || table.empty()) return t; // default: linear
    t = juce::jlimit(0.0f, 1.0f, t);
    float pos = t * (ENV_TABLE_SIZE - 1);
    int idx = (int)pos;
    float frac = pos - idx;
    idx = std::min(idx, ENV_TABLE_SIZE - 2);
    return table[idx] + frac * (table[idx + 1] - table[idx]);
}

void TerrainSynthProcessor::rebuildEnvCurves() {
    if (!node.envAttackCurve.empty())
        attackCurve.buildFromExpression(node.envAttackCurve);
    else if (!node.envAttackPoints.empty())
        attackCurve.buildFromPoints(node.envAttackPoints);
    else
        attackCurve.valid = false;

    if (!node.envDecayCurve.empty())
        decayCurve.buildFromExpression(node.envDecayCurve);
    else if (!node.envDecayPoints.empty())
        decayCurve.buildFromPoints(node.envDecayPoints);
    else
        decayCurve.valid = false;

    if (!node.envReleaseCurve.empty())
        releaseCurve.buildFromExpression(node.envReleaseCurve);
    else if (!node.envReleasePoints.empty())
        releaseCurve.buildFromPoints(node.envReleasePoints);
    else
        releaseCurve.valid = false;
}

// ==============================================================================
// TerrainSynthProcessor::Voice
// ==============================================================================

float TerrainSynthProcessor::Voice::advanceEnv(float sr, float a, float d, float s, float r,
                                                 const EnvCurve* aCurve, const EnvCurve* dCurve,
                                                 const EnvCurve* rCurve) {
    if (envStage == Off) return 0.0f;
    float dt = 1.0f / sr;
    envTime += dt;
    switch (envStage) {
        case Attack: {
            float t = (float)(envTime / std::max(0.001, (double)a));
            if (t >= 1.0f) { envLevel = 1.0f; envStage = Decay; envTime = 0; }
            else envLevel = (aCurve && aCurve->valid) ? aCurve->evaluate(t) : t;
            break;
        }
        case Decay: {
            float t = (float)(envTime / std::max(0.001, (double)d));
            if (t >= 1.0f) { envLevel = s; envStage = Sustain; envTime = 0; }
            else {
                float shape = (dCurve && dCurve->valid) ? dCurve->evaluate(t) : t;
                envLevel = 1.0f - shape * (1.0f - s); // 1.0 -> sustain
            }
            break;
        }
        case Sustain:
            envLevel = s;
            break;
        case Release: {
            float t = (float)(envTime / std::max(0.001, (double)r));
            if (t >= 1.0f) { envLevel = 0; envStage = Off; active = false; }
            else {
                float shape = (rCurve && rCurve->valid) ? rCurve->evaluate(t) : t;
                envLevel = (1.0f - shape) * s; // sustain -> 0
            }
            break;
        }
        default: break;
    }
    return juce::jlimit(0.0f, 1.0f, envLevel);
}

// ==============================================================================
// TerrainSynthProcessor
// ==============================================================================

TerrainSynthProcessor::TerrainSynthProcessor(Node& n, Transport& t) : node(n), transport(t) {
    auto& script = node.script;

    if (script.find("__image__:") == 0) {
        terrain.fillFromImage(script.substr(10));
    } else if (script.find("__audio__:") == 0) {
        terrain.fillFromAudioFile(script.substr(10));
    } else if (script.find("__layered__:") == 0) {
        // Layered waveform — decode the layer list and sum into a 1D terrain.
        // This is effectively a single-frame wavetable, so flag it as such so
        // the render loop uses cycle-based phase advancement instead of the
        // sample-based formula.
        LayeredWaveform lw;
        if (lw.decode(script)) {
            std::vector<float> samples;
            lw.render(samples);
            terrain.init({(int)samples.size()});
            auto& d = terrain.getData();
            if ((int)d.size() == (int)samples.size())
                d = std::move(samples);
        } else {
            terrain.init({2048});
            terrain.fillFromExpression("sin(x)");
        }
        traversalParams.mode = TraversalMode::Linear;
        mode = TerrainSynthMode::SamplePerPoint;
        isWavetable = true;
        wtFrameCount = 1;
    } else if (script.find("__wavetable__:") == 0) {
        // N-dimensional wavetable — Grid mode builds an (N+1)-D terrain;
        // Scatter mode keeps frames in a flat list and computes a Wendland
        // RBF blend each block.
        WavetableDoc doc;
        bool decoded = doc.decode(script);
        if (decoded && doc.mode == WavetableMode::Scatter && !doc.scatterFrames.empty()) {
            int ts = doc.tableSize;
            wtScatterFrameSamples.clear();
            wtScatterFramePositions.clear();
            for (auto& sf : doc.scatterFrames) {
                std::vector<float> samples;
                sf.wave.tableSize = ts;
                sf.wave.render(samples);
                if ((int)samples.size() != ts) samples.resize(ts, 0.0f);
                wtScatterFrameSamples.push_back(std::move(samples));
                wtScatterFramePositions.push_back(sf.position);
            }
            // 1D terrain — the per-block blend writes the active waveform
            // into terrain.data so the per-sample render path is unchanged.
            terrain.init({ts});
            wtScatter = true;
            wtScatterDims = doc.scatterDims;
            wtScatterRadius = doc.scatterRadius;
            isWavetable = true;
            wtFrameCount = (int)wtScatterFrameSamples.size();
            wtNumDims = doc.scatterDims;
            traversalParams.mode = TraversalMode::Linear;
            mode = TerrainSynthMode::SamplePerPoint;
        } else if (decoded && !doc.frames.empty()) {
            int ts = doc.tableSize;

            // Build terrain dimensions: {tableSize, dim0, dim1, ...}
            std::vector<int> terrainDims = {ts};
            for (int d : doc.gridDims) terrainDims.push_back(std::max(1, d));
            terrain.init(terrainDims);
            auto& data = terrain.getData();

            // Compute stride for each grid dimension to map flat frame index
            // to the correct position in the N-dimensional terrain.
            int nf = (int)doc.frames.size();
            for (int f = 0; f < nf; ++f) {
                std::vector<float> samples;
                doc.frames[f].tableSize = ts;
                doc.frames[f].render(samples);

                // Compute the flat terrain offset for this frame.
                // The terrain is {ts, dim0, dim1, ...}. Frame f maps to
                // some (d0, d1, ...) in the grid. We need to compute the
                // stride for dimension 0 (phase) then write samples there.
                // Using coordToFlatIndex logic: stride for dim0=ts is last,
                // so data layout is: outermost dims first, phase last...
                // Actually: terrain.coordToFlatIndex does:
                //   flat = sum(indices[d] * stride_d) where stride is
                //   computed from last dim backwards.
                // For {ts, dim0}: flat = phase * dim0 + frameIdx
                // For {ts, d0, d1}: flat = phase * d0*d1 + d0idx * d1 + d1idx
                // So we need to decompose f into grid coords.
                std::vector<int> gridCoord;
                int remaining = f;
                for (int di = (int)doc.gridDims.size() - 1; di >= 0; --di) {
                    gridCoord.push_back(remaining % std::max(1, doc.gridDims[di]));
                    remaining /= std::max(1, doc.gridDims[di]);
                }
                std::reverse(gridCoord.begin(), gridCoord.end());

                for (int i = 0; i < ts && i < (int)samples.size(); ++i) {
                    std::vector<int> fullIdx = {i};
                    fullIdx.insert(fullIdx.end(), gridCoord.begin(), gridCoord.end());
                    int flatIdx = terrain.coordToFlatIndex(fullIdx);
                    if (flatIdx >= 0 && flatIdx < terrain.totalSize())
                        data[flatIdx] = samples[i];
                }
            }
            isWavetable = true;
            wtFrameCount = nf;
            wtNumDims = doc.numDimensions();
        } else {
            terrain.init({2048});
            terrain.fillFromExpression("sin(x)");
        }
        traversalParams.mode = TraversalMode::Linear;
        mode = TerrainSynthMode::SamplePerPoint;
    } else if (script.find("__spectral__:") == 0) {
        // Format: __spectral__:<fftSize>:<phaseMode>:<magExpr>|<phaseExpr>
        std::string rest = script.substr(13);
        auto c1 = rest.find(':');
        auto c2 = (c1 != std::string::npos) ? rest.find(':', c1 + 1) : std::string::npos;
        int fftSize = 2048;
        int phaseMode = 1; // random
        std::string magExpr, phaseExpr;
        if (c1 != std::string::npos && c2 != std::string::npos) {
            try { fftSize = std::stoi(rest.substr(0, c1)); } catch (...) {}
            try { phaseMode = std::stoi(rest.substr(c1 + 1, c2 - c1 - 1)); } catch (...) {}
            std::string body = rest.substr(c2 + 1);
            auto bar = body.find('|');
            if (bar != std::string::npos) {
                magExpr = body.substr(0, bar);
                phaseExpr = body.substr(bar + 1);
            } else {
                magExpr = body;
            }
        } else {
            magExpr = "exp(-f/20)";
        }
        terrain.fillFromSpectralExpression(magExpr, phaseExpr, fftSize, phaseMode);
        traversalParams.mode = TraversalMode::Linear;
        mode = TerrainSynthMode::SamplePerPoint;
        isWavetable = true;
        wtFrameCount = 1;
    } else {
        std::string expr = script.empty() ? "sin(x)" : script;

        // Auto-detect dimensions from the expression:
        // If it uses y, z, w → create higher-dimensional terrain
        bool usesY = expr.find('y') != std::string::npos;
        bool usesZ = expr.find('z') != std::string::npos;
        bool usesW = expr.find('w') != std::string::npos;

        if (usesW)
            terrain.init({64, 64, 64, 64});
        else if (usesZ)
            terrain.init({64, 64, 64});
        else if (usesY)
            terrain.init({256, 256});
        else
            terrain.init({2048}); // 1D waveform

        terrain.fillFromExpression(expr);

        // For 1D, default to linear traversal and SamplePerPoint
        if (!usesY) {
            traversalParams.mode = TraversalMode::Linear;
            mode = TerrainSynthMode::SamplePerPoint;
        }
    }
}

void TerrainSynthProcessor::prepareToPlay(double sr, int bs) {
    sampleRate = sr;
    rebuildEnvCurves();
}

float TerrainSynthProcessor::getParam(int idx, float def) const {
    if (idx >= 0 && idx < (int)node.params.size())
        return node.params[idx].value;
    return def;
}

// Look up a parameter by name (for sparse param lists where index doesn't apply).
static float getParamByName(const Node& node, const std::string& name, float def) {
    for (const auto& p : node.params)
        if (p.name == name) return p.value;
    return def;
}

void TerrainSynthProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) {
    buf.clear();
    int numSamples = buf.getNumSamples();
    int numChannels = buf.getNumChannels();
    if (numChannels == 0) return;

    float attack  = getParam(0, 0.01f);
    float decay   = getParam(1, 0.1f);
    float sustain = getParam(2, 0.7f);
    float release = getParam(3, 0.3f);
    float volume  = getParam(4, 0.5f);

    // Traversal param modulation from node params — only override defaults
    // for nodes that actually have these params. Slimmed synths leave them
    // at the constructor-set defaults so 1D playback works correctly.
    if ((int)node.params.size() > 11) {
        traversalParams.speed           = getParam(5, 1.0f);
        traversalParams.radiusX         = getParam(6, 0.3f);
        traversalParams.radiusY         = getParam(7, 0.3f);
        traversalParams.centerX         = getParam(8, 0.5f);
        traversalParams.centerY         = getParam(9, 0.5f);
        traversalParams.radiusModSpeed  = getParam(10, 0.0f);
        traversalParams.radiusModAmount = getParam(11, 0.0f);
    }

    // Traversal mode: read from param if present, otherwise leave alone.
    // Slimmed-param synths (Waveform, Piano, Drum Machine) don't have a
    // Traversal param at index 12, so we keep whatever the constructor set
    // (Linear for 1D layered/wavetable nodes) instead of forcing Orbit.
    if ((int)node.params.size() > 12) {
        int modeInt = (int)getParam(12, 0.0f);
        traversalParams.mode = (modeInt == 1) ? TraversalMode::Linear
                             : (modeInt == 2) ? TraversalMode::Lissajous
                             : (modeInt == 3) ? TraversalMode::Physics
                             : TraversalMode::Orbit;
    }

    // Synth mode: 0=SamplePerPoint, 1=WaveformPerPoint
    mode = ((int)getParam(13, 0.0f) == 1) ? TerrainSynthMode::WaveformPerPoint
                                           : TerrainSynthMode::SamplePerPoint;

    // Internal LFO modulation
    lfo1.frequency = getParam(14, 0.5f);
    lfo2.frequency = getParam(15, 0.2f);
    float lfo1Amt = getParam(16, 0.0f);
    float lfo2Amt = getParam(17, 0.0f);

    // Graintable parameters
    grainSize = getParam(18, 0.0f);         // seconds, 0 = off
    bool newFreeze = ((int)getParam(19, 0.0f) != 0);
    if (newFreeze && !grainFreeze) {
        // Just activated freeze: capture current position
        freezePosition = lastPosition.empty() ? 0.5f : lastPosition[0];
    }
    grainFreeze = newFreeze;
    float grainJitter = getParam(20, 0.0f); // random offset per grain, 0-1

    // Process MIDI
    for (auto metadata : midi) {
        auto msg = metadata.getMessage();
        if (msg.isNoteOn()) {
            int vi = -1;
            float minLev = 999;
            for (int i = 0; i < MAX_VOICES; ++i) {
                if (!voices[i].active) { vi = i; break; }
                if (voices[i].envLevel < minLev) { minLev = voices[i].envLevel; vi = i; }
            }
            if (vi >= 0) {
                int ch = juce::jlimit(1, 16, msg.getChannel());
                voices[vi].active = true;
                voices[vi].noteNumber = msg.getNoteNumber();
                voices[vi].midiChannel = ch;
                voices[vi].baseFrequency = transport.noteToFreq(msg.getNoteNumber());
                // Seed effective frequency with the current bend factor so
                // notes triggered while the pitch wheel is held start at the
                // bent pitch rather than the nominal one.
                voices[vi].frequency =
                    voices[vi].baseFrequency * pitchBendFactor[ch - 1];
                // Apply the node's velocity-sensitivity setting: sens=0
                // collapses everything to full volume, sens=1 is linear.
                // Default 1.0 preserves prior behavior for older projects.
                {
                    float velSens = getParamByName(node, "Vel Sens", 1.0f);
                    float raw = msg.getVelocity() / 127.0f;
                    voices[vi].velocity = 1.0f - velSens * (1.0f - raw);
                }
                voices[vi].phase = 0;
                voices[vi].startBeat = transport.positionBeats();
                voices[vi].envStage = Voice::Attack;
                voices[vi].envLevel = 0;
                voices[vi].envTime = 0;
            }
        } else if (msg.isNoteOff()) {
            int ch = juce::jlimit(1, 16, msg.getChannel());
            for (int i = 0; i < MAX_VOICES; ++i)
                if (voices[i].active && voices[i].noteNumber == msg.getNoteNumber()
                    && voices[i].envStage != Voice::Release) {
                    // If sustain pedal is held on this channel, defer the
                    // release until the pedal comes back up; otherwise
                    // release immediately.
                    if (sustainPedal[ch - 1]) {
                        voices[i].sustainHeld = true;
                    } else {
                        voices[i].envStage = Voice::Release;
                        voices[i].envTime = 0;
                    }
                }
        } else if (msg.isPitchWheel()) {
            // Pitch bend: update this channel's bend factor and retune any
            // currently-playing voices on the same channel so sustained
            // notes bend in real time.
            int ch = juce::jlimit(1, 16, msg.getChannel());
            float norm = ((float)msg.getPitchWheelValue() - 8192.0f) / 8192.0f;
            float semis = norm * kPitchBendRangeSemis;
            pitchBendFactor[ch - 1] = std::pow(2.0f, semis / 12.0f);
            for (int i = 0; i < MAX_VOICES; ++i)
                if (voices[i].active && voices[i].midiChannel == ch)
                    voices[i].frequency =
                        voices[i].baseFrequency * pitchBendFactor[ch - 1];
        } else if (msg.isController() && msg.getControllerNumber() == 1) {
            // Mod wheel: store per-channel value for the vibrato LFO in the
            // render loop to read.
            int ch = juce::jlimit(1, 16, msg.getChannel());
            modWheel[ch - 1] = (float)msg.getControllerValue() / 127.0f;
        } else if (msg.isController() && msg.getControllerNumber() == 64) {
            // Sustain pedal (CC#64): when released, any voices that had
            // their release deferred (sustainHeld=true) are sent into their
            // release stage immediately.
            int ch = juce::jlimit(1, 16, msg.getChannel());
            bool held = msg.getControllerValue() >= 64;
            sustainPedal[ch - 1] = held;
            if (!held) {
                for (int i = 0; i < MAX_VOICES; ++i) {
                    if (voices[i].active && voices[i].sustainHeld
                        && voices[i].midiChannel == ch) {
                        voices[i].sustainHeld = false;
                        voices[i].envStage = Voice::Release;
                        voices[i].envTime = 0;
                    }
                }
            }
        }
    }

    // Detect audio-rate signal inputs (channels 2+ on the buffer)
    int numSignalInputs = std::max(0, numChannels - 2);
    bool hasSignalInputs = numSignalInputs > 0;

    // Scatter wavetable: blend frames into the 1D terrain at block start
    // using a Wendland C^2 RBF over the current Position. The per-sample
    // path then reads terrain.sample(phase) unchanged.
    if (isWavetable && wtScatter && !wtScatterFrameSamples.empty()) {
        std::vector<float> qpos(wtScatterDims, 0.5f);
        for (int d = 0; d < wtScatterDims; ++d) {
            std::string pname = (wtScatterDims == 1)
                ? std::string("Position")
                : std::string("Position ") + std::to_string(d + 1);
            qpos[d] = juce::jlimit(0.0f, 1.0f, getParamByName(node, pname.c_str(), 0.5f));
        }
        int nFrames = (int)wtScatterFrameSamples.size();
        std::vector<float> weights(nFrames, 0.0f);
        float totalW = 0.0f;
        float r = std::max(1e-3f, wtScatterRadius);
        for (int fi = 0; fi < nFrames; ++fi) {
            const auto& fp = wtScatterFramePositions[fi];
            float d2 = 0.0f;
            for (int dim = 0; dim < wtScatterDims; ++dim) {
                float a = (dim < (int)fp.size()) ? fp[dim] : 0.5f;
                float dd = a - qpos[dim];
                d2 += dd * dd;
            }
            float dist = std::sqrt(d2);
            if (dist < r) {
                float u = dist / r;
                float v = 1.0f - u;
                // Wendland phi_{3,1}: (1-u)^4 * (4u + 1)
                float w = v * v * v * v * (4.0f * u + 1.0f);
                weights[fi] = w;
                totalW += w;
            }
        }
        if (totalW < 1e-9f) {
            // Fall back to nearest frame so we never produce silence.
            int nearest = 0;
            float bestD2 = 1e30f;
            for (int fi = 0; fi < nFrames; ++fi) {
                const auto& fp = wtScatterFramePositions[fi];
                float d2 = 0.0f;
                for (int dim = 0; dim < wtScatterDims; ++dim) {
                    float a = (dim < (int)fp.size()) ? fp[dim] : 0.5f;
                    float dd = a - qpos[dim];
                    d2 += dd * dd;
                }
                if (d2 < bestD2) { bestD2 = d2; nearest = fi; }
            }
            weights[nearest] = 1.0f;
            totalW = 1.0f;
        }
        float invT = 1.0f / totalW;
        auto& tdata = terrain.getData();
        int ts = (int)tdata.size();
        std::fill(tdata.begin(), tdata.end(), 0.0f);
        for (int fi = 0; fi < nFrames; ++fi) {
            if (weights[fi] <= 0.0f) continue;
            float w = weights[fi] * invT;
            const auto& src = wtScatterFrameSamples[fi];
            int n = std::min((int)src.size(), ts);
            for (int i = 0; i < n; ++i) tdata[i] += w * src[i];
        }
    }

    // Render
    double beatPos = transport.positionBeats();
    double beatsPerSample = transport.bpm / (60.0 * sampleRate);
    int nd = terrain.numDimensions();

    for (int s = 0; s < numSamples; ++s) {
        double currentBeat = beatPos + s * beatsPerSample;

        // LFO modulation applied to traversal params
        float l1 = lfo1.advance((float)sampleRate) * lfo1Amt;
        float l2 = lfo2.advance((float)sampleRate) * lfo2Amt;

        // Modulate a copy of traversal params
        TraversalParams modParams = traversalParams;
        modParams.radiusX = juce::jlimit(0.0f, 0.5f, modParams.radiusX + l1 * 0.2f);
        modParams.radiusY = juce::jlimit(0.0f, 0.5f, modParams.radiusY + l2 * 0.2f);
        modParams.centerX = juce::jlimit(0.0f, 1.0f, modParams.centerX + l2 * 0.1f);
        modParams.centerY = juce::jlimit(0.0f, 1.0f, modParams.centerY + l1 * 0.1f);

        // Evaluate traversal position
        auto coord = traversal.evaluate(modParams, nd, currentBeat,
                                         transport.bpm, sampleRate);

        // Wavetable mode: coord[1] is the frame position, driven by a
        // Position parameter (index 21) rather than the traversal. coord[0]
        // stays whatever traversal set and is then pitch-swept as usual.
        if (isWavetable) {
            // For wavetable playback the traversal must NOT modulate the
            // phase axis — only v.phase (driven by note pitch) should sweep
            // the wavetable. Otherwise the Linear traversal's beat-based
            // motion adds an unwanted slow modulation that sounds like noise.
            if (!coord.empty()) coord[0] = 0.0f;
            // Scatter mode: terrain is 1D, blend already happened pre-loop —
            // nothing to write into coord[d+1] (would crash, no such dim).
            if (!wtScatter) {
                // Set each Position dimension from named params.
                // 1D: "Position" → coord[1]
                // ND: "Position 1"..."Position N"
                if (wtNumDims == 1 && nd >= 2) {
                    coord[1] = juce::jlimit(0.0f, 1.0f, getParamByName(node, "Position", 0.0f));
                } else {
                    for (int d = 0; d < wtNumDims && d + 1 < nd; ++d) {
                        std::string pname = (wtNumDims == 1) ? "Position"
                            : "Position " + std::to_string(d + 1);
                        coord[d + 1] = juce::jlimit(0.0f, 1.0f,
                            getParamByName(node, pname.c_str(), 0.0f));
                    }
                }
            }
        }

        // Override coordinates with audio-rate signal inputs (channels 2+)
        if (hasSignalInputs) {
            for (int si = 0; si < numSignalInputs && si < nd; ++si) {
                float sigVal = buf.getSample(2 + si, s);
                coord[si] = juce::jlimit(0.0f, 1.0f, (sigVal + 1.0f) * 0.5f);
            }
        }

        // Graintable: freeze overrides traversal position
        if (grainFreeze && !coord.empty())
            coord[0] = freezePosition;

        if (s == numSamples / 2)
            lastPosition = coord;

        float totalSample = 0.0f;
        int activeVoiceCount = 0;

        // Grain size in samples (0 = off)
        int grainSizeSamples = (grainSize > 0) ? std::max(1, (int)(grainSize * sampleRate)) : 0;

        // Advance the shared vibrato LFO once per sample. Each voice scales
        // its frequency by a per-channel mod-wheel depth. Result: mod wheel
        // up = audible vibrato on all notes through that synth.
        vibratoPhase += kVibratoRateHz / (float)sampleRate;
        if (vibratoPhase > 1.0f) vibratoPhase -= 1.0f;
        float vibratoLfo = std::sin(vibratoPhase * 2.0f * 3.14159265f);

        for (int vi = 0; vi < MAX_VOICES; ++vi) {
            if (!voices[vi].active) continue;
            auto& v = voices[vi];
            float env = v.advanceEnv((float)sampleRate, attack, decay, sustain, release,
                                      &attackCurve, &decayCurve, &releaseCurve);
            if (!v.active) continue;

            // Per-voice effective frequency = (base * pitch-bend) * vibrato
            // v.frequency already has the bend factor baked in by the MIDI
            // handler; multiply by the vibrato factor for this sample.
            // Vibrato Depth param (0..1, default 1) scales the default
            // mod-wheel vibrato. Set to 0 to disable and MIDI-Learn CC1
            // to drive something else instead.
            float mwDepth = modWheel[v.midiChannel - 1];
            float vibAmt  = getParamByName(node, "Vibrato", 1.0f);
            float vibSemis = mwDepth * vibAmt * kVibratoMaxSemis * vibratoLfo;
            float vibratoFactor = std::pow(2.0f, vibSemis / 12.0f);
            float effFreq = v.frequency * vibratoFactor;

            float sample;
            if (mode == TerrainSynthMode::SamplePerPoint) {
                float pitchScale = effFreq / 440.0f;
                auto pitchCoord = coord;
                if (!pitchCoord.empty())
                    pitchCoord[0] = std::fmod(pitchCoord[0] + v.phase, 1.0f);

                if (grainSizeSamples > 0) {
                    // Graintable mode: crossfade between overlapping grains
                    // Two grains offset by half a grain size, triangular crossfade
                    float grainPhase = std::fmod(v.phase * (float)sampleRate, (float)grainSizeSamples);
                    float crossfade = grainPhase / grainSizeSamples; // 0..1 within grain

                    // Grain A: at current position
                    auto coordA = pitchCoord;
                    float sampleA = terrain.sample(coordA);

                    // Grain B: offset by half a grain
                    auto coordB = pitchCoord;
                    float halfGrainNorm = grainSize * 0.5f * pitchScale /
                        std::max(1.0f, (float)terrain.totalSize() / (float)sampleRate);
                    if (!coordB.empty())
                        coordB[0] = std::fmod(coordB[0] + halfGrainNorm + 1.0f, 1.0f);
                    float sampleB = terrain.sample(coordB);

                    // Triangular crossfade: A fades out as B fades in
                    float wA = (crossfade < 0.5f) ? 1.0f : 2.0f * (1.0f - crossfade);
                    float wB = (crossfade < 0.5f) ? 2.0f * crossfade : 1.0f;
                    sample = (sampleA * wA + sampleB * wB) / (wA + wB);
                } else {
                    // Raw sample mode (no grain crossfade)
                    sample = terrain.sample(pitchCoord);
                }

                // Phase advancement: wavetables (one cycle per period) advance
                // by frequency/sampleRate; sample-based playback (Sampler etc)
                // advances by pitchScale/sampleRate, where pitchScale is the
                // transposition factor relative to the base note frequency.
                if (isWavetable) {
                    v.phase += effFreq / (float)sampleRate;
                } else {
                    // Read base note from param (default A4=69 if not set)
                    float baseNote = getParamByName(node, "Base Note", 69.0f);
                    float fineTune = getParamByName(node, "Fine Tune", 0.0f);
                    // Base frequency = MIDI note frequency + fine-tune in cents
                    float baseFreq = transport.noteToFreq((int)baseNote) *
                        std::pow(2.0f, fineTune / 1200.0f); // fine-tune in cents
                    float samplePitchScale = effFreq / std::max(1.0f, baseFreq);
                    v.phase += samplePitchScale / (float)sampleRate;
                }
                if (v.phase > 1.0f) v.phase -= 1.0f;
            } else {
                // WaveformPerPoint: terrain value modulates oscillator timbre
                float terrainVal = terrain.sample(coord);
                sample = std::sin(v.phase * 2.0f * 3.14159265f) * (0.5f + 0.5f * terrainVal);
                v.phase += effFreq / (float)sampleRate;
                if (v.phase > 1.0f) v.phase -= 1.0f;
            }

            totalSample += sample * env * v.velocity;
            activeVoiceCount++;
        }

        // Scale by active voice count to prevent clipping when many notes
        // play simultaneously. sqrt gives a perceptual balance between
        // loudness and avoiding distortion (pure 1/N would be too quiet).
        if (activeVoiceCount > 1)
            totalSample /= std::sqrt((float)activeVoiceCount);
        totalSample *= volume;
        totalSample = juce::jlimit(-1.0f, 1.0f, totalSample);

        for (int c = 0; c < numChannels; ++c)
            buf.addSample(c, s, totalSample);
    }
}

} // namespace SoundShop

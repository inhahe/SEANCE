#include "terrain_synth.h"
#include "builtin_synth.h" // for WaveExprParser
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <random>
#include <numeric>
#include <fstream>

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

    // Traversal param modulation from node params
    traversalParams.speed           = getParam(5, 1.0f);
    traversalParams.radiusX         = getParam(6, 0.3f);
    traversalParams.radiusY         = getParam(7, 0.3f);
    traversalParams.centerX         = getParam(8, 0.5f);
    traversalParams.centerY         = getParam(9, 0.5f);
    traversalParams.radiusModSpeed  = getParam(10, 0.0f);
    traversalParams.radiusModAmount = getParam(11, 0.0f);

    // Traversal mode: 0=Orbit, 1=Linear, 2=Lissajous, 3=Physics
    int modeInt = (int)getParam(12, 0.0f);
    traversalParams.mode = (modeInt == 1) ? TraversalMode::Linear
                         : (modeInt == 2) ? TraversalMode::Lissajous
                         : (modeInt == 3) ? TraversalMode::Physics
                         : TraversalMode::Orbit;

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
                voices[vi].active = true;
                voices[vi].noteNumber = msg.getNoteNumber();
                voices[vi].frequency = (float)juce::MidiMessage::getMidiNoteInHertz(msg.getNoteNumber());
                voices[vi].velocity = msg.getVelocity() / 127.0f;
                voices[vi].phase = 0;
                voices[vi].startBeat = transport.positionBeats();
                voices[vi].envStage = Voice::Attack;
                voices[vi].envLevel = 0;
                voices[vi].envTime = 0;
            }
        } else if (msg.isNoteOff()) {
            for (int i = 0; i < MAX_VOICES; ++i)
                if (voices[i].active && voices[i].noteNumber == msg.getNoteNumber()
                    && voices[i].envStage != Voice::Release) {
                    voices[i].envStage = Voice::Release;
                    voices[i].envTime = 0;
                }
        }
    }

    // Detect audio-rate signal inputs (channels 2+ on the buffer)
    int numSignalInputs = std::max(0, numChannels - 2);
    bool hasSignalInputs = numSignalInputs > 0;

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

        // Grain size in samples (0 = off)
        int grainSizeSamples = (grainSize > 0) ? std::max(1, (int)(grainSize * sampleRate)) : 0;

        for (int vi = 0; vi < MAX_VOICES; ++vi) {
            if (!voices[vi].active) continue;
            auto& v = voices[vi];
            float env = v.advanceEnv((float)sampleRate, attack, decay, sustain, release,
                                      &attackCurve, &decayCurve, &releaseCurve);
            if (!v.active) continue;

            float sample;
            if (mode == TerrainSynthMode::SamplePerPoint) {
                float pitchScale = v.frequency / 440.0f;
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

                v.phase += pitchScale / (float)sampleRate;
                if (v.phase > 1.0f) v.phase -= 1.0f;
            } else {
                // WaveformPerPoint: terrain value modulates oscillator timbre
                float terrainVal = terrain.sample(coord);
                sample = std::sin(v.phase * 2.0f * 3.14159265f) * (0.5f + 0.5f * terrainVal);
                v.phase += v.frequency / (float)sampleRate;
                if (v.phase > 1.0f) v.phase -= 1.0f;
            }

            totalSample += sample * env * v.velocity;
        }

        totalSample *= volume;
        totalSample = juce::jlimit(-1.0f, 1.0f, totalSample);

        for (int c = 0; c < numChannels; ++c)
            buf.addSample(c, s, totalSample);
    }
}

} // namespace SoundShop

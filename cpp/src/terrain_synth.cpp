#include "terrain_synth.h"
#include "signal_modulation.h"
#include "wavelet.h"
#include "builtin_synth.h" // for WaveExprParser
#include "fft_util.h"
#include "layered_wave_editor.h" // for LayeredWaveform decode/render
#include "spectral_editor.h"     // for SpectralDoc decode/render
#include "wavelet_paint.h"       // for __waveletpaint__: decode
#include "granular_frame.h"      // for GranularFrame side-table entries
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
// Terrain - N-dimensional sample data
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

void Terrain::fillValueNoise(int octaves, float persistence, unsigned int seed) {
    if (dims.empty() || data.empty()) return;
    int nd = (int)dims.size();
    octaves = juce::jlimit(1, 8, octaves);

    std::fill(data.begin(), data.end(), 0.0f);

    // Smallest axis sets the baseline coarseness: octave 0 has ~4 coarse
    // cells along the smallest axis, doubling each octave. Larger axes scale
    // proportionally so the texture stays roughly isotropic regardless of
    // terrain shape.
    int minDim = std::max(1, *std::min_element(dims.begin(), dims.end()));

    float amp = 1.0f;

    for (int oct = 0; oct < octaves; ++oct) {
        int baseCoarse = 4 << oct; // 4, 8, 16, 32, ...

        std::vector<int> coarseDims(nd);
        size_t coarseTotal = 1;
        for (int i = 0; i < nd; ++i) {
            int c = std::max(2, (int)std::round((float)dims[i] / (float)minDim
                                                * (float)baseCoarse));
            coarseDims[i] = c;
            coarseTotal *= (size_t)c;
        }

        // Generate this octave's coarse-grid random samples.
        std::mt19937 rng(seed + (unsigned int)oct * 7919u);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        std::vector<float> coarse(coarseTotal);
        for (auto& v : coarse) v = dist(rng);

        // For each terrain cell, do N-linear interpolation with a smoothstep
        // fade across the coarse grid. The smoothstep is what gives value
        // noise its characteristic organic look vs. plain bilinear (which
        // would produce visible diamond / triangle artifacts).
        std::vector<int> idx(nd, 0);
        const int numCorners = 1 << nd;
        int i0[8] = {0};
        float ft[8] = {0};

        size_t flat = 0;
        const size_t total = data.size();
        while (flat < total) {
            for (int i = 0; i < nd; ++i) {
                float norm = dims[i] > 1
                    ? (float)idx[i] / (float)(dims[i] - 1)
                    : 0.5f;
                float f = norm * (float)(coarseDims[i] - 1);
                int lo = (int)f;
                if (lo >= coarseDims[i] - 1) lo = coarseDims[i] - 2;
                if (lo < 0) lo = 0;
                float t = f - (float)lo;
                t = t * t * (3.0f - 2.0f * t); // smoothstep fade
                i0[i] = lo;
                ft[i] = t;
            }

            float result = 0.0f;
            for (int c = 0; c < numCorners; ++c) {
                float w = 1.0f;
                size_t flatC = 0;
                size_t stride = 1;
                for (int i = nd - 1; i >= 0; --i) {
                    int corner = (c >> i) & 1;
                    int ci = i0[i] + corner;
                    w *= corner ? ft[i] : (1.0f - ft[i]);
                    flatC += (size_t)ci * stride;
                    stride *= (size_t)coarseDims[i];
                }
                result += w * coarse[flatC];
            }
            data[flat] += amp * result;

            ++flat;
            for (int d = nd - 1; d >= 0; --d) {
                if (++idx[d] < dims[d]) break;
                idx[d] = 0;
            }
        }

        amp *= persistence;
    }

    // Peak-normalize to [-1, 1].
    float maxAbs = 0.0f;
    for (auto v : data) maxAbs = std::max(maxAbs, std::abs(v));
    if (maxAbs > 0.0f) {
        float scale = 1.0f / maxAbs;
        for (auto& v : data) v *= scale;
    }
}

void Terrain::fillFromExpression(const std::string& expr) {
    if (dims.empty() || data.empty()) return;
    int nd = (int)dims.size();

    // Simple expression evaluator - reuse WaveExprParser approach
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
            // Higher-dimension axis variables (5D..8D). Match the axis names
            // used by TerrainVisualizer's +Dim button: V, U, S, T. The
            // !isalpha guard prevents these from shadowing function names
            // that start with the same letter (sin, sqrt, tanh).
            if (*pos == 'v' && !std::isalpha(*(pos+1))) { pos++; return vars[4]; }
            if (*pos == 'u' && !std::isalpha(*(pos+1))) { pos++; return vars[5]; }
            if (*pos == 's' && !std::isalpha(*(pos+1))) { pos++; return vars[6]; }
            if (*pos == 't' && !std::isalpha(*(pos+1))) { pos++; return vars[7]; }
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
    // Kill DC offset - it would produce a silent bias on playback.
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

void Terrain::fillFromSpectralDoc(const SpectralDoc& doc) {
    // Round fftSize up to a power of two; matches the SpectralFrame
    // wavetable adapter, since both call renderSpectralToWaveform.
    int n = 2;
    while (n < doc.fftSize && n < 16384) n <<= 1;
    if (n < 2) n = 2;

    init({n});
    std::vector<float> timeDomain;
    renderSpectralToWaveform(doc, n, timeDomain);
    if ((int)data.size() == n && (int)timeDomain.size() == n)
        data = std::move(timeDomain);
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

void Terrain::fillFractal(int size, int iterations, float decay) {
    init({size});
    // Seed: a simple sine wave as the base pattern.
    for (int i = 0; i < size; ++i)
        data[i] = std::sin(2.0f * 3.14159265f * (float)i / (float)size);

    auto filt = getWaveletFilter("db4");

    // Forward DWT to get coefficients.
    std::vector<float> sig = data;
    int levels = dwt(sig, iterations, filt);

    // Replace each detail level's coefficients with a scaled, self-similar
    // copy of the approximation level - creating fractal repetition across
    // scales. Each level decays by `decay` to produce a 1/f-like spectrum.
    int approxLen = size;
    for (int l = 0; l < levels; ++l) approxLen /= 2;

    int bandStart = approxLen;
    for (int band = 0; band < levels; ++band) {
        int bandLen = approxLen * (1 << band);
        float scale = std::pow(decay, (float)(band + 1));
        // Fill this band with a stretched copy of the approximation × scale.
        for (int i = 0; i < bandLen; ++i) {
            int srcIdx = (i * approxLen) / bandLen; // stretch
            if (srcIdx < approxLen)
                sig[bandStart + i] = sig[srcIdx] * scale;
        }
        bandStart += bandLen;
    }

    // Inverse DWT to get the fractal waveform.
    idwt(sig, levels, filt);

    // Normalize to [-1, 1].
    float maxAbs = 0;
    for (auto v : sig) maxAbs = std::max(maxAbs, std::abs(v));
    if (maxAbs > 0) {
        for (int i = 0; i < size; ++i) data[i] = sig[i] / maxAbs;
    }

    fprintf(stderr, "Terrain fractal fill: %d samples, %d iterations\n", size, iterations);
}

void Terrain::buildMipmaps(int maxLevels) {
    mipmaps.clear();
    if (data.empty() || numDimensions() != 1) return;
    int n = (int)data.size();
    if ((n & (n - 1)) != 0) return; // must be power of 2

    auto filt = getWaveletFilter("db4");
    mipmaps.push_back(data); // level 0 = original

    std::vector<float> current = data;
    for (int l = 0; l < maxLevels && n >= (int)filt.h0.size() * 2; ++l) {
        // One DWT step: splits into approximation (half) + detail (half).
        dwtStep(current, n, filt);
        n /= 2;
        // The approximation is the first half - it's the low-pass filtered,
        // downsampled version (fewer harmonics, shorter table).
        std::vector<float> mip(current.begin(), current.begin() + n);
        // IDWT step to get the actual waveform (not coefficients).
        // We need to reconstruct from just the approximation (zero detail).
        std::vector<float> reconBuf(n * 2, 0.0f);
        for (int i = 0; i < n; ++i) reconBuf[i] = mip[i];
        idwtStep(reconBuf, n * 2, filt);
        // The reconstructed waveform is in reconBuf[0..n*2-1] but we want
        // it at half-resolution (n samples). Downsample by 2.
        std::vector<float> downsampled(n);
        for (int i = 0; i < n; ++i)
            downsampled[i] = reconBuf[std::min(i * 2, n * 2 - 1)];
        mipmaps.push_back(downsampled);
    }
}

float Terrain::sampleMipmap(float phase01, float pitchRatio) const {
    if (mipmaps.empty()) return sample({phase01}); // fallback
    // Pick level: log2(pitchRatio) rounded down, clamped to available levels.
    int level = 0;
    if (pitchRatio > 1.0f)
        level = std::min((int)std::floor(std::log2(pitchRatio)),
                         (int)mipmaps.size() - 1);
    const auto& mip = mipmaps[level];
    int n = (int)mip.size();
    if (n == 0) return 0;
    float idx = phase01 * n;
    int i0 = ((int)idx) % n;
    int i1 = (i0 + 1) % n;
    float frac = idx - (int)idx;
    return mip[i0] + (mip[i1] - mip[i0]) * frac;
}

void Terrain::toWaveletBasis(int levels) {
    if (isWaveletBasis) return;
    if (data.empty()) return;
    int n = (int)data.size();
    if ((n & (n - 1)) != 0) return; // must be power of 2
    auto filt = getWaveletFilter("db4");
    dwt(data, levels, filt);
    isWaveletBasis = true;
}

void Terrain::fromWaveletBasis(int levels) {
    if (!isWaveletBasis) return;
    if (data.empty()) return;
    auto filt = getWaveletFilter("db4");
    idwt(data, levels, filt);
    isWaveletBasis = false;
}

// ==============================================================================
// Traversal - maps time to N-dimensional coordinate
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
// EnvCurve - cached envelope shape table
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
    // Primary source: the unified node.ahdsrEnvelope SpectralCurves
    // (per-segment Attack / Decay / Release, with three authoring
    // modes: Equation / Drawn / Freehand). We bake each curve to a
    // 256-sample lookup table by calling SpectralCurve::evaluate.
    //
    // Legacy projects that still carry the old envAttackCurve (raw
    // expression strings) / envAttackPoints fields fall through to
    // those paths below. The menu handler that opens the envelope
    // editor mirrors changes back to the legacy fields so playback
    // doesn't desync during the migration window.
    auto bakeFromSpectral = [](const SpectralCurve& sc, EnvCurve& out) {
        auto samples = sc.evaluate(ENV_TABLE_SIZE);
        if ((int)samples.size() != ENV_TABLE_SIZE) {
            out.valid = false;
            return false;
        }
        out.table = std::move(samples);
        out.valid = true;
        return true;
    };

    // Attack
    if (!bakeFromSpectral(node.ahdsrEnvelope.attackCurve, attackCurve)) {
        if (!node.envAttackCurve.empty())
            attackCurve.buildFromExpression(node.envAttackCurve);
        else if (!node.envAttackPoints.empty())
            attackCurve.buildFromPoints(node.envAttackPoints);
        else
            attackCurve.valid = false;
    }
    // Hold (a 0..1 multiplier on peak across the hold window; default
    // flat "1"). No legacy fallback - the Hold curve is new and lives
    // only on the unified envelope.
    if (!bakeFromSpectral(node.ahdsrEnvelope.holdCurve, holdCurve))
        holdCurve.valid = false;
    // Decay
    if (!bakeFromSpectral(node.ahdsrEnvelope.decayCurve, decayCurve)) {
        if (!node.envDecayCurve.empty())
            decayCurve.buildFromExpression(node.envDecayCurve);
        else if (!node.envDecayPoints.empty())
            decayCurve.buildFromPoints(node.envDecayPoints);
        else
            decayCurve.valid = false;
    }
    // Release
    if (!bakeFromSpectral(node.ahdsrEnvelope.releaseCurve, releaseCurve)) {
        if (!node.envReleaseCurve.empty())
            releaseCurve.buildFromExpression(node.envReleaseCurve);
        else if (!node.envReleasePoints.empty())
            releaseCurve.buildFromPoints(node.envReleasePoints);
        else
            releaseCurve.valid = false;
    }
}

// ==============================================================================
// TerrainSynthProcessor::Voice
// ==============================================================================

float TerrainSynthProcessor::Voice::advanceEnv(float sr, float a, float h, float d, float s, float r,
                                                 const EnvCurve* aCurve, const EnvCurve* hCurve,
                                                 const EnvCurve* dCurve, const EnvCurve* rCurve) {
    if (envStage == Off) return 0.0f;
    float dt = 1.0f / sr;
    envTime += dt;
    // All shapes scale to envPeak so velocity sensitivity is honored
    // uniformly across stages. envPeak is set by the caller at note-on
    // from velSens * vel/127 + (1 - velSens).
    const float peak = envPeak;
    switch (envStage) {
        case Attack: {
            float t = (float)(envTime / std::max(0.001, (double)a));
            if (t >= 1.0f) {
                envLevel = peak;
                // Skip Hold entirely when its duration is zero - common
                // case for classic ADSR patches.
                envStage = (h > 0.0001f) ? Hold : Decay;
                envTime = 0;
            } else {
                float shape = (aCurve && aCurve->valid) ? aCurve->evaluate(t) : t;
                envLevel = shape * peak;
            }
            break;
        }
        case Hold: {
            float t = (float)(envTime / std::max(0.001, (double)h));
            if (t >= 1.0f) {
                // End the plateau at peak so Decay (which starts from peak)
                // continues without a discontinuity, whatever the hold
                // curve's final value.
                envLevel = peak;
                envStage = Decay;
                envTime = 0;
            } else {
                // hCurve is a 0..1 multiplier on peak (default flat 1).
                float shape = (hCurve && hCurve->valid) ? hCurve->evaluate(t) : 1.0f;
                envLevel = shape * peak;
            }
            break;
        }
        case Decay: {
            float t = (float)(envTime / std::max(0.001, (double)d));
            float sustainLevel = s * peak;
            if (t >= 1.0f) { envLevel = sustainLevel; envStage = Sustain; envTime = 0; }
            else {
                // dCurve goes 1 -> 0 across the segment; map onto
                // peak -> sustainLevel so the curve shape is preserved
                // when the user changes velocity.
                float shape = (dCurve && dCurve->valid) ? dCurve->evaluate(t) : (1.0f - t);
                envLevel = sustainLevel + (peak - sustainLevel) * shape;
            }
            break;
        }
        case Sustain:
            envLevel = s * peak;
            break;
        case Release: {
            float t = (float)(envTime / std::max(0.001, (double)r));
            if (t >= 1.0f) { envLevel = 0; envStage = Off; active = false; }
            else {
                float shape = (rCurve && rCurve->valid) ? rCurve->evaluate(t) : (1.0f - t);
                envLevel = shape * (s * peak);
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

void TerrainSynthProcessor::reloadIfScriptChanged() {
    // node.script is rewritten by UI-thread editors (setNodeScriptSynced) and
    // read here on the audio thread. A std::string assignment is not atomic,
    // so an unsynchronised read can see the new size with a stale/freed data
    // pointer - for a multi-MB granular wavetable that means memcpy'ing ~1.4 MB
    // from a garbage pointer and crashing. Take a locked snapshot off the
    // shared per-node mutex, then work off that copy so nothing else in
    // processBlock touches node.script concurrently with a writer.
    {
        std::lock_guard<std::mutex> lock(*node.auditionMutex);
        if (node.script == cachedScript) return;
        cachedScript = node.script;
    }

    // For now, only re-parse __layered__ scripts at runtime - other
    // script types (audio, image, wavetable) load files and don't
    // change during a session.
    if (cachedScript.find("__layered__:") == 0) {
        LayeredWaveform lw;
        if (lw.decode(cachedScript)) {
            std::vector<float> samples;
            lw.render(samples);
            terrain.init({(int)samples.size()});
            auto& d = terrain.getData();
            if ((int)d.size() == (int)samples.size())
                d = std::move(samples);
        }
    }
}

// Classify a Terrain Synth source by its script prefix. Mirrors the
// detection chain in the TerrainSynthProcessor constructor / reload path
// so the node-graph UI can filter the Synth Mode picker without holding
// a pointer to the live audio processor.
SynthSourceClass classifySynthSource(const std::string& script) {
    // Explicit source-type prefixes first.
    if (script.rfind("__audio__:", 0)         == 0) return SynthSourceClass::Sample;
    if (script.rfind("__image__:", 0)         == 0) return SynthSourceClass::Surface;
    if (script.rfind("__layered__:", 0)       == 0) return SynthSourceClass::Wavetable;
    if (script.rfind("__wavetable__:", 0)     == 0) return SynthSourceClass::Wavetable;
    if (script.rfind("__wavetable2__:", 0)    == 0) return SynthSourceClass::Wavetable;
    if (script.rfind("__wavetable3__:", 0)    == 0) return SynthSourceClass::Wavetable;
    if (script.rfind("__wavetable4__:", 0)    == 0) return SynthSourceClass::Wavetable;
    if (script.rfind("__wavetable5__:", 0)    == 0) return SynthSourceClass::Wavetable;
    if (script.rfind("__waveletpaint__:", 0)  == 0) return SynthSourceClass::Wavetable;
    if (script.rfind("__spectral__:", 0)      == 0) return SynthSourceClass::Wavetable;
    if (script.rfind("__spectral2__:", 0)     == 0) return SynthSourceClass::Wavetable;

    // Fractal value noise: classified by terrain dimensionality. The
    // script format is "__valuenoise__:D0,D1,...:OCTAVES:..." so we look
    // at how many comma-separated dims appear in the first ':'-segment.
    static const std::string kVN = "__valuenoise__:";
    if (script.rfind(kVN, 0) == 0) {
        size_t i = kVN.size();
        size_t j = script.find(':', i);
        std::string dims = (j == std::string::npos) ? script.substr(i)
                                                    : script.substr(i, j - i);
        int ndims = 1;
        for (char c : dims) if (c == ',') ++ndims;
        return (ndims <= 1) ? SynthSourceClass::Wavetable
                            : SynthSourceClass::Surface;
    }

    // Fallthrough: bare math expression. The processor auto-detects dims
    // from variable usage (y/z/w => N-D); we mirror the same naive scan
    // here so a script using `sin(x*y)` shows up as Surface.
    bool usesYZW = script.find('y') != std::string::npos
                || script.find('z') != std::string::npos
                || script.find('w') != std::string::npos;
    return usesYZW ? SynthSourceClass::Surface : SynthSourceClass::Wavetable;
}

SynthModeAvailability synthModeAvailabilityFor(SynthSourceClass cls) {
    SynthModeAvailability a;
    switch (cls) {
        case SynthSourceClass::Wavetable:
            a.direct       = true;
            a.amSine       = false;
            a.additiveBank = true;
            break;
        case SynthSourceClass::Sample:
            a.direct       = true;
            a.amSine       = false;
            a.additiveBank = false;
            break;
        case SynthSourceClass::Surface:
            a.direct       = false;
            a.amSine       = true;
            a.additiveBank = false;
            break;
    }
    return a;
}

TerrainSynthProcessor::TerrainSynthProcessor(Node& n, Transport& t) : node(n), transport(t) {
    auto& script = node.script;
    cachedScript = script;

    if (script.find("__image__:") == 0) {
        terrain.fillFromImage(script.substr(10));
    } else if (script.find("__audio__:") == 0) {
        terrain.fillFromAudioFile(script.substr(10));
        isAudioSample = true;
    } else if (script.find("__layered__:") == 0) {
        // Layered waveform - decode the layer list and sum into a 1D terrain.
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
    } else if (script.find("__wavetable__:")  == 0
               || script.find("__wavetable2__:") == 0
               || script.find("__wavetable3__:") == 0
               || script.find("__wavetable4__:") == 0
               || script.find("__wavetable5__:") == 0) {
        // N-dimensional wavetable - Grid mode builds an (N+1)-D terrain;
        // Scatter mode keeps frames in a flat list and computes a Wendland
        // RBF blend each block. WavetableDoc::decode handles the v5
        // library+cell format with colorIdx + per-frame gain (current) plus
        // auto-migrating __wavetable4__ (no gain), __wavetable3__ (pre-
        // colorIdx), __wavetable2__, and legacy __wavetable__ payloads.
        WavetableDoc doc;
        bool decoded = doc.decode(script);
        if (decoded && doc.mode == WavetableMode::Scatter && !doc.scatterFrames.empty()) {
            int ts = doc.tableSize;
            wtScatterFrameSamples.clear();
            wtScatterFramePositions.clear();
            wtGranularFrames.clear();
            for (auto& sf : doc.scatterFrames) {
                // Granular frames feed the granular layer, not the cycle
                // terrain: bake a zero-cycle for the cycle blend so the
                // cell contributes nothing through terrain.sample(), then
                // stash the source PCM + grain length in the side table.
                // We still keep an entry in wtScatterFrameSamples (zeroed)
                // so the per-block weight indexing aligns 1:1 across both
                // layers - simplifies the morph math.
                IWavetableFrame* w = doc.libraryFrameById(sf.waveformId);
                bool isGran = (w && std::string(w->typeId()) == "granular");
                std::vector<float> samples;
                if (w && !isGran) w->render(ts, samples);
                if ((int)samples.size() != ts) samples.resize(ts, 0.0f);
                wtScatterFrameSamples.push_back(std::move(samples));
                wtScatterFramePositions.push_back(sf.position);

                if (isGran) {
                    auto* gf = static_cast<GranularFrame*>(w);
                    GranularLayerEntry e;
                    e.source           = gf->source;
                    e.sourceSampleRate = gf->sourceSampleRate;
                    e.grainLength      = std::max(16, gf->grainLength);
                    e.embeddedPitchHz  = (gf->embeddedPitchHz > 0.0f)
                                          ? gf->embeddedPitchHz : 440.0f;
                    e.freezeMode       = (int)gf->freezeMode;
                    e.crossfadeSamples = std::max(0, gf->crossfadeSamples);
                    e.position         = sf.position;
                    wtGranularFrames.push_back(std::move(e));
                }
            }
            // 1D terrain - the per-block blend writes the active waveform
            // into terrain.data so the per-sample render path is unchanged.
            terrain.init({ts});
            wtScatter = true;
            wtScatterDims = doc.scatterDims;
            wtScatterRadius = doc.scatterRadius;
            wtAbsoluteBlend = doc.absoluteBlend;
            isWavetable = true;
            wtFrameCount = (int)wtScatterFrameSamples.size();
            wtNumDims = doc.scatterDims;
            wtEffectiveAxes = doc.effectiveAxes();
            traversalParams.mode = TraversalMode::Linear;
            mode = TerrainSynthMode::SamplePerPoint;
        } else if (decoded && !doc.cellWaveformIds.empty()) {
            int ts = doc.tableSize;

            // Build terrain dimensions: {tableSize, dim0, dim1, ...}
            std::vector<int> terrainDims = {ts};
            for (int d : doc.gridDims) terrainDims.push_back(std::max(1, d));
            terrain.init(terrainDims);
            auto& data = terrain.getData();
            wtGranularFrames.clear();

            // Occupancy mask over the morph axes only (gridDims, no phase
            // axis): 1.0 where a cell holds a frame, 0.0 where it's empty.
            // Used to renormalize the cycle sample over filled cells so empty
            // cells don't drain volume (unless wtAbsoluteBlend is on).
            wtAbsoluteBlend = doc.absoluteBlend;
            std::vector<int> occDims;
            for (int d : doc.gridDims) occDims.push_back(std::max(1, d));
            wtGridOccupancy.init(occDims);
            auto& occData = wtGridOccupancy.getData();
            wtGridHasEmptyCells = false;

            // Compute stride for each grid dimension to map flat frame index
            // to the correct position in the N-dimensional terrain.
            int nf = (int)doc.cellWaveformIds.size();
            for (int f = 0; f < nf; ++f) {
                // Granular cells bake zero into the terrain (cycle layer)
                // and instead register a side-table entry that the per-
                // voice OLA stream reads. Cycle frames bake their rendered
                // samples as usual.
                IWavetableFrame* w = doc.frameAt(f);
                bool isGran = (w && std::string(w->typeId()) == "granular");
                std::vector<float> samples;
                if (w && !isGran)
                    w->render(ts, samples);
                if ((int)samples.size() != ts) samples.resize(ts, 0.0f);

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

                // Record cell occupancy at this grid coord (cycle or granular
                // both count as filled; only a missing frame is "empty").
                {
                    bool filled = (w != nullptr);
                    if (!filled) wtGridHasEmptyCells = true;
                    int occFlat = wtGridOccupancy.coordToFlatIndex(gridCoord);
                    if (occFlat >= 0 && occFlat < wtGridOccupancy.totalSize())
                        occData[occFlat] = filled ? 1.0f : 0.0f;
                }

                for (int i = 0; i < ts && i < (int)samples.size(); ++i) {
                    std::vector<int> fullIdx = {i};
                    fullIdx.insert(fullIdx.end(), gridCoord.begin(), gridCoord.end());
                    int flatIdx = terrain.coordToFlatIndex(fullIdx);
                    if (flatIdx >= 0 && flatIdx < terrain.totalSize())
                        data[flatIdx] = samples[i];
                }

                if (isGran) {
                    auto* gf = static_cast<GranularFrame*>(w);
                    GranularLayerEntry e;
                    e.source           = gf->source;
                    e.sourceSampleRate = gf->sourceSampleRate;
                    e.grainLength      = std::max(16, gf->grainLength);
                    e.embeddedPitchHz  = (gf->embeddedPitchHz > 0.0f)
                                          ? gf->embeddedPitchHz : 440.0f;
                    e.freezeMode       = (int)gf->freezeMode;
                    e.crossfadeSamples = std::max(0, gf->crossfadeSamples);
                    // Normalize gridCoord into [0,1] per dim so the per-
                    // block Position weight math doesn't need to know about
                    // the underlying grid resolution.
                    e.position.assign(gridCoord.size(), 0.0f);
                    for (size_t d = 0; d < gridCoord.size(); ++d) {
                        int dim = std::max(1, doc.gridDims[d]);
                        e.position[d] = (dim <= 1) ? 0.0f
                            : (float)gridCoord[d] / (float)(dim - 1);
                    }
                    wtGranularFrames.push_back(std::move(e));
                }
            }
            isWavetable = true;
            wtFrameCount = nf;
            wtNumDims = doc.numDimensions();
            wtEffectiveAxes = doc.effectiveAxes();
        } else {
            terrain.init({2048});
            terrain.fillFromExpression("sin(x)");
        }
        traversalParams.mode = TraversalMode::Linear;
        mode = TerrainSynthMode::SamplePerPoint;
    } else if (script.find("__waveletpaint__:") == 0) {
        // Wavelet Space painter (#65): the script encodes a DWT coefficient
        // grid plus the filter/level/size header. IDWT it to a one-cycle
        // waveform and fill the 1D terrain. waveletPaintToWaveform handles
        // peak normalisation; size matches the declared totalSize.
        std::vector<float> coeffs;
        int nLevels = kWaveletPaintDefaultLevels;
        int nSize   = kWaveletPaintDefaultSize;
        std::string filt = kWaveletPaintDefaultFilter;
        if (decodeWaveletPaint(script, coeffs, nLevels, nSize, filt)) {
            auto wave = waveletPaintToWaveform(coeffs, nLevels, filt);
            terrain.init({(int)wave.size()});
            // Copy IDWT output into terrain data. terrain.init resizes
            // data to totalSize; we assume the IDWT produced exactly
            // that many samples.
            if ((int)terrain.getData().size() == (int)wave.size())
                terrain.getData() = std::move(wave);
        } else {
            // Couldn't parse - fall back to a silent 1D terrain.
            terrain.init({nSize});
            terrain.fillConstant(0.0f);
        }
        traversalParams.mode = TraversalMode::Linear;
        mode = TerrainSynthMode::SamplePerPoint;
        isWavetable = true;
        wtFrameCount = 1;
    } else if (script.find("__valuenoise__:") == 0) {
        // Fractal value noise terrain. Format:
        //   __valuenoise__:D0,D1,...:OCTAVES:PERSISTENCE:SEED
        // The terrain is filled by Terrain::fillValueNoise so the orbit
        // reads smooth, structured noise instead of independent samples
        // per cell (which would just degenerate into a noise oscillator).
        std::string body = script.substr(std::string("__valuenoise__:").size());
        auto split = [](const std::string& s, char sep) {
            std::vector<std::string> out; std::string cur;
            for (char ch : s) {
                if (ch == sep) { out.push_back(cur); cur.clear(); }
                else cur.push_back(ch);
            }
            out.push_back(cur);
            return out;
        };
        auto parts = split(body, ':');
        std::vector<int> tdims = {256, 256};
        int octaves = 4;
        float persistence = 0.55f;
        unsigned int seed = 42;
        if (parts.size() >= 1) {
            auto dimParts = split(parts[0], ',');
            tdims.clear();
            for (auto& p : dimParts) {
                int v = std::atoi(p.c_str());
                if (v > 0) tdims.push_back(v);
            }
            if (tdims.empty()) tdims = {256, 256};
        }
        if (parts.size() >= 2) octaves     = std::atoi(parts[1].c_str());
        if (parts.size() >= 3) persistence = (float)std::atof(parts[2].c_str());
        if (parts.size() >= 4) seed        = (unsigned int)std::strtoul(parts[3].c_str(), nullptr, 10);

        terrain.init(tdims);
        terrain.fillValueNoise(octaves, persistence, seed);

        // 1D noise plays as a noisy wavetable - use Linear traversal.
        // 2D+ uses the default Orbit traversal (set at construction).
        if ((int)tdims.size() == 1) {
            traversalParams.mode = TraversalMode::Linear;
            mode = TerrainSynthMode::SamplePerPoint;
        }
    } else if (script.find("__spectral__:") == 0
               || script.find("__spectral2__:") == 0)
    {
        // Both new (`__spectral2__:`) and legacy (`__spectral__:`) formats
        // decode through SpectralDoc, which maps the legacy phaseMode combo
        // to an equivalent Equation expression. From there we have two
        // SpectralCurves to evaluate and IFFT.
        SpectralDoc sd;
        if (!sd.decode(script))
            sd = SpectralDoc::defaultBuiltin();
        terrain.fillFromSpectralDoc(sd);
        traversalParams.mode = TraversalMode::Linear;
        mode = TerrainSynthMode::SamplePerPoint;
        isWavetable = true;
        wtFrameCount = 1;
    } else {
        std::string expr = script.empty() ? "sin(x)" : script;

        // Auto-detect dimensions from the expression:
        // If it uses y, z, w -> create higher-dimensional terrain
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

// Extract harmonic magnitudes and phases from the current 1D wavetable
// cycle, by FFT'ing it. Cached in partialBank so we only recompute when
// the cycle data changes (which we detect via a cheap fingerprint hash).
//
// The synthesised cycle is whatever terrain.data currently holds. For
// non-scatter grid wavetables we'd want to read a 1D slice at the current
// Position - but in practice the scatter blend code (see processBlock)
// already writes the active cycle into terrain.data each block in scatter
// mode, and grid wavetables that are 2D ({tableSize, nFrames}) need their
// slice extracted explicitly. We handle both cases here.
void TerrainSynthProcessor::refreshPartialBank() {
    // Decide what 1D cycle to analyse.
    std::vector<float> cycle;
    {
        const auto& dims = terrain.getDimensions();
        // The terrain exposes at(int) so we copy out into our own vector
        // (this allocation happens once per block when in AdditiveBank
        // mode - acceptable; the FFT below dominates the cost anyway).
        int total = terrain.totalSize();
        if (dims.size() <= 1 || !isWavetable) {
            // 1D terrain - the whole thing IS the cycle (or whatever scatter
            // blended into it).
            cycle.resize(total);
            for (int i = 0; i < total; ++i) cycle[i] = terrain.at(i);
        } else {
            // Multi-frame grid wavetable: extract a 1D slice at the current
            // Position by nearest-neighbour in the frame axis (good enough
            // for the additive analysis - the user can morph more smoothly
            // by switching to scatter mode where the blend is per-block).
            int tableSize = dims[0];
            int nFrames = (dims.size() > 1) ? dims[1] : 1;
            // Pick the Position param that drives geometric axis 0 (the frame
            // axis sliced here). Under the contiguous "Position 1..K" naming the
            // bare "Position" only exists when there's a single traversable axis,
            // so resolve by name from wtEffectiveAxes instead of assuming it.
            std::string posName = "Position";
            {
                const int K = (int)wtEffectiveAxes.size();
                for (int k = 0; k < K; ++k)
                    if (wtEffectiveAxes[k] == 0) {
                        posName = (K == 1) ? std::string("Position")
                                : std::string("Position ") + std::to_string(k + 1);
                        break;
                    }
            }
            float pos = juce::jlimit(0.0f, 1.0f,
                getParamByName(node, posName.c_str(), 0.5f));
            int fr = juce::jlimit(0, nFrames - 1, (int)std::round(pos * (nFrames - 1)));
            cycle.resize(tableSize);
            for (int i = 0; i < tableSize; ++i)
                cycle[i] = terrain.at(fr * tableSize + i);
        }
    }

    // Round cycle length down to the nearest power of two for the FFT.
    int n = (int)cycle.size();
    int fftN = 1;
    while ((fftN << 1) <= n) fftN <<= 1;
    if (fftN < 4) { partialBank.valid = false; return; }
    cycle.resize(fftN);

    // Cheap fingerprint to skip recompute when the cycle is unchanged.
    // Sums every 8th sample with an integer mix; if it matches the cached
    // hash we trust the cached partials.
    size_t h = (size_t)fftN;
    for (int i = 0; i < fftN; i += 8) {
        h = h * 1315423911u
          + (size_t)juce::roundToInt(cycle[i] * 16384.0f);
    }
    if (partialBank.valid && partialBank.cycleHash == h) return;
    partialBank.cycleHash = h;

    // FFT the cycle.
    FFT fft(fftN);
    std::vector<std::complex<float>> spec;
    fft.forwardReal(cycle, spec);
    // spec has fftN/2+1 bins. Bin k = k-th harmonic of the cycle (the cycle
    // IS one period of the fundamental, so bin k is at frequency k*f0 when
    // the wavetable is played at note frequency f0).

    int K = std::min(kAdditiveBankMaxPartials, (int)spec.size());
    partialBank.magnitude.assign(kAdditiveBankMaxPartials, 0.0f);
    partialBank.phase    .assign(kAdditiveBankMaxPartials, 0.0f);
    // Magnitudes are normalised by fftN so that summing them back as a
    // partial bank reproduces the cycle's amplitude (within rounding).
    // Factor of 2 because we're only keeping the positive-frequency half of
    // a real signal's spectrum.
    float norm = 2.0f / (float)fftN;
    for (int k = 1; k < K; ++k) { // skip DC (bin 0)
        float re = spec[k].real();
        float im = spec[k].imag();
        partialBank.magnitude[k] = std::sqrt(re*re + im*im) * norm;
        partialBank.phase[k]     = std::atan2(im, re);
    }
    partialBank.valid = true;
}

// Compute the per-granular-frame morph weight at the current Position.
// Mirrors the cycle layer's morph math so a granular frame and a cycle
// frame at the same wavetable position contribute equally to the output.
//
// Grid mode: N-linear interp weight of the current Position into the
// granular frame's grid cell. Identical to the weighting Terrain::sample
// applies to that cell - so the granular layer "stands in" for the cell's
// share of the morph, while terrain.sample only delivers the (zero)
// granular cell + the real contribution from neighbouring cycle cells.
//
// Scatter mode: Wendland RBF weight at the current Position, normalized
// by the total RBF weight (identical to the cycle blend's normalization).
std::vector<float> TerrainSynthProcessor::scatterQueryPosition() {
    std::vector<float> qpos(std::max(1, wtScatterDims), 0.5f);
    // Default every axis to the dots' shared coordinate on that axis (read
    // from frame 0). For a NON-traversable axis every dot shares the same
    // value, so pinning the query there makes that axis contribute zero to all
    // RBF distances - exactly what "this axis is inert" should mean. (Pinning
    // to a hardcoded 0.5 instead would add a constant |0.5 - shared| offset to
    // every distance, wrongly shrinking all weights when the dots sit off the
    // midpoint.) Traversable axes are then overwritten by their live Position
    // param below, so frame 0's value there is irrelevant.
    if (!wtScatterFramePositions.empty()) {
        const auto& p0 = wtScatterFramePositions.front();
        for (int d = 0; d < wtScatterDims && d < (int)p0.size(); ++d)
            qpos[(size_t)d] = p0[(size_t)d];
    }
    const int K = (int)wtEffectiveAxes.size();
    for (int k = 0; k < K; ++k) {
        const int axis = wtEffectiveAxes[k];
        std::string pname = (K == 1) ? std::string("Position")
                          : std::string("Position ") + std::to_string(k + 1);
        if (axis >= 0 && axis < (int)qpos.size())
            qpos[axis] = juce::jlimit(0.0f, 1.0f, getParamByName(node, pname.c_str(), 0.5f));
    }
    return qpos;
}

void TerrainSynthProcessor::updateGranularWeights() {
    // Live Position from the named params -> the shared, all-voices weights.
    // `pos` is always GEOMETRIC-axis-indexed so it lines up with the granular
    // frames' stored positions (gp[d] / fp[d] use the geometric axis order).
    std::vector<float> pos;
    if (wtScatter) {
        // Scatter: Position params exist only for traversable axes
        // (wtEffectiveAxes); non-traversable axes pin to 0.5. See
        // scatterQueryPosition() for the full rationale.
        pos = scatterQueryPosition();
    } else {
        // Grid: Position params exist only for traversable axes (numbered
        // contiguously); map the k-th param back to geometric axis
        // wtEffectiveAxes[k]. Inert axes keep 0 - the hat function special-cases
        // their single cell (dimSize<=1) and ignores the value.
        pos.assign(std::max(1, wtNumDims), 0.0f);
        const int K = (int)wtEffectiveAxes.size();
        for (int k = 0; k < K; ++k) {
            const int axis = wtEffectiveAxes[k];
            std::string pname = (K == 1) ? std::string("Position")
                              : std::string("Position ") + std::to_string(k + 1);
            if (axis >= 0 && axis < (int)pos.size())
                pos[axis] = juce::jlimit(0.0f, 1.0f, getParamByName(node, pname.c_str(), 0.0f));
        }
    }
    computeGranularWeights(pos, granWeights);
}

void TerrainSynthProcessor::computeGranularWeights(const std::vector<float>& pos,
                                                   std::vector<float>& out) {
    out.assign(wtGranularFrames.size(), 0.0f);
    if (wtGranularFrames.empty()) return;

    if (wtScatter) {
        // Wendland weights - normalized to sum-1 by default, matching the
        // cycle blend; in "distance fades volume" mode (wtAbsoluteBlend)
        // the raw weight is used as gain instead (invT = 1, no fallback).
        const float r = std::max(1e-3f, wtScatterRadius);
        float totalW = 0.0f;
        std::vector<float> wAll(wtScatterFrameSamples.size(), 0.0f);
        for (size_t fi = 0; fi < wtScatterFrameSamples.size(); ++fi) {
            const auto& fp = wtScatterFramePositions[fi];
            float d2 = 0.0f;
            for (int d = 0; d < wtScatterDims; ++d) {
                float a = (d < (int)fp.size()) ? fp[d] : 0.5f;
                float dd = a - (d < (int)pos.size() ? pos[d] : 0.5f);
                d2 += dd * dd;
            }
            float dist = std::sqrt(d2);
            if (dist < r) {
                float u = dist / r;
                float v = 1.0f - u;
                float w = v * v * v * v * (4.0f * u + 1.0f);
                wAll[fi] = w;
                totalW += w;
            }
        }
        // Edge case: zero total weight. Fall back to "nearest granular
        // frame wins" so the user always hears something when the
        // wavetable contains only granular frames. Skipped in absolute mode,
        // where a Position outside every radius is intentionally silent.
        if (!wtAbsoluteBlend && totalW < 1e-9f) {
            int nearest = -1;
            float bestD2 = 1e30f;
            for (size_t gi = 0; gi < wtGranularFrames.size(); ++gi) {
                const auto& gp = wtGranularFrames[gi].position;
                float d2 = 0.0f;
                for (int d = 0; d < wtScatterDims; ++d) {
                    float a = (d < (int)gp.size()) ? gp[d] : 0.5f;
                    float dd = a - (d < (int)pos.size() ? pos[d] : 0.5f);
                    d2 += dd * dd;
                }
                if (d2 < bestD2) { bestD2 = d2; nearest = (int)gi; }
            }
            if (nearest >= 0) out[nearest] = 1.0f;
            return;
        }
        // Map granular-frame entries to their per-frame weight in wAll.
        // The granular entries are pushed in the same order as the
        // scatter-frame iteration but we store position copies, so we
        // match by position equality - cheaper to just iterate both
        // arrays together since they were populated in lockstep skipping
        // cycle frames. Use position equality as the join key.
        const float invT = wtAbsoluteBlend ? 1.0f
                                           : (totalW > 1e-9f ? 1.0f / totalW : 0.0f);
        size_t gi = 0;
        for (size_t fi = 0; fi < wtScatterFrameSamples.size() && gi < wtGranularFrames.size(); ++fi) {
            const auto& fp = wtScatterFramePositions[fi];
            const auto& gp = wtGranularFrames[gi].position;
            if (fp.size() != gp.size()) continue;
            bool match = true;
            for (size_t d = 0; d < fp.size(); ++d) {
                if (std::abs(fp[d] - gp[d]) > 1e-6f) { match = false; break; }
            }
            if (match) {
                out[gi] = wAll[fi] * invT;
                ++gi;
            }
        }
        return;
    }

    // Grid mode: N-linear interp weight. For each granular frame, its
    // grid-coord position (already in [0,1]) yields a weight as the
    // product of per-dimension hat functions: max(0, 1 - |pos - gp| * (dim-1)).
    // This is exactly the weight the terrain's bilinear/N-linear interp
    // would apply to that grid cell.
    //
    // We need the original grid dims to know the cell spacing; we stored
    // wtNumDims but not the per-dim grid size. Derive it from the terrain
    // shape: terrainDims = {tableSize, d0, d1, ...} so dims[d+1] is the
    // grid size in dimension d.
    const auto& tdims = terrain.getDimensions();
    for (size_t gi = 0; gi < wtGranularFrames.size(); ++gi) {
        const auto& gp = wtGranularFrames[gi].position;
        float w = 1.0f;
        for (int d = 0; d < (int)gp.size() && (d + 1) < (int)tdims.size(); ++d) {
            const int   dimSize = std::max(1, tdims[d + 1]);
            // Single-cell axis: the lone cell covers the whole [0,1]
            // range of the Position param. With no neighbour to
            // interpolate to, weight is 1.0 regardless of pos[d] -
            // matching Terrain::sample, which special-cases dim=1 by
            // collapsing both interpolation corners onto index 0.
            // Without this, a 1x1x...x1 wavetable comes out at half
            // amplitude at the default Position=0.5, and goes completely
            // silent the moment the user moves Position to 1.0 (because
            // the cell's stored gp[d] is 0.0 and the hat function reads
            // h = 1 - |1 - 0| = 0).
            if (dimSize <= 1) continue;
            const float scale   = (float)(dimSize - 1);
            // |pos - gp| measured in cell units. Hat width = 1 cell.
            const float diff    = std::abs((d < (int)pos.size() ? pos[d] : 0.5f)
                                           - gp[d]) * scale;
            const float h       = std::max(0.0f, 1.0f - diff);
            w *= h;
        }
        out[gi] = w;
    }
}

int TerrainSynthProcessor::startVoice(int noteNumber, int channel, int velocity) {
    channel = juce::jlimit(1, 16, channel);
    // Allocate a free voice, else steal the quietest one.
    int vi = -1;
    float minLev = 999.0f;
    for (int i = 0; i < MAX_VOICES; ++i) {
        if (!voices[i].active) { vi = i; break; }
        if (voices[i].envLevel < minLev) { minLev = voices[i].envLevel; vi = i; }
    }
    if (vi < 0) return -1;

    auto& v = voices[vi];
    v.active = true;
    v.noteNumber = noteNumber;
    v.midiChannel = channel;
    v.baseFrequency = transport.noteToFreq(noteNumber);
    // Seed effective frequency with the current bend factor so notes
    // triggered while the pitch wheel is held start at the bent pitch.
    v.frequency = v.baseFrequency * pitchBendFactor[channel - 1];
    // Velocity sensitivity from the unified envelope (sens=0 -> always full
    // volume, organ-like; sens=1 -> linear velocity scaling, piano-like).
    // Fall back to the legacy "Vel Sens" param for old projects.
    {
        float velSens = node.ahdsrEnvelope.velocitySensitivity;
        velSens = getParamByName(node, "Vel Sens", velSens);
        float raw = juce::jlimit(0, 127, velocity) / 127.0f;
        // envPeak scales the envelope across every stage (adsr_envelope.cpp:
        // 740-742) - the single place velocity is applied to the voice level.
        // Do NOT multiply by a separate velocity factor downstream.
        v.envPeak = 1.0f - velSens * (1.0f - raw);
    }
    v.phase = 0;
    v.startBeat = transport.positionBeats();
    v.envStage = Voice::Attack;
    v.envLevel = 0;
    v.envTime = 0;
    v.sustainHeld = false;
    v.polyAftertouch = 0.0f;
    // Clear any audition Position override left over from a previous note on
    // this (reused) voice; the audition drain re-sets it for editor previews.
    v.hasAuditionPos = false;
    v.auditionPos.clear();
    return vi;
}

void TerrainSynthProcessor::releaseNote(int noteNumber, int channel) {
    channel = juce::jlimit(1, 16, channel);
    for (int i = 0; i < MAX_VOICES; ++i) {
        if (voices[i].active && voices[i].noteNumber == noteNumber
            && voices[i].envStage != Voice::Release) {
            // Sustain pedal held: defer release until the pedal comes up.
            if (sustainPedal[channel - 1]) {
                voices[i].sustainHeld = true;
            } else {
                voices[i].envStage = Voice::Release;
                voices[i].envTime = 0;
            }
        }
    }
}

void TerrainSynthProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) {
    applySignalModulations(node, buf);
    reloadIfScriptChanged();

    // TEMP DIAGNOSTIC (#88 Position modulation): accumulate the LFO signal and
    // the resulting Position value across every block, then dump min / max /
    // mean over the window ~twice a second. This measures the two reported
    // symptoms directly:
    //   * "favouring waveform 1" -> mean Position != 0.5, or a lopsided
    //     min..max range (e.g. 0.00..0.50 instead of 0.00..1.00).
    //   * "sudden jump to the right every cycle" -> max creeping upward over
    //     successive windows (a drift / accumulation bug) vs a stable range
    //     (the LFO shape's own endpoints not matching = expected).
    if (isWavetable && !node.modPins.empty()) {
        const auto& mp = node.modPins.front();
        int slotIdx = -1, sigCount = 0;
        for (auto& pin : node.pinsIn) {
            if (pin.id == mp.pinId) { slotIdx = sigCount; break; }
            if (pin.kind == PinKind::Signal || pin.kind == PinKind::Param) ++sigCount;
        }
        int ch = 2 + slotIdx;
        bool chOk = (slotIdx >= 0 && ch < buf.getNumChannels());
        float sig = chOk ? buf.getSample(ch, 0) : 0.0f;
        float pval = (mp.paramIndex >= 0 && mp.paramIndex < (int)node.params.size())
                        ? node.params[mp.paramIndex].value : 0.0f;

        static int   dbgN     = 0;
        static float sigMin    = 1e9f,  sigMax    = -1e9f,  sigSum    = 0.0f;
        static float valMin    = 1e9f,  valMax    = -1e9f,  valSum    = 0.0f;
        if (chOk) {
            sigMin = std::min(sigMin, sig); sigMax = std::max(sigMax, sig); sigSum += sig;
            valMin = std::min(valMin, pval); valMax = std::max(valMax, pval); valSum += pval;
            ++dbgN;
        }
        if (dbgN >= 90) {
            juce::String pname = (mp.paramIndex >= 0 && mp.paramIndex < (int)node.params.size())
                            ? juce::String(node.params[mp.paramIndex].name) : juce::String("?");
            juce::String s;
            s << "[WT-MOD] node=" << node.name << " bufCh=" << buf.getNumChannels()
              << " param=" << pname << " ch=" << ch
              << " | sig[min=" << juce::String(sigMin, 3) << " max=" << juce::String(sigMax, 3)
              << " mean=" << juce::String(sigSum / dbgN, 3) << "]"
              << " | pos[min=" << juce::String(valMin, 3) << " max=" << juce::String(valMax, 3)
              << " mean=" << juce::String(valSum / dbgN, 3) << "]";
            juce::Logger::writeToLog(s);
            dbgN = 0;
            sigMin = valMin = 1e9f; sigMax = valMax = -1e9f; sigSum = valSum = 0.0f;
        }
    }

    // Drain UI-side audition events directly into voices. This is how editor
    // preview buttons (e.g. the wavetable editor's GranularFrameEditor-
    // Component Play button) audition through the actual voice / envelope /
    // Volume path instead of the audio engine's separate preview mixer - so
    // the editor preview matches the eventual graph playback. We start voices
    // here rather than re-injecting MIDI so each audition note-on can carry a
    // per-voice wavetable Position override (ev.position): a frame's Play
    // button must audition THAT frame, regardless of where the live Position
    // knob sits. Plain MIDI / timeline notes go through the same startVoice/
    // releaseNote helpers below with no override.
    {
        std::lock_guard<std::mutex> lock(*node.auditionMutex);
        for (auto& ev : node.pendingAudition) {
            if (ev.isNoteOn) {
                int vi = startVoice(ev.pitch, 1, ev.velocity);
                if (vi >= 0 && !ev.position.empty()) {
                    voices[vi].hasAuditionPos = true;
                    voices[vi].auditionPos    = ev.position;
                }
            } else {
                releaseNote(ev.pitch, 1);
            }
        }
        node.pendingAudition.clear();
    }

    buf.clear();
    int numSamples = buf.getNumSamples();
    int numChannels = buf.getNumChannels();
    if (numChannels == 0) return;

    float attack  = getParam(0, 0.01f);
    float decay   = getParam(1, 0.1f);
    float sustain = getParam(2, 0.7f);
    float release = getParam(3, 0.3f);
    float volume  = getParam(4, 0.5f);

    // Traversal param modulation from node params - only override defaults
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

    // Synth mode: 0=Direct (SamplePerPoint), 1=AM-sine (WaveformPerPoint),
    // 2=Additive bank. Values are clamped via switch so future enum
    // additions don't crash older projects that have the param at an
    // unrecognised value. Then we clamp again against the source's
    // applicability (e.g. AM-sine on a wavetable -> Direct) so legacy
    // projects with a stale mode value still produce sound that matches
    // what the picker would offer for the current source.
    {
        int modeInt = juce::jlimit(0, 2, (int)getParam(13, 0.0f));
        TerrainSynthMode requested;
        switch (modeInt) {
            case 1:  requested = TerrainSynthMode::WaveformPerPoint; break;
            case 2:  requested = TerrainSynthMode::AdditiveBank;     break;
            default: requested = TerrainSynthMode::SamplePerPoint;   break;
        }
        // Use cachedScript (the audio-thread-owned snapshot refreshed by
        // reloadIfScriptChanged at the top of this block), never node.script
        // directly - reading the live string here would re-introduce the
        // UI/audio data race that reloadIfScriptChanged exists to avoid.
        SynthSourceClass cls = isAudioSample ? SynthSourceClass::Sample
                             : isWavetable   ? SynthSourceClass::Wavetable
                             : classifySynthSource(cachedScript);
        mode = synthModeAvailabilityFor(cls).clamp(requested);
    }
    // Additive-bank mode needs a fresh harmonic decomposition whenever the
    // wavetable cycle changes. Refresh once per block - the FFT is N log N
    // with N=1024 typical, so under 100us; not on the hot per-sample path.
    if (mode == TerrainSynthMode::AdditiveBank)
        refreshPartialBank();

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
            startVoice(msg.getNoteNumber(),
                       juce::jlimit(1, 16, msg.getChannel()),
                       msg.getVelocity());
        } else if (msg.isNoteOff()) {
            releaseNote(msg.getNoteNumber(),
                        juce::jlimit(1, 16, msg.getChannel()));
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
        } else if (msg.isChannelPressure()) {
            // Channel aftertouch: hardware keyboards send this when the
            // player presses harder on a key already held down. We store
            // per-channel and let the render loop multiply final volume
            // by (1 + sensitivity * aftertouch). The "Aftertouch" signal
            // input pin, when wired, overrides this with the signal's
            // sample value instead.
            int ch = juce::jlimit(1, 16, msg.getChannel());
            channelAftertouch[ch - 1] = (float)msg.getChannelPressureValue() / 127.0f;
        } else if (msg.isAftertouch()) {
            // Polyphonic aftertouch (per-note pressure). Store it on
            // every voice that currently holds the same note number on
            // the same channel. Treated as an additional layer on top
            // of channel aftertouch.
            int ch = juce::jlimit(1, 16, msg.getChannel());
            float v = (float)msg.getAfterTouchValue() / 127.0f;
            for (int i = 0; i < MAX_VOICES; ++i) {
                if (voices[i].active && voices[i].midiChannel == ch
                    && voices[i].noteNumber == msg.getNoteNumber()) {
                    voices[i].polyAftertouch = v;
                }
            }
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
        } else if (msg.isAllNotesOff()) {
            for (int i = 0; i < MAX_VOICES; ++i) {
                if (voices[i].active && voices[i].envStage != Voice::Release) {
                    voices[i].sustainHeld = false;
                    voices[i].envStage = Voice::Release;
                    voices[i].envTime = 0;
                }
            }
        } else if (msg.isAllSoundOff()) {
            for (int i = 0; i < MAX_VOICES; ++i)
                voices[i] = {};
        }
    }

    // Detect audio-rate signal inputs (channels 2+ on the buffer)
    int numSignalInputs = std::max(0, numChannels - 2);
    bool hasSignalInputs = numSignalInputs > 0;

    // Find the "Aftertouch" input pin among the node's control inputs
    // and read its current block-mean value. Control inputs map to audio
    // buffer channels starting at channel 2, in the order they appear in
    // node.pinsIn. The slot index must count BOTH Signal and Param pins,
    // exactly the way graph_processor assigns control channels (it routes
    // Signal *and* Param input pins onto channels 2+). Counting only Signal
    // pins here mis-mapped the channel once the Position modulation pins -
    // and the Aftertouch pin itself - became block-rate Param, reading a
    // neighbouring pin's data as aftertouch. When unwired (no node provides
    // this channel) the channel stays at silence and we fall back to MIDI
    // channel-pressure. We use the block mean rather than per-sample so a
    // slow LFO drives a smooth aftertouch rather than carrying its audio
    // shape into the amplitude swell - which is exactly why this pin is a
    // block-rate Param, not an audio-rate Signal.
    {
        aftertouchOverride = -1.0f;
        int sigIdx = 0;
        int targetSigIdx = -1;
        for (auto& p : node.pinsIn) {
            if (p.kind != PinKind::Signal && p.kind != PinKind::Param) continue;
            if (p.name == "Aftertouch") { targetSigIdx = sigIdx; break; }
            ++sigIdx;
        }
        if (targetSigIdx >= 0) {
            int chan = 2 + targetSigIdx;
            if (chan < numChannels) {
                const float* data = buf.getReadPointer(chan);
                double acc = 0.0;
                for (int s = 0; s < numSamples; ++s) acc += std::abs(data[s]);
                float mean = (numSamples > 0) ? (float)(acc / numSamples) : 0.0f;
                // Only treat the pin as "wired" when the channel
                // actually carries non-zero data. This keeps the
                // MIDI-pressure fallback live in the common case of a
                // dangling pin (no cable plugged in).
                if (mean > 1e-6f)
                    aftertouchOverride = juce::jlimit(0.0f, 1.0f, mean);
            }
        }
    }

    // Granular layer: refresh the per-frame morph weights from the current
    // Position so the per-sample voice loop can mix in Σ wGran[i] *
    // olaStream[i] on top of the cycle terrain.
    updateGranularWeights();

    // Wavetable-editor audition voices select their frame from a per-voice
    // Position override rather than the live Position. Compute each such
    // voice's own weight set once per block (cheap - one pass over the
    // granular frames) so the per-sample loop can read them without touching
    // the shared granWeights. Only audition voices pay this.
    if (!wtGranularFrames.empty()) {
        for (int vi = 0; vi < MAX_VOICES; ++vi) {
            auto& v = voices[vi];
            if (v.active && v.hasAuditionPos && !v.auditionPos.empty())
                computeGranularWeights(v.auditionPos, v.auditionWeights);
        }
    }

    // Scatter wavetable: blend frames into the 1D terrain at block start
    // using a Wendland C^2 RBF over the current Position. The per-sample
    // path then reads terrain.sample(phase) unchanged.
    if (isWavetable && wtScatter && !wtScatterFrameSamples.empty()) {
        std::vector<float> qpos = scatterQueryPosition();
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
        float invT;
        if (wtAbsoluteBlend) {
            // "Distance fades volume": use the raw Wendland weight as gain.
            // No nearest-frame fallback - a Position outside every frame's
            // radius is deliberately silent (totalW stays 0, the weighted
            // accumulation below produces no output).
            invT = 1.0f;
        } else {
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
            invT = 1.0f / totalW;
        }
        auto& tdata = terrain.getData();
        int ts = (int)tdata.size();

        // Wavelet-domain morphing (#52): instead of blending raw samples
        // (which can cause spectral smearing), decompose each contributing
        // frame via DWT, blend the wavelet coefficients with the same
        // Wendland weights, then reconstruct a single blended waveform.
        // Falls back to time-domain blend if the table size isn't a power
        // of 2 or if wavelet decomposition fails.
        bool useWaveletMorph = (ts >= 8) && ((ts & (ts - 1)) == 0);
        if (useWaveletMorph) {
            auto filt = getWaveletFilter("db2");
            int maxLevels = 4;

            // Accumulate weighted wavelet coefficients.
            std::vector<float> blended(ts, 0.0f);
            for (int fi = 0; fi < nFrames; ++fi) {
                if (weights[fi] <= 0.0f) continue;
                float w = weights[fi] * invT;
                const auto& src = wtScatterFrameSamples[fi];
                std::vector<float> coeffs(ts, 0.0f);
                int n = std::min((int)src.size(), ts);
                for (int i = 0; i < n; ++i) coeffs[i] = src[i];
                dwt(coeffs, maxLevels, filt);
                for (int i = 0; i < ts; ++i) blended[i] += w * coeffs[i];
            }
            // Inverse DWT to get the blended waveform.
            idwt(blended, maxLevels, filt);
            for (int i = 0; i < ts; ++i) tdata[i] = blended[i];
        } else {
            // Time-domain fallback (original behavior).
            std::fill(tdata.begin(), tdata.end(), 0.0f);
            for (int fi = 0; fi < nFrames; ++fi) {
                if (weights[fi] <= 0.0f) continue;
                float w = weights[fi] * invT;
                const auto& src = wtScatterFrameSamples[fi];
                int n = std::min((int)src.size(), ts);
                for (int i = 0; i < n; ++i) tdata[i] += w * src[i];
            }
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
            // phase axis - only v.phase (driven by note pitch) should sweep
            // the wavetable. Otherwise the Linear traversal's beat-based
            // motion adds an unwanted slow modulation that sounds like noise.
            if (!coord.empty()) coord[0] = 0.0f;
            // Scatter mode: terrain is 1D, blend already happened pre-loop -
            // nothing to write into coord[d+1] (would crash, no such dim).
            if (!wtScatter) {
                // Grid: coord[axis+1] is geometric position axis `axis`. Default
                // every position axis to 0 (inert single-cell axes only have
                // index 0, and Terrain::sample collapses them onto 0 anyway),
                // then drive each *traversable* axis from its Position param.
                // Position params are numbered contiguously over the traversable
                // axes; wtEffectiveAxes[k] is the geometric axis the k-th param
                // controls. Naming matches syncPositionParams(): "Position" for a
                // lone traversable axis, "Position 1".."Position K" otherwise.
                for (int d = 1; d < nd; ++d) coord[d] = 0.0f;
                const int K = (int)wtEffectiveAxes.size();
                for (int k = 0; k < K; ++k) {
                    const int axis = wtEffectiveAxes[k];
                    if (axis + 1 >= nd) continue;
                    std::string pname = (K == 1) ? std::string("Position")
                        : std::string("Position ") + std::to_string(k + 1);
                    coord[axis + 1] = juce::jlimit(0.0f, 1.0f,
                        getParamByName(node, pname.c_str(), 0.0f));
                }
            }
        }

        // Override coordinates with the explicit per-axis "Sig <axis>" signal
        // inputs (the coordinate-driver pins created on Surface terrain synths
        // - "Sig X", "Sig Y", ...). The Nth such pin maps onto coord[N].
        //
        // This must be PIN-AWARE, not a blind "channel 2+si -> coord[si]" map:
        // tonal synths now also carry an "Aftertouch" Signal pin (#78) and the
        // on-demand "Mod: ..." modulation pins (#88), all of which occupy
        // control channels (2,3,4,...) interleaved with any Sig pins. Those
        // pins are NOT coordinate drivers - Aftertouch feeds the amplitude
        // swell and Mod pins modulate named params via applySignalModulations
        // at the top of the block (which is how a wired LFO already reaches the
        // wavetable Position). Treating their channels as coordinates was the
        // bug that made a "Mod: Position" LFO land on the phase axis instead of
        // the frame position (no audible Position movement, subtle per-note
        // timbre wobble). We walk pinsIn counting control slots exactly the way
        // graph_processor routes them (Signal+Param pins -> channels 2+), and
        // only act on the "Sig " axis pins.
        if (hasSignalInputs) {
            int slot = 0;   // control-slot index; buffer channel = 2 + slot
            int axis = 0;   // which terrain coordinate the next Sig pin drives
            for (auto& p : node.pinsIn) {
                if (p.kind != PinKind::Signal && p.kind != PinKind::Param)
                    continue;
                if (p.name.rfind("Sig ", 0) == 0 && axis < nd) {
                    int ch = 2 + slot;
                    if (ch < numChannels) {
                        float sigVal = buf.getSample(ch, s);
                        coord[axis] = juce::jlimit(0.0f, 1.0f, (sigVal + 1.0f) * 0.5f);
                    }
                    ++axis;
                }
                ++slot;
            }
        }

        // Graintable: freeze overrides traversal position
        if (grainFreeze && !coord.empty())
            coord[0] = freezePosition;

        if (s == numSamples / 2)
            lastPosition = coord;

        // Grid renormalization gain: divide the cycle sample by the fraction
        // of interpolation weight that lands on *filled* cells, so empty cells
        // don't drain volume. Computed once per sample from the morph
        // coordinate (coord[1..]) - the phase axis (coord[0]) is excluded.
        // Skipped when absolute blend is on (empty cells should fade volume)
        // or when there are no empty cells (the mask is all-ones, gain == 1).
        float gridRenormGain = 1.0f;
        if (isWavetable && !wtScatter && !wtAbsoluteBlend && wtGridHasEmptyCells
            && wtGridOccupancy.totalSize() > 0 && (int)coord.size() >= 2) {
            std::vector<float> occCoord(coord.begin() + 1, coord.end());
            float occ = wtGridOccupancy.sample(occCoord);
            if (occ > 1e-4f) gridRenormGain = 1.0f / occ;
        }

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
            // Read AHDSR values: prefer node.ahdsrEnvelope (the new
            // authoritative field), but the existing per-block local
            // copies of attack/decay/sustain/release - read once from
            // params[0..3] - are kept as fallback / migration path so
            // old projects still play. holdMs is new and lives only on
            // the unified envelope.
            float hold = node.ahdsrEnvelope.holdMs * 0.001f;
            float env = v.advanceEnv((float)sampleRate, attack, hold, decay, sustain, release,
                                      &attackCurve, &holdCurve, &decayCurve, &releaseCurve);
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

                // Renormalize the cycle sample over filled grid cells so
                // empty cells don't drain volume (no-op when gain == 1).
                sample *= gridRenormGain;

                // ---- Granular layer mix ----
                //
                // For each granular frame with non-trivial morph weight,
                // sustain the captured marker window as a held note. The
                // algorithm MUST match what the capture/freeze dialog
                // auditions (audio_engine.cpp granularSample) so that
                // "what you audition = what you get". The audition plays
                // a CrossfadeLoop for every freeze mode today (its
                // `(void)freezeMode` fallback), so the synth does too;
                // the only thing the synth adds is pitch tracking, since
                // the audition plays at the captured pitch while a synth
                // note must follow MIDI. When the audition grows per-mode
                // implementations (AsyncGranular, ...), add the matching
                // synth branch here in lockstep - never let the two
                // diverge, or the user hears one thing and plays another.
                if (!wtGranularFrames.empty() && isWavetable) {
                    // Lazy-allocate the per-voice granular stream array on
                    // first use - voices that never hit a granular cell
                    // never pay the allocation.
                    if ((int)v.granStreams.size() != (int)wtGranularFrames.size())
                        v.granStreams.resize(wtGranularFrames.size());

                    // Audition voices (wavetable editor Play) use their own
                    // per-voice weights so they play the edited frame; all
                    // other voices share the live-Position granWeights.
                    const float* gw =
                        (v.hasAuditionPos
                         && v.auditionWeights.size() == wtGranularFrames.size())
                            ? v.auditionWeights.data()
                            : granWeights.data();

                    for (size_t gi = 0; gi < wtGranularFrames.size(); ++gi) {
                        const float w = gw[gi];
                        if (w <= 1e-5f) continue;

                        const auto& gf = wtGranularFrames[gi];
                        auto& gs = v.granStreams[gi];
                        const int srcLen   = (int)gf.source.size();
                        const int grainLen = std::max(16, gf.grainLength);
                        if (srcLen <= 0) continue;

                        // CrossfadeLoop geometry, identical to the
                        // audition: loop the L-sample window [anchor,
                        // anchor+L) and Hann-crossfade the seam against the
                        // L/2-sample lookahead tail [anchor+L, anchor+L+xf).
                        // The anchor is centered so the loop reads the
                        // marker spot. If the source is too short to hold
                        // a loop plus its lookahead tail the audition would
                        // be silent (grainViable==false), so the synth
                        // contributes nothing too - same gate, same result.
                        const int reservedTail = grainLen / 2;
                        const int needLen      = grainLen + reservedTail;
                        if (srcLen < needLen) continue;
                        const int maxStart = srcLen - needLen;
                        const int anchor   = maxStart / 2;
                        const int xfade = std::max(0,
                            std::min(gf.crossfadeSamples, grainLen / 2));

                        if (!gs.initialized) {
                            gs.loopPhase = 0.0f;
                            gs.initialized = true;
                        }

                        // Pitch ratio: resample the whole loop so the held
                        // note tracks MIDI. ratio == 1 (loop plays at its
                        // native rate, exactly the audition) when the note
                        // equals the frame's embedded pitch and the source
                        // and device sample rates match. srRatio corrects
                        // for a source captured at a different rate than the
                        // device (e.g. a 48k song-render on a 44.1k device).
                        const float srRatio = (gf.sourceSampleRate > 0.0)
                            ? (float)(gf.sourceSampleRate / sampleRate)
                            : 1.0f;
                        const float ratio = (effFreq /
                            std::max(1e-3f, gf.embeddedPitchHz)) * srRatio;

                        const float* src = gf.source.data();
                        const float p = gs.loopPhase;          // [0, grainLen)

                        // Main playhead read (linear interp).
                        const float mainF = (float)anchor + p;
                        int   mi0 = (int)mainF;
                        float mfr = mainF - (float)mi0;
                        if (mi0 < 0)               { mi0 = 0; mfr = 0.0f; }
                        else if (mi0 > srcLen - 1) { mi0 = srcLen - 1; mfr = 0.0f; }
                        const int mi1 = std::min(mi0 + 1, srcLen - 1);
                        float out = src[(size_t)mi0] * (1.0f - mfr)
                                  + src[(size_t)mi1] * mfr;

                        // Seam crossfade over the first `xfade` samples of
                        // each loop iteration. alpha rises 0->1 (Hann half-
                        // cycle); the pre-seam tail fades out as the loop
                        // start fades in, so the wrap is click-free.
                        if (xfade > 0 && p < (float)xfade) {
                            const float pi = juce::MathConstants<float>::pi;
                            const float alpha = 0.5f * (1.0f -
                                std::cos(pi * p / (float)xfade));
                            const float tailF = (float)(anchor + grainLen) + p;
                            int   ti0 = (int)tailF;
                            float tfr = tailF - (float)ti0;
                            if (ti0 < 0)               { ti0 = 0; tfr = 0.0f; }
                            else if (ti0 > srcLen - 1) { ti0 = srcLen - 1; tfr = 0.0f; }
                            const int ti1 = std::min(ti0 + 1, srcLen - 1);
                            const float tail = src[(size_t)ti0] * (1.0f - tfr)
                                             + src[(size_t)ti1] * tfr;
                            out = alpha * out + (1.0f - alpha) * tail;
                        }

                        // Advance the fractional playhead by the pitch
                        // ratio and wrap at the loop length.
                        float np = p + ratio;
                        while (np >= (float)grainLen) np -= (float)grainLen;
                        while (np < 0.0f)             np += (float)grainLen;
                        gs.loopPhase = np;

                        // Same empty-cell renormalization the cycle layer
                        // gets, so a granular frame next to empty cells stays
                        // full-volume too (no-op when gain == 1).
                        sample += w * out * gridRenormGain;
                    }
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
            } else if (mode == TerrainSynthMode::WaveformPerPoint) {
                // AM-sine: a sine carrier at the played pitch, amplitude-
                // modulated by the terrain value at the current traversal
                // coordinate. The terrain isn't the sound - it's a slow
                // amplitude envelope on top of a pure tone. Useful (and the
                // only meaningful mode) for non-1D terrains where Direct
                // mode would be noise.
                float terrainVal = terrain.sample(coord);
                sample = std::sin(v.phase * 2.0f * 3.14159265f) * (0.5f + 0.5f * terrainVal);
                v.phase += effFreq / (float)sampleRate;
                if (v.phase > 1.0f) v.phase -= 1.0f;
            } else {
                // Additive bank: sum N independent sine partials at
                // fundamental*k, k=1..K. Magnitudes/phases come from a
                // (cached) FFT of the wavetable cycle.
                if ((int)v.partialPhases.size() < kAdditiveBankMaxPartials)
                    v.partialPhases.assign(kAdditiveBankMaxPartials, 0.0f);

                sample = 0.0f;
                // Anti-aliasing: silence any partial whose frequency exceeds
                // Nyquist (the wavetable's FFT will already have low values
                // for those bins in most cases, but this also clamps
                // user-set pitches that push partials past Nyquist).
                float nyquist = 0.5f * (float)sampleRate;
                const int K = (int)partialBank.magnitude.size();
                const float TWO_PI = 6.28318530718f;
                for (int k = 1; k < K; ++k) {
                    float mag = partialBank.magnitude[k];
                    if (mag <= 1e-5f) continue;
                    float partialFreq = effFreq * (float)k;
                    if (partialFreq >= nyquist) break; // higher partials are too
                    sample += mag * std::sin(TWO_PI * v.partialPhases[k]
                                              + partialBank.phase[k]);
                    // Advance this partial's phase.
                    v.partialPhases[k] += partialFreq / (float)sampleRate;
                    if (v.partialPhases[k] > 1.0f)
                        v.partialPhases[k] -= std::floor(v.partialPhases[k]);
                }
            }

            // Aftertouch volume swell. Channel aftertouch + poly
            // aftertouch combine (capped at 1) so per-note pressure
            // adds on top of the channel-wide value. When the synth's
            // "Aftertouch" input pin is wired, aftertouchOverride
            // replaces channel aftertouch with the wired signal.
            float chanAT = (aftertouchOverride >= 0.0f) ? aftertouchOverride
                                                       : channelAftertouch[v.midiChannel - 1];
            float at = juce::jlimit(0.0f, 1.0f, chanAT + v.polyAftertouch);
            float atMul = 1.0f + node.aftertouchSensitivity * at;
            // env already includes velocity scaling via envPeak (see
            // adsr_envelope.cpp:221) - do not multiply by velocity again.
            totalSample += sample * env * atMul;
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

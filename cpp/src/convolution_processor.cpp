#define _USE_MATH_DEFINES
#include "convolution_processor.h"
#include <cmath>
#include <algorithm>
#include <sstream>

namespace SoundShop {

// ==============================================================================
// IR encode/decode
// ==============================================================================

std::string ConvolutionProcessor::encodeIR(const std::vector<float>& ir) {
    std::ostringstream o;
    o << "__convolution__:" << ir.size();
    for (float s : ir) o << "," << s;
    return o.str();
}

std::vector<float> ConvolutionProcessor::decodeIR(const std::string& script) {
    std::vector<float> ir;
    if (script.rfind("__convolution__:", 0) != 0) return ir;
    std::string body = script.substr(16);
    // First token = length, rest = samples separated by commas
    size_t pos = 0;
    auto nextToken = [&]() -> std::string {
        size_t comma = body.find(',', pos);
        if (comma == std::string::npos) comma = body.size();
        std::string tok = body.substr(pos, comma - pos);
        pos = comma + 1;
        return tok;
    };
    int len = 0;
    try { len = std::stoi(nextToken()); } catch (...) { return ir; }
    ir.reserve(len);
    for (int i = 0; i < len && pos <= body.size(); ++i) {
        try { ir.push_back(std::stof(nextToken())); }
        catch (...) { ir.push_back(0.0f); }
    }
    return ir;
}

// ==============================================================================
// Preset IR generators
// ==============================================================================

static void applyHannWindow(std::vector<float>& ir) {
    int n = (int)ir.size();
    for (int i = 0; i < n; ++i)
        ir[i] *= 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * i / (n - 1)));
}

std::vector<float> ConvolutionProcessor::generateLowpass(float cutoffHz, int order,
                                                          double sampleRate) {
    // Windowed-sinc lowpass filter
    int len = order * 2 + 1; // odd length, symmetric
    len = std::max(3, std::min(len, 4096));
    std::vector<float> ir(len);
    float fc = cutoffHz / (float)sampleRate; // normalized cutoff
    int mid = len / 2;
    for (int i = 0; i < len; ++i) {
        int n = i - mid;
        if (n == 0)
            ir[i] = 2.0f * fc;
        else
            ir[i] = std::sin(2.0f * (float)M_PI * fc * n) / ((float)M_PI * n);
    }
    applyHannWindow(ir);
    // Normalize to unity gain at DC
    float sum = 0;
    for (float s : ir) sum += s;
    if (std::abs(sum) > 1e-6f)
        for (float& s : ir) s /= sum;
    return ir;
}

std::vector<float> ConvolutionProcessor::generateHighpass(float cutoffHz, int order,
                                                           double sampleRate) {
    auto lp = generateLowpass(cutoffHz, order, sampleRate);
    // Spectral inversion: negate all samples, add 1 to center
    for (auto& s : lp) s = -s;
    lp[lp.size() / 2] += 1.0f;
    return lp;
}

std::vector<float> ConvolutionProcessor::generateBandpass(float centerHz, float bwHz,
                                                           int order, double sampleRate) {
    auto lp = generateLowpass(centerHz + bwHz * 0.5f, order, sampleRate);
    auto hp = generateHighpass(centerHz - bwHz * 0.5f, order, sampleRate);
    return convolveIRs(lp, hp);
}

std::vector<float> ConvolutionProcessor::generateEcho(float delayMs, float feedback,
                                                       int numEchoes, double sampleRate) {
    int delaySamples = std::max(1, (int)(delayMs * sampleRate / 1000.0));
    int totalLen = delaySamples * numEchoes + 1;
    totalLen = std::min(totalLen, (int)(sampleRate * 5)); // cap at 5 seconds
    std::vector<float> ir(totalLen, 0.0f);
    float gain = 1.0f;
    for (int i = 0; i <= numEchoes; ++i) {
        int pos = i * delaySamples;
        if (pos < totalLen)
            ir[pos] = gain;
        gain *= feedback;
    }
    return ir;
}

// ==============================================================================
// IR combination
// ==============================================================================

std::vector<float> ConvolutionProcessor::convolveIRs(const std::vector<float>& a,
                                                      const std::vector<float>& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    int outLen = (int)a.size() + (int)b.size() - 1;
    // Use FFT for efficiency
    int fftN = 1;
    while (fftN < outLen) fftN <<= 1;

    FFT fft(fftN);
    std::vector<std::complex<float>> freqA(fftN / 2 + 1), freqB(fftN / 2 + 1);

    // Zero-pad and FFT both
    std::vector<float> padA(fftN, 0.0f), padB(fftN, 0.0f);
    for (int i = 0; i < (int)a.size(); ++i) padA[i] = a[i];
    for (int i = 0; i < (int)b.size(); ++i) padB[i] = b[i];
    fft.forwardReal(padA, freqA);
    fft.forwardReal(padB, freqB);

    // Multiply in frequency domain
    for (int i = 0; i < (int)freqA.size(); ++i)
        freqA[i] *= freqB[i];

    // IFFT back
    std::vector<float> result;
    fft.inverseReal(freqA, result);
    result.resize(outLen);
    return result;
}

std::vector<float> ConvolutionProcessor::sumIRs(const std::vector<float>& a,
                                                 const std::vector<float>& b) {
    int len = std::max(a.size(), b.size());
    std::vector<float> result(len, 0.0f);
    for (int i = 0; i < (int)a.size(); ++i) result[i] += a[i];
    for (int i = 0; i < (int)b.size(); ++i) result[i] += b[i];
    return result;
}

// ==============================================================================
// Processor implementation
// ==============================================================================

ConvolutionProcessor::ConvolutionProcessor(Node& n) : node(n) {
    loadIR();
}

void ConvolutionProcessor::loadIR() {
    ir = decodeIR(node.script);
    if (ir.empty()) {
        // Default: identity (passthrough) — single impulse at t=0
        ir = {1.0f};
    }
}

void ConvolutionProcessor::prepareToPlay(double sr, int bs) {
    sampleRate = sr;
    loadIR();
    if ((int)ir.size() >= 1024)
        setupOverlapAdd();
    else
        setupDirect();
}

double ConvolutionProcessor::getTailLengthSeconds() const {
    return (double)ir.size() / sampleRate;
}

void ConvolutionProcessor::setupDirect() {
    useFFT = false;
    inputHistory.assign(ir.size(), 0.0f);
}

void ConvolutionProcessor::setupOverlapAdd() {
    useFFT = true;
    // Partition the IR into blocks and pre-FFT each one.
    // Partition size = next power of two >= blockSize or 512, whichever is larger.
    partitionSize = 512;
    fftSize = partitionSize * 2; // zero-padded for linear convolution
    fft = std::make_unique<FFT>(fftSize);

    int numPartitions = ((int)ir.size() + partitionSize - 1) / partitionSize;
    irPartitionsFreq.clear();
    for (int p = 0; p < numPartitions; ++p) {
        std::vector<float> partition(fftSize, 0.0f);
        int start = p * partitionSize;
        int count = std::min(partitionSize, (int)ir.size() - start);
        for (int i = 0; i < count; ++i)
            partition[i] = ir[start + i];
        std::vector<std::complex<float>> freq;
        fft->forwardReal(partition, freq);
        irPartitionsFreq.push_back(freq);
    }

    overlapBuffer.assign(fftSize, 0.0f);
    inputBuffer.assign(partitionSize, 0.0f);
    inputBufferPos = 0;
}

void ConvolutionProcessor::processDirect(float* data, int numSamples) {
    int irLen = (int)ir.size();
    for (int s = 0; s < numSamples; ++s) {
        // Shift input history
        for (int i = irLen - 1; i > 0; --i)
            inputHistory[i] = inputHistory[i - 1];
        inputHistory[0] = data[s];

        // Convolve
        float out = 0.0f;
        for (int k = 0; k < irLen; ++k)
            out += inputHistory[k] * ir[k];
        data[s] = out;
    }
}

void ConvolutionProcessor::processOverlapAdd(float* data, int numSamples) {
    // Simple overlap-add: accumulate input samples, process when we have a
    // full partition. Output is read from the overlap buffer.
    for (int s = 0; s < numSamples; ++s) {
        inputBuffer[inputBufferPos] = data[s];

        // Read from overlap buffer (contains previously computed output)
        data[s] = overlapBuffer[inputBufferPos];
        overlapBuffer[inputBufferPos] = 0.0f; // consumed

        inputBufferPos++;
        if (inputBufferPos >= partitionSize) {
            inputBufferPos = 0;

            // FFT the input block (zero-padded to fftSize)
            std::vector<float> padInput(fftSize, 0.0f);
            for (int i = 0; i < partitionSize; ++i)
                padInput[i] = inputBuffer[i];
            std::vector<std::complex<float>> inputFreq;
            fft->forwardReal(padInput, inputFreq);

            // Multiply with each IR partition and accumulate into overlap buffer
            for (int p = 0; p < (int)irPartitionsFreq.size(); ++p) {
                std::vector<std::complex<float>> product(fftSize / 2 + 1);
                for (int i = 0; i < (int)product.size(); ++i)
                    product[i] = inputFreq[i] * irPartitionsFreq[p][i];

                std::vector<float> timeDomain;
                fft->inverseReal(product, timeDomain);

                // Add to overlap buffer at the right offset
                int offset = p * partitionSize;
                for (int i = 0; i < fftSize; ++i) {
                    int idx = (i + offset) % (fftSize + partitionSize * ((int)irPartitionsFreq.size() - 1));
                    // Simple approach: extend overlap buffer if needed
                    if (idx < (int)overlapBuffer.size())
                        overlapBuffer[idx] += timeDomain[i];
                }
            }
        }
    }
}

void ConvolutionProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) {
    if (ir.size() <= 1 && ir[0] == 1.0f) return; // identity, passthrough

    // Process each channel independently with the same IR
    for (int ch = 0; ch < buf.getNumChannels(); ++ch) {
        auto* data = buf.getWritePointer(ch);
        if (useFFT)
            processOverlapAdd(data, buf.getNumSamples());
        else
            processDirect(data, buf.getNumSamples());
    }
}

} // namespace SoundShop

#pragma once
#include "node_graph.h"
#include "fft_util.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>
#include <complex>
#include <memory>

namespace SoundShop {

// ==============================================================================
// Convolution filter: convolves audio with a user-defined impulse response.
//
// Short IRs (< 1024 samples): direct time-domain convolution.
// Long IRs (>= 1024 samples): partitioned overlap-add FFT convolution.
//
// The IR is stored on node.script as "__convolution__:<length>,<sample>,<sample>,..."
// or as "__convolution_file__:<path>" for file-based IRs.
// ==============================================================================

class ConvolutionProcessor : public juce::AudioProcessor {
public:
    ConvolutionProcessor(Node& node);

    const juce::String getName() const override { return "Convolution"; }
    void prepareToPlay(double sr, int bs) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;
    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    // Get the IR for display / combining
    const std::vector<float>& getIR() const { return ir; }

    // Static helpers for IR manipulation
    static std::vector<float> convolveIRs(const std::vector<float>& a,
                                           const std::vector<float>& b);
    static std::vector<float> sumIRs(const std::vector<float>& a,
                                      const std::vector<float>& b);

    // Encode/decode IR to/from node.script
    static std::string encodeIR(const std::vector<float>& ir);
    static std::vector<float> decodeIR(const std::string& script);

    // Generate preset IRs
    static std::vector<float> generateLowpass(float cutoffHz, int order, double sampleRate);
    static std::vector<float> generateHighpass(float cutoffHz, int order, double sampleRate);
    static std::vector<float> generateBandpass(float centerHz, float bwHz, int order, double sampleRate);
    static std::vector<float> generateEcho(float delayMs, float feedback, int numEchoes, double sampleRate);

private:
    Node& node;
    double sampleRate = 44100;
    std::vector<float> ir;

    // Direct convolution state (short IRs)
    std::vector<float> inputHistory; // circular buffer

    // Overlap-add state (long IRs)
    bool useFFT = false;
    int fftSize = 0;
    int partitionSize = 0;
    std::unique_ptr<FFT> fft;
    std::vector<std::vector<std::complex<float>>> irPartitionsFreq; // FFT'd IR partitions
    std::vector<float> overlapBuffer;  // overlap-add accumulator
    std::vector<float> inputBuffer;    // accumulates input samples
    int inputBufferPos = 0;

    void loadIR();
    void setupDirect();
    void setupOverlapAdd();
    void processDirect(float* data, int numSamples);
    void processOverlapAdd(float* data, int numSamples);
};

} // namespace SoundShop

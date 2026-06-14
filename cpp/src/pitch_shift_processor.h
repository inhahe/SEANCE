#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>

#if HAS_RUBBERBAND
namespace RubberBand { class RubberBandStretcher; }
#endif

namespace SoundShop {

// Real-time pitch shifting and time stretching using Rubber Band
class PitchShiftProcessor : public juce::AudioProcessor {
public:
    PitchShiftProcessor(Node& node);
    ~PitchShiftProcessor() override;

    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double sr, int bs) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;
    // Tail = Rubber Band's internal processing latency.  The library
    // doesn't expose a per-call tail query, but its R2 engine buffers
    // roughly a window-length (≈ 50-100 ms at 44.1 kHz with default
    // settings).  Use a named upper bound here, not a magic constant.
    double getTailLengthSeconds() const override {
        static constexpr double kRubberBandWindowSeconds = 0.1;
        return kRubberBandWindowSeconds;
    }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
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

private:
    Node& node;
    double sampleRate = 44100;
    int blockSize = 512;

#if HAS_RUBBERBAND
    std::unique_ptr<RubberBand::RubberBandStretcher> stretcher;
    float lastPitchSemitones = 0;
    float lastTimeRatio = 1.0f;
#endif

    float getParam(int idx, float def) const;
};

} // namespace SoundShop

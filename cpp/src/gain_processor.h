#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>

namespace SoundShop {

// Applies a fixed gain (in dB) to audio passing through.
// Used to implement send amounts on connections.
class GainProcessor : public juce::AudioProcessor {
public:
    GainProcessor(float& gainDbRef) : gainDb(gainDbRef) {}

    const juce::String getName() const override { return "Gain"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        float g = gainDb;
        if (g == 0.0f) return; // unity = no change
        float linear = juce::Decibels::decibelsToGain(g);
        for (int c = 0; c < buf.getNumChannels(); ++c)
            buf.applyGain(c, 0, buf.getNumSamples(), linear);
    }

    double getTailLengthSeconds() const override { return 0; }
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

private:
    float& gainDb; // reference to the Link's gainDb, so changes are live
};

} // namespace SoundShop

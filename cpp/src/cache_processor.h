#pragma once
#include "node_graph.h"
#include "transport.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace SoundShop {

// Plays back cached audio from a frozen node
class CachePlaybackProcessor : public juce::AudioProcessor {
public:
    CachePlaybackProcessor(Node& node, Transport& transport)
        : node(node), transport(transport) {}

    const juce::String getName() const override { return node.name + " [frozen]"; }
    void prepareToPlay(double sr, int bs) override { sampleRate = sr; }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override {
        buf.clear();
        if (!node.cache.valid || node.cache.left.empty()) return;

        int numSamples = buf.getNumSamples();
        int64_t pos = transport.positionSamples - node.cache.startSample;

        for (int s = 0; s < numSamples; ++s) {
            int64_t idx = pos + s;
            if (idx >= 0 && idx < node.cache.numSamples) {
                if (buf.getNumChannels() > 0) buf.addSample(0, s, node.cache.left[idx]);
                if (buf.getNumChannels() > 1) buf.addSample(1, s, node.cache.right[idx]);
            }
        }
    }

    double getTailLengthSeconds() const override { return 0; }
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
    Transport& transport;
    double sampleRate = 44100;
};

} // namespace SoundShop

#pragma once
#include "node_graph.h"
#include "audio_crossfader.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>

namespace SoundShop {

// Applies stereo panning to audio passing through.
// Uses equal-power pan law: L = cos(theta), R = sin(theta)
class PanProcessor : public juce::AudioProcessor {
public:
    PanProcessor(Node& node, NodeGraph& graph) : node(node), graph(graph) {}

    const juce::String getName() const override { return "Pan"; }
    void prepareToPlay(double sr, int) override {
        // Initialize the mute/solo crossfader; snap to current state so the
        // first block doesn't fade in from 0 on graph rebuild.
        muteFader.prepare(sr, graph.globalCrossfadeSec);
        muteFader.snapTo(currentlyMuted() ? 0.0f : 1.0f);
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override {
        // Mute/solo: smooth ramp instead of an instant clear, so toggling
        // mute or solo during playback fades out over globalCrossfadeSec
        // and doesn't click.
        bool muted = currentlyMuted();
        muteFader.setCrossfadeDuration(graph.globalCrossfadeSec);
        muteFader.setTarget(muted ? 0.0f : 1.0f);
        // Drop MIDI as soon as the user mutes — we don't want new note-ons
        // arriving during the fade-out tail. Audio fades out naturally.
        if (muted) midi.clear();
        muteFader.process(buf);
        if (muteFader.isFullyOff()) return;

        if (buf.getNumChannels() < 2) return;
        // Read pan from the named param if present, falling back to node.pan
        float p = node.pan; // legacy fallback
        for (const auto& param : node.params)
            if (param.name == "Pan") { p = param.value; break; }
        p = juce::jlimit(-1.0f, 1.0f, p);

        if (p == 0.0f) return; // center = no change

        // Equal-power pan law
        // theta = 0 (full left) to pi/2 (full right), pi/4 = center
        float theta = (p + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
        float gainL = std::cos(theta);
        float gainR = std::sin(theta);

        // Normalize so center is unity (cos(pi/4) = sin(pi/4) ≈ 0.707)
        static const float centerGain = std::cos(juce::MathConstants<float>::pi * 0.25f);
        gainL /= centerGain;
        gainR /= centerGain;

        auto* left = buf.getWritePointer(0);
        auto* right = buf.getWritePointer(1);
        int n = buf.getNumSamples();

        for (int i = 0; i < n; ++i) {
            float l = left[i];
            float r = right[i];
            left[i] = l * gainL;
            right[i] = r * gainR;
        }
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
    Node& node;
    NodeGraph& graph;
    AudioCrossfader muteFader;

    // True iff this node is muted directly OR muted because some other node
    // is soloed and we're not. Recomputed every block — solo state changes
    // come from the UI thread, so re-checking is cheap and avoids stale state.
    bool currentlyMuted() const {
        if (node.muted) return true;
        for (auto& n : graph.nodes) if (n.soloed) return !node.soloed;
        return false;
    }
};

} // namespace SoundShop

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
        //
        // The mute fade ONLY applies to audio channels (0+1). Control
        // signals on channels 2+ (Signal/Param outputs) pass through
        // unchanged - muting a node silences its audio output, but its
        // Signal pins keep emitting so downstream nodes that depend on
        // those signals still work. Likewise we never early-return when
        // the audio is fully off: the control channels still need their
        // values to reach the downstream processors.
        bool muted = currentlyMuted();
        muteFader.setCrossfadeDuration(graph.globalCrossfadeSec);
        muteFader.setTarget(muted ? 0.0f : 1.0f);
        // Drop MIDI as soon as the user mutes - we don't want new note-ons
        // arriving during the fade-out tail. Audio fades out naturally.
        if (muted) midi.clear();
        // Run the fader on a wrapper view of just channels 0+1 so
        // channels 2+ (control signals) are untouched.
        int numAudioCh = std::min(2, buf.getNumChannels());
        if (numAudioCh > 0) {
            float* audioPtrs[2] = { buf.getWritePointer(0),
                numAudioCh > 1 ? buf.getWritePointer(1) : nullptr };
            juce::AudioBuffer<float> audioOnly(audioPtrs, numAudioCh, buf.getNumSamples());
            muteFader.process(audioOnly);
        }

        // Peak metering (#99): scan the post-mute buffer for this block's
        // peak and write it atomically. The UI reads these at 30 Hz to
        // draw meter bars. We capture at PanProcessor because it's the
        // last processor in the per-node chain before routing - so the
        // meter shows exactly what leaves the node after pan and mute.
        {
            float pkL = 0, pkR = 0;
            if (buf.getNumChannels() >= 1) {
                auto* data = buf.getReadPointer(0);
                for (int i = 0; i < buf.getNumSamples(); ++i)
                    pkL = std::max(pkL, std::abs(data[i]));
            }
            if (buf.getNumChannels() >= 2) {
                auto* data = buf.getReadPointer(1);
                for (int i = 0; i < buf.getNumSamples(); ++i)
                    pkR = std::max(pkR, std::abs(data[i]));
            } else {
                pkR = pkL; // mono: both channels show the same level
            }
            node.meterPeakL = pkL;
            node.meterPeakR = pkR;
        }

        if (buf.getNumChannels() < 2) return;
        // Read pan from the named param if present, falling back to node.pan
        float p = node.pan; // legacy fallback
        for (const auto& param : node.params)
            if (param.name == "Pan") { p = param.value; break; }
        p = juce::jlimit(-1.0f, 1.0f, p);

        if (p == 0.0f) return; // center = no change

        float gainL, gainR;
        if (node.panLaw == PanLaw::Linear) {
            // Linear pan law - used by tracker imports (MOD/IT/S3M/XM)
            // where per-channel default pans are typically hard L/R and
            // the level reference is "channel at full amplitude when
            // hard-panned, half on each when centered".
            //
            // Normalized to match the EqualPower path at hard-pan so
            // tracker output level stays parity with the historical
            // (EqualPower) rendering:
            //
            //   p=-1 -> L=sqrt(2)  R=0          (hard L, matches EqualPower)
            //   p= 0 -> L=0.707    R=0.707      (center, -3 dB per channel)
            //   p=+1 -> L=0        R=sqrt(2)    (hard R, matches EqualPower)
            //
            // An earlier version of this code scaled by 2 so that center
            // came out unity; that pushed hard-pan to +6 dB and made MOD
            // imports clip on export. Subsequent attempt scaled by 1
            // (hard-pan = 1.0); that was -3 dB too quiet vs the historical
            // rendering. Matching EqualPower at hard-pan is the right
            // calibration: it keeps tracker imports at parity with the
            // pre-PanLaw split while still making the pan curve linear
            // in level (the part that actually distinguishes the two
            // laws perceptually).
            constexpr float kHardPanGain = 1.41421356f; // sqrt(2)
            gainL = (1.0f - (p + 1.0f) * 0.5f) * kHardPanGain;
            gainR =        ((p + 1.0f) * 0.5f) * kHardPanGain;
        } else {
            // Equal-power pan law (industry standard for DAWs).
            // theta = 0 (full left) to pi/2 (full right), pi/4 = center
            float theta = (p + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
            gainL = std::cos(theta);
            gainR = std::sin(theta);

            // Normalize so center is unity (cos(pi/4) = sin(pi/4) ~ 0.707)
            static const float centerGain = std::cos(juce::MathConstants<float>::pi * 0.25f);
            gainL /= centerGain;
            gainR /= centerGain;
        }

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
    // is soloed and we're not. Recomputed every block - solo state changes
    // come from the UI thread, so re-checking is cheap and avoids stale state.
    bool currentlyMuted() const {
        if (node.muted) return true;
        for (auto& n : graph.nodes) if (n.soloed) return !node.soloed;
        return false;
    }
};

} // namespace SoundShop

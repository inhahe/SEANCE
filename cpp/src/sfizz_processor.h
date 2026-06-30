#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>

#ifdef HAS_SFIZZ
#include <sfizz.hpp>
#endif

namespace SoundShop {

// ==============================================================================
// SfizzProcessor - full SFZ compliance via the sfizz library (#37)
//
// When HAS_SFIZZ is defined (sfizz cloned into third_party/sfizz),
// this processor uses sfizz::Sfizz to load and play any SFZ instrument
// with full spec coverage (ARIA extensions, round-robin, modulation
// envelopes, filters, etc.).
//
// When HAS_SFIZZ is NOT defined, the processor logs a message and
// passes audio/MIDI through unchanged - the built-in basic SFZ loader
// still works for simple patches.
//
// Script format: "__sfizz__:<path>" where path is the .sfz file.
// ==============================================================================
class SfizzProcessor : public juce::AudioProcessor {
public:
    SfizzProcessor(Node& n) : node(n) {
#ifdef HAS_SFIZZ
        sfizz = std::make_unique<sfizz::Sfizz>();
        if (node.script.rfind("__sfizz__:", 0) == 0) {
            auto path = node.script.substr(10);
            sfizz->loadSfzFile(path);
        }
#else
        fprintf(stderr, "SfizzProcessor: sfizz library not available. "
                "Clone https://github.com/sfztools/sfizz into third_party/sfizz "
                "and rebuild to enable full SFZ compliance.\n");
#endif
    }

    const juce::String getName() const override { return "SFZ (sfizz)"; }

    void prepareToPlay(double sr, int bs) override {
#ifdef HAS_SFIZZ
        if (sfizz) {
            sfizz->setSampleRate(sr);
            sfizz->setSamplesPerBlock(bs);
        }
#endif
        (void)sr; (void)bs;
    }
    void releaseResources() override {}
    // Transport panic (Stop): cut all sounding/ringing voices immediately so SFZ
    // release tails don't ring out after the user hits Stop.
    void reset() override {
#ifdef HAS_SFIZZ
        if (sfizz) sfizz->allSoundOff();
#endif
    }

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override {
#ifdef HAS_SFIZZ
        if (!sfizz) return;
        // Feed MIDI events to sfizz.
        for (auto meta : midi) {
            auto msg = meta.getMessage();
            if (msg.isNoteOn())
                sfizz->noteOn(meta.samplePosition, msg.getNoteNumber(), msg.getVelocity());
            else if (msg.isNoteOff())
                sfizz->noteOff(meta.samplePosition, msg.getNoteNumber(), 0);
            else if (msg.isController())
                sfizz->cc(meta.samplePosition, msg.getControllerNumber(), msg.getControllerValue());
            else if (msg.isPitchWheel())
                sfizz->pitchWheel(meta.samplePosition, msg.getPitchWheelValue());
        }
        // Render audio.
        buf.clear();
        int n = buf.getNumSamples();
        if (buf.getNumChannels() >= 2) {
            float* outputs[2] = {buf.getWritePointer(0), buf.getWritePointer(1)};
            sfizz->renderBlock(outputs, n);
        } else if (buf.getNumChannels() >= 1) {
            float* outputs[2] = {buf.getWritePointer(0), buf.getWritePointer(0)};
            sfizz->renderBlock(outputs, n);
        }
#else
        (void)buf; (void)midi;
#endif
    }

    // Tail: libsfizz doesn't expose a per-instrument max-release query,
    // so we use a named upper bound that covers typical SFZ pad / string
    // releases.  If sfizz adds a tail-length API in a future release,
    // replace this with the live query.
    double getTailLengthSeconds() const override {
        static constexpr double kSfizzMaxReleaseSeconds = 2.0;
        return kSfizzMaxReleaseSeconds;
    }
    bool acceptsMidi() const override { return true; }
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
#ifdef HAS_SFIZZ
    std::unique_ptr<sfizz::Sfizz> sfizz;
#endif
};

} // namespace SoundShop

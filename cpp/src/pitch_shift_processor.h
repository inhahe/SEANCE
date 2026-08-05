#pragma once
#include "node_graph.h"
#include "pitch_core.h"     // PhaseVocoderShifter
#include <juce_audio_processors/juce_audio_processors.h>

namespace SoundShop {

// Real-time pitch shifting for the "Pitch Shift" node.
//
// HISTORY -- this used to wrap Rubber Band, and dropping that is why the node
// looks the way it does now. Rubber Band is GPL v2 and was linked statically,
// which made the whole binary GPL-encumbered; see the licensing entry in
// known-issues.md. Before deleting it, all three of the node's params were
// measured (the node had had no test coverage at all), and only one of them
// actually did anything:
//
//   * Pitch      -- worked. Still works, now via PhaseVocoderShifter.
//   * Time Ratio -- broken by construction, and unfixable in a live node: a
//                   processBlock must emit exactly as many samples as it is
//                   handed, so a duration change has nowhere to go. At ratio 2
//                   the surplus backed up inside the stretcher forever (latency
//                   growing half a block per block); at ratio 0.5 it starved and
//                   zero-filled, leaving 50% of a continuous tone as exact
//                   silence. REMOVED, along with the dependency. A real
//                   time-stretch feature has to be offline/clip-based.
//   * Formant    -- inert. It was never read; the stretcher was constructed with
//                   OptionFormantPreserved unconditionally, so the knob did
//                   nothing in either position (proved by a bit-identical
//                   render at 0 and 1). Formant preservation itself was real
//                   though, so it was built into PhaseVocoderShifter rather than
//                   lost, and this knob now genuinely drives it.
//
// So the node is strictly better off: two params instead of three, and both of
// them work.
class PitchShiftProcessor : public juce::AudioProcessor {
public:
    PitchShiftProcessor(Node& node);
    ~PitchShiftProcessor() override;

    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double sr, int bs) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;

    // The shifter is a fixed-latency FIFO, so the tail is exactly its latency --
    // no estimate needed, unlike the Rubber Band version which had to guess at
    // a window length because the library exposed no tail query.
    double getTailLengthSeconds() const override {
        return sampleRate > 0 ? (double)shifter[0].latencySamples() / sampleRate : 0.0;
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

    // Param names, shared with the node-creation site and the self-test so a
    // rename cannot silently desync them into reading defaults forever.
    static constexpr const char* kPitchParam   = "Pitch (semi)";
    static constexpr const char* kFormantParam = "Formant";

private:
    Node& node;
    double sampleRate = 44100;   // only needed to express the tail in seconds

    // One shifter per channel: sharing one would cross-contaminate the phase
    // accumulators and collapse the stereo image.
    PhaseVocoderShifter shifter[2];
};

} // namespace SoundShop

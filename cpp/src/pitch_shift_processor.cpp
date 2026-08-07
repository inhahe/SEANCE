#include "pitch_shift_processor.h"
#include <cmath>

namespace SoundShop {

PitchShiftProcessor::PitchShiftProcessor(Node& n) : node(n) {}
PitchShiftProcessor::~PitchShiftProcessor() = default;

void PitchShiftProcessor::prepareToPlay(double sr, int bs) {
    sampleRate = sr;
    // The block size is deliberately unused: PhaseVocoderShifter is a
    // sample-driven FIFO with no opinion about block boundaries, so it copes
    // with any block size, including one that varies call to call.
    juce::ignoreUnused(bs);

    for (auto& s : shifter)
        s.prepare(11, 4, sr);

    // Report the shifter's latency so the graph's PDC lines this node up against
    // its siblings. The node is 100% wet, so unlike the wavelet shifters there
    // is no internal dry path needing its own LatencyDelay.
    setLatencySamples(shifter[0].latencySamples());
}

void PitchShiftProcessor::releaseResources() {
    for (auto& s : shifter)
        s.reset();
}

void PitchShiftProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) {
    const int numSamples  = buf.getNumSamples();
    const int numChannels = std::min(buf.getNumChannels(), 2);
    if (numSamples <= 0 || numChannels <= 0) return;

    // Params BY NAME. The old implementation read them by index, which is why
    // removing one was hazardous: saved projects keep the param list they were
    // saved with, so an old file would have landed Formant's value on the Pitch
    // slot. By name, a stale param is simply ignored and a missing one falls
    // back to its default.
    const float semitones = paramByName(node, kPitchParam, 0.0f);
    const bool  formant   = paramByName(node, kFormantParam, 0.0f) >= 0.5f;
    const float ratio     = PhaseVocoderShifter::ratioForSemitones(semitones);

    // No early-out at ratio 1. This is a fixed-latency node, so returning early
    // would make the output jump forward by the full latency the moment the
    // knob crossed zero - an audible click, and a phase discontinuity against
    // every other path the graph has already delay-compensated.
    for (int c = 0; c < numChannels; ++c) {
        shifter[c].setPitchRatio(ratio);
        shifter[c].setFormantPreserve(formant);
        float* d = buf.getWritePointer(c);
        shifter[c].process(d, d, numSamples);   // in-place: process() may alias
    }

    // Mono input on a stereo buffer: mirror rather than leaving channel 1 with
    // whatever stale content it held.
    if (buf.getNumChannels() > 1 && numChannels == 1)
        buf.copyFrom(1, 0, buf, 0, 0, numSamples);
}

} // namespace SoundShop

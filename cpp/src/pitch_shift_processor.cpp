#include "pitch_shift_processor.h"
#include <cmath>
#include <cstdio>

#if HAS_RUBBERBAND
#include "rubberband/RubberBandStretcher.h"
using RubberBand::RubberBandStretcher;
#endif

namespace SoundShop {

PitchShiftProcessor::PitchShiftProcessor(Node& n) : node(n) {}
PitchShiftProcessor::~PitchShiftProcessor() = default;

float PitchShiftProcessor::getParam(int idx, float def) const {
    if (idx >= 0 && idx < (int)node.params.size())
        return node.params[idx].value;
    return def;
}

void PitchShiftProcessor::prepareToPlay(double sr, int bs) {
    sampleRate = sr;
    blockSize = bs;

#if HAS_RUBBERBAND
    stretcher = std::make_unique<RubberBandStretcher>(
        (size_t)sr, 2,
        RubberBandStretcher::OptionProcessRealTime |
        RubberBandStretcher::OptionPitchHighConsistency |
        RubberBandStretcher::OptionFormantPreserved);

    stretcher->setMaxProcessSize(bs);
    lastPitchSemitones = 0;
    lastTimeRatio = 1.0f;
    fprintf(stderr, "[PitchShift] Rubber Band initialized (%.0f Hz, %d samples)\n", sr, bs);
#else
    fprintf(stderr, "[PitchShift] Rubber Band not available\n");
#endif
}

void PitchShiftProcessor::releaseResources() {
#if HAS_RUBBERBAND
    stretcher.reset();
#endif
}

void PitchShiftProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) {
#if HAS_RUBBERBAND
    if (!stretcher) return;
    int numSamples = buf.getNumSamples();
    int numChannels = std::min(buf.getNumChannels(), 2);

    // Read params
    // 0 = Pitch (semitones, -24 to +24)
    // 1 = Time ratio (0.5 = half speed, 2.0 = double speed)
    // 2 = Formant preservation (0 = off, 1 = on)
    float pitchSemitones = getParam(0, 0.0f);
    float timeRatio = getParam(1, 1.0f);
    timeRatio = juce::jlimit(0.25f, 4.0f, timeRatio);

    // Update stretcher if params changed
    if (std::abs(pitchSemitones - lastPitchSemitones) > 0.01f) {
        double pitchScale = std::pow(2.0, pitchSemitones / 12.0);
        stretcher->setPitchScale(pitchScale);
        lastPitchSemitones = pitchSemitones;
    }
    if (std::abs(timeRatio - lastTimeRatio) > 0.01f) {
        stretcher->setTimeRatio((double)timeRatio);
        lastTimeRatio = timeRatio;
    }

    // Feed input
    const float* inputs[2] = {
        buf.getReadPointer(0),
        numChannels > 1 ? buf.getReadPointer(1) : buf.getReadPointer(0)
    };
    stretcher->process(inputs, numSamples, false);

    // Retrieve output
    int available = (int)stretcher->available();
    if (available > 0) {
        int toRetrieve = std::min(available, numSamples);
        float* outputs[2] = {
            buf.getWritePointer(0),
            numChannels > 1 ? buf.getWritePointer(1) : buf.getWritePointer(0)
        };

        // Clear buffer first
        buf.clear();

        stretcher->retrieve(outputs, toRetrieve);
        // If we got fewer samples than the block size, the rest stays silent
    } else {
        buf.clear();
    }
#else
    // No Rubber Band — pass through unchanged
    (void)buf;
#endif
}

} // namespace SoundShop

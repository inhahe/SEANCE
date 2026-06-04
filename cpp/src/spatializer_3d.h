#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <vector>
#include <array>

namespace SoundShop {

// ==============================================================================
// HRTF-based 3D Spatializer
//
// Positions a mono/stereo sound source in 3D space around the listener's head.
// Uses Head-Related Transfer Functions to create binaural audio (headphones).
//
// Params:
//   Azimuth:   -180 to +180 degrees (0=front, +90=right, -90=left, 180=behind)
//   Elevation: -90 to +90 degrees (0=ear level, +90=above, -90=below)
//   Distance:  0 to 1 (0=close/loud, 1=far/quiet with more reverb character)
//
// The HRTF is approximated mathematically for v1 (interaural time delay +
// frequency-dependent head shadow + pinna filtering). A follow-up can load
// real measured HRTF datasets (MIT KEMAR, CIPIC) for more accuracy.
// ==============================================================================

class Spatializer3DProcessor : public juce::AudioProcessor {
public:
    Spatializer3DProcessor(Node& node);
    const juce::String getName() const override { return "3D Spatializer"; }
    void prepareToPlay(double sr, int bs) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override;
    // Tail = HRTF convolution IR length / sample rate.  Once input stops,
    // the FIR convolution buffer flushes out in HRTF_IR_LENGTH samples
    // (the ITD delay line is shorter, so it doesn't dominate).
    double getTailLengthSeconds() const override {
        return (double) HRTF_IR_LENGTH / std::max(1.0, sampleRate);
    }
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
    double sampleRate = 44100;

    // Delay lines for interaural time difference (ITD)
    static constexpr int MAX_DELAY = 64; // ~1.4ms at 44100Hz (max ITD)
    float delayBufL[MAX_DELAY] = {};
    float delayBufR[MAX_DELAY] = {};
    int delayWritePos = 0;

    // Head shadow filters: simple one-pole lowpass per ear
    // Simulates the frequency-dependent attenuation caused by the head
    float filterStateL = 0, filterStateR = 0;

    // Pinna (outer ear) comb filter state
    float pinnaDelayL[32] = {}, pinnaDelayR[32] = {};
    int pinnaWritePos = 0;

    // Previous params for smooth interpolation
    float prevAzimuth = 0, prevElevation = 0, prevDistance = 0.5f;

    // HRTF convolution: per-ear IR from the lookup table
    static constexpr int HRTF_IR_LENGTH = 64;
    static constexpr int CONV_HISTORY_SIZE = 256; // must be > HRTF_IR_LENGTH
    float currentIR_L[HRTF_IR_LENGTH] = {};
    float currentIR_R[HRTF_IR_LENGTH] = {};
    float convHistoryL[CONV_HISTORY_SIZE] = {};
    float convHistoryR[CONV_HISTORY_SIZE] = {};
    int convWritePos = 0;

    // Measured HRTF dataset (#44). If loaded, overrides the synthetic
    // model. Each entry is an (azimuth, elevation) → (left IR, right IR)
    // pair. The processor interpolates between the nearest entries.
    struct HrtfEntry {
        float azimuth = 0;    // degrees, -180..180
        float elevation = 0;  // degrees, -90..90
        std::vector<float> irL, irR; // impulse response per ear
    };
    std::vector<HrtfEntry> measuredHrtfs;
    bool hasMeasuredHrtf = false;

    // Load HRTF dataset from a directory of WAV files. Expected naming:
    //   hrtf_az{N}_el{M}_L.wav / hrtf_az{N}_el{M}_R.wav
    // where N is azimuth and M is elevation in degrees.
    // Returns the number of entries loaded.
    int loadHrtfDirectory(const std::string& dirPath);

    // Find the nearest measured HRTF for a given azimuth/elevation
    // and copy its IRs into currentIR_L / currentIR_R.
    void selectMeasuredHrtf(float azimuth, float elevation);
};

} // namespace SoundShop

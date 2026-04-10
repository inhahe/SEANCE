#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include <juce_core/juce_core.h>

namespace SoundShop {

struct PitchResult {
    float frequencyHz = 0.0f;
    int midiNote = -1;         // nearest MIDI note (0-127)
    float centsOffset = 0.0f;  // how far from the nearest note (-50..+50)
    float confidence = 0.0f;   // 0..1, how confident the detection is

    // Derive MIDI note and cents from frequency
    void computeNoteAndCents() {
        if (frequencyHz <= 0) { midiNote = -1; centsOffset = 0; return; }
        float exactNote = 12.0f * std::log2(frequencyHz / 440.0f) + 69.0f;
        midiNote = (int)std::round(exactNote);
        midiNote = std::max(0, std::min(127, midiNote));
        centsOffset = (exactNote - midiNote) * 100.0f;
    }

    // Get the frequency of the nearest MIDI note (for auto-tune)
    float nearestNoteHz() const {
        if (midiNote < 0) return 440.0f;
        return 440.0f * std::pow(2.0f, (midiNote - 69) / 12.0f);
    }
};

// Autocorrelation pitch detection. Simple, fast, works well for clean
// periodic signals. Can be confused by octave errors (detecting 2x or
// 0.5x the true frequency).
inline PitchResult detectPitchAutocorrelation(const float* samples, int numSamples,
                                                double sampleRate)
{
    PitchResult result;
    if (numSamples < 64) return result;

    // Use the middle portion of the sample for stability
    int analyzeLen = std::min(numSamples, (int)(sampleRate * 0.1)); // up to 100ms
    int offset = std::max(0, (numSamples - analyzeLen) / 2);
    const float* data = samples + offset;

    // Min/max lag: 20 Hz to 5000 Hz
    int minLag = std::max(1, (int)(sampleRate / 5000.0));
    int maxLag = std::min(analyzeLen / 2, (int)(sampleRate / 20.0));
    if (maxLag <= minLag) return result;

    // Compute autocorrelation
    std::vector<float> corr(maxLag + 1, 0.0f);
    float energy = 0;
    for (int i = 0; i < analyzeLen - maxLag; ++i)
        energy += data[i] * data[i];
    if (energy < 1e-8f) return result;

    for (int lag = minLag; lag <= maxLag; ++lag) {
        float sum = 0;
        for (int i = 0; i < analyzeLen - maxLag; ++i)
            sum += data[i] * data[i + lag];
        corr[lag] = sum / energy;
    }

    // Find the first peak above a threshold after the initial dip
    bool pastDip = false;
    int bestLag = minLag;
    float bestCorr = 0;
    for (int lag = minLag; lag <= maxLag; ++lag) {
        if (!pastDip && corr[lag] < 0.0f) pastDip = true;
        if (pastDip && corr[lag] > bestCorr) {
            bestCorr = corr[lag];
            bestLag = lag;
        }
    }

    if (bestCorr < 0.2f) return result; // too low confidence

    // Parabolic interpolation around the peak for sub-sample accuracy
    float refinedLag = (float)bestLag;
    if (bestLag > minLag && bestLag < maxLag) {
        float y0 = corr[bestLag - 1], y1 = corr[bestLag], y2 = corr[bestLag + 1];
        float d = (y0 - y2) / (2.0f * (y0 - 2.0f * y1 + y2));
        if (std::abs(d) < 1.0f) refinedLag += d;
    }

    result.frequencyHz = (float)(sampleRate / refinedLag);
    result.confidence = bestCorr;
    result.computeNoteAndCents();
    return result;
}

// YIN pitch detection. More robust than autocorrelation — uses a
// cumulative mean normalized difference function that reduces octave
// errors. Standard algorithm from de Cheveigné & Kawahara (2002).
inline PitchResult detectPitchYIN(const float* samples, int numSamples,
                                   double sampleRate, float threshold = 0.15f)
{
    PitchResult result;
    if (numSamples < 64) return result;

    int analyzeLen = std::min(numSamples, (int)(sampleRate * 0.1));
    int offset = std::max(0, (numSamples - analyzeLen) / 2);
    const float* data = samples + offset;

    int halfLen = analyzeLen / 2;
    int minLag = std::max(1, (int)(sampleRate / 5000.0));
    int maxLag = std::min(halfLen - 1, (int)(sampleRate / 20.0));
    if (maxLag <= minLag) return result;

    // Step 1: Difference function d(tau) = sum((x[i] - x[i+tau])^2)
    std::vector<float> diff(maxLag + 1, 0.0f);
    for (int tau = 1; tau <= maxLag; ++tau) {
        float sum = 0;
        for (int i = 0; i < halfLen; ++i) {
            float d = data[i] - data[i + tau];
            sum += d * d;
        }
        diff[tau] = sum;
    }

    // Step 2: Cumulative mean normalized difference function
    // d'(tau) = d(tau) / ((1/tau) * sum(d(j), j=1..tau))
    std::vector<float> cmndf(maxLag + 1, 1.0f);
    cmndf[0] = 1.0f;
    float runningSum = 0;
    for (int tau = 1; tau <= maxLag; ++tau) {
        runningSum += diff[tau];
        cmndf[tau] = (runningSum > 1e-8f) ? diff[tau] * tau / runningSum : 1.0f;
    }

    // Step 3: Absolute threshold — find the first tau where cmndf dips
    // below the threshold, then find the local minimum after that dip.
    int bestLag = -1;
    for (int tau = minLag; tau < maxLag; ++tau) {
        if (cmndf[tau] < threshold) {
            // Walk forward to find the local minimum
            while (tau + 1 < maxLag && cmndf[tau + 1] < cmndf[tau])
                ++tau;
            bestLag = tau;
            break;
        }
    }

    if (bestLag < 0) {
        // Fallback: find the global minimum
        float minVal = 999;
        for (int tau = minLag; tau <= maxLag; ++tau) {
            if (cmndf[tau] < minVal) { minVal = cmndf[tau]; bestLag = tau; }
        }
        if (minVal > 0.5f) return result; // no clear pitch
    }

    // Parabolic interpolation for sub-sample accuracy
    float refinedLag = (float)bestLag;
    if (bestLag > minLag && bestLag < maxLag) {
        float y0 = cmndf[bestLag - 1], y1 = cmndf[bestLag], y2 = cmndf[bestLag + 1];
        float denom = 2.0f * (y0 - 2.0f * y1 + y2);
        if (std::abs(denom) > 1e-6f) {
            float d = (y0 - y2) / denom;
            if (std::abs(d) < 1.0f) refinedLag += d;
        }
    }

    result.frequencyHz = (float)(sampleRate / refinedLag);
    result.confidence = 1.0f - std::min(1.0f, cmndf[bestLag]);
    result.computeNoteAndCents();
    return result;
}

// Note name helper
inline const char* midiNoteName(int note) {
    static const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    if (note < 0 || note > 127) return "?";
    return names[note % 12];
}

inline int midiNoteOctave(int note) {
    return (note / 12) - 1;
}

inline juce::String midiNoteFullName(int note) {
    if (note < 0 || note > 127) return "?";
    return juce::String(midiNoteName(note)) + juce::String(midiNoteOctave(note));
}

} // namespace SoundShop

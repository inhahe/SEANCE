#pragma once
#include <cstdint>
#include <vector>
#include <algorithm>

namespace SoundShop {

// Time signature at a specific bar
struct TimeSignature {
    double beatPosition = 0;  // beat where this time sig starts
    int numerator = 4;        // beats per bar (top number)
    int denominator = 4;      // beat unit (bottom number: 4 = quarter, 8 = eighth)

    // Beats per bar in quarter-note units (what our beat system uses)
    double beatsPerBar() const {
        return (double)numerator * 4.0 / denominator;
    }
};

// A tempo change point on the timeline
struct TempoPoint {
    double beatPosition = 0;  // beat where this tempo starts
    double bpm = 120.0;
    // For linear ramps: endBpm. If same as bpm, it's constant.
    double endBpm = 120.0;
    double endBeat = 0;       // beat where this segment ends (0 = extends to next point)
};

// Tempo map — supports multiple tempo changes over time
class TempoMap {
public:
    TempoMap() {
        // Default: single constant tempo
        points.push_back({0, 120.0, 120.0, 0});
    }

    // Get BPM at a given beat position
    double bpmAtBeat(double beat) const {
        if (points.empty()) return 120.0;
        // Find the last point at or before this beat
        const TempoPoint* active = &points[0];
        for (auto& p : points)
            if (p.beatPosition <= beat) active = &p;

        if (active->endBpm != active->bpm && active->endBeat > active->beatPosition) {
            // Linear ramp
            double frac = (beat - active->beatPosition) / (active->endBeat - active->beatPosition);
            frac = std::clamp(frac, 0.0, 1.0);
            return active->bpm + (active->endBpm - active->bpm) * frac;
        }
        return active->bpm;
    }

    // Convert beats to seconds (integrates tempo)
    double beatsToSeconds(double beats) const {
        if (points.size() <= 1 && points[0].bpm == points[0].endBpm) {
            // Simple constant tempo
            return beats * 60.0 / points[0].bpm;
        }
        // Numerical integration for variable tempo
        double seconds = 0;
        double currentBeat = 0;
        double step = 0.25; // quarter-beat steps
        while (currentBeat < beats) {
            double dt = std::min(step, beats - currentBeat);
            double bpm = bpmAtBeat(currentBeat + dt * 0.5); // midpoint
            seconds += dt * 60.0 / bpm;
            currentBeat += dt;
        }
        return seconds;
    }

    // Convert seconds to beats (inverse of above)
    double secondsToBeats(double seconds) const {
        if (points.size() <= 1 && points[0].bpm == points[0].endBpm) {
            return seconds * points[0].bpm / 60.0;
        }
        // Binary search
        double lo = 0, hi = seconds * 300.0 / 60.0; // generous upper bound
        for (int i = 0; i < 50; ++i) {
            double mid = (lo + hi) / 2;
            if (beatsToSeconds(mid) < seconds) lo = mid; else hi = mid;
        }
        return (lo + hi) / 2;
    }

    // Convert beats to samples
    double beatsToSamples(double beats, double sampleRate) const {
        return beatsToSeconds(beats) * sampleRate;
    }

    double samplesToBeats(int64_t samples, double sampleRate) const {
        return secondsToBeats(samples / sampleRate);
    }

    void addTempoChange(double beat, double bpm) {
        points.push_back({beat, bpm, bpm, 0});
        std::sort(points.begin(), points.end(),
                  [](auto& a, auto& b) { return a.beatPosition < b.beatPosition; });
    }

    void addTempoRamp(double startBeat, double endBeat, double startBpm, double endBpm) {
        points.push_back({startBeat, startBpm, endBpm, endBeat});
        std::sort(points.begin(), points.end(),
                  [](auto& a, auto& b) { return a.beatPosition < b.beatPosition; });
    }

    void clear() {
        points.clear();
        points.push_back({0, 120.0, 120.0, 0});
    }

    void setGlobalBpm(double bpm) {
        points.clear();
        points.push_back({0, bpm, bpm, 0});
    }

    std::vector<TempoPoint> points;
};

// Time signature map — supports changes over time
class TimeSignatureMap {
public:
    TimeSignatureMap() {
        sigs.push_back({0, 4, 4}); // default 4/4
    }

    // Get time signature at a given beat
    const TimeSignature& at(double beat) const {
        const TimeSignature* active = &sigs[0];
        for (auto& ts : sigs)
            if (ts.beatPosition <= beat) active = &ts;
        return *active;
    }

    // Get beats-per-bar at a given beat
    double beatsPerBar(double beat) const {
        return at(beat).beatsPerBar();
    }

    // Convert beat position to bar:beat (1-indexed)
    // Returns {barNumber, beatInBar} where both are 1-based
    std::pair<int, double> beatToBarBeat(double beat) const {
        int bar = 1;
        double pos = 0;
        int sigIdx = 0;

        while (pos < beat) {
            // Find the active time sig at this position
            while (sigIdx + 1 < (int)sigs.size() && sigs[sigIdx + 1].beatPosition <= pos)
                sigIdx++;
            double bpb = sigs[sigIdx].beatsPerBar();
            if (pos + bpb > beat) {
                // We're in this bar
                return {bar, beat - pos + 1.0};
            }
            pos += bpb;
            bar++;
        }
        return {bar, 1.0};
    }

    // Get the beat position of bar N (1-indexed)
    double barToBeat(int barNumber) const {
        double pos = 0;
        int sigIdx = 0;
        for (int b = 1; b < barNumber; ++b) {
            while (sigIdx + 1 < (int)sigs.size() && sigs[sigIdx + 1].beatPosition <= pos)
                sigIdx++;
            pos += sigs[sigIdx].beatsPerBar();
        }
        return pos;
    }

    void addTimeSignature(double beat, int num, int den) {
        // Remove any existing at this exact position
        sigs.erase(std::remove_if(sigs.begin(), sigs.end(),
            [beat](auto& ts) { return std::abs(ts.beatPosition - beat) < 0.001; }),
            sigs.end());
        sigs.push_back({beat, num, den});
        std::sort(sigs.begin(), sigs.end(),
            [](auto& a, auto& b) { return a.beatPosition < b.beatPosition; });
    }

    void setGlobal(int num, int den) {
        sigs.clear();
        sigs.push_back({0, num, den});
    }

    void clear() {
        sigs.clear();
        sigs.push_back({0, 4, 4});
    }

    std::vector<TimeSignature> sigs;
};

// Global transport state shared between UI and audio engine
struct Transport {
    bool playing = false;
    bool recording = false;
    double bpm = 120.0;     // Current BPM (for display / simple mode)
    double sampleRate = 44100.0;
    int64_t positionSamples = 0;
    TempoMap tempoMap;
    TimeSignatureMap timeSigMap;

    // Loop region
    bool loopEnabled = false;
    double loopStartBeat = 0;
    double loopEndBeat = 0;

    double positionBeats() const {
        return tempoMap.samplesToBeats(positionSamples, sampleRate);
    }

    double beatsToSamples(double beats) const {
        return tempoMap.beatsToSamples(beats, sampleRate);
    }

    double samplesToBeats(int64_t samples) const {
        return tempoMap.samplesToBeats(samples, sampleRate);
    }
};

} // namespace SoundShop

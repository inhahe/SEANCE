#pragma once
// Silence MSVC C++20 deprecation of std::atomic_load/store for shared_ptr.
// We use these for the lock-free TempoMap swap (#22); replacing them with
// std::atomic<shared_ptr> would require C++20 library support that not all
// toolchains ship yet.
#define _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING
#include <cstdint>
#include <vector>
#include <memory>
#include <algorithm>
#include "tuning.h"

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
// Lock-free TempoMap via shared_ptr swap (#22). The UI thread builds a
// new immutable vector and atomically publishes it. The audio thread
// grabs a snapshot via atomic_load and reads it without locking. No
// iterator-invalidation races, no empty-vector windows.
class TempoMap {
public:
    using Points = std::vector<TempoPoint>;
    // Not const-qualified because MSVC's std::atomic_store can't resolve
    // the const/non-const shared_ptr overload. The vector is treated as
    // immutable by convention — once published, nobody mutates it.
    using PointsPtr = std::shared_ptr<Points>;

    TempoMap() {
        auto p = std::make_shared<Points>();
        p->push_back({0, 120.0, 120.0, 0});
        std::atomic_store(&pts, std::move(p));
    }

    // Snapshot accessor — audio thread calls this once per block and
    // reads through the returned shared_ptr for the rest of the block.
    PointsPtr snapshot() const { return std::atomic_load(&pts); }

    // Get BPM at a given beat position.
    double bpmAtBeat(double beat) const {
        auto sp = snapshot();
        if (!sp || sp->empty()) return 120.0;
        const TempoPoint* active = &(*sp)[0];
        for (auto& p : *sp)
            if (p.beatPosition <= beat) active = &p;
        if (active->endBpm != active->bpm && active->endBeat > active->beatPosition) {
            double frac = (beat - active->beatPosition) / (active->endBeat - active->beatPosition);
            frac = std::clamp(frac, 0.0, 1.0);
            return active->bpm + (active->endBpm - active->bpm) * frac;
        }
        return active->bpm;
    }

    double beatsToSeconds(double beats) const {
        auto sp = snapshot();
        if (!sp || sp->empty()) return beats * 60.0 / 120.0;
        if (sp->size() == 1 && (*sp)[0].bpm == (*sp)[0].endBpm)
            return beats * 60.0 / (*sp)[0].bpm;
        double seconds = 0, currentBeat = 0, step = 0.25;
        while (currentBeat < beats) {
            double dt = std::min(step, beats - currentBeat);
            double bpm = bpmAtBeat(currentBeat + dt * 0.5);
            seconds += dt * 60.0 / bpm;
            currentBeat += dt;
        }
        return seconds;
    }

    double secondsToBeats(double seconds) const {
        auto sp = snapshot();
        if (!sp || sp->empty()) return seconds * 120.0 / 60.0;
        if (sp->size() == 1 && (*sp)[0].bpm == (*sp)[0].endBpm)
            return seconds * (*sp)[0].bpm / 60.0;
        double lo = 0, hi = seconds * 300.0 / 60.0;
        for (int i = 0; i < 50; ++i) {
            double mid = (lo + hi) / 2;
            if (beatsToSeconds(mid) < seconds) lo = mid; else hi = mid;
        }
        return (lo + hi) / 2;
    }

    double beatsToSamples(double beats, double sampleRate) const {
        return beatsToSeconds(beats) * sampleRate;
    }
    double samplesToBeats(int64_t samples, double sampleRate) const {
        return secondsToBeats(samples / sampleRate);
    }

    // Mutators — UI thread only. Each builds a new vector and
    // atomically replaces the shared pointer.
    void addTempoChange(double beat, double bpm) {
        auto old = snapshot();
        auto nv = std::make_shared<Points>(old ? *old : Points{{0,120,120,0}});
        nv->push_back({beat, bpm, bpm, 0});
        std::sort(nv->begin(), nv->end(),
                  [](auto& a, auto& b) { return a.beatPosition < b.beatPosition; });
        std::atomic_store(&pts, PointsPtr(std::move(nv)));
    }

    void addTempoRamp(double startBeat, double endBeat, double startBpm, double endBpm) {
        auto old = snapshot();
        auto nv = std::make_shared<Points>(old ? *old : Points{{0,120,120,0}});
        nv->push_back({startBeat, startBpm, endBpm, endBeat});
        std::sort(nv->begin(), nv->end(),
                  [](auto& a, auto& b) { return a.beatPosition < b.beatPosition; });
        std::atomic_store(&pts, PointsPtr(std::move(nv)));
    }

    void clear() {
        auto nv = std::make_shared<Points>();
        nv->push_back({0, 120.0, 120.0, 0});
        std::atomic_store(&pts, PointsPtr(std::move(nv)));
    }

    void setGlobalBpm(double bpm) {
        auto nv = std::make_shared<Points>();
        nv->push_back({0, bpm, bpm, 0});
        std::atomic_store(&pts, PointsPtr(std::move(nv)));
    }

    // Legacy accessor kept for code that reads `tempoMap.points` directly
    // (e.g. timerCallback sync checks). Returns a copy of the current
    // snapshot. Safe but not lock-free — UI thread only.
    Points getPoints() const {
        auto sp = snapshot();
        return sp ? *sp : Points{{0, 120.0, 120.0, 0}};
    }

private:
    PointsPtr pts;
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

    // Tuning (synced from NodeGraph each frame)
    TuningSystem tuningSystem = TuningSystem::Equal12;
    float concertPitch = 440.0f;

    // Convert MIDI note to frequency using the project's tuning system
    float noteToFreq(int midiNote) const {
        return midiNoteToFrequency(midiNote, tuningSystem, concertPitch);
    }
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

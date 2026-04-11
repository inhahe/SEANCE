#pragma once
#include <cmath>
#include <string>

namespace SoundShop {

// ==============================================================================
// Tuning Systems
//
// Each system defines a cent offset per chromatic pitch class (0-11) from
// 12-tone equal temperament. The offset is added to the note's frequency
// calculation:
//   freq = concertPitch * 2^((note - 69 + centsOffset[note%12]/100) / 12)
//
// Concert Pitch sets the frequency of A4 (default 440 Hz).
// ==============================================================================

enum class TuningSystem {
    Equal12,        // 12-TET (standard, all offsets = 0)
    Pythagorean,    // Pure fifths (3:2), stacked from C
    JustIntonation, // 5-limit JI, pure thirds and fifths
    Meantone,       // Quarter-comma meantone, pure major thirds
    COUNT
};

inline const char* tuningSystemName(TuningSystem t) {
    switch (t) {
        case TuningSystem::Equal12:        return "Equal Temperament (standard)";
        case TuningSystem::Pythagorean:    return "Pythagorean (pure fifths)";
        case TuningSystem::JustIntonation: return "Just Intonation (pure intervals)";
        case TuningSystem::Meantone:       return "Quarter-Comma Meantone";
        default: return "Unknown";
    }
}

// Cent offsets from 12-TET for each pitch class (C=0, C#=1, ..., B=11).
// Positive = sharper than 12-TET, negative = flatter.
inline const float* tuningCentsOffset(TuningSystem t) {
    switch (t) {
        case TuningSystem::Equal12: {
            static const float offsets[] = {0,0,0,0,0,0,0,0,0,0,0,0};
            return offsets;
        }
        case TuningSystem::Pythagorean: {
            // Built from stacked pure 3:2 fifths starting from C.
            // C    C#      D      Eb     E      F      F#     G      Ab     A      Bb     B
            static const float offsets[] = {
                0.0f, -9.78f, 3.91f, -5.87f, 7.82f, -1.96f, 11.73f, 1.96f, -7.82f, 5.87f, -3.91f, 9.78f
            };
            return offsets;
        }
        case TuningSystem::JustIntonation: {
            // 5-limit JI. Ratios: 1/1, 16/15, 9/8, 6/5, 5/4, 4/3, 45/32, 3/2, 8/5, 5/3, 9/5, 15/8
            static const float offsets[] = {
                0.0f, 11.73f, 3.91f, 15.64f, -13.69f, -1.96f, -9.78f, 1.96f, 13.69f, -15.64f, 17.60f, -11.73f
            };
            return offsets;
        }
        case TuningSystem::Meantone: {
            // Quarter-comma meantone. Fifths narrowed by 1/4 syntonic comma
            // so major thirds are pure (386.31 cents = 5/4).
            static const float offsets[] = {
                0.0f, -24.04f, -6.84f, 10.26f, -13.69f, 3.42f, -20.53f, -3.42f, 13.69f, -10.26f, 6.84f, -17.11f
            };
            return offsets;
        }
        default: {
            static const float offsets[] = {0,0,0,0,0,0,0,0,0,0,0,0};
            return offsets;
        }
    }
}

// Convert a MIDI note number to frequency using the given tuning system
// and concert pitch (Hz for A4).
inline float midiNoteToFrequency(int note, TuningSystem tuning = TuningSystem::Equal12,
                                  float concertPitch = 440.0f) {
    const float* offsets = tuningCentsOffset(tuning);
    float centsOff = offsets[note % 12];
    return concertPitch * std::pow(2.0f, ((float)(note - 69) + centsOff / 100.0f) / 12.0f);
}

// Common concert pitch standards
struct ConcertPitchPreset {
    const char* name;
    float hz;
};

inline const ConcertPitchPreset* getConcertPitchPresets(int& count) {
    static const ConcertPitchPreset presets[] = {
        {"A = 440 Hz (modern standard)",     440.0f},
        {"A = 432 Hz (Verdi tuning)",        432.0f},
        {"A = 415 Hz (Baroque)",             415.0f},
        {"A = 442 Hz (European orchestral)", 442.0f},
        {"A = 443 Hz (Berlin Philharmonic)", 443.0f},
        {"A = 444 Hz (some US orchestras)",  444.0f},
        {"A = 430 Hz (classical era ~1820)", 430.0f},
        {"A = 466 Hz (Renaissance pitch)",   466.0f},
    };
    count = sizeof(presets) / sizeof(presets[0]);
    return presets;
}

} // namespace SoundShop

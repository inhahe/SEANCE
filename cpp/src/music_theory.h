#pragma once
#include <vector>
#include <string>
#include <map>
#include <cmath>

namespace SoundShop {

struct ScaleDefinition {
    std::string name;
    std::vector<int> intervals; // semitones from root
};

// Scale degree info for a note
struct DegreeInfo {
    int degree = 0;          // 0-based index into scale
    int octave = 4;
    int chromaticOffset = 0; // semitones from nearest scale tone
};

// Ordered map that preserves insertion order (unlike std::map which sorts alphabetically)
using ScaleMap = std::vector<std::pair<std::string, std::vector<int>>>;

// Find intervals by name in a ScaleMap
inline const std::vector<int>* findScale(const ScaleMap& map, const std::string& name) {
    for (auto& [n, v] : map)
        if (n == name) return &v;
    return nullptr;
}

class MusicTheory {
public:
    static const char* const NOTE_NAMES[12];
    static const char* const DEGREE_NAMES[7];

    // Key types (major, minor variants)
    static const ScaleMap& keys();
    // Modes (rotations of major scale + others)
    static const ScaleMap& modes();
    // Scales (pentatonic, blues, etc.)
    static const ScaleMap& scales();

    static std::string noteName(int pitch);
    static bool isBlackKey(int pitch);
    static int snapToScale(int pitch, int root, const std::vector<int>& scale);
    static DegreeInfo pitchToDegree(int pitch, int root, const std::vector<int>& scale);
    static int degreeToPitch(int degree, int octave, int chromaticOffset,
                             int root, const std::vector<int>& scale);

    // Detect possible keys/modes for a set of pitches
    struct KeyMatch {
        int root;              // 0-11
        std::string scaleName; // e.g. "Major", "Dorian"
        std::string category;  // "key", "mode", or "scale"
        int scaleSize;         // how many notes in the scale
        int notesMatched;      // how many input notes matched
        float coverage;        // notesMatched / scaleSize (1.0 = perfect fit, notes use all scale degrees)
    };
    static std::vector<KeyMatch> detectKeys(const std::vector<int>& pitches);

    // Reference pitch for A4 (default 440 Hz)
    static inline float referencePitch = 440.0f;

    // Common tuning presets
    static void setStandardTuning() { referencePitch = 440.0f; }
    static void setVerdiTuning() { referencePitch = 432.0f; }    // A=432 Hz ("Verdi pitch")
    static void setBaroqueTuning() { referencePitch = 415.0f; }  // Common baroque pitch
    static void setReferencePitch(float hz) { referencePitch = hz; }

    // Frequency of a MIDI pitch with detune in cents
    static float pitchToFrequency(int pitch, float detuneCents = 0.0f) {
        return referencePitch * std::pow(2.0f, (pitch - 69 + detuneCents / 100.0f) / 12.0f);
    }
};

} // namespace SoundShop

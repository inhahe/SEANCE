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

    // Parent scales (major + minor variants). In the piano-roll Key/Mode/Scale
    // UI these act as the PARENT scale that Mode rotates.
    static const ScaleMap& keys();
    // The seven modes of the major scale, in rotation order. The "Mode" control
    // picks a rotation DEGREE (its index here) which is applied to the chosen
    // Key. For the Major parent these reproduce the intervals listed.
    static const ScaleMap& modes();
    // Fixed non-diatonic scales (pentatonic, blues, whole-tone, chromatic, and
    // named modes of non-major parents). Selected directly, bypassing Key+Mode.
    static const ScaleMap& scales();

    // Rotate a parent scale to its `degree`-th mode (0-based) and re-anchor so
    // the new tonic is 0. e.g. rotateScale(Major,1) == Dorian. `degree` is taken
    // modulo the parent size, so it is always valid.
    static std::vector<int> rotateScale(const std::vector<int>& parent, int degree);

    // 0-based rotation degree of a mode NAME within modes(). Tolerant of the
    // legacy "Ionian" spelling (now displayed "Ionian (Major)"). Unknown -> 0.
    static int modeIndex(const std::string& modeName);

    // The interval set currently chosen by the piano-roll Key/Mode/Scale UI.
    //   category == "scale": the named scale from scales(), used directly.
    //   otherwise          : Key is the PARENT and Mode is a rotation degree, so
    //                        the result is rotateScale(keys()[keyName],
    //                        modeIndex(modeName)). This is how Key and Mode
    //                        combine -- the Root control supplies the tonic, the
    //                        Key supplies the 7 notes, and the Mode picks which
    //                        of those notes is "home".
    static std::vector<int> activeIntervals(const std::string& category,
                                            const std::string& keyName,
                                            const std::string& modeName,
                                            const std::string& scaleName);

    static std::string noteName(int pitch);

    // ---- Note-name parsing (inverse of noteName) ----
    // Shared by every MIDI-scripting surface (Lua, Python, expressions) so a
    // user can write "C4" anywhere a note NUMBER is accepted. Octave convention
    // matches noteName(): C4 = MIDI 60, A4 = MIDI 69 (so pitch = (octave+1)*12
    // + pitch-class). Case-insensitive letter A-G; any number of accidentals,
    // '#' = +1 semitone, 'b' = -1 (so "C##4"=D4, "Cb4"=B3, "B#4"=C5 - standard
    // enharmonics, the accidental flows into the octave). Returns -1 on any
    // parse error OR a result outside 0..127, so callers can detect "not a
    // valid note name" and fall back / report.
    //
    //   parseNoteName("C#4")  -> 61   (octave embedded in the string)
    //   noteNumber("C", 4)    -> 60   (pitch-class string + explicit octave;
    //                                  the string must be letter+accidentals
    //                                  only, no embedded octave digits)
    // parseNoteName tolerates a missing octave and assumes defaultOctave then.
    static int parseNoteName(const std::string& s, int defaultOctave = 4);
    static int noteNumber(const std::string& pitchClass, int octave);

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

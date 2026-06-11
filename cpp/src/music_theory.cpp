#include "music_theory.h"
#include <algorithm>
#include <cstdlib>
#include <set>

namespace SoundShop {

const char* const MusicTheory::NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

const char* const MusicTheory::DEGREE_NAMES[7] = {
    "1st", "2nd", "3rd", "4th", "5th", "6th", "7th"
};

static ScaleMap s_keys = {
    {"Major",            {0, 2, 4, 5, 7, 9, 11}},
    {"Natural Minor",    {0, 2, 3, 5, 7, 8, 10}},
    {"Harmonic Minor",   {0, 2, 3, 5, 7, 8, 11}},
    {"Harmonic Major",   {0, 2, 4, 5, 7, 8, 11}},
    {"Melodic Minor",    {0, 2, 3, 5, 7, 9, 11}},
    {"Neapolitan Major", {0, 1, 3, 5, 7, 9, 11}},
    {"Neapolitan Minor", {0, 1, 3, 5, 7, 8, 11}},
    {"Double Harmonic",  {0, 1, 4, 5, 7, 8, 11}},
    {"Hungarian Minor",  {0, 2, 3, 6, 7, 8, 11}},
    {"Hungarian Major",  {0, 3, 4, 6, 7, 9, 10}},
};

// The seven modes of the major scale, in rotation order. These are NOT used as
// standalone interval sets by the piano-roll UI anymore: the "Mode" control
// selects a rotation DEGREE (this list's index) that is applied to the chosen
// "Key" (parent scale) via rotateScale(). For the Major parent the rotations
// reproduce exactly the intervals below, which is why index 0 is labelled
// "Ionian (Major)" -- Ionian *is* the major scale. detectKeys() still scans
// these interval sets so it can recognise a bare mode in a melody.
// (Modes of NON-major parents -- Phrygian Dominant, Acoustic, etc. -- live in
// s_scales instead, since they aren't rotations of the major scale; they're
// also reachable here as Key=<their parent> + a rotation degree.)
static ScaleMap s_modes = {
    {"Ionian (Major)",    {0, 2, 4, 5, 7, 9, 11}},
    {"Dorian",            {0, 2, 3, 5, 7, 9, 10}},
    {"Phrygian",          {0, 1, 3, 5, 7, 8, 10}},
    {"Lydian",            {0, 2, 4, 6, 7, 9, 11}},
    {"Mixolydian",        {0, 2, 4, 5, 7, 9, 10}},
    {"Aeolian",           {0, 2, 3, 5, 7, 8, 10}},
    {"Locrian",           {0, 1, 3, 5, 6, 8, 10}},
};

static ScaleMap s_scales = {
    {"Chromatic",             {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
    {"Major Pentatonic",      {0, 2, 4, 7, 9}},
    {"Minor Pentatonic",      {0, 3, 5, 7, 10}},
    {"Blues",                  {0, 3, 5, 6, 7, 10}},
    {"Whole Tone",            {0, 2, 4, 6, 8, 10}},
    {"Augmented",             {0, 3, 4, 7, 8, 11}},
    {"Bebop Dominant",        {0, 2, 4, 5, 7, 9, 10, 11}},
    {"Octatonic (W-H)",       {0, 2, 3, 5, 6, 8, 9, 11}},
    {"Octatonic (H-W)",       {0, 1, 3, 4, 6, 7, 9, 10}},
    {"Prometheus",            {0, 2, 4, 6, 9, 10}},
    {"Tritone",               {0, 1, 4, 6, 7, 10}},
    {"Two-Semitone Tritone",  {0, 1, 2, 6, 7, 8}},
    {"Enigmatic",             {0, 1, 4, 6, 8, 10, 11}},
    {"Persian",               {0, 1, 4, 5, 6, 8, 11}},
    {"Algerian",              {0, 2, 3, 6, 7, 8, 11}},
    {"Flamenco",              {0, 1, 4, 5, 7, 8, 11}},
    {"Romani",                {0, 2, 3, 6, 7, 8, 10}},
    {"Half-Diminished",       {0, 2, 3, 5, 6, 8, 10}},
    {"Harmonics",             {0, 3, 4, 5, 7, 9}},
    {"Hirajoshi",             {0, 4, 6, 7, 11}},
    {"In",                    {0, 1, 5, 7, 8}},
    {"Insen",                 {0, 1, 5, 7, 10}},
    {"Iwato",                 {0, 1, 5, 6, 10}},
    {"Yo",                    {0, 3, 5, 7, 10}},
    // Ancient Greek tetrachords (4 notes spanning a perfect fourth)
    {"Tetrachord (Diatonic)",  {0, 2, 4, 5}},           // W-W-H
    {"Tetrachord (Chromatic)", {0, 1, 3, 5}},            // H-m3-H
    // Named modes of NON-major parent scales. These aren't rotations of the
    // major scale, so they live here as fixed scales rather than in s_modes.
    // (Each is also reachable via Key + a rotation degree, e.g. Phrygian
    // Dominant = Key "Harmonic Minor" + Mode 5.)
    {"Lydian Augmented",  {0, 2, 4, 6, 8, 9, 11}},  // 3rd mode of melodic minor
    {"Locrian Major",     {0, 2, 4, 5, 6, 8, 10}},  // 5th mode of harmonic major
    {"Super Locrian",     {0, 1, 3, 4, 6, 8, 10}},  // 7th mode of melodic minor
    {"Phrygian Dominant", {0, 1, 4, 5, 7, 8, 10}},  // 5th mode of harmonic minor
    {"Acoustic",          {0, 2, 4, 6, 7, 9, 10}},  // 4th mode of melodic minor
    {"Ukrainian Dorian",  {0, 2, 3, 6, 7, 9, 10}},  // 4th mode of harmonic minor
    // NOTE: "Pythagorean" intentionally does NOT appear here. It's a *tuning*,
    // not a scale (its note selection is identical to Major/Ionian; what makes
    // it Pythagorean is the cent offsets, which live in the project tuning
    // system, TuningSystem::Pythagorean in tuning.h). Don't add it back as a
    // scale -- doing so was a non-functional duplicate of Major.
};

const ScaleMap& MusicTheory::keys()   { return s_keys; }
const ScaleMap& MusicTheory::modes()  { return s_modes; }
const ScaleMap& MusicTheory::scales() { return s_scales; }

std::vector<int> MusicTheory::rotateScale(const std::vector<int>& parent, int degree) {
    std::vector<int> out;
    int n = (int)parent.size();
    if (n == 0) return out;
    degree = ((degree % n) + n) % n;
    int base = parent[degree];
    for (int i = 0; i < n; ++i) {
        int v = parent[(degree + i) % n] - base;
        v = ((v % 12) + 12) % 12;   // wrap notes that crossed the octave
        out.push_back(v);
    }
    return out;                     // ascending by construction (one trip round)
}

int MusicTheory::modeIndex(const std::string& modeName) {
    const auto& m = modes();
    for (int i = 0; i < (int)m.size(); ++i)
        if (m[i].first == modeName) return i;
    if (modeName == "Ionian") return 0; // legacy spelling -> "Ionian (Major)"
    return 0;
}

std::vector<int> MusicTheory::activeIntervals(const std::string& category,
                                              const std::string& keyName,
                                              const std::string& modeName,
                                              const std::string& scaleName) {
    if (category == "scale") {
        if (auto* v = findScale(scales(), scaleName)) return *v;
        return {0, 2, 4, 5, 7, 9, 11};
    }
    // "keymode" (or legacy "key"/"mode"): Key is the parent, Mode is a rotation.
    const std::vector<int>* parent = findScale(keys(), keyName);
    std::vector<int> base = parent ? *parent : std::vector<int>{0, 2, 4, 5, 7, 9, 11};
    return rotateScale(base, modeIndex(modeName));
}

std::string MusicTheory::noteName(int pitch) {
    int note = pitch % 12;
    int octave = pitch / 12 - 1;
    return std::string(NOTE_NAMES[note]) + std::to_string(octave);
}

// Parse the pitch-class portion (letter + accidentals) at s[idx], advancing
// idx past what it consumes. semiOut is the semitone relative to C in this
// octave and MAY be negative or >11 (e.g. "Cb" = -1, "B#" = 12) so the caller
// can let an accidental spill into the adjacent octave, matching standard
// scientific-pitch enharmonics. Returns false if s[idx] isn't a letter A-G.
static bool parsePitchClass(const std::string& s, size_t& idx, int& semiOut) {
    // Base semitone for each letter (C=0 .. B=11).
    static const int kBase[7] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
    if (idx >= s.size()) return false;
    char c = s[idx];
    if (c >= 'a' && c <= 'g') c = (char)(c - 'a' + 'A');
    if (c < 'A' || c > 'G') return false;
    int semi = kBase[c - 'A'];
    ++idx;
    // Accidentals: '#' / '+' raise, 'b' / '-' would be ambiguous with the
    // octave sign, so only lowercase 'b' lowers. Allow runs ("##", "bb").
    while (idx < s.size()) {
        char a = s[idx];
        if (a == '#' || a == '+')      { semi += 1; ++idx; }
        else if (a == 'b' || a == 'B') { semi -= 1; ++idx; } // 'B' here is a flat,
                                                             // a leading note-B was
                                                             // already consumed above
        else break;
    }
    semiOut = semi;
    return true;
}

int MusicTheory::parseNoteName(const std::string& s, int defaultOctave) {
    size_t idx = 0;
    // Skip leading whitespace so "  C4" still parses.
    while (idx < s.size() && (s[idx] == ' ' || s[idx] == '\t')) ++idx;
    int semi;
    if (!parsePitchClass(s, idx, semi)) return -1;
    // Optional octave: sign + digits. Missing => defaultOctave.
    int octave = defaultOctave;
    {
        size_t j = idx;
        bool neg = false;
        if (j < s.size() && (s[j] == '-' || s[j] == '+')) { neg = (s[j] == '-'); ++j; }
        size_t digitsStart = j;
        int val = 0;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') { val = val * 10 + (s[j] - '0'); ++j; }
        if (j > digitsStart) { octave = neg ? -val : val; idx = j; }
        else if (neg) return -1; // a lone '-' with no digits is malformed
    }
    // Trailing whitespace is fine; anything else means it wasn't a note name.
    while (idx < s.size() && (s[idx] == ' ' || s[idx] == '\t')) ++idx;
    if (idx != s.size()) return -1;
    int pitch = (octave + 1) * 12 + semi;
    if (pitch < 0 || pitch > 127) return -1;
    return pitch;
}

int MusicTheory::noteNumber(const std::string& pitchClass, int octave) {
    size_t idx = 0;
    while (idx < pitchClass.size() && (pitchClass[idx] == ' ' || pitchClass[idx] == '\t')) ++idx;
    int semi;
    if (!parsePitchClass(pitchClass, idx, semi)) return -1;
    while (idx < pitchClass.size() && (pitchClass[idx] == ' ' || pitchClass[idx] == '\t')) ++idx;
    // Explicit-octave form: the string is the pitch class only, so any leftover
    // (including embedded octave digits) is a usage error - reject it rather
    // than silently ignoring a second octave source.
    if (idx != pitchClass.size()) return -1;
    int pitch = (octave + 1) * 12 + semi;
    if (pitch < 0 || pitch > 127) return -1;
    return pitch;
}

bool MusicTheory::isBlackKey(int pitch) {
    int n = pitch % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

int MusicTheory::snapToScale(int pitch, int root, const std::vector<int>& scale) {
    int note = pitch % 12;
    int rel = ((note - root) % 12 + 12) % 12;
    int bestDist = 999;
    int bestSemi = 0;
    for (int s : scale) {
        int dist = std::abs(rel - s);
        int distWrap = 12 - dist;
        int d = std::min(dist, distWrap);
        if (d < std::abs(bestDist)) {
            bestDist = d;
            bestSemi = s;
        }
    }
    return pitch - rel + bestSemi;
}

DegreeInfo MusicTheory::pitchToDegree(int pitch, int root, const std::vector<int>& scale) {
    DegreeInfo info;
    int note = ((pitch - root) % 12 + 12) % 12;
    info.octave = (pitch - root) / 12;

    int bestDeg = 0;
    int bestDist = 999;
    for (int i = 0; i < (int)scale.size(); ++i) {
        int dist = note - scale[i];
        if (std::abs(dist) < std::abs(bestDist)) {
            bestDist = dist;
            bestDeg = i;
        }
        int distWrap = note - (scale[i] + 12);
        if (std::abs(distWrap) < std::abs(bestDist)) {
            bestDist = distWrap;
            bestDeg = i;
        }
    }
    info.degree = bestDeg;
    info.chromaticOffset = bestDist;
    return info;
}

int MusicTheory::degreeToPitch(int degree, int octave, int chromaticOffset,
                                int root, const std::vector<int>& scale) {
    int sz = (int)scale.size();
    int degInScale = ((degree % sz) + sz) % sz;
    int extraOctaves = degree / sz;
    int semitone = scale[degInScale];
    return root + (octave + extraOctaves) * 12 + semitone + chromaticOffset;
}

std::vector<MusicTheory::KeyMatch> MusicTheory::detectKeys(const std::vector<int>& pitches) {
    std::vector<KeyMatch> results;
    if (pitches.empty()) return results;

    // Collect unique pitch classes
    std::set<int> pitchClasses;
    for (int p : pitches)
        pitchClasses.insert(((p % 12) + 12) % 12);

    int numInputNotes = (int)pitchClasses.size();

    // Try every root (0-11) against every scale in all categories
    struct CatTable {
        const char* category;
        const ScaleMap* table;
    };
    CatTable tables[] = {
        {"key", &keys()},
        {"mode", &modes()},
        {"scale", &scales()},
    };

    for (auto& [category, table] : tables) {
        for (auto& [scaleName, intervals] : *table) {
            int scaleSize = (int)intervals.size();
            // Skip chromatic - everything matches, not useful
            if (scaleSize >= 12) continue;
            // Skip scales smaller than the input - can't contain all notes
            if (scaleSize < numInputNotes) continue;

            for (int root = 0; root < 12; ++root) {
                // Build the set of pitch classes for this root + scale
                std::set<int> scaleSet;
                for (int s : intervals)
                    scaleSet.insert((s + root) % 12);

                // Check if all input pitches are in the scale
                bool allMatch = true;
                for (int pc : pitchClasses) {
                    if (scaleSet.find(pc) == scaleSet.end()) {
                        allMatch = false;
                        break;
                    }
                }

                if (allMatch) {
                    float coverage = (float)numInputNotes / (float)scaleSize;
                    results.push_back({root, scaleName, category,
                                       scaleSize, numInputNotes, coverage});
                }
            }
        }
    }

    // Sort by: coverage descending (tighter fit first), then scale size ascending
    // (prefer simpler scales), then by root
    std::sort(results.begin(), results.end(), [](const KeyMatch& a, const KeyMatch& b) {
        if (std::abs(a.coverage - b.coverage) > 0.001f)
            return a.coverage > b.coverage; // higher coverage first
        if (a.scaleSize != b.scaleSize)
            return a.scaleSize < b.scaleSize; // smaller scales first
        if (a.root != b.root)
            return a.root < b.root;
        return a.scaleName < b.scaleName;
    });

    return results;
}

} // namespace SoundShop

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

// Modes in order of rotation (standard music theory order)
static ScaleMap s_modes = {
    {"Ionian",            {0, 2, 4, 5, 7, 9, 11}},
    {"Dorian",            {0, 2, 3, 5, 7, 9, 10}},
    {"Phrygian",          {0, 1, 3, 5, 7, 8, 10}},
    {"Lydian",            {0, 2, 4, 6, 7, 9, 11}},
    {"Mixolydian",        {0, 2, 4, 5, 7, 9, 10}},
    {"Aeolian",           {0, 2, 3, 5, 7, 8, 10}},
    {"Locrian",           {0, 1, 3, 5, 6, 8, 10}},
    // Extended modes
    {"Lydian Augmented",  {0, 2, 4, 6, 8, 9, 11}},
    {"Locrian Major",     {0, 2, 4, 5, 6, 8, 10}},
    {"Super Locrian",     {0, 1, 3, 4, 6, 8, 10}},
    {"Phrygian Dominant", {0, 1, 4, 5, 7, 8, 10}},
    {"Acoustic",          {0, 2, 4, 6, 7, 9, 10}},
    {"Ukrainian Dorian",  {0, 2, 3, 6, 7, 9, 10}},
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
    // Pythagorean — same note selection as Major in 12-TET
    // (true Pythagorean tuning requires microtuning for the cent offsets)
    {"Pythagorean",            {0, 2, 4, 5, 7, 9, 11}}, // same as Major/Ionian
};

const ScaleMap& MusicTheory::keys()   { return s_keys; }
const ScaleMap& MusicTheory::modes()  { return s_modes; }
const ScaleMap& MusicTheory::scales() { return s_scales; }

std::string MusicTheory::noteName(int pitch) {
    int note = pitch % 12;
    int octave = pitch / 12 - 1;
    return std::string(NOTE_NAMES[note]) + std::to_string(octave);
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
            // Skip chromatic — everything matches, not useful
            if (scaleSize >= 12) continue;
            // Skip scales smaller than the input — can't contain all notes
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

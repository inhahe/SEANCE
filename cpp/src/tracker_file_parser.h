#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace SoundShop {

// ==============================================================================
// Raw-file parser for tracker module instrument metadata.
//
// libopenmpt only exposes names for instruments (openmpt_module_get_instrument_name),
// nothing about their keymap / envelopes / NNA / fadeout / default pan / filter
// defaults — all of which are essential for importing instrument-mode IT and
// XM songs with fidelity. This parser opens the raw file bytes directly and
// walks the format-specific instrument headers to extract that data.
//
// We only parse metadata, not sample PCM — libopenmpt's render-and-capture
// path (see mod_import.cpp's extractSampleToWav) handles PCM extraction.
//
// Formats currently supported:
//   - IT: Impulse Tracker .it files (modern format, IMPI headers)
//   - XM: FastTracker II .xm files (header tag "Extended Module:")
//
// Sample-mode formats (MOD, S3M, or XM/IT with numInstruments == 0) don't
// have instrument headers at all — the caller falls back to the per-sample
// tracker-import path for those.
// ==============================================================================

struct TrackerEnvelopePoint {
    float tick = 0;   // in ticks (IT uses 0..9999 range; we convert to seconds later)
    float value = 0;  // volume envelope: 0..64; pan envelope: -32..+32; pitch: -32..+32
};

struct TrackerEnvelope {
    bool enabled = false;
    bool hasLoop = false;
    bool hasSustain = false;
    bool isFilterEnv = false;      // IT pitch-envelope flag bit 7 reuses it as filter env
    int  loopStart = 0;
    int  loopEnd   = 0;
    int  sustainStart = 0;
    int  sustainEnd   = 0;
    std::vector<TrackerEnvelopePoint> points;
};

struct TrackerNoteMapEntry {
    int note   = 0;   // MIDI-like note number after the instrument's keymap lookup
    int sample = 0;   // 1-based sample id to play; 0 = silence
};

enum class TrackerNNA : int { Cut = 0, Continue = 1, Off = 2, Fade = 3 };

struct TrackerInstrument {
    std::string name;

    // 120-entry note→sample map. Index is the raw MIDI note (0..119).
    // For IT, the map also lets a pattern note be re-mapped to a different
    // playback note — noteMap[raw].note is the note that actually gets
    // played back, noteMap[raw].sample is which sample plays it.
    std::vector<TrackerNoteMapEntry> noteMap;

    // Envelopes (IT has three, XM has two plus a vibrato).
    TrackerEnvelope volumeEnv;
    TrackerEnvelope panEnv;
    TrackerEnvelope pitchEnv;    // IT-only; carries filter env when isFilterEnv

    // New-note action defaults (IT instrument mode).
    TrackerNNA newNoteAction = TrackerNNA::Cut;

    // Fadeout rate per IT/XM semantics. IT: 0..1024, representing the
    // amount the "fade volume" drops per tick once the note is released.
    // 0 means "no fadeout" (note rings until release envelope ends).
    int fadeOut = 0;

    // Default volume (0..64 for IT, 0..64 scaled to 0..128 for XM).
    int defaultVolume = 128;

    // Default pan: -1..+1, or a sentinel value (isnan) meaning "no
    // instrument-level pan override" — pan is then determined by the
    // playing sample or by channel state.
    bool  hasDefaultPan = false;
    float defaultPan = 0.0f;

    // Random volume/pan variation percentages (0..100). IT only.
    int randomVol = 0;
    int randomPan = 0;

    // Initial filter cutoff / resonance. For IT, cutoff and resonance each
    // have a "use" bit — when false the instrument inherits the channel
    // default. Cutoff 0..127, resonance 0..127.
    bool useFilterCutoff = false;
    int  filterCutoff = 127;
    bool useFilterResonance = false;
    int  filterResonance = 0;
};

// MIDI macro configuration (IT only). Each macro is a template string
// up to 31 bytes with placeholder tokens ('z' for Zxx param value,
// 'n' for note, 'v' for velocity, etc.). The importer expands these
// at pattern-walk time and emits the resulting MIDI bytes as CC events.
struct TrackerMidiMacros {
    // 16 "SFx" parametric macros (invoked by SF0..SFF subcommands).
    std::string sfMacros[16];
    // 128 "Zxx" fixed macros (invoked by Z00..Z7F). Index = Zxx value.
    std::string zMacros[128];
};

struct ParsedTrackerFile {
    enum class Format { Unknown, IT, XM } format = Format::Unknown;
    int numInstruments = 0;
    std::vector<TrackerInstrument> instruments; // 1-based indexing — instruments[0] is unused
    TrackerMidiMacros midiMacros;
    bool hasMidiMacros = false;
    std::string errorMessage;   // set when parsing failed; instruments is empty
};

// Parse a module file's instrument metadata. Returns a ParsedTrackerFile
// whose `instruments` list is populated on success. On failure (unknown
// format, truncated file, corrupt headers) the returned object has
// format == Unknown and errorMessage set — callers should fall back to
// sample-mode import.
ParsedTrackerFile parseTrackerFile(const std::string& path);

} // namespace SoundShop

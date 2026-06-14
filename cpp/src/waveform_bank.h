#pragma once

#include <juce_core/juce_core.h>
#include <string>
#include <vector>
#include <unordered_map>

// =============================================================================
// WaveformBank — the built-in "factory" single-cycle waveform library.
// =============================================================================
//
// SEANCE ships a large library of pre-made single-cycle waveforms (sourced from
// the public-domain Adventure Kid Waveforms / AKWF set, ~4000 cycles) so a user
// can drop a rich oscillator shape into a wavetable without drawing or capturing
// one. Each waveform is one cycle, peak-normalised, stored at a fixed 512-sample
// resolution — exactly the shape of a Drawn/Freehand WaveLayer's drawnSamples,
// so importing a factory waveform is a straight copy into a layer (see
// makeFactoryFrame). The 512 samples are linearly interpolated to whatever table
// size the synth renders at.
//
// On disk the whole library is ONE packed binary asset, `waveforms.bin`, sitting
// next to the executable (copied there by CMake, same as docs/). We deliberately
// do NOT ship 4000 loose .wav files or embed them in the binary: one mmap-style
// read of a ~4 MB file is far cheaper than opening thousands of files, and keeps
// the build fast. The packer that produces it is cpp/tools/pack_waveforms.py.
//
// Binary format (all little-endian — the asset is produced and consumed on the
// same x86 toolchain; the loader validates the magic and bails cleanly if a
// future big-endian port ever reads it):
//
//   magic        : 4 bytes  'S','S','W','B'
//   version      : uint32   == 1
//   sampleCount  : uint32   samples per waveform (== kSampleCount, 512)
//   entryCount   : uint32   number of waveforms
//   index        : entryCount records, each:
//                    curated : uint8   (1 = in the curated "best of" subset)
//                    nameLen : uint16  ; name  bytes (UTF-8, no terminator)
//                    catLen  : uint16  ; category bytes (UTF-8, no terminator)
//   samples      : entryCount * sampleCount int16 (LE), each / 32767 → [-1,1]
//
// The bank is a process-wide singleton, loaded lazily on first access (the first
// time the user opens the factory-waveform browser). If the asset is missing or
// malformed, the bank loads empty and the browser shows an explanatory message
// rather than crashing.

class WaveformBank {
public:
    static constexpr int kSampleCount = 512;  // samples per stored cycle

    struct Entry {
        std::string name;       // display name, e.g. "AKWF oboe 0001"
        std::string category;   // group, e.g. "Oboe" — used for the browser tree
        bool        curated = false;  // true → ★ + sorted to top of its category
        int         sampleStart = 0;  // index into `samples` (in floats)
    };

    // Process-wide instance. Lazily empty until load() succeeds.
    static WaveformBank& get();

    // Attempt to load the packed asset. Searches next to the executable first
    // (where CMake copies it), then a couple of dev-tree fallbacks. Safe to call
    // repeatedly — it's a no-op once loaded. Returns true if the bank now holds
    // at least one waveform.
    bool ensureLoaded();

    bool isLoaded() const { return loaded; }
    bool isEmpty()  const { return entries.empty(); }

    // Why loading failed (empty when loaded OK or not yet attempted). Surfaced
    // by the browser so a missing asset explains itself instead of looking like
    // an empty library.
    const std::string& loadError() const { return error; }

    int numEntries() const { return (int)entries.size(); }
    const Entry& entry(int i) const { return entries[(size_t)i]; }

    // Distinct category names, in first-seen order (matches the packer's folder
    // ordering). Each category's entries are already curated-first then
    // alphabetical (the packer sorts them), so the browser can render the tree
    // straight from entriesInCategory without re-sorting.
    const std::vector<std::string>& categories() const { return categoryOrder; }

    // Indices (into entries) belonging to a category, in display order.
    std::vector<int> entriesInCategory(const std::string& cat) const;

    // The 512 normalised samples for entry i, copied out (ready to assign to a
    // WaveLayer::drawnSamples). Returns kSampleCount zeros if i is out of range.
    std::vector<float> samples(int i) const;

    // ---- Generator-language access (the cross-language waveform() function) ---
    //
    // The terrain/wavetable generator languages (Builtin / Lua / Python / GLSL)
    // expose a single helper `waveform(id, phase)` that reads one of these factory
    // cycles by its STABLE ENTRY INDEX — the same integer for every language, and
    // the integer shown in the factory-waveform browser and returned by Lua/Python
    // `waveforms["name"]`. Builtin/Lua/Python also accept the name string directly
    // (a convenience name->index lookup); GLSL is integer-only (no strings on the
    // GPU). These methods back all of that: indexForName() turns a name into its
    // stable index, sampleData()/sampleAtPhase() read one entry, and
    // allSampleData()/totalSamples() expose the whole packed blob for a one-shot
    // bulk upload to a GLSL SSBO (the entire bank is uploaded once, globally
    // indexed, so a shader can read any waveform by index with no source rewrite).
    // The name lookups are case-insensitive (the display names are mixed-case).

    // Entry index for a waveform by name, or -1 if no such name. Case-insensitive,
    // leading/trailing whitespace ignored. Builds a lookup map on first call.
    int indexForName(const std::string& name) const;

    // Pointer to entry i's kSampleCount contiguous floats in the in-memory blob,
    // or nullptr if i is out of range. The pointer is valid for the process
    // lifetime (the blob is never reallocated after load).
    const float* sampleData(int i) const;

    // Pointer to the ENTIRE packed sample blob: numEntries()*kSampleCount
    // contiguous floats with entry i starting at i*kSampleCount (so a flat index
    // i*kSampleCount + s reads sample s of entry i). nullptr if the bank is empty.
    // Used to upload the whole library to a GLSL SSBO in one shot so the shader's
    // waveform(int id, phase) can index any cycle by its stable entry index. The
    // pointer is valid for the process lifetime (the blob is never reallocated).
    const float* allSampleData() const { return sampleBlob.empty() ? nullptr : sampleBlob.data(); }

    // Total floats in the packed blob (== numEntries()*kSampleCount). 0 if empty.
    size_t totalSamples() const { return sampleBlob.size(); }

    // Read entry i at a normalised phase in [0,1) (wraps), linearly interpolated
    // across the kSampleCount-sample cycle. Returns the raw sample in [-1,1], or
    // 0 if i is out of range. This is the exact semantics every generator
    // language's waveform(name, phase) uses, so they all read identically.
    float sampleAtPhase(int i, float phase) const;

private:
    WaveformBank() = default;

    bool loadFrom(const juce::File& f);

    bool loaded = false;
    std::string error;
    std::vector<Entry> entries;
    std::vector<float> sampleBlob;          // entryCount * kSampleCount floats
    std::vector<std::string> categoryOrder; // first-seen category order

    // Lazily-built case-insensitive name -> entry index map (keys are the entry
    // names lowercased + trimmed). Mutable so the const lookup can populate it.
    mutable std::unordered_map<std::string, int> nameLookup;
};

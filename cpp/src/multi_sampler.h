#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <string>
#include <vector>

namespace SoundShop {

// ==============================================================================
// MultiSampler: a zone-based sampling instrument.
//
// A MultiSampler node holds N sample zones, each pointing at a WAV file and
// tagged with a key range (low/high MIDI note) and velocity range. When a
// MIDI note arrives, the processor picks the zone(s) that match and plays
// them back pitch-shifted from the zone's base note to the triggered note.
//
// Overlapping zones are allowed: every matching zone fires, layered. This
// lets you do velocity-layered instruments (soft/loud samples overlap
// vertically, split horizontally on velocity) and key-layered pianos
// (different samples per octave). Non-overlapping single-zone configs are
// the simplest case and replace what the old __audio__: single-file
// Sampler used to do.
//
// Playback features:
//   - Linear-interpolated pitch shift (source rate -> triggered pitch)
//   - Per-zone loop region (start/end in source samples)
//   - Polyphonic voice pool with ADSR envelope per voice
//   - Multi-point volume envelope (if present, replaces ADSR decay)
//   - Simple state-variable filter with cutoff + resonance + mode
//   - Global volume / pan, per-zone gain / pan / fineTune
//
// Persistence: the doc encodes to a plain-text multi-line format prefixed
// with "__multisampler__:" and stored in node.script. See encode()/decode().
// ==============================================================================

// Linear-interpolated breakpoint envelope. Matches IT / OpenMPT semantics
// (all segments linear, sustain clamps at a chosen point while the note
// is held, optional loop between two points while the note is held).
struct SamplerEnvelope {
    struct Point { float time = 0; float value = 0; };
    std::vector<Point> points;

    bool  hasSustain = false;
    int   sustainStart = 0;    // indices into points[]
    int   sustainEnd   = 0;
    bool  hasLoop = false;
    int   loopStart = 0;
    int   loopEnd   = 0;

    // Evaluate at `t` seconds since note-on. `noteHeld` controls whether
    // sustain/loop clamps are active - once the note is released, the
    // envelope runs from its current position to the end without looping.
    float evaluate(float t, bool noteHeld) const;

    bool empty() const { return points.empty(); }
};

struct MultiSamplerZone {
    std::string samplePath;
    int   loNote  = 0;
    int   hiNote  = 127;
    int   loVel   = 1;
    int   hiVel   = 127;
    int   baseNote = 60;        // MIDI note at which the sample plays unpitched
    float fineTuneCents = 0.0f;
    float gainDb  = 0.0f;
    float pan     = 0.0f;       // -1..1
    bool  loopEnabled = false;
    int   loopStart   = 0;      // in source samples
    int   loopEnd     = 0;      // in source samples (exclusive)

    // ---- Loaded on prepareToPlay, not serialized ----
    std::vector<float> dataL, dataR; // interleaved to stereo; mono files have L==R
    double fileSampleRate = 44100;
    int    lengthSamples  = 0;
};

// Interpolation quality for sample playback.
enum class InterpMode {
    Linear = 0,   // 2-point linear - authentic tracker sound, some aliasing
    Cubic  = 1,   // 4-point Catmull-Rom - good balance
    Sinc   = 2,   // 8-point Lanczos-4 - highest quality, least aliasing
};

struct MultiSamplerDoc {
    std::vector<MultiSamplerZone> zones;

    // Global envelopes. Empty envelope falls through to the ADSR below.
    SamplerEnvelope volumeEnv;   // 0..1 gain
    SamplerEnvelope panEnv;      // -1..1
    SamplerEnvelope pitchEnv;    // semitones (typically -24..+24)
    SamplerEnvelope filterEnv;   // 0..1 cutoff multiplier

    // Simple ADSR fallback (used when volumeEnv is empty).
    float attack  = 0.005f;
    float decay   = 0.05f;
    float sustain = 1.0f;
    float release = 0.1f;

    // Filter. cutoff in Hz, resonance 0..1, mode: 0=LP, 1=HP, 2=BP, 3=off.
    float filterCutoff    = 20000.0f;
    float filterResonance = 0.1f;
    int   filterMode      = 3;

    // Interpolation mode for sample playback.  New instruments default to
    // Sinc (highest quality); tracker imports default to Linear to match
    // the authentic tracker sound.
    InterpMode interpMode = InterpMode::Sinc;

    // Amiga-style output lowpass - emulates the PAULA chip's analog
    // reconstruction filter that all MOD-era trackers rendered through.
    // When on, a 4-pole Butterworth LP is applied to the post-mix output
    // at amigaFilterHz.  Default is off (modern instruments); MOD/S3M
    // imports turn it on with a ~5.5 kHz cutoff which matches OpenMPT /
    // Winamp's reference output spectrum (brick-wall rolloff ~5-7 kHz).
    // Without this filter, sample playback above the source's Nyquist
    // produces aliased high-frequency content that the reference doesn't
    // have - the audible "texture" mismatch.
    bool  amigaFilter   = false;
    float amigaFilterHz = 5500.0f;

    // NOTE: global Volume and Pan are NOT part of the doc - they live on
    // the node as real Params (node.params) so automation lanes and
    // signal cables can target them. The processor reads them via
    // getParamByName at block time.

    // Encode to a plain-text multi-line format with the sentinel prefix.
    // The returned string is suitable for assigning to node.script so the
    // project file round-trips it.
    std::string encode() const;

    // Parse a string produced by encode() back into this doc. Returns
    // false if the header is missing or malformed. Partial parsing is
    // tolerated: unknown keys are skipped, missing keys keep defaults.
    bool decode(const std::string& script);

    // Script sentinel prefix.
    static constexpr const char* kPrefix = "__multisampler__:";
};

// ==============================================================================
// Processor
// ==============================================================================
class MultiSamplerProcessor : public juce::AudioProcessor {
public:
    MultiSamplerProcessor(Node& n);

    const juce::String getName() const override { return "MultiSampler"; }
    void  prepareToPlay(double sr, int /*bs*/) override;
    void  releaseResources() override {}
    void  processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;
    // Tail = how long the sampler can keep producing audio after the last
    // note-off.  Derived from the doc, not a magic constant:
    //   - ADSR path: just doc.release.
    //   - Multi-point volumeEnv: time from sustainEnd to last envelope
    //     point, plus the release fade - matches the voice-kill condition
    //     in processBlock (timeHeld >= points.back().time + doc.release).
    double getTailLengthSeconds() const override {
        double t = (double) doc.release;
        if (!doc.volumeEnv.empty() && doc.volumeEnv.hasSustain) {
            const auto& pts = doc.volumeEnv.points;
            int se = doc.volumeEnv.sustainEnd;
            if (se >= 0 && se < (int) pts.size()) {
                double susEndT = (double) pts[se].time;
                double lastT   = (double) pts.back().time;
                t = std::max(t, (lastT - susEndT) + (double) doc.release);
            }
        }
        return t;
    }
    bool  acceptsMidi() const override { return true; }
    bool  producesMidi() const override { return false; }
    bool  isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int   getNumPrograms() override { return 1; }
    int   getCurrentProgram() override { return 0; }
    void  setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void  changeProgramName(int, const juce::String&) override {}
    void  getStateInformation(juce::MemoryBlock&) override {}
    void  setStateInformation(const void*, int) override {}

private:
    Node& node;
    MultiSamplerDoc doc;
    std::string lastLoadedScript;     // tracks which script doc we parsed/loaded
    double sampleRate = 44100;

    struct Voice {
        bool   active = false;
        int    zoneIdx = -1;
        int    midiNote = 0;
        float  velocity = 1.0f;
        double phase = 0.0;          // read head in source samples (fractional)
        double rate  = 1.0;          // source samples per output sample
        float  timeHeld = 0.0f;      // seconds since note-on
        bool   noteHeld = true;
        float  releasedAtTime = 0.0f; // timeHeld when note-off fired
        // Simple SVF filter state, per stereo channel.
        std::array<float, 2> svfLow {0, 0};
        std::array<float, 2> svfBand{0, 0};
    };
    std::vector<Voice> voices;

    // Amiga reconstruction-filter state.  Two cascaded biquad LP sections
    // give a 4-pole Butterworth response (~24 dB/oct).  Coefficients are
    // recomputed in prepareToPlay so we don't allocate per block.  Per
    // stereo channel × 2 stages × 2 state delays.
    float amigaB0 = 1, amigaB1 = 0, amigaB2 = 0, amigaA1 = 0, amigaA2 = 0;
    std::array<std::array<float, 2>, 2> amigaZ1 {}; // [stage][channel]
    std::array<std::array<float, 2>, 2> amigaZ2 {};

    // Diagnostic counters - rate-limit the silent-MOD-playback debug logs so
    // the log doesn't flood. See multi_sampler.cpp comments at each use site.
    bool diagFirstProcess = true;
    bool diagFirstMidiSeen = false;
    int  diagNoteOnsLogged = 0;
    int  diagUnmatchedLogged = 0;

    // Re-parse the script and reload WAV files into zones if node.script
    // has changed since the last call. Called at the top of processBlock
    // so runtime edits to the doc take effect without a graph rebuild.
    void reloadIfNeeded();
    void loadZoneSamples();

    // Return the first voice slot currently marked inactive, or the
    // oldest (simple LRU by timeHeld) if all are busy.
    Voice& allocateVoice();

    // Find every zone whose (loNote..hiNote, loVel..hiVel) rectangle
    // covers the given note+velocity. Caller starts one voice per match.
    std::vector<int> findZonesFor(int midiNote, int velocity) const;
};

} // namespace SoundShop

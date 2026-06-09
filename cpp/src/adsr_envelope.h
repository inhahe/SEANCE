#pragma once
#include "curve_editor.h"
#include <juce_core/juce_core.h>
#include <string>
#include <vector>

namespace SoundShop {

// ==============================================================================
// AHDSREnvelope - shared amplitude envelope model used by every tonal /
// note-triggered synth in SoundShop. Five time-axis stages instead of
// the usual four:
//
//   Attack  : 0 -> 1 over `attackMs` milliseconds
//   Hold    : stays at peak (flat) for `holdMs` milliseconds
//   Decay   : 1 -> sustain over `decayMs` milliseconds
//   Sustain : stays at `sustain` (flat) for as long as the key is held
//   Release : current level -> 0 over `releaseMs` milliseconds after note-off
//
// The Attack / Decay / Release stages each have an independent
// SpectralCurve describing the shape of the ramp (linear, exponential,
// S-curve, freehand, etc.) using the same three-mode (Equation / Drawn /
// Freehand) editor used elsewhere in the app. Hold and Sustain are flat by
// definition so they have no curve.
//
// `velocitySensitivity` (0..1) scales the envelope's peak amplitude by the
// note's MIDI velocity:
//   peak = lerp(1.0, velocity/127, velocitySensitivity)
// At 0 every note plays at full amplitude regardless of how hard the key
// was struck (organ-like). At 1 a quiet key press gives a quiet note
// (piano-like).
// ==============================================================================
struct AHDSREnvelope {
    float attackMs   = 5.0f;
    float holdMs     = 0.0f;
    float decayMs    = 200.0f;
    float sustain    = 0.7f;   // 0..1
    float releaseMs  = 300.0f;
    float velocitySensitivity = 1.0f; // 0..1

    // Default shapes are linear ramps. The editor seeds Attack with
    // expression "x", Decay with "1-x", Release with "1-x" so the
    // first time the user opens an editor for an unset curve they
    // see something sensible rather than a flat line.
    SpectralCurve attackCurve;
    SpectralCurve decayCurve;
    SpectralCurve releaseCurve;

    AHDSREnvelope();

    // Default-construct curves so they hold the standard linear shapes
    // even before the user touches them. Without this every fresh
    // envelope would render as a flat 0 in the preview.
    static void setDefaultCurves(AHDSREnvelope& e);

    // Serialize / deserialize for project files, presets, and node.script.
    // The string contains no newlines and no '|' characters so it can be
    // safely embedded inside any '|'-delimited or line-based payload.
    // Per-segment curves are encoded via SpectralCurve::encode() (which
    // is itself '|'-safe).
    std::string encode() const;
    static bool decode(const std::string& s, AHDSREnvelope& out);
};

// ==============================================================================
// AHDSRCurveTables - the shared, per-node baked lookup tables for one
// AHDSREnvelope's Attack / Decay / Release shape curves. A synth processor
// holds ONE of these and calls prepare(env) once per processBlock (or
// whenever the envelope changed). The bake is hashed so it is a no-op when
// the curves are unchanged. All voices of that node sample these same tables,
// so the 256-sample curve evaluation happens once per node, not once per
// voice. This is the "defining the curves" half of the shared envelope code.
// ==============================================================================
class AHDSRCurveTables {
public:
    // Rebake the Attack / Decay / Release tables from the envelope's curves.
    // Cheap (early-out) when the curves match the last bake.
    void prepare(const AHDSREnvelope& env);

    // Sample a baked curve at t in 0..1 (linear interp between table entries).
    float attack(float t)  const { return sample(attackTable,  t); }
    float decay(float t)   const { return sample(decayTable,   t); }
    float release(float t) const { return sample(releaseTable, t); }

    static constexpr int kTableSize = 256;

private:
    static float sample(const std::vector<float>& tbl, float t);
    std::vector<float> attackTable;
    std::vector<float> decayTable;
    std::vector<float> releaseTable;
    size_t lastCurveHash = 0;     // skip rebake when curves haven't changed
};

// ==============================================================================
// AHDSREnvelopeRuntime - per-voice playback state for the envelope above.
// Synths construct one per voice; on note-on they call noteOn(velocity); on
// note-off they call noteOff(); each sample they call tick(sampleRate,
// envelope, tables) to advance and read the current 0..1 amplitude. The
// tables are owned by the processor (one per node) and passed in. This is the
// "applying the curves while playing" half of the shared envelope code.
//
// Velocity: noteOn() stores the 0..1 velocity. tick() scales the *target
// peak* by the envelope's velocitySensitivity, so a synth routing its master
// amplitude through this runtime must NOT also multiply by velocity itself.
// ==============================================================================
class AHDSREnvelopeRuntime {
public:
    enum class Stage { Off, Attack, Hold, Decay, Sustain, Release };

    void noteOn(float velocity01);
    void noteOff();

    // Advance one sample and return the current 0..1 amplitude.
    float tick(float sampleRate, const AHDSREnvelope& env,
               const AHDSRCurveTables& tables);

    bool isActive() const { return stage != Stage::Off; }
    Stage currentStage() const { return stage; }
    float level() const { return currentLevel; }   // for voice-steal "quietest"

    // Forcibly silence + reset (for steal / panic). Does not transition
    // through release.
    void hardReset();

private:
    Stage stage = Stage::Off;
    double timeInStage = 0.0;
    float currentLevel = 0.0f;
    float peak = 1.0f;            // scaled by velocity at note-on
    float releaseStartLevel = 0.0f;
};

} // namespace SoundShop

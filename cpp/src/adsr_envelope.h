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
//   Hold    : stays near 1 for `holdMs` milliseconds (shapeable plateau)
//   Decay   : 1 -> sustain over `decayMs` milliseconds
//   Sustain : stays at `sustain` (flat) for as long as the key is held
//   Release : current level -> 0 over `releaseMs` milliseconds after note-off
//
// The Attack / Hold / Decay / Release stages each have an independent
// SpectralCurve describing the shape of the ramp (linear, exponential,
// S-curve, freehand, etc.) using the same three-mode (Equation / Drawn /
// Freehand) editor used elsewhere in the app. The Hold curve is a multiplier
// on the peak level across the hold window: its default expression is the
// constant "1" (a true flat plateau at peak), but it can be shaped into a
// swell or a dip during the hold. The curve always re-arrives at peak at the
// end of Hold so the following Decay starts from peak without a discontinuity.
// Sustain is flat by definition so it has no curve.
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
    SpectralCurve holdCurve;
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
// AHDSREnvelopeRuntime - per-voice runtime state for the envelope above.
// Synths construct one per voice; on note-on they call noteOn(velocity);
// on note-off they call noteOff(); each sample they call tick(sampleRate,
// envelope) to advance and read the current 0..1 amplitude.
//
// The runtime owns the precomputed curve tables (256-sample lookup per
// stage) so curve evaluation in the audio thread is a cheap linear lerp
// of two table entries.
// ==============================================================================
class AHDSREnvelopeRuntime {
public:
    enum class Stage { Off, Attack, Hold, Decay, Sustain, Release };

    void noteOn(float velocity01);
    void noteOff();

    // Called once per processBlock (or whenever the envelope has changed)
    // to refresh the curve lookup tables. Cheap if the envelope hasn't
    // changed since the last call (it stores the hash of the curves it
    // last baked from and skips the work when unchanged).
    void prepareCurves(const AHDSREnvelope& env);

    // Advance one sample and return the current 0..1 amplitude.
    float tick(float sampleRate, const AHDSREnvelope& env);

    bool isActive() const { return stage != Stage::Off; }
    Stage currentStage() const { return stage; }

    // Forcibly silence + reset (for steal / panic). Does not transition
    // through release.
    void hardReset();

private:
    Stage stage = Stage::Off;
    double timeInStage = 0.0;
    float currentLevel = 0.0f;
    float peak = 1.0f;            // scaled by velocity at note-on
    float releaseStartLevel = 0.0f;

    // 256-sample precomputed curve tables (x in 0..1 -> shape in 0..1).
    static constexpr int kTableSize = 256;
    std::vector<float> attackTable;
    std::vector<float> holdTable;
    std::vector<float> decayTable;
    std::vector<float> releaseTable;
    size_t lastCurveHash = 0;     // skip rebake when curves haven't changed
};

} // namespace SoundShop

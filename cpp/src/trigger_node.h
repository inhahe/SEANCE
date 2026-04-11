#pragma once
#include "node_graph.h"
#include "transport.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <string>
#include <random>

namespace SoundShop {

// ============================================================================
// Trigger node — "notes trigger other notes / signals"
//
// Sits on a MIDI path. For each incoming MIDI note-on, every matching rule
// fires an action: either a generated MIDI note (on the MIDI output pin)
// or a parametric signal shape (on the Signal output pin). Generated events
// can be delayed, so a queue tracks events whose fire time is in the future.
//
// The rule list is stored in node.script as a text blob so it round-trips
// through the project save/load path without needing new fields on Node.
// ============================================================================

enum class TriggerTarget {
    Midi,    // rule emits a MIDI note on the MIDI out pin
    Signal   // rule emits a signal shape on the Signal out pin
};

enum class TriggerShape {
    Step,          // instant jump to target value, hold, then return to rest
    Envelope,      // classic ADSR
    Ramp,          // linear slew from current value to target over duration
    FromVelocity   // value = velocityScale * (note velocity / 127) + velocityOffset
};

// A single rule: "when a matching note comes in, fire this action."
struct TriggerRule {
    // Match: which incoming notes activate this rule.
    int   minPitch   = 0;     // MIDI pitch range
    int   maxPitch   = 127;
    int   minVel     = 1;
    int   maxVel     = 127;
    float probability = 1.0f; // 0..1, stochastic gate

    TriggerTarget target = TriggerTarget::Midi;

    // --- MIDI action fields ---
    int   pitchOffset    = 12;     // relative to incoming note (semitones)
    int   velocityDelta  = 0;      // added to incoming velocity
    float delayBeats     = 0.0f;   // beats to wait before firing
    float lengthBeats    = 0.25f;  // duration of the triggered note
    int   outChannel     = 1;      // 1..16

    // --- Signal action fields ---
    TriggerShape shape     = TriggerShape::Envelope;
    float attackMs         = 10.0f;
    float decayMs          = 100.0f;
    float sustainLevel     = 0.3f; // 0..1 (fraction of peak)
    float releaseMs        = 200.0f;
    float peakValue        = 1.0f;  // target value at peak
    float restValue        = 0.0f;  // what the signal returns to
    float rampDurationMs   = 500.0f; // for Ramp shape
    float holdMs           = 100.0f; // for Step shape (how long to hold target)
    float velocityScale    = 1.0f;   // for FromVelocity
    float velocityOffset   = 0.0f;

    // Label shown in the editor row. Auto-filled by presets, editable.
    std::string label;
};

// Holds the rule list + output pin configuration for a single Trigger node.
struct TriggerDoc {
    std::vector<TriggerRule> rules;

    // Encode/decode to/from the node.script blob.
    std::string encode() const;
    bool decode(const std::string& s);

    // Preset rule sets — a one-click starting point.
    static TriggerDoc presetOctaveDouble();
    static TriggerDoc presetChordMajor();
    static TriggerDoc presetFlam();
    static TriggerDoc presetPluckEnvelope();
    static TriggerDoc presetVelocityFollower();
    static TriggerDoc defaultDoc();
};

// ============================================================================
// TriggerProcessor — the audio-thread side.
// ============================================================================

class TriggerProcessor : public juce::AudioProcessor {
public:
    TriggerProcessor(Node& node, Transport& transport);

    const juce::String getName() const override { return "Trigger"; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;
    double getTailLengthSeconds() const override { return 2.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    Node& node;
    Transport& transport;
    double sampleRate = 44100.0;
    int64_t absSampleTime = 0; // monotonically increasing sample counter

    // Cached rule list — re-read from node.script at the start of each block
    // only if the script text actually changed (cheap string compare).
    TriggerDoc doc;
    std::string cachedScript;
    void rereadDocIfChanged();

    // Scheduled MIDI events (both note-on and note-off).
    struct ScheduledMidi {
        int64_t sampleTime;
        juce::MidiMessage msg;
    };
    std::vector<ScheduledMidi> pendingMidi;

    // Currently-active signal shapes driven by triggers. Each one writes to
    // the Signal output buffer until its duration expires; the most recent
    // shape wins on overlap (replace semantics).
    struct ActiveShape {
        int64_t startSample;
        TriggerShape shape;
        // Snapshot of rule params at trigger time (so later edits to the
        // rule don't retroactively change a running shape).
        float attackSamples;
        float decaySamples;
        float releaseSamples;
        float sustainLevel;
        float peakValue;
        float restValue;
        float rampDurationSamples;
        float holdSamples;     // for Step
        float velocityNorm;    // for FromVelocity (0..1)
        float velocityScale;
        float velocityOffset;
    };
    std::vector<ActiveShape> activeShapes;

    std::mt19937 rng{1234};

    // Schedule a rule's action against a specific trigger note.
    void scheduleRule(const TriggerRule& r,
                      const juce::MidiMessage& trig,
                      int64_t trigSample);

    // Evaluate a signal shape at a given absolute sample. Returns the output
    // value; sets `expired` true when the shape should be removed.
    float evalShape(const ActiveShape& s, int64_t nowSample, bool& expired) const;
};

// ============================================================================
// TriggerEditorComponent — a small dialog for editing rules.
//
// MVP scope: a rule list with preset buttons and minimal per-rule editing
// (target, pitch offset, delay, length, probability for MIDI; shape +
// peak/attack/release for Signal). Full per-field editor can come in pass 2.
// ============================================================================

class TriggerEditorComponent : public juce::Component {
public:
    TriggerEditorComponent(NodeGraph& graph, int nodeId, std::function<void()> onApply);

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    NodeGraph& graph;
    int nodeId;
    std::function<void()> onApply;
    TriggerDoc doc;

    juce::TextButton addMidiBtn { "+ MIDI Rule" };
    juce::TextButton addSignalBtn { "+ Signal Rule" };
    juce::TextButton applyBtn { "Apply" };
    juce::TextButton closeBtn { "Close" };
    juce::TextButton helpBtn  { "?" };

    // Preset buttons
    juce::TextButton presetOctaveBtn  { "Octave Double" };
    juce::TextButton presetChordBtn   { "Chord I-III-V" };
    juce::TextButton presetFlamBtn    { "Flam" };
    juce::TextButton presetPluckBtn   { "Signal Pluck" };
    juce::TextButton presetVelFollowBtn { "Velocity Follow" };

    juce::Viewport rulesViewport;
    juce::Component rulesContainer;

    class RuleRow;
    std::vector<std::unique_ptr<RuleRow>> rows;

    void rebuildRows();
    void commitToNode();
    void onDocChanged();
    void loadPreset(const TriggerDoc& preset);
};

} // namespace SoundShop

#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <mutex>
#include <cstdint>
#include "music_theory.h"
#include "piano_roll.h"
#include "effect_regions.h"
#include "tuning.h"
#include "undo.h"
#include "plugin_host.h"
#include "adsr_envelope.h"
#include "warp.h"            // WarpOp (granular element warp on audition frames)
#include "content_store.h"   // content-addressed side-store for baked blobs
#include "asset_library.h"   // project-level asset stores (waveforms, instruments, ...)

namespace SoundShop {

// Simple 2D point (replaces Vec2)
struct Vec2 {
    float x = 0, y = 0;
};

enum class PinKind { Audio, Midi, Param, Signal }; // Signal = audio-rate control signal (mono)

// Transient (not serialized) per-node plugin instantiation state, used to drive
// the async project-load path. When a project is opened, plugin nodes appear
// immediately as Pending; a serial background loader walks them one at a time
// (Loading), then marks each Ready or Failed. None = node has no plugin to load.
enum class PluginLoadState { None, Pending, Loading, Ready, Failed };

// Two pin kinds are compatible at the cable level if they're either the same
// kind, or both control kinds (Param + Signal). Param is conceptually
// block-rate and Signal is audio-rate, but at the routing layer we treat them
// as a single "control" family - the conversion is implicit and free, since
// the audio-graph routing already carries them on the same channel slot. The
// receiver decides whether to read once per block (Param semantics) or every
// sample (Signal semantics). See task #82.
// Two pin kinds are compatible at the cable level if:
//   - They're the same kind, OR
//   - Both are control kinds (Param + Signal), OR
//   - An Audio output connects to a Signal input (the graph processor
//     mono-downmixes channel 0 of the source to the signal slot; this
//     enables sidechain inputs where an audio signal triggers a
//     control-rate detector).
inline bool arePinKindsCompatible(PinKind a, PinKind b) {
    if (a == b) return true;
    bool aCtrl = (a == PinKind::Param || a == PinKind::Signal);
    bool bCtrl = (b == PinKind::Param || b == PinKind::Signal);
    if (aCtrl && bCtrl) return true;
    // Audio output -> Signal input (mono downmix for sidechain etc.)
    if (a == PinKind::Audio && b == PinKind::Signal) return true;
    return false;
}
// Panning law applied by PanProcessor.  EqualPower is the industry
// standard for DAWs; Linear matches tracker behavior (MOD/IT/S3M/XM).
enum class PanLaw { EqualPower, Linear };

enum class NodeType {
    AudioTimeline, MidiTimeline, Instrument, Effect, Mixer, Output, Script, Group, TerrainSynth, SignalShape,
    // MidiInput represents a single live MIDI input source (computer keyboard,
    // hardware MIDI device, network MIDI client, virtual port, etc). It has
    // no inputs and one MIDI output. The cable wiring from the Input node to
    // a Timeline or synth IS the live-input routing - no flags, no hidden
    // state. See project_midi_input_architecture.md.
    MidiInput,
    // MidiScript: an algorithmic MIDI generator. Runs a small program (the
    // SEANCE mini-language with statements, persistent state and MIDI emit
    // functions) once per sample and outputs MIDI live. One merged MIDI input,
    // N Signal inputs, and 1..16 independent MIDI outputs. See midi_script_node.h.
    // NOTE: new enum values MUST be appended at the END - project files store
    // node.type as a raw int, so reordering would corrupt existing saves.
    MidiScript,
    // MidiBreakout: taps a live MIDI stream and exposes its expression
    // controllers as block-rate control (Signal) outputs - Velocity, Pressure
    // (channel aftertouch), Mod Wheel (CC1) and Pitch Bend - so they can be
    // wired anywhere a control cable is accepted (filter cutoff, wavetable
    // position, a different synth's Pressure input, etc.). One MIDI input, four
    // Signal outputs. See midi_breakout_node.h.
    MidiBreakout,
    // --- Per-voice polyphony (see poly-voice-architecture.md). NOTE: still
    // append-only; node.type is stored as a raw int. ---
    // VoiceContainer: a polyphonic instrument built from graph primitives. On
    // the main canvas it is one node (MIDI in, audio out). Internally it owns an
    // inner subgraph (nodes whose voiceContainerId == this node's id) that is
    // instantiated N times - once per sounding MIDI note - and summed. Driven by
    // PolyVoiceProcessor.
    VoiceContainer,
    // Voice-context source modules. These live INSIDE a VoiceContainer's inner
    // graph and expose the per-note state of the voice they're being rendered
    // for as a Signal output. The container writes each voice's value into the
    // module's output before rendering that voice. No inputs.
    // RESERVED: standalone context-signal source nodes. Superseded for M1 by the
    // consolidated VoiceIn puck (which carries Pitch/Gate/Velocity as output pins
    // alongside raw MIDI), so these are not built or offered in the UI yet. Kept
    // in the enum (append-only - node.type is a raw int) for a possible future
    // "separate context modules" mode. createNodeProcessor returns Passthrough.
    VoicePitch,     // note pitch: Signal out (note number and/or Hz)
    VoiceGate,      // 1.0 while the note is held, 0.0 after note-off
    VoiceVelocity,  // note-on velocity, 0..1
    // Voice boundary pucks. These live INSIDE a VoiceContainer's inner graph.
    // VoiceIn is the single per-note context source: the container drives it with
    // the current voice's MIDI (raw per-voice note stream, for MIDI-driven synths)
    // AND its Pitch (Hz), Gate (0/1) and Velocity (0..1) as Signal outputs. VoiceOut
    // is the inner audio sink - the per-voice patch's audio leaves through it and is
    // summed across voices (mapped to the inner graph's output node, like Output).
    VoiceIn,
    VoiceOut
};

struct Pin {
    // Default to -1 so an uninitialized Pin (e.g. one default-constructed
    // by project_file.cpp's `[PinIn]` / `[PinOut]` section opener before
    // the `id=` line is parsed) reads as a recognizable sentinel rather
    // than as garbage from whatever memory the int happened to occupy.
    // Real pin IDs come from NodeGraph::newId() which starts at 1.
    int id = -1;
    std::string name;
    PinKind kind;
    bool isInput;
    int channels = 2; // 1=mono, 2=stereo, 6=5.1, etc.

    // Optional hover-tooltip text shown when the mouse rests over this pin in
    // the node graph (NodeGraphComponent::getTooltip). Empty = no tooltip.
    // Not serialized: pins that need a tooltip (e.g. the synth "Pressure"
    // input, the MIDI Breakout outputs) re-set it every graph build / node
    // creation, so the text always reflects the current code, never a stale
    // copy baked into an old project file.
    std::string tooltip;
};

// Automation point on a parameter timeline
struct AutomationPoint {
    float beat;    // absolute beat position
    float value;   // parameter value (in param's range, not normalized)
};

// Automation lane for one parameter
struct AutomationLane {
    std::vector<AutomationPoint> points;

    // Evaluate automation value at a given beat using Catmull-Rom
    // interpolation through the control points. Returns -1 (sentinel)
    // if no automation points exist. Passes through every point exactly;
    // curves smoothly between them.
    float evaluate(float beat) const {
        if (points.empty()) return -1.0f; // sentinel: no automation
        int n = (int)points.size();
        if (n == 1) return points[0].value;
        if (beat <= points.front().beat) return points.front().value;
        if (beat >= points.back().beat) return points.back().value;

        // Find the segment [i1, i2] that contains beat
        int i1 = 0;
        for (int i = 1; i < n; ++i) {
            if (beat <= points[i].beat) { i1 = i - 1; break; }
        }
        int i0 = (i1 > 0) ? i1 - 1 : i1;
        int i2 = i1 + 1;
        int i3 = (i2 < n - 1) ? i2 + 1 : i2;

        float segLen = points[i2].beat - points[i1].beat;
        float t = (segLen > 1e-6f) ? (beat - points[i1].beat) / segLen : 0.0f;

        float y0 = points[i0].value;
        float y1 = points[i1].value;
        float y2 = points[i2].value;
        float y3 = points[i3].value;

        // Catmull-Rom cubic interpolation
        float t2 = t * t, t3 = t2 * t;
        return 0.5f * ((2.0f * y1)
                     + (-y0 + y2) * t
                     + (2.0f*y0 - 5.0f*y1 + 4.0f*y2 - y3) * t2
                     + (-y0 + 3.0f*y1 - 3.0f*y2 + y3) * t3);
    }
};

// Automation record mode - governs whether live control moves are captured
// into automation lanes during playback, and how. Forms a cascade:
//   global (session) -> node -> param
// Lower scopes default to Inherit and defer upward; the global scope is always
// explicit (never Inherit). See resolveArmMode() below the Node struct.
//   Off    - do not record here.
//   Touch  - write to the lane only while the control is actively held; on
//            release the param snaps back to reading its lane.
//   Latch  - start writing when the control is first grabbed and keep writing
//            the held value through the rest of the transport pass.
//   Write  - overwrite the whole pass at the current value whether or not the
//            control is touched (the destructive "flatten to static" tool).
//            NODE/PARAM SCOPE ONLY - never selectable globally, so no single
//            switch can wipe every lane in the project.
enum class AutoArmMode {
    Inherit = 0, // defer to the parent scope; invalid at global scope
    Off,
    Touch,
    Latch,
    Write
};

// Short human label for an AutoArmMode (menus, tooltips, effective-state UI).
inline const char* autoArmModeName(AutoArmMode m) {
    switch (m) {
        case AutoArmMode::Inherit: return "Inherit";
        case AutoArmMode::Off:     return "Off";
        case AutoArmMode::Touch:   return "Touch";
        case AutoArmMode::Latch:   return "Latch";
        case AutoArmMode::Write:   return "Write";
    }
    return "Off";
}

struct Param {
    std::string name;
    float value;
    float minVal;
    float maxVal;
    std::string format = "%.2f";
    AutomationLane automation; // recorded automation for this param
    bool autoWriteArmed = false; // when armed, "Write Automation to Selection" includes this param

    // Automation record cascade - per-param override (record axis). Inherit =
    // follow the owning node's mode (which itself may inherit the global mode).
    // Serialized so a project remembers "this knob is set to Latch"; the global
    // mode that actually gates recording is session-only, so a reload never
    // silently starts recording. See resolveArmMode().
    AutoArmMode armMode = AutoArmMode::Inherit;

    // Read axis - per-param lane bypass. When true this param ignores its own
    // automation lane during playback (the lane is preserved, just muted), so
    // the user can audition the param held still without losing the recording.
    // Serialized. See automationReadEnabled().
    bool bypassAutomation = false;

    // Signal modulation support (#88). When a Signal cable drives this
    // param, `baseValue` holds the user's intended setting and `value`
    // is rewritten each audio block to `baseValue + signal * depth`.
    // When no modulation is active, baseValue is unused (value is the
    // source of truth). The `modulated` flag indicates whether
    // baseValue/value are split or identical this block.
    float baseValue = 0.0f;
    bool  modulated = false;

    // Transient automation-recording state (NOT serialized). Driven by the
    // playback timer's capture loop, never by save/load.
    //   recWriting  - true while this param should have its live value written
    //                 into `automation` each tick. Set on a Touch/Latch gesture
    //                 begin, or at play-start for a Write-resolved param;
    //                 cleared on release (Touch) or transport stop (all).
    //   recLastBeat - sweep cursor: the last beat a point was written in the
    //                 CURRENT contiguous write segment (-1 = segment not started
    //                 yet). Reset to -1 on a Touch release so the next punch-in
    //                 starts a fresh sweep and the untouched gap keeps its
    //                 existing automation instead of being wiped.
    //   recDidWrite - true if ANY point was written this pass (survives Touch
    //                 punch-out unlike recLastBeat). Drives end-of-pass
    //                 simplify + one-snapshot; cleared when the pass ends.
    bool  recWriting = false;
    float recLastBeat = -1.0f;
    bool  recDidWrite = false;

    // Warp slot key (unified warp/morph model). >= 0 marks this as the
    // modulation param for warp-chain op `warpSlot` (0-based); -1 = not a warp
    // param. This is the STABLE key the synth + reconcile logic address the op
    // by, DECOUPLED from `name` - which is now a human label that follows the
    // op's method (e.g. "Soft Clip Drive 1"), so renaming on a method change
    // never disturbs which op a wired modulation pin drives. See
    // syncWarpParamsForNode / warpParamIndexForOp.
    int warpSlot = -1;
    // Warp scope: which warp chain `warpSlot` indexes into.
    //   -1 = the frame-scope (summation-morph) chain  (IWavetableFrame::warpChain)
    //   >=0 = the per-layer chain of layer `warpLayer`  (WaveLayer::warpChain)
    // Per-layer warp params exist ON DEMAND - one is created only when the user
    // opts a per-layer op into modulation (the "Mod" checkbox), and removed when
    // they opt out or the op/layer goes away. Frame-scope params (warpLayer==-1)
    // exist for every op so the amount is always modulatable. Only meaningful
    // when warpSlot >= 0.
    int warpLayer = -1;

    // Which wavetable FRAME (library entry id) this warp/layer-field param
    // belongs to. The summation-morph warp chain, the per-layer warp chains,
    // and the per-layer Phase/Amp fields all live PER FRAME now (a wavetable
    // node can hold several frames, each shaping its own cycle before the
    // cross-frame morph blend), so a warp param must record which frame it
    // drives or two frames carrying the same op (e.g. both a "Drive" at slot 0)
    // would collide on the (warpLayer, warpSlot) key. >= 0 = the owning frame's
    // library id. -1 = legacy/whole-node: pre-per-frame projects stored a single
    // shared chain with no frame id; on load those params keep -1 until the
    // migration in syncWarpParamsForNode reassigns them. Only meaningful when
    // warpSlot >= 0 or layerField >= 0. Serialized as the optional "warpFrameId"
    // field (omitted when -1, so old projects round-trip unchanged).
    int warpFrameId = -1;

    // Per-layer field modulation key (layered wavetable). Marks this as the
    // on-demand modulation param for a layer's Phase or Amplitude slider:
    //   -1 = not a layer-field param (default)
    //    0 = layer Phase
    //    1 = layer Amplitude
    // When layerField >= 0, `warpLayer` holds the 0-based layer index and
    // `warpSlot` stays -1 - so a layer-field param never collides with warp
    // param lookups (which require warpSlot >= 0). Created on demand when the
    // user ticks the per-layer Phase/Amp "Mod" checkbox, removed when unticked
    // or the layer goes away. See setLayerFieldModulated / renderWithLiveOverrides.
    int layerField = -1;
};

// Rational fraction for exact beat subdivisions (e.g., triplets)
struct BeatFraction {
    int num = 0;
    int den = 0; // 0 = not set (use float instead)
    float toFloat() const { return den > 0 ? (float)num / den : 0; }
};

// MPE expression breakpoint (time relative to note start, in beats)
struct ExpressionPoint {
    float time;   // beats from note start
    float value;  // normalized 0.0-1.0
};

// Per-note MPE expression curves
struct NoteExpression {
    std::vector<ExpressionPoint> pitchBend;  // 0.5 = center, 0/1 = full bend range
    std::vector<ExpressionPoint> slide;      // CC74, 0.0-1.0
    std::vector<ExpressionPoint> pressure;   // channel aftertouch, 0.0-1.0

    bool hasData() const {
        return !pitchBend.empty() || !slide.empty() || !pressure.empty();
    }

    static float evaluate(const std::vector<ExpressionPoint>& curve, float time, float defaultVal) {
        if (curve.empty()) return defaultVal;
        if (time <= curve.front().time) return curve.front().value;
        if (time >= curve.back().time) return curve.back().value;
        for (size_t i = 1; i < curve.size(); ++i) {
            if (time <= curve[i].time) {
                float t = (time - curve[i - 1].time) / (curve[i].time - curve[i - 1].time);
                return curve[i - 1].value + t * (curve[i].value - curve[i - 1].value);
            }
        }
        return curve.back().value;
    }
};

struct MidiNote {
    float offset;       // beat offset within clip
    int pitch;          // MIDI pitch 0-127
    float duration;     // beats
    int velocity = 100; // MIDI velocity 0-127
    int degree = 0;
    int octave = 4;
    int chromaticOffset = 0;
    float detune = 0.0f; // cents

    // Optional exact fractions for precise subdivision timing
    BeatFraction exactOffset;   // if den > 0, overrides offset
    BeatFraction exactDuration; // if den > 0, overrides duration

    // MPE per-note expression
    NoteExpression expression;

    float getOffset() const { return exactOffset.den > 0 ? exactOffset.toFloat() : offset; }
    float getDuration() const { return exactDuration.den > 0 ? exactDuration.toFloat() : duration; }
};

struct MidiCCEvent {
    float offset;      // beat offset within clip
    int controller;    // CC number (0-127)
    int value;         // CC value (0-127)
    int channel = 1;   // MIDI channel (1-16)
};

struct Clip {
    std::string name;
    float startBeat;
    float lengthBeats;
    uint32_t color;
    int channels = 2;
    std::vector<MidiNote> notes;
    std::vector<MidiCCEvent> ccEvents;
    int waveformView = 0; // 0=L/R, 1=Mid/Side

    // Audio clip properties
    std::string audioFilePath;        // path to audio file (empty = no audio)
    float slipOffset = 0.0f;          // offset into the audio file in seconds
    float fadeInBeats = 0.0f;
    float fadeOutBeats = 0.0f;
    float gainDb = 0.0f;

    // Per-clip key/mode/scale
    int keyRoot = 0;           // 0=C
    std::string keyType = "Major";  // from KEYS table
    bool hasCustomKey = false; // false = inherit from piano roll state

    // Take lane index (-1 = not part of a take system)
    int takeLaneIdx = -1;
};

// A comp segment selects which take lane is active during a time range
struct CompSegment {
    float startBeat;
    float endBeat;
    int takeLaneIdx;           // which take lane is active here
    float crossfadeBeats = 0;  // crossfade duration at boundaries
};

// A take lane holds clips from one recording pass
struct TakeLane {
    std::string name;
    std::vector<Clip> clips;
    float timeOffsetSamples = 0; // alignment offset for this take
    bool muted = false;
};

struct Node {
    // Default to -1 so an uninitialized Node (e.g. one default-constructed
    // by project_file.cpp's `[Node]` section opener before the `id=` line
    // is parsed, or a torn read of this field from a concurrent push_back
    // reallocation) reads as a recognizable sentinel rather than as
    // garbage memory. Real node IDs come from NodeGraph::newId() which
    // starts at 1. Downstream code (graph rebuild, findNode, save) can
    // and should skip / refuse nodes with id < 0.
    int id = -1;
    std::string name;
    NodeType type;
    std::vector<Pin> pinsIn;
    std::vector<Pin> pinsOut;
    Vec2 pos{0, 0};
    bool posSet = false;
    bool muted = false;
    bool soloed = false;

    // Peak level metering (#99). Updated by the audio thread each block
    // (via PanProcessor which runs after every audio-producing node).
    // Read by the UI thread at 30 Hz for drawing meter bars. Plain
    // floats (not atomic) because Node must be copyable for std::vector.
    // The audio thread writes, the UI thread reads - a torn read is at
    // worst a meter glitch, never a crash. Decay is applied UI-side.
    float meterPeakL = 0.0f;
    float meterPeakR = 0.0f;

    // Runtime-only (not serialized): does this node have an audio path to an
    // Output node? Recomputed by GraphProcessor::rebuildGraph after every
    // topology change. Used by synth audition (editor "Play" on an unplaced
    // library frame): when a synth node can't reach an Output, its audition
    // voices are diverted to the AudioEngine's audition-monitor bus so the
    // preview is still audible. When the node IS routed to output, the
    // audition stays in the normal graph path so it flows through the user's
    // downstream effects/pan exactly like a played note. Defaults to true so
    // the conservative "stay in graph" behavior holds before the first build.
    bool reachesOutput = true;
    std::vector<Param> params;

    // On-demand signal modulation pins (#88). Each entry binds a
    // dynamically added Signal input pin to a specific param index.
    // When a Signal cable is connected to the pin, the processor reads
    // the signal from audio channel (2 + pin's control-slot index) and
    // modulates the param each block. The pin lives in pinsIn alongside
    // the node's static pins - it's serialized as part of the normal
    // pin list in project_file.cpp. The modPin just records the binding.
    struct ModPin {
        // How an incoming control cable affects the bound param:
        //  - Modulate ("Mod"): bipolar-additive around the knob's resting value
        //    (baseValue). 0.5 = no change. The user can still edit the knob; the
        //    cable swings the param around that center. Pin labelled "Mod: ".
        //  - Absolute ("Set"): the cable's value *is* the param value, mapped
        //    edge-to-edge across [min,max]. The knob is locked while connected.
        //    Pin labelled "Set: ".
        enum class Mode { Modulate, Absolute };
        int paramIndex = -1;  // index into this node's params[]
        int pinId = -1;       // matching pin id in pinsIn
        float depth = 1.0f;   // modulation depth: 0=none, 1=full range (Modulate only)
        Mode mode = Mode::Modulate;  // default Modulate: old projects (no saved
                                     // mode field) keep their original behaviour

        // Runtime-only connectivity cache (NOT serialized). Recomputed by the
        // graph processor at build time: true iff some link's endPin == pinId,
        // i.e. a cable is actually feeding this modulation input. Some pins
        // (e.g. wavetable "Mod: Position" pins) are created eagerly so the user
        // can cable to them, but their modPin binding exists even when nothing
        // is connected. applySignalModulations() must skip those idle bindings -
        // otherwise it reads the pin's silent control channel (0.0) and forces
        // the bound param to its minimum, overriding the user's manual setting.
        // Mirrors Node::reachesOutput (same recompute-on-build discipline).
        bool connected = false;
    };
    std::vector<ModPin> modPins;

    std::vector<Clip> clips;

    // Take lanes for comping (audio timelines)
    std::vector<TakeLane> takeLanes;
    std::vector<CompSegment> compSegments;
    int activeTakeLane = -1; // -1 = use clips directly, >=0 = recording to this lane

    std::string script;

    // Oscilloscope display settings (node.script == "__oscilloscope__").
    //   scopeTriggered: true  = triggered acquisition - the display is aligned
    //                           to a level crossing of the chosen slope so a
    //                           periodic waveform appears stationary;
    //                   false = roll mode - free-running strip chart, the most
    //                           recent samples are drawn each frame (newest at
    //                           the right edge), no edge alignment.
    //   scopeTrigLevel:  trigger threshold in -1..1 (0 = zero crossing).
    //   scopeTrigRising: edge slope to trigger on (true = rising, false = falling).
    // Serialized in project_file.cpp only when non-default; defaults give a
    // brand-new scope (and any pre-this-feature project) a stable triggered view.
    bool  scopeTriggered = true;
    float scopeTrigLevel = 0.0f;
    bool  scopeTrigRising = true;

    // Performance mode - play preset melody by pressing any keys
    bool performanceMode = false;
    int performanceReleaseMode = 1;   // 0=OnKeyUp, 1=OnNextEvent (legato)
    bool performanceVelocity = true;  // use incoming velocity

    // The unified AHDSR amplitude envelope used by every tonal /
    // note-triggered synth (built-in, terrain, wavetable, layered,
    // spectral, FM, additive, PD, particle). Stores A/H/D/S/R time
    // values, sustain level, velocity sensitivity, and the per-segment
    // SpectralCurve shapes for Attack / Decay / Release.
    //
    // Edited via the shared AHDSREnvelopeComponent (opened either inline
    // from a synth dialog or via a right-click "Envelope..." menu on
    // the node). Save/load and undo serialize this through encode/decode.
    AHDSREnvelope ahdsrEnvelope;

    // Optional LIVE reference to a project asset-library AHDSR curve. -1 means
    // "independent" - ahdsrEnvelope is this node's own local copy (the original
    // behavior). When >= 0, this node references the AhdsrCurve asset with that
    // id: ahdsrEnvelope is kept as a mirror of the stored curve, and editing the
    // curve (from any referencing node's envelope dialog) propagates to every
    // node that shares the id. See NodeGraph::resolveAhdsrReferences(). The
    // audio thread still reads ahdsrEnvelope directly, so referencing costs it
    // nothing - resolution happens at edit/load time, never per block.
    int ahdsrAssetId = -1;

    // Additional per-component AHDSR envelopes for instruments that need more
    // than one envelope per voice. Empty for almost every node type. The FM
    // synth populates exactly 4 (one full AHDSR per operator), replacing the
    // old per-operator "Op{i} A/D/S/R" linear-ramp params with the shared
    // AHDSR model (hold stage, per-segment curves, tension, velocity
    // sensitivity). Edited via the multi-tab operator-envelope dialog
    // (launchOpEnvelopesDialog). Serialized as opEnvelope0..N in project
    // files; on load, projects predating this field rebuild the 4 entries
    // from the legacy "Op{i} A/D/S/R" params. Each entry is "independent"
    // (no asset-library reference - that single-id model only fits the main
    // ahdsrEnvelope). The audio thread reads these directly per block.
    std::vector<AHDSREnvelope> opEnvelopes;

    // Per-voice pressure (aftertouch) input. When something is wired to the
    // "Pressure" Param input pin on a synth node, the wired control's
    // value (0..1, read as the block mean) drives the per-voice
    // pressure swell. It's a Param (block-rate) pin because the consumer
    // averages it over the whole block to stay smooth. When the
    // pin is unwired, the synth uses channel-pressure (aftertouch) events
    // from the incoming MIDI stream instead. Either way the value is exposed
    // to every voice as a modulation source that defaults to scaling output
    // amplitude by 1 + 0.5*pressure. (The pin was historically named
    // "Aftertouch"; the graph builder migrates that name to "Pressure".)
    float aftertouchSensitivity = 0.5f;  // 0 = ignore, 1 = full volume swell

    // Panning and spatial positioning
    PanLaw panLaw = PanLaw::EqualPower; // panning law for PanProcessor
    float pan = 0.0f;            // stereo pan: -1.0 (full left) to 1.0 (full right), 0 = center
    float spatialX = 0.0f;       // surround: front-back (-1 = back, 1 = front)
    float spatialY = 0.0f;       // surround: left-right (-1 = left, 1 = right)
    float spatialZ = 0.0f;       // height (for Atmos-style)

    // MPE (MIDI Polyphonic Expression)
    bool mpeEnabled = false;
    int mpePitchBendRange = 48;       // semitones, must match synth setting
    std::shared_ptr<PluginHost::LoadedPlugin> plugin; // hosted VST3/AU plugin
    int pluginIndex = -1; // index into PluginHost::availablePlugins, -1 = none
    std::string pendingPluginState; // base64-encoded state to restore after plugin loads

    // Automation lanes for hosted-plugin (VST3/AU) parameters, keyed by plugin
    // parameter index. Native params carry their lane inline (Param::automation);
    // a hosted plugin exposes its parameters through JUCE's AudioProcessorParameter
    // interface and has NO Param row, so their recorded lanes live here instead.
    // Lane values are NORMALIZED (0..1) - exactly what AudioProcessorParameter::
    // setValue() consumes on playback (native lanes store real param units). These
    // are recorded by dragging a knob inside the plugin's OWN editor window (via
    // GraphProcessor's AudioProcessorListener) and are read/muted uniformly with
    // native lanes through ignoreAutomation. Serialized (see project_file.cpp).
    std::map<int, AutomationLane> pluginParamAutomation;

    // Transient per-plugin-param recording state (NOT serialized), mirroring the
    // Param::recWriting/recLastBeat/recDidWrite trio. Created on demand as the
    // user drives plugin params during a recording pass. gestureActive tracks
    // whether a real begin/endChangeGesture pair is bracketing the moves (many
    // plugins omit gestures and only fire parameterChanged, so Touch mode falls
    // back to a short idle timeout keyed off lastChangeMs).
    struct PluginParamRec {
        bool  writing       = false;
        float recLastBeat   = -1.0f;
        bool  recDidWrite   = false;
        bool  gestureActive = false;
        double lastChangeMs = 0.0;
    };
    std::map<int, PluginParamRec> pluginParamRec;

    // Per-plugin dirty tracking for the slow autosave path (#86). When a
    // plugin's parameters change via host automation, MIDI Learn CC, or
    // any other host-driven path, this flag is set so the next autosave
    // re-queries getStateInformation. When clear, the saver reuses the
    // cached base64 string instead - avoiding the expensive query for
    // plugins whose state hasn't changed since the last save. Defaults
    // to true so a freshly loaded plugin gets queried at least once.
    //
    // Limitation: changes made by the user inside the plugin's own UI
    // can't be detected here (no general-purpose API to listen for them
    // across plugin formats). Mitigation: a periodic "force-dirty all"
    // tick in the autosave path bounds staleness to a known interval.
    bool pluginStateDirty = true;
    std::string cachedPluginStateBase64;

    // Transient async-load state (NOT serialized). Drives the per-node loading
    // badge and the serial background loader after a project open. See
    // MainContentComponent::beginAsyncPluginLoad.
    PluginLoadState pluginLoadState = PluginLoadState::None;

    // Group - contains child node IDs
    std::vector<int> childNodeIds;  // IDs of nodes inside this group
    int parentGroupId = -1;         // -1 = top-level (not in any group)
    float groupBeatOffset = 0.0f;   // children's timelines start at this beat in the parent
    std::string anchorMarker;       // if non-empty, groupBeatOffset is overridden by this marker's beat
    float absoluteBeatOffset = 0.0f; // cached: cascading offset through all parents (updated by resolveAnchors)
    bool groupExpanded = true;      // show children in graph view

    // Voice container (per-voice polyphony) - see poly-voice-architecture.md.
    // Kept SEPARATE from the timeline-group fields above so the two membership
    // meanings never collide. A node with voiceContainerId >= 0 lives inside the
    // inner per-note patch of that VoiceContainer node.
    int voiceContainerId = -1;   // -1 = not inside any Voice container (top level)
    int voicePolyphony = 8;      // VoiceContainer: number of simultaneous voices
    int voiceStealMode = 0;      // VoiceContainer: 0=oldest, 1=quietest, 2=round-robin
    float voiceGlideMs = 0.0f;   // VoiceContainer: portamento time when a voice is
                                 // stolen for a new note (0 = off / instant pitch)
    int voiceUnison = 1;         // VoiceContainer: stacked detuned voices per note
                                 // (1 = off). Each note grabs this many slots.
    float voiceUnisonDetune = 12.0f; // cents of detune spread across the stack
    float voiceUnisonSpread = 0.5f;  // 0..1 stereo spread across the stack

    // MOD-import song-setting restore: when a module import overrides the
    // global song settings (repeat mode, song length, loop region), the
    // PRE-import values are stashed on the import's root group node here.
    // Deleting that root group node (the grey node that cascades to the
    // whole tree) restores them, backing out the module's loop contribution
    // while preserving whatever the user had set before importing. Stored as
    // int for the repeat mode because SongRepeat is declared later in the
    // header (after struct Node), so Node cannot name the enum type.
    bool   modImportSavedSong     = false;
    int    modImportPrevRepeatMode = 0;     // (int)NodeGraph::SongRepeat
    int    modImportPrevRepeatCount = 1;
    double modImportPrevSongLength = 0.0;
    bool   modImportPrevLoopEnabled = false;
    double modImportPrevLoopStart  = 0.0;
    double modImportPrevLoopEnd    = 0.0;

    // Direct granular-frame payload carried by an audition note-on so the
    // synth can render a SPECIFIC granular frame that isn't placed into the
    // wavetable's grid/scatter (and therefore isn't in the synth's
    // wtGranularFrames table). The wavetable editor's Play button uses this
    // so a freshly-captured, library-only frame is audible immediately and
    // faithfully (exact on-screen bytes, no wait for the ~150ms graph
    // rebuild). Mirrors GranularFrame's fields without pulling
    // granular_frame.h into this header. Shared so the large source PCM isn't
    // deep-copied through the audio-thread queue.
    struct AuditionGranularFrame {
        std::shared_ptr<std::vector<float>> source; // mono PCM at sourceSampleRate
        double sourceSampleRate = 0.0;
        int    grainLength      = 4800;
        int    windowStart      = -1;   // freeze-window start; -1 = auto-centre
        int    windowLen        = -1;   // freeze-window width; -1 = auto (= grain)
        float  embeddedPitchHz  = 440.0f;
        int    freezeMode       = 0;    // 0 = CrossfadeLoop
        int    grainCount       = 4;    // cloud-mode overlapping grains [2,16]
        int    fftSize          = 0;    // SpectralFreeze FFT size; 0 = auto
        int    crossfadeSamples = 2400;
        float  gain             = 1.0f; // mirrors IWavetableFrame::gain
        // Bucket C element warp (amplitude-domain). Mirrors GranularFrame::
        // warpAmpOps() so the editor's Play audition carries the same
        // waveshaping the placed-frame synth path and renderRaw apply -
        // "what you audition = what you get". Empty = no warp.
        std::vector<WarpOp> warpAmpOps;
    };

    // Direct inharmonic-frame payload carried by an audition note-on so the
    // synth can render a SPECIFIC inharmonic stack that isn't placed into the
    // wavetable's grid/scatter (and therefore isn't in the synth's
    // wtInharmonicFrames table). The inharmonic frame editor's Play button uses
    // this so an unplaced stack is audible immediately and faithfully, the same
    // way AuditionGranularFrame does for granular library frames. Mirrors
    // InharmonicFrame's partial fields without pulling inharmonic_frame.h into
    // this header. The live voice plays one sine oscillator per partial at
    // noteHz * ratio, sums amp*sin, scales by normGain (so it's as loud as the
    // editor thumbnail), applies the amplitude-domain warp, then the gain.
    struct AuditionInharmonicFrame {
        struct Partial { float ratio = 1.0f; float amp = 1.0f; float phase = 0.0f; };
        std::vector<Partial> partials;
        float               gain     = 1.0f;  // mirrors IWavetableFrame::gain
        float               normGain = 1.0f;  // InharmonicFrame::normGainFor(partials)
        std::vector<WarpOp> warpAmpOps;        // amplitude-domain element warp
    };

    // Direct single-cycle payload carried by an audition note-on so the synth
    // can render a SPECIFIC wavetable cycle that isn't placed into the grid/
    // scatter. This is the generic, frame-type-agnostic audition path used by
    // the layered-waveform editor's Play button (and, eventually, every frame
    // editor): any IWavetableFrame::render(tableSize, out) produces one final
    // single cycle with the frame's gain and internal warps already baked in,
    // so the voice just reads it as a wavetable oscillator (linear-interpolated
    // at the played pitch), bypassing the cycle terrain / granular / inharmonic
    // layers entirely. That makes the edited frame audible immediately and
    // faithfully - "what the editor previews = what you hear" - even before
    // it's dropped into a cell. Empty cycle = nothing to audition.
    struct AuditionCycleFrame {
        std::vector<float> cycle;  // final single cycle, gain + warps baked in
    };

    // Audition MIDI events injected from the UI (thread-safe via simple flag)
    struct AuditionEvent {
        bool isNoteOn;
        int pitch;
        int velocity;
        // Optional wavetable Position override for the voice this note-on
        // creates. Empty = no override (voice follows the live Position
        // params). Used by the wavetable editor so a frame's Play button
        // auditions THAT frame regardless of where the Position knob sits.
        // One entry per Position dimension, each in [0,1]. Ignored on
        // note-off events.
        std::vector<float> position;
        // Optional direct granular frame to render for this note-on. When set
        // (non-null), the voice plays THIS frame exclusively, bypassing both
        // the cycle terrain and the placed-frame morph - this is how the
        // editor auditions an unplaced library frame. Null for ordinary
        // MIDI / timeline notes and for non-granular frame auditions.
        std::shared_ptr<AuditionGranularFrame> granularFrame;
        // Optional direct inharmonic frame to render for this note-on. Same
        // role as granularFrame but for an unplaced inharmonic stack; the voice
        // plays its oscillator bank exclusively. Null otherwise.
        std::shared_ptr<AuditionInharmonicFrame> inharmonicFrame;
        // Optional direct single cycle to render for this note-on. Same role as
        // granularFrame / inharmonicFrame but for an unplaced wavetable cycle
        // (layered / spectral / wavelet / sample frames); the voice reads this
        // cycle exclusively as a wavetable oscillator. Null otherwise.
        std::shared_ptr<AuditionCycleFrame> cycleFrame;
    };
    std::vector<AuditionEvent> pendingAudition; // written by UI, read by audio thread

    // Sustained editor audition (the granular wave editor's Play button holds a
    // note for as long as the user listens). Unlike the momentary, edge-
    // triggered pendingAudition events, this is LEVEL-triggered: while non-null
    // it means "a voice should be sounding with this data." A debounced wavetable
    // edit triggers a full graph rebuild (GraphProcessor::rebuildGraph clears and
    // recreates every processor), which destroys all held voices - so an audition
    // routed only through pendingAudition goes silent on the first edit ("preview
    // stops until I press start again"). The synth re-establishes a voice from
    // heldAudition whenever it (re)starts, so the audition survives the rebuild.
    // The editor refreshes the snapshot (sharing the source PCM, not re-copying
    // it) on each audible edit so the post-rebuild voice reflects the new freeze
    // window / grain. Cleared on Stop. Accessed under auditionMutex.
    std::shared_ptr<AuditionEvent> heldAudition;  // null = nothing held
    std::shared_ptr<std::mutex> auditionMutex = std::make_shared<std::mutex>();

    // MPE pass-through / MidiInput node event queue. Originally used only
    // for MPE timelines; now also used by MidiInput nodes as the queue the
    // audio engine writes live events into and the MidiInput processor
    // drains into its output MIDI buffer.
    std::vector<std::pair<int, juce::MidiMessage>> pendingMpePassthrough; // (sampleOffset, msg)
    std::shared_ptr<std::mutex> mpePassthroughMutex = std::make_shared<std::mutex>();

    // MidiInput node: identifier of the physical (or virtual) input source
    // this node represents. "keyboard" = the computer keyboard. For hardware
    // MIDI devices this will be set to the device's JUCE identifier string.
    // Empty on non-MidiInput nodes.
    std::string midiInputSourceId;

    // Node audio cache (freeze + automatic memoization)
    struct AudioCache {
        bool enabled = false;       // user opted into caching (manual freeze)
        bool autoCache = true;      // automatic caching when deterministic
        bool valid = false;
        bool deterministic = true;  // false if any input is live/unpredictable
        uint64_t inputHash = 0;     // hash of all inputs; if unchanged, cache is valid
        bool useDisk = false;       // true = load/save from disk instead of memory
        std::string diskPath;       // path to cached audio file on disk

        // In-memory cache
        std::vector<float> left, right;
        double sampleRate = 0;
        int64_t startSample = 0;
        int64_t numSamples = 0;

        void invalidate() { valid = false; }
        void clear() { left.clear(); right.clear(); valid = false; numSamples = 0; inputHash = 0; }

        bool hasCachedAudio() const {
            return valid && (numSamples > 0 || (!diskPath.empty() && useDisk));
        }
    };
    AudioCache cache;

    // Transient (NOT serialized): the node is armed as a target for the next
    // batch "Freeze armed nodes" pass. Arming is a pending user action, not
    // saved project state - a reload starts with nothing armed. Multiple nodes
    // can be armed, then frozen together in a single render pass (freezeNodes)
    // rather than one full-project render each. Cleared when the freeze runs.
    bool armedForFreeze = false;

    // Automation record cascade - per-node override (record axis). Inherit =
    // follow the global session mode. A node override refines recording for all
    // of this node's params when globally armed (e.g. global Touch but this
    // synth on Write). Serialized. See resolveArmMode().
    AutoArmMode armMode = AutoArmMode::Inherit;

    // Read axis - per-node automation ignore. When true, none of this node's
    // param lanes drive their params during playback (all lanes preserved, just
    // muted at the node level). Lets the user A/B a whole node with vs without
    // its recorded automation in one toggle. Serialized. See
    // automationReadEnabled().
    bool ignoreAutomation = false;

    // Effect regions: time-bounded activation of links/groups on this track.
    // Drawn as colored bars on the track's timeline. Each region gates either
    // a single link (linkId >= 0) or an entire effect group (groupId >= 0).
    std::vector<EffectRegion> effectRegions;

    // Multi-track recording: per-track input assignment
    int recordInputChannel = -1;  // which audio input channel to record from (-1 = none)
    bool recordArmed = false;     // armed for recording
    bool inputMonitor = false;    // pass input through to output in real-time
};

// Resolve the effective automation record mode for one param, given the global
// session mode. Global Off is a HARD GATE: nothing records regardless of node
// or param overrides, so loading a project (which restores overrides but leaves
// the global mode at its Off default) can never silently begin recording. When
// globally armed (Touch/Latch), the cascade refines per param then per node;
// Inherit at both defers to the global mode. Write only ever originates from a
// node/param override (it is never a valid global value).
inline AutoArmMode resolveArmMode(AutoArmMode global, const Node& node, const Param& param) {
    if (global == AutoArmMode::Off || global == AutoArmMode::Inherit)
        return AutoArmMode::Off;                 // master gate: no recording
    if (param.armMode != AutoArmMode::Inherit) return param.armMode;
    if (node.armMode  != AutoArmMode::Inherit) return node.armMode;
    return global;                                // Touch or Latch
}

// Node-scope variant, for hosted-plugin parameters which have no Param row (and
// therefore no per-param override) - only the node override + global apply. Same
// hard Off gate as the param-scope resolver.
inline AutoArmMode resolveArmModeNode(AutoArmMode global, const Node& node) {
    if (global == AutoArmMode::Off || global == AutoArmMode::Inherit)
        return AutoArmMode::Off;
    if (node.armMode != AutoArmMode::Inherit) return node.armMode;
    return global;
}

// Read axis: should this param's lane drive it during playback? False if the
// owning node ignores automation wholesale or this param's lane is individually
// bypassed. Both are non-destructive mutes (the lane points are preserved).
inline bool automationReadEnabled(const Node& node, const Param& param) {
    return !node.ignoreAutomation && !param.bypassAutomation;
}

// Record one automation point for a param at the current playhead beat, called
// once per timer tick while `p.recWriting` is set. As the playhead sweeps
// forward, existing points in the just-passed span (recLastBeat, beat] are
// removed and replaced (so re-recording over a region overwrites rather than
// layering). A backward jump (loop wrap) starts a fresh sweep segment. Points
// are kept sorted by beat via ordered insert. The dense per-tick points are
// thinned by simplifyAutomationLane() at end of pass.
// Core lane-level recorder, parameterised on the lane + its per-sweep state so
// it can serve BOTH native params (Param::automation) and hosted-plugin param
// lanes (Node::pluginParamAutomation), which have no Param row. See the Param
// overload below.
inline void recordAutomationPointLane(AutomationLane& lane, float& recLastBeat,
                                      bool& recDidWrite, float beat, float value) {
    auto& pts = lane.points;
    if (recLastBeat >= 0.0f && beat >= recLastBeat) {
        // Overwrite-sweep: drop existing points in (recLastBeat, beat].
        const float lo = recLastBeat, hi = beat;
        pts.erase(std::remove_if(pts.begin(), pts.end(),
            [lo, hi](const AutomationPoint& ap) {
                return ap.beat > lo + 1e-6f && ap.beat <= hi + 1e-6f;
            }), pts.end());
    }
    // Avoid an exact-duplicate point when the playhead hasn't advanced.
    if (!pts.empty() && std::abs(pts.back().beat - beat) < 1e-6f
        && recLastBeat >= 0.0f && beat >= recLastBeat) {
        pts.back().value = value;
    } else {
        // Ordered insert (points beyond `beat` may exist from prior recordings
        // or authored automation, so append-then-hope is not safe).
        auto it = std::lower_bound(pts.begin(), pts.end(), beat,
            [](const AutomationPoint& ap, float b) { return ap.beat < b; });
        pts.insert(it, { beat, value });
    }
    recLastBeat = beat;
    recDidWrite = true;
}

inline void recordAutomationPoint(Param& p, float beat, float value) {
    recordAutomationPointLane(p.automation, p.recLastBeat, p.recDidWrite, beat, value);
}

// Collinear-reduction pass run once at end of a recording pass: drops any point
// that lies (within `eps`) on the straight line between its neighbours, so a
// static Write pass collapses to two endpoints while a genuine sweep keeps its
// shape. `eps` is in the param's value units (caller scales by param range).
inline void simplifyAutomationLane(AutomationLane& lane, float eps) {
    auto& p = lane.points;
    if (p.size() < 3) return;
    std::vector<AutomationPoint> out;
    out.reserve(p.size());
    out.push_back(p.front());
    for (size_t i = 1; i + 1 < p.size(); ++i) {
        const AutomationPoint& a = out.back();
        const AutomationPoint& b = p[i];
        const AutomationPoint& c = p[i + 1];
        float span = c.beat - a.beat;
        float t = (std::abs(span) > 1e-9f) ? (b.beat - a.beat) / span : 0.0f;
        float interp = a.value + t * (c.value - a.value);
        if (std::abs(interp - b.value) > eps) out.push_back(b);
    }
    out.push_back(p.back());
    p = std::move(out);
}

// Thread-safe write to a live node's `script`.
//
// Several audio-thread processors poll their owning node's `script` every
// block to pick up live editor edits without a full graph rebuild (e.g.
// TerrainSynthProcessor::reloadIfScriptChanged, #23). Meanwhile UI-thread
// editors rewrite `node.script` in place when the user edits a waveform.
// A raw `node.script = ...` assignment is therefore a data race: a
// std::string assignment is not atomic, so the audio thread can observe the
// new size paired with a stale/freed data pointer and copy from garbage.
// For a multi-megabyte granular-wavetable script this reliably crashes
// mid-copy (~1.4 MB memcpy from a bad pointer) about a second after an edit,
// when the autosave/poll happens to land inside the write window.
//
// Writers that target a node which may have a running processor MUST go
// through this helper, and the matching audio-thread reader must take the
// same per-node mutex (`auditionMutex`) around its read. Node-creation sites
// that build a fresh node before any processor exists don't need it.
inline void setNodeScriptSynced(Node& node, std::string s) {
    std::lock_guard<std::mutex> lock(*node.auditionMutex);
    node.script = std::move(s);
}

// Ship one final single cycle to a node's level-triggered held audition - the
// generic "Preview" path used by the waveform/frame editors (see
// Node::heldAudition / AuditionCycleFrame). The voice reads `cycle` as a
// wavetable oscillator at a fixed A4 (note 69) while held, so the edited
// waveform is audible immediately and survives the debounced graph rebuild an
// edit fires. Re-call on every audible edit to keep the audition in sync; an
// empty cycle clears it. Thread-safe via the node's auditionMutex. The node
// must reach an Output node for the audition to actually be heard.
inline void setNodeHeldAuditionCycle(Node& node, std::vector<float> cycle) {
    if (cycle.empty()) {
        std::lock_guard<std::mutex> lock(*node.auditionMutex);
        node.heldAudition.reset();
        return;
    }
    auto cyc = std::make_shared<Node::AuditionCycleFrame>();
    cyc->cycle = std::move(cycle);
    auto ev = std::make_shared<Node::AuditionEvent>();
    ev->isNoteOn   = true;
    ev->pitch      = 69;   // A4, the same fixed audition pitch the shell uses
    ev->velocity   = 127;
    ev->cycleFrame = std::move(cyc);
    std::lock_guard<std::mutex> lock(*node.auditionMutex);
    node.heldAudition = std::move(ev);
}

// Stop a node's held audition (editor Preview -> Stop / editor close). The
// synth releases the voice on its next block. Thread-safe via auditionMutex.
inline void clearNodeHeldAudition(Node& node) {
    std::lock_guard<std::mutex> lock(*node.auditionMutex);
    node.heldAudition.reset();
}

// Named marker on the project timeline
struct Marker {
    int id;
    std::string name;
    float beat;           // absolute beat position
    uint32_t color = 0xFFFFFF00; // yellow default
};

struct Link {
    int id;
    int startPin; // output pin id
    int endPin;   // input pin id
    float gainDb = 0.0f; // gain applied on this connection (0 = unity)
};

class NodeGraph {
public:
    NodeGraph();
    void setupDefaultGraph();

    Node& addNode(const std::string& name, NodeType type,
                  std::vector<Pin> ins, std::vector<Pin> outs,
                  Vec2 pos = {0, 0});
    void addLink(int outPin, int inPin);

    // Group operations
    Node& createGroup(const std::string& name, Vec2 pos = {0, 0});
    void addToGroup(int groupId, int childId);
    void removeFromGroup(int childId);
    // True if `ancestorId` appears in `nodeId`'s parent chain (cycle guard).
    bool isAncestorOf(int ancestorId, int nodeId);
    void resolveAnchors();
    float getAbsoluteBeatOffset(int nodeId); // cascading offset through parent chain

    // Insert/delete time
    void insertTime(float atBeat, float duration, int nodeId = -1); // -1 = all nodes
    void deleteTime(float fromBeat, float toBeat, int nodeId = -1); // update groupBeatOffset from anchor markers
    Node* findNode(int id);

    // Snapshot automation: write the current value of all armed params as
    // flat automation across a beat range. Clears any existing automation
    // points within the range first, then inserts two points (start + end)
    // at the current value = flat line.
    void writeAutomationToSelection(float startBeat, float endBeat) {
        for (auto& node : nodes) {
            for (auto& p : node.params) {
                if (!p.autoWriteArmed) continue;
                // Remove existing points within the range
                p.automation.points.erase(
                    std::remove_if(p.automation.points.begin(), p.automation.points.end(),
                        [startBeat, endBeat](const AutomationPoint& pt) {
                            return pt.beat >= startBeat && pt.beat <= endBeat;
                        }),
                    p.automation.points.end());
                // Insert flat line: two points at current value
                p.automation.points.push_back({startBeat, p.value});
                p.automation.points.push_back({endBeat, p.value});
                // Keep sorted
                std::sort(p.automation.points.begin(), p.automation.points.end(),
                    [](const AutomationPoint& a, const AutomationPoint& b) {
                        return a.beat < b.beat;
                    });
            }
        }
        dirty = true;
    }

    // Arm/disarm all params on all nodes
    void armAllParams(bool armed) {
        for (auto& node : nodes)
            for (auto& p : node.params)
                p.autoWriteArmed = armed;
    }

    // Arm/disarm all params on a specific node
    void armNodeParams(int nodeId, bool armed) {
        if (auto* nd = findNode(nodeId))
            for (auto& p : nd->params)
                p.autoWriteArmed = armed;
    }

    std::vector<Node> nodes;
    std::vector<Link> links;
    std::vector<int> openEditors;  // node IDs - never store Node*

    // Synchronization between graph-mutating threads (UI / file load /
    // tracker import) and the audio thread, which iterates `nodes` and
    // `links` every block (both directly for MIDI routing in
    // AudioEngine::audioDeviceIOCallbackWithContext and indirectly via
    // GraphProcessor::rebuildGraph when the node count changes).
    //
    // Why this is required: a previous tracker-import crash (SEANCE.exe
    // .63000.dmp) was traced to a data race. MOD import calls
    // graph.addNode() ~10 times in succession; each call may trigger a
    // std::vector reallocation. The audio callback, observing the size
    // change between blocks, rebuilds the JUCE processor graph by
    // iterating graph.nodes. If reallocation happened mid-iteration, the
    // audio thread read garbage Node::id values (observed 0 and
    // 1132382734 in the rebuilt nodeMap), which then crashed downstream
    // in the JUCE graph wiring.
    //
    // Usage pattern (audio thread): take a try-lock at the top of the
    // audio callback and fall through to silence if the lock can't be
    // acquired immediately - blocking on the audio thread would risk
    // device underruns, and a single silent block during a multi-second
    // import is barely audible compared to a crash.
    //
    // Usage pattern (mutators): hold a lock_guard for the duration of any
    // batch that clears/rebuilds graph.nodes/links (tracker import, project
    // file load, undo snapshot restore). Crucially, even a SINGLE addNode()
    // /addLink() must be locked: a lone push_back that reallocates the vector
    // move-constructs every existing Node into new storage, which nulls the
    // moved-from Node's shared_ptr members (e.g. mpePassthroughMutex). If the
    // audio thread is mid-processBlock holding a Node& into the old storage,
    // it then dereferences a null mutex and crashes - exactly the new-MIDI-
    // timeline crash (SEANCE.exe.118460.dmp: MidiInputProcessor::processBlock
    // locking *node.mpePassthroughMutex, rbx=0). An earlier comment here
    // wrongly claimed single-node additions "race-resolve quickly" and didn't
    // need the lock; they do. addNode()/addLink() now take the lock
    // themselves, so every structural mutation is covered whether or not the
    // caller wrapped it.
    //
    // Recursive because batch mutators (which hold this lock) compose
    // addNode()/addLink() (which also take it): setupDefaultGraph() runs under
    // the lock at the new-project callsite, and ProjectFile / MOD import call
    // addNode while already locked. A plain std::mutex would self-deadlock on
    // that nesting; recursive_mutex lets the same thread re-enter. The audio
    // thread never owns the lock, so its try_lock still fails (and goes silent)
    // whenever any GUI thread is mid-mutation.
    mutable std::recursive_mutex mutationLock;

    float editorPanelHeight = 250.0f;
    int activeEditorNodeId = -1; // node ID of the currently focused editor
    PluginHost* pluginHost = nullptr; // set by App

    // Content-addressed store for large immutable baked blobs (generated /
    // imported terrain grids; later decoded video, wavetable PCM). Nodes refer
    // to blobs by short hash in node.script; the bytes live here once, keyed by
    // a hash of their canonical .npy payload. Excluded from undo snapshots (the
    // hash travels in the snapshot, the bytes do not) - see content_store.h.
    ContentStore contentStore;

    // Project-level asset library ("stores") - reusable waveforms / generators,
    // independent instruments, ADHSR curves, and morph algorithms, each published
    // once and referenced by a stable integer id from many places. See
    // asset_library.h for the model (explicit add, live-reference-by-id, soft-
    // delete, disjoint user/built-in id spaces).
    AssetLibrary assets;

    // Dirty tracking - set on any mutation
    bool dirty = false;

    // Transport state
    float bpm = 120.0f;
    int timeSignatureNum = 4;
    int timeSignatureDen = 4;
    bool metronomeEnabled = false;
    bool loopEnabled = false;
    double loopStartBeat = 0;
    double loopEndBeat = 0;
    double projectSampleRate = 0; // 0 = use device rate

    // Global automation record mode (record axis root of the cascade). Session
    // state - deliberately NOT serialized, so opening a project always starts
    // with recording disarmed (Off) and node/param overrides dormant. Set by the
    // transport-bar "Auto" control. See resolveArmMode().
    AutoArmMode autoArmGlobal = AutoArmMode::Off;

    // Saved view state for the main node-graph component. Persisted to
    // the project file so reopening the project restores the user's
    // last pan/zoom instead of snapping back to a fit-all view.
    //   viewZoom = 0  -> "no saved view", the component falls back to
    //                    fitAll() on first paint (the default for new
    //                    or pre-feature projects).
    //   viewZoom > 0  -> restore that zoom and (viewPanX, viewPanY) as
    //                    the screen-space offset.
    // These are intentionally excluded from undo snapshots (writeProject
    // is given includeView=false in serializeForUndo) so undoing graph
    // edits does not also jerk the user's view around.
    float viewZoom = 0.0f;
    float viewPanX = 0.0f;
    float viewPanY = 0.0f;

    // Song length and repeat behavior.
    //
    // songLengthBeats = 0 means "auto" - derive the effective end from the
    // last clip across all timeline nodes (see effectiveSongLengthBeats()).
    // > 0 marks an explicit end beat that overrides the auto-derived value.
    // When the playhead reaches the effective end, the repeat policy
    // decides what happens next.
    //
    // Repeat modes:
    //   None    - stop at the song end and halt playback.
    //   Forever - wrap back to beat 0 and keep playing until Stop.
    //   NTimes  - wrap back to beat 0, play the song N times total, then
    //             stop. N = songRepeatCount, where 1 means "play once
    //             then stop" (same as None), 2 means "play twice", etc.
    //
    // The user-region loop (loopEnabled / loopStartBeat / loopEndBeat) is
    // an inner A-B cycler and takes precedence while active - the song-
    // length policy only fires when the user loop is disabled or the
    // playhead is outside the user-loop range.
    //
    // Tracker import uses Forever to preserve Bxx song-loop semantics
    // instead of unrolling the order list in place.
    enum class SongRepeat : int { None = 0, Forever = 1, NTimes = 2 };
    double    songLengthBeats = 0;
    SongRepeat songRepeatMode  = SongRepeat::None;
    int       songRepeatCount  = 1;   // only used when mode == NTimes

    // Exact, un-rounded end of the last clip across all AudioTimeline /
    // MidiTimeline nodes (max of clip.startBeat + clip.lengthBeats). Unlike
    // getTimelineBeats(), this does NOT round up to a 4-beat bar, so it can
    // land mid-bar. Used as the auto-derived playback song length so the
    // audible end matches where content actually stops (and where the
    // song-end marker is drawn). Returns 0 if no timeline has clips.
    double contentEndBeats() const;

    // Returns the effective song-end beat used by the transport.  If
    // songLengthBeats was set explicitly (> 0), that value wins. Otherwise
    // walks all AudioTimeline / MidiTimeline nodes and returns the exact
    // end of the last clip (contentEndBeats(), un-rounded - playback may end
    // mid-bar). Returns 0 if there are no timelines with clips (in which case
    // the engine treats the song as having no end and just plays until the
    // user presses Stop). Note: the timeline grid/display width still rounds
    // up to a full bar via getTimelineBeats(); only the audible end is exact.
    double effectiveSongLengthBeats() const;

    // When content is added past an explicit song-length override (e.g. the
    // user pastes or draws notes beyond the current song end), the override
    // would otherwise clamp playback and silently cut off the new content -
    // the auto-derived length follows the clips, but the override doesn't.
    // Call this after any edit that can extend a clip: in auto mode
    // (songLengthBeats <= 0) it's a no-op (the auto value already covers the
    // content); with an explicit override it grows the override to the new
    // content end so the added beats actually play. Never shrinks the
    // override, so a deliberately-longer "trailing silence" length is kept.
    // Returns the prior override value so callers can restore it on undo.
    double growSongLengthToContent();

    // Tuning system and concert pitch (project-wide)
    TuningSystem tuningSystem = TuningSystem::Equal12;
    float concertPitch = 440.0f; // Hz for A4

    // Global crossfade duration (seconds) used to smooth audio discontinuities
    // anywhere the engine starts or stops a routing path mid-stream - effect
    // region edges, mute/solo toggles, plugin bypass, future child-track
    // entry/exit, etc. Per-feature overrides (e.g. EffectGroup::crossfadeSec)
    // take precedence when explicitly set.
    float globalCrossfadeSec = 0.05f;

    // Check if a node has any incoming signal/param connections (meaning
    // one or more params may be externally controlled). Used to grey out
    // and lock sliders when a signal is driving them.
    bool hasSignalInput(int nodeId) const {
        for (const auto& link : links) {
            for (const auto& node : nodes) {
                for (const auto& pin : node.pinsOut) {
                    if (pin.id == link.startPin &&
                        (pin.kind == PinKind::Signal || pin.kind == PinKind::Param)) {
                        // Found a signal/param output pin as the source.
                        // Check if destination is our node.
                        for (const auto& dstNode : nodes) {
                            if (dstNode.id != nodeId) continue;
                            for (const auto& dstPin : dstNode.pinsIn)
                                if (dstPin.id == link.endPin) return true;
                        }
                    }
                }
            }
        }
        return false;
    }

    // True if one *specific* param on a node is currently being driven by a
    // connected Signal/Param cable - i.e. it has a modulation input pin
    // (Node::ModPin) whose pin has a live link plugged into it. This is the
    // per-param version of hasSignalInput(): only the actually-driven param's
    // manual control should lock, not every param on the node.
    bool paramHasSignalInput(int nodeId, int paramIndex) const {
        const Node* nd = nullptr;
        for (const auto& n : nodes) if (n.id == nodeId) { nd = &n; break; }
        if (!nd) return false;
        for (const auto& mp : nd->modPins) {
            if (mp.paramIndex != paramIndex) continue;
            for (const auto& link : links)
                if (link.endPin == mp.pinId) return true;
        }
        return false;
    }

    // True if a param is driven by a connected *Absolute* ("Set") cable. Such a
    // param is fully locked in the UI - the cable sets its value directly, so
    // there is no resting/base value for the user to edit. A param driven only
    // by Modulate ("Mod") cables is NOT absolute-locked: the user can still
    // drag its knob to set the center the modulation swings around.
    bool paramHasAbsoluteInput(int nodeId, int paramIndex) const {
        const Node* nd = nullptr;
        for (const auto& n : nodes) if (n.id == nodeId) { nd = &n; break; }
        if (!nd) return false;
        for (const auto& mp : nd->modPins) {
            if (mp.paramIndex != paramIndex) continue;
            if (mp.mode != Node::ModPin::Mode::Absolute) continue;
            for (const auto& link : links)
                if (link.endPin == mp.pinId) return true;
        }
        return false;
    }

    // Effect groups (bundles of links that activate together as one layer)
    std::vector<EffectGroup> effectGroups;
    int nextEffectGroupId = 1;

    EffectGroup& addEffectGroup(const std::string& name = "") {
        EffectGroup g;
        g.id = nextEffectGroupId++;
        g.name = name;
        // Auto-assign color: fixed palette first, then golden-angle generated
        g.color = getDistinctColor((int)effectGroups.size());
        effectGroups.push_back(g);
        return effectGroups.back();
    }

    EffectGroup* findEffectGroup(int id) {
        for (auto& g : effectGroups)
            if (g.id == id) return &g;
        return nullptr;
    }

    // Project markers (named beat positions)
    std::vector<Marker> markers;
    Marker* findMarker(const std::string& name) {
        for (auto& m : markers)
            if (m.name == name) return &m;
        return nullptr;
    }
    float resolveMarkerBeat(const std::string& name) {
        auto* m = findMarker(name);
        return m ? m->beat : -1.0f;
    }

    // Signal automation script - Python code that defines signal bindings
    // Re-executed when project is loaded
    std::string signalScript;
    UndoTree undoTree;

    // Shared undo history sidecar path. If non-empty, the project is
    // associated with an external history file (stored alongside the
    // .ssp so it can travel with the project when shared). The path is
    // relative to the .ssp file's directory. See task #90 for the full
    // lifecycle (save-as opt-in, open-prompt with three options, known-
    // histories preferences).
    //
    // Empty string = no sidecar; undo history persists only to the
    // machine-local userAppData/SEANCE/undo-tree.dat file.
    std::string historyFilePath;

    // Shared waveform library - named waveforms usable by any synth/signal node
    struct WaveformEntry {
        std::string name;
        std::string expression;    // source expression (empty if from points)
        std::vector<std::pair<float, float>> points; // control points (phase, amplitude)
        std::vector<float> samples; // cached rendered waveform
    };
    std::vector<WaveformEntry> waveformLibrary;

    // CC mappings stored here for save/load (synced with AutomationManager at save time)
    struct CCMap { int midiCh; int ccNum; int nodeId; int paramIdx; };
    std::vector<CCMap> ccMappings;

    void setNextId(int id) { nextId = std::max(nextId, id); }
    int getNextId() const { return nextId; }
    // Allocate and return a fresh id, bumping the internal counter so the
    // same value isn't handed out twice. Use this when you need a NEW id
    // for a pin you're appending from outside addNode (e.g. SignalShape's
    // dynamic-signal-input pin rewrite). getNextId() above is the
    // peek-without-allocate variant - useful for save/load size hints,
    // not for actually creating ids.
    int allocId() { return newId(); }

    float getTimelineBeats(const Node& node) const;

    // Execute a command through the undo system
    void exec(std::unique_ptr<Command> cmd) { undoTree.execute(std::move(cmd)); }
    void exec(const std::string& desc, std::function<void()> doFn, std::function<void()> undoFn) {
        exec(std::make_unique<LambdaCommand>(desc, std::move(doFn), std::move(undoFn)));
    }

    // Commit a snapshot of the current graph state to the undo tree as a
    // new step. The serialization is performed via ProjectFile::serializeForUndo
    // (graph-only, plugin state excluded). If the resulting text is identical
    // to the previous step's snapshot - i.e., nothing actually changed - this
    // is a no-op, so it's safe (and intended) to call defensively from any
    // mutating function. See CLAUDE.md "Undo Strategy" for the policy on
    // when to use commitSnapshot vs. exec().
    //
    // Defined in node_graph.cpp because the implementation needs project_file.h,
    // which itself includes node_graph.h.
    void commitSnapshot(const std::string& description);

    // Re-resolve every node that LIVE-references an asset-library AHDSR curve
    // (ahdsrAssetId >= 0): decode the stored curve into the node's local
    // ahdsrEnvelope mirror so the audio thread (which reads ahdsrEnvelope
    // directly) sees the current library curve. Call after project load and
    // after any edit to a referenced AhdsrCurve asset. Defined in
    // node_graph.cpp (needs adsr_envelope.h / asset_library.h, both included).
    void resolveAhdsrReferences();

    std::map<int, PianoRollState> pianoRollStates;

private:
    int nextId = 1;
    int newId() { return nextId++; }
    std::string scriptConsoleText;
    std::string scriptConsoleOutput;

    // Node editor context
    void* editorContext = nullptr;
};

// Resolve every live Waveform asset reference in the graph. For each node whose
// script is a wavetable, any library entry referencing a published Waveform
// asset (assetId >= 0) has its frame replaced by a fresh copy of the asset's
// frame, so the audio thread (which re-decodes node.script) sees the asset
// content. A missing/erased asset detaches the entry to independent. Re-encodes
// the affected node scripts in place. Free function (not a NodeGraph method)
// because the implementation lives in layered_wave_editor.cpp where the
// wavetable codec + frame factory are; declared here so the non-GUI
// serialization layer (project_file.cpp) can call it without the editor header.
// Returns the number of references resolved. Mirrors resolveAhdsrReferences().
int resolveWaveformReferences(NodeGraph& graph);

// Resolve every live PER-LAYER Waveform asset reference in the graph: the
// per-layer analogue of resolveWaveformReferences. For each wavetable node, any
// WaveLayer with assetId >= 0 (inside a LayeredWaveform library entry) has its
// shape content replaced by a fresh decode of the referenced asset, preserving
// the layer's own amp (slot volume). A missing/erased asset detaches that layer
// to independent. Re-encodes affected node scripts in place. Free function for
// the same reason as resolveWaveformReferences (codec lives in
// layered_wave_editor.cpp). Returns the number of layer references resolved.
int resolvePerLayerWaveformReferences(NodeGraph& graph);

// Resolve every live MorphAlgorithm (warp-chain) asset reference in the graph.
// For each wavetable node whose WavetableDoc has warpAssetId >= 0, the frame-
// scope warp chain is replaced by a fresh decode of the asset's stored chain,
// and the node's "Warp N" modulation params are reconciled to the new op count
// (resolving a chain can change its length). A missing/erased asset detaches to
// independent (keeps the cached chain). Re-encodes affected node scripts in
// place. Free function for the same reason as resolveWaveformReferences (codec
// lives in layered_wave_editor.cpp). Returns the number of references resolved.
int resolveWarpReferences(NodeGraph& graph);

// Load-time reconcile of frame-scope warp modulation params across EVERY
// wavetable node (not just asset-referenced ones). Migrates legacy "Warp N"
// params to the warpSlot key + named-morph display labels and ensures the synth
// (which reads warp amounts by warpSlot) always finds them. Idempotent. Defined
// in layered_wave_editor.cpp (needs the WavetableDoc codec). Call after load.
void reconcileAllWarpParams(NodeGraph& graph);

// Reconcile the per-layer (warpLayer >= 0) Type-2 warp params of ONE frame
// (identified by `frameId`, the owning library entry id stored in
// Param::warpFrameId) for a single wavetable node against that frame's live
// per-layer warp chains. Removes params of this frame whose (warpLayer,
// warpSlot) no longer addresses a live op (dropping their modPins, pins and
// links), remaps survivors to their new positional slot, and relabels them from
// the method name. Params of OTHER frames are left untouched. `layerChains[L]`
// is the ordered warp chain of layer L. Defined in layered_wave_editor.cpp.
// Idempotent.
void reconcilePerLayerWarpParams(NodeGraph& graph, int nodeId, int frameId,
                                 const std::vector<std::vector<WarpOp>>& layerChains);

// ---------------------------------------------------------------------------
// On-demand modulation pins (#88) - graph-level helpers.
//
// These are the pure data-model operations behind the node-graph right-click
// "Add/Remove modulation input" menu AND the warp/morph editor's per-param
// "modulate" checkbox (the unified warp/morph model: any shaping param can opt
// into a modulation pin). They mutate the graph ONLY - no snapshot, no repaint,
// no rebuild callback - so every caller drives its own commit/undo/rebuild flow
// (NodeGraphComponent does its commitSnapshot()+onNodeEdited(); the layered-wave
// editor folds the change into its settled-edit commit). Addressing is by stable
// (nodeId, paramIndex) so nothing dangles across the call.
// ---------------------------------------------------------------------------

// True iff node `nodeId` has a ModPin bound to param `paramIndex`.
bool hasParamModPin(const NodeGraph& graph, int nodeId, int paramIndex);

// Add a modulation pin for (nodeId, paramIndex) if one doesn't already exist.
// `absolute` chooses Set (true) vs Modulate (false) mode. Returns the new (or
// existing) pin id, or -1 if the node/param is invalid. Does NOT commit/rebuild.
int addParamModPin(NodeGraph& graph, int nodeId, int paramIndex, bool absolute);

// Remove the modulation pin bound to (nodeId, paramIndex) - dropping its input
// pin and any cables into it, and clearing the param's modulated state so it
// returns to its resting value. Returns true if a pin was removed. No commit.
bool removeParamModPin(NodeGraph& graph, int nodeId, int paramIndex);

// Restore the pin<->modPin invariant on `nodeId`: drop any "Mod:"/"Set:" Param
// input pin with no backing modPin (a dangling ghost modulation input), and any
// modPin whose param is out of range or whose pin is gone. Returns the count
// removed (0 = already consistent, the common case). Idempotent. No commit.
// Run wherever a node's params/pins are reconciled (e.g. syncWarpParamsForNode,
// which fires on load + every warp edit) so historical corruption self-heals.
int pruneOrphanModPins(NodeGraph& graph, int nodeId);

// FM operator envelopes. Ensures an FM synth node (`script == "__fmsynth__"`)
// carries exactly 4 per-operator AHDSREnvelopes in `node.opEnvelopes`,
// migrating any legacy "Op{i} A/D/S/R" linear-ramp params into them (and
// stripping those params) the first time. A no-op for non-FM nodes and for FM
// nodes already holding 4 envelopes. Called both at node creation (seeds
// defaults) and after project load (migrates old files). Safe to call
// repeatedly.
void ensureFmOpEnvelopes(Node& node);

// Build a Voice (polyphonic) container plus a ready-made inner patch + container
// settings for the given factory preset, centred around `pos`. Adds the nodes
// and links to `graph` and returns the new container node's id (or -1 on a bad
// preset id, though unknown ids fall back to the Basic preset). Does NOT commit
// an undo snapshot or fire any rebuild callback - the caller owns those (the GUI
// path commits + rebuilds; the self-test inspects the raw graph). Preset ids:
//   0 = Basic (FM Synth)   1 = Warm Pad   2 = Pluck
//   3 = Supersaw Lead      4 = Noise Perc
// See poly-voice-architecture.md.
int buildVoicePreset(NodeGraph& graph, Vec2 pos, int preset);

} // namespace SoundShop

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

namespace SoundShop {

// Simple 2D point (replaces Vec2)
struct Vec2 {
    float x = 0, y = 0;
};

enum class PinKind { Audio, Midi, Param, Signal }; // Signal = audio-rate control signal (mono)

// Two pin kinds are compatible at the cable level if they're either the same
// kind, or both control kinds (Param + Signal). Param is conceptually
// block-rate and Signal is audio-rate, but at the routing layer we treat them
// as a single "control" family — the conversion is implicit and free, since
// the audio-graph routing already carries them on the same channel slot. The
// receiver decides whether to read once per block (Param semantics) or every
// sample (Signal semantics). See task #82.
inline bool arePinKindsCompatible(PinKind a, PinKind b) {
    if (a == b) return true;
    bool aCtrl = (a == PinKind::Param || a == PinKind::Signal);
    bool bCtrl = (b == PinKind::Param || b == PinKind::Signal);
    return aCtrl && bCtrl;
}
enum class NodeType {
    AudioTimeline, MidiTimeline, Instrument, Effect, Mixer, Output, Script, Group, TerrainSynth, SignalShape,
    // MidiInput represents a single live MIDI input source (computer keyboard,
    // hardware MIDI device, network MIDI client, virtual port, etc). It has
    // no inputs and one MIDI output. The cable wiring from the Input node to
    // a Timeline or synth IS the live-input routing — no flags, no hidden
    // state. See project_midi_input_architecture.md.
    MidiInput
};

struct Pin {
    int id;
    std::string name;
    PinKind kind;
    bool isInput;
    int channels = 2; // 1=mono, 2=stereo, 6=5.1, etc.
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

struct Param {
    std::string name;
    float value;
    float minVal;
    float maxVal;
    std::string format = "%.2f";
    AutomationLane automation; // recorded automation for this param
    bool autoWriteArmed = false; // when armed, "Write Automation to Selection" includes this param
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
    int id;
    std::string name;
    NodeType type;
    std::vector<Pin> pinsIn;
    std::vector<Pin> pinsOut;
    Vec2 pos{0, 0};
    bool posSet = false;
    bool muted = false;
    bool soloed = false;
    std::vector<Param> params;
    std::vector<Clip> clips;

    // Take lanes for comping (audio timelines)
    std::vector<TakeLane> takeLanes;
    std::vector<CompSegment> compSegments;
    int activeTakeLane = -1; // -1 = use clips directly, >=0 = recording to this lane

    std::string script;

    // Performance mode — play preset melody by pressing any keys
    bool performanceMode = false;
    int performanceReleaseMode = 1;   // 0=OnKeyUp, 1=OnNextEvent (legato)
    bool performanceVelocity = true;  // use incoming velocity

    // Custom envelope curves (expressions mapping x in 0..1 to amplitude 0..1)
    // Empty = default linear. x=0 is start of stage, x=1 is end.
    // Attack: default "x" (linear rise). Try "x^0.5" for fast attack.
    // Decay: default "1-x*(1-s)" where s is sustain. Try "1-x^2*(1-s)".
    // Release: default "1-x" (linear fall). Try "(1-x)^3" for long tail.
    std::string envAttackCurve;   // expression, empty = linear
    std::string envDecayCurve;
    std::string envReleaseCurve;
    // Control points alternative (phase 0-1, amplitude 0-1)
    std::vector<std::pair<float, float>> envAttackPoints;
    std::vector<std::pair<float, float>> envDecayPoints;
    std::vector<std::pair<float, float>> envReleasePoints;

    // Panning and spatial positioning
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

    // Per-plugin dirty tracking for the slow autosave path (#86). When a
    // plugin's parameters change via host automation, MIDI Learn CC, or
    // any other host-driven path, this flag is set so the next autosave
    // re-queries getStateInformation. When clear, the saver reuses the
    // cached base64 string instead — avoiding the expensive query for
    // plugins whose state hasn't changed since the last save. Defaults
    // to true so a freshly loaded plugin gets queried at least once.
    //
    // Limitation: changes made by the user inside the plugin's own UI
    // can't be detected here (no general-purpose API to listen for them
    // across plugin formats). Mitigation: a periodic "force-dirty all"
    // tick in the autosave path bounds staleness to a known interval.
    bool pluginStateDirty = true;
    std::string cachedPluginStateBase64;

    // Group — contains child node IDs
    std::vector<int> childNodeIds;  // IDs of nodes inside this group
    int parentGroupId = -1;         // -1 = top-level (not in any group)
    float groupBeatOffset = 0.0f;   // children's timelines start at this beat in the parent
    std::string anchorMarker;       // if non-empty, groupBeatOffset is overridden by this marker's beat
    float absoluteBeatOffset = 0.0f; // cached: cascading offset through all parents (updated by resolveAnchors)
    bool groupExpanded = true;      // show children in graph view

    // Audition MIDI events injected from the UI (thread-safe via simple flag)
    struct AuditionEvent {
        bool isNoteOn;
        int pitch;
        int velocity;
    };
    std::vector<AuditionEvent> pendingAudition; // written by UI, read by audio thread
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

    // Effect regions: time-bounded activation of links/groups on this track.
    // Drawn as colored bars on the track's timeline. Each region gates either
    // a single link (linkId >= 0) or an entire effect group (groupId >= 0).
    std::vector<EffectRegion> effectRegions;

    // Multi-track recording: per-track input assignment
    int recordInputChannel = -1;  // which audio input channel to record from (-1 = none)
    bool recordArmed = false;     // armed for recording
    bool inputMonitor = false;    // pass input through to output in real-time
};

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
    std::vector<Node*> openEditors;

    float editorPanelHeight = 250.0f;
    int activeEditorNodeId = -1; // node ID of the currently focused editor
    PluginHost* pluginHost = nullptr; // set by App
    // Dirty tracking — set on any mutation
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

    // Tuning system and concert pitch (project-wide)
    TuningSystem tuningSystem = TuningSystem::Equal12;
    float concertPitch = 440.0f; // Hz for A4

    // Global crossfade duration (seconds) used to smooth audio discontinuities
    // anywhere the engine starts or stops a routing path mid-stream — effect
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

    // Signal automation script — Python code that defines signal bindings
    // Re-executed when project is loaded
    std::string signalScript;
    UndoTree undoTree;

    // Shared waveform library — named waveforms usable by any synth/signal node
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

    float getTimelineBeats(const Node& node) const;

    // Execute a command through the undo system
    void exec(std::unique_ptr<Command> cmd) { undoTree.execute(std::move(cmd)); }
    void exec(const std::string& desc, std::function<void()> doFn, std::function<void()> undoFn) {
        exec(std::make_unique<LambdaCommand>(desc, std::move(doFn), std::move(undoFn)));
    }

    // Commit a snapshot of the current graph state to the undo tree as a
    // new step. The serialization is performed via ProjectFile::serializeForUndo
    // (graph-only, plugin state excluded). If the resulting text is identical
    // to the previous step's snapshot — i.e., nothing actually changed — this
    // is a no-op, so it's safe (and intended) to call defensively from any
    // mutating function. See CLAUDE.md "Undo Strategy" for the policy on
    // when to use commitSnapshot vs. exec().
    //
    // Defined in node_graph.cpp because the implementation needs project_file.h,
    // which itself includes node_graph.h.
    void commitSnapshot(const std::string& description);

    std::map<int, PianoRollState> pianoRollStates;

private:
    int nextId = 1;
    int newId() { return nextId++; }
    std::string scriptConsoleText;
    std::string scriptConsoleOutput;

    // Node editor context
    void* editorContext = nullptr;
};

} // namespace SoundShop

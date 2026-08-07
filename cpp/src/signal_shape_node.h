#pragma once
#include "node_graph.h"
#include "transport.h"
#include "builtin_synth.h"        // reuse Wavetable and WaveExprParser
#include "script_runtime.h"       // IScriptRuntime, ScriptLang/Rate
#include "script_lang_bar.h"      // ScriptLangBar + WasmFilePanel (editor UI)
#include "layered_wave_editor.h"  // reuse WaveLayer + WaveLayerEditor
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace SoundShop {

// -----------------------------------------------------------------------------
// Signal Shape (LFO + Envelope unified)
// -----------------------------------------------------------------------------
//
// A Signal Shape node emits a control signal (-1..1) defined by:
//
//   1. A waveform "shape" - a LayeredWaveform: a stack of layers (each
//      sine/saw/square/triangle/noise/drawn/freehand/formula at a harmonic
//      ratio + phase + amp) summed into one cycle, exactly like the wavetable
//      editor. A fresh node has no layers (flat 0). This is sampled per-tick
//      from a phase that advances at the user-set rate.
//
//   2. A composition expression `expr` evaluated per sample. By default
//      "curve" (passes the shape through), but the user can write arbitrary
//      math involving signal inputs, MIDI state, time, etc.
//
//   3. A trigger expression `triggerExpr` evaluated per sample. While the
//      trigger holds (> 0), the shape advances. Empty string => always
//      running (free-running LFO). On a rising edge the phase resets and
//      the repeat counter starts over.
//
//   4. A repeat mode controlling what happens at the end of one shape cycle:
//      Forever loops, Once plays through and holds, NTimes repeats N times
//      then holds.
//
// Vocabulary available inside expr / triggerExpr:
//   curve         - the shape's value at the current phase (-1..1)
//   x, phase      - current phase 0..1 through the cycle
//   t             - seconds since trigger fired (always advancing while running)
//   beat          - transport beat position
//   bpm           - current tempo (beats per minute)
//   gate          - 1 if any MIDI note is held, 0 otherwise
//   freq          - frequency in Hz of the most-recently-pressed held note (0 if none)
//   note          - MIDI note number of the most-recent note-on (-1 if none)
//   vel           - velocity 0..1 of the most-recent note-on
//   rep           - repeats completed since the last trigger (0 on first cycle)
//   rate          - the Rate param
//   s1..sN        - signal-input slot values at this sample (sN = 0 if missing)
//
// Plus the full WaveExprParser vocabulary (sin, cos, saw, square, triangle,
// noise, random, abs, sqrt, exp, log, pow, tanh, clamp, floor, ceil,
// min(a,b), max(a,b), if(c,a,b), ternary c?a:b, comparison ops, && || !).
//
// This consolidates the previous LFO / Envelope split into a single node
// type. "Envelope behavior" = set repeat to Once + use trigger="gate" to fire
// on note-on. "LFO behaviour" = leave triggerExpr empty + repeat=Forever.
struct SignalShapeDoc {
    // The shape waveform: a stack of layers summed into one cycle, identical to
    // the wavetable editor's layered waveform. A fresh node has ZERO layers
    // (flat 0 output) so it does nothing until the user adds a layer - it never
    // starts sweeping a default shape on its own.
    LayeredWaveform layers;
    std::string expr = "curve";       // Composition source. For Builtin this is a
                                      // one-line expression of `curve`/etc.; for
                                      // Lua it holds the whole Lua program (its
                                      // loop() returns the value, or fills the
                                      // buffer in block mode). Ignored for Wasm.
    std::string triggerExpr;          // Trigger condition; empty = always running.

    // Which scripting language evaluates `expr`:
    //   Builtin - the SEANCE mini-expression language (per-sample only).
    //   Lua     - embedded Lua 5.4 (per-sample OR per-block, see `rate`).
    //   Wasm    - a user-supplied .wasm binary (per-block only; from wasmPath).
    // The drawn shape, trigger, repeat and phase machinery apply to the
    // per-sample path (Builtin, Lua-per-sample). Per-block (Lua block-rate,
    // Wasm) is a raw-signal mode: the script fills the output buffer directly
    // and the trigger / phase / curve machinery is bypassed.
    ScriptLang language = ScriptLang::Builtin;

    // Execution rate. Builtin is always PerSample; Wasm is always PerBlock; only
    // Lua honours this choice. PerSample is sample-accurate but heavy Lua can
    // stutter the audio thread; PerBlock runs the script once per audio block.
    ScriptRate rate = ScriptRate::PerSample;

    // Filesystem path to the .wasm binary (Wasm language only).
    std::string wasmPath;

    enum RepeatMode { Forever = 0, Once = 1, NTimes = 2 };
    RepeatMode repeatMode = Forever;
    int repeatN = 1;                  // Only used when repeatMode == NTimes.

    // Phase source for a non-triggered (LFO) shape:
    //   false (default) - phase is locked to the transport/song position, so
    //                     the shape is deterministic (export == playback,
    //                     identical every playthrough) and freezes when the
    //                     transport is stopped.
    //   true            - phase free-runs off its own clock (analog-LFO style),
    //                     drifting independently of the transport and never
    //                     stopping. Ignored when a Trigger expression is set
    //                     (a triggered shape is already deterministic per
    //                     trigger).
    bool freeRun = false;

    // Number of Signal input pins (0..16). Exposed in expressions as s1..sN.
    int signalInputCount = 0;

    // ---- Unified scriptable-node I/O (see node header comment) --------------
    //
    // The Signal Shape node is the general scriptable node: one program both
    // assigns continuous outputs o1..oP AND emits MIDI. The four I/O dimensions:
    //
    //   signalInputCount   - Signal/Param INPUT pins  s1..sN  (above)
    //   continuousOutputCount - Signal/Param OUTPUT pins, exposed as o1..oP.
    //                        The program assigns o1..oP (o1 defaults to the
    //                        program's last bare expression, so "curve" still
    //                        means o1 = curve). Always >= 1.
    //   midiOutputCount    - MIDI OUTPUT pins (0..16). 0 = pure signal source
    //                        (no MIDI emit, classic Signal Shape). >=1 enables
    //                        the emit functions note()/cc()/bend()/... with the
    //                        reserved `out` variable selecting the pin.
    //   midiInputCount     - MIDI INPUT pins (0..16). 0 = no MIDI input (the
    //                        note/vel/gate/freq variables stay at their idle
    //                        values). 1 = the classic single "MIDI In" pin. >1
    //                        exposes "MIDI In 1..N"; events carry which pin they
    //                        arrived on (pollmidi()/the structured pull report a
    //                        1-based input index), so one program can react to
    //                        several MIDI sources independently. The note/vel/gate
    //                        convenience variables track the most recent note-on
    //                        across ALL inputs.
    //
    // A node with midiOutputCount==0 behaves exactly like the old Signal Shape;
    // continuousOutputCount==1 + a MIDI-emitting program behaves like the old
    // MIDI Script (its single continuous output is just unused). Both > 0 is a
    // hybrid (e.g. an arpeggiator that also outputs an envelope).
    int continuousOutputCount = 1;
    int midiOutputCount = 0;
    int midiInputCount = 1;

    // Pin kind for the continuous input/output pins: false = Signal (blue),
    // true = Param (orange). Purely semantic - both route identically on the
    // wire (control channels 2+). The editor's type dropdown flips this for ALL
    // continuous pins at once.
    bool paramKind = false;

    // Round-trip via node.script. Prefix: "__signalshape__:".
    // Format (newline-separated key=value, base64 where values may contain
    // structural characters):
    //   __signalshape__:v1
    //   expr=<base64>
    //   trigger=<base64>
    //   repeat=<int>
    //   repeatN=<int>
    //   freeRun=<0/1>
    //   sigCount=<int>
    //   layer=<LayeredWaveform::encodeBody() - tableSize|layer1|layer2|...>
    // The "layer=" key carries the whole layer stack (one body line). Old
    // single-layer scripts used the same encoding for their one layer, so they
    // decode straight into a one-element stack with no migration needed.
    std::string encode() const;
    bool decode(const std::string& s);

    // Default: free-running LFO config (Forever, no trigger, expr="curve") but
    // with NO layers, so the node outputs a constant 0 until the user adds a
    // layer. Intentionally does not seed a sine - a brand-new node should never
    // start sweeping a shape the user didn't ask for.
    static SignalShapeDoc defaultLFO();

    // True iff `s` starts with the __signalshape__: prefix - cheap way for
    // call sites to distinguish a new-style doc from a legacy expression.
    static bool isSignalShapeScript(const std::string& s);
};

class SignalShapeProcessor : public juce::AudioProcessor {
public:
    SignalShapeProcessor(Node& node, Transport& transport);

    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double sr, int bs) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return true; }
    // Produces MIDI only when the program declares >=1 MIDI output pin. `doc` is
    // kept in sync with node.script by reloadIfScriptChanged(); the graph reads
    // this when wiring MIDI cables.
    bool producesMidi() const override { return doc.midiOutputCount > 0; }
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

    float getCurrentValue() const { return lastOutputValue; }

    // How many MIDI output pins this program declares (0..16). graph_processor
    // reads this to decide whether to insert per-cable channel filters (same
    // mechanism as the retired MIDI Script node).
    int getMidiOutputCount() const { return doc.midiOutputCount; }

    // UI-thread: queue one manual-trigger firing for the next audio block.
    // Equivalent to the user typing a rising edge on the trigger expression
    // by hand. Atomic so it's safe from any thread.
    void fireManualTrigger() { manualTriggerPending.store(true); }

    // True when the most recent script (re)load failed to compile/link (Lua
    // syntax error, Wasm load/link error, ...). Set on the AUDIO thread in
    // reloadIfScriptChanged(); read on the MESSAGE thread by the node-graph
    // editor to draw a red error badge. atomic<bool> so the cross-thread read
    // is data-race-free. Builtin never errors here (lazy per-sample parse).
    bool hasScriptError() const { return scriptHasError.load(std::memory_order_relaxed); }

private:
    Node& node;
    Transport& transport;
    double sampleRate = 44100;

    // Live doc, refreshed from node.script whenever the script string
    // changes (set by the editor / project load / undo).
    SignalShapeDoc doc;
    std::string lastScriptSeen;
    void reloadIfScriptChanged();

    // Pre-rendered single-cycle of the shape waveform, sampled by `phase`
    // with linear interpolation. Rebuilt by reloadIfScriptChanged when
    // doc.layer changes (and at first load). Fixed size keeps the per-sample
    // lookup cheap regardless of the layer's internal representation.
    static constexpr int kShapeTableSize = 1024;
    std::vector<float> shapeSamples;
    void rebuildShapeSamples();

    // The active language runtime (Builtin / Lua / Wasm) that evaluates the
    // composition. Re-created when the script or language changes.
    std::unique_ptr<IScriptRuntime> runtime;
    // True when the runtime fills the whole block (Wasm always; Lua block-rate).
    // In that mode the trigger / phase / repeat / curve machinery is bypassed and
    // the script writes the output buffer directly via runBlock().
    bool runBlockMode = false;
    // Set true after a failed runtime->load() (Lua/Wasm compile/link error);
    // cleared on a successful load. Written on the audio thread, read on the
    // message thread (see hasScriptError()).
    std::atomic<bool> scriptHasError{false};
    // Scratch output buffer for block mode (filled by runBlock, then fanned out
    // to every Signal/Param output channel). Kept as a member so it isn't
    // reallocated on the audio thread every block.
    std::vector<float> blockOut;
    // Multi-output scratch for STREAMING block mode: one buffer per continuous
    // output pin o1..oP, plus a pointer array handed to ScriptBlockCtx::outs.
    // A streaming script writes each pin independently via out(pin,v); we route
    // outBufs[p] to output channel outChStart+p. Members so nothing reallocates
    // on the audio thread per block (only when the output count changes).
    std::vector<std::vector<float>> outBufs;
    std::vector<float*>             outPtrs;
    // Event-driven MIDI-input scratch for block mode (ScriptBlockCtx::midiIn).
    // A member so it isn't reallocated on the audio thread every block.
    std::vector<ScriptMidiEvent> midiInEvents;
    // Bind the shape(pos) sampler on the runtime after a shape rebuild.
    void bindShape();

    // Per-sample run state. `running` is true while the trigger expression
    // evaluates > 0 (or always, when triggerExpr is empty). `repeatsDone`
    // counts full cycles completed since the last rising-edge trigger so
    // we can implement Once / NTimes terminations.
    float phase = 0.0f;
    bool  running = false;
    int   repeatsDone = 0;
    double timeSinceTrigger = 0.0;
    bool  prevTriggerHigh = false;
    // Neutral resting value on the unipolar 0..1 control wire is 0.5 ("no
    // modulation"), so an idle / not-yet-triggered node holds at neutral rather
    // than pegging its target param to the minimum.
    float lastOutputValue = 0.5f;
    // Per-continuous-output last value (o1..oP), held when the shape isn't
    // running (envelope "stuck at the end"). Index 0 mirrors lastOutputValue.
    // Sized lazily to continuousOutputCount; defaults to 0.5 (neutral).
    std::vector<float> lastOuts;
    bool  wasPlaying = false;   // transport play rising edge, for the start hook

    std::atomic<bool> manualTriggerPending { false };

    // Snapshot of the incoming control-input channels (Signal/Param inputs ride
    // buffer channels 2+). Captured BEFORE buf.clear() because input and output
    // channels share storage in the JUCE graph, so clearing the buffer to write
    // outputs would otherwise zero s1..sN before the per-sample loop reads them
    // (the same hazard the terrain synth hit). Kept as a member to avoid
    // reallocating on the audio thread.
    juce::AudioBuffer<float> ctrlIn;

    // ---- audio-thread scratch (see scratchCapacityBytes) --------------------
    // All three of these were locals in processBlock, i.e. heap traffic on every
    // callback:
    //   sigChans  - read pointers for the s1..sN control inputs. Also handed to
    //               the block-mode runtime as ScriptBlockCtx::sig, so it has to
    //               outlive the call anyway.
    //   vars      - the expression variable bindings. Worst of the three: a
    //               fresh unordered_map per block meant a bucket array plus a
    //               node allocation for every one of the ~12 fixed keys and each
    //               s1..sN. Kept alive across blocks, operator[] on an existing
    //               key only overwrites the value. Rebuilt (varsSigCount) when
    //               the input count changes, so a shrunk s-list can't leave a
    //               stale sN readable by an expression.
    //   startVars - the play-edge start() hook's bindings; same reuse.
    std::vector<const float*> sigChans;
    ScriptVars                vars;
    int                       varsSigCount = -1;
    ScriptVars                startVars;
    // Cached "s1".."sN" key names, so the per-sample binding doesn't rebuild a
    // std::string for every input of every sample.
    std::vector<std::string>  sigVarNames;

    // shape(pos) sampler handed to the expression parser. Built once (it only
    // captures `this`, and shapeSamples is a member, so it stays valid across
    // rebuilds) instead of being reconstructed per block. std::function is
    // allowed to heap-allocate for captures that don't fit its small-buffer
    // optimisation, and that's an implementation detail we shouldn't be betting
    // the audio thread on.
    std::function<float(float)> shapeFn;

public:
    // Total bytes held by the audio-thread scratch above. The self-test asserts
    // this doesn't grow across a render, which is how we prove processBlock is
    // allocation-free without hooking global operator new (SEANCE hosts
    // third-party VST3s, whose allocations a global hook would also catch).
    // `vars` contributes both its bucket count and its size: a rehash grows the
    // former, and a newly inserted key (one node allocation) grows the latter.
    size_t scratchCapacityBytes() const {
        size_t n = sigChans.capacity() * sizeof(const float*)
                 + blockOut.capacity() * sizeof(float)
                 + outPtrs.capacity()  * sizeof(float*)
                 + outBufs.capacity()  * sizeof(std::vector<float>)
                 + (vars.bucket_count()      + vars.size())      * sizeof(void*)
                 + (startVars.bucket_count() + startVars.size()) * sizeof(void*)
                 + midiInEvents.capacity() * sizeof(ScriptMidiEvent)
                 + lastOuts.capacity() * sizeof(float)
                 + sigVarNames.capacity() * sizeof(std::string);
        // ctrlIn is deliberately not counted: setSize(..., avoidReallocating)
        // reports the *requested* sample count, which tracks the host block
        // size, so it would make the proxy fluctuate for a buffer that only ever
        // grows (and only when a block is longer than any seen before).
        for (const auto& b : outBufs) n += b.capacity() * sizeof(float);
        return n;
    }

private:

    // MIDI state tracked across blocks (used by gate / freq / note / vel
    // expression variables).
    int   notesHeld = 0;
    int   lastNoteOn = -1;
    float lastVelocity = 0.0f;

    bool  wasPlayingMidi = false;  // play edge for the MIDI-emit stop flush

    // ---- MIDI emission (unified node, midiOutputCount > 0) ------------------
    // Ported from the retired MIDI Script node: a note(p,v,d) call schedules a
    // note-off `samplesRemaining` samples from the current block start; drained
    // once per block so durations longer than one block still end correctly.
    struct PendingNoteOff {
        long long samplesRemaining;
        int channel;   // 1..16 (out+1)
        int pitch;     // 0..127
    };
    std::vector<PendingNoteOff> pendingOffs;
    static constexpr size_t kMaxPendingOffs = 512;

    // Emit sink implementation (see builtin_synth.h IExprEmitSink). Routes the
    // program's emit calls into the output MIDI buffer, tagging each with
    // channel = out+1 so graph_processor's per-cable channel filters can split
    // the outputs. Defined in the .cpp.
    struct Sink;
    friend struct Sink;

    // Drain note-offs scheduled by note(p,v,d). samplesRemaining is measured
    // from the current block start; subtract the block length and emit any that
    // came due at their within-block offset. Survivors carry to the next block.
    void drainPendingNoteOffs(juce::MidiBuffer& midi, int numSamples);

    float getParam(int idx, float def) const;
};

// -----------------------------------------------------------------------------
// SignalShapeEditorComponent
// -----------------------------------------------------------------------------
//
// Modeless editor for a SignalShape node. Edits node.script in place via
// SignalShapeDoc::encode/decode and notifies the owner whenever the user
// mutates something (graph rebuild is typically what the caller does on
// onChanged).
//
// Layout (top -> bottom):
//   - Shape: an embedded LayerStackComponent - the same shared layered-
//     waveform editor the Wavetable editor uses. Add/remove layers with
//     "+ Layer"; each layer row is a sine/saw/square/triangle/noise/drawn/
//     freehand/formula shape at a harmonic ratio + phase + amp. A summation
//     preview at the bottom shows all layers summed (what the LFO actually
//     plays). A fresh node starts with zero layers (flat 0 output).
//   - Composition expression `expr`. Defaults to "curve" (passes the
//     waveform through unmodified).
//   - Trigger expression `triggerExpr`. Empty = always running.
//   - Repeat mode (radio buttons: Forever / Once / N times) + N spinner.
//   - Signal input count (0..16) - changes the node's input-pin count.
//   - Manual Trigger button.
//
// Pin reconciliation: when the user changes signalInputCount, the editor
// rewrites node.pinsIn (preserving the leading MIDI In pin, replacing the
// Signal s1..sN tail) and fires onChanged so the caller can rebuild the
// graph. Existing cables to deleted pins are dropped by the graph rebuild
// (orphan links are skipped in graph_processor::rebuildGraph).
class SignalShapeEditorComponent : public juce::Component,
                                   private juce::Timer {
public:
    // graph + nodeId: which SignalShape node we edit. We look up the live
    // Node by id on every access (never store a Node&) so vector
    // reallocation can't dangle our reference.
    //
    // onChanged: fired after every doc mutation. Caller usually rebuilds
    // the audio graph here and triggers a project-dirty flag.
    //
    // onManualTrigger: fired when the user clicks "Manual Trigger". Caller
    // looks up the live SignalShapeProcessor via GraphProcessor and calls
    // fireManualTrigger() on it. Optional - if null the button is hidden.
    SignalShapeEditorComponent(NodeGraph& graph, int nodeId,
                               std::function<void()> onChanged,
                               std::function<void()> onManualTrigger);
    ~SignalShapeEditorComponent() override;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    NodeGraph& graph;
    int nodeId;
    std::function<void()> onChanged;
    std::function<void()> onManualTrigger;

    // Working copy of the doc; encoded back to node.script on every change.
    SignalShapeDoc doc;

    // Language + execution-rate control strip and the .wasm file picker shown
    // in place of the program editor for the WebAssembly language.
    std::unique_ptr<ScriptLangBar> langBar;
    std::unique_ptr<WasmFilePanel> wasmPanel;
    // A contextual one-liner under the program editor explaining what the script
    // does in the current language/rate (e.g. "transforms curve each sample" vs
    // "fills the whole block - phase/repeat/curve are bypassed").
    juce::Label   scriptNote;
    // One-line red error strip shown only when the current program fails to
    // compile. Fed by validateScript() (a message-thread linter, debounced via
    // the Timer). Hidden while the script is clean.
    juce::Label   errorLabel;

    // Embedded layer-stack editor: the SAME shared widget the Wavetable
    // editor uses for its layer rows (+ Layer button, per-layer shape/ratio/
    // phase/amp/draw/formula rows) plus a summation preview. Bound to
    // &doc.layers; doc outlives the editor so the pointer stays stable.
    std::unique_ptr<LayerStackComponent> layerStack;

    juce::Label   exprLabel       { {}, "Output (expression of curve, etc.):" };
    juce::TextEditor exprEditor;
    juce::Label   triggerLabel    { {}, "Trigger (empty = always run):" };
    juce::TextEditor triggerEditor;

    juce::Label   repeatLabel     { {}, "Repeat:" };
    juce::TextButton foreverBtn   { "Forever" };
    juce::TextButton onceBtn      { "Once" };
    juce::TextButton nTimesBtn    { "N times" };
    juce::TextEditor repeatNEditor;

    juce::Label   sigCountLabel   { {}, "Signal inputs (s1..sN):" };
    juce::TextEditor sigCountEditor;

    // Unified-node I/O controls. continuousOutputCount (o1..oP) is always >=1;
    // midiOutputCount 0 = pure signal source; midiInEditor sets how many MIDI In
    // pins (0 = none, 1 = single "MIDI In", >1 = "MIDI In 1..N"); kindCombo flips
    // ALL continuous pins between Signal (blue) and Param (orange). All call
    // syncPins() then commitToNode() on change.
    juce::Label   outCountLabel   { {}, "Signal outputs (o1..oP):" };
    juce::TextEditor outCountEditor;
    juce::Label   midiOutLabel    { {}, "MIDI outputs:" };
    juce::TextEditor midiOutEditor;
    juce::Label   midiInLabel     { {}, "MIDI inputs:" };
    juce::TextEditor midiInEditor;
    juce::Label   kindLabel       { {}, "Pin type:" };
    juce::ComboBox kindCombo;

    // Speed controls. "Cycle length" and "Rate" are two views of the SAME
    // underlying node param ("Rate"): Rate = 1 / Cycle length. Editing either
    // field writes the Rate param and updates the other field to stay
    // reciprocal. The Beat Sync toggle (the node's "Beat Sync" param) chooses
    // the units shown: free-run -> Cycle length in seconds / Rate in Hz;
    // beat-synced -> Cycle length in beats / Rate in cycles-per-beat. These
    // controls edit node.params directly (not doc/script), since Rate and
    // Beat Sync are node params, not part of the SignalShapeDoc.
    juce::Label   speedLabel      { {}, "Speed:" };
    juce::Label   cycleCaption    { {}, "Cycle length" };
    juce::TextEditor cycleEditor;
    juce::Label   cycleUnit       { {}, "sec" };
    juce::Label   eqLabel         { {}, "=" };
    juce::Label   rateCaption     { {}, "Rate" };
    juce::TextEditor rateEditor;
    juce::Label   rateUnit        { {}, "Hz" };
    juce::ToggleButton beatSyncToggle { "Sync to beat" };

    // Phase source toggle: off = lock phase to song position (deterministic),
    // on = free-run off its own clock. Edits doc.freeRun. Disabled (and
    // irrelevant) while a Trigger expression is set.
    juce::ToggleButton freeRunToggle { "Free-run (ignore song position)" };

    juce::TextButton manualTriggerBtn { "Manual Trigger" };
    juce::TextButton closeBtn         { "Close" };

    // "Variables & functions…" reference. The full list used to sit in an
    // always-visible label that ate ~150px of vertical space the layer stack
    // badly needs; it now lives behind this button (and its tooltip) so the
    // shape editor gets that room back. Click pops the reference in a
    // main-window-parented message box.
    juce::TextButton varsHelpBtn { "Variables & functions..." };

    // Push `doc` into node.script (via SignalShapeDoc::encode) and notify.
    // Also reconciles node.pinsIn to match doc.signalInputCount when needed.
    void commitToNode();
    void loadFromNode();
    void timerCallback() override;
    // Compile the current program with a throwaway runtime and show/hide the
    // error strip. Cheap for Lua (luaL_loadstring), a no-op for Builtin, and
    // validates the .wasm path for WebAssembly. Debounced via the Timer.
    void validateScript();
    // Show the expression editor (Builtin / Lua) or the .wasm file panel (Wasm),
    // toggle multi-line for Lua programs, and update the caption + contextual
    // note to match the current language/rate.
    void updateLanguageUI();
    void refreshRepeatButtons();
    // Read the "Rate"/"Beat Sync" node params into the speed controls and
    // format the unit labels for the current sync mode. Does not overwrite a
    // field that currently has keyboard focus (so typing isn't disrupted).
    void refreshSpeedControls();
    // Clamp and write a new Rate into the "Rate" node param, then refresh both
    // speed fields and fire onChanged. `rate` is cycles/sec (free-run) or
    // cycles/beat (beat-synced) - same units the Rate field shows.
    void writeRate(float rate);
    // Pointer to a node param by name, or nullptr if the node/param is gone.
    Param* paramByName(const std::string& name);
    // Reconcile BOTH pin lists to the current doc:
    //   pinsIn  = [MIDI In pin(s), doc.midiInputCount of them] + s1..sN
    //             (ctrl kind = paramKind)
    //   pinsOut = o1..oP (ctrl kind) + MIDI Out 1..Q  (PinKind::Midi)
    // where N = signalInputCount, P = continuousOutputCount (>=1),
    // Q = midiOutputCount. Pin ids are preserved by name so existing cables
    // survive a count change; the continuous-pin kind follows doc.paramKind.
    // Returns true if either list actually changed (callers fire onChanged).
    bool syncPins();
};

} // namespace SoundShop

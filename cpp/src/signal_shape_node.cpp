#include "signal_shape_node.h"
#include "layered_wave_editor.h"
#include <juce_core/juce_core.h>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <functional>

namespace SoundShop {

// -----------------------------------------------------------------------------
// SignalShapeDoc encode / decode
// -----------------------------------------------------------------------------
//
// We Base64-encode `expr` and `triggerExpr` so that user-supplied math
// (which can contain '=', '\n', '|', etc.) cannot collide with our
// key=value framing. Integer fields and the layer encoding are written
// verbatim - integers can't break the format, and the layer encoding
// (which uses '|' internally) is kept on a single line of its own that
// the decoder reads as "everything after the leading 'layer=' on that
// line".

static const char* kSignalShapePrefix = "__signalshape__:v1";

static std::string b64(const std::string& s) {
    juce::MemoryBlock mb(s.data(), s.size());
    return mb.toBase64Encoding().toStdString();
}

static std::string unb64(const std::string& s) {
    juce::MemoryBlock mb;
    if (!mb.fromBase64Encoding(juce::String(s))) return {};
    return std::string((const char*)mb.getData(), mb.getSize());
}

bool SignalShapeDoc::isSignalShapeScript(const std::string& s) {
    return s.rfind("__signalshape__:", 0) == 0;
}

SignalShapeDoc SignalShapeDoc::defaultLFO() {
    SignalShapeDoc d;
    // No layers: the node outputs a flat 0 until the user adds one. We
    // deliberately do NOT seed a sine here (a new node must not sweep a
    // default shape). tableSize matches the processor's shape table so the
    // encoded body round-trips a sensible value.
    d.layers.layers.clear();
    d.layers.tableSize = 1024;
    d.expr = "curve";
    d.triggerExpr.clear();
    d.repeatMode = Forever;
    d.repeatN = 1;
    d.freeRun = false;   // default: lock phase to song position (deterministic)
    d.signalInputCount = 0;
    d.continuousOutputCount = 1;  // one continuous output (o1)
    d.midiOutputCount = 0;        // no MIDI emit by default (pure signal source)
    d.midiInputCount = 1;         // one MIDI input pin by default
    d.paramKind = false;
    return d;
}

std::string SignalShapeDoc::encode() const {
    std::ostringstream o;
    o << kSignalShapePrefix << "\n";
    o << "expr=" << b64(expr) << "\n";
    o << "trigger=" << b64(triggerExpr) << "\n";
    o << "repeat=" << (int)repeatMode << "\n";
    o << "repeatN=" << repeatN << "\n";
    o << "freeRun=" << (freeRun ? 1 : 0) << "\n";
    o << "sigCount=" << signalInputCount << "\n";
    o << "outCount=" << continuousOutputCount << "\n";
    o << "midiOut=" << midiOutputCount << "\n";
    o << "midiInCount=" << midiInputCount << "\n";
    o << "paramKind=" << (paramKind ? 1 : 0) << "\n";
    o << "lang=" << (int)language << "\n";
    o << "rate=" << (int)rate << "\n";
    o << "wasm=" << b64(wasmPath) << "\n";

    // Encode the whole layer stack via LayeredWaveform's encodeBody so the
    // full state lands on one line. An empty stack encodes as just the
    // tableSize, which decodes back to zero layers.
    o << "layer=" << layers.encodeBody();
    return o.str();
}

bool SignalShapeDoc::decode(const std::string& s) {
    *this = defaultLFO();
    if (s.rfind("__signalshape__:", 0) != 0) return false;

    auto lines = juce::StringArray::fromLines(juce::String(s));
    for (int i = 0; i < lines.size(); ++i) {
        auto line = lines[i].toStdString();
        if (i == 0) continue; // prefix line
        if (line.empty()) continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "expr") {
            expr = unb64(val);
            if (expr.empty()) expr = "curve";
        } else if (key == "trigger") {
            triggerExpr = unb64(val);
        } else if (key == "repeat") {
            int m = 0;
            try { m = std::stoi(val); } catch (...) {}
            repeatMode = (RepeatMode) juce::jlimit(0, 2, m);
        } else if (key == "repeatN") {
            try { repeatN = std::max(1, std::stoi(val)); } catch (...) {}
        } else if (key == "freeRun") {
            try { freeRun = std::stoi(val) != 0; } catch (...) {}
        } else if (key == "sigCount") {
            try { signalInputCount = juce::jlimit(0, 16, std::stoi(val)); } catch (...) {}
        } else if (key == "outCount") {
            try { continuousOutputCount = juce::jlimit(1, 16, std::stoi(val)); } catch (...) {}
        } else if (key == "midiOut") {
            try { midiOutputCount = juce::jlimit(0, 16, std::stoi(val)); } catch (...) {}
        } else if (key == "midiIn") {
            // Legacy boolean form (0/1). Map to a 0- or 1-pin count.
            try { midiInputCount = (std::stoi(val) != 0) ? 1 : 0; } catch (...) {}
        } else if (key == "midiInCount") {
            try { midiInputCount = juce::jlimit(0, 16, std::stoi(val)); } catch (...) {}
        } else if (key == "paramKind") {
            try { paramKind = std::stoi(val) != 0; } catch (...) {}
        } else if (key == "lang") {
            try { language = (ScriptLang)juce::jlimit(0, 2, std::stoi(val)); } catch (...) {}
        } else if (key == "rate") {
            try { rate = (ScriptRate)juce::jlimit(0, 1, std::stoi(val)); } catch (...) {}
        } else if (key == "wasm") {
            wasmPath = unb64(val);
        } else if (key == "layer") {
            // Decode the whole stack. Old single-layer scripts used the same
            // body encoding for their one layer, so they decode into a
            // one-element stack with no special-casing. decodeBody clears
            // `layers` first, so an empty body leaves zero layers.
            layers.decodeBody(val);
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
// SignalShapeProcessor
// -----------------------------------------------------------------------------

// Forward decl: defined below, but referenced by bindShape()'s shape lambda.
static float sampleShape(const std::vector<float>& tbl, float phase);

SignalShapeProcessor::SignalShapeProcessor(Node& n, Transport& t)
    : node(n), transport(t)
{
    reloadIfScriptChanged();
}

float SignalShapeProcessor::getParam(int idx, float def) const {
    if (idx >= 0 && idx < (int)node.params.size())
        return node.params[idx].value;
    return def;
}

void SignalShapeProcessor::reloadIfScriptChanged() {
    if (node.script == lastScriptSeen && !shapeSamples.empty()) return;
    lastScriptSeen = node.script;

    // Three accepted forms in node.script:
    //   1. "__signalshape__:v1\n..." - new format (decode in place).
    //   2. "__xypad__"               - XY Pad mode (handled in processBlock).
    //   3. Anything else             - legacy: treat as a wavetable expression
    //                                  driving a sine-style LFO. Convert by
    //                                  parking it in doc.layer.formulaExpr.
    if (SignalShapeDoc::isSignalShapeScript(node.script)) {
        doc.decode(node.script);
    } else if (node.script == "__xypad__"
               || node.script.rfind("__controlbank__", 0) == 0) {
        // No real shape to render; rebuildShapeSamples will fill with zeros
        // but the XY Pad / Control Bank paths in processBlock bypass shape
        // evaluation entirely.
        doc = SignalShapeDoc::defaultLFO();
    } else if (!node.script.empty()) {
        doc = SignalShapeDoc::defaultLFO();
        WaveLayer l;
        l.shape = WaveLayer::Formula;
        l.formulaExpr = node.script;
        doc.layers.layers.clear();
        doc.layers.layers.push_back(l);
    } else {
        doc = SignalShapeDoc::defaultLFO();
    }

    rebuildShapeSamples();
    // Reset per-trigger state so the next block doesn't play half-way
    // through the previous shape.
    phase = 0.0f;
    repeatsDone = 0;
    timeSinceTrigger = 0.0;
    prevTriggerHigh = false;
    running = doc.triggerExpr.empty(); // no trigger -> free-running
    // Reset MIDI emission state when the program changes (unified node).
    pendingOffs.clear();
    wasPlayingMidi = false;

    // (Re)create the language runtime. Effective rate from the capability matrix:
    // Builtin -> PerSample, Wasm -> PerBlock, Lua -> doc.rate.
    // Role is Unified: the program both assigns continuous outputs o1..oP AND
    // emits MIDI (the emit functions are no-ops when no sink / no MIDI output).
    ScriptRate effRate = doc.rate;
    if (!scriptLangSupportsRate(doc.language, effRate))
        effRate = (doc.language == ScriptLang::Wasm) ? ScriptRate::PerBlock
                                                     : ScriptRate::PerSample;
    runtime = makeScriptRuntime(doc.language, ScriptRole::Unified, effRate);
    if (!runtime) // language unavailable in this build -> fall back to Builtin.
        runtime = makeScriptRuntime(ScriptLang::Builtin, ScriptRole::Unified,
                                    ScriptRate::PerSample);
    bindShape();

    // Builtin / Lua run `expr` (the composition source); Wasm loads from the path.
    const std::string& src = (doc.language == ScriptLang::Wasm) ? doc.wasmPath
                                                                : doc.expr;
    std::string err;
    bool ok = runtime->load(src, err);
    // Latch the error state for the node-graph badge (read on the message
    // thread) and log the failure so there's a persistent record.
    scriptHasError.store(!ok, std::memory_order_relaxed);
    if (!ok)
        juce::Logger::writeToLog("Signal Shape \"" + juce::String(node.name)
                                 + "\": " + juce::String(err));
    runtime->reset();

    // A streaming program (defines stream()) owns its own sample loop and pulls
    // input one sample at a time via pull()/poll(), so it is inherently
    // sample-accurate regardless of the Run dropdown — the per-sample vs
    // per-block distinction is meaningless for it. Always drive it via the block
    // path (which hands the coroutine the whole block and lets it iterate), so
    // streaming works at "audio rate" as well as "block rate".
    runBlockMode = runtime->supportsPerBlock()
                   && (effRate == ScriptRate::PerBlock || !runtime->supportsPerSample()
                       || runtime->isStreaming());
}

void SignalShapeProcessor::bindShape() {
    if (!runtime) return;
    runtime->setShape([this](float pos) { return sampleShape(shapeSamples, pos); });
    // Bind notefreq()'s mapper to the project tuning so a Signal-role Lua script
    // that drives an oscillator at a note's pitch matches the rest of the project.
    runtime->setNoteToFreq([this](int n) { return transport.noteToFreq(n); });
}

void SignalShapeProcessor::rebuildShapeSamples() {
    // Render the layer stack into the shape table. render() produces a BIPOLAR
    // -1..1 waveform (same renderer the Wavetable instrument uses for audio),
    // but a control signal on the wire is UNIPOLAR 0..1 (see signal_modulation.h
    // for the convention). So we remap -1..1 -> 0..1 here, making the drawn
    // shape's height read directly as the output value: trough = 0, peak = 1.
    // This keeps `curve` (and the default "curve" expression) natively 0..1.
    //
    // With zero layers render() yields all zeros, which remap to a flat 0.5 -
    // the neutral "no modulation" resting level, exactly what a freshly-created
    // node should output until the user adds a layer.
    doc.layers.render(kShapeTableSize, shapeSamples);
    if ((int)shapeSamples.size() != kShapeTableSize) {
        // Defensive: render should always produce kShapeTableSize samples,
        // but make sure the per-sample sampler can't index OOB if it didn't.
        shapeSamples.assign(kShapeTableSize, 0.5f);
    } else {
        for (float& v : shapeSamples) v = v * 0.5f + 0.5f;
    }
}

// Sample the pre-rendered shape table at phase in [0,1) with linear
// interpolation. Wraps modulo 1.
static float sampleShape(const std::vector<float>& tbl, float phase) {
    if (tbl.empty()) return 0.0f;
    phase -= std::floor(phase);
    int n = (int)tbl.size();
    float f = phase * (float)n;
    int i0 = (int)f;
    if (i0 < 0) i0 = 0;
    if (i0 >= n) i0 = n - 1;
    int i1 = (i0 + 1) % n;
    float t = f - (float)i0;
    return tbl[i0] * (1.0f - t) + tbl[i1] * t;
}

// -----------------------------------------------------------------------------
// SignalShapeProcessor::Sink - MIDI emit interface for the unified node.
// -----------------------------------------------------------------------------
// Ported from the retired MIDI Script node. Emit calls land at `sampleOffset`
// (the per-sample loop's current sample) in the OUTPUT MIDI buffer; events are
// tagged channel = out+1 so graph_processor's per-cable channel filters split
// the multiple MIDI outputs into independent single-stream cables.
struct SignalShapeProcessor::Sink : public IExprEmitSink {
    juce::MidiBuffer* midi = nullptr;
    int sampleOffset = 0;
    double sampleRate = 44100.0;
    int outputCount = 1;
    std::vector<PendingNoteOff>* pendingOffs = nullptr;

    int channelFor(int out) const {
        int idx = juce::jlimit(0, juce::jmax(0, outputCount - 1), out);
        return juce::jlimit(1, 16, idx + 1);
    }
    static int clampNote(float v) { return juce::jlimit(0, 127, (int)std::lround(v)); }

    void setSampleOffset(int off) override { sampleOffset = off; }

    void emitNote(int out, float pitch, float vel, float durSec) override {
        if (!midi) return;
        int ch = channelFor(out);
        int p  = clampNote(pitch);
        int v  = clampNote(vel);
        if (v <= 0) return;
        midi->addEvent(juce::MidiMessage::noteOn(ch, p, (juce::uint8)v), sampleOffset);
        if (pendingOffs && pendingOffs->size() < kMaxPendingOffs) {
            long long durSamples = std::max<long long>(1,
                (long long)std::lround((double)durSec * sampleRate));
            pendingOffs->push_back({ (long long)sampleOffset + durSamples, ch, p });
        }
    }
    void emitNoteOn(int out, float pitch, float vel) override {
        if (!midi) return;
        int v = clampNote(vel);
        if (v <= 0) { emitNoteOff(out, pitch); return; }
        midi->addEvent(juce::MidiMessage::noteOn(channelFor(out), clampNote(pitch),
                                                 (juce::uint8)v), sampleOffset);
    }
    void emitNoteOff(int out, float pitch) override {
        if (!midi) return;
        midi->addEvent(juce::MidiMessage::noteOff(channelFor(out), clampNote(pitch)),
                       sampleOffset);
    }
    void emitCC(int out, float num, float value01) override {
        if (!midi) return;
        int n = juce::jlimit(0, 127, (int)std::lround(num));
        int v = juce::jlimit(0, 127, (int)std::lround(juce::jlimit(0.0f, 1.0f, value01) * 127.0f));
        midi->addEvent(juce::MidiMessage::controllerEvent(channelFor(out), n, v), sampleOffset);
    }
    void emitBend(int out, float value) override {
        if (!midi) return;
        int w = juce::jlimit(0, 16383, (int)std::lround((juce::jlimit(-1.0f, 1.0f, value) * 0.5f + 0.5f) * 16383.0f));
        midi->addEvent(juce::MidiMessage::pitchWheel(channelFor(out), w), sampleOffset);
    }
};

void SignalShapeProcessor::drainPendingNoteOffs(juce::MidiBuffer& midi, int numSamples) {
    for (auto it = pendingOffs.begin(); it != pendingOffs.end();) {
        it->samplesRemaining -= numSamples;
        if (it->samplesRemaining <= 0) {
            int off = juce::jlimit(0, juce::jmax(0, numSamples - 1),
                (int)(numSamples + it->samplesRemaining));
            midi.addEvent(juce::MidiMessage::noteOff(it->channel, it->pitch), off);
            it = pendingOffs.erase(it);
        } else ++it;
    }
}

void SignalShapeProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) {
    int numSamples = buf.getNumSamples();
    int numCh = buf.getNumChannels();

    // XY Pad mode - same NodeType, different node, identified by the
    // "__xypad__" script tag. Three Signal output pins (X, Y, Z) driven
    // by params 0/1/2. Bypasses all shape/trigger/expression machinery.
    if (node.script == "__xypad__") {
        buf.clear();
        const int kNumOuts = 3;
        for (int i = 0; i < kNumOuts; ++i) {
            int ch = 2 + i;
            if (ch >= numCh) break;
            float v = juce::jlimit(0.0f, 1.0f, getParam(i, 0.5f));
            auto* out = buf.getWritePointer(ch);
            for (int s = 0; s < numSamples; ++s) out[s] = v;
        }
        return;
    }

    // Control Bank mode - one Signal output per slider, identified by the
    // "__controlbank__" script tag (a trailing ":h" only flags horizontal UI
    // layout). Each output channel 2+i is driven by param i (the slider value,
    // 0..1). Like XY Pad, this bypasses all shape/trigger machinery.
    if (node.script.rfind("__controlbank__", 0) == 0) {
        buf.clear();
        const int numOuts = (int)node.params.size();
        for (int i = 0; i < numOuts; ++i) {
            int ch = 2 + i;
            if (ch >= numCh) break;
            float v = juce::jlimit(0.0f, 1.0f, getParam(i, 0.5f));
            auto* out = buf.getWritePointer(ch);
            for (int s = 0; s < numSamples; ++s) out[s] = v;
        }
        return;
    }

    // Snapshot the incoming control-input channels BEFORE clearing the buffer.
    // Input and output channels share storage in the JUCE graph, and we clear
    // the buffer below before writing our outputs, so s1..sN must be captured
    // first or they would read as zeros (the terrain-synth control-input bug).
    int numSigInPins = 0;
    for (auto& pin : node.pinsIn)
        if (pin.kind == PinKind::Signal || pin.kind == PinKind::Param) ++numSigInPins;
    ctrlIn.setSize(juce::jmax(0, numSigInPins), juce::jmax(1, numSamples),
                   false, false, true);
    for (int i = 0; i < numSigInPins; ++i) {
        int ch = 2 + i;
        if (ch < numCh) ctrlIn.copyFrom(i, 0, buf, ch, 0, numSamples);
        else            ctrlIn.clear(i, 0, numSamples);
    }

    buf.clear();

    reloadIfScriptChanged();

    // ---- Read node params (these are the *node-level* params editable on
    //      the node UI, not the editor doc's per-cycle expressions) ----
    //
    // Param layout (matches node_graph_component.cpp Signal Shape creation):
    //   0 = Rate (cycles per second, or cycles per beat when Beat Sync = 1)
    //   1 = Beat Sync (0/1)
    //   2 = Phase offset (0..1, added before evaluating each sample)
    //   3 = Output (read-only mirror of the last computed sample)
    float rate         = getParam(0, 1.0f);
    bool  beatSync     = (int)getParam(1, 0.0f) != 0;
    float phaseOffset  = getParam(2, 0.0f);

    // ---- Process MIDI events for gate / freq / note / vel ----------------
    for (auto meta : midi) {
        auto msg = meta.getMessage();
        if (msg.isNoteOn()) {
            notesHeld++;
            lastNoteOn = msg.getNoteNumber();
            lastVelocity = (float)msg.getVelocity() / 127.0f;
        } else if (msg.isNoteOff()) {
            if (notesHeld > 0) notesHeld--;
        }
    }
    // Capture the block's input events for event-driven block-mode scripts
    // (midiin()/midievent()). Done here, before the emitsMidi branch may clear
    // `midi`. Only needed when the script owns the block.
    if (runBlockMode)
        buildScriptMidiIn(midi, numSamples, midiInEvents, doc.midiInputCount);

    // ---- Phase advance per sample ----------------------------------------
    // beatSync interpretation: rate=1 means "one cycle per beat".
    // Free-run interpretation: rate=1 means "one cycle per second" (1 Hz).
    float phaseDelta;
    if (beatSync) {
        float beatsPerSample = (float)(transport.bpm / (60.0 * sampleRate));
        phaseDelta = rate * beatsPerSample;
    } else {
        phaseDelta = rate / (float)sampleRate;
    }

    // Acquire grab-once snapshot of expressions and signal-input pointers
    // so we don't re-fetch them every sample.
    const std::string& expr        = doc.expr;
    const std::string& triggerExpr = doc.triggerExpr;
    const bool freeRunning         = triggerExpr.empty();
    const int sigCount             = doc.signalInputCount;
    auto repeatMode                = doc.repeatMode;
    const int repeatN              = doc.repeatN;

    // Transport-synced phase: a non-triggered (LFO) shape derives its phase
    // directly from the song position unless the user opted into free-run. This
    // makes the shape deterministic - the same song position always yields the
    // same phase, so what you hear matches the exported audio and replaying a
    // section is identical every time - and it freezes the shape while the
    // transport is stopped. A triggered shape is already deterministic relative
    // to its trigger, so it never uses this path (and freeRun is ignored there).
    const bool transportSync       = freeRunning && !doc.freeRun;

    // Cache pointers to the signal-input channels (Param/Signal pins map
    // to channels 2+ in pin declaration order; we don't distinguish kinds
    // here since the upstream cable carries the same data either way).
    // sigCount of 0 leaves this empty - the s1..sN variables then resolve
    // to 0 via the parser's "unknown identifier" fallback.
    // Read signal inputs from the pre-clear snapshot (ctrlIn), NOT the live
    // buffer (which we just cleared and will overwrite with outputs).
    std::vector<const float*> sigChans;
    sigChans.reserve(sigCount);
    for (int i = 0; i < sigCount; ++i) {
        sigChans.push_back((i < ctrlIn.getNumChannels()) ? ctrlIn.getReadPointer(i)
                                                         : nullptr);
    }

    // Output channels: every Signal/Param OUT pin maps to channel 2+ in pin
    // declaration order on the OUTPUT side of the buffer. The JUCE
    // AudioProcessorGraph model uses ONE buffer per node whose channel
    // indices are shared between input and output reads/writes - so if
    // this node has 2 Signal IN pins and 2 Signal/Param OUT pins, channels
    // 2 and 3 are simultaneously "s1 / s2 input" (on the way in) and
    // "Param Out / Signal Out" (on the way out).
    //
    // That overlap is safe as long as we read each input sample BEFORE
    // overwriting its channel. Our per-sample loop already does this:
    // it reads sigChans[i][s] into the vars map at the top of the
    // iteration, then writes to buf.getWritePointer(ch)[s] at the bottom.
    // The next iteration reads the still-untouched index s+1.
    //
    // numOutCtrlPins counts ACTUAL Signal/Param output pins so we don't
    // write into channels that aren't output pins (XY Pad path above uses
    // the same convention - writes to channels 2..2+kNumOuts-1).
    const int outChStart = 2;
    int numOutCtrlPins = 0;
    for (auto& pin : node.pinsOut)
        if (pin.kind == PinKind::Signal || pin.kind == PinKind::Param)
            ++numOutCtrlPins;
    const int outChEnd = std::min(numCh, outChStart + numOutCtrlPins);

    // Transport play rising edge -> run the language's start hook (Lua start()).
    const bool playStart = transport.playing && !wasPlaying;
    wasPlaying = transport.playing;

    // ---- MIDI emission setup (unified node) ------------------------------
    // When the program declares >=1 MIDI output pin, this node OWNS the MIDI
    // buffer as its output: we read the input note state above (the loop that
    // updates note/vel/gate), then clear it and write only our emitted events.
    // With 0 MIDI outputs the node never touches `midi` (it's a pure signal
    // source). The Sink tags events channel = out+1 for graph_processor's
    // per-cable channel filters.
    const int  midiOutCount = doc.midiOutputCount;
    const bool emitsMidi    = midiOutCount > 0;
    Sink sink;
    sink.midi        = &midi;
    sink.sampleRate  = sampleRate;
    sink.outputCount = midiOutCount;
    sink.pendingOffs = &pendingOffs;
    IExprEmitSink* sinkPtr = emitsMidi ? &sink : nullptr;

    if (emitsMidi) {
        const bool playStopMidi = !transport.playing && wasPlayingMidi;
        midi.clear();  // we don't pass input MIDI through; it only drove note/vel/gate
        if (playStopMidi) {
            for (int ch = 1; ch <= 16; ++ch)
                midi.addEvent(juce::MidiMessage::allNotesOff(ch), 0);
            pendingOffs.clear();
        }
    }
    wasPlayingMidi = transport.playing;

    // Ensure the per-output hold buffer matches the declared output count.
    const int outCount = juce::jmax(1, doc.continuousOutputCount);
    if ((int)lastOuts.size() != outCount)
        lastOuts.assign((size_t)outCount, 0.5f);

    // ---- Per-block dispatch (Lua block-rate / Wasm) ----------------------
    // Raw-signal mode: the script fills the whole output buffer itself. The
    // trigger / phase / repeat / curve machinery below is bypassed entirely.
    if (runBlockMode && runtime) {
        if ((int)blockOut.size() < numSamples)
            blockOut.assign((size_t)juce::jmax(1, numSamples), 0.0f);

        const float freqHz = (lastNoteOn >= 0)
                           ? 440.0f * std::pow(2.0f, (lastNoteOn - 69) / 12.0f) : 0.0f;
        if (playStart) {
            ScriptVars sv;
            sv["bpm"]  = (float)transport.bpm;
            sv["note"] = (float)lastNoteOn;
            sv["vel"]  = lastVelocity;
            sv["gate"] = (notesHeld > 0) ? 1.0f : 0.0f;
            sv["freq"] = freqHz;
            sv["rate"] = rate;
            sink.sampleOffset = 0;
            runtime->onStart(sv, sinkPtr); // emit allowed when MIDI outputs exist
        }

        ScriptBlockCtx ctx;
        ctx.sink       = sinkPtr;     // block-mode MIDI emit (Lua-block / Wasm)
        ctx.sampleRate = juce::jmax(1.0, sampleRate);
        ctx.numSamples = numSamples;
        ctx.bpm        = transport.bpm;
        ctx.playing    = transport.playing;
        ctx.posSamples = transport.positionSamples;
        ctx.posBeats   = transport.positionBeats();
        ctx.note       = lastNoteOn;
        ctx.vel        = lastVelocity;
        ctx.gate       = (notesHeld > 0) ? 1.0f : 0.0f;
        ctx.freq       = freqHz;
        ctx.rate       = rate;
        ctx.sig        = &sigChans;
        ctx.sigCount   = sigCount;
        ctx.midiIn     = &midiInEvents;
        ctx.midiInCount = (int)midiInEvents.size();
        ctx.out        = blockOut.data();

        // Streaming programs drive each continuous output o1..oP independently via
        // out(pin,v), so hand the runtime P distinct buffers (one per output pin).
        // Legacy block loop()/Wasm fills only ctx.out (pin 0), which we fan to all
        // pins below. We size to the number of REAL output channels so we never
        // allocate buffers for pins that don't exist on this node.
        const bool streamingNode = runtime->isStreaming();
        const int  numStreamOuts = juce::jmax(1, outChEnd - outChStart);
        if (streamingNode) {
            if ((int)outBufs.size() != numStreamOuts)
                outBufs.assign((size_t)numStreamOuts, std::vector<float>());
            outPtrs.assign((size_t)numStreamOuts, nullptr);
            for (int p = 0; p < numStreamOuts; ++p) {
                if ((int)outBufs[(size_t)p].size() < numSamples)
                    outBufs[(size_t)p].assign((size_t)juce::jmax(1, numSamples), 0.0f);
                else
                    std::fill_n(outBufs[(size_t)p].data(), numSamples, 0.0f);
                outPtrs[(size_t)p] = outBufs[(size_t)p].data();
            }
            ctx.outs     = &outPtrs;
            ctx.outCount = numStreamOuts;
            ctx.out      = outPtrs[0];    // out(v) with no pin -> pin 0
        }

        runtime->runBlock(ctx);

        if (streamingNode) {
            // Route each output pin's buffer to its channel; no fan-out.
            for (int p = 0; p < numStreamOuts; ++p) {
                int ch = outChStart + p;
                if (ch < outChEnd)
                    std::memcpy(buf.getWritePointer(ch), outPtrs[(size_t)p],
                                sizeof(float) * (size_t)numSamples);
            }
            lastOutputValue = (numSamples > 0) ? outPtrs[0][numSamples - 1]
                                               : lastOutputValue;
        } else {
            // Legacy block loop()/Wasm produces ONE continuous signal; fan it to
            // every Signal/Param output channel.
            for (int ch = outChStart; ch < outChEnd; ++ch)
                std::memcpy(buf.getWritePointer(ch), blockOut.data(),
                            sizeof(float) * (size_t)numSamples);
            lastOutputValue = (numSamples > 0) ? blockOut[numSamples - 1]
                                               : lastOutputValue;
        }
        if ((int)node.params.size() > 3)
            node.params[3].value = lastOutputValue;

        drainPendingNoteOffs(midi, numSamples);
        return;
    }

    // ---- Per-sample loop -------------------------------------------------
    bool fireManualNow = manualTriggerPending.exchange(false);

    // Per-sample start hook (Builtin start: / Lua start()) on the play edge.
    if (playStart && runtime) {
        ScriptVars sv;
        sv["bpm"]  = (float)transport.bpm;
        sv["note"] = (float)lastNoteOn;
        sv["vel"]  = lastVelocity;
        sv["gate"] = (notesHeld > 0) ? 1.0f : 0.0f;
        sv["freq"] = (lastNoteOn >= 0)
                   ? 440.0f * std::pow(2.0f, (lastNoteOn - 69) / 12.0f) : 0.0f;
        sv["rate"] = rate;
        sink.sampleOffset = 0;
        runtime->onStart(sv, sinkPtr);
    }

    // shape(pos) sampler: lets expressions resample the drawn waveform at an
    // arbitrary position (input-driven lookup / waveshaping), independent of
    // the phase-locked `curve` variable. `pos` is treated as a 0..1 phase that
    // wraps - sampleShape() handles the modulo. Built once per block (cheap),
    // referenced by the parser only when the expression actually calls shape().
    const std::vector<float>& tbl = shapeSamples;
    std::function<float(float)> shapeFn =
        [&tbl](float pos) { return sampleShape(tbl, pos); };

    std::unordered_map<std::string, float> vars;
    vars.reserve(16 + sigCount);

    for (int s = 0; s < numSamples; ++s) {
        // Transport-synced phase: derive `phase` (and the run/repeat state) as a
        // pure function of the song position for this sample, instead of
        // integrating it across blocks. Deterministic, and frozen when stopped.
        if (transportSync) {
            double cyclePos;
            if (beatSync) {
                double beatAtSample = transport.positionBeats()
                                    + transport.bpm / (60.0 * sampleRate) * s;
                cyclePos = beatAtSample * rate;          // rate = cycles per beat
            } else {
                double secAtSample = (double)(transport.positionSamples + s)
                                   / juce::jmax(1.0, transport.sampleRate);
                cyclePos = secAtSample * rate;           // rate = cycles per second
            }
            if (cyclePos < 0.0) cyclePos = 0.0;

            // Honour finite repeats: after the allowed number of cycles, hold at
            // the very end of the last cycle (deterministically, regardless of
            // where playback started).
            double maxCycles = (repeatMode == SignalShapeDoc::Once)   ? 1.0
                             : (repeatMode == SignalShapeDoc::NTimes) ? (double)repeatN
                             : std::numeric_limits<double>::infinity();
            if (cyclePos >= maxCycles)
                cyclePos = std::nextafter(maxCycles, 0.0);

            phase            = (float)(cyclePos - std::floor(cyclePos));
            repeatsDone      = (int)std::floor(cyclePos);
            running          = true;   // output is a pure function of phase here
            timeSinceTrigger = (double)(transport.positionSamples + s)
                             / juce::jmax(1.0, transport.sampleRate);
        }

        // Build variable bindings.
        float curPhase = phase + phaseOffset;
        curPhase -= std::floor(curPhase);
        float curve = sampleShape(shapeSamples, curPhase);

        float freqHz = (lastNoteOn >= 0)
                       ? 440.0f * std::pow(2.0f, (lastNoteOn - 69) / 12.0f)
                       : 0.0f;

        vars["curve"] = curve;
        vars["x"]     = curPhase;
        vars["phase"] = curPhase;
        vars["t"]     = (float)timeSinceTrigger;
        vars["beat"]  = (float)(transport.positionBeats()
                                + transport.bpm / (60.0 * sampleRate) * s);
        vars["bpm"]   = (float)transport.bpm;
        vars["gate"]  = (notesHeld > 0) ? 1.0f : 0.0f;
        vars["freq"]  = freqHz;
        vars["note"]  = (float)lastNoteOn;
        vars["vel"]   = lastVelocity;
        vars["rep"]   = (float)repeatsDone;
        vars["rate"]  = rate;
        for (int i = 0; i < sigCount; ++i) {
            float v = sigChans[i] ? sigChans[i][s] : 0.0f;
            vars["s" + std::to_string(i + 1)] = v;
        }

        // Trigger detection and per-sample phase integration apply only when we
        // are NOT deriving phase from the transport. In transport-synced mode
        // `phase`, `running` and `repeatsDone` were already set deterministically
        // above, so we leave them alone (shouldAdvance stays false).
        bool shouldAdvance = false;
        if (!transportSync) {
            // Evaluate trigger expression (if any) for edge detection.
            bool triggerHigh;
            if (freeRunning) {
                triggerHigh = true;
            } else {
                float trig = WaveExprParser::evaluateWithVars(triggerExpr, vars, shapeFn);
                triggerHigh = (trig > 0.0f);
            }

            // Rising edge OR manual trigger fires the run cycle.
            bool risingEdge = (triggerHigh && !prevTriggerHigh);
            if (fireManualNow && s == 0) { risingEdge = true; fireManualNow = false; }
            prevTriggerHigh = triggerHigh;

            if (risingEdge) {
                phase = 0.0f;
                repeatsDone = 0;
                timeSinceTrigger = 0.0;
                running = true;
                // Re-evaluate curve at the freshly-reset phase so this sample
                // outputs from the start of the cycle, not the previous tail.
                curPhase = phaseOffset - std::floor(phaseOffset);
                vars["x"]     = curPhase;
                vars["phase"] = curPhase;
                vars["curve"] = sampleShape(shapeSamples, curPhase);
            }

            // For free-running mode we don't need triggerHigh to keep going;
            // for trigger-gated mode we stop advancing once trigger goes low
            // (but only if we haven't already finished a finite-repeat cycle).
            shouldAdvance = freeRunning ? true : triggerHigh;
        }

        if (running) {
            // Run the unified program for this sample: it emits MIDI through
            // `sinkPtr` (when the node has MIDI outputs) AND assigns continuous
            // outputs o1..oP, harvested into outsBuf. For Builtin this runs the
            // multi-statement program (the default "curve" yields o1 = curve);
            // for Lua-per-sample it runs loop(). The trigger expression above is
            // always the Builtin DSL.
            if (sinkPtr) sink.sampleOffset = s;
            float outsBuf[16];
            const int p = juce::jmin(outCount, 16);
            if (runtime) {
                runtime->runUnified(vars, sinkPtr, outsBuf, p);
            } else {
                outsBuf[0] = WaveExprParser::evaluateWithVars(expr, vars, shapeFn);
                for (int i = 1; i < p; ++i) outsBuf[i] = 0.0f;
            }
            // Control signals are unipolar 0..1 on the wire (see
            // signal_modulation.h). `curve` is already 0..1; bipolar math like a
            // bare sin() clips its negative half unless wrapped (unipolar/usin).
            for (int i = 0; i < p; ++i)
                lastOuts[i] = juce::jlimit(0.0f, 1.0f, outsBuf[i]);

            if (shouldAdvance) {
                phase += phaseDelta;
                timeSinceTrigger += 1.0 / sampleRate;
                if (phase >= 1.0f) {
                    phase -= std::floor(phase);
                    repeatsDone++;
                    // End-of-cycle termination logic.
                    if (repeatMode == SignalShapeDoc::Once && repeatsDone >= 1) {
                        running = false;
                    } else if (repeatMode == SignalShapeDoc::NTimes
                               && repeatsDone >= repeatN) {
                        running = false;
                    }
                }
            }
        }
        // else: hold the last computed values (envelope "stuck at the end" for
        // Once / NTimes), and do NOT run the program (no MIDI emit while held).

        lastOutputValue = lastOuts.empty() ? 0.5f : lastOuts[0];

        // Write each continuous output o1..oP to its own channel (2+i).
        // outChStart..outChEnd may overlap input channels, but inputs were
        // snapshotted into ctrlIn before the buffer was cleared, so the
        // overwrite is safe.
        for (int ch = outChStart; ch < outChEnd; ++ch) {
            int oi = ch - outChStart;
            buf.getWritePointer(ch)[s] = (oi < (int)lastOuts.size()) ? lastOuts[oi]
                                                                     : lastOuts.empty() ? 0.5f
                                                                                        : lastOuts[0];
        }
    }

    drainPendingNoteOffs(midi, numSamples);

    // Reflect the last computed value into the node's "Output" param so
    // UI-rate consumers (e.g. param-slider feedback) can display it.
    if ((int)node.params.size() > 3)
        node.params[3].value = lastOutputValue;
}

// =============================================================================
// SignalShapeEditorComponent
// =============================================================================

namespace {
// Multi-line tooltip / help text shown in the editor. Sticks to the
// vocabulary documented at the top of signal_shape_node.h so users have
// one place to learn what's bound. Kept short - long-form docs live in
// the Help menu / REFERENCE.md.
const char* kSignalShapeHelp =
    "Output is a control signal in the range 0..1 (0.5 = neutral / no change).\n"
    "Variables:\n"
    "  curve = the drawn waveform's height at the current phase, as 0..1.\n"
    "    (The default expression is just \"curve\" - plays the drawn shape as-is.)\n"
    "  x = phase 0..1, t = seconds since trigger, beat, bpm,\n"
    "  gate = MIDI note held 0/1, freq = held-note Hz, note = MIDI #, vel = velocity 0..1,\n"
    "  rep = repeats done, rate = Rate param, s1..sN = signal inputs.\n"
    "Functions: sin cos tan abs sqrt exp log pow tanh saw square triangle noise,\n"
    "  floor ceil min(a,b) max(a,b) clamp(v,lo,hi) if(c,a,b), <  >  <=  >=  ==  !=  && || !, c?a:b.\n"
    "  shape(pos) samples the drawn waveform at pos (0..1 phase, wraps) - e.g.\n"
    "  shape(s1) reads the drawing at a position set by input s1.\n"
    "Heads up: sin/cos/tan (and saw/square/triangle/noise) return -1..1, so a bare\n"
    "  sin(...) clips its negative half on the 0..1 output. Use the unipolar forms\n"
    "  usin/ucos/utan (or usaw/usquare/utriangle), or wrap a whole bipolar\n"
    "  expression in unipolar(...): e.g. unipolar(sin(x*6.28)*0.5 + ...). bipolar(x)\n"
    "  does the reverse (0..1 -> -1..1). Output clamped to [0, 1].";

const char* kSignalShapeLuaHelp =
    "Signal Shape (Lua) generates the control signal with an embedded Lua 5.4\n"
    "program.\n"
    "\n"
    "Program structure (define ONE of loop / stream as the body):\n"
    "  - Top-level code runs ONCE at load (globals persist).\n"
    "  - function start()  optional - runs once when playback starts.\n"
    "  - function loop()   per-sample or per-block body.\n"
    "  - function stream() streaming body: own the loop, pull input / push output.\n"
    "\n"
    "Output is a control signal in 0..1 (0.5 = neutral / no change).\n"
    "Per sample: loop() RETURNS the output value (0..1) for this sample. The\n"
    "  built-in machinery still runs, so you can read `curve` (the drawn shape at\n"
    "  the current phase, as 0..1) plus x/phase, t, beat, bpm, gate, freq, note,\n"
    "  vel, rep, rate, s1..sN, and shape(pos).\n"
    "  Example - a tremolo that follows the drawing (stays in 0..1):\n"
    "    function loop() return curve * (0.5 + 0.5*sin(t*6.28*5)) end\n"
    "\n"
    "Per block (raw mode): loop() fills the whole block itself with out(i, value),\n"
    "  i = 0..n-1. The phase/repeat/curve machinery is bypassed; use shape(pos)\n"
    "  and sig(k, i) to read the drawing and inputs. Variables: n, sr, dt, bpm,\n"
    "  rate, t/tStart/tEnd, beat/beatStart/beatEnd, note, vel, gate, freq, s1..sN.\n"
    "  One interpreter call per block (scales to many instances).\n"
    "  Example - a 5 Hz sine LFO, sample-accurate (usin keeps it in 0..1):\n"
    "    function loop()\n"
    "      for i = 0, n-1 do\n"
    "        local tt = tStart + i*dt\n"
    "        out(i, usin(tt*6.28318*5))\n"
    "      end\n"
    "    end\n"
    "\n"
    "Streaming (function stream()): the program OWNS its loop and runs forever,\n"
    "  pulling input and pushing output at its own pace. It is started once (off\n"
    "  the audio thread) and AUTOMATICALLY SUSPENDS when it runs out of input for\n"
    "  the current block, resuming in the next block with its local variables\n"
    "  intact - so you never have to think in blocks. The SAME loop works at audio\n"
    "  rate (pull one sample) or block rate (pullblock the whole block); the Run\n"
    "  dropdown is ignored for streaming. Pull/push API:\n"
    "    pull()       next input sample(s); BLOCKS (suspends) at the block end.\n"
    "                 Returns one value per signal-in pin: local a,b = pull().\n"
    "    poll()       like pull() but NON-blocking: returns nil if none left now.\n"
    "    wait()       suspend until the next block (the hand-back for poll loops).\n"
    "    out(v)       write the current sample to output o1; out(n,v) writes pin n\n"
    "                 (1-based o1,o2,..) so a node can drive many outputs at once.\n"
    "    pullblock(p) grab input pin p's WHOLE block as an array (block-rate); blocks.\n"
    "    pullblock()  no arg -> params,events: params[k] is pin k's sample array\n"
    "                 (one list per input pin), events is a list of MIDI-in event\n"
    "                 tables {kind,offset,a,b,idx}. Blocks at the block end.\n"
    "    pollblock([p]) the remaining input (same two shapes), or nil; non-blocking.\n"
    "    outblock([n,]t) write array t to output pin n (1-based; o1 default).\n"
    "    pollmidi()   next MIDI-input event the cursor has reached, or nil:\n"
    "                 kind,offset,a,b,idx (idx = 1-based MIDI-in pin; idx is LAST so\n"
    "                 local kind,off,a,b = pollmidi() still works).\n"
    "  Example - a one-pole low-pass that keeps state across blocks for free:\n"
    "    function stream()\n"
    "      local y = 0\n"
    "      while true do\n"
    "        local x = pull()\n"
    "        y = y + 0.05*(x - y)\n"
    "        out(y)\n"
    "      end\n"
    "    end\n"
    "  Example - event-driven gate (note held -> 1, else 0):\n"
    "    function stream()\n"
    "      local held = 0\n"
    "      while true do\n"
    "        pull()\n"
    "        local k = pollmidi()\n"
    "        while k do\n"
    "          if k=='on' then held=held+1 elseif k=='off' then held=math.max(0,held-1) end\n"
    "          k = pollmidi()\n"
    "        end\n"
    "        out(held>0 and 1 or 0)\n"
    "      end\n"
    "    end\n"
    "\n"
    "Event-driven MIDI input (per block): midiin() returns how many MIDI events\n"
    "  arrived this block; midievent(k) (k = 1..midiin()) returns kind,offset,a,b,idx\n"
    "  (idx = 1-based MIDI-in pin; idx is LAST so local kind,off,a,b = midievent(k)\n"
    "  still works):\n"
    "    kind 'on'  -> a = note, b = velocity 0..1\n"
    "    kind 'off' -> a = note\n"
    "    kind 'cc'  -> a = controller#, b = value 0..1\n"
    "    kind 'bend'-> b = -1..1 (0 = centre)\n"
    "  offset is the sample within the block (for sample-accurate reactions).\n"
    "  Example - gate the output when any note is held:\n"
    "    function loop()\n"
    "      for k = 1, midiin() do\n"
    "        local kind,off,a,b = midievent(k)\n"
    "        if kind=='on' then held=(held or 0)+1\n"
    "        elseif kind=='off' then held=math.max(0,(held or 0)-1) end\n"
    "      end\n"
    "      for i = 0, n-1 do out(i, (held or 0)>0 and 1 or 0) end\n"
    "    end\n"
    "\n"
    "Heads up: sin/cos/tan return -1..1, so a bare sin() clips its negative half\n"
    "  on the 0..1 output. Use usin/ucos/utan (or usaw/usquare/utriangle), or wrap\n"
    "  a bipolar expression in unipolar(...); bipolar(x) does the reverse.\n"
    "Maths: the math.* library plus aliases sin cos tan sqrt abs exp log floor\n"
    "  ceil min max pow(a,b) clamp(x,lo,hi) saw square triangle noise() pi,\n"
    "  unipolar bipolar usin ucos utan usaw usquare utriangle.\n"
    "Sandboxed: no io/os/package/debug, no require/load/dofile.";
} // namespace

SignalShapeEditorComponent::SignalShapeEditorComponent(NodeGraph& g, int nid,
                                                       std::function<void()> ch,
                                                       std::function<void()> mt)
    : graph(g), nodeId(nid), onChanged(std::move(ch)),
      onManualTrigger(std::move(mt))
{
    // NOTE: setSize() is deliberately called at the END of this constructor,
    // not here. setSize() triggers resized() synchronously, and resized()
    // lays out shapeEditor (a unique_ptr child). If we sized the component
    // before creating shapeEditor, that first resized() would run with
    // shapeEditor == null, the `if (shapeEditor)` guard would skip reserving
    // its row, and — because the host DialogWindow shows the content at the
    // same size and never re-triggers resized() — the shape editor would
    // stay at 0x0 and never appear. Sizing last guarantees the layout pass
    // sees every child.

    // Pull current state from node.script (decode or seed defaults).
    loadFromNode();

    // Language + execution-rate control strip.
    {
        ScriptLangBar::Callbacks cbs;
        cbs.onLanguage = [this](ScriptLang l) {
            doc.language = l;
            updateLanguageUI();
            resized();        // body region changes size with the language
            commitToNode();
        };
        cbs.onRate = [this](ScriptRate r) {
            doc.rate = r;
            updateLanguageUI();
            resized();
            commitToNode();
        };
        langBar = std::make_unique<ScriptLangBar>(std::move(cbs));
        langBar->setState(doc.language, doc.rate);
        addAndMakeVisible(*langBar);
    }

    // Embedded layer-stack editor - the shared LayerStackComponent (same
    // widget the Wavetable editor uses). Summation preview ON so the user
    // sees the combined shape; new layers default to the next harmonic
    // (ratio = count+1), full amplitude for the first layer and 0.5 for
    // blended additions, matching the wavetable editor's seeding.
    LayerStackComponent::Options lsOpts;
    lsOpts.showSummationPreview = true;
    lsOpts.summationPreviewHeight = 120;
    lsOpts.addLayerButtonText = "+ Layer";
    lsOpts.emptyHint = "No layers yet. Click \"+ Layer\" to give this LFO / "
                       "envelope a shape (until then it outputs a steady 0.5 - "
                       "the neutral \"no modulation\" level).";
    lsOpts.makeNewLayer = [](int count) {
        WaveLayer l;
        l.shape = WaveLayer::Sine;
        l.ratio = count + 1;
        l.phase = 0.0f;
        l.amp   = (count == 0) ? 1.0f : 0.5f;
        return l;
    };
    layerStack = std::make_unique<LayerStackComponent>(
        std::move(lsOpts), [this]() { commitToNode(); });
    layerStack->setTarget(&doc.layers);
    addAndMakeVisible(*layerStack);

    // Expression / program editor. For the Built-in language this is the
    // single-line "curve transform" expression; for Lua it becomes a multi-line
    // program (updateLanguageUI toggles multi-line + height). For Wasm it is
    // hidden and the wasmPanel takes its place.
    addAndMakeVisible(exprLabel);
    addAndMakeVisible(exprEditor);
    exprEditor.setMultiLine(false);
    exprEditor.setReturnKeyStartsNewLine(true);
    exprEditor.setText(doc.expr, juce::dontSendNotification);
    exprEditor.onTextChange = [this]() {
        doc.expr = exprEditor.getText().toStdString();
        commitToNode();
    };
    exprEditor.setTooltip("How the curve is converted to the output value.\n"
                          "Default 'curve' passes the waveform through unchanged.\n"
                          "Use shape(pos) to read the drawing at an arbitrary\n"
                          "position, e.g. shape(s1) for input-driven lookup.");

    // .wasm file picker (only visible when the language is WebAssembly).
    wasmPanel = std::make_unique<WasmFilePanel>([this](std::string p) {
        doc.wasmPath = std::move(p);
        commitToNode();
    });
    wasmPanel->setPath(doc.wasmPath);
    addChildComponent(*wasmPanel);   // visibility toggled by updateLanguageUI()

    // Contextual one-liner under the program editor (set by updateLanguageUI).
    addChildComponent(scriptNote);
    scriptNote.setColour(juce::Label::textColourId, juce::Colour(0xff8a8a90));
    scriptNote.setFont(juce::Font(12.0f));
    scriptNote.setJustificationType(juce::Justification::topLeft);
    scriptNote.setMinimumHorizontalScale(1.0f);

    // Error strip (hidden until a compile fails).
    addChildComponent(errorLabel);
    errorLabel.setColour(juce::Label::textColourId, juce::Colours::orangered);
    errorLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xff2a1414));
    errorLabel.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 12.0f,
                                  juce::Font::plain));
    errorLabel.setMinimumHorizontalScale(1.0f);
    errorLabel.setTooltip("The program failed to compile. The output stays at its "
                          "neutral level until you fix the error.");

    addAndMakeVisible(triggerLabel);
    addAndMakeVisible(triggerEditor);
    triggerEditor.setMultiLine(false);
    triggerEditor.setText(doc.triggerExpr, juce::dontSendNotification);
    triggerEditor.onTextChange = [this]() {
        doc.triggerExpr = triggerEditor.getText().toStdString();
        // Free-run is meaningless once a trigger is set - reflect that in the UI.
        freeRunToggle.setEnabled(doc.triggerExpr.empty());
        commitToNode();
    };
    triggerEditor.setTooltip("Empty = always running (LFO).\n"
                             "Otherwise: any rising edge of this expression "
                             "(false -> true) resets phase and starts a new cycle. "
                             "Example: 'gate' triggers on every note-on.");

    // Repeat mode buttons act as a radio group.
    addAndMakeVisible(repeatLabel);
    for (auto* b : { &foreverBtn, &onceBtn, &nTimesBtn }) {
        addAndMakeVisible(*b);
        b->setClickingTogglesState(true);
        b->setRadioGroupId(7331);
    }
    foreverBtn.onClick = [this]() {
        doc.repeatMode = SignalShapeDoc::Forever;
        refreshRepeatButtons();
        commitToNode();
    };
    onceBtn.onClick = [this]() {
        doc.repeatMode = SignalShapeDoc::Once;
        refreshRepeatButtons();
        commitToNode();
    };
    nTimesBtn.onClick = [this]() {
        doc.repeatMode = SignalShapeDoc::NTimes;
        refreshRepeatButtons();
        commitToNode();
    };

    addAndMakeVisible(repeatNEditor);
    repeatNEditor.setInputRestrictions(3, "0123456789");
    repeatNEditor.setText(juce::String(doc.repeatN), juce::dontSendNotification);
    repeatNEditor.onTextChange = [this]() {
        int n = std::max(1, repeatNEditor.getText().getIntValue());
        if (n != doc.repeatN) {
            doc.repeatN = n;
            commitToNode();
        }
    };
    repeatNEditor.setTooltip("Number of cycles to play before holding "
                             "(only used when repeat mode is N times).");

    // Signal-input count.
    addAndMakeVisible(sigCountLabel);
    addAndMakeVisible(sigCountEditor);
    sigCountEditor.setInputRestrictions(2, "0123456789");
    sigCountEditor.setText(juce::String(doc.signalInputCount),
                           juce::dontSendNotification);
    sigCountEditor.onTextChange = [this]() {
        int n = juce::jlimit(0, 16, sigCountEditor.getText().getIntValue());
        if (n != doc.signalInputCount) {
            doc.signalInputCount = n;
            syncPins();  // rewrites node.pinsIn / pinsOut
            commitToNode();
        }
    };
    sigCountEditor.setTooltip("Number of incoming Signal pins exposed in "
                              "expressions as s1, s2, ... sN. 0..16.");

    // Continuous-output count (o1..oP). Always >= 1.
    addAndMakeVisible(outCountLabel);
    addAndMakeVisible(outCountEditor);
    outCountEditor.setInputRestrictions(2, "0123456789");
    outCountEditor.setText(juce::String(doc.continuousOutputCount),
                           juce::dontSendNotification);
    outCountEditor.onTextChange = [this]() {
        int n = juce::jlimit(1, 16, outCountEditor.getText().getIntValue());
        if (n != doc.continuousOutputCount) {
            doc.continuousOutputCount = n;
            syncPins();
            commitToNode();
        }
    };
    outCountEditor.setTooltip("Number of continuous output pins, assigned in the "
                              "program as o1, o2, ... oP. o1 defaults to the "
                              "program's last value. 1..16.");

    // MIDI-output count (0 = pure signal source).
    addAndMakeVisible(midiOutLabel);
    addAndMakeVisible(midiOutEditor);
    midiOutEditor.setInputRestrictions(2, "0123456789");
    midiOutEditor.setText(juce::String(doc.midiOutputCount),
                          juce::dontSendNotification);
    midiOutEditor.onTextChange = [this]() {
        int n = juce::jlimit(0, 16, midiOutEditor.getText().getIntValue());
        if (n != doc.midiOutputCount) {
            doc.midiOutputCount = n;
            syncPins();
            commitToNode();
        }
    };
    midiOutEditor.setTooltip("Number of MIDI output pins. 0 = pure signal source. "
                             "With >=1, the program can emit MIDI with note(), "
                             "cc(), bend()...; the `out` variable picks the pin "
                             "(0-based). 0..16.");

    // MIDI-input count: 0 = no MIDI input, 1 = single "MIDI In", >1 = "MIDI In
    // 1..N". The MIDI In pin(s) drive the note/vel/gate/freq variables; with >1
    // pin, pollmidi() / the structured pull report which input each event came
    // from (1-based) so one program can react to several sources independently.
    addAndMakeVisible(midiInLabel);
    addAndMakeVisible(midiInEditor);
    midiInEditor.setInputRestrictions(2, "0123456789");
    midiInEditor.setText(juce::String(doc.midiInputCount),
                         juce::dontSendNotification);
    midiInEditor.onTextChange = [this]() {
        int n = juce::jlimit(0, 16, midiInEditor.getText().getIntValue());
        if (n != doc.midiInputCount) {
            doc.midiInputCount = n;
            syncPins();
            commitToNode();
        }
    };
    midiInEditor.setTooltip("Number of MIDI input pins. 0 = none (note/vel/gate/"
                            "freq stay idle). 1 = a single MIDI In. With >1 you "
                            "get MIDI In 1..N, and pollmidi() / the structured "
                            "pull tell you which input each event arrived on "
                            "(1-based). 0..16.");

    // Pin-type dropdown (Signal vs Param) for ALL continuous input/output pins.
    addAndMakeVisible(kindLabel);
    addAndMakeVisible(kindCombo);
    kindCombo.addItem("Signal", 1);
    kindCombo.addItem("Param", 2);
    kindCombo.setSelectedId(doc.paramKind ? 2 : 1, juce::dontSendNotification);
    kindCombo.onChange = [this]() {
        bool param = (kindCombo.getSelectedId() == 2);
        if (param != doc.paramKind) {
            doc.paramKind = param;
            syncPins();
            commitToNode();
        }
    };
    kindCombo.setTooltip("Pin type for all continuous input/output pins: Signal "
                         "(blue, audio-rate control) or Param (orange, parameter "
                         "automation). Both carry the same 0..1 data and "
                         "interconvert on the wire.");

    // Speed controls: Cycle length <-> Rate (two views of the "Rate" param),
    // plus the Beat Sync toggle that chooses their units. See header for the
    // full units logic.
    addAndMakeVisible(speedLabel);
    addAndMakeVisible(cycleCaption);
    addAndMakeVisible(cycleEditor);
    addAndMakeVisible(cycleUnit);
    addAndMakeVisible(eqLabel);
    addAndMakeVisible(rateCaption);
    addAndMakeVisible(rateEditor);
    addAndMakeVisible(rateUnit);
    addAndMakeVisible(beatSyncToggle);

    eqLabel.setJustificationType(juce::Justification::centred);
    for (auto* l : { &cycleCaption, &rateCaption, &cycleUnit, &rateUnit, &eqLabel })
        l->setColour(juce::Label::textColourId, juce::Colour(0xff909096));

    cycleEditor.setInputRestrictions(0, "0123456789.");
    cycleEditor.onTextChange = [this]() {
        float cyc = cycleEditor.getText().getFloatValue();
        if (cyc > 0.0001f) writeRate(1.0f / cyc);
    };
    // Reformat to the (possibly clamped) true value once editing finishes.
    cycleEditor.onFocusLost = [this]() { refreshSpeedControls(); };
    cycleEditor.setTooltip("Time for one full cycle of the shape.\n"
                           "Free-run: seconds. With 'Sync to beat' on: beats.\n"
                           "This is just 1 / Rate - the same setting as the\n"
                           "Rate field, shown the other way round.");

    rateEditor.setInputRestrictions(0, "0123456789.");
    rateEditor.onTextChange = [this]() {
        float rate = rateEditor.getText().getFloatValue();
        if (rate > 0.0f) writeRate(rate);
    };
    rateEditor.onFocusLost = [this]() { refreshSpeedControls(); };
    rateEditor.setTooltip("How many cycles play per second (Hz) in free-run,\n"
                          "or per beat with 'Sync to beat' on. This is\n"
                          "1 / Cycle length - the same setting as the Cycle\n"
                          "length field, shown the other way round.");

    beatSyncToggle.onClick = [this]() {
        if (auto* p = paramByName("Beat Sync")) {
            p->value     = beatSyncToggle.getToggleState() ? 1.0f : 0.0f;
            p->baseValue = p->value;
        }
        refreshSpeedControls();
        if (onChanged) onChanged();
    };
    beatSyncToggle.setTooltip("Off: cycle length is in seconds, Rate in Hz - the\n"
                              "shape ignores song tempo.\n"
                              "On: cycle length is in beats, Rate in cycles per\n"
                              "beat - the shape follows the song tempo.");

    addAndMakeVisible(freeRunToggle);
    freeRunToggle.setToggleState(doc.freeRun, juce::dontSendNotification);
    freeRunToggle.onClick = [this]() {
        doc.freeRun = freeRunToggle.getToggleState();
        commitToNode();
    };
    freeRunToggle.setTooltip(
        "Off (default): the shape's phase is locked to the song position, so it\n"
        "plays identically every time and matches the exported audio - and it\n"
        "freezes while playback is stopped.\n"
        "On: the shape runs continuously off its own clock (like an analog LFO),\n"
        "drifting independently of the song and never stopping. Livelier, but not\n"
        "reproducible.\n"
        "Has no effect while a Trigger is set (a triggered shape already restarts\n"
        "deterministically on each trigger), so it's disabled in that case.");
    // Free-run only matters for non-triggered shapes; grey it out when a trigger
    // is set so it's clear it has no effect there.
    freeRunToggle.setEnabled(doc.triggerExpr.empty());

    refreshSpeedControls();

    addAndMakeVisible(manualTriggerBtn);
    manualTriggerBtn.onClick = [this]() {
        if (onManualTrigger) onManualTrigger();
    };
    manualTriggerBtn.setTooltip("Fire one trigger as if the trigger expression "
                                "just went from low to high. Useful for auditioning "
                                "envelope-style shapes (repeat: Once).");
    if (!onManualTrigger) manualTriggerBtn.setEnabled(false);

    addAndMakeVisible(closeBtn);
    closeBtn.onClick = [this]() {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->setVisible(false);
    };

    addAndMakeVisible(varsHelpBtn);
    // The full reference is the button's tooltip (hover) AND its click action
    // (a message box), so it's discoverable both ways without permanently
    // occupying editor space.
    varsHelpBtn.setTooltip(kSignalShapeHelp);
    varsHelpBtn.onClick = [this]() {
        const bool lua = (doc.language == ScriptLang::Lua);
        juce::NativeMessageBox::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::InfoIcon)
                .withTitle(lua ? "Signal Shape - Lua reference"
                               : "Signal Shape - variables & functions")
                .withMessage(lua ? kSignalShapeLuaHelp : kSignalShapeHelp)
                .withButton("OK")
                .withAssociatedComponent(this),
            nullptr);
    };

    refreshRepeatButtons();
    updateLanguageUI();

    // Size LAST (see note at top of constructor): now that every child —
    // including the layer stack — exists, the resized() that setSize() triggers
    // lays the whole editor out correctly, layer stack included. One layer row
    // is ~238px tall, so the default height is generous enough to show a full
    // row plus the summation preview without scrolling; the dialog is resizable
    // for more. The variables/functions reference is now a button (not an
    // always-visible block), freeing ~150px for the layer stack.
    setSize(720, 1016);
    validateScript();   // surface any pre-existing error in the loaded script
}

SignalShapeEditorComponent::~SignalShapeEditorComponent() {
    // Per-edit commitToNode() writes node.script live (so the audio graph hears
    // edits immediately) but deliberately does NOT push an undo step - that
    // would fragment undo into one step per keystroke/click and re-serialize the
    // whole graph each time. The natural commit point for a modeless dialog is
    // when it closes, so push a single snapshot here covering the whole editing
    // session (expression, trigger, repeat, layers, language/rate/wasm, signal
    // count). commitSnapshot de-dups against the previous step, so closing
    // without changes is a no-op; if anything changed it also sets graph.dirty
    // so the user is prompted to save on quit. (See known-issues.md - this also
    // fixes the long-standing gap where Signal Shape edits left no undo step.)
    graph.commitSnapshot("Edit Signal Shape");
}

void SignalShapeEditorComponent::loadFromNode() {
    auto* nd = graph.findNode(nodeId);
    if (!nd) { doc = SignalShapeDoc::defaultLFO(); return; }

    if (SignalShapeDoc::isSignalShapeScript(nd->script)) {
        doc.decode(nd->script);
    } else if (nd->script.empty() || nd->script == "__xypad__") {
        // XY Pad shares NodeType::SignalShape but uses its own editor; this
        // editor shouldn't be opened for an XY Pad. Defensive fallback only.
        doc = SignalShapeDoc::defaultLFO();
    } else {
        // Legacy: convert a plain expression script into a one-layer Formula
        // stack so the user sees their old expression in the new editor and
        // can keep editing it.
        doc = SignalShapeDoc::defaultLFO();
        WaveLayer l;
        l.shape = WaveLayer::Formula;
        l.formulaExpr = nd->script;
        doc.layers.layers.push_back(l);
    }
}

void SignalShapeEditorComponent::refreshRepeatButtons() {
    foreverBtn.setToggleState(doc.repeatMode == SignalShapeDoc::Forever,
                              juce::dontSendNotification);
    onceBtn   .setToggleState(doc.repeatMode == SignalShapeDoc::Once,
                              juce::dontSendNotification);
    nTimesBtn .setToggleState(doc.repeatMode == SignalShapeDoc::NTimes,
                              juce::dontSendNotification);
    repeatNEditor.setEnabled(doc.repeatMode == SignalShapeDoc::NTimes);
}

Param* SignalShapeEditorComponent::paramByName(const std::string& name) {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return nullptr;
    for (auto& p : nd->params)
        if (p.name == name) return &p;
    return nullptr;
}

void SignalShapeEditorComponent::refreshSpeedControls() {
    Param* rateP = paramByName("Rate");
    Param* syncP = paramByName("Beat Sync");
    bool  beatSync = syncP && syncP->value != 0.0f;
    float rate     = rateP ? rateP->value : 1.0f;

    beatSyncToggle.setToggleState(beatSync, juce::dontSendNotification);
    cycleUnit.setText(beatSync ? "beats" : "sec",  juce::dontSendNotification);
    rateUnit .setText(beatSync ? "/beat" : "Hz",   juce::dontSendNotification);

    // Don't clobber the field the user is actively typing into.
    if (!rateEditor.hasKeyboardFocus(true))
        rateEditor.setText(juce::String(rate, 3), juce::dontSendNotification);
    if (!cycleEditor.hasKeyboardFocus(true)) {
        float cyc = (rate > 0.0001f) ? (1.0f / rate) : 0.0f;
        cycleEditor.setText(juce::String(cyc, 3), juce::dontSendNotification);
    }
}

void SignalShapeEditorComponent::writeRate(float rate) {
    Param* rateP = paramByName("Rate");
    if (!rateP) return;
    rate = juce::jlimit(rateP->minVal, rateP->maxVal, rate);
    rateP->value     = rate;
    rateP->baseValue = rate;
    refreshSpeedControls();
    if (onChanged) onChanged();
}

void SignalShapeEditorComponent::commitToNode() {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;
    nd->script = doc.encode();
    if (onChanged) onChanged();
    // Re-lint after edits settle (300ms debounce) so a heavy Lua top-level
    // isn't recompiled on every keystroke.
    startTimer(300);
}

void SignalShapeEditorComponent::timerCallback() {
    stopTimer();
    validateScript();
}

void SignalShapeEditorComponent::validateScript() {
    std::string err;
    // Pick the effective rate from the capability matrix (mirrors
    // SignalShapeProcessor::reloadIfScriptChanged).
    ScriptRate effRate = doc.rate;
    if (!scriptLangSupportsRate(doc.language, effRate))
        effRate = (doc.language == ScriptLang::Wasm) ? ScriptRate::PerBlock
                                                     : ScriptRate::PerSample;
    if (auto rt = makeScriptRuntime(doc.language, ScriptRole::Unified, effRate)) {
        // Wasm validates its binary path; Builtin/Lua compile the expression.
        const std::string& src = (doc.language == ScriptLang::Wasm) ? doc.wasmPath
                                                                    : doc.expr;
        rt->load(src, err);
    }
    const bool hasErr = !err.empty();
    if (hasErr)
        errorLabel.setText("Script error: " + juce::String(err),
                           juce::dontSendNotification);
    if (errorLabel.isVisible() != hasErr) {
        errorLabel.setVisible(hasErr);
        resized();   // claim / release the bottom strip
    }
}

void SignalShapeEditorComponent::updateLanguageUI() {
    const bool isWasm = (doc.language == ScriptLang::Wasm);
    const bool isLua  = (doc.language == ScriptLang::Lua);
    // raw-buffer mode: the script fills the whole block itself, bypassing the
    // built-in phase / repeat / curve machinery (Lua per-block and Wasm).
    const bool rawMode = isWasm || (isLua && doc.rate == ScriptRate::PerBlock);

    exprEditor.setVisible(!isWasm);
    if (wasmPanel) wasmPanel->setVisible(isWasm);
    exprEditor.setMultiLine(isLua, false);

    if (isWasm)
        exprLabel.setText("WebAssembly module:", juce::dontSendNotification);
    else if (isLua)
        exprLabel.setText("Lua program (return the output value):",
                          juce::dontSendNotification);
    else
        exprLabel.setText("Output (expression of curve, etc.):",
                          juce::dontSendNotification);

    // Contextual note: explain what the script controls in this mode, and call
    // out which built-in controls below are bypassed in raw-buffer mode.
    juce::String note;
    if (doc.language == ScriptLang::Builtin) {
        note = {};
    } else if (rawMode) {
        note = "Raw mode: your script writes every sample of the block itself. "
               "The Trigger, Repeat, Speed and drawn-curve controls below are "
               "bypassed (the shape is still readable via shape(pos)).";
    } else { // Lua per-sample
        note = "Per-sample Lua: your program runs once per sample and returns the "
               "output value. It can read `curve` (the drawn shape at the current "
               "phase) and all the variables below still apply.";
    }
    scriptNote.setText(note, juce::dontSendNotification);
    scriptNote.setVisible(note.isNotEmpty());

    // In raw-buffer mode the phase/curve machinery is bypassed, so grey out the
    // controls that no longer have any effect (their tooltips still explain why
    // via the contextual note above).
    const bool machineryActive = !rawMode;
    for (auto* b : { &foreverBtn, &onceBtn, &nTimesBtn })
        b->setEnabled(machineryActive);
    repeatNEditor.setEnabled(machineryActive && doc.repeatMode == SignalShapeDoc::NTimes);
    triggerEditor.setEnabled(machineryActive);
    freeRunToggle.setEnabled(machineryActive && doc.triggerExpr.empty());
    manualTriggerBtn.setEnabled(machineryActive && onManualTrigger != nullptr);

    // Point the reference button at the active language.
    if (isLua) {
        varsHelpBtn.setButtonText("Lua reference...");
        varsHelpBtn.setTooltip(kSignalShapeLuaHelp);
    } else {
        varsHelpBtn.setButtonText("Variables & functions...");
        varsHelpBtn.setTooltip(kSignalShapeHelp);
    }
}

bool SignalShapeEditorComponent::syncPins() {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return false;

    // Continuous pins (s1..sN inputs, o1..oP outputs) share one user-chosen
    // kind; MIDI pins are always PinKind::Midi.
    const PinKind ctrlKind = doc.paramKind ? PinKind::Param : PinKind::Signal;

    // Find an existing pin id by name within a list (preserves cables across a
    // rewrite). Matches a continuous pin only if it currently carries a control
    // kind, so a name collision against a MIDI pin can't steal its id.
    auto findId = [](const std::vector<Pin>& pins, const std::string& name,
                     bool wantCtrl) -> int {
        for (auto& p : pins) {
            const bool isCtrl = (p.kind == PinKind::Signal || p.kind == PinKind::Param);
            if (p.name == name && (wantCtrl ? isCtrl : (p.kind == PinKind::Midi)))
                return p.id;
        }
        return -1;
    };
    auto makePin = [&](int id, const std::string& name, PinKind kind, bool input) {
        Pin p;
        p.id      = (id >= 0) ? id : graph.allocId();
        p.name    = name;
        p.kind    = kind;
        p.isInput = input;
        return p;
    };

    // ---- Inputs: [MIDI In pins] + s1..sN (ctrlKind) ----------------------
    // 0 = none, 1 = a single "MIDI In", >1 = "MIDI In 1..N".
    const int midiInCount = juce::jlimit(0, 16, doc.midiInputCount);
    std::vector<Pin> inRebuilt;
    inRebuilt.reserve(midiInCount + doc.signalInputCount);
    if (midiInCount == 1) {
        inRebuilt.push_back(makePin(findId(nd->pinsIn, "MIDI In", false),
                                    "MIDI In", PinKind::Midi, true));
    } else {
        for (int i = 0; i < midiInCount; ++i) {
            std::string name = "MIDI In " + std::to_string(i + 1);
            inRebuilt.push_back(makePin(findId(nd->pinsIn, name, false),
                                        name, PinKind::Midi, true));
        }
    }
    for (int i = 0; i < doc.signalInputCount; ++i) {
        std::string name = "s" + std::to_string(i + 1);
        inRebuilt.push_back(makePin(findId(nd->pinsIn, name, true),
                                    name, ctrlKind, true));
    }

    // ---- Outputs: o1..oP (ctrlKind) + MIDI Out 1..Q ----------------------
    const int outCount = juce::jmax(1, doc.continuousOutputCount);
    std::vector<Pin> outRebuilt;
    outRebuilt.reserve(outCount + doc.midiOutputCount);
    for (int i = 0; i < outCount; ++i) {
        std::string name = "o" + std::to_string(i + 1);
        outRebuilt.push_back(makePin(findId(nd->pinsOut, name, true),
                                     name, ctrlKind, false));
    }
    for (int i = 0; i < doc.midiOutputCount; ++i) {
        std::string name = "MIDI Out " + std::to_string(i + 1);
        outRebuilt.push_back(makePin(findId(nd->pinsOut, name, false),
                                     name, PinKind::Midi, false));
    }

    auto differs = [](const std::vector<Pin>& a, const std::vector<Pin>& b) {
        if (a.size() != b.size()) return true;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].id != b[i].id || a[i].name != b[i].name || a[i].kind != b[i].kind)
                return true;
        return false;
    };

    bool changed = differs(inRebuilt, nd->pinsIn) || differs(outRebuilt, nd->pinsOut);
    if (changed) {
        nd->pinsIn  = std::move(inRebuilt);
        nd->pinsOut = std::move(outRebuilt);
    }
    return changed;
}

void SignalShapeEditorComponent::resized() {
    auto r = getLocalBounds().reduced(10);

    // Language + rate control strip pinned to the very top.
    if (langBar) {
        langBar->setBounds(r.removeFromTop(ScriptLangBar::kHeight));
        r.removeFromTop(8);
    }

    // The layer stack is the primary editing surface, so it takes the top and
    // absorbs all spare vertical space (it scrolls internally for many layers
    // and contains its own + Layer button and summation preview). Everything
    // below it is a fixed-height block; we reserve that block's height up
    // front and hand the remainder to the stack.
    // For Lua / Wasm the "expression" row grows into a multi-line program editor
    // (or the .wasm file panel), so reserve more height there; a contextual note
    // sits just under it when a script language is active.
    const bool scriptLang = (doc.language != ScriptLang::Builtin);
    const int exprBodyH = scriptLang ? 150 : 24;
    const int noteH     = scriptLang ? (16 + 4) : 0;
    const int speedH   = 24 + 8;
    const int clockH   = 24 + 6;   // free-run toggle row
    const int exprH    = 18 + exprBodyH + noteH + 6;
    const int trigH    = 18 + 24 + 6;
    const int repeatH  = 24 + 8;
    const int sigH     = 24 + 8;   // signal-in count + signal-out count row
    const int midiH    = 24 + 8;   // MIDI-out count + MIDI-in toggle row
    const int kindH    = 24 + 8;   // pin-type dropdown row
    // The variables/functions reference no longer occupies a fixed block here —
    // it's a button in the bottom row, so all the space it used to take goes to
    // the layer stack.
    const int btnH     = 28;
    const int errH     = errorLabel.isVisible() ? (22 + 6) : 0;  // bottom error strip
    const int fixedBottom = speedH + clockH + exprH + trigH + repeatH
                          + sigH + midiH + kindH + btnH + errH;

    if (layerStack) {
        // One WaveLayerEditor row is ~238px; reserve enough that at least one
        // full layer (plus the stack's own summation preview) is visible before
        // the viewport has to scroll. Spare height beyond the fixed bottom block
        // goes to the stack.
        int layerH = juce::jmax(420, r.getHeight() - fixedBottom);
        layerStack->setBounds(r.removeFromTop(layerH));
        r.removeFromTop(8);
    }

    // Speed row: Cycle length <-> Rate (two views of "Rate"), plus Beat Sync.
    {
        auto row = r.removeFromTop(24);
        speedLabel  .setBounds(row.removeFromLeft(50));
        cycleCaption.setBounds(row.removeFromLeft(80));
        cycleEditor .setBounds(row.removeFromLeft(64));
        row.removeFromLeft(4);
        cycleUnit   .setBounds(row.removeFromLeft(38));
        eqLabel     .setBounds(row.removeFromLeft(20));
        rateCaption .setBounds(row.removeFromLeft(38));
        rateEditor  .setBounds(row.removeFromLeft(64));
        row.removeFromLeft(4);
        rateUnit    .setBounds(row.removeFromLeft(40));
        row.removeFromLeft(12);
        beatSyncToggle.setBounds(row.removeFromLeft(120));
        r.removeFromTop(8);
    }

    // Phase-source row: the free-run toggle, aligned under the speed fields.
    {
        auto row = r.removeFromTop(24);
        row.removeFromLeft(50);
        freeRunToggle.setBounds(row.removeFromLeft(280));
        r.removeFromTop(6);
    }

    auto labelRow = [&](juce::Label& l, juce::TextEditor& e) {
        l.setBounds(r.removeFromTop(18));
        e.setBounds(r.removeFromTop(24));
        r.removeFromTop(6);
    };

    // Expression / program row: variable height. The exprEditor and the .wasm
    // file panel share the same rectangle (updateLanguageUI chooses which is
    // visible). A contextual note sits just under it for script languages.
    {
        exprLabel.setBounds(r.removeFromTop(18));
        auto body = r.removeFromTop(exprBodyH);
        exprEditor.setBounds(body);
        if (wasmPanel) wasmPanel->setBounds(body);
        if (noteH > 0) scriptNote.setBounds(r.removeFromTop(noteH).withTrimmedBottom(4));
        r.removeFromTop(6);
    }

    labelRow(triggerLabel, triggerEditor);

    // Repeat row: label + 3 buttons + repeatN spinner.
    {
        auto row = r.removeFromTop(24);
        repeatLabel.setBounds(row.removeFromLeft(60));
        foreverBtn .setBounds(row.removeFromLeft(80));
        row.removeFromLeft(4);
        onceBtn    .setBounds(row.removeFromLeft(70));
        row.removeFromLeft(4);
        nTimesBtn  .setBounds(row.removeFromLeft(80));
        row.removeFromLeft(8);
        repeatNEditor.setBounds(row.removeFromLeft(60));
        r.removeFromTop(8);
    }

    // I/O counts row: signal inputs (s1..sN) + signal outputs (o1..oP).
    {
        auto row = r.removeFromTop(24);
        sigCountLabel .setBounds(row.removeFromLeft(150));
        sigCountEditor.setBounds(row.removeFromLeft(54));
        row.removeFromLeft(16);
        outCountLabel .setBounds(row.removeFromLeft(150));
        outCountEditor.setBounds(row.removeFromLeft(54));
        r.removeFromTop(8);
    }

    // MIDI I/O row: MIDI output count + MIDI input count.
    {
        auto row = r.removeFromTop(24);
        midiOutLabel .setBounds(row.removeFromLeft(100));
        midiOutEditor.setBounds(row.removeFromLeft(54));
        row.removeFromLeft(16);
        midiInLabel  .setBounds(row.removeFromLeft(100));
        midiInEditor .setBounds(row.removeFromLeft(54));
        r.removeFromTop(8);
    }

    // Pin-type row: Signal vs Param for all continuous pins.
    {
        auto row = r.removeFromTop(24);
        kindLabel.setBounds(row.removeFromLeft(60));
        kindCombo.setBounds(row.removeFromLeft(120));
        r.removeFromTop(8);
    }

    // Error strip at the very bottom (only when a compile failed).
    if (errorLabel.isVisible()) {
        errorLabel.setBounds(r.removeFromBottom(22));
        r.removeFromBottom(6);
    }

    // Bottom button row: Manual Trigger (left), the variables/functions help
    // button next to it, and Close (right).
    auto btnRow = r.removeFromBottom(28);
    manualTriggerBtn.setBounds(btnRow.removeFromLeft(130));
    btnRow.removeFromLeft(8);
    varsHelpBtn.setBounds(btnRow.removeFromLeft(170));
    closeBtn.setBounds(btnRow.removeFromRight(80));
}

void SignalShapeEditorComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff1e1e22));
}

} // namespace SoundShop

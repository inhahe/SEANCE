#include "midi_script_node.h"
#include <juce_core/juce_core.h>
#include <cmath>
#include <sstream>
#include <functional>

namespace SoundShop {

// -----------------------------------------------------------------------------
// MidiScriptDoc encode / decode
// -----------------------------------------------------------------------------
// Same framing strategy as SignalShapeDoc: base64 the free-form program text
// (it may contain '=', newlines, '|', etc.) so it can't collide with our
// key=value lines; integers and the layer body are written verbatim.

static const char* kMidiScriptPrefix = "__midiscript__:v1";

static std::string b64(const std::string& s) {
    juce::MemoryBlock mb(s.data(), s.size());
    return mb.toBase64Encoding().toStdString();
}
static std::string unb64(const std::string& s) {
    juce::MemoryBlock mb;
    if (!mb.fromBase64Encoding(juce::String(s))) return {};
    return std::string((const char*)mb.getData(), mb.getSize());
}

bool MidiScriptDoc::isMidiScriptScript(const std::string& s) {
    return s.rfind("__midiscript__:", 0) == 0;
}

MidiScriptDoc MidiScriptDoc::defaultDoc() {
    MidiScriptDoc d;
    d.program.clear();          // silent until the user writes a program
    d.signalInputCount = 0;
    d.outputCount = 1;
    d.shapeLayers.layers.clear();
    d.shapeLayers.tableSize = 1024;
    return d;
}

std::string MidiScriptDoc::encode() const {
    std::ostringstream o;
    o << kMidiScriptPrefix << "\n";
    o << "program=" << b64(program) << "\n";
    o << "sigCount=" << signalInputCount << "\n";
    o << "outCount=" << outputCount << "\n";
    o << "lang=" << (int)language << "\n";
    o << "rate=" << (int)rate << "\n";
    o << "wasm=" << b64(wasmPath) << "\n";
    o << "layer=" << shapeLayers.encodeBody();
    return o.str();
}

bool MidiScriptDoc::decode(const std::string& s) {
    *this = defaultDoc();
    if (s.rfind("__midiscript__:", 0) != 0) return false;

    auto lines = juce::StringArray::fromLines(juce::String(s));
    for (int i = 0; i < lines.size(); ++i) {
        if (i == 0) continue; // prefix line
        auto line = lines[i].toStdString();
        if (line.empty()) continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "program") {
            program = unb64(val);
        } else if (key == "sigCount") {
            try { signalInputCount = juce::jlimit(0, 16, std::stoi(val)); } catch (...) {}
        } else if (key == "outCount") {
            try { outputCount = juce::jlimit(1, 16, std::stoi(val)); } catch (...) {}
        } else if (key == "lang") {
            try { language = (ScriptLang)juce::jlimit(0, 2, std::stoi(val)); } catch (...) {}
        } else if (key == "rate") {
            try { rate = (ScriptRate)juce::jlimit(0, 1, std::stoi(val)); } catch (...) {}
        } else if (key == "wasm") {
            wasmPath = unb64(val);
        } else if (key == "layer") {
            shapeLayers.decodeBody(val);
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
// MidiScriptProcessor::Sink
// -----------------------------------------------------------------------------
//
// Implements the program-mode emit interface. Set up once per block; emit calls
// land at `sampleOffset` (the index of the sample currently being evaluated).
struct MidiScriptProcessor::Sink : public IExprEmitSink {
    juce::MidiBuffer* midi = nullptr;
    int sampleOffset = 0;
    double sampleRate = 44100.0;
    int outputCount = 1;
    std::vector<PendingNoteOff>* pendingOffs = nullptr;

    // Map the program's `out` value to a real MIDI channel 1..16. We clamp to
    // the declared output count so a typo can't address a non-existent cable.
    int channelFor(int out) const {
        int idx = juce::jlimit(0, outputCount - 1, out);
        return juce::jlimit(1, 16, idx + 1);
    }

    static int clampNote(float v) { return juce::jlimit(0, 127, (int)std::lround(v)); }

    // The runtime calls this in PerBlock dispatch to stamp the within-block
    // offset of subsequently-emitted events; the per-sample loop sets it directly.
    void setSampleOffset(int off) override { sampleOffset = off; }

    void emitNote(int out, float pitch, float vel, float durSec) override {
        if (!midi) return;
        int ch = channelFor(out);
        int p  = clampNote(pitch);
        int v  = clampNote(vel);
        if (v <= 0) return; // velocity 0 = silence; skip
        midi->addEvent(juce::MidiMessage::noteOn(ch, p, (juce::uint8)v), sampleOffset);
        if (pendingOffs && pendingOffs->size() < kMaxPendingOffs) {
            long long durSamples = std::max<long long>(1,
                (long long)std::lround((double)durSec * sampleRate));
            // Schedule the note-off relative to the CURRENT block start: due at
            // this note-on's offset + duration. The processor drains it once per
            // block (subtract block length, emit when <= 0). Works for both
            // per-sample and per-block dispatch.
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
        // value -1..1 -> 0..16383, centre 8192.
        int w = juce::jlimit(0, 16383, (int)std::lround((juce::jlimit(-1.0f, 1.0f, value) * 0.5f + 0.5f) * 16383.0f));
        midi->addEvent(juce::MidiMessage::pitchWheel(channelFor(out), w), sampleOffset);
    }
};

// -----------------------------------------------------------------------------
// MidiScriptProcessor
// -----------------------------------------------------------------------------

MidiScriptProcessor::MidiScriptProcessor(Node& n, Transport& t)
    : node(n), transport(t) {
    reloadIfScriptChanged();
}

float MidiScriptProcessor::getParam(int idx, float def) const {
    if (idx >= 0 && idx < (int)node.params.size())
        return node.params[idx].value;
    return def;
}

static float sampleShapeTbl(const std::vector<float>& tbl, float phase);

// Build the standard per-sample variable bindings at sample offset `s`.
void MidiScriptProcessor::buildVars(ScriptVars& vars,
                                    int s, double sr, double secPerSample,
                                    float gateVal, float freqHz,
                                    const std::vector<const float*>& sigChans,
                                    int sigCount) const {
    double secAtSample  = (double)(transport.positionSamples + s) / sr;
    double beatAtSample = transport.positionBeats() + transport.bpm / (60.0 * sr) * s;
    vars["t"]       = (float)secAtSample;
    vars["beat"]    = (float)beatAtSample;
    vars["bar"]     = (float)(beatAtSample / 4.0);
    vars["bpm"]     = (float)transport.bpm;
    vars["playing"] = transport.playing ? 1.0f : 0.0f;
    vars["sr"]      = (float)sr;
    vars["dt"]      = (float)secPerSample;
    vars["note"]    = (float)lastNoteOn;
    vars["vel"]     = lastVelocity;
    vars["gate"]    = gateVal;
    vars["freq"]    = freqHz;
    for (int i = 0; i < sigCount; ++i)
        vars["s" + std::to_string(i + 1)] = sigChans[i] ? sigChans[i][s] : 0.0f;
}

// Bind the shape(pos) sampler on the runtime. Captures `this` so it always reads
// the current shapeSamples (rebuilt in place on shape changes - no dangling).
void MidiScriptProcessor::bindShape() {
    if (!runtime) return;
    runtime->setShape([this](float pos) { return sampleShapeTbl(shapeSamples, pos); });
}

void MidiScriptProcessor::reloadIfScriptChanged() {
    if (node.script == lastScriptSeen && runtime && !shapeSamples.empty()) return;
    lastScriptSeen = node.script;

    if (MidiScriptDoc::isMidiScriptScript(node.script))
        doc.decode(node.script);
    else
        doc = MidiScriptDoc::defaultDoc();

    rebuildShapeSamples();
    // Reset any in-flight notes when the program changes, so the new program
    // starts from a clean slate.
    pendingOffs.clear();

    // (Re)create the language runtime. Pick the effective rate from the capability
    // matrix: Builtin -> PerSample, Wasm -> PerBlock, Lua -> doc.rate.
    ScriptRate effRate = doc.rate;
    if (!scriptLangSupportsRate(doc.language, effRate))
        effRate = (doc.language == ScriptLang::Wasm) ? ScriptRate::PerBlock
                                                     : ScriptRate::PerSample;
    runtime = makeScriptRuntime(doc.language, ScriptRole::Midi, effRate);
    if (!runtime) // language unavailable in this build -> fall back to Builtin.
        runtime = makeScriptRuntime(ScriptLang::Builtin, ScriptRole::Midi,
                                    ScriptRate::PerSample);

    bindShape();

    // For Wasm the runtime source is the file path; for Builtin/Lua it's the text.
    const std::string& src = (doc.language == ScriptLang::Wasm) ? doc.wasmPath
                                                                : doc.program;
    std::string err;
    runtime->load(src, err); // errors surface via getError(); audio stays silent
    runtime->reset();        // runs the language's init hook (Builtin init:, ...)

    runBlockMode = runtime->supportsPerBlock()
                   && (effRate == ScriptRate::PerBlock || !runtime->supportsPerSample());

    // Force the next block to treat the transport as a fresh start so the start
    // hook fires if we're already playing when the script changes.
    wasPlaying = false;
}

void MidiScriptProcessor::rebuildShapeSamples() {
    doc.shapeLayers.render(kShapeTableSize, shapeSamples);
    if ((int)shapeSamples.size() != kShapeTableSize)
        shapeSamples.assign(kShapeTableSize, 0.0f);
}

static float sampleShapeTbl(const std::vector<float>& tbl, float phase) {
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

void MidiScriptProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midiIn) {
    reloadIfScriptChanged();

    const int numSamples = buf.getNumSamples();
    const int numCh = buf.getNumChannels();

    // The MIDI input arrives in `midiIn`; we want our generated events in the
    // OUTPUT MIDI buffer, which JUCE's graph hands us as the same buffer. We
    // read the input note state first, then clear and write our own events.
    // (A MIDI generator does not pass its input through; the input only drives
    // the program via the note/vel/gate variables.)
    for (auto meta : midiIn) {
        auto msg = meta.getMessage();
        if (msg.isNoteOn()) {
            notesHeld++;
            lastNoteOn = msg.getNoteNumber();
            lastVelocity = (float)msg.getVelocity() / 127.0f;
        } else if (msg.isNoteOff()) {
            if (notesHeld > 0) notesHeld--;
        }
    }
    midiIn.clear();

    // Audio buffer is unused by a pure MIDI node, but Signal inputs ride on
    // channels 2+ (one per Signal pin in declaration order). Read them per
    // sample for the s1..sN variables. We do NOT clear the audio buffer's
    // control channels because nothing downstream reads this node's audio.
    const int sigCount = doc.signalInputCount;
    std::vector<const float*> sigChans;
    sigChans.reserve(sigCount);
    for (int i = 0; i < sigCount; ++i) {
        int ch = 2 + i;
        sigChans.push_back((ch < numCh) ? buf.getReadPointer(ch) : nullptr);
    }

    // Detect the transport play rising edge (stopped -> rolling) for the start hook.
    const bool playStart = transport.playing && !wasPlaying;
    wasPlaying = transport.playing;

    if (!runtime) return;

    Sink sink;
    sink.midi = &midiIn;
    sink.sampleRate = sampleRate;
    sink.outputCount = doc.outputCount;
    sink.pendingOffs = &pendingOffs;

    ScriptVars vars;
    vars.reserve(16 + sigCount);

    const double sr = juce::jmax(1.0, transport.sampleRate);
    const double secPerSample = 1.0 / sr;
    const float gateVal = (notesHeld > 0) ? 1.0f : 0.0f;
    const float freqHz = (lastNoteOn >= 0)
                       ? 440.0f * std::pow(2.0f, (lastNoteOn - 69) / 12.0f) : 0.0f;

    if (runBlockMode) {
        // ---- Per-block dispatch (Lua block-rate / Wasm) ---------------------
        // Run the start hook first (offset 0) so a one-shot can emit a downbeat.
        if (playStart) {
            buildVars(vars, 0, sr, secPerSample, gateVal, freqHz, sigChans, sigCount);
            sink.sampleOffset = 0;
            runtime->onStart(vars, &sink);
        }

        ScriptBlockCtx ctx;
        ctx.sampleRate = sr;
        ctx.numSamples = numSamples;
        ctx.bpm        = transport.bpm;
        ctx.playing    = transport.playing;
        ctx.posSamples = transport.positionSamples;
        ctx.posBeats   = transport.positionBeats();
        ctx.note       = lastNoteOn;
        ctx.vel        = lastVelocity;
        ctx.gate       = gateVal;
        ctx.freq       = freqHz;
        ctx.sig        = &sigChans;
        ctx.sigCount   = sigCount;
        ctx.sink       = &sink;
        runtime->runBlock(ctx);
    } else {
        // ---- Per-sample dispatch (Builtin / Lua sample-rate) ----------------
        // Run the start hook once, at sample offset 0, with a live sink.
        if (playStart) {
            buildVars(vars, 0, sr, secPerSample, gateVal, freqHz, sigChans, sigCount);
            sink.sampleOffset = 0;
            runtime->onStart(vars, &sink);
        }

        for (int s = 0; s < numSamples; ++s) {
            buildVars(vars, s, sr, secPerSample, gateVal, freqHz, sigChans, sigCount);
            sink.sampleOffset = s;
            runtime->runMidi(vars, &sink);
        }
    }

    // Drain scheduled note-offs once per block. samplesRemaining is measured from
    // THIS block's start; subtract the block length and emit any that came due
    // (at their within-block offset). Survivors carry over to the next block.
    for (auto it = pendingOffs.begin(); it != pendingOffs.end();) {
        it->samplesRemaining -= numSamples;
        if (it->samplesRemaining <= 0) {
            int off = juce::jlimit(0, numSamples - 1,
                (int)(numSamples + it->samplesRemaining));
            midiIn.addEvent(juce::MidiMessage::noteOff(it->channel, it->pitch), off);
            it = pendingOffs.erase(it);
        } else ++it;
    }
}

// -----------------------------------------------------------------------------
// MidiChannelFilterProcessor
// -----------------------------------------------------------------------------

void MidiChannelFilterProcessor::processBlock(juce::AudioBuffer<float>&,
                                              juce::MidiBuffer& midi) {
    // Keep only events on the target channel; rewrite them to channel 1 so the
    // downstream cable is a clean single-stream. Channel-less messages (clock,
    // etc.) are dropped - a MidiScript node never emits them.
    juce::MidiBuffer kept;
    for (auto meta : midi) {
        auto msg = meta.getMessage();
        if (msg.getChannel() == targetChannel) {
            msg.setChannel(1);
            kept.addEvent(msg, meta.samplePosition);
        }
    }
    midi.swapWith(kept);
}

} // namespace SoundShop

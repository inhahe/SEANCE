#include "script_runtime.h"

#ifdef HAS_LUA

#include "lua_prelude.h"
#include "music_theory.h"
#include "waveform_bank.h"
#include "warp.h"
#include "buffer_warp.h"
#include <juce_core/juce_core.h>
#include <cmath>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace SoundShop {

// -----------------------------------------------------------------------------
// LuaRuntime - embedded Lua 5.4.
// -----------------------------------------------------------------------------
//
// Program model (mirrors the Builtin sections):
//   * Top-level code runs ONCE when the script loads / resets - this is your
//     init (seed globals, build tables). Emit calls here are no-ops.
//   * function start()  - optional; runs once each time the transport starts.
//   * function loop()   - the per-sample / per-block body, runs every sample
//     (PerSample) or once per block (PerBlock). Signal role: return the output
//     value (PerSample) or call out(i, value) for each sample (PerBlock). MIDI
//     role: call note()/cc()/...
//   * function stream() - the STREAMING (coroutine pull-model) body, an
//     alternative to loop() for PerBlock. The program owns its control flow: it
//     loops forever, PULLS input with pull()/poll()/pullblock() and PUSHES output
//     with out()/outblock(), and consumes MIDI event-driven with pollmidi().
//     pull() blocks (suspends the coroutine) until the host has the next block of
//     input, so "the next sample doesn't exist yet" is handled by suspension
//     rather than by re-invoking the whole program. Started once (off the audio
//     thread) and resumed each block. The SAME pull/poll API serves audio-rate
//     work (grab one sample per loop) and block-rate work (grab a whole block).
//
// A program defines exactly one of loop() / stream() / generate() as its body.
//
// Globals persist between calls (Lua globals ARE the persistent state).
//
// Real-time safety: a script that uses only numbers + math never allocates, so
// the GC never runs and per-sample calls are cheap. A script that builds tables
// or strings every sample creates garbage and can stutter - hence the editor's
// PerSample-Lua warning, and the PerBlock option for heavy / many-instance use.
class LuaRuntime : public IScriptRuntime {
public:
    LuaRuntime(ScriptRole r, ScriptRate rt) : role(r), rate(rt) {}
    ~LuaRuntime() override { if (L) lua_close(L); }

    bool load(const std::string& src, std::string& error) override {
        source = src;
        rebuild();
        if (errored) { error = lastErr; return false; }
        return true;
    }

    void reset() override { rebuild(); }

    void onStart(const ScriptVars& vars, IExprEmitSink* sink) override {
        if (!L || errored || startRef == LUA_NOREF) return;
        curSink = sink; curCtx = nullptr;
        setGlobals(vars);
        if (role == ScriptRole::Midi && sink) sink->setSampleOffset(0);
        lua_rawgeti(L, LUA_REGISTRYINDEX, startRef);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) handleError();
        curSink = nullptr;
    }

    float evalSignal(const ScriptVars& vars) override {
        if (!L || errored || loopRef == LUA_NOREF) return 0.0f;
        curSink = nullptr; curCtx = nullptr;
        setGlobals(vars);
        lua_rawgeti(L, LUA_REGISTRYINDEX, loopRef);
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) { handleError(); return 0.0f; }
        float v = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        if (!std::isfinite(v)) v = 0.0f;
        // Control signals are unipolar 0..1 on the wire (see signal_modulation.h).
        return juce::jlimit(0.0f, 1.0f, v);
    }

    void runMidi(const ScriptVars& vars, IExprEmitSink* sink) override {
        if (!L || errored || loopRef == LUA_NOREF) return;
        curSink = sink; curCtx = nullptr;
        setGlobals(vars);
        lua_rawgeti(L, LUA_REGISTRYINDEX, loopRef);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) handleError();
        curSink = nullptr;
    }

    void runUnified(const ScriptVars& vars, IExprEmitSink* sink,
                    float* outs, int outCount) override {
        for (int i = 0; i < outCount; ++i) outs[i] = (i == 0) ? 0.0f : 0.0f;
        if (!L || errored || loopRef == LUA_NOREF) return;
        curSink = sink; curCtx = nullptr;
        setGlobals(vars);
        lua_rawgeti(L, LUA_REGISTRYINDEX, loopRef);
        // loop() may return o1 directly (single-output convenience) and/or set
        // globals o1..oP for the multi-output case. Side-effect emit calls land
        // through curSink.
        float ret = 0.0f;
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) { handleError(); curSink = nullptr; return; }
        if (lua_isnumber(L, -1)) ret = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        curSink = nullptr;
        for (int i = 0; i < outCount; ++i) {
            lua_getglobal(L, ("o" + std::to_string(i + 1)).c_str());
            float v = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1)
                                          : (i == 0 ? ret : 0.0f);
            lua_pop(L, 1);
            if (!std::isfinite(v)) v = 0.0f;
            outs[i] = juce::jlimit(0.0f, 1.0f, v);
        }
    }

    void runBlock(const ScriptBlockCtx& ctx) override {
        if (!L || errored) return;
        // Streaming program (defines stream()): resume the persistent coroutine,
        // which pulls input / pushes output at its own pace and suspends mid-block.
        if (streaming) {
            curSink = ctx.sink; curCtx = &ctx;
            runStreamBlock(ctx);
            curSink = nullptr; curCtx = nullptr;
            return;
        }
        // Classic per-block program (defines loop()): one interpreter call fills
        // the whole block; the script addresses samples by absolute index out(i,v).
        if (loopRef == LUA_NOREF) return;
        curSink = ctx.sink; curCtx = &ctx;
        setBlockGlobals(ctx);
        lua_rawgeti(L, LUA_REGISTRYINDEX, loopRef);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) handleError();
        curSink = nullptr; curCtx = nullptr;
    }

    bool runGenerate(const std::vector<int>& dims, float* data,
                     std::string& error) override {
        if (!L || errored) { error = lastErr.empty() ? "script failed to load" : lastErr; return false; }
        if (generateRef == LUA_NOREF) {
            error = "whole-grid mode needs a function generate() in your program";
            return false;
        }
        long long total = 1;
        for (int d : dims) total *= (d > 0 ? d : 1);

        genData  = data;
        genDims  = &dims;
        genTotal = total;

        // Expose grid shape to the script: dims (1-based table), nd, total.
        lua_newtable(L);
        for (size_t i = 0; i < dims.size(); ++i) {
            lua_pushinteger(L, (lua_Integer)dims[i]);
            lua_rawseti(L, -2, (lua_Integer)(i + 1));
        }
        lua_setglobal(L, "dims");
        lua_pushinteger(L, (lua_Integer)dims.size()); lua_setglobal(L, "nd");
        lua_pushinteger(L, (lua_Integer)total);       lua_setglobal(L, "total");

        lua_rawgeti(L, LUA_REGISTRYINDEX, generateRef);
        bool ok = (lua_pcall(L, 0, 0, 0) == LUA_OK);
        if (!ok) { handleError(); error = lastErr; }

        genData = nullptr; genDims = nullptr; genTotal = 0;
        return ok;
    }

    bool supportsPerSample() const override { return true; }
    bool supportsPerBlock()  const override { return true; }
    bool isStreaming()       const override { return streaming; }
    std::string getError() const override { return lastErr; }

    // --- whole-grid generation state (set during runGenerate) ---
    float* genData = nullptr;
    const std::vector<int>* genDims = nullptr;
    long long genTotal = 0;

    // --- accessed by the C trampolines ---
    ScriptRole role;
    ScriptRate rate;
    IExprEmitSink* curSink = nullptr;
    const ScriptBlockCtx* curCtx = nullptr;

    // --- streaming (coroutine pull-model) state, read/written by trampolines ---
    // A streaming script (one that defines `function stream()`) owns its own
    // control flow: it loops forever and PULLS input via pull()/poll() and PUSHES
    // output via out()/outblock(), suspending (Lua coroutine yield) when it runs
    // out of input for the current block and resuming when the host hands it the
    // next block. These fields are the bridge between the host's per-block
    // ScriptBlockCtx and the suspended coroutine.
    bool inStream      = false;  // true only while resuming the stream coroutine
    int  streamPos     = -1;     // current within-block sample index (read==write cursor)
    int  midiCursor    = 0;      // # of midiIn events already drained this block
    bool blockConsumed = false;  // pullblock()/pollblock() whole-block guard
    // Mode of the pending pullblock()/pollblock() call, preserved across a yield
    // (the original Lua args are gone once the coroutine suspends): >0 = a single
    // 1-based input pin requested (returns one flat array), 0 = no pin given (the
    // structured form: a list-of-lists of every input pin's samples plus a list
    // of this block's MIDI-input events).
    int  pullPin       = 0;

    int readOut() {
        lua_getglobal(L, "out");
        int o = (int)lua_tonumber(L, -1);
        lua_pop(L, 1);
        return o;
    }

    // Public accessor for the protected shape sampler so the static C
    // trampolines (free functions, not members) can reach it.
    float callShape(float pos) const { return shape ? shape(pos) : 0.0f; }

    // Public accessor for the protected note->Hz mapper (project tuning).
    float callNoteFreq(int note) const { return noteFreq(note); }

private:
    lua_State* L = nullptr;
    std::string source;
    int loopRef = LUA_NOREF, startRef = LUA_NOREF, generateRef = LUA_NOREF;
    bool errored = false;
    std::string lastErr;

    // Streaming coroutine. `co` is a Lua thread spun off the main state in
    // rebuild() (NOT on the audio thread) when the program defines stream();
    // runBlock() resumes it each block. `coRef` anchors the thread against GC.
    lua_State* co        = nullptr;
    int        streamRef = LUA_NOREF;   // the stream() function
    int        coRef     = LUA_NOREF;   // registry anchor for the thread
    bool       streaming = false;       // program defined stream()
    bool       streamDone = false;      // stream() returned / errored -> output holds 0
    void runStreamBlock(const ScriptBlockCtx& ctx);

    void handleError() {
        errored = true;
        const char* m = lua_tostring(L, -1);
        lastErr = m ? m : "Lua error";
        lua_pop(L, 1);
    }

    void setGlobals(const ScriptVars& vars) {
        for (auto& kv : vars) {
            lua_pushnumber(L, (lua_Number)kv.second);
            lua_setglobal(L, kv.first.c_str());
        }
    }

    void setBlockGlobals(const ScriptBlockCtx& c) {
        auto setn = [&](const char* k, double v) {
            lua_pushnumber(L, (lua_Number)v); lua_setglobal(L, k);
        };
        double sr = c.sampleRate > 0 ? c.sampleRate : 44100.0;
        double tStart = (double)c.posSamples / sr;
        double secPerBlock = (double)c.numSamples / sr;
        double beatPerBlock = c.bpm / 60.0 * secPerBlock;
        setn("n", c.numSamples);
        setn("sr", sr);
        setn("dt", 1.0 / sr);
        setn("bpm", c.bpm);
        setn("playing", c.playing ? 1.0 : 0.0);
        setn("t", tStart);
        setn("tStart", tStart);
        setn("tEnd", tStart + secPerBlock);
        setn("beat", c.posBeats);
        setn("beatStart", c.posBeats);
        setn("beatEnd", c.posBeats + beatPerBlock);
        setn("note", c.note);
        setn("vel", c.vel);
        setn("gate", c.gate);
        setn("freq", c.freq);
        setn("rate", c.rate);
        // Block-start signal values for convenience (s1..sN); use sig(k,i) for
        // per-sample access inside the loop.
        for (int i = 0; i < c.sigCount; ++i) {
            const float* p = (c.sig && i < (int)c.sig->size()) ? (*c.sig)[i] : nullptr;
            lua_pushnumber(L, p ? (lua_Number)p[0] : 0.0);
            lua_setglobal(L, ("s" + std::to_string(i + 1)).c_str());
        }
    }

    void rebuild();
    void registerApi();
};

// Retrieve the runtime instance stashed in the lua_State's extra space.
static LuaRuntime* self_(lua_State* L) {
    return *static_cast<LuaRuntime**>(lua_getextraspace(L));
}

// Read a note argument that may be a number (MIDI pitch, possibly fractional)
// or a note-name string ("C4", "c#4", "Bb3", "C" with the default octave). A
// string that fails to parse yields -1 (out of range) so the note simply gets
// clamped/dropped downstream rather than silently becoming pitch 0.
static float readNoteArg(lua_State* L, int idx, float def) {
    if (lua_type(L, idx) == LUA_TSTRING) {
        const char* s = lua_tostring(L, idx);
        return (float)MusicTheory::parseNoteName(s ? s : "");
    }
    return (float)luaL_optnumber(L, idx, def);
}

// ---- MIDI emit trampolines --------------------------------------------------
static void setOffsetIfBlock(lua_State* L, LuaRuntime* self, int argIndex) {
    // Block-rate and streaming scripts both emit at sample-accurate offsets. In
    // streaming mode the natural default offset is the current cursor position
    // (the sample the coroutine is processing right now); an explicit offset
    // argument still overrides it. In classic per-block mode the default is 0.
    if ((self->rate == ScriptRate::PerBlock || self->inStream) && self->curSink) {
        int n = self->curCtx ? self->curCtx->numSamples : 0;
        int def = self->inStream ? juce::jmax(0, self->streamPos) : 0;
        int off = (int)luaL_optinteger(L, argIndex, def);
        if (n > 0) off = (int)juce::jlimit(0, n - 1, off);
        else off = juce::jmax(0, off);
        self->curSink->setSampleOffset(off);
    }
}

static int l_note(lua_State* L) {
    LuaRuntime* self = self_(L);
    if (!self->curSink) return 0;
    float p = readNoteArg(L, 1, 0);
    float v = (float)luaL_optnumber(L, 2, 0);
    float d = (float)luaL_optnumber(L, 3, 0.1);
    setOffsetIfBlock(L, self, 4);
    self->curSink->emitNote(self->readOut(), p, v, d);
    return 0;
}
static int l_noteon(lua_State* L) {
    LuaRuntime* self = self_(L);
    if (!self->curSink) return 0;
    float p = readNoteArg(L, 1, 0);
    float v = (float)luaL_optnumber(L, 2, 0);
    setOffsetIfBlock(L, self, 3);
    self->curSink->emitNoteOn(self->readOut(), p, v);
    return 0;
}
static int l_noteoff(lua_State* L) {
    LuaRuntime* self = self_(L);
    if (!self->curSink) return 0;
    float p = readNoteArg(L, 1, 0);
    setOffsetIfBlock(L, self, 2);
    self->curSink->emitNoteOff(self->readOut(), p);
    return 0;
}
static int l_cc(lua_State* L) {
    LuaRuntime* self = self_(L);
    if (!self->curSink) return 0;
    float n = (float)luaL_optnumber(L, 1, 0);
    float v = (float)luaL_optnumber(L, 2, 0);
    setOffsetIfBlock(L, self, 3);
    self->curSink->emitCC(self->readOut(), n, v);
    return 0;
}
static int l_bend(lua_State* L) {
    LuaRuntime* self = self_(L);
    if (!self->curSink) return 0;
    float v = (float)luaL_optnumber(L, 1, 0);
    setOffsetIfBlock(L, self, 2);
    self->curSink->emitBend(self->readOut(), v);
    return 0;
}

// ---- Signal / shared trampolines --------------------------------------------
// Output buffer for streaming pin `pin` (0-based); defined below, forward-declared
// here because l_out (streaming branch) writes through it.
static float* streamOutBuf(const ScriptBlockCtx* c, int pin);
static int l_out(lua_State* L) {
    LuaRuntime* self = self_(L);
    if (!self->curCtx || !self->curCtx->out) return 0;
    // Streaming mode: out(v) writes the CURRENT cursor sample (the one just
    // pull()ed) to output pin o1; out(pin, v) writes that sample to output pin
    // `pin` (1-based: o1, o2, ...), so a multi-output node drives each o1..oP
    // independently — symmetric with the 1-based s1..sN signal inputs.
    if (self->inStream) {
        const ScriptBlockCtx* c = self->curCtx;
        int pin = 0;   // 0-based buffer index
        float v;
        if (lua_gettop(L) >= 2) {
            pin = (int)luaL_checkinteger(L, 1) - 1;   // 1-based o1.. -> 0-based
            v   = (float)luaL_checknumber(L, 2);
        } else {
            v   = (float)luaL_checknumber(L, 1);
        }
        if (!std::isfinite(v)) v = 0.0f;
        float* buf = streamOutBuf(c, pin);
        int i = self->streamPos;
        if (buf && i >= 0 && i < c->numSamples)
            buf[i] = juce::jlimit(0.0f, 1.0f, v);
        return 0;
    }
    // Classic per-block mode: out(i, v) writes by ABSOLUTE sample index.
    int i = (int)luaL_checkinteger(L, 1);
    float v = (float)luaL_checknumber(L, 2);
    if (i >= 0 && i < self->curCtx->numSamples)
        // Control signals are unipolar 0..1 on the wire (signal_modulation.h).
        self->curCtx->out[i] = juce::jlimit(0.0f, 1.0f, v);
    return 0;
}
static int l_sig(lua_State* L) {
    LuaRuntime* self = self_(L);
    int k = (int)luaL_checkinteger(L, 1);     // 1-based: s1, s2, ...
    int i = (int)luaL_optinteger(L, 2, 0);
    float val = 0.0f;
    const ScriptBlockCtx* c = self->curCtx;
    if (c && c->sig) {
        int idx = k - 1;
        if (idx >= 0 && idx < c->sigCount) {
            const float* p = (idx < (int)c->sig->size()) ? (*c->sig)[idx] : nullptr;
            if (p && i >= 0 && i < c->numSamples) val = p[i];
        }
    }
    lua_pushnumber(L, val);
    return 1;
}
static int l_shape(lua_State* L) {
    LuaRuntime* self = self_(L);
    float pos = (float)luaL_optnumber(L, 1, 0);
    lua_pushnumber(L, self->callShape(pos));
    return 1;
}

// ---- Streaming pull/push trampolines (coroutine pull-model) ------------------
//
// Active only inside a stream() coroutine (self->inStream). The script owns its
// loop and calls these to advance through the stream one sample (pull/poll) or a
// whole block (pullblock/pollblock) at a time, suspending the coroutine when the
// current block is exhausted. The SAME functions serve audio-rate work (loop
// pulling one sample) and block-rate work (loop pulling a whole block) - the only
// difference is how much the script grabs per iteration.
//
// streamPos is the shared read==write cursor: pull()/poll() advance it and read
// the input pins at the new position; out() writes the output buffer AT that same
// position, so a `local x = pull(); out(f(x))` transducer is automatically
// sample-aligned. Outside streaming these are inert (pull/poll return nil, wait
// is a no-op) so a non-streaming program referencing them never errors or yields
// from outside a coroutine.

// Output buffer for streaming pin `pin` (0-based): the per-pin buffer from the
// multi-output array when present, else the single `out` buffer for pin 0, else
// null (out-of-range pin / no output -> the write is dropped).
static float* streamOutBuf(const ScriptBlockCtx* c, int pin) {
    if (!c) return nullptr;
    if (c->outs && pin >= 0 && pin < c->outCount && pin < (int)c->outs->size())
        return (*c->outs)[(size_t)pin];
    if (pin == 0) return c->out;
    return nullptr;
}

// Push the input-pin values (s1..sN) at sample `pos` as multiple return values.
// A node with no signal inputs still returns a single 0, so `local x = pull()`
// works for a pure generator that pulls only to advance the time cursor.
static int pushStreamInputs(lua_State* L, const ScriptBlockCtx* c, int pos) {
    if (!c || c->sigCount <= 0) { lua_pushnumber(L, 0.0); return 1; }
    int pushed = 0;
    for (int k = 0; k < c->sigCount; ++k) {
        const float* p = (c->sig && k < (int)c->sig->size()) ? (*c->sig)[k] : nullptr;
        lua_pushnumber(L, (p && pos >= 0 && pos < c->numSamples) ? (lua_Number)p[pos] : 0.0);
        ++pushed;
    }
    return pushed;
}

// pull() continuation: runs when the coroutine is resumed for the NEXT block
// (host has reset streamPos to -1). Advance to that block's first sample.
static int k_pull(lua_State* L, int /*status*/, lua_KContext /*ctx*/) {
    LuaRuntime* self = self_(L);
    const ScriptBlockCtx* c = self->curCtx;
    self->streamPos++;                       // -1 -> 0 (first sample of new block)
    if (!c || self->streamPos >= c->numSamples)
        return lua_yieldk(L, 0, 0, k_pull);  // empty block; keep waiting
    return pushStreamInputs(L, c, self->streamPos);
}
// pull() -> next input sample(s), BLOCKING. Advances the cursor; if the current
// block is exhausted it suspends the coroutine (the host returns from runBlock)
// and resumes here when the next block arrives. Returns one value per signal-in
// pin (or a single 0 when there are none).
static int l_pull(lua_State* L) {
    LuaRuntime* self = self_(L);
    const ScriptBlockCtx* c = self->curCtx;
    if (!self->inStream || !c) { lua_pushnil(L); return 1; }
    if (self->streamPos + 1 < c->numSamples) {
        self->streamPos++;
        return pushStreamInputs(L, c, self->streamPos);
    }
    return lua_yieldk(L, 0, 0, k_pull);      // block exhausted -> wait for next
}
// poll() -> next input sample(s) if one is still available this block, else nil.
// Non-blocking: never suspends. Pair with wait() to hand the block back.
static int l_poll(lua_State* L) {
    LuaRuntime* self = self_(L);
    const ScriptBlockCtx* c = self->curCtx;
    if (!self->inStream || !c) { lua_pushnil(L); return 1; }
    if (self->streamPos + 1 < c->numSamples) {
        self->streamPos++;
        return pushStreamInputs(L, c, self->streamPos);
    }
    lua_pushnil(L);
    return 1;
}
// wait() continuation: nothing to return; the loop simply resumes in the new block.
static int k_wait(lua_State* L, int /*status*/, lua_KContext /*ctx*/) { (void)L; return 0; }
// wait() -> suspend the coroutine until the next block (no value). The explicit
// hand-back primitive for poll()-based loops: poll() until nil, then wait().
static int l_wait(lua_State* L) {
    LuaRuntime* self = self_(L);
    if (!self->inStream) return 0;           // inert outside streaming (never yields)
    return lua_yieldk(L, 0, 0, k_wait);
}

// Push a flat Lua array of input-pin `pin0` (0-based) samples over
// [start, numSamples). Pin out of range / no input pushes zeros.
static void pushPinArray(lua_State* L, const ScriptBlockCtx* c, int pin0, int start) {
    const float* p = (c->sig && pin0 >= 0 && pin0 < c->sigCount
                      && pin0 < (int)c->sig->size()) ? (*c->sig)[pin0] : nullptr;
    lua_newtable(L);
    int idx = 1;
    for (int i = start; i < c->numSamples; ++i) {
        lua_pushnumber(L, p ? (lua_Number)p[i] : 0.0);
        lua_rawseti(L, -2, idx++);
    }
}
// Push the unified MIDI-input event list for events at offset >= `start`: an
// array of self-describing tables {kind, offset, a, b, idx}. idx is the 1-based
// MIDI-INPUT pin the event arrived on (the same shape param events would take if
// represented sparsely). Empty array when there are no events.
static void pushMidiEventList(lua_State* L, const ScriptBlockCtx* c, int start) {
    lua_newtable(L);
    int n = (c && c->midiIn) ? c->midiInCount : 0;
    int out = 1;
    for (int i = 0; i < n; ++i) {
        const ScriptMidiEvent& e = (*c->midiIn)[(size_t)i];
        if (e.offset < start) continue;
        const char* kind = e.kind == 1 ? "on" : e.kind == 0 ? "off"
                         : e.kind == 2 ? "cc" : "bend";
        lua_newtable(L);
        lua_pushstring(L, kind);     lua_setfield(L, -2, "kind");
        lua_pushinteger(L, e.offset); lua_setfield(L, -2, "offset");
        lua_pushinteger(L, e.a);      lua_setfield(L, -2, "a");
        lua_pushnumber(L, e.b);       lua_setfield(L, -2, "b");
        lua_pushinteger(L, e.inIndex + 1); lua_setfield(L, -2, "idx");
        lua_rawseti(L, -2, out++);
    }
}
// Build the whole-block input snapshot starting at sample `start` and leave it on
// the stack, marking the block consumed. Two shapes, selected by self->pullPin:
//   pullPin > 0  -> ONE flat array of that 1-based pin's samples (1 return value).
//   pullPin == 0 -> the STRUCTURED form (2 return values):
//                     params : list-of-lists, params[k] = pin k's sample array
//                              (k 1-based; params has one entry per input pin,
//                              all the same length since param inputs are
//                              synchronized), and
//                     events : list of this block's MIDI-input event tables.
static int buildStreamBlockTable(lua_State* L, LuaRuntime* self,
                                 const ScriptBlockCtx* c, int start) {
    self->streamPos    = c->numSamples - 1;  // out() now targets the block tail
    self->blockConsumed = true;
    if (self->pullPin > 0) {
        pushPinArray(L, c, self->pullPin - 1, start);
        return 1;
    }
    int nPins = juce::jmax(0, c->sigCount);
    lua_createtable(L, nPins, 0);
    for (int k = 0; k < nPins; ++k) {
        pushPinArray(L, c, k, start);
        lua_rawseti(L, -2, k + 1);
    }
    pushMidiEventList(L, c, start);
    return 2;
}
// pullblock() continuation: resumed in the next block (host reset blockConsumed).
static int k_pullblock(lua_State* L, int /*status*/, lua_KContext /*ctx*/) {
    LuaRuntime* self = self_(L);
    const ScriptBlockCtx* c = self->curCtx;
    if (!c || c->numSamples <= 0) return lua_yieldk(L, 0, 0, k_pullblock);
    return buildStreamBlockTable(L, self, c, 0);
}
// pullblock([pin]) -> whole-block input snapshot, BLOCKING. With a numeric `pin`
// it returns one flat array of that 1-based input pin's samples (convenient for a
// single-input transducer). With NO argument it returns the structured form:
//   local params, events = pullblock()
//   -- params[k][s] = sample s of input pin k;  events = {{kind,offset,a,b,idx},..}
// The whole-block (block-rate) counterpart to pull(): one grab per resume, then it
// suspends until the next block. Pair with outblock().
static int l_pullblock(lua_State* L) {
    LuaRuntime* self = self_(L);
    const ScriptBlockCtx* c = self->curCtx;
    if (!self->inStream || !c) { lua_pushnil(L); return 1; }
    self->pullPin = (lua_type(L, 1) == LUA_TNUMBER) ? (int)lua_tointeger(L, 1) : 0;
    if (self->blockConsumed) return lua_yieldk(L, 0, 0, k_pullblock);
    return buildStreamBlockTable(L, self, c, 0);
}
// pollblock([pin]) -> the REMAINING input this block (same two shapes as
// pullblock), or nil if none is left. Non-blocking. Pair with wait().
static int l_pollblock(lua_State* L) {
    LuaRuntime* self = self_(L);
    const ScriptBlockCtx* c = self->curCtx;
    if (!self->inStream || !c) { lua_pushnil(L); return 1; }
    if (self->blockConsumed || self->streamPos + 1 >= c->numSamples) {
        lua_pushnil(L);
        return 1;
    }
    self->pullPin = (lua_type(L, 1) == LUA_TNUMBER) ? (int)lua_tointeger(L, 1) : 0;
    return buildStreamBlockTable(L, self, c, self->streamPos + 1);
}
// outblock([pin,] t) -> write the array `t` (t[1..]) to output pin `pin`
// (1-based: o1 by default) starting at sample 0, clamped to the block length.
// The whole-block output partner to out(v); values are clamped to the unipolar
// 0..1 wire range.
static int l_outblock(lua_State* L) {
    LuaRuntime* self = self_(L);
    const ScriptBlockCtx* c = self->curCtx;
    if (!self->inStream || !c) return 0;
    int pin = 0, tblArg = 1;
    if (lua_type(L, 1) == LUA_TNUMBER) { pin = (int)lua_tointeger(L, 1) - 1; tblArg = 2; }
    luaL_checktype(L, tblArg, LUA_TTABLE);
    float* buf = streamOutBuf(c, pin);
    if (!buf) return 0;
    int len = (int)lua_rawlen(L, tblArg);
    int n = juce::jmin(len, c->numSamples);
    for (int i = 0; i < n; ++i) {
        lua_rawgeti(L, tblArg, i + 1);
        float v = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        if (!std::isfinite(v)) v = 0.0f;
        buf[i] = juce::jlimit(0.0f, 1.0f, v);
    }
    return 0;
}
// pollmidi() -> kind, offset, a, b, idx for the next MIDI-input event that the
// stream cursor has reached (offset <= streamPos), or nil if none are due yet.
//   kind   : "on" | "off" | "cc" | "bend"
//   offset : sample offset within the block
//   a, b   : note/cc number, velocity/value (see midievent())
//   idx    : 1-based MIDI-INPUT pin the event arrived on (1 today; ready for the
//            multi-MIDI-input model where M > 1 pins share one event stream)
// idx is LAST so the common `local kind,off,a,b = pollmidi()` idiom keeps working
// and a script that cares about the source pin writes `...,idx = pollmidi()`.
// Drains the block's event list in order as the cursor advances, giving
// EVENT-DRIVEN MIDI consumption inside a streaming loop:
//   pull(); local k = pollmidi(); while k do ...; k = pollmidi() end
static int l_pollmidi(lua_State* L) {
    LuaRuntime* self = self_(L);
    const ScriptBlockCtx* c = self->curCtx;
    if (!c || !c->midiIn || self->midiCursor >= c->midiInCount) {
        lua_pushnil(L);
        return 1;
    }
    const ScriptMidiEvent& e = (*c->midiIn)[(size_t)self->midiCursor];
    if (self->streamPos < e.offset) { lua_pushnil(L); return 1; }  // not reached yet
    self->midiCursor++;
    const char* kind = e.kind == 1 ? "on"
                     : e.kind == 0 ? "off"
                     : e.kind == 2 ? "cc"
                                   : "bend";
    lua_pushstring(L, kind);
    lua_pushinteger(L, e.offset);
    lua_pushinteger(L, e.a);
    lua_pushnumber(L, e.b);
    lua_pushinteger(L, e.inIndex + 1);   // 0-based storage -> 1-based script pin
    return 5;
}

// midiin() -> number of MIDI-input events this block (0 outside block mode, so a
// per-sample program just sees an empty list and the loop body never runs). Lets
// a streaming script react EVENT-DRIVEN instead of polling note/vel/gate.
static int l_midiin(lua_State* L) {
    LuaRuntime* self = self_(L);
    int n = self->curCtx ? self->curCtx->midiInCount : 0;
    lua_pushinteger(L, n);
    return 1;
}
// midievent(i) -> kind, offset, a, b  (1-based i; nil if out of range).
//   kind  : "on" | "off" | "cc" | "bend"
//   offset: sample position within the block
//   a     : note number (on/off) or controller number (cc); 0 for bend
//   b     : velocity 0..1 (on) | 0 (off) | CC value 0..1 (cc) | bend -1..1
//   idx   : 1-based MIDI-input pin the event arrived on (always 1 with a single
//           MIDI input). Returned LAST so the 4-value idiom keeps working.
// Typical use inside loop():  for k=1,midiin() do local kind,off,a,b = midievent(k) ... end
static int l_midievent(lua_State* L) {
    LuaRuntime* self = self_(L);
    const ScriptBlockCtx* c = self->curCtx;
    int i = (int)luaL_checkinteger(L, 1) - 1;     // 1-based -> 0-based
    if (!c || !c->midiIn || i < 0 || i >= c->midiInCount) {
        lua_pushnil(L);
        return 1;
    }
    const ScriptMidiEvent& e = (*c->midiIn)[(size_t)i];
    const char* kind = e.kind == 1 ? "on"
                     : e.kind == 0 ? "off"
                     : e.kind == 2 ? "cc"
                                   : "bend";
    lua_pushstring(L, kind);
    lua_pushinteger(L, e.offset);
    lua_pushinteger(L, e.a);
    lua_pushnumber(L, e.b);
    lua_pushinteger(L, e.inIndex + 1);            // 0-based -> 1-based input pin
    return 5;
}

// waveform(name, phase) -> sample. Read one of the ~4000 built-in factory
// single-cycle waveforms at a normalised phase in [0,1) (wraps), linearly
// interpolated, returning the raw sample in [-1,1]. The first argument is the
// waveform NAME (case-insensitive) or a numeric entry index; an unknown name /
// out-of-range index reads as 0. Same semantics as the Builtin/Python/GLSL
// waveform() so every generator language reads the bank identically. Passing an
// integer (e.g. waveforms["name"] cached in a local, or a literal id) skips the
// per-call name hash — the fast path for hot loops.
static int l_waveform(lua_State* L) {
    int id;
    if (lua_type(L, 1) == LUA_TSTRING) {
        const char* nm = lua_tostring(L, 1);
        auto& bank = WaveformBank::get();
        bank.ensureLoaded();
        id = bank.indexForName(nm ? nm : "");
    } else {
        id = (int)luaL_optinteger(L, 1, -1);
    }
    float ph = (float)luaL_optnumber(L, 2, 0);
    lua_pushnumber(L, WaveformBank::get().sampleAtPhase(id, ph));
    return 1;
}

// Resolve a warp method argument at stack index `idx`: a method NAME string
// ("softclip", "bend+", ...) via warpMethodFromName, or a numeric WarpMethod id.
// Shared by l_warpamp / l_warpphase (idx 1) and the Bucket C buffer warps (idx 2,
// after the buffer table).
static WarpMethod luaWarpMethodArg(lua_State* L, int idx = 1) {
    if (lua_type(L, idx) == LUA_TSTRING) {
        const char* nm = lua_tostring(L, idx);
        return warpMethodFromName(nm ? nm : "");
    }
    return (WarpMethod)(int)luaL_optinteger(L, idx, 0);
}

// warpamp(method, x, amount) -> x shaped by an amplitude-domain warp (clip, fold,
// saturate, quantize, ...). warpphase(method, phase, amount) -> a read position
// remapped by a phase-domain warp (bend, asym, PWM-skew, phase-distortion, ...).
// Both route to the SAME warpAmpValue/warpPhaseValue primitives the wavetable
// editor and synth voice use, so an in-script warp matches the node bit-for-bit.
// `amount` is the 0..1 morph knob (0 = identity); an unknown method is identity.
static int l_warpamp(lua_State* L) {
    WarpMethod m = luaWarpMethodArg(L);
    float x   = (float)luaL_optnumber(L, 2, 0);
    float amt = (float)luaL_optnumber(L, 3, 0);
    lua_pushnumber(L, warpAmpValue(m, x, amt));
    return 1;
}
static int l_warpphase(lua_State* L) {
    WarpMethod m = luaWarpMethodArg(L);
    float ph  = (float)luaL_optnumber(L, 2, 0);
    float amt = (float)luaL_optnumber(L, 3, 0);
    lua_pushnumber(L, warpPhaseValue(m, ph, amt));
    return 1;
}

// --- Bucket C: representation-bound whole-buffer warps ----------------------
// spectralwarp(buf, method, amount) and waveletwarp(buf, method, amount,
// [filter], [levels]) take an array of samples (a Lua table, 1-indexed), warp it
// in the FFT-magnitude / DWT-coefficient domain (see buffer_warp.h), and return a
// NEW table of the same length. These only make sense over a whole buffer, so
// they live in the buffer-capable languages (Lua / Python / WASM), not the
// per-sample expression parser.
static void luaTableToBuffer(lua_State* L, int idx, std::vector<float>& out) {
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_Integer n = (lua_Integer)lua_rawlen(L, idx);
    out.resize((size_t)(n < 0 ? 0 : n));
    for (lua_Integer i = 0; i < n; ++i) {
        lua_rawgeti(L, idx, i + 1);
        out[(size_t)i] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
}
static void luaPushBuffer(lua_State* L, const std::vector<float>& buf) {
    lua_createtable(L, (int)buf.size(), 0);
    for (size_t i = 0; i < buf.size(); ++i) {
        lua_pushnumber(L, buf[i]);
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
}
static int l_spectralwarp(lua_State* L) {
    std::vector<float> buf;
    luaTableToBuffer(L, 1, buf);
    WarpMethod m = luaWarpMethodArg(L, 2);
    float amt = (float)luaL_optnumber(L, 3, 0);
    spectralWarpBuffer(buf, m, amt);
    luaPushBuffer(L, buf);
    return 1;
}
static int l_waveletwarp(lua_State* L) {
    std::vector<float> buf;
    luaTableToBuffer(L, 1, buf);
    WarpMethod m = luaWarpMethodArg(L, 2);
    float amt = (float)luaL_optnumber(L, 3, 0);
    const char* filter = luaL_optstring(L, 4, "db4");
    int levels = (int)luaL_optinteger(L, 5, 5);
    waveletWarpBuffer(buf, m, amt, filter ? filter : "db4", levels);
    luaPushBuffer(L, buf);
    return 1;
}

// waveforms[name] -> stable entry index (integer), or -1 for an unknown name.
// Backs a Lua table whose __index metamethod resolves a factory-waveform NAME to
// the same integer id every language uses (and the id shown in the factory
// browser). The result is rawset back into the table, so a given name hashes
// through indexForName ONCE and every later lookup is a plain table read. Resolve
// once (top level, or in start() for a streaming script) and reuse the integer
// with waveform(id, phase) to avoid per-call name lookups:
//   local w = waveforms["AKWF sin"]   -- one lookup
//   ... waveform(w, phase) ...         -- hot loop, no hashing
static int l_waveforms_index(lua_State* L) {
    // L stack: 1=the waveforms table, 2=the key (waveform name).
    int id = -1;
    if (lua_type(L, 2) == LUA_TSTRING) {
        const char* nm = lua_tostring(L, 2);
        auto& bank = WaveformBank::get();
        bank.ensureLoaded();
        id = bank.indexForName(nm ? nm : "");
    }
    // Cache: table[key] = id, so subsequent reads bypass the metamethod entirely.
    lua_pushvalue(L, 2);
    lua_pushinteger(L, id);
    lua_rawset(L, 1);
    lua_pushinteger(L, id);
    return 1;
}

// ---- Whole-grid generation trampolines (active only during runGenerate) -----
// set(i, v): write cell i (0-based flat index) to v, clamped to [0,1] (the
// unipolar height contract, mapped to bipolar terrain later).
static int l_gen_set(lua_State* L) {
    LuaRuntime* self = self_(L);
    if (!self->genData) return 0;
    long long i = (long long)luaL_checkinteger(L, 1);
    float v = (float)luaL_checknumber(L, 2);
    if (i >= 0 && i < self->genTotal)
        self->genData[(size_t)i] = juce::jlimit(0.0f, 1.0f, std::isfinite(v) ? v : 0.0f);
    return 0;
}
// get(i): read cell i back (0 outside range / before it's written).
static int l_gen_get(lua_State* L) {
    LuaRuntime* self = self_(L);
    long long i = (long long)luaL_checkinteger(L, 1);
    float v = 0.0f;
    if (self->genData && i >= 0 && i < self->genTotal) v = self->genData[(size_t)i];
    lua_pushnumber(L, v);
    return 1;
}
// coord(i, axis): normalized position [0,1] of flat cell i along axis (0-based).
static int l_gen_coord(lua_State* L) {
    LuaRuntime* self = self_(L);
    long long i = (long long)luaL_checkinteger(L, 1);
    int axis = (int)luaL_checkinteger(L, 2);
    double norm = 0.0;
    if (self->genDims && axis >= 0 && axis < (int)self->genDims->size()
        && i >= 0 && i < self->genTotal) {
        const auto& d = *self->genDims;
        long long tmp = i;
        // row-major: last axis is the fastest-varying.
        int idx = 0;
        for (int a = (int)d.size() - 1; a >= 0; --a) {
            int sz = d[(size_t)a] > 0 ? d[(size_t)a] : 1;
            int coordA = (int)(tmp % sz);
            tmp /= sz;
            if (a == axis) { idx = coordA; break; }
        }
        int sz = d[(size_t)axis] > 0 ? d[(size_t)axis] : 1;
        norm = (sz > 1) ? (double)idx / (double)(sz - 1) : 0.0;
    }
    lua_pushnumber(L, norm);
    return 1;
}
// coordAxis(i, axis): INTEGER coordinate of flat cell i along axis (0-based) -
// the inverse companion to flatten(). 0 outside range. Mirrors the GLSL
// whole-grid coordAxis() so N-D index math reads the same in every language.
static int l_gen_coordAxis(lua_State* L) {
    LuaRuntime* self = self_(L);
    long long i = (long long)luaL_checkinteger(L, 1);
    int axis = (int)luaL_checkinteger(L, 2);
    long long c = 0;
    if (self->genDims && axis >= 0 && axis < (int)self->genDims->size()
        && i >= 0 && i < self->genTotal) {
        const auto& d = *self->genDims;
        long long tmp = i;
        // row-major: last axis is the fastest-varying.
        for (int a = (int)d.size() - 1; a >= 0; --a) {
            int sz = d[(size_t)a] > 0 ? d[(size_t)a] : 1;
            int coordA = (int)(tmp % sz);
            tmp /= sz;
            if (a == axis) { c = coordA; break; }
        }
    }
    lua_pushinteger(L, (lua_Integer)c);
    return 1;
}
// flatten(c0, c1, ...): row-major flat index from per-axis INTEGER coordinates,
// each clamped to [0, dim-1]. Missing trailing args count as 0; extra args are
// ignored. The inverse of coordAxis() and the CPU twin of the GLSL whole-grid
// flatten(); use it to jump to an arbitrary computed cell for get()/set().
static int l_gen_flatten(lua_State* L) {
    LuaRuntime* self = self_(L);
    long long idx = 0;
    if (self->genDims) {
        const auto& d = *self->genDims;
        int n = (int)d.size();
        int nargs = lua_gettop(L);
        for (int a = 0; a < n; ++a) {
            int sz = d[(size_t)a] > 0 ? d[(size_t)a] : 1;
            long long c = (a + 1 <= nargs) ? (long long)lua_tointeger(L, a + 1) : 0;
            if (c < 0) c = 0; else if (c > sz - 1) c = sz - 1;
            idx = idx * sz + c;  // Horner form, last axis fastest
        }
    }
    lua_pushinteger(L, (lua_Integer)idx);
    return 1;
}
// neighbor(i, axis, delta): flat index of the cell delta steps from i along
// axis, clamped to the edge. Compose for diagonals/N-D stencils. Matches the
// GLSL whole-grid neighbor(); on the CPU you read it back with get(neighbor(...)).
static int l_gen_neighbor(lua_State* L) {
    LuaRuntime* self = self_(L);
    long long i = (long long)luaL_checkinteger(L, 1);
    int axis = (int)luaL_checkinteger(L, 2);
    long long delta = (long long)luaL_optinteger(L, 3, 0);
    long long result = i;
    if (self->genDims && axis >= 0 && axis < (int)self->genDims->size()
        && i >= 0 && i < self->genTotal) {
        const auto& d = *self->genDims;
        int n = (int)d.size();
        std::vector<long long> co((size_t)n, 0);
        long long tmp = i;
        for (int a = n - 1; a >= 0; --a) {
            int sz = d[(size_t)a] > 0 ? d[(size_t)a] : 1;
            co[(size_t)a] = tmp % sz;
            tmp /= sz;
        }
        int sz = d[(size_t)axis] > 0 ? d[(size_t)axis] : 1;
        long long c = co[(size_t)axis] + delta;
        if (c < 0) c = 0; else if (c > sz - 1) c = sz - 1;
        co[(size_t)axis] = c;
        long long idx = 0;
        for (int a = 0; a < n; ++a) {
            int s = d[(size_t)a] > 0 ? d[(size_t)a] : 1;
            idx = idx * s + co[(size_t)a];
        }
        result = idx;
    }
    lua_pushinteger(L, (lua_Integer)result);
    return 1;
}
// getAt(c0, c1, ...): DIRECT N-D read - the cell value at per-axis INTEGER
// coordinates, with no manual flatten(). Each coordinate is EDGE-CLAMPED to
// [0, dim-1], so reads past a border replicate the nearest edge cell (the useful
// default for N-D stencils/convolution). Missing trailing coords count as 0;
// extras are ignored. Returns 0 outside a whole-grid generate().
static int l_gen_getAt(lua_State* L) {
    LuaRuntime* self = self_(L);
    float v = 0.0f;
    if (self->genData && self->genDims) {
        const auto& d = *self->genDims;
        int n = (int)d.size();
        int nargs = lua_gettop(L);
        long long idx = 0;
        for (int a = 0; a < n; ++a) {
            int sz = d[(size_t)a] > 0 ? d[(size_t)a] : 1;
            long long c = (a + 1 <= nargs) ? (long long)lua_tointeger(L, a + 1) : 0;
            if (c < 0) c = 0; else if (c > sz - 1) c = sz - 1;
            idx = idx * sz + c;  // clamped coords => idx always in [0, total-1]
        }
        if (idx >= 0 && idx < self->genTotal) v = self->genData[(size_t)idx];
    }
    lua_pushnumber(L, v);
    return 1;
}
// setAt(c0, c1, ..., v): DIRECT N-D write - store v (clamped to [0,1]) at the
// cell at per-axis INTEGER coordinates. The value is the argument right after
// the nd coordinates (i.e. arg nd+1). Unlike getAt's clamped reads, an
// OUT-OF-RANGE coordinate makes the write a no-op (mirrors set(i, v)), so an
// off-by-one loop never silently clobbers an edge cell.
static int l_gen_setAt(lua_State* L) {
    LuaRuntime* self = self_(L);
    if (!self->genData || !self->genDims) return 0;
    const auto& d = *self->genDims;
    int n = (int)d.size();
    float v = (float)luaL_optnumber(L, n + 1, 0.0);  // value follows the nd coords
    long long idx = 0;
    for (int a = 0; a < n; ++a) {
        int sz = d[(size_t)a] > 0 ? d[(size_t)a] : 1;
        long long c = (long long)lua_tointeger(L, a + 1);
        if (c < 0 || c > sz - 1) return 0;            // OOB coord => ignore write
        idx = idx * sz + c;
    }
    if (idx >= 0 && idx < self->genTotal)
        self->genData[(size_t)idx] = juce::jlimit(0.0f, 1.0f, std::isfinite(v) ? v : 0.0f);
    return 0;
}

// ---- Note-name / frequency conversion ---------------------------------------
// notenum("C4") -> 60   |   notenum("C", 4) -> 60   |   notenum(60) -> 60
// Returns -1 on a parse error or out-of-range result.
static int l_notenum(lua_State* L) {
    if (lua_type(L, 1) == LUA_TSTRING) {
        const char* s = lua_tostring(L, 1);
        int n;
        if (lua_gettop(L) >= 2 && lua_isnumber(L, 2))
            n = MusicTheory::noteNumber(s ? s : "", (int)lua_tointeger(L, 2));
        else
            n = MusicTheory::parseNoteName(s ? s : "");
        lua_pushinteger(L, n);
    } else {
        lua_pushinteger(L, (lua_Integer)luaL_optinteger(L, 1, -1));
    }
    return 1;
}
// notename(60) -> "C4". Accepts a note name too (round-trips via its number).
static int l_notename(lua_State* L) {
    int n;
    if (lua_type(L, 1) == LUA_TSTRING) {
        const char* s = lua_tostring(L, 1);
        n = MusicTheory::parseNoteName(s ? s : "");
    } else {
        n = (int)luaL_optinteger(L, 1, -1);
    }
    if (n < 0 || n > 127) { lua_pushnil(L); return 1; }
    lua_pushstring(L, MusicTheory::noteName(n).c_str());
    return 1;
}
// notefreq("C4") / notefreq(60) / notefreq("C", 4) -> Hz via the PROJECT tuning.
static int l_notefreq(lua_State* L) {
    LuaRuntime* self = self_(L);
    int n;
    if (lua_type(L, 1) == LUA_TSTRING) {
        const char* s = lua_tostring(L, 1);
        if (lua_gettop(L) >= 2 && lua_isnumber(L, 2))
            n = MusicTheory::noteNumber(s ? s : "", (int)lua_tointeger(L, 2));
        else
            n = MusicTheory::parseNoteName(s ? s : "");
    } else {
        n = (int)luaL_optinteger(L, 1, -1);
    }
    if (n < 0 || n > 127) { lua_pushnumber(L, 0.0); return 1; }
    lua_pushnumber(L, self->callNoteFreq(n));
    return 1;
}

// The convenience-alias prelude (sin, clamp, saw, noise, ...) is shared with the
// static-shape baker; see lua_prelude.h (kSoundShopLuaPrelude).

void LuaRuntime::registerApi() {
    // Shared.
    lua_pushcfunction(L, l_shape);    lua_setglobal(L, "shape");
    lua_pushcfunction(L, l_sig);      lua_setglobal(L, "sig");
    // Event-driven MIDI INPUT (block mode). Available in every role - a Signal or
    // Unified node with a MIDI-in pin can react to notes too. Inert (count 0)
    // outside the per-block path, so referencing it never errors.
    lua_pushcfunction(L, l_midiin);    lua_setglobal(L, "midiin");
    lua_pushcfunction(L, l_midievent); lua_setglobal(L, "midievent");
    // Streaming pull-model (active inside a stream() coroutine; inert otherwise).
    // pull/poll/wait operate one sample at a time; pullblock/pollblock/outblock a
    // whole block at a time; pollmidi drains MIDI input event-driven at the cursor.
    lua_pushcfunction(L, l_pull);      lua_setglobal(L, "pull");
    lua_pushcfunction(L, l_poll);      lua_setglobal(L, "poll");
    lua_pushcfunction(L, l_wait);      lua_setglobal(L, "wait");
    lua_pushcfunction(L, l_pullblock); lua_setglobal(L, "pullblock");
    lua_pushcfunction(L, l_pollblock); lua_setglobal(L, "pollblock");
    lua_pushcfunction(L, l_outblock);  lua_setglobal(L, "outblock");
    lua_pushcfunction(L, l_pollmidi);  lua_setglobal(L, "pollmidi");
    lua_pushcfunction(L, l_waveform); lua_setglobal(L, "waveform");
    lua_pushcfunction(L, l_warpamp);   lua_setglobal(L, "warpamp");
    lua_pushcfunction(L, l_warpphase); lua_setglobal(L, "warpphase");
    lua_pushcfunction(L, l_spectralwarp); lua_setglobal(L, "spectralwarp");
    lua_pushcfunction(L, l_waveletwarp);  lua_setglobal(L, "waveletwarp");
    // waveforms[name] -> stable integer id (lazy + cached via __index; see
    // l_waveforms_index). Lets a program resolve a name once and reuse the int.
    lua_newtable(L);                              // the waveforms table
    lua_newtable(L);                              // its metatable
    lua_pushcfunction(L, l_waveforms_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "waveforms");
    // Whole-grid generation helpers (inert outside runGenerate, since genData
    // is null then). Available in every role so a generate() program works
    // regardless of how the runtime was constructed.
    lua_pushcfunction(L, l_gen_set);       lua_setglobal(L, "set");
    lua_pushcfunction(L, l_gen_get);       lua_setglobal(L, "get");
    lua_pushcfunction(L, l_gen_coord);     lua_setglobal(L, "coord");
    lua_pushcfunction(L, l_gen_coordAxis); lua_setglobal(L, "coordAxis");
    lua_pushcfunction(L, l_gen_flatten);   lua_setglobal(L, "flatten");
    lua_pushcfunction(L, l_gen_neighbor);  lua_setglobal(L, "neighbor");
    lua_pushcfunction(L, l_gen_getAt);     lua_setglobal(L, "getAt");
    lua_pushcfunction(L, l_gen_setAt);     lua_setglobal(L, "setAt");
    // Note-name conversions are pure helpers - available in both roles (a Signal
    // script can use notefreq() to drive an oscillator at a note's pitch).
    lua_pushcfunction(L, l_notenum);  lua_setglobal(L, "notenum");
    lua_pushcfunction(L, l_notename); lua_setglobal(L, "notename");
    lua_pushcfunction(L, l_notefreq); lua_setglobal(L, "notefreq");
    // Midi and Unified roles can emit MIDI; Signal and Unified roles can write
    // continuous output samples. The Unified role gets both sets so one script
    // can emit MIDI *and* assign continuous outputs.
    if (role == ScriptRole::Midi || role == ScriptRole::Unified) {
        lua_pushcfunction(L, l_note);    lua_setglobal(L, "note");
        lua_pushcfunction(L, l_noteon);  lua_setglobal(L, "noteon");
        lua_pushcfunction(L, l_noteoff); lua_setglobal(L, "noteoff");
        lua_pushcfunction(L, l_cc);      lua_setglobal(L, "cc");
        lua_pushcfunction(L, l_bend);    lua_setglobal(L, "bend");
    }
    if (role == ScriptRole::Signal || role == ScriptRole::Unified) {
        lua_pushcfunction(L, l_out);     lua_setglobal(L, "out_sample");
        lua_pushcfunction(L, l_out);     lua_setglobal(L, "out");  // out(i,v) in block mode
    }
}

void LuaRuntime::rebuild() {
    if (L) { lua_close(L); L = nullptr; }   // closes co too (it's a child thread)
    loopRef = startRef = generateRef = LUA_NOREF;
    co = nullptr; streamRef = coRef = LUA_NOREF;
    streaming = false; streamDone = false;
    inStream = false; streamPos = -1; midiCursor = 0; blockConsumed = false;
    pullPin = 0;
    errored = false;
    lastErr.clear();

    L = luaL_newstate();
    if (!L) { errored = true; lastErr = "out of memory"; return; }
    *static_cast<LuaRuntime**>(lua_getextraspace(L)) = this;

    // Sandbox: open only the safe standard libraries (base, table, string,
    // math). NOT io / os / package / debug, so a script can't touch the
    // filesystem, run programs, or load native code.
    luaL_requiref(L, LUA_GNAME,      luaopen_base,   1); lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table,  1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME,luaopen_math,   1); lua_pop(L, 1);

    // Remove a few base globals that allow code loading / introspection escapes.
    const char* strip[] = { "dofile", "loadfile", "load", "loadstring",
                            "collectgarbage", "require" };
    for (const char* g : strip) { lua_pushnil(L); lua_setglobal(L, g); }

    registerApi();

    if (luaL_dostring(L, kSoundShopLuaPrelude) != LUA_OK) { handleError(); return; }

    if (luaL_loadstring(L, source.c_str()) != LUA_OK) { handleError(); return; }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) { handleError(); return; }

    // Capture loop() (the per-sample / per-block entry) and generate() (the
    // whole-grid terrain entry). At least one must exist: audio scripts define
    // loop(); whole-grid terrain generators define generate(). start() is always
    // optional.
    lua_getglobal(L, "loop");
    if (lua_isfunction(L, -1)) loopRef = luaL_ref(L, LUA_REGISTRYINDEX);
    else lua_pop(L, 1);

    lua_getglobal(L, "generate");
    if (lua_isfunction(L, -1)) generateRef = luaL_ref(L, LUA_REGISTRYINDEX);
    else lua_pop(L, 1);

    // stream() is the streaming (coroutine pull-model) entry. A program defines
    // exactly one of loop() / generate() / stream() as its body.
    lua_getglobal(L, "stream");
    if (lua_isfunction(L, -1)) streamRef = luaL_ref(L, LUA_REGISTRYINDEX);
    else lua_pop(L, 1);

    if (loopRef == LUA_NOREF && generateRef == LUA_NOREF && streamRef == LUA_NOREF) {
        errored = true;
        lastErr = "no function loop(), stream() or generate() defined";
        return;
    }

    lua_getglobal(L, "start");
    if (lua_isfunction(L, -1)) startRef = luaL_ref(L, LUA_REGISTRYINDEX);
    else lua_pop(L, 1);

    // Spin up the streaming coroutine NOW (message/load thread, never the audio
    // thread) so runBlock() only ever has to lua_resume() it - no per-block
    // allocation. The thread shares this state's globals; coRef anchors it
    // against GC. The stream() function sits at the bottom of the thread's stack,
    // ready for the first resume.
    if (streamRef != LUA_NOREF) {
        streaming = true;
        co = lua_newthread(L);                      // pushes the thread
        coRef = luaL_ref(L, LUA_REGISTRYINDEX);     // anchor + pop
        lua_rawgeti(co, LUA_REGISTRYINDEX, streamRef);  // push stream() onto co
    }
}

// Resume the streaming coroutine for one audio block. The coroutine runs until it
// either suspends (ran out of input for this block - normal) or returns (the
// program ended its own loop) or errors. On suspend we just come back next block;
// on return/error we latch streamDone and the output simply holds at 0.
void LuaRuntime::runStreamBlock(const ScriptBlockCtx& ctx) {
    if (!co || streamDone) return;
    setBlockGlobals(ctx);          // n, sr, dt, bpm, note/vel/gate/freq, rate, s1..sN
    streamPos     = -1;            // first pull() advances to sample 0
    midiCursor    = 0;
    blockConsumed = false;
    inStream      = true;
    int nres = 0;
    int status = lua_resume(co, L, 0, &nres);
    inStream = false;
    if (status == LUA_YIELD) {
        if (nres > 0) lua_pop(co, nres);            // discard any yielded values
    } else if (status == LUA_OK) {
        streamDone = true;                          // stream() returned; done
        if (nres > 0) lua_pop(co, nres);
    } else {
        const char* m = lua_tostring(co, -1);
        lastErr = m ? m : "Lua stream error";
        errored = true; streamDone = true;
        if (lua_gettop(co) > 0) lua_pop(co, 1);
    }
}

std::unique_ptr<IScriptRuntime> makeLuaRuntime(ScriptRole role, ScriptRate rate) {
    return std::make_unique<LuaRuntime>(role, rate);
}

} // namespace SoundShop

#endif // HAS_LUA

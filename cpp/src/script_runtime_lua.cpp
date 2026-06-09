#include "script_runtime.h"

#ifdef HAS_LUA

#include "lua_prelude.h"
#include <juce_core/juce_core.h>
#include <cmath>

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
//   * function loop()   - required; runs every sample (PerSample) or once per
//     block (PerBlock). Signal role: return the output value (PerSample) or call
//     out(i, value) for each sample (PerBlock). MIDI role: call note()/cc()/...
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
        return juce::jlimit(-1.0f, 1.0f, v);
    }

    void runMidi(const ScriptVars& vars, IExprEmitSink* sink) override {
        if (!L || errored || loopRef == LUA_NOREF) return;
        curSink = sink; curCtx = nullptr;
        setGlobals(vars);
        lua_rawgeti(L, LUA_REGISTRYINDEX, loopRef);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) handleError();
        curSink = nullptr;
    }

    void runBlock(const ScriptBlockCtx& ctx) override {
        if (!L || errored || loopRef == LUA_NOREF) return;
        curSink = ctx.sink; curCtx = &ctx;
        setBlockGlobals(ctx);
        lua_rawgeti(L, LUA_REGISTRYINDEX, loopRef);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) handleError();
        curSink = nullptr; curCtx = nullptr;
    }

    bool supportsPerSample() const override { return true; }
    bool supportsPerBlock()  const override { return true; }
    std::string getError() const override { return lastErr; }

    // --- accessed by the C trampolines ---
    ScriptRole role;
    ScriptRate rate;
    IExprEmitSink* curSink = nullptr;
    const ScriptBlockCtx* curCtx = nullptr;

    int readOut() {
        lua_getglobal(L, "out");
        int o = (int)lua_tonumber(L, -1);
        lua_pop(L, 1);
        return o;
    }

    // Public accessor for the protected shape sampler so the static C
    // trampolines (free functions, not members) can reach it.
    float callShape(float pos) const { return shape ? shape(pos) : 0.0f; }

private:
    lua_State* L = nullptr;
    std::string source;
    int loopRef = LUA_NOREF, startRef = LUA_NOREF;
    bool errored = false;
    std::string lastErr;

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

// ---- MIDI emit trampolines --------------------------------------------------
static void setOffsetIfBlock(lua_State* L, LuaRuntime* self, int argIndex) {
    if (self->rate == ScriptRate::PerBlock && self->curSink) {
        int n = self->curCtx ? self->curCtx->numSamples : 0;
        int off = (int)luaL_optinteger(L, argIndex, 0);
        if (n > 0) off = (int)juce::jlimit(0, n - 1, off);
        else off = juce::jmax(0, off);
        self->curSink->setSampleOffset(off);
    }
}

static int l_note(lua_State* L) {
    LuaRuntime* self = self_(L);
    if (!self->curSink) return 0;
    float p = (float)luaL_optnumber(L, 1, 0);
    float v = (float)luaL_optnumber(L, 2, 0);
    float d = (float)luaL_optnumber(L, 3, 0.1);
    setOffsetIfBlock(L, self, 4);
    self->curSink->emitNote(self->readOut(), p, v, d);
    return 0;
}
static int l_noteon(lua_State* L) {
    LuaRuntime* self = self_(L);
    if (!self->curSink) return 0;
    float p = (float)luaL_optnumber(L, 1, 0);
    float v = (float)luaL_optnumber(L, 2, 0);
    setOffsetIfBlock(L, self, 3);
    self->curSink->emitNoteOn(self->readOut(), p, v);
    return 0;
}
static int l_noteoff(lua_State* L) {
    LuaRuntime* self = self_(L);
    if (!self->curSink) return 0;
    float p = (float)luaL_optnumber(L, 1, 0);
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
static int l_out(lua_State* L) {
    LuaRuntime* self = self_(L);
    if (!self->curCtx || !self->curCtx->out) return 0;
    int i = (int)luaL_checkinteger(L, 1);
    float v = (float)luaL_checknumber(L, 2);
    if (i >= 0 && i < self->curCtx->numSamples)
        self->curCtx->out[i] = juce::jlimit(-1.0f, 1.0f, v);
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

// The convenience-alias prelude (sin, clamp, saw, noise, ...) is shared with the
// static-shape baker; see lua_prelude.h (kSoundShopLuaPrelude).

void LuaRuntime::registerApi() {
    // Shared.
    lua_pushcfunction(L, l_shape);  lua_setglobal(L, "shape");
    lua_pushcfunction(L, l_sig);    lua_setglobal(L, "sig");
    if (role == ScriptRole::Midi) {
        lua_pushcfunction(L, l_note);    lua_setglobal(L, "note");
        lua_pushcfunction(L, l_noteon);  lua_setglobal(L, "noteon");
        lua_pushcfunction(L, l_noteoff); lua_setglobal(L, "noteoff");
        lua_pushcfunction(L, l_cc);      lua_setglobal(L, "cc");
        lua_pushcfunction(L, l_bend);    lua_setglobal(L, "bend");
    } else {
        lua_pushcfunction(L, l_out);     lua_setglobal(L, "out_sample");
        lua_pushcfunction(L, l_out);     lua_setglobal(L, "out");  // out(i,v) in block mode
    }
}

void LuaRuntime::rebuild() {
    if (L) { lua_close(L); L = nullptr; }
    loopRef = startRef = LUA_NOREF;
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

    // Capture loop() (required) and start() (optional).
    lua_getglobal(L, "loop");
    if (lua_isfunction(L, -1)) loopRef = luaL_ref(L, LUA_REGISTRYINDEX);
    else { lua_pop(L, 1); errored = true; lastErr = "no function loop() defined"; return; }

    lua_getglobal(L, "start");
    if (lua_isfunction(L, -1)) startRef = luaL_ref(L, LUA_REGISTRYINDEX);
    else lua_pop(L, 1);
}

std::unique_ptr<IScriptRuntime> makeLuaRuntime(ScriptRole role, ScriptRate rate) {
    return std::make_unique<LuaRuntime>(role, rate);
}

} // namespace SoundShop

#endif // HAS_LUA

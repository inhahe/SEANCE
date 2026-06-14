#include "script_runtime.h"

namespace SoundShop {

// Per-backend factories (defined in the matching translation units).
std::unique_ptr<IScriptRuntime> makeBuiltinRuntime(ScriptRole role, ScriptRate rate);
#ifdef HAS_LUA
std::unique_ptr<IScriptRuntime> makeLuaRuntime(ScriptRole role, ScriptRate rate);
#endif
std::unique_ptr<IScriptRuntime> makeWasmRuntime(ScriptRole role, ScriptRate rate);
bool wasmRuntimeAvailable();

// -----------------------------------------------------------------------------
// Capability matrix (mirrors the doc comment in script_runtime.h):
//   Builtin -> PerSample only
//   Lua     -> PerSample and PerBlock
//   Wasm    -> PerBlock only
// -----------------------------------------------------------------------------
void buildScriptMidiIn(const juce::MidiBuffer& midi, int numSamples,
                       std::vector<ScriptMidiEvent>& out, int midiInputCount) {
    out.clear();
    const int maxOff = numSamples > 0 ? numSamples - 1 : 0;
    const bool multiIn = midiInputCount > 1;
    for (const auto meta : midi) {
        const auto msg = meta.getMessage();
        ScriptMidiEvent e;
        e.offset = juce::jlimit(0, maxOff, meta.samplePosition);
        // For a multi-input node the graph stamped channel = inIndex + 1.
        e.inIndex = multiIn ? juce::jlimit(0, midiInputCount - 1, msg.getChannel() - 1) : 0;
        if (msg.isNoteOn()) {
            e.kind = 1; e.a = msg.getNoteNumber();
            e.b = (float) msg.getVelocity() / 127.0f;
        } else if (msg.isNoteOff()) {
            e.kind = 0; e.a = msg.getNoteNumber(); e.b = 0.0f;
        } else if (msg.isController()) {
            e.kind = 2; e.a = msg.getControllerNumber();
            e.b = (float) msg.getControllerValue() / 127.0f;
        } else if (msg.isPitchWheel()) {
            e.kind = 3; e.a = 0;
            // 14-bit 0..16383, centre 8192 -> -1..1.
            e.b = ((float) msg.getPitchWheelValue() - 8192.0f) / 8192.0f;
        } else {
            continue;
        }
        out.push_back(e);
    }
}

bool scriptLangSupportsRate(ScriptLang lang, ScriptRate rate) {
    switch (lang) {
        case ScriptLang::Builtin: return rate == ScriptRate::PerSample;
        case ScriptLang::Lua:     return true;
        case ScriptLang::Wasm:    return rate == ScriptRate::PerBlock;
    }
    return false;
}

bool scriptLangAvailable(ScriptLang lang) {
    switch (lang) {
        case ScriptLang::Builtin: return true;
#ifdef HAS_LUA
        case ScriptLang::Lua:     return true;
#else
        case ScriptLang::Lua:     return false;
#endif
        case ScriptLang::Wasm:    return wasmRuntimeAvailable();
    }
    return false;
}

std::unique_ptr<IScriptRuntime> makeScriptRuntime(ScriptLang lang,
                                                  ScriptRole role,
                                                  ScriptRate rate) {
    switch (lang) {
        case ScriptLang::Builtin:
            return makeBuiltinRuntime(role, rate);
        case ScriptLang::Lua:
#ifdef HAS_LUA
            return makeLuaRuntime(role, rate);
#else
            return nullptr; // Lua not vendored in this build.
#endif
        case ScriptLang::Wasm:
            return makeWasmRuntime(role, rate);
    }
    return nullptr;
}

} // namespace SoundShop

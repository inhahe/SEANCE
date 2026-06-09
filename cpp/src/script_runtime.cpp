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

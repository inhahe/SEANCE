#include "shape_expr.h"
#include "builtin_synth.h"   // WaveExprParser
#include "scripting.h"       // ScriptEngine (Python)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

#ifdef HAS_LUA
#include "lua_prelude.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace SoundShop {

// -----------------------------------------------------------------------------
// Language metadata
// -----------------------------------------------------------------------------
const char* shapeLangKey(ShapeLang lang) {
    switch (lang) {
        case ShapeLang::Lua:    return "lua";
        case ShapeLang::Python: return "python";
        case ShapeLang::Builtin:
        default:                return "builtin";
    }
}

ShapeLang shapeLangFromKey(const std::string& key) {
    if (key == "lua")    return ShapeLang::Lua;
    if (key == "python") return ShapeLang::Python;
    return ShapeLang::Builtin;
}

const char* shapeLangDisplayName(ShapeLang lang) {
    switch (lang) {
        case ShapeLang::Lua:    return "Lua";
        case ShapeLang::Python: return "Python";
        case ShapeLang::Builtin:
        default:                return "Built-in";
    }
}

bool shapeLangAvailable(ShapeLang lang) {
    switch (lang) {
        case ShapeLang::Lua:
#ifdef HAS_LUA
            return true;
#else
            return false;
#endif
        case ShapeLang::Python:
            // The embedded interpreter is compiled in; it initialises lazily.
            return true;
        case ShapeLang::Builtin:
        default:
            return true;
    }
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

// Value of the shape variable `x` at sample i of N.
static double loopValue(bool domainRadians, int i, int N) {
    if (domainRadians)
        return (double)i / (double)N * 2.0 * M_PI;     // [0, 2*pi)
    return (N > 1) ? (double)i / (double)(N - 1) : 0.0; // [0, 1]
}

static std::string trimCopy(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// A bare expression ("sin(x)") has no newline and no `return` - wrap it so the
// generated function body returns it. A multi-statement body is used verbatim
// and the user is responsible for the `return`.
static bool isBareExpression(const std::string& src) {
    return src.find('\n') == std::string::npos &&
           src.find("return") == std::string::npos;
}

// -----------------------------------------------------------------------------
// Built-in (pure C++) bake
// -----------------------------------------------------------------------------
static bool bakeBuiltin(const std::string& src, bool domainRadians, int N,
                        std::vector<float>& out, std::string& error) {
    out.assign(N, 0.0f);
    if (domainRadians) {
        // WaveExprParser::evaluate sweeps x across [0, 2*pi) and clamps to [-1,1].
        out = WaveExprParser::evaluate(src, N);
        if ((int)out.size() != N) out.resize(N, 0.0f);
    } else {
        for (int i = 0; i < N; ++i) {
            float xn = (float)loopValue(false, i, N);
            std::unordered_map<std::string, float> vars;
            vars["x"] = xn;   // normalized position [0,1]
            vars["f"] = xn;   // alias, matching the non-periodic contract
            out[i] = WaveExprParser::evaluateWithVars(src, vars);
        }
    }
    error.clear();
    return true;
}

// -----------------------------------------------------------------------------
// Lua bake (local sandboxed state, created per bake)
// -----------------------------------------------------------------------------
#ifdef HAS_LUA
static bool bakeLua(const std::string& src, bool domainRadians, int N,
                    std::vector<float>& out, std::string& error) {
    out.assign(N, 0.0f);
    std::string trimmed = trimCopy(src);
    std::string body = isBareExpression(trimmed) ? ("return (" + trimmed + ")")
                                                 : trimmed;
    // The shape variable is always `x`. Non-periodic curves also expose `f` as
    // an alias of the normalized position so either name evaluates the same.
    std::string alias = domainRadians ? "" : "local f = x\n";
    std::string chunk = "function __shape(x)\n" + alias + body + "\nend\n";

    lua_State* L = luaL_newstate();
    if (!L) { error = "out of memory"; return false; }

    luaL_requiref(L, LUA_GNAME,       luaopen_base,   1); lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME,  luaopen_table,  1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME,  luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math,   1); lua_pop(L, 1);
    const char* strip[] = { "dofile", "loadfile", "load", "loadstring",
                            "collectgarbage", "require" };
    for (const char* g : strip) { lua_pushnil(L); lua_setglobal(L, g); }

    if (luaL_dostring(L, kSoundShopLuaPrelude) != LUA_OK) {
        error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "prelude error";
        lua_close(L); return false;
    }
    if (luaL_loadstring(L, chunk.c_str()) != LUA_OK ||
        lua_pcall(L, 0, 0, 0) != LUA_OK) {
        error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "compile error";
        lua_close(L); return false;
    }

    lua_getglobal(L, "__shape");
    if (!lua_isfunction(L, -1)) {
        error = "no shape value produced";
        lua_close(L); return false;
    }
    int fnRef = luaL_ref(L, LUA_REGISTRYINDEX);

    bool ok = true;
    for (int i = 0; i < N; ++i) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, fnRef);
        lua_pushnumber(L, (lua_Number)loopValue(domainRadians, i, N));
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "runtime error";
            lua_pop(L, 1);
            ok = false;
            break;
        }
        float v = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        if (domainRadians) v = std::max(-1.0f, std::min(1.0f, v));
        out[i] = v;
    }

    luaL_unref(L, LUA_REGISTRYINDEX, fnRef);
    lua_close(L);
    if (!ok) { out.assign(N, 0.0f); return false; }
    error.clear();
    return true;
}
#endif // HAS_LUA

// -----------------------------------------------------------------------------
// Dispatch
// -----------------------------------------------------------------------------
bool bakeShapeExpr(ShapeLang lang, const std::string& src,
                   bool domainRadians, int N,
                   std::vector<float>& out, std::string& error) {
    if (N <= 0) { out.clear(); error.clear(); return true; }
    out.assign(N, 0.0f);
    error.clear();

    if (trimCopy(src).empty()) return true; // empty source => flat zero, no error

    switch (lang) {
        case ShapeLang::Lua:
#ifdef HAS_LUA
            return bakeLua(src, domainRadians, N, out, error);
#else
            error = "Lua is not available in this build";
            return false;
#endif
        case ShapeLang::Python:
            return ScriptEngine::instance().bakeShapeExpr(
                src, domainRadians, N, out, error);
        case ShapeLang::Builtin:
        default:
            return bakeBuiltin(src, domainRadians, N, out, error);
    }
}

} // namespace SoundShop

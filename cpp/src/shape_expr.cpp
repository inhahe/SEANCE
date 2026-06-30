#include "shape_expr.h"
#include "builtin_synth.h"   // WaveExprParser
#include "scripting.h"       // ScriptEngine (Python)
#include "glsl_compute.h"    // glslComputeAvailable / glslDispatchCompute / remapGlslErrorLog
#include "glsl_waveform.h"   // shared GLSL waveform() bank wiring

#include <juce_core/juce_core.h>  // juce::Logger (full GLSL log to seance.log)
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
        case ShapeLang::Glsl:   return "glsl";
        case ShapeLang::Builtin:
        default:                return "builtin";
    }
}

ShapeLang shapeLangFromKey(const std::string& key) {
    if (key == "lua")    return ShapeLang::Lua;
    if (key == "python") return ShapeLang::Python;
    if (key == "glsl")   return ShapeLang::Glsl;
    return ShapeLang::Builtin;
}

const char* shapeLangDisplayName(ShapeLang lang) {
    switch (lang) {
        case ShapeLang::Lua:    return "Lua";
        case ShapeLang::Python: return "Python";
        case ShapeLang::Glsl:   return "GLSL";
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
            // Only when SEANCE was built with Python AND the (delay-loaded)
            // Python DLL can be loaded at runtime. The UI greys out the Python
            // option in shape-language combos when this is false.
            return ScriptEngine::pythonAvailable();
        case ShapeLang::Glsl:
            // Only when a headless GL 4.3 core compute context can be created on
            // this machine. The UI greys out the GLSL option otherwise.
            return glslComputeAvailable();
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

static int countNewlines(const std::string& s) {
    int n = 0;
    for (char c : s) if (c == '\n') ++n;
    return n;
}

// A bare expression ("sin(x)") has no newline and no `return` - wrap it so the
// generated function body returns it. A multi-statement body is used verbatim
// and the user is responsible for the `return`.
static bool isBareExpression(const std::string& src) {
    return src.find('\n') == std::string::npos &&
           src.find("return") == std::string::npos;
}

// The Built-in language has no `return` keyword: a program is a sequence of
// statements (separated by newlines or ';') whose last statement's value is the
// result, and statements may assign named intermediates (`a = sin(x)`). This
// lets a formula build up several "wave objects" and combine them
// (`a=sin(x)`, `b=0.5*sin(3*x)`, `a+b`) - the Built-in equivalent of what Lua/
// Python/GLSL already do with local variables. We route through runProgram()
// whenever the source contains a statement separator or a plain assignment;
// a single bare expression keeps the cheaper single-pass evaluate() path.
static bool builtinIsProgram(const std::string& src) {
    if (src.find('\n') != std::string::npos) return true;
    if (src.find(';')  != std::string::npos) return true;
    // Detect a top-level assignment `=` that is not part of ==, <=, >=, !=.
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] != '=') continue;
        char prev = i > 0 ? src[i - 1] : '\0';
        char next = i + 1 < src.size() ? src[i + 1] : '\0';
        if (next == '=') { ++i; continue; }                 // ==
        if (prev == '<' || prev == '>' || prev == '!' || prev == '=') continue;
        return true;                                        // assignment
    }
    return false;
}

// -----------------------------------------------------------------------------
// Built-in (pure C++) bake
// -----------------------------------------------------------------------------
static bool bakeBuiltin(const std::string& src, bool domainRadians, int N,
                        std::vector<float>& out, std::string& error) {
    out.assign(N, 0.0f);
    if (builtinIsProgram(src)) {
        // Multi-statement program: assign named intermediates and combine them.
        // Each sample is an independent pure function of the sweep position, so
        // we give every sample a fresh state store (no accumulation across the
        // table) and bind the position as `x` (and `f` for the [0,1] contract).
        std::function<float(float)> noShape;   // shape()/notefreq() unavailable
        std::function<float(int)>   noNote;    // in an offline bake -> 0.
        for (int i = 0; i < N; ++i) {
            float xv = (float)loopValue(domainRadians, i, N);
            std::unordered_map<std::string, float> vars;
            vars["x"] = xv;
            if (!domainRadians) vars["f"] = xv; // [0,1] curves alias f to position
            std::unordered_map<std::string, float> state; // fresh per sample
            float v = WaveExprParser::runProgram(src, vars, state, nullptr, noShape, noNote);
            if (domainRadians) v = std::max(-1.0f, std::min(1.0f, v));
            out[i] = v;
        }
    } else if (domainRadians) {
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
// GLSL bake (offscreen GL 4.3 compute; same backend as terrain generation)
// -----------------------------------------------------------------------------
// The user authors a per-sample GLSL body that returns one float. We template it
// into shapeValue(x, f, i, n), run one GPU thread per sample over a flat output
// SSBO, and read the N floats back. The variable contract mirrors the other
// languages: `x` is the loop variable (radians for waveshapes, normalized for
// curves), `f` is always the normalized [0,1] position, plus `i`/`n` and the
// constant TAU, and waveform(int,float) for the factory bank. domainRadians
// waveshapes are clamped to [-1,1] (matching Builtin/Lua); curves are not.
static bool bakeGlsl(const std::string& src, bool domainRadians, int N,
                     std::vector<float>& out, std::string& error) {
    out.assign(N, 0.0f);

    std::string why;
    if (!glslComputeAvailable(&why)) {
        error = "GLSL compute is unavailable on this machine: " + why;
        return false;
    }

    std::string trimmed = trimCopy(src);
    // A bare expression ("sin(x)") is not a valid GLSL statement on its own, so
    // wrap it as `return (<expr>);`. A multi-statement body is used verbatim and
    // the user supplies the `return` (mirrors the Lua/terrain convention).
    std::string body = isBareExpression(trimmed) ? ("return (" + trimmed + ");")
                                                 : trimmed;

    std::string wfFn;
    {
        std::string upErr;
        if (!glslWaveformFn(src, wfFn, &upErr)) { error = upErr; return false; }
    }

    // x: radians [0,2*pi) for waveshapes, normalized [0,1] for curves. f: always
    // the normalized [0,1] position. The final write clamps only for waveshapes.
    const std::string xLine = domainRadians
        ? "  float x = float(i) / float(n) * TAU;\n"
        : "  float x = pos;\n";
    const std::string writeLine = domainRadians
        ? "  data[gid] = clamp(shapeValue(x, f, i, n), -1.0, 1.0);\n"
        : "  data[gid] = shapeValue(x, f, i, n);\n";

    // Build the scaffolding that precedes the user body separately so we can
    // count how many generated lines sit above it; the GLSL info-log line
    // numbers are then remapped back to the user's own source (see Python's
    // identical userLineOffset trick in scripting.cpp).
    const std::string prefix =
        "#version 430\n"
        "layout(local_size_x = 64) in;\n"
        "layout(std430, binding = 0) buffer Out { float data[]; };\n"
        "uniform int uTotal;\n"
        "const float TAU = 6.28318530717958648;\n"
        + wfFn +
        "float shapeValue(float x, float f, int i, int n) {\n";
    const int userLineOffset = countNewlines(prefix);
    const int userLineCount  = countNewlines(body) + 1;

    std::string shader =
        prefix
        + body + "\n"
        "}\n"
        "void main() {\n"
        "  uint gid = gl_GlobalInvocationID.x;\n"
        "  if (gid >= uint(uTotal)) return;\n"
        "  int i = int(gid);\n"
        "  int n = uTotal;\n"
        "  float pos = (n > 1) ? float(i) / float(n - 1) : 0.0;\n"
        + xLine +
        "  float f = pos;\n"
        + writeLine +
        "}\n";

    auto res = glslDispatchCompute(shader, N, { { "uTotal", { N } } });
    if (!res.ok) {
        juce::Logger::writeToLog("GLSL shape bake error:\n" + juce::String(res.error));
        error = remapGlslErrorLog(res.error, userLineOffset, userLineCount);
        return false;
    }
    if ((int) res.data.size() != N) { error = "GLSL readback size mismatch"; return false; }

    out.resize(N);
    for (int i = 0; i < N; ++i) {
        float v = res.data[i];
        out[i] = std::isfinite(v) ? v : 0.0f;   // NaN/Inf -> 0, defensive
    }
    error.clear();
    return true;
}

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
        case ShapeLang::Glsl:
            return bakeGlsl(src, domainRadians, N, out, error);
        case ShapeLang::Builtin:
        default:
            return bakeBuiltin(src, domainRadians, N, out, error);
    }
}

} // namespace SoundShop

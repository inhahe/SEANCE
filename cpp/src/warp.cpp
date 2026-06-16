#include "warp.h"

#include <cmath>
#include <algorithm>
#include <sstream>
#include <cctype>
#include <utility>

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
namespace {

inline float frac01(float p) {
    p -= std::floor(p);
    if (p < 0.0f) p += 1.0f;
    if (p >= 1.0f) p -= 1.0f;
    return p;
}
inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// Reflective triangle fold of an arbitrary value back into [-1,1].
inline float foldTri(float y) {
    // period-4 triangle: 0->0, 1->1, 2->0, 3->-1 ...
    float t = std::fmod(std::fabs(y + 1.0f), 4.0f);
    if (t > 2.0f) t = 4.0f - t;
    return t - 1.0f;
}
// Wrap an arbitrary value into [-1,1).
inline float wrapBi(float y) {
    return y - 2.0f * std::floor((y + 1.0f) * 0.5f);
}

} // namespace

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
const std::vector<WarpMethodInfo>& warpMethodRegistry() {
    static const std::vector<WarpMethodInfo> reg = {
        // method,                      domain,                name,                 recommended, tooltip,                                                                paramName
        { WarpMethod::None,             WarpDomain::Phase,     "None",               false, "No shaping.",                                                             "Amount" },

        // --- Amplitude-domain (waveshaping) ---
        { WarpMethod::SoftClip,         WarpDomain::Amplitude, "Soft Clip",          true,  "Smooth saturation - rounds peaks, adds warmth.",                          "Drive" },
        { WarpMethod::HardClip,         WarpDomain::Amplitude, "Hard Clip",          false, "Flattens peaks past a threshold - aggressive edge.",                      "Drive" },
        { WarpMethod::Wavefold,         WarpDomain::Amplitude, "Wavefold",           true,  "Folds peaks back inward - dense West-Coast harmonics.",                   "Fold" },
        { WarpMethod::Wavewrap,         WarpDomain::Amplitude, "Wavewrap",           false, "Wraps peaks around instead of folding - harsh, glitchy.",                 "Wrap" },
        { WarpMethod::Rectify,          WarpDomain::Amplitude, "Rectify",            false, "Folds the negative half up - octave-up / even harmonics.",                "Amount" },
        { WarpMethod::Quantize,         WarpDomain::Amplitude, "Quantize (bitcrush)",true,  "Steps the amplitude - 8-bit / lo-fi character.",                          "Crush" },
        { WarpMethod::TubeSat,          WarpDomain::Amplitude, "Tube Saturate",      true,  "Asymmetric drive - even-harmonic, tube-like warmth.",                     "Drive" },
        { WarpMethod::TapeSat,          WarpDomain::Amplitude, "Tape Saturate",      false, "Symmetric soft drive - odd-harmonic, tape-like glue.",                    "Drive" },
        { WarpMethod::Flip,             WarpDomain::Amplitude, "Flip (invert)",      false, "Crossfades toward the vertically inverted wave.",                         "Amount" },
        { WarpMethod::Chebyshev,        WarpDomain::Amplitude, "Chebyshev",          false, "Polynomial shaper - injects a specific harmonic (best on simple waves).", "Amount" },

        // --- Phase-domain (read-position remap) ---
        { WarpMethod::BendPlus,         WarpDomain::Phase,     "Bend +",             true,  "Pinches the cycle - pushes energy toward the end.",                       "Bend" },
        { WarpMethod::BendMinus,        WarpDomain::Phase,     "Bend -",             true,  "Pinches the cycle - pulls energy toward the start.",                      "Bend" },
        { WarpMethod::AsymPlus,         WarpDomain::Phase,     "Asym +",             true,  "Stretches one half, squeezes the other (rightward).",                     "Skew" },
        { WarpMethod::AsymMinus,        WarpDomain::Phase,     "Asym -",             true,  "Stretches one half, squeezes the other (leftward).",                      "Skew" },
        { WarpMethod::PwmSkew,          WarpDomain::Phase,     "PWM Skew",           true,  "Duty-cycle skew - pulse-width-modulation feel on any wave.",              "Width" },
        { WarpMethod::PhaseQuantize,    WarpDomain::Phase,     "Phase Quantize",     false, "Stair-steps the read position - stepped, gritty motion.",                 "Steps" },
        { WarpMethod::PhaseDistortion,  WarpDomain::Phase,     "Phase Distortion",   true,  "Casio-CZ-style phase warp - sine toward saw/square.",                     "Amount" },
        { WarpMethod::VectorPhaseShaping,WarpDomain::Phase,    "Vector Phase Shape", false, "VPS - two-segment phase warp with a movable control point.",              "Amount" },
        { WarpMethod::Remap,            WarpDomain::Phase,     "Remap (S-curve)",    true,  "Remaps the cycle through a curve (S-curve here; full editor soon).",      "Curve" },
        { WarpMethod::SelfSync,         WarpDomain::Phase,     "Self-Sync",          true,  "Plays extra cycles per period - formant-shift / sync sweep.",             "Sync" },
    };
    return reg;
}

const WarpMethodInfo* warpMethodInfo(WarpMethod m) {
    for (const auto& info : warpMethodRegistry())
        if (info.method == m) return &info;
    return nullptr;
}

WarpDomain warpDomainOf(WarpMethod m) {
    const auto* info = warpMethodInfo(m);
    return info ? info->domain : WarpDomain::Phase;
}

const char* warpMethodName(WarpMethod m) {
    const auto* info = warpMethodInfo(m);
    return info ? info->name : "?";
}

const char* warpParamLabel(WarpMethod m) {
    const auto* info = warpMethodInfo(m);
    if (info && info->paramName && *info->paramName) return info->paramName;
    return "Amount";
}

namespace {
// Collapse a method name to a canonical lookup key: lowercase, alphanumerics
// only, with '+'/'-' spelled out so "Bend +" and "bendplus" agree. Spaces,
// underscores, and parenthetical qualifiers ("(bitcrush)", "(invert)") drop out.
std::string normalizeWarpName(const char* s) {
    std::string o;
    for (const char* p = s; p && *p; ++p) {
        char c = *p;
        if (c == '+')      o += "plus";
        else if (c == '-') o += "minus";
        else if (std::isalnum((unsigned char)c))
            o += (char)std::tolower((unsigned char)c);
        // everything else (space, '_', '(', ')', '/') is ignored
    }
    return o;
}
} // namespace

WarpMethod warpMethodFromName(const char* name) {
    if (!name || !*name) return WarpMethod::None;
    const std::string key = normalizeWarpName(name);
    if (key.empty()) return WarpMethod::None;

    // Canonical script tokens + common aliases. These are the preferred
    // spellings to document; the registry display names are matched as a
    // fallback below so the exact picker label also works.
    static const std::vector<std::pair<const char*, WarpMethod>> aliases = {
        {"none",            WarpMethod::None},
        {"softclip",        WarpMethod::SoftClip},
        {"hardclip",        WarpMethod::HardClip},
        {"wavefold",        WarpMethod::Wavefold},  {"fold",     WarpMethod::Wavefold},
        {"wavewrap",        WarpMethod::Wavewrap},  {"wrap",     WarpMethod::Wavewrap},
        {"rectify",         WarpMethod::Rectify},
        {"quantize",        WarpMethod::Quantize},  {"bitcrush", WarpMethod::Quantize},
        {"tubesat",         WarpMethod::TubeSat},   {"tubesaturate", WarpMethod::TubeSat},
        {"tapesat",         WarpMethod::TapeSat},   {"tapesaturate", WarpMethod::TapeSat},
        {"flip",            WarpMethod::Flip},      {"invert",   WarpMethod::Flip},
        {"chebyshev",       WarpMethod::Chebyshev},
        {"bendplus",        WarpMethod::BendPlus},  {"bend",     WarpMethod::BendPlus},
        {"bendminus",       WarpMethod::BendMinus},
        {"asymplus",        WarpMethod::AsymPlus},  {"asym",     WarpMethod::AsymPlus},
        {"asymminus",       WarpMethod::AsymMinus},
        {"pwmskew",         WarpMethod::PwmSkew},   {"pwm",      WarpMethod::PwmSkew},
        {"phasequantize",   WarpMethod::PhaseQuantize},
        {"phasedistortion", WarpMethod::PhaseDistortion}, {"pd", WarpMethod::PhaseDistortion},
        {"vectorphaseshape",WarpMethod::VectorPhaseShaping}, {"vps", WarpMethod::VectorPhaseShaping},
        {"remap",           WarpMethod::Remap},
        {"selfsync",        WarpMethod::SelfSync},  {"sync",     WarpMethod::SelfSync},
    };
    for (const auto& a : aliases)
        if (key == a.first) return a.second;

    // Fall back to the normalized registry display name (so "Quantize (bitcrush)"
    // -> "quantizebitcrush" etc. also resolve if typed verbatim).
    for (const auto& info : warpMethodRegistry())
        if (normalizeWarpName(info.name) == key) return info.method;

    return WarpMethod::None;
}

// ---------------------------------------------------------------------------
// Per-sample primitives
// ---------------------------------------------------------------------------
float warpPhaseValue(WarpMethod m, float phase, float amount) {
    const float p = frac01(phase);
    const float a = clampf(amount, 0.0f, 1.0f);
    if (a <= 0.0f) return p;

    switch (m) {
        case WarpMethod::BendPlus: {
            const float e = 1.0f + a * 3.0f;
            return frac01(std::pow(p, e));
        }
        case WarpMethod::BendMinus: {
            const float e = 1.0f / (1.0f + a * 3.0f);
            return frac01(std::pow(p, e));
        }
        case WarpMethod::AsymPlus: {
            const float mid = 0.5f + a * 0.49f;
            return (p < mid) ? (0.5f * p / mid)
                             : (0.5f + 0.5f * (p - mid) / (1.0f - mid));
        }
        case WarpMethod::AsymMinus: {
            const float mid = 0.5f - a * 0.49f;
            return (p < mid) ? (0.5f * p / mid)
                             : (0.5f + 0.5f * (p - mid) / (1.0f - mid));
        }
        case WarpMethod::PwmSkew: {
            // Compress the first part of the cycle - duty-cycle skew.
            const float d = 0.5f - a * 0.45f;
            return (p < d) ? (0.5f * p / d)
                           : (0.5f + 0.5f * (p - d) / (1.0f - d));
        }
        case WarpMethod::PhaseQuantize: {
            const float steps = std::max(2.0f, std::round(2.0f + (1.0f - a) * 30.0f));
            const float q = std::round(p * steps) / steps;
            return lerpf(p, q, a);
        }
        case WarpMethod::PhaseDistortion: {
            // CZ-style: a knee that sweeps the read through the first portion.
            const float k = 0.5f - a * 0.49f;
            return (p < k) ? (0.5f * p / k)
                           : (0.5f + 0.5f * (p - k) / (1.0f - k));
        }
        case WarpMethod::VectorPhaseShaping: {
            // Two-line phase warp through control point (pivotX, v). amount moves
            // the height v away from the identity midpoint (pivot fixed at 0.5
            // for v1; a movable pivot arrives with the curve editor in M5).
            const float pivotX = 0.5f;
            const float v = clampf(0.5f - a * 0.45f, 0.02f, 0.98f);
            return (p < pivotX) ? (v * p / pivotX)
                                : (v + (1.0f - v) * (p - pivotX) / (1.0f - pivotX));
        }
        case WarpMethod::Remap: {
            const float sc = p * p * (3.0f - 2.0f * p); // smoothstep
            return lerpf(p, sc, a);
        }
        case WarpMethod::SelfSync: {
            const float cycles = 1.0f + a * 3.0f;
            return frac01(p * cycles);
        }
        default:
            return p; // non-phase method
    }
}

float warpAmpValue(WarpMethod m, float x, float amount) {
    const float a = clampf(amount, 0.0f, 1.0f);
    if (a <= 0.0f) return x;

    switch (m) {
        case WarpMethod::SoftClip: {
            const float K = 4.0f;
            const float shaped = std::tanh(x * K) / std::tanh(K);
            return lerpf(x, shaped, a);
        }
        case WarpMethod::HardClip: {
            const float t = 1.0f - 0.95f * a;
            const float shaped = clampf(x, -t, t) / t;
            return lerpf(x, shaped, a);
        }
        case WarpMethod::Wavefold: {
            const float g = 1.0f + a * 4.0f;
            return lerpf(x, foldTri(x * g), a);
        }
        case WarpMethod::Wavewrap: {
            const float g = 1.0f + a * 4.0f;
            return lerpf(x, wrapBi(x * g), a);
        }
        case WarpMethod::Rectify: {
            const float shaped = 2.0f * std::fabs(x) - 1.0f;
            return lerpf(x, shaped, a);
        }
        case WarpMethod::Quantize: {
            const float levels = std::max(2.0f, std::round(2.0f + (1.0f - a) * 62.0f));
            const float shaped = std::round(x * levels) / levels;
            return lerpf(x, shaped, a);
        }
        case WarpMethod::TubeSat: {
            const float K = 3.0f;
            const float bias = 0.2f * a;
            const float shaped = (std::tanh((x + bias) * K) - std::tanh(bias * K))
                               / std::tanh(K);
            return lerpf(x, shaped, a);
        }
        case WarpMethod::TapeSat: {
            const float s = a * 3.0f;
            return x * (1.0f + s) / (1.0f + s * std::fabs(x)); // identity at a=0
        }
        case WarpMethod::Flip:
            return lerpf(x, -x, a);
        case WarpMethod::Chebyshev: {
            const float xc = clampf(x, -1.0f, 1.0f);
            const float t3 = 4.0f * xc * xc * xc - 3.0f * xc; // odd 3rd harmonic
            return lerpf(x, t3, a);
        }
        default:
            return x; // non-amplitude method
    }
}

// ---------------------------------------------------------------------------
// Buffer helpers
// ---------------------------------------------------------------------------
float warpReadCycle(const std::vector<float>& cycle, float phase) {
    const int n = (int)cycle.size();
    if (n == 0) return 0.0f;
    float fp = frac01(phase) * (float)n;
    int i0 = (int)fp;
    float t = fp - (float)i0;
    if (i0 >= n) i0 -= n;
    int i1 = i0 + 1; if (i1 >= n) i1 -= n;
    return cycle[i0] * (1.0f - t) + cycle[i1] * t;
}

void applyWarpChain(const std::vector<WarpOp>& ops, std::vector<float>& cycle) {
    const int n = (int)cycle.size();
    if (n == 0) return;

    for (const auto& op : ops) {
        if (!op.enabled || op.method == WarpMethod::None) continue;
        const WarpDomain dom = warpDomainOf(op.method);

        if (dom == WarpDomain::Phase) {
            std::vector<float> out(n);
            for (int i = 0; i < n; ++i) {
                const float p = (float)i / (float)n;
                const float wp = warpPhaseValue(op.method, p, op.amount);
                out[i] = warpReadCycle(cycle, wp);
            }
            cycle.swap(out);
        } else if (dom == WarpDomain::Amplitude) {
            for (int i = 0; i < n; ++i)
                cycle[i] = warpAmpValue(op.method, cycle[i], op.amount);
        }
        // Modulation / Spectral / Wavelet / Granular: handled by their frame
        // types (Bucket B/C), not in this generic cycle pass.
    }
}

// ---------------------------------------------------------------------------
// Compact warp-chain (de)serialization (see warp.h for the grammar).
// ---------------------------------------------------------------------------
std::string encodeWarpChain(const std::vector<WarpOp>& chain) {
    std::ostringstream o;
    o << chain.size();
    for (const auto& w : chain)
        o << ":" << (int)w.method << ";" << w.amount << ";"
          << w.aux << ";" << (w.enabled ? 1 : 0);
    return o.str();
}

std::vector<WarpOp> decodeWarpChain(const std::string& s) {
    std::vector<WarpOp> chain;
    auto splitOn = [](const std::string& str, char d) {
        std::vector<std::string> out;
        size_t p = 0;
        while (p <= str.size()) {
            size_t n = str.find(d, p);
            if (n == std::string::npos) n = str.size();
            out.push_back(str.substr(p, n - p));
            p = n + 1;
        }
        return out;
    };
    auto segs = splitOn(s, ':');
    // segs[0] = count (advisory); segs[1..] = ops "m;a;aux;en".
    for (size_t i = 1; i < segs.size(); ++i) {
        auto fld = splitOn(segs[i], ';');
        WarpOp w;
        if (fld.size() >= 1) { try { w.method = (WarpMethod)std::stoi(fld[0]); } catch (...) {} }
        if (fld.size() >= 2) { try { w.amount = std::stof(fld[1]); } catch (...) {} }
        if (fld.size() >= 3) { try { w.aux    = std::stof(fld[2]); } catch (...) {} }
        if (fld.size() >= 4) { w.enabled = (fld[3] != "0"); }
        chain.push_back(w);
    }
    return chain;
}

// ---------------------------------------------------------------------------
// Built-in morph presets (curated Type-2 / Bucket A chains)
// ---------------------------------------------------------------------------
const std::vector<BuiltinMorphChain>& builtinMorphChains() {
    auto op = [](WarpMethod m, float amount, float aux = 0.0f) {
        WarpOp w; w.method = m; w.amount = amount; w.aux = aux; w.enabled = true;
        return w;
    };
    static const std::vector<BuiltinMorphChain> chains = {
        { kBuiltinMorphIdBase + 0, "Warm Saturation",
          "Gentle tube + soft-clip drive - rounds peaks and adds even-harmonic warmth.",
          { op(WarpMethod::TubeSat, 0.35f), op(WarpMethod::SoftClip, 0.30f) } },

        { kBuiltinMorphIdBase + 1, "West Coast Fold",
          "Wavefolder into a touch of soft clip - dense, metallic Buchla-style harmonics.",
          { op(WarpMethod::Wavefold, 0.55f), op(WarpMethod::SoftClip, 0.20f) } },

        { kBuiltinMorphIdBase + 2, "Lo-Fi Crush",
          "Bit/sample-style amplitude quantize plus hard clip - gritty 8-bit character.",
          { op(WarpMethod::Quantize, 0.45f), op(WarpMethod::HardClip, 0.25f) } },

        { kBuiltinMorphIdBase + 3, "Tape Glue",
          "Symmetric tape-style soft saturation - odd-harmonic glue, subtle thickening.",
          { op(WarpMethod::TapeSat, 0.40f) } },

        { kBuiltinMorphIdBase + 4, "Pulse Width",
          "Duty-cycle skew - a PWM-style pulse-width feel on any waveform.",
          { op(WarpMethod::PwmSkew, 0.50f) } },

        { kBuiltinMorphIdBase + 5, "Soft Bend",
          "Pinches the cycle toward its end - asymmetric, vowel-like brightening.",
          { op(WarpMethod::BendPlus, 0.45f) } },

        { kBuiltinMorphIdBase + 6, "Formant Sync",
          "Self-sync read-restart - formant-shift / hard-sync sweep on the existing wave.",
          { op(WarpMethod::SelfSync, 0.40f) } },

        { kBuiltinMorphIdBase + 7, "Rectify Octave",
          "Folds the negative half up then warms it - octave-up even-harmonic edge.",
          { op(WarpMethod::Rectify, 0.60f), op(WarpMethod::SoftClip, 0.20f) } },
    };
    return chains;
}

const BuiltinMorphChain* builtinMorphChain(int id) {
    for (const auto& c : builtinMorphChains())
        if (c.id == id) return &c;
    return nullptr;
}

bool isBuiltinMorphAssetId(int id) {
    return id >= kBuiltinMorphIdBase
        && id <  SoundShop::AssetLibrary::kUserIdBase;
}

void seedBuiltinMorphLibrary(SoundShop::AssetLibrary& lib) {
    using namespace SoundShop;
    for (const auto& c : builtinMorphChains()) {
        if (lib.find(c.id)) continue;   // idempotent: already seeded
        AssetEntry e;
        e.id          = c.id;
        e.kind        = AssetKind::MorphAlgorithm;
        e.name        = c.name;
        e.subType     = "";
        e.payload     = encodeWarpChain(c.ops);
        e.contentHash = AssetLibrary::computeHash(e.kind, e.subType, e.payload);
        e.archived    = false;
        e.starred     = true;   // built-ins are curated favourites
        lib.insertRaw(std::move(e));
    }
}

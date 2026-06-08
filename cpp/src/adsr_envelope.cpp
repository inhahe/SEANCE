#include "adsr_envelope.h"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace SoundShop {

// ==============================================================================
// AHDSREnvelope
// ==============================================================================
AHDSREnvelope::AHDSREnvelope() {
    setDefaultCurves(*this);
}

void AHDSREnvelope::setDefaultCurves(AHDSREnvelope& e) {
    // Linear ramps. Attack rises 0..1 -> "x". Decay and Release fall 1..0
    // -> "1-x". We choose Equation mode because it survives encode/decode
    // losslessly and is the most user-readable starting point.
    e.attackCurve.mode = SpectralCurve::Equation;
    e.attackCurve.expression = "x";
    e.attackCurve.freehandMode = false;

    e.decayCurve.mode = SpectralCurve::Equation;
    e.decayCurve.expression = "1-x";
    e.decayCurve.freehandMode = false;

    e.releaseCurve.mode = SpectralCurve::Equation;
    e.releaseCurve.expression = "1-x";
    e.releaseCurve.freehandMode = false;
}

// '|'-delimited field encoding. We use ';' as the inner separator since
// '|' is reserved for outer node.script payloads (Spectrum Tap uses the
// same convention). Curves themselves can contain ';' though, so we
// instead encode them as base64-ish (no, they're already '|'-safe but
// may contain ';'). To stay robust we wrap each field length-prefixed:
//   "ahdsrv1:a=<f>;h=<f>;d=<f>;s=<f>;r=<f>;vs=<f>;ac=<n>:<encoded>;dc=<n>:<encoded>;rc=<n>:<encoded>"
// where <n> is the byte length of the curve payload that follows the ':'.
// This makes parsing unambiguous regardless of curve content.
std::string AHDSREnvelope::encode() const {
    auto fnum = [](float v) {
        std::ostringstream o;
        o.precision(6);
        o << v;
        return o.str();
    };
    auto curveField = [](const char* key, const SpectralCurve& c) {
        std::string enc = c.encode();
        std::ostringstream o;
        o << key << "=" << enc.size() << ":" << enc;
        return o.str();
    };

    std::ostringstream o;
    o << "ahdsrv1:"
      << "a=" << fnum(attackMs) << ";"
      << "h=" << fnum(holdMs) << ";"
      << "d=" << fnum(decayMs) << ";"
      << "s=" << fnum(sustain) << ";"
      << "r=" << fnum(releaseMs) << ";"
      << "vs=" << fnum(velocitySensitivity) << ";"
      << curveField("ac", attackCurve) << ";"
      << curveField("dc", decayCurve) << ";"
      << curveField("rc", releaseCurve);
    return o.str();
}

bool AHDSREnvelope::decode(const std::string& s, AHDSREnvelope& out) {
    out = AHDSREnvelope{};
    if (s.rfind("ahdsrv1:", 0) != 0) return false;
    size_t i = 8;
    auto parseFloat = [&](const char* key, float& dst) -> bool {
        size_t klen = std::strlen(key);
        if (s.compare(i, klen, key) != 0) return false;
        i += klen;
        if (i >= s.size() || s[i] != '=') return false;
        ++i;
        size_t semi = s.find(';', i);
        if (semi == std::string::npos) semi = s.size();
        try { dst = std::stof(s.substr(i, semi - i)); }
        catch (...) { return false; }
        i = (semi < s.size()) ? semi + 1 : semi;
        return true;
    };
    auto parseCurve = [&](const char* key, SpectralCurve& dst) -> bool {
        size_t klen = std::strlen(key);
        if (s.compare(i, klen, key) != 0) return false;
        i += klen;
        if (i >= s.size() || s[i] != '=') return false;
        ++i;
        size_t colon = s.find(':', i);
        if (colon == std::string::npos) return false;
        size_t len;
        try { len = (size_t)std::stoul(s.substr(i, colon - i)); }
        catch (...) { return false; }
        i = colon + 1;
        if (i + len > s.size()) return false;
        std::string enc = s.substr(i, len);
        if (!SpectralCurve::decode(enc, dst)) return false;
        i += len;
        if (i < s.size() && s[i] == ';') ++i;
        return true;
    };
    if (!parseFloat("a",  out.attackMs))  return false;
    if (!parseFloat("h",  out.holdMs))    return false;
    if (!parseFloat("d",  out.decayMs))   return false;
    if (!parseFloat("s",  out.sustain))   return false;
    if (!parseFloat("r",  out.releaseMs)) return false;
    if (!parseFloat("vs", out.velocitySensitivity)) return false;
    if (!parseCurve("ac", out.attackCurve))  return false;
    if (!parseCurve("dc", out.decayCurve))   return false;
    if (!parseCurve("rc", out.releaseCurve)) return false;
    return true;
}

// ==============================================================================
// AHDSREnvelopeRuntime
// ==============================================================================
void AHDSREnvelopeRuntime::noteOn(float velocity01) {
    velocity01 = juce::jlimit(0.0f, 1.0f, velocity01);
    stage = Stage::Attack;
    timeInStage = 0.0;
    currentLevel = 0.0f;
    // peak is set in tick() once we know velocitySensitivity from the
    // envelope. Save velocity here for that computation.
    peak = velocity01;
    releaseStartLevel = 0.0f;
}

void AHDSREnvelopeRuntime::noteOff() {
    if (stage == Stage::Off || stage == Stage::Release) return;
    releaseStartLevel = currentLevel;
    stage = Stage::Release;
    timeInStage = 0.0;
}

void AHDSREnvelopeRuntime::hardReset() {
    stage = Stage::Off;
    timeInStage = 0.0;
    currentLevel = 0.0f;
    peak = 1.0f;
    releaseStartLevel = 0.0f;
}

// Cheap hash combining the three curve encodings + mode-dependent state.
// We re-bake only when this changes. Re-baking is ~256 expression evals
// per stage so it's not free.
static size_t hashCurveSet(const AHDSREnvelope& env) {
    std::hash<std::string> h;
    size_t r = 0;
    auto mix = [&](size_t v) { r ^= v + 0x9e3779b9 + (r << 6) + (r >> 2); };
    mix(h(env.attackCurve.encode()));
    mix(h(env.decayCurve.encode()));
    mix(h(env.releaseCurve.encode()));
    return r;
}

void AHDSREnvelopeRuntime::prepareCurves(const AHDSREnvelope& env) {
    size_t hh = hashCurveSet(env);
    if (hh == lastCurveHash && (int)attackTable.size() == kTableSize) return;
    lastCurveHash = hh;
    attackTable  = env.attackCurve.evaluate(kTableSize);
    decayTable   = env.decayCurve.evaluate(kTableSize);
    releaseTable = env.releaseCurve.evaluate(kTableSize);
    // Defensive: if any failed to return the requested size, pad with linear.
    auto fixup = [](std::vector<float>& v, bool rising) {
        if ((int)v.size() == kTableSize) return;
        v.resize(kTableSize);
        for (int i = 0; i < kTableSize; ++i) {
            float t = (float)i / (float)(kTableSize - 1);
            v[i] = rising ? t : 1.0f - t;
        }
    };
    fixup(attackTable,  true);
    fixup(decayTable,   false);
    fixup(releaseTable, false);
}

static float sampleTable(const std::vector<float>& tbl, float t) {
    if (tbl.empty()) return t;
    t = juce::jlimit(0.0f, 1.0f, t);
    float pos = t * (float)(tbl.size() - 1);
    int i = (int)pos;
    int j = std::min(i + 1, (int)tbl.size() - 1);
    float frac = pos - (float)i;
    return tbl[i] * (1.0f - frac) + tbl[j] * frac;
}

float AHDSREnvelopeRuntime::tick(float sampleRate, const AHDSREnvelope& env) {
    if (stage == Stage::Off) return 0.0f;
    // Velocity scaling: scale the *target peak* (not the instantaneous
    // output) so the envelope shape is preserved across velocities.
    float velPeak = juce::jmap(env.velocitySensitivity, 0.0f, 1.0f,
                                1.0f, std::max(0.0f, peak));
    timeInStage += 1.0 / std::max(1.0, (double)sampleRate);

    switch (stage) {
        case Stage::Attack: {
            float dur = std::max(0.0005f, env.attackMs * 0.001f);
            float t = (float)(timeInStage / dur);
            if (t >= 1.0f) {
                currentLevel = velPeak;
                stage = (env.holdMs > 0.0f) ? Stage::Hold : Stage::Decay;
                timeInStage = 0.0;
            } else {
                currentLevel = sampleTable(attackTable, t) * velPeak;
            }
            break;
        }
        case Stage::Hold: {
            currentLevel = velPeak;
            if (timeInStage * 1000.0 >= (double)env.holdMs) {
                stage = Stage::Decay;
                timeInStage = 0.0;
            }
            break;
        }
        case Stage::Decay: {
            float dur = std::max(0.0005f, env.decayMs * 0.001f);
            float t = (float)(timeInStage / dur);
            float sustainLevel = juce::jlimit(0.0f, 1.0f, env.sustain) * velPeak;
            if (t >= 1.0f) {
                currentLevel = sustainLevel;
                stage = Stage::Sustain;
                timeInStage = 0.0;
            } else {
                // decayTable goes 1 -> 0; map onto velPeak -> sustainLevel.
                float shape = sampleTable(decayTable, t);
                currentLevel = sustainLevel + (velPeak - sustainLevel) * shape;
            }
            break;
        }
        case Stage::Sustain: {
            currentLevel = juce::jlimit(0.0f, 1.0f, env.sustain) * velPeak;
            break;
        }
        case Stage::Release: {
            float dur = std::max(0.0005f, env.releaseMs * 0.001f);
            float t = (float)(timeInStage / dur);
            if (t >= 1.0f) {
                currentLevel = 0.0f;
                stage = Stage::Off;
            } else {
                // releaseTable goes 1 -> 0; we scale to releaseStartLevel
                // so a key released mid-decay starts the release from
                // wherever the envelope currently sits, not from peak.
                float shape = sampleTable(releaseTable, t);
                currentLevel = releaseStartLevel * shape;
            }
            break;
        }
        case Stage::Off:
            currentLevel = 0.0f;
            break;
    }
    return juce::jlimit(0.0f, 1.0f, currentLevel);
}

} // namespace SoundShop

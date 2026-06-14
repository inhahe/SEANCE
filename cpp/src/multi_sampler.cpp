#include "multi_sampler.h"
#include "signal_modulation.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <sstream>

namespace SoundShop {

// ---- Lanczos-4 windowed sinc interpolation helpers ----
// sinc(x) = sin(pi*x) / (pi*x), with sinc(0) = 1.
static inline float sinc(float x) {
    if (std::abs(x) < 1e-6f) return 1.0f;
    const float px = 3.14159265358979323846f * x;
    return std::sin(px) / px;
}
// Lanczos window: sinc(x/a) for |x| < a, else 0. a = 4 for Lanczos-4.
static inline float lanczos4(float x) {
    if (std::abs(x) >= 4.0f) return 0.0f;
    return sinc(x) * sinc(x * 0.25f);  // sinc(x) * sinc(x/4)
}

// ==============================================================================
// SamplerEnvelope
// ==============================================================================

float SamplerEnvelope::evaluate(float t, bool noteHeld) const {
    if (points.empty()) return 1.0f;
    if (points.size() == 1) return points[0].value;

    // Sustain: while the note is held, clamp playback time to the
    // sustainEnd point so the envelope freezes at that value until
    // note-off. This is the simplest interpretation of IT's "sustain
    // loop" - IT also supports looping between sustainStart and
    // sustainEnd, which we could add later if needed.
    if (noteHeld && hasSustain
        && sustainEnd >= 0 && sustainEnd < (int)points.size())
    {
        float susT = points[sustainEnd].time;
        if (t > susT) t = susT;
    }

    if (t <= points.front().time) return points.front().value;
    if (t >= points.back().time)  return points.back().value;

    // Linear interpolation between adjacent points - matches
    // OpenMPT / Impulse Tracker envelope rendering.
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        const auto& a = points[i];
        const auto& b = points[i + 1];
        if (t >= a.time && t <= b.time) {
            float span = b.time - a.time;
            if (span < 1e-9f) return b.value;
            float f = (t - a.time) / span;
            return a.value + (b.value - a.value) * f;
        }
    }
    return points.back().value;
}

// ==============================================================================
// Encode / decode
// ==============================================================================
//
// Plain-text multi-line format stored as node.script:
//
//   __multisampler__:
//   version=1
//   volume=0.5
//   pan=0.0
//   attack=0.005
//   decay=0.05
//   sustain=1.0
//   release=0.1
//   filterCutoff=20000
//   filterResonance=0.1
//   filterMode=3
//   volumeEnv=<n>;t1,v1;t2,v2;...[;sus=a:b][;loop=c:d]
//   panEnv=<same shape>
//   pitchEnv=<same shape>
//   filterEnv=<same shape>
//   zone
//     path=<abs path>
//     loNote=0
//     hiNote=127
//     loVel=1
//     hiVel=127
//     baseNote=60
//     fineTune=0
//     gainDb=0
//     pan=0
//     loopEnabled=0
//     loopStart=0
//     loopEnd=0
//   endzone
//
// Any number of zone...endzone blocks may follow in order. Unknown keys
// are skipped on decode so the format can grow compatibly.

static std::string encodeEnv(const SamplerEnvelope& e) {
    if (e.points.empty()) return {};
    std::ostringstream s;
    s << e.points.size();
    for (auto& p : e.points) s << ";" << p.time << "," << p.value;
    if (e.hasSustain) s << ";sus=" << e.sustainStart << ":" << e.sustainEnd;
    if (e.hasLoop)    s << ";loop=" << e.loopStart << ":" << e.loopEnd;
    return s.str();
}

static SamplerEnvelope decodeEnv(const std::string& val) {
    SamplerEnvelope e;
    if (val.empty()) return e;
    // First token is the expected point count (advisory - we trust the
    // actual parsed points rather than the header).
    std::vector<std::string> toks;
    {
        std::string cur;
        for (char c : val) {
            if (c == ';') { toks.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) toks.push_back(cur);
    }
    if (toks.empty()) return e;
    // toks[0] = point count (ignored), toks[1..] = points or metadata.
    for (size_t i = 1; i < toks.size(); ++i) {
        const auto& t = toks[i];
        if (t.rfind("sus=", 0) == 0) {
            auto val2 = t.substr(4);
            auto colon = val2.find(':');
            if (colon != std::string::npos) {
                e.sustainStart = std::atoi(val2.substr(0, colon).c_str());
                e.sustainEnd   = std::atoi(val2.substr(colon + 1).c_str());
                e.hasSustain   = true;
            }
        } else if (t.rfind("loop=", 0) == 0) {
            auto val2 = t.substr(5);
            auto colon = val2.find(':');
            if (colon != std::string::npos) {
                e.loopStart = std::atoi(val2.substr(0, colon).c_str());
                e.loopEnd   = std::atoi(val2.substr(colon + 1).c_str());
                e.hasLoop   = true;
            }
        } else {
            auto comma = t.find(',');
            if (comma != std::string::npos) {
                SamplerEnvelope::Point p;
                p.time  = (float)std::atof(t.substr(0, comma).c_str());
                p.value = (float)std::atof(t.substr(comma + 1).c_str());
                e.points.push_back(p);
            }
        }
    }
    return e;
}

std::string MultiSamplerDoc::encode() const {
    std::ostringstream s;
    s << kPrefix << "\n";
    s << "version=1\n";
    s << "attack="          << attack          << "\n";
    s << "decay="           << decay           << "\n";
    s << "sustain="         << sustain         << "\n";
    s << "release="         << release         << "\n";
    s << "filterCutoff="    << filterCutoff    << "\n";
    s << "filterResonance=" << filterResonance << "\n";
    s << "filterMode="      << filterMode      << "\n";
    if (interpMode != InterpMode::Sinc) s << "interpMode=" << (int)interpMode << "\n";
    if (amigaFilter) {
        s << "amigaFilter=1\n";
        s << "amigaFilterHz=" << amigaFilterHz << "\n";
    }
    if (!volumeEnv.empty()) s << "volumeEnv=" << encodeEnv(volumeEnv) << "\n";
    if (!panEnv.empty())    s << "panEnv="    << encodeEnv(panEnv)    << "\n";
    if (!pitchEnv.empty())  s << "pitchEnv="  << encodeEnv(pitchEnv)  << "\n";
    if (!filterEnv.empty()) s << "filterEnv=" << encodeEnv(filterEnv) << "\n";
    for (auto& z : zones) {
        s << "zone\n";
        s << "  path="        << z.samplePath    << "\n";
        s << "  loNote="      << z.loNote        << "\n";
        s << "  hiNote="      << z.hiNote        << "\n";
        s << "  loVel="       << z.loVel         << "\n";
        s << "  hiVel="       << z.hiVel         << "\n";
        s << "  baseNote="    << z.baseNote      << "\n";
        s << "  fineTune="    << z.fineTuneCents << "\n";
        s << "  gainDb="      << z.gainDb        << "\n";
        s << "  pan="         << z.pan           << "\n";
        s << "  loopEnabled=" << (z.loopEnabled ? 1 : 0) << "\n";
        s << "  loopStart="   << z.loopStart     << "\n";
        s << "  loopEnd="     << z.loopEnd       << "\n";
        s << "endzone\n";
    }
    return s.str();
}

bool MultiSamplerDoc::decode(const std::string& script) {
    // Require the sentinel prefix.
    if (script.rfind(kPrefix, 0) != 0) return false;

    *this = MultiSamplerDoc{};

    std::istringstream ss(script);
    std::string line;
    // Skip the prefix line (it may end with newline or be the first
    // characters of the first line).
    std::getline(ss, line);

    MultiSamplerZone* curZone = nullptr;

    auto trim = [](std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'
                         || s[b - 1] == '\r')) --b;
        s = s.substr(a, b - a);
    };

    while (std::getline(ss, line)) {
        trim(line);
        if (line.empty()) continue;

        if (line == "zone") {
            zones.emplace_back();
            curZone = &zones.back();
            continue;
        }
        if (line == "endzone") { curZone = nullptr; continue; }

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        trim(key); trim(val);

        if (curZone) {
            if      (key == "path")        curZone->samplePath    = val;
            else if (key == "loNote")      curZone->loNote        = std::atoi(val.c_str());
            else if (key == "hiNote")      curZone->hiNote        = std::atoi(val.c_str());
            else if (key == "loVel")       curZone->loVel         = std::atoi(val.c_str());
            else if (key == "hiVel")       curZone->hiVel         = std::atoi(val.c_str());
            else if (key == "baseNote")    curZone->baseNote      = std::atoi(val.c_str());
            else if (key == "fineTune")    curZone->fineTuneCents = (float)std::atof(val.c_str());
            else if (key == "gainDb")      curZone->gainDb        = (float)std::atof(val.c_str());
            else if (key == "pan")         curZone->pan           = (float)std::atof(val.c_str());
            else if (key == "loopEnabled") curZone->loopEnabled   = (val == "1" || val == "true");
            else if (key == "loopStart")   curZone->loopStart     = std::atoi(val.c_str());
            else if (key == "loopEnd")     curZone->loopEnd       = std::atoi(val.c_str());
        } else {
            if      (key == "version")         {/* header-only */}
            else if (key == "attack")          attack          = (float)std::atof(val.c_str());
            else if (key == "decay")           decay           = (float)std::atof(val.c_str());
            else if (key == "sustain")         sustain         = (float)std::atof(val.c_str());
            else if (key == "release")         release         = (float)std::atof(val.c_str());
            else if (key == "filterCutoff")    filterCutoff    = (float)std::atof(val.c_str());
            else if (key == "filterResonance") filterResonance = (float)std::atof(val.c_str());
            else if (key == "filterMode")      filterMode      = std::atoi(val.c_str());
            else if (key == "interpMode")      interpMode      = (InterpMode)std::atoi(val.c_str());
            else if (key == "amigaFilter")     amigaFilter     = (val == "1" || val == "true");
            else if (key == "amigaFilterHz")   amigaFilterHz   = (float)std::atof(val.c_str());
            else if (key == "volumeEnv")       volumeEnv       = decodeEnv(val);
            else if (key == "panEnv")          panEnv          = decodeEnv(val);
            else if (key == "pitchEnv")        pitchEnv        = decodeEnv(val);
            else if (key == "filterEnv")       filterEnv       = decodeEnv(val);
        }
    }
    return true;
}

// ==============================================================================
// Processor
// ==============================================================================

MultiSamplerProcessor::MultiSamplerProcessor(Node& n) : node(n) {
    voices.resize(32);
    // Silent-MOD-playback diagnostic: log every MultiSampler that gets
    // constructed so we can see how many the project has, and what their
    // script payload looks like at construction time. We only print the
    // first 80 chars so the log stays scannable.
    std::string scriptPrefix = node.script.substr(0, 80);
    std::fprintf(stderr,
                 "[MultiSampler] ctor: node id=%d name='%s' scriptLen=%d head='%s'\n",
                 node.id, node.name.c_str(), (int)node.script.size(),
                 scriptPrefix.c_str());
    std::fflush(stderr);
}

void MultiSamplerProcessor::prepareToPlay(double sr, int /*bs*/) {
    sampleRate = sr;
    reloadIfNeeded();

    // ---- Amiga reconstruction-filter coefficients ----
    // RBJ biquad LP (Butterworth, Q = 1/sqrt(2)).  Two cascaded stages
    // give a 4-pole 24 dB/oct rolloff, which matches the brick-wall
    // shape that OpenMPT/Winamp produce for MOD-era output (see analysis
    // notes in multi_sampler.h).  We compute coefs once here so the
    // per-sample inner loop stays cheap.
    {
        const double kPi = 3.14159265358979323846;
        double fc = juce::jlimit(20.0, sr * 0.45, (double)doc.amigaFilterHz);
        double w0 = 2.0 * kPi * fc / sr;
        double cw = std::cos(w0), sw = std::sin(w0);
        double Q  = 0.70710678; // Butterworth
        double alpha = sw / (2.0 * Q);
        double b0 = (1.0 - cw) * 0.5;
        double b1 = (1.0 - cw);
        double b2 = (1.0 - cw) * 0.5;
        double a0 = 1.0 + alpha;
        double a1 = -2.0 * cw;
        double a2 = 1.0 - alpha;
        amigaB0 = (float)(b0 / a0);
        amigaB1 = (float)(b1 / a0);
        amigaB2 = (float)(b2 / a0);
        amigaA1 = (float)(a1 / a0);
        amigaA2 = (float)(a2 / a0);
        for (auto& s : amigaZ1) s.fill(0.0f);
        for (auto& s : amigaZ2) s.fill(0.0f);
    }
}

void MultiSamplerProcessor::reloadIfNeeded() {
    // Short-circuit only when (a) the script is unchanged, AND (b) every
    // zone actually has sample data loaded. The earlier guard checked just
    // `!doc.zones.empty()`, which trapped us in a permanent-silence state
    // whenever loadZoneSamples() decoded zones but failed to populate any
    // of their dataL/dataR buffers (e.g. the WAV file was missing the
    // first time we entered, or AudioFormatManager couldn't open it). In
    // that state findZonesFor() skipped every zone (lengthSamples == 0)
    // and there was no path back to a retry without the user re-importing
    // the MOD - and even re-import only worked if it produced a different
    // node.script, which isn't guaranteed.
    //
    // The corrected condition: only consider ourselves "loaded" when at
    // least one zone has lengthSamples > 0. If every zone is still empty,
    // we'll retry loadZoneSamples() on the next block. This is cheap
    // (juce::File::existsAsFile is fast, and we early-exit per zone) and
    // self-healing: once the source files exist, the next block fixes it.
    auto anyZoneLoaded = [this]() {
        for (auto& z : doc.zones) if (z.lengthSamples > 0) return true;
        return false;
    };
    if (node.script == lastLoadedScript && !doc.zones.empty() && anyZoneLoaded())
        return;
    if (node.script.rfind(MultiSamplerDoc::kPrefix, 0) != 0) return;
    if (!doc.decode(node.script)) return;
    lastLoadedScript = node.script;
    loadZoneSamples();
}

void MultiSamplerProcessor::loadZoneSamples() {
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    // One-line per-load trace so silent-load failures are visible on the
    // user's stderr / debug log. We don't spam every block: reloadIfNeeded
    // only calls us when zones aren't loaded yet (or when the script
    // changed), so this fires at most once per successful load.
    int loaded = 0, missing = 0, unreadable = 0;

    for (auto& z : doc.zones) {
        z.dataL.clear();
        z.dataR.clear();
        z.lengthSamples = 0;
        z.fileSampleRate = sampleRate;

        auto file = juce::File(z.samplePath);
        if (!file.existsAsFile()) {
            ++missing;
            std::fprintf(stderr,
                         "[MultiSampler] missing sample file: '%s'\n",
                         z.samplePath.c_str());
            continue;
        }

        std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
        if (!reader) {
            ++unreadable;
            std::fprintf(stderr,
                         "[MultiSampler] no reader for '%s' (unknown format?)\n",
                         z.samplePath.c_str());
            continue;
        }

        int len = (int)reader->lengthInSamples;
        int numCh = std::min<int>(2, reader->numChannels);
        juce::AudioBuffer<float> buf(std::max(1, numCh), len);
        reader->read(&buf, 0, len, 0, true, numCh > 1);

        z.dataL.assign(buf.getReadPointer(0), buf.getReadPointer(0) + len);
        if (numCh > 1)
            z.dataR.assign(buf.getReadPointer(1), buf.getReadPointer(1) + len);
        else
            z.dataR = z.dataL; // mono -> duplicate

        z.fileSampleRate = reader->sampleRate;
        z.lengthSamples  = len;
        ++loaded;
    }

    // Always log the summary so we can correlate against UI state.
    std::fprintf(stderr,
                 "[MultiSampler] loadZoneSamples node=%d ('%s'): "
                 "%d loaded, %d missing, %d unreadable (of %d zones)\n",
                 node.id, node.name.c_str(),
                 loaded, missing, unreadable, (int)doc.zones.size());
    // For the first few zones, dump their key/vel ranges and lengthSamples
    // so we can see whether the doc decode produced anything sensible and
    // whether the note-on we receive can actually match a zone.
    const int kZonesToDump = std::min<int>(4, (int)doc.zones.size());
    for (int i = 0; i < kZonesToDump; ++i) {
        const auto& z = doc.zones[i];
        std::fprintf(stderr,
                     "[MultiSampler]   zone %d: note=[%d..%d] vel=[%d..%d] "
                     "base=%d len=%d path='%s'\n",
                     i, z.loNote, z.hiNote, z.loVel, z.hiVel, z.baseNote,
                     z.lengthSamples, z.samplePath.c_str());
    }
    std::fflush(stderr);
}

std::vector<int> MultiSamplerProcessor::findZonesFor(int note, int vel) const {
    std::vector<int> out;
    out.reserve(doc.zones.size());
    for (int i = 0; i < (int)doc.zones.size(); ++i) {
        const auto& z = doc.zones[i];
        if (z.lengthSamples <= 0) continue;
        if (note < z.loNote || note > z.hiNote) continue;
        if (vel  < z.loVel  || vel  > z.hiVel)  continue;
        out.push_back(i);
    }
    return out;
}

MultiSamplerProcessor::Voice& MultiSamplerProcessor::allocateVoice() {
    // Prefer inactive slots.
    for (auto& v : voices) if (!v.active) return v;
    // Otherwise evict the oldest (longest-held) voice.
    int oldestIdx = 0;
    float oldestT = -1;
    for (int i = 0; i < (int)voices.size(); ++i) {
        if (voices[i].timeHeld > oldestT) {
            oldestT = voices[i].timeHeld;
            oldestIdx = i;
        }
    }
    return voices[oldestIdx];
}

void MultiSamplerProcessor::processBlock(juce::AudioBuffer<float>& buf,
                                          juce::MidiBuffer& midi) {
    applySignalModulations(node, buf);
    buf.clear();

    // Pick up any runtime edits to node.script (editor committing new
    // zones / envelopes) without needing a full graph rebuild.
    reloadIfNeeded();

    // Silent-MOD-playback diagnostic: log the first block so we can confirm
    // the graph is actually pumping this processor at all. If the sampler
    // never logs this, the issue is upstream (graph topology / connection
    // ordering / processor not wired to audio thread).
    if (diagFirstProcess) {
        diagFirstProcess = false;
        std::fprintf(stderr,
                     "[MultiSampler] first processBlock node=%d ('%s') "
                     "sr=%.0f zones=%d midiEvents=%d\n",
                     node.id, node.name.c_str(), sampleRate,
                     (int)doc.zones.size(), midi.getNumEvents());
        std::fflush(stderr);
    }

    if (doc.zones.empty()) return;

    const int numSamples = buf.getNumSamples();
    const int numChannels = buf.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0) return;

    // Silent-MOD-playback diagnostic: log the first MIDI buffer we ever
    // see with events on it. Distinguishes "no MIDI ever arrives" (cable
    // not wired) from "MIDI arrives but no zones match" (zone ranges
    // wrong vs note pitches).
    if (!diagFirstMidiSeen && midi.getNumEvents() > 0) {
        diagFirstMidiSeen = true;
        std::fprintf(stderr,
                     "[MultiSampler] first MIDI seen node=%d events=%d\n",
                     node.id, midi.getNumEvents());
        std::fflush(stderr);
    }

    // ---- handle incoming MIDI ----
    for (const auto meta : midi) {
        const auto msg = meta.getMessage();
        if (msg.isNoteOn()) {
            int note = msg.getNoteNumber();
            int vel  = msg.getVelocity();
            auto zoneIdxs = findZonesFor(note, vel);

            // Silent-MOD-playback diagnostic: log the first 4 note-ons
            // with matched-zone counts. If matched=0 here even when the
            // sampler has zones, the note doesn't fall in any zone's
            // key/vel rectangle - which would point at decode or import
            // putting wrong ranges in the zones.
            if (diagNoteOnsLogged < 4) {
                ++diagNoteOnsLogged;
                std::fprintf(stderr,
                             "[MultiSampler] note-on node=%d note=%d vel=%d "
                             "matchedZones=%d\n",
                             node.id, note, vel, (int)zoneIdxs.size());
                std::fflush(stderr);
            }
            // Even after the first 4, log unmatched note-ons (a few of them)
            // so a totally-quiet sampler still produces evidence.
            if (zoneIdxs.empty() && diagUnmatchedLogged < 8) {
                ++diagUnmatchedLogged;
                std::fprintf(stderr,
                             "[MultiSampler] UNMATCHED note-on node=%d note=%d "
                             "vel=%d (zones=%d)\n",
                             node.id, note, vel, (int)doc.zones.size());
                // Show what the first couple of zones look like so we can
                // see whether ranges are wrong vs the note is out-of-range.
                int dump = std::min<int>(2, (int)doc.zones.size());
                for (int i = 0; i < dump; ++i) {
                    const auto& z = doc.zones[i];
                    std::fprintf(stderr,
                                 "[MultiSampler]   zone %d: note=[%d..%d] "
                                 "vel=[%d..%d] base=%d len=%d\n",
                                 i, z.loNote, z.hiNote, z.loVel, z.hiVel,
                                 z.baseNote, z.lengthSamples);
                }
                std::fflush(stderr);
            }

            for (int zi : zoneIdxs) {
                auto& z = doc.zones[zi];
                auto& v = allocateVoice();
                v.active = true;
                v.zoneIdx = zi;
                v.midiNote = note;
                v.velocity = vel / 127.0f;
                v.phase = 0.0;
                // Pitch shift factor: 2^((note - baseNote)/12) plus fine tune
                float semi = (float)(note - z.baseNote) + z.fineTuneCents / 100.0f;
                double pitchRatio = std::pow(2.0, semi / 12.0);
                // Rate in source-samples-per-output-sample: compensate for
                // source/output sample-rate mismatch and then pitch-shift.
                v.rate = pitchRatio * (z.fileSampleRate / sampleRate);
                v.timeHeld = 0;
                v.noteHeld = true;
                v.releasedAtTime = 0;
                v.svfLow  = {0, 0};
                v.svfBand = {0, 0};
            }
        } else if (msg.isNoteOff()) {
            int note = msg.getNoteNumber();
            for (auto& v : voices) {
                if (v.active && v.midiNote == note && v.noteHeld) {
                    v.noteHeld = false;
                    v.releasedAtTime = v.timeHeld;
                }
            }
        } else if (msg.isAllNotesOff()) {
            // Release all held notes (let them fade through their release
            // envelope) rather than killing them instantly.
            for (auto& v : voices) {
                if (v.active && v.noteHeld) {
                    v.noteHeld = false;
                    v.releasedAtTime = v.timeHeld;
                }
            }
        } else if (msg.isAllSoundOff()) {
            // CC 120 = instant silence - kill everything immediately.
            for (auto& v : voices) v.active = false;
        }
    }

    // ---- render voices ----
    // Global Volume/Pan live as real node.params so automation lanes
    // and Signal cables can target them. Fall back to sensible defaults
    // if the node was created without them.
    auto paramByName = [&](const char* name, float def) -> float {
        for (auto& p : node.params) if (p.name == name) return p.value;
        return def;
    };
    const float globalVol = paramByName("Volume", 0.5f);

    const double dtPerSample = 1.0 / sampleRate;

    for (int s = 0; s < numSamples; ++s) {
        float outL = 0, outR = 0;

        for (auto& v : voices) {
            if (!v.active) continue;
            const auto& z = doc.zones[v.zoneIdx];
            if (z.lengthSamples <= 0) { v.active = false; continue; }

            // ---- envelope (volumeEnv breakpoint OR ADSR) ----
            float envGain = 1.0f;
            if (!doc.volumeEnv.empty()) {
                envGain = doc.volumeEnv.evaluate(v.timeHeld, v.noteHeld);
                if (!v.noteHeld && v.timeHeld >= doc.volumeEnv.points.back().time + doc.release) {
                    v.active = false;
                    continue;
                }
            } else {
                // Simple ADSR: attack -> decay -> sustain (held) -> release
                if (v.noteHeld) {
                    if (v.timeHeld < doc.attack)
                        envGain = v.timeHeld / std::max(1e-6f, doc.attack);
                    else if (v.timeHeld < doc.attack + doc.decay) {
                        float f = (v.timeHeld - doc.attack)
                                  / std::max(1e-6f, doc.decay);
                        envGain = 1.0f + (doc.sustain - 1.0f) * f;
                    } else {
                        envGain = doc.sustain;
                    }
                } else {
                    float tRel = v.timeHeld - v.releasedAtTime;
                    float envAtRelease = doc.sustain;
                    // If released early (during A/D), evaluate the envelope
                    // at the release moment for a smoother tail.
                    if (v.releasedAtTime < doc.attack)
                        envAtRelease = v.releasedAtTime
                                       / std::max(1e-6f, doc.attack);
                    else if (v.releasedAtTime < doc.attack + doc.decay) {
                        float f = (v.releasedAtTime - doc.attack)
                                  / std::max(1e-6f, doc.decay);
                        envAtRelease = 1.0f + (doc.sustain - 1.0f) * f;
                    }
                    float relFrac = tRel / std::max(1e-6f, doc.release);
                    envGain = envAtRelease * std::max(0.0f, 1.0f - relFrac);
                    if (tRel >= doc.release) { v.active = false; continue; }
                }
            }

            // ---- sample lookup (linear interp, stereo) ----
            double idx = v.phase;
            int i0 = (int)idx;
            int i1 = i0 + 1;
            // Loop handling: when looping is enabled and the read head
            // crosses loopEnd while the note is held, wrap back to loopStart.
            // When the note is released, play straight through to the end
            // of the sample regardless of loop points.
            if (z.loopEnabled && v.noteHeld
                && z.loopEnd > z.loopStart && z.loopEnd <= z.lengthSamples)
            {
                if (i0 >= z.loopEnd) {
                    int loopLen = z.loopEnd - z.loopStart;
                    while (i0 >= z.loopEnd) i0 -= loopLen;
                    while (i0 < z.loopStart) i0 += loopLen;
                    i1 = i0 + 1;
                    if (i1 >= z.loopEnd) i1 = z.loopStart;
                    v.phase = i0 + (idx - (int)idx);
                }
            }
            if (i0 >= z.lengthSamples || i1 >= z.lengthSamples) {
                v.active = false;
                continue;
            }
            float frac = (float)(idx - (int)idx);

            // ---- sample interpolation ----
            // Mode is per-instrument: Linear for authentic tracker sound,
            // Sinc for highest quality, Cubic as a middle ground.
            float sL = 0.0f, sR = 0.0f;
            bool looping = z.loopEnabled && v.noteHeld
                           && z.loopEnd > z.loopStart
                           && z.loopEnd <= z.lengthSamples;

            auto wrapSample = [&](int si) -> int {
                if (looping) {
                    int loopLen = z.loopEnd - z.loopStart;
                    while (si >= z.loopEnd)  si -= loopLen;
                    while (si < z.loopStart) si += loopLen;
                } else {
                    si = std::clamp(si, 0, z.lengthSamples - 1);
                }
                return si;
            };

            switch (doc.interpMode) {
            case InterpMode::Linear: {
                // 2-point linear - authentic tracker sound.
                int s0 = wrapSample(i0), s1 = wrapSample(i0 + 1);
                sL = z.dataL[s0] + frac * (z.dataL[s1] - z.dataL[s0]);
                sR = z.dataR[s0] + frac * (z.dataR[s1] - z.dataR[s0]);
                break;
            }
            case InterpMode::Cubic: {
                // 4-point Catmull-Rom (Hermite) interpolation.
                int im1 = wrapSample(i0 - 1);
                int si0 = wrapSample(i0);
                int si1 = wrapSample(i0 + 1);
                int si2 = wrapSample(i0 + 2);
                float f2 = frac * frac, f3 = f2 * frac;
                auto hermite = [&](const std::vector<float>& d) {
                    float a = d[im1], b = d[si0], c = d[si1], e = d[si2];
                    return b + 0.5f * frac * (c - a)
                        + f2 * (a - 2.5f * b + 2.0f * c - 0.5f * e)
                        + f3 * (1.5f * (b - c) + 0.5f * (e - a));
                };
                sL = hermite(z.dataL);
                sR = hermite(z.dataR);
                break;
            }
            default: // InterpMode::Sinc
            case InterpMode::Sinc: {
                // 8-point Lanczos-4 windowed sinc - highest quality.
                for (int k = -3; k <= 4; ++k) {
                    int si = wrapSample(i0 + k);
                    float w = lanczos4(frac - (float)k);
                    sL += z.dataL[si] * w;
                    sR += z.dataR[si] * w;
                }
                break;
            }
            }

            // ---- filter (state variable) ----
            if (doc.filterMode != 3) {
                float cutoffHz = doc.filterCutoff;
                // Filter envelope modulates cutoff as a multiplier in [0..1].
                if (!doc.filterEnv.empty()) {
                    float mod = doc.filterEnv.evaluate(v.timeHeld, v.noteHeld);
                    cutoffHz *= mod;
                }
                cutoffHz = juce::jlimit(20.0f, (float)(sampleRate * 0.49), cutoffHz);
                const float kPi = 3.14159265358979323846f;
                float f = 2.0f * std::sin(kPi * cutoffHz / (float)sampleRate);
                float q = 1.0f - juce::jlimit(0.0f, 0.99f, doc.filterResonance);
                for (int c = 0; c < 2; ++c) {
                    float in = c == 0 ? sL : sR;
                    float lo = v.svfLow[c]  + f * v.svfBand[c];
                    float hi = in - lo - q * v.svfBand[c];
                    float bp = f * hi + v.svfBand[c];
                    v.svfLow[c]  = lo;
                    v.svfBand[c] = bp;
                    float outF = doc.filterMode == 0 ? lo
                                : doc.filterMode == 1 ? hi
                                : bp;
                    if (c == 0) sL = outF; else sR = outF;
                }
            }

            // ---- per-zone gain / pan ----
            float gLin = std::pow(10.0f, z.gainDb / 20.0f);
            // Per-zone pan (equal-power).
            float zp = juce::jlimit(-1.0f, 1.0f, z.pan);
            float zpL = std::cos((zp + 1.0f) * 0.25f * 3.14159265358979323846f);
            float zpR = std::sin((zp + 1.0f) * 0.25f * 3.14159265358979323846f);

            float vGain = envGain * v.velocity * gLin;

            // Pan envelope contribution (adds to zone pan).
            if (!doc.panEnv.empty()) {
                float panMod = doc.panEnv.evaluate(v.timeHeld, v.noteHeld);
                zp = juce::jlimit(-1.0f, 1.0f, zp + panMod);
                zpL = std::cos((zp + 1.0f) * 0.25f * 3.14159265358979323846f);
                zpR = std::sin((zp + 1.0f) * 0.25f * 3.14159265358979323846f);
            }

            outL += sL * vGain * zpL * 1.41421356f; // *sqrt(2) keeps center loud
            outR += sR * vGain * zpR * 1.41421356f;

            // Advance phase and time.
            // Pitch envelope modulates the read rate.
            double rate = v.rate;
            if (!doc.pitchEnv.empty()) {
                float semiOffset = doc.pitchEnv.evaluate(v.timeHeld, v.noteHeld);
                rate *= std::pow(2.0, semiOffset / 12.0);
            }
            v.phase += rate;
            v.timeHeld += (float)dtPerSample;
        }

        // ---- global volume ----
        // Pan is NOT applied here - the PanProcessor in the graph chain
        // handles it, using the node's Pan param with the correct pan law
        // (equal-power for DAW instruments, linear for tracker imports).
        // Applying it here too would double-apply the panning.
        outL *= globalVol;
        outR *= globalVol;

        // ---- Amiga PAULA-style reconstruction filter ----
        // 4-pole Butterworth LP at amigaFilterHz applied to the post-mix
        // output (two cascaded Direct-Form-II Transposed biquads per
        // channel).  Without it, MOD/S3M imports sound markedly brighter
        // than the OpenMPT/Winamp reference because the sampler's
        // polyphase aliasing isn't being band-limited the way the Amiga
        // PAULA chip's analog reconstruction filter would have done.
        if (doc.amigaFilter) {
            auto runStage = [&](float in, int stage, int ch) -> float {
                float out = amigaB0 * in + amigaZ1[stage][ch];
                amigaZ1[stage][ch] = amigaB1 * in - amigaA1 * out
                                     + amigaZ2[stage][ch];
                amigaZ2[stage][ch] = amigaB2 * in - amigaA2 * out;
                return out;
            };
            outL = runStage(runStage(outL, 0, 0), 1, 0);
            outR = runStage(runStage(outR, 0, 1), 1, 1);
        }

        buf.setSample(0, s, outL);
        if (numChannels > 1) buf.setSample(1, s, outR);
    }
}

} // namespace SoundShop

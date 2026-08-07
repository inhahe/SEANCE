#include "self_test.h"
#include "terrain_synth.h"
#include "layered_wave_editor.h"   // WavetableDoc - warp serialization round-trip
#include "warp.h"                  // warp primitives + registry
#include "buffer_warp.h"           // Bucket C whole-buffer spectral/wavelet warps
#include "spectral_editor.h"       // SpectralDoc - Bucket C per-bin warp
#include "wavelet_frame.h"         // WaveletFrame - Bucket C per-coeff warp
#include "granular_frame.h"        // GranularFrame - Bucket C per-grain warp
#include "granular_freeze.h"       // GrainFreezeVoice - SingleCycle / freeze modes
#include "inharmonic_frame.h"      // InharmonicFrame - Milestone 9 additive stack
#include "waveform_bank.h"         // WaveformBank - factory single-cycle library
#include "transport.h"
#include "node_graph.h"
#include "content_store.h"         // ContentStore - baked-blob side-store tests
#include "project_file.h"          // serializeForUndo / writeProject - blob persistence
#include "asset_import.h"           // importAssets - cross-project asset merge
#include "spectrum_tap.h"           // SpectrumTap FrequencyGraph live references
#include "pitch_detect.h"           // detectPitchYIN / detectPitchAutocorrelation
#include "adsr_envelope.h"
#include "video_decoder.h"
#include "script_runtime.h"        // ScriptLang / scriptLangAvailable - generator tests
#include "scripting.h"             // ScriptEngine::bakeTerrain - Python generator tests
#include "glsl_compute.h"          // headless GL 4.3 compute - GLSL generator backend
#include "shape_expr.h"            // bakeShapeExpr (Builtin/Lua/Python/GLSL curve bakes)
#include "builtin_synth.h"         // WaveExprParser - Builtin expression vocabulary
#include "builtin_effects.h"       // ParametricEQProcessor - variable EQ band count
#include "convolution_processor.h"  // ConvolutionProcessor - PDC latency reporting
#include "voice_allocator.h"       // VoiceAllocator - per-voice polyphony policy
#include "poly_voice_processor.h"   // PolyVoiceProcessor - end-to-end voice audio
#include "signal_math.h"            // SignalMathProcessor - modular-kit math module
#include "signal_lfo.h"             // SignalLFOProcessor - modular-kit LFO module
#include "signal_sample_hold.h"     // SampleHoldProcessor - modular-kit S&H module
#include "signal_logic.h"           // SignalLogicProcessor - modular-kit logic module
#include "signal_filter.h"          // SignalFilterProcessor - modular-kit resonant filter
#include "signal_noise.h"           // SignalNoiseProcessor - modular-kit gated noise
#include "signal_oscillator.h"      // SignalOscillatorProcessor - Signal-driven oscillator
#include "pitch_core.h"             // PhaseVocoderShifter - in-house pitch-shift core
#include "pitch_shift_processor.h"  // PitchShiftProcessor - the Pitch Shift node
#include "graph_processor.h"        // AudioTimelineProcessor - audio-clip playback
#include "time_gate_processor.h"    // TimeGateProcessor - local-beat effect-layer gating
#include "dialog_helpers.h"         // AppLookAndFeel / ToolDialogWindow - taskbar flags
#include "multitrack_recorder.h"    // MultitrackRecorder - live input capture
#include "pan_processor.h"          // PanProcessor - the mute/solo/record-mute chokepoint
#include "soundfont_processor.h"    // SoundFontProcessor - .sf2 / .sfz instrument node
#include "signal_shape_node.h"      // SignalShapeProcessor - the scriptable Signal node
#include "midi_script_node.h"       // MidiScriptProcessor - algorithmic MIDI generator

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_graphics/juce_graphics.h>

#include <vector>
#include <string>
#include <functional>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <set>
#include <chrono>

namespace SoundShop {
namespace {

// ---------------------------------------------------------------------------
// Report accumulator. Every check funnels through check()/section() so the
// report file and the pass/fail tally stay in sync.
// ---------------------------------------------------------------------------
struct Report {
    juce::String text;
    int passed = 0, failed = 0, knownBugs = 0;

    void line(const juce::String& s) { text << s << "\n"; }
    void section(const juce::String& s) {
        text << "\n=== " << s << " ===\n";
    }
    bool check(bool cond, const juce::String& what) {
        text << (cond ? "  [PASS] " : "  [FAIL] ") << what << "\n";
        if (cond) ++passed; else ++failed;
        return cond;
    }
    // Numeric check with the measured value appended for diagnosis.
    bool checkVal(bool cond, const juce::String& what, double value) {
        text << (cond ? "  [PASS] " : "  [FAIL] ") << what
             << "  (measured " << juce::String(value, 5) << ")\n";
        if (cond) ++passed; else ++failed;
        return cond;
    }
    // Known-bug check ("expected failure").
    //
    // For behaviour that is genuinely wrong, is documented in known-issues.md,
    // and is not being fixed in this change. The assertion is still evaluated
    // and its measured value still recorded, but a failure is tallied
    // separately from `failed` so it does not turn the suite red. A suite that
    // is permanently red teaches people to ignore red, which costs more than
    // the bug being tracked.
    //
    // The important half is the other branch: if a known-bug check ever
    // PASSES, that is reported as a real failure. Either the bug got fixed and
    // this call plus its known-issues.md entry should be deleted, or the test
    // stopped actually testing anything. Both need a human.
    bool knownBug(bool condIfFixed, const juce::String& what,
                  double value, const juce::String& issue) {
        if (condIfFixed) {
            text << "  [FAIL] " << what
                 << " -- KNOWN BUG NOW PASSES; remove the knownBug() call and the "
                 << "'" << issue << "' entry in known-issues.md"
                 << "  (measured " << juce::String(value, 5) << ")\n";
            ++failed;
            return false;
        }
        text << "  [KNOWN-BUG] " << what << " -- tracked as '" << issue
             << "' in known-issues.md  (measured " << juce::String(value, 5) << ")\n";
        ++knownBugs;
        return true;
    }
    void note(const juce::String& s) { text << "  - " << s << "\n"; }
};

// ---------------------------------------------------------------------------
// Small numeric helpers.
// ---------------------------------------------------------------------------
double pearson(const std::vector<double>& a, const std::vector<double>& b) {
    const int n = (int) std::min(a.size(), b.size());
    if (n < 2) return 0.0;
    double ma = 0, mb = 0;
    for (int i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= n; mb /= n;
    double num = 0, da = 0, db = 0;
    for (int i = 0; i < n; ++i) {
        double x = a[i] - ma, y = b[i] - mb;
        num += x * y; da += x * x; db += y * y;
    }
    if (da < 1e-20 || db < 1e-20) return 0.0;
    return num / std::sqrt(da * db);
}

double rmsOf(const std::vector<float>& v) {
    if (v.empty()) return 0.0;
    double acc = 0;
    for (float s : v) acc += (double) s * s;
    return std::sqrt(acc / v.size());
}

float peakAbs(const std::vector<float>& v) {
    float p = 0;
    for (float s : v) p = std::max(p, std::abs(s));
    return p;
}

bool allFinite(const std::vector<float>& v) {
    for (float s : v) if (!std::isfinite(s)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Media writers.
// ---------------------------------------------------------------------------
bool writeWavFloat(const juce::File& f, const std::vector<float>& samples, double sr) {
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    juce::WavAudioFormat fmt;
    auto* os = f.createOutputStream().release();   // writer takes ownership on success
    if (os == nullptr) return false;
    std::unique_ptr<juce::AudioFormatWriter> w(
        fmt.createWriterFor(os, sr, 1, 32, {}, 0)); // 32-bit -> IEEE float -> exact round-trip
    if (w == nullptr) { delete os; return false; }
    juce::AudioBuffer<float> b(1, (int) samples.size());
    if (!samples.empty())
        std::memcpy(b.getWritePointer(0), samples.data(), samples.size() * sizeof(float));
    return w->writeFromAudioSampleBuffer(b, 0, (int) samples.size());
}

bool writePngGray(const juce::File& f, int w, int h,
                  const std::function<int(int x, int y)>& valueFn) {
    juce::Image img(juce::Image::RGB, w, h, true);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int v = juce::jlimit(0, 255, valueFn(x, y));
            img.setPixelAt(x, y, juce::Colour::fromRGB((juce::uint8) v,
                                                       (juce::uint8) v,
                                                       (juce::uint8) v));
        }
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    juce::FileOutputStream os(f);
    if (os.failedToOpen()) return false;
    juce::PNGImageFormat png;
    return png.writeImageToStream(img, os);
}

// ---------------------------------------------------------------------------
// Standalone render of a held note through a real TerrainSynthProcessor.
//
// nd          = terrain dimensionality (1, 2, 3)
// synthMode   = 0 Direct, 1 AM-sine, 2 Additive
// sigAt(d, g) = the control-signal value (0..1) feeding "Sig <axis d>" at the
//               global sample index g. This is exactly how a Signal cable
//               drives a coordinate in the node graph.
//
// On return, `audio` is the mono synth output and (AM-sine only) `refEnv[g]`
// is the amplitude envelope the output SHOULD have: volume*(0.5 + 0.5*terrain
// .sample(sig coord at g)), i.e. the terrain readout the position sweep traces.
// ---------------------------------------------------------------------------
struct RenderOut {
    std::vector<float>  audio;
    std::vector<double> refEnv;   // empty unless AM-sine
    double sr = 44100.0;
    bool   built = false;         // terrain non-empty after construction
};

RenderOut renderTerrain(const std::string& script, int nd, int synthMode,
                        const std::function<float(int, int)>& sigAt,
                        double durSec) {
    static const char* axisNames[] = { "X", "Y", "Z", "W", "V", "U", "S", "T" };

    Transport transport;
    transport.sampleRate = 44100.0;
    transport.bpm = 120.0;

    Node node;
    node.id   = 1;
    node.type = NodeType::TerrainSynth;
    node.name = "selftest-terrain";
    node.script = script;

    node.pinsIn.push_back(Pin{ 1, "MIDI", PinKind::Midi, true, 2 });
    for (int d = 0; d < nd; ++d)
        node.pinsIn.push_back(Pin{ 2 + d, std::string("Sig ") + axisNames[d],
                                   PinKind::Signal, true, 1 });
    node.pinsOut.push_back(Pin{ 100, "Audio", PinKind::Audio, false, 2 });

    node.params.push_back({ "Volume",     1.0f,            0.0f, 1.0f });
    node.params.push_back({ "Synth Mode", (float) synthMode, 0.0f, 2.0f });

    // Flat envelope: ~instant attack, very long hold at full level, full
    // sustain. Keeps the gain effectively constant for the whole render so the
    // measured amplitude tracks the terrain readout rather than an ADSR shape.
    node.ahdsrEnvelope.attackMs  = 1.0f;
    node.ahdsrEnvelope.holdMs    = (float) (durSec * 1000.0 * 4.0);
    node.ahdsrEnvelope.decayMs   = 1.0f;
    node.ahdsrEnvelope.sustain   = 1.0f;
    node.ahdsrEnvelope.releaseMs = 1.0f;
    node.ahdsrEnvelope.velocitySensitivity = 0.0f;
    AHDSREnvelope::setDefaultCurves(node.ahdsrEnvelope);

    TerrainSynthProcessor proc(node, transport);

    RenderOut ro;
    ro.sr = transport.sampleRate;
    const int total = std::max(1, (int) (durSec * ro.sr));
    const int block = 512;
    const int numCh = 2 + nd;
    proc.prepareToPlay(ro.sr, block);

    ro.audio.assign((size_t) total, 0.0f);
    Terrain& terr = proc.getTerrain();
    ro.built = terr.totalSize() > 1;

    const float volume = 1.0f;
    const bool computeRef = (synthMode == 1);   // AM-sine has a clean envelope
    if (computeRef) ro.refEnv.assign((size_t) total, 0.0);

    juce::AudioBuffer<float> buf(numCh, block);

    for (int start = 0; start < total; start += block) {
        const int n = std::min(block, total - start);
        buf.setSize(numCh, n, false, false, true);
        buf.clear();
        for (int d = 0; d < nd; ++d) {
            float* ch = buf.getWritePointer(2 + d);
            for (int s = 0; s < n; ++s)
                ch[s] = juce::jlimit(0.0f, 1.0f, sigAt(d, start + s));
        }
        juce::MidiBuffer midi;
        if (start == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8) 100), 0);

        proc.processBlock(buf, midi);

        const float* out = buf.getReadPointer(0);
        for (int s = 0; s < n; ++s) ro.audio[(size_t)(start + s)] = out[s];

        if (computeRef) {
            std::vector<float> coord((size_t) nd);
            for (int s = 0; s < n; ++s) {
                for (int d = 0; d < nd; ++d)
                    coord[(size_t) d] = juce::jlimit(0.0f, 1.0f, sigAt(d, start + s));
                float readout = terr.sample(coord);
                ro.refEnv[(size_t)(start + s)] = (double) volume * (0.5 + 0.5 * readout);
            }
        }
    }
    return ro;
}

// Chunk-average |audio| and refEnv, trim the attack/release transients, and
// correlate. Returns the Pearson correlation (~1.0 == the rendered amplitude
// follows the predicted terrain readout).
double envelopeCorrelation(const RenderOut& ro, int chunks = 200) {
    if (ro.refEnv.empty() || ro.audio.empty()) return 0.0;
    const int total = (int) ro.audio.size();
    const int chunk = std::max(1, total / chunks);
    std::vector<double> aEnv, bRef;
    for (int c = 0; c * chunk < total; ++c) {
        int s0 = c * chunk, s1 = std::min(total, s0 + chunk);
        double aAcc = 0, bAcc = 0;
        for (int s = s0; s < s1; ++s) {
            aAcc += std::abs(ro.audio[(size_t) s]);
            bAcc += ro.refEnv[(size_t) s];
        }
        int len = s1 - s0;
        aEnv.push_back(aAcc / len);
        bRef.push_back(bAcc / len);
    }
    // Trim 3 chunks at each end (attack ramp / final block edge effects).
    const int trim = 3;
    if ((int) aEnv.size() > 2 * trim + 2) {
        aEnv.erase(aEnv.end() - trim, aEnv.end());
        aEnv.erase(aEnv.begin(), aEnv.begin() + trim);
        bRef.erase(bRef.end() - trim, bRef.end());
        bRef.erase(bRef.begin(), bRef.begin() + trim);
    }
    return pearson(aEnv, bRef);
}

double envRange(const RenderOut& ro, int chunks = 200) {
    if (ro.audio.empty()) return 0.0;
    const int total = (int) ro.audio.size();
    const int chunk = std::max(1, total / chunks);
    double lo = 1e30, hi = -1e30;
    for (int c = 0; c * chunk < total; ++c) {
        int s0 = c * chunk, s1 = std::min(total, s0 + chunk);
        double acc = 0;
        for (int s = s0; s < s1; ++s) acc += std::abs(ro.audio[(size_t) s]);
        double m = acc / std::max(1, s1 - s0);
        lo = std::min(lo, m); hi = std::max(hi, m);
    }
    return hi - lo;
}

// ===========================================================================
// LAYER 1 - exact terrain-data tests
// ===========================================================================
void testTerrainData(Report& r, const juce::File& dir) {
    r.section("Layer 1: terrain data (exact)");

    // ---- 1D from audio file: linear ramp -1 .. +1 -----------------------
    {
        const int N = 512;
        std::vector<float> ramp((size_t) N);
        for (int i = 0; i < N; ++i) ramp[(size_t) i] = -1.0f + 2.0f * i / (N - 1);
        auto wav = dir.getChildFile("test_audio_1d.wav");
        bool wrote = writeWavFloat(wav, ramp, 44100.0);
        r.check(wrote, "1D: wrote test audio WAV");

        Terrain t;
        t.fillFromAudioFile(wav.getFullPathName().toStdString());
        r.check(t.numDimensions() == 1 && t.totalSize() == N,
                "1D: terrain is 1D with N samples");
        double maxErr = 0;
        for (int i = 0; i < N; ++i)
            maxErr = std::max(maxErr, (double) std::abs(t.at(i) - ramp[(size_t) i]));
        r.checkVal(maxErr < 1e-4, "1D: data matches ramp", maxErr);
        // sample() endpoints + midpoint interpolation
        r.checkVal(std::abs(t.sample({ 0.0f }) - (-1.0f)) < 1e-4, "1D: sample(0) == -1",
                   t.sample({ 0.0f }));
        r.checkVal(std::abs(t.sample({ 1.0f }) - ( 1.0f)) < 1e-4, "1D: sample(1) == +1",
                   t.sample({ 1.0f }));
        r.checkVal(std::abs(t.sample({ 0.5f })) < 1e-3, "1D: sample(0.5) ~ 0",
                   t.sample({ 0.5f }));
    }

    // ---- 2D from image: column gradient (varies along x only) -----------
    {
        const int W = 80, H = 60;
        auto png = dir.getChildFile("test_image_2d.png");
        bool wrote = writePngGray(png, W, H,
            [&](int x, int /*y*/) { return (int) std::lround(255.0 * x / (W - 1)); });
        r.check(wrote, "2D: wrote test image PNG");

        Terrain t;
        t.fillFromImage(png.getFullPathName().toStdString());
        r.check(t.numDimensions() == 2 && t.getDimensions() == std::vector<int>{ H, W },
                "2D: terrain dims == {H, W}");
        // brightness(x) = x/(W-1); data = brightness*2-1. Check a few cells.
        double maxErr = 0;
        for (int x = 0; x < W; x += 7)
            for (int y = 0; y < H; y += 11) {
                float expect = (float) x / (W - 1) * 2.0f - 1.0f;
                maxErr = std::max(maxErr, (double) std::abs(t.at(y * W + x) - expect));
            }
        r.checkVal(maxErr < 2e-2, "2D: data matches gradient (1/255 quant ok)", maxErr);
        // sample() along the column axis (coord[1]); coord[0]=row is irrelevant.
        r.checkVal(std::abs(t.sample({ 0.5f, 0.0f }) - (-1.0f)) < 2e-2,
                   "2D: sample(col 0) == -1", t.sample({ 0.5f, 0.0f }));
        r.checkVal(std::abs(t.sample({ 0.5f, 1.0f }) - ( 1.0f)) < 2e-2,
                   "2D: sample(col 1) == +1", t.sample({ 0.5f, 1.0f }));
    }

    // ---- 3D from video grid: brightness ramps with the frame (time) axis -
    {
        const int F = 12, H = 16, W = 16;
        std::vector<uint8_t> gray((size_t) F * H * W);
        for (int f = 0; f < F; ++f)
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    gray[(size_t)((f * H + y) * W + x)] =
                        (uint8_t) std::lround(255.0 * f / (F - 1));

        Terrain t;
        t.fillFromVideoData(gray, F, H, W);
        r.check(t.numDimensions() == 3 &&
                t.getDimensions() == std::vector<int>{ F, H, W },
                "3D: terrain dims == {F, H, W}");
        // The gray grid is uint8_t, so each cell is quantized to 1/255 before
        // the b/255*2-1 mapping. Intermediate frames where 255*f/(F-1) isn't an
        // integer therefore carry up to ~0.5/255*2 (~0.004) of rounding error -
        // the same 1/255 tolerance the 2D image test uses. (Frames 0 and F-1 map
        // to bytes 0/255 exactly, which is why the sample() endpoint checks below
        // can stay tight.)
        double maxErr = 0;
        for (int f = 0; f < F; ++f) {
            float expect = (float) f / (F - 1) * 2.0f - 1.0f;
            maxErr = std::max(maxErr,
                              (double) std::abs(t.at((f * H + 3) * W + 5) - expect));
        }
        r.checkVal(maxErr < 2e-2, "3D: data ramps along frame axis (1/255 quant ok)",
                   maxErr);
        r.checkVal(std::abs(t.sample({ 0.0f, 0.5f, 0.5f }) - (-1.0f)) < 1e-3,
                   "3D: sample(frame 0) == -1", t.sample({ 0.0f, 0.5f, 0.5f }));
        r.checkVal(std::abs(t.sample({ 1.0f, 0.5f, 0.5f }) - ( 1.0f)) < 1e-3,
                   "3D: sample(frame 1) == +1", t.sample({ 1.0f, 0.5f, 0.5f }));
        r.checkVal(std::abs(t.sample({ 0.5f, 0.5f, 0.5f })) < 1e-3,
                   "3D: sample(frame 0.5) ~ 0", t.sample({ 0.5f, 0.5f, 0.5f }));
    }

    // ---- video terrain script round-trip --------------------------------
    {
        VideoTerrainParams p;
        p.path = "C:/some path/with spaces/clip.mp4";
        p.t0 = 1.25; p.t1 = 3.75;
        p.cropX = 4; p.cropY = 8; p.cropW = 320; p.cropH = 240;
        p.outW = 6; p.outH = 5; p.outFrames = 4;
        p.gray.resize((size_t) p.outW * p.outH * p.outFrames);
        for (size_t i = 0; i < p.gray.size(); ++i)
            p.gray[i] = (uint8_t) (i * 7 + 11);

        auto s = makeVideoTerrainScript(p);
        VideoTerrainParams q;
        bool ok = parseVideoTerrainScript(s, q, true);
        r.check(ok, "script: parse succeeded");
        r.check(q.path == p.path, "script: path with spaces preserved");
        r.check(std::abs(q.t0 - p.t0) < 1e-6 && std::abs(q.t1 - p.t1) < 1e-6,
                "script: time crop preserved");
        r.check(q.cropX == p.cropX && q.cropY == p.cropY &&
                q.cropW == p.cropW && q.cropH == p.cropH, "script: pixel crop preserved");
        r.check(q.outW == p.outW && q.outH == p.outH && q.outFrames == p.outFrames,
                "script: grid size preserved");
        r.check(q.gray == p.gray, "script: gray bytes round-trip exactly");
    }

    // ---- script-generated terrain (Builtin always; Lua for unbounded N-D) ----
    // fillFromScript runs a per-cell program. Output contract: the script
    // returns 0..1 (heightmap/brightness), mapped to the terrain's bipolar
    // [-1,1] as v*2-1 (see Terrain::fillFromScript). Builtin source is a bare
    // expression; Lua source defines loop() and returns its value.
    {
        // Builtin: constant 0.75 -> every cell 0.75*2-1 = 0.5. Validates the
        // backend runs end-to-end without assuming coordinate-var support.
        Terrain tb;
        std::string err;
        bool ok = tb.fillFromScript(ScriptLang::Builtin, "0.75", { 4, 5 }, err);
        r.check(ok, "gen: Builtin program loads + runs");
        if (ok) {
            double maxErr = 0;
            for (int i = 0; i < tb.totalSize(); ++i)
                maxErr = std::max(maxErr, (double) std::abs(tb.at(i) - 0.5f));
            r.checkVal(maxErr < 1e-5, "gen: Builtin constant maps 0.75 -> 0.5", maxErr);
        }

        // Block-only language can't generate per-cell: deterministic rejection
        // (the rate check fails before any Wasm backend is needed).
        Terrain tw;
        std::string werr;
        bool wok = tw.fillFromScript(ScriptLang::Wasm, "0.0", { 4 }, werr);
        r.check(!wok && !werr.empty(), "gen: block-only language rejected with error");

        // Lua: the real N-D story. Gated on the Lua backend being built in.
        if (scriptLangAvailable(ScriptLang::Lua)) {
            // 2D ramp along dim 0 (rows): script returns c0 (0..1) -> -1..1.
            Terrain t2;
            std::string e2;
            bool ok2 = t2.fillFromScript(ScriptLang::Lua,
                "function loop() return c0 end", { 5, 4 }, e2);
            r.check(ok2, "gen: Lua 2D program loads + runs");
            if (ok2) {
                // dims = {5,4}; row r0 in [0..4], c0 = r0/4 -> cell = r0*4+c1.
                double maxErr = 0;
                for (int r0 = 0; r0 < 5; ++r0)
                    for (int c1 = 0; c1 < 4; ++c1) {
                        float expect = ((float) r0 / 4.0f) * 2.0f - 1.0f;
                        maxErr = std::max(maxErr,
                            (double) std::abs(t2.at(r0 * 4 + c1) - expect));
                    }
                r.checkVal(maxErr < 1e-5, "gen: Lua 2D ramp matches c0 (mapped)", maxErr);
            }

            // >8 dimensions: prove there is NO dimensionality cap. A rank-10
            // terrain (every dim size 2 -> 1024 cells) reads its 10th axis:
            // loop() returns c9, which is 0 when index9==0 (-> -1) and 1 when
            // index9==1 (-> +1). The 8-letter Builtin axis set could never name
            // c9, so this only works through the indexed coordinate API.
            std::vector<int> d10(10, 2);
            Terrain t10;
            std::string e10;
            bool ok10 = t10.fillFromScript(ScriptLang::Lua,
                "function loop() return c9 end", d10, e10);
            r.check(ok10 && t10.numDimensions() == 10,
                    "gen: Lua rank-10 terrain builds (no D cap)");
            if (ok10) {
                // dim 9 is the innermost (fastest) flat axis: even flats have
                // index9==0 (-1), odd flats index9==1 (+1).
                double maxErr = 0;
                for (int flat = 0; flat < t10.totalSize(); ++flat) {
                    float expect = (flat & 1) ? 1.0f : -1.0f;
                    maxErr = std::max(maxErr,
                        (double) std::abs(t10.at(flat) - expect));
                }
                r.checkVal(maxErr < 1e-5, "gen: Lua 10-D reads c9 correctly", maxErr);
            }
        }

        // ---- __generate__ script encode/decode round-trip ----------------
        // The node bakes its generator into node.script and reproduces the
        // terrain from it on load (no data stored). Verify the codec survives
        // newlines / pipes / odd chars and that dims + lang come back intact.
        {
            GenerateTerrainParams gp;
            gp.lang = (int)ScriptLang::Lua;
            gp.dims = { 7, 9, 3 };
            gp.source = "function loop()\n  return c0 -- pipe|brace{}newline test\nend\n";
            std::string enc = makeGenerateTerrainScript(gp);
            r.check(enc.rfind("__generate__:", 0) == 0,
                    "gen: __generate__ script has correct prefix");

            GenerateTerrainParams dec;
            bool pok = parseGenerateTerrainScript(enc, dec);
            r.check(pok, "gen: __generate__ script parses back");
            r.check(dec.lang == gp.lang, "gen: lang round-trips");
            r.check(dec.dims == gp.dims, "gen: dims round-trip (any rank)");
            r.check(dec.source == gp.source,
                    "gen: source round-trips through base64 (newlines/pipes)");

            // Non-generate scripts must be rejected.
            GenerateTerrainParams junk;
            r.check(!parseGenerateTerrainScript("__video__:foo", junk),
                    "gen: parser rejects non-generate scripts");
        }

        // ---- baked-data round-trip (gzip+base64 blob) --------------------
        // Generated terrains now bake their final grid into the tag so they
        // never re-run on load. Verify the float blob survives encode->decode
        // bit-exactly and that the size is validated against dims.
        {
            GenerateTerrainParams gp;
            gp.lang = (int)GenLang::Lua;
            gp.dims = { 4, 5 };           // 20 cells
            gp.source = "function loop() return 0.5 end\n";
            gp.data.resize(20);
            for (int i = 0; i < 20; ++i) gp.data[(size_t)i] = (float)i / 19.0f * 2.0f - 1.0f;

            std::string enc = makeGenerateTerrainScript(gp);
            GenerateTerrainParams dec;
            r.check(parseGenerateTerrainScript(enc, dec), "gen: baked tag parses");
            r.check(dec.data.size() == gp.data.size(),
                    "gen: baked data count round-trips");
            float maxErr = 0.0f;
            for (size_t i = 0; i < dec.data.size() && i < gp.data.size(); ++i)
                maxErr = std::max(maxErr, std::abs(dec.data[i] - gp.data[i]));
            r.checkVal(maxErr == 0.0f, "gen: baked floats are bit-exact", maxErr);

            // A blob whose float count doesn't match dims is dropped (forces the
            // recompute fallback rather than loading a corrupt grid).
            GenerateTerrainParams bad = gp;
            bad.dims = { 6, 6 };          // 36 != 20
            std::string encBad = makeGenerateTerrainScript(bad);
            // Re-encode with mismatched dims but the 20-float blob: hand-build by
            // swapping the dims field is awkward, so instead decode the good tag
            // against wrong-dims by editing: simplest is to verify the validated
            // path drops a short blob. Use a tag with dims that exceed the blob.
            GenerateTerrainParams decBad;
            // Build a tag: same data blob (20 floats) but dims claiming 36 cells.
            GenerateTerrainParams mk; mk.lang = gp.lang; mk.dims = { 6, 6 };
            mk.source = gp.source; mk.data = gp.data;        // 20 floats, dims=36
            std::string mismatch = makeGenerateTerrainScript(mk);
            r.check(parseGenerateTerrainScript(mismatch, decBad)
                        && decBad.data.empty(),
                    "gen: baked blob with wrong cell count is dropped");
        }

        // ---- mode field round-trip + backward compatibility --------------
        // mode 1 (whole-grid) is encoded as "<lang>:<mode>"; old projects had
        // a bare "<lang>" (no ':'), which must still parse as mode 0.
        {
            GenerateTerrainParams gp;
            gp.lang = (int)ScriptLang::Lua;
            gp.mode = 1;
            gp.dims = { 8 };
            gp.source = "function generate() end\n";
            std::string enc = makeGenerateTerrainScript(gp);
            GenerateTerrainParams dec;
            r.check(parseGenerateTerrainScript(enc, dec) && dec.mode == 1,
                    "gen: whole-grid mode round-trips");

            // Legacy tag with no mode field defaults to per-cell (mode 0).
            GenerateTerrainParams legacy;
            r.check(parseGenerateTerrainScript("__generate__:1|4,5|", legacy)
                        && legacy.mode == 0,
                    "gen: legacy tag (no mode) defaults to per-cell");
        }

        // ---- whole-grid (Lua generate()) actually fills the array ---------
        // The program runs ONCE and owns the whole grid via set()/coord(). Here
        // a 1D ramp: cell i := coord(i,0). Mapped 0..1 -> bipolar like per-cell.
        if (scriptLangAvailable(ScriptLang::Lua)) {
            Terrain tg;
            std::string gerr;
            bool gok = tg.fillFromScriptWholeGrid(ScriptLang::Lua,
                "function generate()\n"
                "  for i = 0, total - 1 do set(i, coord(i, 0)) end\n"
                "end\n",
                { 5 }, gerr);
            r.check(gok, "gen: Lua whole-grid generate() runs");
            if (gok) {
                const auto& d = tg.getData();
                float maxErr = 0.0f;
                for (int i = 0; i < (int)d.size(); ++i) {
                    float expect = ((float)i / 4.0f) * 2.0f - 1.0f;  // coord*2-1
                    maxErr = std::max(maxErr, std::abs(d[(size_t)i] - expect));
                }
                r.checkVal(maxErr < 1e-5, "gen: Lua whole-grid 1D ramp matches coord", maxErr);
            }

            // Builtin can't do whole-grid (per-cell only): must error cleanly.
            Terrain tb2;
            std::string berr;
            bool bok = tb2.fillFromScriptWholeGrid(ScriptLang::Builtin, "0.5", { 4 }, berr);
            r.check(!bok && !berr.empty(),
                    "gen: per-cell-only language rejected for whole-grid");

            // A whole-grid program missing generate() must error, not crash.
            Terrain tn;
            std::string nerr;
            bool nok = tn.fillFromScriptWholeGrid(ScriptLang::Lua,
                "function loop() return 0.5 end\n", { 4 }, nerr);
            r.check(!nok && !nerr.empty(),
                    "gen: whole-grid without generate() errors");

            // ---- N-D index helpers (flatten / coordAxis / neighbor) ----------
            // On a {3,4} grid each cell verifies: flatten(coordAxis(i,0),
            // coordAxis(i,1)) round-trips to i; flatten edge-clamps out-of-range
            // coords; neighbor steps + clamps. Each cell stores 1 iff all hold,
            // so the whole grid must come back as bipolar +1.
            Terrain tf;
            std::string ferr;
            bool fok = tf.fillFromScriptWholeGrid(ScriptLang::Lua,
                "function generate()\n"
                "  for i = 0, total - 1 do\n"
                "    local r = coordAxis(i, 0)\n"
                "    local c = coordAxis(i, 1)\n"
                "    local ok = (flatten(r, c) == i)\n"
                "    ok = ok and (flatten(-1, -1) == 0) and (flatten(99, 99) == total - 1)\n"
                "    ok = ok and (neighbor(i, 1, 1) == flatten(r, math.min(c + 1, 3)))\n"
                "    ok = ok and (neighbor(i, 0, -1) == flatten(math.max(r - 1, 0), c))\n"
                "    set(i, ok and 1 or 0)\n"
                "  end\n"
                "end\n",
                { 3, 4 }, ferr);
            r.check(fok, "gen: Lua flatten/coordAxis/neighbor run");
            if (fok) {
                const auto& d = tf.getData();
                float minV = 1.0f;
                for (float v : d) minV = std::min(minV, v);
                r.checkVal(d.size() == 12 && minV > 0.999f,
                           "gen: Lua N-D index helpers round-trip", minV);
            }

            // ---- Direct N-D pixel access (getAt / setAt) ---------------------
            // Write a known pattern with setAt(r,c,v), read it back with
            // getAt(r,c), check getAt edge-clamps out-of-range reads, and check
            // an out-of-range setAt is a no-op. Then overwrite the whole grid
            // with the all-OK flag so a correct run reads back as bipolar +1.
            Terrain tap;
            std::string aperr;
            bool apok = tap.fillFromScriptWholeGrid(ScriptLang::Lua,
                "function generate()\n"
                "  for rr = 0, 2 do for cc = 0, 3 do setAt(rr, cc, (rr*4+cc)/11) end end\n"
                "  local ok = true\n"
                "  for rr = 0, 2 do for cc = 0, 3 do\n"
                "    if math.abs(getAt(rr,cc) - (rr*4+cc)/11) > 1e-4 then ok = false end\n"
                "  end end\n"
                "  if math.abs(getAt(-1,-1) - getAt(0,0)) > 1e-6 then ok = false end\n"
                "  if math.abs(getAt(99,99) - getAt(2,3)) > 1e-6 then ok = false end\n"
                "  local before = getAt(0,0)\n"
                "  setAt(-1, 0, 1.0)\n"
                "  if math.abs(getAt(0,0) - before) > 1e-6 then ok = false end\n"
                "  for i = 0, total - 1 do set(i, ok and 1 or 0) end\n"
                "end\n",
                { 3, 4 }, aperr);
            r.check(apok, "gen: Lua getAt/setAt run");
            if (apok) {
                const auto& d = tap.getData();
                float minV = 1.0f;
                for (float v : d) minV = std::min(minV, v);
                r.checkVal(d.size() == 12 && minV > 0.999f,
                           "gen: Lua getAt/setAt round-trip + clamp + OOB", minV);
            }
        }

        // ---- Python generator backend (ScriptEngine::bakeTerrain) ----------
        // Guarded: the test build often lacks the Python DLL, in which case
        // bakeTerrain must fail cleanly (never touch the C API). When Python IS
        // present we verify both modes produce the bipolar [-1,1] grid.
        if (ScriptEngine::pythonAvailable()) {
            ScriptEngine::instance().init();

            // Per-cell: a flat 0.75 -> bipolar 0.5 in every cell.
            {
                std::vector<float> out;
                std::string err;
                bool ok = ScriptEngine::instance().bakeTerrain(
                    "0.75", /*wholeGrid*/false, { 6 }, out, err);
                r.check(ok, "gen: Python per-cell bakeTerrain runs");
                if (ok) {
                    r.check(out.size() == 6, "gen: Python per-cell fills product(dims)");
                    float maxErr = 0.0f;
                    for (float v : out) maxErr = std::max(maxErr, std::abs(v - 0.5f));
                    r.checkVal(maxErr < 1e-5, "gen: Python per-cell 0.75->bipolar 0.5", maxErr);
                }
            }

            // Per-cell coords: c0 sweeps 0..1 over a 5-cell axis -> bipolar ramp.
            {
                std::vector<float> out;
                std::string err;
                bool ok = ScriptEngine::instance().bakeTerrain(
                    "c0", /*wholeGrid*/false, { 5 }, out, err);
                r.check(ok && out.size() == 5, "gen: Python per-cell c0 axis runs");
                if (ok && out.size() == 5) {
                    float maxErr = 0.0f;
                    for (int i = 0; i < 5; ++i) {
                        float expect = ((float)i / 4.0f) * 2.0f - 1.0f;
                        maxErr = std::max(maxErr, std::abs(out[(size_t)i] - expect));
                    }
                    r.checkVal(maxErr < 1e-5, "gen: Python per-cell c0 ramp matches coord", maxErr);
                }
            }

            // Whole-grid: generate() fills a 1D ramp via set(i, coord(i,0)).
            {
                std::vector<float> out;
                std::string err;
                bool ok = ScriptEngine::instance().bakeTerrain(
                    "def generate():\n"
                    "    for i in range(total):\n"
                    "        set(i, coord(i, 0))\n",
                    /*wholeGrid*/true, { 5 }, out, err);
                r.check(ok && out.size() == 5, "gen: Python whole-grid generate() runs");
                if (ok && out.size() == 5) {
                    float maxErr = 0.0f;
                    for (int i = 0; i < 5; ++i) {
                        float expect = ((float)i / 4.0f) * 2.0f - 1.0f;
                        maxErr = std::max(maxErr, std::abs(out[(size_t)i] - expect));
                    }
                    r.checkVal(maxErr < 1e-5, "gen: Python whole-grid 1D ramp matches coord", maxErr);
                }
            }

            // Whole-grid N-D index helpers: same {3,4} round-trip as the Lua test
            // (flatten/coordAxis/neighbor), so each cell comes back as bipolar +1.
            {
                std::vector<float> out;
                std::string err;
                bool ok = ScriptEngine::instance().bakeTerrain(
                    "def generate():\n"
                    "    for i in range(total):\n"
                    "        r = coordAxis(i, 0)\n"
                    "        c = coordAxis(i, 1)\n"
                    "        ok = (flatten(r, c) == i)\n"
                    "        ok = ok and (flatten(-1, -1) == 0) and (flatten(99, 99) == total - 1)\n"
                    "        ok = ok and (neighbor(i, 1, 1) == flatten(r, min(c + 1, 3)))\n"
                    "        ok = ok and (neighbor(i, 0, -1) == flatten(max(r - 1, 0), c))\n"
                    "        set(i, 1.0 if ok else 0.0)\n",
                    /*wholeGrid*/true, { 3, 4 }, out, err);
                r.check(ok && out.size() == 12, "gen: Python flatten/coordAxis/neighbor run");
                if (ok && out.size() == 12) {
                    float minV = 1.0f;
                    for (float v : out) minV = std::min(minV, v);
                    r.checkVal(minV > 0.999f, "gen: Python N-D index helpers round-trip", minV);
                }
            }

            // Direct N-D pixel access (getAt/setAt): same checks as the Lua test -
            // pattern round-trip, edge-clamped reads, OOB-write no-op - collapsed
            // into an all-OK flag written across the grid (bipolar +1 on success).
            {
                std::vector<float> out;
                std::string err;
                bool ok = ScriptEngine::instance().bakeTerrain(
                    "def generate():\n"
                    "    for rr in range(3):\n"
                    "        for cc in range(4):\n"
                    "            setAt(rr, cc, (rr*4+cc)/11)\n"
                    "    good = True\n"
                    "    for rr in range(3):\n"
                    "        for cc in range(4):\n"
                    "            if abs(getAt(rr,cc) - (rr*4+cc)/11) > 1e-4: good = False\n"
                    "    if abs(getAt(-1,-1) - getAt(0,0)) > 1e-6: good = False\n"
                    "    if abs(getAt(99,99) - getAt(2,3)) > 1e-6: good = False\n"
                    "    before = getAt(0,0)\n"
                    "    setAt(-1, 0, 1.0)\n"
                    "    if abs(getAt(0,0) - before) > 1e-6: good = False\n"
                    "    for i in range(total):\n"
                    "        set(i, 1.0 if good else 0.0)\n",
                    /*wholeGrid*/true, { 3, 4 }, out, err);
                r.check(ok && out.size() == 12, "gen: Python getAt/setAt run");
                if (ok && out.size() == 12) {
                    float minV = 1.0f;
                    for (float v : out) minV = std::min(minV, v);
                    r.checkVal(minV > 0.999f, "gen: Python getAt/setAt round-trip + clamp + OOB", minV);
                }
            }

            // numpy ndarray store: when numpy is importable, `grid` is a 2-D
            // float64 ndarray shaped like the terrain, so `grid[r, c] = ...`
            // writes the cell directly (read back via the buffer protocol). The
            // program degrades to setAt when numpy is absent, so this test passes
            // in BOTH environments and produces the same i/11 ramp either way.
            {
                std::vector<float> out;
                std::string err;
                bool ok = ScriptEngine::instance().bakeTerrain(
                    "def generate():\n"
                    "    if grid is not None:\n"
                    "        for rr in range(3):\n"
                    "            for cc in range(4):\n"
                    "                grid[rr, cc] = (rr*4+cc)/11.0\n"
                    "    else:\n"
                    "        for rr in range(3):\n"
                    "            for cc in range(4):\n"
                    "                setAt(rr, cc, (rr*4+cc)/11.0)\n",
                    /*wholeGrid*/true, { 3, 4 }, out, err);
                r.check(ok && out.size() == 12, "gen: Python numpy grid[r,c] runs");
                if (ok && out.size() == 12) {
                    float maxErr = 0.0f;
                    for (int i = 0; i < 12; ++i) {
                        float expect = ((float)i / 11.0f) * 2.0f - 1.0f;
                        maxErr = std::max(maxErr, std::abs(out[(size_t)i] - expect));
                    }
                    r.checkVal(maxErr < 1e-4, "gen: Python numpy grid[r,c] ramp round-trips", maxErr);
                }
            }

            // numpy out-of-range clamp: raw numpy writes bypass the helper clamp,
            // so a cell set to 5.0 must be clamped to bipolar 1.0 at readback
            // (np.clip). When numpy is absent this is a no-op pass.
            {
                std::vector<float> out;
                std::string err;
                bool ok = ScriptEngine::instance().bakeTerrain(
                    "def generate():\n"
                    "    if grid is not None:\n"
                    "        grid[:] = 5.0\n"
                    "    else:\n"
                    "        for i in range(total):\n"
                    "            set(i, 5.0)\n",
                    /*wholeGrid*/true, { 3, 4 }, out, err);
                r.check(ok && out.size() == 12, "gen: Python numpy clamp runs");
                if (ok && out.size() == 12) {
                    float maxErr = 0.0f;
                    for (float v : out) maxErr = std::max(maxErr, std::abs(v - 1.0f));
                    r.checkVal(maxErr < 1e-5, "gen: Python numpy raw write clamps >1 at readback", maxErr);
                }
            }

            // Output clamps: a per-cell value of 5.0 must clamp to 1.0 -> bipolar 1.0.
            {
                std::vector<float> out;
                std::string err;
                bool ok = ScriptEngine::instance().bakeTerrain(
                    "5.0", /*wholeGrid*/false, { 3 }, out, err);
                r.check(ok, "gen: Python out-of-range value bakes");
                if (ok) {
                    float maxErr = 0.0f;
                    for (float v : out) maxErr = std::max(maxErr, std::abs(v - 1.0f));
                    r.checkVal(maxErr < 1e-5, "gen: Python per-cell clamps >1 to bipolar 1", maxErr);
                }
            }

            // A syntax error must fail cleanly with a message, not crash.
            {
                std::vector<float> out;
                std::string err;
                bool ok = ScriptEngine::instance().bakeTerrain(
                    "this is not python", /*wholeGrid*/false, { 4 }, out, err);
                r.check(!ok && !err.empty(), "gen: Python syntax error fails cleanly");
            }
        } else {
            // No Python: bakeTerrain must refuse without touching the C API.
            std::vector<float> out;
            std::string err;
            bool ok = ScriptEngine::instance().bakeTerrain(
                "0.5", /*wholeGrid*/false, { 4 }, out, err);
            r.check(!ok && !err.empty(),
                    "gen: bakeTerrain fails cleanly when Python unavailable");
        }

        // ---- WASM whole-grid generation (ss_generate + ss_grid_* imports) ---
        // A terrain WASM module is chosen as a compiled .wasm FILE (not source),
        // runs whole-grid only, and fills the host-owned grid via the ss_grid_*
        // imports. First: a missing/empty path must fail cleanly (the routing
        // through WasmRuntime + the load-failure message), independent of whether
        // wasm3 is built in.
        {
            Terrain twe;
            std::string weErr;
            bool weOk = twe.fillFromScriptWholeGrid(ScriptLang::Wasm, "", { 4 }, weErr);
            r.check(!weOk && !weErr.empty(),
                    "gen: WASM whole-grid with no file fails cleanly");
        }

        // GenLang/ScriptLang value-collision guard: the two enums only agree on
        // Builtin/Lua. GenLang::Python==2 collides with ScriptLang::Wasm==2 and
        // GenLang::Wasm==4, so a blind cross-cast would mis-route - this documents
        // why generate_dialog.cpp maps explicitly (genLangToScriptLang).
        r.check((int)GenLang::Python == 2 && (int)ScriptLang::Wasm == 2
                    && (int)GenLang::Wasm == 4 && (int)GenLang::Wasm != (int)GenLang::Python,
                "gen: GenLang/ScriptLang values diverge (no cross-cast)");

        // End-to-end: a real (hand-assembled) .wasm module exports ss_init (empty)
        // and ss_generate, importing ss_grid_set. ss_generate writes cells 0,1,2 =
        // 0.0, 0.5, 1.0; whole-grid maps v*2-1, so the {3} grid must come back as
        // bipolar -1, 0, +1. Only runs when wasm3 is compiled in.
        if (scriptLangAvailable(ScriptLang::Wasm)) {
            // Minimal WebAssembly binary (see soundshop_wasm.h grid ABI):
            //   (import "env" "ss_grid_set" (func (param i32 f32)))
            //   (memory (export "memory") 1)
            //   (func (export "ss_init"))
            //   (func (export "ss_generate")
            //     i32.const 0  f32.const 0.0  call ss_grid_set
            //     i32.const 1  f32.const 0.5  call ss_grid_set
            //     i32.const 2  f32.const 1.0  call ss_grid_set)
            static const unsigned char kWasm[] = {
                0x00,0x61,0x73,0x6D, 0x01,0x00,0x00,0x00,            // magic + version
                0x01,0x09, 0x02, 0x60,0x02,0x7F,0x7D,0x00, 0x60,0x00,0x00, // type
                0x02,0x13, 0x01, 0x03,0x65,0x6E,0x76,               // import: "env"
                    0x0B,0x73,0x73,0x5F,0x67,0x72,0x69,0x64,0x5F,0x73,0x65,0x74, // "ss_grid_set"
                    0x00,0x00,                                       // func, type 0
                0x03,0x03, 0x02, 0x01,0x01,                          // function: 2 funcs type 1
                0x05,0x03, 0x01, 0x00,0x01,                          // memory: min 1
                0x07,0x22, 0x03,                                     // export: 3 entries
                    0x06,0x6D,0x65,0x6D,0x6F,0x72,0x79, 0x02,0x00,   // "memory" mem 0
                    0x07,0x73,0x73,0x5F,0x69,0x6E,0x69,0x74, 0x00,0x01, // "ss_init" func 1
                    0x0B,0x73,0x73,0x5F,0x67,0x65,0x6E,0x65,0x72,0x61,0x74,0x65, 0x00,0x02, // "ss_generate" func 2
                0x0A,0x22, 0x02,                                     // code: 2 bodies
                    0x02, 0x00,0x0B,                                 // ss_init: empty
                    0x1D, 0x00,                                      // ss_generate: size 29, 0 locals
                        0x41,0x00, 0x43,0x00,0x00,0x00,0x00, 0x10,0x00, // i32.const 0, f32.const 0.0, call 0
                        0x41,0x01, 0x43,0x00,0x00,0x00,0x3F, 0x10,0x00, // i32.const 1, f32.const 0.5, call 0
                        0x41,0x02, 0x43,0x00,0x00,0x80,0x3F, 0x10,0x00, // i32.const 2, f32.const 1.0, call 0
                        0x0B                                          // end
            };
            juce::TemporaryFile tmp(".wasm");
            tmp.getFile().replaceWithData(kWasm, sizeof(kWasm));
            Terrain tww;
            std::string wwErr;
            bool wwOk = tww.fillFromScriptWholeGrid(
                ScriptLang::Wasm, tmp.getFile().getFullPathName().toStdString(),
                { 3 }, wwErr);
            r.check(wwOk, "gen: WASM whole-grid module runs (ss_generate)");
            if (wwOk) {
                const auto& d = tww.getData();
                float expect[3] = { -1.0f, 0.0f, 1.0f };
                float maxErr = (d.size() == 3) ? 0.0f : 1.0f;
                for (int i = 0; i < (int)d.size() && i < 3; ++i)
                    maxErr = std::max(maxErr, std::abs(d[(size_t)i] - expect[i]));
                r.checkVal(maxErr < 1e-5f,
                           "gen: WASM ss_grid_set fills bipolar -1/0/+1", maxErr);
            } else {
                // Surface the load/run error so a malformed fixture is debuggable.
                juce::Logger::writeToLog("WASM gen test error: " + juce::String(wwErr));
            }
        }

        // ---- waveform() cross-language factory-bank reads -------------------
        // The waveform("name", phase) helper is exposed in Builtin / Lua /
        // Python / GLSL and all four read the SAME WaveformBank::sampleAtPhase.
        // A terrain built with `unipolar(waveform(name, c0))` therefore
        // reproduces the bank's cycle exactly: unipolar maps the [-1,1] sample to
        // [0,1], and the terrain's own [0,1]->[-1,1] mapping recovers the raw
        // sample. Gated on the factory bank actually loading (waveforms.bin sits
        // next to the executable).
        {
            auto& bank = WaveformBank::get();
            bank.ensureLoaded();
            if (bank.numEntries() > 0) {
                const std::string name = bank.entry(0).name;
                const int id = bank.indexForName(name);
                r.check(id == 0, "waveform: indexForName resolves entry 0 by name");

                std::string upper = name;
                for (char& c : upper) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                r.check(bank.indexForName(upper) == 0,
                        "waveform: name lookup is case-insensitive");
                r.check(bank.sampleAtPhase(-1, 0.3f) == 0.0f
                     && bank.sampleAtPhase(bank.numEntries(), 0.3f) == 0.0f,
                        "waveform: out-of-range index reads 0");
                {
                    auto s0 = bank.samples(0);
                    r.checkVal((double)std::abs(bank.sampleAtPhase(0, 0.0f) - s0[0]) < 1e-6,
                               "waveform: phase 0 hits sample 0", 0.0);
                }

                const int DN = 16;
                std::vector<float> ref((size_t)DN);
                for (int i = 0; i < DN; ++i) {
                    float c0 = (DN > 1) ? (float)i / (float)(DN - 1) : 0.0f;
                    ref[(size_t)i] = bank.sampleAtPhase(id, c0);
                }
                auto cmpField = [&](const Terrain& t, const char* label) {
                    if (t.totalSize() != DN) { r.check(false, label); return; }
                    double maxErr = 0;
                    for (int i = 0; i < DN; ++i)
                        maxErr = std::max(maxErr, (double)std::abs(t.at(i) - ref[(size_t)i]));
                    r.checkVal(maxErr < 1e-4, label, maxErr);
                };
                // Names carry no quotes/backslashes, but escape defensively.
                std::string qn = "\"";
                for (char c : name) { if (c == '"' || c == '\\') qn.push_back('\\'); qn.push_back(c); }
                qn.push_back('"');

                // Builtin per-cell.
                {
                    Terrain t; std::string err;
                    bool ok = t.fillFromScript(ScriptLang::Builtin,
                        "unipolar(waveform(" + qn + ", c0))", { DN }, err);
                    r.check(ok, "waveform: Builtin waveform() runs");
                    if (ok) cmpField(t, "waveform: Builtin matches bank");
                }
                // Lua per-cell.
                if (scriptLangAvailable(ScriptLang::Lua)) {
                    Terrain t; std::string err;
                    bool ok = t.fillFromScript(ScriptLang::Lua,
                        "function loop() return waveform(" + qn + ", c0) * 0.5 + 0.5 end",
                        { DN }, err);
                    r.check(ok, "waveform: Lua waveform() runs");
                    if (ok) cmpField(t, "waveform: Lua matches bank");
                }
                // Python per-cell.
                if (ScriptEngine::pythonAvailable()) {
                    ScriptEngine::instance().init();
                    std::vector<float> out; std::string err;
                    bool ok = ScriptEngine::instance().bakeTerrain(
                        "waveform(" + qn + ", c0) * 0.5 + 0.5", false, { DN }, out, err);
                    r.check(ok, "waveform: Python waveform() runs");
                    if (ok && (int)out.size() == DN) {
                        double maxErr = 0;
                        for (int i = 0; i < DN; ++i)
                            maxErr = std::max(maxErr,
                                (double)std::abs(out[(size_t)i] - ref[(size_t)i]));
                        r.checkVal(maxErr < 1e-4, "waveform: Python matches bank", maxErr);
                    }
                }
                // GLSL per-cell. GLSL is integer-only (no strings on the GPU):
                // the user passes the stable entry index, which equals `id` here.
                // The whole bank is uploaded once to the cached binding-2 SSBO; no
                // source rewriting happens.
                if (glslComputeAvailable(nullptr)) {
                    Terrain t; std::string err;
                    bool ok = t.fillFromGlsl(
                        "return waveform(" + std::to_string(id) + ", c[0]) * 0.5 + 0.5;",
                        false, { DN }, err, 1);
                    r.check(ok, "waveform: GLSL waveform() runs");
                    if (ok) cmpField(t, "waveform: GLSL matches bank");

                    // Out-of-range id -> silence (0.5 -> bipolar 0).
                    Terrain tu; std::string eu;
                    bool oku = tu.fillFromGlsl(
                        "return waveform(999999999, c[0]) * 0.5 + 0.5;",
                        false, { DN }, eu, 1);
                    r.check(oku, "waveform: GLSL out-of-range id still compiles");
                    if (oku) {
                        double maxErr = 0;
                        for (int i = 0; i < DN; ++i)
                            maxErr = std::max(maxErr, (double)std::abs(tu.at(i)));
                        r.checkVal(maxErr < 1e-5, "waveform: GLSL out-of-range id reads silence", maxErr);
                    }
                }

                // waveforms[name] -> stable id, identical in Lua and Python, and
                // equal to indexForName(). Reading via the dict and via the name
                // string must reproduce the same bank cycle.
                if (scriptLangAvailable(ScriptLang::Lua)) {
                    Terrain t; std::string err;
                    bool ok = t.fillFromScript(ScriptLang::Lua,
                        "local w = waveforms[" + qn + "]\n"
                        "function loop() return waveform(w, c0) * 0.5 + 0.5 end",
                        { DN }, err);
                    r.check(ok, "waveform: Lua waveforms[name] runs");
                    if (ok) cmpField(t, "waveform: Lua waveforms[name] matches bank");

                    // Unknown name -> -1 -> silence.
                    Terrain tu; std::string eu;
                    bool oku = tu.fillFromScript(ScriptLang::Lua,
                        "local w = waveforms[\"__no_such_waveform__\"]\n"
                        "function loop() return (w == -1) and 0.5 or 0.0 end",
                        { DN }, eu);
                    r.check(oku && tu.totalSize() == DN
                                && std::abs(tu.at(0)) < 1e-5,
                            "waveform: Lua waveforms[unknown] == -1");
                }
                if (ScriptEngine::pythonAvailable()) {
                    ScriptEngine::instance().init();
                    std::vector<float> out; std::string err;
                    bool ok = ScriptEngine::instance().bakeTerrain(
                        "waveform(waveforms[" + qn + "], c0) * 0.5 + 0.5",
                        false, { DN }, out, err);
                    r.check(ok, "waveform: Python waveforms[name] runs");
                    if (ok && (int)out.size() == DN) {
                        double maxErr = 0;
                        for (int i = 0; i < DN; ++i)
                            maxErr = std::max(maxErr,
                                (double)std::abs(out[(size_t)i] - ref[(size_t)i]));
                        r.checkVal(maxErr < 1e-4, "waveform: Python waveforms[name] matches bank", maxErr);
                    }
                }
            } else {
                r.check(true, "waveform: factory bank unavailable (read tests skipped)");
            }
        }

        // ---- Event-driven MIDI input (PerBlock streaming scripts) ----------
        // A block-mode Lua signal script reads the block's MIDI-input events via
        // midiin()/midievent() and reacts to them, instead of only polling the
        // block-constant note/vel/gate. We feed a synthetic ScriptBlockCtx with a
        // hand-built event list and check the script saw each event correctly.
        if (scriptLangAvailable(ScriptLang::Lua)) {
            auto rt = makeScriptRuntime(ScriptLang::Lua, ScriptRole::Signal,
                                        ScriptRate::PerBlock);
            r.check(rt != nullptr, "midiin: Lua block-mode runtime created");
            if (rt) {
                // Program: count events, and stamp the output buffer so we can
                // read back what the script observed:
                //   out(0) = number of events
                //   out(1) = note number of the first note-on (/127)
                //   out(2) = velocity of that note-on
                //   out(3) = its sample offset (/n)
                //   out(4) = CC value of the first cc event
                //   out(5) = (bend+1)/2 of the first bend event
                std::string err;
                const char* prog =
                    "function loop()\n"
                    "  local cnt = midiin()\n"
                    "  out(0, cnt/127)\n"
                    "  for k=1,cnt do\n"
                    "    local kind, off, a, b = midievent(k)\n"
                    "    if kind=='on' and get1==nil then\n"
                    "      get1=1; out(1, a/127); out(2, b); out(3, off/n)\n"
                    "    elseif kind=='cc' and getc==nil then\n"
                    "      getc=1; out(4, b)\n"
                    "    elseif kind=='bend' and getb==nil then\n"
                    "      getb=1; out(5, (b+1)/2)\n"
                    "    end\n"
                    "  end\n"
                    "end\n";
                bool lok = rt->load(prog, err);
                r.check(lok, "midiin: program loads");

                std::vector<ScriptMidiEvent> ev;
                ev.push_back({ 10, 1, 64, 100.0f / 127.0f }); // note-on C, vel 100, off 10
                ev.push_back({ 20, 2, 7, 64.0f / 127.0f });   // CC7 ~0.5
                ev.push_back({ 30, 3, 0, 0.5f });             // bend +0.5
                ev.push_back({ 40, 0, 64, 0.0f });            // note-off

                const int N = 64;
                std::vector<float> outBuf((size_t)N, -1.0f);
                ScriptBlockCtx ctx;
                ctx.sampleRate = 44100.0;
                ctx.numSamples = N;
                ctx.out        = outBuf.data();
                ctx.midiIn     = &ev;
                ctx.midiInCount = (int)ev.size();
                rt->runBlock(ctx);
                r.check(rt->getError().empty(),
                        "midiin: no runtime error [" + rt->getError() + "]");

                auto near = [](float a, float b) { return std::abs(a - b) < 1e-3f; };
                r.checkVal(near(outBuf[0], 4.0f / 127.0f),
                           "midiin: midiin() count == 4", outBuf[0] * 127.0f);
                r.checkVal(near(outBuf[1], 64.0f / 127.0f),
                           "midiin: first note-on note == 64", outBuf[1] * 127.0f);
                r.checkVal(near(outBuf[2], 100.0f / 127.0f),
                           "midiin: first note-on velocity", outBuf[2]);
                r.checkVal(near(outBuf[3], 10.0f / N),
                           "midiin: first note-on offset == 10", outBuf[3] * N);
                r.checkVal(near(outBuf[4], 64.0f / 127.0f),
                           "midiin: cc value preserved", outBuf[4]);
                r.checkVal(near(outBuf[5], 0.75f),
                           "midiin: bend +0.5 -> 0.75", outBuf[5]);

                // Per-sample (no block ctx) -> midiin() returns 0 gracefully.
                auto rt2 = makeScriptRuntime(ScriptLang::Lua, ScriptRole::Signal,
                                             ScriptRate::PerSample);
                if (rt2) {
                    std::string err2;
                    bool l2 = rt2->load("function loop() return midiin()*0 end", err2);
                    ScriptVars sv;
                    float v = rt2->evalSignal(sv);
                    r.check(l2 && v == 0.0f,
                            "midiin: per-sample midiin() is 0 (no block ctx)");
                }
            }
        }

        // ---- Streaming pull-model (coroutine stream() scripts) -------------
        // A streaming script owns its own loop and PULLS input / PUSHES output,
        // suspending (Lua coroutine yield) at the block boundary and resuming in
        // the next block with its local state intact. We verify: (1) a 1-sample
        // delay carries a value ACROSS a block boundary (proving the coroutine's
        // locals persist across suspend/resume); (2) pollmidi() drains MIDI input
        // event-driven as the cursor advances; (3) pullblock()/outblock() process
        // a whole block at once and still suspend/resume correctly.
        if (scriptLangAvailable(ScriptLang::Lua)) {
            auto nearf = [](float a, float b) { return std::abs(a - b) < 1e-3f; };

            // (1) 1-sample delay across two blocks.
            {
                auto rt = makeScriptRuntime(ScriptLang::Lua, ScriptRole::Signal,
                                            ScriptRate::PerBlock);
                r.check(rt != nullptr, "stream: Lua block-mode runtime created");
                if (rt) {
                    std::string err;
                    const char* prog =
                        "function stream()\n"
                        "  local prev = 0\n"
                        "  while true do\n"
                        "    local x = pull()\n"   // next input sample (blocks at block end)
                        "    out(prev)\n"          // output the PREVIOUS sample
                        "    prev = x\n"
                        "  end\n"
                        "end\n";
                    r.check(rt->load(prog, err), "stream: delay program loads [" + err + "]");

                    const int N = 4;
                    std::vector<float> in1 = { 0.1f, 0.2f, 0.3f, 0.4f };
                    std::vector<float> in2 = { 0.5f, 0.6f, 0.7f, 0.8f };
                    std::vector<float> out1((size_t)N, -1.0f), out2((size_t)N, -1.0f);
                    std::vector<const float*> sigPtrs(1, nullptr);

                    ScriptBlockCtx ctx;
                    ctx.sampleRate = 44100.0; ctx.numSamples = N;
                    ctx.sig = &sigPtrs; ctx.sigCount = 1;

                    sigPtrs[0] = in1.data(); ctx.out = out1.data();
                    rt->runBlock(ctx);
                    sigPtrs[0] = in2.data(); ctx.out = out2.data();
                    rt->runBlock(ctx);
                    r.check(rt->getError().empty(),
                            "stream: no runtime error [" + rt->getError() + "]");

                    bool b1 = nearf(out1[0], 0.0f) && nearf(out1[1], 0.1f)
                            && nearf(out1[2], 0.2f) && nearf(out1[3], 0.3f);
                    r.check(b1, "stream: block1 = 1-sample delay of input");
                    // The key assertion: out2[0] is block1's LAST input, proving
                    // the coroutine local `prev` survived the block boundary.
                    r.checkVal(nearf(out2[0], 0.4f),
                               "stream: state persists across blocks (out2[0]==in1 tail)",
                               out2[0]);
                    bool b2 = nearf(out2[1], 0.5f) && nearf(out2[2], 0.6f)
                            && nearf(out2[3], 0.7f);
                    r.check(b2, "stream: block2 delayed samples");
                }
            }

            // (2) Event-driven pollmidi() inside a streaming loop.
            {
                auto rt = makeScriptRuntime(ScriptLang::Lua, ScriptRole::Signal,
                                            ScriptRate::PerBlock);
                if (rt) {
                    std::string err;
                    const char* prog =
                        "hits = 0\n"
                        "function stream()\n"
                        "  while true do\n"
                        "    pull()\n"                          // advance one sample
                        "    local k = pollmidi()\n"
                        "    while k do\n"
                        "      if k=='on' then hits = hits + 1 end\n"
                        "      k = pollmidi()\n"
                        "    end\n"
                        "    out(hits/127)\n"
                        "  end\n"
                        "end\n";
                    r.check(rt->load(prog, err), "stream: pollmidi program loads [" + err + "]");

                    const int N = 4;
                    std::vector<ScriptMidiEvent> ev;
                    ev.push_back({ 1, 1, 60, 1.0f });   // note-on at sample 1
                    ev.push_back({ 3, 1, 64, 1.0f });   // note-on at sample 3
                    std::vector<float> out((size_t)N, -1.0f);
                    ScriptBlockCtx ctx;
                    ctx.sampleRate = 44100.0; ctx.numSamples = N;
                    ctx.out = out.data();
                    ctx.midiIn = &ev; ctx.midiInCount = (int)ev.size();
                    rt->runBlock(ctx);
                    r.check(rt->getError().empty(),
                            "stream: pollmidi no runtime error [" + rt->getError() + "]");
                    // Cursor reaches event 1 at sample 1, event 2 at sample 3.
                    bool ok = nearf(out[0], 0.0f) && nearf(out[1], 1.0f / 127.0f)
                            && nearf(out[2], 1.0f / 127.0f) && nearf(out[3], 2.0f / 127.0f);
                    r.check(ok, "stream: pollmidi drains events at the cursor");
                }
            }

            // (3) Whole-block pullblock()/outblock() (block-rate streaming).
            {
                auto rt = makeScriptRuntime(ScriptLang::Lua, ScriptRole::Signal,
                                            ScriptRate::PerBlock);
                if (rt) {
                    std::string err;
                    const char* prog =
                        "function stream()\n"
                        "  while true do\n"
                        "    local t = pullblock(1)\n"      // single-pin convenience form
                        "    for i=1,#t do t[i] = t[i]*0.5 end\n"
                        "    outblock(t)\n"
                        "  end\n"
                        "end\n";
                    r.check(rt->load(prog, err), "stream: pullblock program loads [" + err + "]");

                    const int N = 4;
                    std::vector<float> in1 = { 0.2f, 0.4f, 0.6f, 0.8f };
                    std::vector<float> in2 = { 1.0f, 1.0f, 1.0f, 1.0f };
                    std::vector<float> out1((size_t)N, -1.0f), out2((size_t)N, -1.0f);
                    std::vector<const float*> sigPtrs(1, nullptr);
                    ScriptBlockCtx ctx;
                    ctx.sampleRate = 44100.0; ctx.numSamples = N;
                    ctx.sig = &sigPtrs; ctx.sigCount = 1;

                    sigPtrs[0] = in1.data(); ctx.out = out1.data();
                    rt->runBlock(ctx);
                    sigPtrs[0] = in2.data(); ctx.out = out2.data();
                    rt->runBlock(ctx);
                    r.check(rt->getError().empty(),
                            "stream: pullblock no runtime error [" + rt->getError() + "]");
                    bool b1 = nearf(out1[0], 0.1f) && nearf(out1[1], 0.2f)
                            && nearf(out1[2], 0.3f) && nearf(out1[3], 0.4f);
                    bool b2 = nearf(out2[0], 0.5f) && nearf(out2[3], 0.5f);
                    r.check(b1, "stream: pullblock/outblock halves block1");
                    r.check(b2, "stream: pullblock/outblock resumes for block2");
                }
            }

            // (4) Multi-output: a streaming script drives two output pins
            // independently via out(1,v)/out(2,v). The host hands it two distinct
            // buffers through ScriptBlockCtx::outs and we verify they differ.
            {
                auto rt = makeScriptRuntime(ScriptLang::Lua, ScriptRole::Signal,
                                            ScriptRate::PerBlock);
                if (rt) {
                    std::string err;
                    const char* prog =
                        "function stream()\n"
                        "  while true do\n"
                        "    local x = pull()\n"
                        "    out(1, x)\n"            // pin o1 = input
                        "    out(2, 1 - x)\n"        // pin o2 = inverted input
                        "  end\n"
                        "end\n";
                    r.check(rt->load(prog, err), "stream: multi-out program loads [" + err + "]");

                    const int N = 4;
                    std::vector<float> in = { 0.1f, 0.3f, 0.6f, 0.9f };
                    std::vector<float> o1((size_t)N, -1.0f), o2((size_t)N, -1.0f);
                    std::vector<const float*> sigPtrs(1, in.data());
                    std::vector<float*> outPtrs = { o1.data(), o2.data() };
                    ScriptBlockCtx ctx;
                    ctx.sampleRate = 44100.0; ctx.numSamples = N;
                    ctx.sig = &sigPtrs; ctx.sigCount = 1;
                    ctx.out = o1.data();            // out(v)/out(1,..) -> pin 0
                    ctx.outs = &outPtrs; ctx.outCount = 2;
                    rt->runBlock(ctx);
                    r.check(rt->getError().empty(),
                            "stream: multi-out no runtime error [" + rt->getError() + "]");
                    bool ok = nearf(o1[0], 0.1f) && nearf(o1[3], 0.9f)
                            && nearf(o2[0], 0.9f) && nearf(o2[3], 0.1f);
                    r.check(ok, "stream: out(1,..)/out(2,..) drive separate buffers");
                }
            }

            // (5) Structured pullblock(): no-arg form returns params (list-of-lists,
            // one array per input pin) plus a unified MIDI event list. We feed two
            // param inputs and one MIDI event; the script copies input pin 2 to
            // output pin 1, and writes the first event's (note/127, idx/16) to the
            // whole of output pin 2 — so we can assert both the param and event paths
            // through the output buffers.
            {
                auto rt = makeScriptRuntime(ScriptLang::Lua, ScriptRole::Signal,
                                            ScriptRate::PerBlock);
                if (rt) {
                    std::string err;
                    const char* prog =
                        "function stream()\n"
                        "  while true do\n"
                        "    local params, events = pullblock()\n"
                        "    outblock(1, params[2])\n"      // pin o1 = input pin 2
                        "    local note, idx = 0, 0\n"
                        "    if #events > 0 then\n"
                        "      note = events[1].a\n"
                        "      idx  = events[1].idx\n"
                        "    end\n"
                        "    local e = {}\n"
                        "    for i = 1, 4 do e[i] = (i <= 2) and note/127 or idx/16 end\n"
                        "    outblock(2, e)\n"
                        "  end\n"
                        "end\n";
                    r.check(rt->load(prog, err), "stream: structured pullblock loads [" + err + "]");

                    const int N = 4;
                    std::vector<float> in1 = { 0.0f, 0.0f, 0.0f, 0.0f };
                    std::vector<float> in2 = { 0.2f, 0.4f, 0.6f, 0.8f };
                    std::vector<const float*> sigPtrs = { in1.data(), in2.data() };
                    std::vector<float> o1((size_t)N, -1.0f), o2((size_t)N, -1.0f);
                    std::vector<float*> outPtrs = { o1.data(), o2.data() };
                    std::vector<ScriptMidiEvent> ev;
                    ev.push_back({ 0, 1, 72, 1.0f });   // note-on, note 72, at sample 0
                    ScriptBlockCtx ctx;
                    ctx.sampleRate = 44100.0; ctx.numSamples = N;
                    ctx.sig = &sigPtrs; ctx.sigCount = 2;
                    ctx.out = o1.data();
                    ctx.outs = &outPtrs; ctx.outCount = 2;
                    ctx.midiIn = &ev; ctx.midiInCount = (int)ev.size();
                    rt->runBlock(ctx);
                    r.check(rt->getError().empty(),
                            "stream: structured pullblock no error [" + rt->getError() + "]");
                    bool sigOk = nearf(o1[0], 0.2f) && nearf(o1[1], 0.4f)
                               && nearf(o1[2], 0.6f) && nearf(o1[3], 0.8f);
                    r.check(sigOk, "stream: params[2] copied to output (list-of-lists)");
                    bool evOk = nearf(o2[0], 72.0f / 127.0f)   // event note number
                              && nearf(o2[3], 1.0f / 16.0f);   // 1-based MIDI-input idx
                    r.check(evOk, "stream: pullblock events list carries note + idx");
                }
            }

            // (6) pollmidi() returns idx LAST so the kind-first idiom still works,
            // and the idx reflects the (1-based) MIDI-input pin.
            {
                auto rt = makeScriptRuntime(ScriptLang::Lua, ScriptRole::Signal,
                                            ScriptRate::PerBlock);
                if (rt) {
                    std::string err;
                    const char* prog =
                        "lastidx = 0\n"
                        "function stream()\n"
                        "  while true do\n"
                        "    pull()\n"
                        "    local k, off, a, b, idx = pollmidi()\n"
                        "    while k do\n"
                        "      lastidx = idx\n"
                        "      out(a / 127)\n"
                        "      k, off, a, b, idx = pollmidi()\n"
                        "    end\n"
                        "  end\n"
                        "end\n";
                    r.check(rt->load(prog, err), "stream: pollmidi idx program loads [" + err + "]");
                    const int N = 4;
                    std::vector<ScriptMidiEvent> ev;
                    ev.push_back({ 2, 1, 64, 1.0f });   // note-on at sample 2
                    std::vector<float> out((size_t)N, -1.0f);
                    ScriptBlockCtx ctx;
                    ctx.sampleRate = 44100.0; ctx.numSamples = N;
                    ctx.out = out.data();
                    ctx.midiIn = &ev; ctx.midiInCount = (int)ev.size();
                    rt->runBlock(ctx);
                    r.check(rt->getError().empty(),
                            "stream: pollmidi idx no error [" + rt->getError() + "]");
                    // out[2] gets 64/127 once the cursor reaches the event.
                    r.check(nearf(out[2], 64.0f / 127.0f),
                            "stream: pollmidi kind-first idiom intact (a==64 at sample 2)");
                }
            }
        }

        // ---- Multi-MIDI-input event tagging (buildScriptMidiIn) -------------
        // A script node with >1 MIDI input pin has its incoming cables stamped by
        // the graph so each event's channel nibble = (input-pin index + 1). The
        // host helper buildScriptMidiIn must recover ScriptMidiEvent::inIndex from
        // that channel. With a single input pin the channel is meaningless and
        // inIndex stays 0. This exercises the routing contract without standing up
        // a full audio graph (the stamping itself is a pure channel rewrite).
        {
            const int N = 16;
            juce::MidiBuffer buf;
            buf.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0); // pin 0
            buf.addEvent(juce::MidiMessage::noteOn(2, 62, (juce::uint8)100), 2); // pin 1
            buf.addEvent(juce::MidiMessage::noteOn(3, 64, (juce::uint8)100), 4); // pin 2
            buf.addEvent(juce::MidiMessage::controllerEvent(5, 7, 64),       6); // pin 4

            // Multi-input: inIndex = channel - 1, clamped to 0..count-1.
            std::vector<ScriptMidiEvent> ev;
            buildScriptMidiIn(buf, N, ev, /*midiInputCount*/ 5);
            bool countOk = (ev.size() == 4);
            r.check(countOk, "multimidi: all 4 events converted");
            if (countOk) {
                r.checkVal(ev[0].inIndex == 0, "multimidi: ch1 -> input pin 0", ev[0].inIndex);
                r.checkVal(ev[1].inIndex == 1, "multimidi: ch2 -> input pin 1", ev[1].inIndex);
                r.checkVal(ev[2].inIndex == 2, "multimidi: ch3 -> input pin 2", ev[2].inIndex);
                r.checkVal(ev[3].inIndex == 4, "multimidi: ch5 -> input pin 4", ev[3].inIndex);
            }

            // A channel beyond the declared count clamps to the last pin.
            std::vector<ScriptMidiEvent> ev2;
            buildScriptMidiIn(buf, N, ev2, /*midiInputCount*/ 2);
            r.check(ev2.size() == 4 && ev2[0].inIndex == 0 && ev2[1].inIndex == 1
                    && ev2[2].inIndex == 1 && ev2[3].inIndex == 1,
                    "multimidi: channels past count clamp to last input pin");

            // Single input: channel carries no routing meaning -> inIndex all 0.
            std::vector<ScriptMidiEvent> ev1;
            buildScriptMidiIn(buf, N, ev1, /*midiInputCount*/ 1);
            bool allZero = true;
            for (auto& e : ev1) if (e.inIndex != 0) allZero = false;
            r.check(ev1.size() == 4 && allZero,
                    "multimidi: single input -> inIndex always 0 (channel ignored)");
        }

        // ---- ContentStore: round-trip, determinism, dedup ------------------
        // The content-addressed side-store holds baked grids out of undo
        // snapshots (hash travels, bytes don't) and dedups identical grids.
        {
            ContentStore cs;
            std::vector<float> grid(48);
            for (int i = 0; i < 48; ++i) grid[(size_t)i] = std::sin(i * 0.3f);
            std::vector<int> shape = { 4, 12 };

            std::string h1 = cs.putFloatGrid(grid, shape);
            r.check(h1.size() == 32, "store: hash is 32 hex chars");
            r.check(cs.has(h1), "store: hash present after put");
            r.check(cs.size() == 1, "store: one entry after first put");

            std::vector<float> back; std::vector<int> backShape;
            bool got = cs.getFloatGrid(h1, back, backShape);
            r.check(got, "store: getFloatGrid resolves hash");
            r.check(backShape == shape, "store: shape round-trips");
            float maxErr = 0.0f;
            for (size_t i = 0; i < back.size() && i < grid.size(); ++i)
                maxErr = std::max(maxErr, std::abs(back[i] - grid[i]));
            r.checkVal(back.size() == grid.size() && maxErr == 0.0f,
                       "store: floats bit-exact through shuffle+DEFLATE", maxErr);

            // Determinism + dedup: same grid -> same hash, stored once.
            std::string h2 = cs.putFloatGrid(grid, shape);
            r.check(h2 == h1, "store: identical grid -> identical hash");
            r.check(cs.size() == 1, "store: dedup - identical grid stored once");

            // Different data -> different hash.
            std::vector<float> grid2 = grid; grid2[0] += 1.0f;
            std::string h3 = cs.putFloatGrid(grid2, shape);
            r.check(h3 != h1, "store: changed cell -> different hash");

            // Same data, different shape -> different hash (shape is hashed).
            std::string h4 = cs.putFloatGrid(grid, { 12, 4 });
            r.check(h4 != h1, "store: same data different shape -> different hash");

            // Absent hash resolves false.
            std::vector<float> none; std::vector<int> noneShape;
            r.check(!cs.getFloatGrid("00000000000000000000000000000000", none, noneShape),
                    "store: absent hash resolves false");
        }

        // ---- makeNpy/parseNpy codec across ranks ---------------------------
        {
            std::vector<std::vector<int>> shapes = {
                { 5 }, { 1 }, { 2, 3 }, { 4, 5, 6 }, { 2, 2, 2, 2 }
            };
            bool allOk = true;
            for (auto& shp : shapes) {
                long long n = 1; for (int d : shp) n *= d;
                std::vector<float> data((size_t)n);
                for (long long i = 0; i < n; ++i)
                    data[(size_t)i] = (float)(i * 0.5 - 3.0);
                auto npy = ContentStore::makeNpy(data, shp);
                // header length (preamble 10 + hlen) is a multiple of 64.
                size_t hlen = (size_t)(npy[8] | (npy[9] << 8));
                allOk = allOk && (((10 + hlen) % 64) == 0);
                std::vector<float> od; std::vector<int> os;
                bool ok = ContentStore::parseNpy(npy.data(), npy.size(), od, os);
                allOk = allOk && ok && os == shp && od.size() == data.size();
                for (size_t i = 0; ok && i < od.size(); ++i)
                    allOk = allOk && (od[i] == data[i]);
            }
            r.check(allOk, "store: makeNpy/parseNpy round-trips 1D..4D bit-exact");
        }

        // ---- .npz container: ZIP of STORED .npy members --------------------
        // makeNpz must produce a valid ZIP (local-file + EOCD signatures) whose
        // STORED member decodes back to the exact grid (this is what np.savez
        // writes, so NumPy's np.load can read it).
        {
            std::vector<float> data = { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 0.25f };
            std::vector<int> shape = { 2, 3 };
            auto npy = ContentStore::makeNpy(data, shape);
            auto npz = ContentStore::makeNpz({ { "terrain.npy", npy } });

            bool sigOk = npz.size() > 4 && npz[0] == 0x50 && npz[1] == 0x4b
                         && npz[2] == 0x03 && npz[3] == 0x04;
            r.check(sigOk, "store: .npz starts with ZIP local-file signature");

            bool eocd = false;
            for (size_t i = 0; i + 4 <= npz.size(); ++i)
                if (npz[i] == 0x50 && npz[i+1] == 0x4b
                    && npz[i+2] == 0x05 && npz[i+3] == 0x06) { eocd = true; break; }
            r.check(eocd, "store: .npz has end-of-central-directory record");

            // STORED member begins at 30 + nameLen (fixed 30-byte local header,
            // zero extra field). Extract it and re-parse the contained .npy.
            uint16_t nameLen = (uint16_t)(npz[26] | (npz[27] << 8));
            size_t dataOff = 30u + nameLen;
            std::vector<float> od; std::vector<int> os;
            bool parsed = dataOff + npy.size() <= npz.size()
                && ContentStore::parseNpy(npz.data() + dataOff, npy.size(), od, os);
            r.check(parsed && os == shape && od == data,
                    "store: .npz member round-trips back to the exact grid");
        }

        // ---- undo snapshot excludes blobs; hash travels in the script ------
        // serializeForUndo must NOT emit the blob bytes (the whole point of the
        // side-store), but the node's '#hash' reference must survive so undo/redo
        // resolves the grid from the in-memory store. A real save DOES emit it.
        {
            NodeGraph g;
            GenerateTerrainParams gp;
            gp.lang = (int)GenLang::Builtin;
            gp.dims = { 8, 8 };
            gp.source = "0.5";
            gp.data.resize(64);
            for (int i = 0; i < 64; ++i)
                gp.data[(size_t)i] = (float)i / 63.0f * 2.0f - 1.0f;

            std::string script = makeGenerateTerrainScript(gp, &g.contentStore);
            r.check(script.find("|#") != std::string::npos,
                    "store: generate script carries '#hash' (not inline blob)");
            r.check(g.contentStore.size() == 1, "store: bake populated the graph store");

            auto& n = g.addNode("gen", NodeType::TerrainSynth, {}, {});
            n.script = script;

            std::string snap = ProjectFile::serializeForUndo(g);
            r.check(snap.find("[Blob]") == std::string::npos,
                    "store: undo snapshot omits [Blob] sections");
            size_t hp = script.rfind("|#");
            std::string hash = script.substr(hp + 2);
            r.check(snap.find(hash) != std::string::npos,
                    "store: undo snapshot keeps the '#hash' reference");

            // Real save round-trips the blob into a fresh graph's store.
            std::ostringstream oss;
            ProjectFile::writeProject(oss, g, nullptr, /*includeView*/false,
                                      /*includeBlobs*/true);
            std::string saved = oss.str();
            r.check(saved.find("[Blob]") != std::string::npos,
                    "store: real save emits [Blob] section");

            NodeGraph g2;
            std::istringstream iss(saved);
            ProjectFile::readProject(iss, g2, nullptr);
            r.check(g2.contentStore.has(hash),
                    "store: blob round-trips save/load into a fresh store");
            std::vector<float> rt; std::vector<int> rtShape;
            bool rok = g2.contentStore.getFloatGrid(hash, rt, rtShape);
            float maxErr = 0.0f;
            for (size_t i = 0; rok && i < rt.size() && i < gp.data.size(); ++i)
                maxErr = std::max(maxErr, std::abs(rt[i] - gp.data[i]));
            r.checkVal(rok && rt.size() == gp.data.size() && maxErr == 0.0f,
                       "store: loaded blob bit-exact with baked grid", maxErr);
        }

        // ---- automation record/read cascade: serialization round-trip -------
        // Per-node armMode + ignoreAutomation and per-param armMode +
        // bypassAutomation must survive save/load; the global session mode must
        // NOT persist (a reload always starts disarmed). Also spot-check the
        // resolver's gate + cascade precedence.
        {
            NodeGraph g;
            auto& n = g.addNode("synth", NodeType::Instrument, {}, {});
            n.armMode = AutoArmMode::Write;
            n.ignoreAutomation = true;
            n.params.push_back({ "Cutoff", 0.5f, 0.0f, 1.0f });
            n.params.push_back({ "Res",    0.2f, 0.0f, 1.0f });
            n.params[0].armMode = AutoArmMode::Latch;
            n.params[0].bypassAutomation = true;
            // params[1] left at defaults (Inherit / not bypassed)
            g.autoArmGlobal = AutoArmMode::Touch; // session-only, must be dropped

            std::ostringstream oss;
            ProjectFile::writeProject(oss, g, nullptr, /*includeView*/false,
                                      /*includeBlobs*/false);
            NodeGraph g2;
            std::istringstream iss(oss.str());
            ProjectFile::readProject(iss, g2, nullptr);

            r.check(g2.nodes.size() == 1, "autocascade: node round-trips");
            auto& n2 = g2.nodes[0];
            r.check(n2.armMode == AutoArmMode::Write, "autocascade: node armMode round-trips");
            r.check(n2.ignoreAutomation, "autocascade: node ignoreAutomation round-trips");
            r.check(n2.params.size() == 2, "autocascade: params round-trip");
            r.check(n2.params[0].armMode == AutoArmMode::Latch,
                    "autocascade: param armMode round-trips");
            r.check(n2.params[0].bypassAutomation,
                    "autocascade: param bypassAutomation round-trips");
            r.check(n2.params[1].armMode == AutoArmMode::Inherit,
                    "autocascade: default param armMode stays Inherit");
            r.check(!n2.params[1].bypassAutomation,
                    "autocascade: default param not bypassed");
            r.check(g2.autoArmGlobal == AutoArmMode::Off,
                    "autocascade: global mode is session-only (reload disarms)");

            // Resolver: global Off is a hard gate regardless of overrides.
            r.check(resolveArmMode(AutoArmMode::Off, n2, n2.params[0]) == AutoArmMode::Off,
                    "autocascade: global Off gates all recording");
            // Globally armed: param override wins over node override.
            r.check(resolveArmMode(AutoArmMode::Touch, n2, n2.params[0]) == AutoArmMode::Latch,
                    "autocascade: param override beats node override");
            // Globally armed, param inherits -> node override applies.
            r.check(resolveArmMode(AutoArmMode::Touch, n2, n2.params[1]) == AutoArmMode::Write,
                    "autocascade: node override applies when param inherits");
            // Read axis: node ignore suppresses all lanes; else per-param bypass.
            r.check(!automationReadEnabled(n2, n2.params[1]),
                    "autocascade: node ignore suppresses reads");
            Node clean; Param cp; cp.bypassAutomation = false;
            r.check(automationReadEnabled(clean, cp),
                    "autocascade: default node/param reads its lane");
            cp.bypassAutomation = true;
            r.check(!automationReadEnabled(clean, cp),
                    "autocascade: per-param bypass suppresses read");
        }

        // ---- time-gated effect regions: save/load AND undo round-trip --------
        // EffectRegion (the colored "layer" bars above the notes in the piano
        // roll) was never emitted by writeProject. Because serializeForUndo
        // shares that same writer, the omission had two compounding effects:
        // a region silently vanished on save->reload, AND the very next undo
        // step wiped every region in the project. Both paths are asserted here
        // so a future writer refactor can't quietly drop them again.
        // Measured with the serializer removed: regionsAfterSave = 0 and
        // regionsAfterUndo = 0 (vs 2 and 2 fixed) - i.e. total loss, not a
        // partial-fidelity bug.
        {
            NodeGraph g;
            int outPin = 0, inPin = 0;
            {
                auto& src = g.addNode("MIDI Track", NodeType::MidiTimeline, {},
                                      { Pin{0, "MIDI", PinKind::Midi, false} });
                outPin = src.pinsOut[0].id;
            }
            {
                auto& dst = g.addNode("Arpeggiator", NodeType::Effect,
                                      { Pin{0, "MIDI", PinKind::Midi, true} }, {});
                inPin = dst.pinsIn[0].id;
            }
            g.addLink(outPin, inPin);
            const int linkId = g.links.empty() ? -1 : g.links[0].id;
            const int groupId = g.addEffectGroup("Chorus section").id;

            // Two regions on the source track: one gating a bare link, one
            // gating a whole group. Non-default beats/colour so a "wrote the
            // struct default back" bug can't pass either.
            EffectRegion byLink;
            byLink.linkId = linkId; byLink.startBeat = 8.5f; byLink.endBeat = 12.25f;
            byLink.color = 0xFF3CB44B;
            EffectRegion byGroup;
            byGroup.groupId = groupId; byGroup.startBeat = 16.0f; byGroup.endBeat = 24.0f;
            byGroup.color = 0xFF911EB4;
            g.nodes[0].effectRegions = { byLink, byGroup };

            auto checkRegions = [&](NodeGraph& gg, const char* what) {
                const size_t n = gg.nodes.empty() ? 0 : gg.nodes[0].effectRegions.size();
                r.checkVal(n == 2, juce::String("fxregion: both regions survive ") + what,
                           (double)n);
                if (n != 2) return;
                auto& a = gg.nodes[0].effectRegions[0];
                auto& b = gg.nodes[0].effectRegions[1];
                r.check(a.linkId == linkId && a.groupId == -1,
                        juce::String("fxregion: per-link region keeps its link id after ") + what);
                r.check(std::abs(a.startBeat - 8.5f) < 1e-4f
                        && std::abs(a.endBeat - 12.25f) < 1e-4f,
                        juce::String("fxregion: per-link region keeps its beat range after ") + what);
                r.check(a.color == 0xFF3CB44Bu,
                        juce::String("fxregion: per-link region keeps its colour after ") + what);
                r.check(b.groupId == groupId && b.linkId == -1,
                        juce::String("fxregion: group region keeps its group id after ") + what);
                r.check(std::abs(b.startBeat - 16.0f) < 1e-4f
                        && std::abs(b.endBeat - 24.0f) < 1e-4f,
                        juce::String("fxregion: group region keeps its beat range after ") + what);
                r.check(b.color == 0xFF911EB4u,
                        juce::String("fxregion: group region keeps its colour after ") + what);
            };

            // Save -> load.
            {
                std::ostringstream oss;
                ProjectFile::writeProject(oss, g, nullptr, /*includeView*/false,
                                          /*includeBlobs*/false);
                NodeGraph g2;
                std::istringstream iss(oss.str());
                ProjectFile::readProject(iss, g2, nullptr);
                checkRegions(g2, "a save/load round-trip");
                r.check(g2.effectGroups.size() == 1
                        && g2.effectGroups[0].id == groupId,
                        "fxregion: the referenced effect group round-trips alongside it");
            }

            // Undo snapshot -> restore (the path that used to erase them).
            {
                std::string snap = ProjectFile::serializeForUndo(g);
                NodeGraph g3;
                std::istringstream iss(snap);
                ProjectFile::readProject(iss, g3, nullptr);
                checkRegions(g3, "an undo snapshot restore");
            }

            // ---- the gate itself reads LOCAL beats, not transport beats -----
            // Region beats are stored local to their node, exactly like clips
            // and notes, so that sliding a track's start position carries its
            // layers with it. TimeGateProcessor used to compare regions against
            // the raw transport beat, so the layers moved *visually* with the
            // track but fired at the old absolute beats *audibly*. Pushing the
            // node four bars out and probing on both sides of the shift pins
            // that down. Measured with `- beatOffset` removed from the gate:
            // the "unshifted" probe reads 1.0 instead of 0.0 and the "slid
            // along" probe reads 0.0 instead of 1.0 - i.e. the gate opens over
            // exactly the wrong stretch of music, which is what the bug
            // sounded like. (The two shut-side probes read 0.0 either way, so
            // they are regression guards, not discriminators - the inverting
            // pair is what actually catches this.)
            {
                Transport t;
                t.bpm = 120.0; t.sampleRate = 44100.0;
                g.nodes[0].effectRegions = { byLink };          // local beats 8.5 .. 12.25
                g.nodes[0].absoluteBeatOffset = 16.0f;          // track slid 4 bars right

                TimeGateProcessor gate(linkId, g.nodes[0], g, t);
                gate.prepareToPlay(t.sampleRate, 64);

                // Probe the gate at an absolute beat by running a block of DC
                // through it: the surviving amplitude *is* the wet amount.
                auto wetAt = [&](double absBeat) {
                    t.positionSamples = (int64_t)t.beatsToSamples(absBeat);
                    juce::AudioBuffer<float> buf(1, 64);
                    for (int i = 0; i < 64; ++i) buf.setSample(0, i, 1.0f);
                    juce::MidiBuffer midi;
                    gate.processBlock(buf, midi);
                    return buf.getSample(0, 0);
                };

                // Absolute 10.0 == local -6.0: nowhere near the region, but it
                // IS inside the region's raw beat range - so this is the probe
                // that fires if the gate forgets the offset.
                r.checkVal(wetAt(10.0) < 1e-4f,
                           "fxgate: a shifted track's layer is shut at its pre-slide beats",
                           (double)wetAt(10.0));
                // Absolute 22.0 == local 6.0: outside 8.5..12.25, so shut.
                r.checkVal(wetAt(22.0) < 1e-4f,
                           "fxgate: a shifted track's layer is shut ahead of its start beat",
                           (double)wetAt(22.0));
                // Absolute 26.0 == local 10.0: mid-region, well past the 50 ms
                // crossfade (0.1 beat at 120 bpm), so fully open.
                r.checkVal(wetAt(26.0) > 0.999f,
                           "fxgate: a shifted track's layer opens at its slid-along beat",
                           (double)wetAt(26.0));
                // And it still shuts again past the region's local end.
                r.checkVal(wetAt(30.0) < 1e-4f,
                           "fxgate: the layer shuts again past its end beat",
                           (double)wetAt(30.0));

                g.nodes[0].absoluteBeatOffset = 0.0f;
            }
        }

        // ---- insert/delete time ripples effect layers ------------------------
        // Layers share the clip/note local beat space, so the arrangement-level
        // "insert N beats here" / "delete this range" edits have to move them
        // too. They originally didn't, which silently desynced every layer in
        // the project from the music it was gating the moment a bar was added.
        // Measured with both ripple passes disabled: 3 of these fail outright
        // (straddle-stretch, post-insert slide, and the delete leaving 4 layers
        // instead of 3) and the 3 delete-side position checks never even run,
        // since they are guarded on that size.
        {
            NodeGraph g;
            g.addNode("MIDI Track", NodeType::MidiTimeline, {},
                      { Pin{0, "MIDI", PinKind::Midi, false} });
            auto mk = [](float s, float e) {
                EffectRegion r2; r2.linkId = 1; r2.startBeat = s; r2.endBeat = e; return r2;
            };

            // Insert 4 beats at beat 8: a layer entirely after the cut slides,
            // one straddling it stretches, one entirely before is untouched.
            g.nodes[0].effectRegions = { mk(0.0f, 4.0f), mk(6.0f, 10.0f), mk(12.0f, 16.0f) };
            g.insertTime(8.0f, 4.0f);
            auto& ins = g.nodes[0].effectRegions;
            r.check(ins.size() == 3 && std::abs(ins[0].startBeat - 0.0f) < 1e-4f
                    && std::abs(ins[0].endBeat - 4.0f) < 1e-4f,
                    "fxripple: a layer entirely before an insert is left alone");
            r.check(ins.size() == 3 && std::abs(ins[1].startBeat - 6.0f) < 1e-4f
                    && std::abs(ins[1].endBeat - 14.0f) < 1e-4f,
                    "fxripple: a layer straddling an insert stretches by the inserted length");
            r.check(ins.size() == 3 && std::abs(ins[2].startBeat - 16.0f) < 1e-4f
                    && std::abs(ins[2].endBeat - 20.0f) < 1e-4f,
                    "fxripple: a layer after an insert slides by the inserted length");

            // Delete beats 8..12: a layer wholly inside the deleted range is
            // dropped, one overlapping is clamped to the cut point, one after
            // pulls back by the deleted length.
            g.nodes[0].effectRegions = { mk(0.0f, 4.0f), mk(9.0f, 11.0f),
                                         mk(10.0f, 16.0f), mk(20.0f, 24.0f) };
            g.deleteTime(8.0f, 12.0f);
            auto& del = g.nodes[0].effectRegions;
            r.checkVal(del.size() == 3,
                       "fxripple: a layer wholly inside a deleted range is removed",
                       (double)del.size());
            if (del.size() == 3) {
                r.check(std::abs(del[0].startBeat - 0.0f) < 1e-4f
                        && std::abs(del[0].endBeat - 4.0f) < 1e-4f,
                        "fxripple: a layer entirely before a delete is left alone");
                r.check(std::abs(del[1].startBeat - 8.0f) < 1e-4f
                        && std::abs(del[1].endBeat - 12.0f) < 1e-4f,
                        "fxripple: a layer overlapping a delete is clamped to the cut point");
                r.check(std::abs(del[2].startBeat - 16.0f) < 1e-4f
                        && std::abs(del[2].endBeat - 20.0f) < 1e-4f,
                        "fxripple: a layer after a delete pulls back by the deleted length");
            }
        }

        // ---- directional magnetic snap for layer edges -----------------------
        // Layer edges are pixel-precise; a grid marker only grabs an edge on the
        // side the drag is LEAVING it from, so you can slide right up to a beat
        // and stop just short of it, but land exactly on it once you cross.
        // Measured by reinstating each plausible wrong implementation and
        // rebuilding: the old hard quantiser (round-to-nearest, ignoring dir)
        // fails 7 of these, and inverting the direction sense (floor/ceil
        // swapped) fails 9. The dir==0 and snap-off checks survive both, since
        // those are guard clauses rather than marker maths.
        {
            using SoundShop::magneticSnapBeat;
            const float grid = 1.0f, pull = 0.1f;
            auto near_ = [](float a, float b) { return std::abs(a - b) < 1e-4f; };

            // Moving right, still short of beat 4: free, so "just before the
            // downbeat" is reachable.
            r.check(near_(magneticSnapBeat(3.94f, +1, grid, pull), 3.94f),
                    "fxsnap: approaching a marker from the left leaves the edge free");
            // Moving right, just past beat 4: the marker holds it.
            r.check(near_(magneticSnapBeat(4.06f, +1, grid, pull), 4.0f),
                    "fxsnap: departing a marker rightwards snaps back onto it");
            // Moving right, well past: released again.
            r.check(near_(magneticSnapBeat(4.30f, +1, grid, pull), 4.30f),
                    "fxsnap: past the pull radius the edge is free again");

            // Mirror image for leftward travel.
            r.check(near_(magneticSnapBeat(4.06f, -1, grid, pull), 4.06f),
                    "fxsnap: approaching a marker from the right leaves the edge free");
            r.check(near_(magneticSnapBeat(3.94f, -1, grid, pull), 4.0f),
                    "fxsnap: departing a marker leftwards snaps back onto it");
            r.check(near_(magneticSnapBeat(3.70f, -1, grid, pull), 3.70f),
                    "fxsnap: past the pull radius leftwards the edge is free again");

            // The two remaining approach/departure pairs, at a non-integer grid,
            // to prove nothing is hard-coded to whole beats.
            r.check(near_(magneticSnapBeat(1.97f, +1, 0.25f, 0.05f), 1.97f),
                    "fxsnap: quarter-beat grid, rightward approach stays free");
            r.check(near_(magneticSnapBeat(2.03f, -1, 0.25f, 0.05f), 2.03f),
                    "fxsnap: quarter-beat grid, leftward approach stays free");
            r.check(near_(magneticSnapBeat(2.03f, +1, 0.25f, 0.05f), 2.0f),
                    "fxsnap: quarter-beat grid, rightward departure snaps");
            r.check(near_(magneticSnapBeat(1.97f, -1, 0.25f, 0.05f), 2.0f),
                    "fxsnap: quarter-beat grid, leftward departure snaps");

            // dir 0 (gesture hasn't moved yet) and snap-off are both no-ops -
            // the latter is what Alt-drag and the "Snap: Off" button produce.
            r.check(near_(magneticSnapBeat(4.02f, 0, grid, pull), 4.02f),
                    "fxsnap: no travel direction yet means no snap");
            r.check(near_(magneticSnapBeat(4.02f, +1, 0.0f, pull), 4.02f),
                    "fxsnap: snapping off (Alt / Snap:Off) leaves the edge free");

            // A pull radius wider than a grid step (zoomed out until a step is
            // under 6 px) degenerates to "always snap to the departing marker",
            // and crucially still never reaches past that marker's neighbour.
            r.check(near_(magneticSnapBeat(3.5f, +1, grid, 2.0f), 3.0f)
                    && near_(magneticSnapBeat(3.99f, +1, grid, 2.0f), 3.0f)
                    && near_(magneticSnapBeat(3.5f, -1, grid, 2.0f), 4.0f),
                    "fxsnap: an oversized pull radius never reaches past one grid step");

            // The `snapped` out-flag has to distinguish "landed on the marker"
            // from "left alone", including the case where the input already sat
            // exactly on a marker so the returned value is unchanged either way.
            // Moving a layer picks between its two edges with this flag; using
            // "did the value change" instead makes a zero correction look like
            // no snap, so the free edge always wins and the layer never aligns.
            bool hit = true;
            magneticSnapBeat(3.5f, +1, grid, 0.1f, &hit);
            r.check(!hit, "fxsnap: out-flag is false when no marker catches the edge");
            magneticSnapBeat(4.05f, +1, grid, 0.1f, &hit);
            r.check(hit, "fxsnap: out-flag is true when a marker catches the edge");
            hit = false;
            magneticSnapBeat(4.0f, +1, grid, 0.1f, &hit);
            r.check(hit, "fxsnap: an edge already on a marker still reports a snap");
            hit = true;
            magneticSnapBeat(4.05f, 0, grid, 0.1f, &hit);
            r.check(!hit, "fxsnap: out-flag is false when snapping is disabled");
        }

        // ---- hosted-plugin param automation lanes: round-trip + helpers ------
        // Plugin params have no Param row, so their recorded lanes live in
        // Node::pluginParamAutomation (normalized 0..1) and serialize via
        // pluginAuto= lines. The actual knob-drag capture needs a live plugin
        // (covered manually - see known-issues.md), but the data plumbing,
        // node-scope resolver, and lane recorder are testable here.
        {
            NodeGraph g;
            auto& n = g.addNode("vst", NodeType::Effect, {}, {});
            // Param 4: a two-point sweep; param 9: a single point.
            n.pluginParamAutomation[4].points = { {0.0f, 0.10f}, {8.0f, 0.90f} };
            n.pluginParamAutomation[9].points = { {2.0f, 0.50f} };

            std::ostringstream oss;
            ProjectFile::writeProject(oss, g, nullptr, false, false);
            NodeGraph g2;
            std::istringstream iss(oss.str());
            ProjectFile::readProject(iss, g2, nullptr);

            r.check(g2.nodes.size() == 1, "pluginauto: node round-trips");
            auto& n2 = g2.nodes[0];
            r.check(n2.pluginParamAutomation.size() == 2,
                    "pluginauto: both plugin lanes round-trip");
            auto it4 = n2.pluginParamAutomation.find(4);
            auto it9 = n2.pluginParamAutomation.find(9);
            r.check(it4 != n2.pluginParamAutomation.end()
                    && it4->second.points.size() == 2,
                    "pluginauto: lane[4] keeps both points");
            r.check(it9 != n2.pluginParamAutomation.end()
                    && it9->second.points.size() == 1,
                    "pluginauto: lane[9] keeps its point");
            if (it4 != n2.pluginParamAutomation.end() && it4->second.points.size() == 2) {
                r.check(std::abs(it4->second.points[1].beat - 8.0f) < 1e-3f
                        && std::abs(it4->second.points[1].value - 0.90f) < 1e-3f,
                        "pluginauto: lane[4] point values round-trip");
            }
            // A plain plugin node with no lanes emits no pluginAuto= lines.
            r.check(oss.str().find("pluginAuto=") != std::string::npos,
                    "pluginauto: pluginAuto lines are emitted when lanes exist");

            // Node-scope resolver: hard Off gate + node override + inherit.
            Node pn; pn.armMode = AutoArmMode::Latch;
            r.check(resolveArmModeNode(AutoArmMode::Off, pn) == AutoArmMode::Off,
                    "pluginauto: node resolver honours the global Off gate");
            r.check(resolveArmModeNode(AutoArmMode::Touch, pn) == AutoArmMode::Latch,
                    "pluginauto: node override beats global");
            Node pn2; // Inherit
            r.check(resolveArmModeNode(AutoArmMode::Touch, pn2) == AutoArmMode::Touch,
                    "pluginauto: node inherits global when unset");

            // Lane recorder: an overwrite sweep replaces the just-passed span.
            AutomationLane lane;
            float last = -1.0f; bool did = false;
            recordAutomationPointLane(lane, last, did, 0.0f, 0.2f);
            recordAutomationPointLane(lane, last, did, 1.0f, 0.4f);
            recordAutomationPointLane(lane, last, did, 2.0f, 0.6f);
            r.check(did && lane.points.size() == 3,
                    "pluginauto: lane recorder appends points forward");
            // Re-sweep from 1.0 overwrites the (1,2] span instead of layering.
            last = -1.0f;
            recordAutomationPointLane(lane, last, did, 1.0f, 0.4f);
            recordAutomationPointLane(lane, last, did, 2.0f, 0.9f);
            bool has2 = false; int count2 = 0;
            for (auto& p : lane.points) if (std::abs(p.beat - 2.0f) < 1e-4f) { has2 = true; ++count2; }
            r.check(has2 && count2 == 1,
                    "pluginauto: overwrite sweep replaces (not layers) the span");
        }

        // ---- backward-compat: legacy inline base64 blob still loads ---------
        // Old projects embedded the grid as gzip+base64 in the 4th field (no
        // store). make/parse with a null store reproduce and decode that form.
        {
            GenerateTerrainParams gp;
            gp.lang = (int)GenLang::Builtin;
            gp.dims = { 4, 4 };
            gp.source = "0.5";
            gp.data.resize(16);
            for (int i = 0; i < 16; ++i) gp.data[(size_t)i] = (float)i / 15.0f;
            std::string legacy = makeGenerateTerrainScript(gp, /*store*/nullptr);
            r.check(legacy.find("|#") == std::string::npos,
                    "store: null-store make embeds legacy inline blob (no '#')");
            GenerateTerrainParams dec;
            bool ok = parseGenerateTerrainScript(legacy, dec, /*store*/nullptr);
            float maxErr = 0.0f;
            for (size_t i = 0; ok && i < dec.data.size() && i < gp.data.size(); ++i)
                maxErr = std::max(maxErr, std::abs(dec.data[i] - gp.data[i]));
            r.checkVal(ok && dec.data.size() == gp.data.size() && maxErr == 0.0f,
                       "store: legacy inline base64 blob still decodes", maxErr);
        }
    }
}

// ===========================================================================
// LAYER 2 - render through a real TerrainSynthProcessor + WAV export
// ===========================================================================
void testRender(Report& r, const juce::File& dir) {
    r.section("Layer 2: synth render (Sig-driven position -> audio)");
    const double dur = 1.0;

    // ---- 1D audio: Direct-mode playback of the ramp wavetable -----------
    {
        auto wav = dir.getChildFile("test_audio_1d.wav");  // written in layer 1
        std::string script = "__audio__:" + wav.getFullPathName().toStdString();
        // 1D: the lone axis is the phase axis, so a Sig sweep mixes with the
        // played pitch. We don't predict the waveform; we assert structural
        // correctness and export the WAV. Drive Sig X with a slow sweep so the
        // read position moves across the file.
        auto ro = renderTerrain(script, 1, /*Direct*/0,
            [](int /*d*/, int /*g*/) { return 0.0f; }, dur);
        writeWavFloat(dir.getChildFile("render_1d_direct.wav"), ro.audio, ro.sr);
        r.check(ro.built, "1D: terrain built from audio file");
        r.check(allFinite(ro.audio), "1D: output is finite (no NaN/Inf)");
        r.checkVal(rmsOf(ro.audio) > 1e-4, "1D: output non-silent", rmsOf(ro.audio));
        r.checkVal(peakAbs(ro.audio) <= 1.001f, "1D: output bounded |x|<=1",
                   peakAbs(ro.audio));
    }

    // ---- 2D image: AM-sine, sweep the column axis (Sig Y -> coord[1]) ----
    {
        auto png = dir.getChildFile("test_image_2d.png");   // from layer 1
        std::string script = "__image__:" + png.getFullPathName().toStdString();
        // Image brightness varies left->right; coord[1] is the column axis,
        // driven by the 2nd Sig pin ("Sig Y"). Ramp it 0->1, hold coord[0]
        // (row) constant. Predicted envelope swells from ~0 to full.
        auto ro = renderTerrain(script, 2, /*AM-sine*/1,
            [&](int d, int g) {
                if (d == 1) return (float) g / (float) (int)(dur * 44100 - 1); // sweep col
                return 0.5f;                                                    // row fixed
            }, dur);
        writeWavFloat(dir.getChildFile("render_2d_amsine_sweepY.wav"), ro.audio, ro.sr);
        double corr = envelopeCorrelation(ro);
        double rng  = envRange(ro);
        r.check(ro.built, "2D: terrain built from image");
        r.check(allFinite(ro.audio), "2D: output is finite");
        r.checkVal(rmsOf(ro.audio) > 1e-3, "2D: output non-silent", rmsOf(ro.audio));
        r.checkVal(rng > 0.02, "2D: amplitude moves as position sweeps (Sig routing live)",
                   rng);
        r.checkVal(corr > 0.9, "2D: output envelope tracks terrain readout", corr);
    }

    // ---- 3D video: AM-sine, sweep the frame/time axis (Sig X -> coord[0]) -
    {
        // Build the same gradient-by-frame video used in layer 1, bake it into
        // a __video__ script, and sweep coord[0] (frame) 0->1.
        const int F = 24, H = 16, W = 16;
        std::vector<uint8_t> gray((size_t) F * H * W);
        for (int f = 0; f < F; ++f)
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    gray[(size_t)((f * H + y) * W + x)] =
                        (uint8_t) std::lround(255.0 * f / (F - 1));
        VideoTerrainParams p;
        p.path = "selftest://synthetic";
        p.t0 = 0; p.t1 = 1; p.cropX = 0; p.cropY = 0; p.cropW = W; p.cropH = H;
        p.outW = W; p.outH = H; p.outFrames = F; p.gray = gray;
        std::string script = makeVideoTerrainScript(p);

        auto ro = renderTerrain(script, 3, /*AM-sine*/1,
            [&](int d, int g) {
                if (d == 0) return (float) g / (float) (int)(dur * 44100 - 1); // sweep frame
                return 0.5f;
            }, dur);
        writeWavFloat(dir.getChildFile("render_3d_amsine_sweepFrame.wav"), ro.audio, ro.sr);
        double corr = envelopeCorrelation(ro);
        double rng  = envRange(ro);
        r.check(ro.built, "3D: terrain built from baked video grid");
        r.check(allFinite(ro.audio), "3D: output is finite");
        r.checkVal(rmsOf(ro.audio) > 1e-3, "3D: output non-silent", rmsOf(ro.audio));
        r.checkVal(rng > 0.02, "3D: amplitude moves as position sweeps (Sig routing live)",
                   rng);
        r.checkVal(corr > 0.9, "3D: output envelope tracks terrain readout", corr);
    }
}

// ===========================================================================
// Direct single-cycle audition (the layered-frame editor's Play button)
// ===========================================================================
//
// The Play button ships the edited frame's rendered single cycle as a
// Node::AuditionCycleFrame on node.heldAudition; the synth voice must read
// ONLY that cycle (as a wavetable oscillator), replacing the cycle terrain
// entirely - so the audition is faithful even for an unplaced frame. We drive
// a real TerrainSynthProcessor with no MIDI, only a held audition, and assert:
//   - a non-trivial cycle produces finite, bounded, non-silent output, and
//   - an all-zero cycle produces silence (proving the override replaces the
//     terrain rather than leaking it), and
//   - clearing heldAudition releases the note (output decays to silence).
void testFrameAudition(Report& r, const juce::File& dir) {
    r.section("Frame audition (layered-editor Play: direct single-cycle override)");

    auto wav = dir.getChildFile("test_audio_1d.wav");   // written in layer 1
    if (!wav.existsAsFile()) {
        r.note("test_audio_1d.wav missing (layer 1 did not run) - skipping.");
        return;
    }
    const std::string script =
        "__audio__:" + wav.getFullPathName().toStdString();

    Transport transport;
    transport.sampleRate = 44100.0;
    transport.bpm = 120.0;

    auto makeNode = [&]() {
        Node node;
        node.id = 1;
        node.type = NodeType::TerrainSynth;
        node.name = "selftest-audition";
        node.script = script;
        node.pinsIn.push_back(Pin{ 1, "MIDI", PinKind::Midi, true, 2 });
        node.pinsIn.push_back(Pin{ 2, "Sig X", PinKind::Signal, true, 1 });
        node.pinsOut.push_back(Pin{ 100, "Audio", PinKind::Audio, false, 2 });
        node.params.push_back({ "Volume", 1.0f, 0.0f, 1.0f });
        node.params.push_back({ "Synth Mode", 0.0f, 0.0f, 2.0f }); // Direct
        node.ahdsrEnvelope.attackMs = 1.0f;
        node.ahdsrEnvelope.holdMs = 4000.0f;
        node.ahdsrEnvelope.decayMs = 1.0f;
        node.ahdsrEnvelope.sustain = 1.0f;
        node.ahdsrEnvelope.releaseMs = 1.0f;
        node.ahdsrEnvelope.velocitySensitivity = 0.0f;
        AHDSREnvelope::setDefaultCurves(node.ahdsrEnvelope);
        return node;
    };

    // Render `blocks` blocks of 512 frames through the processor, holding the
    // supplied audition the whole time (or clearing it mid-way if clearAt >= 0).
    auto runHeld = [&](std::shared_ptr<Node::AuditionCycleFrame> cyc,
                       int blocks, int clearAt) {
        Node node = makeNode();
        TerrainSynthProcessor proc(node, transport);
        proc.prepareToPlay(44100.0, 512);

        auto ev = std::make_shared<Node::AuditionEvent>();
        ev->isNoteOn = true;
        ev->pitch = 69;
        ev->velocity = 127;
        ev->cycleFrame = cyc;
        node.heldAudition = ev;

        std::vector<float> out;
        juce::AudioBuffer<float> buf(3, 512); // 2 audio + 1 sig
        for (int b = 0; b < blocks; ++b) {
            if (clearAt >= 0 && b == clearAt) node.heldAudition.reset();
            buf.setSize(3, 512, false, false, true);
            buf.clear();
            juce::MidiBuffer midi;
            proc.processBlock(buf, midi);
            const float* p = buf.getReadPointer(0);
            for (int s = 0; s < 512; ++s) out.push_back(p[s]);
        }
        return out;
    };

    // A non-trivial single cycle (one period of a sine, 256 samples).
    auto sineCyc = std::make_shared<Node::AuditionCycleFrame>();
    sineCyc->cycle.resize(256);
    for (int i = 0; i < 256; ++i)
        sineCyc->cycle[(size_t)i] = std::sin(2.0 * 3.14159265358979 * i / 256.0);

    {
        auto out = runHeld(sineCyc, 8, -1);
        r.check(allFinite(out), "audition: output is finite (no NaN/Inf)");
        r.checkVal(peakAbs(out) <= 1.001f, "audition: output bounded |x|<=1",
                   peakAbs(out));
        r.checkVal(rmsOf(out) > 1e-3, "audition: non-trivial cycle is audible",
                   rmsOf(out));
    }

    {
        // All-zero cycle: the override replaces the terrain with silence, so the
        // output must be silent - this is what proves the audition cycle (not
        // the underlying terrain) drives the voice.
        auto zeroCyc = std::make_shared<Node::AuditionCycleFrame>();
        zeroCyc->cycle.assign(256, 0.0f);
        auto out = runHeld(zeroCyc, 8, -1);
        r.checkVal(rmsOf(out) < 1e-5,
                   "audition: zero cycle is silent (override replaces terrain)",
                   rmsOf(out));
    }

    {
        // Clearing heldAudition mid-render releases the note; with a 1ms release
        // the tail must fall to silence well before the end.
        auto out = runHeld(sineCyc, 16, /*clearAt*/4);
        const int n = (int)out.size();
        std::vector<float> tail(out.begin() + (size_t)(n - 512), out.end());
        r.checkVal(rmsOf(tail) < 1e-4,
                   "audition: clearing heldAudition releases the voice",
                   rmsOf(tail));
    }
}

// ===========================================================================
// Held-audition helpers (setNodeHeldAuditionCycle / clearNodeHeldAudition)
// ===========================================================================
//
// The standalone Frequency Domain / Wavelet Space editors' Preview buttons
// ship their rendered cycle through these shared helpers (node_graph.h). Verify
// the helper packages the cycle into a held A4 note-on and that empty-cycle /
// clear both release it. Pure data checks - no synth needed.
void testHeldAuditionHelpers(Report& r) {
    r.section("Held audition helpers (editor Preview path)");

    Node node;
    node.id = 1;

    std::vector<float> cyc(64);
    for (int i = 0; i < 64; ++i) cyc[(size_t)i] = (float)i / 63.0f;

    setNodeHeldAuditionCycle(node, cyc);
    r.check((bool)node.heldAudition, "held audition: ship sets heldAudition");
    if (node.heldAudition) {
        r.check(node.heldAudition->isNoteOn, "held audition: event is a note-on");
        r.checkVal(node.heldAudition->pitch == 69, "held audition: pitch is A4 (69)",
                   node.heldAudition->pitch);
        r.checkVal(node.heldAudition->velocity == 127, "held audition: full velocity",
                   node.heldAudition->velocity);
        bool cycOk = node.heldAudition->cycleFrame
                     && node.heldAudition->cycleFrame->cycle.size() == cyc.size();
        r.check(cycOk, "held audition: cycle carried through unchanged");
    }

    setNodeHeldAuditionCycle(node, std::vector<float>{});
    r.check(!node.heldAudition, "held audition: empty cycle clears it");

    setNodeHeldAuditionCycle(node, cyc);
    clearNodeHeldAudition(node);
    r.check(!node.heldAudition, "held audition: clearNodeHeldAudition releases it");
}

// ===========================================================================
// LAYER 3 - ffmpeg round-trip (optional)
// ===========================================================================
void testVideoDecode(Report& r, const juce::File& dir) {
    r.section("Layer 3: ffmpeg video decode (optional)");
    if (!VideoDecoder::available()) {
        r.note("ffmpeg not found on PATH - skipping video decode test.");
        return;
    }

    auto video = dir.getChildFile("test_video.mp4");
    video.deleteFile();
    juce::ChildProcess cp;
    juce::StringArray cmd;
    cmd.add(VideoDecoder::ffmpegCommand());
    cmd.add("-y");
    cmd.add("-f"); cmd.add("lavfi");
    cmd.add("-i"); cmd.add("testsrc=size=64x64:rate=10:duration=1");
    cmd.add("-pix_fmt"); cmd.add("yuv420p");
    cmd.add(video.getFullPathName());
    // Capture stdout+stderr and read it fully: readAllProcessOutput() blocks
    // until ffmpeg closes its pipes (i.e. actually exits), which is a more
    // reliable "wait for completion" than waitForProcessToFinish() followed by
    // an immediate file-exists check (that race made this test flaky - the file
    // could still be flushing when we looked). On failure the captured output
    // is surfaced so a real ffmpeg error is diagnosable instead of a bare skip.
    bool started = cp.start(cmd, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr);
    juce::String childOut;
    if (started) childOut = cp.readAllProcessOutput();
    bool made = video.existsAsFile() && video.getSize() > 0;
    if (!r.check(made, "video: ffmpeg generated a test clip")) {
        if (!started)
            r.note("ffmpeg child process failed to start - skipping decode assertions.");
        else
            r.note("ffmpeg ran but produced no clip - skipping decode assertions. Output:\n"
                   + childOut);
        return;
    }

    auto info = VideoDecoder::probe(video);
    r.check(info.ok && info.width == 64 && info.height == 64,
            "video: probe reports 64x64");

    std::string err;
    auto grid = VideoDecoder::decodeGrid(video, 0.0, 1.0,
                                         0, 0, 64, 64, /*outW*/8, /*outH*/8,
                                         /*outFrames*/4, &err);
    const size_t expect = (size_t) 8 * 8 * 4;
    r.check(grid.size() == expect, "video: decodeGrid returned 8x8x4 bytes");
    if (grid.size() == expect) {
        // testsrc is a spatially+temporally varying pattern, so the decoded
        // grid must not be uniform.
        uint8_t lo = 255, hi = 0;
        for (uint8_t v : grid) { lo = std::min(lo, v); hi = std::max(hi, v); }
        r.check(hi > lo, "video: decoded grid is non-uniform");
    } else if (!err.empty()) {
        r.note("decodeGrid error: " + juce::String(err));
    }
}

// ===========================================================================
// LAYER 4 - warp framework (Bucket A primitives + serialization)
// ===========================================================================
void testWarp(Report& r) {
    r.section("Layer 4: warp framework (shape bending)");

    // ---- Identity at amount 0 (no-op contract) -------------------------
    {
        bool phaseId = true, ampId = true;
        for (float p = 0.0f; p < 1.0f; p += 0.05f) {
            for (const auto& info : warpMethodRegistry()) {
                if (info.domain == WarpDomain::Phase) {
                    if (std::abs(warpPhaseValue(info.method, p, 0.0f) - p) > 1e-5f)
                        phaseId = false;
                } else if (info.domain == WarpDomain::Amplitude) {
                    float x = 2.0f * p - 1.0f;
                    if (std::abs(warpAmpValue(info.method, x, 0.0f) - x) > 1e-5f)
                        ampId = false;
                }
            }
        }
        r.check(phaseId, "warp: every phase method is identity at amount 0");
        r.check(ampId,   "warp: every amplitude method is identity at amount 0");
    }

    // ---- All methods stay finite + phase stays in [0,1) ----------------
    {
        bool finiteOk = true, phaseRangeOk = true;
        for (float a = 0.0f; a <= 1.0f; a += 0.1f) {
            for (float p = 0.0f; p < 1.0f; p += 0.05f) {
                for (const auto& info : warpMethodRegistry()) {
                    if (info.domain == WarpDomain::Phase) {
                        float wp = warpPhaseValue(info.method, p, a);
                        if (!std::isfinite(wp)) finiteOk = false;
                        if (wp < 0.0f || wp >= 1.0001f) phaseRangeOk = false;
                    } else if (info.domain == WarpDomain::Amplitude) {
                        float x = 2.0f * p - 1.0f;
                        if (!std::isfinite(warpAmpValue(info.method, x, a)))
                            finiteOk = false;
                    }
                }
            }
        }
        r.check(finiteOk, "warp: all methods finite across amount/phase sweep");
        r.check(phaseRangeOk, "warp: phase methods keep read phase in [0,1)");
    }

    // ---- Known-shape checks --------------------------------------------
    {
        // BendPlus pinches energy toward the end: the midpoint maps earlier.
        float bp = warpPhaseValue(WarpMethod::BendPlus, 0.5f, 0.8f);
        r.checkVal(bp < 0.5f, "warp: Bend+ pulls the midpoint phase earlier", bp);
        // Flip at full amount inverts the sample.
        float fl = warpAmpValue(WarpMethod::Flip, 0.7f, 1.0f);
        r.checkVal(std::abs(fl + 0.7f) < 1e-5f, "warp: Flip inverts at amount 1", fl);
        // SoftClip is normalized tanh: monotonic, maps 0->0 and 1->1, pushes
        // mid/high values toward the rail (the harmonic-adding "warmth"), and
        // never lets a unit input exceed the rail. Check the rail behaviour and
        // monotonic boost of a low value.
        float scHot = warpAmpValue(WarpMethod::SoftClip, 0.9f, 1.0f);
        float scLow = warpAmpValue(WarpMethod::SoftClip, 0.2f, 1.0f);
        r.checkVal(scHot > 0.9f && scHot <= 1.001f,
                   "warp: SoftClip pushes a hot sample toward the rail", scHot);
        r.checkVal(scLow > 0.2f && scLow < scHot,
                   "warp: SoftClip stays monotonic (low < high)", scLow);
    }

    // ---- applyWarpChain on a buffer ------------------------------------
    {
        std::vector<float> cyc(64);
        for (int i = 0; i < 64; ++i) cyc[i] = std::sin(2.0f * 3.14159265f * i / 64.0f);
        std::vector<float> flipped = cyc;
        std::vector<WarpOp> ops = { { WarpMethod::Flip, 1.0f, 0.0f, true } };
        applyWarpChain(ops, flipped);
        bool inverted = true;
        for (int i = 0; i < 64; ++i)
            if (std::abs(flipped[i] + cyc[i]) > 1e-4f) inverted = false;
        r.check(inverted, "warp: applyWarpChain(Flip) inverts a sine buffer");
    }

    // ---- Chain order matters (the reason reorder controls exist) -------
    // Wavefold-then-HardClip is not the same transfer curve as
    // HardClip-then-Wavefold, so swapping two stages must audibly change the
    // result. This is the invariant the up/down reorder arrows let the user
    // exploit; if it ever became order-independent the controls would be inert.
    {
        std::vector<float> base(128);
        for (int i = 0; i < 128; ++i)
            base[i] = 1.6f * std::sin(2.0f * 3.14159265f * i / 128.0f);
        WarpOp fold{ WarpMethod::Wavefold, 0.7f, 0.0f, true };
        WarpOp clip{ WarpMethod::HardClip, 0.6f, 0.0f, true };

        std::vector<float> ab = base, ba = base;
        applyWarpChain({ fold, clip }, ab);   // fold then clip
        applyWarpChain({ clip, fold }, ba);   // clip then fold

        float maxDiff = 0.0f;
        for (int i = 0; i < 128; ++i)
            maxDiff = std::max(maxDiff, std::abs(ab[i] - ba[i]));
        r.checkVal(maxDiff > 1e-3f,
                   "warp: chain order changes the result (reorder is meaningful)",
                   maxDiff);

        // And a swap of the two-element chain reproduces the other ordering
        // exactly - the operation the up/down arrows perform on the vector.
        std::vector<WarpOp> chain = { fold, clip };
        std::swap(chain[0], chain[1]);
        std::vector<float> swapped = base;
        applyWarpChain(chain, swapped);
        bool sameAsBA = true;
        for (int i = 0; i < 128; ++i)
            if (std::abs(swapped[i] - ba[i]) > 1e-6f) sameAsBA = false;
        r.check(sameAsBA, "warp: swapping two ops yields the reversed-order chain");
    }

    // ---- Factory waveform importer (makeFactoryFrame) ------------------
    // A single cycle imported from the factory bank (or a user wav) becomes a
    // one-layer Drawn/Freehand LayeredWaveform. Verify the importer preserves a
    // 512-sample cycle and resamples an off-size cycle, and that the result is
    // an editable "layered" frame that renders the shape back.
    {
        std::vector<float> sine512(512);
        for (int i = 0; i < 512; ++i)
            sine512[(size_t)i] = std::sin(2.0f * 3.14159265f * i / 512.0f);
        auto frame = LayeredWaveEditorComponent::makeFactoryFrame(sine512);
        r.check(frame != nullptr && std::strcmp(frame->typeId(), "layered") == 0,
                "factory: imported cycle becomes an editable layered frame");
        if (frame) {
            std::vector<float> out;
            frame->renderRaw(512, out);
            // Rendered cycle should correlate strongly with the source sine
            // (peak-normalisation aside). Use normalised cross-correlation.
            double dot = 0, na = 0, nb = 0;
            for (int i = 0; i < 512 && i < (int)out.size(); ++i) {
                dot += (double)out[(size_t)i] * sine512[(size_t)i];
                na += (double)out[(size_t)i] * out[(size_t)i];
                nb += (double)sine512[(size_t)i] * sine512[(size_t)i];
            }
            const double corr = (na > 0 && nb > 0) ? dot / std::sqrt(na * nb) : 0;
            r.checkVal(corr > 0.99,
                       "factory: rendered cycle matches the imported sine", corr);
        }
        // Off-size (600-sample) cycle is resampled to a valid 512-sample layer.
        std::vector<float> sine600(600);
        for (int i = 0; i < 600; ++i)
            sine600[(size_t)i] = std::sin(2.0f * 3.14159265f * i / 600.0f);
        auto frame600 = LayeredWaveEditorComponent::makeFactoryFrame(sine600);
        std::vector<float> out600;
        if (frame600) frame600->renderRaw(512, out600);
        r.check(frame600 != nullptr && out600.size() == 512,
                "factory: off-size cycle resamples to a 512-sample layer");
    }

    // ---- WaveformBank packed-asset loader (soft: asset is a build artifact) --
    // The library ships as cpp/resources/waveforms.bin, copied next to the exe.
    // It's generated by pack_waveforms.py, so a fresh checkout without the asset
    // shouldn't fail the suite - we note absence and pass. When present, sanity-
    // check the structure the browser relies on.
    {
        auto& bank = WaveformBank::get();
        const bool ok = bank.ensureLoaded();
        if (!ok || bank.isEmpty()) {
            r.note("factory bank: waveforms.bin not present (generated asset) - "
                   "skipping structural checks");
        } else {
            r.check(!bank.categories().empty(),
                    "factory bank: at least one category present");
            r.check(bank.samples(0).size() == 512,
                    "factory bank: entry sample buffer is 512 long");
            int curated = 0;
            for (int i = 0; i < bank.numEntries(); ++i)
                if (bank.entry(i).curated) ++curated;
            r.checkVal(curated > 0,
                       "factory bank: at least one curated (starred) waveform",
                       curated);
        }
    }

    // ---- Factory-waveform REFERENCE (fork-by-default / store-by-name) -------
    // A picked factory waveform stores only its stable bank NAME in the project
    // (a "factory=" field), not the 512 samples, and re-resolves on load. Editing
    // the cycle forks it (the samples get embedded). Content-addressed: the
    // serializer only writes the name while drawnSamples still match the bank.
    {
        auto& bank = WaveformBank::get();
        if (bank.ensureLoaded() && !bank.isEmpty()) {
            const std::string name = bank.entry(0).name;
            std::vector<float> cycle = bank.samples(0);   // 512

            // Build a one-layer frame referencing factory entry 0.
            LayeredWaveform ref;
            { WaveLayer l; l.shape = WaveLayer::Drawn; l.freehandMode = true;
              l.drawnSamples = cycle; l.factoryRef = name; ref.layers.push_back(l); }
            const std::string encRef = ref.encode();

            // The same cycle WITHOUT the reference embeds all 512 samples.
            LayeredWaveform emb = ref;
            emb.layers[0].factoryRef.clear();
            const std::string encEmb = emb.encode();

            r.check(encRef.find("factory=") != std::string::npos,
                    "factory ref: serializes the bank name, not samples");
            r.check(encRef.size() * 4 < encEmb.size(),
                    "factory ref: encoded form is far smaller than embedding");

            // Round-trip: name decodes back, samples re-resolve from the bank.
            LayeredWaveform back;
            back.decode(encRef);
            bool resolved = !back.layers.empty()
                && back.layers[0].factoryRef == name
                && back.layers[0].drawnSamples.size() == cycle.size()
                && std::equal(cycle.begin(), cycle.end(),
                              back.layers[0].drawnSamples.begin());
            r.check(resolved,
                    "factory ref: round-trips by name and re-resolves the cycle");

            // Edit forks it: a diverged cycle (even with factoryRef still set)
            // must embed the real samples, never the stale name.
            LayeredWaveform edited = ref;
            edited.layers[0].drawnSamples[10] =
                edited.layers[0].drawnSamples[10] > 0.0f ? -0.9f : 0.9f;
            const std::string encEd = edited.encode();
            r.check(encEd.find("factory=") == std::string::npos,
                    "factory ref: edited cycle forks (embeds samples, drops name)");
            LayeredWaveform edBack;
            edBack.decode(encEd);
            bool forkOk = !edBack.layers.empty()
                && edBack.layers[0].factoryRef.empty()
                && std::abs(edBack.layers[0].drawnSamples[10]
                            - edited.layers[0].drawnSamples[10]) < 1e-4f;
            r.check(forkOk,
                    "factory ref: forked cycle round-trips the edit with no ref");
        } else {
            r.note("factory ref: waveforms.bin not present - skipping ref round-trip");
        }

        // Unresolvable name (always runs, no bank file needed): resolve degrades
        // to a silent cycle but KEEPS the name so a re-save still references it.
        {
            WaveLayer bad;
            bad.factoryRef = "__no_such_factory_waveform_xyz__";
            bad.resolveFactoryRef();
            r.check(bad.drawnSamples.empty()
                    && bad.factoryRef == "__no_such_factory_waveform_xyz__"
                    && bad.shape == WaveLayer::Drawn,
                    "factory ref: unresolvable name degrades to silent, keeps ref");
        }

        // Load-scoped collector: an active FactoryRefResolutionScope captures
        // unresolved names (deduplicated) so a project load can warn the user;
        // with no scope active, resolution failures stay silent.
        {
            WaveLayer a, b, c;
            a.factoryRef = "__missing_one__";
            b.factoryRef = "__missing_two__";
            c.factoryRef = "__missing_one__";  // duplicate of a
            {
                FactoryRefResolutionScope scope;
                a.resolveFactoryRef();
                b.resolveFactoryRef();
                c.resolveFactoryRef();
                r.check(scope.unresolved.size() == 2
                        && scope.unresolved[0] == "__missing_one__"
                        && scope.unresolved[1] == "__missing_two__",
                        "factory ref: scope collects unresolved names, deduped, in order");
            }
            // Outside any scope, resolution must not crash and reports nowhere.
            WaveLayer d;
            d.factoryRef = "__missing_three__";
            d.resolveFactoryRef();
            r.check(d.drawnSamples.empty(),
                    "factory ref: resolve with no active scope is a silent no-op");
        }
    }

    // ---- Serialization round-trip (per-frame morph chain) ---
    {
        WavetableDoc doc;
        doc.mode = WavetableMode::Grid;
        int fid = doc.addLibraryEntry(std::make_unique<LayeredWaveform>(), "F");
        doc.libraryFrameById(fid)->morphChain = {
            { WarpMethod::BendPlus, 0.42f, 0.0f, true },
            { WarpMethod::SoftClip, 0.75f, 0.1f, false },
        };
        doc.gridDims = { 1 };
        doc.cellWaveformIds = { fid };
        std::string enc = doc.encode();
        bool hasTag = enc.find(":morph:") != std::string::npos;
        r.check(hasTag, "morph: encode appends a :morph: section when a frame carries a chain");

        WavetableDoc back;
        bool ok = back.decode(enc);
        r.check(ok, "morph: doc with per-frame morph chain decodes");
        const IWavetableFrame* bf = back.libraryFrameById(fid);
        bool match = bf && bf->morphChain.size() == 2;
        if (match) {
            const auto& a0 = bf->morphChain[0];
            const auto& a1 = bf->morphChain[1];
            match = a0.method == WarpMethod::BendPlus
                 && std::abs(a0.amount - 0.42f) < 1e-4f && a0.enabled
                 && a1.method == WarpMethod::SoftClip
                 && std::abs(a1.amount - 0.75f) < 1e-4f
                 && std::abs(a1.aux - 0.1f) < 1e-4f && !a1.enabled;
        }
        r.check(match, "morph: per-frame morph chain survives an encode->decode round trip");

        // A frame with an empty chain must NOT write the morph tag (byte-
        // compatible with files whose frames have no morph chain).
        WavetableDoc empt;
        empt.mode = WavetableMode::Grid;
        int efid = empt.addLibraryEntry(std::make_unique<LayeredWaveform>(), "E");
        empt.gridDims = { 1 };
        empt.cellWaveformIds = { efid };
        std::string encEmpty = empt.encode();
        bool noTag = encEmpty.find(":morph:") == std::string::npos;
        r.check(noTag, "morph: a frame with an empty chain omits the :morph: section");
        WavetableDoc emptBack;
        emptBack.decode(encEmpty);
        const IWavetableFrame* ebf = emptBack.libraryFrameById(efid);
        r.check(ebf && ebf->morphChain.empty(),
                "morph: no-morph payload decodes to an empty chain");
    }

    // ---- Standalone single-frame instruments ("__framesynth__:") -------------
    // The six frame types as their own node type wrap one frame in a 1x1-grid
    // WavetableDoc behind a __framesynth__ prefix, then reuse the entire
    // wavetable render path. Validate the wrapper helpers, the prefix strip, and
    // that a frame synth classifies / decodes identically to the equivalent
    // single-frame wavetable (the "not a 1-frame wavetable in disguise to the
    // USER, but identical DSP under the hood" guarantee).
    {
        const char* kTypeIds[] = { "layered", "spectral", "wavelet",
                                   "sample", "granular", "inharmonic" };
        for (const char* tid : kTypeIds) {
            std::string script = defaultFrameSynthScriptForType(tid);
            r.check(!script.empty(),
                    std::string("framesynth: defaultFrameSynthScriptForType('") + tid + "') is non-empty");
            r.check(isFrameSynthScript(script),
                    std::string("framesynth: '") + tid + "' script carries the __framesynth__ prefix");

            // The effective (prefix-stripped) body is a plain wavetable encode,
            // so it must NOT itself look like a frame synth and must decode as a
            // WavetableDoc.
            std::string body = effectiveSynthScript(script);
            r.check(!isFrameSynthScript(body),
                    std::string("framesynth: '") + tid + "' effective body has the prefix stripped");
            WavetableDoc bodyDoc;
            r.check(bodyDoc.decode(body),
                    std::string("framesynth: '") + tid + "' effective body decodes as a WavetableDoc");

            // decodeFrameSynthScript yields the single frame of the right type.
            WavetableDoc outDoc;
            std::unique_ptr<IWavetableFrame> frame = decodeFrameSynthScript(script, &outDoc);
            r.check(frame != nullptr,
                    std::string("framesynth: '") + tid + "' decodes back to a frame");
            r.check(frame && frame->typeId() == std::string(tid),
                    std::string("framesynth: '") + tid + "' decoded frame keeps its type id");
            r.check(outDoc.library.size() == 1 && outDoc.cellWaveformIds.size() == 1,
                    std::string("framesynth: '") + tid + "' is exactly one frame in a 1-cell grid");

            // A frame synth must classify the same as the equivalent single-frame
            // wavetable (the synth dispatch treats the wrapped body identically).
            // Build the bare wavetable from a freshly-decoded clone of the frame.
            std::unique_ptr<IWavetableFrame> frameForBare = decodeFrameSynthScript(script);
            std::string bareWavetable =
                frameForBare ? makeSingleFrameWavetable(std::move(frameForBare)).encode()
                             : std::string();
            r.check(!bareWavetable.empty()
                        && classifySynthSource(script) == classifySynthSource(bareWavetable),
                    std::string("framesynth: '") + tid + "' classifies like its bare single-frame wavetable");
        }

        // effectiveSynthScript is a no-op for a plain (non-prefixed) script, so
        // the wavetable path is untouched by the frame-synth machinery.
        std::string plainWt = WavetableDoc::defaultSingleSine().encode();
        r.check(effectiveSynthScript(plainWt) == plainWt,
                "framesynth: effectiveSynthScript leaves a plain wavetable script unchanged");
        r.check(!isFrameSynthScript(plainWt),
                "framesynth: a plain wavetable script is not a frame synth");
    }

    // ---- Waveshaper effect-node identity round-trip --------------------------
    // Each amplitude-domain Waveshaper node stores its method as a
    // "__waveshaper:<token>__" script; the processor parses it back. Verify the
    // script<->method round-trip for every exposed method, and that a plain /
    // unknown script is treated as identity (None).
    {
        const auto& wm = waveshaperMethods();
        r.check(wm.size() == 10,
                "waveshaper: exactly ten amplitude-domain methods exposed");
        for (WarpMethod m : wm) {
            std::string script = waveshaperScriptFor(m);
            r.check(isWaveshaperScript(script),
                    std::string("waveshaper: script for '") + warpMethodName(m)
                        + "' carries the __waveshaper: prefix");
            r.check(waveshaperMethodFromScript(script) == m,
                    std::string("waveshaper: '") + warpMethodName(m)
                        + "' round-trips script -> method");
            // Every exposed method must be amplitude-domain (phase warps can't
            // be standalone effect nodes - they need the synth read position).
            r.check(warpDomainOf(m) == WarpDomain::Amplitude,
                    std::string("waveshaper: '") + warpMethodName(m)
                        + "' is an amplitude-domain method");
        }
        r.check(!isWaveshaperScript("__tremolo__"),
                "waveshaper: a non-waveshaper script is not a waveshaper");
        r.check(waveshaperMethodFromScript("__tremolo__") == WarpMethod::None,
                "waveshaper: a non-waveshaper script parses to None (identity)");
        r.check(waveshaperMethodFromScript("__waveshaper:bogus__") == WarpMethod::None,
                "waveshaper: an unknown token parses to None (identity passthrough)");
    }

    // ---- Project save/load: framesynth instrument + waveshaper node ----------
    // Both new node kinds round-trip through nothing but node.script + params, so
    // a full project save/load must preserve them. Build a graph with one
    // single-frame instrument and one waveshaper, serialize, reload, and verify
    // each node survives with its identity and a non-default param value intact.
    {
        NodeGraph g;
        // Single-frame Granular instrument (the framesynth-wrapped wavetable).
        int instId = g.addNode("Granular Inst", NodeType::Instrument, {}, {}).id;
        if (Node* inst = g.findNode(instId)) {
            inst->script = defaultFrameSynthScriptForType("granular");
            inst->params.push_back({ "Volume", 0.8f, 0.0f, 1.0f }); // non-default
        }
        // Waveshaper effect node (Wavefold), non-default Fold amount.
        int wsId = g.addNode("Waveshaper", NodeType::Effect,
                             { Pin{0, "Audio In", PinKind::Audio, true} },
                             { Pin{0, "Audio Out", PinKind::Audio, false} }).id;
        if (Node* ws = g.findNode(wsId)) {
            ws->script = waveshaperScriptFor(WarpMethod::Wavefold);
            const char* pl = warpParamLabel(WarpMethod::Wavefold);
            ws->params.push_back({ (pl && *pl) ? pl : "Amount", 0.42f, 0.0f, 1.0f });
        }

        std::string saved = ProjectFile::serializeForUndo(g);
        NodeGraph g2;
        bool ld = ProjectFile::loadFromString(saved, g2);
        r.check(ld, "fs/ws save-load: project round-trips");

        const Node* inst2 = g2.findNode(instId);
        r.check(inst2 && isFrameSynthScript(inst2->script),
                "fs/ws save-load: framesynth instrument keeps its script");
        if (inst2) {
            std::unique_ptr<IWavetableFrame> fr =
                decodeFrameSynthScript(inst2->script);
            r.check(fr && std::string(fr->typeId()) == "granular",
                    "fs/ws save-load: framesynth body still decodes to its frame type");
            float vol = -1.0f;
            for (const auto& p : inst2->params) if (p.name == "Volume") vol = p.value;
            r.checkVal(std::abs(vol - 0.8f) < 1e-4,
                       "fs/ws save-load: framesynth param value survives", vol);
        }

        const Node* ws2 = g2.findNode(wsId);
        r.check(ws2 && waveshaperMethodFromScript(ws2->script) == WarpMethod::Wavefold,
                "fs/ws save-load: waveshaper keeps its method");
        if (ws2) {
            const char* pl = warpParamLabel(WarpMethod::Wavefold);
            float amt = -1.0f;
            for (const auto& p : ws2->params) if (p.name == pl) amt = p.value;
            r.checkVal(std::abs(amt - 0.42f) < 1e-4,
                       "fs/ws save-load: waveshaper amount value survives", amt);
        }
    }

    // ---- Freeze persistence: node cache metadata round-trips through a save ---
    // Regression for the "freezes don't survive save/reload" bug. A real save
    // (writeProject with includeBlobs=true) must emit the freeze metadata for a
    // disk-backed frozen node, and a reload must restore enabled/valid/useDisk/
    // inputHash/sampleRate/numSamples so rehydrateNodeCaches can re-attach the
    // PCM. Undo snapshots (includeBlobs=false) must NOT carry any of it - freeze
    // state is out-of-band session state for undo, and reparsing a snapshot must
    // never imply a disk cache. This test only exercises the metadata layer (no
    // real PCM file), which is the part project_file.cpp owns.
    {
        NodeGraph g;
        int aId = g.addNode("FrozenSynth", NodeType::Instrument, {},
                            { Pin{0, "Audio Out", PinKind::Audio, false} }).id;
        int bId = g.addNode("AutoOff", NodeType::Instrument, {},
                            { Pin{0, "Audio Out", PinKind::Audio, false} }).id;
        if (Node* a = g.findNode(aId)) {
            a->cache.enabled = true;
            a->cache.valid = true;
            a->cache.useDisk = true;               // disk-backed freeze
            a->cache.inputHash = 0xABCDEF1234567890ull;
            a->cache.sampleRate = 48000.0;
            a->cache.startSample = 0;
            a->cache.numSamples = 123456;
        }
        // Node B: no freeze, but the user turned auto-cache OFF - a preference
        // that must persist independently of any cached audio.
        if (Node* b = g.findNode(bId))
            b->cache.autoCache = false;

        // Real save (blobs on) - freeze payload expected.
        std::ostringstream oss;
        ProjectFile::writeProject(oss, g, nullptr, /*includeView*/ true,
                                  /*includeBlobs*/ true);
        NodeGraph g2;
        bool ld = ProjectFile::loadFromString(oss.str(), g2);
        r.check(ld, "freeze persist: project round-trips");

        const Node* a2 = g2.findNode(aId);
        r.check(a2 && a2->cache.enabled && a2->cache.valid && a2->cache.useDisk,
                "freeze persist: frozen flags survive save/reload");
        if (a2) {
            r.check(a2->cache.inputHash == 0xABCDEF1234567890ull,
                    "freeze persist: inputHash survives (unsigned 64-bit)");
            r.checkVal(std::abs(a2->cache.sampleRate - 48000.0) < 1.0,
                       "freeze persist: sampleRate survives", (float)a2->cache.sampleRate);
            r.checkVal(a2->cache.numSamples == 123456,
                       "freeze persist: numSamples survives", (float)a2->cache.numSamples);
        }
        const Node* b2 = g2.findNode(bId);
        r.check(b2 && !b2->cache.autoCache,
                "freeze persist: auto-cache=off preference survives");
        r.check(b2 && !b2->cache.valid && !b2->cache.enabled,
                "freeze persist: non-frozen node stays unfrozen");

        // Undo snapshot (blobs off) - NO freeze payload. The frozen node must
        // come back with a default (non-frozen) cache so undo/redo never implies
        // a disk cache that isn't there.
        std::string snap = ProjectFile::serializeForUndo(g);
        NodeGraph g3;
        ProjectFile::loadFromString(snap, g3);
        const Node* a3 = g3.findNode(aId);
        r.check(a3 && !a3->cache.enabled && !a3->cache.valid && !a3->cache.useDisk,
                "freeze persist: undo snapshot omits freeze payload");
        const Node* b3 = g3.findNode(bId);
        // autoCache is NOT gated on includeBlobs (it's a preference), so it
        // still travels in snapshots.
        r.check(b3 && !b3->cache.autoCache,
                "freeze persist: auto-cache preference travels in snapshots too");
    }

    // ---- Per-frame morph: two frames carry INDEPENDENT chains ----------------
    {
        // The whole point of moving the morph chain onto the frame: editing one
        // frame's chain must not touch another's. Build a 2-frame grid where each
        // frame has a distinct chain, round-trip, and verify each frame keeps its
        // own ops. Also verify the node's frame-scope warp params are keyed by
        // warpFrameId so the two frames' params don't collide.
        NodeGraph g;
        int nId = g.addNode("wt", NodeType::TerrainSynth, {}, {}).id;
        WavetableDoc doc;
        doc.mode = WavetableMode::Grid;
        int fA = doc.addLibraryEntry(std::make_unique<LayeredWaveform>(), "A");
        int fB = doc.addLibraryEntry(std::make_unique<LayeredWaveform>(), "B");
        doc.libraryFrameById(fA)->morphChain = {
            { WarpMethod::SoftClip, 0.30f, 0.0f, true } };
        doc.libraryFrameById(fB)->morphChain = {
            { WarpMethod::Wavefold, 0.60f, 0.0f, true },
            { WarpMethod::BendPlus, 0.20f, 0.0f, true } };
        doc.gridDims = { 2 };
        doc.cellWaveformIds = { fA, fB };
        g.findNode(nId)->script = doc.encode();

        WavetableDoc back; back.decode(g.findNode(nId)->script);
        const IWavetableFrame* bA = back.libraryFrameById(fA);
        const IWavetableFrame* bB = back.libraryFrameById(fB);
        r.check(bA && bA->morphChain.size() == 1 &&
                bA->morphChain[0].method == WarpMethod::SoftClip,
                "per-frame morph: frame A keeps its own 1-op chain");
        r.check(bB && bB->morphChain.size() == 2 &&
                bB->morphChain[0].method == WarpMethod::Wavefold &&
                bB->morphChain[1].method == WarpMethod::BendPlus,
                "per-frame morph: frame B keeps its own 2-op chain (independent of A)");

        // Reconcile node params: frame A -> 1 param keyed to fA, frame B -> 2
        // params keyed to fB. None should share a (warpFrameId, warpSlot) key.
        reconcileAllWarpParams(g);
        Node* nd = g.findNode(nId);
        int nA = 0, nB = 0;
        bool prefixedA = false, prefixedB = false;
        for (const auto& p : nd->params) {
            if (p.warpLayer != -1 || p.warpSlot < 0) continue;
            if (p.warpFrameId == fA) { ++nA; if (p.name.rfind("A:", 0) == 0) prefixedA = true; }
            if (p.warpFrameId == fB) { ++nB; if (p.name.rfind("B:", 0) == 0) prefixedB = true; }
        }
        r.checkVal(nA == 1, "per-frame morph: frame A gets exactly one frame-scope param", nA);
        r.checkVal(nB == 2, "per-frame morph: frame B gets exactly two frame-scope params", nB);
        r.check(prefixedA && prefixedB,
                "per-frame morph: params are name-prefixed per frame when >1 frame morphs");
    }

    // ---- Built-in morph presets (curated Type-2 chains) ----------------
    {
        const auto& builtins = builtinMorphChains();
        r.check(!builtins.empty(),
                "morph-presets: at least one built-in chain is registered");

        bool idsOk = true, idsDisjoint = true, opsOk = true, lookupOk = true;
        bool allType2 = true, nonEmptyOps = true;
        std::set<int> seen;
        for (const auto& b : builtins) {
            // Ids in the reserved built-in range, disjoint from the user id space
            // and the Independent sentinel (1).
            if (b.id < kBuiltinMorphIdBase || b.id >= AssetLibrary::kUserIdBase || b.id == 1)
                idsOk = false;
            if (!seen.insert(b.id).second) idsDisjoint = false;
            if (b.ops.empty()) nonEmptyOps = false;
            // Every op must be a real Bucket A (Type-2) method - phase or
            // amplitude domain. A built-in must never carry a Type-1 generator
            // (those aren't in the WarpMethod enum at all) or a Bucket C domain.
            for (const auto& op : b.ops) {
                WarpDomain d = warpDomainOf(op.method);
                if (op.method == WarpMethod::None) opsOk = false;
                if (d != WarpDomain::Phase && d != WarpDomain::Amplitude)
                    allType2 = false;
            }
            // Lookup by id resolves back to the same chain.
            const BuiltinMorphChain* found = builtinMorphChain(b.id);
            if (found != &b) lookupOk = false;
        }
        r.check(idsOk,       "morph-presets: ids sit in the reserved built-in range");
        r.check(idsDisjoint, "morph-presets: built-in ids are unique");
        r.check(nonEmptyOps, "morph-presets: every built-in has at least one op");
        r.check(opsOk,       "morph-presets: no built-in op is None");
        r.check(allType2,    "morph-presets: every built-in op is Type-2 (phase/amplitude)");
        r.check(lookupOk,    "morph-presets: builtinMorphChain(id) resolves each entry");
        // An unknown id resolves to nullptr.
        r.check(builtinMorphChain(424242) == nullptr,
                "morph-presets: unknown id resolves to nullptr");
    }

    // ---- Built-in morph chains are seeded into the library --------------
    {
        // seedBuiltinMorphLibrary populates an empty library with one
        // MorphAlgorithm entry per built-in chain, at its fixed built-in id.
        AssetLibrary lib;
        seedBuiltinMorphLibrary(lib);
        const auto& builtins = builtinMorphChains();
        auto seeded = lib.list(AssetKind::MorphAlgorithm);
        r.check(seeded.size() == builtins.size(),
                "morph-seed: every built-in chain becomes a library entry");

        bool payloadOk = true, idOk = true, kindOk = true, starredOk = true;
        for (const auto& b : builtins) {
            const AssetEntry* e = lib.find(b.id);
            if (!e) { idOk = false; continue; }
            if (e->kind != AssetKind::MorphAlgorithm) kindOk = false;
            if (e->payload != encodeWarpChain(b.ops)) payloadOk = false;
            if (!e->starred) starredOk = false;
            if (!isBuiltinMorphAssetId(e->id)) idOk = false;
        }
        r.check(idOk,       "morph-seed: each built-in id resolves in the library");
        r.check(kindOk,     "morph-seed: seeded entries are MorphAlgorithm");
        r.check(payloadOk,  "morph-seed: seeded payload matches the chain ops");
        r.check(starredOk,  "morph-seed: built-ins are seeded as starred");

        // Idempotent: re-seeding never duplicates, and user ids stay disjoint.
        seedBuiltinMorphLibrary(lib);
        seedBuiltinMorphLibrary(lib);
        r.check(lib.list(AssetKind::MorphAlgorithm).size() == builtins.size(),
                "morph-seed: re-seeding is idempotent (no duplicates)");
        int uid = lib.add(AssetKind::MorphAlgorithm, "user morph", "", "X");
        r.check(uid >= AssetLibrary::kUserIdBase,
                "morph-seed: user-published ids stay in the user space (>= 1e6)");
    }

    // ---- Built-in morph chains are NOT serialized -----------------------
    {
        // A fresh graph seeds built-ins; saving + reloading must keep exactly the
        // built-in set (re-seeded on load), not double them, and must persist
        // user-published morphs alongside.
        NodeGraph g;
        seedBuiltinMorphLibrary(g.assets);
        const size_t nBuiltin = builtinMorphChains().size();
        int uid = g.assets.add(AssetKind::MorphAlgorithm, "keep me", "", "USEROPS");

        std::string saved = ProjectFile::serializeForUndo(g);

        NodeGraph g2;
        bool ld = ProjectFile::loadFromString(saved, g2);
        r.check(ld, "morph-seed: project with seeded built-ins round-trips");

        auto morphs = g2.assets.list(AssetKind::MorphAlgorithm);
        size_t nB = 0, nU = 0;
        const AssetEntry* user = nullptr;
        for (const AssetEntry* e : morphs) {
            if (isBuiltinMorphAssetId(e->id)) ++nB;
            else { ++nU; user = e; }
        }
        r.check(nB == nBuiltin,
                "morph-seed: built-ins re-seed on load (no duplication)");
        r.check(nU == 1 && user && user->id == uid && user->payload == "USEROPS",
                "morph-seed: user-published morph survives save/load");
    }

    // ---- AHDSR per-segment tension: warp properties + round-trip --------
    {
        // tensionWarp() must pin the endpoints, be the identity at 0, and be
        // strictly monotonic across the interior for both signs of tension.
        bool endpointsOk = std::abs(AHDSREnvelope::tensionWarp(0.0f,  0.7f)) < 1e-5f
                        && std::abs(AHDSREnvelope::tensionWarp(1.0f,  0.7f) - 1.0f) < 1e-5f
                        && std::abs(AHDSREnvelope::tensionWarp(0.0f, -0.7f)) < 1e-5f
                        && std::abs(AHDSREnvelope::tensionWarp(1.0f, -0.7f) - 1.0f) < 1e-5f;
        r.check(endpointsOk, "ahdsr tension: warp pins both endpoints for +/-tension");

        bool identityOk = true;
        for (int i = 0; i <= 10; ++i) {
            float t = (float)i / 10.0f;
            if (std::abs(AHDSREnvelope::tensionWarp(t, 0.0f) - t) > 1e-5f) identityOk = false;
        }
        r.check(identityOk, "ahdsr tension: tension 0 is the identity warp");

        auto monotonic = [](float tension) {
            float prev = -1.0f;
            for (int i = 0; i <= 64; ++i) {
                float w = AHDSREnvelope::tensionWarp((float)i / 64.0f, tension);
                if (w < prev - 1e-6f) return false;
                prev = w;
            }
            return true;
        };
        r.check(monotonic(0.9f) && monotonic(-0.9f),
                "ahdsr tension: warp is monotonic for strong +/-tension");

        // T>0 ("slow start") must lag below the diagonal in the interior;
        // T<0 ("fast start") must lead above it.
        bool slowStart = AHDSREnvelope::tensionWarp(0.5f,  0.9f) < 0.5f;
        bool fastStart = AHDSREnvelope::tensionWarp(0.5f, -0.9f) > 0.5f;
        r.check(slowStart && fastStart,
                "ahdsr tension: +tension lags, -tension leads at the midpoint");

        // bakeSegment with tension 0 must equal the raw curve evaluation.
        AHDSREnvelope env0;
        auto raw  = env0.attackCurve.evaluate(64);
        auto bake = AHDSREnvelope::bakeSegment(env0.attackCurve, 0.0f, 64);
        bool bakeMatch = raw.size() == bake.size();
        if (bakeMatch)
            for (size_t i = 0; i < raw.size(); ++i)
                if (std::abs(raw[i] - bake[i]) > 1e-5f) bakeMatch = false;
        r.check(bakeMatch, "ahdsr tension: bakeSegment at 0 equals curve.evaluate");

        // Encode -> decode must carry the three tension fields, and old
        // payloads without them must decode to tension 0 (back-compat).
        AHDSREnvelope env;
        env.attackTension  = 0.55f;
        env.decayTension   = -0.30f;
        env.releaseTension =  0.80f;
        std::string enc = env.encode();
        AHDSREnvelope back;
        bool decOk = AHDSREnvelope::decode(enc, back);
        bool tenMatch = decOk
            && std::abs(back.attackTension  - 0.55f) < 1e-4f
            && std::abs(back.decayTension   + 0.30f) < 1e-4f
            && std::abs(back.releaseTension - 0.80f) < 1e-4f;
        r.check(tenMatch, "ahdsr tension: at/dt/rt survive an encode->decode round trip");

        // A payload with the tension fields stripped (simulating an old file)
        // must still decode, leaving tensions at their 0 default.
        std::string legacy = enc;
        for (const char* key : { "at=", "dt=", "rt=" }) {
            size_t p = legacy.find(key);
            if (p != std::string::npos) {
                size_t e = legacy.find(';', p);
                if (e != std::string::npos) legacy.erase(p, e - p + 1);
            }
        }
        AHDSREnvelope legacyBack;
        bool legOk = AHDSREnvelope::decode(legacy, legacyBack);
        bool legZero = legOk
            && std::abs(legacyBack.attackTension)  < 1e-6f
            && std::abs(legacyBack.decayTension)   < 1e-6f
            && std::abs(legacyBack.releaseTension) < 1e-6f;
        r.check(legZero, "ahdsr tension: payload without at/dt/rt decodes to tension 0");
    }

    // ---- Per-layer (element-scope) warp: serialize + render ------------
    {
        LayeredWaveform lw;
        lw.tableSize = 256;
        WaveLayer base;       // clean sine fundamental
        base.shape = WaveLayer::Sine; base.ratio = 1; base.amp = 1.0f;
        WaveLayer folded;     // wavefolded sine
        folded.shape = WaveLayer::Sine; folded.ratio = 1; folded.amp = 1.0f;
        folded.warpChain = { { WarpMethod::Wavefold, 0.8f, 0.0f, true } };
        lw.layers = { folded };

        std::string enc = lw.encode();
        r.check(enc.find("warp=") != std::string::npos,
                "warp: layer encode appends a warp= field");

        LayeredWaveform back;
        bool ok = back.decode(enc);
        bool layerOk = ok && back.layers.size() == 1
                    && back.layers[0].warpChain.size() == 1
                    && back.layers[0].warpChain[0].method == WarpMethod::Wavefold
                    && std::abs(back.layers[0].warpChain[0].amount - 0.8f) < 1e-4f;
        r.check(layerOk, "warp: per-layer warp survives encode->decode");

        // The warped render must differ from the clean sine render (the fold
        // injects harmonics), proving the chain is actually applied.
        std::vector<float> cleanOut, foldedOut;
        LayeredWaveform clean; clean.tableSize = 256; clean.layers = { base };
        clean.render(cleanOut);
        lw.render(foldedOut);
        double diff = 0.0;
        int n = (int)std::min(cleanOut.size(), foldedOut.size());
        for (int i = 0; i < n; ++i) diff += std::abs(cleanOut[i] - foldedOut[i]);
        r.checkVal(diff > 1.0, "warp: per-layer Wavefold changes the rendered cycle", diff);

        // A layer with NO warp must encode without the field (back-compat).
        LayeredWaveform plain; plain.tableSize = 256; plain.layers = { base };
        r.check(plain.encode().find("warp=") == std::string::npos,
                "warp: un-warped layer omits the warp= field");

        // renderWithLiveWarp (inc 4): the block-rate re-bake path the synth uses
        // when a per-layer warp op is opted into live modulation.
        // (1) Empty overrides -> byte-for-byte identical to render() (held value).
        std::vector<float> bakedOut, liveOut;
        lw.render(bakedOut);
        lw.renderWithLiveWarp({}, liveOut);
        bool identical = bakedOut.size() == liveOut.size();
        for (size_t i = 0; identical && i < bakedOut.size(); ++i)
            identical = bakedOut[i] == liveOut[i];
        r.check(identical,
                "warp: renderWithLiveWarp({}) == render() (held value identity)");

        // (2) An override matching the baked amount (0.8) reproduces the baked
        //     cycle exactly - the substitution itself introduces no drift.
        std::vector<float> heldOut;
        lw.renderWithLiveWarp({ { 0.8f } }, heldOut);
        bool heldIdentical = heldOut.size() == bakedOut.size();
        for (size_t i = 0; heldIdentical && i < heldOut.size(); ++i)
            heldIdentical = std::abs(heldOut[i] - bakedOut[i]) < 1e-6f;
        r.check(heldIdentical,
                "warp: live override == baked amount reproduces the baked cycle");

        // (3) A different override amount (0.2 vs baked 0.8) actually changes the
        //     cycle, proving the live amount feeds applyWarpChain.
        std::vector<float> modOut;
        lw.renderWithLiveWarp({ { 0.2f } }, modOut);
        double modDiff = 0.0;
        int mn = (int)std::min(modOut.size(), bakedOut.size());
        for (int i = 0; i < mn; ++i) modDiff += std::abs(modOut[i] - bakedOut[i]);
        r.checkVal(modDiff > 1.0,
                   "warp: live override amount changes the re-baked cycle", modDiff);

        // (4) A negative sentinel in the override keeps the baked amount (so an
        //     un-modulated op in a partially-modulated layer is untouched).
        std::vector<float> sentinelOut;
        lw.renderWithLiveWarp({ { -1.0f } }, sentinelOut);
        bool sentinelHeld = sentinelOut.size() == bakedOut.size();
        for (size_t i = 0; sentinelHeld && i < sentinelOut.size(); ++i)
            sentinelHeld = std::abs(sentinelOut[i] - bakedOut[i]) < 1e-6f;
        r.check(sentinelHeld,
                "warp: negative override sentinel keeps the baked amount");
    }

    // ---- Per-layer Phase / Amplitude live modulation (#88, item-M) ---------
    // renderWithLiveOverrides drives a layer's Phase/Amp from a live (modulated)
    // value, with a NaN sentinel meaning "keep the layer's stored value".
    {
        WaveLayer a; a.shape = WaveLayer::Sine; a.ratio = 1; a.amp = 1.0f; a.phase = 0.0f;
        WaveLayer b; b.shape = WaveLayer::Sine; b.ratio = 2; b.amp = 0.5f; b.phase = 0.0f;
        LayeredWaveform lw; lw.tableSize = 256; lw.layers = { a, b };

        const float kNaN = std::numeric_limits<float>::quiet_NaN();
        std::vector<float> baked; lw.render(baked);

        // (1) All-NaN phase/amp overrides reproduce the baked cycle exactly.
        std::vector<float> heldOut;
        lw.renderWithLiveOverrides({}, { kNaN, kNaN }, { kNaN, kNaN }, {}, {}, heldOut);
        bool held = heldOut.size() == baked.size();
        for (size_t i = 0; held && i < heldOut.size(); ++i)
            held = std::abs(heldOut[i] - baked[i]) < 1e-6f;
        r.check(held, "layerfield: NaN phase/amp overrides reproduce the baked cycle");

        // (2) An amp override that matches the stored amp reproduces the cycle;
        //     a different amp changes it (proving amp override feeds the mix).
        std::vector<float> ampHeld, ampMod;
        lw.renderWithLiveOverrides({}, { kNaN, kNaN }, { kNaN, 0.5f }, {}, {}, ampHeld);
        bool ampSame = ampHeld.size() == baked.size();
        for (size_t i = 0; ampSame && i < ampHeld.size(); ++i)
            ampSame = std::abs(ampHeld[i] - baked[i]) < 1e-6f;
        r.check(ampSame, "layerfield: amp override == stored amp reproduces the cycle");
        lw.renderWithLiveOverrides({}, { kNaN, kNaN }, { kNaN, 0.0f }, {}, {}, ampMod);
        double ampDiff = 0.0;
        int an = (int)std::min(ampMod.size(), baked.size());
        for (int i = 0; i < an; ++i) ampDiff += std::abs(ampMod[i] - baked[i]);
        r.checkVal(ampDiff > 1.0, "layerfield: amp override changes the re-baked cycle", ampDiff);

        // (3) A phase override on layer 0 changes the summed cycle (the two
        //     layers add up differently once the phase relationship shifts).
        std::vector<float> phaseMod;
        lw.renderWithLiveOverrides({}, { 0.25f, kNaN }, { kNaN, kNaN }, {}, {}, phaseMod);
        double phaseDiff = 0.0;
        int pn = (int)std::min(phaseMod.size(), baked.size());
        for (int i = 0; i < pn; ++i) phaseDiff += std::abs(phaseMod[i] - baked[i]);
        r.checkVal(phaseDiff > 1.0, "layerfield: phase override changes the re-baked cycle", phaseDiff);

        // (3b) A generator-parameter override (field 2 = shapeParam) changes a
        //      Pulse layer's cycle, proving duty/amount/index is modulatable like
        //      phase/amp. A matching override reproduces it; a different one moves
        //      it. field 3 (shapeParam2) is exercised for FM below.
        {
            WaveLayer pg; pg.shape = WaveLayer::Pulse; pg.ratio = 1; pg.amp = 1.0f;
            pg.shapeParam = 0.5f;  // square
            LayeredWaveform plw; plw.tableSize = 256; plw.layers = { pg };
            std::vector<float> pBaked; plw.render(pBaked);
            std::vector<float> pHeld, pMod;
            plw.renderWithLiveOverrides({}, {}, {}, { 0.5f }, {}, pHeld);
            bool pSame = pHeld.size() == pBaked.size();
            for (size_t i = 0; pSame && i < pHeld.size(); ++i)
                pSame = std::abs(pHeld[i] - pBaked[i]) < 1e-6f;
            r.check(pSame, "layerfield: shapeParam override == stored reproduces the cycle");
            plw.renderWithLiveOverrides({}, {}, {}, { 0.1f }, {}, pMod);
            double spDiff = 0.0;
            int spn = (int)std::min(pMod.size(), pBaked.size());
            for (int i = 0; i < spn; ++i) spDiff += std::abs(pMod[i] - pBaked[i]);
            r.checkVal(spDiff > 1.0, "layerfield: shapeParam override changes the re-baked cycle", spDiff);

            // (3c) FM ratio (field 3 = shapeParam2) modulation changes an FM layer.
            WaveLayer fg; fg.shape = WaveLayer::FM; fg.ratio = 1; fg.amp = 1.0f;
            fg.shapeParam = 0.5f; fg.shapeParam2 = 0.15f;
            LayeredWaveform flw; flw.tableSize = 256; flw.layers = { fg };
            std::vector<float> fBaked; flw.render(fBaked);
            std::vector<float> fMod;
            flw.renderWithLiveOverrides({}, {}, {}, {}, { 0.8f }, fMod);
            double f2Diff = 0.0;
            int fn = (int)std::min(fMod.size(), fBaked.size());
            for (int i = 0; i < fn; ++i) f2Diff += std::abs(fMod[i] - fBaked[i]);
            r.checkVal(f2Diff > 1.0, "layerfield: shapeParam2 (FM ratio) override changes the cycle", f2Diff);
        }

        // (4) renderWithLiveWarp delegates here with empty phase/amp -> identical
        //     to render() (the back-compat path the synth's warp-only loop uses).
        std::vector<float> warpDelegate;
        lw.renderWithLiveWarp({}, warpDelegate);
        bool delegateSame = warpDelegate.size() == baked.size();
        for (size_t i = 0; delegateSame && i < warpDelegate.size(); ++i)
            delegateSame = warpDelegate[i] == baked[i];
        r.check(delegateSame, "layerfield: renderWithLiveWarp delegates to render() identity");
    }

    // ---- Per-layer field Param key round-trips save/load --------------------
    {
        NodeGraph g;
        int nId = g.addNode("wt", NodeType::TerrainSynth, {}, {}).id;
        Param p;
        p.name = "Layer 1 Phase"; p.warpLayer = 0; p.warpSlot = -1; p.layerField = 0;
        p.value = p.baseValue = 0.3f; p.minVal = 0.0f; p.maxVal = 1.0f;
        g.findNode(nId)->params.push_back(p);
        std::string saved = ProjectFile::serializeForUndo(g);
        NodeGraph g2;
        bool ld = ProjectFile::loadFromString(saved, g2);
        Node* nd2 = g2.findNode(nId);
        const Param* fp = nullptr;
        if (nd2)
            for (const auto& q : nd2->params)
                if (q.layerField == 0) { fp = &q; break; }
        bool fieldOk = ld && fp
                    && fp->warpLayer == 0
                    && fp->warpSlot == -1
                    && std::abs(fp->value - 0.3f) < 1e-4f;
        r.check(fieldOk, "layerfield: Param layerField key survives save/load");
    }

    // ---- Bucket C: spectral (per-bin) element warp ---------------------
    {
        SpectralDoc doc = SpectralDoc::defaultBuiltin();
        doc.warpChain = { { WarpMethod::Wavefold, 0.7f, 0.0f, true } };
        std::string enc = doc.encode();
        r.check(enc.find("warp:") != std::string::npos,
                "warp: spectral encode appends a warp section when non-empty");

        SpectralDoc back;
        bool ok = back.decode(enc);
        bool match = ok && back.warpChain.size() == 1
                  && back.warpChain[0].method == WarpMethod::Wavefold
                  && std::abs(back.warpChain[0].amount - 0.7f) < 1e-4f;
        r.check(match, "warp: spectral per-bin warp survives encode->decode");

        // Render with vs without the warp must differ (the warp reshapes the
        // magnitude spectrum before the IFFT).
        std::vector<float> warped, clean;
        renderSpectralToWaveform(doc, 512, warped);
        SpectralDoc plain = doc; plain.warpChain.clear();
        renderSpectralToWaveform(plain, 512, clean);
        double sdiff = 0.0;
        int sn = (int)std::min(warped.size(), clean.size());
        for (int i = 0; i < sn; ++i) sdiff += std::abs(warped[i] - clean[i]);
        r.checkVal(sdiff > 1e-3, "warp: spectral warp changes the rendered cycle", sdiff);

        // Empty chain omits the section (byte-compatible with old files).
        r.check(plain.encode().find("warp:") == std::string::npos,
                "warp: un-warped spectral doc omits the warp section");
    }

    // ---- Bucket C: wavelet (per-coefficient) element warp --------------
    {
        WaveletFrame f = WaveletFrame::defaultEmpty();
        // Seed a couple of coefficients so the IDWT produces a non-trivial cycle.
        if (f.coefficients.size() > 8) {
            f.coefficients[2] = 0.6f;
            f.coefficients[5] = -0.4f;
        }
        f.warpChain = { { WarpMethod::SoftClip, 0.8f, 0.0f, true } };
        std::string body = f.encodeBody();
        r.check(body.find(":warp:") != std::string::npos,
                "warp: wavelet encode appends a :warp: section when non-empty");

        WaveletFrame back;
        bool ok = back.decodeBody(body);
        bool match = ok && back.warpChain.size() == 1
                  && back.warpChain[0].method == WarpMethod::SoftClip
                  && std::abs(back.warpChain[0].amount - 0.8f) < 1e-4f;
        r.check(match, "warp: wavelet per-coeff warp survives encode->decode");

        std::vector<float> warped, clean;
        f.renderRaw(256, warped);
        WaveletFrame plain = f; plain.warpChain.clear();
        plain.renderRaw(256, clean);
        double wdiff = 0.0;
        int wn = (int)std::min(warped.size(), clean.size());
        for (int i = 0; i < wn; ++i) wdiff += std::abs(warped[i] - clean[i]);
        r.checkVal(wdiff > 1e-3, "warp: wavelet warp changes the rendered cycle", wdiff);

        r.check(plain.encodeBody().find(":warp:") == std::string::npos,
                "warp: un-warped wavelet frame omits the :warp: section");
    }

    // ---- Bucket C: granular (per-grain) element warp -------------------
    {
        GranularFrame f = GranularFrame::defaultEmpty();   // 1 s A4 sine
        // Granular warp is amplitude-domain only; a phase-domain op must be
        // dropped by warpAmpOps() (it has no meaning on a grain stream).
        f.warpChain = {
            { WarpMethod::Wavefold, 0.8f, 0.0f, true },   // amplitude - kept
            { WarpMethod::BendPlus, 0.5f, 0.0f, true },   // phase - dropped
        };
        auto amp = f.warpAmpOps();
        r.check(amp.size() == 1 && amp[0].method == WarpMethod::Wavefold,
                "warp: granular warpAmpOps keeps only amplitude-domain ops");

        std::string body = f.encodeBody();
        r.check(body.find(";warp:") != std::string::npos,
                "warp: granular encode appends a ;warp: section when non-empty");

        GranularFrame back;
        bool ok = back.decodeBody(body);
        // Round-trips the FULL chain (both ops); the amplitude filter only runs
        // at apply time, not at serialize time.
        bool match = ok && back.warpChain.size() == 2
                  && back.warpChain[0].method == WarpMethod::Wavefold
                  && back.warpChain[1].method == WarpMethod::BendPlus
                  && (int)back.source.size() == (int)f.source.size();
        r.check(match, "warp: granular per-grain warp survives encode->decode");

        // renderRaw bakes the amplitude warp into the representative cycle.
        std::vector<float> warped, clean;
        f.renderRaw(256, warped);
        GranularFrame plain = f; plain.warpChain.clear();
        plain.renderRaw(256, clean);
        double gdiff = 0.0;
        int gn = (int)std::min(warped.size(), clean.size());
        for (int i = 0; i < gn; ++i) gdiff += std::abs(warped[i] - clean[i]);
        r.checkVal(gdiff > 1e-3, "warp: granular warp changes the representative cycle", gdiff);

        r.check(plain.encodeBody().find(";warp:") == std::string::npos,
                "warp: un-warped granular frame omits the ;warp: section");
    }

    // ---- Bucket B: generator-morph layer shapes (Pulse/Sync/FM/PD) -----
    {
        // Each generator must produce a non-trivial, in-range, finite cycle and
        // the morph parameter must actually change the rendered cycle.
        auto renderShape = [](WaveLayer::Shape s, float p1, float p2,
                              std::vector<float>& out) {
            LayeredWaveform lw; lw.tableSize = 512;
            WaveLayer l; l.shape = s; l.ratio = 1; l.amp = 1.0f;
            l.shapeParam = p1; l.shapeParam2 = p2;
            lw.layers = { l };
            lw.render(out);
        };
        struct GenCase { WaveLayer::Shape shape; const char* name; float a; float b; };
        GenCase cases[] = {
            { WaveLayer::Pulse,     "pulse",     0.25f, 0.75f },
            { WaveLayer::Sync,      "sync",      0.3f,  0.7f  },
            { WaveLayer::FM,        "fm",        0.4f,  0.6f  },
            { WaveLayer::PhaseDist, "phasedist", 0.3f,  0.8f  },
        };
        for (const auto& c : cases) {
            std::vector<float> a, b;
            renderShape(c.shape, c.a, 0.5f, a);
            renderShape(c.shape, c.b, 0.5f, b);
            // Finite + in range.
            bool finite = true, inRange = true;
            for (float v : a) {
                if (!std::isfinite(v)) finite = false;
                if (std::abs(v) > 1.0001f) inRange = false;
            }
            r.check(finite && inRange && !a.empty(),
                    std::string("warp: ") + c.name + " renders a finite in-range cycle");
            // The morph parameter changes the cycle.
            double mdiff = 0.0;
            int mn = (int)std::min(a.size(), b.size());
            for (int i = 0; i < mn; ++i) mdiff += std::abs(a[i] - b[i]);
            r.checkVal(mdiff > 1e-3,
                       std::string("warp: ") + c.name + " morph parameter changes the cycle", mdiff);
        }

        // FM's modulator:carrier ratio (shapeParam2) must also affect the cycle.
        {
            std::vector<float> a, b;
            renderShape(WaveLayer::FM, 0.6f, 0.1f, a);
            renderShape(WaveLayer::FM, 0.6f, 0.9f, b);
            double mdiff = 0.0;
            int mn = (int)std::min(a.size(), b.size());
            for (int i = 0; i < mn; ++i) mdiff += std::abs(a[i] - b[i]);
            r.checkVal(mdiff > 1e-3, "warp: FM ratio (shapeParam2) changes the cycle", mdiff);
        }

        // Serialization: shapeParam/shapeParam2 round-trip via the p1=/p2= fields.
        {
            LayeredWaveform lw; lw.tableSize = 256;
            WaveLayer fm; fm.shape = WaveLayer::FM; fm.shapeParam = 0.37f; fm.shapeParam2 = 0.81f;
            WaveLayer pw; pw.shape = WaveLayer::Pulse; pw.shapeParam = 0.22f;
            lw.layers = { fm, pw };
            std::string enc = lw.encode();
            r.check(enc.find("p1=") != std::string::npos && enc.find("p2=") != std::string::npos,
                    "warp: generator-morph layer encodes p1=/p2= fields");
            LayeredWaveform back;
            bool ok = back.decode(enc);
            bool match = ok && back.layers.size() == 2
                      && back.layers[0].shape == WaveLayer::FM
                      && std::abs(back.layers[0].shapeParam  - 0.37f) < 1e-3f
                      && std::abs(back.layers[0].shapeParam2 - 0.81f) < 1e-3f
                      && back.layers[1].shape == WaveLayer::Pulse
                      && std::abs(back.layers[1].shapeParam  - 0.22f) < 1e-3f;
            r.check(match, "warp: generator-morph params survive encode->decode");

            // A classic-shape layer must NOT emit p1=/p2= (byte-compat).
            LayeredWaveform classic; classic.tableSize = 256;
            WaveLayer sine; sine.shape = WaveLayer::Sine;
            classic.layers = { sine };
            r.check(classic.encode().find("p1=") == std::string::npos,
                    "warp: classic-shape layer omits the p1= field");
        }
    }

    // ---- Formula bake embedding (#crash-python314) ----------------------
    // A Lua/Python/GLSL Formula layer must carry its baked one-cycle inside the
    // encoded script so the audio thread can render it WITHOUT re-running an
    // interpreter (which crashes deep in python3xx.dll when forced onto the
    // audio thread). Old projects with no embedded bake must be detectable
    // (decodedNeedsBakeEmbed) and re-encodable on the message thread.
    {
        // Build a non-Built-in Formula layer with a known baked cycle. We set
        // formulaSamples directly rather than running an interpreter so the test
        // is independent of which script languages this build links.
        WaveLayer fl;
        fl.shape = WaveLayer::Formula;
        fl.formulaLang = ShapeLang::Python;
        fl.formulaExpr = "sin(x)";
        fl.formulaSamples.resize(2048);
        const double kTwoPi = 6.283185307179586;
        for (int i = 0; i < 2048; ++i)
            fl.formulaSamples[i] = (float)(std::sin(kTwoPi * i / 2048.0) * 0.5);

        LayeredWaveform lw; lw.tableSize = 256; lw.layers = { fl };
        std::string enc = lw.encode();
        r.check(enc.find(",bake=") != std::string::npos,
                "bake: non-Built-in Formula layer embeds a bake= field");

        // Round-trip: the embedded samples come back verbatim, and decode does
        // NOT flag the script as needing migration.
        LayeredWaveform back;
        bool ok = back.decode(enc);
        bool sizeOk = ok && back.layers.size() == 1
                   && back.layers[0].formulaSamples.size() == 2048;
        double sdiff = 0.0;
        if (sizeOk)
            for (int i = 0; i < 2048; ++i)
                sdiff += std::abs(back.layers[0].formulaSamples[i] - fl.formulaSamples[i]);
        r.checkVal(sizeOk && sdiff < 1e-2 && !back.decodedNeedsBakeEmbed,
                   "bake: embedded cycle round-trips and needs no migration", sdiff);

        // A Built-in Formula layer must NOT embed a bake= field (it evaluates
        // live in C++), keeping classic saves compact.
        WaveLayer bi; bi.shape = WaveLayer::Formula; bi.formulaLang = ShapeLang::Builtin;
        bi.formulaExpr = "sin(x)";
        LayeredWaveform lwb; lwb.tableSize = 256; lwb.layers = { bi };
        r.check(lwb.encode().find(",bake=") == std::string::npos,
                "bake: Built-in Formula layer omits the bake= field");

        // Simulate an OLD project: strip the bake= field from the encoded
        // script. decode must flag it for migration, and the migration helper
        // must re-embed a bake= field (re-baking on this thread).
        std::string old = enc;
        size_t bpos = old.find(",bake=");
        if (bpos != std::string::npos) {
            size_t end = old.find('|', bpos);          // bake= is the last field
            if (end == std::string::npos) end = old.size();
            old.erase(bpos, end - bpos);
        }
        LayeredWaveform oldDec;
        oldDec.decode(old);
        r.check(oldDec.decodedNeedsBakeEmbed,
                "bake: un-embedded old script is flagged as needing migration");

        std::string migrated = old;
        bool changed = migrateLayeredScriptEmbedBake(migrated);
        r.check(changed && migrated.find(",bake=") != std::string::npos,
                "bake: migration re-embeds a bake= field on the message thread");

        // A script that already has the embed must be a no-op for the migrator.
        std::string already = enc;
        r.check(!migrateLayeredScriptEmbedBake(already) && already == enc,
                "bake: migration is a no-op when the cycle is already embedded");
    }

    // ---- Milestone 9: inharmonic additive stack ------------------------
    {
        // The default bell renders a finite, in-range, non-trivial cycle.
        InharmonicFrame bell = InharmonicFrame::defaultBell();
        r.check(bell.partials.size() == 5,
                "inharmonic: defaultBell has 5 partials");
        std::vector<float> cyc;
        bell.renderRaw(512, cyc);
        bool finite = true, inRange = true, nonTrivial = false;
        float peak = 0.0f;
        for (float v : cyc) {
            if (!std::isfinite(v)) finite = false;
            if (std::abs(v) > 1.0001f) inRange = false;
            if (std::abs(v) > 1e-3f) nonTrivial = true;
            peak = std::max(peak, std::abs(v));
        }
        r.check(finite && inRange && nonTrivial && !cyc.empty(),
                "inharmonic: defaultBell renders a finite in-range non-trivial cycle");
        // renderRaw is peak-normalised to ~1.0 (the live-voice loudness anchor).
        r.checkVal(std::abs(peak - 1.0f) < 1e-3,
                   "inharmonic: renderRaw peak-normalises to 1.0", peak);
        // normGainFor agrees with that normalisation: scaling the raw sum by it
        // yields peak ~1.0.
        float ng = InharmonicFrame::normGainFor(bell.partials, 512);
        r.checkVal(ng > 0.0f && std::isfinite(ng),
                   "inharmonic: normGainFor returns a finite positive gain", ng);

        // Partials + amplitude warp round-trip through encode->decode.
        InharmonicFrame f;
        f.partials = { {1.0f, 1.0f, 0.0f}, {2.76f, 0.6f, 0.1f}, {5.4f, 0.4f, 0.25f} };
        f.warpChain = {
            { WarpMethod::Wavefold, 0.7f, 0.0f, true },   // amplitude - kept
            { WarpMethod::BendPlus, 0.5f, 0.0f, true },   // phase - dropped at apply
        };
        // warpAmpOps drops the phase-domain op (no meaning on an additive stream).
        auto amp = f.warpAmpOps();
        r.check(amp.size() == 1 && amp[0].method == WarpMethod::Wavefold,
                "inharmonic: warpAmpOps keeps only amplitude-domain ops");

        std::string body = f.encodeBody();
        r.check(body.find(";warp:") != std::string::npos,
                "inharmonic: encode appends a ;warp: section when non-empty");
        InharmonicFrame back;
        bool ok = back.decodeBody(body);
        bool match = ok && back.partials.size() == 3
                  && std::abs(back.partials[1].ratio - 2.76f) < 1e-3f
                  && std::abs(back.partials[1].amp   - 0.6f)  < 1e-3f
                  && std::abs(back.partials[2].phase - 0.25f) < 1e-3f
                  && back.warpChain.size() == 2
                  && back.warpChain[0].method == WarpMethod::Wavefold
                  && back.warpChain[1].method == WarpMethod::BendPlus;
        r.check(match, "inharmonic: partials + warp survive encode->decode");

        // The amplitude warp changes the representative cycle.
        std::vector<float> warped, clean;
        f.renderRaw(256, warped);
        InharmonicFrame plain = f; plain.warpChain.clear();
        plain.renderRaw(256, clean);
        double diff = 0.0;
        int n = (int)std::min(warped.size(), clean.size());
        for (int i = 0; i < n; ++i) diff += std::abs(warped[i] - clean[i]);
        r.checkVal(diff > 1e-3, "inharmonic: amplitude warp changes the cycle", diff);

        // An un-warped frame omits the ;warp: section (byte-clean round-trip).
        r.check(plain.encodeBody().find(";warp:") == std::string::npos,
                "inharmonic: un-warped frame omits the ;warp: section");
    }

    // ---- Real-time-safe wavelet transform (shared WaveletWorkspace) ------
    // The wavelet effect nodes run dwt()/idwt() on the audio thread. They used
    // to allocate scratch inside the transform (two vectors per level per
    // channel per block, plus a freshly-built filter bank) - a hard real-time
    // violation. The scratch now lives in a caller-owned WaveletWorkspace that
    // is sized once in prepareToPlay and reused across blocks.
    //
    // The risk that introduces is stale scratch: the workspace is generally
    // LARGER than the current transform needs and still holds the previous
    // block's data, so any read of an unwritten scratch slot would silently
    // leak old audio into the new block. These tests pin that down.
    {
        auto makeChirp = [](int n, float k) {
            std::vector<float> b((size_t)n);
            for (int i = 0; i < n; ++i) {
                float t = (float)i / (float)n;
                b[(size_t)i] = std::sin(6.28318530718f * k * t * t) * (0.3f + 0.7f * t);
            }
            return b;
        };
        auto maxDiff = [](const std::vector<float>& a, const std::vector<float>& b) {
            double d = 0.0;
            size_t n = std::min(a.size(), b.size());
            for (size_t i = 0; i < n; ++i) d = std::max(d, (double)std::abs(a[i] - b[i]));
            return d;
        };

        const auto filt = getWaveletFilter("sym4");

        // 1. A fresh workspace and the allocating convenience overload must
        //    agree exactly - the overload just wraps a local workspace, so any
        //    difference means the two paths have drifted apart.
        {
            std::vector<float> a = makeChirp(256, 9.0f), b = a;
            WaveletWorkspace ws;
            int la = dwt(a, 4, filt, ws);
            int lb = dwt(b, 4, filt);
            r.check(la == lb, "wavelet-ws: workspace dwt reports the same level count");
            r.checkVal(maxDiff(a, b) == 0.0,
                       "wavelet-ws: workspace dwt is bit-identical to the allocating overload",
                       maxDiff(a, b));
            idwt(a, la, filt, ws);
            idwt(b, lb, filt);
            r.checkVal(maxDiff(a, b) == 0.0,
                       "wavelet-ws: workspace idwt is bit-identical to the allocating overload",
                       maxDiff(a, b));
        }

        // 2. A workspace pre-sized far larger than the signal, and already
        //    dirtied by a previous transform, must give the same answer as a
        //    virgin one. This is the stale-scratch regression guard.
        {
            std::vector<float> ref = makeChirp(128, 5.0f), reuse = ref;

            WaveletWorkspace fresh;
            int lref = dwt(ref, 5, filt, fresh);
            idwt(ref, lref, filt, fresh);

            WaveletWorkspace dirty;
            dirty.ensure(8192);                       // much bigger than needed
            std::vector<float> junk = makeChirp(4096, 40.0f);
            dwt(junk, 6, filt, dirty);                // leave real data in the scratch
            int lre = dwt(reuse, 5, filt, dirty);
            idwt(reuse, lre, filt, dirty);

            r.check(lref == lre, "wavelet-ws: oversized dirty workspace gives the same level count");
            r.checkVal(maxDiff(ref, reuse) == 0.0,
                       "wavelet-ws: oversized dirty workspace leaks nothing into the result",
                       maxDiff(ref, reuse));
        }

        // 3. The two synthesis conventions, pinned.
        //
        //    idwtPR is the true adjoint of the analysis, so it reconstructs
        //    exactly. idwt is the legacy painter convention, which does NOT -
        //    it loses well over half the energy. Every wavelet EFFECT node runs
        //    "analyse real audio -> tweak coefficients -> resynthesise" and so
        //    must use the PR inverse; using the legacy one made even a neutral
        //    setting badly colour the signal. The legacy pair stays for the
        //    wavelet painter / fractal terrain, which author coefficients by
        //    hand and depend on its exact sound.
        {
            std::vector<float> x = makeChirp(512, 13.0f);

            std::vector<float> pr = x;
            WaveletWorkspace ws;
            int l = dwt(pr, 4, filt, ws);
            idwtPR(pr, l, filt, ws);
            r.checkVal(maxDiff(x, pr) < 1e-4,
                       "wavelet-pr: idwtPR(dwt(x)) reconstructs x exactly",
                       maxDiff(x, pr));

            std::vector<float> legacy = x;
            int l2 = dwt(legacy, 4, filt, ws);
            idwt(legacy, l2, filt, ws);
            double ex = 0, ey = 0;
            for (size_t i = 0; i < x.size(); ++i) {
                ex += (double)x[i] * x[i];
                ey += (double)legacy[i] * legacy[i];
            }
            r.checkVal(ey / ex < 0.75,
                       "wavelet-pr: the legacy inverse is lossy (documented, frozen)",
                       ey / ex);
        }

        // 4. WaveletFxScratch::load zero-pads to a power of two, keeps an
        //    untouched dry copy, and stays correct when the block size shrinks
        //    (the buffers are reused, so a shorter block must not expose the
        //    tail of the longer one).
        {
            WaveletFxScratch s;
            s.prepare(512);
            std::vector<float> big = makeChirp(400, 7.0f);
            int pad = s.load(big.data(), 400);
            r.check(pad == 512, "wavelet-scratch: load pads 400 samples up to 512");
            r.check((int)s.sig.size() == 512 && (int)s.dry.size() == 400,
                    "wavelet-scratch: sig is the padded length, dry is the raw length");
            bool tailZero = true;
            for (int i = 400; i < 512; ++i) if (s.sig[(size_t)i] != 0.0f) tailZero = false;
            r.check(tailZero, "wavelet-scratch: pad region is zeroed");

            std::vector<float> small = makeChirp(100, 3.0f);
            int pad2 = s.load(small.data(), 100);
            r.check(pad2 == 128, "wavelet-scratch: a shorter block re-pads to 128");
            r.check((int)s.sig.size() == 128 && (int)s.dry.size() == 100,
                    "wavelet-scratch: buffers shrink to the new block, no stale tail");
            bool tail2Zero = true;
            for (int i = 100; i < 128; ++i) if (s.sig[(size_t)i] != 0.0f) tail2Zero = false;
            r.check(tail2Zero, "wavelet-scratch: pad region is re-zeroed after a shrink");
            bool dryMatches = true;
            for (int i = 0; i < 100; ++i)
                if (s.dry[(size_t)i] != small[(size_t)i]) dryMatches = false;
            r.check(dryMatches, "wavelet-scratch: dry copy matches the input exactly");
        }

        // 5. The whole point: after prepare(), repeated blocks must not
        //    reallocate. Capacity is the observable proxy - if any buffer grows
        //    its capacity during steady-state processing, something in the path
        //    is still allocating on the audio thread.
        {
            WaveletFxScratch s;
            s.prepare(512);
            std::vector<float> block = makeChirp(512, 11.0f);
            s.load(block.data(), 512);               // first block sizes everything
            const size_t capSig    = s.sig.capacity();
            const size_t capDry    = s.dry.capacity();
            const size_t capApprox = s.ws.approx.capacity();
            const size_t capDetail = s.ws.detail.capacity();
            const size_t capRecon  = s.ws.recon.capacity();
            for (int b = 0; b < 32; ++b) {
                s.useFilter("sym4");
                s.load(block.data(), 512);
                int l = dwt(s.sig, 5, filt, s.ws);
                idwt(s.sig, l, filt, s.ws);
            }
            r.check(s.sig.capacity() == capSig && s.dry.capacity() == capDry,
                    "wavelet-scratch: signal/dry buffers never reallocate in steady state");
            r.check(s.ws.approx.capacity() == capApprox
                    && s.ws.detail.capacity() == capDetail
                    && s.ws.recon.capacity() == capRecon,
                    "wavelet-scratch: transform scratch never reallocates in steady state");
        }

        // 6. The symptom that made the wrong inverse worth chasing: a wavelet
        //    effect at neutral settings must pass audio through UNCHANGED.
        //    One case per effect below.
        {
            const int N = 512;
            auto fillSine = [&](juce::AudioBuffer<float>& b) {
                b.setSize(2, N);
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < N; ++i)
                        b.getWritePointer(c)[i] =
                            0.5f * std::sin(6.28318530718f * 8.0f * (float)i / (float)N);
            };
            auto maxAbsDiffBuf = [&](const juce::AudioBuffer<float>& a,
                                     const juce::AudioBuffer<float>& b) {
                double d = 0.0;
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < N; ++i)
                        d = std::max(d, (double)std::abs(a.getReadPointer(c)[i]
                                                       - b.getReadPointer(c)[i]));
                return d;
            };

            // Every wavelet effect, driven at its NEUTRAL setting - the
            // parameter combination where it is mathematically an identity -
            // and required to hand the input back.
            //
            // This is a stronger check than it looks. In every case below the
            // neutral setting still runs the full forward DWT and inverse; only
            // the between-transform coefficient surgery is a no-op. So each of
            // these is really "the transform round-trip is lossless for this
            // effect's filter and level count", which is exactly what was
            // broken before the PR-inverse switch in 1d9a0c0 - the old inverse
            // dropped over half the energy, making every "bypass" setting a
            // heavy colouration.
            //
            // Deliberately NOT tested via Mix=0: that path never touches the
            // wavelet code, so it would pass even with a completely broken
            // transform. Every case here runs at Mix=1 (full wet).
            //
            // Each case is PAIRED with a non-vacuity check: the same processor,
            // one parameter moved off neutral, asserting the output now differs
            // materially. Without that pairing a unity assertion would sail
            // through if processBlock did nothing at all - which is exactly the
            // failure mode a "bypass is clean" test must not be blind to.
            auto runOnce = [&](std::vector<Param>& params,
                               std::function<std::unique_ptr<juce::AudioProcessor>(Node&)>& make) {
                NodeGraph g;
                int nId = g.addNode("neutral", NodeType::Effect, {}, {}).id;
                Node& nd = *g.findNode(nId);
                for (auto& p : params) nd.params.push_back(p);
                auto proc = make(nd);
                proc->prepareToPlay(48000.0, N);
                juce::AudioBuffer<float> buf, dryRef;
                fillSine(buf); fillSine(dryRef);
                juce::MidiBuffer mb;
                proc->processBlock(buf, mb);
                return maxAbsDiffBuf(buf, dryRef);
            };
            auto checkNeutral = [&](const char* label,
                                    std::vector<Param> params,
                                    std::function<std::unique_ptr<juce::AudioProcessor>(Node&)> make,
                                    const char* activeParam, float activeValue) {
                const double neutral = runOnce(params, make);
                r.checkVal(neutral < 1e-4,
                           juce::String("wavelet-fx: ") + label, neutral);
                // Same node, one knob off neutral: the effect has to bite.
                for (auto& p : params) if (p.name == activeParam) p.value = activeValue;
                const double active = runOnce(params, make);
                r.checkVal(active > 1e-3,
                           juce::String("wavelet-fx: ") + label
                             + " - and NOT unity once " + activeParam + " moves off it",
                           active);
            };

            // Transient Split with both gains at 1.0 sorts every coefficient
            // into exactly one of two complementary streams, so summing them
            // has to give the input back. This is the case that exposed the
            // legacy inverse: it dropped well over half the energy, i.e. the
            // "neutral" setting was a heavy, unavoidable colouration on the
            // flagship wavelet effect.
            checkNeutral("Transient Split at gains 1/1 is unity (no colouration)",
                         { {"Transient", 1.0f, 0.0f, 2.0f},
                           {"Sustain", 1.0f, 0.0f, 2.0f},
                           {"Threshold", 0.3f, 0.0f, 1.0f},
                           {"Levels", 4.0f, 1.0f, 8.0f} },
                         [](Node& nd) { return std::make_unique<TransientSplitProcessor>(nd); },
                         "Transient", 0.0f);

            // Threshold 0 puts no coefficient below it, so nothing is shrunk.
            checkNeutral("Denoiser at threshold 0 / full wet is unity",
                         { {"Threshold", 0.0f, 0.0f, 1.0f},
                           {"Levels", 4.0f, 1.0f, 8.0f},
                           {"Mix", 1.0f, 0.0f, 1.0f} },
                         [](Node& nd) { return std::make_unique<WaveletDenoiserProcessor>(nd); },
                         "Threshold", 0.9f);

            // Bits=16 is the finest quantisation the param allows: a step of
            // 1/65536, so every coefficient survives rounding to within ~8e-6.
            checkNeutral("Bitcrush at Bits=16 / full wet is unity",
                         { {"Bits", 16.0f, 1.0f, 16.0f},
                           {"Band Lo", 0.0f, 0.0f, 7.0f},
                           {"Band Hi", 7.0f, 0.0f, 7.0f},
                           {"Levels", 4.0f, 1.0f, 8.0f},
                           {"Mix", 1.0f, 0.0f, 1.0f} },
                         [](Node& nd) { return std::make_unique<WaveletBitcrushProcessor>(nd); },
                         "Bits", 2.0f);

            // Shift=0 means "don't move any band". Note this one short-circuits
            // before the transform, so unlike its siblings it only proves the
            // early-out, not the round-trip - which is the honest scope of the
            // param's neutral position.
            checkNeutral("Octave Shift at Shift=0 is unity",
                         { {"Shift", 0.0f, -2.0f, 2.0f},
                           {"Mix", 1.0f, 0.0f, 1.0f} },
                         [](Node& nd) { return std::make_unique<OctaveShiftProcessor>(nd); },
                         "Shift", -1.0f);

            // Ratio=1 makes the gain computer an identity whatever the band
            // peak is (dbReduction = dbOver * (1 - 1/1) = 0), and both tilt
            // gains at 0 dB leave the per-band trim at unity.
            checkNeutral("MB Comp at Ratio=1 / 0 dB tilt is unity",
                         { {"Threshold", -20.0f, -60.0f, 0.0f},
                           {"Ratio", 1.0f, 1.0f, 20.0f},
                           {"Levels", 4.0f, 1.0f, 6.0f},
                           {"Low Gain", 0.0f, -24.0f, 24.0f},
                           {"High Gain", 0.0f, -24.0f, 24.0f},
                           {"Mix", 1.0f, 0.0f, 1.0f} },
                         [](Node& nd) { return std::make_unique<WaveletMultibandCompProcessor>(nd); },
                         "Ratio", 20.0f);

            // Complexity=1 keeps every coefficient (keep = padLen), so the
            // partial_sort and the keep-mask select the whole set.
            checkNeutral("Complexity at 1.0 (keep everything) is unity",
                         { {"Complexity", 1.0f, 0.0f, 1.0f},
                           {"Levels", 4.0f, 1.0f, 8.0f},
                           {"Mix", 1.0f, 0.0f, 1.0f} },
                         [](Node& nd) { return std::make_unique<WaveletComplexityProcessor>(nd); },
                         "Complexity", 0.02f);

            // Both gains at 1.0 flatten the asymmetric envelope: the pre-attack
            // ramp becomes 1 + (1-1)*frac and the post-decay becomes
            // 1 + (1-1)*(1-frac), so gainEnv stays 1 even where transients are
            // detected. Detection still runs - this is not an early-out.
            checkNeutral("Asymmetric Filter at gains 1/1 is unity",
                         { {"Pre-Attack", 20.0f, 0.0f, 200.0f},
                           {"Post-Decay", 50.0f, 0.0f, 500.0f},
                           {"Pre Gain", 1.0f, 0.0f, 4.0f},
                           {"Post Gain", 1.0f, 0.0f, 4.0f},
                           {"Levels", 4.0f, 1.0f, 8.0f},
                           {"Mix", 1.0f, 0.0f, 1.0f} },
                         [](Node& nd) { return std::make_unique<AsymmetricFilterProcessor>(nd); },
                         "Post Gain", 0.0f);

            // The reverb's neutral is the least obvious of the set. It works
            // on an 8192-sample tail buffer: each block shifts the tail left by
            // n and writes the new input into the last n slots, transforms the
            // whole tail, weights each band by 1/(band+1)^Color, inverts, and
            // takes the last n samples back out. So with Color=0 (every weight
            // = 1) and Decay=1 (the shift doesn't attenuate), the samples that
            // come out are exactly the ones just written in - full wet, and
            // still a complete 8192-point round-trip.
            checkNeutral("Reverb at Decay=1 / Color=0 / full wet is unity",
                         { {"Decay", 1.0f, 0.0f, 1.0f},
                           {"Color", 0.0f, 0.0f, 3.0f},
                           {"Levels", 5.0f, 1.0f, 8.0f},
                           {"Mix", 1.0f, 0.0f, 1.0f} },
                         [](Node& nd) { return std::make_unique<WaveletReverbProcessor>(nd); },
                         "Color", 3.0f);

            // The vocoder is the one effect with no neutral *parameter* - it
            // imposes the modulator's per-band energy on the carrier, so there
            // is no knob position that makes it an identity. Its identity is a
            // property of the SIGNALS instead: feed the same audio as carrier
            // (ch 0) and modulator (ch 2) and every band's scale factor is
            // sqrt(modE/carE) = 1, so the carrier must come back untouched.
            //
            // Needs its own block because it wants >2 channels, and because it
            // mono-ises (it copies ch0 over ch1), so only ch0 is comparable.
            {
                NodeGraph g;
                int nId = g.addNode("vocoder", NodeType::Effect, {}, {}).id;
                Node& nd = *g.findNode(nId);
                nd.params.push_back({"Bands", 5.0f, 1.0f, 8.0f});
                nd.params.push_back({"Mix",   1.0f, 0.0f, 1.0f});

                WaveletVocoderProcessor proc(nd);
                proc.prepareToPlay(48000.0, N);

                juce::AudioBuffer<float> buf(3, N);
                buf.clear();
                for (int i = 0; i < N; ++i) {
                    const float v =
                        0.5f * std::sin(6.28318530718f * 8.0f * (float)i / (float)N);
                    buf.getWritePointer(0)[i] = v;   // carrier
                    buf.getWritePointer(2)[i] = v;   // modulator - identical
                }
                std::vector<float> dryRef(buf.getReadPointer(0),
                                          buf.getReadPointer(0) + N);
                juce::MidiBuffer mb;
                proc.processBlock(buf, mb);

                double d = 0.0;
                for (int i = 0; i < N; ++i)
                    d = std::max(d, (double)std::abs(buf.getReadPointer(0)[i]
                                                   - dryRef[(size_t)i]));
                r.checkVal(d < 1e-4,
                           "wavelet-fx: Vocoder with modulator == carrier is unity", d);

                // Non-vacuity for the above: a modulator with a *different*
                // spectrum has to reshape the carrier. (The same sine eight
                // octaves up, so its band energies land somewhere else.)
                for (int i = 0; i < N; ++i) {
                    buf.getWritePointer(0)[i] = dryRef[(size_t)i];
                    buf.getWritePointer(2)[i] =
                        0.5f * std::sin(6.28318530718f * 64.0f * (float)i / (float)N);
                }
                juce::MidiBuffer mb3;
                proc.processBlock(buf, mb3);
                double dActive = 0.0;
                for (int i = 0; i < N; ++i)
                    dActive = std::max(dActive, (double)std::abs(buf.getReadPointer(0)[i]
                                                               - dryRef[(size_t)i]));
                r.checkVal(dActive > 1e-3,
                           "wavelet-fx: Vocoder - and NOT unity once the modulator "
                           "differs from the carrier", dActive);

                // And the documented "nothing plugged into the Signal input"
                // behaviour: with no channel 2 there is no modulator, so the
                // node must pass the carrier straight through rather than
                // muting (scale would otherwise be sqrt(0/carE) = 0 per band).
                juce::AudioBuffer<float> stereo(2, N);
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < N; ++i)
                        stereo.getWritePointer(c)[i] =
                            0.5f * std::sin(6.28318530718f * 8.0f * (float)i / (float)N);
                juce::AudioBuffer<float> stereoRef;
                fillSine(stereoRef);
                juce::MidiBuffer mb2;
                proc.processBlock(stereo, mb2);
                r.checkVal(maxAbsDiffBuf(stereo, stereoRef) < 1e-6,
                           "wavelet-fx: Vocoder with no modulator connected is a "
                           "passthrough, not silence",
                           maxAbsDiffBuf(stereo, stereoRef));
            }
        }

        // 7. The Wavelet Reverb's decay must not depend on the audio device's
        //    buffer size. The tail is aged once per processBlock, so applying
        //    the Decay knob verbatim each block attenuated a sample
        //    tailLen/blockSize times over its life - 16 times at a 512-sample
        //    buffer but 128 at a 64-sample one. At Decay = 0.7 that is 0.003
        //    versus 1.4e-6: the same preset was an ambience on one machine and
        //    silence on another. The fix derives the per-block coefficient from
        //    the block size; this test is what pins it down.
        //
        //    Getting a clean reading here took three attempts, so the reasoning
        //    is worth recording - the obvious tests all measure something else.
        //
        //    a) "Burst, then watch the tail fade during silence" fails because
        //       this node does not ring. It emits only the LAST `n` samples of
        //       the transformed tail, and those sit exactly on top of the freshly
        //       written input, so once the input stops the content marches left
        //       out of the readout window within a couple of hundred samples and
        //       the output is gone long before any decay curve is visible.
        //       (Confirmed: with Color = 0 the node is bit-exact unity, which is
        //       another way of saying the readout window only ever contains the
        //       new block.) What Decay actually controls is how heavily the older
        //       content is faded BEFORE the transform, i.e. how much history
        //       bleeds into the current output through the long wavelet basis
        //       functions.
        //    b) "Compare raw output level at two block sizes" fails because the
        //       readout window is the region most distorted by the transform's
        //       right-hand boundary, so level per sample is block-size dependent
        //       no matter what the decay does - measured 2.4x between 64 and 512
        //       with the decay behaving perfectly.
        //    c) "Divide out (b) using each block size's own Decay = 1 run, under
        //       a steady tone" fails because a sample is aged once per block, so
        //       the attenuation profile along the buffer is a STAIRCASE whose
        //       step is the per-block coefficient. Multiplied into a continuous
        //       tone that staircase is a train of amplitude discontinuities, and
        //       their broadband splatter swamps the decay: it made Decay = 0.9
        //       measure LOUDER than Decay = 1.
        //
        //    What works is an impulse. The staircase then multiplies an almost
        //    entirely zero buffer, so it generates no artefacts of its own, and
        //    the single non-zero sample carries exactly the accumulated
        //    attenuation for its age as it migrates out. Summing |output| and
        //    dividing by the same run at Decay = 1 gives the mean attenuation
        //    over the ages the readout can see - the quantity the bug corrupted,
        //    with the boundary geometry of (b) cancelled by the self-ratio.
        //
        //    The `kSkip` window is essential and was the last thing to get
        //    right. Summed over the impulse's WHOLE life the reading is
        //    dominated by its first few hundred samples, when it is still inside
        //    the readout window and has barely been aged at all - and a metric
        //    dominated by un-aged output is blind to the ageing rate. Measured:
        //    with the bug deliberately reinstated that version scored 0.927,
        //    i.e. it passed more comfortably than the fixed code did (0.756).
        //    Skipping the first 1024 samples restricts the measurement to ages
        //    where Decay has actually had a chance to act.
        //
        //    Levels = 8 / Color = 3 widen the window into history: the level-8
        //    db4 scaling function spans (8-1)*(2^8-1)+1 = 1786 samples, and
        //    crushing the short detail bands (gains 1, 1/8, 1/27, ...) leaves
        //    that long blur as the output.
        //
        //    Verified by reinstating the bug: this test reads 1.09 against the
        //    fixed code and 122204 against the broken code, so the margin is
        //    five orders of magnitude and the tolerance below is nowhere near
        //    the limiting factor.
        {
            auto impulseLevel = [](int bs, float decay) {
                NodeGraph g;
                int nId = g.addNode("rev", NodeType::Effect, {}, {}).id;
                Node& nd = *g.findNode(nId);
                nd.params.push_back({"Decay",  decay, 0.0f, 1.0f});
                nd.params.push_back({"Color",  3.0f, 0.0f, 3.0f}); // must be > 0:
                nd.params.push_back({"Levels", 8.0f, 1.0f, 8.0f}); // at Color 0 the
                nd.params.push_back({"Mix",    1.0f, 0.0f, 1.0f}); // node is unity
                WaveletReverbProcessor proc(nd);
                proc.prepareToPlay(48000.0, bs);

                const int kTotal = 8192;   // one full traversal of the tail
                const int kSkip  = 1024;   // ...but ignore the impulse's youth
                juce::AudioBuffer<float> buf(2, bs);
                juce::MidiBuffer mb;

                double level = 0.0;
                for (int s = 0; s < kTotal; s += bs) {
                    buf.clear();
                    if (s == 0)              // impulse at absolute sample 0, so
                        for (int c = 0; c < 2; ++c)   // both runs age it over
                            buf.getWritePointer(c)[0] = 1.0f;  // the same clock
                    proc.processBlock(buf, mb);
                    if (s < kSkip) continue;
                    for (int i = 0; i < bs; ++i)
                        level += std::abs((double)buf.getReadPointer(0)[i]);
                }
                return level;
            };

            const double open512 = impulseLevel(512, 1.0f);
            const double open64  = impulseLevel(64,  1.0f);
            // Non-vacuity first: if the node emitted nothing there would be
            // nothing to compare and the ratio checks below would pass on 0/0.
            r.checkVal(open512 > 1e-3 && open64 > 1e-3,
                       "wavelet-fx: Reverb smears an impulse at Levels 8 / Color 3",
                       juce::jmin(open512, open64));

            const double att512 = impulseLevel(512, 0.9f) / juce::jmax(1e-12, open512);
            const double att64  = impulseLevel(64,  0.9f) / juce::jmax(1e-12, open64);
            // ...and the knob must actually attenuate, or a node that ignored
            // Decay entirely would satisfy the block-size check trivially.
            r.checkVal(att512 < 0.95,
                       "wavelet-fx: Reverb's Decay knob attenuates the tail",
                       att512);

            const double ratio = att512 / juce::jmax(1e-12, att64);
            // The tolerance is set by the design, not by the fix: the two runs
            // approximate the same exponential with staircases of different step
            // size (10% at 512 samples, 1.3% at 64), and the readout reaches
            // 1786 + n samples back, so the range of ages being averaged differs
            // by the block size too. Those leave a residual 9% (measured 1.086);
            // the bug leaves 122204.
            r.checkVal(ratio > 0.8 && ratio < 1.25,
                       "wavelet-fx: Reverb decay is independent of the audio "
                       "buffer size (512 vs 64 samples)", ratio);
        }

        // 8. The Asymmetric Filter must put its pre-attack region where the ms
        //    knob says it is: immediately BEFORE the onset, on the time axis.
        //
        //    It used to lay the envelope out across the concatenated wavelet
        //    coefficient array, which is not a time axis - one step in the
        //    approximation band is 2^Levels samples and one step in the finest
        //    detail band is 2 - and it indexed that array with onset positions
        //    counted in finest-band coefficients. So a "20 ms pre-attack" was
        //    neither 20 ms nor the same width in any two bands, and its low
        //    indices landed in the approximation region at the START of the
        //    block instead of next to the onset. Now the envelope is built in
        //    samples and resampled onto each band by that band's stride.
        //
        //    Test signal: a quiet 200 Hz tone (something for the gain to act on)
        //    plus one loud click at sample 700. The tone is low enough in
        //    frequency to leave the finest detail band alone, so the click is
        //    unambiguously the only onset. With Pre Gain = 4 and Post Gain = 1,
        //    wet-minus-dry has to concentrate just before sample 700 and leave
        //    the start of the block alone.
        {
            const int   kN     = 1024;
            const int   kOnset = 700;
            const float kPreMs = 5.0f;   // 240 samples at 48 kHz -> [460, 700)

            NodeGraph g;
            int nId = g.addNode("asym", NodeType::Effect, {}, {}).id;
            Node& nd = *g.findNode(nId);
            nd.params.push_back({"Pre-Attack", kPreMs, 0.0f, 100.0f});
            nd.params.push_back({"Post-Decay",   1.0f, 0.0f, 200.0f});
            nd.params.push_back({"Pre Gain",     4.0f, 0.0f,   4.0f});
            nd.params.push_back({"Post Gain",    1.0f, 0.0f,   2.0f});
            nd.params.push_back({"Sensitivity",  0.5f, 0.0f,   1.0f});
            nd.params.push_back({"Levels",       4.0f, 1.0f,   8.0f});
            nd.params.push_back({"Mix",          1.0f, 0.0f,   1.0f});
            AsymmetricFilterProcessor proc(nd);
            proc.prepareToPlay(48000.0, kN);

            juce::AudioBuffer<float> buf(2, kN), dry(2, kN);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < kN; ++i)
                    dry.getWritePointer(c)[i] =
                        0.2f * std::sin(6.28318530718f * 200.0f * (float)i / 48000.0f)
                        + (i == kOnset ? 1.0f : 0.0f);
            buf.makeCopyOf(dry);
            juce::MidiBuffer mb;
            proc.processBlock(buf, mb);

            auto diffOver = [&](int a, int b) {
                double d = 0.0;
                for (int i = a; i < b; ++i)
                    d += std::abs((double)buf.getReadPointer(0)[i]
                                  - (double)dry.getReadPointer(0)[i]);
                return d;
            };
            // The db4 basis at 4 levels has a support of (8-1)*(2^4-1)+1 = 106
            // samples, so any change smears about that far either side of where
            // it is applied. The two windows are 150 samples clear of the
            // pre-attack region's edges, comfortably outside that.
            const double inRegion  = diffOver(500, 800);
            const double atBlockStart = diffOver(0, 310);
            r.checkVal(inRegion > 1e-3,
                       "wavelet-fx: Asymmetric Filter's pre-attack region "
                       "changes the audio at all", inRegion);
            const double leak = atBlockStart / juce::jmax(1e-12, inRegion);
            // Verified by reinstating the bug: laying the envelope out along the
            // coefficient array instead reads 0.28 here (against 0.00 for the
            // fixed code), and its in-region figure collapses from 48.8 to 2.1 -
            // so the old version was applying most of its gain nowhere near the
            // onset it had just detected.
            r.checkVal(leak < 0.05,
                       "wavelet-fx: Asymmetric Filter's pre-attack lands before "
                       "the onset, not at the start of the block", leak);
        }
    }

    // ---- Wavelet effects: real-time CPU budget --------------------------
    //
    // Every wavelet effect is a node that can be placed many times in one
    // graph, and the whole graph has to finish inside a single buffer period
    // or the audio device underruns. So "does it run at all" is not the bar -
    // each effect must run at a small FRACTION of real time on its own.
    //
    // The metric here is the realtime factor: seconds of audio produced per
    // second of wall clock. 1.0x means the effect alone exactly consumes the
    // entire audio budget (already unusable - there is no headroom for the
    // rest of the graph, the UI, or a slower machine). We require 10x, i.e.
    // no single effect may eat more than a tenth of one core's budget.
    //
    // This is deliberately a permanent test rather than a one-off benchmark:
    // the wavelet suite is the part of SEANCE with no free competitor, it is
    // the part most likely to be shipped as a plugin, and a plugin that
    // cannot sustain realtime fails validation outright. A regression here
    // (a stray allocation, an accidental O(n^2), a levels default bumped up)
    // is otherwise invisible until a user hears crackling.
    //
    // The threshold is loose on purpose so it does not flake on a slow or
    // loaded CI box; it is sized to catch order-of-magnitude problems, which
    // is the failure mode that actually happens. The measured value is
    // recorded for every effect so trends are visible in the report even
    // when everything passes.
    {
        const int    BS     = 512;
        const double SR     = 48000.0;
        const int    BLOCKS = 100;              // ~1.07 s of stereo audio
        const double MIN_RT = 10.0;             // must be >=10x realtime

        // Source material: a tone plus noise, so transient detectors and
        // threshold-based branches actually take their expensive paths
        // rather than early-outing on silence.
        // 3 channels: 0/1 are the stereo audio, 2 is a Signal-pin input. The
        // Vocoder reads its modulator from channel 2 and returns immediately
        // if the buffer has fewer than 3 channels, so a stereo-only buffer
        // would "measure" it at ~200000x realtime while doing no work at all.
        const int SRC_CH = 3;
        juce::AudioBuffer<float> src(SRC_CH, BS);
        {
            juce::Random rng(20260805);
            for (int c = 0; c < SRC_CH; ++c) {
                float* d = src.getWritePointer(c);
                for (int i = 0; i < BS; ++i)
                    d[i] = 0.4f * std::sin(6.28318530718f * 220.0f * (float)i / (float)SR)
                         + 0.1f * (rng.nextFloat() * 2.0f - 1.0f);
            }
        }

        // Best of N repeats, not a single timing. Interference from other
        // processes can only ever make a run SLOWER, never faster, so the
        // maximum across repeats is a far more stable estimator of the true
        // throughput than one sample or an average. Without this the whole
        // suite drifts by ~2x depending on machine load, which would make an
        // effect sitting near the threshold flake intermittently.
        const int REPEATS = 3;
        auto realtimeFactor = [&](juce::AudioProcessor& proc) {
            juce::AudioBuffer<float> buf(SRC_CH, BS);
            juce::MidiBuffer mb;
            proc.prepareToPlay(SR, BS);

            // Warm-up block, untimed: first-touch page faults and any lazy
            // one-time setup would otherwise be charged to the measurement.
            buf.makeCopyOf(src);
            proc.processBlock(buf, mb);

            double best = 0.0;
            for (int rep = 0; rep < REPEATS; ++rep) {
                auto t0 = std::chrono::steady_clock::now();
                for (int b = 0; b < BLOCKS; ++b) {
                    for (int c = 0; c < SRC_CH; ++c)
                        buf.copyFrom(c, 0, src, c, 0, BS);   // memcpy, not a realloc
                    proc.processBlock(buf, mb);
                }
                auto t1 = std::chrono::steady_clock::now();

                double wall  = std::chrono::duration<double>(t1 - t0).count();
                double audio = (double)(BLOCKS * BS) / SR;
                best = std::max(best, wall > 1e-9 ? audio / wall : 1e9);
            }
            return best;
        };

        // Each effect is driven at a NON-neutral setting - several of them
        // early-out at their default (e.g. Wavelet Pitch returns immediately
        // when |Semitones| < 0.01), which would measure nothing at all.
        struct ParamSpec { const char* name; float val, lo, hi; };
        auto makeNode = [&](NodeGraph& g, std::initializer_list<ParamSpec> ps) -> Node& {
            int nId = g.addNode("cpu", NodeType::Effect, {}, {}).id;
            Node& nd = *g.findNode(nId);
            for (auto& p : ps) nd.params.push_back({p.name, p.val, p.lo, p.hi});
            return nd;
        };

        auto budget = [&](const char* label, juce::AudioProcessor& proc) {
            double rt = realtimeFactor(proc);
            r.checkVal(rt >= MIN_RT,
                       juce::String("wavelet-cpu: ") + label
                           + " runs >=10x realtime (x realtime)", rt);
        };
        // Same measurement, but for an effect whose slowness is a tracked bug.

        { NodeGraph g; Node& nd = makeNode(g, {{"Transient",2.0f,0,2},{"Sustain",0.5f,0,2},
                                               {"Threshold",0.3f,0,1},{"Levels",4.0f,1,8}});
          TransientSplitProcessor p(nd);          budget("Transient Split", p); }
        { NodeGraph g; Node& nd = makeNode(g, {{"Threshold",0.1f,0,1},{"Levels",4.0f,1,8},
                                               {"Mix",1.0f,0,1}});
          WaveletDenoiserProcessor p(nd);         budget("Denoiser", p); }
        { NodeGraph g; Node& nd = makeNode(g, {{"Bits",4.0f,1,16},{"Band Lo",0.0f,0,7},
                                               {"Band Hi",7.0f,0,7},{"Levels",4.0f,1,8},
                                               {"Mix",1.0f,0,1}});
          WaveletBitcrushProcessor p(nd);         budget("Bitcrush", p); }
        { NodeGraph g; Node& nd = makeNode(g, {{"Shift",-1.0f,-2,2},{"Mix",0.5f,0,1}});
          OctaveShiftProcessor p(nd);             budget("Octave Shift", p); }
        { NodeGraph g; Node& nd = makeNode(g, {{"Threshold",-20.0f,-60,0},{"Ratio",4.0f,1,20},
                                               {"Levels",4.0f,1,6},{"Low Gain",0.0f,-12,12},
                                               {"High Gain",0.0f,-12,12},{"Mix",1.0f,0,1}});
          WaveletMultibandCompProcessor p(nd);    budget("Multiband Comp", p); }
        { NodeGraph g; Node& nd = makeNode(g, {{"Decay",0.7f,0,1},{"Color",1.0f,0,3},
                                               {"Levels",5.0f,1,8},{"Mix",0.3f,0,1}});
          WaveletReverbProcessor p(nd);           budget("Reverb (1/f)", p); }
        { NodeGraph g; Node& nd = makeNode(g, {{"Semitones",7.0f,-24,24},{"Threshold",0.3f,0,1},
                                               {"Trans Gain",1.0f,0,2},{"Levels",4.0f,1,8},
                                               {"Mix",1.0f,0,1}});
          IndependentPitchShiftProcessor p(nd);   budget("Ind. Pitch Shift", p); }
        { NodeGraph g; Node& nd = makeNode(g, {{"Complexity",0.5f,0,1},{"Levels",4.0f,1,8},
                                               {"Mix",1.0f,0,1}});
          WaveletComplexityProcessor p(nd);       budget("Complexity", p); }
        { NodeGraph g; Node& nd = makeNode(g, {{"Pre-Attack",20.0f,0,100},{"Post-Decay",50.0f,0,200},
                                               {"Pre Gain",2.0f,0,4},{"Post Gain",0.5f,0,2},
                                               {"Levels",4.0f,1,8},{"Mix",1.0f,0,1}});
          AsymmetricFilterProcessor p(nd);        budget("Asymmetric Filter", p); }
        { NodeGraph g; Node& nd = makeNode(g, {{"Min Hz",50.0f,20,5000},{"Max Hz",2000.0f,20,5000},
                                               {"Detected Hz",0.0f,0,5000}});
          WaveletPitchTrackerProcessor p(nd);     budget("Pitch Tracker", p); }
        { NodeGraph g; Node& nd = makeNode(g, {{"Bands",5.0f,1,8},{"Mix",1.0f,0,1}});
          WaveletVocoderProcessor p(nd);          budget("Vocoder", p); }
        { NodeGraph g; Node& nd = makeNode(g, {{"Semitones",7.0f,-24,24},{"Formant Lock",0.8f,0,1},
                                               {"Levels",5.0f,1,8},{"Mix",1.0f,0,1}});
          FormantPitchShiftProcessor p(nd);       budget("Formant Pitch Shift", p); }
    }

    // ---- The two node-level pitch shifters, end to end ------------------
    //
    // Independent Pitch Shift and Formant Pitch Shift used to transpose by
    // resampling INSIDE the current block: `srcPos = i * ratio`, reading source
    // sample `i * ratio` to produce output sample `i`. For an upward shift that
    // runs off the end of the block - at ratio 2 every output sample past the
    // halfway point wanted a source sample that did not exist, and the code
    // emitted silence for it. Each block was a correctly-shifted first half
    // followed by a zeroed second half: a 50%-duty gate at the block rate
    // (~94 Hz at 512/48k), measured here as a last-quarter energy fraction of
    // 0.00000 against a healthy 0.25. Both now run on PhaseVocoderShifter.
    //
    // Two things are checked, and the continuity one is the reason this test
    // drives many consecutive blocks rather than one big buffer: a single call
    // cannot reveal block-rate structure.
    {
        const int BS = 512;
        const double SR = 48000.0;
        const double inHz = 440.0;

        auto dominantHz = [&](const float* d, int n) {
            double bestP = -1, bestF = 0;
            for (double f = 100; f <= 4000; f += 1.0) {
                double re = 0, im = 0;
                for (int i = 0; i < n; ++i) {
                    double a = 6.28318530718 * f * i / SR;
                    re += d[i] * std::cos(a); im += d[i] * std::sin(a);
                }
                double p = re * re + im * im;
                if (p > bestP) { bestP = p; bestF = f; }
            }
            return bestF;
        };

        // Drive a continuous 440 Hz tone through the node and return the
        // steady-state output. `skipBlocks` must clear the node's reported
        // latency, or the ramp-up is what gets measured.
        auto runNode = [&](juce::AudioProcessor& proc, int keepBlocks) {
            proc.prepareToPlay(SR, BS);
            const int skipBlocks = proc.getLatencySamples() / BS + 4;
            juce::AudioBuffer<float> buf(2, BS);
            juce::MidiBuffer mb;
            std::vector<float> out;
            out.reserve((size_t)(keepBlocks * BS));
            int phase = 0;
            for (int b = 0; b < skipBlocks + keepBlocks; ++b) {
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < BS; ++i)
                        buf.getWritePointer(c)[i] =
                            0.5f * (float)std::sin(6.28318530718 * inHz * (phase + i) / SR);
                proc.processBlock(buf, mb);
                if (b >= skipBlocks) {
                    const float* d = buf.getReadPointer(0);
                    out.insert(out.end(), d, d + BS);
                }
                phase += BS;
            }
            return out;
        };

        auto tailFraction = [&](const std::vector<float>& out) {
            double tailE = 0, totalE = 0;
            for (size_t b = 0; b * BS < out.size(); ++b)
                for (int i = 0; i < BS; ++i) {
                    double v = out[b * BS + (size_t)i];
                    totalE += v * v;
                    if (i >= (BS * 3) / 4) tailE += v * v;
                }
            return totalE > 1e-20 ? tailE / totalE : 0.0;
        };

        auto checkShifter = [&](const char* label, juce::AudioProcessor& proc) {
            auto out = runNode(proc, 12);
            double frac = tailFraction(out);
            r.checkVal(std::abs(frac - 0.25) <= 0.06,
                       juce::String("wavelet-fx: ") + label + " +12 spreads energy evenly "
                       "across the block (last-quarter energy fraction, 0.25 = uniform)",
                       frac);
            double outHz = dominantHz(out.data(), (int)out.size());
            double cents = 1200.0 * std::log2(outHz / (inHz * 2.0));
            r.checkVal(std::abs(cents) <= 50.0,
                       juce::String("wavelet-fx: ") + label + " +12 lands within 50 cents "
                       "of 880 Hz (cents error)",
                       cents);
            r.checkVal(proc.getLatencySamples() > 0,
                       juce::String("wavelet-fx: ") + label + " reports its pitch-shifter "
                       "latency for PDC (samples)",
                       proc.getLatencySamples());
        };

        {
            NodeGraph g;
            int nId = g.addNode("indps", NodeType::Effect, {}, {}).id;
            Node& nd = *g.findNode(nId);
            nd.params.push_back({"Semitones",  12.0f, -24.0f, 24.0f});
            nd.params.push_back({"Threshold",   1.0f,   0.0f,  1.0f}); // all tonal
            nd.params.push_back({"Trans Gain",  0.0f,   0.0f,  2.0f});
            nd.params.push_back({"Levels",      4.0f,   1.0f,  8.0f});
            nd.params.push_back({"Mix",         1.0f,   0.0f,  1.0f});
            IndependentPitchShiftProcessor proc(nd);
            checkShifter("Ind. Pitch Shift", proc);
        }
        {
            NodeGraph g;
            int nId = g.addNode("fps", NodeType::Effect, {}, {}).id;
            Node& nd = *g.findNode(nId);
            nd.params.push_back({"Semitones",    12.0f, -24.0f, 24.0f});
            nd.params.push_back({"Formant Lock",  0.0f,   0.0f,  1.0f});
            nd.params.push_back({"Levels",        5.0f,   1.0f,  8.0f});
            nd.params.push_back({"Mix",           1.0f,   0.0f,  1.0f});
            FormantPitchShiftProcessor proc(nd);
            checkShifter("Formant Pitch Shift", proc);
        }

        // ---- The Pitch Shift node ---------------------------------------
        //
        // This node used to wrap Rubber Band and had NO test coverage at all.
        // That mattered, because it was the only remaining user of the GPL v2
        // `third_party/rubberband`, so whether the dependency could be deleted
        // rested entirely on what the node actually delivered. Measuring it
        // found that of its three params only Pitch worked: Time Ratio could
        // not work in a live node (a processBlock must emit as many samples as
        // it is handed, so a duration change has nowhere to go - at ratio 2 the
        // surplus backed up forever, at ratio 0.5 it starved and zero-filled
        // half the output), and Formant was never read at all, rendering
        // bit-identical at 0 and 1. Both are gone; both remaining params work.
        //
        // Params are read BY NAME now, so these must match the names the
        // node-creation site uses. The constants exist so a rename cannot
        // silently desync them and leave everything reading defaults.
        auto makePitchShiftNode = [&](NodeGraph& g, float semis, float formant) -> Node& {
            int nId = g.addNode("ps", NodeType::Effect, {}, {}).id;
            Node& nd = *g.findNode(nId);
            nd.params.push_back({PitchShiftProcessor::kPitchParam, semis, -24.0f, 24.0f});
            nd.params.push_back({PitchShiftProcessor::kFormantParam, formant, 0.0f, 1.0f});
            return nd;
        };

        {
            NodeGraph g;
            Node& nd = makePitchShiftNode(g, 12.0f, 0.0f);
            PitchShiftProcessor proc(nd);
            auto out = runNode(proc, 12);
            double outHz = dominantHz(out.data(), (int)out.size());
            double cents = 1200.0 * std::log2(outHz / (inHz * 2.0));
            r.checkVal(std::abs(cents) <= 50.0,
                       "pitch-shift-node: +12 lands within 50 cents of 880 Hz "
                       "(cents error)", cents);
            r.checkVal(std::abs(tailFraction(out) - 0.25) <= 0.06,
                       "pitch-shift-node: +12 spreads energy evenly across the block "
                       "(last-quarter energy fraction, 0.25 = uniform)",
                       tailFraction(out));
            r.checkVal(proc.getLatencySamples() > 0,
                       "pitch-shift-node: reports its latency so the graph can PDC it "
                       "(samples)", (double)proc.getLatencySamples());
        }

        // The Formant knob used to be inert. It is now wired to the core's
        // cepstral formant preservation, so the SAME comparison that once
        // proved it dead (render at 0, render at 1, diff them) must now show a
        // real difference. Keeping the test in this shape is deliberate: it is
        // the direct regression guard against the knob going inert again.
        {
            auto render = [&](float formant) {
                NodeGraph g;
                Node& nd = makePitchShiftNode(g, 12.0f, formant);
                PitchShiftProcessor proc(nd);
                return runNode(proc, 12);
            };
            auto a = render(0.0f);
            auto b = render(1.0f);
            double maxDiff = 0.0;
            const size_t nCmp = std::min(a.size(), b.size());
            for (size_t i = 0; i < nCmp; ++i)
                maxDiff = std::max(maxDiff, (double)std::abs(a[i] - b[i]));
            r.checkVal(nCmp > 0 && maxDiff > 1e-4,
                       "pitch-shift-node: the Formant knob is a LIVE control - Formant 0 "
                       "and Formant 1 render audibly different output (max sample "
                       "difference; this param was inert under Rubber Band)",
                       maxDiff);
        }

        // A stale "Time Ratio" left over in an old project must not disturb
        // anything. This is the payoff for reading params by name: the same
        // node with a junk extra param renders bit-identically. Under the old
        // by-index reading, an extra param would have re-pointed every later
        // one and silently changed what the node did.
        {
            NodeGraph g1, g2;
            Node& a = makePitchShiftNode(g1, 12.0f, 0.0f);
            int nId = g2.addNode("ps2", NodeType::Effect, {}, {}).id;
            Node& b = *g2.findNode(nId);
            b.params.push_back({PitchShiftProcessor::kPitchParam, 12.0f, -24.0f, 24.0f});
            b.params.push_back({"Time Ratio", 0.5f, 0.25f, 4.0f});   // the ghost
            b.params.push_back({PitchShiftProcessor::kFormantParam, 0.0f, 0.0f, 1.0f});
            PitchShiftProcessor pa(a), pb(b);
            auto oa = runNode(pa, 12);
            auto ob = runNode(pb, 12);
            double maxDiff = 0.0;
            const size_t nCmp = std::min(oa.size(), ob.size());
            for (size_t i = 0; i < nCmp; ++i)
                maxDiff = std::max(maxDiff, (double)std::abs(oa[i] - ob[i]));
            r.checkVal(nCmp > 0 && maxDiff == 0.0,
                       "pitch-shift-node: a leftover Time Ratio param from an old project "
                       "changes nothing (max sample difference vs the same node without "
                       "it)", maxDiff);
        }
    }

    // ---- PhaseVocoderShifter: the in-house pitch-shift core -------------
    //
    // This is the replacement for both the block-chopping resamplers above and
    // (eventually) the GPL Rubber Band dependency, so it is tested directly
    // rather than only through the nodes that will consume it. The three
    // obligations, in order of importance:
    //
    //   1. put the energy at the requested pitch, accurately enough that a
    //      one-semitone shift is audibly one semitone (the specific thing the
    //      old implementations could not do),
    //   2. keep the level intact,
    //   3. produce a continuous signal with no block-rate structure.
    //
    // Plus a capacity check, because this runs on the audio thread and the
    // whole point of the design is that process() never allocates.
    {
        const double SR = 48000.0;
        const int    BS = 512;
        const double inHz = 440.0;

        // Coarse DFT peak-pick, 1 Hz resolution over the musical range. Fine
        // enough that the quantisation is ~4 cents at 440 Hz, well inside the
        // tolerances below.
        auto dominantHz = [&](const float* d, int n) {
            double bestP = -1, bestF = 0;
            for (double f = 100; f <= 4000; f += 1.0) {
                double re = 0, im = 0;
                for (int i = 0; i < n; ++i) {
                    double a = 6.28318530718 * f * i / SR;
                    re += d[i] * std::cos(a); im += d[i] * std::sin(a);
                }
                double p = re * re + im * im;
                if (p > bestP) { bestP = p; bestF = f; }
            }
            return bestF;
        };
        auto rmsOf = [](const std::vector<float>& v) {
            double s = 0;
            for (float x : v) s += (double)x * x;
            return v.empty() ? 0.0 : std::sqrt(s / (double)v.size());
        };

        // Drive a continuous sine through the shifter in BS-sized blocks and
        // return the steady-state output, with the pipeline latency plus a
        // safety margin discarded so the ramp-up is never measured.
        auto runTone = [&](PhaseVocoderShifter& ps, int keepSamples) {
            const int skip = ps.latencySamples() + 4 * ps.fftLength();
            std::vector<float> out;
            out.reserve((size_t)(skip + keepSamples + BS));
            std::vector<float> in((size_t)BS), tmp((size_t)BS);
            int phase = 0;
            while ((int)out.size() < skip + keepSamples) {
                for (int i = 0; i < BS; ++i)
                    in[(size_t)i] = 0.5f * (float)std::sin(6.28318530718 * inHz * (phase + i) / SR);
                ps.process(in.data(), tmp.data(), BS);
                out.insert(out.end(), tmp.begin(), tmp.end());
                phase += BS;
            }
            return std::vector<float>(out.begin() + skip, out.begin() + skip + keepSamples);
        };

        // 1. Pitch accuracy. 25 cents is a quarter of a semitone - tight enough
        //    that no interval can be confused with its neighbour, and well
        //    inside what a listener would call in tune.
        for (float semis : {-12.0f, -7.0f, -1.0f, 0.0f, 1.0f, 7.0f, 12.0f}) {
            PhaseVocoderShifter ps;
            ps.prepare(11, 4);
            ps.setPitchRatio(PhaseVocoderShifter::ratioForSemitones(semis));
            auto out = runTone(ps, 4096);
            double outHz  = dominantHz(out.data(), (int)out.size());
            double expect = inHz * std::pow(2.0, semis / 12.0);
            double cents  = 1200.0 * std::log2(outHz / expect);
            r.checkVal(std::abs(cents) <= 25.0,
                       juce::String("pitch-core: ") + juce::String(semis, 0)
                           + " semitones lands within 25 cents of "
                           + juce::String(expect, 1) + " Hz (cents error)",
                       cents);
        }

        // 2. Level preservation. A phase vocoder redistributes energy between
        //    bins, so exact unity is not the bar; 3 dB is. Unison is checked
        //    tighter because there the overlap-add normalisation is the only
        //    thing acting and any error in it shows up directly.
        {
            const double inLevel = 0.5 / std::sqrt(2.0);   // RMS of a 0.5 sine
            for (float semis : {-12.0f, 0.0f, 12.0f}) {
                PhaseVocoderShifter ps;
                ps.prepare(11, 4);
                ps.setPitchRatio(PhaseVocoderShifter::ratioForSemitones(semis));
                auto out = runTone(ps, 4096);
                double db = 20.0 * std::log10(std::max(1e-12, rmsOf(out) / inLevel));
                double tol = (semis == 0.0f) ? 1.0 : 3.0;
                r.checkVal(std::abs(db) <= tol,
                           juce::String("pitch-core: ") + juce::String(semis, 0)
                               + " semitones preserves level within "
                               + juce::String(tol, 0) + " dB (dB change)",
                           db);
            }
        }

        // 3. No block-rate structure. This is the exact measurement that
        //    condemned the old resamplers: they scored 0.00000 here because the
        //    tail of every block was silence. A continuous effect scores ~0.25.
        {
            PhaseVocoderShifter ps;
            ps.prepare(11, 4);
            ps.setPitchRatio(2.0f);
            auto out = runTone(ps, BS * 12);
            double tailE = 0, totalE = 0;
            for (int b = 0; b * BS < (int)out.size(); ++b) {
                for (int i = 0; i < BS; ++i) {
                    double e = (double)out[(size_t)(b * BS + i)] * out[(size_t)(b * BS + i)];
                    totalE += e;
                    if (i >= (BS * 3) / 4) tailE += e;
                }
            }
            double frac = totalE > 1e-20 ? tailE / totalE : 0.0;
            r.checkVal(std::abs(frac - 0.25) <= 0.05,
                       "pitch-core: +12 semitones spreads energy evenly across the block "
                       "(last-quarter energy fraction, 0.25 = uniform)",
                       frac);
        }

        // 4. Block-size independence. process() is a sample-driven FIFO, so a
        //    stream chopped into ragged blocks must produce the same samples as
        //    the same stream in one call. Callers get arbitrary block sizes from
        //    the host, and PDC assumes a fixed latency regardless.
        {
            const int TOTAL = 8192;
            std::vector<float> in((size_t)TOTAL);
            for (int i = 0; i < TOTAL; ++i)
                in[(size_t)i] = 0.5f * (float)std::sin(6.28318530718 * inHz * i / SR);

            PhaseVocoderShifter a, b;
            a.prepare(11, 4); b.prepare(11, 4);
            a.setPitchRatio(1.5f); b.setPitchRatio(1.5f);

            std::vector<float> oneShot((size_t)TOTAL), ragged((size_t)TOTAL);
            a.process(in.data(), oneShot.data(), TOTAL);

            const int chunks[] = { 1, 7, 64, 333, 512, 1000 };
            int pos = 0, ci = 0;
            while (pos < TOTAL) {
                int n = std::min(chunks[ci % 6], TOTAL - pos);
                b.process(in.data() + pos, ragged.data() + pos, n);
                pos += n; ++ci;
            }
            double maxDiff = 0;
            for (int i = 0; i < TOTAL; ++i)
                maxDiff = std::max(maxDiff, (double)std::abs(oneShot[(size_t)i] - ragged[(size_t)i]));
            r.checkVal(maxDiff < 1e-6,
                       "pitch-core: ragged block sizes give bit-comparable output to one "
                       "big call (max sample difference)",
                       maxDiff);
        }

        // 5. Allocation-freedom on the audio thread. Capacity is the observable
        //    proxy: if any internal buffer grew, process() called the allocator.
        //    Checked across a ratio change and a transient trigger too, since
        //    those are the other things a caller does mid-stream.
        {
            PhaseVocoderShifter ps;
            ps.prepare(11, 4);
            ps.setPitchRatio(1.0f);
            std::vector<float> in((size_t)BS, 0.0f), out((size_t)BS);
            for (int i = 0; i < BS; ++i)
                in[(size_t)i] = 0.25f * (float)std::sin(6.28318530718 * 220.0 * i / SR);
            ps.process(in.data(), out.data(), BS);      // warm up, settle capacities

            const size_t before = ps.capacityBytes();
            for (int b = 0; b < 200; ++b) {
                ps.setPitchRatio(1.0f + 0.5f * (float)((b % 5) - 2) * 0.4f);
                if (b % 17 == 0) ps.triggerTransient();
                ps.process(in.data(), out.data(), BS);
            }
            const size_t after = ps.capacityBytes();
            r.checkVal(after == before,
                       "pitch-core: 200 blocks with ratio changes and transient triggers "
                       "allocate nothing (buffer capacity growth in bytes)",
                       (double)after - (double)before);
        }

        // 6. Stability: no NaN/Inf, and silence in gives silence out. A phase
        //    vocoder divides nothing, but atan2 on an all-zero bin and the phase
        //    integrator are both places where garbage could creep in and then
        //    persist forever in the accumulators.
        {
            PhaseVocoderShifter ps;
            ps.prepare(11, 4);
            ps.setPitchRatio(1.7f);
            std::vector<float> in((size_t)BS, 0.0f), out((size_t)BS);
            double worst = 0;
            bool finite = true;
            for (int b = 0; b < 40; ++b) {
                ps.process(in.data(), out.data(), BS);
                for (float x : out) {
                    if (!std::isfinite(x)) finite = false;
                    worst = std::max(worst, (double)std::abs(x));
                }
            }
            r.check(finite, "pitch-core: silence in stays finite");
            r.checkVal(worst < 1e-6,
                       "pitch-core: silence in gives silence out (peak output)", worst);
        }

        // 7. CPU budget, same 10x-realtime bar the wavelet effects are held to.
        //    Measured for a STEREO pair, because that is what a node actually
        //    instantiates - a mono figure would flatter it by 2x. Best-of-3:
        //    machine interference only ever makes a run slower, so the minimum
        //    time is a far more stable estimator than a mean.
        {
            PhaseVocoderShifter l, rr;
            l.prepare(11, 4); rr.prepare(11, 4);
            l.setPitchRatio(1.5f); rr.setPitchRatio(1.5f);
            std::vector<float> in((size_t)BS), outL((size_t)BS), outR((size_t)BS);
            for (int i = 0; i < BS; ++i)
                in[(size_t)i] = 0.3f * (float)std::sin(6.28318530718 * 330.0 * i / SR);

            const int BLOCKS = 200;
            l.process(in.data(), outL.data(), BS);      // warm-up, untimed
            double best = 0.0;
            for (int rep = 0; rep < 3; ++rep) {
                auto t0 = std::chrono::steady_clock::now();
                for (int b = 0; b < BLOCKS; ++b) {
                    l.process(in.data(), outL.data(), BS);
                    rr.process(in.data(), outR.data(), BS);
                }
                auto t1 = std::chrono::steady_clock::now();
                double wall  = std::chrono::duration<double>(t1 - t0).count();
                double audio = (double)(BLOCKS * BS) / SR;
                best = std::max(best, wall > 1e-9 ? audio / wall : 1e9);
            }
            r.checkVal(best >= 10.0,
                       "pitch-core: stereo pair runs at >=10x realtime (realtime factor)",
                       best);
        }

        // 8. Formant preservation.
        //
        //    Plain pitch shifting scales the whole spectrum, envelope included,
        //    so a voice shifted up an octave gets its formants dragged up too -
        //    the chipmunk sound. Preservation is supposed to move the pitch and
        //    leave the timbre alone.
        //
        //    Test signal is a crude voice: a 200 Hz pulse train (the excitation,
        //    which carries the PITCH) through a fixed resonator at 1500 Hz (the
        //    formant, which carries the TIMBRE). Shifting up an octave must move
        //    the 200 Hz to 400 Hz in both modes; what distinguishes the modes is
        //    whether the 1500 Hz resonance moves with it.
        //
        //    The statistic is the spectral centroid, which tracks where the
        //    envelope sits without needing to resolve individual formants.
        {
            const int NSIG = 32768;

            auto makeVoice = [&](std::vector<float>& dst) {
                dst.assign((size_t)NSIG, 0.0f);
                // Two-pole resonator at 1500 Hz, driven by a 200 Hz pulse train.
                const double f0 = 1500.0, q = 12.0;
                const double w = 6.28318530718 * f0 / SR;
                const double rr2 = std::exp(-w / (2.0 * q));
                const double a1 = -2.0 * rr2 * std::cos(w), a2 = rr2 * rr2;
                double y1 = 0, y2 = 0;
                const int period = (int)(SR / 200.0);
                for (int i = 0; i < NSIG; ++i) {
                    const double x = (i % period == 0) ? 1.0 : 0.0;
                    const double y = x - a1 * y1 - a2 * y2;
                    y2 = y1; y1 = y;
                    dst[(size_t)i] = (float)(y * 0.3);
                }
            };

            // Spectral centroid over 100 Hz - 8 kHz, magnitude-weighted.
            auto centroidHz = [&](const std::vector<float>& v, int from, int count) {
                const int N = 8192;
                if (from + N > (int)v.size()) return 0.0;
                FFT f(N);
                std::vector<FFT::cplx> spec((size_t)N);
                std::vector<float> win((size_t)N);
                for (int i = 0; i < N; ++i)
                    win[(size_t)i] = v[(size_t)(from + i)]
                        * 0.5f * (1.0f - std::cos(6.28318530718f * (float)i / (float)N));
                f.forwardReal(win.data(), spec.data());
                double num = 0, den = 0;
                const int kLo = (int)(100.0 * N / SR), kHi = (int)(8000.0 * N / SR);
                for (int k = kLo; k <= kHi; ++k) {
                    const double m = std::abs(spec[(size_t)k]);
                    const double hz = (double)k * SR / N;
                    num += m * hz; den += m;
                }
                (void)count;
                return den > 0 ? num / den : 0.0;
            };

            std::vector<float> voice;
            makeVoice(voice);

            auto shiftVoice = [&](bool formant) {
                PhaseVocoderShifter ps;
                ps.prepare(11, 4, SR);
                ps.setFormantPreserve(formant);
                ps.setPitchRatio(2.0f);
                std::vector<float> out((size_t)NSIG, 0.0f);
                ps.process(voice.data(), out.data(), NSIG);
                return out;
            };

            const int SKIP = 8192;               // past the ramp-up
            const double cIn   = centroidHz(voice, SKIP, 0);
            auto plain  = shiftVoice(false);
            auto formed = shiftVoice(true);
            const double cPlain = centroidHz(plain,  SKIP, 0);
            const double cForm  = centroidHz(formed, SKIP, 0);

            r.checkVal(cIn > 0 && cPlain / cIn >= 1.5,
                       "pitch-core: WITHOUT formant preservation, +12 drags the spectral "
                       "centroid up with the pitch (out/in centroid ratio, ~2 = formants "
                       "moved an octave)",
                       cIn > 0 ? cPlain / cIn : 0.0);
            r.checkVal(cIn > 0 && cForm / cIn <= 1.25,
                       "pitch-core: WITH formant preservation, +12 leaves the spectral "
                       "centroid put (out/in centroid ratio, ~1 = timbre preserved)",
                       cIn > 0 ? cForm / cIn : 0.0);
            r.checkVal(cPlain > cForm * 1.3,
                       "pitch-core: formant preservation makes a large, unambiguous "
                       "difference (centroid ratio between the two modes)",
                       cForm > 0 ? cPlain / cForm : 0.0);

            // The pitch must still shift by the full octave in formant mode -
            // an implementation that "preserves formants" by simply shifting
            // less would pass the centroid test above and be useless.
            {
                PhaseVocoderShifter ps;
                ps.prepare(11, 4, SR);
                ps.setFormantPreserve(true);
                ps.setPitchRatio(2.0f);
                std::vector<float> in((size_t)BS), tmp((size_t)BS), out;
                const int skip = ps.latencySamples() + 4 * ps.fftLength();
                int phase = 0;
                while ((int)out.size() < skip + 16384) {
                    for (int i = 0; i < BS; ++i)
                        in[(size_t)i] = 0.5f * (float)std::sin(6.28318530718 * inHz * (phase + i) / SR);
                    ps.process(in.data(), tmp.data(), BS);
                    out.insert(out.end(), tmp.begin(), tmp.end());
                    phase += BS;
                }
                const double hz = dominantHz(out.data() + skip, 16384);
                const double cents = 1200.0 * std::log2(hz / (inHz * 2.0));
                r.checkVal(std::abs(cents) <= 25.0,
                           "pitch-core: formant preservation still shifts the pitch a full "
                           "octave (cents error vs 880 Hz)", cents);
            }

            // Formant mode costs two extra FFTs per frame, so it gets its own
            // CPU measurement rather than inheriting the plain-mode figure.
            {
                PhaseVocoderShifter l, rr;
                l.prepare(11, 4, SR); rr.prepare(11, 4, SR);
                l.setFormantPreserve(true); rr.setFormantPreserve(true);
                l.setPitchRatio(1.5f); rr.setPitchRatio(1.5f);
                std::vector<float> in((size_t)BS), outL((size_t)BS), outR((size_t)BS);
                for (int i = 0; i < BS; ++i)
                    in[(size_t)i] = 0.3f * (float)std::sin(6.28318530718 * 330.0 * i / SR);
                const int BLOCKS = 200;
                l.process(in.data(), outL.data(), BS);   // warm-up, untimed
                double best = 0.0;
                for (int rep = 0; rep < 3; ++rep) {
                    auto t0 = std::chrono::steady_clock::now();
                    for (int b = 0; b < BLOCKS; ++b) {
                        l.process(in.data(), outL.data(), BS);
                        rr.process(in.data(), outR.data(), BS);
                    }
                    auto t1 = std::chrono::steady_clock::now();
                    double wall = std::chrono::duration<double>(t1 - t0).count();
                    best = std::max(best, wall > 1e-9 ? (double)(BLOCKS * BS) / SR / wall : 1e9);
                }
                r.checkVal(best >= 10.0,
                           "pitch-core: stereo pair with formant preservation runs at "
                           ">=10x realtime (realtime factor)", best);
            }

            // Allocation-freedom must hold in formant mode too - it adds two
            // buffers, and they are sized in prepare() precisely so that
            // toggling the flag mid-stream cannot allocate on the audio thread.
            {
                PhaseVocoderShifter ps;
                ps.prepare(11, 4, SR);
                std::vector<float> in((size_t)BS), out((size_t)BS);
                for (int i = 0; i < BS; ++i)
                    in[(size_t)i] = 0.2f * (float)std::sin(6.28318530718 * 220.0 * i / SR);
                ps.process(in.data(), out.data(), BS);
                const size_t before = ps.capacityBytes();
                for (int b = 0; b < 200; ++b) {
                    ps.setFormantPreserve((b / 10) % 2 == 0);   // toggle mid-stream
                    ps.setPitchRatio(0.5f + 0.01f * (float)(b % 100));
                    ps.process(in.data(), out.data(), BS);
                }
                const double grew = (double)ps.capacityBytes() - (double)before;
                r.checkVal(grew == 0.0,
                           "pitch-core: toggling formant preservation mid-stream allocates "
                           "nothing (capacity growth, bytes)", grew);
            }
        }
    }

    // ---- Bucket C: whole-buffer spectral / wavelet warps ----------------
    // The scripting primitives behind spectralwarp()/waveletwarp() in Lua /
    // Python / WASM. They transform the buffer into a representation, warp each
    // bin/coefficient via warpAmpValue (single source of truth), and transform
    // back - so amount 0 is ~identity, a hot amount changes the buffer, length
    // is preserved, and the result stays finite.
    {
        auto makeSine = [](int n) {
            std::vector<float> b((size_t)n);
            for (int i = 0; i < n; ++i)
                b[(size_t)i] = std::sin(6.28318530718f * (float)i / (float)n);
            return b;
        };
        auto maxAbsDiff = [](const std::vector<float>& a, const std::vector<float>& b) {
            double d = 0.0;
            size_t n = std::min(a.size(), b.size());
            for (size_t i = 0; i < n; ++i) d = std::max(d, (double)std::abs(a[i] - b[i]));
            return d;
        };
        auto finite = [](const std::vector<float>& b) {
            for (float v : b) if (!std::isfinite(v)) return false;
            return true;
        };

        // --- Spectral ---
        std::vector<float> base = makeSine(512);
        std::vector<float> sp0 = base;
        spectralWarpBuffer(sp0, WarpMethod::SoftClip, 0.0f);
        r.check(sp0.size() == base.size(),
                "buffer-warp: spectralwarp preserves length");
        r.checkVal(maxAbsDiff(sp0, base) < 1e-3,
                   "buffer-warp: spectralwarp at amount 0 is ~identity",
                   maxAbsDiff(sp0, base));
        std::vector<float> sp1 = base;
        spectralWarpBuffer(sp1, WarpMethod::Wavefold, 0.9f);
        r.check(finite(sp1), "buffer-warp: spectralwarp stays finite");
        r.checkVal(maxAbsDiff(sp1, base) > 1e-4,
                   "buffer-warp: spectralwarp at a hot amount changes the buffer",
                   maxAbsDiff(sp1, base));

        // An unknown method is identity (the warpAmpValue contract).
        std::vector<float> spNone = base;
        spectralWarpBuffer(spNone, WarpMethod::None, 1.0f);
        r.checkVal(maxAbsDiff(spNone, base) < 1e-3,
                   "buffer-warp: spectralwarp with None is identity",
                   maxAbsDiff(spNone, base));

        // --- Wavelet ---
        std::vector<float> wv0 = base;
        waveletWarpBuffer(wv0, WarpMethod::SoftClip, 0.0f, "db4", 5);
        r.check(wv0.size() == base.size(),
                "buffer-warp: waveletwarp preserves length");
        r.checkVal(maxAbsDiff(wv0, base) < 1e-3,
                   "buffer-warp: waveletwarp at amount 0 is ~identity",
                   maxAbsDiff(wv0, base));
        std::vector<float> wv1 = base;
        waveletWarpBuffer(wv1, WarpMethod::Wavefold, 0.9f, "db4", 5);
        r.check(finite(wv1), "buffer-warp: waveletwarp stays finite");
        r.checkVal(maxAbsDiff(wv1, base) > 1e-4,
                   "buffer-warp: waveletwarp at a hot amount changes the buffer",
                   maxAbsDiff(wv1, base));

        // Too-short buffers are a no-op rather than a crash.
        std::vector<float> tiny = { 0.5f };
        spectralWarpBuffer(tiny, WarpMethod::Wavefold, 1.0f);
        waveletWarpBuffer(tiny, WarpMethod::Wavefold, 1.0f, "db4", 5);
        r.check(tiny.size() == 1 && std::abs(tiny[0] - 0.5f) < 1e-6f,
                "buffer-warp: 1-sample buffer is left untouched");
    }
}

} // namespace

// ---------------------------------------------------------------------------
// GLSL headless compute backend. Proves we can stand up an offscreen GL 4.3
// core context with no window, dispatch a compute shader, and read an SSBO
// back. Gated on GL availability: a machine with no GPU/driver capable of 4.3
// core (e.g. a headless CI box with a software rasteriser) reports the reason
// and the GLSL checks are skipped rather than failed.
// ---------------------------------------------------------------------------
void testGlslCompute(Report& r, const juce::File&) {
    r.section("GLSL compute backend (headless GL 4.3)");

    // remapGlslErrorLog is pure string processing (no GL needed) so it runs even
    // on machines with no GL 4.3 driver. It rewrites driver info-log line numbers
    // from the generated-shader space back to the user's own source. With 6
    // generated lines above the body and a 3-line body, generated lines 7..9 map
    // to user lines 1..3; lines inside the wrapper (<=6) or past the body (>9)
    // are left untouched. Both NVIDIA "0(L)" and AMD/Mesa "0:L:" forms remap.
    {
        r.check(remapGlslErrorLog("0(7) : error C0000: syntax error", 6, 3)
                    == "0(1) : error C0000: syntax error",
                "remapGlslErrorLog: NVIDIA line 7 -> user line 1");
        r.check(remapGlslErrorLog("ERROR: 0:8: 'x' : undeclared identifier", 6, 3)
                    == "ERROR: 0:2: 'x' : undeclared identifier",
                "remapGlslErrorLog: AMD line 8 -> user line 2");
        r.check(remapGlslErrorLog("0(3) : error", 6, 3) == "0(3) : error",
                "remapGlslErrorLog: wrapper line (<=offset) left unchanged");
        r.check(remapGlslErrorLog("0(10) : error", 6, 3) == "0(10) : error",
                "remapGlslErrorLog: line past user body left unchanged");
    }

    std::string why;
    bool avail = glslComputeAvailable(&why);
    if (!avail) {
        r.note("GL 4.3 compute unavailable on this machine: " + juce::String(why));
        r.note("GLSL generator backend checks skipped (not a failure).");
        return;
    }
    r.check(true, "headless GL 4.3 core context created");

    // 1) Trivial ramp: data[gid] = gid / total. Verifies dispatch coverage +
    //    SSBO readback for a non-multiple-of-64 size (forces a partial group).
    {
        const int total = 100; // not a multiple of 64
        const char* src =
            "#version 430\n"
            "layout(local_size_x = 64) in;\n"
            "layout(std430, binding = 0) buffer Out { float data[]; };\n"
            "uniform int uTotal;\n"
            "void main() {\n"
            "  uint gid = gl_GlobalInvocationID.x;\n"
            "  if (gid >= uint(uTotal)) return;\n"
            "  data[gid] = float(gid) / float(uTotal);\n"
            "}\n";
        auto res = glslDispatchCompute(src, total, { { "uTotal", { total } } });
        bool ok = r.check(res.ok, "ramp shader dispatched + read back");
        if (!ok) r.note("error: " + juce::String(res.error));
        if (ok) {
            double maxErr = 0.0;
            for (int i = 0; i < total; ++i)
                maxErr = std::max(maxErr, std::abs((double) res.data[i] - (double) i / total));
            r.checkVal(maxErr < 1e-6, "ramp values exact (incl. partial workgroup)", maxErr);
        }
    }

    // 2) 2D unflatten via uniforms: data[gid] encodes column index col/(W-1),
    //    proving int-array uniform plumbing (uDims) and row-major unflattening.
    {
        const int W = 7, H = 5, total = W * H;
        const char* src =
            "#version 430\n"
            "layout(local_size_x = 64) in;\n"
            "layout(std430, binding = 0) buffer Out { float data[]; };\n"
            "uniform int uDims[2];\n"
            "uniform int uTotal;\n"
            "void main() {\n"
            "  uint gid = gl_GlobalInvocationID.x;\n"
            "  if (gid >= uint(uTotal)) return;\n"
            "  int col = int(gid) % uDims[1];\n"      // dims = {H, W} row-major
            "  data[gid] = float(col) / float(uDims[1] - 1);\n"
            "}\n";
        auto res = glslDispatchCompute(src, total,
                                       { { "uDims", { H, W } }, { "uTotal", { total } } });
        bool ok = r.check(res.ok, "2D uniform-array shader dispatched");
        if (!ok) r.note("error: " + juce::String(res.error));
        if (ok) {
            double maxErr = 0.0;
            for (int row = 0; row < H; ++row)
                for (int col = 0; col < W; ++col) {
                    double exp = (double) col / (W - 1);
                    maxErr = std::max(maxErr, std::abs((double) res.data[row * W + col] - exp));
                }
            r.checkVal(maxErr < 1e-6, "2D column ramp exact via uDims uniform", maxErr);
        }
    }

    // 3) Compile error reporting: a syntactically broken shader must come back
    //    ok=false with a non-empty error log (not a crash).
    {
        const char* bad =
            "#version 430\n"
            "layout(local_size_x = 64) in;\n"
            "layout(std430, binding = 0) buffer Out { float data[]; };\n"
            "void main() { data[0] = ; }\n"; // syntax error
        auto res = glslDispatchCompute(bad, 1);
        r.check(!res.ok && !res.error.empty(), "broken shader reports compile error (no crash)");
    }

    // 4) Multi-pass ping-pong (low level): a 1D "shift" stencil. Pass 0 seeds a
    //    single spike at index 2; each later pass copies the cell's LEFT neighbour
    //    from the previous pass's snapshot (prev[]), so the spike moves right by
    //    one cell per pass. With 4 passes total the spike lands at index 5. This
    //    exercises the two-buffer ping-pong, the swap, prev[] (binding 1), and the
    //    auto-set uPass uniform all at once.
    {
        const int total = 8, passes = 4;
        const char* src =
            "#version 430\n"
            "layout(local_size_x = 64) in;\n"
            "layout(std430, binding = 0) buffer Out  { float data[]; };\n"
            "layout(std430, binding = 1) buffer Prev { float prev[]; };\n"
            "uniform int uTotal;\n"
            "uniform int uPass;\n"
            "void main() {\n"
            "  uint gid = gl_GlobalInvocationID.x;\n"
            "  if (gid >= uint(uTotal)) return;\n"
            "  int i = int(gid);\n"
            "  if (uPass == 0) { data[gid] = (i == 2) ? 1.0 : 0.0; }\n"
            "  else {\n"
            "    int j = i - 1;\n"
            "    data[gid] = (j >= 0 && j < uTotal) ? prev[j] : 0.0;\n"
            "  }\n"
            "}\n";
        auto res = glslDispatchComputePingPong(src, total, passes, { { "uTotal", { total } } });
        bool ok = r.check(res.ok, "ping-pong shift dispatched (4 passes)");
        if (!ok) r.note("error: " + juce::String(res.error));
        if (ok) {
            double maxErr = 0.0;
            for (int i = 0; i < total; ++i) {
                double exp = (i == 5) ? 1.0 : 0.0;   // spike shifted 2 -> 5
                maxErr = std::max(maxErr, std::abs((double) res.data[i] - exp));
            }
            r.checkVal(maxErr < 1e-6, "spike moved to index 5 (ping-pong + uPass + prev[])", maxErr);
        }
    }

    // 5) End-to-end via Terrain::fillFromGlsl whole-grid + passes: this exercises
    //    the GENERATED whole-grid template (prev[]/prevAt()/neighbor()) and the
    //    [0,1]->[-1,1] output mapping. 4x4 grid; pass 0 seeds a spike at (row1,
    //    col1); each later pass copies the left-column neighbour (axis 1) from the
    //    previous pass, so the spike walks right one column per pass. 3 passes ->
    //    spike ends at (row1, col3) = flat index 7. Output is bipolar: spike +1,
    //    everything else -1.
    {
        SoundShop::Terrain t;
        std::vector<int> dims = { 4, 4 };
        const std::string body =
            "uint gid = gl_GlobalInvocationID.x;\n"
            "if (gid >= uint(uTotal)) return;\n"
            "int i = int(gid);\n"
            "if (uPass == 0) { data[gid] = (i == 5) ? 1.0 : 0.0; }\n"
            "else { data[gid] = prevAt(neighbor(i, 1, -1)); }\n";
        std::string err;
        bool ok = t.fillFromGlsl(body, /*wholeGrid=*/true, dims, err, /*passes=*/3);
        bool dispatched = r.check(ok, "fillFromGlsl whole-grid multi-pass (3 passes)");
        if (!dispatched) r.note("error: " + juce::String(err));
        if (dispatched) {
            const auto& d = t.getData();
            bool sizeOk = r.check(d.size() == 16, "multi-pass grid size = 16");
            if (sizeOk) {
                double maxErr = 0.0;
                for (int i = 0; i < 16; ++i) {
                    double exp = (i == 7) ? 1.0 : -1.0;  // spike (1,1)->(1,3)=idx7, bipolar
                    maxErr = std::max(maxErr, std::abs((double) d[(size_t) i] - exp));
                }
                r.checkVal(maxErr < 1e-6, "neighbor()/prevAt() shift exact, mapped to [-1,1]", maxErr);
            }
        }
    }

    // 6) Tailored flatten(): the generated whole-grid template emits a flatten()
    //    specialised to the terrain's rank and literal per-axis sizes. On a 3x4
    //    grid, flatten(coordAxis(i,0), coordAxis(i,1)) must round-trip to i for
    //    every cell, and out-of-range args must edge-clamp. Each cell writes 1.0
    //    iff its checks pass; any mismatch shows up as a -1.0 in the bipolar grid.
    {
        SoundShop::Terrain t;
        std::vector<int> dims = { 3, 4 };   // total 12; last axis varies fastest
        const std::string body =
            "uint gid = gl_GlobalInvocationID.x;\n"
            "if (gid >= uint(uTotal)) return;\n"
            "int i = int(gid);\n"
            "int r = coordAxis(i, 0);\n"
            "int c = coordAxis(i, 1);\n"
            "bool ok = (flatten(r, c) == i) && (DIM0 == 3) && (DIM1 == 4);\n"
            "if (i == 0) ok = ok && (flatten(-1, -1) == 0) && (flatten(999, 999) == uTotal - 1);\n"
            "data[gid] = ok ? 1.0 : 0.0;\n";
        std::string err;
        bool ok = t.fillFromGlsl(body, /*wholeGrid=*/true, dims, err, /*passes=*/1);
        bool dispatched = r.check(ok, "fillFromGlsl whole-grid with tailored flatten()");
        if (!dispatched) r.note("error: " + juce::String(err));
        if (dispatched) {
            const auto& d = t.getData();
            double minV = 2.0;
            for (float v : d) minV = std::min(minV, (double) v);
            // Every cell must be +1 (bipolar) => every flatten/DIM/clamp check passed.
            r.check(d.size() == 12 && minV > 0.5,
                    "flatten() round-trips every cell + clamps + DIMn constants correct");
        }
    }

    // 6) GLSL as a wavetable/curve language (bakeShapeExpr with ShapeLang::Glsl).
    //    Proves the curve bake path templates a per-sample GLSL body, runs it on
    //    the GPU, and reads the cycle back with the right variable contract.
    {
        // Waveshape (domainRadians=true): `x` sweeps [0,2*pi); compare to sin.
        std::vector<float> out; std::string err;
        bool ok = bakeShapeExpr(ShapeLang::Glsl, "sin(x)", /*domainRadians=*/true,
                                512, out, err);
        bool dispatched = r.check(ok && (int) out.size() == 512,
                                  "GLSL shape: sin(x) waveshape baked (512 samples)");
        if (!dispatched) r.note("error: " + juce::String(err));
        if (dispatched) {
            double maxErr = 0.0;
            for (int i = 0; i < 512; ++i) {
                double want = std::sin((double) i / 512.0 * 6.28318530717958648);
                maxErr = std::max(maxErr, std::abs((double) out[i] - want));
            }
            r.checkVal(maxErr < 1e-3, "GLSL shape: matches sin within tolerance", maxErr);
        }

        // domainRadians waveshapes clamp to [-1,1]: a body that overshoots stays bounded.
        ok = bakeShapeExpr(ShapeLang::Glsl, "3.0 * sin(x)", /*domainRadians=*/true,
                           256, out, err);
        if (r.check(ok && (int) out.size() == 256, "GLSL shape: overshoot bakes")) {
            double maxAbs = 0.0;
            for (float v : out) maxAbs = std::max(maxAbs, (double) std::abs(v));
            r.checkVal(maxAbs <= 1.0 + 1e-6, "GLSL shape: waveshape clamped to [-1,1]", maxAbs);
        }

        // Spectral curve (domainRadians=false): `f` is normalized [0,1], unclamped.
        ok = bakeShapeExpr(ShapeLang::Glsl, "f * f", /*domainRadians=*/false,
                           128, out, err);
        if (r.check(ok && (int) out.size() == 128, "GLSL curve: f*f spectral baked")) {
            double maxErr = 0.0;
            for (int i = 0; i < 128; ++i) {
                double pos = (double) i / 127.0;
                maxErr = std::max(maxErr, std::abs((double) out[i] - pos * pos));
            }
            r.checkVal(maxErr < 1e-3, "GLSL curve: f*f matches (unclamped, > -1)", maxErr);
        }

        // A multi-statement body that supplies its own `return`.
        ok = bakeShapeExpr(ShapeLang::Glsl,
                           "float s = sin(x) + 0.5 * sin(2.0 * x);\nreturn s * 0.5;",
                           /*domainRadians=*/true, 256, out, err);
        if (!r.check(ok && (int) out.size() == 256, "GLSL shape: multi-statement body bakes"))
            r.note("error: " + juce::String(err));

        // A syntactically broken body must surface a compile error (ok=false,
        // non-empty message) instead of silently baking garbage. The error text
        // is the driver info log with line numbers remapped to the user's source.
        err.clear();
        ok = bakeShapeExpr(ShapeLang::Glsl, "return sin(x", /*domainRadians=*/true,
                           128, out, err);
        r.check(!ok && !err.empty(),
                "GLSL shape: broken body reports compile error (not silent)");
    }
}

// GLSL-parity scalar builtins added to the WaveExprParser (Builtin language).
// Each is pure and shared by the real-time Script node and the offline bakes.
static void testBuiltinMath(Report& r) {
    r.section("Builtin language - GLSL-parity scalar math");
    auto eval = [](const char* e) {
        return WaveExprParser::evaluateAt(std::string(e), 0.0f, 0.0f);
    };
    auto approx = [&](const char* expr, double want, const char* label) {
        double got = (double) eval(expr);
        r.checkVal(std::abs(got - want) < 1e-4, label, got);
    };
    approx("mix(2, 4, 0.25)",            2.5,  "mix(a,b,t) lerps");
    approx("smoothstep(0, 1, 0.5)",      0.5,  "smoothstep midpoint = 0.5");
    approx("smoothstep(0, 1, 0)",        0.0,  "smoothstep at low edge = 0");
    approx("step(0.5, 0.4)",             0.0,  "step below edge = 0");
    approx("step(0.5, 0.6)",             1.0,  "step at/above edge = 1");
    approx("fract(2.25)",                0.25, "fract drops integer part");
    approx("sign(-3)",                  -1.0,  "sign of negative = -1");
    approx("sign(0)",                    0.0,  "sign of zero = 0");
    approx("mod(5, 3)",                  2.0,  "mod(5,3) = 2");
    approx("round(2.6)",                 3.0,  "round(2.6) = 3");
    approx("trunc(2.9)",                 2.0,  "trunc(2.9) = 2");
    approx("inversesqrt(4)",             0.5,  "inversesqrt(4) = 0.5");
    approx("degrees(radians(90))",      90.0,  "degrees/radians round-trip");
    approx("atan(1, 1)",  0.785398163,  "atan(y,x) = atan2 = pi/4");
    approx("atan(1)",     0.785398163,  "atan(1) = pi/4");
    approx("asin(1)",     1.570796327,  "asin(1) = pi/2");
    approx("acos(1)",                    0.0,  "acos(1) = 0");
    // Names must not collide with the prefix functions they extend.
    approx("sinh(0)",                    0.0,  "sinh(0) = 0 (not shadowed by sin)");
    approx("sign(2)",                    1.0,  "sign(2) = 1 (not shadowed by sin)");
    // Remaining GLSL exponential / inverse-hyperbolic / common builtins.
    approx("exp2(3)",                    8.0,  "exp2(3) = 8");
    approx("log2(8)",                    3.0,  "log2(8) = 3 (not shadowed by log)");
    approx("fma(2, 3, 4)",              10.0,  "fma(2,3,4) = 2*3+4 = 10");
    approx("asinh(0)",                   0.0,  "asinh(0) = 0 (not shadowed by asin)");
    approx("acosh(1)",                   0.0,  "acosh(1) = 0 (not shadowed by acos)");
    approx("atanh(0)",                   0.0,  "atanh(0) = 0 (not shadowed by atan)");
    approx("roundEven(2.5)",             2.0,  "roundEven(2.5) = 2 (half to even)");
    approx("roundEven(3.5)",             4.0,  "roundEven(3.5) = 4 (half to even)");
    approx("tanh(0)",                    0.0,  "tanh(0) = 0 (still distinct from atanh)");

    // Multi-statement Built-in program: assign named "wave objects" and combine
    // them. Must match the equivalent inline expression sample-for-sample, and
    // the bake must route a program (newlines/assignments) through runProgram.
    {
        std::vector<float> prog, inlineExpr; std::string err;
        bool ok1 = bakeShapeExpr(ShapeLang::Builtin,
                                 "a = sin(x)\nb = 0.5 * sin(3 * x)\na + b",
                                 /*domainRadians=*/true, 64, prog, err);
        bool ok2 = bakeShapeExpr(ShapeLang::Builtin,
                                 "sin(x) + 0.5 * sin(3 * x)",
                                 /*domainRadians=*/true, 64, inlineExpr, err);
        bool match = ok1 && ok2 && prog.size() == 64 && inlineExpr.size() == 64;
        if (match)
            for (int i = 0; i < 64; ++i)
                if (std::abs(prog[i] - inlineExpr[i]) > 1e-4f) { match = false; break; }
        r.check(match, "Builtin program (named waves summed) == inline expression");
    }

    // WaveExprParser::validate - structural checker that backs the Built-in
    // language's error strip / node badge (Stage 2 of the script-error feature).
    // It must accept every valid program but reject obvious structural mistakes,
    // and crucially must NOT flag unknown identifiers (the evaluator reads those
    // as 0 by design). The self-test doesn't otherwise exercise load(), so these
    // assertions are the regression guard for the validator's allowlist.
    {
        std::string err;
        auto ok  = [&](const char* p) { err.clear(); return WaveExprParser::validate(p, err); };
        auto bad = [&](const char* p) { err.clear(); return !WaveExprParser::validate(p, err) && !err.empty(); };

        // --- Valid programs must pass ---
        r.check(ok("sin(x) + 0.5*sin(3*x)"),            "validate: plain expression passes");
        r.check(ok("a = sin(x)\nb = 0.5*sin(3*x)\na+b"),"validate: multi-statement program passes");
        r.check(ok("waveform(\"square\", x)"),          "validate: quoted name literal passes");
        r.check(ok("init:\nphase = 0\nloop:\nnote(60,100,0.5)"), "validate: section headers + ':' pass");
        r.check(ok("gate>0 ? freq : 0"),                "validate: ternary / comparison ops pass");
        r.check(ok("foo + bar * baz"),                  "validate: unknown identifiers are NOT flagged");
        r.check(ok("   \n\t ; \n "),                    "validate: whitespace/separator-only passes");

        // --- Structural errors must be caught ---
        r.check(bad("sin(x"),                           "validate: unclosed '(' rejected");
        r.check(bad("sin(x))"),                         "validate: extra ')' rejected");
        r.check(bad("waveform(\"square, x)"),           "validate: unterminated string rejected");
        r.check(bad("x % 2"),                           "validate: out-of-grammar '%' rejected");
        r.check(bad("a @ b"),                           "validate: out-of-grammar '@' rejected");
        r.check(bad("x # comment"),                     "validate: out-of-grammar '#' rejected");

        // --- Quote-awareness: an illegal char INSIDE a literal is fine ---
        r.check(ok("waveform(\"50% duty\", x)"),        "validate: '%' inside a string literal is ignored");
    }

    // Python bake error reporting (script-error Stage 3). A Python shape bake
    // wraps the user's source in generated scaffolding, so a raw Python line
    // number points at machine code the user never sees. formatPythonError()
    // must map both SyntaxError and runtime-traceback lines back onto the user's
    // OWN 1-based line, and tag the message with the exception type. Guarded by
    // pythonAvailable() so the suite still runs on Python-less builds.
    if (ScriptEngine::pythonAvailable()) {
        ScriptEngine::instance().init();
        auto& eng = ScriptEngine::instance();
        std::vector<float> out;

        // Bare-expression runtime error -> user line 1.
        {
            std::string err;
            bool ok = eng.bakeShapeExpr("1 / 0", /*domainRadians=*/true, 16, out, err);
            r.check(!ok, "py-error: divide-by-zero bake fails");
            r.check(err.find("ZeroDivisionError") != std::string::npos,
                    "py-error: message names ZeroDivisionError");
            r.check(err.find("(line 1)") != std::string::npos,
                    "py-error: bare-expr runtime error maps to line 1");
        }

        // Multi-line runtime error -> user line 2 (the second of the user's lines).
        {
            std::string err;
            bool ok = eng.bakeShapeExpr("a = sin(x)\nreturn a / 0",
                                        /*domainRadians=*/true, 16, out, err);
            r.check(!ok, "py-error: multi-line runtime error bake fails");
            r.check(err.find("(line 2)") != std::string::npos,
                    "py-error: multi-line runtime error maps to line 2");
        }

        // Syntax error on the second user line -> reported at line 2.
        {
            std::string err;
            bool ok = eng.bakeShapeExpr("a = 1\nb = = 2\nreturn b",
                                        /*domainRadians=*/true, 16, out, err);
            r.check(!ok, "py-error: syntax-error bake fails");
            r.check(err.find("SyntaxError") != std::string::npos
                        || err.find("(line 2)") != std::string::npos,
                    "py-error: syntax error reported (line 2 / SyntaxError)");
        }

        // A clean program still bakes fine (no false-positive error tagging).
        {
            std::string err;
            bool ok = eng.bakeShapeExpr("sin(x)", /*domainRadians=*/true, 16, out, err);
            r.check(ok && err.empty(), "py-error: valid program bakes with no error");
        }
    }

    // Bucket A warp bindings: warpamp(method, x, amount) / warpphase(method,
    // phase, amount) must route to the SAME warpAmpValue/warpPhaseValue primitives
    // (the shared single source of truth), whether the method is given as a numeric
    // id or a readable name string. The expression parser evaluates with c0=c1=0,
    // so the literal arguments below are what actually drive the warp.
    {
        // Numeric id form (SoftClip == 1, BendPlus == 20).
        double ampNum = (double) eval("warpamp(1, 0.9, 1.0)");
        r.checkVal(std::abs(ampNum - (double)warpAmpValue(WarpMethod::SoftClip, 0.9f, 1.0f)) < 1e-4,
                   "warpamp(id): Builtin matches warpAmpValue(SoftClip)", ampNum);
        double phNum = (double) eval("warpphase(20, 0.5, 0.8)");
        r.checkVal(std::abs(phNum - (double)warpPhaseValue(WarpMethod::BendPlus, 0.5f, 0.8f)) < 1e-4,
                   "warpphase(id): Builtin matches warpPhaseValue(BendPlus)", phNum);

        // Name string form (case/space/punctuation tolerant via warpMethodFromName).
        double ampName = (double) eval("warpamp(\"soft clip\", 0.9, 1.0)");
        r.checkVal(std::abs(ampName - (double)warpAmpValue(WarpMethod::SoftClip, 0.9f, 1.0f)) < 1e-4,
                   "warpamp(name): \"soft clip\" resolves to SoftClip", ampName);
        double phName = (double) eval("warpphase(\"bend+\", 0.5, 0.8)");
        r.checkVal(std::abs(phName - (double)warpPhaseValue(WarpMethod::BendPlus, 0.5f, 0.8f)) < 1e-4,
                   "warpphase(name): \"bend+\" resolves to BendPlus", phName);

        // amount 0 is identity in both domains.
        r.checkVal(std::abs((double)eval("warpamp(3, 0.42, 0.0)") - 0.42) < 1e-4,
                   "warpamp: amount 0 is identity", (double)eval("warpamp(3, 0.42, 0.0)"));
        r.checkVal(std::abs((double)eval("warpphase(20, 0.42, 0.0)") - 0.42) < 1e-4,
                   "warpphase: amount 0 is identity", (double)eval("warpphase(20, 0.42, 0.0)"));
    }
}

// ---------------------------------------------------------------------------
// Pitch detection. Covers the two trackers (YIN + autocorrelation) used by the
// Pitch Detector node: detection accuracy on synthetic sines across the audible
// band, the window-implied low-frequency floor, and the log/linear 0..1 output
// mapping math the node applies to the detected frequency.
// ---------------------------------------------------------------------------
void testPitchDetect(Report& r) {
    r.section("Pitch detection (YIN / autocorrelation)");

    const double sr = 48000.0;
    auto makeSine = [&](float hz, int n) {
        std::vector<float> v(n);
        for (int i = 0; i < n; ++i)
            v[i] = std::sin(2.0 * juce::MathConstants<double>::pi * hz * i / sr);
        return v;
    };

    // Detection accuracy: a clean sine should be found within ~1% by both
    // algorithms across a few octaves. Window of 8192 at 48k resolves down to
    // ~12 Hz, so all these are comfortably inside the floor.
    for (float hz : { 110.0f, 220.0f, 440.0f, 880.0f, 1760.0f }) {
        auto sig = makeSine(hz, 8192);
        auto y = detectPitchYIN(sig.data(), (int)sig.size(), sr, 0.15f, 50.0f, 5000.0f);
        r.checkVal(y.frequencyHz > 0 && std::abs(y.frequencyHz - hz) / hz < 0.01,
                   "YIN detects " + juce::String(hz, 0) + " Hz within 1%", y.frequencyHz);
        auto a = detectPitchAutocorrelation(sig.data(), (int)sig.size(), sr, 50.0f, 5000.0f);
        r.checkVal(a.frequencyHz > 0 && std::abs(a.frequencyHz - hz) / hz < 0.01,
                   "Autocorr detects " + juce::String(hz, 0) + " Hz within 1%", a.frequencyHz);
    }

    // computeNoteAndCents: 440 Hz must map to MIDI 69 (A4) with ~0 cents.
    {
        auto sig = makeSine(440.0f, 8192);
        auto y = detectPitchYIN(sig.data(), (int)sig.size(), sr, 0.15f, 50.0f, 5000.0f);
        r.checkVal(y.midiNote == 69 && std::abs(y.centsOffset) < 5.0f,
                   "440 Hz -> MIDI 69 (A4), within 5 cents", y.centsOffset);
    }

    // Band guard: minHz >= maxHz returns an empty result (no detection).
    {
        auto sig = makeSine(440.0f, 8192);
        auto y = detectPitchYIN(sig.data(), (int)sig.size(), sr, 0.15f, 1000.0f, 1000.0f);
        r.check(y.frequencyHz == 0.0f, "YIN: minHz>=maxHz yields no detection");
    }

    // Output mapping math: the node maps detected Hz to 0..1 over [minHz,maxHz].
    // Logarithmic spacing puts the geometric mean at 0.5; linear puts the
    // arithmetic mean at 0.5. Verify both, plus the endpoints.
    {
        float minHz = 50.0f, maxHz = 2000.0f;
        auto logMap = [&](float f) {
            return std::log(f / minHz) / std::log(maxHz / minHz);
        };
        auto linMap = [&](float f) { return (f - minHz) / (maxHz - minHz); };
        float geo = std::sqrt(minHz * maxHz);          // 316.2 Hz
        float arith = 0.5f * (minHz + maxHz);          // 1025 Hz
        r.checkVal(std::abs(logMap(minHz) - 0.0f) < 1e-5, "log map: minHz -> 0", logMap(minHz));
        r.checkVal(std::abs(logMap(maxHz) - 1.0f) < 1e-5, "log map: maxHz -> 1", logMap(maxHz));
        r.checkVal(std::abs(logMap(geo) - 0.5f) < 1e-4, "log map: geo mean -> 0.5", logMap(geo));
        r.checkVal(std::abs(linMap(arith) - 0.5f) < 1e-4, "linear map: arith mean -> 0.5", linMap(arith));
    }

    // Window floor: a window too short to hold ~2 periods of a low note can't
    // resolve it. The node clamps Min Hz up to ~2*sr/window. With a 512-sample
    // window at 48k the floor is ~187 Hz, so 80 Hz must be rejected while a
    // larger window detects it.
    {
        auto low = makeSine(80.0f, 512);
        auto narrow = detectPitchYIN(low.data(), (int)low.size(), sr, 0.15f, 187.0f, 5000.0f);
        r.check(narrow.frequencyHz == 0.0f || narrow.frequencyHz >= 150.0f,
                "Short window cannot resolve below its floor");
        auto wide = detectPitchYIN(makeSine(80.0f, 16384).data(), 16384, sr, 0.15f, 50.0f, 5000.0f);
        r.checkVal(wide.frequencyHz > 0 && std::abs(wide.frequencyHz - 80.0f) / 80.0f < 0.02,
                   "Large window resolves 80 Hz within 2%", wide.frequencyHz);
    }
}

// ---------------------------------------------------------------------------
// Granular freeze voice - focus on the SingleCycle mode (autocorrelation
// period detection -> one repeating cycle). Verifies the frozen output is
// finite, non-silent, level-bounded, and PERIODIC at the source's true period
// (a stable single-cycle tone), and that it re-pitches with `ratio`.
// ---------------------------------------------------------------------------
void testGranularFreeze(Report& r) {
    r.section("Granular freeze (SingleCycle mode)");

    const double sr = 48000.0;
    const float  hz = 220.0f;
    const int    srcLen = (int)sr;                    // 1 s
    const int    period = (int)std::lround(sr / hz);  // ~218 samples

    // A mildly-complex periodic source (fundamental + a couple harmonics) so
    // the test exercises real period detection, not a trivial pure sine.
    std::vector<float> src((size_t)srcLen);
    for (int i = 0; i < srcLen; ++i) {
        const double t = 2.0 * juce::MathConstants<double>::pi * hz * i / sr;
        src[(size_t)i] = (float)(0.7 * std::sin(t)
                                 + 0.2 * std::sin(2.0 * t)
                                 + 0.1 * std::sin(3.0 * t));
    }

    // Render the frozen single cycle at native pitch (ratio == 1).
    GrainFreezeVoice voice;
    const int outN = 4800;
    std::vector<float> out((size_t)outN, 0.0f);
    for (int i = 0; i < outN; ++i)
        out[(size_t)i] = voice.process(src.data(), srcLen, /*grainLen*/ 480,
                                       /*windowStart*/ -1, /*windowLen*/ -1,
                                       /*grainCount*/ 0, /*fftSize*/ 0,
                                       /*xfade*/ 64, /*embeddedPitchHz*/ hz,
                                       /*srcRate*/ sr, /*deviceRate*/ sr,
                                       /*ratio*/ 1.0f,
                                       GranularFreezeMode::SingleCycle);

    r.check(allFinite(out), "SingleCycle output is finite");
    r.checkVal(rmsOf(out) > 0.1, "SingleCycle output is non-silent", rmsOf(out));
    r.checkVal(peakAbs(out) < 1.5f, "SingleCycle output is level-bounded",
               peakAbs(out));

    // Periodicity: the back half of the buffer (past loop warm-up) should match
    // itself shifted by one period - a stable single cycle. Correlate
    // out[n] vs out[n+period] over a window.
    {
        const int base = outN / 2;
        std::vector<double> a, b;
        for (int i = base; i + period < outN; ++i) {
            a.push_back((double)out[(size_t)i]);
            b.push_back((double)out[(size_t)(i + period)]);
        }
        const double corr = pearson(a, b);
        r.checkVal(corr > 0.95,
                   "SingleCycle output repeats at the source period (stable cycle)",
                   corr);
    }

    // Pitch tracking: at ratio 2 the loop advances twice as fast, so the output
    // period halves. Render a fresh voice and check the half-period correlation.
    {
        GrainFreezeVoice up;
        std::vector<float> outUp((size_t)outN, 0.0f);
        for (int i = 0; i < outN; ++i)
            outUp[(size_t)i] = up.process(src.data(), srcLen, 480, -1, -1, 0, 0,
                                          64, hz, sr, sr, /*ratio*/ 2.0f,
                                          GranularFreezeMode::SingleCycle);
        const int halfP = std::max(1, period / 2);
        const int base = outN / 2;
        std::vector<double> a, b;
        for (int i = base; i + halfP < outN; ++i) {
            a.push_back((double)outUp[(size_t)i]);
            b.push_back((double)outUp[(size_t)(i + halfP)]);
        }
        const double corr = pearson(a, b);
        r.check(allFinite(outUp), "SingleCycle (ratio 2) output is finite");
        r.checkVal(corr > 0.9,
                   "SingleCycle re-pitches: ratio 2 halves the output period",
                   corr);
    }
}

// ---------------------------------------------------------------------------
// FM synth per-operator AHDSR envelopes (node.opEnvelopes). Covers the
// ensureFmOpEnvelopes seed/migration helper (legacy "Op{i} A/D/S/R" params ->
// 4 AHDSR envelopes, with the old params stripped) and a save/load round-trip
// proving the 4 envelopes survive serialization with per-index fidelity.
// ---------------------------------------------------------------------------
void testFmOpEnvelopes(Report& r) {
    r.section("FM operator envelopes (AHDSR migration + round-trip)");

    // (1) Migration from a legacy project carrying "Op{i} A/D/S/R" params.
    {
        NodeGraph g;
        int id = g.addNode("FM", NodeType::Instrument, {}, {}).id;
        Node* n = g.findNode(id);
        n->script = "__fmsynth__";
        for (int i = 1; i <= 4; ++i) {
            std::string p = "Op" + std::to_string(i) + " ";
            n->params.push_back({p + "Ratio", (float)i, 0.1f, 16.0f});
            n->params.push_back({p + "Level", 0.5f, 0.0f, 1.0f});
            n->params.push_back({p + "A", 0.02f * i, 0.001f, 2.0f});
            n->params.push_back({p + "D", 0.15f, 0.001f, 5.0f});
            n->params.push_back({p + "S", 0.6f, 0.0f, 1.0f});
            n->params.push_back({p + "R", 0.4f, 0.001f, 10.0f});
        }
        ensureFmOpEnvelopes(*n);
        r.check(n->opEnvelopes.size() == 4, "migration builds 4 op envelopes");

        bool anyADSR = false, hasRatio = false, hasLevel = false;
        for (auto& p : n->params) {
            const char last = p.name.back();
            const char sep  = p.name.size() >= 2 ? p.name[p.name.size() - 2] : 0;
            if (p.name.rfind("Op", 0) == 0 && sep == ' ' &&
                (last == 'A' || last == 'D' || last == 'S' || last == 'R'))
                anyADSR = true;
            if (p.name == "Op1 Ratio") hasRatio = true;
            if (p.name == "Op3 Level") hasLevel = true;
        }
        r.check(!anyADSR, "migration strips legacy A/D/S/R params");
        r.check(hasRatio && hasLevel, "migration keeps Ratio/Level params");

        if (n->opEnvelopes.size() == 4) {
            // Op1 A=0.02s -> 20ms; S=0.6; R=0.4s -> 400ms; velSens forced to 0.
            r.checkVal(std::abs(n->opEnvelopes[0].attackMs - 20.0f) < 0.5,
                       "migrated attack matches legacy A param",
                       n->opEnvelopes[0].attackMs);
            r.checkVal(std::abs(n->opEnvelopes[0].sustain - 0.6f) < 0.01,
                       "migrated sustain matches legacy S param",
                       n->opEnvelopes[0].sustain);
            r.checkVal(std::abs(n->opEnvelopes[3].releaseMs - 400.0f) < 0.5,
                       "migrated release matches legacy R param",
                       n->opEnvelopes[3].releaseMs);
            r.check(n->opEnvelopes[0].velocitySensitivity == 0.0f,
                    "migrated op envelope has velocitySensitivity 0 (master applies velocity)");
        }

        ensureFmOpEnvelopes(*n);  // idempotent: already 4, no-op
        r.check(n->opEnvelopes.size() == 4, "ensureFmOpEnvelopes is idempotent");
    }

    // (2) Save/load round-trip preserves the 4 envelopes with per-index fidelity.
    {
        NodeGraph g;
        int id = g.addNode("FM", NodeType::Instrument, {}, {}).id;
        Node* n = g.findNode(id);
        n->script = "__fmsynth__";
        ensureFmOpEnvelopes(*n);   // seed 4 defaults
        for (int i = 0; i < 4; ++i) {
            n->opEnvelopes[(size_t)i].attackMs  = 5.0f + 10.0f * i;
            n->opEnvelopes[(size_t)i].decayMs   = 100.0f + 20.0f * i;
            n->opEnvelopes[(size_t)i].sustain   = 0.2f + 0.1f * i;
            n->opEnvelopes[(size_t)i].releaseMs = 200.0f + 50.0f * i;
        }
        std::string saved = ProjectFile::serializeForUndo(g);
        NodeGraph g2;
        bool ld = ProjectFile::loadFromString(saved, g2);
        r.check(ld, "FM project round-trip loads");
        Node* n2 = g2.findNode(id);
        r.check(n2 != nullptr, "FM node survives round-trip");
        if (n2) {
            r.check(n2->opEnvelopes.size() == 4,
                    "round-trip preserves 4 op envelopes");
            if (n2->opEnvelopes.size() == 4) {
                double err = 0;
                for (int i = 0; i < 4; ++i) {
                    err += std::abs(n2->opEnvelopes[(size_t)i].attackMs - (5.0f + 10.0f * i));
                    err += std::abs(n2->opEnvelopes[(size_t)i].releaseMs - (200.0f + 50.0f * i));
                    err += std::abs(n2->opEnvelopes[(size_t)i].sustain - (0.2f + 0.1f * i));
                }
                r.checkVal(err < 0.5,
                           "round-trip preserves per-op envelope values", err);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Project-level asset library ("stores"). Covers the data-model invariants:
// disjoint user id space, add+find, content-hash dedup, soft-delete, duplicate /
// update live-edit, and a save/load round-trip through writeProject/readProject.
// ---------------------------------------------------------------------------
void testAssetLibrary(Report& r) {
    r.section("Asset library (stores)");

    // ---- id allocation: user ids start at the disjoint base ----------------
    {
        AssetLibrary lib;
        r.check(AssetLibrary::kUserIdBase >= 1000000,
                "assets: user id base is in the high disjoint range");
        int a = lib.add(AssetKind::Waveform, "saw", "layered", "BODY_A");
        int b = lib.add(AssetKind::Waveform, "sine", "layered", "BODY_B");
        r.check(a == AssetLibrary::kUserIdBase, "assets: first id == kUserIdBase");
        r.check(b == a + 1, "assets: second id increments");
        r.check(lib.find(a) && lib.find(a)->name == "saw",
                "assets: find() returns the added entry by id");
        r.check(lib.find(99) == nullptr, "assets: find() of unknown id is null");
    }

    // ---- content-hash dedup: identical (kind,subType,payload) -> same hash --
    {
        AssetLibrary lib;
        int a = lib.add(AssetKind::Waveform, "name-one", "layered", "SAME");
        int b = lib.add(AssetKind::Waveform, "name-two", "layered", "SAME");
        r.check(lib.find(a)->contentHash == lib.find(b)->contentHash,
                "assets: same content -> same hash (name/id ignored)");
        // Different payload -> different hash.
        int c = lib.add(AssetKind::Waveform, "name-three", "layered", "DIFF");
        r.check(lib.find(a)->contentHash != lib.find(c)->contentHash,
                "assets: different payload -> different hash");
        // Different kind -> different hash even with identical subType/payload.
        int d = lib.add(AssetKind::Instrument, "name-four", "layered", "SAME");
        r.check(lib.find(a)->contentHash != lib.find(d)->contentHash,
                "assets: different kind -> different hash");
        // Field-boundary safety: subType/payload split must not alias.
        std::string h1 = AssetLibrary::computeHash(AssetKind::Waveform, "a", "bc");
        std::string h2 = AssetLibrary::computeHash(AssetKind::Waveform, "ab", "c");
        r.check(h1 != h2, "assets: hash respects subType/payload boundary");
        // findByHash resolves to a live, non-archived entry.
        const AssetEntry* hit = lib.findByHash(lib.find(a)->contentHash);
        r.check(hit != nullptr, "assets: findByHash finds a matching entry");
    }

    // ---- soft-delete: archive hides from list() but stays resolvable -------
    {
        AssetLibrary lib;
        int a = lib.add(AssetKind::AhdsrCurve, "env1", "", "P1");
        int b = lib.add(AssetKind::AhdsrCurve, "env2", "", "P2");
        r.check(lib.list(AssetKind::AhdsrCurve).size() == 2,
                "assets: list shows both before archiving");
        r.check(lib.archive(a), "assets: archive returns true for known id");
        r.check(lib.list(AssetKind::AhdsrCurve).size() == 1,
                "assets: archived entry hidden from default list");
        r.check(lib.list(AssetKind::AhdsrCurve, /*includeArchived*/true).size() == 2,
                "assets: includeArchived re-includes the archived entry");
        r.check(lib.find(a) != nullptr,
                "assets: archived entry still resolvable by id (refs stay valid)");
        r.check(lib.findByHash(lib.find(a)->contentHash) == nullptr,
                "assets: findByHash skips archived entries");
        r.check(lib.restore(a) && lib.list(AssetKind::AhdsrCurve).size() == 2,
                "assets: restore un-hides");
        (void) b;
    }

    // ---- duplicate + live update -------------------------------------------
    {
        AssetLibrary lib;
        int a = lib.add(AssetKind::MorphAlgorithm, "warpA", "chain", "OPS1");
        int dup = lib.duplicate(a, "warpA copy");
        r.check(dup != 0 && dup != a, "assets: duplicate yields a new id");
        r.check(lib.find(dup)->payload == "OPS1" && lib.find(dup)->name == "warpA copy",
                "assets: duplicate copies payload, takes new name");
        r.check(lib.find(dup)->contentHash == lib.find(a)->contentHash,
                "assets: duplicate shares content hash (same content)");
        // update() repoints payload in place (live edit) and rehashes.
        std::string oldHash = lib.find(a)->contentHash;
        r.check(lib.update(a, "chain", "OPS2"), "assets: update returns true");
        r.check(lib.find(a)->payload == "OPS2", "assets: update changed payload in place");
        r.check(lib.find(a)->contentHash != oldHash, "assets: update rehashed");
        r.check(lib.find(dup)->payload == "OPS1",
                "assets: duplicate is independent of the original after update");
    }

    // ---- save / load round-trip through project_file -----------------------
    {
        NodeGraph g;
        int w = g.assets.add(AssetKind::Waveform, "my wave", "layered", "WAVE_PAYLOAD\nline2");
        int inst = g.assets.add(AssetKind::Instrument, "my inst", "composite", "INST_PAYLOAD");
        int arch = g.assets.add(AssetKind::AhdsrCurve, "old env", "", "ENV");
        g.assets.archive(arch);
        // Star the waveform so the starred flag is exercised through persistence.
        r.check(g.assets.setStarred(w, true), "assets: setStarred returns true for known id");
        r.check(!g.assets.setStarred(123456789, true),
                "assets: setStarred returns false for unknown id");

        std::ostringstream oss;
        ProjectFile::writeProject(oss, g, nullptr, /*includeView*/false,
                                  /*includeBlobs*/true);
        std::string saved = oss.str();
        r.check(saved.find("[AssetStore]") != std::string::npos,
                "assets: save emits [AssetStore] sections");

        NodeGraph g2;
        std::istringstream iss(saved);
        ProjectFile::readProject(iss, g2, nullptr);
        // readProject re-seeds the code-owned built-in morph chains, which are not
        // part of this project's saved content - count only the user assets.
        size_t userAssetCount = 0;
        for (const auto& e : g2.assets.all())
            if (!(e.kind == AssetKind::MorphAlgorithm && isBuiltinMorphAssetId(e.id)))
                ++userAssetCount;
        r.check(userAssetCount == 3, "assets: all three entries round-trip");
        const AssetEntry* rw = g2.assets.find(w);
        r.check(rw && rw->name == "my wave" && rw->subType == "layered" &&
                    rw->payload == "WAVE_PAYLOAD\nline2",
                "assets: waveform payload (multi-line) survives round-trip");
        r.check(rw && rw->contentHash ==
                    AssetLibrary::computeHash(AssetKind::Waveform, "layered",
                                              "WAVE_PAYLOAD\nline2"),
                "assets: loaded hash matches recomputed content hash");
        const AssetEntry* ri = g2.assets.find(inst);
        r.check(ri && ri->kind == AssetKind::Instrument,
                "assets: instrument kind round-trips");
        const AssetEntry* ra = g2.assets.find(arch);
        r.check(ra && ra->archived, "assets: archived flag round-trips");
        r.check(rw && rw->starred, "assets: starred flag round-trips");
        r.check(ri && !ri->starred, "assets: unstarred default round-trips (no starred line)");
        // nextId must be bumped past the loaded ids so new allocs don't collide.
        r.check(g2.assets.allocId() > inst,
                "assets: load bumps nextId past all loaded ids");

        // Undo serialization must INCLUDE assets (store edits are undoable state).
        std::string snap = ProjectFile::serializeForUndo(g);
        r.check(snap.find("[AssetStore]") != std::string::npos,
                "assets: undo snapshot includes [AssetStore] (store edits undoable)");
    }

    // ---- AHDSR live-reference: two nodes share one stored curve -------------
    {
        NodeGraph g;
        // Capture ids immediately - addNode can reallocate g.nodes, so never
        // hold a Node& across a second addNode (would dangle).
        int aId = g.addNode("a", NodeType::TerrainSynth, {}, {}).id;
        int bId = g.addNode("b", NodeType::TerrainSynth, {}, {}).id;

        // Publish a curve from node A, then have both nodes reference it.
        g.findNode(aId)->ahdsrEnvelope.attackMs = 42.0f;
        int curve = g.assets.add(AssetKind::AhdsrCurve, "shared env", "",
                                 g.findNode(aId)->ahdsrEnvelope.encode());
        g.findNode(aId)->ahdsrAssetId = curve;
        g.findNode(bId)->ahdsrAssetId = curve;
        g.resolveAhdsrReferences();
        r.checkVal(std::abs(g.findNode(bId)->ahdsrEnvelope.attackMs - 42.0f) < 0.01f,
                   "assets: reference resolves stored curve into the node",
                   g.findNode(bId)->ahdsrEnvelope.attackMs);

        // Edit the shared curve -> propagates to every referencing node.
        AHDSREnvelope edited;
        edited.attackMs = 99.0f;
        g.assets.update(curve, "", edited.encode());
        g.resolveAhdsrReferences();
        r.checkVal(std::abs(g.findNode(aId)->ahdsrEnvelope.attackMs - 99.0f) < 0.01f &&
                   std::abs(g.findNode(bId)->ahdsrEnvelope.attackMs - 99.0f) < 0.01f,
                   "assets: editing the curve propagates to all references",
                   g.findNode(aId)->ahdsrEnvelope.attackMs);

        // Save/load preserves the reference id and re-resolves on load.
        std::ostringstream oss;
        ProjectFile::writeProject(oss, g, nullptr, false, true);
        NodeGraph g2;
        std::istringstream iss(oss.str());
        ProjectFile::readProject(iss, g2, nullptr);
        r.check(g2.findNode(aId) && g2.findNode(aId)->ahdsrAssetId == curve,
                "assets: ahdsrAssetId round-trips through save/load");
        r.checkVal(g2.findNode(aId) &&
                   std::abs(g2.findNode(aId)->ahdsrEnvelope.attackMs - 99.0f) < 0.01f,
                   "assets: load re-resolves referenced curve into the node",
                   g2.findNode(aId) ? g2.findNode(aId)->ahdsrEnvelope.attackMs : 0.0f);

        // Hard-deleting the curve makes referencing nodes fall back to independent.
        g.assets.erase(curve);
        g.resolveAhdsrReferences();
        r.check(g.findNode(aId)->ahdsrAssetId == -1 &&
                g.findNode(bId)->ahdsrAssetId == -1,
                "assets: deleted curve -> references fall back to independent");
    }

    // ---- Waveform live-reference: a wavetable library entry references a -----
    // ---- published Waveform asset (frame lives in node.script, resolved -----
    // ---- in place by the string-level resolver) -----------------------------
    {
        NodeGraph g;
        int nId = g.addNode("wt", NodeType::TerrainSynth, {}, {}).id;

        // Distinct LayeredWaveform frames keyed by harmonic ratio so their
        // encodeBody() strings differ - lets us assert resolution swapped them.
        auto makeLayered = [](int ratio) {
            auto lw = std::make_unique<LayeredWaveform>();
            WaveLayer ly; ly.shape = WaveLayer::Saw; ly.ratio = ratio;
            lw->layers.push_back(ly);
            return lw;
        };

        // Publish the asset frame (ratio 7).
        std::string subType, payload;
        { auto assetFrame = makeLayered(7);
          waveformAssetFromFrame(assetFrame.get(), subType, payload); }
        int wAsset = g.assets.add(AssetKind::Waveform, "shared wave", subType, payload);

        // Node wavetable: one library entry that starts as a DIFFERENT local
        // frame (ratio 2) but live-references the asset.
        WavetableDoc doc;
        int eid = doc.addLibraryEntry(makeLayered(2), "local");
        doc.library[doc.findLibraryIndexById(eid)].assetId = wAsset;
        g.findNode(nId)->script = doc.encode();

        // The assetId must survive the wavetable codec round-trip.
        WavetableDoc rt; rt.decode(g.findNode(nId)->script);
        r.check(rt.library.size() == 1 && rt.library[0].assetId == wAsset,
                "assets: waveform entry assetId round-trips through wavetable codec");

        // Resolve -> the entry's frame becomes the asset's frame (ratio 7).
        int nres = resolveWaveformReferences(g);
        r.checkVal(nres == 1,
                   "assets: resolveWaveformReferences resolves the one reference", nres);
        WavetableDoc after; after.decode(g.findNode(nId)->script);
        r.check(after.library.size() == 1 && after.library[0].wave &&
                    after.library[0].wave->encodeBody() == payload,
                "assets: resolved entry frame matches the published asset body");

        // Save/load preserves the reference id and re-resolves on load.
        std::ostringstream oss;
        ProjectFile::writeProject(oss, g, nullptr, false, true);
        NodeGraph g2; std::istringstream iss(oss.str());
        ProjectFile::readProject(iss, g2, nullptr);
        WavetableDoc loaded; loaded.decode(g2.findNode(nId)->script);
        r.check(loaded.library.size() == 1 && loaded.library[0].assetId == wAsset &&
                    loaded.library[0].wave &&
                    loaded.library[0].wave->encodeBody() == payload,
                "assets: waveform reference re-resolves after save/load");

        // Edit the asset -> propagates on next resolve (ratio 3).
        std::string sub2, pay2;
        { auto edited = makeLayered(3); waveformAssetFromFrame(edited.get(), sub2, pay2); }
        g.assets.update(wAsset, sub2, pay2);
        resolveWaveformReferences(g);
        WavetableDoc edDoc; edDoc.decode(g.findNode(nId)->script);
        r.check(edDoc.library.size() == 1 && edDoc.library[0].wave &&
                    edDoc.library[0].wave->encodeBody() == pay2,
                "assets: editing waveform asset propagates to the referencing node");

        // Erase the asset -> entry detaches to independent, keeps its last frame.
        g.assets.erase(wAsset);
        resolveWaveformReferences(g);
        WavetableDoc delDoc; delDoc.decode(g.findNode(nId)->script);
        r.check(delDoc.library.size() == 1 && delDoc.library[0].assetId == -1,
                "assets: erased waveform asset -> entry falls back to independent");
    }

    // ---- Per-layer Waveform live-reference: an individual WaveLayer inside a -
    // ---- LayeredWaveform entry references a published single-layer asset -----
    {
        NodeGraph g;
        int nId = g.addNode("wt", NodeType::TerrainSynth, {}, {}).id;

        auto makeLayer = [](int ratio) {
            WaveLayer ly; ly.shape = WaveLayer::Saw; ly.ratio = ratio; ly.amp = 1.0f;
            return ly;
        };

        // Publish a single-layer asset from a Saw ratio-7 layer.
        std::string subType, payload;
        layerToWaveformAsset(makeLayer(7), subType, payload);
        int wAsset = g.assets.add(AssetKind::Waveform, "shared layer", subType, payload);

        // Doc entry = a 2-layer stack; layer 1 references the asset but starts as a
        // different local shape (ratio 2) with a distinct amp (0.3) to prove amp
        // (a per-slot property) is preserved across resolves; layer 0 is untouched.
        auto lw = std::make_unique<LayeredWaveform>();
        lw->layers.push_back(makeLayer(1));
        { WaveLayer ref; ref.shape = WaveLayer::Saw; ref.ratio = 2; ref.amp = 0.3f;
          ref.assetId = wAsset; lw->layers.push_back(ref); }
        WavetableDoc doc;
        doc.addLibraryEntry(std::move(lw), "stack");
        g.findNode(nId)->script = doc.encode();

        // layer.assetId survives the codec round-trip.
        WavetableDoc rt; rt.decode(g.findNode(nId)->script);
        auto* rtlw = rt.library.empty() ? nullptr
                     : dynamic_cast<LayeredWaveform*>(rt.library[0].wave.get());
        r.check(rtlw && rtlw->layers.size() == 2 && rtlw->layers[1].assetId == wAsset,
                "assets: per-layer assetId round-trips through wavetable codec");

        // Resolve -> layer 1 becomes the asset's layer (ratio 7), amp preserved
        // (0.3); layer 0 untouched (ratio 1).
        int nres = resolvePerLayerWaveformReferences(g);
        r.checkVal(nres == 1,
                   "assets: resolvePerLayerWaveformReferences resolves one ref", nres);
        WavetableDoc after; after.decode(g.findNode(nId)->script);
        auto* alw = dynamic_cast<LayeredWaveform*>(after.library[0].wave.get());
        r.check(alw && alw->layers.size() == 2 &&
                    alw->layers[0].ratio == 1 && alw->layers[1].ratio == 7 &&
                    std::abs(alw->layers[1].amp - 0.3f) < 1e-6f,
                "assets: per-layer resolve pulls asset shape, preserves layer amp");

        // Save/load re-resolves.
        std::ostringstream oss;
        ProjectFile::writeProject(oss, g, nullptr, false, true);
        NodeGraph g2; std::istringstream iss(oss.str());
        ProjectFile::readProject(iss, g2, nullptr);
        WavetableDoc loaded; loaded.decode(g2.findNode(nId)->script);
        auto* llw = dynamic_cast<LayeredWaveform*>(loaded.library[0].wave.get());
        r.check(llw && llw->layers.size() == 2 && llw->layers[1].assetId == wAsset &&
                    llw->layers[1].ratio == 7,
                "assets: per-layer reference re-resolves after save/load");

        // Edit the asset -> propagates to the referencing layer (ratio 5).
        std::string sub2, pay2; layerToWaveformAsset(makeLayer(5), sub2, pay2);
        g.assets.update(wAsset, sub2, pay2);
        resolvePerLayerWaveformReferences(g);
        WavetableDoc edDoc; edDoc.decode(g.findNode(nId)->script);
        auto* elw = dynamic_cast<LayeredWaveform*>(edDoc.library[0].wave.get());
        r.check(elw && elw->layers.size() == 2 && elw->layers[1].ratio == 5,
                "assets: editing the asset propagates to the referencing layer");

        // Erase the asset -> the referencing layer detaches to independent.
        g.assets.erase(wAsset);
        resolvePerLayerWaveformReferences(g);
        WavetableDoc delDoc; delDoc.decode(g.findNode(nId)->script);
        auto* dlw = dynamic_cast<LayeredWaveform*>(delDoc.library[0].wave.get());
        r.check(dlw && dlw->layers.size() == 2 && dlw->layers[1].assetId == -1,
                "assets: erased asset -> referencing layer falls back to independent");
    }

    // ---- Desync / Unlink: detaching (assetId = -1) keeps the current content --
    // ---- and stops propagation FROM the still-existing asset. This is the -----
    // ---- data-model contract behind the three "Unlink from Library" buttons. --
    {
        NodeGraph g;
        int nId = g.addNode("wt", NodeType::TerrainSynth, {}, {}).id;

        auto makeLayered = [](int ratio) {
            auto lw = std::make_unique<LayeredWaveform>();
            WaveLayer ly; ly.shape = WaveLayer::Saw; ly.ratio = ratio; ly.amp = 1.0f;
            lw->layers.push_back(ly);
            return lw;
        };

        // Frame-scope: an entry live-linked to a Waveform asset, then unlinked.
        std::string subType, payload;
        { auto f = makeLayered(7); waveformAssetFromFrame(f.get(), subType, payload); }
        int wAsset = g.assets.add(AssetKind::Waveform, "shared", subType, payload);
        WavetableDoc doc;
        int eid = doc.addLibraryEntry(makeLayered(7), "linked");
        doc.library[doc.findLibraryIndexById(eid)].assetId = wAsset;
        g.findNode(nId)->script = doc.encode();
        resolveWaveformReferences(g);

        // Simulate the Unlink button: clear assetId, keep the frame as-is.
        WavetableDoc d1; d1.decode(g.findNode(nId)->script);
        d1.library[0].assetId = -1;
        g.findNode(nId)->script = d1.encode();

        // Now change the asset and re-resolve. The detached entry must NOT update.
        std::string sub2, pay2;
        { auto f2 = makeLayered(3); waveformAssetFromFrame(f2.get(), sub2, pay2); }
        g.assets.update(wAsset, sub2, pay2);
        resolveWaveformReferences(g);
        WavetableDoc d2; d2.decode(g.findNode(nId)->script);
        r.check(d2.library.size() == 1 && d2.library[0].assetId == -1 &&
                    d2.library[0].wave &&
                    d2.library[0].wave->encodeBody() == payload,
                "assets: frame Unlink detaches and stops propagation from the asset");

        // Per-layer scope: same contract on a single WaveLayer.
        std::string lsub, lpay;
        { WaveLayer ly; ly.shape = WaveLayer::Saw; ly.ratio = 7; ly.amp = 1.0f;
          layerToWaveformAsset(ly, lsub, lpay); }
        int lAsset = g.assets.add(AssetKind::Waveform, "shared layer", lsub, lpay);
        auto lw = std::make_unique<LayeredWaveform>();
        { WaveLayer ref; ref.shape = WaveLayer::Saw; ref.ratio = 7; ref.amp = 0.5f;
          ref.assetId = lAsset; lw->layers.push_back(ref); }
        WavetableDoc ldoc; ldoc.addLibraryEntry(std::move(lw), "stack");
        int lnId = g.addNode("wt2", NodeType::TerrainSynth, {}, {}).id;
        g.findNode(lnId)->script = ldoc.encode();
        resolvePerLayerWaveformReferences(g);

        // Unlink the layer, then mutate the asset + re-resolve.
        WavetableDoc l1; l1.decode(g.findNode(lnId)->script);
        auto* l1lw = dynamic_cast<LayeredWaveform*>(l1.library[0].wave.get());
        l1lw->layers[0].assetId = -1;
        g.findNode(lnId)->script = l1.encode();
        std::string lsub2, lpay2;
        { WaveLayer ly; ly.shape = WaveLayer::Saw; ly.ratio = 2; ly.amp = 1.0f;
          layerToWaveformAsset(ly, lsub2, lpay2); }
        g.assets.update(lAsset, lsub2, lpay2);
        resolvePerLayerWaveformReferences(g);
        WavetableDoc l2; l2.decode(g.findNode(lnId)->script);
        auto* l2lw = dynamic_cast<LayeredWaveform*>(l2.library[0].wave.get());
        r.check(l2lw && l2lw->layers.size() == 1 && l2lw->layers[0].assetId == -1 &&
                    l2lw->layers[0].ratio == 7,
                "assets: layer Unlink detaches and stops propagation from the asset");

        // Morph scope: detaching warpAssetId keeps the cached chain frozen.
        auto makeChain = [](std::vector<WarpMethod> methods) {
            std::vector<WarpOp> c;
            for (WarpMethod m : methods) {
                WarpOp op; op.method = m; op.amount = 0.5f; op.enabled = true;
                c.push_back(op);
            }
            return c;
        };
        const std::string mPayload = encodeWarpChain(
            makeChain({ WarpMethod::SoftClip, WarpMethod::HardClip }));
        int mAsset = g.assets.add(AssetKind::MorphAlgorithm, "shared morph",
                                  "", mPayload);
        WavetableDoc mdoc;
        int mfid = mdoc.addLibraryEntry(std::make_unique<LayeredWaveform>(), "w");
        mdoc.libraryFrameById(mfid)->morphChain =
            makeChain({ WarpMethod::SoftClip, WarpMethod::HardClip });
        mdoc.libraryFrameById(mfid)->morphAssetId = mAsset;
        int mnId = g.addNode("wt3", NodeType::TerrainSynth, {}, {}).id;
        g.findNode(mnId)->script = mdoc.encode();
        resolveWarpReferences(g);

        // Unlink: clear morphAssetId, keep the chain.
        WavetableDoc m1; m1.decode(g.findNode(mnId)->script);
        m1.libraryFrameById(mfid)->morphAssetId = -1;
        g.findNode(mnId)->script = m1.encode();
        g.assets.update(mAsset, "", encodeWarpChain(makeChain({ WarpMethod::Wavefold })));
        resolveWarpReferences(g);
        WavetableDoc m2; m2.decode(g.findNode(mnId)->script);
        const IWavetableFrame* m2f = m2.libraryFrameById(mfid);
        r.check(m2f && m2f->morphAssetId == -1 && m2f->morphChain.size() == 2 &&
                    encodeWarpChain(m2f->morphChain) == mPayload,
                "assets: morph Unlink detaches and stops propagation from the asset");
    }

    // ---- MorphAlgorithm (frame-scope warp chain) live reference -------------
    {
        NodeGraph g;
        int nId = g.addNode("wt", NodeType::TerrainSynth, {}, {}).id;

        auto makeChain = [](std::vector<WarpMethod> methods) {
            std::vector<WarpOp> c;
            for (WarpMethod m : methods) {
                WarpOp op; op.method = m; op.amount = 0.5f; op.enabled = true;
                c.push_back(op);
            }
            return c;
        };

        // Publish a 2-op morph algorithm asset.
        const std::string assetPayload = encodeWarpChain(
            makeChain({ WarpMethod::SoftClip, WarpMethod::HardClip }));
        int mAsset = g.assets.add(AssetKind::MorphAlgorithm, "shared morph",
                                  "", assetPayload);

        // Node wavetable: a frame whose morph chain is a stale 1-op cache that
        // live-references the 2-op asset.
        WavetableDoc doc;
        int fid = doc.addLibraryEntry(std::make_unique<LayeredWaveform>(), "w");
        doc.libraryFrameById(fid)->morphChain = makeChain({ WarpMethod::Wavefold }); // stale 1-op cache
        doc.libraryFrameById(fid)->morphAssetId = mAsset;
        g.findNode(nId)->script = doc.encode();

        // morphAssetId must survive the wavetable codec round-trip.
        WavetableDoc rt; rt.decode(g.findNode(nId)->script);
        const IWavetableFrame* rtf = rt.libraryFrameById(fid);
        r.check(rtf && rtf->morphAssetId == mAsset && rtf->morphChain.size() == 1,
                "assets: morphAssetId + cached chain round-trip through wavetable codec");

        // Resolve -> chain becomes the asset's 2-op chain, and the node gains
        // two reconciled morph modulation params.
        int nres = resolveWarpReferences(g);
        r.checkVal(nres == 1,
                   "assets: resolveWarpReferences resolves the one reference", nres);
        WavetableDoc after; after.decode(g.findNode(nId)->script);
        const IWavetableFrame* af = after.libraryFrameById(fid);
        r.check(af && af->morphChain.size() == 2 &&
                    encodeWarpChain(af->morphChain) == assetPayload,
                "assets: resolved warp chain matches the published asset");
        {
            int warpParams = 0;
            for (const auto& p : g.findNode(nId)->params)
                if (p.warpSlot >= 0) ++warpParams;
            r.checkVal(warpParams == 2,
                       "assets: resolved warp chain reconciles two warp-slot params",
                       warpParams);
            // Named morph params (inc 2): the slot params carry their method's
            // full name, not "Warp N". The asset chain is {SoftClip, HardClip},
            // two DIFFERENT methods, so each reads as its method name with no
            // disambiguating number ("Soft Clip", "Hard Clip").
            const Param* s0 = nullptr;
            const Param* s1 = nullptr;
            for (const auto& p : g.findNode(nId)->params) {
                if (p.warpSlot == 0) s0 = &p;
                else if (p.warpSlot == 1) s1 = &p;
            }
            r.check(s0 && s1 && s0->name == "Soft Clip" && s1->name == "Hard Clip",
                    "assets: warp slot params use method-name labels (Soft Clip / Hard Clip)");
        }

        // Save/load preserves the reference id and re-resolves on load.
        std::ostringstream oss;
        ProjectFile::writeProject(oss, g, nullptr, false, true);
        NodeGraph g2; std::istringstream iss(oss.str());
        ProjectFile::readProject(iss, g2, nullptr);
        WavetableDoc loaded; loaded.decode(g2.findNode(nId)->script);
        const IWavetableFrame* lf = loaded.libraryFrameById(fid);
        r.check(lf && lf->morphAssetId == mAsset && lf->morphChain.size() == 2 &&
                    encodeWarpChain(lf->morphChain) == assetPayload,
                "assets: warp reference re-resolves after save/load");

        // Edit the asset (now 3 ops) -> propagates on next resolve.
        const std::string pay2 = encodeWarpChain(makeChain(
            { WarpMethod::SoftClip, WarpMethod::HardClip, WarpMethod::Wavefold }));
        g.assets.update(mAsset, "", pay2);
        resolveWarpReferences(g);
        WavetableDoc edDoc; edDoc.decode(g.findNode(nId)->script);
        const IWavetableFrame* ef = edDoc.libraryFrameById(fid);
        r.check(ef && ef->morphChain.size() == 3 && encodeWarpChain(ef->morphChain) == pay2,
                "assets: editing morph asset propagates to the referencing node");

        // Erase the asset -> frame detaches to independent, keeps its last chain.
        g.assets.erase(mAsset);
        resolveWarpReferences(g);
        WavetableDoc delDoc; delDoc.decode(g.findNode(nId)->script);
        const IWavetableFrame* df = delDoc.libraryFrameById(fid);
        r.check(df && df->morphAssetId == -1 && df->morphChain.size() == 3,
                "assets: erased morph asset -> frame falls back to independent");
    }

    // ---- Frame-scope morph param actually modulates the synth output --------
    // Regression for "sliding a pinned morph's node slider does nothing": a
    // frame-scope warp-slot Param on a layered scatter frame must drive the
    // per-block re-bake (rebakeFramesIfNeeded -> getParamByWarpSlot), so two
    // renders at different param values produce different audio. This exercises
    // the real synth path, not just the editor preview.
    {
        Transport transport;
        transport.sampleRate = 44100.0;
        transport.bpm = 120.0;

        // One layered frame (single Sine layer) placed as a lone scatter dot,
        // carrying a 1-op HardClip morph chain (amplitude warp: identity at
        // amount 0, hard-clipped at high amount -> clearly different cycle).
        auto buildScript = [&](int& fidOut) {
            WavetableDoc doc;
            doc.mode = WavetableMode::Scatter;
            doc.scatterDims = 1;
            doc.tableSize = 2048;
            auto lw = std::make_unique<LayeredWaveform>();
            lw->layers.push_back(WaveLayer{});             // default Sine
            int fid = doc.addLibraryEntry(std::move(lw), "w");
            WarpOp op; op.method = WarpMethod::HardClip; op.amount = 0.5f; op.enabled = true;
            doc.libraryFrameById(fid)->morphChain = { op };
            ScatterFrame sf; sf.waveformId = fid; sf.position = { 0.5f };
            doc.scatterFrames.push_back(sf);
            fidOut = fid;
            return doc.encode();
        };

        // Render a held A4 through a fresh processor whose frame-scope warp
        // param (warpSlot 0) is pinned to `morphAmt`. Returns mono output.
        auto renderWithMorph = [&](float morphAmt) {
            int fid = -1;
            std::string script = buildScript(fid);

            Node node;
            node.id = 1;
            node.type = NodeType::TerrainSynth;
            node.name = "selftest-morph-mod";
            node.script = script;
            node.pinsIn.push_back(Pin{ 1, "MIDI", PinKind::Midi, true, 2 });
            node.pinsIn.push_back(Pin{ 2, "Sig X", PinKind::Signal, true, 1 });
            node.pinsOut.push_back(Pin{ 100, "Audio", PinKind::Audio, false, 2 });
            node.params.push_back({ "Volume", 1.0f, 0.0f, 1.0f });
            node.params.push_back({ "Synth Mode", 0.0f, 0.0f, 2.0f }); // Direct
            // The pinned frame-scope morph param: warpLayer -1 (frame-scope),
            // warpSlot 0, warpFrameId = owning library frame id.
            Param mp;
            mp.name = "HardClip"; mp.value = mp.baseValue = morphAmt;
            mp.minVal = 0.0f; mp.maxVal = 1.0f;
            mp.warpLayer = -1; mp.warpSlot = 0; mp.warpFrameId = fid;
            node.params.push_back(mp);
            // Simulate the op being PINNED: an on-demand modulation input pin +
            // ModPin binding exist, but no cable feeds it (connected == false).
            // applySignalModulations must skip it so the manual param value (the
            // node slider) still reaches the synth. This is exactly the state the
            // user reported sliding "did nothing" in.
            const int paramIdx = (int)node.params.size() - 1;
            const int modPinId = 200;
            node.pinsIn.push_back(Pin{ modPinId, "Mod: HardClip", PinKind::Param, true, 1 });
            Node::ModPin mpin;
            mpin.paramIndex = paramIdx;
            mpin.pinId = modPinId;
            mpin.mode = Node::ModPin::Mode::Modulate;
            mpin.connected = false; // no cable
            node.modPins.push_back(mpin);

            node.ahdsrEnvelope.attackMs = 1.0f;
            node.ahdsrEnvelope.holdMs = 4000.0f;
            node.ahdsrEnvelope.decayMs = 1.0f;
            node.ahdsrEnvelope.sustain = 1.0f;
            node.ahdsrEnvelope.releaseMs = 1.0f;
            node.ahdsrEnvelope.velocitySensitivity = 0.0f;
            AHDSREnvelope::setDefaultCurves(node.ahdsrEnvelope);

            TerrainSynthProcessor proc(node, transport);
            proc.prepareToPlay(44100.0, 512);

            std::vector<float> out;
            juce::AudioBuffer<float> buf(3, 512); // 2 audio + 1 sig
            for (int b = 0; b < 16; ++b) {
                buf.setSize(3, 512, false, false, true);
                buf.clear();
                juce::MidiBuffer midi;
                if (b == 0)
                    midi.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8)100), 0);
                proc.processBlock(buf, midi);
                const float* p = buf.getReadPointer(0);
                // Skip the attack ramp: collect from block 4 onward (steady state).
                if (b >= 4) for (int s = 0; s < 512; ++s) out.push_back(p[s]);
            }
            return out;
        };

        auto lo = renderWithMorph(0.0f);   // identity -> clean sine
        auto hi = renderWithMorph(0.95f);  // hard-clipped -> flat-topped

        r.check(allFinite(lo) && allFinite(hi),
                "morph-mod: synth output finite at both morph amounts");
        r.check(rmsOf(lo) > 1e-3 && rmsOf(hi) > 1e-3,
                "morph-mod: synth audible at both morph amounts");

        // The two renders must differ: compute the normalized RMS of their
        // sample-wise difference relative to the louder render. A working morph
        // param yields a large difference; a broken one (param ignored) yields
        // ~0 because both bakes use the same resting amount.
        double diffSq = 0.0, refSq = 0.0;
        size_t n = std::min(lo.size(), hi.size());
        for (size_t i = 0; i < n; ++i) {
            double d = (double)lo[i] - (double)hi[i];
            diffSq += d * d;
            refSq  += (double)hi[i] * (double)hi[i];
        }
        double relDiff = (refSq > 1e-12) ? std::sqrt(diffSq / refSq) : 0.0;
        r.checkVal(relDiff > 0.05,
                   "morph-mod: frame-scope morph param changes the synth output",
                   relDiff);
    }

    // ---- TerrainSynth: processBlock is allocation-free -----------------------
    // TerrainSynthProcessor::processBlock used to reach the allocator on almost
    // every line of its hot loop: Traversal::evaluate returned a std::vector BY
    // VALUE once per SAMPLE, the SamplePerPoint path copied that vector twice
    // more per sample PER VOICE (pitchCoord / coordA / coordB), the grid
    // occupancy lookup built a fresh coord vector per sample, and the Position
    // params were addressed by rebuilding a std::string ("Position 3") and
    // linear-scanning node.params for every axis of every sample. Per block
    // there were more: the scatter blend allocated weights/dists/blended/coeffs,
    // fetched the db2 filter by value, and called the dwt/idwt convenience
    // overloads that construct a WaveletWorkspace internally - roughly 40
    // allocations a block with eight frames.
    //
    // malloc can block on a global lock, which the user hears as a dropout, and
    // it's an automatic fail under pluginval strictness 10 (which gates the
    // planned plugin spin-offs). Total reserved scratch is the observable proxy:
    // prepareToPlay sizes everything up front, so any growth across a sweep
    // means processBlock reached the allocator. Paired with a non-vacuity check
    // so the sweep can't pass by rendering silence.
    {
        Transport transport;
        transport.sampleRate = 44100.0;
        transport.bpm = 120.0;

        // Four scatter dots along one axis, so the wavelet-domain blend runs
        // with several contributing frames (the heaviest per-block path).
        WavetableDoc doc;
        doc.mode = WavetableMode::Scatter;
        doc.scatterDims = 1;
        doc.tableSize = 2048;                 // power of two -> wavelet morph on
        for (int i = 0; i < 4; ++i) {
            auto lw = std::make_unique<LayeredWaveform>();
            lw->layers.push_back(WaveLayer{});
            int fid = doc.addLibraryEntry(std::move(lw), "w" + std::to_string(i));
            ScatterFrame sf;
            sf.waveformId = fid;
            sf.position = { i / 3.0f };
            doc.scatterFrames.push_back(sf);
        }

        Node node;
        node.id = 1;
        node.type = NodeType::TerrainSynth;
        node.name = "selftest-terrain-alloc";
        node.script = doc.encode();
        node.pinsIn.push_back(Pin{ 1, "MIDI", PinKind::Midi, true, 2 });
        node.pinsIn.push_back(Pin{ 2, "Sig X", PinKind::Signal, true, 1 });
        node.pinsOut.push_back(Pin{ 100, "Audio", PinKind::Audio, false, 2 });
        node.params.push_back({ "Volume",     1.0f, 0.0f, 1.0f });
        node.params.push_back({ "Synth Mode", 0.0f, 0.0f, 2.0f });
        node.params.push_back({ "Position",   0.5f, 0.0f, 1.0f });
        node.ahdsrEnvelope.attackMs  = 1.0f;
        node.ahdsrEnvelope.holdMs    = 60000.0f;   // hold for the whole sweep
        node.ahdsrEnvelope.decayMs   = 1.0f;
        node.ahdsrEnvelope.sustain   = 1.0f;
        node.ahdsrEnvelope.releaseMs = 1.0f;
        node.ahdsrEnvelope.velocitySensitivity = 0.0f;
        AHDSREnvelope::setDefaultCurves(node.ahdsrEnvelope);

        const int maxBlock = 512;
        TerrainSynthProcessor proc(node, transport);
        proc.prepareToPlay(44100.0, maxBlock);

        juce::AudioBuffer<float> buf(3, maxBlock);   // 2 audio + 1 Sig X
        auto runBlock = [&](int len, bool noteOn) {
            juce::AudioBuffer<float> view(buf.getArrayOfWritePointers(),
                                          buf.getNumChannels(), len);
            view.clear();
            juce::MidiBuffer midi;
            if (noteOn)
                midi.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8)100), 0);
            proc.processBlock(view, midi);
        };

        auto idxOf = [&](const char* nm) {
            for (size_t i = 0; i < node.params.size(); ++i)
                if (node.params[i].name == nm) return (int)i;
            return -1;
        };
        const int posIdx  = idxOf("Position");
        const int modeIdx = idxOf("Synth Mode");
        r.check(posIdx >= 0 && modeIdx >= 0,
                "terrain-alloc: Position / Synth Mode params present");

        // Warm up: sound a note and let every lazily-sized buffer settle. Six
        // voices so the per-voice scratch is exercised too, and all three Synth
        // Modes because AdditiveBank is the only one that touches the partial
        // analysis buffers - measuring `before` without visiting it would score
        // its legitimate one-time sizing as a leak.
        runBlock(maxBlock, true);
        for (int nn = 60; nn < 66; ++nn) {
            juce::AudioBuffer<float> view(buf.getArrayOfWritePointers(), 3, maxBlock);
            view.clear();
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1, nn, (juce::uint8)100), 0);
            proc.processBlock(view, midi);
        }
        for (int mode = 0; mode < 3; ++mode) {
            node.params[(size_t)modeIdx].value = (float)mode;
            for (int i = 0; i < 3; ++i) runBlock(maxBlock, false);
        }

        const size_t before = proc.scratchCapacityBytes();

        // Sweep block length, Position and Synth Mode. Block length varies
        // because a host is free to change it (480 is common), Position moves
        // the scatter query so the blend recomputes, and Synth Mode switches
        // between the Direct / AM-sine / Additive render paths.
        const int lens[] = { 512, 480, 256, 64, 333, 512 };
        for (int pass = 0; pass < 60; ++pass) {
            node.params[(size_t)posIdx].value  = (pass % 11) / 10.0f;
            node.params[(size_t)modeIdx].value = (float)(pass % 3);
            runBlock(lens[pass % 6], false);
        }
        const size_t after = proc.scratchCapacityBytes();
        r.checkVal(after == before,
                   "terrain-alloc: 60 blocks sweeping Position / Synth Mode / "
                   "block length allocate nothing (scratch capacity growth, bytes)",
                   (double)after - (double)before);

        // Non-vacuity: the synth must still be making sound at the end of the
        // sweep, so the loop above was doing real work rather than bailing.
        node.params[(size_t)modeIdx].value = 0.0f;   // Direct
        runBlock(maxBlock, false);
        double peak = 0;
        for (int i = 0; i < maxBlock; ++i)
            peak = std::max(peak, (double)std::abs(buf.getSample(0, i)));
        r.checkVal(peak > 1e-3, "terrain-alloc: synth still audible after the sweep",
                   peak);
    }

    // ---- SoundFontProcessor: loads its file, and renders without allocating --
    //
    // Two things are under test here, both found in the audio-thread allocation
    // sweep (agent-todo item 3).
    //
    // 1. loadFile() stripped the script tag with substr(7), but "__sfz__:" is
    //    EIGHT characters, so every path arrived with a leading ':' and neither
    //    tsf_load_filename nor juce::File could find it. .sf2 and .sfz nodes
    //    loaded nothing and rendered silence - the whole node type was dead.
    //    The region-count assertion below is what pins the offset down.
    // 2. processBlock allocated on every callback: `interleaved` was a local
    //    std::vector sized to the block, and findRegions returned a fresh
    //    std::vector<const SFZRegion*> by value on every note-on. Both are now
    //    members sized in prepareToPlay / loadFile, so total reserved scratch is
    //    the observable proxy - any growth across the sweep means the allocator
    //    was reached from the audio thread.
    //
    // The instrument is built on the fly (a looping sine .wav plus a two-region
    // .sfz) so the test carries no binary fixture and doesn't depend on a
    // SoundFont being installed. SFZ rather than SF2 because SF2 has no
    // human-writable text form.
    {
        auto sfDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("seance_selftest_sfz");
        sfDir.deleteRecursively();
        sfDir.createDirectory();

        const double sr = 44100.0;
        const int    sampleLen = (int)sr;              // 1 s, looped
        std::vector<float> tone((size_t)sampleLen);
        for (int i = 0; i < sampleLen; ++i)
            tone[(size_t)i] = 0.5f * std::sin(2.0 * juce::MathConstants<double>::pi
                                              * 220.0 * i / sr);
        auto wav = sfDir.getChildFile("tone.wav");
        r.check(writeWavFloat(wav, tone, sr), "soundfont: test sample written");

        // Two regions splitting the velocity range, so a note-on has to match
        // exactly one and regionMatches is genuinely repopulated per note.
        auto sfzFile = sfDir.getChildFile("inst.sfz");
        sfzFile.replaceWithText(
            "<group> loop_mode=loop_continuous loop_start=0 loop_end="
            + juce::String(sampleLen - 1) + " ampeg_release=0.05\n"
            "<region> sample=tone.wav lokey=0 hikey=127 pitch_keycenter=57 lovel=0 hivel=63\n"
            "<region> sample=tone.wav lokey=0 hikey=127 pitch_keycenter=57 lovel=64 hivel=127\n");

        Node node;
        node.id   = 1;
        node.type = NodeType::Instrument;
        node.name = "selftest-sfz";
        node.script = "__sfz__:" + sfzFile.getFullPathName().toStdString();
        node.pinsIn .push_back(Pin{ 1, "MIDI",  PinKind::Midi,  true,  2 });
        node.pinsOut.push_back(Pin{ 100, "Audio", PinKind::Audio, false, 2 });
        node.params.push_back({ "Volume",   0.8f, 0.0f, 1.0f });
        node.params.push_back({ "Vel Sens", 0.0f, 0.0f, 1.0f });

        const int maxBlock = 512;
        SoundFontProcessor proc(node);
        r.check(proc.isSFZ(), "soundfont: .sfz path resolves from the node script "
                              "(regression: substr(7) left a stray ':')");
        proc.prepareToPlay(sr, maxBlock);

        juce::AudioBuffer<float> buf(2, maxBlock);
        auto runBlock = [&](int len, int noteOn, int noteOff) {
            juce::AudioBuffer<float> view(buf.getArrayOfWritePointers(),
                                          buf.getNumChannels(), len);
            view.clear();
            juce::MidiBuffer midi;
            if (noteOn  >= 0) midi.addEvent(juce::MidiMessage::noteOn (1, noteOn,
                                              (juce::uint8)100), 0);
            if (noteOff >= 0) midi.addEvent(juce::MidiMessage::noteOff(1, noteOff), 0);
            proc.processBlock(view, midi);
        };

        // Warm up: sound a note and render a few blocks so every lazily-sized
        // buffer settles before the capacity snapshot.
        runBlock(maxBlock, 57, -1);
        for (int i = 0; i < 3; ++i) runBlock(maxBlock, -1, -1);

        const size_t before = proc.scratchCapacityBytes();

        // Sweep block length (480 is a common host size; 333 is deliberately
        // awkward) with continuous note-on/note-off traffic, so both the render
        // path and the per-note-on region match run every pass.
        const int lens[] = { 512, 480, 256, 64, 333, 512 };
        for (int pass = 0; pass < 60; ++pass) {
            const int nn = 55 + (pass % 7);
            runBlock(lens[pass % 6], nn, (pass % 3 == 0) ? 55 + ((pass + 3) % 7) : -1);
        }
        const size_t after = proc.scratchCapacityBytes();
        r.checkVal(after == before,
                   "soundfont: 60 blocks of note traffic at varying block lengths "
                   "allocate nothing (scratch capacity growth, bytes)",
                   (double)after - (double)before);

        // Non-vacuity: the sampler must actually be producing sound, otherwise
        // the sweep above proves nothing.
        runBlock(maxBlock, 57, -1);
        double peak = 0;
        for (int i = 0; i < maxBlock; ++i)
            peak = std::max(peak, (double)std::abs(buf.getSample(0, i)));
        r.checkVal(peak > 1e-3, "soundfont: sampler audible after the sweep", peak);

        sfDir.deleteRecursively();
    }

    // ---- SignalShapeProcessor renders without allocating --------------------
    //
    // The Signal Shape node's per-sample loop was the worst offender found in
    // the allocation sweep (agent-todo item 3), because the cost scaled with the
    // expression vocabulary rather than being one buffer:
    //   - `std::vector<const float*> sigChans` built per block (and handed to
    //     the block-mode runtime as ScriptBlockCtx::sig, so it had to outlive
    //     the call anyway).
    //   - `std::unordered_map<std::string,float> vars` CONSTRUCTED PER BLOCK:
    //     a bucket array plus one node allocation for each of the ~12 fixed
    //     variables and every s1..sN, then all of it freed at the end of the
    //     block. Roughly 15 allocations per callback.
    //   - `std::function<float(float)> shapeFn` constructed per block. It fits
    //     MSVC's small-buffer optimisation today, but whether a std::function
    //     heap-allocates is an implementation detail we shouldn't bet the audio
    //     thread on.
    //   - `ScriptVars sv` on the transport play edge, same map cost.
    //   - the per-sample s-list binding rebuilt a `"s" + std::to_string(i+1)`
    //     key for every input of every sample.
    // All are now members; `vars` survives across blocks so operator[] only
    // overwrites, and is rebuilt only when the s-list width changes.
    //
    // The capacity proxy counts `vars.bucket_count() + vars.size()`, so both a
    // rehash and a single newly-inserted key (one node allocation) register.
    {
        Transport transport;
        transport.sampleRate = 44100.0;
        transport.bpm        = 120.0;
        transport.playing    = true;

        SignalShapeDoc doc = SignalShapeDoc::defaultLFO();
        doc.expr             = "curve * 0.5 + s1 * 0.25 + gate * 0.25";
        doc.triggerExpr      = "gate";      // exercises the trigger-eval path too
        doc.signalInputCount = 2;
        doc.layers.layers.push_back(WaveLayer{});   // a real shape to sample

        Node node;
        node.id     = 1;
        node.type   = NodeType::SignalShape;
        node.name   = "selftest-signalshape";
        node.script = doc.encode();
        node.pinsIn .push_back(Pin{ 1, "MIDI In", PinKind::Midi,   true,  2 });
        node.pinsIn .push_back(Pin{ 2, "s1",      PinKind::Signal, true,  1 });
        node.pinsIn .push_back(Pin{ 3, "s2",      PinKind::Signal, true,  1 });
        node.pinsOut.push_back(Pin{ 100, "o1",    PinKind::Signal, false, 1 });
        node.params.push_back({ "Rate",      2.0f, 0.0f, 20.0f });
        node.params.push_back({ "Beat Sync", 0.0f, 0.0f, 1.0f });
        node.params.push_back({ "Phase",     0.0f, 0.0f, 1.0f });
        node.params.push_back({ "Output",    0.5f, 0.0f, 1.0f });

        const int maxBlock = 512;
        SignalShapeProcessor proc(node, transport);
        proc.prepareToPlay(44100.0, maxBlock);

        // 2 audio channels + 2 control channels (s1/s2 in, o1 out share them).
        juce::AudioBuffer<float> buf(4, maxBlock);
        int64_t pos = 0;
        auto runBlock = [&](int len, bool noteOn) {
            juce::AudioBuffer<float> view(buf.getArrayOfWritePointers(),
                                          buf.getNumChannels(), len);
            view.clear();
            // Feed the control inputs something non-constant so s1/s2 actually
            // move and the expression can't be folded away.
            for (int ch = 2; ch < 4; ++ch)
                for (int s = 0; s < len; ++s)
                    view.setSample(ch, s, 0.5f + 0.4f * std::sin(0.01f * (s + ch)));
            juce::MidiBuffer midi;
            if (noteOn)
                midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
            proc.processBlock(view, midi);
            transport.positionSamples = (pos += len);
        };

        // Warm up: cross the play edge (which runs the start() hook and its own
        // variable map) and let every lazily-sized buffer settle.
        runBlock(maxBlock, true);
        for (int i = 0; i < 3; ++i) runBlock(maxBlock, false);

        const size_t before = proc.scratchCapacityBytes();

        const int lens[] = { 512, 480, 256, 64, 333, 512 };
        for (int pass = 0; pass < 60; ++pass) {
            node.params[0].value = 0.5f + (pass % 9);       // Rate
            node.params[1].value = (float)(pass % 2);       // Beat Sync
            node.params[2].value = (pass % 7) / 7.0f;       // Phase
            runBlock(lens[pass % 6], (pass % 5) == 0);
        }
        const size_t after = proc.scratchCapacityBytes();
        r.checkVal(after == before,
                   "signalshape: 60 blocks sweeping Rate / Beat Sync / Phase / "
                   "block length allocate nothing (scratch capacity growth, bytes)",
                   (double)after - (double)before);

        // Non-vacuity: the node must be driving its output channel, and the
        // output must actually vary - a stuck constant would pass a peak test
        // while proving the expression never ran.
        runBlock(maxBlock, true);
        float lo = 1e9f, hi = -1e9f;
        for (int s = 0; s < maxBlock; ++s) {
            float v = buf.getSample(2, s);
            lo = std::min(lo, v); hi = std::max(hi, v);
        }
        r.checkVal(hi - lo > 1e-4f,
                   "signalshape: output still modulating after the sweep", hi - lo);
    }

    // ---- MidiScriptProcessor renders without allocating ---------------------
    //
    // Same shape as the Signal Shape node above (they're sibling scriptable
    // nodes): `sigChans` and the `vars` binding map were both locals in
    // processBlock, so every callback built and tore down a map with a node
    // allocation per variable, and buildVars rebuilt a "s"+to_string(i+1) key
    // for every input of every sample. Both are members now.
    {
        Transport transport;
        transport.sampleRate = 44100.0;
        transport.bpm        = 120.0;
        transport.playing    = true;

        MidiScriptDoc doc = MidiScriptDoc::defaultDoc();
        // A program that actually emits, and that reads the signal inputs so
        // the s-list binding isn't dead code. `note()` schedules a note-off,
        // exercising pendingOffs across blocks too.
        doc.program          = "(s1 > 0.6) ? note(48 + s2 * 12, 100, 0.05) : 0";
        doc.signalInputCount = 2;

        Node node;
        node.id     = 1;
        node.type   = NodeType::MidiScript;
        node.name   = "selftest-midiscript";
        node.script = doc.encode();
        node.pinsIn .push_back(Pin{ 1, "MIDI In", PinKind::Midi,   true,  2 });
        node.pinsIn .push_back(Pin{ 2, "s1",      PinKind::Signal, true,  1 });
        node.pinsIn .push_back(Pin{ 3, "s2",      PinKind::Signal, true,  1 });
        node.pinsOut.push_back(Pin{ 100, "MIDI Out", PinKind::Midi, false, 2 });

        const int maxBlock = 512;
        MidiScriptProcessor proc(node, transport);
        proc.prepareToPlay(44100.0, maxBlock);

        juce::AudioBuffer<float> buf(4, maxBlock);   // 2 audio + s1/s2 on 2..3
        int64_t pos = 0;
        int totalEmitted = 0;
        auto runBlock = [&](int len) {
            juce::AudioBuffer<float> view(buf.getArrayOfWritePointers(),
                                          buf.getNumChannels(), len);
            view.clear();
            for (int ch = 2; ch < 4; ++ch)
                for (int s = 0; s < len; ++s)
                    view.setSample(ch, s, 0.5f + 0.45f * std::sin(0.02f * (s + 7 * ch)));
            juce::MidiBuffer midi;
            proc.processBlock(view, midi);
            totalEmitted += midi.getNumEvents();
            transport.positionSamples = (pos += len);
        };

        // Warm up: cross the play edge (start hook + its own bindings) and let
        // the note scheduler reach steady state.
        for (int i = 0; i < 4; ++i) runBlock(maxBlock);

        const size_t before = proc.scratchCapacityBytes();
        const int emittedBefore = totalEmitted;

        const int lens[] = { 512, 480, 256, 64, 333, 512 };
        for (int pass = 0; pass < 60; ++pass) runBlock(lens[pass % 6]);

        const size_t after = proc.scratchCapacityBytes();
        r.checkVal(after == before,
                   "midiscript: 60 blocks at varying block lengths allocate "
                   "nothing (scratch capacity growth, bytes)",
                   (double)after - (double)before);

        // Non-vacuity: the program has to have been emitting throughout, or the
        // sweep never reached the interesting code.
        r.checkVal(totalEmitted - emittedBefore > 0,
                   "midiscript: program still emitting MIDI during the sweep",
                   (double)(totalEmitted - emittedBefore));
    }

    // ---- ArpeggiatorProcessor renders without allocating --------------------
    //
    // The arpeggiator used to keep held notes in a std::set<int> (a tree-node
    // allocation on the audio thread per note-on) and rebuild `seq` plus a
    // `baseNotes` copy as processBlock locals every callback. Held notes are a
    // std::bitset<128> now (MIDI pitch = bit index, and walking it already
    // yields ascending order, so the sort went too) and `seq` is a member
    // reserved to the worst case in prepareToPlay.
    {
        Node node;
        node.id   = 1;
        node.name = "selftest-arp";
        node.params.push_back({ "Rate",    8.0f, 0.1f, 50.0f });
        node.params.push_back({ "Pattern", 0.0f, 0.0f,  3.0f });
        node.params.push_back({ "Octaves", 1.0f, 1.0f,  4.0f });

        auto setParam = [&](const char* name, float v) {
            for (auto& p : node.params) if (p.name == name) p.value = v;
        };

        const int maxBlock = 512;
        ArpeggiatorProcessor proc(node);
        proc.prepareToPlay(44100.0, maxBlock);

        juce::AudioBuffer<float> buf(2, maxBlock);
        int totalEmitted = 0;
        // Re-asserts a chord of `held` notes (48, 49, ... ) each time it's
        // asked to, so a stray note-off can never leave the arp idle - which
        // would make the whole test vacuous.
        auto runBlock = [&](int len, int held) {
            juce::AudioBuffer<float> view(buf.getArrayOfWritePointers(), 2, len);
            view.clear();
            juce::MidiBuffer midi;
            for (int i = 0; i < held; ++i)
                midi.addEvent(juce::MidiMessage::noteOn(1, i, (juce::uint8)100), 0);
            proc.processBlock(view, midi);
            totalEmitted += midi.getNumEvents();
        };

        // Warm up on a small chord ON PURPOSE. If the reserve in prepareToPlay
        // were ever removed, `seq` would settle at a handful of ints here and
        // the wide chords below would then have to grow it - which is exactly
        // what the capacity assertion is looking for. Warming up at the worst
        // case instead would make this test pass vacuously.
        for (int i = 0; i < 4; ++i) runBlock(maxBlock, 6);

        const size_t before = proc.scratchCapacityBytes();
        const int emittedBefore = totalEmitted;

        const int lens[] = { 512, 480, 256, 64, 333, 512 };
        for (int pass = 0; pass < 60; ++pass) {
            // Sweep everything that changes the sequence length: the pattern
            // (up-down roughly doubles it), the octave count (x4) and the rate.
            setParam("Pattern", (float)(pass % 4));
            setParam("Octaves", (float)(1 + pass % 4));
            setParam("Rate",    4.0f + (float)(pass % 17));
            // Ramp the held-note count all the way to a full 128-note keyboard,
            // which with 4 octaves and the up-down pattern is the worst case
            // prepareToPlay reserves for.
            runBlock(lens[pass % 6], 2 + (pass * 128) / 60);
        }

        const size_t after = proc.scratchCapacityBytes();
        r.checkVal(after == before,
                   "arpeggiator: 60 blocks sweeping Pattern / Octaves / Rate up "
                   "to a full 128-note chord allocate nothing (scratch capacity "
                   "growth, bytes)",
                   (double)after - (double)before);
        // The reserve has to be big enough for that worst case, or the check
        // above only proves the sequence never got long - not that it couldn't.
        r.checkVal(before >= 128 * 4 * 2 * sizeof(int),
                   "arpeggiator: prepareToPlay reserves the worst-case sequence "
                   "(bytes)", (double)before);
        r.checkVal(totalEmitted - emittedBefore > 0,
                   "arpeggiator: still emitting notes during the sweep",
                   (double)(totalEmitted - emittedBefore));
    }

    // ---- ParticleSynthProcessor renders without allocating ------------------
    //
    // The grain cloud grew by push_back with no reserve and no ceiling, so a
    // high Density allocated on the audio thread mid-block. `grains` is now
    // reserved to kMaxGrains and the spawn loop refuses to exceed it. The
    // sweep deliberately drives Density to absurd values to hit that ceiling.
    {
        Node node;
        node.id   = 1;
        node.name = "selftest-particle";
        node.params.push_back({ "Density",    30.0f, 1.0f, 5000.0f });
        node.params.push_back({ "Spread",      7.0f, 0.0f,   24.0f });
        node.params.push_back({ "Grain Size", 50.0f, 1.0f,  500.0f });
        node.params.push_back({ "Attack",      0.1f, 0.0f,    1.0f });
        node.params.push_back({ "Release",     0.3f, 0.0f,    1.0f });
        node.params.push_back({ "Shape",       0.0f, 0.0f,    3.0f });
        node.params.push_back({ "Volume",      0.5f, 0.0f,    1.0f });

        auto setParam = [&](const char* name, float v) {
            for (auto& p : node.params) if (p.name == name) p.value = v;
        };

        const int maxBlock = 512;
        ParticleSynthProcessor proc(node);
        proc.prepareToPlay(44100.0, maxBlock);

        juce::AudioBuffer<float> buf(2, maxBlock);
        float peak = 0.0f;
        auto runBlock = [&](int len, bool noteOn) {
            juce::AudioBuffer<float> view(buf.getArrayOfWritePointers(), 2, len);
            view.clear();
            juce::MidiBuffer midi;
            if (noteOn)
                midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)110), 0);
            proc.processBlock(view, midi);
            for (int ch = 0; ch < 2; ++ch)
                for (int s = 0; s < len; ++s)
                    peak = std::max(peak, std::abs(view.getSample(ch, s)));
        };

        // Warm up at a LOW density on purpose: if the reserve in prepareToPlay
        // were removed, the cloud would settle small here and the saturating
        // sweep below would have to grow it - which is what we want to catch.
        // Warming up already-saturated would make the assertion vacuous.
        setParam("Density", 5.0f);
        runBlock(maxBlock, true);
        for (int i = 0; i < 3; ++i) runBlock(maxBlock, false);

        const size_t before = proc.scratchCapacityBytes();
        peak = 0.0f;

        const int lens[] = { 512, 480, 256, 64, 333, 512 };
        for (int pass = 0; pass < 60; ++pass) {
            setParam("Density",    1.0f + (float)((pass * 397) % 5000));
            setParam("Grain Size", 1.0f + (float)((pass * 73) % 500));
            setParam("Spread",     (float)(pass % 25));
            setParam("Shape",      (float)(pass % 4));
            runBlock(lens[pass % 6], pass % 7 == 0);
        }

        const size_t after = proc.scratchCapacityBytes();
        r.checkVal(after == before,
                   "particlesynth: 60 blocks sweeping Density / Grain Size / "
                   "Shape allocate nothing (scratch capacity growth, bytes)",
                   (double)after - (double)before);
        r.checkVal(proc.reservedGrainCount() >= ParticleSynthProcessor::kMaxGrains,
                   "particlesynth: reserve covers the whole grain ceiling",
                   (double)proc.reservedGrainCount());
        r.checkVal(peak > 1e-4f,
                   "particlesynth: cloud still audible during the sweep", peak);
    }

    // ---- SpectralGrainProcessor renders without allocating ------------------
    //
    // Same shape as the particle synth: activeGrains was an unbounded
    // push_back. Reserved to kMaxActiveGrains with the ceiling enforced at the
    // spawn site. Note this one is polyphonic, so the sweep holds several
    // voices to multiply the spawn rate.
    {
        Node node;
        node.id     = 1;
        node.name   = "selftest-spectralgrain";
        node.script = "__spectralgrain__:exp(-f/10)";
        node.params.push_back({ "Density",    20.0f, 1.0f, 2000.0f });
        node.params.push_back({ "Grain Size", 40.0f, 1.0f,  500.0f });
        node.params.push_back({ "Volume",      0.5f, 0.0f,    1.0f });

        auto setParam = [&](const char* name, float v) {
            for (auto& p : node.params) if (p.name == name) p.value = v;
        };

        const int maxBlock = 512;
        SpectralGrainProcessor proc(node);
        proc.prepareToPlay(44100.0, maxBlock);

        juce::AudioBuffer<float> buf(2, maxBlock);
        float peak = 0.0f;
        auto runBlock = [&](int len, bool chordOn) {
            juce::AudioBuffer<float> view(buf.getArrayOfWritePointers(), 2, len);
            view.clear();
            juce::MidiBuffer midi;
            if (chordOn)
                for (int n : { 48, 55, 60, 64, 67, 72 })
                    midi.addEvent(juce::MidiMessage::noteOn(1, n, (juce::uint8)110), 0);
            proc.processBlock(view, midi);
            for (int ch = 0; ch < 2; ++ch)
                for (int s = 0; s < len; ++s)
                    peak = std::max(peak, std::abs(view.getSample(ch, s)));
        };

        // Low-density warm-up for the same non-vacuity reason as the particle
        // synth above: the saturating sweep must be what grows the cloud.
        setParam("Density", 5.0f);
        runBlock(maxBlock, true);
        for (int i = 0; i < 3; ++i) runBlock(maxBlock, false);

        const size_t before = proc.scratchCapacityBytes();
        peak = 0.0f;

        const int lens[] = { 512, 480, 256, 64, 333, 512 };
        for (int pass = 0; pass < 60; ++pass) {
            setParam("Density",    1.0f + (float)((pass * 397) % 2000));
            setParam("Grain Size", 1.0f + (float)((pass * 73) % 500));
            runBlock(lens[pass % 6], pass % 7 == 0);
        }

        const size_t after = proc.scratchCapacityBytes();
        r.checkVal(after == before,
                   "spectralgrain: 60 blocks sweeping Density / Grain Size "
                   "allocate nothing (scratch capacity growth, bytes)",
                   (double)after - (double)before);
        r.checkVal(proc.reservedGrainCount() >= SpectralGrainProcessor::kMaxActiveGrains,
                   "spectralgrain: reserve covers the whole grain ceiling",
                   (double)proc.reservedGrainCount());
        r.checkVal(peak > 1e-4f,
                   "spectralgrain: cloud still audible during the sweep", peak);
    }

    // ---- Every built-in synth survives a transport Stop ----------------------
    //
    // Transport panic calls AudioProcessorGraph::reset(), which calls reset()
    // on every processor. Four of the builtin_effects.h synths implemented that
    // as `voices.clear()` - but their voice pool is only ever sized in the
    // CONSTRUCTOR, so nothing ever refilled it. After a single press of Stop:
    // allocVoice() found no free voice, fell through to its steal path, and
    // returned voices[0] on an empty vector (out of bounds); the render loop
    // then iterated zero voices. The synth went permanently silent and stayed
    // that way for the rest of the session.
    //
    // This checks the two halves that matter for every one of them: panic
    // really does silence a held note, AND the synth still plays afterwards.
    //
    // Verified by reinstating both defects. voices.clear() makes "still plays
    // after a transport Stop" read exactly 0.00000 for FM / PD / Additive
    // (against 0.19-0.43 for the fixed code). Dropping ParticleSynth's
    // noteAmpEnv.hardReset() makes "actually silences a held note" read
    // 0.39244 (against 0.00000) - its spawn loop is gated on the envelope, so
    // clearing the grain list alone let the cloud regrow within a millisecond.
    {
        auto panicRoundTrip = [&r](const char* label, const char* script,
                                   std::vector<Param> params,
                                   auto&& makeProc) {
            Node node;
            node.id     = 1;
            node.name   = "selftest-panic";
            node.script = script;
            node.params = std::move(params);

            const int bs = 512;
            auto proc = makeProc(node);
            proc->prepareToPlay(44100.0, bs);
            juce::AudioBuffer<float> buf(2, bs);

            auto play = [&](int blocks, bool noteOn) {
                float pk = 0.0f;
                for (int b = 0; b < blocks; ++b) {
                    buf.clear();
                    juce::MidiBuffer midi;
                    if (noteOn && b == 0)
                        midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)110), 0);
                    proc->processBlock(buf, midi);
                    for (int s = 0; s < bs; ++s)
                        pk = std::max(pk, std::abs(buf.getSample(0, s)));
                }
                return pk;
            };

            const float before = play(10, true);
            r.checkVal(before > 1e-4f,
                       juce::String(label) + ": sounds before the transport Stop", before);

            proc->reset();     // <- the transport-panic path

            // The note is still HELD (no note-off was sent), so this is the
            // case where panic has to do the work.
            const float during = play(4, false);
            r.checkVal(during < 1e-4f,
                       juce::String(label) + ": transport Stop actually silences a held note",
                       during);

            const float after = play(10, true);
            r.checkVal(after > 1e-4f,
                       juce::String(label) + ": still plays after a transport Stop",
                       after);
        };

        panicRoundTrip("panic/fmsynth", "",
                       { { "Algorithm", 0.0f, 0.0f, 7.0f },
                         { "Volume",    0.5f, 0.0f, 1.0f } },
                       [](Node& n) { return std::make_unique<FMSynthProcessor>(n); });

        panicRoundTrip("panic/pdsynth", "",
                       { { "Volume", 0.5f, 0.0f, 1.0f } },
                       [](Node& n) { return std::make_unique<PDSynthProcessor>(n); });

        panicRoundTrip("panic/additive", "",
                       { { "Partials", 16.0f, 1.0f, 64.0f },
                         { "Volume",    0.5f, 0.0f,  1.0f } },
                       [](Node& n) { return std::make_unique<AdditiveSynthProcessor>(n); });

        panicRoundTrip("panic/particlesynth", "",
                       { { "Density",    60.0f, 1.0f, 200.0f },
                         { "Grain Size", 40.0f, 1.0f, 500.0f },
                         { "Volume",      0.5f, 0.0f,   1.0f } },
                       [](Node& n) { return std::make_unique<ParticleSynthProcessor>(n); });
    }

    // ---- Every built-in synth honours All Notes Off / All Sound Off ----------
    //
    // None of the synths in builtin_effects.h handled either message, though
    // six other synth files do. GraphProcessor emits All Notes Off on all 16
    // channels when the transport stops (graph_processor.cpp:94), and a
    // controller's panic button or the end of a MIDI file send them too. A
    // synth that ignores them never receives the matching note-off, so a held
    // note sounds forever.
    //
    // Verified by deleting the handling again: every synth reads 0.17-0.51 for
    // "All Sound Off cuts the note dead" (against 0.00000 fixed) and 0.13-0.30
    // for "the note is gone once the release has run" - i.e. the note simply
    // sustains forever, exactly as reported. The arpeggiator emits 4 more
    // note-ons after the panic instead of 0.
    //
    // The two messages differ and both halves are checked:
    //   All Sound Off (CC#120) - silent immediately.
    //   All Notes Off (CC#123) - enters the RELEASE stage, so it must still be
    //                            audible right after the message and silent
    //                            once the release has run.
    {
        auto checkPanic = [&r](const char* label, std::vector<Param> params,
                               auto&& makeProc) {
            const int bs = 512;

            // Plays 6 blocks with a note-on in the first, then sends `panic`,
            // then reports (level in the 2 blocks right after, level once the
            // release has had 60 blocks - 700 ms - to finish).
            auto run = [&](const juce::MidiMessage& panic) {
                Node node;
                node.id     = 1;
                node.name   = "selftest-allnotesoff";
                node.params = params;
                // Long, obvious release so "released but still ringing" and
                // "cut dead" are far apart.
                node.ahdsrEnvelope.releaseMs = 400.0f;

                auto proc = makeProc(node);
                proc->prepareToPlay(44100.0, bs);
                juce::AudioBuffer<float> buf(2, bs);

                auto blocks = [&](int n, const juce::MidiMessage* m) {
                    float pk = 0.0f;
                    for (int b = 0; b < n; ++b) {
                        buf.clear();
                        juce::MidiBuffer midi;
                        if (m && b == 0) midi.addEvent(*m, 0);
                        proc->processBlock(buf, midi);
                        for (int s = 0; s < bs; ++s)
                            pk = std::max(pk, std::abs(buf.getSample(0, s)));
                    }
                    return pk;
                };

                const auto on = juce::MidiMessage::noteOn(1, 60, (juce::uint8)110);
                const float held = blocks(6, &on);
                const float just = blocks(2, &panic);
                // Run the 400 ms release out (60 blocks = 700 ms) and DISCARD
                // that level - it is dominated by the start of the release,
                // which is still near full volume. Only the level after it has
                // finished says whether the note actually ended.
                blocks(60, nullptr);
                const float late = blocks(4, nullptr);
                return std::array<float, 3>{ held, just, late };
            };

            const auto soundOff = run(juce::MidiMessage::allSoundOff(1));
            r.checkVal(soundOff[0] > 1e-4f,
                       juce::String(label) + ": sounds before All Sound Off", soundOff[0]);
            r.checkVal(soundOff[1] < 1e-4f,
                       juce::String(label) + ": All Sound Off cuts the note dead", soundOff[1]);

            const auto notesOff = run(juce::MidiMessage::allNotesOff(1));
            r.checkVal(notesOff[1] > 1e-4f,
                       juce::String(label) + ": All Notes Off lets the note RELEASE "
                       "rather than cutting it", notesOff[1]);
            r.checkVal(notesOff[2] < 1e-4f,
                       juce::String(label) + ": the note is gone once the release has run",
                       notesOff[2]);
        };

        checkPanic("allnotesoff/fmsynth",
                   { { "Algorithm", 0.0f, 0.0f, 7.0f }, { "Volume", 0.5f, 0.0f, 1.0f } },
                   [](Node& n) { return std::make_unique<FMSynthProcessor>(n); });
        checkPanic("allnotesoff/pdsynth",
                   { { "Volume", 0.5f, 0.0f, 1.0f } },
                   [](Node& n) { return std::make_unique<PDSynthProcessor>(n); });
        checkPanic("allnotesoff/additive",
                   { { "Partials", 16.0f, 1.0f, 64.0f }, { "Volume", 0.5f, 0.0f, 1.0f } },
                   [](Node& n) { return std::make_unique<AdditiveSynthProcessor>(n); });
        checkPanic("allnotesoff/particlesynth",
                   { { "Density", 60.0f, 1.0f, 200.0f },
                     { "Grain Size", 40.0f, 1.0f, 500.0f },
                     { "Volume", 0.5f, 0.0f, 1.0f } },
                   [](Node& n) { return std::make_unique<ParticleSynthProcessor>(n); });

        // Spectral Grain needs its magnitude expression, so it can't go through
        // the helper above (which builds a bare Node).
        {
            const int bs = 512;
            auto run = [&](const juce::MidiMessage& panic) {
                Node node;
                node.id     = 1;
                node.script = "__spectralgrain__:exp(-f/10)";
                node.params = { { "Density",    80.0f, 1.0f, 200.0f },
                                { "Grain Size", 40.0f, 1.0f, 200.0f },
                                { "Volume",      0.8f, 0.0f,   1.0f } };
                node.ahdsrEnvelope.releaseMs = 400.0f;

                SpectralGrainProcessor proc(node);
                proc.prepareToPlay(44100.0, bs);
                juce::AudioBuffer<float> buf(2, bs);
                auto blocks = [&](int n, const juce::MidiMessage* m) {
                    float pk = 0.0f;
                    for (int b = 0; b < n; ++b) {
                        buf.clear();
                        juce::MidiBuffer midi;
                        if (m && b == 0) midi.addEvent(*m, 0);
                        proc.processBlock(buf, midi);
                        for (int s = 0; s < bs; ++s)
                            pk = std::max(pk, std::abs(buf.getSample(0, s)));
                    }
                    return pk;
                };
                const auto on = juce::MidiMessage::noteOn(1, 60, (juce::uint8)110);
                const float held = blocks(6, &on);
                const float just = blocks(2, &panic);
                // Run the 400 ms release out (60 blocks = 700 ms) and DISCARD
                // that level - it is dominated by the start of the release,
                // which is still near full volume. Only the level after it has
                // finished says whether the note actually ended.
                blocks(60, nullptr);
                const float late = blocks(4, nullptr);
                return std::array<float, 3>{ held, just, late };
            };
            const auto so = run(juce::MidiMessage::allSoundOff(1));
            r.checkVal(so[0] > 1e-4f,
                       "allnotesoff/spectralgrain: sounds before All Sound Off", so[0]);
            r.checkVal(so[1] < 1e-4f,
                       "allnotesoff/spectralgrain: All Sound Off cuts the note dead", so[1]);
            const auto no = run(juce::MidiMessage::allNotesOff(1));
            r.checkVal(no[1] > 1e-4f,
                       "allnotesoff/spectralgrain: All Notes Off lets the note RELEASE "
                       "rather than cutting it", no[1]);
            r.checkVal(no[2] < 1e-4f,
                       "allnotesoff/spectralgrain: the note is gone once the release has run",
                       no[2]);
        }

        // The Arpeggiator is the worst case: it CLEARS the incoming MIDI buffer
        // and emits its own, so a panic it ignores never reaches the synth
        // downstream either - one stuck arp jams the whole chain.
        {
            Node node;
            node.id = 1;
            // Rate is notes per SECOND: at 16, a note lands every ~2756
            // samples, i.e. roughly every 5th 512-sample block.
            node.params = { { "Rate", 16.0f, 1.0f, 32.0f },
                            { "Pattern", 0.0f, 0.0f, 4.0f },
                            { "Octaves", 1.0f, 1.0f, 4.0f },
                            { "Gate", 0.5f, 0.05f, 1.0f } };
            ArpeggiatorProcessor proc(node);
            proc.prepareToPlay(44100.0, 512);

            auto countNoteOns = [&](int blocks, const juce::MidiMessage* m) {
                int n = 0;
                for (int b = 0; b < blocks; ++b) {
                    juce::AudioBuffer<float> buf(2, 512);
                    buf.clear();
                    juce::MidiBuffer midi;
                    if (m && b == 0) midi.addEvent(*m, 0);
                    proc.processBlock(buf, midi);
                    for (auto meta : midi)
                        if (meta.getMessage().isNoteOn()) ++n;
                }
                return n;
            };

            const auto on = juce::MidiMessage::noteOn(1, 60, (juce::uint8)110);
            const int running = countNoteOns(20, &on);
            r.checkVal(running > 0,
                       "allnotesoff/arpeggiator: arpeggiating before the panic",
                       (double)running);
            const auto panic = juce::MidiMessage::allNotesOff(1);
            countNoteOns(1, &panic);
            const int after = countNoteOns(20, nullptr);
            r.checkVal(after == 0,
                       "allnotesoff/arpeggiator: All Notes Off releases the stuck chord",
                       (double)after);
        }
    }

    // ---- SpectralGrain still plays after a transport Stop --------------------
    //
    // reset() (the transport-panic hook) used to be `voices.clear()`. Nothing
    // ever refilled the pool - prepareToPlay only regenerates the grain bank -
    // so afterwards allocVoice() fell through to its steal path and returned
    // voices[0] on an EMPTY vector (out-of-bounds write into the freed-but-owned
    // buffer), and the render loop then iterated zero voices, leaving the node
    // permanently silent. Hitting Stop once bricked the node for the session.
    {
        Node node;
        node.id     = 1;
        node.name   = "selftest-spectralgrain-reset";
        node.script = "__spectralgrain__:exp(-f/10)";
        node.params.push_back({ "Density",    80.0f, 1.0f, 2000.0f });
        node.params.push_back({ "Grain Size", 60.0f, 1.0f,  500.0f });
        node.params.push_back({ "Volume",      0.8f, 0.0f,    1.0f });

        const int bs = 512;
        SpectralGrainProcessor proc(node);
        proc.prepareToPlay(44100.0, bs);
        juce::AudioBuffer<float> buf(2, bs);

        // Runs `blocks` blocks, firing a note-on in the first, and returns the
        // peak. 8 blocks at 44.1 kHz is ~93 ms, comfortably past the 12.5 ms
        // first spawn at Density 80.
        auto playPeak = [&](int blocks) {
            float pk = 0.0f;
            for (int b = 0; b < blocks; ++b) {
                buf.clear();
                juce::MidiBuffer midi;
                if (b == 0)
                    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)110), 0);
                proc.processBlock(buf, midi);
                for (int s = 0; s < bs; ++s)
                    pk = std::max(pk, std::abs(buf.getSample(0, s)));
            }
            return pk;
        };

        const float beforeStop = playPeak(8);
        r.checkVal(beforeStop > 1e-4f,
                   "spectralgrain: note sounds before the transport Stop", beforeStop);

        proc.reset();                     // <- the transport-panic path
        r.checkVal(proc.activeVoiceCount() == 0,
                   "spectralgrain: Stop silences every voice",
                   (double)proc.activeVoiceCount());
        r.checkVal(proc.activeGrainCount() == 0,
                   "spectralgrain: Stop drops every in-flight grain",
                   (double)proc.activeGrainCount());

        const float afterStop = playPeak(8);
        // Verified by reinstating the bug: it reads exactly 0.00000 (there are
        // no voices left to render), against 0.33737 for the fixed code - so
        // the margin is total, and 1e-4 only guards against denormal noise.
        r.checkVal(afterStop > 1e-4f,
                   "spectralgrain: node still plays after a transport Stop "
                   "(reset must not empty the voice pool)", afterStop);
    }

    // ---- SpectralGrain advances each grain once per SAMPLE, not per voice ----
    //
    // activeGrains is one pool shared by all voices, but the render loop used
    // to sit inside the per-voice loop, so every grain's read position was
    // stepped once per ACTIVE VOICE. With V voices sounding, grains played V
    // times too fast (V times shorter, and at V times the pitch ratio) and were
    // summed V times.
    //
    // The observable that pins this down is grain LIFETIME, via the steady-state
    // cloud size. Each voice spawns at `Density` grains/sec and each grain lives
    // `Grain Size` seconds, so at equilibrium:
    //
    //     grains == voices x Density x GrainSize
    //
    // With the bug the lifetime is GrainSize/V instead, which cancels the V and
    // pins the count at Density x GrainSize regardless of how many notes are
    // held. So the single-voice case is IDENTICAL either way (that's the control
    // below) and the polyphonic case differs by exactly the voice count.
    {
        auto steadyGrains = [](const std::vector<int>& notes, float grainMs) {
            Node node;
            node.id     = 1;
            node.name   = "selftest-spectralgrain-rate";
            node.script = "__spectralgrain__:exp(-f/10)";
            node.params.push_back({ "Density",   100.0f, 1.0f, 2000.0f });
            node.params.push_back({ "Grain Size", grainMs, 1.0f, 500.0f });
            node.params.push_back({ "Volume",      0.5f, 0.0f,    1.0f });
            // Long sustain so every voice is still held (and so still spawning)
            // for the whole measurement.
            node.ahdsrEnvelope.attackMs = 1.0f;
            node.ahdsrEnvelope.decayMs  = 1.0f;
            node.ahdsrEnvelope.sustain  = 1.0f;

            const int bs = 512;
            SpectralGrainProcessor proc(node);
            proc.prepareToPlay(44100.0, bs);
            juce::AudioBuffer<float> buf(2, bs);

            // 40 blocks = ~465 ms, i.e. ~4.6 grain lifetimes: long past the
            // point where spawning and expiry balance.
            for (int b = 0; b < 40; ++b) {
                buf.clear();
                juce::MidiBuffer midi;
                if (b == 0)
                    for (int n : notes)
                        midi.addEvent(juce::MidiMessage::noteOn(1, n, (juce::uint8)110), 0);
                proc.processBlock(buf, midi);
            }
            return std::pair<double, double>{ (double)proc.activeGrainCount(),
                                              (double)proc.activeVoiceCount() };
        };

        // All notes are 69 (A4) so every grain's rate is exactly 1.0 and the
        // derivation above has no pitch term. Four note-ons on the same number
        // still take four separate voice slots.
        auto [g1, v1] = steadyGrains({ 69 },                 100.0f);
        auto [g4, v4] = steadyGrains({ 69, 69, 69, 69 },     100.0f);

        r.checkVal(v1 == 1.0 && v4 == 4.0,
                   "spectralgrain: the rate test really is holding 1 vs 4 voices", v4);

        // Density 100 x GrainSize 0.1 s = 10 grains per voice.
        r.checkVal(g1 > 8.0 && g1 < 12.0,
                   "spectralgrain: one voice holds Density x GrainSize grains", g1);
        // Verified by reinstating the bug (grain positions stepped once per
        // active voice): grains-per-voice reads 3.00 against 10.00 for the
        // fixed code, i.e. the cloud stops growing with polyphony. 8..12 sits
        // well clear of 3 while still allowing spawn-phase jitter.
        const double perVoice = g4 / juce::jmax(1.0, v4);
        r.checkVal(perVoice > 8.0 && perVoice < 12.0,
                   "spectralgrain: grain lifetime is independent of how many "
                   "voices are sounding (grains per voice, 4-note chord)", perVoice);

        // ...and the same derivation is what proves Grain Size still means
        // something past the FFT frame. kGrainFFTSize is 1024 samples = 23.2 ms
        // at 44.1 kHz; grain length used to be clamped to it, so 50 ms and
        // 100 ms produced IDENTICAL clouds (~2 grains each at Density 100).
        // Now they scale with the knob: 100 x 0.05 = 5 and 100 x 0.1 = 10.
        auto [gShort, vShort] = steadyGrains({ 69 }, 50.0f);
        r.checkVal(gShort > 3.5 && gShort < 6.5,
                   "spectralgrain: Grain Size 50 ms holds ~5 grains at Density 100",
                   gShort);
        const double sizeRatio = g1 / juce::jmax(1.0, gShort);
        // Verified by reinstating the clamp: both sides then read 2.00 grains
        // and this ratio reads exactly 1.00000 - the knob was completely inert
        // above 23 ms. Fixed code reads 2.00000 (10.00 vs 5.00 grains).
        r.checkVal(sizeRatio > 1.6,
                   "spectralgrain: Grain Size keeps scaling past the FFT frame "
                   "(100 ms vs 50 ms grain count ratio)", sizeRatio);
    }

    // ---- PitchDetectorProcessor renders without allocating ------------------
    //
    // The per-hop analysis copy was a local std::vector<float> sized to
    // `window`, which is DERIVED from the Min Hz param - so turning that knob
    // resized a heap buffer on the audio thread. It's a member sized once to
    // kMaxWindow now. The sweep moves Min Hz across its whole range, which is
    // exactly the motion that used to reallocate.
    {
        Node node;
        node.id   = 1;
        node.name = "selftest-pitchdetect";
        node.params.push_back({ "Algorithm",    0.0f,   0.0f,     1.0f });
        node.params.push_back({ "Hop",          0.0f,   0.0f,  8192.0f });
        node.params.push_back({ "Mapping",      0.0f,   0.0f,     1.0f });
        node.params.push_back({ "Min Hz",      50.0f,   5.0f,  1000.0f });
        node.params.push_back({ "Max Hz",    2000.0f, 100.0f, 10000.0f });
        node.params.push_back({ "Detected Hz",  0.0f,   0.0f, 20000.0f });

        auto setParam = [&](const char* name, float v) {
            for (auto& p : node.params) if (p.name == name) p.value = v;
        };
        auto getParam = [&](const char* name) {
            for (auto& p : node.params) if (p.name == name) return p.value;
            return 0.0f;
        };

        const double sr = 44100.0;
        const int maxBlock = 512;
        PitchDetectorProcessor proc(node);
        proc.prepareToPlay(sr, maxBlock);

        // 4 channels: audio in on 0/1, the normalized pitch comes out on 2.
        juce::AudioBuffer<float> buf(4, maxBlock);
        int64_t phasePos = 0;
        float sigLo = 1.0f, sigHi = -1.0f;
        auto runBlock = [&](int len, double hz) {
            juce::AudioBuffer<float> view(buf.getArrayOfWritePointers(), 4, len);
            view.clear();
            for (int s = 0; s < len; ++s) {
                float v = 0.4f * (float)std::sin(
                    juce::MathConstants<double>::twoPi * hz * (double)(phasePos + s) / sr);
                view.setSample(0, s, v);
                view.setSample(1, s, v);
            }
            phasePos += len;
            juce::MidiBuffer midi;
            proc.processBlock(view, midi);
            for (int s = 0; s < len; ++s) {
                const float o = view.getSample(2, s);
                sigLo = std::min(sigLo, o);
                sigHi = std::max(sigHi, o);
            }
        };

        // Warm up: fill the ring buffer at the widest window (lowest Min Hz)
        // so every scratch buffer is at its final size before the snapshot.
        setParam("Min Hz", 5.0f);
        for (int i = 0; i < 64; ++i) runBlock(maxBlock, 440.0);

        const size_t before = proc.scratchCapacityBytes();

        const int lens[] = { 512, 480, 256, 64, 333, 512 };
        for (int pass = 0; pass < 60; ++pass) {
            setParam("Min Hz",    5.0f + (float)((pass * 37) % 400));
            setParam("Max Hz",  800.0f + (float)((pass * 211) % 4000));
            setParam("Algorithm", (float)(pass % 2));
            setParam("Mapping",   (float)(pass % 2));
            setParam("Hop",       (float)(64 << (pass % 5)));
            runBlock(lens[pass % 6], 440.0);
        }

        const size_t after = proc.scratchCapacityBytes();
        r.checkVal(after == before,
                   "pitchdetect: 60 blocks sweeping Min Hz / Algorithm / Hop "
                   "allocate nothing (scratch capacity growth, bytes)",
                   (double)after - (double)before);

        // Non-vacuity: the detector must actually have locked onto the 440 Hz
        // tone at some point, or the expensive path never ran.
        setParam("Min Hz",  50.0f);
        setParam("Max Hz", 2000.0f);
        setParam("Hop",      0.0f);
        for (int i = 0; i < 32; ++i) runBlock(maxBlock, 440.0);
        const float detected = getParam("Detected Hz");
        r.checkVal(std::abs(detected - 440.0f) < 10.0f,
                   "pitchdetect: locked onto the 440 Hz probe tone (Hz)", detected);
        r.checkVal(sigHi > 1e-4f && sigHi <= 1.0f && sigLo >= 0.0f,
                   "pitchdetect: normalized pitch emitted on the Signal output "
                   "throughout the sweep (max)", sigHi);
    }

    // ---- Song length: mid-bar content end plays in full (no bar-rounding) ----
    {
        // Bug: a MIDI track whose last clip ends mid-bar (e.g. at beat 1.0 of a
        // 4-beat bar) had the song-end MARKER drawn at the exact clip end, but
        // PLAYBACK ran to the bar-rounded end (beat 4). The fix routes the
        // auto-derived playback length through contentEndBeats() (un-rounded),
        // so effectiveSongLengthBeats() matches the marker. getTimelineBeats()
        // still rounds up for the grid/display width.
        NodeGraph g;
        int tlId = g.addNode("Track", NodeType::MidiTimeline, {},
                             { Pin{0, "MIDI Out", PinKind::Midi, false} }).id;
        Node* tl = g.findNode(tlId);
        Clip c{}; c.name = "clip"; c.startBeat = 0.0f; c.lengthBeats = 1.0f;
        tl->clips.push_back(c);

        r.checkVal(std::abs(g.contentEndBeats() - 1.0) < 1e-6,
                   "song-len: exact content end is un-rounded (1.0 beat)",
                   (float) g.contentEndBeats());
        r.checkVal(std::abs(g.effectiveSongLengthBeats() - 1.0) < 1e-6,
                   "song-len: auto playback length matches the mid-bar marker",
                   (float) g.effectiveSongLengthBeats());
        // Display width still rounds up to a full 4-beat bar.
        r.checkVal(std::abs(g.getTimelineBeats(*tl) - 4.0f) < 1e-6,
                   "song-len: timeline display width still rounds up to a bar",
                   g.getTimelineBeats(*tl));

        // An explicit override still wins verbatim.
        g.songLengthBeats = 2.5;
        r.checkVal(std::abs(g.effectiveSongLengthBeats() - 2.5) < 1e-6,
                   "song-len: explicit override wins over content end",
                   (float) g.effectiveSongLengthBeats());
        // growSongLengthToContent never shrinks an override that already
        // exceeds content, and grows to the exact (un-rounded) content end.
        g.songLengthBeats = 0.5;          // shorter than the 1.0-beat clip
        g.growSongLengthToContent();
        r.checkVal(std::abs(g.songLengthBeats - 1.0) < 1e-6,
                   "song-len: grow-to-content uses exact (mid-bar) clip end",
                   (float) g.songLengthBeats);
    }

    // ---- Wavetable library: unique "(copy N)" names on duplicate ------------
    {
        // Duplicating a library waveform must give the copy an incremented
        // "(copy N)" name instead of a bare "(copy)" that collides with prior
        // copies. makeUniqueCopyName picks the lowest free N and strips an
        // existing copy suffix so repeated duplication increments.
        WavetableDoc doc;
        doc.addLibraryEntry(std::make_unique<LayeredWaveform>(), "Saw");

        const std::string c1 = doc.makeUniqueCopyName("Saw");
        r.check(c1 == "Saw (copy 1)", "wt-copy: first copy is \"(copy 1)\"");
        doc.addLibraryEntry(std::make_unique<LayeredWaveform>(), c1);

        const std::string c2 = doc.makeUniqueCopyName("Saw");
        r.check(c2 == "Saw (copy 2)", "wt-copy: second copy increments to \"(copy 2)\"");
        doc.addLibraryEntry(std::make_unique<LayeredWaveform>(), c2);

        // Duplicating an existing copy strips its suffix and continues the run
        // off the base name rather than nesting ("Saw (copy 1) (copy 1)").
        const std::string c3 = doc.makeUniqueCopyName("Saw (copy 1)");
        r.check(c3 == "Saw (copy 3)",
                "wt-copy: duplicating a copy increments off the base, no nesting");

        // A name that merely contains "(copy...)" as real text (not our suffix)
        // is left intact as the base.
        const std::string keep = doc.makeUniqueCopyName("My (copyright) wave");
        r.check(keep == "My (copyright) wave (copy 1)",
                "wt-copy: non-suffix parentheses are preserved as the base name");
    }

    // ---- Named morph params: legacy "Warp N" migration (inc 2) --------------
    {
        // A pre-warpSlot project stored warp params as "Warp 1"/"Warp 2" with
        // warpSlot == -1, and a modulation pin could be bound to one. The
        // load-time reconcileAllWarpParams pass must adopt them by parsing the
        // slot (so a bound modPin stays attached) and rename them to the named-
        // morph label, WITHOUT moving the modPin's paramIndex.
        NodeGraph g;
        int nId = g.addNode("wt", NodeType::TerrainSynth, {}, {}).id;
        WavetableDoc doc;
        int fid = doc.addLibraryEntry(std::make_unique<LayeredWaveform>(), "w");
        WarpOp o0; o0.method = WarpMethod::Wavefold; o0.amount = 0.3f; o0.enabled = true;
        WarpOp o1; o1.method = WarpMethod::PwmSkew;  o1.amount = 0.7f; o1.enabled = true;
        doc.libraryFrameById(fid)->morphChain = { o0, o1 };
        Node* nd = g.findNode(nId);
        nd->script = doc.encode();
        // Inject legacy params (warpSlot defaults to -1) + a modPin on "Warp 2".
        // Real legacy params load from disk with warpSlot == -1 (the key didn't
        // exist pre-inc-2); set it explicitly so the test simulates that exactly.
        Param lp0{}; lp0.name = "Warp 1"; lp0.value = lp0.baseValue = 0.3f;
        lp0.minVal = 0; lp0.maxVal = 1; lp0.format = "%.2f"; lp0.warpSlot = -1;
        Param lp1{}; lp1.name = "Warp 2"; lp1.value = lp1.baseValue = 0.7f;
        lp1.minVal = 0; lp1.maxVal = 1; lp1.format = "%.2f"; lp1.warpSlot = -1;
        nd->params.push_back(lp0);
        nd->params.push_back(lp1);
        const int legacyIdx1 = (int)nd->params.size() - 1; // index of "Warp 2"
        int pinId = g.allocId();
        nd->pinsIn.push_back({pinId, "Mod: Warp 2", PinKind::Param, true, 1});
        Node::ModPin mp; mp.paramIndex = legacyIdx1; mp.pinId = pinId;
        mp.mode = Node::ModPin::Mode::Modulate;
        nd->modPins.push_back(mp);

        reconcileAllWarpParams(g);

        nd = g.findNode(nId);
        const Param* s0 = nullptr; const Param* s1 = nullptr;
        int s1Idx = -1;
        for (int i = 0; i < (int)nd->params.size(); ++i) {
            if (nd->params[i].warpSlot == 0) s0 = &nd->params[i];
            else if (nd->params[i].warpSlot == 1) { s1 = &nd->params[i]; s1Idx = i; }
        }
        r.check(s0 && s1 && s0->name == "Wavefold" && s1->name == "PWM Skew",
                "named-morph: legacy 'Warp N' params adopt warpSlot + method-name labels");
        // The modPin must still point at the (renamed) slot-1 param, and its pin
        // label must follow the new name.
        bool pinOk = false;
        for (const auto& m : nd->modPins)
            if (m.paramIndex == s1Idx && m.pinId == pinId) pinOk = true;
        const Pin* movedPin = nullptr;
        for (const auto& p : nd->pinsIn) if (p.id == pinId) movedPin = &p;
        r.check(pinOk, "named-morph: migrated modPin stays bound to slot-1 param");
        r.check(movedPin && movedPin->name == "Mod: PWM Skew",
                std::string("named-morph: migrated modPin pin relabelled (got '") +
                (movedPin ? movedPin->name : std::string("<none>")) + "')");
    }

    // ---- Pinned morph param survives a full save/load round-trip (regression) ----
    {
        // User report: pinning a morph (or layer) param, saving, and reloading
        // dropped the pin. A frame-scope warp param is reconciled on load by
        // syncWarpParamsForNode, which REMOVES any warp param whose (warpFrameId,
        // warpSlot) no longer addresses a live op in the decoded doc - taking its
        // modPin + "Mod:" pin with it. So the pin survives only if the node script
        // round-trips the morph chain AND warpFrameId round-trips and still matches
        // the decoded library entry id. Exercise the FULL writeProject->readProject
        // path (the earlier per-layer test had no chain + no pin, so it missed this).
        NodeGraph g;
        int nId = g.addNode("wt", NodeType::TerrainSynth, {}, {}).id;
        WavetableDoc doc;
        int fid = doc.addLibraryEntry(std::make_unique<LayeredWaveform>(), "w");
        WarpOp op; op.method = WarpMethod::HardClip; op.amount = 0.5f; op.enabled = true;
        doc.libraryFrameById(fid)->morphChain = { op };
        Node* nd = g.findNode(nId);
        nd->script = doc.encode();
        Param wp; wp.name = "HardClip"; wp.value = wp.baseValue = 0.5f;
        wp.minVal = 0; wp.maxVal = 1; wp.format = "%.2f";
        wp.warpLayer = -1; wp.warpSlot = 0; wp.warpFrameId = fid;
        nd->params.push_back(wp);
        const int wpIdx = (int)nd->params.size() - 1;
        int pinId = g.allocId();
        nd->pinsIn.push_back({pinId, "Mod: HardClip", PinKind::Param, true, 1});
        { Node::ModPin mp; mp.paramIndex = wpIdx; mp.pinId = pinId;
          mp.mode = Node::ModPin::Mode::Modulate; nd->modPins.push_back(mp); }

        std::ostringstream oss;
        ProjectFile::writeProject(oss, g, nullptr, /*includeView*/false,
                                  /*includeBlobs*/true);
        NodeGraph g2;
        std::istringstream iss(oss.str());
        ProjectFile::readProject(iss, g2, nullptr);
        Node* nd2 = g2.findNode(nId);
        const Param* rp = nullptr; int rpIdx = -1;
        if (nd2) for (int i = 0; i < (int)nd2->params.size(); ++i)
            if (nd2->params[i].warpLayer == -1 && nd2->params[i].warpSlot == 0) {
                rp = &nd2->params[i]; rpIdx = i;
            }
        r.check(rp != nullptr,
                "pin round-trip: frame-scope warp param survives save/load");
        bool modOk = false;
        if (nd2) for (auto& m : nd2->modPins)
            if (m.paramIndex == rpIdx && m.pinId == pinId) modOk = true;
        r.check(modOk, "pin round-trip: modPin survives and stays bound to the param");
        bool pinOk = false;
        if (nd2) for (auto& p : nd2->pinsIn)
            if (p.id == pinId && p.name.rfind("Mod:", 0) == 0) pinOk = true;
        r.check(pinOk, "pin round-trip: 'Mod:' input pin survives");
    }

    // ---- Dangling modulation pin: prune orphan "Mod:"/"Set:" pins -----------
    {
        // Reproduces a real corrupted project (after_j.ssp): a warp param "Drive 1"
        // with a VALID backing modPin/pin (851), plus a DUPLICATE "Mod: Drive 1"
        // input pin (847) that NO modPin references - a ghost modulation input
        // stranded by a historical reconcile/remap bug, then preserved by
        // save/load. reconcileAllWarpParams must prune the orphan while leaving the
        // valid pin (and an unrelated valid Position pin) untouched.
        NodeGraph g;
        int nId = g.addNode("wt", NodeType::TerrainSynth, {}, {}).id;
        WavetableDoc doc;
        int fid = doc.addLibraryEntry(std::make_unique<LayeredWaveform>(), "w");
        WarpOp o0; o0.method = WarpMethod::SoftClip; o0.amount = 0.4f; o0.enabled = true;
        doc.libraryFrameById(fid)->morphChain = { o0 };   // single op -> one "Drive 1" param at slot 0
        Node* nd = g.findNode(nId);
        nd->script = doc.encode();

        // The warp-slot param (index 0 here) that the valid pin will bind to.
        Param dp{}; dp.name = "Drive 1"; dp.value = dp.baseValue = 0.4f;
        dp.minVal = 0; dp.maxVal = 1; dp.format = "%.2f"; dp.warpSlot = 0;
        nd->params.push_back(dp);
        const int driveIdx = (int)nd->params.size() - 1;
        // An unrelated, correctly-backed Position param + pin (must NOT be pruned).
        Param pp{}; pp.name = "Position"; pp.value = pp.baseValue = 0.5f;
        pp.minVal = 0; pp.maxVal = 1; pp.format = "%.2f";
        nd->params.push_back(pp);
        const int posIdx = (int)nd->params.size() - 1;

        int ghostPin = g.allocId();   // 847-analogue: no modPin backs it
        nd->pinsIn.push_back({ghostPin, "Mod: Drive 1", PinKind::Param, true, 1});
        int validPin = g.allocId();   // 851-analogue: backed by a modPin
        nd->pinsIn.push_back({validPin, "Mod: Drive 1", PinKind::Param, true, 1});
        int posPin = g.allocId();
        nd->pinsIn.push_back({posPin, "Mod: Position X", PinKind::Param, true, 1});
        { Node::ModPin m; m.paramIndex = driveIdx; m.pinId = validPin; nd->modPins.push_back(m); }
        { Node::ModPin m; m.paramIndex = posIdx;   m.pinId = posPin;   nd->modPins.push_back(m); }

        reconcileAllWarpParams(g);   // -> syncWarpParamsForNode -> pruneOrphanModPins

        nd = g.findNode(nId);
        auto hasPin = [&](int id) {
            for (const auto& p : nd->pinsIn) if (p.id == id) return true;
            return false;
        };
        r.check(!hasPin(ghostPin),
                "prune: dangling 'Mod: Drive 1' pin (no backing modPin) is removed");
        r.check(hasPin(validPin) && hasPin(posPin),
                "prune: validly-backed Mod pins survive the prune");
        int modPinCount = (int)nd->modPins.size();
        r.checkVal(modPinCount == 2,
                   "prune: both valid modPins are retained", modPinCount);
        // Idempotent: a second pass removes nothing.
        int again = pruneOrphanModPins(g, nId);
        r.checkVal(again == 0, "prune: idempotent on an already-consistent node", again);
    }

    // ---- Per-layer warp params: warpLayer round-trip (inc 4) ----------------
    {
        // A per-layer Type-2 warp param carries warpLayer >= 0 (which layer's
        // chain it indexes) alongside warpSlot. Both must survive save/load so a
        // pinned per-layer morph reattaches to the right op after reload.
        NodeGraph g;
        int nId = g.addNode("wt", NodeType::TerrainSynth, {}, {}).id;
        Node* nd = g.findNode(nId);
        Param p; p.name = "Fold 1"; p.value = p.baseValue = 0.4f;
        p.minVal = 0; p.maxVal = 1; p.format = "%.2f";
        p.warpSlot = 0; p.warpLayer = 2;   // op 0 of layer 2
        nd->params.push_back(p);

        std::ostringstream oss;
        ProjectFile::writeProject(oss, g, nullptr, /*includeView*/false,
                                  /*includeBlobs*/true);
        std::string saved = oss.str();
        r.check(saved.find("warpLayer=2") != std::string::npos,
                "per-layer warp: save emits warpLayer for a per-layer param");

        NodeGraph g2;
        std::istringstream iss(saved);
        ProjectFile::readProject(iss, g2, nullptr);
        Node* nd2 = g2.findNode(nId);
        const Param* rp = nullptr;
        if (nd2) for (auto& q : nd2->params) if (q.name == "Fold 1") rp = &q;
        r.check(rp && rp->warpSlot == 0 && rp->warpLayer == 2,
                "per-layer warp: warpSlot + warpLayer survive round-trip");
    }

    // ---- Per-layer warp params: reconcilePerLayerWarpParams (inc 4) ---------
    {
        // reconcilePerLayerWarpParams keys per-layer params by (warpLayer,
        // warpSlot) against a 2-D set of layer chains. It NEVER adds (per-layer
        // params are created on demand); it removes params whose op is gone
        // (dropping their mod pins + cables, remapping survivors) and relabels
        // survivors from the op method. Frame-scope params (warpLayer == -1) are
        // left untouched.
        NodeGraph g;
        int nId = g.addNode("wt", NodeType::TerrainSynth, {}, {}).id;
        Node* nd = g.findNode(nId);

        // All params belong to one frame (library id 7); the per-layer reconcile
        // is keyed by (warpFrameId, warpLayer, warpSlot).
        const int frameId = 7;
        auto makeParam = [frameId](const char* name, int layer, int slot, float v) {
            Param p; p.name = name; p.value = p.baseValue = v;
            p.minVal = 0; p.maxVal = 1; p.format = "%.2f";
            p.warpSlot = slot; p.warpLayer = layer; p.warpFrameId = frameId;
            return p;
        };
        // A frame-scope warp param (must be ignored by the per-layer reconcile).
        nd->params.push_back(makeParam("Fold 1", -1, 0, 0.2f));
        // Per-layer params: layer0 slot0 (Fold), layer0 slot1 (Drive),
        // layer1 slot0 (Width). Give layer0 slot1 a bound modPin.
        nd->params.push_back(makeParam("Fold 1",  0, 0, 0.3f)); // idx 1
        nd->params.push_back(makeParam("Drive 2", 0, 1, 0.5f)); // idx 2  (pinned)
        nd->params.push_back(makeParam("Width 1", 1, 0, 0.7f)); // idx 3
        const int pinnedIdx = 2;
        int pinId = g.allocId();
        nd->pinsIn.push_back({pinId, "Mod: Drive 2", PinKind::Param, true, 1});
        Node::ModPin mp; mp.paramIndex = pinnedIdx; mp.pinId = pinId;
        mp.mode = Node::ModPin::Mode::Modulate;
        nd->modPins.push_back(mp);

        // (a) All ops live -> nothing removed, survivors relabelled to method.
        std::vector<std::vector<WarpOp>> chains(2);
        auto op = [](WarpMethod m) { WarpOp o; o.method = m; o.amount = 0.5f; o.enabled = true; return o; };
        chains[0] = { op(WarpMethod::Wavefold), op(WarpMethod::SoftClip) };
        chains[1] = { op(WarpMethod::PwmSkew) };
        reconcilePerLayerWarpParams(g, nId, frameId, chains);
        nd = g.findNode(nId);
        r.check(nd->params.size() == 4,
                "per-layer reconcile: all-live keeps every param (no add, no remove)");

        // (b) Drop layer0 slot1 (SoftClip) -> its param removed, modPin + pin +
        //     cable dropped, the layer1 param's index remapped past the hole.
        chains[0] = { op(WarpMethod::Wavefold) };  // slot1 gone
        reconcilePerLayerWarpParams(g, nId, frameId, chains);
        nd = g.findNode(nId);
        bool drivePresent = false, foldPL = false, widthPL = false, foldFrame = false;
        for (auto& q : nd->params) {
            if (q.warpLayer == 0 && q.warpSlot == 1) drivePresent = true;
            if (q.warpLayer == 0 && q.warpSlot == 0) foldPL = true;
            if (q.warpLayer == 1 && q.warpSlot == 0) widthPL = true;
            if (q.warpLayer == -1) foldFrame = true;
        }
        r.check(!drivePresent && foldPL && widthPL && foldFrame,
                "per-layer reconcile: dead op's param removed; others (incl frame-scope) kept");
        bool pinGone = true;
        for (auto& p : nd->pinsIn) if (p.id == pinId) pinGone = false;
        bool modPinGone = true;
        for (auto& m : nd->modPins) if (m.pinId == pinId) modPinGone = false;
        r.check(pinGone && modPinGone,
                "per-layer reconcile: removed param's mod pin + cable are dropped");
        // The surviving modPins (none here) must still point at valid params; the
        // Width param must still be addressable by (1,0) with a sane index.
        const Param* widthP = nullptr;
        for (auto& q : nd->params) if (q.warpLayer == 1 && q.warpSlot == 0) widthP = &q;
        r.check(widthP && widthP->name == "L2 PWM Skew",
                "per-layer reconcile: remapped survivor keeps its identity");

        // (c) Method change on layer0 slot0 (Wavefold -> SoftClip) relabels the
        //     surviving per-layer param "L1 Wavefold" -> "L1 Soft Clip" in place.
        chains[0] = { op(WarpMethod::SoftClip) };
        reconcilePerLayerWarpParams(g, nId, frameId, chains);
        nd = g.findNode(nId);
        const Param* relabelled = nullptr;
        for (auto& q : nd->params) if (q.warpLayer == 0 && q.warpSlot == 0) relabelled = &q;
        r.check(relabelled && relabelled->name == "L1 Soft Clip",
                "per-layer reconcile: method change relabels survivor by op method");
    }

    // ---- FrequencyGraph asset kind: SpectralCurve payload, all source forms -
    {
        // The FrequencyGraph kind stores SpectralCurve::encode() payloads. It is
        // a leaf asset (no child ids) shared by the Spectral FFT type, the EQ
        // node, and the Spectrum Tap. The key requirement (req C) is that every
        // authoring form survives a round-trip: the equation text + language for
        // formula curves, the control points for Drawn/Points, and the per-sample
        // buffer for Drawn/Freehand.

        // Tag <-> kind round-trips through the stable string.
        AssetKind k;
        r.check(std::string(assetKindTag(AssetKind::FrequencyGraph)) == "frequency" &&
                    assetKindFromTag("frequency", k) && k == AssetKind::FrequencyGraph,
                "freqgraph: AssetKind <-> 'frequency' tag round-trips");

        // (1) Equation form with a non-default language: expression + lang survive.
        SpectralCurve eq;
        eq.mode = SpectralCurve::Equation;
        eq.expression = "exp(-f/13)";
        eq.lang = ShapeLang::Lua;
        {
            SpectralCurve dec;
            r.check(SpectralCurve::decode(eq.encode(), dec) &&
                        dec.mode == SpectralCurve::Equation &&
                        dec.expression == "exp(-f/13)" && dec.lang == ShapeLang::Lua,
                    "freqgraph: equation expression + language survive encode/decode");
        }

        // (2) Drawn/Points form: control points survive (this is the source form,
        // not just a baked array).
        SpectralCurve pts;
        pts.mode = SpectralCurve::Drawn;
        pts.freehandMode = false;
        pts.drawnPoints = { {0.0f, 1.0f}, {0.5f, 0.25f}, {1.0f, 0.0f} };
        {
            SpectralCurve dec;
            r.check(SpectralCurve::decode(pts.encode(), dec) &&
                        dec.mode == SpectralCurve::Drawn && !dec.freehandMode &&
                        dec.drawnPoints.size() == 3 &&
                        std::abs(dec.drawnPoints[1].first - 0.5f) < 1e-4f &&
                        std::abs(dec.drawnPoints[1].second - 0.25f) < 1e-4f,
                    "freqgraph: drawn control points survive encode/decode");
        }

        // (3) Drawn/Freehand form: per-sample painted buffer survives.
        SpectralCurve fh;
        fh.mode = SpectralCurve::Drawn;
        fh.freehandMode = true;
        fh.drawnSamples.assign(512, 0.0f);
        for (int i = 0; i < 512; ++i) fh.drawnSamples[i] = (float) i / 511.0f;
        {
            SpectralCurve dec;
            r.check(SpectralCurve::decode(fh.encode(), dec) &&
                        dec.mode == SpectralCurve::Drawn && dec.freehandMode &&
                        dec.drawnSamples.size() == 512 &&
                        std::abs(dec.drawnSamples[256] - 256.0f / 511.0f) < 1e-3f,
                    "freqgraph: freehand per-sample buffer survives encode/decode");
        }

        // Publish all three into a library; content-hash dedup distinguishes them.
        NodeGraph g;
        int idEq  = g.assets.add(AssetKind::FrequencyGraph, "EQ tilt", "", eq.encode());
        int idPts = g.assets.add(AssetKind::FrequencyGraph, "drawn", "", pts.encode());
        int idFh  = g.assets.add(AssetKind::FrequencyGraph, "freehand", "", fh.encode());
        r.check(idEq != idPts && idPts != idFh && idEq != idFh,
                "freqgraph: three distinct curves get three distinct ids");
        r.check(g.assets.findByHash(AssetLibrary::computeHash(
                    AssetKind::FrequencyGraph, "", eq.encode()))->id == idEq,
                "freqgraph: findByHash locates the equation curve by content");

        // Re-adding an identical equation curve hashes equal (dedup candidate).
        r.check(AssetLibrary::computeHash(AssetKind::FrequencyGraph, "", eq.encode()) ==
                    AssetLibrary::computeHash(AssetKind::FrequencyGraph, "", eq.encode()),
                "freqgraph: identical content hashes equal");

        // Save/load: the FrequencyGraph entries survive the project round-trip
        // with kind + payload intact, and the decoded source forms still match.
        std::ostringstream oss;
        ProjectFile::writeProject(oss, g, nullptr, false, true);
        NodeGraph g2; std::istringstream iss(oss.str());
        ProjectFile::readProject(iss, g2, nullptr);
        const AssetEntry* le = g2.assets.find(idEq);
        const AssetEntry* lp = g2.assets.find(idPts);
        const AssetEntry* lf = g2.assets.find(idFh);
        r.check(le && le->kind == AssetKind::FrequencyGraph &&
                lp && lp->kind == AssetKind::FrequencyGraph &&
                lf && lf->kind == AssetKind::FrequencyGraph,
                "freqgraph: all three entries survive save/load as FrequencyGraph");
        {
            SpectralCurve dEq, dPts, dFh;
            r.check(le && SpectralCurve::decode(le->payload, dEq) &&
                        dEq.expression == "exp(-f/13)" && dEq.lang == ShapeLang::Lua,
                    "freqgraph: equation source form survives save/load");
            r.check(lp && SpectralCurve::decode(lp->payload, dPts) &&
                        dPts.drawnPoints.size() == 3,
                    "freqgraph: drawn points survive save/load");
            r.check(lf && SpectralCurve::decode(lf->payload, dFh) &&
                        dFh.freehandMode && dFh.drawnSamples.size() == 512,
                    "freqgraph: freehand samples survive save/load");
        }
    }

    // ---- convolution IR library round-trip ----------------------------------
    {
        // A Convolution Filter's impulse response can be published to the asset
        // library as a ConvolutionIR asset and loaded back into any Convolution
        // Filter. The payload is exactly the node-script encoding
        // (ConvolutionProcessor::encodeIR), so publish -> decode reproduces the IR.

        // Tag <-> kind round-trips through the stable string.
        AssetKind k;
        r.check(std::string(assetKindTag(AssetKind::ConvolutionIR)) == "convolution_ir" &&
                    assetKindFromTag("convolution_ir", k) && k == AssetKind::ConvolutionIR,
                "convlib: AssetKind <-> 'convolution_ir' tag round-trips");

        std::vector<float> ir = { 1.0f, -0.5f, 0.25f, 0.0f, 0.125f, -0.0625f };
        std::string payload = ConvolutionProcessor::encodeIR(ir);

        NodeGraph g;
        int idA = g.assets.add(AssetKind::ConvolutionIR, "Small Room", "", payload);
        int idB = g.assets.add(AssetKind::ConvolutionIR, "Big Hall", "",
                               ConvolutionProcessor::encodeIR({ 1.0f, 0.0f, 0.0f }));
        r.check(idA != idB && idA >= AssetLibrary::kUserIdBase,
                "convlib: two IRs get distinct user-space ids");

        // Loading back reproduces the exact sample list.
        const AssetEntry* e = g.assets.find(idA);
        std::vector<float> back = e ? ConvolutionProcessor::decodeIR(e->payload)
                                    : std::vector<float>{};
        bool same = back.size() == ir.size();
        for (size_t i = 0; same && i < ir.size(); ++i)
            same = std::abs(back[i] - ir[i]) < 1e-6f;
        r.check(same, "convlib: stored IR decodes back to the original samples");

        // Survives a project save/load with kind + payload intact.
        std::ostringstream oss;
        ProjectFile::writeProject(oss, g, nullptr, false, true);
        NodeGraph g2; std::istringstream iss(oss.str());
        ProjectFile::readProject(iss, g2, nullptr);
        const AssetEntry* le = g2.assets.find(idA);
        r.check(le && le->kind == AssetKind::ConvolutionIR && le->payload == payload,
                "convlib: ConvolutionIR asset survives save/load");
    }

    // ---- timeline nesting: no depth cap, cycle-safe --------------------------
    {
        // MIDI timelines can be nested as children of other timelines. The
        // absolute beat offset is the sum of groupBeatOffset up the parent chain.
        // The walk uses a visited-set loop detector (not an arbitrary depth cap),
        // so nesting can go arbitrarily deep AND a corrupt cyclic chain can't hang.
        NodeGraph g;
        const int N = 64; // deliberately well past the old depth cap of 20
        std::vector<int> ids;
        ids.reserve(N);
        for (int i = 0; i < N; ++i)
            ids.push_back(g.addNode("tl", NodeType::MidiTimeline, {}, {}).id);
        // Set offsets by id AFTER creating all nodes (addNode can reallocate the
        // node vector, so never hold a Node& across an addNode call).
        for (int id : ids)
            if (auto* n = g.findNode(id)) n->groupBeatOffset = 2.0f;
        // Chain them: ids[0] is the root, each subsequent node is a child of the
        // previous one -> a 64-deep nest.
        for (int i = 1; i < N; ++i)
            g.addToGroup(ids[i - 1], ids[i]);

        // Deepest node's absolute offset = 64 * 2.0 = 128 (the old cap would have
        // truncated at 20 levels -> 40).
        r.checkVal(std::abs(g.getAbsoluteBeatOffset(ids[N - 1]) - 128.0f) < 1e-3f,
                   "nesting: 64-deep timeline chain sums the full offset (no cap)",
                   g.getAbsoluteBeatOffset(ids[N - 1]));

        // addToGroup refuses a cycle: making the root a child of the deepest node
        // would close the loop, so it must be rejected and leave the root's parent
        // unchanged (-1).
        g.addToGroup(ids[N - 1], ids[0]);
        const Node* root = g.findNode(ids[0]);
        r.check(root && root->parentGroupId == -1,
                "nesting: addToGroup refuses a cycle (deep descendant -> root)");

        // Even a forcibly-corrupted cyclic chain must terminate (not hang). Force
        // a 3-cycle by hand and confirm both chain walks return.
        int a = g.addNode("a", NodeType::MidiTimeline, {}, {}).id;
        int b = g.addNode("b", NodeType::MidiTimeline, {}, {}).id;
        int c = g.addNode("c", NodeType::MidiTimeline, {}, {}).id;
        if (auto* na = g.findNode(a)) { na->groupBeatOffset = 1.0f; na->parentGroupId = b; }
        if (auto* nb = g.findNode(b)) { nb->groupBeatOffset = 1.0f; nb->parentGroupId = c; }
        if (auto* nc = g.findNode(c)) { nc->groupBeatOffset = 1.0f; nc->parentGroupId = a; }
        float cyc = g.getAbsoluteBeatOffset(a);   // must return, not spin forever
        r.check(std::isfinite(cyc) && std::abs(cyc - 3.0f) < 1e-3f,
                "nesting: cyclic parent chain terminates (each node counted once)");
        r.check(!g.isAncestorOf(ids[0], a),
                "nesting: isAncestorOf terminates on a cyclic chain");
    }

    // ---- anchored child ripples when time is inserted/cut upstream -----------
    {
        // A nested timeline whose start offset is anchored to a named marker must
        // follow that marker when insert/deleteTime shifts it. insertTime and
        // deleteTime call resolveAnchors() internally, so the child's
        // groupBeatOffset (and cascading absoluteBeatOffset) re-read the marker's
        // new beat with no explicit call from the UI/scripting layer.
        NodeGraph g;
        int parent = g.addNode("parent", NodeType::MidiTimeline, {}, {}).id;
        int child  = g.addNode("child",  NodeType::MidiTimeline, {}, {}).id;
        g.markers.push_back(Marker{1, "cue", 8.0f});
        g.addToGroup(parent, child);
        if (auto* c = g.findNode(child)) {
            c->groupBeatOffset = 8.0f;   // starts at the marker
            c->anchorMarker = "cue";     // bound to it
        }
        g.resolveAnchors();
        r.checkVal(std::abs(g.findNode(child)->groupBeatOffset - 8.0f) < 1e-3f,
                   "anchor: child starts at its anchored marker (beat 8)",
                   g.findNode(child)->groupBeatOffset);

        // Insert 4 beats at beat 2 (all-tracks scope). Marker "cue" moves 8 -> 12,
        // and the anchored child must ripple right to follow it.
        g.insertTime(2.0f, 4.0f, -1);
        r.checkVal(std::abs(g.resolveMarkerBeat("cue") - 12.0f) < 1e-3f,
                   "anchor: insertTime shifts the marker (8 -> 12)",
                   g.resolveMarkerBeat("cue"));
        r.checkVal(std::abs(g.findNode(child)->groupBeatOffset - 12.0f) < 1e-3f,
                   "anchor: child offset ripples right with the marker (insert)",
                   g.findNode(child)->groupBeatOffset);

        // Cut 4 beats from beat 2. Marker "cue" moves 12 -> 8, child follows back.
        g.deleteTime(2.0f, 6.0f, -1);
        r.checkVal(std::abs(g.resolveMarkerBeat("cue") - 8.0f) < 1e-3f,
                   "anchor: deleteTime shifts the marker back (12 -> 8)",
                   g.resolveMarkerBeat("cue"));
        r.checkVal(std::abs(g.findNode(child)->groupBeatOffset - 8.0f) < 1e-3f,
                   "anchor: child offset ripples left with the marker (delete)",
                   g.findNode(child)->groupBeatOffset);
    }

    // ---- Spectrum Tap live-references a FrequencyGraph asset per bin --------
    {
        // encode/decode must carry the per-bin asset id alongside the cached
        // curve, and a legacy script (no #refs section) must decode to -1 ids.
        {
            std::vector<FrequencyBin> bins(2);
            bins[0].useCustomResponse = true;
            bins[0].responseCurve.expression = "exp(-f/5)";
            bins[0].responseCurveAssetId = 1000007;
            bins[1].useCustomResponse = true;
            bins[1].responseCurve.expression = "f";    // independent
            std::string script = SpectrumTapProcessor::encodeScript(
                bins, SpectrumTapProcessor::kDefaultFftSize);

            int fft; std::vector<SpectralCurve> cv; std::vector<bool> uc;
            std::vector<int> ids;
            SpectrumTapProcessor::decodeScript(script, fft, cv, uc, ids);
            r.check(ids.size() == 2 && ids[0] == 1000007 && ids[1] == -1 &&
                        uc[0] && uc[1],
                    "spectap: per-bin asset id round-trips through script codec");

            // Legacy script (no #refs) -> all ids default to -1.
            std::string legacy = "__spectrumtap__|1024|" +
                                 bins[1].responseCurve.encode();
            int f2; std::vector<SpectralCurve> c2; std::vector<bool> u2;
            std::vector<int> i2;
            SpectrumTapProcessor::decodeScript(legacy, f2, c2, u2, i2);
            r.check(i2.size() == 1 && i2[0] == -1 && u2[0],
                    "spectap: legacy script (no #refs) decodes ids as -1");
        }

        // Full live-reference flow: publish a curve, reference it from a bin
        // that caches a STALE curve, resolve, edit, save/load, erase.
        NodeGraph g;
        SpectralCurve shared;
        shared.mode = SpectralCurve::Equation;
        shared.expression = "exp(-f/7)";
        int aid = g.assets.add(AssetKind::FrequencyGraph, "shared resp", "",
                               shared.encode());

        int nId = g.addNode("tap", NodeType::Effect, {}, {}).id;
        {
            std::vector<FrequencyBin> bins(1);
            bins[0].useCustomResponse = true;
            bins[0].responseCurve.expression = "1";          // stale cache
            bins[0].responseCurveAssetId = aid;
            g.findNode(nId)->script = SpectrumTapProcessor::encodeScript(
                bins, SpectrumTapProcessor::kDefaultFftSize);
        }

        int n = resolveSpectrumTapReferences(g);
        r.checkVal(n == 1, "spectap: resolve mirrors the one referenced curve", n);
        {
            int fft; std::vector<SpectralCurve> cv; std::vector<bool> uc;
            std::vector<int> ids;
            SpectrumTapProcessor::decodeScript(g.findNode(nId)->script, fft, cv, uc, ids);
            r.check(cv.size() == 1 && cv[0].expression == "exp(-f/7)" &&
                        ids[0] == aid,
                    "spectap: resolved bin curve matches the published asset");
        }

        // Edit the asset -> propagates on next resolve.
        SpectralCurve edited; edited.expression = "exp(-f/3)";
        g.assets.update(aid, "", edited.encode());
        resolveSpectrumTapReferences(g);
        {
            int fft; std::vector<SpectralCurve> cv; std::vector<bool> uc;
            std::vector<int> ids;
            SpectrumTapProcessor::decodeScript(g.findNode(nId)->script, fft, cv, uc, ids);
            r.check(cv.size() == 1 && cv[0].expression == "exp(-f/3)",
                    "spectap: editing the asset propagates to the referencing bin");
        }

        // Save/load preserves the reference and re-resolves on load.
        std::ostringstream oss;
        ProjectFile::writeProject(oss, g, nullptr, false, true);
        NodeGraph g2; std::istringstream iss(oss.str());
        ProjectFile::readProject(iss, g2, nullptr);
        {
            int fft; std::vector<SpectralCurve> cv; std::vector<bool> uc;
            std::vector<int> ids;
            SpectrumTapProcessor::decodeScript(g2.findNode(nId)->script, fft, cv, uc, ids);
            r.check(cv.size() == 1 && ids[0] == aid &&
                        cv[0].expression == "exp(-f/3)",
                    "spectap: bin reference re-resolves after save/load");
        }

        // Erase the asset -> bin detaches to independent, keeps its last curve.
        g.assets.erase(aid);
        resolveSpectrumTapReferences(g);
        {
            int fft; std::vector<SpectralCurve> cv; std::vector<bool> uc;
            std::vector<int> ids;
            SpectrumTapProcessor::decodeScript(g.findNode(nId)->script, fft, cv, uc, ids);
            r.check(cv.size() == 1 && ids[0] == -1 &&
                        cv[0].expression == "exp(-f/3)",
                    "spectap: erased asset -> bin falls back to independent");
        }

        // Allocation-freedom on the audio thread. processBlock used to build
        // three std::vectors sized by the bin count on EVERY callback
        // (binParamIdx, sigOut, customTarget). They now live in rebuildBins(),
        // which only runs when the bin count actually changes - and that
        // changes the node's pin count, so it happens off the audio thread via
        // a graph rebuild. Capacity is the observable proxy: any growth means
        // processBlock reached the allocator.
        {
            NodeGraph g3;
            int tapId = g3.addNode("tap", NodeType::Effect, {}, {}).id;
            Node& nd = *g3.findNode(tapId);
            // "Bin N: ..." params carry centre freq in minVal, bandwidth in maxVal.
            const float centres[] = { 100.0f, 400.0f, 1200.0f, 4000.0f };
            for (int i = 0; i < 4; ++i)
                nd.params.push_back({ "Bin " + std::to_string(i + 1) + ": tap",
                                      0.0f, centres[i], centres[i] * 0.5f });
            nd.script = "__spectrumtap__";     // all bins in biquad mode

            const double sr2 = 44100.0;
            const int maxBlock = 1024;
            SpectrumTapProcessor proc(nd);
            proc.prepareToPlay(sr2, maxBlock);

            // Channels: 0/1 audio passthrough, 2+ one Signal Out per bin.
            juce::AudioBuffer<float> buf(2 + 4, maxBlock);
            juce::MidiBuffer mb;
            auto fill = [&](int len) {
                for (int c = 0; c < buf.getNumChannels(); ++c)
                    for (int i = 0; i < len; ++i)
                        buf.setSample(c, i, c < 2
                            ? 0.4f * (float)std::sin(6.28318530718 * 400.0 * i / sr2)
                            : 0.0f);
            };

            fill(maxBlock);
            proc.processBlock(buf, mb);        // warm up, settle capacities
            const size_t before = proc.scratchCapacityBytes();

            const int lens[] = { 1024, 512, 333, 64, 1024, 480 };
            for (int pass = 0; pass < 30; ++pass) {
                const int len = lens[pass % (int)(sizeof(lens)/sizeof(lens[0]))];
                juce::AudioBuffer<float> view(buf.getArrayOfWritePointers(),
                                              buf.getNumChannels(), len);
                fill(len);
                proc.processBlock(view, mb);
            }
            const size_t after = proc.scratchCapacityBytes();
            r.checkVal(after == before,
                       "spectap: 30 blocks of varying length allocate nothing "
                       "(scratch capacity growth in bytes)",
                       (double)after - (double)before);

            // Non-vacuity: the bin straddling the 400 Hz tone must actually be
            // reporting energy on its Signal Out channel, so the loop above did
            // real work rather than bailing out early.
            fill(maxBlock);
            proc.processBlock(buf, mb);
            double peak = 0;
            for (int i = 0; i < maxBlock; ++i)
                peak = std::max(peak, (double)std::abs(buf.getSample(2 + 1, i)));
            r.checkVal(peak > 1e-3,
                       "spectap: the 400 Hz bin reports energy for a 400 Hz tone",
                       peak);
        }
    }

    // ---- Spectral (FFT) mag/phase curves live-reference FrequencyGraph -------
    {
        // A) SpectralDoc encode/decode carries the two asset ids, coexists with
        //    the optional warp block, and a script without "refs:" decodes to -1.
        {
            SpectralDoc d;
            d.fftSize = 1024;
            d.mag.expression = "exp(-f/9)";
            d.phase.expression = "noise()*pi";
            d.magAssetId = 4242;
            d.phaseAssetId = 777;
            SpectralDoc d2;
            r.check(d2.decode(d.encode()), "spectral: doc with refs decodes");
            r.check(d2.magAssetId == 4242 && d2.phaseAssetId == 777,
                    "spectral: mag/phase asset ids round-trip through encode");

            // Warp + refs together (refs emitted AFTER warp).
            SpectralDoc dw;
            dw.warpChain.push_back(WarpOp{});   // one default op -> non-empty chain
            dw.magAssetId = 5;
            SpectralDoc dw2;
            r.check(dw2.decode(dw.encode()) && dw2.magAssetId == 5 &&
                        dw2.warpChain.size() == 1,
                    "spectral: warp block and refs coexist in encode");

            // Legacy script (no refs field) -> ids default to -1.
            SpectralDoc leg;
            leg.decode("__spectral2__:2048|" + leg.mag.encode() + "|" +
                       leg.phase.encode());
            r.check(leg.magAssetId == -1 && leg.phaseAssetId == -1,
                    "spectral: doc without refs decodes ids as -1");
        }

        // B) Standalone Frequency Domain node: publish, reference w/ stale cache,
        //    resolve, edit propagates, save/load re-resolves, erase detaches.
        {
            NodeGraph g;
            SpectralCurve shared; shared.expression = "exp(-f/4)";
            int aid = g.assets.add(AssetKind::FrequencyGraph, "mag lib", "",
                                   shared.encode());
            int nId = g.addNode("spec", NodeType::Effect, {}, {}).id;
            {
                SpectralDoc d = SpectralDoc::defaultBuiltin();
                d.mag.expression = "1";        // stale cache
                d.magAssetId = aid;
                g.findNode(nId)->script = d.encode();
            }
            int n = resolveSpectralReferences(g);
            r.checkVal(n == 1, "spectral: resolve mirrors the one referenced curve", n);
            {
                SpectralDoc d; d.decode(g.findNode(nId)->script);
                r.check(d.mag.expression == "exp(-f/4)" && d.magAssetId == aid,
                        "spectral: resolved mag curve matches the published asset");
            }
            // Edit the asset -> propagates on next resolve.
            SpectralCurve edited; edited.expression = "exp(-f/2)";
            g.assets.update(aid, "", edited.encode());
            resolveSpectralReferences(g);
            {
                SpectralDoc d; d.decode(g.findNode(nId)->script);
                r.check(d.mag.expression == "exp(-f/2)",
                        "spectral: editing the asset propagates to the node");
            }
            // Save/load preserves + re-resolves.
            std::ostringstream oss;
            ProjectFile::writeProject(oss, g, nullptr, false, true);
            NodeGraph g2; std::istringstream iss(oss.str());
            ProjectFile::readProject(iss, g2, nullptr);
            {
                SpectralDoc d; d.decode(g2.findNode(nId)->script);
                r.check(d.magAssetId == aid && d.mag.expression == "exp(-f/2)",
                        "spectral: node reference re-resolves after save/load");
            }
            // Erase asset -> detach, keep last curve.
            g.assets.erase(aid);
            resolveSpectralReferences(g);
            {
                SpectralDoc d; d.decode(g.findNode(nId)->script);
                r.check(d.magAssetId == -1 && d.mag.expression == "exp(-f/2)",
                        "spectral: erased asset -> node falls back to independent");
            }
        }

        // C) SpectralFrame nested inside a wavetable node also resolves.
        {
            NodeGraph g;
            SpectralCurve shared; shared.expression = "exp(-f/6)";
            int aid = g.assets.add(AssetKind::FrequencyGraph, "frame lib", "",
                                   shared.encode());
            WavetableDoc wt;
            auto sf = std::make_unique<SpectralFrame>();
            sf->doc.mag.expression = "1";    // stale cache
            sf->doc.magAssetId = aid;
            wt.addLibraryEntry(std::move(sf), "spec frame");
            int nId = g.addNode("wt", NodeType::Effect, {}, {}).id;
            g.findNode(nId)->script = wt.encode();

            int n = resolveSpectralReferences(g);
            r.checkVal(n == 1, "spectral: resolve mirrors a wavetable-nested frame", n);

            WavetableDoc back;
            r.check(back.decode(g.findNode(nId)->script),
                    "spectral: wavetable script still decodes after resolve");
            bool found = false;
            for (auto& e : back.library)
                if (auto* s = dynamic_cast<SpectralFrame*>(e.wave.get())) {
                    found = (s->doc.mag.expression == "exp(-f/6)" &&
                             s->doc.magAssetId == aid);
                }
            r.check(found, "spectral: nested frame curve matches the published asset");
        }
    }

    // ---- parametric EQ: variable band count -------------------------------
    {
        // Helper mirroring what the node context menu does: add/remove a band's
        // four params as a group. (The menu itself lives in
        // node_graph_component.cpp; this exercises the same shape.)
        auto addBand = [](Node& n) {
            int nb = ParametricEQProcessor::countBands(n);
            std::string pfx = "B" + std::to_string(nb + 1) + " ";
            n.params.push_back({pfx + "Type", 0.0f, 0.0f, 4.0f});
            n.params.push_back({pfx + "Freq", 1000.0f, 20.0f, 20000.0f});
            n.params.push_back({pfx + "Gain", 0.0f, -24.0f, 24.0f});
            n.params.push_back({pfx + "Q",    0.707f, 0.1f, 10.0f});
        };
        auto removeBand = [](Node& n) {
            int nb = ParametricEQProcessor::countBands(n);
            if (nb <= 0) return;
            std::string pfx = "B" + std::to_string(nb) + " ";
            n.params.erase(
                std::remove_if(n.params.begin(), n.params.end(),
                    [&](const Param& p) {
                        return juce::String(p.name).startsWith(pfx);
                    }),
                n.params.end());
        };

        NodeGraph g;
        int nId = g.addNode("EQ", NodeType::Effect, {}, {}).id;
        g.findNode(nId)->script = "__eq__";

        // Default: a freshly created EQ in the app has 4 bands; here we build it
        // up from empty to verify the count tracks the params exactly.
        r.checkVal(ParametricEQProcessor::countBands(*g.findNode(nId)) == 0,
                   "eq: empty node has zero bands",
                   ParametricEQProcessor::countBands(*g.findNode(nId)));
        for (int i = 0; i < 4; ++i) addBand(*g.findNode(nId));
        r.checkVal(ParametricEQProcessor::countBands(*g.findNode(nId)) == 4,
                   "eq: four bands after four adds",
                   ParametricEQProcessor::countBands(*g.findNode(nId)));

        // Add up to the max and confirm it doesn't overrun.
        while (ParametricEQProcessor::countBands(*g.findNode(nId))
               < ParametricEQProcessor::kMaxBands)
            addBand(*g.findNode(nId));
        r.checkVal(ParametricEQProcessor::countBands(*g.findNode(nId))
                       == ParametricEQProcessor::kMaxBands,
                   "eq: band count saturates at kMaxBands",
                   ParametricEQProcessor::countBands(*g.findNode(nId)));

        // Remove one: count drops, and the removed group is the highest band.
        removeBand(*g.findNode(nId));
        r.checkVal(ParametricEQProcessor::countBands(*g.findNode(nId))
                       == ParametricEQProcessor::kMaxBands - 1,
                   "eq: removing a band drops the count by one",
                   ParametricEQProcessor::countBands(*g.findNode(nId)));
        {
            bool topGone = true;
            std::string gone = "B" + std::to_string(ParametricEQProcessor::kMaxBands)
                               + " ";
            for (auto& p : g.findNode(nId)->params)
                if (juce::String(p.name).startsWith(gone)) topGone = false;
            r.check(topGone, "eq: the removed band's params are gone");
        }

        // Save/load preserves the (non-default) band count.
        int beforeCount = ParametricEQProcessor::countBands(*g.findNode(nId));
        std::ostringstream oss;
        ProjectFile::writeProject(oss, g, nullptr, false, true);
        NodeGraph g2; std::istringstream iss(oss.str());
        ProjectFile::readProject(iss, g2, nullptr);
        r.checkVal(ParametricEQProcessor::countBands(*g2.findNode(nId)) == beforeCount,
                   "eq: band count round-trips through save/load",
                   ParametricEQProcessor::countBands(*g2.findNode(nId)));
    }

    // ---- Curve EQ: script helpers, FrequencyGraph link, zero-latency DSP ----
    {
        // A) CurveEq::encode/decode round-trips the curve and the optional id.
        {
            SpectralCurve c; c.expression = "exp(-f/8)";
            std::string s = CurveEq::encode(c, -1);
            r.check(s.rfind("__curveeq__:", 0) == 0,
                    "curveeq: encode carries the __curveeq__: prefix");
            SpectralCurve c2; int id2 = 999;
            r.check(CurveEq::decode(s, c2, id2) && id2 == -1 &&
                        c2.expression == "exp(-f/8)",
                    "curveeq: decode round-trips curve + independent id (-1)");

            SpectralCurve c3; c3.expression = "1";
            SpectralCurve c4; int id4 = -1;
            r.check(CurveEq::decode(CurveEq::encode(c3, 42), c4, id4) && id4 == 42,
                    "curveeq: decode round-trips a linked asset id");

            SpectralCurve junk; int idj = 7;
            r.check(!CurveEq::decode("__eq__", junk, idj),
                    "curveeq: decode rejects a non-curveeq script");
        }

        // B) FrequencyGraph live reference: publish, resolve/propagate, save/load,
        //    erase->detach. Mirrors the spectral resolver test.
        {
            NodeGraph g;
            SpectralCurve shared; shared.expression = "exp(-f/4)";
            int aid = g.assets.add(AssetKind::FrequencyGraph, "eq lib", "",
                                   shared.encode());
            int nId = g.addNode("ceq", NodeType::Effect, {}, {}).id;
            {
                SpectralCurve stale; stale.expression = "1";  // stale cache
                g.findNode(nId)->script = CurveEq::encode(stale, aid);
            }
            int n = resolveCurveEqReferences(g);
            r.checkVal(n == 1, "curveeq: resolve mirrors the referenced curve", n);
            {
                SpectralCurve c; int id = -1;
                CurveEq::decode(g.findNode(nId)->script, c, id);
                r.check(c.expression == "exp(-f/4)" && id == aid,
                        "curveeq: resolved curve matches the published asset");
            }
            // Edit the asset -> propagates on next resolve.
            SpectralCurve edited; edited.expression = "exp(-f/2)";
            g.assets.update(aid, "", edited.encode());
            resolveCurveEqReferences(g);
            {
                SpectralCurve c; int id = -1;
                CurveEq::decode(g.findNode(nId)->script, c, id);
                r.check(c.expression == "exp(-f/2)",
                        "curveeq: editing the asset propagates to the node");
            }
            // Save/load preserves + re-resolves.
            std::ostringstream oss;
            ProjectFile::writeProject(oss, g, nullptr, false, true);
            NodeGraph g2; std::istringstream iss(oss.str());
            ProjectFile::readProject(iss, g2, nullptr);
            {
                SpectralCurve c; int id = -1;
                CurveEq::decode(g2.findNode(nId)->script, c, id);
                r.check(id == aid && c.expression == "exp(-f/2)",
                        "curveeq: node reference re-resolves after save/load");
            }
            // Erase asset -> detach, keep last curve.
            g.assets.erase(aid);
            resolveCurveEqReferences(g);
            {
                SpectralCurve c; int id = 0;
                CurveEq::decode(g.findNode(nId)->script, c, id);
                r.check(id == -1 && c.expression == "exp(-f/2)",
                        "curveeq: erased asset -> node falls back to independent");
            }
        }

        // B2) Fork isolation (new library model): an INDEPENDENT node (assetId
        //     -1) holding a copy of an asset's content must NOT track later edits
        //     to that asset. This is the "load a copy forks by default" contract -
        //     only a *linked* node propagates. Guards against any accidental
        //     reintroduction of consumer-side write-back / auto-link.
        {
            NodeGraph g;
            SpectralCurve shared; shared.expression = "exp(-f/4)";
            int aid = g.assets.add(AssetKind::FrequencyGraph, "eq lib", "",
                                   shared.encode());
            // Forked node: same content, but independent (id -1).
            int nId = g.addNode("ceq", NodeType::Effect, {}, {}).id;
            {
                SpectralCurve copy; copy.expression = "exp(-f/4)";
                g.findNode(nId)->script = CurveEq::encode(copy, -1);
            }
            // Edit the asset and resolve: the forked node is untouched.
            SpectralCurve edited; edited.expression = "exp(-f/2)";
            g.assets.update(aid, "", edited.encode());
            int n = resolveCurveEqReferences(g);
            {
                SpectralCurve c; int id = 0;
                CurveEq::decode(g.findNode(nId)->script, c, id);
                r.check(id == -1 && c.expression == "exp(-f/4)" && n == 0,
                        "curveeq: forked (independent) node ignores asset edits");
            }
        }

        // C) DSP sanity: unity curve "1" passes audio through (central RMS ~=
        //    input); zero curve "0" silences it. Zero latency, so central
        //    samples line up with the dry signal.
        {
            const int N = 8192;
            const double sr = 44100.0;
            const float freq = 440.0f;
            auto makeSine = [&](juce::AudioBuffer<float>& b) {
                b.setSize(1, N);
                float* d = b.getWritePointer(0);
                for (int i = 0; i < N; ++i)
                    d[i] = 0.5f * std::sin(2.0 * 3.14159265358979 * freq * i / sr);
            };
            auto centralRMS = [&](const juce::AudioBuffer<float>& b) {
                const float* d = b.getReadPointer(0);
                double acc = 0; int cnt = 0;
                for (int i = 2048; i < 6144; ++i) { acc += (double)d[i]*d[i]; ++cnt; }
                return std::sqrt(acc / std::max(1, cnt));
            };

            // Unity.
            {
                NodeGraph g;
                int nId = g.addNode("ceq", NodeType::Effect, {}, {}).id;
                Node& nd = *g.findNode(nId);
                nd.params.push_back({"FFT Size", 11.0f, 8.0f, 12.0f});
                nd.params.push_back({"Mix",       1.0f, 0.0f,  1.0f});
                SpectralCurve c; c.expression = "1";
                nd.script = CurveEq::encode(c, -1);

                CurveEQProcessor proc(nd);
                proc.prepareToPlay(sr, N);
                juce::AudioBuffer<float> buf; makeSine(buf);
                juce::AudioBuffer<float> dry; makeSine(dry);
                juce::MidiBuffer mb;
                proc.processBlock(buf, mb);
                double rWet = centralRMS(buf), rDry = centralRMS(dry);
                r.check(rDry > 1e-3 && std::abs(rWet - rDry) / rDry < 0.1,
                        "curveeq: unity curve preserves central RMS (zero-latency)");
            }
            // Zero (full cut).
            {
                NodeGraph g;
                int nId = g.addNode("ceq", NodeType::Effect, {}, {}).id;
                Node& nd = *g.findNode(nId);
                nd.params.push_back({"FFT Size", 11.0f, 8.0f, 12.0f});
                nd.params.push_back({"Mix",       1.0f, 0.0f,  1.0f});
                SpectralCurve c; c.expression = "0";
                nd.script = CurveEq::encode(c, -1);

                CurveEQProcessor proc(nd);
                proc.prepareToPlay(sr, N);
                juce::AudioBuffer<float> buf; makeSine(buf);
                juce::MidiBuffer mb;
                proc.processBlock(buf, mb);
                r.check(centralRMS(buf) < 1e-3,
                        "curveeq: zero curve silences the central region");
            }

            // Allocation-freedom on the audio thread. Curve EQ used to build a
            // whole FFT (twiddle + bit-reversal tables) and seven std::vectors
            // on EVERY block, and re-evaluate the curve whenever the transform
            // size changed - i.e. a dropout in the middle of an FFT Size drag,
            // which is exactly when it is most audible.
            //
            // Capacity is the observable proxy: if any scratch buffer grew,
            // processBlock reached the allocator. Sweeping FFT Size and Mix and
            // feeding ragged block lengths covers everything a user gesture or
            // a host can vary underneath it.
            {
                NodeGraph g;
                int nId = g.addNode("ceq", NodeType::Effect, {}, {}).id;
                Node& nd = *g.findNode(nId);
                nd.params.push_back({"FFT Size", 11.0f, 8.0f, 12.0f});
                nd.params.push_back({"Mix",       1.0f, 0.0f,  1.0f});
                SpectralCurve c; c.expression = "1 - 0.5 * f";
                nd.script = CurveEq::encode(c, -1);

                const int maxBlock = 2048;
                CurveEQProcessor proc(nd);
                proc.prepareToPlay(sr, maxBlock);

                juce::AudioBuffer<float> buf(2, maxBlock);
                juce::MidiBuffer mb;
                auto fill = [&](int len) {
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < len; ++i)
                            buf.setSample(ch, i, 0.25f * (float)std::sin(
                                6.28318530718 * 440.0 * i / sr));
                };

                fill(maxBlock);
                proc.processBlock(buf, mb);          // warm up, settle capacities
                const size_t before = proc.scratchCapacityBytes();

                const int lens[] = { 2048, 1024, 512, 777, 256, 2048, 333 };
                for (int pass = 0; pass < 40; ++pass) {
                    for (auto& p : nd.params) {
                        if (p.name == "FFT Size") p.value = (float)(8 + (pass % 5));
                        if (p.name == "Mix")      p.value = (float)(pass % 2);
                    }
                    const int len = lens[pass % (int)(sizeof(lens)/sizeof(lens[0]))];
                    juce::AudioBuffer<float> view(buf.getArrayOfWritePointers(), 2, len);
                    fill(len);
                    proc.processBlock(view, mb);
                }
                const size_t after = proc.scratchCapacityBytes();
                r.checkVal(after == before,
                           "curveeq: 40 blocks sweeping FFT Size / Mix / block length "
                           "allocate nothing (scratch capacity growth in bytes)",
                           (double)after - (double)before);

                // The whole point of the sweep is that it actually reached the
                // different transform sizes - otherwise the check above passes
                // vacuously. A block shorter than the selected FFT clamps down,
                // so the smallest length must still produce audio.
                juce::AudioBuffer<float> small(buf.getArrayOfWritePointers(), 2, 256);
                fill(256);
                proc.processBlock(small, mb);
                double acc = 0;
                for (int i = 0; i < 256; ++i) { float s = small.getSample(0, i); acc += s*s; }
                r.check(std::sqrt(acc / 256) > 1e-4,
                        "curveeq: still produces audio at the smallest swept block size");
            }

            // ---- Signal EQ -------------------------------------------------
            // Same sine/RMS rig. The Signal EQ's response is a product of
            // peaking bells (one per point), shared across the Zero-latency
            // (biquad cascade) and FFT-exact engines. We pin: (a) a flat node
            // passes a 440 Hz sine through unchanged in BOTH modes, and (b) a
            // deep narrow dip ON 440 Hz strongly attenuates it in BOTH modes -
            // proving the points map to filter bands and that Mode selects an
            // engine without changing the magnitude target.
            auto makeSignalEqNode = [&](NodeGraph& g, int mode,
                                        std::vector<std::array<float,2>> pts) -> int {
                int nId = g.addNode("seq", NodeType::Effect, {}, {}).id;
                Node& nd = *g.findNode(nId);
                nd.script = "__signaleq__";
                nd.params.push_back({"Mode",     (float)mode, 0.0f,  1.0f});
                nd.params.push_back({"Width",    8.0f,        0.1f, 24.0f});
                nd.params.push_back({"Mix",      1.0f,        0.0f,  1.0f});
                nd.params.push_back({"FFT Size", 11.0f,       8.0f, 12.0f});
                for (size_t i = 0; i < pts.size(); ++i) {
                    std::string pfx = "P" + std::to_string((int)i + 1) + " ";
                    nd.params.push_back({pfx + "Freq", pts[i][0],  20.0f, 20000.0f});
                    nd.params.push_back({pfx + "Gain", pts[i][1], -24.0f,    24.0f});
                }
                return nId;
            };

            for (int mode = 0; mode <= 1; ++mode) {
                const char* mn = (mode == 0 ? "zero-latency" : "FFT");
                // (a) Flat (3 points at 0 dB) preserves the sine.
                {
                    NodeGraph g;
                    int nId = makeSignalEqNode(g, mode,
                        {{200.0f, 0.0f}, {440.0f, 0.0f}, {5000.0f, 0.0f}});
                    SignalEQProcessor proc(*g.findNode(nId));
                    proc.prepareToPlay(sr, N);
                    juce::AudioBuffer<float> buf; makeSine(buf);
                    juce::AudioBuffer<float> dry; makeSine(dry);
                    juce::MidiBuffer mb;
                    proc.processBlock(buf, mb);
                    double rWet = centralRMS(buf), rDry = centralRMS(dry);
                    r.check(rDry > 1e-3 && std::abs(rWet - rDry) / rDry < 0.1,
                            juce::String("signaleq: flat curve preserves RMS (")
                                + mn + ")");
                }
                // (b) Deep narrow dip on 440 Hz attenuates the 440 Hz sine.
                {
                    NodeGraph g;
                    int nId = makeSignalEqNode(g, mode, {{440.0f, -24.0f}});
                    SignalEQProcessor proc(*g.findNode(nId));
                    proc.prepareToPlay(sr, N);
                    juce::AudioBuffer<float> buf; makeSine(buf);
                    juce::AudioBuffer<float> dry; makeSine(dry);
                    juce::MidiBuffer mb;
                    proc.processBlock(buf, mb);
                    double rWet = centralRMS(buf), rDry = centralRMS(dry);
                    r.check(rDry > 1e-3 && rWet < 0.5 * rDry,
                            juce::String("signaleq: -24 dB notch on tone cuts RMS (")
                                + mn + ")");
                }
            }
            // countPoints reflects the contiguous P<n> params.
            {
                NodeGraph g;
                int nId = makeSignalEqNode(g, 0,
                    {{100.0f, 3.0f}, {1000.0f, -6.0f}, {8000.0f, 2.0f}});
                r.checkVal(SignalEQProcessor::countPoints(*g.findNode(nId)) == 3,
                           "signaleq: countPoints counts contiguous points",
                           SignalEQProcessor::countPoints(*g.findNode(nId)));
            }

            // Allocation-freedom on the audio thread (same bug, same proxy, as
            // the Curve EQ check above). Signal EQ is the harsher case: every
            // point coordinate is signal-modulatable, so the per-bin gain table
            // genuinely has to be rebuilt on the audio thread - it can't be
            // precomputed at prepare time the way Curve EQ's can. The sweep
            // therefore also moves the points and changes how many there are,
            // which is what resizes both the band bank and the gain row.
            {
                NodeGraph g;
                int nId = makeSignalEqNode(g, 1,
                    {{200.0f, 3.0f}, {440.0f, -6.0f}, {5000.0f, 2.0f}});
                Node& nd = *g.findNode(nId);

                const int maxBlock = 2048;
                SignalEQProcessor proc(nd);
                proc.prepareToPlay(sr, maxBlock);

                juce::AudioBuffer<float> buf(2, maxBlock);
                juce::MidiBuffer mb;
                auto fill = [&](int len) {
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < len; ++i)
                            buf.setSample(ch, i, 0.25f * (float)std::sin(
                                6.28318530718 * 440.0 * i / sr));
                };
                auto setParam = [&](const char* name, float v) {
                    for (auto& p : nd.params) if (p.name == name) p.value = v;
                };

                fill(maxBlock);
                proc.processBlock(buf, mb);          // warm up, settle capacities
                const size_t before = proc.scratchCapacityBytes();

                const int lens[] = { 2048, 1024, 512, 777, 256, 2048, 333 };
                for (int pass = 0; pass < 48; ++pass) {
                    setParam("Mode",     (float)(pass % 3 == 2 ? 0 : 1));
                    setParam("FFT Size", (float)(8 + (pass % 5)));
                    setParam("Mix",      (float)(pass % 2));
                    setParam("Width",    1.0f + (float)(pass % 12));
                    // Move the points every pass: forces the gain table rebuild.
                    setParam("P1 Freq",  120.0f + 40.0f * (float)(pass % 9));
                    setParam("P2 Gain",  -12.0f + (float)(pass % 7));
                    // Grow the point count up to the hard maximum and back down,
                    // exercising the band-bank resize on the audio thread.
                    const int wantPts = 1 + (pass % SignalEQProcessor::kMaxPoints);
                    while (SignalEQProcessor::countPoints(nd) < wantPts) {
                        std::string pfx = "P" + std::to_string(
                            SignalEQProcessor::countPoints(nd) + 1) + " ";
                        nd.params.push_back({pfx + "Freq", 1000.0f, 20.0f, 20000.0f});
                        nd.params.push_back({pfx + "Gain",    0.0f, -24.0f,  24.0f});
                    }
                    while (SignalEQProcessor::countPoints(nd) > wantPts)
                        nd.params.pop_back();

                    const int len = lens[pass % (int)(sizeof(lens)/sizeof(lens[0]))];
                    juce::AudioBuffer<float> view(buf.getArrayOfWritePointers(), 2, len);
                    fill(len);
                    proc.processBlock(view, mb);
                }
                const size_t after = proc.scratchCapacityBytes();
                r.checkVal(after == before,
                           "signaleq: 48 blocks sweeping Mode / FFT Size / Mix / point "
                           "count / block length allocate nothing (capacity growth, bytes)",
                           (double)after - (double)before);

                // Guard against the check above passing vacuously: the smallest
                // swept block must still come out as audio, in both engines.
                for (int mode = 0; mode <= 1; ++mode) {
                    setParam("Mode", (float)mode);
                    juce::AudioBuffer<float> small(buf.getArrayOfWritePointers(), 2, 256);
                    fill(256);
                    proc.processBlock(small, mb);
                    double acc = 0;
                    for (int i = 0; i < 256; ++i) {
                        float s = small.getSample(0, i); acc += s * s;
                    }
                    r.check(std::sqrt(acc / 256) > 1e-4,
                            juce::String("signaleq: still produces audio at the smallest "
                                         "swept block size (mode ") + juce::String(mode) + ")");
                }
            }

            // ---- SMS (spectral modeling synthesis) -------------------------
            // Splits the signal into deterministic (spectral peaks above
            // Threshold) and stochastic (everything else) halves, each with its
            // own gain. These are the node's first tests.
            auto makeSmsNode = [&](NodeGraph& g, float threshold, float harmGain,
                                   float noiseGain, float fftExp = 10.0f) -> int {
                int nId = g.addNode("sms", NodeType::Effect, {}, {}).id;
                Node& nd = *g.findNode(nId);
                nd.params.push_back({"Threshold",     threshold, 0.0f,  1.0f});
                nd.params.push_back({"Harmonic Gain", harmGain,  0.0f,  4.0f});
                nd.params.push_back({"Noise Gain",    noiseGain, 0.0f,  4.0f});
                nd.params.push_back({"FFT Size",      fftExp,    8.0f, 12.0f});
                nd.params.push_back({"Mix",           1.0f,      0.0f,  1.0f});
                return nId;
            };

            // (a) Threshold 0 puts EVERY bin in the deterministic half, so the
            //     residual is exactly zero and the output must be the input
            //     again at unity gain. This pins the whole round trip - window
            //     -> FFT -> IFFT -> overlap-add -> normalise - and in
            //     particular the normalisation: before it existed, Hann^2 at
            //     50% overlap summed to 0.5*(1+cos^2), so this ratio came out
            //     at 0.77 with a tremolo riding on it.
            {
                NodeGraph g;
                int nId = makeSmsNode(g, 0.0f, 1.0f, 1.0f);
                SMSProcessor proc(*g.findNode(nId));
                proc.prepareToPlay(sr, N);
                juce::AudioBuffer<float> buf; makeSine(buf);
                juce::AudioBuffer<float> dryB; makeSine(dryB);
                juce::MidiBuffer mb;
                proc.processBlock(buf, mb);
                double rWet = centralRMS(buf), rDry = centralRMS(dryB);
                r.checkVal(rDry > 1e-3 && std::abs(rWet / rDry - 1.0) < 0.02,
                           "sms: threshold 0 reproduces the input at unity gain",
                           rDry > 1e-3 ? rWet / rDry : 0.0);
            }

            // (b) The actual claim of the node: keeping only the deterministic
            //     half preserves a pure tone but throws away most of a noise
            //     signal. Both are measured against their own dry level so the
            //     OLA scaling above cancels out.
            {
                auto keepRatio = [&](bool noiseInput) {
                    NodeGraph g;
                    int nId = makeSmsNode(g, 0.85f, 1.0f, 0.0f);
                    SMSProcessor proc(*g.findNode(nId));
                    proc.prepareToPlay(sr, N);
                    juce::AudioBuffer<float> buf(1, N);
                    float* d = buf.getWritePointer(0);
                    std::mt19937 rngLocal(9876);
                    std::uniform_real_distribution<float> uni(-0.5f, 0.5f);
                    for (int i = 0; i < N; ++i)
                        d[i] = noiseInput ? uni(rngLocal)
                                          : 0.5f * (float)std::sin(2.0 * 3.14159265358979
                                                                   * 440.0 * i / sr);
                    juce::AudioBuffer<float> dryB(1, N);
                    dryB.copyFrom(0, 0, buf, 0, 0, N);
                    juce::MidiBuffer mb;
                    proc.processBlock(buf, mb);
                    double rD = centralRMS(dryB);
                    return rD > 1e-6 ? centralRMS(buf) / rD : 0.0;
                };
                const double tone  = keepRatio(false);
                const double noise = keepRatio(true);
                r.checkVal(tone > 3.0 * noise,
                           "sms: harmonic-only keeps far more of a tone than of noise",
                           noise > 1e-9 ? tone / noise : 0.0);
            }

            // (c) Regression: a block length that is not a power of two. The
            //     old code clamped the transform size with `fftSize = n`, which
            //     handed a non-power-of-two size to FFT; its bit-reversal table
            //     is then indexed past its end, writing outside the spectrum
            //     buffer. 480 samples is a perfectly ordinary host block size.
            {
                NodeGraph g;
                int nId = makeSmsNode(g, 0.2f, 1.0f, 1.0f);
                SMSProcessor proc(*g.findNode(nId));
                proc.prepareToPlay(sr, 512);
                juce::AudioBuffer<float> buf(1, 480);
                float* d = buf.getWritePointer(0);
                for (int i = 0; i < 480; ++i)
                    d[i] = 0.5f * (float)std::sin(2.0 * 3.14159265358979 * 440.0 * i / sr);
                juce::MidiBuffer mb;
                proc.processBlock(buf, mb);
                bool finite = true, nonZero = false;
                for (int i = 0; i < 480; ++i) {
                    const float s = buf.getSample(0, i);
                    if (!std::isfinite(s)) finite = false;
                    if (std::abs(s) > 1e-5f) nonZero = true;
                }
                r.check(finite && nonZero,
                        "sms: non-power-of-two block (480) processes cleanly");
            }

            // (d) Allocation-freedom, same capacity-as-proxy check as the two
            //     EQs. SMS built an FFT plus eight vectors PER FRAME, so a
            //     single 2048-sample block at FFT Size 8 hit the allocator ~15
            //     times over.
            {
                NodeGraph g;
                int nId = makeSmsNode(g, 0.3f, 1.0f, 1.0f);
                Node& nd = *g.findNode(nId);

                const int maxBlock = 2048;
                SMSProcessor proc(nd);
                proc.prepareToPlay(sr, maxBlock);

                juce::AudioBuffer<float> buf(2, maxBlock);
                juce::MidiBuffer mb;
                auto fill = [&](int len) {
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < len; ++i)
                            buf.setSample(ch, i, 0.25f * (float)std::sin(
                                6.28318530718 * 440.0 * i / sr));
                };
                auto setParam = [&](const char* name, float v) {
                    for (auto& p : nd.params) if (p.name == name) p.value = v;
                };

                fill(maxBlock);
                proc.processBlock(buf, mb);          // warm up, settle capacities
                const size_t before = proc.scratchCapacityBytes();

                const int lens[] = { 2048, 1024, 512, 480, 777, 256, 2048, 333 };
                for (int pass = 0; pass < 40; ++pass) {
                    setParam("FFT Size",      (float)(8 + (pass % 5)));
                    setParam("Threshold",     (float)(pass % 5) * 0.25f);
                    setParam("Harmonic Gain", (float)(pass % 3));
                    setParam("Noise Gain",    (float)(pass % 4) * 0.5f);
                    setParam("Mix",           (float)(pass % 2));
                    const int len = lens[pass % (int)(sizeof(lens)/sizeof(lens[0]))];
                    juce::AudioBuffer<float> view(buf.getArrayOfWritePointers(), 2, len);
                    fill(len);
                    proc.processBlock(view, mb);
                }
                const size_t after = proc.scratchCapacityBytes();
                r.checkVal(after == before,
                           "sms: 40 blocks sweeping FFT Size / gains / block length "
                           "allocate nothing (capacity growth, bytes)",
                           (double)after - (double)before);

                // Non-vacuity: the smallest swept block must still emit audio.
                setParam("Harmonic Gain", 1.0f);
                setParam("Noise Gain",    1.0f);
                setParam("Mix",           1.0f);
                juce::AudioBuffer<float> small(buf.getArrayOfWritePointers(), 2, 256);
                fill(256);
                proc.processBlock(small, mb);
                double acc = 0;
                for (int i = 0; i < 256; ++i) { float s = small.getSample(0, i); acc += s*s; }
                r.check(std::sqrt(acc / 256) > 1e-4,
                        "sms: still produces audio at the smallest swept block size");
            }
        }
    }

    // ---- plugin-delay-compensation: JUCE graph aligns parallel paths --------
    {
        // SEANCE renders the whole node graph through a juce::AudioProcessorGraph
        // and relies on its built-in delay compensation: a node that reports
        // latency (a hosted VST3, or a future latency-bearing built-in) must stay
        // time-aligned with any parallel dry path it's mixed back against. This
        // pins that behaviour so a JUCE upgrade that dropped it would fail loudly.
        using namespace juce;

        // Emits a unit impulse at sample 0 of the first block, then silence.
        struct ImpulseSource : AudioProcessor {
            ImpulseSource() : AudioProcessor(BusesProperties()
                .withOutput("Out", AudioChannelSet::stereo(), true)) {}
            const String getName() const override { return "ImpulseSrc"; }
            void prepareToPlay(double, int) override { fired = false; }
            void releaseResources() override {}
            void processBlock(AudioBuffer<float>& b, MidiBuffer&) override {
                b.clear();
                if (!fired) {
                    for (int c = 0; c < b.getNumChannels(); ++c) b.setSample(c, 0, 1.0f);
                    fired = true;
                }
            }
            double getTailLengthSeconds() const override { return 0; }
            bool acceptsMidi() const override { return false; }
            bool producesMidi() const override { return false; }
            AudioProcessorEditor* createEditor() override { return nullptr; }
            bool hasEditor() const override { return false; }
            int getNumPrograms() override { return 1; }
            int getCurrentProgram() override { return 0; }
            void setCurrentProgram(int) override {}
            const String getProgramName(int) override { return {}; }
            void changeProgramName(int, const String&) override {}
            void getStateInformation(MemoryBlock&) override {}
            void setStateInformation(const void*, int) override {}
            bool fired = false;
        };
        // Reports `lat` samples of latency and actually delays its input by that
        // much within the (single, large) test block.
        struct DelayProc : AudioProcessor {
            int lat;
            explicit DelayProc(int n) : AudioProcessor(BusesProperties()
                .withInput("In",   AudioChannelSet::stereo(), true)
                .withOutput("Out", AudioChannelSet::stereo(), true)), lat(n) {
                setLatencySamples(n);
            }
            const String getName() const override { return "Delay"; }
            void prepareToPlay(double, int) override {}
            void releaseResources() override {}
            void processBlock(AudioBuffer<float>& b, MidiBuffer&) override {
                const int n = b.getNumSamples();
                for (int c = 0; c < b.getNumChannels(); ++c) {
                    float* d = b.getWritePointer(c);
                    for (int i = n - 1; i >= 0; --i) d[i] = (i >= lat) ? d[i - lat] : 0.0f;
                }
            }
            double getTailLengthSeconds() const override { return 0; }
            bool acceptsMidi() const override { return false; }
            bool producesMidi() const override { return false; }
            AudioProcessorEditor* createEditor() override { return nullptr; }
            bool hasEditor() const override { return false; }
            int getNumPrograms() override { return 1; }
            int getCurrentProgram() override { return 0; }
            void setCurrentProgram(int) override {}
            const String getProgramName(int) override { return {}; }
            void changeProgramName(int, const String&) override {}
            void getStateInformation(MemoryBlock&) override {}
            void setStateInformation(const void*, int) override {}
        };

        const int kLat = 64;
        AudioProcessorGraph g;
        g.setPlayConfigDetails(0, 2, 44100.0, 512);

        auto src = g.addNode(std::make_unique<ImpulseSource>());
        auto dly = g.addNode(std::make_unique<DelayProc>(kLat));
        auto out = g.addNode(std::make_unique<AudioProcessorGraph::AudioGraphIOProcessor>(
            AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

        // Wet path: src -> delay(64) -> out.ch0.   Dry path: src -> out.ch0.
        using NodeAndChannel = AudioProcessorGraph::NodeAndChannel;
        g.addConnection({ NodeAndChannel{ src->nodeID, 0 }, NodeAndChannel{ dly->nodeID, 0 } });
        g.addConnection({ NodeAndChannel{ dly->nodeID, 0 }, NodeAndChannel{ out->nodeID, 0 } });
        g.addConnection({ NodeAndChannel{ src->nodeID, 0 }, NodeAndChannel{ out->nodeID, 0 } });

        g.prepareToPlay(44100.0, 512);

        AudioBuffer<float> buf(2, 512);
        buf.clear();
        MidiBuffer mb;
        g.processBlock(buf, mb);

        const float* o = buf.getReadPointer(0);
        // With PDC the dry branch is delayed by 64 to match the wet branch, so
        // both impulses land together at sample 64 (sum ~2.0) and nothing at 0.
        // Without PDC there would be a 1.0 spike at 0 (dry) and a 1.0 at 64 (wet).
        r.check(std::abs(o[kLat] - 2.0f) < 0.05f && std::abs(o[0]) < 0.05f,
                "pdc: JUCE graph delay-compensates a parallel dry path against a latency node");
        r.checkVal(g.getLatencySamples() == kLat,
                   "pdc: graph reports the max-path latency", g.getLatencySamples());
    }

    // ---- convolution filter reports latency for PDC -------------------------
    {
        // The Convolution Filter has two algorithm paths chosen by IR length:
        //   short IR  (< 1024 samples) -> direct time-domain, ZERO added latency
        //   long IR  (>= 1024 samples) -> partitioned overlap-add FFT, which
        //     buffers a full partition (512) before it can emit output.
        // Only that artificial block-buffering delay is reported via
        // setLatencySamples so the graph's PDC time-aligns the effect against
        // parallel branches. The IR's own group delay is deliberately NOT
        // reported (it's part of the intended filtering sound).
        Node shortNode, longNode;

        // Short IR: a 101-sample lowpass -> direct path.
        auto shortIR = ConvolutionProcessor::generateLowpass(2000.0f, 50, 44100.0);
        shortNode.script = ConvolutionProcessor::encodeIR(shortIR);
        ConvolutionProcessor shortConv(shortNode);
        shortConv.prepareToPlay(44100.0, 512);
        r.checkVal(shortConv.getLatencySamples() == 0,
                   "convolution: short IR (direct path) reports zero latency",
                   shortConv.getLatencySamples());

        // Long IR: 2000 samples of arbitrary content -> overlap-add path.
        std::vector<float> longIR(2000, 0.0f);
        longIR[0] = 1.0f;
        longIR[1500] = 0.5f; // ensure >= 1024 so the FFT path is chosen
        longNode.script = ConvolutionProcessor::encodeIR(longIR);
        ConvolutionProcessor longConv(longNode);
        longConv.prepareToPlay(44100.0, 512);
        r.checkVal(longConv.getLatencySamples() == 512,
                   "convolution: long IR (overlap-add path) reports 512-sample latency",
                   longConv.getLatencySamples());
    }

    // ---- import / merge: dedup by content, id remap, name-clash suffix ------
    {
        // Source library (from "another project"): three assets.
        AssetLibrary src;
        int sShared = src.add(AssetKind::Waveform, "Warm Pad", "layered", "SHARED");
        int sNew    = src.add(AssetKind::Waveform, "Bright",   "layered", "UNIQUE");
        int sClash  = src.add(AssetKind::Instrument, "Bass",   "composite", "SRC_BASS");

        // Destination already has a content-identical "Warm Pad" (different id) and
        // a same-name-different-content "Bass" instrument.
        AssetLibrary dst;
        int dShared = dst.add(AssetKind::Waveform, "Warm Pad (mine)", "layered", "SHARED");
        int dBass   = dst.add(AssetKind::Instrument, "Bass", "composite", "DST_BASS");

        std::vector<AssetEntry> srcAll(src.all().begin(), src.all().end());
        AssetImportResult res = importAssets(dst, srcAll);

        r.checkVal(res.added == 2, "import: two genuinely-new assets added", res.added);
        r.checkVal(res.deduped == 1, "import: one content-identical asset deduped",
                   res.deduped);
        r.checkVal(res.renamed == 1, "import: one name-clash renamed", res.renamed);

        // Dedup: the shared waveform repoints to the EXISTING destination id+name.
        int mappedShared = -1, mappedNew = -1, mappedClash = -1;
        for (auto& pr : res.remap) {
            if (pr.first == sShared) mappedShared = pr.second;
            if (pr.first == sNew)    mappedNew    = pr.second;
            if (pr.first == sClash)  mappedClash  = pr.second;
        }
        r.check(mappedShared == dShared,
                "import: content-identical asset remaps to the existing dest id");
        r.check(dst.find(dShared)->name == "Warm Pad (mine)",
                "import: deduped asset keeps the destination's name (dest wins)");

        // New unique waveform inserted under a fresh id, original name kept.
        r.check(mappedNew >= AssetLibrary::kUserIdBase && dst.find(mappedNew) &&
                    dst.find(mappedNew)->payload == "UNIQUE" &&
                    dst.find(mappedNew)->name == "Bright",
                "import: new asset inserted with fresh id and original name");

        // Name clash: same name, different content -> imported with numeric suffix.
        r.check(mappedClash != dBass && dst.find(mappedClash) &&
                    dst.find(mappedClash)->name == "Bass 2" &&
                    dst.find(mappedClash)->payload == "SRC_BASS",
                "import: same-name different-content gets the lowest free suffix");
        r.check(dst.find(dBass)->name == "Bass",
                "import: the pre-existing same-name asset is untouched");

        // Total = original 2 + 2 inserted (the deduped one added nothing).
        r.checkVal((int) dst.size() == 4, "import: dest grows by exactly the new count",
                   (int) dst.size());

        // Re-importing the same source again is fully idempotent (all dedupe).
        AssetImportResult res2 = importAssets(dst, srcAll);
        r.check(res2.added == 0 && res2.deduped == 3 && (int) dst.size() == 4,
                "import: re-importing identical source is idempotent");

        // Selection: import only one id (closure = itself for leaves).
        AssetLibrary dst2;
        AssetImportResult sel = importAssets(dst2, srcAll, { sNew });
        r.check(sel.added == 1 && (int) dst2.size() == 1 &&
                    dst2.all().front().payload == "UNIQUE",
                "import: selected-id import pulls only that asset");
    }

    // ---- export -> import round-trip through a library-export file ----------
    {
        NodeGraph g;
        g.assets.add(AssetKind::Waveform, "exp wave", "layered", "EXP\nmulti");
        int starred = g.assets.add(AssetKind::MorphAlgorithm, "exp morph", "", "MORPH");
        g.assets.setStarred(starred, true);
        int arch = g.assets.add(AssetKind::AhdsrCurve, "exp env", "", "ENV");
        g.assets.archive(arch);

        juce::File tmp = juce::File::createTempFile("seancelib");
        bool ok = ProjectFile::exportAssets(tmp.getFullPathName().toStdString(), g.assets);
        r.check(ok, "export: exportAssets writes a library file");

        // Import it into a fresh project via the same readProject path.
        AssetLibrary dst;
        std::ifstream in(tmp.getFullPathName().toStdString());
        NodeGraph tmpG;
        ProjectFile::readProject(in, tmpG, nullptr);
        in.close();
        // readProject re-seeds the code-owned built-in morph chains (not part of
        // the export file's content), so count/import only the user assets.
        std::vector<AssetEntry> all;
        for (const auto& e : tmpG.assets.all())
            if (!(e.kind == AssetKind::MorphAlgorithm && isBuiltinMorphAssetId(e.id)))
                all.push_back(e);
        r.checkVal((int) all.size() == 3,
                   "export: all three assets (incl. archived) survive the export file",
                   (int) all.size());
        AssetImportResult res = importAssets(dst, all);
        r.check(res.added == 3, "export: round-trip import adds all three");
        const AssetEntry* m = nullptr;
        for (auto& e : dst.all()) if (e.kind == AssetKind::MorphAlgorithm) m = &e;
        r.check(m && m->starred, "export: starred flag survives export+import");
        bool anyArchived = false;
        for (auto& e : dst.all()) if (e.archived) anyArchived = true;
        r.check(anyArchived, "export: archived flag survives export+import");
        tmp.deleteFile();
    }
}

// ---------------------------------------------------------------------------
// VoiceAllocator: the per-voice polyphony allocation/lifecycle policy behind the
// Voice container (free-slot -> steal-oldest, note-matched note-off, RMS-based
// voice-free). Pure bookkeeping, no audio graph, so it's checkable here. Mirrors
// poly_voice_processor.cpp's use; see poly-voice-architecture.md.
// ---------------------------------------------------------------------------
void testVoiceAllocator(Report& r) {
    r.section("Voice allocator (per-voice polyphony policy)");

    // Helper RMS values: one above the free floor, one below.
    const float floorRms = 1.0e-4f, freeMs = 250.0f;
    const float loud = 0.5f, quiet = 0.0f;

    // --- Basic allocation across distinct free slots ---
    {
        VoiceAllocator a;
        a.resize(4);
        r.check(a.size() == 4 && a.activeCount() == 0, "alloc: 4 empty slots");

        auto r0 = a.noteOn(60);
        auto r1 = a.noteOn(64);
        auto r2 = a.noteOn(67);
        r.check(r0.slot == 0 && r1.slot == 1 && r2.slot == 2,
                "alloc: three notes take the first three free slots in order");
        r.check(!r0.stole && !r1.stole && !r2.stole,
                "alloc: filling free slots never reports a steal");
        r.check(a.activeCount() == 3, "alloc: three voices active");
        r.check(a.slots[0].note == 60 && a.slots[1].note == 64 && a.slots[2].note == 67,
                "alloc: slots remember their note numbers");
    }

    // --- Steal-oldest when full ---
    {
        VoiceAllocator a;
        a.resize(3);
        a.noteOn(60); // age 1, slot 0
        a.noteOn(62); // age 2, slot 1
        a.noteOn(64); // age 3, slot 2
        r.check(a.activeCount() == 3, "steal: all 3 slots busy");
        auto s = a.noteOn(66); // must steal the oldest = slot 0
        r.check(s.slot == 0 && s.stole, "steal: 4th note steals the oldest slot (0)");
        r.check(a.slots[0].note == 66 && a.slots[0].gateHeld,
                "steal: stolen slot now plays the new note and is gate-held");
        r.check(a.activeCount() == 3, "steal: still exactly 3 active after a steal");
        // The new oldest is slot 1 (age 2); steal again should take it.
        auto s2 = a.noteOn(68);
        r.check(s2.slot == 1 && s2.stole, "steal: next steal takes the new oldest (slot 1)");
    }

    // --- note-off releases the gate but keeps the voice sounding (tail) ---
    {
        VoiceAllocator a;
        a.resize(4);
        a.noteOn(60);
        int slot = a.noteOff(60);
        r.check(slot == 0, "noteOff: returns the slot that was playing the note");
        r.check(a.slots[0].active && !a.slots[0].gateHeld,
                "noteOff: voice stays active (release tail) with gate down");
        r.check(a.noteOff(99) == -1, "noteOff: an unheld note returns -1");
        // A released note is no longer matchable by a second note-off.
        r.check(a.noteOff(60) == -1, "noteOff: releasing an already-released note returns -1");
    }

    // --- duplicate notes: note-off releases only ONE (the first) voice ---
    {
        VoiceAllocator a;
        a.resize(4);
        a.noteOn(60); // slot 0
        a.noteOn(60); // slot 1 - same note, second voice
        r.check(a.activeCount() == 2, "dup: same note twice uses two slots");
        int slot = a.noteOff(60);
        r.check(slot == 0, "dup: note-off releases the first held voice");
        r.check(!a.slots[0].gateHeld && a.slots[1].gateHeld,
                "dup: the second voice for that note is still held");
        int slot2 = a.noteOff(60);
        r.check(slot2 == 1, "dup: a second note-off releases the remaining voice");
    }

    // --- voice-free detection: released + silent for >= kFreeMs frees the slot ---
    {
        VoiceAllocator a;
        a.resize(2);
        a.noteOn(60); // slot 0
        a.noteOff(60);
        // 100 ms blocks of silence: needs >= 250 ms to free => frees on the 3rd.
        r.check(!a.postRender(0, quiet, 100.0f, floorRms, freeMs),
                "free: 100ms silence - not yet freed");
        r.check(!a.postRender(0, quiet, 100.0f, floorRms, freeMs),
                "free: 200ms silence - not yet freed");
        r.check(a.slots[0].active, "free: still active at 200ms");
        r.check(a.postRender(0, quiet, 100.0f, floorRms, freeMs),
                "free: 300ms silence crosses kFreeMs -> freed");
        r.check(!a.slots[0].active, "free: slot is now inactive and reusable");
        // Reusing the freed slot is a fresh alloc, not a steal.
        auto re = a.noteOn(72);
        r.check(re.slot == 0 && !re.stole, "free: freed slot is reused without a steal");
    }

    // --- a still-loud released voice does NOT free, and resets its timer ---
    {
        VoiceAllocator a;
        a.resize(1);
        a.noteOn(60);
        a.noteOff(60);
        a.postRender(0, quiet, 100.0f, floorRms, freeMs); // 100ms of silence banked
        // A loud block resets the silence timer.
        r.check(!a.postRender(0, loud, 100.0f, floorRms, freeMs),
                "free: a loud block does not free the voice");
        r.check(a.slots[0].silenceMs == 0.0f, "free: a loud block resets the silence timer");
        // Now it takes a fresh full kFreeMs of silence to free.
        a.postRender(0, quiet, 100.0f, floorRms, freeMs);
        a.postRender(0, quiet, 100.0f, floorRms, freeMs);
        r.check(a.slots[0].active, "free: timer truly restarted (still active at 200ms)");
        r.check(a.postRender(0, quiet, 100.0f, floorRms, freeMs),
                "free: frees after a fresh 300ms of silence");
    }

    // --- a held (gate-down) voice never frees, however quiet ---
    {
        VoiceAllocator a;
        a.resize(1);
        a.noteOn(60); // still held
        for (int i = 0; i < 10; ++i)
            r.check(!a.postRender(0, quiet, 100.0f, floorRms, freeMs),
                    "held: a gate-held voice never frees even when silent");
        r.check(a.slots[0].active && a.slots[0].silenceMs == 0.0f,
                "held: held voice keeps its silence timer pinned at zero");
    }

    // --- allNotesOff drops every gate but leaves voices in their tails ---
    {
        VoiceAllocator a;
        a.resize(3);
        a.noteOn(60); a.noteOn(62); a.noteOn(64);
        a.allNotesOff();
        bool anyHeld = false;
        for (auto& s : a.slots) if (s.gateHeld) anyHeld = true;
        r.check(!anyHeld, "panic: allNotesOff clears every gate");
        r.check(a.activeCount() == 3, "panic: voices remain active for their release tails");
    }

    // --- steal-quietest: when full, the new note steals the quietest slot ---
    {
        VoiceAllocator a;
        a.resize(3);
        a.stealMode = VoiceAllocator::StealQuietest;
        a.noteOn(60); a.noteOn(62); a.noteOn(64); // slots 0,1,2 all held & active
        // Give each slot a distinct last-block level; slot 1 is quietest.
        a.postRender(0, 0.40f, 10.0f, floorRms, freeMs);
        a.postRender(1, 0.05f, 10.0f, floorRms, freeMs);
        a.postRender(2, 0.30f, 10.0f, floorRms, freeMs);
        auto s = a.noteOn(66);
        r.check(s.slot == 1 && s.stole, "steal-quietest: steals the lowest-RMS slot (1)");
        r.check(a.slots[1].note == 66 && a.slots[1].gateHeld,
                "steal-quietest: stolen slot now plays the new note");
        // After the steal, refresh levels: slot 0 now quietest.
        a.postRender(0, 0.02f, 10.0f, floorRms, freeMs);
        a.postRender(1, 0.50f, 10.0f, floorRms, freeMs);
        a.postRender(2, 0.30f, 10.0f, floorRms, freeMs);
        auto s2 = a.noteOn(68);
        r.check(s2.slot == 0 && s2.stole, "steal-quietest: next steal takes the new quietest (0)");
    }

    // --- steal-quietest still prefers a free slot over stealing ---
    {
        VoiceAllocator a;
        a.resize(3);
        a.stealMode = VoiceAllocator::StealQuietest;
        a.noteOn(60); // slot 0
        a.postRender(0, 0.001f, 10.0f, floorRms, freeMs); // very quiet, but still free slots
        auto s = a.noteOn(62);
        r.check(s.slot == 1 && !s.stole,
                "steal-quietest: a free slot wins over stealing a quiet active one");
    }

    // --- steal-round-robin: cycles slots predictably once full ---
    {
        VoiceAllocator a;
        a.resize(3);
        a.stealMode = VoiceAllocator::StealRoundRobin;
        // Fill the free slots first (round-robin still prefers free).
        r.check(a.noteOn(60).slot == 0 && a.noteOn(62).slot == 1 && a.noteOn(64).slot == 2,
                "round-robin: free slots fill in order first");
        // Now full: steals advance the cursor 0,1,2,0,...
        auto a0 = a.noteOn(66);
        auto a1 = a.noteOn(67);
        auto a2 = a.noteOn(68);
        auto a3 = a.noteOn(69);
        r.check(a0.slot == 0 && a1.slot == 1 && a2.slot == 2 && a3.slot == 0,
                "round-robin: steals cycle 0,1,2,0 regardless of age/level");
        r.check(a0.stole && a1.stole && a2.stole && a3.stole,
                "round-robin: each wrap-around allocation reports a steal");
    }

    // --- round-robin cursor resets on resize ---
    {
        VoiceAllocator a;
        a.resize(2);
        a.stealMode = VoiceAllocator::StealRoundRobin;
        a.noteOn(60); a.noteOn(62);     // fill
        a.noteOn(64);                   // steal slot 0 (cursor -> 0)
        a.resize(2);                    // cursor reset to -1
        a.stealMode = VoiceAllocator::StealRoundRobin;
        a.noteOn(60); a.noteOn(62);     // fill again
        auto s = a.noteOn(64);          // first steal after reset -> slot 0
        r.check(s.slot == 0, "round-robin: resize() resets the cursor to start at slot 0");
    }

    // --- default stealMode is oldest (unset == 0) ---
    {
        VoiceAllocator a;
        a.resize(2);
        r.check(a.stealMode == VoiceAllocator::StealOldest,
                "default: a fresh allocator steals oldest");
        a.noteOn(60); a.noteOn(62);
        auto s = a.noteOn(64);
        r.check(s.slot == 0 && s.stole, "default: steals slot 0 (oldest) by default");
    }

    // --- degenerate: zero slots is safe ---
    {
        VoiceAllocator a;
        a.resize(0);
        auto s = a.noteOn(60);
        r.check(s.slot == -1 && !s.stole, "edge: note-on with no slots returns -1");
        r.check(a.noteOff(60) == -1, "edge: note-off with no slots returns -1");
        r.check(!a.postRender(0, quiet, 100.0f, floorRms, freeMs),
                "edge: postRender on an out-of-range slot is a safe no-op");
        // round-robin with no slots must not divide by zero.
        a.stealMode = VoiceAllocator::StealRoundRobin;
        r.check(a.noteOn(60).slot == -1, "edge: round-robin with no slots is safe");
    }
}

// ---------------------------------------------------------------------------
// VoiceIn signal emission: drive a VoiceInProcessor directly and inspect the
// Pitch/Gate/Velocity control channels (2/3/4) sample-by-sample to prove the
// note-on/off edges land on their EXACT within-block offset (M2: sample-accurate
// gates), that pitch/velocity step with the gate, and that a release holds pitch
// while only the gate falls. Also covers carry across blocks, multiple segments
// in one block, and reset().
// ---------------------------------------------------------------------------
void testVoiceInSignals(Report& r) {
    r.section("VoiceIn signals (sample-accurate gates)");

    NodeGraph graph;
    auto& node = graph.addNode("Voice In", NodeType::VoiceIn, {},
        {Pin{0, "MIDI",     PinKind::Midi,   false},
         Pin{0, "Pitch",    PinKind::Signal, false, 1},
         Pin{0, "Gate",     PinKind::Signal, false, 1},
         Pin{0, "Velocity", PinKind::Signal, false, 1}}, {0.0f, 0.0f});

    VoiceInProcessor vip(node);
    const int N = 512;
    vip.prepareToPlay(48000.0, N);

    // Channels: 0/1 audio (unused here), 2 = Pitch, 3 = Gate, 4 = Velocity.
    juce::AudioBuffer<float> buf(5, N);
    const float a5 = VoiceInProcessor::midiToHz(81); // 880 Hz

    // --- Block 1: note-on at offset 100 (A5, vel 0.8). ---
    {
        buf.clear();
        juce::MidiBuffer midi;
        vip.noteOn(100, 81, 0.8f);
        vip.processBlock(buf, midi);

        r.check(buf.getSample(3, 99) == 0.0f, "gate: low at sample 99 (before offset)");
        r.check(buf.getSample(3, 100) == 1.0f, "gate: high exactly at offset 100");
        r.check(buf.getSample(3, N - 1) == 1.0f, "gate: stays high to end of block");

        r.check(std::abs(buf.getSample(2, 99) - 440.0f) < 0.5f,
                "pitch: carried default (440) before the note-on");
        r.check(std::abs(buf.getSample(2, 100) - a5) < 0.5f,
                "pitch: steps to 880 exactly at offset 100");

        r.check(buf.getSample(4, 99) == 0.0f, "velocity: 0 before the note-on");
        r.check(std::abs(buf.getSample(4, 100) - 0.8f) < 1e-4f,
                "velocity: latched to 0.8 at offset 100");

        // MIDI note-on must be emitted at the same offset.
        bool foundOn = false;
        for (const auto m : midi)
            if (m.getMessage().isNoteOn() && m.samplePosition == 100) foundOn = true;
        r.check(foundOn, "midi: note-on emitted at sample 100");
    }

    // --- Block 2: note-off at offset 200 - gate falls, pitch/velocity hold. ---
    {
        buf.clear();
        juce::MidiBuffer midi;
        vip.noteOff(200, 81);
        vip.processBlock(buf, midi);

        r.check(buf.getSample(3, 199) == 1.0f, "gate: still high at 199 (carried from block 1)");
        r.check(buf.getSample(3, 200) == 0.0f, "gate: falls exactly at offset 200");
        r.check(std::abs(buf.getSample(2, 0) - a5) < 0.5f,
                "pitch: held at 880 through the release (block start)");
        r.check(std::abs(buf.getSample(2, N - 1) - a5) < 0.5f,
                "pitch: held at 880 through the release (block end)");
        r.check(std::abs(buf.getSample(4, N - 1) - 0.8f) < 1e-4f,
                "velocity: latched value held through release");
    }

    // --- Block 3: reset() drops the gate at the block start. ---
    {
        buf.clear();
        juce::MidiBuffer midi;
        vip.reset();
        vip.processBlock(buf, midi);
        r.check(buf.getSample(3, 0) == 0.0f && buf.getSample(3, N - 1) == 0.0f,
                "reset: gate low across the whole block");
        bool foundAllOff = false;
        for (const auto m : midi)
            if (m.getMessage().isAllNotesOff()) foundAllOff = true;
        r.check(foundAllOff, "reset: emits all-notes-off");
    }

    // --- Block 4: multiple segments in ONE block (on @50, off @150, on @300). ---
    {
        buf.clear();
        juce::MidiBuffer midi;
        const float c4 = VoiceInProcessor::midiToHz(60); // 261.63 Hz
        const float e4 = VoiceInProcessor::midiToHz(64); // 329.63 Hz
        vip.noteOn(50, 60, 1.0f);
        vip.noteOff(150, 60);
        vip.noteOn(300, 64, 0.5f);
        vip.processBlock(buf, midi);

        r.check(buf.getSample(3, 49)  == 0.0f, "multi: gate low before first on (49)");
        r.check(buf.getSample(3, 50)  == 1.0f, "multi: gate high at first on (50)");
        r.check(buf.getSample(3, 149) == 1.0f, "multi: gate high before off (149)");
        r.check(buf.getSample(3, 150) == 0.0f, "multi: gate low at off (150)");
        r.check(buf.getSample(3, 299) == 0.0f, "multi: gate low before second on (299)");
        r.check(buf.getSample(3, 300) == 1.0f, "multi: gate high at second on (300)");
        r.check(std::abs(buf.getSample(2, 60)  - c4) < 0.5f, "multi: pitch C4 during first note");
        r.check(std::abs(buf.getSample(2, 320) - e4) < 0.5f, "multi: pitch E4 during second note");
    }

    const float c4 = VoiceInProcessor::midiToHz(60); // 261.63 Hz
    const float c5 = VoiceInProcessor::midiToHz(72); // 523.25 Hz

    // --- Block 5: glide (portamento) C4 -> C5 over 10 ms (480 < 512 samples). ---
    {
        // Establish a current pitch with a plain (no-glide) note-on first.
        buf.clear();
        juce::MidiBuffer m;
        vip.noteOn(0, 60, 1.0f);
        vip.processBlock(buf, m);
        r.check(std::abs(buf.getSample(2, 0) - c4) < 0.5f, "glide: plain note-on sets C4 instantly");

        buf.clear();
        juce::MidiBuffer m2;
        vip.noteOn(0, 72, 1.0f, 10.0f); // 10 ms @ 48 kHz = 480 samples
        vip.processBlock(buf, m2);
        r.check(buf.getSample(2, 0) > c4 && buf.getSample(2, 0) < c5 - 1.0f,
                "glide: pitch starts sliding up from C4 (not an instant jump)");
        r.check(buf.getSample(2, 100) < buf.getSample(2, 400),
                "glide: pitch rises monotonically through the slide");
        r.check(std::abs(buf.getSample(2, 479) - c5) < 1.0f,
                "glide: reaches the target C5 at the end of the ramp");
        r.check(std::abs(buf.getSample(2, 511) - c5) < 1.0f,
                "glide: holds the target after the ramp completes");
        // Gate still steps instantly even while pitch glides.
        r.check(buf.getSample(3, 0) == 1.0f, "glide: gate retriggers instantly (not ramped)");
    }

    // --- Block 6: glide spanning more than one block (20 ms = 960 > 512). ---
    {
        buf.clear();
        juce::MidiBuffer m;
        vip.noteOn(0, 60, 1.0f, 20.0f); // C5 -> C4 over 960 samples
        vip.processBlock(buf, m);
        const float endB1 = buf.getSample(2, 511);
        r.check(endB1 < c5 - 1.0f && endB1 > c4,
                "glide(cross-block): still mid-slide at the end of the first block");

        buf.clear();
        juce::MidiBuffer m2;
        vip.processBlock(buf, m2); // no new events; ramp must continue on its own
        r.check(buf.getSample(2, 0) < endB1,
                "glide(cross-block): keeps sliding into the second block");
        r.check(std::abs(buf.getSample(2, 511) - c4) < 1.0f,
                "glide(cross-block): reaches the target by the end of the second block");
    }
}

// ---------------------------------------------------------------------------
// MPE per-note expression. Two halves:
//  (1) Unit: drive a VoiceInProcessor's expression setters and read the
//      Pressure (ch5) / Timbre (ch6) signals + the bent Pitch (ch2) directly -
//      neutral defaults, smoothing toward target, pitch-bend folding, and the
//      reset-to-neutral on a fresh note-on.
//  (2) End-to-end routing: a real PolyVoiceProcessor + Signal Osc patch, proving
//      a channel pitch-bend reaches the right voice (member channel targets one
//      voice; master channel 1 broadcasts) measured by output zero-crossings.
//  (3) Migration: an old 4-output VoiceIn gains Pressure/Timbre pins on load.
// ---------------------------------------------------------------------------
void testVoiceMpe(Report& r) {
    r.section("MPE per-note expression (Pressure/Timbre/bend)");

    // ---- (1) VoiceInProcessor expression signals -------------------------
    {
        NodeGraph graph;
        auto& node = graph.addNode("Voice In", NodeType::VoiceIn, {},
            {Pin{0, "MIDI",     PinKind::Midi,   false},
             Pin{0, "Pitch",    PinKind::Signal, false, 1},
             Pin{0, "Gate",     PinKind::Signal, false, 1},
             Pin{0, "Velocity", PinKind::Signal, false, 1},
             Pin{0, "Pressure", PinKind::Signal, false, 1},
             Pin{0, "Timbre",   PinKind::Signal, false, 1}}, {0.0f, 0.0f});

        VoiceInProcessor vip(node);
        const int N = 512;
        vip.prepareToPlay(48000.0, N);
        juce::AudioBuffer<float> buf(7, N); // 0/1 audio, 2 pitch,3 gate,4 vel,5 pres,6 timbre

        const float a4 = VoiceInProcessor::midiToHz(69); // 440 Hz

        // Note-on: expression at neutral rest (pressure 0, timbre 0.5, no bend).
        auto run = [&](){ buf.clear(); juce::MidiBuffer mb; vip.processBlock(buf, mb); };
        vip.noteOn(0, 69, 1.0f);
        run();
        r.check(std::abs(buf.getSample(5, N - 1) - 0.0f) < 1e-4f, "expr: pressure rests at 0");
        r.check(std::abs(buf.getSample(6, N - 1) - 0.5f) < 1e-4f, "expr: timbre rests at 0.5 (centre)");
        r.check(std::abs(buf.getSample(2, N - 1) - a4) < 0.5f, "expr: pitch is the un-bent note at rest");

        // Pressure ramps toward its target (smoothed, not instant).
        vip.setPressure(1.0f);
        run();
        r.check(buf.getSample(5, 0) < 0.5f, "expr: pressure starts ramping (not an instant jump)");
        for (int b = 0; b < 6; ++b) run();
        r.check(buf.getSample(5, N - 1) > 0.95f, "expr: pressure settles near its target 1.0");

        // Timbre ramps down toward 0.
        vip.setTimbre(0.0f);
        for (int b = 0; b < 8; ++b) run();
        r.check(buf.getSample(6, N - 1) < 0.05f, "expr: timbre settles near its target 0.0");

        // Pitch bend +12 semitones folds into Pitch -> ~2x frequency once settled.
        vip.setPitchBend(12.0f);
        for (int b = 0; b < 10; ++b) run();
        r.check(std::abs(buf.getSample(2, N - 1) - a4 * 2.0f) < 4.0f,
                "expr: +12 semitone bend doubles the Pitch signal");

        // A fresh note-on snaps EVERY expression dimension back to neutral so the
        // prior note's bend/pressure/timbre cannot bleed into the reused voice.
        vip.noteOn(0, 69, 1.0f);
        run();
        r.check(std::abs(buf.getSample(2, 0) - a4) < 0.5f, "expr: note-on resets bend (pitch back to A4)");
        r.check(std::abs(buf.getSample(5, 0) - 0.0f) < 1e-3f, "expr: note-on resets pressure to 0");
        r.check(std::abs(buf.getSample(6, 0) - 0.5f) < 1e-3f, "expr: note-on resets timbre to 0.5");
    }

    // ---- (2) End-to-end routing through PolyVoiceProcessor ----------------
    {
        NodeGraph graph;
        Transport transport;
        int containerId;
        {
            auto& c = graph.addNode("Voice", NodeType::VoiceContainer,
                {Pin{0, "MIDI", PinKind::Midi, true}},
                {Pin{0, "Audio", PinKind::Audio, false}}, {0.0f, 0.0f});
            c.voicePolyphony = 4;
            containerId = c.id;
        }
        int viPitch, viGate, viVel;
        {
            auto& vi = graph.addNode("Voice In", NodeType::VoiceIn, {},
                {Pin{0, "MIDI",     PinKind::Midi,   false},
                 Pin{0, "Pitch",    PinKind::Signal, false, 1},
                 Pin{0, "Gate",     PinKind::Signal, false, 1},
                 Pin{0, "Velocity", PinKind::Signal, false, 1},
                 Pin{0, "Pressure", PinKind::Signal, false, 1},
                 Pin{0, "Timbre",   PinKind::Signal, false, 1}}, {-200.0f, 0.0f});
            vi.voiceContainerId = containerId;
            viPitch = vi.pinsOut[1].id;
            viGate  = vi.pinsOut[2].id;
            viVel   = vi.pinsOut[3].id;
        }
        int oscPitch, oscGate, oscVel, oscAudio;
        {
            auto& s = graph.addNode("Signal Osc", NodeType::Instrument,
                {Pin{0, "Pitch",    PinKind::Signal, true, 1},
                 Pin{0, "Gate",     PinKind::Signal, true, 1},
                 Pin{0, "Velocity", PinKind::Signal, true, 1}},
                {Pin{0, "Audio", PinKind::Audio, false}}, {0.0f, 0.0f});
            s.voiceContainerId = containerId;
            s.script = "__signalosc__";
            s.params.push_back({"Waveform", 0.0f, 0.0f, 3.0f}); // sine
            s.params.push_back({"Volume",   0.5f, 0.0f, 1.0f});
            s.ahdsrEnvelope.attackMs  = 2.0f;
            s.ahdsrEnvelope.decayMs   = 10.0f;
            s.ahdsrEnvelope.sustain   = 1.0f; // steady tone for crossing measurement
            s.ahdsrEnvelope.releaseMs = 40.0f;
            oscPitch = s.pinsIn[0].id;
            oscGate  = s.pinsIn[1].id;
            oscVel   = s.pinsIn[2].id;
            oscAudio = s.pinsOut[0].id;
        }
        int voAudio;
        {
            auto& vo = graph.addNode("Voice Out", NodeType::VoiceOut,
                {Pin{0, "Audio", PinKind::Audio, true}}, {}, {200.0f, 0.0f});
            vo.voiceContainerId = containerId;
            voAudio = vo.pinsIn[0].id;
        }
        graph.addLink(viPitch, oscPitch);
        graph.addLink(viGate,  oscGate);
        graph.addLink(viVel,   oscVel);
        graph.addLink(oscAudio, voAudio);

        Node* container = graph.findNode(containerId);
        if (!container) { r.check(false, "mpe-route: container exists"); return; }

        const double sr = 44100.0;
        const int bs = 512;
        PolyVoiceProcessor poly(*container, graph, transport);
        poly.setPlayConfigDetails(0, 2, sr, bs);
        poly.prepareToPlay(sr, bs);

        auto zc = [](juce::AudioBuffer<float>& b) {
            int c = 0; const float* d = b.getReadPointer(0);
            for (int i = 1; i < b.getNumSamples(); ++i)
                if ((d[i - 1] <= 0.0f) != (d[i] <= 0.0f)) ++c;
            return c;
        };
        // Settle: render `extra` blocks of empty MIDI, return last-block crossings.
        auto settleZc = [&](int extra) {
            juce::AudioBuffer<float> out(2, bs);
            juce::MidiBuffer empty;
            for (int i = 0; i < extra; ++i) poly.processBlock(out, empty);
            return zc(out);
        };

        // Note 69 (A4, 220-ish crossings/block) on MEMBER channel 2.
        {
            juce::AudioBuffer<float> out(2, bs);
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::noteOn(2, 69, (juce::uint8) 110), 0);
            poly.processBlock(out, on);
        }
        const int baseZc = settleZc(12);
        r.check(baseZc > 0, "mpe-route: voice produces a tone (nonzero crossings)");

        // +12 semitones on channel 2 (48-semi member range -> wheel 10240).
        {
            juce::AudioBuffer<float> out(2, bs);
            juce::MidiBuffer bend;
            bend.addEvent(juce::MidiMessage::pitchWheel(2, 10240), 0);
            poly.processBlock(out, bend);
        }
        const int bentZc = settleZc(12);
        r.check(bentZc > baseZc * 16 / 10,
                "mpe-route: member-channel bend raises this voice's pitch (~2x)");

        // A bend on an UNRELATED member channel (3) must NOT move the channel-2
        // voice. Reset channel 2 to neutral first, then bend channel 3.
        {
            juce::AudioBuffer<float> out(2, bs);
            juce::MidiBuffer reset; reset.addEvent(juce::MidiMessage::pitchWheel(2, 8192), 0);
            poly.processBlock(out, reset);
        }
        settleZc(12);
        {
            juce::AudioBuffer<float> out(2, bs);
            juce::MidiBuffer other; other.addEvent(juce::MidiMessage::pitchWheel(3, 10240), 0);
            poly.processBlock(out, other);
        }
        const int isolatedZc = settleZc(12);
        r.check(std::abs(isolatedZc - baseZc) < baseZc / 5,
                "mpe-route: a bend on a different channel leaves this voice alone");

        // Master-channel (1) bend broadcasts to all voices (incl. the ch-2 note),
        // using the conventional +/-2 semitone range. +2 semis -> ~1.12x.
        {
            juce::AudioBuffer<float> out(2, bs);
            juce::MidiBuffer master; master.addEvent(juce::MidiMessage::pitchWheel(1, 16383), 0);
            poly.processBlock(out, master);
        }
        const int masterZc = settleZc(12);
        r.check(masterZc > baseZc + baseZc / 20,
                "mpe-route: master-channel bend broadcasts to the voice (+~2 semis)");
    }

    // ---- (3) Load migration: old 4-output VoiceIn gains Pressure/Timbre ----
    {
        NodeGraph graph;
        graph.addNode("Voice In", NodeType::VoiceIn, {},
            {Pin{0, "MIDI",     PinKind::Midi,   false},
             Pin{0, "Pitch",    PinKind::Signal, false, 1},
             Pin{0, "Gate",     PinKind::Signal, false, 1},
             Pin{0, "Velocity", PinKind::Signal, false, 1}}, {0.0f, 0.0f});

        const std::string text = ProjectFile::serializeForUndo(graph);

        NodeGraph loaded;
        const bool ok = ProjectFile::loadFromString(text, loaded);
        r.check(ok, "mpe-migrate: old VoiceIn project loads");
        Node* vi = nullptr;
        for (auto& n : loaded.nodes) if (n.type == NodeType::VoiceIn) vi = &n;
        r.check(vi != nullptr, "mpe-migrate: VoiceIn present after load");
        if (vi) {
            auto hasPin = [&](const std::string& nm) {
                for (auto& p : vi->pinsOut) if (p.name == nm) return true;
                return false;
            };
            r.check(hasPin("Pressure"), "mpe-migrate: Pressure output pin added on load");
            r.check(hasPin("Timbre"),   "mpe-migrate: Timbre output pin added on load");
        }
    }
}

// ---------------------------------------------------------------------------
// Unison: a struck note allocates a STACK of detuned/panned voices that release
// together. Three parts: the VoiceAllocator group API (allocate a stack, release
// the whole stack, clamp to polyphony, steal a whole stack), end-to-end audio
// (a spread unison decorrelates L/R; the note frees to silence), and the
// container field save/load round-trip.
// ---------------------------------------------------------------------------
void testVoiceUnison(Report& r) {
    r.section("Unison (stacked detuned voices per note)");

    // ---- (1) VoiceAllocator group allocation ------------------------------
    {
        VoiceAllocator a;
        a.resize(8);
        auto g = a.noteOnGroup(60, 1, 3);
        r.check(g.count == 3, "uni-alloc: a 3-voice unison grabs 3 slots");
        r.check(a.activeCount() == 3, "uni-alloc: 3 voices now active");
        // All three share one group id and the note/channel.
        long long grp = a.slots[(size_t) g.slot[0]].group;
        bool sameGroup = true, sameNote = true;
        for (int k = 0; k < g.count; ++k) {
            if (a.slots[(size_t) g.slot[k]].group != grp) sameGroup = false;
            if (a.slots[(size_t) g.slot[k]].note != 60)   sameNote = false;
        }
        r.check(sameGroup, "uni-alloc: every slot of the stack shares one group id");
        r.check(sameNote,  "uni-alloc: every slot of the stack plays the note");

        auto rel = a.noteOffGroup(60, 1);
        r.check(rel.count == 3, "uni-off: note-off releases the whole 3-voice stack");
        bool anyHeld = false;
        for (auto& s : a.slots) if (s.gateHeld) anyHeld = true;
        r.check(!anyHeld, "uni-off: no gate is left held after the stack release");
        r.check(a.activeCount() == 3, "uni-off: voices stay active for their tails");
    }

    // Clamp: a unison larger than the polyphony can't exceed the slot count.
    {
        VoiceAllocator a;
        a.resize(2);
        auto g = a.noteOnGroup(60, 1, 4);
        r.check(g.count == 2, "uni-clamp: unison clamps to the available slot count");
    }

    // Channel-matched release: two notes (same number) on different MPE channels
    // are independent stacks; releasing one channel leaves the other held.
    {
        VoiceAllocator a;
        a.resize(8);
        a.noteOnGroup(60, 2, 2); // ch 2 stack
        a.noteOnGroup(60, 3, 2); // ch 3 stack, same note number
        auto rel = a.noteOffGroup(60, 2);
        r.check(rel.count == 2, "uni-chan: note-off on ch2 releases only the ch2 stack");
        int held = 0;
        for (auto& s : a.slots) if (s.gateHeld) ++held;
        r.check(held == 2, "uni-chan: the ch3 stack stays held");
    }

    // ---- (2) End-to-end: spread unison decorrelates the stereo field -------
    {
        NodeGraph graph;
        Transport transport;
        int containerId;
        {
            auto& c = graph.addNode("Voice", NodeType::VoiceContainer,
                {Pin{0, "MIDI", PinKind::Midi, true}},
                {Pin{0, "Audio", PinKind::Audio, false}}, {0.0f, 0.0f});
            c.voicePolyphony = 8;
            c.voiceUnison = 4;
            c.voiceUnisonDetune = 20.0f;
            c.voiceUnisonSpread = 1.0f; // full stereo spread
            containerId = c.id;
        }
        int viPitch, viGate, viVel;
        {
            auto& vi = graph.addNode("Voice In", NodeType::VoiceIn, {},
                {Pin{0, "MIDI",     PinKind::Midi,   false},
                 Pin{0, "Pitch",    PinKind::Signal, false, 1},
                 Pin{0, "Gate",     PinKind::Signal, false, 1},
                 Pin{0, "Velocity", PinKind::Signal, false, 1},
                 Pin{0, "Pressure", PinKind::Signal, false, 1},
                 Pin{0, "Timbre",   PinKind::Signal, false, 1}}, {-200.0f, 0.0f});
            vi.voiceContainerId = containerId;
            viPitch = vi.pinsOut[1].id;
            viGate  = vi.pinsOut[2].id;
            viVel   = vi.pinsOut[3].id;
        }
        int oscPitch, oscGate, oscVel, oscAudio;
        {
            auto& s = graph.addNode("Signal Osc", NodeType::Instrument,
                {Pin{0, "Pitch",    PinKind::Signal, true, 1},
                 Pin{0, "Gate",     PinKind::Signal, true, 1},
                 Pin{0, "Velocity", PinKind::Signal, true, 1}},
                {Pin{0, "Audio", PinKind::Audio, false}}, {0.0f, 0.0f});
            s.voiceContainerId = containerId;
            s.script = "__signalosc__";
            s.params.push_back({"Waveform", 0.0f, 0.0f, 3.0f});
            s.params.push_back({"Volume",   0.5f, 0.0f, 1.0f});
            s.ahdsrEnvelope.attackMs  = 2.0f;
            s.ahdsrEnvelope.decayMs   = 10.0f;
            s.ahdsrEnvelope.sustain   = 1.0f;
            s.ahdsrEnvelope.releaseMs = 40.0f;
            oscPitch = s.pinsIn[0].id;
            oscGate  = s.pinsIn[1].id;
            oscVel   = s.pinsIn[2].id;
            oscAudio = s.pinsOut[0].id;
        }
        int voAudio;
        {
            auto& vo = graph.addNode("Voice Out", NodeType::VoiceOut,
                {Pin{0, "Audio", PinKind::Audio, true}}, {}, {200.0f, 0.0f});
            vo.voiceContainerId = containerId;
            voAudio = vo.pinsIn[0].id;
        }
        graph.addLink(viPitch, oscPitch);
        graph.addLink(viGate,  oscGate);
        graph.addLink(viVel,   oscVel);
        graph.addLink(oscAudio, voAudio);

        Node* container = graph.findNode(containerId);
        if (!container) { r.check(false, "uni-audio: container exists"); return; }

        const double sr = 44100.0;
        const int bs = 512;
        PolyVoiceProcessor poly(*container, graph, transport);
        poly.setPlayConfigDetails(0, 2, sr, bs);
        poly.prepareToPlay(sr, bs);

        juce::AudioBuffer<float> out(2, bs);
        {
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::noteOn(1, 57, (juce::uint8) 110), 0); // A3
            poly.processBlock(out, on);
        }
        juce::MidiBuffer empty;
        for (int i = 0; i < 10; ++i) poly.processBlock(out, empty);

        const float rmsL = out.getRMSLevel(0, 0, bs);
        r.check(rmsL > 1.0e-2f, "uni-audio: a unison note produces a tone");

        // Full spread => L and R carry different detuned voices => they differ.
        const float* L = out.getReadPointer(0);
        const float* R = out.getReadPointer(1);
        float meanAbsDiff = 0.0f, meanAbs = 0.0f;
        for (int i = 0; i < bs; ++i) {
            meanAbsDiff += std::abs(L[i] - R[i]);
            meanAbs     += 0.5f * (std::abs(L[i]) + std::abs(R[i]));
        }
        meanAbsDiff /= bs; meanAbs /= bs;
        r.check(meanAbs > 1.0e-3f && meanAbsDiff > 0.1f * meanAbs,
                "uni-audio: full stereo spread decorrelates L and R");

        // Release the note -> the whole stack frees -> silence.
        {
            juce::MidiBuffer off;
            off.addEvent(juce::MidiMessage::noteOff(1, 57), 0);
            poly.processBlock(out, off);
        }
        for (int i = 0; i < 80; ++i) poly.processBlock(out, empty);
        r.check(out.getRMSLevel(0, 0, bs) < 1.0e-4f,
                "uni-audio: releasing the note frees the whole stack to silence");
    }

    // ---- (3) Save/load round-trip of the unison fields --------------------
    {
        NodeGraph graph;
        {
            auto& c = graph.addNode("Voice", NodeType::VoiceContainer,
                {Pin{0, "MIDI", PinKind::Midi, true}},
                {Pin{0, "Audio", PinKind::Audio, false}}, {0.0f, 0.0f});
            c.voiceUnison = 6;
            c.voiceUnisonDetune = 25.0f;
            c.voiceUnisonSpread = 0.66f;
        }
        const std::string text = ProjectFile::serializeForUndo(graph);
        NodeGraph loaded;
        ProjectFile::loadFromString(text, loaded);
        Node* c = nullptr;
        for (auto& n : loaded.nodes) if (n.type == NodeType::VoiceContainer) c = &n;
        r.check(c != nullptr, "uni-save: container present after load");
        if (c) {
            r.check(c->voiceUnison == 6, "uni-save: voiceUnison round-trips");
            r.check(std::abs(c->voiceUnisonDetune - 25.0f) < 0.01f, "uni-save: detune round-trips");
            r.check(std::abs(c->voiceUnisonSpread - 0.66f) < 0.01f, "uni-save: spread round-trips");
        }
    }
}

// ---------------------------------------------------------------------------
// Signal Math module: per-sample arithmetic on two control signals. Verify each
// operation and that the node + its Operation param survive a save/load round-trip.
// ---------------------------------------------------------------------------
void testSignalMath(Report& r) {
    r.section("Signal Math module (modular kit)");

    NodeGraph graph;
    auto& node = graph.addNode("Signal Math", NodeType::SignalShape,
        {Pin{0, "A", PinKind::Signal, true, 1},
         Pin{0, "B", PinKind::Signal, true, 1}},
        {Pin{0, "Out", PinKind::Signal, false, 1}}, {0.0f, 0.0f});
    node.script = "__signalmath__";
    node.params.push_back({"Operation", 0.0f, 0.0f, 5.0f});

    SignalMathProcessor proc(node);
    const int N = 64;
    proc.prepareToPlay(48000.0, N);

    // Layout: ch0/1 audio (unused), ch2 = A, ch3 = B, output Out on ch2.
    juce::AudioBuffer<float> buf(4, N);
    auto setInputs = [&](float aVal, float bVal) {
        buf.clear();
        for (int i = 0; i < N; ++i) {
            buf.setSample(2, i, aVal);
            buf.setSample(3, i, bVal);
        }
    };
    auto run = [&](int op, float aVal, float bVal) -> float {
        node.params[0].value = (float) op;
        setInputs(aVal, bVal);
        juce::MidiBuffer m;
        proc.processBlock(buf, m);
        return buf.getSample(2, N / 2);
    };

    r.check(std::abs(run(0, 3.0f, 4.0f) - 7.0f) < 1e-5f, "math: Add  3+4=7");
    r.check(std::abs(run(1, 3.0f, 4.0f) - (-1.0f)) < 1e-5f, "math: Subtract  3-4=-1");
    r.check(std::abs(run(2, 3.0f, 4.0f) - 12.0f) < 1e-5f, "math: Multiply  3*4=12");
    r.check(std::abs(run(3, 12.0f, 4.0f) - 3.0f) < 1e-5f, "math: Divide  12/4=3");
    r.check(run(3, 5.0f, 0.0f) == 0.0f, "math: Divide by zero -> 0 (no NaN/inf)");
    r.check(std::isfinite(run(3, 5.0f, 0.0f)), "math: Divide by zero stays finite");
    r.check(std::abs(run(4, 3.0f, 4.0f) - 3.0f) < 1e-5f, "math: Min(3,4)=3");
    r.check(std::abs(run(5, 3.0f, 4.0f) - 4.0f) < 1e-5f, "math: Max(3,4)=4");

    // Unwired B reads as 0: Subtract with A=0 negates B (classic invert use).
    {
        node.params[0].value = 1.0f; // Subtract
        buf.clear();
        for (int i = 0; i < N; ++i) buf.setSample(3, i, 0.5f); // only B wired
        juce::MidiBuffer m;
        proc.processBlock(buf, m);
        r.check(std::abs(buf.getSample(2, N / 2) - (-0.5f)) < 1e-5f,
                "math: unwired A(=0) - B inverts B");
    }

    // Output must not leak the raw inputs onto the audio bus.
    r.check(buf.getSample(0, N / 2) == 0.0f && buf.getSample(1, N / 2) == 0.0f,
            "math: audio channels stay silent");

    // Save/load round-trip: node, script tag, and Operation value survive.
    node.params[0].value = 3.0f; // Divide
    const std::string text = ProjectFile::serializeForUndo(graph);
    NodeGraph dst;
    const bool ok = ProjectFile::loadFromString(text, dst);
    r.check(ok, "math-saveload: project text parses back");
    Node* d = dst.findNode(node.id);
    r.check(d != nullptr && d->script == "__signalmath__",
            "math-saveload: script tag survives");
    r.check(d != nullptr && d->type == NodeType::SignalShape,
            "math-saveload: node type survives");
    bool foundOp = false;
    if (d) for (auto& p : d->params)
        if (p.name == "Operation") { foundOp = std::abs(p.value - 3.0f) < 0.01f; }
    r.check(foundOp, "math-saveload: Operation value round-trips");
    r.check(d != nullptr && d->pinsIn.size() == 2 && d->pinsOut.size() == 1,
            "math-saveload: pins (2 in / 1 out) round-trip");
}

// ---------------------------------------------------------------------------
// Signal LFO module: control-rate oscillator with optional per-voice sync.
// ---------------------------------------------------------------------------
void testSignalLFO(Report& r) {
    r.section("Signal LFO module (modular kit)");

    NodeGraph graph;
    auto& node = graph.addNode("Signal LFO", NodeType::SignalShape,
        {Pin{0, "Sync", PinKind::Signal, true, 1}},
        {Pin{0, "Out", PinKind::Signal, false, 1}}, {0.0f, 0.0f});
    node.script = "__signallfo__";
    node.params.push_back({"Rate", 1.0f, 0.1f, 20.0f});
    node.params.push_back({"Shape", 0.0f, 0.0f, 3.0f});
    node.params.push_back({"Polarity", 0.0f, 0.0f, 1.0f});

    SignalLFOProcessor proc(node);
    const double SR = 1000.0; // 1 kHz -> at Rate 1 Hz one cycle == 1000 samples
    const int N = 1000;
    // Layout: ch0/1 audio, ch2 = Sync input AND Out output.
    juce::AudioBuffer<float> buf(3, N);

    auto setParam = [&](const char* name, float v) {
        for (auto& p : node.params) if (p.name == name) p.value = v;
    };

    // --- Square wave, bipolar, free-run: +1 first half, -1 second half. ---
    {
        setParam("Shape", 3.0f); setParam("Polarity", 0.0f); setParam("Rate", 1.0f);
        proc.prepareToPlay(SR, N);
        buf.clear();
        juce::MidiBuffer m;
        proc.processBlock(buf, m);
        r.check(buf.getSample(2, 0)   ==  1.0f, "lfo: square +1 at phase 0");
        r.check(buf.getSample(2, 499) ==  1.0f, "lfo: square +1 just before half");
        r.check(buf.getSample(2, 500) == -1.0f, "lfo: square -1 at half cycle");
        r.check(buf.getSample(2, 999) == -1.0f, "lfo: square -1 at end of cycle");
        r.check(buf.getSample(0, 10) == 0.0f && buf.getSample(1, 10) == 0.0f,
                "lfo: audio channels stay silent");
    }

    // --- Sine, bipolar: starts at 0, stays within [-1,1], reaches ~+1 at 1/4. ---
    {
        setParam("Shape", 0.0f); setParam("Polarity", 0.0f); setParam("Rate", 1.0f);
        proc.prepareToPlay(SR, N);
        buf.clear();
        juce::MidiBuffer m;
        proc.processBlock(buf, m);
        r.check(std::abs(buf.getSample(2, 0)) < 1e-5f, "lfo: sine starts at 0");
        r.check(std::abs(buf.getSample(2, 250) - 1.0f) < 1e-2f, "lfo: sine peaks near +1 at quarter cycle");
        bool inRange = true;
        for (int i = 0; i < N; ++i)
            if (buf.getSample(2, i) < -1.0001f || buf.getSample(2, i) > 1.0001f) inRange = false;
        r.check(inRange, "lfo: sine stays within [-1, 1] (bipolar)");
    }

    // --- Unipolar sine: range [0,1], midpoint 0.5 at phase 0. ---
    {
        setParam("Shape", 0.0f); setParam("Polarity", 1.0f); setParam("Rate", 1.0f);
        proc.prepareToPlay(SR, N);
        buf.clear();
        juce::MidiBuffer m;
        proc.processBlock(buf, m);
        r.check(std::abs(buf.getSample(2, 0) - 0.5f) < 1e-5f, "lfo: unipolar sine = 0.5 at phase 0");
        bool inRange = true;
        for (int i = 0; i < N; ++i)
            if (buf.getSample(2, i) < -1e-4f || buf.getSample(2, i) > 1.0001f) inRange = false;
        r.check(inRange, "lfo: unipolar sine stays within [0, 1]");
    }

    // --- Sync: a rising edge mid-block resets phase (square retriggers to +1). ---
    {
        setParam("Shape", 3.0f); setParam("Polarity", 0.0f); setParam("Rate", 1.0f);
        proc.prepareToPlay(SR, N);
        buf.clear();
        // Sync low for 0..599, high from 600 on -> rising edge at 600.
        for (int i = 600; i < N; ++i) buf.setSample(2, i, 1.0f);
        juce::MidiBuffer m;
        proc.processBlock(buf, m);
        r.check(buf.getSample(2, 599) == -1.0f, "lfo: pre-sync in -1 half (phase 0.599)");
        r.check(buf.getSample(2, 600) ==  1.0f, "lfo: sync rising edge resets phase -> +1");
    }

    // --- Save/load round-trip. ---
    {
        setParam("Shape", 2.0f); setParam("Polarity", 1.0f); setParam("Rate", 5.0f);
        const std::string text = ProjectFile::serializeForUndo(graph);
        NodeGraph dst;
        const bool ok = ProjectFile::loadFromString(text, dst);
        r.check(ok, "lfo-saveload: project text parses back");
        Node* d = dst.findNode(node.id);
        r.check(d != nullptr && d->script == "__signallfo__", "lfo-saveload: script tag survives");
        auto pv = [&](const char* nm) -> float {
            if (d) for (auto& p : d->params) if (p.name == nm) return p.value;
            return -999.0f;
        };
        r.check(std::abs(pv("Rate") - 5.0f) < 0.01f, "lfo-saveload: Rate round-trips");
        r.check(std::abs(pv("Shape") - 2.0f) < 0.01f, "lfo-saveload: Shape round-trips");
        r.check(std::abs(pv("Polarity") - 1.0f) < 0.01f, "lfo-saveload: Polarity round-trips");
    }
}

// ---------------------------------------------------------------------------
// Sample & Hold module: latch a value on each trigger rising edge.
// ---------------------------------------------------------------------------
void testSampleHold(Report& r) {
    r.section("Sample & Hold module (modular kit)");

    NodeGraph graph;
    auto& node = graph.addNode("Sample & Hold", NodeType::SignalShape,
        {Pin{0, "In", PinKind::Signal, true, 1},
         Pin{0, "Trigger", PinKind::Signal, true, 1}},
        {Pin{0, "Out", PinKind::Signal, false, 1}}, {0.0f, 0.0f});
    node.script = "__signalsh__";
    node.params.push_back({"Source", 0.0f, 0.0f, 2.0f});

    SampleHoldProcessor proc(node);
    const int N = 512;
    juce::AudioBuffer<float> buf(4, N); // ch2=In/Out, ch3=Trigger
    auto setSource = [&](float v) { node.params[0].value = v; };

    // --- Input mode: sample a ramp at two trigger edges, hold between. ---
    {
        setSource(0.0f);
        proc.prepareToPlay(48000.0, N);
        buf.clear();
        for (int i = 0; i < N; ++i) buf.setSample(2, i, (float) i);       // In = ramp
        for (int i = 100; i < 150; ++i) buf.setSample(3, i, 1.0f);        // edge @100
        for (int i = 300; i < 350; ++i) buf.setSample(3, i, 1.0f);        // edge @300
        juce::MidiBuffer m;
        proc.processBlock(buf, m);
        r.check(buf.getSample(2, 50)  == 0.0f,   "sh: holds initial 0 before first trigger");
        r.check(buf.getSample(2, 100) == 100.0f, "sh: samples In(=100) at first trigger");
        r.check(buf.getSample(2, 200) == 100.0f, "sh: holds steady between triggers");
        r.check(buf.getSample(2, 299) == 100.0f, "sh: still holding just before second trigger");
        r.check(buf.getSample(2, 300) == 300.0f, "sh: re-samples In(=300) at second trigger");
        r.check(buf.getSample(2, 400) == 300.0f, "sh: holds the new value after");
        r.check(buf.getSample(0, 10) == 0.0f && buf.getSample(1, 10) == 0.0f,
                "sh: audio channels stay silent");
    }

    // --- Random +/-1: one trigger, ignores In, value in [-1,1], then held. ---
    {
        setSource(1.0f);
        proc.prepareToPlay(48000.0, N);
        buf.clear();
        for (int i = 0; i < N; ++i) buf.setSample(2, i, (float) i * 1000.0f); // huge In ramp
        for (int i = 10; i < 20; ++i) buf.setSample(3, i, 1.0f);              // single edge @10
        juce::MidiBuffer m;
        proc.processBlock(buf, m);
        r.check(buf.getSample(2, 5) == 0.0f, "sh-rand: initial 0 before trigger");
        const float v = buf.getSample(2, 10);
        r.check(v >= -1.0f && v <= 1.0f, "sh-rand: held value within [-1, 1] (ignores In ramp)");
        r.check(buf.getSample(2, 400) == v, "sh-rand: holds the random value steady");
    }

    // --- Random 0..1: value in [0,1]. ---
    {
        setSource(2.0f);
        proc.prepareToPlay(48000.0, N);
        buf.clear();
        for (int i = 10; i < 20; ++i) buf.setSample(3, i, 1.0f);
        juce::MidiBuffer m;
        proc.processBlock(buf, m);
        const float v = buf.getSample(2, 200);
        r.check(v >= 0.0f && v <= 1.0f, "sh-rand: held value within [0, 1]");
    }

    // --- Save/load round-trip. ---
    {
        setSource(2.0f);
        const std::string text = ProjectFile::serializeForUndo(graph);
        NodeGraph dst;
        const bool ok = ProjectFile::loadFromString(text, dst);
        r.check(ok, "sh-saveload: project text parses back");
        Node* d = dst.findNode(node.id);
        r.check(d != nullptr && d->script == "__signalsh__", "sh-saveload: script tag survives");
        bool found = false;
        if (d) for (auto& p : d->params)
            if (p.name == "Source") found = std::abs(p.value - 2.0f) < 0.01f;
        r.check(found, "sh-saveload: Source value round-trips");
    }
}

// ---------------------------------------------------------------------------
// Signal Logic module: comparison + boolean logic -> 0/1 gate.
// ---------------------------------------------------------------------------
void testSignalLogic(Report& r) {
    r.section("Signal Logic module (modular kit)");

    NodeGraph graph;
    auto& node = graph.addNode("Signal Logic", NodeType::SignalShape,
        {Pin{0, "A", PinKind::Signal, true, 1},
         Pin{0, "B", PinKind::Signal, true, 1}},
        {Pin{0, "Out", PinKind::Signal, false, 1}}, {0.0f, 0.0f});
    node.script = "__signallogic__";
    node.params.push_back({"Operation", 0.0f, 0.0f, 5.0f});

    SignalLogicProcessor proc(node);
    const int N = 16;
    juce::AudioBuffer<float> buf(4, N); // ch2=A/Out, ch3=B
    proc.prepareToPlay(48000.0, N);

    auto run = [&](int op, float a, float b) -> float {
        node.params[0].value = (float) op;
        buf.clear();
        for (int i = 0; i < N; ++i) { buf.setSample(2, i, a); buf.setSample(3, i, b); }
        juce::MidiBuffer m;
        proc.processBlock(buf, m);
        return buf.getSample(2, N / 2);
    };

    r.check(run(0, 0.8f, 0.5f) == 1.0f, "logic: A>B true (0.8 > 0.5)");
    r.check(run(0, 0.3f, 0.5f) == 0.0f, "logic: A>B false (0.3 > 0.5)");
    r.check(run(1, 0.3f, 0.5f) == 1.0f, "logic: A<B true (0.3 < 0.5)");
    r.check(run(1, 0.8f, 0.5f) == 0.0f, "logic: A<B false");
    r.check(run(2, 1.0f, 1.0f) == 1.0f, "logic: AND true (both high)");
    r.check(run(2, 1.0f, 0.0f) == 0.0f, "logic: AND false (one low)");
    r.check(run(3, 1.0f, 0.0f) == 1.0f, "logic: OR true (one high)");
    r.check(run(3, 0.0f, 0.0f) == 0.0f, "logic: OR false (both low)");
    r.check(run(4, 1.0f, 0.0f) == 1.0f, "logic: XOR true (exactly one)");
    r.check(run(4, 1.0f, 1.0f) == 0.0f, "logic: XOR false (both high)");
    r.check(run(5, 0.0f, 0.0f) == 1.0f, "logic: NOT A true (A low)");
    r.check(run(5, 1.0f, 0.0f) == 0.0f, "logic: NOT A false (A high)");
    r.check(buf.getSample(0, 0) == 0.0f && buf.getSample(1, 0) == 0.0f,
            "logic: audio channels stay silent");

    // Save/load round-trip.
    node.params[0].value = 4.0f; // XOR
    const std::string text = ProjectFile::serializeForUndo(graph);
    NodeGraph dst;
    const bool ok = ProjectFile::loadFromString(text, dst);
    r.check(ok, "logic-saveload: project text parses back");
    Node* d = dst.findNode(node.id);
    r.check(d != nullptr && d->script == "__signallogic__", "logic-saveload: script tag survives");
    bool found = false;
    if (d) for (auto& p : d->params)
        if (p.name == "Operation") found = std::abs(p.value - 4.0f) < 0.01f;
    r.check(found, "logic-saveload: Operation value round-trips");
}

void testSignalFilter(Report& r) {
    r.section("Signal Filter module (resonant LP/HP/BP)");

    NodeGraph graph;
    auto& node = graph.addNode("Signal Filter", NodeType::Effect,
        {Pin{0, "Audio In", PinKind::Audio, true}},
        {Pin{0, "Audio Out", PinKind::Audio, false}}, {0.0f, 0.0f});
    node.script = "__signalfilter__";
    node.params.push_back({"Type", 0.0f, 0.0f, 2.0f});
    node.params.push_back({"Cutoff", 1000.0f, 20.0f, 20000.0f});
    node.params.push_back({"Resonance", 0.2f, 0.0f, 1.0f});

    const double SR = 48000.0;
    const int N = 512;

    // Run a sine of frequency `freq` (or DC if freq==0) through the filter for a
    // few blocks to reach steady state, then return the RMS of the final block.
    auto rmsThrough = [&](int type, float cutoff, float freq) -> float {
        node.params[0].value = (float) type;
        node.params[1].value = cutoff;
        SignalFilterProcessor proc(node);
        proc.prepareToPlay(SR, N);
        double phase = 0.0;
        const double inc = 2.0 * juce::MathConstants<double>::pi * freq / SR;
        juce::AudioBuffer<float> buf(2, N);
        float rms = 0.0f;
        for (int blk = 0; blk < 12; ++blk) {
            for (int i = 0; i < N; ++i) {
                float s = (freq <= 0.0f) ? 1.0f : (float) std::sin(phase);
                phase += inc;
                buf.setSample(0, i, s);
                buf.setSample(1, i, s);
            }
            juce::MidiBuffer m;
            proc.processBlock(buf, m);
            // Measure on the last block only (steady state).
            if (blk == 11) {
                double acc = 0.0;
                for (int i = 0; i < N; ++i) {
                    float v = buf.getSample(0, i);
                    acc += (double) v * v;
                }
                rms = (float) std::sqrt(acc / N);
            }
        }
        return rms;
    };

    const float kSineRms = 0.707f; // RMS of a unit sine

    // Low-pass: passes a low tone, rejects a high tone.
    float lpLow  = rmsThrough(0, 500.0f, 50.0f);
    float lpHigh = rmsThrough(0, 500.0f, 8000.0f);
    r.check(lpLow > 0.5f, "filter LP: 50 Hz passes (rms " + juce::String(lpLow, 3) + ")");
    r.check(lpHigh < 0.15f, "filter LP: 8 kHz rejected (rms " + juce::String(lpHigh, 3) + ")");
    r.check(lpLow > lpHigh * 4.0f, "filter LP: low tone louder than high tone");

    // High-pass: rejects DC, passes a high tone.
    float hpDc   = rmsThrough(1, 500.0f, 0.0f);
    float hpHigh = rmsThrough(1, 500.0f, 8000.0f);
    r.check(hpDc < 0.1f, "filter HP: DC rejected (rms " + juce::String(hpDc, 3) + ")");
    r.check(hpHigh > 0.5f, "filter HP: 8 kHz passes (rms " + juce::String(hpHigh, 3) + ")");

    // Band-pass: rejects DC and a far-off tone, passes a tone at the cutoff.
    float bpDc   = rmsThrough(2, 1000.0f, 0.0f);
    float bpAt   = rmsThrough(2, 1000.0f, 1000.0f);
    float bpFar  = rmsThrough(2, 1000.0f, 12000.0f);
    r.check(bpDc < 0.1f, "filter BP: DC rejected (rms " + juce::String(bpDc, 3) + ")");
    r.check(bpAt > bpDc + 0.2f && bpAt > bpFar, "filter BP: tone at cutoff passes strongest");

    // Stability: output is always finite, even with high resonance + a step.
    {
        node.params[0].value = 0.0f;       // LP
        node.params[1].value = 2000.0f;
        node.params[2].value = 1.0f;       // max resonance
        SignalFilterProcessor proc(node);
        proc.prepareToPlay(SR, N);
        juce::AudioBuffer<float> buf(2, N);
        bool finite = true;
        for (int blk = 0; blk < 8; ++blk) {
            for (int i = 0; i < N; ++i) {
                float s = (blk == 0 && i == 0) ? 1.0f : 0.0f; // impulse
                buf.setSample(0, i, s);
                buf.setSample(1, i, s);
            }
            juce::MidiBuffer m;
            proc.processBlock(buf, m);
            for (int i = 0; i < N; ++i)
                if (!std::isfinite(buf.getSample(0, i))) finite = false;
        }
        r.check(finite, "filter: high-resonance impulse stays finite (no blow-up)");
    }

    // Save/load round-trip.
    node.params[0].value = 2.0f;     // BP
    node.params[1].value = 3500.0f;
    node.params[2].value = 0.7f;
    const std::string text = ProjectFile::serializeForUndo(graph);
    NodeGraph dst;
    const bool ok = ProjectFile::loadFromString(text, dst);
    r.check(ok, "filter-saveload: project text parses back");
    Node* d = dst.findNode(node.id);
    r.check(d != nullptr && d->script == "__signalfilter__",
            "filter-saveload: script tag survives");
    if (d) {
        auto val = [&](const char* nm) -> float {
            for (auto& p : d->params) if (p.name == nm) return p.value;
            return -999.0f;
        };
        r.check(std::abs(val("Type") - 2.0f) < 0.01f, "filter-saveload: Type round-trips");
        r.check(std::abs(val("Cutoff") - 3500.0f) < 0.5f, "filter-saveload: Cutoff round-trips");
        r.check(std::abs(val("Resonance") - 0.7f) < 0.01f, "filter-saveload: Resonance round-trips");
    }
}

void testSignalNoise(Report& r) {
    r.section("Signal Noise module (gated noise generator)");

    NodeGraph graph;
    auto& node = graph.addNode("Signal Noise", NodeType::Instrument,
        {Pin{0, "Gate",     PinKind::Signal, true, 1},
         Pin{0, "Velocity", PinKind::Signal, true, 1}},
        {Pin{0, "Audio", PinKind::Audio, false}}, {0.0f, 0.0f});
    node.script = "__signalnoise__";
    node.params.push_back({"Type", 0.0f, 0.0f, 2.0f});
    node.params.push_back({"Volume", 1.0f, 0.0f, 1.0f});
    // Sustain held at 1.0 so a held gate gives a steady level to measure.
    node.ahdsrEnvelope.attackMs  = 1.0f;
    node.ahdsrEnvelope.decayMs   = 5.0f;
    node.ahdsrEnvelope.sustain   = 1.0f;
    node.ahdsrEnvelope.releaseMs = 5.0f;

    const double SR = 48000.0;
    const int N = 1024;

    // Capture channel-0 output for `type` with the gate held high, after the
    // attack/decay have settled to sustain. Returns the final block's samples.
    auto capture = [&](int type, std::vector<float>& out) {
        node.params[0].value = (float) type;
        SignalNoiseProcessor proc(node);
        proc.prepareToPlay(SR, N);
        juce::AudioBuffer<float> buf(4, N); // ch0/1 audio, ch2 Gate, ch3 Vel
        for (int blk = 0; blk < 6; ++blk) {
            buf.clear();
            for (int i = 0; i < N; ++i) { buf.setSample(2, i, 1.0f); buf.setSample(3, i, 1.0f); }
            juce::MidiBuffer m;
            proc.processBlock(buf, m);
        }
        out.assign(N, 0.0f);
        for (int i = 0; i < N; ++i) out[i] = buf.getSample(0, i);
    };

    auto rms = [](const std::vector<float>& v) {
        double acc = 0.0; for (float x : v) acc += (double)x * x;
        return (float) std::sqrt(acc / juce::jmax((size_t)1, v.size()));
    };
    // Lag-1 autocorrelation: ~0 for white, strongly positive for brown.
    auto lag1 = [](const std::vector<float>& v) {
        double num = 0.0, den = 0.0;
        for (size_t i = 1; i < v.size(); ++i) num += (double)v[i] * v[i - 1];
        for (float x : v) den += (double)x * x;
        return den > 0 ? (float)(num / den) : 0.0f;
    };

    std::vector<float> white, pink, brown;
    capture(0, white);
    capture(1, pink);
    capture(2, brown);

    r.check(rms(white) > 0.1f, "noise: white gate-on produces output (rms " + juce::String(rms(white), 3) + ")");
    r.check(rms(pink)  > 0.05f, "noise: pink gate-on produces output");
    r.check(rms(brown) > 0.05f, "noise: brown gate-on produces output");

    // Spectral character via adjacent-sample correlation: white is near-zero,
    // brown is heavily low-pass (smooth) so strongly correlated, pink between.
    float cw = lag1(white), cp = lag1(pink), cb = lag1(brown);
    r.check(std::abs(cw) < 0.2f, "noise: white is near-uncorrelated (lag1 " + juce::String(cw, 3) + ")");
    r.check(cb > 0.8f, "noise: brown is strongly correlated (lag1 " + juce::String(cb, 3) + ")");
    r.check(cp > cw && cb > cp, "noise: correlation white < pink < brown");

    // Bounds: every sample stays inside [-1, 1].
    bool inBounds = true;
    for (auto* v : { &white, &pink, &brown })
        for (float x : *v) if (x < -1.0001f || x > 1.0001f) inBounds = false;
    r.check(inBounds, "noise: all output within [-1, 1]");

    // Gate low -> silence after the envelope releases.
    {
        node.params[0].value = 0.0f;
        SignalNoiseProcessor proc(node);
        proc.prepareToPlay(SR, N);
        juce::AudioBuffer<float> buf(4, N);
        // One block gate-high then several gate-low to fully release.
        for (int blk = 0; blk < 5; ++blk) {
            buf.clear();
            float g = (blk == 0) ? 1.0f : 0.0f;
            for (int i = 0; i < N; ++i) { buf.setSample(2, i, g); buf.setSample(3, i, 1.0f); }
            juce::MidiBuffer m;
            proc.processBlock(buf, m);
        }
        std::vector<float> tail(N);
        for (int i = 0; i < N; ++i) tail[i] = buf.getSample(0, i);
        r.check(rms(tail) < 1e-4f, "noise: gate-off releases to silence");
    }

    // Save/load round-trip (Type, Volume, and the AHDSR envelope).
    node.params[0].value = 1.0f; // Pink
    node.params[1].value = 0.6f;
    const std::string text = ProjectFile::serializeForUndo(graph);
    NodeGraph dst;
    const bool ok = ProjectFile::loadFromString(text, dst);
    r.check(ok, "noise-saveload: project text parses back");
    Node* d = dst.findNode(node.id);
    r.check(d != nullptr && d->script == "__signalnoise__", "noise-saveload: script tag survives");
    if (d) {
        auto val = [&](const char* nm) -> float {
            for (auto& p : d->params) if (p.name == nm) return p.value;
            return -999.0f;
        };
        r.check(std::abs(val("Type") - 1.0f) < 0.01f, "noise-saveload: Type round-trips");
        r.check(std::abs(val("Volume") - 0.6f) < 0.01f, "noise-saveload: Volume round-trips");
    }
}

// ---------------------------------------------------------------------------
// Voice container end-to-end audio: build a real NodeGraph with a VoiceContainer
// whose inner patch is VoiceIn -> Signal Oscillator -> VoiceOut, drive it with a
// synthetic MIDI buffer through PolyVoiceProcessor, and confirm the clone/build/
// sum path actually makes (and stops making) sound. This is the auditory check
// the design doc flagged as not unit-testable - made testable by using the
// deterministic, signal-driven Signal Oscillator instead of a stochastic synth.
// ---------------------------------------------------------------------------
void testVoiceContainerAudio(Report& r) {
    r.section("Voice container (end-to-end audio: clone + sum)");

    NodeGraph graph;
    Transport transport;

    // --- Build the container + inner patch (mirrors the Add-Node handlers).
    //     addNode reallocates graph.nodes, so capture stable IDs immediately. ---
    int containerId;
    {
        auto& c = graph.addNode("Voice", NodeType::VoiceContainer,
            {Pin{0, "MIDI", PinKind::Midi, true}},
            {Pin{0, "Audio", PinKind::Audio, false}}, {0.0f, 0.0f});
        c.voicePolyphony = 4;
        containerId = c.id;
    }

    int viPitch, viGate, viVel;
    {
        auto& vi = graph.addNode("Voice In", NodeType::VoiceIn, {},
            {Pin{0, "MIDI",     PinKind::Midi,   false},
             Pin{0, "Pitch",    PinKind::Signal, false, 1},
             Pin{0, "Gate",     PinKind::Signal, false, 1},
             Pin{0, "Velocity", PinKind::Signal, false, 1}}, {-200.0f, 0.0f});
        vi.voiceContainerId = containerId;
        viPitch = vi.pinsOut[1].id;
        viGate  = vi.pinsOut[2].id;
        viVel   = vi.pinsOut[3].id;
    }

    int oscPitch, oscGate, oscVel, oscAudio;
    {
        auto& s = graph.addNode("Signal Osc", NodeType::Instrument,
            {Pin{0, "Pitch",    PinKind::Signal, true, 1},
             Pin{0, "Gate",     PinKind::Signal, true, 1},
             Pin{0, "Velocity", PinKind::Signal, true, 1}},
            {Pin{0, "Audio", PinKind::Audio, false}}, {0.0f, 0.0f});
        s.voiceContainerId = containerId;
        s.script = "__signalosc__";
        s.params.push_back({"Waveform", 0.0f, 0.0f, 3.0f});
        s.params.push_back({"Volume",   0.5f, 0.0f, 1.0f});
        s.ahdsrEnvelope.attackMs  = 2.0f;
        s.ahdsrEnvelope.decayMs   = 20.0f;
        s.ahdsrEnvelope.sustain   = 0.8f;
        s.ahdsrEnvelope.releaseMs = 40.0f;
        oscPitch = s.pinsIn[0].id;
        oscGate  = s.pinsIn[1].id;
        oscVel   = s.pinsIn[2].id;
        oscAudio = s.pinsOut[0].id;
    }

    int voAudio;
    {
        auto& vo = graph.addNode("Voice Out", NodeType::VoiceOut,
            {Pin{0, "Audio", PinKind::Audio, true}}, {}, {200.0f, 0.0f});
        vo.voiceContainerId = containerId;
        voAudio = vo.pinsIn[0].id;
    }

    graph.addLink(viPitch, oscPitch);
    graph.addLink(viGate,  oscGate);
    graph.addLink(viVel,   oscVel);
    graph.addLink(oscAudio, voAudio);

    Node* container = graph.findNode(containerId);
    r.check(container != nullptr, "vc-audio: container node exists");
    if (!container) return;

    const double sr = 44100.0;
    const int bs = 512;
    PolyVoiceProcessor poly(*container, graph, transport);
    poly.setPlayConfigDetails(0, 2, sr, bs);
    poly.prepareToPlay(sr, bs);

    auto rmsOf = [](juce::AudioBuffer<float>& b) {
        return b.getRMSLevel(0, 0, b.getNumSamples());
    };
    auto runBlocks = [&](juce::AudioBuffer<float>& out, juce::MidiBuffer& first, int extra) {
        poly.processBlock(out, first);
        juce::MidiBuffer empty;
        for (int i = 0; i < extra; ++i) poly.processBlock(out, empty);
        return rmsOf(out);
    };

    // No notes -> silence.
    {
        juce::AudioBuffer<float> out(2, bs);
        juce::MidiBuffer midi;
        poly.processBlock(out, midi);
        r.check(rmsOf(out) < 1.0e-5f, "vc-audio: silent before any note");
    }

    // One held note -> a tone appears once the attack settles. This proves the
    // whole chain: MIDI parse -> alloc -> VoiceIn drive -> inner-graph clone
    // (buildScope) -> Signal Oscillator reads the control signals -> VoiceOut.
    float rms1 = 0.0f;
    {
        juce::AudioBuffer<float> out(2, bs);
        juce::MidiBuffer on;
        on.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8) 110), 0); // A4
        rms1 = runBlocks(out, on, 4);
        r.check(rms1 > 1.0e-2f, "vc-audio: a held note produces a tone");
    }

    // Two more simultaneous notes -> three voices summed -> more energy than one
    // (different pitches are incoherent, so the sum's RMS exceeds a single voice).
    {
        juce::AudioBuffer<float> out(2, bs);
        juce::MidiBuffer on;
        on.addEvent(juce::MidiMessage::noteOn(1, 72, (juce::uint8) 110), 0); // C5
        on.addEvent(juce::MidiMessage::noteOn(1, 76, (juce::uint8) 110), 0); // E5
        float rms3 = runBlocks(out, on, 4);
        r.check(rms3 > rms1 * 1.2f, "vc-audio: three summed voices louder than one");
    }

    // Release everything -> after the release tail + the RMS free window, the
    // container falls back to silence (no stuck/leaked voices).
    {
        juce::AudioBuffer<float> out(2, bs);
        juce::MidiBuffer off;
        off.addEvent(juce::MidiMessage::allNotesOff(1), 0);
        float tail = runBlocks(out, off, 80); // ~80 blocks ~= 0.9s >> 40ms rel + 250ms free
        r.check(tail < 1.0e-4f, "vc-audio: voices decay to silence after release");
    }
}

// ---------------------------------------------------------------------------
// Voice container save/load: the new membership fields (voiceContainerId,
// voicePolyphony, voiceStealMode) and the container<->inner relationship must
// survive a serialize/deserialize cycle. Uses the exact in-memory path that
// commitSnapshot()/undo uses (serializeForUndo -> loadFromString), so this also
// guards the undo round-trip. CLAUDE.md "Save/Load" checklist item.
// ---------------------------------------------------------------------------
void testVoiceContainerSaveLoad(Report& r) {
    r.section("Voice container (save/load round-trip)");

    NodeGraph src;
    int containerId, innerId;
    {
        auto& c = src.addNode("Voice", NodeType::VoiceContainer,
            {Pin{0, "MIDI", PinKind::Midi, true}},
            {Pin{0, "Audio", PinKind::Audio, false}}, {0.0f, 0.0f});
        c.voicePolyphony = 6;
        c.voiceStealMode = 2;     // round-robin (non-default, to prove it round-trips)
        c.voiceGlideMs   = 60.0f; // glide time (non-default)
        containerId = c.id;
    }
    int viMidi, synMidi, synAudio, voAudio;
    {
        auto& vi = src.addNode("Voice In", NodeType::VoiceIn, {},
            {Pin{0, "MIDI", PinKind::Midi, false}}, {-200.0f, 0.0f});
        vi.voiceContainerId = containerId;
        viMidi = vi.pinsOut[0].id;
    }
    {
        auto& s = src.addNode("FM Synth", NodeType::Instrument,
            {Pin{0, "MIDI", PinKind::Midi, true}},
            {Pin{0, "Audio", PinKind::Audio, false}}, {0.0f, 0.0f});
        s.voiceContainerId = containerId;
        s.script = "__fmsynth__";
        innerId = s.id;
        synMidi  = s.pinsIn[0].id;
        synAudio = s.pinsOut[0].id;
    }
    {
        auto& vo = src.addNode("Voice Out", NodeType::VoiceOut,
            {Pin{0, "Audio", PinKind::Audio, true}}, {}, {200.0f, 0.0f});
        vo.voiceContainerId = containerId;
        voAudio = vo.pinsIn[0].id;
    }
    src.addLink(viMidi, synMidi);
    src.addLink(synAudio, voAudio);

    const std::string text = ProjectFile::serializeForUndo(src);
    NodeGraph dst;
    const bool ok = ProjectFile::loadFromString(text, dst);
    r.check(ok, "vc-saveload: project text parses back");
    r.check(dst.nodes.size() == src.nodes.size(),
            "vc-saveload: node count preserved");
    r.check(dst.links.size() == src.links.size(),
            "vc-saveload: link count preserved");

    Node* c = dst.findNode(containerId);
    r.check(c != nullptr && c->type == NodeType::VoiceContainer,
            "vc-saveload: container survives with its type");
    r.check(c != nullptr && c->voicePolyphony == 6,
            "vc-saveload: voicePolyphony round-trips");
    r.check(c != nullptr && c->voiceStealMode == 2,
            "vc-saveload: voiceStealMode round-trips");
    r.check(c != nullptr && std::abs(c->voiceGlideMs - 60.0f) < 0.01f,
            "vc-saveload: voiceGlideMs round-trips");

    Node* inner = dst.findNode(innerId);
    r.check(inner != nullptr && inner->voiceContainerId == containerId,
            "vc-saveload: inner node keeps its container membership");
    r.check(inner != nullptr && inner->script == "__fmsynth__",
            "vc-saveload: inner synth keeps its identity");

    // Top-level membership (-1) must not leak onto inner nodes, and vice-versa.
    int innerCount = 0;
    for (auto& n : dst.nodes) if (n.voiceContainerId == containerId) ++innerCount;
    r.check(innerCount == 3, "vc-saveload: all three inner nodes stay scoped");
}

// ---------------------------------------------------------------------------
// Voice factory presets: buildVoicePreset() must produce a well-formed
// container + inner patch for each preset id, with the right container settings
// (polyphony / glide / unison), the right inner instrument, and a fully-wired
// VoiceIn -> instrument -> VoiceOut chain. Then one preset is rendered
// end-to-end to prove the built patch actually sounds, and one is round-tripped
// through save/load. This exercises the SAME construction code the GUI menu
// calls (the menu wrapper only adds commitSnapshot + rebuild on top).
// ---------------------------------------------------------------------------
void testVoicePresets(Report& r) {
    r.section("Voice factory presets (buildVoicePreset)");

    // Helper: count nodes scoped to a container, find the inner instrument
    // (the one node that's neither VoiceIn nor VoiceOut), and confirm every
    // inner node was tagged with the container id.
    auto innerInstrument = [](NodeGraph& g, int cid) -> Node* {
        for (auto& n : g.nodes)
            if (n.voiceContainerId == cid
                && n.type != NodeType::VoiceIn && n.type != NodeType::VoiceOut)
                return &n;
        return nullptr;
    };

    struct Expect { int preset; const char* name; int poly; int unison;
                    float glide; const char* script; };
    const Expect cases[] = {
        {0, "Voice",         8, 1,  0.0f, "__fmsynth__"},
        {1, "Warm Pad",      8, 3,  0.0f, "__signalosc__"},
        {2, "Pluck",         8, 1,  0.0f, "__signalosc__"},
        {3, "Supersaw Lead", 1, 7, 50.0f, "__signalosc__"},
        {4, "Noise Perc",    8, 1,  0.0f, "__signalnoise__"},
    };

    for (const auto& e : cases) {
        NodeGraph g;
        const int cid = buildVoicePreset(g, Vec2{0.0f, 0.0f}, e.preset);
        const std::string tag = std::string("preset[") + e.name + "]: ";

        Node* c = g.findNode(cid);
        r.check(c != nullptr && c->type == NodeType::VoiceContainer,
                tag + "container created");
        if (!c) continue;
        r.check(c->name == e.name, tag + "container named");
        r.check(c->voicePolyphony == e.poly, tag + "polyphony set");
        r.check(c->voiceUnison == e.unison, tag + "unison set");
        r.check(std::abs(c->voiceGlideMs - e.glide) < 0.01f, tag + "glide set");

        // Exactly 3 inner nodes (VoiceIn + instrument + VoiceOut), all scoped.
        int inner = 0, vin = 0, vout = 0;
        for (auto& n : g.nodes) {
            if (n.voiceContainerId != cid) continue;
            ++inner;
            if (n.type == NodeType::VoiceIn)  ++vin;
            if (n.type == NodeType::VoiceOut) ++vout;
        }
        r.check(inner == 3, tag + "three inner nodes, all scoped");
        r.check(vin == 1 && vout == 1, tag + "one VoiceIn and one VoiceOut");

        Node* instr = innerInstrument(g, cid);
        r.check(instr != nullptr && instr->script == e.script,
                tag + "inner instrument is the expected kind");

        // The instrument must be wired on both sides: at least one link into it
        // (from VoiceIn) and exactly one out of it (to VoiceOut). 4 nodes total
        // means 2-4 links depending on how many control pins the instr uses.
        if (instr) {
            int into = 0, outOf = 0;
            for (auto& l : g.links) {
                for (auto& pin : instr->pinsIn)  if (pin.id == l.endPin)   ++into;
                for (auto& pin : instr->pinsOut) if (pin.id == l.startPin) ++outOf;
            }
            r.check(into >= 1,  tag + "instrument driven by VoiceIn");
            r.check(outOf == 1, tag + "instrument feeds VoiceOut");
        }
    }

    // End-to-end: the Pluck preset should actually sound when a note is played.
    {
        NodeGraph g;
        Transport transport;
        const int cid = buildVoicePreset(g, Vec2{0.0f, 0.0f}, 2); // Pluck
        Node* c = g.findNode(cid);
        r.check(c != nullptr, "preset-audio: container exists");
        if (c) {
            const double sr = 44100.0; const int bs = 512;
            PolyVoiceProcessor poly(*c, g, transport);
            poly.setPlayConfigDetails(0, 2, sr, bs);
            poly.prepareToPlay(sr, bs);
            juce::AudioBuffer<float> out(2, bs);
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)110), 0);
            poly.processBlock(out, on);
            juce::MidiBuffer empty;
            float peak = 0.0f;
            for (int i = 0; i < 3; ++i) {
                poly.processBlock(out, empty);
                peak = std::max(peak, out.getMagnitude(0, 0, bs));
            }
            r.check(peak > 1.0e-2f, "preset-audio: Pluck preset produces a tone");
        }
    }

    // Save/load round-trip of a built preset (the Supersaw Lead, since it has
    // the most non-default container settings to preserve).
    {
        NodeGraph src;
        const int cid = buildVoicePreset(src, Vec2{0.0f, 0.0f}, 3);
        const std::string text = ProjectFile::serializeForUndo(src);
        NodeGraph dst;
        const bool ok = ProjectFile::loadFromString(text, dst);
        r.check(ok, "preset-saveload: project text parses back");
        r.check(dst.nodes.size() == src.nodes.size(),
                "preset-saveload: node count preserved");
        Node* c = dst.findNode(cid);
        r.check(c != nullptr && c->voicePolyphony == 1,
                "preset-saveload: Supersaw polyphony (mono) round-trips");
        r.check(c != nullptr && c->voiceUnison == 7,
                "preset-saveload: Supersaw unison round-trips");
        r.check(c != nullptr && std::abs(c->voiceGlideMs - 50.0f) < 0.01f,
                "preset-saveload: Supersaw glide round-trips");
    }
}

// ---------------------------------------------------------------------------
// Signal Oscillator - Pulse waveform (M4 "more oscillator flavours"). The Pulse
// shape (Waveform == 4) emits +1 for the first `Pulse Width` fraction of each
// cycle and -1 for the rest, so the duty cycle of the output sign should track
// the Pulse Width param. Drives the processor directly with constant control
// signals (ch2 Pitch, ch3 Gate, ch4 Velocity) and measures the positive sample
// fraction at two widths.
// ---------------------------------------------------------------------------
void testSignalOscPulse(Report& r) {
    r.section("Signal Oscillator (Pulse waveform / PWM)");

    auto dutyCycle = [&](float width) {
        NodeGraph graph;
        auto& n = graph.addNode("Signal Osc", NodeType::Instrument,
            {Pin{0, "Pitch",    PinKind::Signal, true, 1},
             Pin{0, "Gate",     PinKind::Signal, true, 1},
             Pin{0, "Velocity", PinKind::Signal, true, 1}},
            {Pin{0, "Audio", PinKind::Audio, false}}, {0.0f, 0.0f});
        n.script = "__signalosc__";
        n.params.push_back({"Waveform", 4.0f, 0.0f, 4.0f});      // Pulse
        n.params.push_back({"Volume",   1.0f, 0.0f, 1.0f});
        n.params.push_back({"Pulse Width", width, 0.05f, 0.95f});
        // Near-instant attack, full sustain -> amp is ~1 almost immediately, so
        // the output sign reflects the waveform (not the envelope ramp).
        n.ahdsrEnvelope.attackMs  = 0.05f;
        n.ahdsrEnvelope.decayMs   = 1.0f;
        n.ahdsrEnvelope.sustain   = 1.0f;
        n.ahdsrEnvelope.releaseMs = 50.0f;

        const double sr = 48000.0; const int N = 512;
        SignalOscillatorProcessor osc(n);
        osc.prepareToPlay(sr, N);

        juce::AudioBuffer<float> buf(5, N);   // 0/1 audio, 2 pitch, 3 gate, 4 vel
        int pos = 0, neg = 0;
        // Warm up a couple of blocks so the envelope is fully open, then count.
        for (int blk = 0; blk < 8; ++blk) {
            buf.clear();
            for (int i = 0; i < N; ++i) {
                buf.setSample(2, i, 200.0f); // 200 Hz pitch
                buf.setSample(3, i, 1.0f);   // gate held
                buf.setSample(4, i, 1.0f);   // full velocity
            }
            juce::MidiBuffer midi;
            osc.processBlock(buf, midi);
            if (blk < 4) continue;           // skip warmup blocks
            for (int i = 0; i < N; ++i) {
                const float s = buf.getSample(0, i);
                if (s > 1.0e-3f) ++pos; else if (s < -1.0e-3f) ++neg;
            }
        }
        const int total = pos + neg;
        return total > 0 ? (float)pos / (float)total : 0.0f;
    };

    const float d25 = dutyCycle(0.25f);
    const float d75 = dutyCycle(0.75f);
    r.check(std::abs(d25 - 0.25f) < 0.06f,
            "pulse: 25% width -> ~25% positive duty (" + juce::String(d25, 3) + ")");
    r.check(std::abs(d75 - 0.75f) < 0.06f,
            "pulse: 75% width -> ~75% positive duty (" + juce::String(d75, 3) + ")");
    r.check(d75 > d25 + 0.2f, "pulse: wider Pulse Width raises the duty cycle");
}

// ---------------------------------------------------------------------------
// Transport panic (Stop = immediate silence). When the transport stops, the
// audio engine calls GraphProcessor::requestPanic(), which on the next audio
// block calls juce::AudioProcessorGraph::reset() -> every node's reset(). Each
// tail-bearing processor's reset() override must wipe its state so trailing
// sound is cut at once instead of ringing out over the envelope release time.
//
// This drives a SignalOscillatorProcessor directly: hold the gate to open the
// amp envelope, then release it. Without a panic the envelope enters its 200ms
// Release stage and the first post-release block is still audibly loud. Calling
// reset() (the panic lever) must instead hard-reset the envelope so that same
// block is silent. We check BOTH halves so the test fails if reset() ever stops
// actually clearing the envelope.
// ---------------------------------------------------------------------------
void testTransportPanic(Report& r) {
    r.section("Transport panic (Stop silences trailing sound)");

    auto buildOsc = [](Node& n) {
        n.script = "__signalosc__";
        n.params.clear();
        n.params.push_back({"Waveform", 0.0f, 0.0f, 4.0f});   // sine
        n.params.push_back({"Volume",   1.0f, 0.0f, 1.0f});
        n.params.push_back({"Pulse Width", 0.5f, 0.05f, 0.95f});
        n.ahdsrEnvelope.attackMs  = 0.5f;
        n.ahdsrEnvelope.decayMs   = 1.0f;
        n.ahdsrEnvelope.sustain   = 1.0f;
        n.ahdsrEnvelope.releaseMs = 200.0f;   // long tail so the contrast is clear
    };

    const double sr = 48000.0; const int N = 512;

    // Render `blocks` blocks holding the gate, then ONE block with the gate
    // released. If `panic` is true, call reset() right before the released
    // block (simulating the Stop panic). Returns the RMS of that final block.
    auto releaseBlockRms = [&](bool panic) {
        NodeGraph graph;
        auto& n = graph.addNode("Signal Osc", NodeType::Instrument,
            {Pin{0, "Pitch",    PinKind::Signal, true, 1},
             Pin{0, "Gate",     PinKind::Signal, true, 1},
             Pin{0, "Velocity", PinKind::Signal, true, 1}},
            {Pin{0, "Audio", PinKind::Audio, false}}, {0.0f, 0.0f});
        buildOsc(n);

        SignalOscillatorProcessor osc(n);
        osc.prepareToPlay(sr, N);
        juce::AudioBuffer<float> buf(5, N); // 0/1 audio, 2 pitch, 3 gate, 4 vel

        // Hold the gate to open the envelope.
        for (int blk = 0; blk < 8; ++blk) {
            buf.clear();
            for (int i = 0; i < N; ++i) {
                buf.setSample(2, i, 220.0f);
                buf.setSample(3, i, 1.0f);
                buf.setSample(4, i, 1.0f);
            }
            juce::MidiBuffer midi;
            osc.processBlock(buf, midi);
        }

        if (panic) osc.reset(); // the Stop panic lever

        // One block with the gate released.
        buf.clear();
        for (int i = 0; i < N; ++i) {
            buf.setSample(2, i, 220.0f);
            buf.setSample(3, i, 0.0f); // gate low -> release (or silent after reset)
            buf.setSample(4, i, 1.0f);
        }
        juce::MidiBuffer midi;
        osc.processBlock(buf, midi);

        double sum = 0.0;
        for (int i = 0; i < N; ++i) { float s = buf.getSample(0, i); sum += (double)s * s; }
        return (float)std::sqrt(sum / N);
    };

    const float tailRms  = releaseBlockRms(false); // normal release tail
    const float panicRms = releaseBlockRms(true);  // after panic reset()

    r.check(tailRms > 1.0e-2f,
            "panic: without reset, a released note still rings (tail RMS "
            + juce::String(tailRms, 4) + ")");
    r.check(panicRms < 1.0e-4f,
            "panic: reset() cuts the release tail to silence (RMS "
            + juce::String(panicRms, 6) + ")");
    r.check(panicRms < tailRms * 0.01f,
            "panic: reset() is >=100x quieter than the natural release tail");
}

// Audio tracks nest under other tracks exactly like MIDI tracks do
// (TrackNestingMenu accepts AudioTimeline, MidiTimeline and Group). The
// nesting offset is node-level: clip beats stay local and the cascading
// absoluteBeatOffset shifts the whole track.
//
// Regression: AudioTimelineProcessor::processBlock used to read clip.startBeat
// raw, so a nested audio track DREW at its offset position (the piano roll's
// beatToX adds absoluteBeatOffset) but PLAYED at the un-offset one - a silent
// audio/visual desync. The MIDI generator in the same file always applied the
// offset, which is why only audio tracks were affected.
void testAudioTrackNesting(Report& r, const juce::File& dir) {
    r.section("Audio track nesting (clips play at the cascading parent offset)");

    const double sr = 44100.0;
    const int    N  = 512;

    // 4 seconds of steady tone, so any block landing inside the clip has a
    // clearly non-zero RMS no matter where in the clip it falls.
    std::vector<float> samples((size_t)(sr * 4.0), 0.0f);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = 0.5f * (float)std::sin(2.0 * juce::MathConstants<double>::pi
                                            * 440.0 * (double)i / sr);
    auto wav = dir.getChildFile("nested_audio_track.wav");
    if (!writeWavFloat(wav, samples, sr)) {
        r.check(false, "audio-nest: could not write the test wav");
        return;
    }

    // parent track (offset 8 beats) -> child audio track with one 4-beat clip
    // at local beat 0. At 120 BPM a beat is 0.5 s, so the clip must sound over
    // absolute beats 8..12 and be silent over 0..4.
    NodeGraph g;
    int parentId = g.addNode("parent", NodeType::MidiTimeline, {}, {}).id;
    int childId  = g.addNode("child",  NodeType::AudioTimeline, {},
                             { Pin{0, "Audio", PinKind::Audio, false} }).id;
    // Set fields by id only after every addNode call - addNode can reallocate
    // graph.nodes, so a Node& taken earlier would dangle.
    if (auto* p = g.findNode(parentId)) p->groupBeatOffset = 8.0f;
    g.addToGroup(parentId, childId);
    {
        Clip c{};
        c.name = "clip";
        c.startBeat = 0.0f;
        c.lengthBeats = 4.0f;
        c.audioFilePath = wav.getFullPathName().toStdString();
        g.findNode(childId)->clips.push_back(c);
    }
    g.resolveAnchors();

    r.checkVal(std::abs(g.findNode(childId)->absoluteBeatOffset - 8.0f) < 1e-3f,
               "audio-nest: child audio track inherits the 8-beat parent offset",
               g.findNode(childId)->absoluteBeatOffset);

    Transport tr;
    tr.bpm = 120.0;
    tr.sampleRate = sr;
    tr.tempoMap.setGlobalBpm(120.0);
    tr.playing = true;

    AudioTimelineProcessor proc(*g.findNode(childId), tr, g);
    proc.prepareToPlay(sr, N);

    auto rmsAtBeat = [&](double beat) {
        tr.positionSamples = (int64_t)(beat * 60.0 / tr.bpm * sr);
        juce::AudioBuffer<float> buf(2, N);
        juce::MidiBuffer midi;
        proc.processBlock(buf, midi);
        double sum = 0.0;
        for (int i = 0; i < N; ++i) { float s = buf.getSample(0, i); sum += (double)s * s; }
        return (float)std::sqrt(sum / N);
    };

    const float atBeat2  = rmsAtBeat(2.0);   // where the clip would play un-nested
    const float atBeat10 = rmsAtBeat(10.0);  // where it must actually play

    r.check(atBeat10 > 1.0e-2f,
            "audio-nest: nested clip sounds at its offset position (beat 10, RMS "
            + juce::String(atBeat10, 4) + ")");
    r.check(atBeat2 < 1.0e-6f,
            "audio-nest: nothing plays at the un-offset position (beat 2, RMS "
            + juce::String(atBeat2, 8) + ")");

    // Song length has to include the offset too, or an export would stop
    // before the nested track's tail (clip ends at local beat 4 -> absolute 12).
    r.checkVal(std::abs(g.contentEndBeats() - 12.0) < 1e-6,
               "audio-nest: contentEndBeats includes the nesting offset",
               (float) g.contentEndBeats());

    // Detaching folds the inherited offset away again: the same clip is back at
    // beats 0..4, which is what "Clear parent" in the nesting menu relies on.
    g.removeFromGroup(childId);
    if (auto* c = g.findNode(childId)) c->groupBeatOffset = 0.0f;
    g.resolveAnchors();
    r.check(rmsAtBeat(2.0) > 1.0e-2f,
            "audio-nest: un-nesting moves the clip back to beat 0");
}

// Records live input into two nested tracks at once and plays the result back.
// Guards three things that were each broken independently:
//   * the take lands where the playhead was, not at beat 0;
//   * Clip::startBeat is stored node-local, so a take on a nested track does
//     not double-count the nesting offset the processor adds back on;
//   * every armed input gets its own file and its own clip.
void testMultitrackRecording(Report& r, const juce::File& dir) {
    r.section("Multitrack recording (per-input capture, nesting-correct placement)");

    const double sr = 44100.0;
    const int    N  = 512;
    const int    blocks = 86;                      // ~1 s of capture
    const int64_t expectedSamples = (int64_t) blocks * N;

    // parent (offset 8) -> two armed audio tracks on inputs 0 and 1.
    NodeGraph g;
    int parentId = g.addNode("parent", NodeType::MidiTimeline, {}, {}).id;
    int trackAId = g.addNode("micA", NodeType::AudioTimeline, {},
                             { Pin{0, "Audio", PinKind::Audio, false} }).id;
    int trackBId = g.addNode("micB", NodeType::AudioTimeline, {},
                             { Pin{0, "Audio", PinKind::Audio, false} }).id;
    // Fields by id only after the last addNode - addNode can reallocate nodes.
    if (auto* p = g.findNode(parentId)) p->groupBeatOffset = 8.0f;
    for (int id : { trackAId, trackBId }) {
        auto* n = g.findNode(id);
        n->recordArmed = true;
        n->recordInputChannel = (id == trackAId ? 0 : 1);
    }
    g.addToGroup(parentId, trackAId);
    g.addToGroup(parentId, trackBId);
    g.resolveAnchors();

    Transport tr;
    tr.bpm = 120.0;
    tr.sampleRate = sr;
    tr.tempoMap.setGlobalBpm(120.0);
    tr.playing = true;
    tr.positionSamples = (int64_t)(10.0 * 60.0 / tr.bpm * sr);   // absolute beat 10

    auto recDir = dir.getChildFile("recordings_selftest");
    recDir.deleteRecursively();

    MultitrackRecorder rec;
    rec.startRecording(g, tr, sr, recDir.getFullPathName().toStdString());
    r.check(rec.isRecording(), "record: both armed tracks started");
    r.checkVal(rec.getActiveTrackCount() == 2,
               "record: one capture stream per armed input",
               (float) rec.getActiveTrackCount());

    // Feed the two inputs distinct steady tones.
    std::vector<float> chan0(N), chan1(N);
    double phase = 0.0;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < N; ++i, phase += 1.0) {
            chan0[(size_t) i] = 0.5f * (float) std::sin(2.0 * juce::MathConstants<double>::pi
                                                        * 440.0 * phase / sr);
            chan1[(size_t) i] = 0.25f * (float) std::sin(2.0 * juce::MathConstants<double>::pi
                                                         * 660.0 * phase / sr);
        }
        const float* in[2] = { chan0.data(), chan1.data() };
        rec.processSamples(in, 2, N);
    }

    rec.stopRecording(g, tr, sr);
    r.check(!rec.isRecording(), "record: stopped cleanly");
    r.checkVal(rec.getDroppedSampleCount() == 0,
               "record: no samples were dropped on the way to disk",
               (float) rec.getDroppedSampleCount());

    // Each track should have exactly one clip, on disk, at local beat 2
    // (absolute beat 10 minus the 8-beat parent offset).
    for (int id : { trackAId, trackBId }) {
        auto* n = g.findNode(id);
        juce::String who = juce::String(n->name) + ": ";
        if (!r.check(n->clips.size() == 1, ("record: " + who + "got exactly one clip")))
            continue;

        const Clip& c = n->clips[0];
        r.checkVal(std::abs(c.startBeat - 2.0f) < 1e-3f,
                   ("record: " + who + "clip start is node-local (absolute 10 - offset 8)"),
                   c.startBeat);
        r.check(!c.audioFilePath.empty() && juce::File(c.audioFilePath).existsAsFile(),
                ("record: " + who + "wav was written to disk"));
        // 1 s at 120 BPM is 2 beats.
        r.checkVal(std::abs(c.lengthBeats - 2.0f) < 0.05f,
                   ("record: " + who + "clip length matches the captured duration"),
                   c.lengthBeats);
        r.check(!n->recordArmed, ("record: " + who + "disarmed after the take"));

        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::AudioFormatReader> rd(
            wavFormat.createReaderFor(new juce::FileInputStream(juce::File(c.audioFilePath)), true));
        if (r.check(rd != nullptr, ("record: " + who + "wav is readable"))) {
            r.checkVal(rd->lengthInSamples == expectedSamples,
                       ("record: " + who + "every sample the callback saw reached the file"),
                       (float) rd->lengthInSamples);
        }
    }

    // Round trip: play the freshly recorded take back. It must sound at the
    // absolute beat it was recorded at (10), not at the raw local beat (2).
    auto rmsAtBeat = [&](int nodeId, double beat) {
        AudioTimelineProcessor proc(*g.findNode(nodeId), tr, g);
        proc.prepareToPlay(sr, N);
        tr.positionSamples = (int64_t)(beat * 60.0 / tr.bpm * sr);
        juce::AudioBuffer<float> buf(2, N);
        juce::MidiBuffer midi;
        proc.processBlock(buf, midi);
        double sum = 0.0;
        for (int i = 0; i < N; ++i) { float s = buf.getSample(0, i); sum += (double) s * s; }
        return (float) std::sqrt(sum / N);
    };

    const float back = rmsAtBeat(trackAId, 10.5);
    r.check(back > 1.0e-2f,
            "record: the take plays back at the beat it was recorded at (RMS "
            + juce::String(back, 4) + ")");
    r.check(rmsAtBeat(trackAId, 2.5) < 1.0e-6f,
            "record: the take does not also sound at the un-offset beat");

    recDir.deleteRecursively();
}

// ensureInputTracks(): the "press Record and every live mic gets a track"
// decision logic, plus the two-condition record mute that goes with it.
//
// The subtle one is re-use. A take disarms its track when it finishes, so
// anything that decided "already covered?" by looking at recordArmed would
// spawn a fresh duplicate track on every single record press. The check has to
// be on the "Input Channel" param, which persists.
// ===========================================================================
// Dialogs stay off the Windows taskbar
// ===========================================================================
//
// CLAUDE.md: a dialog must never earn its own taskbar button - the shell reads
// an unowned WS_EX_APPWINDOW popup as a second copy of SEANCE. Every
// juce::AlertWindow in the app (~25 of them) is kept off the taskbar by exactly
// one line, installAppLookAndFeel() in main.cpp, which strips
// ComponentPeer::windowAppearsOnTaskbar from LookAndFeel::getAlertBoxWindowFlags().
//
// That single line is the whole fix and nothing else references it, so deleting
// it - or reordering it after an early return in initialise() - would silently
// regress every alert in the app with no compile error and no visible symptom
// until someone happened to look at their taskbar. Hence this test.
//
// It deliberately checks the *peer's actual style flags*, not just the
// look-and-feel's return value. The value is only a request; what matters is
// what AlertWindow built its window from, and the two came apart before (an
// AlertWindow subclass overriding getDesktopWindowStyleFlags() never had its
// override consulted, because the peer is created during construction). Reading
// the peer is the only way to test the thing that actually reaches the OS.
void testDialogTaskbarFlags(Report& r) {
    r.section("Dialogs stay off the Windows taskbar");

    const int taskbar = juce::ComponentPeer::windowAppearsOnTaskbar;

    // The look-and-feel must already be installed by the time any test runs -
    // main.cpp does it at the very top of initialise(), above the --self-test
    // dispatch. If this fails, the install moved below an early return.
    const int lnfFlags = juce::LookAndFeel::getDefaultLookAndFeel().getAlertBoxWindowFlags();
    r.check((lnfFlags & taskbar) == 0,
            "alert flags: the app look-and-feel strips windowAppearsOnTaskbar");

    // End-to-end: a real AlertWindow, and the flags its real peer was built
    // with. TopLevelWindow's constructor puts it on the desktop, so the peer
    // exists immediately; it is never made visible, so nothing appears
    // on screen during the test run.
    juce::AlertWindow aw("Self-test", "Self-test", juce::MessageBoxIconType::NoIcon);
    if (auto* peer = juce::ComponentPeer::getPeerFor(&aw)) {
        // This is also what guards the assumption the whole fix rests on -
        // that AlertWindow sources its flags from the look-and-feel. If a JUCE
        // upgrade decoupled the two, the look-and-feel check above would still
        // pass while every dialog silently regressed; this one would go red.
        r.check((peer->getStyleFlags() & taskbar) == 0,
                "alert flags: a live AlertWindow's peer has no taskbar flag");
    } else {
        r.check(false, "alert flags: AlertWindow created a desktop peer");
    }

    // The other dialog route: custom components go through the launch*ToolDialog
    // helpers, whose ToolDialogWindow drops the same flag by overriding
    // getDesktopWindowStyleFlags(). That override does work - but for a reason
    // worth being humble about, which is why it is tested rather than reasoned
    // about. A DialogWindow is put on the desktop by its base constructor too,
    // with the *base* flags, so at that instant the override is no more visible
    // than AlertWindow's is; what saves it is that ResizableWindow/DialogWindow
    // re-apply the flags through several post-construction paths
    // (lookAndFeelChanged(), setResizable(), setUsingNativeTitleBar(), ...), by
    // which time the vtable is ToolDialogWindow's. It is genuinely redundant -
    // deleting any single one of those calls leaves the behaviour correct
    // (measured) - so this test exists to catch a JUCE upgrade removing the
    // last of them, not to pin any particular one.
    //
    // launchManagedToolDialog is the non-modal variant, so this neither blocks
    // nor leaves anything behind; it does briefly show a small window.
    auto content = std::make_unique<juce::Component>();
    content->setSize(120, 60);
    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(content.release());
    opts.dialogTitle = "Self-test";
    opts.useNativeTitleBar = false;
    opts.resizable = false;
    if (auto* dlg = launchManagedToolDialog(opts)) {
        if (auto* peer = dlg->getPeer())
            r.check((peer->getStyleFlags() & taskbar) == 0,
                    "tool dialog: ToolDialogWindow's peer has no taskbar flag");
        else
            r.check(false, "tool dialog: dialog created a desktop peer");
        dlg->setVisible(false);
        delete dlg;   // launchManagedToolDialog hands lifetime to the caller
    } else {
        r.check(false, "tool dialog: launchManagedToolDialog returned a window");
    }
}

void testAutoInputTracks(Report& r) {
    r.section("Auto-created tracks for live audio inputs");

    NodeGraph g;
    int outId = g.addNode("Output", NodeType::Output,
                          { Pin{0, "Audio In", PinKind::Audio, true} }, {}).id;
    const int outPin = g.findNode(outId)->pinsIn[0].id;

    // Placement is the only part that needs a canvas, so the test supplies a
    // trivial stand-in and walks it right so we can see each call land.
    float nextX = 0;
    auto place = [&] { nextX += 100; return Vec2{nextX, 50}; };

    std::vector<InputTrackSpec> specs = {
        { 0, "Mic 1 (C615)" },
        { 1, "Line In 2 (Focusrite)" },
    };

    auto created = ensureInputTracks(g, specs, outPin, place);
    r.checkVal(created.size() == 2,
               "auto-track: one track created per live input",
               (float) created.size());

    for (size_t i = 0; i < created.size(); ++i) {
        auto* n = g.findNode(created[i]);
        if (!r.check(n != nullptr, "auto-track: created node is findable")) continue;
        juce::String who = juce::String(n->name) + ": ";
        r.check(n->type == NodeType::AudioTimeline,
                ("auto-track: " + who + "is an Audio Track"));
        r.check(n->name == specs[i].trackName,
                ("auto-track: " + who + "named after the device kind and channel"));
        r.checkVal((int) paramByName(*n, "Input Channel", -1.0f) == specs[i].channel,
                   ("auto-track: " + who + "Input Channel param points at the device channel"),
                   paramByName(*n, "Input Channel", -1.0f));
        r.check(n->recordArmed && n->recordInputChannel == specs[i].channel,
                ("auto-track: " + who + "armed on that channel"));
        // A track built for recording starts empty - the placeholder clip a
        // hand-made Audio Track gets would just be a 4-beat block to delete.
        r.check(n->clips.empty(), ("auto-track: " + who + "starts with no placeholder clip"));

        // Wired to the output, so the take is audible next pass without the
        // user dragging a cable.
        bool wired = false;
        for (auto& l : g.links)
            if (!n->pinsOut.empty() && l.startPin == n->pinsOut[0].id && l.endPin == outPin)
                wired = true;
        r.check(wired, ("auto-track: " + who + "cabled to the output node"));
    }

    // Second press: the tracks exist but are disarmed (a take clears the flag).
    // Nothing new must be created; the existing tracks must be re-armed.
    const size_t nodesAfterFirst = g.nodes.size();
    const size_t linksAfterFirst = g.links.size();
    for (int id : created) g.findNode(id)->recordArmed = false;

    auto again = ensureInputTracks(g, specs, outPin, place);
    r.checkVal(again.empty(),
               "auto-track: a second record press re-uses the tracks instead of duplicating",
               (float) again.size());
    r.checkVal(g.nodes.size() == nodesAfterFirst,
               "auto-track: node count unchanged on re-use", (float) g.nodes.size());
    r.checkVal(g.links.size() == linksAfterFirst,
               "auto-track: no duplicate cable to the output", (float) g.links.size());
    for (int id : created)
        r.check(g.findNode(id)->recordArmed,
                "auto-track: existing track re-armed for the new take");

    // A name already taken gets a numeric suffix rather than two identical
    // nodes the user cannot tell apart.
    ensureInputTracks(g, { { 5, "Mic 1 (C615)" } }, outPin, place);
    bool suffixed = false;
    for (auto& n : g.nodes) if (n.name == "Mic 1 (C615) 2") suffixed = true;
    r.check(suffixed, "auto-track: a clashing track name gets a numeric suffix");

    // Channel -1 means "no input assigned" and must never make a track.
    const size_t before = g.nodes.size();
    ensureInputTracks(g, { { -1, "Nothing" } }, outPin, place);
    r.checkVal(g.nodes.size() == before,
               "auto-track: an unassigned input creates nothing", (float) g.nodes.size());

    // --- the two-condition record mute -------------------------------------
    // Silence a track only when BOTH it is mid-take AND the user has not asked
    // to hear tracks while recording them.
    auto rmsThroughPan = [&](int nodeId) {
        PanProcessor pan(*g.findNode(nodeId), g);
        pan.prepareToPlay(44100.0, 512);
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer midi;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 512; ++i) buf.setSample(ch, i, 0.5f);
        pan.processBlock(buf, midi);
        double sum = 0;
        for (int i = 0; i < 512; ++i) { float s = buf.getSample(0, i); sum += (double) s * s; }
        return (float) std::sqrt(sum / 512);
    };

    const int trackId = created[0];
    g.globalCrossfadeSec = 0.0f;   // no ramp, so one block shows the end state
    g.findNode(trackId)->recordingNow = false;
    g.playbackWhileRecording = false;
    r.check(rmsThroughPan(trackId) > 0.4f,
            "record-mute: a track that is not recording passes audio");

    g.findNode(trackId)->recordingNow = true;
    r.check(rmsThroughPan(trackId) < 1e-4f,
            "record-mute: mid-take with playback-while-recording off, the track is silent");

    g.playbackWhileRecording = true;
    r.check(rmsThroughPan(trackId) > 0.4f,
            "record-mute: mid-take with playback-while-recording on, the track is audible");

    // Only the recording track is affected - its neighbours keep playing, so
    // recording an overdub doesn't silence the song you are playing along to.
    g.playbackWhileRecording = false;
    r.check(rmsThroughPan(created[1]) > 0.4f,
            "record-mute: a track that is not mid-take is unaffected");

    // Belt-and-braces: replacing the graph (new project / undo) must be able to
    // clear the flag, or a node could stay silent with nothing in the UI to say why.
    MultitrackRecorder::clearRecordingFlags(g);
    r.check(!g.findNode(trackId)->recordingNow,
            "record-mute: clearRecordingFlags un-sticks a track left mid-take");
}

// ---------------------------------------------------------------------------
// Script Console API: music theory + offline render / raw audio data.
//
// These are the two things the older `soundshop` project could do from Python
// and SEANCE could not: ask the app's music theory questions, and bounce the
// project to a WAV. Both are now bindings on the embedded `soundshop` module
// (see the two banner comments in scripting.cpp).
//
// The checks run through the REAL interpreter via ScriptEngine::run() rather
// than calling the C++ underneath directly, because the entire class of bug
// this guards against lives exactly at that boundary: a wrong PyArg format
// string, a keyword name that doesn't match its docstring, a stolen-vs-borrowed
// reference. Testing MusicTheory:: directly would pass while `import soundshop`
// was broken.
//
// Protocol: the Python side prints one "PASS<tab>name" or "FAIL<tab>name" line
// per check and C++ replays them into the report, so a failure names the exact
// assertion instead of "the script test failed". A trailing COUNT line catches
// a script that died half way through - without it, an exception at check 3
// would quietly report 2 passes and look green.
// ---------------------------------------------------------------------------
void testScriptingApi(Report& r, const juce::File& outDir) {
    r.section("Script Console API (music theory, offline render, wav data)");

    if (!ScriptEngine::pythonAvailable()) {
        r.note("Python unavailable in this build - script API checks skipped");
        return;
    }
    auto& eng = ScriptEngine::instance();
    if (!r.check(eng.init(), "script-api: embedded interpreter initialises")) return;

    // A minimal sounding project: an Output node for the renderer to collect
    // from, plus (built by the script itself) an Audio Track whose clip points
    // at a WAV the script generated. That makes the render check end-to-end -
    // write_wav -> set_audio_file -> renderGraphOffline -> render_samples - so a
    // silent result means a real routing regression, not an empty graph.
    NodeGraph g;
    g.bpm = 120;
    g.addNode("Output", NodeType::Output,
              { Pin{0, "Audio In", PinKind::Audio, true} }, {});

    // Forward slashes so the path needs no escaping inside the Python source,
    // and Windows accepts them either way.
    const std::string outPath =
        outDir.getFullPathName().replaceCharacter('\\', '/').toStdString();

    const std::string code = "OUT = \"" + outPath + "\"\n" + R"PY(
import math, os, array
import soundshop as ss

_count = [0]

def chk(name, fn):
    _count[0] += 1
    try:
        ok = bool(fn())
    except Exception as e:
        print("FAIL\t%s [%s: %s]" % (name, type(e).__name__, e))
        return
    print(("PASS\t" if ok else "FAIL\t") + name)

def raises(exc, fn):
    try:
        fn()
    except exc:
        return True
    except Exception:
        return False
    return False

# ---------------- music theory ----------------

chk("scale_names covers keys, modes and fixed scales",
    lambda: all(n in ss.scale_names() for n in ("Major", "Dorian", "Blues")))
chk("scale_names('mode') is the seven modes of the major scale",
    lambda: len(ss.scale_names("mode")) == 7)
chk("scale_names rejects an unknown category",
    lambda: raises(ValueError, lambda: ss.scale_names("chord")))
chk("scale_intervals('Major') matches the app's table",
    lambda: ss.scale_intervals("Major") == [0, 2, 4, 5, 7, 9, 11])
chk("scale name lookup ignores case and separators",
    lambda: ss.scale_intervals("natural minor") == ss.scale_intervals("Natural_Minor")
            == [0, 2, 3, 5, 7, 8, 10])
chk("'Minor' and 'Ionian' resolve to their spelled-out labels",
    lambda: ss.scale_intervals("Minor") == ss.scale_intervals("Natural Minor")
            and ss.scale_intervals("Ionian") == ss.scale_intervals("Ionian (Major)"))
chk("a scale can be given as explicit semitone offsets",
    lambda: ss.scale_intervals([0, 3, 7]) == [0, 3, 7])
chk("an unknown scale name is a ValueError, not a silent default",
    lambda: raises(ValueError, lambda: ss.scale_intervals("Klingon Lydian")))
chk("rotate_scale('Major', 1) is Dorian - how Key + Mode combine",
    lambda: ss.rotate_scale("Major", 1) == ss.scale_intervals("Dorian"))
chk("rotate_scale('Major', 5) is Aeolian",
    lambda: ss.rotate_scale("Major", 5) == ss.scale_intervals("Aeolian"))
chk("scale_pitches(C major, octave 4) is C4..B4",
    lambda: ss.scale_pitches("C", "Major", 4) == [60, 62, 64, 65, 67, 69, 71])
chk("root accepts a note name, a pitch class and a MIDI number alike",
    lambda: ss.scale_pitches("C", "Major", 4) == ss.scale_pitches(0, "Major", 4)
            == ss.scale_pitches(60, "Major", 4))
chk("scale_pitches count walks on into the next octave",
    lambda: ss.scale_pitches("C", "Major", 4, 8)[7] == 72)
chk("note_degree calls E4 the 3rd of C major",
    lambda: ss.note_degree(64, "C", "Major")["degree"] == 2
            and ss.note_degree(64, "C", "Major")["degree_name"] == "3rd")
chk("note_degree reports scientific octaves (C4 = 60 -> octave 4)",
    lambda: ss.note_degree(60, "C", "Major")["octave"] == 4)
chk("note_degree flags an out-of-scale note and its offset",
    lambda: ss.note_degree(61, "C", "Major")["in_scale"] is False
            and ss.note_degree(61, "C", "Major")["chromatic_offset"] == 1)
chk("degree_to_note inverts note_degree across four octaves",
    lambda: all(ss.degree_to_note(d["degree"], d["octave"], "C", "Major",
                                  d["chromatic_offset"]) == p
                for p, d in ((p, ss.note_degree(p, "C", "Major"))
                             for p in range(36, 85))))
chk("degree_to_note walks below the root without jumping an octave",
    lambda: ss.degree_to_note(-1, 4, "C", "Major") == 59
            and ss.degree_to_note(-7, 4, "C", "Major") == 48)
chk("degree_to_note is strictly increasing across three octaves of degrees",
    lambda: (lambda v: v == sorted(v) and len(set(v)) == len(v) and v[0] == 48)(
        [ss.degree_to_note(d, 4, "C", "Major") for d in range(-7, 15)]))
chk("snap_to_scale pulls C#4 down into C major",
    lambda: ss.snap_to_scale(61, "C", "Major") == 60)
chk("snap_to_scale keeps the shape of its argument (list in, list out)",
    lambda: ss.snap_to_scale([61, 66], "C", "Major") == [60, 65])
chk("transpose works on a single pitch and on a list",
    lambda: ss.transpose(60, 12) == 72 and ss.transpose([60, 62], -2) == [58, 60])
chk("change_key keeps the melody's shape (C major -> D natural minor)",
    lambda: ss.change_key([60, 62, 64, 65, 67], "C", "Major", "D", "Natural Minor")
            == [62, 64, 65, 67, 69])
chk("change_key into the same key is a no-op",
    lambda: ss.change_key([60, 62, 64, 65, 67, 69, 71], "C", "Major", "C", "Major")
            == [60, 62, 64, 65, 67, 69, 71])
chk("pitches may be given as note names",
    lambda: ss.transpose(["C4", "E4"], 0) == [60, 64])
chk("detect_key names C Major Pentatonic as the tightest fit",
    lambda: ss.detect_key([60, 62, 64, 67, 69], 1)[0]["root"] == 0
            and ss.detect_key([60, 62, 64, 67, 69], 1)[0]["scale"] == "Major Pentatonic"
            and ss.detect_key([60, 62, 64, 67, 69], 1)[0]["coverage"] == 1.0)
chk("detect_key honours its limit",
    lambda: len(ss.detect_key([60, 64, 67], 3)) == 3)
chk("is_black_key agrees with the piano roll",
    lambda: ss.is_black_key(61) is True and ss.is_black_key(60) is False)

# ---------------- raw audio data ----------------

SR = 48000
HALF = SR // 2
sine = array.array('f', (0.5 * math.sin(2 * math.pi * 440.0 * i / SR) for i in range(HALF)))
wav = OUT + "/script_api_sine.wav"

written = ss.write_wav(wav, sine, sample_rate=SR, bits=24)
chk("write_wav reports the number of frames it wrote", lambda: written == HALF)
chk("write_wav rejects a relative path (a GUI app's CWD is not the script's)",
    lambda: raises(ValueError, lambda: ss.write_wav("relative.wav", sine)))
chk("read_wav on a missing file raises FileNotFoundError",
    lambda: raises(FileNotFoundError, lambda: ss.read_wav(OUT + "/no_such_file.wav")))

back = ss.read_wav(wav)
chk("read_wav round-trips sample rate, channel count and length",
    lambda: back["sample_rate"] == SR and back["channels"] == 1
            and back["frames"] == HALF)
chk("bulk samples cross as array('f'), not boxed floats",
    lambda: back["data"][0].typecode == "f")
chk("a 24-bit write/read round trip is sample-accurate",
    lambda: max(abs(a - b) for a, b in zip(sine, back["data"][0])) < 1e-5)

stereo = OUT + "/script_api_stereo.wav"
left = array.array('f', [0.25] * 100)
right = array.array('f', [-0.25] * 60)
n2 = ss.write_wav(stereo, [left, right], sample_rate=SR, bits=24)
b2 = ss.read_wav(stereo)
chk("write_wav takes a list of channels and zero-pads the short one",
    lambda: n2 == 100 and b2["channels"] == 2 and b2["frames"] == 100
            and abs(b2["data"][1][10] + 0.25) < 1e-5
            and abs(b2["data"][1][80]) < 1e-5)
chk("write_wav also accepts a plain list of numbers",
    lambda: ss.write_wav(OUT + "/script_api_list.wav", [0.0, 0.5, -0.5, 0.0],
                         sample_rate=SR) == 4)

# ---------------- offline render ----------------

ss.set_bpm(120)
track = ss.add_audio_track("Bounce")
ss.set_audio_file(track, 0, wav)
ss.add_link(track, 0)          # node 0 is the Output the test pre-built

chk("render_samples rejects a channel count it can't produce",
    lambda: raises(ValueError, lambda: ss.render_samples(end_beat=1, channels=3)))
chk("render_samples rejects an impossible sample rate",
    lambda: raises(ValueError, lambda: ss.render_samples(end_beat=1, sample_rate=10)))

rs = ss.render_samples(end_beat=1.0, sample_rate=SR, channels=2)
chk("render length follows the tempo (1 beat at 120 BPM = 0.5 s)",
    lambda: rs["frames"] == HALF and rs["channels"] == 2 and rs["sample_rate"] == SR)
chk("render_samples returns one array('f') per channel",
    lambda: len(rs["data"]) == 2 and rs["data"][0].typecode == "f")
chk("the bounce actually contains the clip's audio",
    lambda: max(abs(v) for v in rs["data"][0]) > 0.1)

rendered = OUT + "/script_api_render.wav"
info = ss.render(rendered, end_beat=1.0, sample_rate=SR, channels=2, bits=24,
                 dither=False)
chk("render writes a decodable file matching its reported frame count",
    lambda: os.path.isfile(rendered) and info["frames"] == HALF
            and ss.read_wav(rendered)["frames"] == HALF)
chk("render reports the span in seconds",
    lambda: abs(info["seconds"] - 0.5) < 1e-6)
chk("render() and render_samples() come out of the same renderer",
    lambda: max(abs(a - b) for a, b in
                zip(rs["data"][0], ss.read_wav(rendered)["data"][0])) < 1e-5)
chk("the default render span covers the content plus a tail",
    lambda: ss.render_samples(sample_rate=SR)["frames"] == (4 + 4) * SR // 2)

# ---------------- the shipped helper modules read the same tables ----------------
#
# cpp/scripts/soundshop_music.py used to carry a hand-written copy of the scale
# tables that had drifted from the app's. It now derives everything from the
# bindings above; these checks are what keeps it that way.

import soundshop_music as sm
import soundshop_tools as tools     # noqa: F401 - must at least import cleanly

chk("soundshop_music's scale index comes from the app's tables",
    lambda: sm.extra_scales["blues"] == ss.scale_intervals("Blues")
            and sm.extra_scales["harmonic minor"] == ss.scale_intervals("Harmonic Minor")
            and sm.extra_scales["pentatonic major"] == ss.scale_intervals("Major Pentatonic"))
chk("soundshop_music knows every scale SEANCE does",
    lambda: all(n.lower() in sm.extra_scales for n in ss.scale_names()))
chk("soundshop_music's mode list is the app's, minus the '(Major)' label",
    lambda: sm.mode_names[0] == "Ionian" and sm.mode_names[1] == "Dorian"
            and sm.modes_dict["ionian"] == 0 and sm.modes_dict["aeolian"] == 5)
chk("soundshop_music.build_table is a rotation of the app's scale",
    lambda: sm.build_table("D", 1) == [(2 + s) % 12 for s in ss.rotate_scale("Major", 1)])
chk("build_table can now reach non-major parent scales too",
    lambda: sm.build_table("C", 0, "Harmonic Minor") == ss.scale_intervals("Harmonic Minor"))
chk("soundshop_music.change_key delegates to the app's Change Key",
    lambda: sm.change_key([60, 62, 64, 65, 67], "C", 0, "D", 5)
            == ss.change_key([60, 62, 64, 65, 67], "C", "Major", "D", "Natural Minor"))
chk("soundshop_music.change_key still takes note-name strings",
    lambda: sm.change_key("C4 E4 G4", "C", 0, "C", 5) == [60, 63, 67])
chk("soundshop_music.detect_keys delegates to the app's ranking",
    lambda: sm.detect_keys([60, 62, 64, 67, 69])[0] == ("C", "Major Pentatonic", 1.0))
chk("soundshop_music note names agree with the app's MIDI numbering",
    lambda: all(sm.Note(n).midi == ss.notenum(n)
                for n in ("C-1", "C0", "C4", "A4", "G9", "F#3", "Eb5")))
chk("soundshop_music spells a MIDI number the ordinary way",
    lambda: str(sm.Note(60)) == "C4" and str(sm.Note(61)) == "C#4"
            and str(sm.Note(63)) == "D#4")
chk("the enharmonic spellings this module exists for still resolve",
    lambda: sm.Note("Eb4").midi == 63 and sm.Note("Cb4").midi == 59
            and sm.Note("B#3").midi == 60 and sm.Note("Fbb5").midi == 75)
chk("soundshop_music.pitch_class handles names, Notes and MIDI numbers",
    lambda: sm.pitch_class("Eb") == 3 and sm.pitch_class(63) == 3
            and sm.pitch_class(sm.Note("Eb4")) == 3)

print("COUNT\t%d" % _count[0])
)PY";

    const juce::String out = juce::String::fromUTF8(eng.run(code, g).c_str());

    juce::StringArray lines;
    lines.addLines(out);
    int seen = 0, declared = -1;
    for (const auto& ln : lines) {
        if (ln.startsWith("PASS\t") || ln.startsWith("FAIL\t")) {
            ++seen;
            r.check(ln.startsWith("PASS\t"), "script-api: " + ln.fromFirstOccurrenceOf("\t", false, false));
        } else if (ln.startsWith("COUNT\t")) {
            declared = ln.fromFirstOccurrenceOf("\t", false, false).getIntValue();
        }
    }

    // Without these two, a script that raised on line 1 would report zero
    // checks and the suite would still be green.
    if (!r.check(declared >= 0, "script-api: the test script ran to completion"))
        r.note("script output was: " + out.substring(0, 3000));
    else
        r.check(seen == declared, "script-api: every check reported a result");
}

int runSelfTest(const juce::File& outDir) {
    outDir.createDirectory();
    Report r;
    r.line("SEANCE terrain-synth self-test");
    r.line("Output dir: " + outDir.getFullPathName());
    r.line("ffmpeg available: " + juce::String(VideoDecoder::available() ? "yes" : "no"));

    testTerrainData(r, outDir);
    testRender(r, outDir);
    testFrameAudition(r, outDir);
    testHeldAuditionHelpers(r);
    testWarp(r);
    testVideoDecode(r, outDir);
    testGlslCompute(r, outDir);
    testBuiltinMath(r);
    testPitchDetect(r);
    testGranularFreeze(r);
    testFmOpEnvelopes(r);
    testAssetLibrary(r);
    testVoiceAllocator(r);
    testVoiceInSignals(r);
    testSignalMath(r);
    testSignalLFO(r);
    testSampleHold(r);
    testSignalLogic(r);
    testSignalFilter(r);
    testSignalNoise(r);
    testVoiceMpe(r);
    testVoiceUnison(r);
    testVoiceContainerAudio(r);
    testVoiceContainerSaveLoad(r);
    testVoicePresets(r);
    testSignalOscPulse(r);
    testTransportPanic(r);
    testAudioTrackNesting(r, outDir);
    testMultitrackRecording(r, outDir);
    testAutoInputTracks(r);
    testDialogTaskbarFlags(r);
    testScriptingApi(r, outDir);

    r.section("Summary");
    r.line("  PASSED: " + juce::String(r.passed));
    r.line("  FAILED: " + juce::String(r.failed));
    if (r.knownBugs > 0)
        r.line("  KNOWN BUGS (tracked in known-issues.md, not counted as failures): "
               + juce::String(r.knownBugs));
    r.line(r.failed == 0 ? "  RESULT: ALL TESTS PASSED" : "  RESULT: FAILURES PRESENT");

    auto reportFile = outDir.getChildFile("selftest_report.txt");
    reportFile.replaceWithText(r.text);

    // Best-effort echo to the parent console (the app is a GUI-subsystem
    // binary, so stdout is normally detached - this only shows when launched
    // from a terminal that we can attach to).
    fprintf(stdout, "%s\n", r.text.toRawUTF8());
    fflush(stdout);

    return r.failed == 0 ? 0 : 1;
}

} // namespace SoundShop

#include "self_test.h"
#include "terrain_synth.h"
#include "transport.h"
#include "node_graph.h"
#include "adsr_envelope.h"
#include "video_decoder.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_graphics/juce_graphics.h>

#include <vector>
#include <string>
#include <functional>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace SoundShop {
namespace {

// ---------------------------------------------------------------------------
// Report accumulator. Every check funnels through check()/section() so the
// report file and the pass/fail tally stay in sync.
// ---------------------------------------------------------------------------
struct Report {
    juce::String text;
    int passed = 0, failed = 0;

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
    bool started = cp.start(cmd);
    if (started) cp.waitForProcessToFinish(20000);
    bool made = video.existsAsFile() && video.getSize() > 0;
    if (!r.check(made, "video: ffmpeg generated a test clip")) {
        r.note("could not generate test video - skipping decode assertions.");
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

} // namespace

int runSelfTest(const juce::File& outDir) {
    outDir.createDirectory();
    Report r;
    r.line("SEANCE terrain-synth self-test");
    r.line("Output dir: " + outDir.getFullPathName());
    r.line("ffmpeg available: " + juce::String(VideoDecoder::available() ? "yes" : "no"));

    testTerrainData(r, outDir);
    testRender(r, outDir);
    testVideoDecode(r, outDir);

    r.section("Summary");
    r.line("  PASSED: " + juce::String(r.passed));
    r.line("  FAILED: " + juce::String(r.failed));
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

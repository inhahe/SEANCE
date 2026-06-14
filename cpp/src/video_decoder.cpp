#include "video_decoder.h"
#include <juce_core/juce_core.h>

namespace SoundShop {

// ---------------------------------------------------------------------------
// ffmpeg / ffprobe location
// ---------------------------------------------------------------------------
namespace {
    juce::File   gOverride;       // override executable or directory ("" = PATH)
    int          gAvailable = -1; // tri-state cache: -1 unknown, 0 no, 1 yes

#if JUCE_WINDOWS
    constexpr const char* kFfmpegExe  = "ffmpeg.exe";
    constexpr const char* kFfprobeExe = "ffprobe.exe";
#else
    constexpr const char* kFfmpegExe  = "ffmpeg";
    constexpr const char* kFfprobeExe = "ffprobe";
#endif

    // Resolve a tool command from the override. If the override is a directory,
    // look for the named tool inside it; if it's the ffmpeg executable itself,
    // resolve ffprobe as its sibling. Falls back to the bare name (PATH).
    juce::String resolveTool(const char* exeName, const char* bareName) {
        if (gOverride != juce::File()) {
            if (gOverride.isDirectory()) {
                auto f = gOverride.getChildFile(exeName);
                if (f.existsAsFile()) return f.getFullPathName();
            } else if (gOverride.existsAsFile()) {
                auto sib = gOverride.getSiblingFile(exeName);
                if (sib.existsAsFile()) return sib.getFullPathName();
                // The override IS ffmpeg; only use it for the ffmpeg command.
                if (gOverride.getFileNameWithoutExtension().equalsIgnoreCase(
                        juce::String(bareName)))
                    return gOverride.getFullPathName();
            }
        }
        return bareName;   // resolved via PATH by the OS
    }

    // Run a child process and capture its output as text.
    juce::String runText(const juce::StringArray& args, int streamFlags,
                         int timeoutMs = 20000) {
        juce::ChildProcess cp;
        if (!cp.start(args, streamFlags)) return {};
        auto out = cp.readAllProcessOutput();
        cp.waitForProcessToFinish(timeoutMs);
        return out;
    }

    // Run a child process and capture its stdout as raw bytes (stderr is left
    // attached to the parent, so it can't corrupt the binary stream).
    bool runBinary(const juce::StringArray& args, std::vector<uint8_t>& out,
                   std::string* err, int timeoutMs = 180000) {
        juce::ChildProcess cp;
        if (!cp.start(args, juce::ChildProcess::wantStdOut)) {
            if (err) *err = "Failed to launch ffmpeg.";
            return false;
        }
        out.clear();
        std::vector<char> buf(1 << 16);
        const auto start = juce::Time::getMillisecondCounter();
        for (;;) {
            int n = cp.readProcessOutput(buf.data(), (int)buf.size());
            if (n > 0) {
                out.insert(out.end(), buf.data(), buf.data() + n);
            } else {
                if (!cp.isRunning()) break;
                juce::Thread::sleep(1);
            }
            if ((int)(juce::Time::getMillisecondCounter() - start) > timeoutMs) {
                cp.kill();
                if (err) *err = "ffmpeg timed out.";
                return false;
            }
        }
        cp.waitForProcessToFinish(2000);
        return true;
    }

    double parseRatio(const juce::String& s) {
        int slash = s.indexOfChar('/');
        if (slash < 0) return s.getDoubleValue();
        double num = s.substring(0, slash).getDoubleValue();
        double den = s.substring(slash + 1).getDoubleValue();
        return (den != 0.0) ? num / den : 0.0;
    }
}

void VideoDecoder::setExecutableOverride(const juce::File& f) {
    gOverride = f;
    gAvailable = -1;   // re-test on next available()
}

juce::File VideoDecoder::executableOverride() { return gOverride; }

juce::String VideoDecoder::ffmpegCommand()  { return resolveTool(kFfmpegExe,  "ffmpeg");  }
juce::String VideoDecoder::ffprobeCommand() { return resolveTool(kFfprobeExe, "ffprobe"); }

void VideoDecoder::refreshAvailability() { gAvailable = -1; }

bool VideoDecoder::available() {
    if (gAvailable >= 0) return gAvailable == 1;
    juce::StringArray args{ ffmpegCommand(), "-hide_banner", "-version" };
    auto out = runText(args, juce::ChildProcess::wantStdOut, 8000);
    gAvailable = out.containsIgnoreCase("ffmpeg version") ? 1 : 0;
    return gAvailable == 1;
}

// ---------------------------------------------------------------------------
// metadata
// ---------------------------------------------------------------------------
VideoInfo VideoDecoder::probe(const juce::File& video) {
    VideoInfo info;
    if (!video.existsAsFile()) { info.error = "File not found."; return info; }

    // Preferred: ffprobe JSON.
    juce::StringArray pargs{
        ffprobeCommand(), "-v", "quiet", "-print_format", "json",
        "-show_format", "-show_streams", "-select_streams", "v:0",
        video.getFullPathName()
    };
    auto json = runText(pargs, juce::ChildProcess::wantStdOut, 15000);
    if (json.isNotEmpty()) {
        auto v = juce::JSON::parse(json);
        auto streams = v.getProperty("streams", juce::var());
        if (auto* arr = streams.getArray()) {
            if (arr->size() > 0) {
                auto s0 = (*arr)[0];
                info.width  = (int)s0.getProperty("width", 0);
                info.height = (int)s0.getProperty("height", 0);
                info.fps    = parseRatio(s0.getProperty("avg_frame_rate", "0/1").toString());
                if (info.fps <= 0.0)
                    info.fps = parseRatio(s0.getProperty("r_frame_rate", "0/1").toString());
            }
        }
        auto fmt = v.getProperty("format", juce::var());
        info.durationSec = fmt.getProperty("duration", "0").toString().getDoubleValue();
        if (info.durationSec <= 0.0) {
            // Some containers carry duration on the stream instead.
            if (auto* arr = streams.getArray())
                if (arr->size() > 0)
                    info.durationSec = (*arr)[0].getProperty("duration", "0")
                                           .toString().getDoubleValue();
        }
        if (info.width > 0 && info.height > 0) {
            info.ok = true;
            return info;
        }
    }

    // Fallback: parse `ffmpeg -i` stderr (no output file -> it errors but prints
    // stream info we can scrape). Works when ffprobe isn't installed.
    juce::StringArray fargs{ ffmpegCommand(), "-hide_banner", "-i",
                             video.getFullPathName() };
    auto err = runText(fargs, juce::ChildProcess::wantStdErr, 15000);
    if (err.isNotEmpty()) {
        // Duration: HH:MM:SS.xx
        int d = err.indexOf("Duration:");
        if (d >= 0) {
            auto t = err.substring(d + 9).trim();
            int h  = t.substring(0, 2).getIntValue();
            int m  = t.substring(3, 5).getIntValue();
            double s = t.substring(6).getDoubleValue();
            info.durationSec = h * 3600.0 + m * 60.0 + s;
        }
        // " 1920x1080" inside a "Video:" line.
        int vline = err.indexOf("Video:");
        if (vline >= 0) {
            auto seg = err.substring(vline, err.indexOfChar(vline, '\n') > 0
                                              ? err.indexOfChar(vline, '\n')
                                              : err.length());
            // find WxH token
            for (int i = 0; i < seg.length(); ++i) {
                if (juce::CharacterFunctions::isDigit(seg[i])) {
                    int j = i;
                    while (j < seg.length() && juce::CharacterFunctions::isDigit(seg[j])) ++j;
                    if (j < seg.length() && seg[j] == 'x') {
                        int k = j + 1;
                        while (k < seg.length() && juce::CharacterFunctions::isDigit(seg[k])) ++k;
                        int w = seg.substring(i, j).getIntValue();
                        int hh = seg.substring(j + 1, k).getIntValue();
                        if (w >= 16 && hh >= 16) { info.width = w; info.height = hh; break; }
                    }
                    i = j;
                }
            }
            int fpsIdx = seg.indexOf("fps");
            if (fpsIdx > 0) {
                // walk back over the number preceding "fps"
                int e = fpsIdx - 1;
                while (e > 0 && seg[e] == ' ') --e;
                int b = e;
                while (b > 0 && (juce::CharacterFunctions::isDigit(seg[b - 1]) || seg[b - 1] == '.'))
                    --b;
                info.fps = seg.substring(b, e + 1).getDoubleValue();
            }
        }
        info.ok = (info.width > 0 && info.height > 0);
        if (!info.ok) info.error = "Could not read video metadata.";
        return info;
    }

    info.error = "ffmpeg/ffprobe not available.";
    return info;
}

// ---------------------------------------------------------------------------
// preview proxy
// ---------------------------------------------------------------------------
int VideoDecoder::extractProxy(const juce::File& video, const juce::File& outDir,
                               int maxDim, int maxFrames, double& proxyFps,
                               std::function<bool()> shouldAbort) {
    proxyFps = 0.0;
    if (!video.existsAsFile()) return 0;
    outDir.createDirectory();
    // Clear any stale frames from a previous extraction in this dir.
    for (auto& f : outDir.findChildFiles(juce::File::findFiles, false, "f*.jpg"))
        f.deleteFile();

    auto info = probe(video);
    double dur = info.durationSec;
    double srcFps = (info.fps > 0.0) ? info.fps : 24.0;
    // Thin the frame rate so the whole video fits in <= maxFrames frames.
    double pf = srcFps;
    if (dur > 0.0 && maxFrames > 0) {
        double capFps = (double)maxFrames / dur;
        if (capFps < pf) pf = capFps;
    }
    pf = juce::jlimit(0.1, 60.0, pf);
    proxyFps = pf;

    juce::String vf = "fps=" + juce::String(pf, 6)
                    + ",scale=" + juce::String(maxDim) + ":" + juce::String(maxDim)
                    + ":force_original_aspect_ratio=decrease";
    juce::StringArray args{
        ffmpegCommand(), "-hide_banner", "-loglevel", "error", "-nostdin",
        "-i", video.getFullPathName(),
        "-vf", vf, "-q:v", "4",
        outDir.getChildFile("f%06d.jpg").getFullPathName()
    };

    juce::ChildProcess cp;
    if (!cp.start(args, juce::ChildProcess::wantStdErr)) return 0;
    while (cp.isRunning()) {
        if (shouldAbort && shouldAbort()) { cp.kill(); return 0; }
        juce::Thread::sleep(40);
    }
    cp.waitForProcessToFinish(2000);

    return outDir.getNumberOfChildFiles(juce::File::findFiles, "f*.jpg");
}

// ---------------------------------------------------------------------------
// final terrain grid
// ---------------------------------------------------------------------------
std::vector<uint8_t> VideoDecoder::decodeGrid(
    const juce::File& video,
    double t0, double t1,
    int cropX, int cropY, int cropW, int cropH,
    int outW, int outH, int outFrames,
    std::string* errOut) {

    std::vector<uint8_t> empty;
    if (!video.existsAsFile()) { if (errOut) *errOut = "Video file not found."; return empty; }
    outW      = juce::jmax(1, outW);
    outH      = juce::jmax(1, outH);
    outFrames = juce::jmax(1, outFrames);
    cropW     = juce::jmax(2, cropW);
    cropH     = juce::jmax(2, cropH);

    double span = juce::jmax(0.0, t1 - t0);
    // Temporal rate that yields ~outFrames frames over the span; -frames:v caps
    // it exactly. A zero/tiny span (or single frame) just grabs one frame.
    juce::String vf = "crop=" + juce::String(cropW) + ":" + juce::String(cropH)
                    + ":" + juce::String(cropX) + ":" + juce::String(cropY);
    if (outFrames > 1 && span > 1.0e-4) {
        double fps = (double)outFrames / span;
        vf += ",fps=" + juce::String(fps, 6);
    }
    vf += ",scale=" + juce::String(outW) + ":" + juce::String(outH);

    juce::StringArray args{
        ffmpegCommand(), "-hide_banner", "-loglevel", "error", "-nostdin",
        "-ss", juce::String(t0, 6)
    };
    if (span > 1.0e-4) { args.add("-t"); args.add(juce::String(span, 6)); }
    args.add("-i");        args.add(video.getFullPathName());
    args.add("-vf");       args.add(vf);
    args.add("-frames:v"); args.add(juce::String(outFrames));
    args.add("-pix_fmt");  args.add("gray");
    args.add("-f");        args.add("rawvideo");
    args.add("-");

    std::vector<uint8_t> raw;
    if (!runBinary(args, raw, errOut)) return empty;

    const size_t frameBytes = (size_t)outW * (size_t)outH;
    const size_t want       = frameBytes * (size_t)outFrames;
    if (raw.size() < frameBytes) {
        if (errOut) *errOut = "ffmpeg produced no frames (check the crop/time range).";
        return empty;
    }
    if (raw.size() >= want) {
        raw.resize(want);
        return raw;
    }
    // Fewer frames than requested (rounding at the tail): pad by repeating the
    // last decoded frame so the grid is exactly outW*outH*outFrames.
    const size_t haveFrames = raw.size() / frameBytes;
    raw.resize(haveFrames * frameBytes);   // drop any partial trailing frame
    raw.reserve(want);
    const uint8_t* last = raw.data() + (haveFrames - 1) * frameBytes;
    std::vector<uint8_t> lastCopy(last, last + frameBytes);
    while (raw.size() < want)
        raw.insert(raw.end(), lastCopy.begin(), lastCopy.end());
    return raw;
}

} // namespace SoundShop

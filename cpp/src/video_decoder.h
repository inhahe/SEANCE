#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_graphics/juce_graphics.h>
#include <vector>
#include <string>
#include <cstdint>
#include <functional>

namespace SoundShop {

// ============================================================================
// Video decoding for the Terrain Synth "import video" feature.
//
// Design: SEANCE is cross-platform, so rather than linking a different native
// decoder per OS (Media Foundation / AVFoundation / GStreamer) or vendoring
// the heavy libav* development libraries, we shell out to the **ffmpeg
// command-line tool** via juce::ChildProcess. One ffmpeg invocation can do
// time-trim + crop + scale + fps-resample + grayscale in a single filter
// chain and stream the raw pixel bytes straight back to us. ffmpeg is the
// universal cross-platform tool for offline frame extraction and is the same
// code path on Windows, macOS and Linux. The only runtime requirement is that
// ffmpeg is installed / on PATH (or pointed at via the override below); when it
// is missing the UI disables the video features with an explanatory tooltip.
//
// The decoded terrain data is baked into the node script (see
// terrain_synth.cpp makeVideoTerrainScript), so reloading a project needs
// neither ffmpeg nor the original video file - this class is only used at
// import / re-crop time and for the preview proxy.
// ============================================================================

struct VideoInfo {
    bool        ok          = false;
    int         width       = 0;
    int         height      = 0;
    double      durationSec = 0.0;
    double      fps         = 0.0;   // average frame rate
    std::string error;               // human-readable message when !ok
};

class VideoDecoder {
public:
    // ---- ffmpeg / ffprobe location --------------------------------------
    // The override points at a directory containing ffmpeg(.exe)/ffprobe(.exe)
    // or at the ffmpeg executable itself. Empty clears it (fall back to PATH).
    static void        setExecutableOverride(const juce::File& ffmpegOrDir);
    static juce::File  executableOverride();
    // The command used to invoke each tool: the override path if valid, else
    // the bare name "ffmpeg" / "ffprobe" (resolved via PATH by the OS).
    static juce::String ffmpegCommand();
    static juce::String ffprobeCommand();
    // True if `ffmpeg -version` runs. Cached after the first successful probe;
    // call refreshAvailability() to re-test (e.g. after changing the override).
    static bool        available();
    static void        refreshAvailability();

    // ---- metadata --------------------------------------------------------
    // Probe width/height/duration/fps. Uses ffprobe when present, else parses
    // `ffmpeg -i` stderr. Blocking; fast (no full decode).
    static VideoInfo   probe(const juce::File& video);

    // ---- preview proxy ---------------------------------------------------
    // Extract a low-resolution JPEG frame sequence (named f000001.jpg ...) into
    // outDir for smooth scrub/playback in the import dialog, so we never spawn
    // ffmpeg per displayed frame. maxDim caps the longest side; maxFrames caps
    // the total (the fps is thinned to fit). Returns the number of frames
    // written and fills proxyFps (frames-per-second of the proxy timeline);
    // frame index i corresponds to source time i / proxyFps. Blocking - run on
    // a background thread. shouldAbort, if set, is polled to allow cancellation.
    static int extractProxy(const juce::File& video, const juce::File& outDir,
                            int maxDim, int maxFrames, double& proxyFps,
                            std::function<bool()> shouldAbort = {});

    // ---- final terrain grid ---------------------------------------------
    // Decode the terrain grid from the ORIGINAL video at full source quality:
    // over the time range [t0, t1] (seconds), crop the source rectangle
    // (cropX, cropY, cropW, cropH in source pixels), scale each frame to
    // outW x outH, and temporally resample to exactly outFrames evenly spaced
    // frames. Returns frame-major row-major grayscale bytes of size
    // outW*outH*outFrames (brightness 0..255; index ((f*outH)+y)*outW + x).
    // Empty on failure (errOut filled when provided). Blocking.
    static std::vector<uint8_t> decodeGrid(
        const juce::File& video,
        double t0, double t1,
        int cropX, int cropY, int cropW, int cropH,
        int outW, int outH, int outFrames,
        std::string* errOut = nullptr);
};

} // namespace SoundShop

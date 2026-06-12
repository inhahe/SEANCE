#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_graphics/juce_graphics.h>
#include "video_decoder.h"
#include "terrain_synth.h"
#include <memory>
#include <atomic>
#include <vector>

namespace SoundShop {

class NodeGraph;

// ============================================================================
// VideoImportDialogComponent - the UI for Terrain Synth's "import video"
// feature (see terrain_synth.h VideoTerrainParams / makeVideoTerrainScript).
//
// Flow:
//   1. Pick a video file (or re-open with the node's existing __video__ script).
//   2. We probe it (ffprobe / ffmpeg -i) for width/height/duration/fps and
//      extract a low-res JPEG proxy sequence in the background for smooth
//      scrub/playback without spawning ffmpeg per frame.
//   3. The user crops in TIME (in/out handles + playhead on the timeline) and
//      in SPACE (a draggable rectangle over the frame), and sets the output
//      grid size: Output W (px), Output H (px) - aspect-linked to the crop so
//      editing one updates the other - and Output Frames (time resolution).
//   4. On Import we decode the final grayscale grid from the ORIGINAL video at
//      full quality (VideoDecoder::decodeGrid, background thread), bake it into
//      the node script as base64, and trigger a graph rebuild.
//
// Threading: ffmpeg work runs on detached std::threads writing into
// std::shared_ptr<Job> structs with atomic flags; a juce::Timer polls them on
// the message thread. The shared_ptr outlives the component if it's closed
// mid-decode, so there's no use-after-free.
// ============================================================================

class VideoImportDialogComponent : public juce::Component,
                                   private juce::Timer {
public:
    // onCommitted is called on the message thread after the node's script has
    // been updated with the decoded terrain - the host wires it to a graph
    // rebuild + undo snapshot + repaint.
    VideoImportDialogComponent(NodeGraph& graph, int nodeId,
                               std::function<void()> onCommitted);
    ~VideoImportDialogComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;

private:
    // ---- background job structs -----------------------------------------
    struct ProxyJob {
        std::atomic<bool> done{false};
        std::atomic<bool> abort{false};
        int    count = 0;
        double fps   = 0.0;
        juce::File dir;
    };
    struct BuildJob {
        std::atomic<bool> done{false};
        std::atomic<bool> abort{false};
        bool ok = false;
        std::vector<uint8_t> gray;
        std::string error;
        VideoTerrainParams params;
    };

    enum class Drag { None, CropMove, CropL, CropR, CropT, CropB,
                      CropTL, CropTR, CropBL, CropBR,
                      TimeIn, TimeOut, Playhead };

    // ---- actions ---------------------------------------------------------
    void chooseVideo();
    void loadVideoFile(const juce::File& f);
    void startProxy();
    void updateAspectLink(bool keepWidth);   // recompute the other output dim
    void loadFrameForTime(double t);
    void doBuildAndCommit();
    void closeSelf();
    void updateControlEnablement();
    void syncDimSlidersFromState();

    // ---- geometry helpers ------------------------------------------------
    juce::Rectangle<float> imageDisplayRect() const;   // where the frame draws
    juce::Rectangle<float> cropScreenRect() const;     // crop in screen px
    Drag hitTest(juce::Point<float> p) const;          // which drag zone
    double timeForX(float x) const;                    // timeline x -> seconds
    float  xForTime(double t) const;                   // seconds -> timeline x

    void timerCallback() override;

    // ---- references / callback ------------------------------------------
    NodeGraph& graph;
    int nodeId;
    std::function<void()> onCommitted;

    // ---- source ----------------------------------------------------------
    juce::File videoFile;
    VideoInfo  info;
    std::shared_ptr<juce::FileChooser> chooser;

    // ---- proxy preview ---------------------------------------------------
    std::shared_ptr<ProxyJob> proxyJob;
    juce::File proxyDir;
    double     proxyFps   = 0.0;
    int        proxyCount = 0;
    juce::Image currentFrameImg;
    int         currentFrameIdx = -1;

    // ---- transport / crop ------------------------------------------------
    double currentTime = 0.0;
    double tIn  = 0.0;
    double tOut = 0.0;
    bool   playing = false;
    juce::Rectangle<float> cropN { 0.0f, 0.0f, 1.0f, 1.0f }; // normalized 0..1

    // ---- output grid -----------------------------------------------------
    int outW = 128, outH = 96, outFrames = 64;

    // ---- build -----------------------------------------------------------
    std::shared_ptr<BuildJob> buildJob;
    bool building = false;

    // ---- interaction state ----------------------------------------------
    Drag dragMode = Drag::None;
    juce::Point<float> dragStartPos;
    juce::Rectangle<float> dragStartCrop;

    // ---- cached layout rects --------------------------------------------
    juce::Rectangle<int> canvasRect;
    juce::Rectangle<int> timelineRect;

    bool syncingDims = false;

    // ---- controls --------------------------------------------------------
    juce::TextButton chooseBtn { "Choose Video..." };
    juce::TextButton playBtn   { "Play" };
    juce::Label      statusLabel;
    juce::Label      titleLabel;

    juce::Slider outWSlider, outHSlider, framesSlider;
    juce::Label  outWLabel, outHLabel, framesLabel;
    juce::Label  gridInfoLabel;

    juce::TextButton okBtn     { "Import" };
    juce::TextButton cancelBtn { "Cancel" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoImportDialogComponent)
};

} // namespace SoundShop

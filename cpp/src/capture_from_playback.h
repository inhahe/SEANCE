#pragma once
#include "wavetable_frame.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>

namespace SoundShop {

class NodeGraph;
struct Transport;

// Source the capture dialog pulls PCM from. Same UI for all three; the
// only differences are (a) where the buffer comes from and (b) whether
// the display auto-refreshes. Playback and Mic are live ring-buffer
// snapshots that tick at 20 Hz; File is a one-shot load.
enum class CaptureSource {
    Playback,  // AudioEngine::getPlaybackTapSnapshot (final mix output)
    Mic,       // AudioEngine::getMicTapSnapshot (audio input)
    File       // user-picked audio file (.wav/.mp3/.aiff/.flac/.ogg)
};

// Capture dialog that snapshots a chunk of PCM from one of three sources
// (recent project playback, the audio input, or an audio file), lets the
// user drag start/end handles to pick a region, and on Capture produces
// N GranularFrames - one per equally-spaced position across the region.
// Each frame holds a multi-sample window (~1 s at the tap's sample rate,
// clamped to the user-selected region) plus a default ~100 ms grain
// length, so the synth's 4-voice OLA stream replays the source's time-
// domain evolution rather than collapsing it to a single cycle. The host
// editor lays the frames along the wavetable's Position dimension so the
// synth can morph through them.
//
// Design constraints
//   - No audio-thread interaction beyond reading a ring buffer once per
//     timer tick (lock-free, torn-read tolerant: a glitch in the display
//     is acceptable; capture itself runs on the GUI thread after a
//     complete snapshot is taken).
//   - The dialog is purely a UI - it produces frames via the OnCapture
//     callback and lets the editor decide where to put them.
//   - Audition (hearing the cycle under the cursor) is intentionally
//     NOT in this first cut; planned as a follow-up. The visual scrub
//     plus auto-refresh is enough for the user to identify a region.
class CaptureFromPlaybackDialog : public juce::Component, private juce::Timer {
public:
    using OnCapture = std::function<void(std::vector<std::unique_ptr<IWavetableFrame>>)>;

    // tableSize - the wavetable's per-frame stored sample count. Kept
    // around for the editor's render-fallback path (GranularFrame::render
    // returns a tableSize-sample preview cycle); not used to truncate the
    // captured source PCM, which is multi-second by design.
    CaptureFromPlaybackDialog(CaptureSource source, int tableSize, OnCapture onCapture);
    ~CaptureFromPlaybackDialog() override;

    // Optional alternative to JUCE's modal dismiss. When set, the Capture
    // and Cancel buttons call this instead of walking up to a parent
    // DialogWindow and exiting its modal state - which is what lets this
    // component be embedded as a child of an arbitrary parent (e.g. the
    // layered wave editor's right pane) instead of living in its own
    // DialogWindow. If left empty the dialog-window behavior is used.
    std::function<void()> onDismiss;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void timerCallback() override;

private:
    OnCapture onCapture;
    CaptureSource source;
    int tableSize;

    // Snapshot of the most recent audio, oldest-first, in `tap`. For
    // Playback / Mic, refilled on every auto-refresh tick. For File,
    // loaded once on file pick and never auto-refreshed.
    std::vector<float> tap;
    double tapSampleRate = 0.0;

    // Region in tap indices, inclusive-start / exclusive-end so a
    // zero-width region is unambiguous. Defaults to the last second of
    // available audio for the live sources; for File it defaults to the
    // full file. Clamped any time `tap` changes size.
    int regionStart = 0;
    int regionEnd   = 0;

    // Currently-dragged handle: -1 none, 0 start handle, 1 end handle,
    // 2 the whole region (middle drag). The drag offset (in samples)
    // anchors the gesture so the click position doesn't jump.
    int   dragHandle  = -1;
    int   dragAnchorSampleOffset = 0;

    // Controls.
    juce::Slider    numFramesSlider;
    juce::Label     numFramesLabel;
    juce::Label     regionInfoLabel;
    juce::Label     sourceInfoLabel;   // shows "Source: <name>" plus file path / mic status
    juce::Label     hintLabel;
    juce::ToggleButton freezeToggle { "Freeze view" };  // Playback / Mic only
    juce::TextButton loadFileBtn    { "Load file..." }; // File only
    juce::TextButton captureBtn { "Capture frames" };
    juce::TextButton cancelBtn  { "Cancel" };

    juce::Rectangle<int> waveRect;

    // True after a file has been successfully loaded (File mode only).
    // Until then captureBtn is disabled and the waveform area shows
    // a "Press Load file..." prompt.
    bool fileLoaded = false;
    juce::String fileSourcePath;

    // Pull a fresh snapshot from AudioEngine (Playback / Mic) and adjust
    // the region to stay within the new buffer length. No-op for File.
    void refreshSnapshot();

    // Prompt for a file, decode it via AudioFormatManager, and store the
    // mono-summed PCM in `tap`. Resets region to the whole file. Only
    // used in File mode.
    void chooseAndLoadFile();

    // Build the N GranularFrames the Capture button delivers. Each frame
    // takes a ~1-second source window (clamped to the user-selected
    // region) centered on its equally-spaced position, paired with a
    // default ~100 ms grain length and embeddedPitchHz = 440 Hz. The
    // synth plays the source back via its 4-voice OLA granular layer.
    std::vector<std::unique_ptr<IWavetableFrame>> buildFrames(int n) const;

    // Region <-> screen-x mapping (within waveRect).
    int xForIdx(int idx) const;
    int idxForX(int x) const;
    void updateRegionInfoLabel();
    void updateSourceInfoLabel();

    // Convenience: live-source modes auto-refresh; File mode does not.
    bool isLiveSource() const {
        return source == CaptureSource::Playback || source == CaptureSource::Mic;
    }

    // Constants used by the layout.
    static constexpr int kHandleHitRadius = 8;
};

// Capture-from-project dialog (v2). Replaces the live-playback ring-buffer
// flow with one that pre-renders the whole song to PCM offline, then lets
// the user transport-control or scrub a marker through it. The synth
// engine plays back the rendered PCM at full fidelity while Playing, and
// loops a short grain centered on the marker while Paused or Scrubbing -
// so the user can audition the spot they're about to capture without
// having to commit to a Capture button blind.
//
// Differences from CaptureFromPlaybackDialog (Mic/File still use that):
//   - No live ring buffer; we render the project once on dialog open via
//     the same offline-render pattern doExportRender uses.
//   - One save = one GranularFrame at the marker (not a batch), since
//     the whole interaction model is "find the spot, save it". Batch
//     capture can return as a follow-up.
//   - Transport states (Stopped / Playing / Paused / Scrubbing) drive
//     AudioEngine::setPreviewMode so the engine mixes the appropriate
//     stream (song PCM or grain loop) into the master output.
//   - Save is disabled while Playing - the audible spot for the captured
//     grain is the marker, and the marker is moving during play; tooltip
//     explains.
class CaptureFromSongDialog : public juce::Component, private juce::Timer {
public:
    using OnCapture = std::function<void(std::vector<std::unique_ptr<IWavetableFrame>>)>;

    // graph / transport: the live project to render. Held by reference -
    // assumed to outlive the dialog (true: dialog is modal, owners stay
    // put). tableSize: same contract as CaptureFromPlaybackDialog. The
    // dialog kicks off the render in the constructor (non-blocking) and
    // becomes interactive once the render completes.
    CaptureFromSongDialog(NodeGraph& graph, Transport& transport,
                          int tableSize, OnCapture onCapture);
    ~CaptureFromSongDialog() override;

    // Optional alternative to JUCE's modal dismiss. When set, the Save and
    // Close buttons call this instead of walking up to a parent
    // DialogWindow and exiting its modal state - which is what lets this
    // component be embedded as a child of an arbitrary parent (e.g. the
    // layered wave editor's right pane) instead of living in its own
    // DialogWindow. If left empty the dialog-window behavior is used.
    std::function<void()> onDismiss;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void timerCallback() override;

private:
    NodeGraph& graph;
    Transport& transport;
    int tableSize;
    OnCapture onCapture;

    // UI-side transport state. Drives AudioEngine::setPreviewMode and
    // the enabled state of the buttons. Scrubbing is identical to Paused
    // for audio purposes (grain loop) but separately tracked so the
    // marker keeps following the mouse instead of staying frozen.
    enum class TState { Stopped, Playing, Paused, Scrubbing };
    TState state = TState::Stopped;

    // Pre-rendered project PCM, mono, at the device sample rate so the
    // engine plays it back without any resampling step. Shared with the
    // engine via std::atomic<std::shared_ptr> so the engine can read the
    // latest pointer lock-free.
    std::shared_ptr<std::vector<float>> songPcm;
    double songSampleRate = 0.0;

    // Current marker position in samples within songPcm. Single source
    // of truth for "where the user is auditioning". During Playing this
    // is updated from the engine's published preview-pos; otherwise it's
    // driven by the user dragging.
    int64_t markerSamplePos = 0;

    juce::TextButton playBtn  { "Play"  };
    juce::TextButton pauseBtn { "Pause" };
    juce::TextButton stopBtn  { "Stop"  };
    juce::Slider     widthSlider;
    juce::Label      widthLabel;
    // CrossfadeLoop seam crossfade length. Only meaningful when freeze
    // mode = CrossfadeLoop; left enabled in other modes so the user can
    // pre-set it before switching, but ignored by the engine for those
    // modes. Range / default in the cpp via kXfadeMinMs etc.
    juce::Slider     crossfadeSlider;
    juce::Label      crossfadeLabel;
    // The crossfade value the user picked, in ms, independent of the
    // slider's current visible value. The visible value gets clamped down
    // to width/2 whenever Width drops, but we keep the user's intended
    // value here so a later Width increase restores the original
    // crossfade rather than leaving it stuck at the clamp. Updated only
    // when the user actually moves the crossfade slider; not updated by
    // syncCrossfadeMaxToWidth's defensive re-set of the slider value.
    double crossfadeDesiredMs = kXfadeDefMs;
    // Set while syncCrossfadeMaxToWidth is mutating crossfadeSlider's
    // range / value. JUCE's Slider::setRange fires onValueChange with
    // sendNotificationSync when its internal clamp shrinks the current
    // value, and we need to suppress that one path so the synthetic
    // clamped value doesn't get written back to crossfadeDesiredMs.
    bool syncingCrossfadeFromWidth = false;
    juce::Label      statusLabel;
    juce::Label      hintLabel;
    // Freeze-mode picker (4 algorithms, see GranularFreezeMode docs in
    // granular_frame.h). Changing the selection updates the engine's
    // audition mode live and bakes into whatever GranularFrame is saved
    // next. Stored as an int 0-3 in the combo's selected-id field.
    juce::ComboBox   freezeModeCombo;
    juce::Label      freezeModeLabel;
    juce::TextButton saveBtn   { "Save frame at marker" };
    juce::TextButton cancelBtn { "Close" };

    juce::Rectangle<int> waveRect;
    bool draggingMarker = false;

    // Background render job. Started in the constructor; the dialog
    // shows "Rendering..." until job->done == true and job->result has
    // been moved into songPcm. Owned by the dialog so destruction
    // cancels the render cleanly.
    class RenderJob;
    std::unique_ptr<RenderJob> renderJob;
    bool renderReady = false;

    void onRenderComplete();
    void regenerateGrain();
    void setState(TState s);
    void updateButtonsForState();
    void updateStatusLabel();
    // Keeps the crossfade slider's range capped at half the current
    // Width slider value. Mirrors the engine's `xfade = min(xfadeReq,
    // grainLen/2)` clamp in the UI so the slider can't visibly point
    // at a value the engine will quietly ignore. Snaps the current
    // value down if it exceeds the new max.
    void syncCrossfadeMaxToWidth();
    int     xForSamplePos(int64_t pos) const;
    int64_t samplePosForX(int x) const;
    int     xForMarker() const { return xForSamplePos(markerSamplePos); }

    // Build the single GranularFrame the Save button delivers. Grain
    // length = widthSlider value (10..500 ms); source PCM = a multi-
    // second window of the rendered song around the marker (max of
    // 4 x grain or 1 second, clamped to song length). Wrapped in a
    // single-element vector to match the existing on-capture signature
    // used by the wave editor.
    std::vector<std::unique_ptr<IWavetableFrame>> buildFrameAtMarker() const;

    // Width slider extents. 100 ms default - small enough that the
    // captured "moment" is recognizable, large enough that the grain
    // loop has audible pitch under loop. Range 10..500 ms covers
    // pad/drone-style very-long-grain frames at the upper end and
    // single-cycle plucks near the lower.
    static constexpr double kWidthMinMs = 10.0;
    static constexpr double kWidthMaxMs = 500.0;
    static constexpr double kWidthDefMs = 100.0;

    // Crossfade slider extents. Default 50 ms = halfway between "barely
    // covers a click" and "blends the whole seam smooth"; well within
    // audible blending for any musical width. The engine clamps the
    // user-set value at L/2 internally, so picking 250 ms here against
    // a 100 ms width effectively becomes 50 ms - the slider's behaviour
    // gracefully saturates without misbehaving. Range 1..500 ms covers
    // everything from tight bare-loop mode to maximally smooth.
    static constexpr double kXfadeMinMs = 1.0;
    static constexpr double kXfadeMaxMs = 500.0;
    static constexpr double kXfadeDefMs = 50.0;
};

} // namespace SoundShop

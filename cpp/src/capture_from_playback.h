#pragma once
#include "wavetable_frame.h"
#include "granular_frame.h"   // GranularFreezeMode
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
//   - Mic source: while the dialog is open the input is monitored through
//     the output (AudioEngine::inputMonitoring) so the user hears what
//     they're about to capture. The Freeze button mutes monitoring and
//     holds the display still for region selection; Go live resumes both.
//   - The selected region can be auditioned (Mic + File): the Preview
//     button loops the slice through the engine's GrainLoop preview, and
//     dragging the region handles plays it automatically (scrub-to-
//     audition), mirroring the Capture-from-project dialog. Mic requires
//     the display to be frozen first (a sweeping ring buffer can't loop
//     stably); File can audition as soon as a file is loaded.
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

    // Live write-through sink for post-hoc metadata edits (the unified save
    // model), mirroring CaptureFromSongDialog::onMetadataEdited. The host sets
    // this ONLY in RE-CAPTURE / replace mode, where the dialog is bound to one
    // existing library GranularFrame: changing the "As note" picker, the Freeze
    // mode, the Grains slider, or the FFT-size combo fires this callback
    // IMMEDIATELY so the host can mutate the live frame and commit, exactly like
    // GranularFrameEditorComponent does on every edit. This is what makes the
    // mic/file re-capture panel "always save" its metadata the way the song
    // dialog does - closing the panel can no longer silently discard a
    // pitch/freeze/grain/FFT change. The PCM grab itself stays an explicit
    // "Capture waveforms" act because it depends on the region selection and
    // per-waveform width (capture-time params), which is inherent to capture,
    // not a save-model discrepancy. crossfadeMs carries this dialog's Crossfade
    // slider (in MILLISECONDS, rate-independent), so moving it writes straight
    // through to the bound frame just like the other metadata. Left null in
    // append mode (no pre-existing frame to write through to).
    std::function<void(double pitchHz, int freezeModeIdx, double crossfadeMs,
                       int grainCount, int fftSize)>
        onMetadataEdited;

    // Seed the dialog's editable metadata controls (note/octave + cents picker,
    // freeze mode, Grains slider, FFT-size combo) to mirror an existing granular
    // frame. Used when this dialog is embedded to RE-CAPTURE an existing library
    // frame: without seeding, the controls would default (A4 pitch, Crossfade
    // loop, default grains/FFT) and any metadata edit under the write-through
    // sink would silently relabel the frame, discarding the user's original
    // choices. crossfadeMs seeds the Crossfade slider so a re-capture starts
    // from the frame's current seam length (not a fixed default), and later
    // moves of the slider write back through onMetadataEdited. Pass
    // freezeModeIdx -1 to leave the freeze picker at its default; grainCount <
    // kGranularMinGrains / fftSize < 0 likewise leave the texture controls
    // untouched. Uses dontSendNotification so seeding never fires the sink.
    void seedFromExistingFrame(double pitchHz, int freezeModeIdx,
                               double crossfadeMs, int grainCount, int fftSize);

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

    // Push the current post-hoc metadata (pitch, freeze, crossfade, grains, FFT)
    // to the onMetadataEdited write-through sink, if one is set. No-op otherwise
    // (append mode). Called from the pitch / freeze / crossfade / grain / FFT
    // onChange handlers.
    void publishMetadataEdit();

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
    // All sources: grain length in MILLISECONDS - the size of each overlapping
    // Hann grain the cloud modes scatter, and (for the loop/FFT modes that don't
    // granulate) the unit the auto window-length tracks. Baked into every
    // produced GranularFrame as grainLength (converted to samples). Greyed out
    // for CrossfadeLoop / SpectralFreeze, which ignore the grain (the window IS
    // the loop / FFT frame) - see refreshFreezeExtras(). Mirrors the wave
    // editor's two-control grain+window model so capture and editor agree.
    juce::Slider    grainLenSlider;
    juce::Label     grainLenLabel;
    // All sources: how many audio samples each captured waveform spans - its
    // freeze WINDOW (band) length, the region the freeze mode roams / loops /
    // analyses over. Auto-tracks autoWindowMultiplier(mode) x grain (4x for the
    // grain-cloud modes so they get roam room, 1x for loop/FFT), mirroring the
    // editor's per-method window default, until the user drags it (windowUserSet)
    // - then it stays a fixed sample count. With a single waveform it simply
    // spans the whole selection (slider disabled). Drives both the produced
    // GranularFrames' windowLen and the section bands drawn over the waveform.
    juce::Slider    samplesPerWaveformSlider;
    juce::Label     samplesPerWaveformLabel;
    // All sources: one-click "set the per-waveform width so the bands tile the
    // current selection with no gap or overlap" - i.e. snap the window to
    // regionLen / numFrames (what syncWindowToFitSlots does). Lets the user
    // freely resize the selection (which opens gaps / overlaps) and then
    // recover an exact tiling without hand-matching the slider. Counts as an
    // explicit window override (sets windowUserSet).
    juce::TextButton fitWidthBtn { "Fit width to selection" };
    juce::Label     regionInfoLabel;
    juce::Label     sourceInfoLabel;   // shows "Source: <name>" plus file path / mic status
    juce::Label     hintLabel;
    juce::ToggleButton pauseToggle { "Pause view" };  // Playback only
    // Mic only: a single prominent button that couples live input monitoring
    // and the sweeping display. "Pause" mutes monitoring AND halts the
    // display so the user can drag region handles on a still waveform;
    // "Go live" resumes both. Deliberately NOT called "Freeze" - that word is
    // reserved for the granular freeze *methods* (CrossfadeLoop, AsyncGranular,
    // PitchSyncGrains, SpectralFreeze) used to sustain a captured waveform, so
    // a "Freeze" button next to a freeze-method picker would be ambiguous.
    // See setMicLive().
    juce::TextButton micLiveBtn     { "Pause" };
    // Mic + File: loop the selected region through the engine's GrainLoop
    // preview so the user can HEAR the slice they're about to capture before
    // committing it to the library. Toggles "Preview" <-> "Stop". The loop
    // follows the region handles as they're dragged (see mouseDrag). Disabled
    // for Mic until the display is paused (a sweeping buffer can't be
    // auditioned). See startRegionAudition() / regenerateAuditionGrain().
    juce::TextButton previewBtn     { "Preview" };
    // Mic + File: which captured waveform (1..numFrames) the Preview button
    // auditions. Capture spreads N waveforms across the selection; this picks
    // which one to hear without committing. 1-based in the UI;
    // regenerateAuditionGrain reads (value - 1) as the band index. Disabled at
    // a single waveform (nothing to choose - Preview plays the whole
    // selection). See updatePreviewIndexControl().
    juce::Slider     previewIndexSlider;
    juce::Label      previewIndexLabel;
    // All sources: output gain applied to every captured waveform. Sets the
    // produced GranularFrame's per-frame gain (IWavetableFrame::gain) - the
    // same scalar the wave editor's Gain knob drives - so a recording that's
    // too quiet or too loud can be levelled at capture time. 1.0 = unity (the
    // raw recorded level). The Preview reflects it live (the engine preview
    // path has no per-frame gain, so regenerateAuditionGrain bakes it into the
    // throwaway preview buffer). Double-click resets to unity. Range 0..4.
    juce::Slider     gainSlider;
    juce::Label      gainLabel;
    // All sources: CrossfadeLoop seam length in MILLISECONDS, mirroring the wave
    // editor's and song dialog's crossfade controls so capture/editor agree.
    // Only audible in CrossfadeLoop mode (the loop blends its end into its start
    // over this duration to hide the seam) but left enabled in every mode so the
    // user can dial it in ahead of time. Baked into every produced GranularFrame
    // (crossfadeSamples) and reflected live in the audition. The slider's max
    // tracks half the freeze window (syncCrossfadeMaxToWindow) - the engine can't
    // overlap two ramps longer than that within one loop. Range / default in the
    // cpp via kXfadeMinMs etc.
    juce::Slider     crossfadeSlider;
    juce::Label      crossfadeLabel;
    // The crossfade value the user picked, in ms, independent of the window-driven
    // max clamp: when the window shrinks the visible slider value is clamped down,
    // but growing the window restores this desired value rather than leaving it
    // stuck at the clamp. Updated only by genuine user moves (guarded by
    // syncingCrossfadeFromWindow), never by syncCrossfadeMaxToWindow's re-set.
    double crossfadeDesiredMs = kXfadeDefMs;
    // Set true while syncCrossfadeMaxToWindow is mutating crossfadeSlider's range
    // / value, so the onValueChange it triggers doesn't overwrite
    // crossfadeDesiredMs or fire a spurious write-through (the user only changed
    // the window, a capture-time param, not the crossfade itself).
    bool syncingCrossfadeFromWindow = false;
    // Mic only: opens the Audio Device Settings dialog. The actionable escape
    // hatch for the known WASAPI-combined-device bug (a USB-webcam mic + a
    // different output device get welded into one shared-mode device whose
    // input is corrupted into garbage). Switching the driver type to
    // DirectSound there fixes it. See the note in the Mic hint text.
    juce::TextButton audioDeviceBtn { "Audio device..." };
    juce::TextButton loadFileBtn    { "Load file..." }; // File only
    juce::TextButton captureBtn { "Capture waveforms" };
    juce::TextButton cancelBtn  { "Cancel" };

    // Embedded pitch picker (note + octave + cents readout). Drives the
    // produced GranularFrame's embeddedPitchHz so MIDI playback at the
    // matching key plays the source at 1:1, with the usual wavetable-
    // style pitch shift for other keys. Mirrors the picker in the
    // post-capture GranularFrameEditorComponent so the user can dial in
    // the pitch at capture time without having to open the editor.
    juce::Label    noteOctaveLabel;
    juce::ComboBox noteCombo;
    juce::ComboBox octaveCombo;
    juce::Label    centsLabel;
    // Current embedded pitch in Hz. Updated by noteCombo / octaveCombo
    // onChange. Defaults to A4 (440 Hz) so MIDI note 69 plays the source
    // at 1:1 if the user doesn't touch the picker. buildFrames reads
    // this for the GranularFrame's embeddedPitchHz.
    double capturedPitchHz = 440.0;

    // Granular freeze-mode picker. Selects which "sustain the marker spot"
    // algorithm the captured frames use (CrossfadeLoop / AsyncGranular /
    // PitchSyncGrains / SpectralFreeze - see GranularFreezeMode in
    // granular_frame.h). Drives the live audition through the shared
    // GrainFreezeVoice and is baked into every produced GranularFrame so the
    // held synth note sounds exactly like the preview. IDs are 1-based:
    // ID = (int)mode + 1. Present for all three capture sources.
    juce::Label    freezeModeLabel;
    juce::ComboBox freezeModeCombo;
    // The freeze mode currently picked in freezeModeCombo (defaults to
    // CrossfadeLoop if nothing is selected). Used by buildFrames (baked into
    // every produced GranularFrame) and startRegionAudition / the engine.
    GranularFreezeMode selectedFreezeMode() const {
        const int idx = freezeModeCombo.getSelectedId() - 1;
        return (idx >= 0) ? (GranularFreezeMode)idx
                          : GranularFreezeMode::CrossfadeLoop;
    }

    // Per-waveform texture controls. The "Grains" slider sets the overlapping-
    // grain count for the two grain-cloud modes; the "FFT size" combo sets the
    // SpectralFreeze FFT size. Both are baked into every produced GranularFrame
    // (buildFrames) and drive the live audition. Each is greyed out in the modes
    // that don't use it (refreshFreezeExtras), per the "grayed-out controls must
    // explain themselves" rule.
    juce::Label    grainCountLabel;
    juce::Slider   grainCountSlider;
    juce::Label    fftSizeLabel;
    juce::ComboBox fftSizeCombo;
    // Current grain count (>= 2) baked into produced frames + the audition.
    int selectedGrainCount() const {
        return juce::jlimit(kGranularMinGrains, kGranularMaxGrains,
                            (int)std::round(grainCountSlider.getValue()));
    }
    // Current requested FFT size (combo ID 1 == Auto -> 0; else the size).
    int selectedFftSize() const {
        const int i = fftSizeCombo.getSelectedId() - 2;
        return (i >= 0 && i < kNumSpectralFftSizes) ? kSpectralFftSizes[i] : 0;
    }
    // Enable/disable + retooltip the two texture controls for the current freeze
    // mode, and push their values to the engine's audition preview.
    void refreshFreezeExtras();

    juce::Rectangle<int> waveRect;

    // True after a file has been successfully loaded (File mode only).
    // Until then captureBtn is disabled and the waveform area shows
    // a "Press Load file..." prompt.
    bool fileLoaded = false;
    juce::String fileSourcePath;

    // ----- Mic live monitoring (issue: "can't hear the mic") -----
    // While the Mic dialog is open we route the input through the output so
    // the user can hear what they're about to capture. micPaused == true
    // means the user pressed Pause: monitoring is muted and the display is
    // held still. priorInputMonitoring snapshots the engine's global
    // inputMonitoring flag at construction so we restore it on close instead
    // of clobbering the main-window "Mon" toggle. Mic source only.
    bool micPaused = false;
    bool priorInputMonitoring = false;
    // Drive monitoring + display-pause + button text together. live == true:
    // hear input + sweep; live == false: mute + hold still. Mic source only.
    void setMicLive(bool live);

    // ----- Region audition (hear the slice before capturing) -----
    // When true, the selected region is looping through the engine's
    // GrainLoop preview. Mic + File only. Mic requires the display to be
    // frozen first (canAudition()); a live sweeping buffer can't be
    // auditioned stably.
    bool auditioning = false;
    bool canAudition() const;          // region valid + buffer stable
    void startRegionAudition();        // begin the GrainLoop preview
    void stopRegionAudition();         // PreviewMode::Off
    void regenerateAuditionGrain();    // (re)publish the region as the loop source
    void updatePreviewButton();        // text / colour / enabled / tooltip

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

    // Per-waveform freeze-WINDOW (band) length in samples. With a single
    // waveform it is the whole selection; with 2+ it is the "Samples per
    // waveform" slider value, which auto-tracks autoWindowMultiplier(mode) x
    // grain until the user overrides it (windowUserSet). Floored at the grain
    // for the cloud modes (a grain must fit in the band) and capped to the
    // available audio. Shared by buildFrames, the region-audition grain, and the
    // section bands drawn in paint() so all three agree.
    int effectiveSrcLen() const;

    // Grain length in samples (the "Grains... ms" slider converted against the
    // tap rate, floored at 64). The size of each overlapping Hann grain the
    // cloud modes use; inert for CrossfadeLoop / SpectralFreeze. Baked into the
    // produced GranularFrame as grainLength and used as the unit the auto window
    // length multiplies.
    int effectiveGrainLen() const;

    // True once the user has explicitly set the per-waveform window (dragged the
    // "Samples per waveform" slider or pressed "Fit width to selection"). While
    // false the window auto-tracks autoWindowMultiplier(mode) x grain via
    // syncWindowToAuto(); once true the window is a fixed sample count that
    // survives grain / mode / count changes (matching the editor's explicit-band
    // behaviour). A single waveform ignores this (its window is the selection).
    bool windowUserSet = false;

    // Source-window start index (in tap samples) for waveform i of n, given a
    // window length srcLen. Two regimes meeting continuously at an exact tiling:
    // when the windows FIT (regionLen >= n*srcLen) the leftover space is split
    // into n+1 equal gaps so the end margins match the inter-band gaps
    // (gap = (regionLen - n*srcLen) / (n + 1)); when they must OVERLAP
    // (regionLen < n*srcLen) the row stays contained within the selection
    // (band 0 flush to the left handle, band n-1 flush to the right, overlap
    // distributed between) so it never spills past the handles. Clamped to the
    // available audio [0, tapLen - srcLen]. Used by buildFrames(); paint() draws
    // the bands from the same geometry (in pixel space) so the drawn bands and
    // the captured audio agree.
    int bandStartForIndex(int i, int n, int srcLen) const;

    // Build a granular source buffer: the windowLen-sample band body starting at
    // startIdx in the tap, PLUS a windowLen/2 lookahead tail (drawn from the tap
    // past the band end, zero-padded if the recording ends first). The reserved
    // tail is what the CrossfadeLoop seam crossfade reads to blend the loop
    // boundary without a click - the same viability requirement (srcLen >= L +
    // L/2) that terrain_synth's renderGrainSample and the audio engine's
    // CrossfadeLoop reader enforce. The captured frame is BANDED (windowStart =
    // 0, windowLen = the band), so the voice's clean per-mode banded math runs:
    // CrossfadeLoop loops [0, windowLen), the cloud modes roam inside it (roam
    // room = windowLen - grain), SpectralFreeze analyses inside it.
    std::vector<float> buildGrainSource(int startIdx, int windowLen) const;

    // All sources: set the "Samples per waveform" slider to the current slot
    // spacing (regionLen / numFrames), so the window equals one slot and the
    // section bands tile the selection with zero gaps. Marks windowUserSet (an
    // explicit override). Invoked by the "Fit width to selection" button.
    void syncWindowToFitSlots();

    // All sources: while the window is in auto mode (!windowUserSet) set the
    // "Samples per waveform" slider to autoWindowMultiplier(mode) x grain,
    // clamped to the available audio - the per-method window default that gives
    // the cloud modes roam room (4x) and keeps the loop/FFT modes at 1x, exactly
    // like the editor's kWindowAutoPerMethod sentinel. No-op once the user has
    // overridden the window, and for a single waveform (its window is the whole
    // selection). Called whenever the grain length, freeze mode, or count
    // changes. Uses dontSendNotification so it never trips windowUserSet.
    void syncWindowToAuto();

    // All sources: enable/disable + relabel the "Samples per waveform" slider for
    // the current waveform count. With a single waveform there is no per-
    // waveform spacing to honour - the one window simply spans the whole
    // selection - so the slider is disabled and shown holding the selection
    // length (kept in sync as the selection is resized). With 2+ waveforms the
    // slider is the live per-waveform length control. Call wherever the count or
    // the region changes.
    void updateSamplesPerWaveformControl();

    // All sources: keep the Crossfade slider's max at half the current freeze
    // window (effectiveSrcLen / 2, in ms) - the largest seam the CrossfadeLoop
    // can hold, mirroring the engine's xfade = min(req, window/2) clamp and the
    // editor's crossfadeMaxSamples. The displayed value is min(desired, newMax),
    // so shrinking the window clamps the visible crossfade down while growing it
    // back restores the user's intent. Guards re-entrancy via
    // syncingCrossfadeFromWindow. Call whenever the window can change (grain /
    // mode / count change, window drag, Fit).
    void syncCrossfadeMaxToWindow();

    // Mic + File: set the "Preview waveform" slider's range (1..numFrames) and
    // enabled state for the current count, clamping the current pick into range.
    // Disabled at a single waveform (there's only one thing to audition). Call
    // wherever the waveform count changes.
    void updatePreviewIndexControl();

    // Region <-> screen-x mapping (within waveRect).
    int xForIdx(int idx) const;
    int idxForX(int x) const;

    // The interactive zone for the two region handles: waveRect widened by the
    // handle hit radius on the left and right (vertical extent unchanged). A
    // handle sitting at the very start/end of the buffer is drawn centred on the
    // buffer edge, so half of its bar/caps would fall outside waveRect and be
    // clipped away - making it hard to grab. Drawing, hit-testing, and the
    // repaint region all use this widened zone so the full bar is visible and
    // grabbable even when its outer half is past the wave-view edge.
    juce::Rectangle<int> handleZone() const;
    void updateRegionInfoLabel();
    void updateSourceInfoLabel();

    // Convenience: live-source modes auto-refresh; File mode does not.
    bool isLiveSource() const {
        return source == CaptureSource::Playback || source == CaptureSource::Mic;
    }

    // Constants used by the layout.
    static constexpr int kHandleHitRadius = 8;

    // Crossfade slider extents (ms). Default 50 ms = a gentle seam blend. The
    // max is dynamic (half the freeze window, via syncCrossfadeMaxToWindow); the
    // floor keeps setRange valid at tiny windows. Mirrors the song dialog's
    // kXfade* so the two crossfade controls behave identically.
    static constexpr double kXfadeMinMs = 1.0;
    static constexpr double kXfadeDefMs = 50.0;
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

    // Live write-through sink for post-hoc metadata edits (the unified save
    // model). When set - which the host does only in RE-CAPTURE / replace
    // mode, where the dialog is bound to an existing library GranularFrame -
    // changing the "As note" picker, the Freeze mode, or the Crossfade slider
    // fires this callback IMMEDIATELY, so the host can mutate the live frame
    // and commit, exactly like GranularFrameEditorComponent does on every
    // edit. This is what makes the capture panel "always save" its metadata
    // the way the frame editor does: closing the panel can no longer silently
    // discard a pitch/freeze/crossfade change. The PCM grab itself stays an
    // explicit "Save waveform at marker" act because it depends on the marker
    // position and Width (capture-time params), which is inherent to capture,
    // not a save-model discrepancy. Crossfade is reported in MILLISECONDS
    // (rate-independent) so the host can convert to samples against the
    // frame's OWN sourceSampleRate rather than this dialog's render rate.
    // Left null in append mode (no pre-existing frame to write through to);
    // there, creation stays an explicit Save and Close legitimately creates
    // nothing.
    std::function<void(double pitchHz, int freezeModeIdx, double crossfadeMs,
                       int grainCount, int fftSize)>
        onMetadataEdited;

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
    // Per-waveform texture controls, mirroring CaptureFromPlaybackDialog. The
    // "Grains" slider sets the overlapping-grain count for the two grain-cloud
    // modes; the "FFT size" combo sets the SpectralFreeze FFT size. Both bake
    // into the produced GranularFrame (buildFrameAtMarker) and drive the live
    // audition. Each is shown only in the modes that use it (refreshFreezeExtras),
    // with a tooltip explaining what it does, per the "grayed-out controls must
    // explain themselves" rule.
    juce::Label      grainCountLabel;
    juce::Slider     grainCountSlider;
    juce::Label      fftSizeLabel;
    juce::ComboBox   fftSizeCombo;
    // Current grain count (>= 2) baked into the produced frame + the audition.
    int selectedGrainCount() const {
        return juce::jlimit(kGranularMinGrains, kGranularMaxGrains,
                            (int)std::round(grainCountSlider.getValue()));
    }
    // Current requested FFT size (combo ID 1 == Auto -> 0; else the size).
    int selectedFftSize() const {
        const int i = fftSizeCombo.getSelectedId() - 2;
        return (i >= 0 && i < kNumSpectralFftSizes) ? kSpectralFftSizes[i] : 0;
    }
    // Enable/show + retooltip the two texture controls for the current freeze
    // mode, and push their values to the engine's audition preview.
    void refreshFreezeExtras();
    // Embedded pitch picker (note + octave + cents readout). Drives the
    // produced GranularFrame's embeddedPitchHz so MIDI playback at the
    // matching key plays the source at 1:1, with the usual wavetable-
    // style pitch shift for other keys. Mirrors the picker in the
    // post-capture GranularFrameEditorComponent so the user can dial in
    // the pitch at capture time without having to open the editor.
    juce::Label      noteOctaveLabel;
    juce::ComboBox   noteCombo;
    juce::ComboBox   octaveCombo;
    juce::Label      centsLabel;
    // Current embedded pitch in Hz. Updated by noteCombo / octaveCombo
    // onChange. Defaults to A4 (440 Hz) so MIDI note 69 plays the source
    // at 1:1 if the user doesn't touch the picker. buildFrameAtMarker
    // reads this for the GranularFrame's embeddedPitchHz.
    double capturedPitchHz = 440.0;
    juce::TextButton saveBtn   { "Save waveform at marker" };
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
    // Publish the GrainLoop playback pitch ratio to the audio engine so the
    // live marker audition is pitched to match what the synth voice will
    // produce when the captured frame is triggered at the editor's reference
    // note (A4 = 440 Hz). ratio = 440 / capturedPitchHz. Called when the
    // "As note" picker changes and whenever the grain audition is respun.
    void publishPreviewPitch();
    // Push the current post-hoc metadata (pitch, freeze mode, crossfade) to
    // the onMetadataEdited write-through sink, if one is set. No-op otherwise
    // (append mode). Called from the pitch / freeze / crossfade handlers.
    void publishMetadataEdit();
    void setState(TState s);

public:
    // Seed the dialog's editable controls (note/octave + cents picker, freeze
    // mode, crossfade slider) AND the live audition to mirror an existing
    // granular frame. Used when this dialog is embedded to RE-CAPTURE an
    // existing library frame: without seeding, the controls would default
    // (A4 pitch, Crossfade loop, default crossfade ms) and a Save - or, under
    // the unified save model, any metadata edit - would silently relabel the
    // frame, discarding the user's original choices. Seeding from the existing
    // frame means the panel opens reflecting the frame's real state, so the
    // write-through sink only ever changes what the user deliberately touches.
    // crossfadeMs is rate-independent; the caller converts the frame's stored
    // crossfadeSamples to ms against the frame's own sourceSampleRate. Safe to
    // call right after construction. Uses dontSendNotification so seeding never
    // fires the write-through sink. Pass freezeModeIdx -1 / crossfadeMs < 0 to
    // leave those controls at their defaults. grainCount < kGranularMinGrains
    // and fftSize < 0 likewise leave the texture controls untouched; otherwise
    // the grain-count slider and FFT-size combo are seeded to match the frame
    // so a later "Save waveform at marker" preserves the frame's density /
    // resolution instead of resetting it to the dialog defaults.
    void seedFromExistingFrame(double pitchHz, int freezeModeIdx,
                               double crossfadeMs, int grainCount, int fftSize);

private:
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

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
    // All sources: how long each captured waveform spans, in MILLISECONDS - its
    // freeze WINDOW (band) length, the region the freeze mode roams / loops /
    // analyses over. Expressed in ms (the same unit as grain length and
    // crossfade) so the window:grain ratio is readable at a glance; converted to
    // samples against the source rate where needed. Auto-tracks
    // autoWindowMultiplier(mode) x grain (4x for the grain-cloud modes so they
    // get roam room, 1x for loop/FFT), mirroring the editor's per-method window
    // default, until the user drags it (windowUserSet) - then it stays a fixed
    // ms value. With a single waveform it simply spans the whole selection
    // (slider disabled). Drives both the produced GranularFrames' windowLen and
    // the section bands drawn over the waveform.
    juce::Slider    windowLenSlider;
    juce::Label     windowLenLabel;
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

    // Granular freeze-mode picker. Selects which "sustain the captured region"
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

    // The window length the slider asks for, in samples (ms slider value
    // converted against the tap rate), floored at the per-mode minimum and
    // capped to the buffer length but NOT capped to the current selection.
    // effectiveSrcLen() applies the region cap on top of this; minSelectionLen()
    // uses the un-capped value to decide how short a resize drag may go.
    int windowFromSlider() const;

    // Minimum selection length a resize drag is allowed to reach, in samples.
    // 0 for a single waveform (any size allowed). For 2+ it is the requested
    // window (windowFromSlider) so a band always fits, EXCEPT when the view is
    // zoomed in tighter than that window, in which case it is capped to the
    // visible window length so the user can still shrink within the view.
    int minSelectionLen() const;

    // Called during a handle drag: if the selection got shorter than
    // minSelectionLen(), push the dragged handle back out so the selection
    // length is restored, falling back to the buffer boundary if needed.
    void enforceMinSelectionDuringDrag();

    // Grain length in samples (the "Grains... ms" slider converted against the
    // tap rate, floored at 64). The size of each overlapping Hann grain the
    // cloud modes use; inert for CrossfadeLoop / SpectralFreeze. Baked into the
    // produced GranularFrame as grainLength and used as the unit the auto window
    // length multiplies.
    int effectiveGrainLen() const;

    // True once the user has explicitly set the per-waveform window (dragged the
    // "Window length" slider or pressed "Fit width to selection"). While
    // false the window auto-tracks autoWindowMultiplier(mode) x grain via
    // syncWindowToAuto(); once true the window is a fixed sample count that
    // survives grain / mode / count changes (matching the editor's explicit-band
    // behaviour). A single waveform ignores this (its window is the selection).
    bool windowUserSet = false;

    // True once the user has manually moved the Grain-length slider. While false
    // the grain snaps to the freeze mode's default (defaultGrainMsForMode: 5 ms
    // for Async, 40 ms for Pitch-sync) whenever the mode changes, so each cloud
    // mode opens at a grain that sounds constant; once the user picks a grain we
    // stop snapping and respect their value. Seeded from / written back to
    // captureDialogPrefs so a manual choice survives re-opening.
    bool grainUserSet = false;

    // The waveform count the selection was last sized for. When the count changes
    // the selection is resized in proportion (resizeSelectionForCountChange) so
    // each waveform's window length and inter-band spacing stay constant - adding
    // waveforms grows the selection instead of cramming more bands into a fixed
    // span. Seeded to the initial slider value; updated in the count handler.
    int lastNumFrames = 8;

    // Resize the selection when the waveform count goes oldN -> newN, scaling its
    // length in proportion (newLen = oldLen * newN / oldN) so the per-waveform
    // window and slot spacing are preserved. Anchored at the selection start, the
    // grown selection is shifted left to fit the buffer if it would overrun the
    // end, and capped to the whole buffer. No-op if the buffer / selection is
    // empty. The window itself is count-independent, so only the selection moves.
    void resizeSelectionForCountChange(int oldN, int newN);

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

    // All sources: set the "Window length" slider to the current slot
    // spacing (regionLen / numFrames), so the window equals one slot and the
    // section bands tile the selection with zero gaps. Marks windowUserSet (an
    // explicit override). Invoked by the "Fit width to selection" button.
    void syncWindowToFitSlots();

    // All sources: while the window is in auto mode (!windowUserSet) set the
    // "Window length" slider to autoWindowMultiplier(mode) x grain,
    // clamped to the available audio - the per-method window default that gives
    // the cloud modes roam room (4x) and keeps the loop/FFT modes at 1x, exactly
    // like the editor's kWindowAutoPerMethod sentinel. No-op once the user has
    // overridden the window, and for a single waveform (its window is the whole
    // selection). Called whenever the grain length, freeze mode, or count
    // changes. Uses dontSendNotification so it never trips windowUserSet.
    void syncWindowToAuto();

    // All sources: enable/disable + relabel the "Window length" slider for
    // the current waveform count. With a single waveform there is no per-
    // waveform spacing to honour - the one window simply spans the whole
    // selection - so the slider is disabled and shown holding the selection
    // length (kept in sync as the selection is resized). With 2+ waveforms the
    // slider is the live per-waveform length control. Call wherever the count or
    // the region changes.
    void updateWindowLenControl();

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

    // ----- Zoom / scroll view window (File source only) -----
    // A long file shows the whole buffer mapped across waveRect, which makes a
    // useful selection a tiny sliver. To fix that the waveform view can be
    // zoomed in and scrolled: viewZoom 1.0 shows the whole buffer, higher values
    // show a proportionally smaller window, and viewScroll (0..1) positions that
    // window along the buffer. Live sources (Mic / Playback) keep zoom 1 / scroll
    // 0, so xForIdx/idxForX collapse to the whole-buffer map (no behaviour
    // change) and the two sliders are hidden - only File exposes the controls.
    double viewZoom   = 1.0;
    double viewScroll = 0.0;
    // Re-entrancy guard for reconfigureZoomForWindow(): its setRange() can clamp
    // the zoom value and fire zoomSlider.onValueChange -> onViewChanged ->
    // updateWindowLenControl -> reconfigureZoomForWindow again. The guard makes
    // the nested call a no-op so the chain terminates (same pattern as
    // syncingCrossfadeFromWindow).
    bool reconfiguringZoom = false;
    juce::Slider zoomSlider;
    juce::Label  zoomLabel;
    juce::Slider scrollSlider;
    juce::Label  scrollLabel;
    // Smallest window the zoom slider can shrink the view to, in samples - keeps
    // a zoomed-in view from collapsing to nothing on a short file.
    static constexpr int kMinViewSamples = 256;

    // File only: the zoom / scroll sliders are shown; other sources hide them.
    bool showsViewControls() const { return source == CaptureSource::File; }
    // Number of samples shown in waveRect / index of the first shown sample,
    // derived from viewZoom + viewScroll against the current buffer length.
    int  viewLenSamples()   const;
    int  viewStartSamples() const;
    // Smallest the view is ever allowed to shrink to, in samples: kMinViewSamples
    // normally, but at least one freeze WINDOW (windowFromSlider()) when there
    // are 2+ waveforms - so the user can never zoom in tighter than one captured
    // waveform spans. Capped to the buffer length. This is what guarantees the
    // per-waveform window always fits on screen (so the min-selection drag clamp
    // never needs a "window bigger than the view" escape hatch).
    int  minViewLenSamples() const;
    // Re-range the zoom slider so its maximum corresponds to minViewLenSamples()
    // (not a fixed floor), clamping the current zoom into the new range. Called
    // both on load and whenever the window changes (grain / mode / count / window
    // drag), so the slider's reach tracks the live window.
    void reconfigureZoomForWindow();
    // Re-range the zoom slider for the current buffer length and reset the view
    // to "whole buffer" (zoom 1 / scroll 0). Called when a file is loaded.
    void configureViewSlidersForBuffer();
    // Peg + cap the selection inside the current view window: a selection that
    // would fall off an edge is translated to sit flush against it, and one
    // longer than the visible window is shortened to it ("can't select more than
    // you can see"). Keeps the selection usable at every zoom level - this is
    // what makes the selection scroll with the audio yet stay pegged to a view
    // edge once it reaches one, and what scales it down to fit when zooming in.
    void clampSelectionToView();
    // After a zoom / scroll change: clamp the selection to the new window,
    // refresh dependent UI (region info, per-waveform slider, audition), repaint.
    void onViewChanged();

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
// the user transport-control it or scrub the selected region. The synth
// engine plays back the rendered PCM at full fidelity while Playing, and
// loops the Preview-selected region waveform while Paused or Scrubbing -
// so the user can audition the waveforms they're about to capture without
// having to commit to a Capture button blind.
//
// Differences from CaptureFromPlaybackDialog (Mic/File still use that):
//   - No live ring buffer; we render the project once on dialog open via
//     the same offline-render pattern doExportRender uses.
//   - The same region / N-waveform selection model as
//     CaptureFromPlaybackDialog: drag two handles to pick a span of the
//     rendered song, slice N equally-spaced waveforms across it (section
//     bands), pick a per-waveform freeze window, and Capture delivers all N
//     frames laid along the wavetable's Position dimension. The band geometry
//     is shared with the file dialog via captureBandStartForIndex().
//   - Transport states (Stopped / Playing / Paused / Scrubbing) drive
//     AudioEngine::setPreviewMode so the engine mixes the appropriate
//     stream into the master output: Playing sweeps the whole rendered song
//     PCM with a moving playhead; Paused / Scrubbing loops the
//     Preview-waveform-selected slice of the region (the grain audition) so
//     the user can hear what a captured waveform will sound like.
//   - Capture is disabled while Playing - the playhead is sweeping, so pause
//     or drag a handle to lock in the region first; the tooltip explains.
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
    // explicit "Capture waveforms" act because it depends on the region
    // selection, per-waveform window, and waveform count (capture-time
    // params), which is inherent to capture, not a save-model discrepancy. Crossfade is reported in MILLISECONDS
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
    // dragged handle keeps following the mouse instead of staying frozen.
    enum class TState { Stopped, Playing, Paused, Scrubbing };
    TState state = TState::Stopped;

    // Pre-rendered project PCM, mono, at the device sample rate so the
    // engine plays it back without any resampling step. Shared with the
    // engine via std::atomic<std::shared_ptr> so the engine can read the
    // latest pointer lock-free.
    std::shared_ptr<std::vector<float>> songPcm;
    double songSampleRate = 0.0;

    // Playhead position in samples within songPcm, used only while Playing:
    // it tracks the engine's published song-playback position so the moving
    // playhead can be drawn over the waveform. The capture spot itself is the
    // region (regionStart/regionEnd) below, not this.
    int64_t playheadSamplePos = 0;

    // Selected region in songPcm sample indices, inclusive-start /
    // exclusive-end (so a zero-width region is unambiguous). The capture
    // slices N waveforms across this span, exactly like
    // CaptureFromPlaybackDialog. Defaults to a short window at the song start
    // once the render completes; clamped into the current zoom/scroll view by
    // clampSelectionToView().
    int64_t regionStart = 0;
    int64_t regionEnd   = 0;
    // Currently-dragged handle: -1 none, 0 start handle, 1 end handle, 2 the
    // whole region (body drag). dragAnchorSampleOffset anchors a body drag so
    // the grabbed point doesn't jump under the mouse.
    int     dragHandle = -1;
    int64_t dragAnchorSampleOffset = 0;

    juce::TextButton playBtn  { "Play"  };
    juce::TextButton pauseBtn { "Pause" };
    juce::TextButton stopBtn  { "Stop"  };
    // Capture controls mirroring CaptureFromPlaybackDialog: how many waveforms
    // to slice across the region, the per-grain length (ms), the per-waveform
    // freeze WINDOW (ms), a one-click "fit window to the selection"
    // button, a "Preview waveform" picker choosing which slice the Paused
    // audition loops, an output Gain, and a region-geometry readout. See the
    // matching members in CaptureFromPlaybackDialog for the full contract; the
    // section-band geometry is shared via captureBandStartForIndex().
    juce::Slider     numFramesSlider;
    juce::Label      numFramesLabel;
    juce::Slider     grainLenSlider;
    juce::Label      grainLenLabel;
    juce::Slider     windowLenSlider;
    juce::Label      windowLenLabel;
    juce::TextButton fitWidthBtn { "Fit width to selection" };
    juce::Slider     previewIndexSlider;
    juce::Label      previewIndexLabel;
    juce::Slider     gainSlider;
    juce::Label      gainLabel;
    juce::Label      regionInfoLabel;
    // CrossfadeLoop seam crossfade length. Only meaningful when freeze
    // mode = CrossfadeLoop; left enabled in other modes so the user can
    // pre-set it before switching, but ignored by the engine for those
    // modes. Range / default in the cpp via kXfadeMinMs etc.
    juce::Slider     crossfadeSlider;
    juce::Label      crossfadeLabel;
    // The crossfade value the user picked, in ms, independent of the
    // slider's current visible value. The visible value gets clamped down
    // to half the freeze window whenever that window shrinks, but we keep the
    // user's intended value here so a later window increase restores the
    // original crossfade rather than leaving it stuck at the clamp. Updated
    // only when the user actually moves the crossfade slider; not updated by
    // syncCrossfadeMaxToWindow's defensive re-set of the slider value.
    double crossfadeDesiredMs = kXfadeDefMs;
    // Set while syncCrossfadeMaxToWindow is mutating crossfadeSlider's
    // range / value. JUCE's Slider::setRange fires onValueChange with
    // sendNotificationSync when its internal clamp shrinks the current
    // value, and we need to suppress that one path so the synthetic
    // clamped value doesn't get written back to crossfadeDesiredMs.
    bool syncingCrossfadeFromWindow = false;
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
    // into the produced GranularFrames (buildFrames) and drive the live
    // audition. Each is shown only in the modes that use it (refreshFreezeExtras),
    // with a tooltip explaining what it does, per the "grayed-out controls must
    // explain themselves" rule.
    juce::Label      grainCountLabel;
    juce::Slider     grainCountSlider;
    juce::Label      fftSizeLabel;
    juce::ComboBox   fftSizeCombo;
    // The freeze mode currently picked (defaults to CrossfadeLoop). Baked into
    // every produced GranularFrame and drives the live audition through the
    // shared GrainFreezeVoice. Mirrors CaptureFromPlaybackDialog.
    GranularFreezeMode selectedFreezeMode() const {
        const int idx = freezeModeCombo.getSelectedId() - 1;
        return (idx >= 0) ? (GranularFreezeMode)idx
                          : GranularFreezeMode::CrossfadeLoop;
    }
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
    // at 1:1 if the user doesn't touch the picker. buildFrames
    // reads this for each GranularFrame's embeddedPitchHz.
    double capturedPitchHz = 440.0;
    juce::TextButton saveBtn   { "Capture waveforms" };
    juce::TextButton cancelBtn { "Close" };

    juce::Rectangle<int> waveRect;

    // ----- Zoom / scroll view window -----
    // A long song shows its whole PCM mapped across waveRect, making precise
    // region placement hard. The waveform view can be zoomed in and scrolled:
    // viewZoom 1.0 shows the whole song, higher values show a proportionally
    // smaller window, and viewScroll (0..1) positions that window along the
    // song. The region scrolls with the audio yet is pegged to a view edge once
    // it reaches one (clampSelectionToView), mirroring the file-capture dialog.
    double viewZoom   = 1.0;
    double viewScroll = 0.0;
    // Re-entrancy guard for reconfigureZoomForWindow() (see the file dialog's
    // member for the full rationale).
    bool reconfiguringZoom = false;
    juce::Slider zoomSlider;
    juce::Label  zoomLabel;
    juce::Slider scrollSlider;
    juce::Label  scrollLabel;
    static constexpr int kMinViewSamples = 256;

    int64_t viewLenSamples()   const;
    int64_t viewStartSamples() const;
    // Smallest the view may shrink to, in samples: kMinViewSamples normally, but
    // at least one freeze WINDOW (windowFromSlider()) with 2+ waveforms - so the
    // user can never zoom in tighter than one captured waveform spans. Capped to
    // the song length. Mirrors CaptureFromPlaybackDialog::minViewLenSamples.
    int64_t minViewLenSamples() const;
    // Re-range the zoom slider so its maximum corresponds to minViewLenSamples(),
    // clamping the current zoom into the new range. Called on render-complete and
    // whenever the window changes (grain / mode / count / window drag).
    void reconfigureZoomForWindow();
    // Re-range the zoom slider for the rendered song length and reset the view
    // to "whole song" (zoom 1 / scroll 0). Called once the render completes.
    void configureViewSlidersForBuffer();
    // Peg + cap the selected region inside the current view window so the
    // capture spot stays visible/usable after a zoom / scroll: a region that
    // would fall off an edge is translated flush against it, and one longer
    // than the visible window is shortened to it ("can't select more than you
    // can see"). Mirrors CaptureFromPlaybackDialog::clampSelectionToView. Not
    // applied while Playing (the playhead genuinely moves through the song).
    void clampSelectionToView();
    // After a zoom / scroll change: peg the region, refresh status/region info,
    // re-anchor the audition, repaint.
    void onViewChanged();

    // Background render job. Started in the constructor; the dialog
    // shows "Rendering..." until job->done == true and job->result has
    // been moved into songPcm. Owned by the dialog so destruction
    // cancels the render cleanly.
    class RenderJob;
    std::unique_ptr<RenderJob> renderJob;
    bool renderReady = false;

    void onRenderComplete();
    // (Re)publish the Preview-waveform-selected slice of the region as the
    // engine's GrainLoop audition source. Mirrors
    // CaptureFromPlaybackDialog::regenerateAuditionGrain - same banded source
    // (window + seam lookahead), grain, per-method window, and output gain the
    // captured frame will carry - so the Paused/Scrubbing audition is honest.
    void regenerateAuditionGrain();
    // Publish the GrainLoop playback pitch ratio to the audio engine so the
    // live region audition is pitched to match what the synth voice will
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
    // so a later "Capture waveforms" preserves the frame's density /
    // resolution instead of resetting it to the dialog defaults.
    void seedFromExistingFrame(double pitchHz, int freezeModeIdx,
                               double crossfadeMs, int grainCount, int fftSize);

private:
    void updateButtonsForState();
    void updateStatusLabel();
    // Keeps the crossfade slider's range capped at half the current freeze
    // window (effectiveSrcLen / 2, in ms). Mirrors the engine's
    // `xfade = min(xfadeReq, window/2)` clamp in the UI so the slider can't
    // visibly point at a value the engine will quietly ignore. Snaps the
    // current value down if it exceeds the new max; restores the user's intent
    // (crossfadeDesiredMs) when the window grows back. Guards re-entrancy via
    // syncingCrossfadeFromWindow.
    void syncCrossfadeMaxToWindow();
    int     xForSamplePos(int64_t pos) const;
    int64_t samplePosForX(int x) const;

    // ----- Region / per-waveform window model (shared with the file dialog) --
    // These mirror CaptureFromPlaybackDialog's identically-named methods, in
    // int64 sample space over songPcm instead of int over `tap`. The subtle
    // band-layout geometry is shared via the file-scope captureBandStartForIndex
    // helper so the two dialogs lay out their section bands the same way.

    // Per-waveform freeze-WINDOW length in samples. Single waveform: the whole
    // selection; 2+: the "Window length" slider (auto-tracks
    // autoWindowMultiplier(mode) x grain until overridden). Floored at the grain
    // (cloud modes) or 256, and capped to the selection so bands never spill
    // past the handles.
    int64_t effectiveSrcLen() const;

    // The window length the slider asks for, in samples (ms slider value
    // converted against the song rate), floored at the per-mode minimum and
    // capped to the buffer length but NOT capped to the current selection.
    int64_t windowFromSlider() const;

    // Minimum selection length a resize drag may reach, in samples. 0 for a
    // single waveform; otherwise the requested window, capped to the visible
    // window length when the view is zoomed in tighter than that window.
    int64_t minSelectionLen() const;

    // Push the dragged handle back out if a resize drag shrank the selection
    // below minSelectionLen(), falling back to the buffer boundary if needed.
    void    enforceMinSelectionDuringDrag();

    // Grain length in samples (the "Grain length (ms)" slider against the song
    // rate, floored at 64).
    int     effectiveGrainLen() const;
    // True once the user explicitly set the per-waveform window (dragged the
    // "Window length" slider or pressed "Fit width"). While false the
    // window auto-tracks autoWindowMultiplier(mode) x grain (syncWindowToAuto).
    bool    windowUserSet = false;
    // True once the user manually moved the Grain-length slider (see the file
    // dialog's grainUserSet for the full contract). While false the grain snaps
    // to the freeze mode's default (defaultGrainMsForMode) on a mode change.
    bool    grainUserSet = false;
    // The waveform count the selection was last sized for; the count handler
    // resizes the selection in proportion when this changes so the per-waveform
    // window stays constant (resizeSelectionForCountChange). Mirrors the file
    // dialog. Seeded to the initial slider value.
    int     lastNumFrames = 8;
    // Resize the selection when the waveform count goes oldN -> newN, scaling its
    // length in proportion so each waveform's window length and slot spacing are
    // preserved (int64 sample space over songPcm). Anchored at the region start,
    // shifted left to fit / capped to the song length. Mirrors the file dialog.
    void    resizeSelectionForCountChange(int oldN, int newN);
    // Window start index for waveform i of n given window length srcLen, via the
    // shared captureBandStartForIndex geometry (FIT -> n+1 equal gaps; OVERLAP
    // -> contained within the selection). Clamped to [0, songLen - srcLen].
    int64_t bandStartForIndex(int i, int n, int64_t srcLen) const;
    // Build a granular source buffer: the windowLen-sample band body starting at
    // startIdx in songPcm, plus a windowLen/2 lookahead tail for the crossfade
    // seam (zero-padded past the song end).
    std::vector<float> buildGrainSource(int64_t startIdx, int64_t windowLen) const;
    void syncWindowToFitSlots();
    void syncWindowToAuto();
    void updateWindowLenControl();
    // Set the "Preview waveform" slider range (1..numFrames) + enabled state for
    // the current count, clamping the current pick into range. Disabled at a
    // single waveform.
    void updatePreviewIndexControl();
    void updateRegionInfoLabel();
    // The interactive zone for the two region handles: waveRect widened by the
    // handle hit radius on the left and right. Drawing + hit-testing + repaint
    // all use it so an edge handle whose outer half is past the wave-view edge
    // is still visible and grabbable.
    juce::Rectangle<int> handleZone() const;
    static constexpr int kHandleHitRadius = 8;

    // Build the N GranularFrames the Capture button delivers - one banded
    // GranularFrame per equally-spaced waveform across the region, exactly the
    // geometry the section bands draw and the audition plays. Replaces the old
    // single-marker buildFrameAtMarker(); the wave editor's on-capture path
    // already handles N frames (append) or frames[0] (replace).
    std::vector<std::unique_ptr<IWavetableFrame>> buildFrames(int n) const;

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

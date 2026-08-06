#include "capture_from_playback.h"
#include "audio_engine.h"
#include "audio_cache.h"
#include "dialog_helpers.h"
#include "granular_frame.h"
#include "graph_processor.h"
#include "node_graph.h"
#include "transport.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cmath>

namespace SoundShop {

// How much recent audio we display from the live ring buffers, in samples
// at the tap's rate. The tap itself holds ~2M samples (~43 s at 48 kHz);
// we only render the most recent kDisplayWindowSamples to keep the
// per-pixel sample count low and the display readable. A bigger window
// than this would average too many samples per pixel for the user to
// pick a useful region. File mode loads the whole file regardless of
// this constant.
static constexpr int kDisplayWindowSamples = 1 << 19;  // ~524 k samples, ~11 s at 48 kHz

// Per-waveform freeze-window length slider range, in MILLISECONDS (both capture
// dialogs). Expressed in ms - not samples - so it sits in the same unit as the
// Grain-length and Crossfade sliders, making the window:grain ratio (e.g. "the
// window is 4x the grain") easy to read off directly. ms is also sample-rate
// independent, so the same value means the same duration across 44.1/48/96 kHz
// sources. The real lower bound a window can take is enforced per-mode in
// effectiveSrcLen() (a grain must fit the cloud modes; 256 samples for the
// others); kWindowMinMs is just the slider's UI floor. The max (~12 s) is
// generous - longer than any per-waveform window a user would reasonably pick -
// and the geometry caps the effective window to the selection anyway.
static constexpr double kWindowMinMs = 1.0;
static constexpr double kWindowMaxMs = 12000.0;

// Cap on how many samples we accept from a loaded file. Long files
// (e.g. a whole song) work fine but you'd be averaging tens of
// thousands of samples per display pixel - the user can't see anything
// meaningful at that density. ~60 s at 96 kHz is a reasonable upper
// bound for a wavetable source. We accept the leading window and warn
// in the status label if we truncated.
static constexpr int kMaxFileSamples = 96000 * 60;

static const char* sourceName(CaptureSource s) {
    switch (s) {
        case CaptureSource::Playback: return "recent project playback";
        case CaptureSource::Mic:      return "microphone / audio input";
        case CaptureSource::File:     return "audio file";
    }
    return "?";
}

// Shared band-layout geometry for the capture dialogs' "N waveforms across a
// selection" model. Given a region [regionStart, regionEnd), the waveform index
// i of n, and the per-waveform window length srcLen, returns the start sample of
// band i. Two regimes meeting continuously where the windows exactly tile the
// selection (freeSpace == 0):
//   * WINDOWS FIT (freeSpace >= 0): the leftover space is split into n+1 EQUAL
//     gaps - one before the first band, one between each adjacent pair, one
//     after the last - so the end margins equal the inter-band gaps.
//        gap     = freeSpace / (n + 1)
//        start_i = regionStart + (i + 1)*gap + i*srcLen
//   * WINDOWS OVERLAP (freeSpace < 0): the windows are wider than their share,
//     so they must overlap; rather than spill past the handles we keep the row
//     CONTAINED - band 0 flush left, band n-1 flush right, overlap distributed.
//        step    = (regLen - srcLen) / (n - 1)
//        start_i = regionStart + i*step
//   * n == 1 overlap: a lone window wider than the selection can't be contained;
//     centre it.
// The result is clamped to [0, maxStart] (the available audio). Callers cap
// srcLen to the selection length via effectiveSrcLen(), so the bands never spill
// past the handles. int64 throughout so it serves both the int-indexed file
// dialog (tap up to ~524 k samples) and the int64-indexed song dialog (a whole
// rendered song). Used by CaptureFromPlaybackDialog AND CaptureFromSongDialog so
// the band geometry stays byte-for-byte identical between file and song capture.
static int64_t captureBandStartForIndex(int64_t regionStart, int64_t regionEnd,
                                        int i, int n, int64_t srcLen,
                                        int64_t maxStart) {
    if (n <= 0 || srcLen <= 0) return regionStart;
    const int64_t regLen = std::max<int64_t>(0, regionEnd - regionStart);
    const double freeSpace = (double)regLen - (double)n * (double)srcLen;
    double startD;
    if (freeSpace >= 0.0) {
        const double gap = freeSpace / (double)(n + 1);
        startD = (double)regionStart + (double)(i + 1) * gap
               + (double)i * (double)srcLen;
    } else if (n == 1) {
        startD = (double)regionStart + 0.5 * ((double)regLen - (double)srcLen);
    } else {
        const double step = (double)(regLen - srcLen) / (double)(n - 1);
        startD = (double)regionStart + (double)i * step;
    }
    int64_t startIdx = (int64_t)std::llround(startD);
    startIdx = juce::jlimit<int64_t>(0, std::max<int64_t>(0, maxStart), startIdx);
    return startIdx;
}

// Sticky capture-dialog settings, remembered across opens within a session.
// The dialog seeds its controls from this on construction and writes the
// current values back on destruction, so re-opening the mic/file/playback
// capture dialog restores the user's last waveform count, gain, and labelled
// pitch instead of resetting to defaults every time. Process-global (one
// shared set for all three sources, since they share these controls);
// deliberately NOT persisted to disk - it's a session convenience, not a
// project/preference value. The per-waveform "Window length" (ms) is
// intentionally excluded: it auto-fits to the selection/count on every open
// (syncWindowToFitSlots), so a remembered value would just be overwritten and
// fighting that would be confusing.
struct CaptureDialogPrefs {
    int    numFrames = 8;       // numFramesSlider
    double gain      = 1.0;     // gainSlider
    int    noteId    = 9 + 1;   // noteCombo selected id (A)
    int    octaveId  = 4 + 1;   // octaveCombo selected id (4)
    double grainMs   = 5.0;     // grainLenSlider (ms) - short by default so the
                                // grain-cloud modes (Async / Pitch-sync) sound
                                // like a smooth, CONSTANT cloud out of the box.
                                // The non-grain modes ignore it (their window is
                                // kNonGrainAutoWindowSamples, not grain-derived).
    bool   grainUserSet = false;// true once the user manually moved the grain
                                // slider. While false the grain snaps to each
                                // freeze mode's default (defaultGrainMsForMode:
                                // 5 ms Async / 40 ms Pitch-sync) on a mode change;
                                // once the user picks a grain we stop snapping and
                                // remember their choice across opens.
    double crossfadeMs = 50.0;  // crossfadeSlider desired ms (= kXfadeDefMs)
};
static CaptureDialogPrefs& captureDialogPrefs() {
    static CaptureDialogPrefs p;
    return p;
}

// ---- Note/Octave <-> Hz helpers (12-TET, A4 = 440 Hz, scientific
//      pitch notation: C4 = middle C, MIDI 60; A4 = MIDI 69).
//
// Hard-coded to 12-TET rather than going through the project's tuning
// system: embeddedPitchHz describes the recording itself, not the
// project's tuning preference, and the user picking "A4" wants 440 Hz
// regardless of what tuning the project later uses for playback.
// Mirrors the helpers in layered_wave_editor.cpp's
// GranularFrameEditorComponent so the capture-time picker and the
// post-capture editor agree on every Hz/note conversion.
static int noteOctaveToMidi(int note, int octave) {
    return 12 * (octave + 1) + note;  // C0 = 12, A4 = 69
}
static double midiToHz(int midi) {
    return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
}
static double noteOctaveToHz(int note, int octave) {
    return midiToHz(noteOctaveToMidi(note, octave));
}
// Returns (note 0..11, octave 0..9) of the nearest MIDI note to hz.
static std::pair<int, int> hzToNearestNoteOctave(double hz) {
    if (hz <= 0.0) return {9, 4};  // safe default = A4
    int midi = (int)std::lround(69.0 + 12.0 * std::log2(hz / 440.0));
    midi = std::clamp(midi, 12, 12 * 10 + 11);  // C0 .. B9
    const int octave = midi / 12 - 1;
    const int note = midi % 12;
    return {note, octave};
}
// Cents offset of hz from the nearest 12-TET note (-49 .. +50).
static int hzToCentsFromNearest(double hz) {
    if (hz <= 0.0) return 0;
    const double midiExact   = 69.0 + 12.0 * std::log2(hz / 440.0);
    const double midiNearest = std::round(midiExact);
    return (int)std::lround((midiExact - midiNearest) * 100.0);
}
static juce::String formatCents(double hz) {
    const int cents = hzToCentsFromNearest(hz);
    juce::String t;
    if (cents > 0) t << "+";
    t << cents << " \xC2\xA2";  // UTF-8 cent sign
    return t;
}
// Note + octave + cents row builder. Shared by both capture dialogs.
// Caller supplies the three controls already-allocated; we addAndMakeVisible
// them, populate the combos, wire tooltips, and set the initial selection
// to match A4 (440 Hz).
static void setUpPitchPicker(juce::Component& parent,
                             juce::Label& noteOctaveLabel,
                             juce::ComboBox& noteCombo,
                             juce::ComboBox& octaveCombo,
                             juce::Label& centsLabel) {
    parent.addAndMakeVisible(noteOctaveLabel);
    noteOctaveLabel.setText("As note", juce::dontSendNotification);
    noteOctaveLabel.setColour(juce::Label::textColourId,
                              juce::Colours::white.withAlpha(0.85f));
    noteOctaveLabel.setFont(juce::Font(juce::FontOptions(11.0f)));

    parent.addAndMakeVisible(noteCombo);
    const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F",
                                  "F#", "G", "G#", "A", "A#", "B"};
    for (int i = 0; i < 12; ++i)
        noteCombo.addItem(kNoteNames[i], i + 1);
    noteCombo.setTooltip(
        "Pitch class to label the captured source with. The synth uses "
        "this so MIDI playback at the matching key plays the source at "
        "1:1, with the usual wavetable-style pitch shift for other keys. "
        "Pair with Octave to set the exact pitch.");

    parent.addAndMakeVisible(octaveCombo);
    for (int o = 0; o <= 9; ++o)
        octaveCombo.addItem(juce::String(o), o + 1);
    octaveCombo.setTooltip(
        "Octave to label the captured source with, using scientific "
        "pitch notation (A4 = 440 Hz, middle C = C4). Combined with "
        "Note this sets the captured waveform's embedded pitch so MIDI "
        "playback tracks correctly.");

    parent.addAndMakeVisible(centsLabel);
    centsLabel.setColour(juce::Label::textColourId,
                         juce::Colours::white.withAlpha(0.7f));
    centsLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    centsLabel.setJustificationType(juce::Justification::centredLeft);
    centsLabel.setText("0 \xC2\xA2", juce::dontSendNotification);
    centsLabel.setTooltip(
        "How far the labelled pitch is from the nearest 12-TET note in "
        "cents. Always 0 from this picker (it produces exact notes); "
        "the post-capture editor's Hz slider can place the pitch "
        "between notes if you need it.");

    // Default to A4 = 440 Hz so MIDI note 69 plays at 1:1 when the user
    // doesn't touch the picker. Matches the GranularFrame default.
    noteCombo.setSelectedId(9 + 1, juce::dontSendNotification);   // A
    octaveCombo.setSelectedId(4 + 1, juce::dontSendNotification); // 4
}

// Drop-arrow box + side padding + visual cushion + a min-width floor so
// single-char items don't render as a tiny arrow-only pill. Same shape
// as the helper in layered_wave_editor.cpp so the two capture-related
// UIs agree on combo proportions.
static int intrinsicComboWidth(juce::ComboBox& cb) {
    const juce::Font f = cb.getLookAndFeel().getComboBoxFont(cb);
    int maxText = 0;
    for (int i = 0; i < cb.getNumItems(); ++i)
        maxText = std::max(maxText,
            juce::GlyphArrangement::getStringWidthInt(f, cb.getItemText(i)));
    return std::max(80, maxText + 54);
}

CaptureFromPlaybackDialog::CaptureFromPlaybackDialog(CaptureSource src, int ts, OnCapture onCap)
    : onCapture(std::move(onCap)),
      source(src),
      tableSize(std::max(64, ts)) {
    // Height bumped to make room for the embedded pitch row, the freeze-mode
    // row, and the grain-length + freeze-window + crossfade rows underneath the
    // num-frames slider while keeping the waveform area unchanged. The File
    // source adds a zoom / scroll row under the waveform, so it gets a little
    // extra height to keep the wave area the same size as the other sources.
    setSize(720, src == CaptureSource::File ? 640 : 606);

    addAndMakeVisible(numFramesLabel);
    numFramesLabel.setText("Waveforms to slice out:", juce::dontSendNotification);
    numFramesLabel.setJustificationType(juce::Justification::centredRight);

    addAndMakeVisible(numFramesSlider);
    numFramesSlider.setRange(1.0, 32.0, 1.0);
    // Seed from the sticky session prefs so re-opening the dialog restores the
    // last waveform count (see captureDialogPrefs()).
    numFramesSlider.setValue((double)captureDialogPrefs().numFrames,
                             juce::dontSendNotification);
    lastNumFrames = (int)std::round(numFramesSlider.getValue());
    numFramesSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    numFramesSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
    numFramesSlider.setTooltip(
        "Slices the selected region into this many short snapshots. The "
        "wavetable morphs through them as its Position parameter sweeps from "
        "0 to 1, so the captured sound evolves the way the source did. "
        "1 = a single frozen snapshot; more = smoother evolution but more "
        "memory. Changing this resizes the SELECTION in proportion (keeping each "
        "waveform's length fixed), so adding waveforms grows the selection rather "
        "than cramming more into the same span.");
    numFramesSlider.onValueChange = [this]() {
        const int newN = (int)std::round(numFramesSlider.getValue());
        // Resize the selection in proportion to the count so each waveform's
        // window length (and slot spacing) stays constant - the user explicitly
        // asked that adding waveforms grow the selection rather than shrink the
        // per-waveform length. The window itself is count-independent, so only
        // the selection moves here.
        resizeSelectionForCountChange(lastNumFrames, newN);
        lastNumFrames = newN;
        // Keep the (possibly grown) selection inside the current zoom view.
        clampSelectionToView();
        // Re-apply the per-method auto window for the new count (no-op once the
        // user has overridden the window). The window stays a fixed sample count
        // across count changes when explicitly set, matching the editor.
        syncWindowToAuto();
        // Enable / disable + relabel the per-waveform slider for the new count
        // (disabled at 1 waveform, where the window just spans the selection).
        updateWindowLenControl();
        // The selection / window may have changed, so re-range the zoom reach.
        reconfigureZoomForWindow();
        // Re-range + clamp the "Preview waveform" picker to the new count.
        updatePreviewIndexControl();
        updateRegionInfoLabel();
        if (auditioning) regenerateAuditionGrain();
        repaint(waveRect);
    };

    // Grain length (all sources), in milliseconds. The size of each overlapping
    // Hann grain the cloud modes scatter; inert for CrossfadeLoop / SpectralFreeze
    // (greyed by refreshFreezeExtras). Mirrors the wave editor's grain control so
    // capture and editor agree. Changing it re-derives the auto window
    // (autoWindowMultiplier x grain) unless the user has overridden the window.
    addAndMakeVisible(grainLenLabel);
    grainLenLabel.setText("Grain length (ms):", juce::dontSendNotification);
    grainLenLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(grainLenSlider);
    grainLenSlider.setRange(1.0, 500.0, 0.5);  // sub-5 ms allowed: very short
                                               // grains give the smoothest,
                                               // most constant cloud
    grainLenSlider.setValue(captureDialogPrefs().grainMs, juce::dontSendNotification);
    grainUserSet = captureDialogPrefs().grainUserSet;
    grainLenSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    grainLenSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
    grainLenSlider.setTooltip(
        "Length of each overlapping grain inside a captured waveform, in "
        "milliseconds. Each grain-cloud mode opens at the grain that sounds most "
        "CONSTANT for it - ~5 ms for Async granular (many tiny grains blur into a "
        "steady cloud), ~40 ms for Pitch-sync grains (needs a few pitch periods "
        "per grain to lock the pitch). Longer grains roam over a proportionally "
        "wider window and so sound more evolving / less constant; very short "
        "(~1 ms) gets buzzy. Once you move this slider it stays where you put it "
        "(no more auto-snapping on mode change). The freeze window (\"Window "
        "length\" below) auto-tracks 4x the grain for the grain-cloud modes so "
        "they have room to roam, until you drag the window yourself. Only Async "
        "and Pitch-sync grains use the grain; Crossfade loop and Spectral freeze "
        "ignore it and use a fixed default window instead.");
    grainLenSlider.onValueChange = [this]() {
        // A manual move pins the grain: stop snapping it to the per-mode default
        // on future freeze-mode changes, and remember the choice across opens.
        grainUserSet = true;
        // Re-derive the auto window (mult x grain) unless the user overrode it,
        // then refresh the displayed window control + bands + audition.
        syncWindowToAuto();
        updateWindowLenControl();
        updateRegionInfoLabel();
        if (auditioning) regenerateAuditionGrain();
        repaint(waveRect);
    };

    // Per-waveform freeze-WINDOW length (all sources), in MILLISECONDS (same
    // unit as Grain length / Crossfade, so the window:grain ratio reads off
    // directly). Auto-tracks autoWindowMultiplier(mode) x grain (see
    // syncWindowToAuto) until the user drags it; with a single waveform it spans
    // the whole selection (disabled).
    addAndMakeVisible(windowLenLabel);
    windowLenLabel.setText("Window length (ms):", juce::dontSendNotification);
    windowLenLabel.setJustificationType(juce::Justification::centredRight);

    addAndMakeVisible(windowLenSlider);
    // Range in ms (kWindowMinMs..kWindowMaxMs). The slider is just the requested
    // window; effectiveSrcLen() converts it to samples against the source rate
    // and caps it to the selection so the bands stay contained. Step 1 ms.
    windowLenSlider.setRange(kWindowMinMs, kWindowMaxMs, 1.0);
    windowLenSlider.setValue(1000.0, juce::dontSendNotification);
    windowLenSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    windowLenSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 22);
    // Tooltip + enabled state are set by updateWindowLenControl().
    // A user drag flips the window into explicit (overridden) mode.
    windowLenSlider.onValueChange = [this]() {
        windowUserSet = true;   // user override - stop auto-tracking the grain
        syncCrossfadeMaxToWindow();   // the crossfade cap tracks the new window
        // A wider window means the view can't zoom in as far - pull the zoom
        // reach (and any over-zoomed view) back in step with the new window.
        reconfigureZoomForWindow();
        updateRegionInfoLabel();
        if (auditioning) regenerateAuditionGrain();
        repaint(waveRect);
    };

    addAndMakeVisible(fitWidthBtn);
    // Tooltip + enabled state for both this button and the slider are set by
    // updateWindowLenControl() (called below), which disables both
    // at a single waveform.
    fitWidthBtn.onClick = [this]() {
        // syncWindowToFitSlots writes the slider with dontSendNotification (so it
        // can be reused without recursing) and marks windowUserSet, so refresh
        // the dependent state here the way onValueChange would.
        syncWindowToFitSlots();
        syncCrossfadeMaxToWindow();   // the crossfade cap tracks the new window
        reconfigureZoomForWindow();   // Fit can grow the window -> re-range zoom
        updateRegionInfoLabel();
        if (auditioning) regenerateAuditionGrain();
        repaint(waveRect);
    };

    syncWindowToAuto();
    updateWindowLenControl();

    addAndMakeVisible(regionInfoLabel);
    regionInfoLabel.setJustificationType(juce::Justification::centredLeft);
    regionInfoLabel.setFont(juce::Font(juce::FontOptions(13.0f)));
    regionInfoLabel.setColour(juce::Label::textColourId, juce::Colour(180, 180, 200));

    addAndMakeVisible(sourceInfoLabel);
    sourceInfoLabel.setJustificationType(juce::Justification::centredLeft);
    sourceInfoLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    sourceInfoLabel.setColour(juce::Label::textColourId, juce::Colour(150, 150, 170));

    addAndMakeVisible(hintLabel);
    {
        juce::String hint;
        switch (source) {
            case CaptureSource::Playback:
                hint = "Play the project, then drag the orange handles to select a region. "
                       "Each captured waveform takes a short window at its position; the "
                       "wavetable's Position parameter sweeps through them.";
                break;
            case CaptureSource::Mic:
                hint = "Make a sound into your audio input device, then drag the orange "
                       "handles to select a region. Each captured waveform takes a short "
                       "window at its position; the wavetable's Position parameter "
                       "sweeps through them.\n"
                       "If the input still sounds wrong (garbled, noisy, or like your "
                       "computer's own audio), click \"Audio device...\" and switch the "
                       "driver type to DirectSound as a last resort.";
                break;
            case CaptureSource::File:
                hint = "Load an audio file, then drag the orange handles to select the "
                       "portion to slice into waveforms. Each captured waveform takes a short "
                       "window at its position; the wavetable's Position parameter "
                       "sweeps through them.";
                break;
        }
        hintLabel.setText(hint, juce::dontSendNotification);
    }
    hintLabel.setJustificationType(juce::Justification::topLeft);
    hintLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    hintLabel.setColour(juce::Label::textColourId, juce::Colour(160, 160, 180));

    // Live-source controls. Playback keeps the simple "Pause view" checkbox;
    // Mic gets a prominent Pause/Go-live button that also drives input
    // monitoring (so the user hears the mic) - set up below.
    if (source == CaptureSource::Playback) {
        addAndMakeVisible(pauseToggle);
        pauseToggle.setTooltip(
            "Pause the auto-refresh so the waveform display stays still while you "
            "drag the region handles. Turn it off to see new audio.");
    } else if (source == CaptureSource::Mic) {
        addAndMakeVisible(micLiveBtn);
        micLiveBtn.setColour(juce::TextButton::buttonColourId,
                             juce::Colour(150, 90, 40)); // amber-ish = live/active
        micLiveBtn.onClick = [this]() { setMicLive(micPaused); };

        // Last-resort escape hatch. The default Windows Audio (WASAPI shared
        // mode) driver normally resamples a webcam/USB mic correctly via its
        // per-endpoint AUTOCONVERTPCM SRC, so the mic should capture fine. If a
        // particular device's format still can't be reconciled, opening Audio
        // Device Settings lets the user switch the driver type to DirectSound.
        // See the note appended to the Mic hint text.
        addAndMakeVisible(audioDeviceBtn);
        audioDeviceBtn.setTooltip(
            "Open Audio Device Settings. The default Windows Audio driver should "
            "capture your mic correctly. If it still sounds wrong (garbled, noisy, "
            "or like your computer's own audio), change the driver type to "
            "DirectSound here as a last resort.");
        audioDeviceBtn.onClick = [this]() {
            if (auto* eng = AudioEngine::getInstance())
                if (auto* dm = eng->getDeviceManager())
                    SoundShop::launchAudioDeviceSettings(*dm, this);
        };

        // Enable input monitoring so the mic is audible immediately, saving
        // the engine's prior global state so we can restore it on close.
        // Also make sure the device actually has a live input channel open -
        // a mic plugged in after launch (or a Windows default-device mismatch)
        // can leave input disabled, so the dialog would show no activity. This
        // restarts the device only if input is currently off.
        if (auto* eng = AudioEngine::getInstance()) {
            priorInputMonitoring = eng->inputMonitoring.load();
            eng->ensureAudioInputEnabled();
        }
    }

    // File-source controls.
    if (source == CaptureSource::File) {
        addAndMakeVisible(loadFileBtn);
        loadFileBtn.setTooltip("Pick a .wav/.mp3/.aiff/.flac/.ogg file. The whole "
                                "file is decoded and shown; drag handles to pick a "
                                "region inside it.");
        loadFileBtn.onClick = [this]() { chooseAndLoadFile(); };
    }

    // Embedded pitch picker. Lets the user label the capture with the
    // pitch it represents (A4 = 440 Hz default) so MIDI playback at the
    // matching key plays the source at 1:1; other keys pitch-shift via
    // the synth's usual wavetable stride math. Mirrors the picker in the
    // post-capture GranularFrameEditorComponent.
    setUpPitchPicker(*this, noteOctaveLabel, noteCombo, octaveCombo, centsLabel);
    auto onPitchPickerChanged = [this]() {
        const int n = noteCombo.getSelectedId() - 1;
        const int o = octaveCombo.getSelectedId() - 1;
        if (n < 0 || n > 11 || o < 0 || o > 9) return;
        capturedPitchHz = noteOctaveToHz(n, o);
        centsLabel.setText(formatCents(capturedPitchHz),
                           juce::dontSendNotification);
        // Keep a running audition pitched to the freshly-labelled note.
        if (auditioning) regenerateAuditionGrain();
        // Re-capture / unified save: commit the pitch change to the bound frame.
        publishMetadataEdit();
    };
    noteCombo.onChange   = onPitchPickerChanged;
    octaveCombo.onChange = onPitchPickerChanged;
    // Restore the last labelled pitch from the sticky session prefs (overrides
    // setUpPitchPicker's A4 default), then recompute the derived Hz / cents.
    noteCombo.setSelectedId(captureDialogPrefs().noteId, juce::dontSendNotification);
    octaveCombo.setSelectedId(captureDialogPrefs().octaveId, juce::dontSendNotification);
    onPitchPickerChanged();

    // Granular freeze-mode picker (all three sources). Selects which "sustain
    // the spot" algorithm a held note uses; the same shared GrainFreezeVoice
    // backs this audition and the synth, so what you A/B here is what you play.
    addAndMakeVisible(freezeModeLabel);
    freezeModeLabel.setText("Freeze:", juce::dontSendNotification);
    freezeModeLabel.setJustificationType(juce::Justification::centredRight);

    addAndMakeVisible(freezeModeCombo);
    // IDs are 1-based; ID = (int)mode + 1. All four are implemented in the
    // shared granular_freeze.cpp.
    freezeModeCombo.addItem("Crossfade loop",
                            (int)GranularFreezeMode::CrossfadeLoop + 1);
    freezeModeCombo.addItem("Async granular (blur)",
                            (int)GranularFreezeMode::AsyncGranular + 1);
    freezeModeCombo.addItem("Pitch-sync grains",
                            (int)GranularFreezeMode::PitchSyncGrains + 1);
    freezeModeCombo.addItem("Spectral freeze",
                            (int)GranularFreezeMode::SpectralFreeze + 1);
    freezeModeCombo.addItem("Single cycle",
                            (int)GranularFreezeMode::SingleCycle + 1);
    freezeModeCombo.setSelectedId(
        (int)GranularFreezeMode::CrossfadeLoop + 1, juce::dontSendNotification);
    freezeModeCombo.setTooltip(
        "How a held note sustains the captured spot. The source pitch is baked "
        "in; held notes resample the result to your MIDI pitch.\n"
        " - Crossfade loop: faithful tape loop with a short fade across the "
        "seam. \"What does this spot literally sound like.\"\n"
        " - Async granular: many short grains at randomised positions. Frozen "
        "blur / GRM-Freeze texture.\n"
        " - Pitch-sync grains: loops exactly one detected pitch period for a "
        "clean sustained tone. Works best on a pitched source.\n"
        " - Spectral freeze: keeps the FFT magnitudes and regenerates phases "
        "each frame. Ethereal pad sustain.\n"
        " - Single cycle: detects one pitch period and loops just that single "
        "cycle - a static single-cycle-oscillator tone (fixed-timbre "
        "counterpart to Pitch-sync grains' living cloud).\n"
        "Drives the Preview you're hearing, so you can A/B before capturing.");
    freezeModeCombo.onChange = [this]() {
        const int idx = freezeModeCombo.getSelectedId() - 1;
        if (idx < 0) return;
        if (auto* eng = AudioEngine::getInstance())
            eng->setGrainFreezeMode((AudioEngine::GrainFreezeMode)idx);
        // Snap the grain to the new mode's default (5 ms Async / 40 ms Pitch-sync)
        // so each grain-cloud mode opens at a grain that sounds constant - unless
        // the user has already picked a grain (grainUserSet), in which case we
        // respect their value. Only the grain-cloud modes use the grain, so don't
        // disturb it when switching to a non-grain mode (Crossfade / Spectral).
        // Done before syncWindowToAuto so the auto window tracks the new grain.
        const GranularFreezeMode newMode = selectedFreezeMode();
        if (!grainUserSet
            && (newMode == GranularFreezeMode::AsyncGranular
                || newMode == GranularFreezeMode::PitchSyncGrains)) {
            grainLenSlider.setValue(defaultGrainMsForMode(newMode),
                                    juce::dontSendNotification);
        }
        // The grain count / FFT size relevance changes with the mode.
        refreshFreezeExtras();
        // The per-method auto window multiplier changes with the mode (4x for
        // the cloud modes, 1x otherwise), so re-derive the auto window unless
        // the user has overridden it, then refresh the displayed control + bands.
        syncWindowToAuto();
        updateWindowLenControl();
        updateRegionInfoLabel();
        repaint(waveRect);
        // Re-spin the audition so the new mode re-anchors on the current slice.
        if (auditioning) regenerateAuditionGrain();
        // Re-capture / unified save: commit the freeze-mode change to the frame.
        publishMetadataEdit();
    };

    // ----- Per-waveform texture controls (grain count / FFT size) -----
    addAndMakeVisible(grainCountLabel);
    grainCountLabel.setText("Grains per waveform:", juce::dontSendNotification);
    grainCountLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(grainCountSlider);
    grainCountSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    grainCountSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 22);
    grainCountSlider.setRange((double)kGranularMinGrains,
                              (double)kGranularMaxGrains, 1.0);
    grainCountSlider.setValue(4.0, juce::dontSendNotification);  // historical default
    grainCountSlider.onValueChange = [this]() {
        if (auto* eng = AudioEngine::getInstance())
            eng->setPreviewGrainCount(selectedGrainCount());
        if (auditioning) regenerateAuditionGrain();
        // Re-capture / unified save: commit the grain-count change to the frame.
        publishMetadataEdit();
    };

    addAndMakeVisible(fftSizeLabel);
    fftSizeLabel.setText("FFT size per waveform:", juce::dontSendNotification);
    fftSizeLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(fftSizeCombo);
    fftSizeCombo.addItem("Auto", 1);
    for (int i = 0; i < kNumSpectralFftSizes; ++i)
        fftSizeCombo.addItem(juce::String(kSpectralFftSizes[i]), i + 2);
    fftSizeCombo.setSelectedId(1, juce::dontSendNotification);  // Auto
    fftSizeCombo.onChange = [this]() {
        if (auto* eng = AudioEngine::getInstance())
            eng->setPreviewFftSize(selectedFftSize());
        if (auditioning) regenerateAuditionGrain();
        // Re-capture / unified save: commit the FFT-size change to the frame.
        publishMetadataEdit();
    };

    // ----- Crossfade slider (all sources) -----
    addAndMakeVisible(crossfadeLabel);
    crossfadeLabel.setText("Crossfade (ms):", juce::dontSendNotification);
    crossfadeLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(crossfadeSlider);
    crossfadeDesiredMs = juce::jmax(kXfadeMinMs, captureDialogPrefs().crossfadeMs);
    // Provisional range; syncCrossfadeMaxToWindow below sets the real max
    // (half the freeze window) once the window controls are configured.
    crossfadeSlider.setRange(kXfadeMinMs, juce::jmax(2.0, crossfadeDesiredMs), 1.0);
    crossfadeSlider.setValue(crossfadeDesiredMs, juce::dontSendNotification);
    crossfadeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    crossfadeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
    // Enabled state + tooltip are owned by refreshFreezeExtras() (mode-aware:
    // crossfade is greyed out outside Crossfade loop). Called below once the
    // crossfade controls are in place.
    crossfadeSlider.onValueChange = [this]() {
        // Ignore the synthetic notification fired when syncCrossfadeMaxToWindow
        // clamps the value to a shrunken window - that's not the user picking a
        // new crossfade, and writing it back would clobber their intent and fire
        // a spurious metadata write on a mere window change.
        if (!syncingCrossfadeFromWindow) {
            crossfadeDesiredMs = crossfadeSlider.getValue();
            if (auditioning) regenerateAuditionGrain();
            // Unified save model: commit the crossfade change to the bound frame
            // (re-capture mode); no-op in append mode.
            publishMetadataEdit();
        }
    };
    // Cap the crossfade at half the current freeze window now that the window
    // controls (grain, samples-per-waveform, count, mode) are all configured.
    // (reverseSliderFill is applied with the other sliders further below.)
    syncCrossfadeMaxToWindow();
    // Now that every freeze control (grain, count, FFT, crossfade) exists, run
    // the mode-aware enable/visibility/tooltip pass once for the initial mode.
    refreshFreezeExtras();

    addAndMakeVisible(captureBtn);
    captureBtn.setTooltip(
        "Build the waveforms and add them to the wavetable along the "
        "Position dimension.");
    captureBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(60, 110, 70));
    captureBtn.onClick = [this]() {
        int n = (int)std::round(numFramesSlider.getValue());
        auto frames = buildFrames(n);
        if (onCapture) onCapture(std::move(frames));
        // Closing path: when embedded (onDismiss set), the host clears the
        // capture panel from the right pane. When launched as a
        // DialogWindow (onDismiss empty), walk up and exit modal state so
        // the JUCE owner can dispose us.
        if (onDismiss) {
            onDismiss();
        } else if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
            dw->exitModalState(1);
        }
    };

    addAndMakeVisible(cancelBtn);
    cancelBtn.onClick = [this]() {
        if (onDismiss) {
            onDismiss();
        } else if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
            dw->exitModalState(0);
        }
    };

    // Output gain for the captured waveforms (all sources). Scales each
    // produced frame's playback level via the per-frame gain field the wave
    // editor's Gain knob also drives, so a recording that's too quiet or too
    // loud can be levelled at capture time without a round-trip through the
    // editor. 1.0 = unity (the raw recorded level); the Preview reflects it
    // live. Double-click resets to unity.
    addAndMakeVisible(gainLabel);
    gainLabel.setText("Gain:", juce::dontSendNotification);
    gainLabel.setJustificationType(juce::Justification::centredRight);

    addAndMakeVisible(gainSlider);
    gainSlider.setRange(0.0, 4.0, 0.01);
    // Seed from the sticky session prefs (see captureDialogPrefs()).
    gainSlider.setValue(captureDialogPrefs().gain, juce::dontSendNotification);
    gainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    gainSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 22);
    gainSlider.setDoubleClickReturnValue(true, 1.0);
    gainSlider.setTooltip(
        "Output level for the captured waveforms. 1.0 = the recorded level; "
        "below 1 makes them quieter, above 1 louder (up to 4x). Applies to "
        "every sliced waveform and is reflected in the Preview. This sets the "
        "same per-waveform gain you can fine-tune later with the Gain knob in "
        "the wave editor.");
    gainSlider.onValueChange = [this]() {
        // Re-publish the loop so the level change is heard immediately while a
        // preview is running (the preview bakes the gain into its buffer).
        if (auditioning) regenerateAuditionGrain();
    };

    // Region-audition button (Mic + File): loop the selected slice so the
    // user can hear it before capturing. Playback's legacy tap doesn't get
    // one (it isn't reachable from the menu).
    if (source == CaptureSource::Mic || source == CaptureSource::File) {
        addAndMakeVisible(previewBtn);
        previewBtn.onClick = [this]() {
            if (auditioning) stopRegionAudition();
            else             startRegionAudition();
        };

        // "Preview waveform" selector: which of the N captured waveforms the
        // Preview button auditions. Range + enabled state are set by
        // updatePreviewIndexControl() (called below and on every count change);
        // disabled at a single waveform.
        addAndMakeVisible(previewIndexLabel);
        previewIndexLabel.setText("Preview waveform:", juce::dontSendNotification);
        previewIndexLabel.setJustificationType(juce::Justification::centredRight);

        addAndMakeVisible(previewIndexSlider);
        previewIndexSlider.setRange(1.0, 1.0, 1.0);   // widened by updatePreviewIndexControl
        previewIndexSlider.setValue(1.0, juce::dontSendNotification);
        previewIndexSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        previewIndexSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
        previewIndexSlider.onValueChange = [this]() {
            // Re-publish the loop for the newly-selected waveform so the change
            // is heard immediately while a preview is running.
            if (auditioning) regenerateAuditionGrain();
        };

        updatePreviewIndexControl();
    }

    // ----- Zoom / scroll view window (File source only) -----
    // Two sliders let the user zoom into and scroll along a long file so a
    // selection is workable instead of a one-pixel sliver. Added as child
    // components for every source but only made visible for File (the live
    // sources keep zoom 1 / scroll 0, a whole-buffer no-op).
    addChildComponent(scrollLabel);
    scrollLabel.setText("Scroll:", juce::dontSendNotification);
    scrollLabel.setJustificationType(juce::Justification::centredRight);
    addChildComponent(scrollSlider);
    scrollSlider.setRange(0.0, 1.0, 0.0);
    scrollSlider.setValue(0.0, juce::dontSendNotification);
    scrollSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    scrollSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    scrollSlider.setTooltip(
        "Slide the view window left or right along the file. Has no effect until "
        "you zoom in. The selection scrolls with the audio, staying pegged to a "
        "view edge once it reaches one.");
    scrollSlider.onValueChange = [this]() {
        viewScroll = scrollSlider.getValue();
        onViewChanged();
    };
    addChildComponent(zoomLabel);
    zoomLabel.setText("Zoom:", juce::dontSendNotification);
    zoomLabel.setJustificationType(juce::Justification::centredRight);
    addChildComponent(zoomSlider);
    zoomSlider.setRange(1.0, 2.0, 0.0);   // real max set per-buffer on load
    zoomSlider.setValue(1.0, juce::dontSendNotification);
    zoomSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    zoomSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    zoomSlider.setTooltip(
        "Zoom the waveform view in or out around the current scroll position. "
        "Zooming in makes a small selection inside a long file easy to work with; "
        "the selection scales with the zoom and is never larger than the visible "
        "window.");
    zoomSlider.onValueChange = [this]() {
        viewZoom = zoomSlider.getValue();
        onViewChanged();
    };
    if (showsViewControls()) {
        scrollLabel.setVisible(true);
        scrollSlider.setVisible(true);
        zoomLabel.setVisible(true);
        zoomSlider.setVisible(true);
        // Nothing to zoom/scroll until a file is loaded; configureViewSliders-
        // ForBuffer() re-enables them with the real range on load.
        scrollSlider.setEnabled(false);
        zoomSlider.setEnabled(false);
    }

    // Reverse each horizontal slider's two-tone fill: by default JUCE paints
    // the bright trackColour to the LEFT of the thumb and the dim
    // backgroundColour to the RIGHT. Swap them so the bright bar reads as
    // "headroom to the right" instead of "amount filled from the left". Done by
    // querying the current colours and writing them into the opposite slots, so
    // any future theme tweak still round-trips through this swap rather than
    // hardcoding hex values. Mirrors the same swap in layered_wave_editor.cpp's
    // granular sub-editor sliders. Applied to every slider in the dialog (some
    // are only created for certain sources; setColour on an unused slider is
    // harmless).
    auto reverseSliderFill = [](juce::Slider& s) {
        const auto bright = s.findColour(juce::Slider::trackColourId);
        const auto dim    = s.findColour(juce::Slider::backgroundColourId);
        s.setColour(juce::Slider::trackColourId,      dim);
        s.setColour(juce::Slider::backgroundColourId, bright);
    };
    reverseSliderFill(numFramesSlider);
    reverseSliderFill(grainLenSlider);
    reverseSliderFill(windowLenSlider);
    reverseSliderFill(gainSlider);
    reverseSliderFill(crossfadeSlider);
    reverseSliderFill(previewIndexSlider);
    reverseSliderFill(scrollSlider);
    reverseSliderFill(zoomSlider);

    // Initial state per source.
    if (isLiveSource()) {
        refreshSnapshot();
        startTimerHz(20);
        // Mic: start live (monitoring on, display sweeping) and label the
        // button as the "Pause" action. Done after addAndMakeVisible above.
        if (source == CaptureSource::Mic)
            setMicLive(true);
    } else {
        // File: disable capture until a file is loaded. The button gets
        // re-enabled in chooseAndLoadFile() on success.
        captureBtn.setEnabled(false);
    }

    updateSourceInfoLabel();
    updateRegionInfoLabel();
    updatePreviewButton();
}

CaptureFromPlaybackDialog::~CaptureFromPlaybackDialog() {
    stopTimer();
    // Remember the user's last settings so the next open restores them
    // (see captureDialogPrefs()). Written on every close, including Cancel -
    // "last settings" tracks what the user dialled in, not only what they
    // committed.
    {
        auto& p = captureDialogPrefs();
        p.numFrames = (int)std::round(numFramesSlider.getValue());
        p.gain      = gainSlider.getValue();
        p.noteId    = noteCombo.getSelectedId();
        p.octaveId  = octaveCombo.getSelectedId();
        p.grainMs   = grainLenSlider.getValue();
        // Remember whether the user picked the grain by hand, so a manual choice
        // isn't snapped away by the per-mode default on the next open.
        p.grainUserSet = grainUserSet;
        // Save the user's DESIRED crossfade (independent of the window-driven
        // clamp) so reopening restores intent, not a clamped value.
        p.crossfadeMs = crossfadeDesiredMs;
    }
    // Tear down any region-audition loop before the buffers go out of scope,
    // so subsequent audio blocks emit silence (the engine caches the preview
    // shared_ptrs until the next block reads the cleared pointers).
    if (auditioning) {
        if (auto* eng = AudioEngine::getInstance()) eng->clearPreview();
        auditioning = false;
    }
    // Restore the engine's global input-monitoring state we took over while
    // the Mic dialog was open, so we don't leave the mic routed to the
    // output (or clobber the user's main-window "Mon" toggle).
    if (source == CaptureSource::Mic) {
        if (auto* eng = AudioEngine::getInstance())
            eng->inputMonitoring.store(priorInputMonitoring);
    }
}

void CaptureFromPlaybackDialog::setMicLive(bool live) {
    micPaused = !live;
    // Going live invalidates any region audition: the ring buffer will sweep
    // out from under the loop, so stop it and require a re-pause to audition.
    if (live && auditioning)
        stopRegionAudition();
    if (auto* eng = AudioEngine::getInstance())
        eng->inputMonitoring.store(live);
    micLiveBtn.setButtonText(live ? "Pause" : "Go live");
    micLiveBtn.setColour(juce::TextButton::buttonColourId,
                         live ? juce::Colour(150, 90, 40)   // amber = live
                              : juce::Colour(60, 80, 110));  // blue = paused
    micLiveBtn.setTooltip(live
        ? "Stop hearing your input and pause the display so you can drag the "
          "region handles on a still waveform. Click again to go back to live "
          "input. (This pauses the mic; it is unrelated to the granular "
          "\"freeze method\" that sustains a captured note.)"
        : "Resume hearing and showing your live input.");
    updateSourceInfoLabel();
    updatePreviewButton();   // pausing enables the Preview button; going live disables it
    repaint(waveRect);
}

void CaptureFromPlaybackDialog::timerCallback() {
    if (!isLiveSource()) return;
    const bool paused = (source == CaptureSource::Mic)
                            ? micPaused
                            : pauseToggle.getToggleState();
    if (!paused)
        refreshSnapshot();
    updateSourceInfoLabel();
    repaint(waveRect);
}

void CaptureFromPlaybackDialog::refreshSnapshot() {
    auto* eng = AudioEngine::getInstance();
    if (!eng) {
        tap.clear();
        tapSampleRate = 0.0;
        return;
    }
    const int prevSize = (int)tap.size();
    double rate = 0.0;
    if (source == CaptureSource::Playback) {
        rate = eng->getPlaybackTapSnapshot(tap, kDisplayWindowSamples);
    } else if (source == CaptureSource::Mic) {
        rate = eng->getMicTapSnapshot(tap, kDisplayWindowSamples);
    } else {
        return; // file: no refresh
    }
    tapSampleRate = rate;

    if (tap.empty()) {
        regionStart = regionEnd = 0;
        return;
    }

    const int sz = (int)tap.size();
    if (prevSize == 0 && sz > 0) {
        // First fill: seat the region on the last ~1 s of audio.
        const int oneSec = (int)std::min((double)sz, std::max(1.0, rate));
        regionEnd   = sz;
        regionStart = std::max(0, sz - oneSec);
        syncWindowToAuto();   // per-method auto window (mult x grain) on first fill
        updateWindowLenControl();  // single-waveform: window = selection
        updateRegionInfoLabel();
    } else {
        regionStart = juce::jlimit(0, sz, regionStart);
        regionEnd   = juce::jlimit(regionStart, sz, regionEnd);
        // Keep the single-waveform window tracking the (re-clamped) selection.
        updateWindowLenControl();
    }
}

void CaptureFromPlaybackDialog::chooseAndLoadFile() {
    auto chooser = std::make_shared<juce::FileChooser>(
        "Load audio file", juce::File(),
        "*.wav;*.mp3;*.aiff;*.flac;*.ogg");
    chooser->launchAsync(juce::FileBrowserComponent::openMode,
        [this, chooser](const juce::FileChooser& fc) {
            auto file = fc.getResult();
            if (!file.existsAsFile()) return;

            juce::AudioFormatManager mgr;
            mgr.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader(mgr.createReaderFor(file));
            if (!reader) {
                sourceInfoLabel.setText(
                    "Could not decode: " + file.getFileName(),
                    juce::dontSendNotification);
                return;
            }

            // Mono-sum on the way in. Cap at kMaxFileSamples so a very
            // long file doesn't balloon the display.
            const int64_t totalLen = reader->lengthInSamples;
            const int     len      = (int)std::min<int64_t>(totalLen, kMaxFileSamples);
            const int     channels = (int)reader->numChannels;
            const bool    truncated = (totalLen > kMaxFileSamples);

            juce::AudioBuffer<float> buf(std::max(1, channels), std::max(1, len));
            reader->read(&buf, 0, len, 0, true, channels > 1);

            tap.assign((size_t)len, 0.0f);
            if (channels <= 0) {
                // Defensive: shouldn't happen for a successfully-created reader.
            } else if (channels == 1) {
                const float* src = buf.getReadPointer(0);
                for (int i = 0; i < len; ++i) tap[(size_t)i] = src[i];
            } else {
                const float invCh = 1.0f / (float)channels;
                for (int ch = 0; ch < channels; ++ch) {
                    const float* src = buf.getReadPointer(ch);
                    for (int i = 0; i < len; ++i)
                        tap[(size_t)i] += src[i] * invCh;
                }
            }

            tapSampleRate   = reader->sampleRate;
            fileLoaded      = true;
            fileSourcePath  = file.getFullPathName();
            regionStart     = 0;
            regionEnd       = len;
            captureBtn.setEnabled(true);
            configureViewSlidersForBuffer();    // reset + re-range zoom for this file
            syncWindowToAuto();                 // per-method auto window for the file
            updateWindowLenControl();  // single-waveform: window = selection

            if (truncated) {
                fileSourcePath += "  (truncated to " +
                    juce::String(kMaxFileSamples) + " samples)";
            }
            updateSourceInfoLabel();
            updateRegionInfoLabel();
            updatePreviewButton();   // a region is now selectable -> enable Preview
            repaint();
        });
}

void CaptureFromPlaybackDialog::resized() {
    auto r = getLocalBounds().reduced(12);

    // Top: title strip is painted in paint(); leave a 26 px gutter.
    r.removeFromTop(26);

    auto bottomRow = r.removeFromBottom(34);
    cancelBtn.setBounds(bottomRow.removeFromRight(100));
    bottomRow.removeFromRight(8);
    captureBtn.setBounds(bottomRow.removeFromRight(150));
    bottomRow.removeFromRight(16);
    if (source == CaptureSource::Playback) {
        pauseToggle.setBounds(bottomRow.removeFromRight(120));
    } else if (source == CaptureSource::Mic) {
        micLiveBtn.setBounds(bottomRow.removeFromRight(120));
        bottomRow.removeFromRight(8);
        previewBtn.setBounds(bottomRow.removeFromRight(110));
        // "Audio device..." sits at the left edge of the button row, away
        // from the capture/cancel cluster on the right.
        audioDeviceBtn.setBounds(bottomRow.removeFromLeft(130));
    }
    if (source == CaptureSource::File) {
        loadFileBtn.setBounds(bottomRow.removeFromLeft(140));
        bottomRow.removeFromLeft(8);
        previewBtn.setBounds(bottomRow.removeFromLeft(110));
    }

    r.removeFromBottom(8);
    // "Preview waveform" selector row (Mic + File) directly above the button
    // row, pairing visually with the Preview button below it. Removed from the
    // bottom first so it ends up just under the Samples-per-waveform / count
    // rows in top-to-bottom reading order.
    if (source == CaptureSource::Mic || source == CaptureSource::File) {
        auto prevRow = r.removeFromBottom(26);
        previewIndexLabel.setBounds(prevRow.removeFromLeft(140));
        previewIndexSlider.setBounds(prevRow.removeFromLeft(260));
        r.removeFromBottom(6);
    }
    // Crossfade row (all sources), just below the freeze window it caps against.
    {
        auto xfadeRow = r.removeFromBottom(26);
        crossfadeLabel.setBounds(xfadeRow.removeFromLeft(140));
        crossfadeSlider.setBounds(xfadeRow.removeFromLeft(260));
        r.removeFromBottom(6);
    }
    // "Window length" (freeze window, ms) row, then the grain-length row,
    // directly above the buttons (all sources). Removed from the bottom in this
    // order so they read top-to-bottom as "Waveforms to slice out", "Grain
    // length", "Window length".
    {
        auto sampRow = r.removeFromBottom(26);
        windowLenLabel.setBounds(sampRow.removeFromLeft(140));
        windowLenSlider.setBounds(sampRow.removeFromLeft(260));
        sampRow.removeFromLeft(12);
        fitWidthBtn.setBounds(sampRow.removeFromLeft(170));
        r.removeFromBottom(6);
    }
    {
        auto grainRow = r.removeFromBottom(26);
        grainLenLabel.setBounds(grainRow.removeFromLeft(140));
        grainLenSlider.setBounds(grainRow.removeFromLeft(260));
        r.removeFromBottom(6);
    }
    auto controlsRow = r.removeFromBottom(26);
    numFramesLabel.setBounds(controlsRow.removeFromLeft(140));
    numFramesSlider.setBounds(controlsRow.removeFromLeft(260));
    controlsRow.removeFromLeft(12);
    regionInfoLabel.setBounds(controlsRow);

    r.removeFromBottom(6);
    // Freeze-mode row: which granular "sustain the spot" algorithm the captured
    // frames use. Same left-edge as the rows above so the form aligns.
    auto freezeRow = r.removeFromBottom(26);
    freezeModeLabel.setBounds(freezeRow.removeFromLeft(140));
    freezeModeCombo.setBounds(
        freezeRow.removeFromLeft(intrinsicComboWidth(freezeModeCombo)));
    // Mode-specific extra to the right of the freeze picker: grain count for
    // the cloud modes, FFT size for Spectral freeze. The two control pairs
    // share the same slot - only the one that applies to the current mode is
    // visible (refreshFreezeExtras toggles visibility), so they don't need
    // separate horizontal space.
    freezeRow.removeFromLeft(14);
    auto extraRow = freezeRow;
    auto grainRow = extraRow;
    grainCountLabel.setBounds(grainRow.removeFromLeft(140));
    grainCountSlider.setBounds(grainRow.removeFromLeft(120));
    auto fftRow = extraRow;
    fftSizeLabel.setBounds(fftRow.removeFromLeft(140));
    fftSizeCombo.setBounds(
        fftRow.removeFromLeft(intrinsicComboWidth(fftSizeCombo)));

    r.removeFromBottom(6);
    // Embedded pitch row: label + note + octave + cents readout. Same
    // left-edge as the controls row above so the form aligns visually.
    auto pitchRow = r.removeFromBottom(26);
    noteOctaveLabel.setBounds(pitchRow.removeFromLeft(140));
    noteCombo.setBounds(pitchRow.removeFromLeft(intrinsicComboWidth(noteCombo)));
    pitchRow.removeFromLeft(6);
    octaveCombo.setBounds(pitchRow.removeFromLeft(intrinsicComboWidth(octaveCombo)));
    pitchRow.removeFromLeft(10);
    centsLabel.setBounds(pitchRow.removeFromLeft(80));
    // Gain control shares the pitch row's right side (the pitch picker leaves
    // it empty); the slider takes whatever width is left after its label.
    pitchRow.removeFromLeft(16);
    gainLabel.setBounds(pitchRow.removeFromLeft(46));
    gainSlider.setBounds(pitchRow);

    r.removeFromBottom(4);
    sourceInfoLabel.setBounds(r.removeFromBottom(20));

    r.removeFromBottom(2);
    // Mic's hint carries an extra line (the DirectSound troubleshooting note),
    // so give it more vertical room than the other sources.
    hintLabel.setBounds(r.removeFromBottom(source == CaptureSource::Mic ? 54 : 36));
    r.removeFromBottom(4);

    // Zoom / scroll row (File only), carved from the bottom of the leftover so
    // it sits directly under the waveform area that takes the remaining space.
    if (showsViewControls()) {
        r.removeFromBottom(6);
        auto viewRow = r.removeFromBottom(24);
        auto leftHalf  = viewRow.removeFromLeft(viewRow.getWidth() / 2);
        auto rightHalf = viewRow;
        scrollLabel.setBounds(leftHalf.removeFromLeft(50));
        scrollSlider.setBounds(leftHalf.reduced(4, 0));
        zoomLabel.setBounds(rightHalf.removeFromLeft(50));
        zoomSlider.setBounds(rightHalf.reduced(4, 0));
        r.removeFromBottom(4);
    }

    waveRect = r;
}

int CaptureFromPlaybackDialog::minViewLenSamples() const {
    const int total = (int)tap.size();
    if (total <= 0) return 0;
    int floor = kMinViewSamples;
    // With 2+ waveforms the view must never get tighter than one freeze window,
    // so a whole captured waveform always fits on screen. (A single waveform has
    // no independent window - it spans the selection - so the plain floor stands
    // and the user can still zoom into a long single-waveform selection.)
    const int n = (int)std::round(numFramesSlider.getValue());
    if (n > 1) floor = std::max(floor, windowFromSlider());
    return std::min(total, floor);
}

int CaptureFromPlaybackDialog::viewLenSamples() const {
    const int total = (int)tap.size();
    if (total <= 0) return 0;
    const double z = juce::jmax(1.0, viewZoom);
    int len = (int)std::llround((double)total / z);
    // Never below the per-window floor (or the whole buffer, if it's shorter).
    len = juce::jlimit(minViewLenSamples(), total, len);
    return len;
}

int CaptureFromPlaybackDialog::viewStartSamples() const {
    const int total = (int)tap.size();
    const int len   = viewLenSamples();
    const int maxStart = std::max(0, total - len);
    return juce::jlimit(0, maxStart,
                        (int)std::llround(viewScroll * (double)maxStart));
}

int CaptureFromPlaybackDialog::xForIdx(int idx) const {
    const int len = viewLenSamples();
    if (len <= 0 || waveRect.getWidth() <= 0) return waveRect.getX();
    const double t = (double)(idx - viewStartSamples()) / (double)len;
    return waveRect.getX() + (int)std::round(t * (double)waveRect.getWidth());
}

int CaptureFromPlaybackDialog::idxForX(int x) const {
    const int len = viewLenSamples();
    if (len <= 0 || waveRect.getWidth() <= 0) return 0;
    const double t = (double)(x - waveRect.getX()) / (double)waveRect.getWidth();
    return juce::jlimit(0, (int)tap.size(),
                        viewStartSamples() + (int)std::round(t * (double)len));
}

void CaptureFromPlaybackDialog::reconfigureZoomForWindow() {
    // Re-range the zoom slider so its maximum lets the view shrink to exactly
    // minViewLenSamples() - kMinViewSamples normally, or one freeze window with
    // 2+ waveforms - and never past 1x on a short file. The current zoom is kept
    // (clamped into the new range), so widening the window just pulls an over-
    // zoomed view back out rather than snapping it to 1x.
    if (reconfiguringZoom) return;   // re-entrancy guard (see header)
    const int total = (int)tap.size();
    if (total <= 0) return;
    const juce::ScopedValueSetter<bool> guard(reconfiguringZoom, true);
    const int    floor   = std::max(1, minViewLenSamples());
    const double maxZoom = (total > floor) ? (double)total / (double)floor : 1.0;
    const double rangeMax = std::max(1.0001, maxZoom);
    const double cur      = juce::jlimit(1.0, rangeMax, viewZoom);
    zoomSlider.setRange(1.0, rangeMax, 0.0);
    // Logarithmic feel: the midpoint of the slider sits at a moderate zoom so
    // the long high-zoom tail doesn't dominate the travel.
    if (maxZoom > 2.0)
        zoomSlider.setSkewFactorFromMidPoint(std::sqrt(maxZoom));
    zoomSlider.setValue(cur, juce::dontSendNotification);
    viewZoom = cur;
    zoomSlider.setEnabled(maxZoom > 1.0001);
}

void CaptureFromPlaybackDialog::configureViewSlidersForBuffer() {
    // Reset to "whole buffer" then range the zoom slider for it (and the current
    // window). reconfigureZoomForWindow keeps whatever viewZoom is, so reset it
    // to 1 first.
    viewZoom   = 1.0;
    viewScroll = 0.0;
    reconfigureZoomForWindow();
    scrollSlider.setValue(0.0, juce::dontSendNotification);
    scrollSlider.setEnabled(false);   // nothing to scroll at zoom 1
}

void CaptureFromPlaybackDialog::clampSelectionToView() {
    const int total = (int)tap.size();
    if (total <= 0) return;
    const int vStart = viewStartSamples();
    const int vLen   = viewLenSamples();
    const int vEnd   = vStart + vLen;

    int len = std::max(0, regionEnd - regionStart);
    // "Can't select more than you can see": cap the selection to the window.
    if (len > vLen) len = vLen;
    // Peg the (possibly shortened) selection flush inside the view.
    int start = regionStart;
    if (start + len > vEnd) start = vEnd - len;
    if (start < vStart)     start = vStart;
    regionStart = start;
    regionEnd   = start + len;
}

void CaptureFromPlaybackDialog::onViewChanged() {
    // The scroll slider only does something once we're zoomed in.
    scrollSlider.setEnabled(viewLenSamples() < (int)tap.size());
    clampSelectionToView();
    updateWindowLenControl();
    updateRegionInfoLabel();
    if (auditioning) regenerateAuditionGrain();
    repaint();
}

juce::Rectangle<int> CaptureFromPlaybackDialog::handleZone() const {
    return waveRect.expanded(kHandleHitRadius, 0);
}

void CaptureFromPlaybackDialog::updateSourceInfoLabel() {
    juce::String msg;
    msg << "Source: " << sourceName(source);
    if (source == CaptureSource::File) {
        if (fileLoaded) msg << "  -  " << fileSourcePath;
        else            msg << "  -  (no file loaded)";
    } else if (source == CaptureSource::Mic) {
        auto* eng = AudioEngine::getInstance();
        const bool sig = eng && eng->hasMicSignal();
        msg << (sig ? "  -  signal detected"
                    : "  -  no input signal yet (make a sound or check your mic)");
        // Live/paused state + a feedback caution while monitoring is on.
        msg << (micPaused
                    ? "   |   PAUSED (display held still)"
                    : "   |   LIVE - hearing your input (use headphones to avoid feedback)");
    } else { // Playback
        if (!tap.empty()) msg << "  -  " << juce::String((int)std::round(tapSampleRate)) << " Hz";
    }
    sourceInfoLabel.setText(msg, juce::dontSendNotification);
}

void CaptureFromPlaybackDialog::updateRegionInfoLabel() {
    if (tap.empty() || tapSampleRate <= 0.0) {
        if (source == CaptureSource::File && !fileLoaded)
            regionInfoLabel.setText("(no file loaded - press Load file...)",
                                    juce::dontSendNotification);
        else
            regionInfoLabel.setText("(no audio in buffer yet)",
                                    juce::dontSendNotification);
        return;
    }
    const int spanSamples = std::max(0, regionEnd - regionStart);
    const double spanSec = (double)spanSamples / tapSampleRate;
    const int n = (int)std::round(numFramesSlider.getValue());
    const double perFrameMs = (n > 0)
        ? (spanSec * 1000.0 / (double)n)
        : 0.0;
    const int srcLen = effectiveSrcLen();
    const double winMs = (tapSampleRate > 0.0)
        ? (double)srcLen * 1000.0 / tapSampleRate
        : 0.0;
    juce::String msg;
    msg << "Region: " << juce::String(spanSec, 2) << " s   |   "
        << n << " waveforms, "
        << juce::String(perFrameMs, 1) << " ms apart, each "
        << juce::String(winMs, 0) << " ms wide";
    regionInfoLabel.setText(msg, juce::dontSendNotification);
}

void CaptureFromPlaybackDialog::mouseDown(const juce::MouseEvent& e) {
    if (tap.empty()) return;

    const int xs = xForIdx(regionStart);
    const int xe = xForIdx(regionEnd);
    const int mx = e.x;

    // Handle grabbing is allowed anywhere in the handle zone (waveRect widened
    // by the hit radius), so an edge handle whose outer half is past the
    // wave-view edge is still grabbable from just outside it. The zone's
    // vertical extent matches waveRect, so this only fires within the handle row.
    if (handleZone().contains(e.getPosition())) {
        const int dStart = std::abs(mx - xs);
        const int dEnd   = std::abs(mx - xe);
        if (std::min(dStart, dEnd) <= kHandleHitRadius) {
            dragHandle = (dStart <= dEnd) ? 0 : 1;
            dragAnchorSampleOffset = 0;
            return;
        }
    }

    // Body drag and click-to-create require the click to land inside the wave
    // area proper, not in the handle slop margin outside it.
    if (!waveRect.contains(e.getPosition())) return;
    if (mx > xs && mx < xe) {
        dragHandle = 2;
        dragAnchorSampleOffset = idxForX(mx) - regionStart;
        return;
    }
    regionStart = idxForX(mx);
    regionEnd   = regionStart;
    dragHandle  = 1;
    updateRegionInfoLabel();
    repaint(handleZone());
}

void CaptureFromPlaybackDialog::mouseDrag(const juce::MouseEvent& e) {
    if (dragHandle < 0 || tap.empty()) return;
    const int sz = (int)tap.size();
    const int idx = juce::jlimit(0, sz, idxForX(e.x));
    if (dragHandle == 0) {
        // Dragging the start handle. If it crosses past the end handle, flip
        // roles: the gesture now controls the END handle. Without this, a fully
        // collapsed selection (both handles coincident) can never be reopened by
        // dragging right - jlimit(0, regionEnd, ...) pins regionStart at regionEnd.
        if (idx > regionEnd) {
            regionStart = regionEnd;
            regionEnd   = idx;
            dragHandle  = 1;
        } else {
            regionStart = idx;
        }
    } else if (dragHandle == 1) {
        // Symmetric: dragging the end handle past the start flips to the start.
        if (idx < regionStart) {
            regionEnd   = regionStart;
            regionStart = idx;
            dragHandle  = 0;
        } else {
            regionEnd = idx;
        }
    } else if (dragHandle == 2) {
        const int span = regionEnd - regionStart;
        const int newStart = juce::jlimit(0, std::max(0, sz - span),
                                          idxForX(e.x) - dragAnchorSampleOffset);
        regionStart = newStart;
        regionEnd   = newStart + span;
    }
    // Don't let a handle drag shrink the selection below one window length (so a
    // section band always fits), except when the view is zoomed in tighter than
    // the window - then only stop further shrinking. Body drags (handle 2) keep
    // a fixed span, so this only affects the two resize handles.
    enforceMinSelectionDuringDrag();
    // With one waveform the window tracks the selection, so keep the (disabled)
    // per-waveform slider showing the live selection length while dragging.
    updateWindowLenControl();
    updateRegionInfoLabel();
    repaint(handleZone());
    // Scrub-to-audition: hear the slice as the handles move, like the
    // capture-from-project dialog. Auto-start the loop once the region is
    // auditionable, and keep its source buffer following the handles.
    if (canAudition()) {
        if (auditioning) regenerateAuditionGrain();
        else             startRegionAudition();
    }
    updatePreviewButton();
}

void CaptureFromPlaybackDialog::mouseUp(const juce::MouseEvent&) {
    dragHandle = -1;
}

void CaptureFromPlaybackDialog::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(22, 22, 28));

    // Title strip at the top, source-specific so the user knows which
    // mode they're in even without looking at the window title bar.
    g.setColour(juce::Colour(220, 220, 230));
    g.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
    juce::String title;
    switch (source) {
        case CaptureSource::Playback: title = "Capture waveforms from recent project playback"; break;
        case CaptureSource::Mic:      title = "Capture waveforms from microphone / audio input"; break;
        case CaptureSource::File:     title = "Capture waveforms from audio file"; break;
    }
    g.drawText(title,
               getLocalBounds().reduced(12).removeFromTop(20),
               juce::Justification::centredLeft);

    g.setColour(juce::Colour(16, 16, 22));
    g.fillRect(waveRect);
    g.setColour(juce::Colour(60, 60, 70));
    g.drawRect(waveRect);

    if (tap.empty()) {
        g.setColour(juce::Colour(120, 120, 130));
        juce::String empty;
        switch (source) {
            case CaptureSource::Playback: empty = "Waiting for playback - press Play in the transport."; break;
            case CaptureSource::Mic:      empty = "Waiting for input - make a sound into your mic."; break;
            case CaptureSource::File:     empty = "Press \"Load file...\" to pick an audio file."; break;
        }
        g.drawText(empty, waveRect, juce::Justification::centred);
        return;
    }

    // Min/max waveform render: one vertical line per pixel. Pixels map the
    // current VIEW window [viewStart, viewStart+viewLen) (zoom / scroll), not
    // the whole buffer - at zoom 1 the view is the whole buffer, so this is the
    // old full-buffer render.
    const int W = waveRect.getWidth();
    const int H = waveRect.getHeight();
    const int sz = (int)tap.size();
    const int vStart = viewStartSamples();
    const int vLen   = std::max(1, viewLenSamples());
    const int x0 = waveRect.getX();
    const int yMid = waveRect.getY() + H / 2;
    const int halfH = H / 2;
    g.setColour(juce::Colour(110, 130, 200));
    for (int px = 0; px < W; ++px) {
        const int sStart = std::min(sz, vStart + (int)((int64_t)px * vLen / W));
        const int sEnd   = std::min(sz, vStart + (int)((int64_t)(px + 1) * vLen / W));
        float mn =  1.0f, mx = -1.0f;
        for (int s = sStart; s < sEnd; ++s) {
            const float v = tap[(size_t)s];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        if (sEnd <= sStart) { mn = mx = 0.0f; }
        const int y1 = yMid - (int)std::round(juce::jlimit(-1.0f, 1.0f, mx) * halfH);
        const int y2 = yMid - (int)std::round(juce::jlimit(-1.0f, 1.0f, mn) * halfH);
        g.drawVerticalLine(x0 + px, (float)std::min(y1, y2), (float)std::max(y1, y2) + 1.0f);
    }

    // Zero line.
    g.setColour(juce::Colour(50, 50, 60));
    g.drawHorizontalLine(yMid, (float)waveRect.getX(), (float)waveRect.getRight());

    // Region shading.
    const int xs = xForIdx(regionStart);
    const int xe = xForIdx(regionEnd);
    if (xe > xs) {
        g.setColour(juce::Colour::fromFloatRGBA(1.0f, 0.65f, 0.2f, 0.18f));
        g.fillRect(juce::Rectangle<int>(xs, waveRect.getY(), xe - xs, H));
    }

    // Per-waveform section bands: each band shows the span of audio one
    // captured waveform covers (its source window, width = effectiveSrcLen).
    // The bands keep a FIXED width but are spaced across the selection (see
    // bandStartForIndex). While they fit, the leftover space is split into
    // equal gaps everywhere - the same margin before the first band, between
    // every pair, and after the last - so the end margins match the inter-band
    // gaps and all grow/shrink together as the selection is resized. When the
    // bands have to overlap (width > selection / count) they instead stay
    // CONTAINED within the selection (first band flush to the left handle, last
    // flush to the right) so the row never spills past the handles. Each band
    // stays exactly srcLen wide, drawn in the same orange as the region.
    const int n        = (int)std::round(numFramesSlider.getValue());
    const int regLen   = std::max(0, regionEnd - regionStart);
    const int srcLen   = effectiveSrcLen();

    if (n > 0 && regLen > 0 && srcLen > 0) {
        // A band's window can extend past the zoomed-in view edges (the window is
        // honoured as-is, not shrunk to the selection), so clip the band graphics
        // to waveRect - otherwise a band reaching past the view would paint into
        // the side margins. At zoom 1 nothing extends past the rect, so this is a
        // no-op there.
        juce::Graphics::ScopedSaveState bandClip(g);
        g.reduceClipRegion(waveRect);
        // Each band is drawn directly from its SAMPLE-space span - the exact
        // window buildFrames() captures (bandStartForIndex), converted to
        // pixels with xForIdx. This is the single source of truth for the
        // geometry, so what you see is what you get.
        //
        // Two properties fall out of drawing in sample space:
        //
        //  * CONSTANT WIDTH ON RESIZE. xForIdx is a linear map over the whole
        //    buffer (pixels-per-sample is fixed, independent of the region),
        //    so a band's pixel width is srcLen * pxPerSample - it depends only
        //    on the "Window length", never on the selection size. Stretching
        //    the selection re-spaces the bands (every gap, end margins included)
        //    but never rescales them.
        //
        //  * ZERO GAP AT THE DEFAULT. When regLen == n*srcLen every uniform gap
        //    is zero, so band i ends at the same sample band i+1 begins and
        //    xForIdx maps the shared boundary to the SAME pixel: the bands tile
        //    edge-to-edge with no seam and the outermost edges land on the two
        //    handles. A wider selection opens every gap by the same amount
        //    (end margins included); a narrower one overlaps them uniformly.
        for (int i = 0; i < n; ++i) {
            const int startIdx = bandStartForIndex(i, n, srcLen);
            const int bx0 = xForIdx(startIdx);
            const int bx1 = xForIdx(startIdx + srcLen);
            juce::Rectangle<int> band(bx0, waveRect.getY(),
                                      std::max(1, bx1 - bx0), H);
            // Brighter orange fill over the region tint so each band reads as
            // a distinct slice; thin orange separators at the band edges (no
            // center line) so overlapping bands stay legible.
            g.setColour(juce::Colour::fromFloatRGBA(1.0f, 0.65f, 0.2f, 0.16f));
            g.fillRect(band);
            g.setColour(juce::Colour::fromFloatRGBA(1.0f, 0.65f, 0.2f, 0.45f));
            g.drawVerticalLine(bx0,     (float)waveRect.getY(), (float)waveRect.getBottom());
            g.drawVerticalLine(bx1 - 1, (float)waveRect.getY(), (float)waveRect.getBottom());
        }
    }

    // Clip the handle graphics to the handle zone (waveRect widened by the hit
    // radius) rather than waveRect itself, so an edge handle's outer half - its
    // bar and +/-6 px caps - is drawn in full instead of being chopped at the
    // buffer edge. The zone is the same region the handle hit-test and the
    // drag/create repaints use, so the overhang is always cleared and never
    // leaves a ghost. Clamping to the zone (not unbounded) still prevents the
    // caps from bleeding into the surrounding chrome.
    const juce::Rectangle<int> hz = handleZone();
    auto drawHandle = [&](int x) {
        g.setColour(juce::Colour(255, 165, 60));
        g.fillRect(juce::Rectangle<int>(x - 2, waveRect.getY(), 4, H).getIntersection(hz));
        g.fillRect(juce::Rectangle<int>(x - 6, waveRect.getY(), 12, 8).getIntersection(hz));
        g.fillRect(juce::Rectangle<int>(x - 6, waveRect.getBottom() - 8, 12, 8).getIntersection(hz));
    };
    drawHandle(xs);
    drawHandle(xe);
}

int CaptureFromPlaybackDialog::effectiveGrainLen() const {
    const double sr = (tapSampleRate > 0.0) ? tapSampleRate : 48000.0;
    const double ms = grainLenSlider.getValue();
    return std::max(64, (int)std::round(ms / 1000.0 * sr));
}

int CaptureFromPlaybackDialog::windowFromSlider() const {
    // The per-waveform window the slider requests, in SAMPLES (ms -> samples at
    // the source rate), floored by the current mode but NOT capped to the
    // selection. effectiveSrcLen() applies the selection cap; minSelectionLen()
    // uses this uncapped value as the floor the selection can't shrink below.
    const double sr = (tapSampleRate > 0.0) ? tapSampleRate : 48000.0;
    const GranularFreezeMode mode = selectedFreezeMode();
    const bool grainCloud = (mode == GranularFreezeMode::AsyncGranular
                             || mode == GranularFreezeMode::PitchSyncGrains);
    const int  floor      = grainCloud ? effectiveGrainLen() : 256;
    int w = (int)std::round(windowLenSlider.getValue() / 1000.0 * sr);
    w = std::max(w, floor);
    const int tapLen = (int)tap.size();
    if (tapLen > 0) w = std::min(w, tapLen);
    return std::max(1, w);
}

int CaptureFromPlaybackDialog::effectiveSrcLen() const {
    const int regionLen = std::max(0, regionEnd - regionStart);
    const int tapLen    = (int)tap.size();
    const int n         = (int)std::round(numFramesSlider.getValue());

    if (n <= 1) {
        // A single waveform spans the WHOLE selection - there is no per-waveform
        // spacing to honour, so the one window simply is the selection. The
        // window-length slider is disabled in this case (see
        // updateWindowLenControl), so the selection size is the source of truth,
        // not the slider. Floor it the same way windowFromSlider would.
        const GranularFreezeMode mode = selectedFreezeMode();
        const bool grainCloud = (mode == GranularFreezeMode::AsyncGranular
                                 || mode == GranularFreezeMode::PitchSyncGrains);
        const int  floor      = grainCloud ? effectiveGrainLen() : 256;
        int w = regionLen;
        w = std::max(w, floor);
        if (tapLen > 0) w = std::min(w, tapLen);
        // Never let a window exceed the selection itself: the section bands are
        // laid out inside [regionStart, regionEnd], and a srcLen wider than the
        // selection makes bandStartForIndex's contained/gap models place bands
        // past the handles (the bug where selection content spills outside the
        // handles after zooming, or out the opposite side once the handles are
        // dragged very close together).
        if (regionLen > 0) w = std::min(w, regionLen);
        return std::max(1, w);
    }

    // 2+ waveforms: the window-length slider is the source of truth (a per-
    // waveform duration). It auto-tracks autoWindowMultiplier(mode) x grain via
    // syncWindowToAuto() until the user overrides it. Cap to the selection so
    // the bands always stay between the two handles regardless of zoom or how
    // tightly the handles are squeezed. (The min-selection drag clamp keeps the
    // selection >= the requested window, so this cap normally only bites when
    // the view is too zoomed-in to fit the full window.)
    int w = windowFromSlider();
    if (regionLen > 0) w = std::min(w, regionLen);
    return std::max(1, w);
}

int CaptureFromPlaybackDialog::minSelectionLen() const {
    const int n = (int)std::round(numFramesSlider.getValue());
    // One waveform: its window IS the selection, so there's no independent
    // minimum - the user may shrink the selection freely.
    if (n <= 1) return 0;
    // The selection can't shrink below one window. The zoom is clamped so the
    // view never gets tighter than one window (minViewLenSamples), so this
    // minimum always fits on screen - no "window bigger than the view" escape
    // hatch is needed.
    return std::max(0, windowFromSlider());
}

void CaptureFromPlaybackDialog::enforceMinSelectionDuringDrag() {
    // A resize drag (start/end handle) can't shrink the selection below the
    // per-waveform window (minSelectionLen()). Body drag (dragHandle==2) keeps
    // the length fixed, so it's exempt. The clamp pushes the *dragged* edge back
    // to the minimum; near a buffer boundary it nudges the anchor instead so the
    // selection stays inside [0, total].
    if (dragHandle != 0 && dragHandle != 1) return;
    const int minLen = minSelectionLen();
    if (minLen <= 0) return;
    if (regionEnd - regionStart >= minLen) return;
    const int total = (int)tap.size();
    if (dragHandle == 0) {            // start edge moving, end anchored
        regionStart = regionEnd - minLen;
        if (regionStart < 0) { regionStart = 0; regionEnd = std::min(total, minLen); }
    } else {                          // end edge moving, start anchored
        regionEnd = regionStart + minLen;
        if (regionEnd > total) { regionEnd = total; regionStart = std::max(0, total - minLen); }
    }
}

void CaptureFromPlaybackDialog::resizeSelectionForCountChange(int oldN, int newN) {
    // Keep each waveform's window length constant when the count changes by
    // scaling the SELECTION in proportion: newLen = oldLen * newN / oldN. Since
    // the slot spacing is regionLen / n, scaling the region by newN/oldN keeps the
    // per-slot spacing (and thus the per-waveform window) fixed - adding waveforms
    // grows the selection rather than cramming more bands into a fixed span. The
    // window control itself is count-independent (autoWindowMultiplier x grain, or
    // the user's fixed value), so nothing else needs to move.
    //
    // The resize is *centred on the currently-selected preview waveform* so the
    // band you're auditioning stays put under the handles while the others spread
    // out / close in around it. When shrinking, the new (smaller) selection is
    // additionally constrained to stay within the OLD selection's bounds, so a
    // smaller count never pushes a handle farther out than it already was.
    const int total = (int)tap.size();
    if (total <= 0 || oldN <= 0 || newN <= 0 || oldN == newN) return;
    const int oldStart = regionStart;
    const int oldEnd   = regionEnd;
    const int oldLen   = std::max(0, oldEnd - oldStart);
    if (oldLen <= 0) return;

    // Centre of the band the Preview-waveform picker currently points at, using
    // the *old* geometry (count + region as they stand before the resize - the
    // preview slider is re-ranged only after this call).
    const int srcLen    = effectiveSrcLen();
    const int pi        = juce::jlimit(1, oldN, (int)std::round(previewIndexSlider.getValue()));
    const int bandStart = bandStartForIndex(pi - 1, oldN, srcLen);
    const double centre = (double)bandStart + 0.5 * (double)srcLen;

    long long newLenLL = std::llround((double)oldLen * (double)newN / (double)oldN);
    int newLen = (int)std::min<long long>(newLenLL, (long long)total);
    newLen = std::max(newLen, std::min(total, 256));   // never collapse to nothing

    int newStart = (int)std::llround(centre - 0.5 * (double)newLen);
    // Clamp to the buffer.
    if (newStart + newLen > total) newStart = total - newLen;
    if (newStart < 0) newStart = 0;

    // Shrinking: never let a handle move farther out than it already was - keep
    // the new (smaller) selection inside [oldStart, oldEnd]. newLen < oldLen <=
    // total guarantees the window [oldStart, oldEnd] can hold it.
    if (newLen < oldLen) {
        if (newStart < oldStart)          newStart = oldStart;
        if (newStart + newLen > oldEnd)   newStart = oldEnd - newLen;
        if (newStart < oldStart)          newStart = oldStart;
    }

    regionStart = newStart;
    regionEnd   = newStart + newLen;
}

int CaptureFromPlaybackDialog::bandStartForIndex(int i, int n, int srcLen) const {
    // Thin wrapper over the shared int64 geometry (captureBandStartForIndex,
    // documented near the top of this file) so file and song capture lay out
    // their section bands identically. srcLen is capped to the selection by
    // effectiveSrcLen(), so the bands never spill past the handles.
    const int tapLen = (int)tap.size();
    return (int)captureBandStartForIndex(
        (int64_t)regionStart, (int64_t)regionEnd, i, n, (int64_t)srcLen,
        (int64_t)std::max(0, tapLen - srcLen));
}

void CaptureFromPlaybackDialog::syncWindowToFitSlots() {
    // Set the window to exactly one slot spacing (regionLen / n) so the n bands
    // tile the selection edge-to-edge with zero gaps. This is an explicit user
    // action (the "Fit width to selection" button), so it counts as a window
    // override - the window then stays fixed across grain / mode changes until
    // re-fit or re-dragged. dontSendNotification so this programmatic set doesn't
    // recurse through onValueChange (which is why we mark the override here).
    const int n      = (int)std::round(numFramesSlider.getValue());
    const int regLen = std::max(0, regionEnd - regionStart);
    if (n <= 0 || regLen <= 0) return;
    const double sr = (tapSampleRate > 0.0) ? tapSampleRate : 48000.0;
    // The slider is in milliseconds, so convert the per-slot sample spacing.
    double spacingMs = (double)regLen / (double)n / sr * 1000.0;
    spacingMs = juce::jlimit(kWindowMinMs, kWindowMaxMs, spacingMs);
    windowLenSlider.setValue(spacingMs, juce::dontSendNotification);
    windowUserSet = true;
}

void CaptureFromPlaybackDialog::syncWindowToAuto() {
    // While the window is auto (the user hasn't dragged it), keep the slider on
    // autoWindowMultiplier(mode) x grain - the per-method default that gives the
    // grain-cloud modes roam room (4x) and keeps the loop / FFT modes at 1x,
    // matching the editor's kWindowAutoPerMethod sentinel. No-op once overridden,
    // and for a single waveform (whose window is the whole selection - handled by
    // updateWindowLenControl / effectiveSrcLen). dontSendNotification so
    // this never flips windowUserSet.
    if (windowUserSet) return;
    const int n = (int)std::round(numFramesSlider.getValue());
    if (n <= 1) return;
    // Grain-cloud modes: window = 4x grain (roam room). Since the grain slider is
    // already in ms, this is a plain ms multiply - no rate conversion needed.
    // Non-grain modes (Crossfade / Spectral): the window is a loop body / FFT
    // frame that must NOT shrink with the (now-short) grain, so it uses the
    // grain-independent kNonGrainAutoWindowSamples default (converted to ms
    // against the source rate) - matching the shared resolveAutoWindowLen.
    const GranularFreezeMode mode = selectedFreezeMode();
    double windowMs;
    if (mode == GranularFreezeMode::AsyncGranular
        || mode == GranularFreezeMode::PitchSyncGrains) {
        windowMs = (double)autoWindowMultiplier(mode) * grainLenSlider.getValue();
    } else {
        const double sr = (tapSampleRate > 0.0) ? tapSampleRate : 48000.0;
        windowMs = (double)kNonGrainAutoWindowSamples / sr * 1000.0;
    }
    windowMs = juce::jlimit(kWindowMinMs, kWindowMaxMs, windowMs);
    windowLenSlider.setValue(windowMs, juce::dontSendNotification);
}

void CaptureFromPlaybackDialog::updateWindowLenControl() {
    // The freeze window may have just changed (count / grain / mode / region),
    // which changes how far the user is allowed to zoom in - keep the zoom
    // slider's reach in step. Cheap and idempotent when nothing changed.
    reconfigureZoomForWindow();
    const int n = (int)std::round(numFramesSlider.getValue());
    if (n <= 1) {
        // One waveform spans the whole selection: there is nothing for this
        // slider to control, so lock it and show it holding the selection
        // length (kept current as the selection is resized) instead of a stale
        // value. The window length itself comes from effectiveSrcLen, which
        // returns the selection size in this case.
        const int    regLen = std::max(0, regionEnd - regionStart);
        const double sr     = (tapSampleRate > 0.0) ? tapSampleRate : 48000.0;
        const double regMs  = (double)regLen / sr * 1000.0;
        windowLenSlider.setValue(
            juce::jlimit(kWindowMinMs, kWindowMaxMs, regMs),
            juce::dontSendNotification);
        windowLenSlider.setEnabled(false);
        windowLenSlider.setTooltip(
            "Disabled while there is a single waveform: one waveform spans the "
            "whole selection, so its length is just the selection size - resize "
            "the selection (drag the orange handles) to change it. Increase "
            "\"Waveforms to slice out\" above 1 to set a per-waveform length "
            "independently of the selection.");
        // The Fit button only tidies the per-waveform width, which is fixed to
        // the selection here, so it has nothing to do.
        fitWidthBtn.setEnabled(false);
        fitWidthBtn.setTooltip(
            "Not needed with a single waveform - it already spans the whole "
            "selection. Add more waveforms to use this.");
        // The window (= selection) drives the crossfade-seam cap.
        syncCrossfadeMaxToWindow();
        return;
    }
    windowLenSlider.setEnabled(true);
    fitWidthBtn.setEnabled(true);
    fitWidthBtn.setTooltip(
        "Set the per-waveform width so the bands exactly tile the current "
        "selection - no gaps, no overlap. Same as dragging \"Window length\" "
        "until each band abuts the next. Use it after resizing "
        "the selection to snap back to a clean edge-to-edge layout.");
    windowLenSlider.setTooltip(
        "The freeze WINDOW, in milliseconds: how long each captured waveform "
        "spans - the region the freeze mode loops (Crossfade), roams (Async / "
        "Pitch-sync grains), or analyses (Spectral). Shown in ms (like grain "
        "length and crossfade) so you can read the window:grain ratio directly. "
        "Until you drag it, this auto-tracks a multiple of the grain length above "
        "- 4x for the grain-cloud modes so they have room to roam, 1x for the "
        "others - exactly like the wave editor's window. Drag it to set an "
        "explicit width (it then stays fixed as you change the grain or mode). "
        "The shaded orange bands over the waveform show each waveform's span, "
        "each exactly this wide and spaced evenly across the selection; "
        "\"Fit width to selection\" snaps them to tile with no gaps.");
    // The freeze window drives the crossfade-seam cap (half the window).
    syncCrossfadeMaxToWindow();
}

void CaptureFromPlaybackDialog::syncCrossfadeMaxToWindow() {
    // The CrossfadeLoop seam can be at most half the loop, and the loop IS the
    // freeze window, so cap the slider at effectiveSrcLen()/2 (in ms). Mirrors
    // the engine's xfade = min(req, window/2) clamp and the editor's
    // crossfadeMaxSamples, so the visible value always equals the audible one.
    // The displayed value is min(desired, newMax): shrinking the window clamps
    // the crossfade down, growing it back restores the user's choice. The
    // syncingCrossfadeFromWindow guard keeps setRange's internal clamp (which
    // fires onValueChange synchronously) from overwriting crossfadeDesiredMs.
    const double sr        = (tapSampleRate > 0.0) ? tapSampleRate : 48000.0;
    const int    windowLen = effectiveSrcLen();
    const double maxMs     = juce::jmax(kXfadeMinMs,
                                        (double)(windowLen / 2) / sr * 1000.0);
    const double visible   = juce::jlimit(kXfadeMinMs, maxMs, crossfadeDesiredMs);
    syncingCrossfadeFromWindow = true;
    crossfadeSlider.setRange(kXfadeMinMs, juce::jmax(2.0, maxMs), 1.0);
    crossfadeSlider.setValue(visible, juce::dontSendNotification);
    syncingCrossfadeFromWindow = false;
}

void CaptureFromPlaybackDialog::updatePreviewIndexControl() {
    if (source != CaptureSource::Mic && source != CaptureSource::File) return;
    const int n = (int)std::round(numFramesSlider.getValue());
    // Clamp the current pick into [1, n] before re-ranging (JUCE would
    // otherwise snap a now-out-of-range value to the new max silently).
    const int cur = juce::jlimit(1, std::max(1, n),
                                 (int)std::round(previewIndexSlider.getValue()));
    previewIndexSlider.setRange(1.0, (double)std::max(1, n), 1.0);
    previewIndexSlider.setValue((double)cur, juce::dontSendNotification);

    const bool enabled = (n >= 2);
    previewIndexSlider.setEnabled(enabled);
    previewIndexLabel.setEnabled(enabled);
    if (enabled) {
        previewIndexSlider.setTooltip(
            "Which of the " + juce::String(n) + " captured waveforms the Preview "
            "button plays. 1 = the first (earliest in the selection), "
            + juce::String(n) + " = the last. Change it while previewing to "
            "audition a different waveform without stopping.");
    } else {
        previewIndexSlider.setTooltip(
            "Disabled with a single waveform - there is only one to hear, and "
            "Preview plays the whole selection. Increase \"Waveforms to slice "
            "out\" above 1 to choose among several.");
    }
}

std::vector<float>
CaptureFromPlaybackDialog::buildGrainSource(int startIdx, int windowLen) const {
    const int tapLen       = (int)tap.size();
    const int reservedTail = std::max(0, windowLen / 2);
    const int srcLen       = std::max(0, windowLen + reservedTail);
    std::vector<float> source((size_t)srcLen, 0.0f);
    for (int s = 0; s < srcLen; ++s) {
        const int idx = startIdx + s;
        if (idx >= 0 && idx < tapLen)
            source[(size_t)s] = tap[(size_t)idx];
    }
    return source;
}

void CaptureFromPlaybackDialog::refreshFreezeExtras() {
    const GranularFreezeMode mode = selectedFreezeMode();
    const bool usesGrain = (mode == GranularFreezeMode::AsyncGranular
                            || mode == GranularFreezeMode::PitchSyncGrains);
    const bool isSpectral = (mode == GranularFreezeMode::SpectralFreeze);
    // The grain-count and FFT-size controls share one slot in the freeze row,
    // so show only the pair that applies to the current mode. CrossfadeLoop
    // uses neither, so both hide. The visible control is always enabled; the
    // tooltip on each still explains what it does.
    grainCountLabel.setVisible(usesGrain);
    grainCountSlider.setVisible(usesGrain);
    fftSizeLabel.setVisible(isSpectral);
    fftSizeCombo.setVisible(isSpectral);
    grainCountLabel.setEnabled(usesGrain);
    grainCountSlider.setEnabled(usesGrain);
    fftSizeLabel.setEnabled(isSpectral);
    fftSizeCombo.setEnabled(isSpectral);

    grainCountSlider.setTooltip(usesGrain
        ? "How many overlapping grains make up each captured waveform's cloud. "
          "More = denser and smoother; fewer = sparser and more granular. The "
          "level stays constant as you change it. Only used by Async and "
          "Pitch-sync grains."
        : "Disabled - the current freeze mode has no grains. Switch to Async or "
          "Pitch-sync grains to set the grain count.");
    fftSizeCombo.setTooltip(isSpectral
        ? "FFT size for each captured waveform's Spectral freeze, in samples. "
          "Larger = finer frequency detail (more bins) but a coarser time "
          "window. Auto picks the largest size that fits the waveform (up to "
          "2048). Only used by Spectral freeze."
        : "Disabled - the FFT size only matters in Spectral-freeze mode. Switch "
          "the freeze mode to Spectral freeze to use it.");

    // Grain length is only used by the grain-cloud modes; CrossfadeLoop loops
    // the whole window and SpectralFreeze FFTs it, so grey it out there (its
    // value still seeds the auto window multiplier, 1x for these modes).
    grainLenLabel.setEnabled(usesGrain);
    grainLenSlider.setEnabled(usesGrain);
    grainLenSlider.setTooltip(usesGrain
        ? "Length of each overlapping grain inside a captured waveform, in "
          "milliseconds. Short (~5 ms, the default) = a smooth, CONSTANT cloud; "
          "longer grains roam a proportionally wider window = more evolving / "
          "less constant. The freeze window below auto-tracks 4x this (until you "
          "drag it) so the cloud has room to roam. Used by Async and Pitch-sync "
          "grains."
        : "Disabled - the current freeze mode has no grains: Crossfade loop "
          "loops the whole freeze window and Spectral freeze analyses it. Switch "
          "to Async or Pitch-sync grains to set the grain length. (Use \"Window "
          "length\" to set the loop / analysis length for this mode.)");

    // The crossfade seam only applies to CrossfadeLoop (the loop blends its end
    // back into its start). The other modes ignore it, so grey it out there -
    // its value is still kept and re-baked into captured frames so a later mode
    // switch in the wave editor already has the seam set.
    const bool usesCrossfade = (mode == GranularFreezeMode::CrossfadeLoop);
    crossfadeLabel.setEnabled(usesCrossfade);
    crossfadeSlider.setEnabled(usesCrossfade);
    crossfadeSlider.setTooltip(usesCrossfade
        ? "Crossfade-loop seam length in milliseconds. The loop blends its end "
          "back into its start over this duration to hide the seam click. Short "
          "(1-10 ms): tight, the loop period is most audible. Long (near the cap "
          "= half the freeze window): the loop becomes a rolling crossfade "
          "between two playheads, smoother but more blurred. The max tracks half "
          "the \"Window length\" window because the engine can't overlap two "
          "ramps longer than that in one loop."
        : "Disabled - the crossfade seam only applies to Crossfade loop mode, "
          "which loops the freeze window and blends its end back into its start. "
          "Switch the freeze mode to Crossfade loop to set the seam length.");

    // Keep the engine audition in sync with the current values.
    if (auto* eng = AudioEngine::getInstance()) {
        eng->setPreviewGrainCount(selectedGrainCount());
        eng->setPreviewFftSize(selectedFftSize());
    }
}

void CaptureFromPlaybackDialog::seedFromExistingFrame(double pitchHz,
                                                      int freezeModeIdx,
                                                      double crossfadeMs,
                                                      int grainCount,
                                                      int fftSize) {
    // ----- Pitch -----
    if (pitchHz > 0.0) {
        capturedPitchHz = pitchHz;
        auto [n, o] = hzToNearestNoteOctave(pitchHz);
        if (n >= 0 && n <= 11) noteCombo.setSelectedId(n + 1, juce::dontSendNotification);
        if (o >= 0 && o <= 9)  octaveCombo.setSelectedId(o + 1, juce::dontSendNotification);
        centsLabel.setText(formatCents(pitchHz), juce::dontSendNotification);
    }

    // ----- Freeze mode -----
    if (freezeModeIdx >= 0 && freezeModeIdx <= 4) {
        freezeModeCombo.setSelectedId(freezeModeIdx + 1, juce::dontSendNotification);
        // Match the live audition to the seeded mode (the onChange handler that
        // would normally do this is suppressed by dontSendNotification).
        if (auto* eng = AudioEngine::getInstance())
            eng->setGrainFreezeMode((AudioEngine::GrainFreezeMode)freezeModeIdx);
    }

    // ----- Crossfade (ms) -----
    // Seed the Crossfade slider from the frame so re-capture starts at its
    // current seam length. Remember the desired value (the source of truth that
    // survives the window-driven clamp); the trailing updateWindowLenControl
    // below re-caps the slider to the seeded mode's window via
    // syncCrossfadeMaxToWindow, so set crossfadeDesiredMs first.
    if (crossfadeMs >= 0.0) {
        crossfadeDesiredMs = juce::jmax(kXfadeMinMs, crossfadeMs);
        const double vis = juce::jlimit(crossfadeSlider.getMinimum(),
                                        crossfadeSlider.getMaximum(),
                                        crossfadeDesiredMs);
        crossfadeSlider.setValue(vis, juce::dontSendNotification);
    }

    // ----- Texture (grain count / FFT size) -----
    if (grainCount >= kGranularMinGrains) {
        grainCountSlider.setValue(
            (double)juce::jlimit(kGranularMinGrains, kGranularMaxGrains, grainCount),
            juce::dontSendNotification);
    }
    if (fftSize >= 0) {
        // 0 == Auto (combo id 1); otherwise pick the matching size's id.
        int id = 1;
        for (int i = 0; i < kNumSpectralFftSizes; ++i)
            if (kSpectralFftSizes[i] == fftSize) { id = i + 2; break; }
        fftSizeCombo.setSelectedId(id, juce::dontSendNotification);
    }
    // Reflect the seeded mode's relevance + push the seeded values to the engine
    // audition (the freeze onChange that would normally do this is suppressed by
    // dontSendNotification above).
    refreshFreezeExtras();
    // The seeded mode's per-method window multiplier may differ from the
    // construction-time default, so re-derive the auto window (no-op once the
    // user overrides it) and refresh the displayed control.
    syncWindowToAuto();
    updateWindowLenControl();
}

void CaptureFromPlaybackDialog::publishMetadataEdit() {
    if (!onMetadataEdited) return;  // append mode: no live frame bound
    const int fz = juce::jlimit(0, 3, freezeModeCombo.getSelectedId() - 1);
    // Report the user's DESIRED crossfade (the intent that survives the
    // window-driven clamp), not the visible slider value, so a frame whose seam
    // is temporarily clamped by a narrow window keeps its true length - mirrors
    // the song dialog's write-through contract.
    onMetadataEdited(capturedPitchHz, fz, crossfadeDesiredMs,
                     selectedGrainCount(), selectedFftSize());
}

std::vector<std::unique_ptr<IWavetableFrame>>
CaptureFromPlaybackDialog::buildFrames(int n) const {
    std::vector<std::unique_ptr<IWavetableFrame>> out;
    if (n <= 0 || tap.empty()) return out;
    n = std::min(n, 32);
    out.reserve((size_t)n);

    // Each emitted frame is a BANDED GranularFrame: its source PCM is one freeze
    // WINDOW (= "Window length") of the tap plus a half-window lookahead
    // tail for the loop seam, with windowStart = 0 / windowLen = the band so the
    // voice runs its clean per-mode banded math (Crossfade loops the window, the
    // grain-cloud modes roam inside it, Spectral analyses inside it). The grain
    // is the separate "Grain length" control, so the cloud modes get roam room
    // exactly like the wave editor. embeddedPitchHz comes from the in-dialog
    // pitch picker (Note + Octave, default A4 = 440 Hz) so MIDI playback at the
    // matching key plays the source at 1:1 and other keys pitch-shift via the
    // usual wavetable stride math.
    const double sr      = (tapSampleRate > 0.0) ? tapSampleRate : 48000.0;

    // Per-frame freeze-window (band) width and grain. For a single waveform the
    // window is the whole selection; for 2+ it is the "Window length"
    // slider (auto = mult x grain until overridden). The section bands are drawn
    // from the same window so the drawn bands and the captured frames agree.
    const int windowLen = effectiveSrcLen();
    // A grain must fit inside the freeze window. When the selection (and thus the
    // window) is smaller than the requested grain, cap the grain to the window so
    // the cloud modes still produce sound: a grain wider than the window makes the
    // engine floor the band width up to the grain, swallowing the source and
    // tripping its "band too short to roam" viability gate -> silent frame. This
    // matches what regenerateAuditionGrain publishes so capture sounds like the
    // preview. The modes that ignore the grain (Crossfade/Spectral) are unaffected.
    const int grainLen  = std::min(effectiveGrainLen(), windowLen);

    // Seam crossfade: the Crossfade slider's ms value (clamped to window/2 by the
    // player - the loop body is the window), matching the audition so capture
    // sounds like preview. Only audible in CrossfadeLoop, but baked into every
    // frame so a later mode switch in the editor already has the seam set.
    const int xfadeSamples = std::min(
        std::max(0, (int)std::round(crossfadeSlider.getValue() * 0.001 * sr)),
        std::max(0, windowLen / 2));

    for (int i = 0; i < n; ++i) {
        // Same slot geometry the section bands draw, so the captured band body
        // matches the on-screen bands exactly. buildGrainSource adds the
        // windowLen/2 lookahead tail past the band end for the seam.
        const int startIdx = bandStartForIndex(i, n, windowLen);
        std::vector<float> source = buildGrainSource(startIdx, windowLen);

        auto frame = std::make_unique<GranularFrame>(
            std::move(source), sr, grainLen, (float)capturedPitchHz,
            selectedFreezeMode(), xfadeSamples);
        // Banded: the band is the first windowLen samples of the source (the rest
        // is the seam lookahead). This is what gives the cloud modes roam room.
        frame->windowStart = 0;
        frame->windowLen   = windowLen;
        // Per-waveform texture: overlapping-grain count (cloud modes) and FFT
        // size (Spectral). Auto-ignored by the modes that don't use them.
        frame->grainCount = selectedGrainCount();
        frame->fftSize    = selectedFftSize();
        // Per-frame output gain (the in-dialog Gain control). Carried on the
        // GranularFrame's IWavetableFrame::gain so the synth's granular layer
        // scales playback by it (and the wave editor's Gain knob can adjust it
        // afterward). The source PCM stays at the recorded level - gain is a
        // separate, reversible scalar, not baked into the samples.
        frame->gain = (float)gainSlider.getValue();
        out.push_back(std::move(frame));
    }
    return out;
}

// ----- Region audition -------------------------------------------------
//
// Lets the user HEAR the selected slice before committing it to the
// library, reusing the same engine GrainLoop preview the capture-from-song
// dialog uses: the slice is published as a granular source buffer and
// looped through the engine's 4-voice OLA stream so the timbre evolves the
// way the captured frame will when it's triggered. Mic can only audition
// once the display is paused (canAudition): a live, sweeping ring buffer
// would shift under the loop on every timer tick.

bool CaptureFromPlaybackDialog::canAudition() const {
    if (tap.empty() || tapSampleRate <= 0.0) return false;
    if ((regionEnd - regionStart) < 64) return false;          // too short to loop
    if (source == CaptureSource::Mic)  return micPaused;        // need a stable buffer
    if (source == CaptureSource::File) return fileLoaded;
    return false;  // Playback has no audition button
}

void CaptureFromPlaybackDialog::regenerateAuditionGrain() {
    auto* eng = AudioEngine::getInstance();
    if (!eng) return;
    if (tap.empty() || tapSampleRate <= 0.0) return;

    const int    regionLen = std::max(0, regionEnd - regionStart);
    if (regionLen < 64) return;
    const double sr        = tapSampleRate;

    // Audition the user-selected frame using the exact geometry buildFrames
    // bakes, so the preview is honest: the same banded source (window + seam
    // lookahead), grain, and per-method window the captured frame will carry.
    // The single-waveform case spans the whole selection (you hear the recorded
    // sound); the multi-waveform case auditions whichever waveform the "Preview
    // waveform" picker selects.
    const int n        = (int)std::round(numFramesSlider.getValue());
    const int windowLen = effectiveSrcLen();
    if (windowLen <= 0) return;
    const int grainLen = effectiveGrainLen();

    // 0-based band index. With a single waveform there is only band 0; with
    // 2+ the "Preview waveform" slider (1-based) chooses, clamped into range.
    const int repIdx   = (n <= 1)
        ? 0
        : juce::jlimit(0, n - 1,
                       (int)std::round(previewIndexSlider.getValue()) - 1);
    const int startIdx = bandStartForIndex(repIdx, n, windowLen);
    auto src = std::make_shared<std::vector<float>>(buildGrainSource(startIdx, windowLen));

    // Bake the dialog's output gain into this throwaway preview buffer so the
    // audition loudness matches what the captured frame will play at. The
    // captured frame instead carries the gain as IWavetableFrame::gain (see
    // buildFrames) which the synth applies; the engine's preview reader has no
    // per-frame gain knob, so the preview applies it to the PCM directly.
    const float previewGain = (float)gainSlider.getValue();
    if (previewGain != 1.0f)
        for (auto& s : *src) s *= previewGain;

    // Pitch ratio: play the slice "as A4" so the audition matches the
    // editor's reference-note audition and a synth note at A4 (440 /
    // capturedPitchHz). Mirrors CaptureFromSongDialog::publishPreviewPitch.
    const double hz = (capturedPitchHz > 0.0) ? capturedPitchHz : 440.0;
    // srRatio corrects a source captured at a rate other than the graph's
    // processing rate. The synth-voice path that plays the BAKED frame
    // multiplies the "as A4" ratio by srcSampleRate/deviceRate
    // (terrain_synth.cpp renderGrainSample). The engine preview plays this
    // throwaway buffer assuming srcRate == deviceRate, so without the same
    // factor a frame captured at e.g. 44.1 kHz and auditioned through a 48 kHz
    // graph sounds ~1.5 semitones sharp here but correct once placed - the
    // "plays a few notes lower in the editor than in the capture preview" bug.
    const double devRate = eng->getSampleRate();
    const double srRatio = (devRate > 0.0) ? (sr / devRate) : 1.0;
    eng->setPreviewGrainRatio((float)((440.0 / hz) * srRatio));
    // Publish the source's natural pitch so PitchSyncGrains can derive the
    // loop period; the other freeze modes ignore it.
    eng->setPreviewEmbeddedPitch((float)hz);
    // Banded preview, identical to the captured frame: the band is the first
    // windowLen samples (windowStart = 0), the grain is the separate grain
    // control. The published grain length is clamped to the WINDOW, not just the
    // source: a grain must fit inside the freeze window (the engine floors the
    // band width up to the grain length, so a grain wider than the window would
    // swallow the whole source and trip the "band too short to roam" viability
    // gate -> silence). When the selection (and thus the window) is smaller than
    // the requested grain, this caps the grain to the window so the cloud modes
    // still sound. The modes that ignore the grain are unaffected (they use the
    // band, not the grain).
    eng->setPreviewGrainLength(std::min(grainLen, windowLen));
    eng->setPreviewWindowStart(0);
    eng->setPreviewWindowLen(windowLen);
    // Seam crossfade from the Crossfade slider, matching buildFrames; engine
    // clamps to window/2.
    const int xfadeSamples = std::min(
        std::max(0, (int)std::round(crossfadeSlider.getValue() * 0.001 * sr)),
        std::max(0, windowLen / 2));
    eng->setPreviewCrossfadeLength(xfadeSamples);
    // Re-publish the texture choices on every respin so a restart after a
    // clearPreview still auditions at the user's grain count / FFT size.
    eng->setPreviewGrainCount(selectedGrainCount());
    eng->setPreviewFftSize(selectedFftSize());
    eng->setPreviewGrainBuffer(std::move(src));
}

void CaptureFromPlaybackDialog::startRegionAudition() {
    auto* eng = AudioEngine::getInstance();
    if (!eng || !canAudition()) { updatePreviewButton(); return; }
    eng->setGrainFreezeMode(selectedFreezeMode());
    regenerateAuditionGrain();
    eng->setPreviewMode(AudioEngine::PreviewMode::GrainLoop);
    auditioning = true;
    updatePreviewButton();
}

void CaptureFromPlaybackDialog::stopRegionAudition() {
    if (auto* eng = AudioEngine::getInstance()) eng->clearPreview();
    auditioning = false;
    updatePreviewButton();
}

void CaptureFromPlaybackDialog::updatePreviewButton() {
    // Playback has no preview button (not reachable from the menu).
    if (source != CaptureSource::Mic && source != CaptureSource::File) return;
    const bool ok = canAudition();
    previewBtn.setButtonText(auditioning ? "Stop" : "Preview");
    previewBtn.setEnabled(ok || auditioning);
    previewBtn.setColour(juce::TextButton::buttonColourId,
                         auditioning ? juce::Colour(120, 60, 60)    // red-ish = playing
                                     : juce::Colour(60, 90, 110));  // blue = idle
    if (auditioning) {
        previewBtn.setTooltip("Stop the preview loop.");
    } else if (ok) {
        previewBtn.setTooltip("Loop the selected region so you can hear the slice "
                              "before capturing it. It also plays automatically while "
                              "you drag the region handles.");
    } else if (source == CaptureSource::Mic && !micPaused) {
        previewBtn.setTooltip("Press Pause first - a live, moving input can't be "
                              "auditioned. Once the display is paused you can preview "
                              "the selected slice.");
    } else if (source == CaptureSource::File && !fileLoaded) {
        previewBtn.setTooltip("Load a file first, then select a region to preview it.");
    } else {
        previewBtn.setTooltip("Select a region (at least a few milliseconds wide) to "
                              "preview it.");
    }
}

// =================================================================
//  CaptureFromSongDialog
// =================================================================

// Find the project's Output node, if any. The song-render cache lives on
// this node, populated either by a previous real-time playback (via
// AudioEngine::stop dumping the live capture into the cache) or by a
// previous CaptureFromSongDialog open that ran its own offline render.
// Returns nullptr if the project has no Output node.
static Node* findOutputNode(NodeGraph& graph) {
    for (auto& n : graph.nodes)
        if (n.type == NodeType::Output)
            return &n;
    return nullptr;
}

// Try to satisfy the song-render request from the Output node's existing
// cache. If the cache is present and the project's hash matches what the
// cache was produced from, build a mono PCM buffer from cache.left/right
// and return it. Returns nullptr on any miss (no cache, hash mismatch,
// empty samples, sample-rate mismatch with the engine's device rate).
static std::shared_ptr<std::vector<float>>
trySongCache(NodeGraph& graph, double expectedSampleRate, double& sampleRateOut) {
    Node* out = findOutputNode(graph);
    if (!out) return nullptr;
    auto& c = out->cache;
    if (!c.valid || c.numSamples <= 0 || c.left.empty()) return nullptr;

    auto* eng = AudioEngine::getInstance();
    if (!eng) return nullptr;
    auto& mgr = eng->getGraphProcessor().getCacheManager();

    // Make sure deterministic flags reflect current graph state, then
    // recompute the project hash. A 0 hash means the project isn't
    // cacheable (live MIDI CC bindings somewhere upstream); in that
    // case we never trust the cache - it would be stale by definition.
    mgr.updateDeterminism(graph);
    const uint64_t h = mgr.computeNodeHash(*out, graph);
    if (h == 0 || h != c.inputHash) return nullptr;

    // Sample-rate mismatch (e.g. user swapped audio device between
    // sessions). The grain stream and song playhead are driven by the
    // engine's current device rate, and resampling on the fly would add
    // complexity for a corner case - just re-render.
    if (std::abs(c.sampleRate - expectedSampleRate) > 0.5) return nullptr;

    // Build mono PCM by averaging the cached stereo channels.
    auto pcm = std::make_shared<std::vector<float>>((size_t)c.numSamples, 0.0f);
    const bool hasR = (int64_t)c.right.size() >= c.numSamples;
    for (int64_t i = 0; i < c.numSamples; ++i) {
        const float l = c.left[(size_t)i];
        const float r = hasR ? c.right[(size_t)i] : l;
        (*pcm)[(size_t)i] = 0.5f * (l + r);
    }
    sampleRateOut = c.sampleRate;
    return pcm;
}

// Stash the freshly-rendered song PCM into the Output node's cache so a
// subsequent dialog open (or a Capture-button bounce from the cache) can
// skip the render. The cache stores stereo; we duplicate mono into both
// channels. Skips silently if hashing the project produces 0 (a non-
// deterministic graph can't safely be cached - any sample we store now
// might disagree with a future render).
static void writeSongCache(NodeGraph& graph,
                            const std::shared_ptr<std::vector<float>>& pcm,
                            double sampleRate) {
    if (!pcm || pcm->empty()) return;
    Node* out = findOutputNode(graph);
    if (!out) return;
    auto* eng = AudioEngine::getInstance();
    if (!eng) return;
    auto& mgr = eng->getGraphProcessor().getCacheManager();
    mgr.updateDeterminism(graph);
    const uint64_t h = mgr.computeNodeHash(*out, graph);
    if (h == 0) return;  // non-deterministic - don't cache

    auto& c = out->cache;
    c.left  = *pcm;          // duplicate mono into both channels
    c.right = *pcm;
    c.sampleRate = sampleRate;
    c.numSamples = (int64_t)pcm->size();
    c.startSample = 0;
    c.useDisk = false;
    c.diskPath.clear();
    c.inputHash = h;
    c.valid = true;
}

// Background render job. Runs the offline GraphProcessor over the project
// at the device sample rate, mono-mixes the result, and exposes it via
// `result` once `done` flips true. Same pattern as MainContentComponent::
// doExportRender, factored into a juce::Thread so the dialog stays
// interactive (showing a progress strip in the wave area) while the
// render proceeds, instead of blocking under a modal progress window.
class CaptureFromSongDialog::RenderJob : public juce::Thread {
public:
    RenderJob(NodeGraph& graphRef,
              Transport& liveTransport,
              double targetSampleRate,
              float maxBeatIn)
        : juce::Thread("CaptureSongRender"),
          graph(graphRef),
          maxBeat(maxBeatIn)
    {
        offTransport.bpm        = graphRef.bpm;
        offTransport.tempoMap   = liveTransport.tempoMap;
        offTransport.timeSigMap = liveTransport.timeSigMap;
        offTransport.sampleRate = targetSampleRate;
        offTransport.playing    = true;
        sampleRate = targetSampleRate;
    }

    void run() override {
        const int blockSize = 512;
        const double totalSeconds = offTransport.tempoMap.beatsToSeconds(maxBeat);
        const int64_t totalSamples = (int64_t)(totalSeconds * sampleRate);
        if (totalSamples <= 0) {
            done.store(true);
            return;
        }

        GraphProcessor offGP;
        offGP.prepare(graph, sampleRate, blockSize);
        offGP.rebuildGraph(graph, offTransport);
        offGP.prepare(graph, sampleRate, blockSize);

        // Stereo intermediate buffer (graph processor outputs stereo);
        // we mono-mix into `pcm` block by block to keep peak memory low.
        juce::AudioBuffer<float> stereo(2, blockSize);
        auto pcm = std::make_shared<std::vector<float>>((size_t)totalSamples, 0.0f);

        for (int64_t pos = 0; pos < totalSamples; pos += blockSize) {
            if (threadShouldExit()) {
                done.store(true);
                return;
            }
            int thisBlock = (int)std::min((int64_t)blockSize, totalSamples - pos);
            stereo.clear();
            offTransport.positionSamples = pos;
            float* outPtrs[2] = {
                stereo.getWritePointer(0),
                stereo.getWritePointer(1)
            };
            offGP.processBlock(graph, offTransport, outPtrs, 2, thisBlock);

            const float* l = stereo.getReadPointer(0);
            const float* r = stereo.getReadPointer(1);
            for (int s = 0; s < thisBlock; ++s)
                (*pcm)[(size_t)(pos + s)] = 0.5f * (l[s] + r[s]);

            progress.store((double)pos / (double)totalSamples);
        }

        result = std::move(pcm);
        progress.store(1.0);
        done.store(true);
    }

    NodeGraph& graph;
    Transport  offTransport;
    double     sampleRate = 0.0;
    float      maxBeat;

    std::atomic<double> progress { 0.0 };
    std::atomic<bool>   done     { false };
    std::shared_ptr<std::vector<float>> result;
};

CaptureFromSongDialog::CaptureFromSongDialog(NodeGraph& g, Transport& t,
                                              int ts, OnCapture onCap)
    : graph(g),
      transport(t),
      tableSize(std::max(64, ts)),
      onCapture(std::move(onCap))
{
    // Height sized for the full region/N-waveform control stack (waveforms,
    // grain, samples-per-waveform + fit, crossfade, preview-waveform picker,
    // pitch + gain, freeze + texture extras), the transport + zoom/scroll rows,
    // and the status / hint lines, while keeping a usable waveform area.
    setSize(820, 772);

    // ----- Transport row -----
    addAndMakeVisible(playBtn);
    playBtn.setTooltip(
        "Play the whole rendered song from the start of the selected region at "
        "full fidelity, with a moving playhead. Use this to find the part you "
        "want; then pause or drag a handle to lock in the region and audition "
        "the slice you'll capture.");
    playBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(60, 110, 70));
    playBtn.onClick = [this]() { setState(TState::Playing); };

    addAndMakeVisible(pauseBtn);
    pauseBtn.setTooltip(
        "Pause playback and switch to the audition loop: the selected waveform "
        "(the \"Preview waveform\" slice of the region) loops continuously so "
        "you can hear what would be captured. Drag the orange handles to reshape "
        "the region while you listen.");
    pauseBtn.onClick = [this]() { setState(TState::Paused); };

    addAndMakeVisible(stopBtn);
    stopBtn.setTooltip("Stop playback and silence the audition. The region "
                       "selection is kept.");
    stopBtn.onClick = [this]() { setState(TState::Stopped); };

    // ----- Waveforms-to-slice-out slider -----
    addAndMakeVisible(numFramesLabel);
    numFramesLabel.setText("Waveforms to slice out:", juce::dontSendNotification);
    numFramesLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(numFramesSlider);
    numFramesSlider.setRange(1.0, 32.0, 1.0);
    numFramesSlider.setValue((double)captureDialogPrefs().numFrames,
                             juce::dontSendNotification);
    lastNumFrames = (int)std::round(numFramesSlider.getValue());
    numFramesSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    numFramesSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
    numFramesSlider.setTooltip(
        "Slices the selected region into this many short snapshots. The "
        "wavetable morphs through them as its Position parameter sweeps from "
        "0 to 1, so the captured sound evolves the way the song did across the "
        "selection. 1 = a single frozen snapshot; more = smoother evolution "
        "but more memory. Changing this resizes the SELECTION in proportion "
        "(keeping each waveform's length fixed), so adding waveforms grows the "
        "selection rather than cramming more into the same span.");
    numFramesSlider.onValueChange = [this]() {
        const int newN = (int)std::round(numFramesSlider.getValue());
        // Resize the selection in proportion so each waveform's window length
        // (and slot spacing) stays constant - adding waveforms grows the
        // selection rather than shrinking the per-waveform length.
        resizeSelectionForCountChange(lastNumFrames, newN);
        lastNumFrames = newN;
        clampSelectionToView();   // keep the (grown) region inside the zoom view
        syncWindowToAuto();
        updateWindowLenControl();
        reconfigureZoomForWindow();
        updatePreviewIndexControl();
        updateRegionInfoLabel();
        if (state == TState::Paused || state == TState::Scrubbing)
            regenerateAuditionGrain();
        repaint(waveRect);
    };

    // ----- Grain length (ms) -----
    addAndMakeVisible(grainLenLabel);
    grainLenLabel.setText("Grain length (ms):", juce::dontSendNotification);
    grainLenLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(grainLenSlider);
    grainLenSlider.setRange(1.0, 500.0, 0.5);  // sub-5 ms allowed: very short
                                               // grains give the smoothest,
                                               // most constant cloud
    grainLenSlider.setValue(captureDialogPrefs().grainMs, juce::dontSendNotification);
    grainUserSet = captureDialogPrefs().grainUserSet;
    grainLenSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    grainLenSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
    grainLenSlider.setTooltip(
        "Length of each overlapping grain inside a captured waveform, in "
        "milliseconds. Each grain-cloud mode opens at the grain that sounds most "
        "CONSTANT for it - ~5 ms for Async granular (many tiny grains blur into a "
        "steady cloud), ~40 ms for Pitch-sync grains (needs a few pitch periods "
        "per grain to lock the pitch). Longer grains roam over a proportionally "
        "wider window and so sound more evolving / less constant; very short "
        "(~1 ms) gets buzzy. Once you move this slider it stays where you put it "
        "(no more auto-snapping on mode change). The freeze window (\"Window "
        "length\" below) auto-tracks 4x the grain for the grain-cloud modes so "
        "they have room to roam, until you drag the window yourself. Only Async "
        "and Pitch-sync grains use the grain; Crossfade loop and Spectral freeze "
        "ignore it and use a fixed default window instead.");
    grainLenSlider.onValueChange = [this]() {
        grainUserSet = true;   // manual move pins the grain (no more mode-snapping)
        syncWindowToAuto();
        updateWindowLenControl();
        updateRegionInfoLabel();
        if (state == TState::Paused || state == TState::Scrubbing)
            regenerateAuditionGrain();
        repaint(waveRect);
    };

    // ----- Per-waveform freeze WINDOW (ms) + Fit button -----
    addAndMakeVisible(windowLenLabel);
    windowLenLabel.setText("Window length (ms):", juce::dontSendNotification);
    windowLenLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(windowLenSlider);
    // In milliseconds (same unit as grain / crossfade). effectiveSrcLen()
    // converts to samples against the song rate and caps to the selection.
    windowLenSlider.setRange(kWindowMinMs, kWindowMaxMs, 1.0);
    windowLenSlider.setValue(1000.0, juce::dontSendNotification);
    windowLenSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    windowLenSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 22);
    windowLenSlider.onValueChange = [this]() {
        windowUserSet = true;
        syncCrossfadeMaxToWindow();
        // A wider window means the view can't zoom in as far - keep the zoom
        // reach (and any over-zoomed view) in step with the new window.
        reconfigureZoomForWindow();
        updateRegionInfoLabel();
        if (state == TState::Paused || state == TState::Scrubbing)
            regenerateAuditionGrain();
        repaint(waveRect);
    };
    addAndMakeVisible(fitWidthBtn);
    fitWidthBtn.onClick = [this]() {
        syncWindowToFitSlots();
        syncCrossfadeMaxToWindow();
        reconfigureZoomForWindow();   // Fit can grow the window -> re-range zoom
        updateRegionInfoLabel();
        if (state == TState::Paused || state == TState::Scrubbing)
            regenerateAuditionGrain();
        repaint(waveRect);
    };
    syncWindowToAuto();
    updateWindowLenControl();

    // ----- Region-geometry readout -----
    addAndMakeVisible(regionInfoLabel);
    regionInfoLabel.setJustificationType(juce::Justification::centredLeft);
    regionInfoLabel.setFont(juce::Font(juce::FontOptions(13.0f)));
    regionInfoLabel.setColour(juce::Label::textColourId, juce::Colour(180, 180, 200));

    // ----- Output gain -----
    addAndMakeVisible(gainLabel);
    gainLabel.setText("Gain:", juce::dontSendNotification);
    gainLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(gainSlider);
    gainSlider.setRange(0.0, 4.0, 0.01);
    gainSlider.setValue(captureDialogPrefs().gain, juce::dontSendNotification);
    gainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    gainSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 22);
    gainSlider.setDoubleClickReturnValue(true, 1.0);
    gainSlider.setTooltip(
        "Output level for the captured waveforms. 1.0 = the rendered level; "
        "below 1 makes them quieter, above 1 louder (up to 4x). Applies to "
        "every sliced waveform and is reflected in the audition. This sets the "
        "same per-waveform gain you can fine-tune later with the Gain knob in "
        "the wave editor.");
    gainSlider.onValueChange = [this]() {
        if (state == TState::Paused || state == TState::Scrubbing)
            regenerateAuditionGrain();
    };

    // ----- Preview-waveform picker -----
    addAndMakeVisible(previewIndexLabel);
    previewIndexLabel.setText("Preview waveform:", juce::dontSendNotification);
    previewIndexLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(previewIndexSlider);
    previewIndexSlider.setRange(1.0, 1.0, 1.0);   // widened by updatePreviewIndexControl
    previewIndexSlider.setValue(1.0, juce::dontSendNotification);
    previewIndexSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    previewIndexSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
    previewIndexSlider.onValueChange = [this]() {
        if (state == TState::Paused || state == TState::Scrubbing)
            regenerateAuditionGrain();
    };
    updatePreviewIndexControl();

    // ----- Crossfade slider -----
    addAndMakeVisible(crossfadeLabel);
    crossfadeLabel.setText("Crossfade:", juce::dontSendNotification);
    crossfadeLabel.setJustificationType(juce::Justification::centredRight);

    addAndMakeVisible(crossfadeSlider);
    crossfadeSlider.setRange(kXfadeMinMs, kXfadeMaxMs, 1.0);
    crossfadeSlider.setValue(kXfadeDefMs, juce::dontSendNotification);
    crossfadeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    crossfadeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 22);
    crossfadeSlider.setTextValueSuffix(" ms");
    crossfadeSlider.setTooltip(
        "Crossfade-loop seam length in milliseconds. The loop blends "
        "the end of the window into the beginning over this duration "
        "to hide the seam click and smooth out the repetition. Short "
        "(1-10 ms): tight, the loop period is most audible. Long "
        "(close to the cap = half the freeze window): the whole loop is "
        "essentially a rolling crossfade between two playheads, "
        "smoother but more blurred. The slider's max tracks half the "
        "\"Window length\" window because the engine can't overlap "
        "two ramps longer than that within one loop. Effective only with "
        "Freeze = Crossfade loop.");
    crossfadeSlider.onValueChange = [this]() {
        // Only treat this as the user setting their desired crossfade
        // when it's NOT a synthetic notification from setRange clamping
        // the value during a freeze-window change. The guard is flipped on
        // exactly around syncCrossfadeMaxToWindow's slider mutations.
        if (!syncingCrossfadeFromWindow)
            crossfadeDesiredMs = crossfadeSlider.getValue();
        updateStatusLabel();
        if (state == TState::Paused || state == TState::Scrubbing)
            regenerateAuditionGrain();
        // Unified save model: write the crossfade through to the live frame in
        // re-capture mode - but only for genuine user moves, not the synthetic
        // value the window-clamp injects (that would clobber the frame's xfade
        // when the user only touched the window, a capture-time param). No-op in
        // append mode.
        if (!syncingCrossfadeFromWindow)
            publishMetadataEdit();
    };
    // Initial range: cap at half the freeze window. The window controls have
    // been configured above, so this is the right moment to mirror the window
    // into the crossfade max.
    syncCrossfadeMaxToWindow();

    // ----- Freeze-mode picker -----
    addAndMakeVisible(freezeModeLabel);
    freezeModeLabel.setText("Freeze:", juce::dontSendNotification);
    freezeModeLabel.setJustificationType(juce::Justification::centredRight);

    addAndMakeVisible(freezeModeCombo);
    // ComboBox IDs are 1-based; we map them to enum values via -1. All four
    // algorithms are implemented in the shared GrainFreezeVoice
    // (granular_freeze.cpp), and the same reader drives both this audition and
    // the held synth note, so what you A/B here is exactly what you'll play.
    freezeModeCombo.addItem("Crossfade loop",
                            (int)GranularFreezeMode::CrossfadeLoop + 1);
    freezeModeCombo.addItem("Async granular (blur)",
                            (int)GranularFreezeMode::AsyncGranular + 1);
    freezeModeCombo.addItem("Pitch-sync grains",
                            (int)GranularFreezeMode::PitchSyncGrains + 1);
    freezeModeCombo.addItem("Spectral freeze",
                            (int)GranularFreezeMode::SpectralFreeze + 1);
    freezeModeCombo.setSelectedId(
        (int)GranularFreezeMode::CrossfadeLoop + 1,
        juce::dontSendNotification);
    freezeModeCombo.setTooltip(
        "How the loop sustains the selected region while a captured note is "
        "held. The source pitch is baked in; held notes resample the result "
        "to your MIDI pitch (sampler-style).\n"
        " - Crossfade loop: faithful tape loop with a short fade across "
        "the seam. \"What does this spot literally sound like.\"\n"
        " - Async granular: many short grains at randomised positions in a "
        "small range. Frozen blur / GRM-Freeze texture.\n"
        " - Pitch-sync grains: loops exactly one detected pitch period so the "
        "output is a clean sustained tone. Works best on a pitched source.\n"
        " - Spectral freeze: keeps the FFT magnitudes and regenerates phases "
        "each frame. Ethereal pad sustain.\n"
        "Also drives the audition you're hearing right now, so you can "
        "A/B them before saving.");
    freezeModeCombo.onChange = [this]() {
        const int idx = freezeModeCombo.getSelectedId() - 1;
        if (idx < 0) return;
        if (auto* eng = AudioEngine::getInstance())
            eng->setGrainFreezeMode((AudioEngine::GrainFreezeMode)idx);
        // Snap the grain to the new mode's default (5 ms Async / 40 ms Pitch-sync)
        // so each grain-cloud mode opens at a constant-sounding grain, unless the
        // user has already picked a grain. Non-grain modes leave the grain alone.
        const GranularFreezeMode newMode = selectedFreezeMode();
        if (!grainUserSet
            && (newMode == GranularFreezeMode::AsyncGranular
                || newMode == GranularFreezeMode::PitchSyncGrains)) {
            grainLenSlider.setValue(defaultGrainMsForMode(newMode),
                                    juce::dontSendNotification);
        }
        // The grain count / FFT size relevance changes with the mode.
        refreshFreezeExtras();
        // The per-method auto window multiplier changes with the mode (4x for
        // the cloud modes, 1x otherwise), so re-derive the auto window unless
        // the user has overridden it, then refresh the displayed control + bands.
        syncWindowToAuto();
        updateWindowLenControl();
        updateRegionInfoLabel();
        repaint(waveRect);
        // Re-spin the audition so the new mode reanchors on the current slice.
        // No-op if not currently in GrainLoop (Paused / Scrubbing).
        if (state == TState::Paused || state == TState::Scrubbing)
            regenerateAuditionGrain();
        // Unified save model: write the freeze mode through to the live frame
        // in re-capture mode. No-op in append.
        publishMetadataEdit();
    };

    // ----- Per-waveform texture controls (grain count / FFT size) -----
    addAndMakeVisible(grainCountLabel);
    grainCountLabel.setText("Grains per waveform:", juce::dontSendNotification);
    grainCountLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(grainCountSlider);
    grainCountSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    grainCountSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 22);
    grainCountSlider.setRange((double)kGranularMinGrains,
                              (double)kGranularMaxGrains, 1.0);
    grainCountSlider.setValue(4.0, juce::dontSendNotification);  // historical default
    grainCountSlider.onValueChange = [this]() {
        if (auto* eng = AudioEngine::getInstance())
            eng->setPreviewGrainCount(selectedGrainCount());
        if (state == TState::Paused || state == TState::Scrubbing)
            regenerateAuditionGrain();
        // Unified save model: write the grain count through to the live frame
        // in re-capture mode. No-op in append.
        publishMetadataEdit();
    };

    addAndMakeVisible(fftSizeLabel);
    fftSizeLabel.setText("FFT size per waveform:", juce::dontSendNotification);
    fftSizeLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(fftSizeCombo);
    fftSizeCombo.addItem("Auto", 1);
    for (int i = 0; i < kNumSpectralFftSizes; ++i)
        fftSizeCombo.addItem(juce::String(kSpectralFftSizes[i]), i + 2);
    fftSizeCombo.setSelectedId(1, juce::dontSendNotification);  // Auto
    fftSizeCombo.onChange = [this]() {
        if (auto* eng = AudioEngine::getInstance())
            eng->setPreviewFftSize(selectedFftSize());
        if (state == TState::Paused || state == TState::Scrubbing)
            regenerateAuditionGrain();
        // Unified save model: write the FFT size through to the live frame in
        // re-capture mode. No-op in append.
        publishMetadataEdit();
    };
    refreshFreezeExtras();

    // ----- Embedded pitch picker -----
    // Sets the captured frame's embeddedPitchHz so MIDI playback at the
    // matching key plays the source at 1:1; other keys pitch-shift via
    // the synth's usual wavetable stride math. Mirrors the picker in the
    // post-capture GranularFrameEditorComponent.
    setUpPitchPicker(*this, noteOctaveLabel, noteCombo, octaveCombo, centsLabel);
    auto onPitchPickerChanged = [this]() {
        const int n = noteCombo.getSelectedId() - 1;
        const int o = octaveCombo.getSelectedId() - 1;
        if (n < 0 || n > 11 || o < 0 || o > 9) return;
        capturedPitchHz = noteOctaveToHz(n, o);
        centsLabel.setText(formatCents(capturedPitchHz),
                           juce::dontSendNotification);
        // Retune the live audition immediately so picking "As note" is audible
        // right away, matching what the synth voice will play.
        publishPreviewPitch();
        // Unified save model: in re-capture mode, write the new pitch label
        // straight through to the live frame so closing the panel can't lose
        // it (matches the frame editor's per-edit commit). No-op in append.
        publishMetadataEdit();
    };
    noteCombo.onChange   = onPitchPickerChanged;
    octaveCombo.onChange = onPitchPickerChanged;
    // Restore the last labelled pitch from the sticky session prefs (overrides
    // setUpPitchPicker's A4 default), then recompute the derived Hz / cents and
    // push them to the audition - matching the file dialog.
    noteCombo.setSelectedId(captureDialogPrefs().noteId, juce::dontSendNotification);
    octaveCombo.setSelectedId(captureDialogPrefs().octaveId, juce::dontSendNotification);
    onPitchPickerChanged();

    // ----- Status / hint labels -----
    addAndMakeVisible(statusLabel);
    statusLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setFont(juce::Font(juce::FontOptions(13.0f)));
    statusLabel.setColour(juce::Label::textColourId, juce::Colour(180, 180, 200));

    addAndMakeVisible(hintLabel);
    hintLabel.setText(
        "Play the song to find the part you want, then drag the orange handles "
        "to select a region. The capture slices it into the chosen number of "
        "waveforms (the orange bands) and the wavetable's Position parameter "
        "morphs through them. Pause or drag a handle to audition the selected "
        "waveform before capturing.",
        juce::dontSendNotification);
    hintLabel.setJustificationType(juce::Justification::topLeft);
    hintLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    hintLabel.setColour(juce::Label::textColourId, juce::Colour(160, 160, 180));

    // ----- Capture / Close -----
    addAndMakeVisible(saveBtn);
    saveBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(60, 110, 70));
    saveBtn.onClick = [this]() {
        const int n = (int)std::round(numFramesSlider.getValue());
        auto frames = buildFrames(n);
        if (onCapture && !frames.empty()) onCapture(std::move(frames));
        // Keep the dialog open after capturing so the user can continue
        // selecting more regions. The audition keeps running.
        updateStatusLabel();
    };

    addAndMakeVisible(cancelBtn);
    cancelBtn.onClick = [this]() {
        // Close path: when embedded (onDismiss set), the host clears the
        // capture panel from the right pane. When launched as a
        // DialogWindow (onDismiss empty), walk up and exit modal state.
        if (onDismiss) {
            onDismiss();
        } else if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
            dw->exitModalState(0);
        }
    };

    // ----- Zoom / scroll view window -----
    // Two sliders let the user zoom into and scroll along a long song so the
    // region can be placed precisely. The real zoom range is set once the
    // render completes and the song length is known (configureViewSlidersForBuffer).
    addAndMakeVisible(scrollLabel);
    scrollLabel.setText("Scroll:", juce::dontSendNotification);
    scrollLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(scrollSlider);
    scrollSlider.setRange(0.0, 1.0, 0.0);
    scrollSlider.setValue(0.0, juce::dontSendNotification);
    scrollSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    scrollSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    scrollSlider.setEnabled(false);
    scrollSlider.setTooltip(
        "Slide the view window left or right along the song. Has no effect until "
        "you zoom in. The region scrolls with the audio, staying pegged to a view "
        "edge once it reaches one.");
    scrollSlider.onValueChange = [this]() {
        viewScroll = scrollSlider.getValue();
        onViewChanged();
    };
    addAndMakeVisible(zoomLabel);
    zoomLabel.setText("Zoom:", juce::dontSendNotification);
    zoomLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(zoomSlider);
    zoomSlider.setRange(1.0, 2.0, 0.0);   // real max set per-song after render
    zoomSlider.setValue(1.0, juce::dontSendNotification);
    zoomSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    zoomSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    zoomSlider.setEnabled(false);
    zoomSlider.setTooltip(
        "Zoom the waveform view in or out around the current scroll position. "
        "Zooming in lets you place the region precisely inside a long song.");
    zoomSlider.onValueChange = [this]() {
        viewZoom = zoomSlider.getValue();
        onViewChanged();
    };

    // ----- Kick off the offline render -----
    // Figure out song length the same way doExportRender does: graph
    // contentEndBeats() (max end of clip over all timeline nodes, including
    // each track's cascading nesting offset); add a 4-beat tail so reverb /
    // release rings out. If the project has no clips, fall back to 4
    // beats so the user gets *something* (silent) to scrub.
    float maxBeat = (float) graph.contentEndBeats();
    if (maxBeat <= 0.0f) maxBeat = 4.0f;
    maxBeat += 4.0f;

    double targetSampleRate = 44100.0;
    if (auto* eng = AudioEngine::getInstance())
        targetSampleRate = std::max(8000.0, eng->getDeviceSampleRate());
    songSampleRate = targetSampleRate;

    // Cache hit? If the project hasn't changed since the last time the
    // Output node's cache was populated (via Play->Stop or a previous
    // open of this dialog), reuse that PCM and skip the offline render
    // entirely. Important: we still want the dialog to behave like the
    // render-completion path for the rest of setup, so we defer the
    // "ready" handoff to the first timer tick instead of doing it
    // inline here - the JUCE component isn't fully laid out yet at
    // constructor time.
    double cachedSr = 0.0;
    if (auto cached = trySongCache(graph, targetSampleRate, cachedSr)) {
        songPcm = std::move(cached);
        songSampleRate = cachedSr;
        // No renderJob - timerCallback's "render done?" branch will see
        // renderJob==null+songPcm non-null and route through
        // onRenderComplete on the first tick.
    } else {
        renderJob = std::make_unique<RenderJob>(graph, transport, targetSampleRate, maxBeat);
        renderJob->startThread(juce::Thread::Priority::normal);
    }

    // Sync the engine's audition freeze-mode and crossfade length to
    // whatever the combo/slider defaults are, so the first paused
    // audition matches the UI without requiring the user to touch a
    // control. Crossfade is in ms here; convert via the song sample
    // rate that's already been resolved above.
    if (auto* eng = AudioEngine::getInstance()) {
        eng->setGrainFreezeMode(GranularFreezeMode::CrossfadeLoop);
        const int xfadeSamples = std::max(0,
            (int)std::round(kXfadeDefMs * 0.001 * songSampleRate));
        eng->setPreviewCrossfadeLength(xfadeSamples);
    }

    updateStatusLabel();
    updateButtonsForState();
    startTimerHz(30);
}

CaptureFromSongDialog::~CaptureFromSongDialog() {
    stopTimer();

    // Remember the user's last settings so the next open of either capture
    // dialog restores them (shared session prefs - see captureDialogPrefs()).
    {
        auto& p = captureDialogPrefs();
        p.numFrames   = (int)std::round(numFramesSlider.getValue());
        p.gain        = gainSlider.getValue();
        p.noteId      = noteCombo.getSelectedId();
        p.octaveId    = octaveCombo.getSelectedId();
        p.grainMs     = grainLenSlider.getValue();
        p.grainUserSet = grainUserSet;
        p.crossfadeMs = crossfadeDesiredMs;
    }

    // Tear the engine preview down before we let the buffers go out of
    // scope. setPreviewMode(Off) plus the buffer clear means subsequent
    // audio blocks emit silence; the engine's audio-thread-cached
    // shared_ptrs hold the data alive until the next block reads the
    // nullptrs.
    if (auto* eng = AudioEngine::getInstance())
        eng->clearPreview();

    if (renderJob) {
        renderJob->signalThreadShouldExit();
        renderJob->stopThread(2000);
        renderJob.reset();
    }
}

void CaptureFromSongDialog::resized() {
    auto r = getLocalBounds().reduced(12);

    // Title strip painted in paint(); reserve gutter.
    r.removeFromTop(26);

    // Capture / Close button row at the very bottom.
    auto bottomRow = r.removeFromBottom(34);
    cancelBtn.setBounds(bottomRow.removeFromRight(100));
    bottomRow.removeFromRight(8);
    saveBtn.setBounds(bottomRow.removeFromRight(200));

    r.removeFromBottom(8);

    // "Preview waveform" selector row (which slice the Paused audition loops),
    // directly above the buttons. Removed first so it reads bottom-most.
    {
        auto prevRow = r.removeFromBottom(26);
        previewIndexLabel.setBounds(prevRow.removeFromLeft(160));
        previewIndexSlider.setBounds(prevRow.removeFromLeft(260));
        r.removeFromBottom(6);
    }

    // Crossfade row, just below the freeze window it caps against.
    {
        auto xfadeRow = r.removeFromBottom(26);
        crossfadeLabel.setBounds(xfadeRow.removeFromLeft(160));
        crossfadeSlider.setBounds(xfadeRow.removeFromLeft(260));
        r.removeFromBottom(6);
    }

    // "Window length" (freeze window, ms) + Fit button row.
    {
        auto sampRow = r.removeFromBottom(26);
        windowLenLabel.setBounds(sampRow.removeFromLeft(160));
        windowLenSlider.setBounds(sampRow.removeFromLeft(260));
        sampRow.removeFromLeft(12);
        fitWidthBtn.setBounds(sampRow.removeFromLeft(170));
        r.removeFromBottom(6);
    }

    // Grain-length row.
    {
        auto grainRow = r.removeFromBottom(26);
        grainLenLabel.setBounds(grainRow.removeFromLeft(160));
        grainLenSlider.setBounds(grainRow.removeFromLeft(260));
        r.removeFromBottom(6);
    }

    // Waveforms-to-slice-out row, with the region-geometry readout to its right.
    {
        auto controlsRow = r.removeFromBottom(26);
        numFramesLabel.setBounds(controlsRow.removeFromLeft(160));
        numFramesSlider.setBounds(controlsRow.removeFromLeft(260));
        controlsRow.removeFromLeft(12);
        regionInfoLabel.setBounds(controlsRow);
        r.removeFromBottom(6);
    }

    // Freeze-mode row: dropdown + the mode-specific grain-count / FFT-size
    // extras (which share one slot, toggled by refreshFreezeExtras).
    auto freezeRow = r.removeFromBottom(26);
    freezeModeLabel.setBounds(freezeRow.removeFromLeft(160));
    freezeModeCombo.setBounds(
        freezeRow.removeFromLeft(intrinsicComboWidth(freezeModeCombo)));
    freezeRow.removeFromLeft(14);
    auto songGrainRow = freezeRow;
    grainCountLabel.setBounds(songGrainRow.removeFromLeft(140));
    grainCountSlider.setBounds(songGrainRow.removeFromLeft(120));
    auto songFftRow = freezeRow;
    fftSizeLabel.setBounds(songFftRow.removeFromLeft(140));
    fftSizeCombo.setBounds(
        songFftRow.removeFromLeft(intrinsicComboWidth(fftSizeCombo)));

    r.removeFromBottom(6);

    // Embedded pitch row: label + note + octave + cents readout, with the Gain
    // control sharing the row's right side.
    auto pitchRow = r.removeFromBottom(26);
    noteOctaveLabel.setBounds(pitchRow.removeFromLeft(160));
    noteCombo.setBounds(pitchRow.removeFromLeft(intrinsicComboWidth(noteCombo)));
    pitchRow.removeFromLeft(6);
    octaveCombo.setBounds(pitchRow.removeFromLeft(intrinsicComboWidth(octaveCombo)));
    pitchRow.removeFromLeft(10);
    centsLabel.setBounds(pitchRow.removeFromLeft(80));
    pitchRow.removeFromLeft(16);
    gainLabel.setBounds(pitchRow.removeFromLeft(46));
    gainSlider.setBounds(pitchRow);

    r.removeFromBottom(6);
    statusLabel.setBounds(r.removeFromBottom(20));

    r.removeFromBottom(4);
    hintLabel.setBounds(r.removeFromBottom(48));

    r.removeFromBottom(4);
    // Transport row, directly under the waveform area.
    auto transportRow = r.removeFromBottom(28);
    playBtn.setBounds(transportRow.removeFromLeft(70));
    transportRow.removeFromLeft(4);
    pauseBtn.setBounds(transportRow.removeFromLeft(70));
    transportRow.removeFromLeft(4);
    stopBtn.setBounds(transportRow.removeFromLeft(70));

    // Zoom / scroll row, directly under the waveform area.
    {
        r.removeFromBottom(6);
        auto viewRow = r.removeFromBottom(24);
        auto leftHalf  = viewRow.removeFromLeft(viewRow.getWidth() / 2);
        auto rightHalf = viewRow;
        scrollLabel.setBounds(leftHalf.removeFromLeft(50));
        scrollSlider.setBounds(leftHalf.reduced(4, 0));
        zoomLabel.setBounds(rightHalf.removeFromLeft(50));
        zoomSlider.setBounds(rightHalf.reduced(4, 0));
        r.removeFromBottom(4);
    }

    waveRect = r;
}

void CaptureFromSongDialog::timerCallback() {
    // Render-completion handoff. Cheap to poll once per frame.
    // Two flavours:
    //   - normal: a RenderJob ran and finished -> onRenderComplete()
    //   - cache hit: songPcm was filled in the constructor and there's
    //     no RenderJob; route through the same completion path so the
    //     "Paused, audition running" state engages.
    if (!renderReady) {
        if (renderJob && renderJob->done.load()) {
            onRenderComplete();
        } else if (!renderJob && songPcm && !songPcm->empty()) {
            onRenderComplete();
        }
    }

    // While playing, the engine advances the song-pos atomically; reflect
    // the latest value in the playhead so it tracks playback. If the
    // engine has auto-stopped (song finished), notice and update state.
    if (state == TState::Playing) {
        auto* eng = AudioEngine::getInstance();
        if (eng) {
            playheadSamplePos = eng->getPreviewSongPosSamples();
            if (eng->getPreviewMode() == AudioEngine::PreviewMode::Off) {
                // Song ran past the end. Snap back to Stopped so the
                // user can pick a new spot.
                state = TState::Stopped;
                playheadSamplePos = 0;
                updateButtonsForState();
            }
        }
    }

    repaint(waveRect);
}

void CaptureFromSongDialog::onRenderComplete() {
    renderReady = true;
    // Two entry paths land here:
    //  (a) RenderJob just finished; pull result into songPcm and stash
    //      into the Output node's cache so the next dialog open hits.
    //  (b) Cache hit: songPcm was filled in the constructor; nothing
    //      to harvest here.
    if (renderJob && renderJob->result) {
        songPcm = renderJob->result;
        writeSongCache(graph, songPcm, songSampleRate);
        playheadSamplePos = 0;
    }
    if (songPcm && !songPcm->empty()) {
        if (auto* eng = AudioEngine::getInstance())
            eng->setPreviewSongPcm(songPcm);
        playheadSamplePos = 0;
    }
    // Now that the song length is known, re-range the zoom slider for it.
    configureViewSlidersForBuffer();

    // Default the selected region to a short window at the song start (~1 s,
    // clamped to the song length). Unlike the file dialog - which can default
    // to the whole file because files are usually short - a rendered song can
    // be minutes long, so defaulting to the whole thing would make the n<=1
    // audition build a multi-minute buffer and the first capture span the
    // entire song. A short default keeps the dialog responsive and lets the
    // user drag the handles out to whatever span they actually want.
    const int64_t total = songPcm ? (int64_t)songPcm->size() : 0;
    if (total > 0) {
        const int64_t defLen = std::min<int64_t>(
            total, (int64_t)std::llround(songSampleRate));   // ~1 second
        regionStart = 0;
        regionEnd   = defLen;
    } else {
        regionStart = regionEnd = 0;
    }
    // Re-derive the per-waveform window / controls now that there's a real
    // region (the constructor ran these with an empty buffer).
    syncWindowToAuto();
    updateWindowLenControl();
    updatePreviewIndexControl();
    updateRegionInfoLabel();

    // Auto-enter Paused so the user hears the selected-waveform audition
    // immediately - the whole point of the dialog is "find the spot",
    // and that's much easier when audio is already running. Without
    // this the user has to press a transport button before anything
    // happens, which makes the dialog feel inert on open.
    if (songPcm && !songPcm->empty())
        setState(TState::Paused);
    else
        updateButtonsForState();
    updateStatusLabel();
    repaint();
}

void CaptureFromSongDialog::setState(TState s) {
    if (!renderReady) return;
    if (!songPcm || songPcm->empty()) return;
    auto* eng = AudioEngine::getInstance();
    if (!eng) return;

    state = s;
    switch (s) {
        case TState::Playing:
            // Full-song playback from the start of the selected region, with a
            // moving playhead. The region start is the natural "play from here"
            // anchor (it's the earliest spot the user is interested in).
            playheadSamplePos = regionStart;
            eng->setPreviewSongPosSamples(playheadSamplePos);
            eng->setPreviewMode(AudioEngine::PreviewMode::SongPlay);
            break;
        case TState::Paused:
        case TState::Scrubbing:
            // Audition the Preview-waveform-selected slice of the region as a
            // grain loop, exactly the geometry a captured frame will carry.
            regenerateAuditionGrain();
            eng->setPreviewMode(AudioEngine::PreviewMode::GrainLoop);
            break;
        case TState::Stopped:
            playheadSamplePos = 0;
            eng->setPreviewMode(AudioEngine::PreviewMode::Off);
            break;
    }
    updateButtonsForState();
    updateStatusLabel();
    repaint(waveRect);
}

// Forward decl note: leave a hook here so updateButtonsForState() can
// gate the freeze-mode picker too (it's only meaningful once the render
// is ready, same as the transport buttons).
void CaptureFromSongDialog::updateButtonsForState() {
    const bool ready = renderReady && songPcm && !songPcm->empty();
    playBtn.setEnabled(ready);
    pauseBtn.setEnabled(ready);
    stopBtn.setEnabled(ready);
    numFramesSlider.setEnabled(ready);
    gainSlider.setEnabled(ready);
    freezeModeCombo.setEnabled(ready);
    // Grain length and crossfade have BOTH a render-ready gate and a freeze-mode
    // gate (grain only in the cloud modes, crossfade only in CrossfadeLoop).
    // refreshFreezeExtras owns that combined enable+tooltip logic, so route it
    // through there rather than force-enabling on `ready` alone (which would
    // wrongly light up an inert control for the current mode).
    refreshFreezeExtras();
    // The per-waveform window + preview-index controls have their own
    // count-dependent enable logic (single-waveform disables them); refresh it
    // here so they also respect the render-not-ready gate.
    if (ready) {
        updateWindowLenControl();
        updatePreviewIndexControl();
    } else {
        windowLenSlider.setEnabled(false);
        fitWidthBtn.setEnabled(false);
        previewIndexSlider.setEnabled(false);
    }

    // Save is disabled while Playing because the playhead is sweeping and
    // the user can't precisely pick a region until they pause/scrub. The
    // tooltip explains so the user isn't left wondering why.
    if (!ready) {
        saveBtn.setEnabled(false);
        saveBtn.setTooltip("Waiting for the project to finish rendering "
                           "before you can capture.");
    } else if (state == TState::Playing) {
        saveBtn.setEnabled(false);
        saveBtn.setTooltip(
            "Capture is disabled during playback - pause or drag a region "
            "handle to lock in the selection first. Press Pause to switch to "
            "the audition loop and freeze the region in place.");
    } else {
        saveBtn.setEnabled(true);
        saveBtn.setTooltip(
            "Capture the selected region as waveforms. The region is sliced "
            "into the chosen number of equally-spaced waveforms, each added to "
            "your wavetable along the Position dimension. "
            "You can keep scrubbing and saving more spots after this.");
    }
}

void CaptureFromSongDialog::syncCrossfadeMaxToWindow() {
    // The CrossfadeLoop seam can be at most half the loop, and the loop IS the
    // freeze window, so cap the slider at effectiveSrcLen()/2 (in ms). Mirrors
    // the engine's xfade = min(req, window/2) clamp and the file dialog's
    // syncCrossfadeMaxToWindow, so the visible value always equals the audible
    // one. The displayed value is min(desired, newMax): shrinking the window
    // clamps the crossfade down, growing it back restores the user's choice. The
    // syncingCrossfadeFromWindow guard keeps setRange's internal clamp (which
    // fires onValueChange synchronously) from overwriting crossfadeDesiredMs.
    const double sr        = (songSampleRate > 0.0) ? songSampleRate : 48000.0;
    const int64_t windowLen = effectiveSrcLen();
    const double maxMs     = juce::jmax(kXfadeMinMs,
                                        (double)(windowLen / 2) / sr * 1000.0);
    const double visible   = juce::jlimit(kXfadeMinMs, maxMs, crossfadeDesiredMs);
    syncingCrossfadeFromWindow = true;
    crossfadeSlider.setRange(kXfadeMinMs, juce::jmax(2.0, maxMs), 1.0);
    crossfadeSlider.setValue(visible, juce::dontSendNotification);
    syncingCrossfadeFromWindow = false;
}

void CaptureFromSongDialog::updateStatusLabel() {
    if (!renderReady) {
        const double p = renderJob ? renderJob->progress.load() : 0.0;
        juce::String msg;
        msg << "Rendering project... " << juce::String((int)(p * 100.0)) << " %";
        statusLabel.setText(msg, juce::dontSendNotification);
        return;
    }
    if (!songPcm || songPcm->empty() || songSampleRate <= 0.0) {
        statusLabel.setText("(project has no audio content)",
                            juce::dontSendNotification);
        return;
    }
    const double totSec = (double)songPcm->size() / songSampleRate;
    juce::String msg;
    if (state == TState::Playing) {
        // Playing the whole song: show the moving playhead position.
        const double posSec = (double)playheadSamplePos / songSampleRate;
        msg << "Playing: " << juce::String(posSec, 2) << " / "
            << juce::String(totSec, 2) << " s";
    } else {
        // Stopped / Paused / Scrubbing: the region is the thing the user is
        // working with, so report its span. (The detailed waveform-count
        // geometry lives in the region-info label next to the count slider.)
        const double startSec = (double)regionStart / songSampleRate;
        const double endSec   = (double)regionEnd   / songSampleRate;
        msg << "Region: " << juce::String(startSec, 2) << " - "
            << juce::String(endSec, 2) << " s   /   "
            << juce::String(totSec, 2) << " s total";
    }
    statusLabel.setText(msg, juce::dontSendNotification);
}

void CaptureFromSongDialog::regenerateAuditionGrain() {
    if (!songPcm || songPcm->empty() || songSampleRate <= 0.0) return;
    auto* eng = AudioEngine::getInstance();
    if (!eng) return;

    const int64_t regionLen = std::max<int64_t>(0, regionEnd - regionStart);
    if (regionLen < 64) return;
    const double sr = songSampleRate;

    // Audition the user-selected waveform using the exact geometry buildFrames
    // bakes, so the preview is honest: the same banded source (window + seam
    // lookahead), grain, and per-method window the captured frame will carry.
    // The single-waveform case spans the whole selection (you hear the recorded
    // sound); the multi-waveform case auditions whichever waveform the "Preview
    // waveform" picker selects.
    const int n         = (int)std::round(numFramesSlider.getValue());
    const int64_t windowLen = effectiveSrcLen();
    if (windowLen <= 0) return;
    const int grainLen = effectiveGrainLen();

    // 0-based band index. With a single waveform there is only band 0; with
    // 2+ the "Preview waveform" slider (1-based) chooses, clamped into range.
    const int repIdx = (n <= 1)
        ? 0
        : juce::jlimit(0, n - 1,
                       (int)std::round(previewIndexSlider.getValue()) - 1);
    const int64_t startIdx = bandStartForIndex(repIdx, n, windowLen);
    auto src = std::make_shared<std::vector<float>>(buildGrainSource(startIdx, windowLen));

    // Bake the dialog's output gain into this throwaway preview buffer so the
    // audition loudness matches what the captured frame will play at (the engine
    // preview path has no per-frame gain knob; the captured frame instead carries
    // the gain as IWavetableFrame::gain - see buildFrames).
    const float previewGain = (float)gainSlider.getValue();
    if (previewGain != 1.0f)
        for (auto& s : *src) s *= previewGain;

    // Publish grain length FIRST so by the time the engine swaps the new buffer
    // pointer in (next block), the N it reads matches the buffer about to arrive.
    // Re-apply the "As note" playback pitch every respin so the audition stays
    // pitched. Mirrors CaptureFromPlaybackDialog::regenerateAuditionGrain.
    publishPreviewPitch();
    // Banded preview: the band is the first windowLen samples (windowStart = 0),
    // the grain is the separate grain control. The published grain length is
    // clamped to the WINDOW (not just the source): a grain must fit inside the
    // freeze window, or the engine floors the band width up to the grain length,
    // swallowing the whole source and tripping the "band too short to roam"
    // viability gate -> silence. Capping the grain to the window keeps the cloud
    // modes sounding even when the selection is smaller than the requested grain.
    eng->setPreviewGrainLength(std::min<int>(grainLen, (int)windowLen));
    eng->setPreviewWindowStart(0);
    eng->setPreviewWindowLen((int)windowLen);
    // Seam crossfade from the Crossfade slider, matching buildFrames; engine
    // clamps to window/2.
    const int xfadeSamples = std::min(
        std::max(0, (int)std::round(crossfadeSlider.getValue() * 0.001 * sr)),
        std::max(0, (int)(windowLen / 2)));
    eng->setPreviewCrossfadeLength(xfadeSamples);
    // Re-publish the texture choices on every respin so a restart after a
    // clearPreview (Stop) still auditions at the user's grain count / FFT size.
    eng->setPreviewGrainCount(selectedGrainCount());
    eng->setPreviewFftSize(selectedFftSize());
    eng->setPreviewGrainBuffer(std::move(src));
}

void CaptureFromSongDialog::seedFromExistingFrame(double pitchHz,
                                                  int freezeModeIdx,
                                                  double crossfadeMs,
                                                  int grainCount,
                                                  int fftSize) {
    // ----- Pitch -----
    if (pitchHz > 0.0) {
        capturedPitchHz = pitchHz;
        auto [n, o] = hzToNearestNoteOctave(pitchHz);
        if (n >= 0 && n <= 11) noteCombo.setSelectedId(n + 1, juce::dontSendNotification);
        if (o >= 0 && o <= 9)  octaveCombo.setSelectedId(o + 1, juce::dontSendNotification);
        centsLabel.setText(formatCents(pitchHz), juce::dontSendNotification);
        publishPreviewPitch();
    }

    // ----- Freeze mode -----
    if (freezeModeIdx >= 0 && freezeModeIdx <= 4) {
        freezeModeCombo.setSelectedId(freezeModeIdx + 1, juce::dontSendNotification);
        // Match the live audition to the seeded mode (the onChange handler that
        // would normally do this is suppressed by dontSendNotification).
        if (auto* eng = AudioEngine::getInstance())
            eng->setGrainFreezeMode((AudioEngine::GrainFreezeMode)freezeModeIdx);
    }

    // ----- Crossfade (ms) -----
    if (crossfadeMs >= 0.0) {
        // The freeze window may cap the crossfade max below the frame's stored
        // value; clamp into the slider's current range so the visible value is
        // honest, but remember the frame's true intent in crossfadeDesiredMs so
        // a later window increase can restore it (mirrors syncCrossfadeMaxToWindow).
        crossfadeDesiredMs = crossfadeMs;
        const double vis = juce::jlimit(crossfadeSlider.getMinimum(),
                                        crossfadeSlider.getMaximum(), crossfadeMs);
        crossfadeSlider.setValue(vis, juce::dontSendNotification);
    }

    // ----- Texture (grain count / FFT size) -----
    if (grainCount >= kGranularMinGrains) {
        grainCountSlider.setValue(
            (double)juce::jlimit(kGranularMinGrains, kGranularMaxGrains, grainCount),
            juce::dontSendNotification);
    }
    if (fftSize >= 0) {
        // 0 == Auto (combo id 1); otherwise pick the matching size's id.
        int id = 1;
        for (int i = 0; i < kNumSpectralFftSizes; ++i)
            if (kSpectralFftSizes[i] == fftSize) { id = i + 2; break; }
        fftSizeCombo.setSelectedId(id, juce::dontSendNotification);
    }
    // Reflect the seeded mode's relevance + push the seeded values to the
    // engine audition (the freeze onChange that would normally do this is
    // suppressed by dontSendNotification above).
    refreshFreezeExtras();
}

void CaptureFromSongDialog::publishMetadataEdit() {
    if (!onMetadataEdited) return;  // append mode: no live frame bound
    const int fz = juce::jlimit(0, 3, freezeModeCombo.getSelectedId() - 1);
    // Report crossfade in ms (rate-independent), using the user's INTENDED value
    // (crossfadeDesiredMs) rather than the visible slider value. The visible
    // value is clamped down to half the current Width; writing that to the bound
    // frame would silently shrink its crossfade whenever an UNRELATED edit (e.g.
    // pitch) fires while Width happens to cap the slider below the frame's real
    // value. The frame keeps its own grainLength (Width is a capture-time param
    // that doesn't write through), and the synth clamps the seam to grainLength/2
    // at use time, so storing the full intent here is both safe and lossless.
    onMetadataEdited(capturedPitchHz, fz, crossfadeDesiredMs,
                     selectedGrainCount(), selectedFftSize());
}

void CaptureFromSongDialog::refreshFreezeExtras() {
    const int idx = juce::jlimit(0, 3, freezeModeCombo.getSelectedId() - 1);
    const GranularFreezeMode mode = (GranularFreezeMode)idx;
    const bool usesGrain = (mode == GranularFreezeMode::AsyncGranular
                            || mode == GranularFreezeMode::PitchSyncGrains);
    const bool isSpectral = (mode == GranularFreezeMode::SpectralFreeze);

    // The grain-count and FFT-size controls share one slot in the freeze row,
    // so show only the pair that applies to the current mode. CrossfadeLoop
    // uses neither, so both hide. The visible control is always enabled; the
    // tooltip on each still explains what it does.
    grainCountLabel.setVisible(usesGrain);
    grainCountSlider.setVisible(usesGrain);
    fftSizeLabel.setVisible(isSpectral);
    fftSizeCombo.setVisible(isSpectral);
    grainCountLabel.setEnabled(usesGrain);
    grainCountSlider.setEnabled(usesGrain);
    fftSizeLabel.setEnabled(isSpectral);
    fftSizeCombo.setEnabled(isSpectral);

    grainCountSlider.setTooltip(usesGrain
        ? "How many overlapping grains make up each captured waveform's cloud. "
          "More = denser and smoother; fewer = sparser and more granular. The "
          "level stays constant as you change it. Only used by Async and "
          "Pitch-sync grains."
        : "Disabled - the current freeze mode has no grains. Switch to Async or "
          "Pitch-sync grains to set the grain count.");
    fftSizeCombo.setTooltip(isSpectral
        ? "FFT size for each captured waveform's Spectral freeze, in samples. "
          "Larger = finer frequency detail (more bins) but a coarser time "
          "window. Auto picks the largest size that fits the waveform (up to "
          "2048). Only used by Spectral freeze."
        : "Disabled - the FFT size only matters in Spectral-freeze mode. Switch "
          "the freeze mode to Spectral freeze to use it.");

    // Grain length and the crossfade seam are mode-specific, and both are also
    // gated on the project render being ready (a half-rendered song has no
    // stable buffer to audition). Grain length only matters for the grain-cloud
    // modes (CrossfadeLoop loops the whole window, SpectralFreeze FFTs it); the
    // crossfade seam only matters for CrossfadeLoop. Grey out the one that
    // doesn't apply so the user isn't left tweaking an inert control. The values
    // are still kept and re-baked into captured frames so a later mode switch in
    // the wave editor already has them set.
    const bool ready = renderReady && songPcm && !songPcm->empty();
    const bool usesCrossfade = (mode == GranularFreezeMode::CrossfadeLoop);
    grainLenLabel.setEnabled(ready && usesGrain);
    grainLenSlider.setEnabled(ready && usesGrain);
    grainLenSlider.setTooltip(usesGrain
        ? "Length of each overlapping grain inside a captured waveform, in "
          "milliseconds. Short (~5 ms, the default) = a smooth, CONSTANT cloud; "
          "longer grains roam a proportionally wider window = more evolving / "
          "less constant. The freeze window auto-tracks 4x this (until you drag "
          "it) so the cloud has room to roam. Used by Async and Pitch-sync "
          "grains."
        : "Disabled - the current freeze mode has no grains: Crossfade loop "
          "loops the whole freeze window and Spectral freeze analyses it. Switch "
          "to Async or Pitch-sync grains to set the grain length. (Use \"Window "
          "length\" to set the loop / analysis length for this mode.)");
    crossfadeLabel.setEnabled(ready && usesCrossfade);
    crossfadeSlider.setEnabled(ready && usesCrossfade);
    crossfadeSlider.setTooltip(usesCrossfade
        ? "Crossfade-loop seam length in milliseconds. The loop blends its end "
          "back into its start over this duration to hide the seam click. Short "
          "(1-10 ms): tight, the loop period is most audible. Long (near the cap "
          "= half the freeze window): the loop becomes a rolling crossfade "
          "between two playheads, smoother but more blurred. The max tracks half "
          "the \"Window length\" window because the engine can't overlap two "
          "ramps longer than that in one loop."
        : "Disabled - the crossfade seam only applies to Crossfade loop mode, "
          "which loops the freeze window and blends its end back into its start. "
          "Switch the freeze mode to Crossfade loop to set the seam length.");

    // Keep the engine audition in sync with the current values.
    if (auto* eng = AudioEngine::getInstance()) {
        eng->setPreviewGrainCount(selectedGrainCount());
        eng->setPreviewFftSize(selectedFftSize());
    }
}

void CaptureFromSongDialog::publishPreviewPitch() {
    auto* eng = AudioEngine::getInstance();
    if (!eng) return;
    // Reference note A4 (440 Hz) matches the wavetable editor's audition pitch
    // (kAuditionPitch = 69), so the region preview, the editor's Play button,
    // and a synth note at A4 all sound the captured frame at the same pitch.
    // capturedPitchHz is the pitch the user is labelling this grain as, so
    // playing it "as A4" resamples by 440 / capturedPitchHz.
    constexpr double kReferenceHz = 440.0;
    const double hz = (capturedPitchHz > 0.0) ? capturedPitchHz : kReferenceHz;
    eng->setPreviewGrainRatio((float)(kReferenceHz / hz));
    // Publish the source's natural pitch so PitchSyncGrains can derive the
    // loop period; the other freeze modes ignore it.
    eng->setPreviewEmbeddedPitch((float)hz);
}

int64_t CaptureFromSongDialog::minViewLenSamples() const {
    const int64_t total = songPcm ? (int64_t)songPcm->size() : 0;
    if (total <= 0) return 0;
    int64_t floor = kMinViewSamples;
    // With 2+ waveforms the view can never get tighter than one freeze window,
    // so a whole captured waveform always fits on screen. (A single waveform
    // spans the selection - no independent window - so the plain floor stands.)
    const int n = (int)std::round(numFramesSlider.getValue());
    if (n > 1) floor = std::max(floor, windowFromSlider());
    return std::min(total, floor);
}

int64_t CaptureFromSongDialog::viewLenSamples() const {
    const int64_t total = songPcm ? (int64_t)songPcm->size() : 0;
    if (total <= 0) return 0;
    const double z = juce::jmax(1.0, viewZoom);
    int64_t len = (int64_t)std::llround((double)total / z);
    return juce::jlimit(minViewLenSamples(), total, len);
}

int64_t CaptureFromSongDialog::viewStartSamples() const {
    const int64_t total = songPcm ? (int64_t)songPcm->size() : 0;
    const int64_t len   = viewLenSamples();
    const int64_t maxStart = std::max<int64_t>(0, total - len);
    return juce::jlimit<int64_t>(0, maxStart,
                                 (int64_t)std::llround(viewScroll * (double)maxStart));
}

int CaptureFromSongDialog::xForSamplePos(int64_t pos) const {
    const int64_t len = viewLenSamples();
    if (len <= 0 || waveRect.getWidth() <= 0)
        return waveRect.getX();
    const double t = (double)(pos - viewStartSamples()) / (double)len;
    return waveRect.getX() + (int)std::round(t * (double)waveRect.getWidth());
}

int64_t CaptureFromSongDialog::samplePosForX(int x) const {
    const int64_t len = viewLenSamples();
    if (len <= 0 || waveRect.getWidth() <= 0)
        return 0;
    const double t = (double)(x - waveRect.getX()) / (double)waveRect.getWidth();
    const int64_t total = songPcm ? (int64_t)songPcm->size() : 0;
    return juce::jlimit<int64_t>(0, total,
                                 viewStartSamples() + (int64_t)std::round(t * (double)len));
}

void CaptureFromSongDialog::reconfigureZoomForWindow() {
    // Re-range the zoom slider so its maximum lets the view shrink to exactly
    // minViewLenSamples() (kMinViewSamples, or one freeze window with 2+
    // waveforms), keeping the current zoom (clamped into the new range). The
    // re-entrancy guard stops setRange's clamp notification from recursing back
    // through onViewChanged -> updateWindowLenControl -> here.
    if (reconfiguringZoom) return;
    const int64_t total = songPcm ? (int64_t)songPcm->size() : 0;
    if (total <= 0) return;
    const juce::ScopedValueSetter<bool> guard(reconfiguringZoom, true);
    const int64_t floor   = std::max<int64_t>(1, minViewLenSamples());
    const double  maxZoom = (total > floor) ? (double)total / (double)floor : 1.0;
    const double  rangeMax = std::max(1.0001, maxZoom);
    const double  cur      = juce::jlimit(1.0, rangeMax, viewZoom);
    zoomSlider.setRange(1.0, rangeMax, 0.0);
    if (maxZoom > 2.0)
        zoomSlider.setSkewFactorFromMidPoint(std::sqrt(maxZoom));
    zoomSlider.setValue(cur, juce::dontSendNotification);
    viewZoom = cur;
    zoomSlider.setEnabled(maxZoom > 1.0001);
}

void CaptureFromSongDialog::configureViewSlidersForBuffer() {
    viewZoom   = 1.0;
    viewScroll = 0.0;
    reconfigureZoomForWindow();
    scrollSlider.setValue(0.0, juce::dontSendNotification);
    scrollSlider.setEnabled(false);   // nothing to scroll at zoom 1
}

void CaptureFromSongDialog::clampSelectionToView() {
    const int64_t total = songPcm ? (int64_t)songPcm->size() : 0;
    if (total <= 0) return;
    const int64_t vStart = viewStartSamples();
    const int64_t vLen   = viewLenSamples();
    const int64_t vEnd   = vStart + vLen;

    int64_t len = std::max<int64_t>(0, regionEnd - regionStart);
    // "Can't select more than you can see": cap the selection to the window.
    if (len > vLen) len = vLen;
    // Peg the (possibly shortened) selection flush inside the view.
    int64_t start = regionStart;
    if (start + len > vEnd) start = vEnd - len;
    if (start < vStart)     start = vStart;
    regionStart = start;
    regionEnd   = start + len;
}

void CaptureFromSongDialog::onViewChanged() {
    scrollSlider.setEnabled(viewLenSamples() < (songPcm ? (int64_t)songPcm->size() : 0));
    // Don't fight the playhead while Playing - it genuinely sweeps the song.
    if (state != TState::Playing) {
        clampSelectionToView();
        updateWindowLenControl();
        updateRegionInfoLabel();
        // Keep the audition anchored on the (possibly pegged) selection.
        if (state == TState::Paused || state == TState::Scrubbing)
            regenerateAuditionGrain();
        updateStatusLabel();
    }
    repaint(waveRect);
}

void CaptureFromSongDialog::mouseDown(const juce::MouseEvent& e) {
    if (!renderReady) return;
    if (!songPcm || songPcm->empty()) return;

    const int xs = xForSamplePos(regionStart);
    const int xe = xForSamplePos(regionEnd);
    const int mx = e.x;

    // Handle grabbing is allowed anywhere in the handle zone (waveRect widened
    // by the hit radius), so an edge handle whose outer half is past the
    // wave-view edge is still grabbable from just outside it.
    if (handleZone().contains(e.getPosition())) {
        const int dStart = std::abs(mx - xs);
        const int dEnd   = std::abs(mx - xe);
        if (std::min(dStart, dEnd) <= kHandleHitRadius) {
            dragHandle = (dStart <= dEnd) ? 0 : 1;
            dragAnchorSampleOffset = 0;
            // Auditioning a region while dragging it requires the grain-loop
            // state; drop out of full-song Playing into Scrubbing so the user
            // hears the slice they're shaping.
            if (state != TState::Paused && state != TState::Scrubbing)
                setState(TState::Scrubbing);
            return;
        }
    }

    // Body drag and click-to-create require the click to land inside the wave
    // area proper, not in the handle slop margin outside it.
    if (!waveRect.contains(e.getPosition())) return;
    if (mx > xs && mx < xe) {
        dragHandle = 2;
        dragAnchorSampleOffset = samplePosForX(mx) - regionStart;
    } else {
        regionStart = samplePosForX(mx);
        regionEnd   = regionStart;
        dragHandle  = 1;
        updateWindowLenControl();
        updateRegionInfoLabel();
    }
    if (state != TState::Paused && state != TState::Scrubbing)
        setState(TState::Scrubbing);
    repaint(handleZone());
}

void CaptureFromSongDialog::mouseDrag(const juce::MouseEvent& e) {
    if (dragHandle < 0 || !songPcm || songPcm->empty()) return;
    const int64_t sz = (int64_t)songPcm->size();
    const int64_t idx = juce::jlimit<int64_t>(0, sz, samplePosForX(e.x));
    if (dragHandle == 0) {
        // Dragging the start handle. If it crosses past the end handle, flip
        // roles so the gesture now controls the END handle (lets a collapsed
        // selection be reopened by dragging the opposite way).
        if (idx > regionEnd) {
            regionStart = regionEnd;
            regionEnd   = idx;
            dragHandle  = 1;
        } else {
            regionStart = idx;
        }
    } else if (dragHandle == 1) {
        if (idx < regionStart) {
            regionEnd   = regionStart;
            regionStart = idx;
            dragHandle  = 0;
        } else {
            regionEnd = idx;
        }
    } else if (dragHandle == 2) {
        const int64_t span = regionEnd - regionStart;
        const int64_t newStart = juce::jlimit<int64_t>(0, std::max<int64_t>(0, sz - span),
                                                       samplePosForX(e.x) - dragAnchorSampleOffset);
        regionStart = newStart;
        regionEnd   = newStart + span;
    }
    // Don't let a resize handle shrink the selection below one window length (so
    // a section band always fits), except when the view is zoomed in tighter
    // than the window - then only stop further shrinking. Body drag keeps a
    // fixed span and is exempt.
    enforceMinSelectionDuringDrag();
    // With one waveform the window tracks the selection, so keep the (disabled)
    // per-waveform slider showing the live selection length while dragging.
    updateWindowLenControl();
    updateRegionInfoLabel();
    // Scrub-to-audition: re-anchor the grain loop on the moving selection.
    if (state == TState::Paused || state == TState::Scrubbing)
        regenerateAuditionGrain();
    updateStatusLabel();
    repaint(handleZone());
}

void CaptureFromSongDialog::mouseUp(const juce::MouseEvent&) {
    if (dragHandle < 0) return;
    dragHandle = -1;
    // Drop into Paused: identical audio (grain loop) but UI semantics are now
    // "you're holding here", not "you're dragging right now".
    if (state == TState::Scrubbing) {
        state = TState::Paused;
        updateButtonsForState();
    }
}

void CaptureFromSongDialog::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(22, 22, 28));

    g.setColour(juce::Colour(220, 220, 230));
    g.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
    g.drawText("Capture waveforms from the project song",
               getLocalBounds().reduced(12).removeFromTop(20),
               juce::Justification::centredLeft);

    g.setColour(juce::Colour(16, 16, 22));
    g.fillRect(waveRect);
    g.setColour(juce::Colour(60, 60, 70));
    g.drawRect(waveRect);

    // Render-progress overlay before we have PCM.
    if (!renderReady) {
        const double p = renderJob ? renderJob->progress.load() : 0.0;
        g.setColour(juce::Colour(120, 120, 130));
        g.setFont(juce::Font(juce::FontOptions(13.0f)));
        juce::String msg;
        msg << "Rendering project to audio... " << juce::String((int)(p * 100.0)) << " %";
        g.drawText(msg, waveRect, juce::Justification::centred);
        // Thin progress bar at the bottom edge of the wave area.
        const int barH = 4;
        juce::Rectangle<int> bar(waveRect.getX(),
                                  waveRect.getBottom() - barH - 4,
                                  waveRect.getWidth(), barH);
        g.setColour(juce::Colour(40, 40, 50));
        g.fillRect(bar);
        g.setColour(juce::Colour(110, 130, 200));
        g.fillRect(bar.withWidth((int)(bar.getWidth() * p)));
        return;
    }

    if (!songPcm || songPcm->empty()) {
        g.setColour(juce::Colour(120, 120, 130));
        g.drawText("Project has no audio content - nothing to capture.",
                   waveRect, juce::Justification::centred);
        return;
    }

    // Min/max waveform. One vertical line per pixel. Pixels map the current
    // VIEW window [viewStart, viewStart+viewLen) (zoom / scroll), not the whole
    // song - at zoom 1 the view is the whole song, so this is the old render.
    const int W = waveRect.getWidth();
    const int H = waveRect.getHeight();
    const int64_t sz = (int64_t)songPcm->size();
    const int64_t vStart = viewStartSamples();
    const int64_t vLen   = std::max<int64_t>(1, viewLenSamples());
    const int x0 = waveRect.getX();
    const int yMid = waveRect.getY() + H / 2;
    const int halfH = H / 2;
    const auto& v = *songPcm;
    g.setColour(juce::Colour(110, 130, 200));
    for (int px = 0; px < W; ++px) {
        const int64_t sStart = std::min(sz, vStart + (int64_t)px * vLen / W);
        const int64_t sEnd   = std::min(sz, vStart + (int64_t)(px + 1) * vLen / W);
        float mn =  1.0f, mx = -1.0f;
        for (int64_t s = sStart; s < sEnd; ++s) {
            const float vv = v[(size_t)s];
            if (vv < mn) mn = vv;
            if (vv > mx) mx = vv;
        }
        if (sEnd <= sStart) { mn = mx = 0.0f; }
        const int y1 = yMid - (int)std::round(juce::jlimit(-1.0f, 1.0f, mx) * halfH);
        const int y2 = yMid - (int)std::round(juce::jlimit(-1.0f, 1.0f, mn) * halfH);
        g.drawVerticalLine(x0 + px, (float)std::min(y1, y2),
                                     (float)std::max(y1, y2) + 1.0f);
    }

    // Zero line.
    g.setColour(juce::Colour(50, 50, 60));
    g.drawHorizontalLine(yMid, (float)waveRect.getX(), (float)waveRect.getRight());

    // Region shading: the selected span between the two handles.
    const int xs = xForSamplePos(regionStart);
    const int xe = xForSamplePos(regionEnd);
    if (xe > xs) {
        g.setColour(juce::Colour::fromFloatRGBA(1.0f, 0.65f, 0.2f, 0.18f));
        g.fillRect(juce::Rectangle<int>(xs, waveRect.getY(), xe - xs, H));
    }

    // Per-waveform section bands: each band shows the span of audio one captured
    // waveform covers (its source window, width = effectiveSrcLen), spaced across
    // the selection via the shared bandStartForIndex geometry. Identical to the
    // file dialog's section bands. Clipped to waveRect so a band reaching past a
    // zoomed-in view edge doesn't paint into the side margins.
    const int n      = (int)std::round(numFramesSlider.getValue());
    const int64_t regLen = std::max<int64_t>(0, regionEnd - regionStart);
    const int64_t srcLen = effectiveSrcLen();
    if (n > 0 && regLen > 0 && srcLen > 0) {
        juce::Graphics::ScopedSaveState bandClip(g);
        g.reduceClipRegion(waveRect);
        for (int i = 0; i < n; ++i) {
            const int64_t startIdx = bandStartForIndex(i, n, srcLen);
            const int bx0 = xForSamplePos(startIdx);
            const int bx1 = xForSamplePos(startIdx + srcLen);
            juce::Rectangle<int> band(bx0, waveRect.getY(),
                                      std::max(1, bx1 - bx0), H);
            g.setColour(juce::Colour::fromFloatRGBA(1.0f, 0.65f, 0.2f, 0.16f));
            g.fillRect(band);
            g.setColour(juce::Colour::fromFloatRGBA(1.0f, 0.65f, 0.2f, 0.45f));
            g.drawVerticalLine(bx0,     (float)waveRect.getY(), (float)waveRect.getBottom());
            g.drawVerticalLine(bx1 - 1, (float)waveRect.getY(), (float)waveRect.getBottom());
        }
    }

    // Playhead, drawn ONLY while Playing - a thin moving line sweeping the song
    // (no T-bar caps, to read differently from the draggable region handles).
    if (state == TState::Playing) {
        juce::Graphics::ScopedSaveState s(g);
        g.reduceClipRegion(waveRect);
        const int xp = xForSamplePos(playheadSamplePos);
        g.setColour(juce::Colour(230, 230, 120));
        g.fillRect(juce::Rectangle<int>(xp - 1, waveRect.getY(), 2, H));
    }

    // Two region handles (start + end), drawn last so they sit on top. Clip to
    // the handle zone (waveRect widened by the hit radius) so an edge handle
    // whose outer half is past the wave-view edge is drawn in full instead of
    // being chopped at the buffer edge.
    const juce::Rectangle<int> hz = handleZone();
    auto drawHandle = [&](int x) {
        g.setColour(juce::Colour(255, 165, 60));
        g.fillRect(juce::Rectangle<int>(x - 2, waveRect.getY(), 4, H).getIntersection(hz));
        g.fillRect(juce::Rectangle<int>(x - 6, waveRect.getY(), 12, 8).getIntersection(hz));
        g.fillRect(juce::Rectangle<int>(x - 6, waveRect.getBottom() - 8, 12, 8).getIntersection(hz));
    };
    drawHandle(xs);
    drawHandle(xe);
}

juce::Rectangle<int> CaptureFromSongDialog::handleZone() const {
    return waveRect.expanded(kHandleHitRadius, 0);
}

int CaptureFromSongDialog::effectiveGrainLen() const {
    const double sr = (songSampleRate > 0.0) ? songSampleRate : 48000.0;
    const double ms = grainLenSlider.getValue();
    return std::max(64, (int)std::round(ms / 1000.0 * sr));
}

int64_t CaptureFromSongDialog::windowFromSlider() const {
    // The per-waveform window the slider requests, in SAMPLES (ms -> samples at
    // the song rate), floored by the current mode but NOT capped to the
    // selection. effectiveSrcLen() applies the selection cap; minSelectionLen()
    // uses this uncapped value as the floor the selection can't shrink below.
    const double sr = (songSampleRate > 0.0) ? songSampleRate : 48000.0;
    const GranularFreezeMode mode = selectedFreezeMode();
    const bool grainCloud = (mode == GranularFreezeMode::AsyncGranular
                             || mode == GranularFreezeMode::PitchSyncGrains);
    const int64_t floor = grainCloud ? (int64_t)effectiveGrainLen() : 256;
    int64_t w = (int64_t)std::llround(windowLenSlider.getValue() / 1000.0 * sr);
    w = std::max(w, floor);
    const int64_t songLen = songPcm ? (int64_t)songPcm->size() : 0;
    if (songLen > 0) w = std::min(w, songLen);
    return std::max<int64_t>(1, w);
}

int64_t CaptureFromSongDialog::effectiveSrcLen() const {
    const int64_t regionLen = std::max<int64_t>(0, regionEnd - regionStart);
    const int64_t songLen   = songPcm ? (int64_t)songPcm->size() : 0;
    const int     grainLen  = effectiveGrainLen();
    const int     n         = (int)std::round(numFramesSlider.getValue());

    if (n <= 1) {
        // A single waveform spans the WHOLE selection - there is no per-waveform
        // spacing to honour, so the one window simply is the selection. Floor it
        // the same way windowFromSlider would.
        const GranularFreezeMode mode = selectedFreezeMode();
        const bool grainCloud = (mode == GranularFreezeMode::AsyncGranular
                                 || mode == GranularFreezeMode::PitchSyncGrains);
        const int64_t floor   = grainCloud ? (int64_t)grainLen : 256;
        int64_t w = regionLen;
        w = std::max(w, floor);
        if (songLen > 0) w = std::min(w, songLen);
        if (regionLen > 0) w = std::min(w, regionLen);
        return std::max<int64_t>(1, w);
    }

    // 2+ waveforms: the window-length slider is the source of truth (a per-
    // waveform duration in ms), auto-tracking autoWindowMultiplier(mode) x grain
    // via syncWindowToAuto() until the user overrides it. Cap to the selection
    // so the bands stay between the handles. (The min-selection drag clamp keeps
    // the selection >= the requested window, so this cap normally only bites
    // when the view is too zoomed-in to fit the full window.)
    int64_t w = windowFromSlider();
    if (regionLen > 0) w = std::min(w, regionLen);
    return std::max<int64_t>(1, w);
}

int64_t CaptureFromSongDialog::minSelectionLen() const {
    const int n = (int)std::round(numFramesSlider.getValue());
    // One waveform: its window IS the selection, so the user may shrink freely.
    if (n <= 1) return 0;
    // The selection can't shrink below one window. The zoom is clamped so the
    // view never gets tighter than one window (minViewLenSamples), so this
    // minimum always fits on screen - no view-exception escape hatch needed.
    return std::max<int64_t>(0, windowFromSlider());
}

void CaptureFromSongDialog::enforceMinSelectionDuringDrag() {
    // A resize drag (start/end handle) can't shrink the selection below the
    // per-waveform window (minSelectionLen()). Body drag (dragHandle==2) keeps
    // the length fixed, so it's exempt. The clamp pushes the *dragged* edge back
    // to the minimum; near a buffer boundary it nudges the anchor instead.
    if (dragHandle != 0 && dragHandle != 1) return;
    const int64_t minLen = minSelectionLen();
    if (minLen <= 0) return;
    if (regionEnd - regionStart >= minLen) return;
    const int64_t total = songPcm ? (int64_t)songPcm->size() : 0;
    if (dragHandle == 0) {            // start edge moving, end anchored
        regionStart = regionEnd - minLen;
        if (regionStart < 0) { regionStart = 0; regionEnd = std::min(total, minLen); }
    } else {                          // end edge moving, start anchored
        regionEnd = regionStart + minLen;
        if (regionEnd > total) { regionEnd = total; regionStart = std::max<int64_t>(0, total - minLen); }
    }
}

int64_t CaptureFromSongDialog::bandStartForIndex(int i, int n, int64_t srcLen) const {
    // Thin wrapper over the shared int64 geometry (captureBandStartForIndex,
    // documented near the top of this file) so file and song capture lay out
    // their section bands identically. srcLen is capped to the selection by
    // effectiveSrcLen(), so the bands never spill past the handles.
    const int64_t songLen = songPcm ? (int64_t)songPcm->size() : 0;
    return captureBandStartForIndex(
        regionStart, regionEnd, i, n, srcLen,
        std::max<int64_t>(0, songLen - srcLen));
}

void CaptureFromSongDialog::resizeSelectionForCountChange(int oldN, int newN) {
    // int64 mirror of CaptureFromPlaybackDialog::resizeSelectionForCountChange.
    // Keep each waveform's window length constant when the count changes by
    // scaling the SELECTION in proportion: newLen = oldLen * newN / oldN. The
    // slot spacing is regionLen / n, so scaling the region by newN/oldN keeps
    // the per-slot spacing (and thus the per-waveform window) fixed - adding
    // waveforms grows the selection rather than cramming more bands into a fixed
    // span. The window control itself is count-independent, so nothing else
    // needs to move. The resize is centred on the currently-selected preview
    // waveform, and when shrinking it stays within the old selection's bounds
    // (a smaller count never pushes a handle farther out). See the file-dialog
    // twin for the full rationale.
    const int64_t total = songPcm ? (int64_t)songPcm->size() : 0;
    if (total <= 0 || oldN <= 0 || newN <= 0 || oldN == newN) return;
    const int64_t oldStart = regionStart;
    const int64_t oldEnd   = regionEnd;
    const int64_t oldLen   = std::max<int64_t>(0, oldEnd - oldStart);
    if (oldLen <= 0) return;

    // Centre of the band the Preview-waveform picker currently points at, using
    // the *old* geometry (the preview slider is re-ranged only after this call).
    const int64_t srcLen    = effectiveSrcLen();
    const int     pi        = juce::jlimit(1, oldN, (int)std::round(previewIndexSlider.getValue()));
    const int64_t bandStart = bandStartForIndex(pi - 1, oldN, srcLen);
    const double  centre    = (double)bandStart + 0.5 * (double)srcLen;

    long long newLenLL = std::llround((double)oldLen * (double)newN / (double)oldN);
    int64_t newLen = std::min<int64_t>((int64_t)newLenLL, total);
    newLen = std::max<int64_t>(newLen, std::min<int64_t>(total, 256));   // never collapse to nothing

    int64_t newStart = (int64_t)std::llround(centre - 0.5 * (double)newLen);
    // Clamp to the buffer.
    if (newStart + newLen > total) newStart = total - newLen;
    if (newStart < 0) newStart = 0;

    // Shrinking: never let a handle move farther out than it already was.
    if (newLen < oldLen) {
        if (newStart < oldStart)          newStart = oldStart;
        if (newStart + newLen > oldEnd)   newStart = oldEnd - newLen;
        if (newStart < oldStart)          newStart = oldStart;
    }

    regionStart = newStart;
    regionEnd   = newStart + newLen;
}

std::vector<float>
CaptureFromSongDialog::buildGrainSource(int64_t startIdx, int64_t windowLen) const {
    const int64_t songLen     = songPcm ? (int64_t)songPcm->size() : 0;
    const int64_t reservedTail = std::max<int64_t>(0, windowLen / 2);
    const int64_t srcLen       = std::max<int64_t>(0, windowLen + reservedTail);
    std::vector<float> source((size_t)srcLen, 0.0f);
    if (songPcm) {
        const auto& song = *songPcm;
        for (int64_t s = 0; s < srcLen; ++s) {
            const int64_t idx = startIdx + s;
            if (idx >= 0 && idx < songLen)
                source[(size_t)s] = song[(size_t)idx];
        }
    }
    return source;
}

void CaptureFromSongDialog::syncWindowToFitSlots() {
    // Set the window to exactly one slot spacing (regionLen / n) so the n bands
    // tile the selection edge-to-edge with zero gaps. An explicit user action
    // (the "Fit width to selection" button), so it counts as a window override.
    const int     n      = (int)std::round(numFramesSlider.getValue());
    const int64_t regLen = std::max<int64_t>(0, regionEnd - regionStart);
    if (n <= 0 || regLen <= 0) return;
    const double sr = (songSampleRate > 0.0) ? songSampleRate : 48000.0;
    // The slider is in milliseconds, so convert the per-slot sample spacing.
    double spacingMs = (double)regLen / (double)n / sr * 1000.0;
    spacingMs = juce::jlimit(kWindowMinMs, kWindowMaxMs, spacingMs);
    windowLenSlider.setValue(spacingMs, juce::dontSendNotification);
    windowUserSet = true;
}

void CaptureFromSongDialog::syncWindowToAuto() {
    // While the window is auto (the user hasn't dragged it), keep the slider on
    // the per-method default: grain-cloud modes track 4x the grain (roam room),
    // while the non-grain modes (Crossfade / Spectral) use the grain-independent
    // kNonGrainAutoWindowSamples default so a short grain can't collapse their
    // loop / FFT window. No-op once overridden, and for a single waveform (whose
    // window is the whole selection). dontSendNotification so this never flips
    // windowUserSet.
    if (windowUserSet) return;
    const int n = (int)std::round(numFramesSlider.getValue());
    if (n <= 1) return;
    const GranularFreezeMode mode = selectedFreezeMode();
    double windowMs;
    if (mode == GranularFreezeMode::AsyncGranular
        || mode == GranularFreezeMode::PitchSyncGrains) {
        // The grain slider is already in ms, so 4x grain is a plain ms multiply.
        windowMs = (double)autoWindowMultiplier(mode) * grainLenSlider.getValue();
    } else {
        const double sr = (songSampleRate > 0.0) ? songSampleRate : 48000.0;
        windowMs = (double)kNonGrainAutoWindowSamples / sr * 1000.0;
    }
    windowMs = juce::jlimit(kWindowMinMs, kWindowMaxMs, windowMs);
    windowLenSlider.setValue(windowMs, juce::dontSendNotification);
}

void CaptureFromSongDialog::updateWindowLenControl() {
    // The freeze window may have just changed (count / grain / mode / region),
    // which changes how far the user is allowed to zoom in - keep the zoom
    // slider's reach in step. Cheap and idempotent when nothing changed.
    reconfigureZoomForWindow();
    const int n = (int)std::round(numFramesSlider.getValue());
    if (n <= 1) {
        // One waveform spans the whole selection: lock the slider and show it
        // holding the selection length (kept current as the selection resizes).
        const int64_t regLen = std::max<int64_t>(0, regionEnd - regionStart);
        const double  sr     = (songSampleRate > 0.0) ? songSampleRate : 48000.0;
        const double  regMs  = (double)regLen / sr * 1000.0;
        windowLenSlider.setValue(
            juce::jlimit(kWindowMinMs, kWindowMaxMs, regMs),
            juce::dontSendNotification);
        windowLenSlider.setEnabled(false);
        windowLenSlider.setTooltip(
            "Disabled while there is a single waveform: one waveform spans the "
            "whole selection, so its length is just the selection size - resize "
            "the selection (drag the orange handles) to change it. Increase "
            "\"Waveforms to slice out\" above 1 to set a per-waveform length "
            "independently of the selection.");
        fitWidthBtn.setEnabled(false);
        fitWidthBtn.setTooltip(
            "Not needed with a single waveform - it already spans the whole "
            "selection. Add more waveforms to use this.");
        syncCrossfadeMaxToWindow();
        return;
    }
    windowLenSlider.setEnabled(true);
    fitWidthBtn.setEnabled(true);
    fitWidthBtn.setTooltip(
        "Set the per-waveform width so the bands exactly tile the current "
        "selection - no gaps, no overlap. Same as dragging \"Window length\" "
        "until each band abuts the next. Use it after resizing "
        "the selection to snap back to a clean edge-to-edge layout.");
    windowLenSlider.setTooltip(
        "The freeze WINDOW, in milliseconds: how long each captured waveform "
        "spans - the region the freeze mode loops (Crossfade), roams (Async / "
        "Pitch-sync grains), or analyses (Spectral). Shown in ms (like grain "
        "length and crossfade) so you can read the window:grain ratio directly. "
        "Until you drag it, this auto-tracks a multiple of the grain length "
        "above - 4x for the grain-cloud modes so they have room to roam, 1x for "
        "the others. Drag it to set an explicit width (it then stays fixed as "
        "you change the grain or mode). The shaded orange bands over the "
        "waveform show each waveform's span, each exactly this wide and spaced "
        "evenly across the selection; \"Fit width to selection\" snaps them to "
        "tile with no gaps.");
    syncCrossfadeMaxToWindow();
}

void CaptureFromSongDialog::updatePreviewIndexControl() {
    const int n = (int)std::round(numFramesSlider.getValue());
    // Clamp the current pick into [1, n] before re-ranging.
    const int cur = juce::jlimit(1, std::max(1, n),
                                 (int)std::round(previewIndexSlider.getValue()));
    previewIndexSlider.setRange(1.0, (double)std::max(1, n), 1.0);
    previewIndexSlider.setValue((double)cur, juce::dontSendNotification);

    const bool enabled = (n >= 2);
    previewIndexSlider.setEnabled(enabled);
    previewIndexLabel.setEnabled(enabled);
    if (enabled) {
        previewIndexSlider.setTooltip(
            "Which of the " + juce::String(n) + " captured waveforms the Paused "
            "audition plays. 1 = the first (earliest in the selection), "
            + juce::String(n) + " = the last. Change it while paused to "
            "audition a different waveform without stopping.");
    } else {
        previewIndexSlider.setTooltip(
            "Disabled with a single waveform - there is only one to hear, and "
            "the audition plays the whole selection. Increase \"Waveforms to "
            "slice out\" above 1 to choose among several.");
    }
}

void CaptureFromSongDialog::updateRegionInfoLabel() {
    if (!songPcm || songPcm->empty() || songSampleRate <= 0.0) {
        regionInfoLabel.setText("(waiting for render...)", juce::dontSendNotification);
        return;
    }
    const int64_t spanSamples = std::max<int64_t>(0, regionEnd - regionStart);
    const double spanSec = (double)spanSamples / songSampleRate;
    const int n = (int)std::round(numFramesSlider.getValue());
    const double perFrameMs = (n > 0) ? (spanSec * 1000.0 / (double)n) : 0.0;
    const int64_t srcLen = effectiveSrcLen();
    const double winMs = (double)srcLen * 1000.0 / songSampleRate;
    juce::String msg;
    msg << "Region: " << juce::String(spanSec, 2) << " s   |   "
        << n << " waveforms, "
        << juce::String(perFrameMs, 1) << " ms apart, each "
        << juce::String(winMs, 0) << " ms wide";
    regionInfoLabel.setText(msg, juce::dontSendNotification);
}

std::vector<std::unique_ptr<IWavetableFrame>>
CaptureFromSongDialog::buildFrames(int n) const {
    std::vector<std::unique_ptr<IWavetableFrame>> out;
    if (n <= 0 || !songPcm || songPcm->empty() || songSampleRate <= 0.0) return out;
    n = std::min(n, 32);
    out.reserve((size_t)n);

    // Each emitted frame is a BANDED GranularFrame: its source PCM is one freeze
    // WINDOW (= "Window length") of the song plus a half-window lookahead
    // tail for the loop seam, with windowStart = 0 / windowLen = the band so the
    // voice runs its clean per-mode banded math. The grain is the separate
    // "Grain length" control. embeddedPitchHz comes from the in-dialog pitch
    // picker (Note + Octave, default A4 = 440 Hz). Identical to the file dialog's
    // buildFrames so song / file / mic captures all produce the same frame shape.
    const double sr        = songSampleRate;
    const int64_t windowLen = effectiveSrcLen();
    // Cap the grain to the window so a grain wider than the selection can't make
    // the engine swallow the whole source and silence the frame (see the file
    // dialog's buildFrames and regenerateAuditionGrain for the full rationale).
    const int     grainLen  = std::min<int>(effectiveGrainLen(), (int)windowLen);

    // Seam crossfade: the Crossfade slider's ms value (clamped to window/2 by the
    // player), matching the audition so capture sounds like preview.
    const int xfadeSamples = std::min(
        std::max(0, (int)std::round(crossfadeSlider.getValue() * 0.001 * sr)),
        std::max(0, (int)(windowLen / 2)));

    for (int i = 0; i < n; ++i) {
        const int64_t startIdx = bandStartForIndex(i, n, windowLen);
        std::vector<float> source = buildGrainSource(startIdx, windowLen);

        auto frame = std::make_unique<GranularFrame>(
            std::move(source), sr, grainLen, (float)capturedPitchHz,
            selectedFreezeMode(), xfadeSamples);
        // Banded: the band is the first windowLen samples of the source (the rest
        // is the seam lookahead). This is what gives the cloud modes roam room.
        frame->windowStart = 0;
        frame->windowLen   = (int)windowLen;
        // Per-waveform texture (grain count for cloud modes, FFT size for
        // Spectral) and per-frame output gain (the in-dialog Gain control).
        frame->grainCount = selectedGrainCount();
        frame->fftSize    = selectedFftSize();
        frame->gain       = (float)gainSlider.getValue();
        out.push_back(std::move(frame));
    }
    return out;
}

} // namespace SoundShop

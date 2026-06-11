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

// Sticky capture-dialog settings, remembered across opens within a session.
// The dialog seeds its controls from this on construction and writes the
// current values back on destruction, so re-opening the mic/file/playback
// capture dialog restores the user's last waveform count, gain, and labelled
// pitch instead of resetting to defaults every time. Process-global (one
// shared set for all three sources, since they share these controls);
// deliberately NOT persisted to disk - it's a session convenience, not a
// project/preference value. The per-waveform "Samples per waveform" window is
// intentionally excluded: it auto-fits to the selection/count on every open
// (syncWindowToFitSlots), so a remembered value would just be overwritten and
// fighting that would be confusing.
struct CaptureDialogPrefs {
    int    numFrames = 8;       // numFramesSlider
    double gain      = 1.0;     // gainSlider
    int    noteId    = 9 + 1;   // noteCombo selected id (A)
    int    octaveId  = 4 + 1;   // octaveCombo selected id (4)
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
    // Height bumped to make room for the embedded pitch row and the freeze-
    // mode row underneath the num-frames slider while keeping the waveform
    // area unchanged.
    setSize(720, 536);

    addAndMakeVisible(numFramesLabel);
    numFramesLabel.setText("Waveforms to slice out:", juce::dontSendNotification);
    numFramesLabel.setJustificationType(juce::Justification::centredRight);

    addAndMakeVisible(numFramesSlider);
    numFramesSlider.setRange(1.0, 32.0, 1.0);
    // Seed from the sticky session prefs so re-opening the dialog restores the
    // last waveform count (see captureDialogPrefs()).
    numFramesSlider.setValue((double)captureDialogPrefs().numFrames,
                             juce::dontSendNotification);
    numFramesSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    numFramesSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
    numFramesSlider.setTooltip(
        "Slices the selected region into this many short snapshots. The "
        "wavetable morphs through them as its Position parameter sweeps from "
        "0 to 1, so the captured sound evolves the way the source did. "
        "1 = a single frozen snapshot; more = smoother evolution but more "
        "memory.");
    numFramesSlider.onValueChange = [this]() {
        // Mic: refit the per-waveform window to the new slot spacing so the
        // bands stay zero-gap when the count changes. Gaps / overlap are
        // reserved for region resizing, so we only refit here, not on a
        // handle drag. No-op for the auto-sizing sources.
        syncWindowToFitSlots();
        // Enable / disable + relabel the per-waveform slider for the new count
        // (disabled at 1 waveform, where the window just spans the selection).
        updateSamplesPerWaveformControl();
        // Re-range + clamp the "Preview waveform" picker to the new count.
        updatePreviewIndexControl();
        updateRegionInfoLabel();
        if (auditioning) regenerateAuditionGrain();
        repaint(waveRect);
    };

    // Mic only: per-waveform source-window length, in samples. Other sources
    // auto-size the window (see effectiveSrcLen). 48000 samples ~ 1 second at
    // 48 kHz, matching the auto default so the captured character is the same
    // until the user deliberately changes it.
    if (source == CaptureSource::Mic) {
        addAndMakeVisible(samplesPerWaveformLabel);
        samplesPerWaveformLabel.setText("Samples per waveform:", juce::dontSendNotification);
        samplesPerWaveformLabel.setJustificationType(juce::Justification::centredRight);

        addAndMakeVisible(samplesPerWaveformSlider);
        // Max = the full display/capture buffer (kDisplayWindowSamples) so the
        // window can always grow to match the whole selection - e.g. with one
        // waveform the section must be able to span the entire selection, which
        // can be the whole buffer. A smaller cap (the old 192000) left a single
        // band stuck far short of a large selection. Step of 1 sample so the
        // window can land exactly on the slot spacing (regionLen / n); a coarse
        // step would leave the bands a few samples shy of tiling and reopen a
        // hairline gap.
        samplesPerWaveformSlider.setRange(2000.0, (double)kDisplayWindowSamples, 1.0);
        samplesPerWaveformSlider.setValue(48000.0, juce::dontSendNotification);
        samplesPerWaveformSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        samplesPerWaveformSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 22);
        // Tooltip + enabled state are set by updateSamplesPerWaveformControl(),
        // which also handles the single-waveform case where this slider is
        // disabled (the lone window just spans the whole selection). Called
        // below once the control exists, and again whenever the count or region
        // changes.
        samplesPerWaveformSlider.onValueChange = [this]() {
            updateRegionInfoLabel();
            if (auditioning) regenerateAuditionGrain();
            repaint(waveRect);
        };

        addAndMakeVisible(fitWidthBtn);
        // Tooltip + enabled state for both this button and the slider are set by
        // updateSamplesPerWaveformControl() (called below), which disables both
        // at a single waveform.
        fitWidthBtn.onClick = [this]() {
            // syncWindowToFitSlots writes the slider with dontSendNotification
            // (so it can be reused from the count handler without recursing),
            // so refresh the dependent state here the way onValueChange would.
            syncWindowToFitSlots();
            updateRegionInfoLabel();
            if (auditioning) regenerateAuditionGrain();
            repaint(waveRect);
        };

        updateSamplesPerWaveformControl();
    }

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
                       "If the input sounds wrong (garbled, noisy, or like your computer's "
                       "own audio), click \"Audio device...\" and switch the driver type to "
                       "DirectSound.";
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

        // Escape hatch for the WASAPI-combined-device input-corruption bug:
        // open the Audio Device Settings so the user can switch the driver
        // type (DirectSound captures a webcam/USB mic correctly when WASAPI
        // garbles it). See the note appended to the Mic hint text.
        addAndMakeVisible(audioDeviceBtn);
        audioDeviceBtn.setTooltip(
            "Open Audio Device Settings. If the microphone sounds wrong "
            "(garbled, noisy, or like your computer's own audio), change the "
            "driver type to DirectSound here - that fixes the most common "
            "Windows capture problem.");
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
        "Drives the Preview you're hearing, so you can A/B before capturing.");
    freezeModeCombo.onChange = [this]() {
        const int idx = freezeModeCombo.getSelectedId() - 1;
        if (idx < 0) return;
        if (auto* eng = AudioEngine::getInstance())
            eng->setGrainFreezeMode((AudioEngine::GrainFreezeMode)idx);
        // Re-spin the audition so the new mode re-anchors on the current slice.
        if (auditioning) regenerateAuditionGrain();
    };

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
    reverseSliderFill(samplesPerWaveformSlider);
    reverseSliderFill(gainSlider);
    reverseSliderFill(previewIndexSlider);

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
        syncWindowToFitSlots();   // Mic: zero-gap bands on first fill.
        updateSamplesPerWaveformControl();  // single-waveform: window = selection
        updateRegionInfoLabel();
    } else {
        regionStart = juce::jlimit(0, sz, regionStart);
        regionEnd   = juce::jlimit(regionStart, sz, regionEnd);
        // Keep the single-waveform window tracking the (re-clamped) selection.
        updateSamplesPerWaveformControl();
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
    // Mic gets an extra "Samples per waveform" row directly above the
    // buttons. Built before the waveforms-count row so the two read
    // top-to-bottom as "Waveforms to slice out" then "Samples per waveform".
    if (source == CaptureSource::Mic) {
        auto sampRow = r.removeFromBottom(26);
        samplesPerWaveformLabel.setBounds(sampRow.removeFromLeft(140));
        samplesPerWaveformSlider.setBounds(sampRow.removeFromLeft(260));
        sampRow.removeFromLeft(12);
        fitWidthBtn.setBounds(sampRow.removeFromLeft(170));
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

    waveRect = r;
}

int CaptureFromPlaybackDialog::xForIdx(int idx) const {
    if (tap.empty() || waveRect.getWidth() <= 0) return waveRect.getX();
    const float t = (float)idx / (float)tap.size();
    return waveRect.getX() + (int)std::round(t * waveRect.getWidth());
}

int CaptureFromPlaybackDialog::idxForX(int x) const {
    if (tap.empty() || waveRect.getWidth() <= 0) return 0;
    const float t = (float)(x - waveRect.getX()) / (float)waveRect.getWidth();
    return juce::jlimit(0, (int)tap.size(), (int)std::round(t * tap.size()));
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
    if (dragHandle == 0) {
        regionStart = juce::jlimit(0, regionEnd, idxForX(e.x));
    } else if (dragHandle == 1) {
        regionEnd = juce::jlimit(regionStart, sz, idxForX(e.x));
    } else if (dragHandle == 2) {
        const int span = regionEnd - regionStart;
        const int newStart = juce::jlimit(0, std::max(0, sz - span),
                                          idxForX(e.x) - dragAnchorSampleOffset);
        regionStart = newStart;
        regionEnd   = newStart + span;
    }
    // With one waveform the window tracks the selection, so keep the (disabled)
    // per-waveform slider showing the live selection length while dragging.
    updateSamplesPerWaveformControl();
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

    // Min/max waveform render: one vertical line per pixel.
    const int W = waveRect.getWidth();
    const int H = waveRect.getHeight();
    const int sz = (int)tap.size();
    const int x0 = waveRect.getX();
    const int yMid = waveRect.getY() + H / 2;
    const int halfH = H / 2;
    g.setColour(juce::Colour(110, 130, 200));
    for (int px = 0; px < W; ++px) {
        const int sStart = (int)((int64_t)px * sz / W);
        const int sEnd   = std::min(sz, (int)((int64_t)(px + 1) * sz / W));
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
        //    on "Samples per waveform", never on the selection size. Stretching
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

int CaptureFromPlaybackDialog::effectiveSrcLen() const {
    const int    regionLen = std::max(0, regionEnd - regionStart);
    const double sr        = (tapSampleRate > 0.0) ? tapSampleRate : 48000.0;
    const int    grainLen  = std::max(64, (int)std::round(0.1 * sr));  // 100 ms
    const int    tapLen    = (int)tap.size();

    if (source == CaptureSource::Mic) {
        const int n = (int)std::round(numFramesSlider.getValue());
        if (n <= 1) {
            // A single waveform spans the WHOLE selection - there is no per-
            // waveform spacing to honour, so the one window simply is the
            // selection. The "Samples per waveform" slider is disabled in this
            // case (see updateSamplesPerWaveformControl), so it can't be the
            // source of truth here; the selection size is.
            int w = regionLen;
            w = std::max(w, grainLen);      // OLA needs at least one grain
            if (tapLen > 0) w = std::min(w, tapLen);
            return w;
        }
        // 2+ waveforms: the "Samples per waveform" slider is the single source
        // of truth for the window length - a fixed sample count, independent of
        // the region size. Resizing the selection then re-spaces the bands
        // without resizing them (the constant window keeps each band's width
        // constant). The slider's step is 1 sample so it can land exactly on the
        // slot spacing (regionLen / n) for the zero-gap tiling default.
        int w = (int)std::round(samplesPerWaveformSlider.getValue());
        w = std::max(w, grainLen);          // OLA needs at least one grain
        if (tapLen > 0) w = std::min(w, tapLen);
        return w;
    }

    // File / Playback auto-size: ~1 s or 4x grain, capped to the region.
    int wanted = std::max((int)std::llround(sr), grainLen * 4);
    wanted = std::max(wanted, grainLen);
    const int cap = regionLen > 0 ? regionLen : tapLen;
    if (cap <= 0) return 0;
    return std::min(wanted, cap);
}

int CaptureFromPlaybackDialog::bandStartForIndex(int i, int n, int srcLen) const {
    if (n <= 0 || srcLen <= 0) return regionStart;
    const int regLen = std::max(0, regionEnd - regionStart);
    const int tapLen = (int)tap.size();

    // Two regimes, meeting continuously at the point where the windows exactly
    // tile the selection (freeSpace == 0):
    //
    //  * WINDOWS FIT (freeSpace >= 0) - uniform-gap model. The leftover space is
    //    split into n+1 EQUAL gaps: one before the first window, one between each
    //    adjacent pair, and one after the last. So the two end margins (leftmost
    //    band to the left handle, rightmost band to the right) always equal the
    //    inter-band gaps, and every gap grows / shrinks together as the
    //    selection is resized.
    //        gap     = freeSpace / (n + 1)
    //        start_i = regionStart + (i + 1)*gap + i*srcLen
    //
    //  * WINDOWS OVERLAP (freeSpace < 0) - contained model. The windows are
    //    wider than their share, so they must overlap. Rather than let a uniform
    //    NEGATIVE end margin push the outer bands past the handles (the selection
    //    visibly spilling its bounds), we keep the row CONTAINED: band 0 flush to
    //    the left handle, band n-1 flush to the right, the overlap distributed
    //    evenly in between.
    //        step    = (regLen - srcLen) / (n - 1)   // start-to-start spacing
    //        start_i = regionStart + i*step
    //
    // At freeSpace == 0 both give the edge-to-edge tiling (gap 0 / step srcLen)
    // that syncWindowToFitSlots and the "Fit width to selection" button produce.
    const double freeSpace = (double)regLen - (double)n * (double)srcLen;
    double startD;
    if (freeSpace >= 0.0) {
        const double gap = freeSpace / (double)(n + 1);
        startD = (double)regionStart + (double)(i + 1) * gap
               + (double)i * (double)srcLen;
    } else if (n == 1) {
        // A lone window wider than the selection can't be contained; centre it.
        startD = (double)regionStart + 0.5 * ((double)regLen - (double)srcLen);
    } else {
        const double step = (double)(regLen - srcLen) / (double)(n - 1);
        startD = (double)regionStart + (double)i * step;
    }
    int startIdx = (int)std::lround(startD);
    // Clamp to the available audio only. The window length is honored as-is,
    // so a window wider than the region honestly extends past the handles
    // rather than being silently shrunk.
    startIdx = juce::jlimit(0, std::max(0, tapLen - srcLen), startIdx);
    return startIdx;
}

void CaptureFromPlaybackDialog::syncWindowToFitSlots() {
    if (source != CaptureSource::Mic) return;
    // Set the window to exactly one slot spacing (regionLen / n) so the n
    // bands tile the selection edge-to-edge with zero gaps. This is the
    // zero-gap default applied when the count changes or on first fill; the
    // user can then override it via the slider, and resizing the selection
    // afterwards keeps this window size (sliding the bands, not rescaling).
    // dontSendNotification so this programmatic set doesn't recurse through
    // onValueChange.
    const int n      = (int)std::round(numFramesSlider.getValue());
    const int regLen = std::max(0, regionEnd - regionStart);
    if (n <= 0 || regLen <= 0) return;
    const double spacing = (double)regLen / (double)n;
    samplesPerWaveformSlider.setValue(spacing, juce::dontSendNotification);
}

void CaptureFromPlaybackDialog::updateSamplesPerWaveformControl() {
    if (source != CaptureSource::Mic) return;
    const int n = (int)std::round(numFramesSlider.getValue());
    if (n <= 1) {
        // One waveform spans the whole selection: there is nothing for this
        // slider to control, so lock it and show it holding the selection
        // length (kept current as the selection is resized) instead of a stale
        // value. The window length itself comes from effectiveSrcLen, which
        // returns the selection size in this case.
        const int regLen = std::max(0, regionEnd - regionStart);
        samplesPerWaveformSlider.setValue((double)regLen, juce::dontSendNotification);
        samplesPerWaveformSlider.setEnabled(false);
        samplesPerWaveformSlider.setTooltip(
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
        return;
    }
    samplesPerWaveformSlider.setEnabled(true);
    fitWidthBtn.setEnabled(true);
    fitWidthBtn.setTooltip(
        "Set the per-waveform width so the bands exactly tile the current "
        "selection - no gaps, no overlap. Same as dragging \"Samples per "
        "waveform\" until each band abuts the next. Use it after resizing "
        "the selection to snap back to a clean edge-to-edge layout.");
    samplesPerWaveformSlider.setTooltip(
        "How many audio samples each captured waveform spans - the length of "
        "the source snapshot behind one waveform. Bigger = each waveform holds "
        "a longer slice of sound (more of the timbre's movement); smaller = a "
        "tighter, more frozen moment. At 48 kHz, 48000 samples is about 1 "
        "second. The shaded orange bands over the waveform show each one's "
        "span, each exactly this wide and spaced evenly across the selection "
        "with the same gap everywhere - including the margins to the two "
        "handles. When the selection equals this times the waveform count "
        "the bands tile it with no gaps; a wider selection opens every gap "
        "equally. Make this large enough that the bands overlap and they "
        "stay contained within the selection (first flush left, last flush "
        "right) instead of spilling past the handles. The band width never "
        "changes, so resizing re-spaces the bands rather than rescaling.");
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

int CaptureFromPlaybackDialog::loopLenForWindow(int windowLen) const {
    if (windowLen <= 0) return 0;
    const int n = (int)std::round(numFramesSlider.getValue());
    if (n <= 1) {
        // Single waveform: the loop is the whole window (= whole selection),
        // so the captured frame plays the entire selected sound on repeat.
        return std::max(16, windowLen);
    }
    // Multiple waveforms: cap the loop at a ~100 ms neutral grain so each
    // frame is a stationary timbral snapshot the wavetable morphs through,
    // but never longer than the window itself.
    const double sr = (tapSampleRate > 0.0) ? tapSampleRate : 48000.0;
    const int neutralGrain = std::max(64, (int)std::round(0.1 * sr));
    return std::max(16, std::min(neutralGrain, windowLen));
}

std::vector<float>
CaptureFromPlaybackDialog::buildGrainSource(int startIdx, int loopLen) const {
    const int tapLen       = (int)tap.size();
    const int reservedTail = std::max(0, loopLen / 2);
    const int srcLen       = std::max(0, loopLen + reservedTail);
    std::vector<float> source((size_t)srcLen, 0.0f);
    for (int s = 0; s < srcLen; ++s) {
        const int idx = startIdx + s;
        if (idx >= 0 && idx < tapLen)
            source[(size_t)s] = tap[(size_t)idx];
    }
    return source;
}

std::vector<std::unique_ptr<IWavetableFrame>>
CaptureFromPlaybackDialog::buildFrames(int n) const {
    std::vector<std::unique_ptr<IWavetableFrame>> out;
    if (n <= 0 || tap.empty()) return out;
    n = std::min(n, 32);
    out.reserve((size_t)n);

    // Mic/file capture mirrors the song-capture path: each emitted frame
    // is a GranularFrame that loops a window of the source PCM. The loop
    // length depends on the waveform count (loopLenForWindow): a single
    // waveform loops the WHOLE window (= whole selection) so the frame
    // plays back the entire recorded sound, while multiple waveforms cap
    // the loop at a ~100 ms timbral snapshot the wavetable Position
    // parameter morphs through. embeddedPitchHz comes from the in-dialog
    // pitch picker (Note + Octave, default A4 = 440 Hz) so MIDI playback at
    // the matching key plays the source at 1:1 and other keys pitch-shift
    // via the usual wavetable stride math.
    const double sr      = (tapSampleRate > 0.0) ? tapSampleRate : 48000.0;

    // Per-frame source WINDOW length (the band width). For Mic this comes
    // from the "Samples per waveform" slider (or the whole selection when
    // there is a single waveform); for File / Playback it auto-sizes. The
    // section bands are drawn from the same windowLen so the drawn bands and
    // the captured loops agree.
    const int windowLen  = effectiveSrcLen();
    const int loopLen    = loopLenForWindow(windowLen);

    // Seam crossfade: ~50 ms (clamped to L/2 by the player), matching the
    // audition so capture sounds like preview. Stored on the frame so the
    // synth's CrossfadeLoop reader uses the same blend.
    const int xfadeSamples = std::min(std::max(0, (int)std::round(0.05 * sr)),
                                      std::max(0, loopLen / 2));

    for (int i = 0; i < n; ++i) {
        // Same slot geometry the section bands draw, so the captured loop
        // body matches the on-screen bands exactly. buildGrainSource adds
        // the loopLen/2 lookahead tail past the band end for the seam.
        const int startIdx = bandStartForIndex(i, n, windowLen);
        std::vector<float> source = buildGrainSource(startIdx, loopLen);

        auto frame = std::make_unique<GranularFrame>(
            std::move(source), sr, loopLen, (float)capturedPitchHz,
            selectedFreezeMode(), xfadeSamples);
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
    // bakes, so the preview is honest: the single-waveform case loops the
    // whole selection (you hear the recorded sound), the multi-waveform case
    // auditions whichever waveform the "Preview waveform" picker selects.
    // windowLen is the band width (effectiveSrcLen); loopLen is what actually
    // loops.
    const int n        = (int)std::round(numFramesSlider.getValue());
    const int windowLen = effectiveSrcLen();
    if (windowLen <= 0) return;
    const int loopLen  = loopLenForWindow(windowLen);
    if (loopLen <= 0) return;

    // 0-based band index. With a single waveform there is only band 0; with
    // 2+ the "Preview waveform" slider (1-based) chooses, clamped into range.
    const int repIdx   = (n <= 1)
        ? 0
        : juce::jlimit(0, n - 1,
                       (int)std::round(previewIndexSlider.getValue()) - 1);
    const int startIdx = bandStartForIndex(repIdx, n, windowLen);
    auto src = std::make_shared<std::vector<float>>(buildGrainSource(startIdx, loopLen));

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
    eng->setPreviewGrainRatio((float)(440.0 / hz));
    // Publish the source's natural pitch so PitchSyncGrains can derive the
    // loop period; the other freeze modes ignore it.
    eng->setPreviewEmbeddedPitch((float)hz);
    eng->setPreviewGrainLength(loopLen);
    // ~50 ms seam crossfade, matching buildFrames; engine clamps to L/2.
    const int xfadeSamples = std::min(std::max(0, (int)std::round(0.05 * sr)),
                                      std::max(0, loopLen / 2));
    eng->setPreviewCrossfadeLength(xfadeSamples);
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
    // Height bumped to make room for the embedded pitch row above the
    // freeze-mode picker while keeping the waveform area unchanged.
    setSize(820, 584);

    // ----- Transport row -----
    addAndMakeVisible(playBtn);
    playBtn.setTooltip(
        "Play the rendered song from the marker forward at full fidelity. "
        "Use this to identify the spot you want to capture; the marker "
        "tracks the playhead so you see exactly where the audio is.");
    playBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(60, 110, 70));
    playBtn.onClick = [this]() { setState(TState::Playing); };

    addAndMakeVisible(pauseBtn);
    pauseBtn.setTooltip(
        "Pause playback and switch to grain-loop audition: a short window "
        "centered on the marker loops continuously so you can hear what "
        "would be captured. Adjust 'Width' to change the grain length.");
    pauseBtn.onClick = [this]() { setState(TState::Paused); };

    addAndMakeVisible(stopBtn);
    stopBtn.setTooltip("Stop and rewind the marker to the start of the song.");
    stopBtn.onClick = [this]() { setState(TState::Stopped); };

    // ----- Width slider -----
    addAndMakeVisible(widthLabel);
    widthLabel.setText("Width:", juce::dontSendNotification);
    widthLabel.setJustificationType(juce::Justification::centredRight);

    addAndMakeVisible(widthSlider);
    widthSlider.setRange(kWidthMinMs, kWidthMaxMs, 1.0);
    widthSlider.setValue(kWidthDefMs, juce::dontSendNotification);
    widthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    widthSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 22);
    widthSlider.setTextValueSuffix(" ms");
    widthSlider.setTooltip(
        "How wide a window around the marker the captured grain comes "
        "from, in milliseconds. Smaller = tighter pluck-like tone; "
        "larger = pad / drone-like tone with more harmonic content. "
        "Also sets the length of the audition loop you hear when paused.");
    widthSlider.onValueChange = [this]() {
        updateStatusLabel();
        // Crossfade length is capped to width/2 by the engine (see
        // audio_engine.cpp's `xfade = min(xfadeReq, grainLen / 2)`);
        // expose that cap in the slider's max so the visible value
        // always equals the effective value. Without this, the user
        // sees a 500 ms slider but hears no change past width/2.
        syncCrossfadeMaxToWidth();
        // Live-update the grain while paused or scrubbing so the user
        // hears the width change immediately. No effect while Playing
        // (engine is reading the rendered song PCM, not the grain).
        if (state == TState::Paused || state == TState::Scrubbing)
            regenerateGrain();
    };

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
        "(close to the cap = half the Width): the whole loop is "
        "essentially a rolling crossfade between two playheads, "
        "smoother but more blurred. The slider's max tracks half the "
        "current Width because the engine can't overlap two ramps "
        "longer than that within one loop. Effective only with "
        "Freeze = Crossfade loop.");
    crossfadeSlider.onValueChange = [this]() {
        // Only treat this as the user setting their desired crossfade
        // when it's NOT a synthetic notification from setRange clamping
        // the value during a Width change. The guard is flipped on
        // exactly around syncCrossfadeMaxToWidth's slider mutations.
        if (!syncingCrossfadeFromWidth)
            crossfadeDesiredMs = crossfadeSlider.getValue();
        updateStatusLabel();
        if (state == TState::Paused || state == TState::Scrubbing)
            regenerateGrain();
        // Unified save model: write the crossfade through to the live frame in
        // re-capture mode - but only for genuine user moves, not the synthetic
        // value the width-clamp injects (that would clobber the frame's xfade
        // when the user only touched Width, a capture-time param). No-op in
        // append mode.
        if (!syncingCrossfadeFromWidth)
            publishMetadataEdit();
    };
    // Initial range: cap at width/2. Width slider has already been
    // configured above with its default value, so this is the right
    // moment to mirror it into the crossfade max.
    syncCrossfadeMaxToWidth();

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
        "How the loop sustains the marker spot while a captured note is "
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
        // Re-spin the audition source so the new mode reanchors on the
        // current marker spot. No-op if not currently in GrainLoop.
        if (state == TState::Paused || state == TState::Scrubbing)
            regenerateGrain();
        // Unified save model: write the freeze mode through to the live frame
        // in re-capture mode. No-op in append.
        publishMetadataEdit();
    };

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
        // Retune the live marker audition immediately so picking "As note" is
        // audible right away, matching what the synth voice will play.
        publishPreviewPitch();
        // Unified save model: in re-capture mode, write the new pitch label
        // straight through to the live frame so closing the panel can't lose
        // it (matches the frame editor's per-edit commit). No-op in append.
        publishMetadataEdit();
    };
    noteCombo.onChange   = onPitchPickerChanged;
    octaveCombo.onChange = onPitchPickerChanged;

    // ----- Status / hint labels -----
    addAndMakeVisible(statusLabel);
    statusLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setFont(juce::Font(juce::FontOptions(13.0f)));
    statusLabel.setColour(juce::Label::textColourId, juce::Colour(180, 180, 200));

    addAndMakeVisible(hintLabel);
    hintLabel.setText(
        "Play the song or drag the marker to find the spot you want to "
        "capture, then press Save. While paused or dragging, you'll hear "
        "a looped grain centered on the marker - that's what gets saved.",
        juce::dontSendNotification);
    hintLabel.setJustificationType(juce::Justification::topLeft);
    hintLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    hintLabel.setColour(juce::Label::textColourId, juce::Colour(160, 160, 180));

    // ----- Save / Close -----
    addAndMakeVisible(saveBtn);
    saveBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(60, 110, 70));
    saveBtn.onClick = [this]() {
        auto frames = buildFrameAtMarker();
        if (onCapture && !frames.empty()) onCapture(std::move(frames));
        // Keep the dialog open after saving so the user can continue
        // scrubbing for more spots. The audition keeps running.
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

    // ----- Kick off the offline render -----
    // Figure out song length the same way doExportRender does: max(end
    // of clip) over all timeline nodes; add a 4-beat tail so reverb /
    // release rings out. If the project has no clips, fall back to 4
    // beats so the user gets *something* (silent) to scrub.
    float maxBeat = 0.0f;
    for (const auto& n : graph.nodes)
        for (const auto& c : n.clips)
            maxBeat = std::max(maxBeat, c.startBeat + c.lengthBeats);
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

    auto bottomRow = r.removeFromBottom(34);
    cancelBtn.setBounds(bottomRow.removeFromRight(100));
    bottomRow.removeFromRight(8);
    saveBtn.setBounds(bottomRow.removeFromRight(200));

    r.removeFromBottom(8);

    // Width slider row.
    auto widthRow = r.removeFromBottom(26);
    widthLabel.setBounds(widthRow.removeFromLeft(70));
    widthSlider.setBounds(widthRow.removeFromLeft(360));
    widthRow.removeFromLeft(12);
    statusLabel.setBounds(widthRow);

    r.removeFromBottom(6);

    // Crossfade slider row (mirrors width-row geometry so the two
    // sliders line up).
    auto xfadeRow = r.removeFromBottom(26);
    crossfadeLabel.setBounds(xfadeRow.removeFromLeft(70));
    crossfadeSlider.setBounds(xfadeRow.removeFromLeft(360));

    r.removeFromBottom(6);

    // Embedded pitch row: label + note + octave + cents readout.
    auto pitchRow = r.removeFromBottom(26);
    noteOctaveLabel.setBounds(pitchRow.removeFromLeft(70));
    noteCombo.setBounds(pitchRow.removeFromLeft(intrinsicComboWidth(noteCombo)));
    pitchRow.removeFromLeft(6);
    octaveCombo.setBounds(pitchRow.removeFromLeft(intrinsicComboWidth(octaveCombo)));
    pitchRow.removeFromLeft(10);
    centsLabel.setBounds(pitchRow.removeFromLeft(80));

    r.removeFromBottom(6);

    // Freeze-mode row: dropdown + brief space (status label stays on
    // the width row above, so this row is the picker alone). Width is
    // measured from the combo's longest item so the dropdown text never
    // gets clipped behind the arrow.
    auto freezeRow = r.removeFromBottom(26);
    freezeModeLabel.setBounds(freezeRow.removeFromLeft(70));
    freezeModeCombo.setBounds(
        freezeRow.removeFromLeft(intrinsicComboWidth(freezeModeCombo)));

    r.removeFromBottom(8);

    // Transport row.
    auto transportRow = r.removeFromBottom(28);
    playBtn.setBounds(transportRow.removeFromLeft(70));
    transportRow.removeFromLeft(4);
    pauseBtn.setBounds(transportRow.removeFromLeft(70));
    transportRow.removeFromLeft(4);
    stopBtn.setBounds(transportRow.removeFromLeft(70));

    r.removeFromBottom(6);
    hintLabel.setBounds(r.removeFromBottom(36));
    r.removeFromBottom(4);

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
    // the latest value in the marker so it tracks the playhead. If the
    // engine has auto-stopped (song finished), notice and update state.
    if (state == TState::Playing) {
        auto* eng = AudioEngine::getInstance();
        if (eng) {
            markerSamplePos = eng->getPreviewSongPosSamples();
            if (eng->getPreviewMode() == AudioEngine::PreviewMode::Off) {
                // Song ran past the end. Snap back to Stopped so the
                // user can pick a new spot.
                state = TState::Stopped;
                markerSamplePos = 0;
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
        markerSamplePos = 0;
    }
    if (songPcm && !songPcm->empty()) {
        if (auto* eng = AudioEngine::getInstance())
            eng->setPreviewSongPcm(songPcm);
        markerSamplePos = 0;
    }
    // Auto-enter Paused so the user hears the grain loop at the marker
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
            eng->setPreviewSongPosSamples(markerSamplePos);
            eng->setPreviewMode(AudioEngine::PreviewMode::SongPlay);
            break;
        case TState::Paused:
        case TState::Scrubbing:
            regenerateGrain();
            eng->setPreviewMode(AudioEngine::PreviewMode::GrainLoop);
            break;
        case TState::Stopped:
            markerSamplePos = 0;
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
    widthSlider.setEnabled(ready);
    crossfadeSlider.setEnabled(ready);
    freezeModeCombo.setEnabled(ready);

    // Save is disabled while Playing because the marker is moving and
    // the user can't precisely pick a spot until they pause/scrub. The
    // tooltip explains so the user isn't left wondering why.
    if (!ready) {
        saveBtn.setEnabled(false);
        saveBtn.setTooltip("Waiting for the project to finish rendering "
                           "before you can capture.");
    } else if (state == TState::Playing) {
        saveBtn.setEnabled(false);
        saveBtn.setTooltip(
            "Save is disabled during playback - pause or drag the marker "
            "to pick an exact spot first. Press Pause to switch to the "
            "audition loop and lock the marker in place.");
    } else {
        saveBtn.setEnabled(true);
        saveBtn.setTooltip(
            "Capture a single waveform at the marker position. The "
            "waveform is added to your wavetable along the Position dimension. "
            "You can keep scrubbing and saving more spots after this.");
    }
}

void CaptureFromSongDialog::syncCrossfadeMaxToWidth() {
    // Engine clamp is xfade = min(xfadeReq, grainLen/2). Mirror that
    // here so the slider's visible value always equals the audible
    // value. Floor the max at kXfadeMinMs to keep setRange happy when
    // width is at its smallest (10 ms -> 5 ms cap, well above the
    // 1 ms floor, but be defensive).
    //
    // The displayed slider value is min(desired, newMax) - so dropping
    // Width clamps the visible crossfade down, but raising Width back
    // up restores the user's original choice rather than leaving it
    // stuck at the clamp. crossfadeDesiredMs is the source of truth for
    // "what the user picked" and is only written by onValueChange when
    // syncingCrossfadeFromWidth is false. We set it true here so
    // setRange's internal clamp (which fires onValueChange synchronously
    // when it shrinks the value) doesn't overwrite the desired value
    // with the clamped one.
    const double widthMs = widthSlider.getValue();
    const double newMax = std::max(kXfadeMinMs, widthMs * 0.5);
    const double newVisible = std::min(crossfadeDesiredMs, newMax);

    syncingCrossfadeFromWidth = true;
    crossfadeSlider.setRange(kXfadeMinMs, newMax, 1.0);
    crossfadeSlider.setValue(newVisible, juce::dontSendNotification);
    syncingCrossfadeFromWidth = false;
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
    const double posSec = (double)markerSamplePos / songSampleRate;
    const double totSec = (double)songPcm->size() / songSampleRate;
    const double widthMs = widthSlider.getValue();
    juce::String msg;
    msg << "Marker: " << juce::String(posSec, 2) << " / "
        << juce::String(totSec, 2) << " s   |   Grain width: "
        << juce::String((int)std::round(widthMs)) << " ms";
    statusLabel.setText(msg, juce::dontSendNotification);
}

void CaptureFromSongDialog::regenerateGrain() {
    if (!songPcm || songPcm->empty() || songSampleRate <= 0.0) return;
    auto* eng = AudioEngine::getInstance();
    if (!eng) return;

    // The dialog publishes (a) a SOURCE buffer that's significantly
    // longer than the grain, and (b) the grain length N. The engine
    // then runs a proper granular synthesis OLA stream: 4 voices, each
    // Hann-windowed of length N, jittered random start positions inside
    // the source buffer. This is what gives the audition the
    // "continuous drone" character a granular synth produces, with no
    // seam to mask. It is NOT a loop of a baked clip - hence no
    // crossfade-into-the-head trickery here anymore.
    const int widthSamples = std::max(64,
        (int)std::round(widthSlider.getValue() * 0.001 * songSampleRate));

    // Source-buffer length: enough for the voices to roam without their
    // jittered grain instances colliding too often. Heuristic: max of
    // 4x the grain length and 1 second around the marker, clamped to
    // the song. A larger source buffer means richer variation in the
    // drone; too large and the "audition is centered on the marker"
    // illusion breaks. ~1 sec or 4 grains is a balanced default.
    const int64_t total = (int64_t)songPcm->size();
    int srcLenWanted = std::max((int64_t)widthSamples * 4,
                                 (int64_t)std::llround(songSampleRate));
    int srcLen = (int)std::min((int64_t)srcLenWanted, total);
    if (srcLen < widthSamples + 1) srcLen = (int)std::min((int64_t)widthSamples, total);
    if (srcLen <= 0) return;

    const int64_t halfSrc = srcLen / 2;
    int64_t startIdx = std::max<int64_t>(0, markerSamplePos - halfSrc);
    if (startIdx + srcLen > total)
        startIdx = std::max<int64_t>(0, total - srcLen);

    auto src = std::make_shared<std::vector<float>>((size_t)srcLen, 0.0f);
    const auto& song = *songPcm;
    for (int i = 0; i < srcLen; ++i) {
        const int64_t s = startIdx + i;
        if (s >= 0 && s < total)
            (*src)[(size_t)i] = song[(size_t)s];
    }

    // Publish grain length FIRST so by the time the engine swaps the
    // new buffer pointer in (next block), the N it reads matches the
    // buffer that's about to arrive. Reordering risk: if engine reads
    // length-then-buffer and a transient block lands in between, it
    // would use the new length against the old buffer for one block;
    // the engine guards against that via grainLen-vs-buffer-size check
    // and grainLastAppliedSrcPtr comparison, falling back to silence
    // if the pairing is invalid.
    // Re-apply the "As note" playback pitch every time we respin the source
    // (marker move, width/mode change) so the audition stays pitched.
    publishPreviewPitch();
    eng->setPreviewGrainLength(widthSamples);
    // Crossfade length, in samples, from the dedicated slider. The
    // engine clamps to [0, L/2] internally so it's always safe; we just
    // publish the user's requested value. Updating per-regenerate also
    // means moving the slider takes effect immediately.
    const int xfadeSamples = std::max(0,
        (int)std::round(crossfadeSlider.getValue() * 0.001 * songSampleRate));
    eng->setPreviewCrossfadeLength(xfadeSamples);
    eng->setPreviewGrainBuffer(std::move(src));
}

void CaptureFromSongDialog::seedFromExistingFrame(double pitchHz,
                                                  int freezeModeIdx,
                                                  double crossfadeMs) {
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
    if (freezeModeIdx >= 0 && freezeModeIdx <= 3) {
        freezeModeCombo.setSelectedId(freezeModeIdx + 1, juce::dontSendNotification);
        // Match the live audition to the seeded mode (the onChange handler that
        // would normally do this is suppressed by dontSendNotification).
        if (auto* eng = AudioEngine::getInstance())
            eng->setGrainFreezeMode((AudioEngine::GrainFreezeMode)freezeModeIdx);
    }

    // ----- Crossfade (ms) -----
    if (crossfadeMs >= 0.0) {
        // Width may cap the crossfade max below the frame's stored value; clamp
        // into the slider's current range so the visible value is honest, but
        // remember the frame's true intent in crossfadeDesiredMs so a later
        // Width increase can restore it (mirrors syncCrossfadeMaxToWidth).
        crossfadeDesiredMs = crossfadeMs;
        const double vis = juce::jlimit(crossfadeSlider.getMinimum(),
                                        crossfadeSlider.getMaximum(), crossfadeMs);
        crossfadeSlider.setValue(vis, juce::dontSendNotification);
    }
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
    onMetadataEdited(capturedPitchHz, fz, crossfadeDesiredMs);
}

void CaptureFromSongDialog::publishPreviewPitch() {
    auto* eng = AudioEngine::getInstance();
    if (!eng) return;
    // Reference note A4 (440 Hz) matches the wavetable editor's audition pitch
    // (kAuditionPitch = 69), so the marker preview, the editor's Play button,
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

int CaptureFromSongDialog::xForSamplePos(int64_t pos) const {
    if (!songPcm || songPcm->empty() || waveRect.getWidth() <= 0)
        return waveRect.getX();
    const double t = (double)pos / (double)songPcm->size();
    return waveRect.getX() + (int)std::round(t * waveRect.getWidth());
}

int64_t CaptureFromSongDialog::samplePosForX(int x) const {
    if (!songPcm || songPcm->empty() || waveRect.getWidth() <= 0)
        return 0;
    const double t = (double)(x - waveRect.getX()) / (double)waveRect.getWidth();
    const double clamped = juce::jlimit(0.0, 1.0, t);
    return (int64_t)std::round(clamped * (double)songPcm->size());
}

void CaptureFromSongDialog::mouseDown(const juce::MouseEvent& e) {
    if (!renderReady) return;
    if (!waveRect.contains(e.getPosition())) return;
    if (!songPcm || songPcm->empty()) return;

    draggingMarker = true;
    markerSamplePos = samplePosForX(e.x);
    setState(TState::Scrubbing);
}

void CaptureFromSongDialog::mouseDrag(const juce::MouseEvent& e) {
    if (!draggingMarker) return;
    if (!songPcm || songPcm->empty()) return;
    markerSamplePos = samplePosForX(e.x);
    // Live grain update during the drag so the audition follows the
    // mouse. setPreviewMode is unchanged (still GrainLoop) - just swap
    // the grain buffer atomically.
    regenerateGrain();
    updateStatusLabel();
    repaint(waveRect);
}

void CaptureFromSongDialog::mouseUp(const juce::MouseEvent&) {
    if (!draggingMarker) return;
    draggingMarker = false;
    // Drop into Paused: identical audio (grain loop) but UI semantics
    // are now "you're holding here", not "you're dragging right now".
    state = TState::Paused;
    updateButtonsForState();
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

    // Min/max waveform. One vertical line per pixel.
    const int W = waveRect.getWidth();
    const int H = waveRect.getHeight();
    const int64_t sz = (int64_t)songPcm->size();
    const int x0 = waveRect.getX();
    const int yMid = waveRect.getY() + H / 2;
    const int halfH = H / 2;
    const auto& v = *songPcm;
    g.setColour(juce::Colour(110, 130, 200));
    for (int px = 0; px < W; ++px) {
        const int64_t sStart = (int64_t)px * sz / W;
        const int64_t sEnd   = std::min(sz, (int64_t)(px + 1) * sz / W);
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

    // Grain-width band around the marker so the user sees how much
    // audio they're about to capture.
    const int widthSamples = std::max(64,
        (int)std::round(widthSlider.getValue() * 0.001 * songSampleRate));
    const int64_t halfW = widthSamples / 2;
    const int xBandL = xForSamplePos(std::max<int64_t>(0, markerSamplePos - halfW));
    const int xBandR = xForSamplePos(std::min<int64_t>(sz, markerSamplePos + halfW));
    if (xBandR > xBandL) {
        g.setColour(juce::Colour::fromFloatRGBA(1.0f, 0.65f, 0.2f, 0.18f));
        g.fillRect(juce::Rectangle<int>(xBandL, waveRect.getY(),
                                         xBandR - xBandL, H));
    }

    // Marker. The T-bar caps are 12 px wide centred on xm, so at xm == 0
    // (markerSamplePos == 0) they extend 6 px left of waveRect.getX() -
    // outside the rect. Without clipping, those stray pixels survive every
    // repaint(waveRect) and leave a stale "left half of the marker" stuck
    // at the left edge when the user moves the marker away.
    {
        juce::Graphics::ScopedSaveState s(g);
        g.reduceClipRegion(waveRect);
        const int xm = xForMarker();
        g.setColour(juce::Colour(255, 165, 60));
        g.fillRect(juce::Rectangle<int>(xm - 1, waveRect.getY(), 2, H));
        g.fillRect(juce::Rectangle<int>(xm - 6, waveRect.getY(), 12, 8));
        g.fillRect(juce::Rectangle<int>(xm - 6, waveRect.getBottom() - 8, 12, 8));
    }
}

std::vector<std::unique_ptr<IWavetableFrame>>
CaptureFromSongDialog::buildFrameAtMarker() const {
    std::vector<std::unique_ptr<IWavetableFrame>> out;
    if (!songPcm || songPcm->empty() || songSampleRate <= 0.0) return out;

    // Mirror the engine's audition source-buffer sizing in regenerateGrain:
    // a multi-second window around the marker, large enough that the OLA
    // voices have somewhere to roam. The grain length (envelope window N)
    // is the slider value in samples. The synth voice runs the same
    // 4-voice OLA against this source as the audition does, so what the
    // user heard is what they get.
    const int grainLenSamples = std::max(64,
        (int)std::round(widthSlider.getValue() * 0.001 * songSampleRate));

    const int64_t total = (int64_t)songPcm->size();
    int srcLenWanted = std::max((int64_t)grainLenSamples * 4,
                                 (int64_t)std::llround(songSampleRate));
    int srcLen = (int)std::min((int64_t)srcLenWanted, total);
    if (srcLen < grainLenSamples + 1) srcLen = (int)std::min((int64_t)grainLenSamples, total);
    if (srcLen <= 0) return out;

    const int64_t halfSrc = srcLen / 2;
    int64_t startIdx = std::max<int64_t>(0, markerSamplePos - halfSrc);
    if (startIdx + srcLen > total)
        startIdx = std::max<int64_t>(0, total - srcLen);

    std::vector<float> source((size_t)srcLen, 0.0f);
    const auto& song = *songPcm;
    for (int i = 0; i < srcLen; ++i) {
        const int64_t s = startIdx + i;
        if (s >= 0 && s < total)
            source[(size_t)i] = song[(size_t)s];
    }

    // Embedded pitch: pull from the in-dialog Note + Octave picker
    // (default A4 = 440 Hz) so MIDI playback at the matching key plays
    // the source at 1:1 and other keys pitch-shift via the synth's
    // wavetable stride math. The post-capture editor can still re-tune
    // by ear via its Hz slider if the user prefers an off-grid pitch.
    //
    // Freeze mode: pull from the picker so the frame replays at note-on
    // with the same algorithm the user just auditioned. The combo's
    // selected id is enum-value + 1; -1 if nothing selected (shouldn't
    // happen, but default to CrossfadeLoop to keep the saved frame
    // recoverable).
    const int comboIdx = freezeModeCombo.getSelectedId() - 1;
    const GranularFreezeMode mode = (comboIdx >= 0 && comboIdx <= 3)
        ? (GranularFreezeMode)comboIdx
        : GranularFreezeMode::CrossfadeLoop;
    // Crossfade length: snapshot the slider in samples-at-songSampleRate
    // so the saved frame's xfade is interpreted at the same rate as its
    // sourceSampleRate (the synth clamps to grainLength/2 at use time).
    const int xfadeSamples = std::max(0,
        (int)std::round(crossfadeSlider.getValue() * 0.001 * songSampleRate));
    auto frame = std::make_unique<GranularFrame>(
        std::move(source), songSampleRate, grainLenSamples,
        (float)capturedPitchHz, mode, xfadeSamples);
    out.push_back(std::move(frame));
    return out;
}

} // namespace SoundShop

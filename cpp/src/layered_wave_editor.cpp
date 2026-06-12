#define _USE_MATH_DEFINES
#include "layered_wave_editor.h"
#include "spectral_editor.h"     // SpectralFrame for __wavetable2__ decode + sub-editor
#include "wavelet_frame.h"        // WaveletFrame for __wavetable2__ decode
#include "wavelet_painter.h"      // WaveletPainterComponent for sub-editor
#include "sample_frame.h"         // SampleFrame for capture-from-playback frames
#include "granular_frame.h"       // GranularFrame for granular capture frames
#include "builtin_synth.h"   // WaveExprParser for Formula shape
#include "help_utils.h"
#include "dialog_helpers.h"       // launchToolDialog for the pop-out wavetable view
#include "capture_from_playback.h" // CaptureFromPlaybackDialog / CaptureFromSongDialog for the "from playback" frame source
#include "audio_engine.h"          // AudioEngine::getInstance() to fetch the live graph/transport for the song-render dialog
#include "effect_regions.h"        // getDistinctColor / kFixedPalette - shared palette with the graph's wire tag circles
#include <cmath>
#include <sstream>
#include <algorithm>
#include <random>
#include <limits>
#include <map>
#include <cctype>
#include <cstdlib>

namespace SoundShop {

// ==============================================================================
// Library colour palette (shared by arrangement view, library list, swatches)
// ==============================================================================
//
// Single source of truth: getDistinctColor() from effect_regions.h - the
// same palette the graph view uses for wire-identity tag circles. Tier 1
// is the 20-colour Trubetskoy "maximally distinct" set; tier 2 is 10
// darker/lighter variants; beyond index 30 the function falls back to a
// golden-angle hue walk that never collides for the first ~200 indices.
// Using the same palette here means a yellow waveform looks the same
// shade of yellow as a yellow wire tag - one visual vocabulary across
// the whole app.
int libraryPaletteSize() {
    // Number of NAMED palette entries we expose in the picker. The
    // function getDistinctColor() works for any non-negative index, so a
    // higher colorIdx loaded from a future-format file will still render
    // a colour - the picker just won't have a row for it.
    return kNumFixedColors; // 30 named entries (Trubetskoy 20 + 10 variants)
}

juce::Colour libraryPalette(int idx) {
    if (idx < 0) idx = 0;
    return juce::Colour(getDistinctColor(idx));
}

juce::Colour libraryEntryDisplayColor(const WaveformLibraryEntry* entry,
                                      int fallbackIdx) {
    if (entry && entry->colorIdx >= 0)
        return libraryPalette(entry->colorIdx);
    // Auto: deterministic by the entry's position in the library (same
    // strategy the graph uses for wire tag circles - getDistinctColor by
    // link id). Distinct entries get distinct colours without the user
    // having to pick, and Position-N stays the same colour across edits.
    return libraryPalette(std::max(0, fallbackIdx));
}

// ==============================================================================
// LibraryColorSwatch - small clickable swatch that opens the palette menu
// ==============================================================================

LibraryColorSwatch::LibraryColorSwatch() {
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setSize(16, 16);
}

void LibraryColorSwatch::setSwatchColor(juce::Colour c) {
    if (col != c) { col = c; repaint(); }
}

void LibraryColorSwatch::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(col);
    g.fillRoundedRectangle(r, 3.0f);
    g.setColour(juce::Colours::white.withAlpha(hover ? 0.95f : 0.65f));
    g.drawRoundedRectangle(r, 3.0f, hover ? 1.4f : 1.0f);
    // No Auto badge - the colour itself reads the same way to the user
    // whether it was picked explicitly or auto-assigned. The tooltip
    // explains what Auto means; the popup picker shows "Auto" as the
    // ticked menu item when applicable.
}

void LibraryColorSwatch::mouseUp(const juce::MouseEvent& e) {
    if (!getLocalBounds().contains(e.getPosition())) return;
    juce::PopupMenu m;
    m.addSectionHeader("Waveform colour");
    m.addItem(1000, "Auto (from waveform content)", true, isAuto);
    m.addSeparator();
    // Names match the kFixedPalette in effect_regions.h - same colours the
    // graph uses for wire-identity tag circles, so users can recognise them.
    static const char* kNames[] = {
        // Tier 1: Trubetskoy 20 maximally-distinct
        "Red", "Green", "Yellow", "Blue",
        "Orange", "Purple", "Cyan", "Magenta",
        "Lime", "Pink", "Teal", "Lavender",
        "Brown", "Beige", "Maroon", "Mint",
        "Olive", "Coral", "Navy", "Grey",
        // Tier 2: 10 variants
        "Dark red", "Dark green", "Dark yellow", "Light blue",
        "Dark orange", "Light purple", "Dark cyan", "Dark magenta",
        "Dark lime", "Dark pink"
    };
    const int n = libraryPaletteSize();
    for (int i = 0; i < n; ++i) {
        juce::PopupMenu::Item it;
        it.itemID = 1 + i;
        it.text = (i < (int)(sizeof(kNames)/sizeof(kNames[0])))
            ? juce::String(kNames[i])
            : juce::String("Colour ") + juce::String(i + 1);
        it.colour = libraryPalette(i);
        m.addItem(it);
    }
    juce::Component::SafePointer<LibraryColorSwatch> safe(this);
    m.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(this),
        [safe](int sel) {
            if (!safe || sel == 0) return;
            const int pickedIdx = (sel == 1000) ? -1 : (sel - 1);
            if (safe->onPick) safe->onPick(pickedIdx);
        });
}

// ==============================================================================
// GranularFrameEditorComponent - embedded right-pane editor for granular frames
// ==============================================================================
//
// Sits in the same screen area as SpectralEditorComponent / WaveletPainterComponent
// when the right-pane editor is bound to a GranularFrame. Shows the captured
// source PCM as a waveform thumbnail, lets the user tweak the four runtime
// params (grain length, crossfade, embedded pitch, freeze mode) live, and
// exposes a "Re-capture..." button that replaces the source PCM via the
// project's existing capture flow.
//
// onApply is called after any param change so the host editor can re-render
// the preview and commit to the node script. onRecapture is called when the
// user clicks the Re-capture button - the host opens the capture panel and
// routes the resulting frame back into the current library entry (so the
// re-captured source replaces in place, doesn't accumulate as a new entry).
class GranularFrameEditorComponent : public juce::Component,
                                     public juce::SettableTooltipClient {
public:
    // sendAuditionIn: optional callback to route the Play button through the
    // owning synth node's pendingAudition queue instead of the AudioEngine's
    // separate GrainLoop preview path. Signature: (bool noteOn, int pitch,
    // int velocity). When provided, the Play button sends a MIDI note-on
    // (default A4=69, vel 100) to the synth and Stop sends note-off, so the
    // audition renders through the actual voice / AHDSR envelope / Volume
    // param path - matching what a wired-up MIDI note would sound like. When
    // null, falls back to the legacy AudioEngine::setPreviewGrainBuffer path
    // (used by the CaptureFromSongDialog post-capture editor where there's
    // no owning synth node yet).
    GranularFrameEditorComponent(GranularFrame& f,
                                 std::function<void()> onApplyIn,
                                 std::function<void()> onRecaptureIn,
                                 std::function<void(bool, int, int)> sendAuditionIn = {})
        : frame(f), onApply(std::move(onApplyIn)),
          onRecapture(std::move(onRecaptureIn)),
          sendAudition(std::move(sendAuditionIn))
    {
        // Tooltip for the waveform thumbnail area (the amber selection band).
        // Sub-region of the component with no child covering it, so the
        // component-level tooltip shows here.
        setTooltip(
            "Captured source. The amber band is the freeze WINDOW - the slice "
            "the current freeze mode actually sustains. Drag the MIDDLE to move "
            "it; drag an EDGE to resize it. What resizing does depends on the "
            "freeze mode:\n"
            " - Async / Pitch-synced grains: the band is an independent window "
            "the grains roam over (one grain wide up to the whole capture). A "
            "wider window is what makes these two modes differ - pin it to the "
            "grain width and they sound the same.\n"
            " - Crossfade loop: the loop IS the whole band, so resizing sets the "
            "loop length (grain length is unused, its slider greys out).\n"
            " - Spectral freeze: the band sets the FFT analysis region; grain "
            "length is unused, its slider greys out.\n"
            "A dashed band is auto-placed; dragging or resizing places it "
            "explicitly.");

        addAndMakeVisible(titleLabel);
        titleLabel.setText("Captured granular waveform",
                           juce::dontSendNotification);
        titleLabel.setColour(juce::Label::textColourId,
                             juce::Colours::white.withAlpha(0.85f));
        titleLabel.setFont(juce::Font(13.0f, juce::Font::bold));

        addAndMakeVisible(sourceInfoLabel);
        sourceInfoLabel.setColour(juce::Label::textColourId,
                                  juce::Colours::white.withAlpha(0.6f));
        sourceInfoLabel.setFont(juce::Font(11.0f));

        auto setupSlider = [this](juce::Slider& s, juce::Label& lab,
                                  const juce::String& labelText,
                                  const juce::String& tip,
                                  double minV, double maxV, double step) {
            addAndMakeVisible(s);
            addAndMakeVisible(lab);
            lab.setText(labelText, juce::dontSendNotification);
            lab.setColour(juce::Label::textColourId,
                          juce::Colours::white.withAlpha(0.85f));
            lab.setFont(juce::Font(11.0f));
            s.setSliderStyle(juce::Slider::LinearHorizontal);
            // Grab the thumb relatively instead of teleporting the value to
            // wherever the track is clicked. With the default (snaps-to-mouse
            // == true), a single stray click low on the log-skewed Embedded
            // pitch track jumps the value into sub-bass (~D1, 36.7 Hz) with no
            // drag - the reported "As note was set to D1 and I never set it"
            // surprise. Relative drag means a click does nothing until you
            // actually move, so accidental clicks can't slam these values.
            s.setSliderSnapsToMousePosition(false);
            s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 18);
            s.setRange(minV, maxV, step);
            s.setTooltip(tip);
            lab.setTooltip(tip);
            // Reverse the LookAndFeel_V4 default fill so the BRIGHT segment
            // sits to the RIGHT of the thumb (unset / remaining range) and
            // the DIM segment sits to the LEFT (already-set value). This is
            // backwards from typical fill-from-zero sliders, but the user
            // wants it that way for these three (grain length, crossfade,
            // embedded pitch) because the bright bar reads as "headroom
            // left" instead of "amount applied". Swap by querying the
            // current trackColour / backgroundColour and writing them in
            // the opposite slots - that way any future theme tweak still
            // round-trips through this swap without us hardcoding hex
            // values that would drift from the rest of the UI.
            const auto bright = s.findColour(juce::Slider::trackColourId);
            const auto dim    = s.findColour(juce::Slider::backgroundColourId);
            s.setColour(juce::Slider::trackColourId,      dim);
            s.setColour(juce::Slider::backgroundColourId, bright);
        };

        setupSlider(grainLengthSlider, grainLengthLabel,
                    "Grain length",
                    "Length of each grain in the OLA cloud, in milliseconds. "
                    "Short (~5 ms, the default) = a smooth, CONSTANT cloud; "
                    "longer grains roam over a proportionally wider window and "
                    "so sound more evolving / less constant. Doesn't change the "
                    "captured source - just the size of the grains the synth "
                    "scatters across the selection band while a note is held. "
                    "Used only by the Async / Pitch-sync modes.",
                    1.0, 500.0, 0.5);
        grainLengthSlider.setTextValueSuffix(" ms");
        grainLengthSlider.onValueChange = [this]() { onGrainLengthChanged(); };

        // Number of overlapping grains in the OLA cloud. Only the two grain-
        // cloud modes use it; greys out for Crossfade / Spectral (tooltip set
        // per-mode in refreshGrainSliderForMode). Range matches the voice's
        // [2, kMaxGrains] clamp.
        setupSlider(grainCountSlider, grainCountLabel,
                    "Grains",
                    "How many overlapping grains make up the cloud. More = "
                    "denser, smoother; fewer = sparser, more granular. The level "
                    "stays constant as you change it.",
                    2.0, (double)GrainFreezeVoice::kMaxGrains, 1.0);
        grainCountSlider.onValueChange = [this]() { onGrainCountChanged(); };

        // FFT size for Spectral freeze. Discrete powers of two + Auto (largest
        // that fits the window). Greys out outside Spectral mode.
        addAndMakeVisible(fftSizeLabel);
        fftSizeLabel.setText("FFT size", juce::dontSendNotification);
        fftSizeLabel.setColour(juce::Label::textColourId,
                               juce::Colours::white.withAlpha(0.85f));
        fftSizeLabel.setFont(juce::Font(11.0f));
        addAndMakeVisible(fftSizeCombo);
        // ID 1 == Auto (fftSize 0); IDs 2.. map to the power-of-two sizes in
        // kFftSizes. Keep this table in sync with fftComboIdToSize / sizeToId.
        fftSizeCombo.addItem("Auto", 1);
        for (int i = 0; i < kNumSpectralFftSizes; ++i)
            fftSizeCombo.addItem(juce::String(kSpectralFftSizes[i]), i + 2);
        fftSizeCombo.setTooltip(
            "FFT size for Spectral freeze, in samples. Larger = finer frequency "
            "detail (more bins) but a coarser time window, and needs a wider "
            "selection band to fit. Auto picks the largest size that fits the "
            "band (up to 2048). Only used by Spectral freeze.");
        fftSizeCombo.onChange = [this]() { onFftSizeChanged(); };

        setupSlider(crossfadeSlider, crossfadeLabel,
                    "Crossfade",
                    "Crossfade length at the loop seam, in milliseconds. "
                    "Only used by the Crossfade-loop freeze mode; ignored "
                    "by the other three. Capped at half the grain length. "
                    "Longer crossfade = smoother but more washed-out seam.",
                    1.0, 250.0, 1.0);
        crossfadeSlider.setTextValueSuffix(" ms");
        crossfadeSlider.onValueChange = [this]() { onCrossfadeChanged(); };

        // Pitch slider goes down to 16 Hz so the lowest scientific-pitch-
        // notation octave (C0 = ~16.35 Hz) round-trips through the Note +
        // Octave picker without clamping. 20 Hz (the old min) would clip
        // C0 .. D#0 to 20 and stop the picker round-tripping cleanly.
        setupSlider(pitchSlider, pitchLabel,
                    "Embedded pitch",
                    "Pitch of the captured source in Hertz. The synth "
                    "scales source reads by noteHz / embeddedPitch so "
                    "MIDI pitch tracks correctly. Defaults to A4 (440 Hz) "
                    "when the source's true pitch is unknown. If you "
                    "don't know the exact Hz, use the Note + Octave "
                    "picker below to snap to a known pitch class.",
                    16.0, 20000.0, 0.1);
        pitchSlider.setSkewFactorFromMidPoint(440.0);  // log feel
        pitchSlider.setTextValueSuffix(" Hz");
        pitchSlider.onValueChange = [this]() { onPitchChanged(); };

        // Note + Octave shortcut: lets the user say "this was an A4" instead
        // of typing 440. Picking a note/octave snaps the slider to that
        // note's 12-TET frequency (A4 = 440); dragging the slider updates
        // the combos to show the nearest note. Independent of the project's
        // tuning system on purpose - embeddedPitchHz is a property of the
        // captured recording, not the project's tuning preference.
        addAndMakeVisible(noteOctaveLabel);
        noteOctaveLabel.setText("As note",
                                juce::dontSendNotification);
        noteOctaveLabel.setColour(juce::Label::textColourId,
                                  juce::Colours::white.withAlpha(0.85f));
        noteOctaveLabel.setFont(juce::Font(11.0f));

        addAndMakeVisible(noteCombo);
        const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F",
                                      "F#", "G", "G#", "A", "A#", "B"};
        for (int i = 0; i < 12; ++i)
            noteCombo.addItem(kNoteNames[i], i + 1);
        noteCombo.setTooltip(
            "Pitch class of the captured source. Snaps the Embedded "
            "pitch slider to this note's 12-TET frequency (A4 = 440 Hz). "
            "Pair with Octave to set the exact pitch. The slider then "
            "tracks back here - drag it and the combos show the nearest "
            "note.");
        noteCombo.onChange = [this]() { onNoteOrOctaveChanged(); };

        addAndMakeVisible(octaveCombo);
        // Scientific pitch notation: A4 = MIDI 69 = 440 Hz, C4 = middle C.
        // Range 0..9 covers the audible spectrum at 12-TET (C0 = 16.35 Hz,
        // B9 = 15804 Hz). IDs are 1-based to keep 0 as ComboBox's "not
        // selected" sentinel; ID = octave + 1.
        for (int o = 0; o <= 9; ++o)
            octaveCombo.addItem(juce::String(o), o + 1);
        octaveCombo.setTooltip(
            "Octave of the captured source, using scientific pitch "
            "notation (A4 = 440 Hz, middle C = C4). Snaps the Embedded "
            "pitch slider to the chosen Note + Octave's frequency.");
        octaveCombo.onChange = [this]() { onNoteOrOctaveChanged(); };

        // Cents readout: shows how far the slider's Hz is off the nearest
        // 12-TET note in cents (1/100 of a semitone). Range -49 .. +50;
        // 0 means the slider is exactly on the displayed note. Read-only
        // - the user edits cents indirectly by dragging the Hz slider.
        addAndMakeVisible(centsLabel);
        centsLabel.setText("0 \xC2\xA2", juce::dontSendNotification);  // UTF-8 cent sign
        centsLabel.setColour(juce::Label::textColourId,
                             juce::Colours::white.withAlpha(0.7f));
        centsLabel.setFont(juce::Font(11.0f));
        centsLabel.setJustificationType(juce::Justification::centredLeft);
        centsLabel.setTooltip(
            "How far the Embedded pitch is from the displayed note, in "
            "cents (1/100 of a semitone). Negative = below the note "
            "(flat), positive = above (sharp), 0 = exact. Range -49 to "
            "+50; outside that range the displayed note ticks to the "
            "next semitone.");

        addAndMakeVisible(freezeModeLabel);
        freezeModeLabel.setText("Freeze mode", juce::dontSendNotification);
        freezeModeLabel.setColour(juce::Label::textColourId,
                                  juce::Colours::white.withAlpha(0.85f));
        freezeModeLabel.setFont(juce::Font(11.0f));
        addAndMakeVisible(freezeModeCombo);
        // IDs are 1-based to keep 0 as the "nothing selected" sentinel
        // ComboBox uses internally; ID = (int)mode + 1.
        freezeModeCombo.addItem("Crossfade loop",      1);
        freezeModeCombo.addItem("Async granular",      2);
        freezeModeCombo.addItem("Pitch-synced grains", 3);
        freezeModeCombo.addItem("Spectral freeze",     4);
        freezeModeCombo.setTooltip(
            "Which 'sustain the spot' algorithm runs while a note is held:\n"
            " - Crossfade loop: faithful tape loop with a short seam blend\n"
            " - Async granular: many grains at random offsets - frozen blur\n"
            " - Pitch-synced grains: like Async but grain origins snap to the\n"
            "   detected pitch period, so the cloud has a clean, stable pitch\n"
            "   (only differs from Async when the freeze window is wider than\n"
            "   the grain - widen the amber band to give it roam room)\n"
            " - Spectral freeze: FFT freeze - ethereal pad sustain");
        freezeModeCombo.onChange = [this]() { onFreezeModeChanged(); };

        addAndMakeVisible(playBtn);
        playBtn.setButtonText("Play");
        playBtn.setTooltip(
            "Audition this captured waveform - feeds the source PCM into "
            "the engine's granular preview path with this editor's current "
            "grain length, crossfade, and freeze mode, so you can hear "
            "what the synth will play at note-on without having to wire "
            "it up first. Click again to stop.");
        playBtn.setColour(juce::TextButton::buttonColourId,
                          juce::Colour(60, 110, 70));
        playBtn.onClick = [this]() { togglePlay(); };

        addAndMakeVisible(recaptureBtn);
        // Name the button after the source this frame was captured from, so a
        // mic-captured frame doesn't mislabel its re-capture as "from song".
        // Unknown origin (-1: pre-field project or from-scratch frame) falls
        // back to the historical "song" default, matching onRecap's fallback.
        const char* srcWord;
        switch (frame.captureSourceKind) {
            case 1:  srcWord = "mic";  break;
            case 2:  srcWord = "file"; break;
            default: srcWord = "song"; break;  // 0 song, -1 unknown
        }
        recaptureBtn.setButtonText(juce::String("Re-capture from ") + srcWord + "...");
        recaptureBtn.setTooltip(
            juce::String("Open the capture panel and replace this granular "
            "waveform's source PCM with a fresh capture from the ") + srcWord +
            ". Grain length, crossfade, embedded pitch, and freeze mode are "
            "preserved on re-capture so you keep your tuning when changing "
            "the source.");
        recaptureBtn.onClick = [this]() {
            if (onRecapture) onRecapture();
        };

        syncFromFrame();
    }

    ~GranularFrameEditorComponent() override {
        // Stop the audition if it was running. The engine's preview state
        // is global, so leaving GrainLoop active after this component dies
        // would keep auditioning silently (the buffer pointer survives
        // until something else replaces it). Editor swaps when the user
        // clicks a different library entry, so this runs on every change.
        stopPlay();
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(22, 22, 28));

        if (waveBounds.isEmpty()) return;
        auto r = waveBounds.toFloat();
        g.setColour(juce::Colour(32, 32, 40));
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(juce::Colour(70, 70, 90));
        g.drawRoundedRectangle(r, 4.0f, 1.0f);

        const auto& src = frame.source;
        if (src.empty()) {
            g.setColour(juce::Colours::grey.withAlpha(0.8f));
            g.setFont(11.0f);
            g.drawText("(empty source - press Re-capture)",
                       r.toNearestInt(), juce::Justification::centred);
            return;
        }

        // Downsample to pixel-width by taking min/max per bin. Same approach
        // the JUCE thumbnail and the capture dialog both use - cheap to draw
        // and stays visually faithful for any source length.
        const int W = (int)r.getWidth() - 4;
        if (W <= 1) return;
        const int N = (int)src.size();
        const float cx = r.getCentreX();
        const float midY = r.getCentreY();
        const float halfH = r.getHeight() * 0.45f;
        g.setColour(juce::Colour(120, 200, 255).withAlpha(0.9f));
        for (int x = 0; x < W; ++x) {
            int i0 = (int)((double)x       / W * N);
            int i1 = (int)((double)(x + 1) / W * N);
            if (i1 <= i0) i1 = i0 + 1;
            if (i1 > N) i1 = N;
            float lo = 0.0f, hi = 0.0f;
            for (int i = i0; i < i1; ++i) {
                lo = std::min(lo, src[i]);
                hi = std::max(hi, src[i]);
            }
            float y0 = midY - hi * halfH;
            float y1 = midY - lo * halfH;
            g.drawLine(r.getX() + 2 + x, y0, r.getX() + 2 + x, y1, 1.0f);
        }

        // Centre baseline.
        g.setColour(juce::Colours::white.withAlpha(0.2f));
        g.drawHorizontalLine((int)midY, r.getX() + 2.0f, r.getRight() - 2.0f);

        // ---- Freeze-window selection band ----
        // The sub-selection the current freeze mode operates on. Amber so it
        // reads clearly against the blue waveform. A dashed outline means "auto"
        // (windowStart == -1, follows the source); a solid outline means the
        // user has placed it explicitly. Drag the middle to move it, the edges
        // to resize it (floor = minWindowSamples(), ceiling = whole capture).
        const int wLen = std::min(effectiveWindowLen(), N);
        const int ws   = effectiveWindowStart();
        const float bx0 = sampleToX(ws);
        const float bx1 = sampleToX(ws + wLen);
        juce::Rectangle<float> band(bx0, r.getY() + 2.0f,
                                    std::max(2.0f, bx1 - bx0),
                                    r.getHeight() - 4.0f);
        const bool autoCentred = (frame.windowStart < 0);
        const juce::Colour amber(255, 190, 70);
        g.setColour(amber.withAlpha(0.16f));
        g.fillRect(band);
        g.setColour(amber.withAlpha(autoCentred ? 0.55f : 0.95f));
        if (autoCentred) {
            // Dashed outline for the auto-centred (unplaced) band.
            const float dashes[2] = {4.0f, 3.0f};
            g.drawDashedLine({band.getTopLeft(), band.getTopRight()}, dashes, 2, 1.2f);
            g.drawDashedLine({band.getBottomLeft(), band.getBottomRight()}, dashes, 2, 1.2f);
            g.drawDashedLine({band.getTopLeft(), band.getBottomLeft()}, dashes, 2, 1.2f);
            g.drawDashedLine({band.getTopRight(), band.getBottomRight()}, dashes, 2, 1.2f);
        } else {
            g.drawRect(band, 1.4f);
        }
        // Grab handles at the band edges.
        g.setColour(amber.withAlpha(0.9f));
        g.fillRect(band.getX() - 1.0f, band.getY(), 2.0f, band.getHeight());
        g.fillRect(band.getRight() - 1.0f, band.getY(), 2.0f, band.getHeight());
    }

    void resized() override {
        auto a = getLocalBounds().reduced(8);
        titleLabel.setBounds(a.removeFromTop(20));
        a.removeFromTop(2);
        sourceInfoLabel.setBounds(a.removeFromTop(16));
        a.removeFromTop(6);

        // Reserve bottom space for the param rows + Re-capture button BEFORE
        // sizing the wave display. The previous layout grabbed 40% of `a`
        // for the wave display first, which on shorter right-panes squeezed
        // the lower controls (pitch slider, freeze-mode combo, Re-capture
        // button) right off the visible area. Reserving space first
        // guarantees the controls are always visible, with the wave display
        // shrinking (down to a 60-px minimum) instead of the controls
        // disappearing.
        const int rowH = 22;
        const int rowGap = 4;
        // 7 rows: grain length, grains, FFT size, crossfade, pitch,
        // note+octave shortcut, freeze mode. The grains/FFT rows sit under
        // grain length (all the per-mode "texture" controls grouped); the
        // note+octave shortcut sits directly under the pitch slider so the
        // pairing is visually obvious.
        const int numRows = 7;
        const int paramsH = numRows * rowH + (numRows - 1) * rowGap;
        const int btnH = 28;
        const int waveToParamsGap = 8;
        const int paramsToBtnGap = 8;
        const int bottomReserved = waveToParamsGap + paramsH
                                 + paramsToBtnGap + btnH;
        const int waveH = std::max(60, a.getHeight() - bottomReserved);
        waveBounds = a.removeFromTop(waveH);
        a.removeFromTop(waveToParamsGap);

        // Most rows are {label (100px) | single control (rest)}; the
        // note+octave row needs a custom layout (label | note combo | octave
        // combo), inlined below.
        auto layoutRow = [&a, rowH, rowGap](juce::Component& lab,
                                            juce::Component& ctrl,
                                            bool isLast) {
            auto row = a.removeFromTop(rowH);
            lab.setBounds(row.removeFromLeft(100));
            row.removeFromLeft(4);
            ctrl.setBounds(row);
            if (!isLast) a.removeFromTop(rowGap);
        };

        // Intrinsic width for a ComboBox = widest item text + chrome
        // (drop-arrow box + side padding). Used by the freeze, note, and
        // octave combos so they're "just wide enough" rather than stretched
        // across the row. Chrome budget = drop-arrow box (~26) + side
        // padding (~16) + visual cushion (~12) so the longest item label
        // has breathing room and never crowds the arrow. The minimum-width
        // floor keeps single-character items (Octave "0..9", Note "C")
        // from rendering as a tiny pill that's mostly just the arrow -
        // they need to be wide enough to be a comfortable click target
        // even when the text inside is narrow.
        //
        // NB1: Font::getStringWidth was deprecated in JUCE 8 and now
        // returns 0 - using it here gave every combo a 36 px (drop-arrow
        // only) width. The replacement is GlyphArrangement::getStringWidthInt.
        //
        // NB2: GlyphArrangement::getStringWidthInt itself is only reliable
        // when the Font handed to it was built with FontOptions. Pulling
        // the font from cb.getLookAndFeel().getComboBoxFont(cb) yields a
        // deprecated-constructed Font in JUCE 8 (zero metrics), which
        // collapsed all three combos to the 80 px floor and clipped
        // "Pitch-synced grains". We build the measurement font ourselves
        // via FontOptions, replicating what LookAndFeel_V4 would use for
        // a ComboBox at this row height (min(15, rowH * 0.85)).
        auto intrinsicComboWidth = [rowH](juce::ComboBox& cb) {
            const float fontH = juce::jmin(15.0f, (float)rowH * 0.85f);
            const juce::Font f{juce::FontOptions(fontH)};
            int maxText = 0;
            for (int i = 0; i < cb.getNumItems(); ++i)
                maxText = std::max(maxText,
                    juce::GlyphArrangement::getStringWidthInt(f, cb.getItemText(i)));
            return std::max(80, maxText + 54);
        };

        layoutRow(grainLengthLabel, grainLengthSlider, false);
        layoutRow(grainCountLabel,  grainCountSlider,  false);
        // FFT-size row: label (100 px) + combo sized to its widest item.
        {
            auto row = a.removeFromTop(rowH);
            fftSizeLabel.setBounds(row.removeFromLeft(100));
            row.removeFromLeft(4);
            const int fftW = intrinsicComboWidth(fftSizeCombo);
            fftSizeCombo.setBounds(row.removeFromLeft(fftW).withHeight(rowH));
            a.removeFromTop(rowGap);
        }
        layoutRow(crossfadeLabel,   crossfadeSlider,   false);
        layoutRow(pitchLabel,       pitchSlider,       false);
        // Note + Octave + Cents shortcut row. Each combo is sized to its
        // widest item (note: "C#"/"D#"/..., octave: "0".."9"), and the
        // cents readout sits immediately after the octave combo rather
        // than flush right - keeps the three related controls visually
        // grouped instead of spread across the full row width.
        {
            auto row = a.removeFromTop(rowH);
            noteOctaveLabel.setBounds(row.removeFromLeft(100));
            row.removeFromLeft(4);
            const int noteW   = intrinsicComboWidth(noteCombo);
            const int octaveW = intrinsicComboWidth(octaveCombo);
            const int centsW  = 52;
            noteCombo.setBounds(row.removeFromLeft(noteW).withHeight(rowH));
            row.removeFromLeft(6);
            octaveCombo.setBounds(row.removeFromLeft(octaveW).withHeight(rowH));
            row.removeFromLeft(8);
            centsLabel.setBounds(row.removeFromLeft(centsW));
            a.removeFromTop(rowGap);
        }
        // Freeze mode row: label (100 px) + combo sized to its widest item
        // ("Pitch-synced grains"). Trailing space in the row is left blank
        // so the combo isn't stretched.
        {
            auto row = a.removeFromTop(rowH);
            freezeModeLabel.setBounds(row.removeFromLeft(100));
            row.removeFromLeft(4);
            const int freezeW = intrinsicComboWidth(freezeModeCombo);
            freezeModeCombo.setBounds(row.removeFromLeft(freezeW)
                                          .withHeight(rowH));
        }

        a.removeFromTop(paramsToBtnGap);
        // Bottom button row: Play (audition the source through the engine
        // preview path) on the left, Re-capture (replace the source PCM)
        // immediately after. Both stay at the left edge so they're easy to
        // find rather than scattered across the row.
        auto btnRow = a.removeFromTop(btnH);
        playBtn.setBounds(btnRow.removeFromLeft(80));
        btnRow.removeFromLeft(8);
        recaptureBtn.setBounds(btnRow.removeFromLeft(220));
    }

    // ---- Selection-band dragging ------------------------------------------
    // Click-drag the amber band over the source thumbnail to choose which
    // sub-section of the capture the freeze operates on. Drag the MIDDLE to move
    // the band; drag either EDGE handle to resize it (the width floor is the
    // grain length, the ceiling is the whole capture). Clicking outside the band
    // re-centres it on the cursor.
    enum class BandDrag { None, Move, ResizeL, ResizeR };
    static constexpr float kBandEdgePx = 6.0f;  // px grab zone for edge handles
    BandDrag bandHitTest(float px, int ws, int len) const {
        const float xL = sampleToX(ws);
        const float xR = sampleToX(ws + len);
        const bool nearL = std::abs(px - xL) <= kBandEdgePx;
        const bool nearR = std::abs(px - xR) <= kBandEdgePx;
        if (nearL && nearR) {
            // Both grab zones overlap: the band has collapsed to roughly the
            // grab-zone width (e.g. min grain length at a zoomed-out thumbnail),
            // so its interior Move zone has vanished. A fixed ResizeL-first
            // preference here makes the RIGHT edge permanently unreachable, and
            // since dragging the left edge rightward is clamped to right-floor,
            // the band can never be re-expanded - the reported "stuck collapsed
            // band, can't re-expand or drag" bug. Resolve by the side of the
            // band centre the cursor sits on: click to the LEFT to grab the
            // left edge (drag further left to grow), to the RIGHT to grab the
            // right edge (drag further right to grow). Either direction can
            // always re-expand the band.
            const float mid = 0.5f * (xL + xR);
            return (px <= mid) ? BandDrag::ResizeL : BandDrag::ResizeR;
        }
        if (nearL) return BandDrag::ResizeL;
        if (nearR) return BandDrag::ResizeR;
        return BandDrag::Move;
    }
    void mouseDown(const juce::MouseEvent& e) override {
        if (!bandDraggable()) return;
        if (!waveBounds.contains(e.getPosition())) return;
        const int ws  = effectiveWindowStart();
        const int len = effectiveWindowLen();
        const float px = (float)e.position.x;
        bandDrag = bandHitTest(px, ws, len);
        if (bandDrag == BandDrag::Move) {
            const int s = xToSample(px);
            // Grab inside the band -> preserve offset; outside -> centre on cursor.
            bandGrabOffset = (s >= ws && s < ws + len) ? (s - ws) : (len / 2);
            setBand(s - bandGrabOffset, len);
        }
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (bandDrag == BandDrag::None) return;
        const int srcLen = (int)frame.source.size();
        const int s = xToSample((float)e.position.x);
        // Resize floor is mode-dependent: the grain-cloud modes hold the window
        // at >= grain (a grain must fit), while CrossfadeLoop / SpectralFreeze
        // (no granulation) let it shrink to ~5 ms / one FFT block.
        const int floorLen = minWindowSamples();
        if (bandDrag == BandDrag::Move) {
            setBand(s - bandGrabOffset, effectiveWindowLen());
        } else if (bandDrag == BandDrag::ResizeR) {
            // Right edge follows the cursor; left edge (start) stays put.
            const int start = effectiveWindowStart();
            setBand(start, std::clamp(s - start, floorLen, srcLen - start));
        } else { // ResizeL: left edge follows the cursor; right edge stays put.
            const int right = effectiveWindowStart() + effectiveWindowLen();
            const int start = std::clamp(s, 0, right - floorLen);
            setBand(start, right - start);
        }
    }
    void mouseUp(const juce::MouseEvent&) override {
        bandDrag = BandDrag::None;
    }
    void mouseMove(const juce::MouseEvent& e) override {
        juce::MouseCursor cur = juce::MouseCursor::NormalCursor;
        if (bandDraggable() && waveBounds.contains(e.getPosition())) {
            const auto hit = bandHitTest((float)e.position.x,
                                         effectiveWindowStart(),
                                         effectiveWindowLen());
            cur = (hit == BandDrag::Move)
                      ? juce::MouseCursor::DraggingHandCursor
                      : juce::MouseCursor::LeftRightResizeCursor;
        }
        setMouseCursor(cur);
    }

    // Commit an explicit band, clamped so it stays inside the source. The band
    // always sets the freeze WINDOW (frame.windowStart / frame.windowLen) - it
    // never touches the grain length. What that window then means is the mode's
    // business (grains roam it / the loop spans it / the FFT analyses it). The
    // resize floor is mode-dependent (minWindowSamples). Drives the live preview
    // and the debounced undo/apply just like the sliders, so a band drag
    // coalesces into one undo step on settle.
    void setBand(int start, int len) {
        const int srcLen = (int)frame.source.size();
        len   = std::clamp(len, std::min(minWindowSamples(), srcLen), srcLen);
        start = std::clamp(start, 0, std::max(0, srcLen - len));
        frame.windowStart = start;
        frame.windowLen   = len;
        // In CrossfadeLoop the loop length == the window, so the crossfade seam
        // cap moves with it; refresh the slider range so it can't sit above
        // window/2 (which the voice would silently halve).
        refreshCrossfadeRange();
        updateSourceInfoLabel();   // window size changed - update the ms read-out
        pushPreviewWindow();
        repaint();
        applyEdit();
    }

private:
    GranularFrame& frame;
    std::function<void()> onApply;
    std::function<void()> onRecapture;
    std::function<void(bool, int, int)> sendAudition; // (noteOn, pitch, velocity)

    juce::Label      titleLabel;
    juce::Label      sourceInfoLabel;
    juce::Slider     grainLengthSlider;
    juce::Label      grainLengthLabel;
    juce::Slider     grainCountSlider;       // # overlapping grains (cloud modes)
    juce::Label      grainCountLabel;
    juce::ComboBox   fftSizeCombo;           // FFT size (Spectral mode)
    juce::Label      fftSizeLabel;
    juce::Slider     crossfadeSlider;
    juce::Label      crossfadeLabel;
    juce::Slider     pitchSlider;
    juce::Label      pitchLabel;
    juce::ComboBox   noteCombo;
    juce::ComboBox   octaveCombo;
    juce::Label      noteOctaveLabel;
    juce::Label      centsLabel;
    juce::ComboBox   freezeModeCombo;
    juce::Label      freezeModeLabel;
    juce::TextButton playBtn;
    juce::TextButton recaptureBtn;

    // Whether this editor instance is currently auditioning through the
    // engine's preview path. Tracked here (rather than reading from the
    // engine) so the destructor only tears the preview down when WE
    // started it - in case some other UI took over the preview slot
    // before this editor was destroyed.
    bool playing = false;

    juce::Rectangle<int> waveBounds;

    // Selection-band drag state. bandDrag (type declared above mouseDown) names
    // which part of the band the current gesture grabbed (None when idle): Move
    // slides it, ResizeL/ResizeR drag an edge. bandGrabOffset is the sample
    // offset from the band's start to the grab point (Move only), so the band
    // tracks the cursor without jumping.
    BandDrag bandDrag = BandDrag::None;
    int  bandGrabOffset = 0;

    // Suppress onValueChange feedback while we're pushing model state into
    // the controls. Without this, syncFromFrame() would re-fire every
    // setValue and push back into the frame (no-op in result but it'd still
    // call onApply / commit and spam the host).
    bool suppressCallbacks = false;

    // Round-trip helper: samples-at-sourceRate <-> ms.
    double sampleRateOrFallback() const {
        return frame.sourceSampleRate > 0.0
             ? frame.sourceSampleRate
             : 48000.0;
    }
    double samplesToMs(int samples) const {
        return 1000.0 * (double)samples / sampleRateOrFallback();
    }
    int msToSamples(double ms) const {
        return (int)std::round(ms * sampleRateOrFallback() / 1000.0);
    }

    // ---- Freeze-window (selection band) geometry --------------------------
    // The band is a sub-selection of the captured source that the current freeze
    // mode operates on, with an independent POSITION (frame.windowStart) and
    // WIDTH (frame.windowLen). The band is resizable in EVERY mode - it just
    // means different things: Async/PitchSync roam grains over it (a band wider
    // than the grain is what makes them diverge), CrossfadeLoop loops the whole
    // band, SpectralFreeze FFTs it. windowStart/windowLen == -1 mean "auto": an
    // unplaced band one grain wide, DISPLAYED at the legacy centred-with-
    // lookahead position so the band always shows where the freeze sits even
    // before any drag.
    //
    // Only the two grain-cloud modes actually USE the grain length, so only they
    // floor the window at the grain (a grain must fit). CrossfadeLoop and
    // SpectralFreeze don't granulate (their grain slider is greyed out), so their
    // window can shrink below the grain - down to ~5 ms (crossfade: a short
    // pitched buzz) or one FFT block (spectral).
    bool modeUsesGrain() const {
        return frame.freezeMode == GranularFreezeMode::AsyncGranular
            || frame.freezeMode == GranularFreezeMode::PitchSyncGrains;
    }
    // FFT-size combo uses the shared kSpectralFftSizes table (granular_frame.h)
    // after the "Auto" entry (combo ID 1, fftSize 0). Combo ID = index + 2.
    static int fftComboIdToSize(int id) {
        const int i = id - 2;
        return (i >= 0 && i < kNumSpectralFftSizes) ? kSpectralFftSizes[i] : 0;
    }
    // frame.fftSize -> combo ID (1 == Auto when no exact match).
    static int fftSizeToComboId(int size) {
        for (int i = 0; i < kNumSpectralFftSizes; ++i)
            if (kSpectralFftSizes[i] == size) return i + 2;
        return 1;  // Auto
    }
    // Smallest allowed window width for the current mode (the band's resize
    // floor). Mirrors the voice's per-mode bandLen floor in granular_freeze.cpp.
    int minWindowSamples() const {
        const int srcLen = std::max(1, (int)frame.source.size());
        switch (frame.freezeMode) {
            case GranularFreezeMode::AsyncGranular:
            case GranularFreezeMode::PitchSyncGrains:
                return std::min(std::max(16, frame.grainLength), srcLen);
            case GranularFreezeMode::SpectralFreeze:
                return std::min(256, srcLen);            // smallest viable FFT
            case GranularFreezeMode::CrossfadeLoop:
            default:
                return std::min(std::max(16, msToSamples(5.0)), srcLen);
        }
    }
    int effectiveWindowLen() const {
        const int srcLen  = std::max(1, (int)frame.source.size());
        if (frame.windowLen >= 0)
            return std::clamp(frame.windowLen, minWindowSamples(), srcLen);
        // Auto width, via the shared resolver so the editor band matches the
        // voice sample-for-sample:
        //   kWindowLegacyAuto (-1)     -> grainLength (legacy one-grain window,
        //                                 keeps old CrossfadeLoop frames looping
        //                                 exactly one grain).
        //   kWindowAutoPerMethod (-2)  -> autoWindowMultiplier(mode) x grainLength
        //                                 (cloud modes get roam room by default).
        const int autoLen = resolveAutoWindowLen(frame.windowLen,
                                                  frame.freezeMode,
                                                  frame.grainLength);
        return std::clamp(autoLen, minWindowSamples(), srcLen);
    }
    // Largest crossfade seam the loop can hold: half the loop length. In
    // CrossfadeLoop the loop IS the window, so it tracks the window width; in the
    // other modes the crossfade slider is inert, so fall back to half the grain.
    int crossfadeMaxSamples() const {
        const int loop = (frame.freezeMode == GranularFreezeMode::CrossfadeLoop)
                             ? effectiveWindowLen()
                             : std::max(16, frame.grainLength);
        return std::max(1, loop / 2);
    }
    int maxWindowStart() const {
        return std::max(0, (int)frame.source.size() - effectiveWindowLen());
    }
    int effectiveWindowStart() const {
        if (frame.windowStart >= 0)
            return std::clamp(frame.windowStart, 0, maxWindowStart());
        const int g = std::max(16, frame.grainLength);
        const int srcLen = (int)frame.source.size();
        return std::clamp(std::max(0, (srcLen - (g + g / 2)) / 2),
                          0, maxWindowStart());
    }
    // Pixel x for a source sample index, within the wave thumbnail's draw area.
    float sampleToX(int sample) const {
        const int srcLen = std::max(1, (int)frame.source.size());
        auto r = waveBounds.toFloat();
        const float W = (float)((int)r.getWidth() - 4);
        return r.getX() + 2.0f + (float)sample / (float)srcLen * W;
    }
    // Inverse: source sample index for a pixel x.
    int xToSample(float x) const {
        const int srcLen = std::max(1, (int)frame.source.size());
        auto r = waveBounds.toFloat();
        const float W = std::max(1.0f, (float)((int)r.getWidth() - 4));
        const float t = (x - (r.getX() + 2.0f)) / W;
        return std::clamp((int)std::round(t * (float)srcLen), 0, srcLen);
    }
    // True when the band is interactive (the minimum window is narrower than the
    // whole source, so there's room to move/resize a sub-window).
    bool bandDraggable() const {
        return !frame.source.empty()
            && minWindowSamples() < (int)frame.source.size();
    }
    // Push the current freeze window (start + width) into the live AudioEngine
    // preview (fallback audition path only). The synth-routed audition re-reads
    // both when its graph rebuilds (debounced via onApply), matching how grain
    // length and the other params already propagate to a held note.
    void pushPreviewWindow() {
        if (playing && !sendAudition)
            if (auto* eng = AudioEngine::getInstance()) {
                eng->setPreviewWindowStart(frame.windowStart);
                eng->setPreviewWindowLen(frame.windowLen);
            }
    }
    // Push the grain count + FFT size into the live AudioEngine preview
    // (fallback audition path only). Same propagation model as
    // pushPreviewWindow: the synth-routed audition re-reads both on graph
    // rebuild (debounced via onApply).
    void pushPreviewExtras() {
        if (playing && !sendAudition)
            if (auto* eng = AudioEngine::getInstance()) {
                eng->setPreviewGrainCount(frame.grainCount);
                eng->setPreviewFftSize(frame.fftSize);
            }
    }

    // ---- Note/Octave <-> Hz helpers (12-TET, A4 = 440 Hz, scientific
    //      pitch notation: C4 = middle C, MIDI 60; A4 = MIDI 69).
    //
    // Hard-coded to 12-TET rather than going through the project's tuning
    // system: embeddedPitchHz describes the recording itself, not the
    // project's tuning preference, and the user picking "A4" wants 440 Hz
    // regardless of what tuning the project later uses for playback.
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
    // Cents offset of hz from the nearest 12-TET note. Range: -50 .. +50,
    // with conventional rounding (lround = round-half-away-from-zero), so
    // in practice -49 .. +50 once rounded to an int. Negative = flat,
    // positive = sharp, 0 = exact match.
    static int hzToCentsFromNearest(double hz) {
        if (hz <= 0.0) return 0;
        const double midiExact = 69.0 + 12.0 * std::log2(hz / 440.0);
        const double midiNearest = std::round(midiExact);
        return (int)std::lround((midiExact - midiNearest) * 100.0);
    }
    void updateCentsLabel(double hz) {
        const int cents = hzToCentsFromNearest(hz);
        juce::String t;
        // Show a sign for non-zero values so the user can see the direction
        // at a glance. "+3 \xC2\xA2" / "-12 \xC2\xA2" / "0 \xC2\xA2".
        if (cents > 0) t << "+";
        t << cents << " \xC2\xA2";
        centsLabel.setText(t, juce::dontSendNotification);
    }

    // Refresh the read-out under the waveform: total captured duration plus the
    // current freeze-WINDOW length and (for the grain-cloud modes) the grain
    // length, all in ms, so the user can compare the selected slice against the
    // grain size at a glance ("the waveform size versus the grain size"). Called
    // from syncFromFrame and from every handler that changes the window, grain,
    // or freeze mode so the numbers track live as the band is dragged.
    void updateSourceInfoLabel() {
        const double sr = sampleRateOrFallback();
        const double srcMs = samplesToMs((int)frame.source.size());
        juce::String info;
        info << juce::String(srcMs, 0) << " ms @ "
             << juce::String((int)std::round(sr / 1000.0)) << " kHz  \xC2\xB7  window "
             << juce::String(samplesToMs(effectiveWindowLen()), 0) << " ms";
        // The grain only applies in the two grain-cloud modes; show it there so
        // the comparison is meaningful and uncluttered elsewhere.
        if (modeUsesGrain())
            info << "  \xC2\xB7  grain "
                 << juce::String(samplesToMs(std::max(16, frame.grainLength)), 0)
                 << " ms";
        sourceInfoLabel.setText(info, juce::dontSendNotification);
    }

    void syncFromFrame() {
        suppressCallbacks = true;

        updateSourceInfoLabel();

        // The grain must fit inside the freeze window, which fits inside the
        // capture, so the grain can never exceed the captured duration. Cap the
        // slider's max at min(kGranularMaxGrainMs, capturedMs). (The freeze
        // window's width is separate - the draggable band - and floors at the
        // grain length, so growing the grain pushes the window's floor up.)
        const double capturedMs = samplesToMs((int)frame.source.size());
        const double maxGrainMs = std::max(5.0,
            std::min((double)kGranularMaxGrainMs, capturedMs));
        grainLengthSlider.setRange(5.0, maxGrainMs, 1.0);
        grainLengthSlider.setValue(
            std::min(samplesToMs(frame.grainLength), maxGrainMs),
            juce::dontSendNotification);
        grainCountSlider.setValue(
            std::clamp(frame.grainCount, 2, GrainFreezeVoice::kMaxGrains),
            juce::dontSendNotification);
        fftSizeCombo.setSelectedId(fftSizeToComboId(frame.fftSize),
                                   juce::dontSendNotification);
        // Crossfade cap = half the loop length (the window in CrossfadeLoop,
        // the grain otherwise) - the voice clamps it at use time, so reflect
        // that in the slider's range so dragging doesn't silently land on a
        // value the engine immediately halves.
        refreshCrossfadeRange();
        pitchSlider.setValue((double)frame.embeddedPitchHz,
                             juce::dontSendNotification);
        // Mirror the slider's Hz into the note/octave combos + cents label
        // so they show the nearest 12-TET note and offset at load time.
        auto [n, o] = hzToNearestNoteOctave((double)frame.embeddedPitchHz);
        noteCombo.setSelectedId(n + 1, juce::dontSendNotification);
        octaveCombo.setSelectedId(o + 1, juce::dontSendNotification);
        updateCentsLabel((double)frame.embeddedPitchHz);

        freezeModeCombo.setSelectedId((int)frame.freezeMode + 1,
                                      juce::dontSendNotification);
        refreshGrainSliderForMode();

        suppressCallbacks = false;
        repaint();
    }

    // Re-range the crossfade slider against the current loop length
    // (crossfadeMaxSamples == window/2 in CrossfadeLoop, grain/2 otherwise) so
    // the user can't drag it above loop/2 - a value the voice would silently
    // halve. Clamps the stored crossfade down (and the slider) when it no longer
    // fits. Uses dontSendNotification, so it never re-fires onCrossfadeChanged.
    void refreshCrossfadeRange() {
        const double maxXfMs = samplesToMs(crossfadeMaxSamples());
        crossfadeSlider.setRange(1.0, std::max(2.0, maxXfMs), 1.0);
        if (samplesToMs(frame.crossfadeSamples) > maxXfMs) {
            crossfadeSlider.setValue(maxXfMs, juce::dontSendNotification);
            frame.crossfadeSamples = msToSamples(maxXfMs);
        } else {
            crossfadeSlider.setValue(samplesToMs(frame.crossfadeSamples),
                                     juce::dontSendNotification);
        }
    }

    // Set the grain length from a sample count: clamp to the slider's valid
    // range (5 ms .. min(500 ms, captured duration)), write frame.grainLength,
    // sync the slider, and re-clamp the crossfade cap. Pure state sync - the
    // caller fires onApply/repaint. Only used by the grain slider now (the band
    // no longer edits the grain in any mode).
    void applyGrainLengthSamples(int gSamples) {
        const int srcLen = (int)frame.source.size();
        const double capturedMs = samplesToMs(srcLen);
        const double maxGrainMs = std::max(1.0,
            std::min((double)kGranularMaxGrainMs, capturedMs));
        // Floor at 1 ms so the async cloud can be made as constant as possible;
        // the voice still enforces a 16-sample engine floor, so very short
        // values collapse to ~0.33 ms at 48 kHz - harmless but buzzy.
        const double ms = std::clamp(samplesToMs(gSamples), 1.0, maxGrainMs);
        frame.grainLength = msToSamples(ms);
        suppressCallbacks = true;
        grainLengthSlider.setRange(1.0, maxGrainMs, 0.5);
        grainLengthSlider.setValue(ms, juce::dontSendNotification);
        refreshCrossfadeRange();
        suppressCallbacks = false;
    }

    // Commit an audible edit (band, grain, pitch, mode...) to the host.
    //
    // onApply persists the change and, in the embedded (synth-hosted) case,
    // triggers a debounced graph rebuild (onNodeEdited -> requestRebuild ->
    // rebuildGraph, which calls processorGraph->clear() and destroys every
    // live voice). That rebuild is what used to silence the audition the
    // moment the user resized the band: the held note's voice was torn down
    // and nothing re-established it.
    //
    // The fix is level-triggered: while auditioning through the synth path we
    // re-publish the audition snapshot (sendAudition(true, ...)) on every
    // audible edit. heldAudition is persistent state the synth reconciles each
    // block, so the post-rebuild processor re-arms the voice from the *refreshed*
    // snapshot - the user keeps hearing a continuous note that now reflects the
    // new band / grain / mode. In the fallback (engine-preview) path there is no
    // rebuild and the preview atomics were already updated by pushPreview*(), so
    // this is a cheap no-op there.
    void applyEdit() {
        if (playing && sendAudition)
            sendAudition(true, kAuditionPitch, kAuditionVelocity);
        if (onApply) onApply();
    }

    void onGrainLengthChanged() {
        if (suppressCallbacks) return;
        applyGrainLengthSamples(msToSamples(grainLengthSlider.getValue()));
        // The grain slider is only enabled in the grain-cloud modes, where the
        // window floors at the grain length. Bump an explicit band up so it
        // still holds the (possibly larger) grain, then slide its start inside
        // the source. Auto (windowStart == -1) needs no fixup.
        if (frame.windowStart >= 0 && modeUsesGrain()) {
            const int srcLen = (int)frame.source.size();
            const int g = std::max(16, frame.grainLength);
            int len = (frame.windowLen >= 0) ? frame.windowLen : g;
            len = std::clamp(std::max(len, g), std::min(g, srcLen), srcLen);
            frame.windowLen   = len;
            frame.windowStart = std::clamp(frame.windowStart, 0,
                                           std::max(0, srcLen - len));
        }
        updateSourceInfoLabel();   // grain (and maybe window) size changed
        pushPreviewWindow();
        applyEdit();
        repaint();  // band geometry may have changed
    }
    void onGrainCountChanged() {
        if (suppressCallbacks) return;
        frame.grainCount = std::clamp((int)std::round(grainCountSlider.getValue()),
                                      2, GrainFreezeVoice::kMaxGrains);
        pushPreviewExtras();
        applyEdit();
    }
    void onFftSizeChanged() {
        if (suppressCallbacks) return;
        frame.fftSize = fftComboIdToSize(fftSizeCombo.getSelectedId());
        pushPreviewExtras();
        applyEdit();
    }
    void onCrossfadeChanged() {
        if (suppressCallbacks) return;
        frame.crossfadeSamples = msToSamples(crossfadeSlider.getValue());
        applyEdit();
    }
    void onPitchChanged() {
        if (suppressCallbacks) return;
        frame.embeddedPitchHz = (float)pitchSlider.getValue();
        // Track the slider in the note + octave combos and cents readout
        // so the user sees the nearest note (and offset) as they drag.
        // Suppress to keep the combo updates from re-firing
        // onNoteOrOctaveChanged (which would snap the slider back and
        // either fight the drag or quantise the value).
        suppressCallbacks = true;
        auto [n, o] = hzToNearestNoteOctave(pitchSlider.getValue());
        noteCombo.setSelectedId(n + 1, juce::dontSendNotification);
        octaveCombo.setSelectedId(o + 1, juce::dontSendNotification);
        suppressCallbacks = false;
        updateCentsLabel(pitchSlider.getValue());
        applyEdit();
    }
    void onNoteOrOctaveChanged() {
        if (suppressCallbacks) return;
        const int n = noteCombo.getSelectedId() - 1;
        const int o = octaveCombo.getSelectedId() - 1;
        if (n < 0 || n > 11 || o < 0 || o > 9) return;
        const double hz = std::clamp(noteOctaveToHz(n, o),
                                     pitchSlider.getMinimum(),
                                     pitchSlider.getMaximum());
        // Drive the change through the slider so its onValueChange runs
        // (which updates the frame, mirrors the combos, and calls onApply).
        // sendNotificationSync ensures all of that happens before we return.
        pitchSlider.setValue(hz, juce::sendNotificationSync);
    }
    // Enable/disable + retooltip the grain-length slider for the current mode.
    // Only the two grain-cloud modes (Async, Pitch-synced) actually granulate,
    // so only they use the grain length. CrossfadeLoop loops the whole window
    // and SpectralFreeze FFTs it - neither has a grain - so grey the slider out
    // and say why (per the "grayed-out controls must explain themselves" rule).
    void refreshGrainSliderForMode() {
        const bool usesGrain   = modeUsesGrain();
        const bool isSpectral  = (frame.freezeMode == GranularFreezeMode::SpectralFreeze);
        // Grain length + grain count are the two grain-cloud controls: enabled
        // together, greyed together.
        grainLengthSlider.setEnabled(usesGrain);
        grainCountSlider.setEnabled(usesGrain);
        // FFT size is the Spectral-only control.
        fftSizeCombo.setEnabled(isSpectral);
        fftSizeLabel.setEnabled(isSpectral);

        if (usesGrain) {
            grainLengthSlider.setTooltip(
                "Length of each grain in the OLA cloud, in milliseconds. "
                "Short (~5 ms, the default) = a smooth, CONSTANT cloud; longer "
                "grains roam over a proportionally wider window and so sound "
                "more evolving / less constant. Doesn't change the captured "
                "source - just the size of the grains the synth scatters across "
                "the selection band while a note is held. The band can't be "
                "narrower than one grain.");
            grainCountSlider.setTooltip(
                "How many overlapping grains make up the cloud. More = denser, "
                "smoother (and a softer per-grain identity); fewer = sparser, "
                "more granular. The level stays constant as you change it.");
        } else if (isSpectral) {
            grainLengthSlider.setTooltip(
                "Disabled in Spectral-freeze mode - this mode freezes the FFT "
                "magnitude spectrum of the selection band, so grain length has "
                "no effect. Switch to Async or Pitch-synced grains to use it, "
                "or drag the band edges to set the spectral analysis region.");
            grainCountSlider.setTooltip(
                "Disabled in Spectral-freeze mode - there are no grains. Use the "
                "FFT size control instead. Switch to Async or Pitch-synced "
                "grains to set the grain count.");
        } else { // CrossfadeLoop
            grainLengthSlider.setTooltip(
                "Disabled in Crossfade-loop mode - this mode loops the whole "
                "selection band (there are no grains), so loop length is set by "
                "resizing the amber band, not by this slider. Switch to Async "
                "or Pitch-synced grains to use it.");
            grainCountSlider.setTooltip(
                "Disabled in Crossfade-loop mode - this mode loops the whole "
                "band, with no grains to count. Switch to Async or Pitch-synced "
                "grains to set the grain count.");
        }

        if (isSpectral) {
            fftSizeCombo.setTooltip(
                "FFT size for Spectral freeze, in samples. Larger = finer "
                "frequency detail (more bins) but a coarser time window, and "
                "needs a wider selection band to fit. Auto picks the largest "
                "size that fits the band (up to 2048).");
        } else {
            fftSizeCombo.setTooltip(
                "Disabled outside Spectral-freeze mode - the FFT size only "
                "matters when freezing the magnitude spectrum. Switch the freeze "
                "mode to Spectral freeze to use it.");
        }
    }
    void onFreezeModeChanged() {
        if (suppressCallbacks) return;
        const int id = freezeModeCombo.getSelectedId();
        if (id >= 1 && id <= 4) {
            frame.freezeMode = (GranularFreezeMode)(id - 1);
            // The window floor changes with the mode (cloud modes floor at the
            // grain; crossfade/spectral floor smaller). Re-clamp an explicit band
            // to the new floor so the stored windowLen stays honest - e.g. a tiny
            // crossfade window grows back to >= grain when switching to a cloud
            // mode. Auto (windowStart == -1) needs no fixup.
            if (frame.windowStart >= 0 && frame.windowLen >= 0) {
                const int srcLen = (int)frame.source.size();
                const int len = std::clamp(frame.windowLen,
                                           std::min(minWindowSamples(), srcLen),
                                           srcLen);
                frame.windowLen   = len;
                frame.windowStart = std::clamp(frame.windowStart, 0,
                                               std::max(0, srcLen - len));
            }
            // The band's meaning and floor change with the mode (only the cloud
            // modes use the grain), so refresh the grain slider's enabled state
            // + tooltip, re-range the crossfade cap (loop length depends on the
            // mode), re-push the live preview window, and repaint the band at
            // its new effective floor.
            refreshGrainSliderForMode();
            refreshCrossfadeRange();
            updateSourceInfoLabel();   // window floor + grain visibility changed
            pushPreviewWindow();
            repaint();
            // applyEdit() re-publishes the held audition snapshot (synth path)
            // so the post-rebuild voice picks up the new freeze mode; in the
            // engine-preview fallback there's no rebuild, so push the mode
            // straight to the live preview atomics instead.
            if (playing && !sendAudition) {
                if (auto* eng = AudioEngine::getInstance())
                    eng->setGrainFreezeMode(frame.freezeMode);
            }
            applyEdit();
        }
    }

    // Audition (Play / Stop button).
    //
    // Preferred path: sendAudition is set when this editor is hosted inside
    // a synth-owned wavetable editor. We send MIDI note-on (A4 / vel 100)
    // to the owning synth node's pendingAudition queue and the
    // TerrainSynthProcessor renders it through the actual voice / AHDSR /
    // Volume path - matching exactly what a wired-up MIDI note plays. This
    // is the right answer for "does this cell sound the same as a played
    // note?" because there is literally one playback path.
    //
    // Fallback path: when sendAudition is null (e.g. the post-capture
    // editor in CaptureFromSongDialog where there is no synth node yet),
    // use the AudioEngine's GrainLoop preview mixer to play the source PCM
    // directly. This bypasses the synth's envelope and Volume param, so
    // levels won't match a wired-up note - but it's the only option when
    // there is no synth node to route through.
    void togglePlay() {
        if (playing) stopPlay();
        else         startPlay();
    }

    // A4 = MIDI note 69 = 440 Hz. Matches the embedded-pitch slider's
    // default for capture flows that don't know the source's true pitch,
    // so an unconfigured source plays at its native rate (no resampling).
    static constexpr int kAuditionPitch    = 69;
    static constexpr int kAuditionVelocity = 127;

    void startPlay() {
        if (sendAudition) {
            sendAudition(true, kAuditionPitch, kAuditionVelocity);
        } else {
            auto* eng = AudioEngine::getInstance();
            if (!eng) return;
            if (frame.source.empty()) return;  // nothing to audition

            // Source: hand the engine a shared_ptr copy of the frame's PCM.
            auto src = std::make_shared<std::vector<float>>(frame.source);
            // A grain must fit inside the freeze window or the engine floors the
            // band width up to the grain, swallowing the source and tripping the
            // "band too short to roam" viability gate -> silent async preview.
            // Clamp to the resolved window exactly as the capture dialog and
            // buildFrames do, so the fallback audition never goes silent.
            const int grainLen = std::max(64,
                std::min(frame.grainLength, effectiveWindowLen()));
            const int xfadeLen = std::max(0, frame.crossfadeSamples);

            eng->setGrainFreezeMode(frame.freezeMode);
            eng->setPreviewGrainLength(grainLen);
            eng->setPreviewCrossfadeLength(xfadeLen);
            // Pitch the preview to the frame's "As note" label, matching the
            // synth-voice path (note 69 = A4): ratio = (440 / embeddedPitchHz)
            // * (sourceSampleRate / deviceRate). The srRatio factor corrects a
            // frame captured at a rate other than the graph's processing rate;
            // without it this fallback preview plays at a different pitch than
            // the placed synth note (terrain_synth.cpp renderGrainSample) and
            // than the capture dialog's fixed preview.
            const double devRate = eng->getSampleRate();
            const double srRatio = (frame.sourceSampleRate > 0.0 && devRate > 0.0)
                ? (frame.sourceSampleRate / devRate) : 1.0;
            eng->setPreviewGrainRatio(
                frame.embeddedPitchHz > 0.0f
                    ? (float)((440.0 / frame.embeddedPitchHz) * srRatio)
                    : (float)srRatio);
            // Freeze the exact selection band the editor shows (-1 = auto).
            eng->setPreviewWindowStart(frame.windowStart);
            eng->setPreviewWindowLen(frame.windowLen);
            // Grain count + FFT size (cloud / spectral modes; auto-ignored by
            // the others), matching the synth-voice path.
            eng->setPreviewGrainCount(frame.grainCount);
            eng->setPreviewFftSize(frame.fftSize);
            eng->setPreviewGrainBuffer(std::move(src));
            eng->setPreviewMode(AudioEngine::PreviewMode::GrainLoop);
        }

        playing = true;
        playBtn.setButtonText("Stop");
        playBtn.setColour(juce::TextButton::buttonColourId,
                          juce::Colour(140, 70, 70));
    }

    void stopPlay() {
        if (!playing) return;
        playing = false;
        if (sendAudition) {
            sendAudition(false, kAuditionPitch, 0);
        } else {
            if (auto* eng = AudioEngine::getInstance())
                eng->clearPreview();
        }
        playBtn.setButtonText("Play");
        playBtn.setColour(juce::TextButton::buttonColourId,
                          juce::Colour(60, 110, 70));
    }
};

// ==============================================================================
// LayeredWaveform - data model
// ==============================================================================

// Seed a fresh Drawn layer with a handful of points arranged like a sine so
// the user has something visible to grab and move.
static constexpr int kFreehandSampleCount = 512;

static std::vector<float> defaultFreehandSamples() {
    std::vector<float> out(kFreehandSampleCount, 0.0f);
    for (int i = 0; i < kFreehandSampleCount; ++i) {
        float phase = (float)i / (float)kFreehandSampleCount;
        out[i] = std::sin(2.0f * (float)M_PI * phase);
    }
    return out;
}

static std::vector<std::pair<float, float>> defaultDrawnPoints() {
    return {
        {0.00f,  0.0f},
        {0.25f,  1.0f},
        {0.50f,  0.0f},
        {0.75f, -1.0f}
    };
}

static float evalShape(WaveLayer::Shape s, float x /*phase 0..1*/, std::mt19937& rng) {
    const float TWOPI = 2.0f * (float)M_PI;
    switch (s) {
        case WaveLayer::Sine:     return std::sin(TWOPI * x);
        case WaveLayer::Saw:      return 2.0f * (x - std::floor(x + 0.5f));
        case WaveLayer::Square:   return (x - std::floor(x) < 0.5f) ? 1.0f : -1.0f;
        case WaveLayer::Triangle: {
            float t = x - std::floor(x);
            return 4.0f * std::abs(t - 0.5f) - 1.0f;
        }
        case WaveLayer::Noise: {
            std::uniform_real_distribution<float> d(-1.0f, 1.0f);
            return d(rng);
        }
        case WaveLayer::Drawn:    return 0.0f; // handled by sampleLayer below
        case WaveLayer::Formula:  return 0.0f; // handled by sampleLayer below
    }
    return 0.0f;
}

// Catmull-Rom interpolation through a periodic sequence of (x, y) points.
// `pts` must be sorted by x. x values in [0, 1); we treat the list as
// cyclic, so p[-1] == p[n-1] and p[n] == p[0] (x-shifted by +/-1 accordingly).
static float sampleDrawnPoints(const std::vector<std::pair<float, float>>& pts,
                               float x)
{
    int n = (int)pts.size();
    if (n == 0) return 0.0f;
    if (n == 1) return pts[0].second;

    // Wrap x into [0, 1)
    x = x - std::floor(x);

    // Find the segment [p1, p2] such that p1.x <= x < p2.x (handling wrap).
    int i1 = -1;
    for (int i = 0; i < n; ++i) {
        float a = pts[i].first;
        float b = (i + 1 < n) ? pts[i + 1].first : pts[0].first + 1.0f;
        float xx = x;
        if (a > b) { // shouldn't happen if sorted, but just in case
            if (xx < a) xx += 1.0f;
        }
        if (xx >= a && xx < b) { i1 = i; break; }
    }
    if (i1 < 0) i1 = n - 1;
    int i0 = (i1 - 1 + n) % n;
    int i2 = (i1 + 1) % n;
    int i3 = (i1 + 2) % n;

    float x1 = pts[i1].first;
    float x2 = (i1 + 1 < n) ? pts[i2].first : pts[i2].first + 1.0f;
    float xx = x;
    if (xx < x1) xx += 1.0f;
    float t = (x2 - x1 > 1e-6f) ? (xx - x1) / (x2 - x1) : 0.0f;
    t = juce::jlimit(0.0f, 1.0f, t);

    float y0 = pts[i0].second, y1 = pts[i1].second;
    float y2 = pts[i2].second, y3 = pts[i3].second;

    // Catmull-Rom
    float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * y1)
                 + (-y0 + y2) * t
                 + (2.0f*y0 - 5.0f*y1 + 4.0f*y2 - y3) * t2
                 + (-y0 + 3.0f*y1 - 3.0f*y2 + y3) * t3);
}

// Linear interpolation through a periodic array of per-sample waveform data.
static float sampleDrawnSamples(const std::vector<float>& samples, float x) {
    int n = (int)samples.size();
    if (n == 0) return 0.0f;
    x = x - std::floor(x);          // wrap to [0, 1)
    float idx = x * (float)n;
    int i0 = (int)idx % n;
    int i1 = (i0 + 1) % n;
    float frac = idx - std::floor(idx);
    return samples[i0] * (1.0f - frac) + samples[i1] * frac;
}

// Resolution of the one-cycle buffer baked from a Lua/Python Formula layer.
static constexpr int kFormulaBakeRes = 2048;

static float sampleLayer(const WaveLayer& layer, float x, std::mt19937& rng) {
    if (layer.shape == WaveLayer::Drawn) {
        if (layer.freehandMode && !layer.drawnSamples.empty())
            return sampleDrawnSamples(layer.drawnSamples, x);
        return sampleDrawnPoints(layer.drawnPoints, x);
    }
    if (layer.shape == WaveLayer::Formula) {
        if (layer.formulaLang == ShapeLang::Builtin) {
            // Built-in: evaluate the expression live at x in radians. The
            // parser is pure C++ and safe to call from the render path.
            if (layer.formulaExpr.empty()) return 0.0f;
            float v = WaveExprParser::evaluateAt(layer.formulaExpr,
                                                 2.0f * (float)M_PI * x,
                                                 0.0f);
            return juce::jlimit(-1.0f, 1.0f, v);
        }
        // Lua/Python: read the pre-baked one-cycle buffer. x carries the layer's
        // ratio/phase, so wrapping the single cycle reproduces the harmonic.
        if (!layer.formulaSamples.empty())
            return sampleDrawnSamples(layer.formulaSamples, x);
        return 0.0f;
    }
    return evalShape(layer.shape, x, rng);
}

void WaveLayer::rebakeFormula() {
    if (shape != Formula || formulaLang == ShapeLang::Builtin) {
        formulaSamples.clear();
        formulaError.clear();
        return;
    }
    std::string err;
    bakeShapeExpr(formulaLang, formulaExpr, /*domainRadians=*/true,
                  kFormulaBakeRes, formulaSamples, err);
    formulaError = err;
}

void LayeredWaveform::render(std::vector<float>& out) const {
    out.assign(tableSize, 0.0f);
    if (layers.empty()) return;

    for (const auto& layer : layers) {
        // Use a layer-specific deterministic seed so noise layers are stable
        // across renders (not changing on every edit).
        std::mt19937 rng(1234u + (unsigned)layer.ratio * 31u + (unsigned)layer.shape * 7u);
        int r = std::max(1, layer.ratio);
        for (int i = 0; i < tableSize; ++i) {
            float phase = (float)i / (float)tableSize;    // 0..1 over base period
            float x = phase * (float)r + layer.phase;     // ratio + phase offset
            out[i] += layer.amp * sampleLayer(layer, x, rng);
        }
    }

    // Normalize to peak 1.0
    float peak = 0.0f;
    for (float v : out) peak = std::max(peak, std::abs(v));
    if (peak > 1e-9f) {
        float inv = 1.0f / peak;
        for (float& v : out) v *= inv;
    }
}

static const char* shapeName(WaveLayer::Shape s) {
    switch (s) {
        case WaveLayer::Sine:     return "sine";
        case WaveLayer::Saw:      return "saw";
        case WaveLayer::Square:   return "square";
        case WaveLayer::Triangle: return "triangle";
        case WaveLayer::Noise:    return "noise";
        case WaveLayer::Drawn:    return "drawn";
        case WaveLayer::Formula:  return "formula";
    }
    return "sine";
}

static WaveLayer::Shape parseShape(const std::string& s) {
    if (s == "saw")      return WaveLayer::Saw;
    if (s == "square")   return WaveLayer::Square;
    if (s == "triangle") return WaveLayer::Triangle;
    if (s == "noise")    return WaveLayer::Noise;
    if (s == "drawn")    return WaveLayer::Drawn;
    if (s == "formula")  return WaveLayer::Formula;
    return WaveLayer::Sine;
}

// The layer-field separator is `,` and the layer separator is `|`, so a
// Formula expression containing those characters has to be escaped. We
// substitute commas with `;` and pipes with `\x1F` (Unit Separator, ASCII 31)
// on encode and reverse it on decode. Neither character is part of the
// WaveExprParser grammar, so this round-trips losslessly for any valid input.
static std::string escapeFormula(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if      (c == ',')  out += ';';
        else if (c == '|')  out += '\x1F';
        else                out += c;
    }
    return out;
}

static std::string unescapeFormula(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if      (c == ';')   out += ',';
        else if (c == '\x1F') out += '|';
        else                 out += c;
    }
    return out;
}

// Emit one layer as a comma-separated field list (no trailing `|`).
static void encodeLayer(std::ostringstream& o, const WaveLayer& l) {
    o << shapeName(l.shape)
      << "," << l.ratio
      << "," << l.phase
      << "," << l.amp;
    if (l.shape == WaveLayer::Drawn) {
        o << "," << (l.freehandMode ? 1 : 0);
        if (l.freehandMode) {
            o << "," << l.drawnSamples.size();
            for (float s : l.drawnSamples)
                o << "," << s;
        } else {
            o << "," << l.drawnPoints.size();
            for (auto& p : l.drawnPoints)
                o << "," << p.first << "," << p.second;
        }
    } else if (l.shape == WaveLayer::Formula) {
        // Field 4 = escaped expression. Commas inside the formula are mapped
        // to `;` so the comma-split parser still sees a single field.
        // Field 5 = authoring language ("builtin"/"lua"/"python"); absent in
        // pre-language saves, which decode as Built-in.
        o << "," << escapeFormula(l.formulaExpr)
          << "," << shapeLangKey(l.formulaLang);
    }
}

// Parse a single layer from a comma-separated string. Returns true on success.
static bool parseLayer(const std::string& lp, WaveLayer& out) {
    std::vector<std::string> f;
    size_t p = 0;
    while (p <= lp.size()) {
        size_t n = lp.find(',', p);
        if (n == std::string::npos) n = lp.size();
        f.push_back(lp.substr(p, n - p));
        p = n + 1;
    }
    if (f.size() < 4) return false;
    out = WaveLayer{};
    out.shape = parseShape(f[0]);
    try { out.ratio = std::stoi(f[1]); } catch (...) { out.ratio = 1; }
    try { out.phase = std::stof(f[2]); } catch (...) { out.phase = 0.0f; }
    try { out.amp   = std::stof(f[3]); } catch (...) { out.amp   = 1.0f; }
    if (out.shape == WaveLayer::Formula) {
        if (f.size() > 4)
            out.formulaExpr = unescapeFormula(f[4]);
        if (out.formulaExpr.empty())
            out.formulaExpr = "sin(x)";
        if (f.size() > 5)
            out.formulaLang = shapeLangFromKey(f[5]);
        out.rebakeFormula();   // populate formulaSamples for Lua/Python layers
        return true;
    }
    if (out.shape == WaveLayer::Drawn && f.size() > 4) {
        // Field 4: freehandMode flag (0 or 1). Legacy data without this flag
        // will have a point count here instead; detect by checking whether the
        // total field count matches point-mode layout.
        int freehandFlag = 0;
        try { freehandFlag = std::stoi(f[4]); } catch (...) {}

        // Heuristic for legacy data: if f[4] > 1, it must be a legacy point
        // count (old format had no freehand flag).  New format always has 0 or 1.
        bool isLegacy = (freehandFlag > 1);
        if (isLegacy) {
            out.freehandMode = false;
            int count = freehandFlag;
            for (int k = 0; k < count; ++k) {
                size_t xi = 5 + (size_t)k * 2;
                size_t yi = xi + 1;
                if (yi >= f.size()) break;
                float x = 0, y = 0;
                try { x = std::stof(f[xi]); } catch (...) {}
                try { y = std::stof(f[yi]); } catch (...) {}
                out.drawnPoints.emplace_back(x, y);
            }
        } else {
            out.freehandMode = (freehandFlag != 0);
            if (f.size() > 5) {
                int count = 0;
                try { count = std::stoi(f[5]); } catch (...) {}
                if (out.freehandMode) {
                    for (int k = 0; k < count && (size_t)(6 + k) < f.size(); ++k) {
                        float v = 0.0f;
                        try { v = std::stof(f[6 + k]); } catch (...) {}
                        out.drawnSamples.push_back(v);
                    }
                } else {
                    for (int k = 0; k < count; ++k) {
                        size_t xi = 6 + (size_t)k * 2;
                        size_t yi = xi + 1;
                        if (yi >= f.size()) break;
                        float x = 0, y = 0;
                        try { x = std::stof(f[xi]); } catch (...) {}
                        try { y = std::stof(f[yi]); } catch (...) {}
                        out.drawnPoints.emplace_back(x, y);
                    }
                }
            }
        }
    }
    return true;
}

std::string LayeredWaveform::encode() const {
    return "__layered__:" + encodeBody();
}

// Body form (no "__layered__:" prefix): tableSize|layer1|layer2|...
// Used by the IWavetableFrame interface so containers can embed the body
// with their own length prefix in the __wavetable2__ format (Phase 2).
std::string LayeredWaveform::encodeBody() const {
    std::ostringstream o;
    o << tableSize;
    for (const auto& l : layers) {
        o << "|";
        encodeLayer(o, l);
    }
    return o.str();
}

bool LayeredWaveform::decodeBody(const std::string& body) {
    // decode() already strips an optional __layered__: prefix and parses
    // the body, so we can just delegate.
    return decode(body);
}

// IWavetableFrame::render(int ts, out) override - render at the caller's
// requested table size instead of the member tableSize. Implemented via a
// shallow copy so the existing const render(out) can stay untouched and the
// member tableSize doesn't have to be mutated through const.
void LayeredWaveform::renderRaw(int ts, std::vector<float>& out) const {
    if (ts <= 0) { out.clear(); return; }
    LayeredWaveform tmp = *this;
    tmp.tableSize = ts;
    tmp.render(out);   // no-arg render = gain-free primitive; base render() applies gain
}

std::unique_ptr<IWavetableFrame> LayeredWaveform::clone() const {
    return std::make_unique<LayeredWaveform>(*this);
}

bool LayeredWaveform::decode(const std::string& s) {
    layers.clear();
    std::string body = s;
    if (body.rfind("__layered__:", 0) == 0) body = body.substr(12);
    if (body.empty()) return false;

    // split by '|'
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t next = body.find('|', pos);
        if (next == std::string::npos) next = body.size();
        parts.push_back(body.substr(pos, next - pos));
        pos = next + 1;
    }
    if (parts.empty()) return false;
    try {
        tableSize = std::stoi(parts[0]);
    } catch (...) { tableSize = 2048; }

    for (size_t i = 1; i < parts.size(); ++i) {
        WaveLayer layer;
        if (parseLayer(parts[i], layer))
            layers.push_back(layer);
    }
    return !layers.empty();
}

// ==============================================================================
// LayerStackComponent
// ==============================================================================

LayerStackComponent::LayerStackComponent(Options o, std::function<void()> ch)
    : opts(std::move(o)), onChanged(std::move(ch))
{
    addLayerBtn.setButtonText(opts.addLayerButtonText);
    addLayerBtn.setTooltip("Add a new layer. Each layer is a sine, saw, square, "
                           "triangle, noise, drawn or formula shape; all the "
                           "layers are summed into the final waveform.");
    addLayerBtn.onClick = [this]() { addLayer(); };
    addAndMakeVisible(addLayerBtn);

    viewport.setViewedComponent(&container, false);
    viewport.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport);

    emptyHintLabel.setJustificationType(juce::Justification::centredTop);
    emptyHintLabel.setColour(juce::Label::textColourId, juce::Colour(0xff808088));
    emptyHintLabel.setText(opts.emptyHint, juce::dontSendNotification);
    addChildComponent(emptyHintLabel);   // visibility toggled in rebuildRows
}

void LayerStackComponent::setTarget(LayeredWaveform* lw) {
    int cnt = lw ? (int)lw->layers.size() : -1;
    // Already in sync (same object, same layer count) -> the existing rows
    // still point at valid WaveLayers, so there's nothing to rebuild. This
    // makes setTarget cheap to call defensively from a resized() pass.
    if (lw == target && cnt == shownLayerCount) return;
    target = lw;
    rebuildRows();
    resized();
}

void LayerStackComponent::refreshFromModel() {
    rebuildRows();
    resized();
}

void LayerStackComponent::rebuildRows() {
    rows.clear();
    container.removeAllChildren();
    shownLayerCount = target ? (int)target->layers.size() : -1;

    int rh = WaveLayerEditor::rowHeight();
    int vw = std::max(viewport.getWidth(), 480);
    int y = 0;
    if (target) {
        for (int i = 0; i < (int)target->layers.size(); ++i) {
            // Capture i by value; the delete callback uses it to identify the
            // slot. The non-realloc invariant holds because we rebuild every
            // row whenever the layer count changes (add / delete).
            WaveLayerEditor::Callbacks cb;
            cb.onChanged = [this]() {
                renderSummation();
                if (onChanged) onChanged();
            };
            cb.indexForLabel = [i]() { return i + 1; };
            cb.onDelete = [this, i]() {
                if (!target) return;
                if (i < 0 || i >= (int)target->layers.size()) return;
                target->layers.erase(target->layers.begin() + i);
                rebuildRows();
                resized();
                renderSummation();
                if (onChanged) onChanged();
            };
            auto row = std::make_unique<WaveLayerEditor>(&target->layers[i],
                                                         std::move(cb));
            row->setBounds(0, y, vw, rh);
            row->syncFromModel();
            container.addAndMakeVisible(row.get());
            rows.push_back(std::move(row));
            y += rh + 4;
        }
    }
    container.setSize(vw, std::max(y, 10));

    bool empty = rows.empty();
    emptyHintLabel.setVisible(empty && opts.emptyHint.isNotEmpty());

    renderSummation();
}

void LayerStackComponent::addLayer() {
    if (!target) return;
    WaveLayer l = opts.makeNewLayer ? opts.makeNewLayer((int)target->layers.size())
                                    : WaveLayer{};
    target->layers.push_back(l);
    rebuildRows();
    resized();
    renderSummation();
    if (onChanged) onChanged();
}

void LayerStackComponent::renderSummation() {
    if (!opts.showSummationPreview || !target || target->layers.empty()) {
        summationSamples.clear();
        repaint();
        return;
    }
    target->render(512, summationSamples);
    repaint();
}

void LayerStackComponent::layoutRows() {
    int vw = viewport.getWidth();
    int rh = WaveLayerEditor::rowHeight();
    int y = 0;
    for (auto& row : rows) {
        row->setBounds(0, y, vw, rh);
        y += rh + 4;
    }
    container.setSize(vw, std::max(y, 10));
}

void LayerStackComponent::resized() {
    auto r = getLocalBounds();

    if (opts.showSummationPreview) {
        summationBounds = r.removeFromBottom(opts.summationPreviewHeight);
        r.removeFromBottom(6);
    } else {
        summationBounds = juce::Rectangle<int>();
    }

    auto header = r.removeFromTop(24);
    addLayerBtn.setBounds(header.removeFromLeft(110));
    r.removeFromTop(4);

    viewport.setBounds(r);
    layoutRows();

    if (emptyHintLabel.isVisible()) {
        emptyHintLabel.setBounds(r.reduced(20).withHeight(60));
        emptyHintLabel.toFront(false);
    }
}

void LayerStackComponent::paint(juce::Graphics& g) {
    if (summationBounds.isEmpty()) return;
    auto a = summationBounds.toFloat();
    g.setColour(juce::Colour(0xff111114));
    g.fillRoundedRectangle(a, 4.0f);
    g.setColour(juce::Colour(0xff2a2a30));
    g.drawRoundedRectangle(a, 4.0f, 1.0f);

    g.setColour(juce::Colour(0xff808088));
    g.setFont(juce::FontOptions(12.0f));
    g.drawText("Sum of all layers", a.reduced(6).removeFromTop(16),
               juce::Justification::topLeft);

    float cy = a.getCentreY();
    g.setColour(juce::Colour(0xff2a2a30));
    g.drawHorizontalLine((int)cy, a.getX(), a.getRight());

    if (summationSamples.empty()) {
        g.setColour(juce::Colour(0xff55555c));
        g.drawText("(no layers yet - add one to shape the signal)", a,
                   juce::Justification::centred);
        return;
    }

    g.setColour(juce::Colour(0xff5fb3ff));
    juce::Path p;
    int n = (int)summationSamples.size();
    float h = a.getHeight();
    for (int i = 0; i < n; ++i) {
        float x = a.getX() + (n > 1 ? (float)i / (float)(n - 1) : 0.0f) * a.getWidth();
        float y = cy - summationSamples[i] * h * 0.42f;
        if (i == 0) p.startNewSubPath(x, y);
        else        p.lineTo(x, y);
    }
    g.strokePath(p, juce::PathStrokeType(1.5f));
}

// ==============================================================================
// WavetableDoc
// ==============================================================================

// Format reference:
//   Grid: __wavetable__:<tableSize>:g;<numDims>;<dim0>;<dim1>;...:<frameCount>:<gridFrame0>:<gridFrame1>:...
//     where gridFrameN = layer1|layer2|...
//   Scatter: __wavetable__:<tableSize>:s;<scatterDims>;<radius>:<frameCount>:<scatterFrame0>:...
//     where scatterFrameN = pos0;pos1;...;posN@<label>@<layer1|layer2|...>
//   Legacy (no g/s prefix): treated as grid format with parts[1] containing dims OR a frame count.
// =============================================================================
// __wavetable2__ format (current)
// =============================================================================
//
// Grammar:
//   __wavetable2__:<ts>:<modeSpec>:<count>[<frame>]*
//
//   modeSpec (Grid):    g;<numDims>;<dim0>;<dim1>;...
//   modeSpec (Scatter): s;<scatterDims>;<radius>
//
//   frame (Grid):    :<typeId>:<bodyLen>:<body of exactly bodyLen chars>
//   frame (Scatter): :<pos0>;<pos1>;...@<label>@<typeId>:<bodyLen>:<body>
//
// The body length prefix lets the body contain any character (including
// the structural delimiters ':' '|' ';' '@') without escaping. This is
// essential because the wavelet body uses ':' internally and the layered
// body uses '|' to separate layers.
//
// Label sanitisation: '@', ':' and '|' are replaced with '_' on encode so
// the position@label@typeId framing stays unambiguous.

static std::string sanitizeLabel(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (c == '@' || c == ':' || c == '|') o.push_back('_');
        else                                   o.push_back(c);
    }
    return o;
}

// =============================================================================
// __wavetable5__ format (current) - library + cell-by-reference model with
// per-entry colour and per-entry gain
// =============================================================================
//
// Top-level grammar:
//   __wavetable5__:<ts>:<modeSpec>:<libCount>[<libEntry>]*:<cellCount>[<cell>]*
//
//   modeSpec (Grid):    g;<numDims>;<dim0>;<dim1>;...
//   modeSpec (Scatter): s;<scatterDims>;<radius>
//
//   libEntry: :<id>:<colorIdx>:<gain>:<nameLen>:<name><typeId>:<bodyLen>:<body>
//     <colorIdx> is -1 (= Auto) or a 0-based palette index. <gain> is the
//     per-frame output gain (IWavetableFrame::gain), a float; 1 = unity.
//     <name> is exactly <nameLen> raw chars (no escaping). <body> is exactly
//     <bodyLen> raw chars, same length-prefix trick as v2.
//
//   cell (Grid):    :<libraryId>
//     -1 = empty cell; any other id must resolve to a library entry.
//   cell (Scatter): :<libraryId>:<pos0>;<pos1>;...@<label>
//     Label is sanitized for '@' ':' '|' (see sanitizeLabel).
//
// Why a fifth version: v4 added per-entry <colorIdx>. v5 adds per-entry
// <gain> (between <colorIdx> and <nameLen>) so a per-waveform volume scaler
// survives save/load. v4 files load through decodeWavetableV4 (gain defaults
// to 1.0). v3 (pre-colour) and v2/v1 decoders also still migrate forward.
// The shared decoder decodeWavetableV4or5() reads the gain field only when
// told the payload is v5, so one function covers both.

std::string WavetableDoc::encode() const {
    std::ostringstream o;
    o << "__wavetable5__:" << tableSize << ":";
    if (mode == WavetableMode::Grid) {
        o << "g;" << gridDims.size();
        for (int d : gridDims) o << ";" << d;
        // Optional trailing field: "empty cells fade volume" (no
        // renormalization). Absent in older payloads, which decode as false
        // (renormalized over filled cells).
        o << ";" << (absoluteBlend ? 1 : 0);
    } else {
        o << "s;" << scatterDims << ";" << scatterRadius
          << ";" << (absoluteBlend ? 1 : 0);
    }

    // Library section.
    o << ":" << library.size();
    for (const auto& e : library) {
        float gain = e.wave ? e.wave->gain : 1.0f;
        o << ":" << e.id << ":" << e.colorIdx << ":" << gain << ":"
          << e.name.size() << ":" << e.name;
        const char* tid = e.wave ? e.wave->typeId() : "layered";
        std::string body = e.wave ? e.wave->encodeBody() : "";
        o << tid << ":" << body.size() << ":" << body;
    }

    // Cell section.
    if (mode == WavetableMode::Grid) {
        o << ":" << cellWaveformIds.size();
        for (int id : cellWaveformIds) o << ":" << id;
    } else {
        o << ":" << scatterFrames.size();
        for (const auto& sf : scatterFrames) {
            o << ":" << sf.waveformId << ":";
            for (int i = 0; i < (int)sf.position.size(); ++i) {
                if (i > 0) o << ";";
                o << sf.position[i];
            }
            o << "@" << sanitizeLabel(sf.label);
        }
    }
    return o.str();
}

// Helper: parse the layer string `layer1|layer2|...` into a LayeredWaveform.
static LayeredWaveform parseLayerString(const std::string& fb, int tableSize) {
    LayeredWaveform lw;
    lw.tableSize = tableSize;
    size_t lp = 0;
    while (lp <= fb.size()) {
        size_t ln = fb.find('|', lp);
        if (ln == std::string::npos) ln = fb.size();
        std::string layerStr = fb.substr(lp, ln - lp);
        if (!layerStr.empty()) {
            WaveLayer layer;
            if (parseLayer(layerStr, layer))
                lw.layers.push_back(layer);
        }
        lp = ln + 1;
    }
    return lw;
}

// Construct a concrete IWavetableFrame by typeId. Hardcoded list of known
// types - swap to a registry if/when this grows. Returns nullptr on
// unknown tag (the caller treats the slot as "frame skipped").
static std::unique_ptr<IWavetableFrame> createFrameByTypeId(const std::string& tid) {
    if (tid == "layered")  return std::make_unique<LayeredWaveform>();
    if (tid == "spectral") return std::make_unique<SpectralFrame>();
    if (tid == "wavelet")  return std::make_unique<WaveletFrame>();
    if (tid == "sample")   return std::make_unique<SampleFrame>();
    if (tid == "granular") return std::make_unique<GranularFrame>();
    return nullptr;
}

// Single-pass cursor over the v2 body. Read* methods advance `p` and
// return false on overrun so the parser fails cleanly.
struct V2Reader {
    const std::string& s;
    size_t p = 0;
    explicit V2Reader(const std::string& src) : s(src) {}

    bool eof() const { return p >= s.size(); }

    // Read up to (and consume) the next occurrence of `delim`. Returns the
    // text before the delimiter. If `delim` isn't found, consumes to EOS
    // and returns the remainder (caller treats this as the final token).
    std::string readUntil(char delim) {
        size_t n = s.find(delim, p);
        if (n == std::string::npos) {
            std::string out = s.substr(p);
            p = s.size();
            return out;
        }
        std::string out = s.substr(p, n - p);
        p = n + 1;
        return out;
    }

    // Read exactly N raw chars without delimiter scanning. Returns ""
    // (and leaves p unchanged) on overrun so the caller can detect failure.
    std::string readN(size_t n) {
        if (p + n > s.size()) return {};
        std::string out = s.substr(p, n);
        p += n;
        return out;
    }

    // Step past a single expected delimiter, if it's at the cursor. Used
    // at field boundaries where the previous read was readN(): readN
    // consumes exactly the length-prefixed body and stops, so the trailing
    // `:` written by the encoder is still sitting at the cursor. Without
    // this skip, the next readUntil(':') sees the separator immediately,
    // returns the empty string before it, and any stoi() on the result
    // throws - which historically truncated the cell section on load and
    // produced the "placements vanish on reopen" bug. No-op if the next
    // char isn't the expected delimiter, so callers can use it defensively
    // without worrying about EOF.
    void consume(char expected) {
        if (p < s.size() && s[p] == expected) ++p;
    }
};

// Shared helpers used by every decoder variant.

static std::vector<std::string> splitSemi(const std::string& src) {
    std::vector<std::string> out;
    size_t dp = 0;
    while (dp <= src.size()) {
        size_t dn = src.find(';', dp);
        if (dn == std::string::npos) dn = src.size();
        out.push_back(src.substr(dp, dn - dp));
        dp = dn + 1;
    }
    return out;
}

// Parse the leading "g;..." / "s;..." mode header. Writes into `doc.mode`,
// `doc.gridDims`, `doc.scatterDims`, `doc.scatterRadius` and returns true if
// `modeSpec` looked well-formed enough to commit to that mode.
static bool parseModeSpec(WavetableDoc& doc, const std::string& modeSpec) {
    if (modeSpec.empty()) return false;
    const bool isScatter = (modeSpec[0] == 's');
    size_t mp = (modeSpec.size() > 1 && modeSpec[1] == ';') ? 2 : 1;
    const std::string modeBody = modeSpec.substr(mp);

    if (isScatter) {
        doc.mode = WavetableMode::Scatter;
        auto dp = splitSemi(modeBody);
        if (!dp.empty())   try { doc.scatterDims   = std::stoi(dp[0]); } catch (...) { doc.scatterDims = 1; }
        if (dp.size() > 1) try { doc.scatterRadius = std::stof(dp[1]); } catch (...) { doc.scatterRadius = 0.45f; }
        // Optional 4th field (added later): non-normalized "distance fades
        // volume" blend. Absent in older v4/v3/v2 payloads, which decode as
        // false (the original volume-normalized behavior).
        if (dp.size() > 2) try { doc.absoluteBlend = (std::stoi(dp[2]) != 0); } catch (...) { doc.absoluteBlend = false; }
        // scatterDims is the geometric view dimension: 0 = drop-target
        // placeholder, 1 = line view, 2+ = square/cube. Clamp to >= 0.
        if (doc.scatterDims < 0) doc.scatterDims = 0;
    } else {
        doc.mode = WavetableMode::Grid;
        auto dp = splitSemi(modeBody);
        int numDims = 0;
        if (!dp.empty()) try { numDims = std::stoi(dp[0]); } catch (...) {}
        for (int d = 0; d < numDims && d + 1 < (int)dp.size(); ++d) {
            try { doc.gridDims.push_back(std::stoi(dp[d + 1])); }
            catch (...) { doc.gridDims.push_back(1); }
        }
        // Optional trailing field after the dims: "empty cells fade volume"
        // (no renormalization). Absent in older payloads -> false.
        if ((int)dp.size() > numDims + 1)
            try { doc.absoluteBlend = (std::stoi(dp[numDims + 1]) != 0); } catch (...) { doc.absoluteBlend = false; }
    }
    return true;
}

// =============================================================================
// __wavetable4__ / __wavetable5__ decoder
// =============================================================================
//
// v4: library entries carry colorIdx (between id and nameLen).
// v5: same, plus a per-entry gain float (between colorIdx and nameLen).
//
// `hasGain` selects which layout to expect. v4 payloads pass false (gain
// defaults to 1.0); v5 payloads pass true. See the format-grammar comment
// above encode().

static bool decodeWavetableV4or5(WavetableDoc& doc, const std::string& body,
                                 bool hasGain) {
    V2Reader r(body);
    try { doc.tableSize = std::stoi(r.readUntil(':')); }
    catch (...) { doc.tableSize = 2048; }

    const std::string modeSpec = r.readUntil(':');
    if (!parseModeSpec(doc, modeSpec)) return false;
    const bool isScatter = (doc.mode == WavetableMode::Scatter);

    // ---- Library section ----
    int libCount = 0;
    try { libCount = std::stoi(r.readUntil(':')); } catch (...) {}

    int maxIdSeen = 0;
    for (int e = 0; e < libCount && !r.eof(); ++e) {
        int id = -1;
        try { id = std::stoi(r.readUntil(':')); } catch (...) { return false; }
        int colorIdx = -1;
        try { colorIdx = std::stoi(r.readUntil(':')); } catch (...) { return false; }
        float gain = 1.0f;
        if (hasGain) {
            try { gain = std::stof(r.readUntil(':')); } catch (...) { gain = 1.0f; }
        }
        size_t nameLen = 0;
        try { nameLen = (size_t)std::stoul(r.readUntil(':')); } catch (...) { return false; }
        std::string name = r.readN(nameLen);
        if (name.size() != nameLen) return false;
        std::string typeId = r.readUntil(':');
        size_t bodyLen = 0;
        try { bodyLen = (size_t)std::stoul(r.readUntil(':')); } catch (...) { return false; }
        std::string fbody = r.readN(bodyLen);
        if (fbody.size() != bodyLen) return false;
        // readN doesn't consume the trailing `:` that the encoder writes
        // between this body and the next field. Step past it so the next
        // readUntil(':') reads the next entry's id (or cellCount when
        // this was the last entry) instead of the empty string before
        // the separator. See V2Reader::consume.
        r.consume(':');

        std::unique_ptr<IWavetableFrame> frame = createFrameByTypeId(typeId);
        if (frame && !frame->decodeBody(fbody)) frame.reset();
        if (!frame) {
            auto lw = std::make_unique<LayeredWaveform>();
            lw->tableSize = doc.tableSize;
            frame = std::move(lw);
        }
        // Per-frame gain lives on the base class and is serialised by the
        // container (not encodeBody), so apply it here after the body decode.
        if (frame) frame->gain = gain;

        WaveformLibraryEntry entry;
        entry.id = id;
        entry.colorIdx = colorIdx;
        entry.name = std::move(name);
        entry.wave = std::move(frame);
        doc.library.push_back(std::move(entry));
        if (id > maxIdSeen) maxIdSeen = id;
    }
    doc.nextLibraryId = std::max(doc.nextLibraryId, maxIdSeen + 1);

    // ---- Cell section ----
    // The `:` before cellCount was already consumed by the per-entry
    // r.consume(':') above (it's the same separator from the perspective
    // of the format - it sits between the last entry's body and the
    // cellCount field).
    int cellCount = 0;
    try { cellCount = std::stoi(r.readUntil(':')); } catch (...) {}

    if (isScatter) {
        for (int c = 0; c < cellCount && !r.eof(); ++c) {
            int libId = -1;
            try { libId = std::stoi(r.readUntil(':')); } catch (...) {}
            const std::string posStr = r.readUntil('@');
            const std::string label  = r.readUntil(':');
            ScatterFrame sf;
            sf.waveformId = libId;
            sf.label = label;
            auto pp = splitSemi(posStr);
            for (auto& pstr : pp) {
                try { sf.position.push_back(std::stof(pstr)); }
                catch (...) { sf.position.push_back(0.5f); }
            }
            while ((int)sf.position.size() < doc.scatterDims) sf.position.push_back(0.5f);
            if ((int)sf.position.size() > doc.scatterDims)    sf.position.resize(doc.scatterDims);
            doc.scatterFrames.push_back(std::move(sf));
        }
    } else {
        for (int c = 0; c < cellCount && !r.eof(); ++c) {
            int libId = -1;
            try { libId = std::stoi(r.readUntil(':')); } catch (...) {}
            doc.cellWaveformIds.push_back(libId);
        }
        if (doc.gridDims.empty()) doc.gridDims = { (int)doc.cellWaveformIds.size() };
    }
    return true;
}

// =============================================================================
// __wavetable3__ decoder (legacy: pre-colorIdx)
// =============================================================================
//
// Reads the library + cell sections from the v3 format. Library entries
// don't carry a colour, so colorIdx defaults to -1 (Auto) on every entry.

static bool decodeWavetableV3(WavetableDoc& doc, const std::string& body) {
    V2Reader r(body);
    try { doc.tableSize = std::stoi(r.readUntil(':')); }
    catch (...) { doc.tableSize = 2048; }

    const std::string modeSpec = r.readUntil(':');
    if (!parseModeSpec(doc, modeSpec)) return false;
    const bool isScatter = (doc.mode == WavetableMode::Scatter);

    // ---- Library section ----
    int libCount = 0;
    try { libCount = std::stoi(r.readUntil(':')); } catch (...) {}

    int maxIdSeen = 0;
    for (int e = 0; e < libCount && !r.eof(); ++e) {
        int id = -1;
        try { id = std::stoi(r.readUntil(':')); } catch (...) { return false; }
        size_t nameLen = 0;
        try { nameLen = (size_t)std::stoul(r.readUntil(':')); } catch (...) { return false; }
        std::string name = r.readN(nameLen);
        if (name.size() != nameLen) return false;
        std::string typeId = r.readUntil(':');
        size_t bodyLen = 0;
        try { bodyLen = (size_t)std::stoul(r.readUntil(':')); } catch (...) { return false; }
        std::string fbody = r.readN(bodyLen);
        if (fbody.size() != bodyLen) return false;
        // Step past the `:` separator after the body. Same reason as V4
        // - readN didn't consume it. Without this, the next readUntil(':')
        // would read "" and stoi() would throw.
        r.consume(':');

        std::unique_ptr<IWavetableFrame> frame = createFrameByTypeId(typeId);
        if (frame && !frame->decodeBody(fbody)) frame.reset();
        if (!frame) {
            auto lw = std::make_unique<LayeredWaveform>();
            lw->tableSize = doc.tableSize;
            frame = std::move(lw);
        }

        WaveformLibraryEntry entry;
        entry.id = id;
        entry.name = std::move(name);
        entry.wave = std::move(frame);
        doc.library.push_back(std::move(entry));
        if (id > maxIdSeen) maxIdSeen = id;
    }
    doc.nextLibraryId = std::max(doc.nextLibraryId, maxIdSeen + 1);

    // ---- Cell section ----
    int cellCount = 0;
    try { cellCount = std::stoi(r.readUntil(':')); } catch (...) {}

    if (isScatter) {
        for (int c = 0; c < cellCount && !r.eof(); ++c) {
            int libId = -1;
            try { libId = std::stoi(r.readUntil(':')); } catch (...) {}
            const std::string posStr = r.readUntil('@');
            const std::string label  = r.readUntil(':');
            ScatterFrame sf;
            sf.waveformId = libId;
            sf.label = label;
            auto pp = splitSemi(posStr);
            for (auto& pstr : pp) {
                try { sf.position.push_back(std::stof(pstr)); }
                catch (...) { sf.position.push_back(0.5f); }
            }
            while ((int)sf.position.size() < doc.scatterDims) sf.position.push_back(0.5f);
            if ((int)sf.position.size() > doc.scatterDims)    sf.position.resize(doc.scatterDims);
            doc.scatterFrames.push_back(std::move(sf));
        }
    } else {
        for (int c = 0; c < cellCount && !r.eof(); ++c) {
            int libId = -1;
            try { libId = std::stoi(r.readUntil(':')); } catch (...) {}
            doc.cellWaveformIds.push_back(libId);
        }
        if (doc.gridDims.empty()) doc.gridDims = { (int)doc.cellWaveformIds.size() };
    }
    return true;
}

// =============================================================================
// __wavetable2__ decoder (legacy; auto-migrates to library + cell ids)
// =============================================================================
//
// v2 stored each cell's waveform inline. To load into the v3 data model, each
// non-null cell is promoted to a fresh library entry and the cell stores that
// id. Sparse-grid empty cells become id = -1 (no library entry created).

static bool decodeWavetableV2(WavetableDoc& doc, const std::string& body) {
    V2Reader r(body);
    try { doc.tableSize = std::stoi(r.readUntil(':')); }
    catch (...) { doc.tableSize = 2048; }

    const std::string modeSpec = r.readUntil(':');
    if (!parseModeSpec(doc, modeSpec)) return false;
    const bool isScatter = (doc.mode == WavetableMode::Scatter);

    int frameCount = 0;
    try { frameCount = std::stoi(r.readUntil(':')); } catch (...) {}

    for (int f = 0; f < frameCount && !r.eof(); ++f) {
        std::string positions, label;
        if (isScatter) {
            positions = r.readUntil('@');
            label     = r.readUntil('@');
        }
        std::string typeId = r.readUntil(':');
        size_t bodyLen = 0;
        try { bodyLen = (size_t)std::stoul(r.readUntil(':')); } catch (...) { return false; }
        std::string fbody = r.readN(bodyLen);
        if (fbody.size() != bodyLen) return false;
        // Defensive: step past a `:` if it's sitting at the cursor. The
        // v2 encoder is no longer in the tree so we can't verify the
        // exact inter-frame separator from source, but the V3/V4
        // encoders write `:` between length-prefixed bodies and the
        // next field, and V2's structure mirrors that pattern. consume
        // is a no-op if the next char isn't `:`, so this is safe either
        // way - and matches the V3/V4 decoder fix for the same class
        // of bug.
        r.consume(':');

        // "null" is the explicit sparse-cell marker for Grid mode and is the
        // one typeId where nullptr is the correct decoded result. For every
        // other typeId, decode failure or unknown type falls back to an empty
        // layered frame so the wavetable still loads with the right frame count.
        const bool isExplicitNull = (typeId == "null");
        std::unique_ptr<IWavetableFrame> frame;
        if (!isExplicitNull) {
            frame = createFrameByTypeId(typeId);
            if (frame && !frame->decodeBody(fbody)) frame.reset();
            if (!frame) {
                auto lw = std::make_unique<LayeredWaveform>();
                lw->tableSize = doc.tableSize;
                frame = std::move(lw);
            }
        }

        if (isScatter) {
            // A null scatter frame would have no waveform at all; coerce to
            // empty layered so the scatter list stays well-formed.
            if (!frame) {
                auto lw = std::make_unique<LayeredWaveform>();
                lw->tableSize = doc.tableSize;
                frame = std::move(lw);
            }
            const int libId = doc.addLibraryEntry(std::move(frame));
            ScatterFrame sf;
            sf.label = label;
            sf.waveformId = libId;
            auto pp = splitSemi(positions);
            for (auto& pstr : pp) {
                try { sf.position.push_back(std::stof(pstr)); }
                catch (...) { sf.position.push_back(0.5f); }
            }
            while ((int)sf.position.size() < doc.scatterDims) sf.position.push_back(0.5f);
            if ((int)sf.position.size() > doc.scatterDims)    sf.position.resize(doc.scatterDims);
            doc.scatterFrames.push_back(std::move(sf));
        } else {
            if (!frame) {
                // Sparse / empty grid cell.
                doc.cellWaveformIds.push_back(-1);
            } else {
                const int libId = doc.addLibraryEntry(std::move(frame));
                doc.cellWaveformIds.push_back(libId);
            }
        }
    }

    if (!isScatter && doc.gridDims.empty())
        doc.gridDims = { (int)doc.cellWaveformIds.size() };

    return isScatter ? !doc.scatterFrames.empty() : !doc.cellWaveformIds.empty();
}

// =============================================================================
// Legacy __wavetable__ decoder (Phase 1; layered-only, auto-migrates to v3)
// =============================================================================

static bool decodeWavetableLegacy(WavetableDoc& doc, const std::string& body) {
    // Split body on ':' -> [tableSize, modeAndDims, frameCount, frame0, frame1, ...]
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t next = body.find(':', pos);
        if (next == std::string::npos) next = body.size();
        parts.push_back(body.substr(pos, next - pos));
        pos = next + 1;
    }
    if (parts.size() < 2) return false;
    try { doc.tableSize = std::stoi(parts[0]); } catch (...) { doc.tableSize = 2048; }

    // Detect mode: parts[1] starts with "g;" (grid v2-style), "s;" (scatter), or
    // is bare digits (grid v1 with semicolons OR very-old single frame count).
    int frameStartIdx = 2;
    int frameCount = 0;
    bool isScatter = false;

    std::string dimsField = parts[1];
    if (!dimsField.empty() && (dimsField[0] == 'g' || dimsField[0] == 's')) {
        isScatter = (dimsField[0] == 's');
        dimsField = dimsField.substr(1);
        if (!dimsField.empty() && dimsField[0] == ';') dimsField.erase(0, 1);
    }

    if (isScatter) {
        // Scatter: dimsField = "scatterDims;radius"
        auto dp = splitSemi(dimsField);
        doc.mode = WavetableMode::Scatter;
        if (!dp.empty())     try { doc.scatterDims   = std::stoi(dp[0]); } catch (...) { doc.scatterDims = 1; }
        if (dp.size() > 1)   try { doc.scatterRadius = std::stof(dp[1]); } catch (...) { doc.scatterRadius = 0.45f; }
        if (dp.size() > 2)   try { doc.absoluteBlend = (std::stoi(dp[2]) != 0); } catch (...) {}
        if (doc.scatterDims < 0) doc.scatterDims = 0;
        if (parts.size() > 2) try { frameCount = std::stoi(parts[2]); } catch (...) {}
        frameStartIdx = 3;

        for (int f = 0; f < frameCount && (size_t)(frameStartIdx + f) < parts.size(); ++f) {
            const std::string& fb = parts[frameStartIdx + f];
            // Split on '@': posList @ label @ layers
            size_t a1 = fb.find('@');
            size_t a2 = (a1 != std::string::npos) ? fb.find('@', a1 + 1) : std::string::npos;
            std::string posStr = (a1 != std::string::npos) ? fb.substr(0, a1) : "";
            std::string label  = (a1 != std::string::npos && a2 != std::string::npos)
                                  ? fb.substr(a1 + 1, a2 - a1 - 1) : "";
            std::string layerStr = (a2 != std::string::npos) ? fb.substr(a2 + 1) : "";

            ScatterFrame sf;
            sf.label = std::move(label);
            auto pp = splitSemi(posStr);
            for (auto& p : pp) {
                try { sf.position.push_back(std::stof(p)); }
                catch (...) { sf.position.push_back(0.5f); }
            }
            while ((int)sf.position.size() < doc.scatterDims) sf.position.push_back(0.5f);
            if ((int)sf.position.size() > doc.scatterDims) sf.position.resize(doc.scatterDims);

            auto lw = std::make_unique<LayeredWaveform>(parseLayerString(layerStr, doc.tableSize));
            sf.waveformId = doc.addLibraryEntry(std::move(lw));
            doc.scatterFrames.push_back(std::move(sf));
        }
        return !doc.scatterFrames.empty();
    }

    // Grid path (v1 with bare numDims;dims, or very-old with bare frameCount).
    doc.mode = WavetableMode::Grid;
    if (dimsField.find(';') != std::string::npos) {
        auto dp = splitSemi(dimsField);
        int numDims = 0;
        if (!dp.empty()) try { numDims = std::stoi(dp[0]); } catch (...) {}
        for (int d = 0; d < numDims && d + 1 < (int)dp.size(); ++d) {
            try { doc.gridDims.push_back(std::stoi(dp[d + 1])); } catch (...) { doc.gridDims.push_back(1); }
        }
        if (parts.size() > 2) try { frameCount = std::stoi(parts[2]); } catch (...) {}
        frameStartIdx = 3;
    } else {
        // Very old: parts[1] is just a frame count, default 1D
        try { frameCount = std::stoi(dimsField); } catch (...) {}
        doc.gridDims = { frameCount };
        frameStartIdx = 2;
    }

    for (int f = 0; f < frameCount && (size_t)(frameStartIdx + f) < parts.size(); ++f) {
        auto lw = std::make_unique<LayeredWaveform>(
            parseLayerString(parts[frameStartIdx + f], doc.tableSize));
        const int libId = doc.addLibraryEntry(std::move(lw));
        doc.cellWaveformIds.push_back(libId);
    }
    if (doc.gridDims.empty()) doc.gridDims = { (int)doc.cellWaveformIds.size() };

    return !doc.cellWaveformIds.empty();
}

bool WavetableDoc::decode(const std::string& s) {
    library.clear();
    nextLibraryId = 1;
    cellWaveformIds.clear();
    scatterFrames.clear();
    gridDims.clear();
    mode = WavetableMode::Grid;
    scatterFromGridSnapshot.reset();

    {
        const std::string prefix = "__wavetable5__:";
        if (s.rfind(prefix, 0) == 0)
            return decodeWavetableV4or5(*this, s.substr(prefix.size()), /*hasGain=*/true);
    }
    {
        const std::string prefix = "__wavetable4__:";
        if (s.rfind(prefix, 0) == 0)
            return decodeWavetableV4or5(*this, s.substr(prefix.size()), /*hasGain=*/false);
    }
    {
        const std::string prefix = "__wavetable3__:";
        if (s.rfind(prefix, 0) == 0)
            return decodeWavetableV3(*this, s.substr(prefix.size()));
    }
    {
        const std::string prefix = "__wavetable2__:";
        if (s.rfind(prefix, 0) == 0)
            return decodeWavetableV2(*this, s.substr(prefix.size()));
    }
    {
        const std::string prefix = "__wavetable__:";
        if (s.rfind(prefix, 0) == 0)
            return decodeWavetableLegacy(*this, s.substr(prefix.size()));
    }
    return false;
}

// ---- Library API ----------------------------------------------------------

int WavetableDoc::addLibraryEntry(std::unique_ptr<IWavetableFrame> wave,
                                  std::string name) {
    WaveformLibraryEntry e;
    e.id = nextLibraryId++;
    if (name.empty()) {
        std::ostringstream o;
        o << "Waveform " << (int)library.size() + 1;
        name = o.str();
    }
    e.name = std::move(name);
    e.wave = std::move(wave);
    library.push_back(std::move(e));
    return library.back().id;
}

bool WavetableDoc::removeLibraryEntry(int id) {
    if (id < 0) return false;
    const int idx = findLibraryIndexById(id);
    if (idx < 0) return false;
    library.erase(library.begin() + idx);
    // Clear cell references to the removed id.
    for (int& c : cellWaveformIds) if (c == id) c = -1;
    for (auto& sf : scatterFrames)  if (sf.waveformId == id) sf.waveformId = -1;
    return true;
}

int WavetableDoc::findLibraryIndexById(int id) const {
    if (id < 0) return -1;
    for (size_t i = 0; i < library.size(); ++i)
        if (library[i].id == id) return (int)i;
    return -1;
}

IWavetableFrame* WavetableDoc::libraryFrameById(int id) {
    const int idx = findLibraryIndexById(id);
    return idx < 0 ? nullptr : library[(size_t)idx].wave.get();
}

const IWavetableFrame* WavetableDoc::libraryFrameById(int id) const {
    const int idx = findLibraryIndexById(id);
    return idx < 0 ? nullptr : library[(size_t)idx].wave.get();
}

int WavetableDoc::libraryIdForCell(int cellIdx) const {
    if (mode == WavetableMode::Grid) {
        if (cellIdx < 0 || cellIdx >= (int)cellWaveformIds.size()) return -1;
        return cellWaveformIds[(size_t)cellIdx];
    }
    if (cellIdx < 0 || cellIdx >= (int)scatterFrames.size()) return -1;
    return scatterFrames[(size_t)cellIdx].waveformId;
}

void WavetableDoc::assignCellToLibrary(int cellIdx, int libraryId) {
    // -1 is the "clear" sentinel; any other id must exist in the library
    // or the call is a no-op (preserves the invariant that every non-(-1)
    // cell id is resolvable).
    if (libraryId != -1 && findLibraryIndexById(libraryId) < 0) return;
    if (mode == WavetableMode::Grid) {
        if (cellIdx < 0 || cellIdx >= (int)cellWaveformIds.size()) return;
        cellWaveformIds[(size_t)cellIdx] = libraryId;
    } else {
        if (cellIdx < 0 || cellIdx >= (int)scatterFrames.size()) return;
        scatterFrames[(size_t)cellIdx].waveformId = libraryId;
    }
}

bool WavetableDoc::isLibraryEntryUsed(int id) const {
    return countCellsUsingLibrary(id) > 0;
}

int WavetableDoc::countCellsUsingLibrary(int id) const {
    if (id < 0) return 0;
    int n = 0;
    // Only count references in the ACTIVE mode's container. Both
    // cellWaveformIds and scatterFrames can carry stale data from the other
    // mode (we don't clear the inactive container on mode switch, because
    // the user may want to flip back without losing placement), but they
    // are not rendered in the arrangement view in the inactive mode. A
    // user-facing usage count that included the invisible references
    // produced the long-standing "library says used 1x but I see no dot"
    // bug: a waveform with one scatter placement showed as used in the
    // library sidebar even after the user switched to Grid mode, where
    // only cellWaveformIds is rendered.
    if (mode == WavetableMode::Grid) {
        for (int c : cellWaveformIds)        if (c == id) ++n;
    } else {
        for (const auto& sf : scatterFrames) if (sf.waveformId == id) ++n;
    }
    return n;
}

// ---- Per-cell frame access (rerouted through the library) ----------------

IWavetableFrame* WavetableDoc::frameAt(int idx) {
    return libraryFrameById(libraryIdForCell(idx));
}

const IWavetableFrame* WavetableDoc::frameAt(int idx) const {
    return libraryFrameById(libraryIdForCell(idx));
}

LayeredWaveform* WavetableDoc::layeredFrameAt(int idx) {
    return dynamic_cast<LayeredWaveform*>(frameAt(idx));
}

LayeredWaveform* WavetableDoc::layeredFrameByLibrary(int libId) {
    return dynamic_cast<LayeredWaveform*>(libraryFrameById(libId));
}

const LayeredWaveform* WavetableDoc::layeredFrameByLibrary(int libId) const {
    return dynamic_cast<const LayeredWaveform*>(libraryFrameById(libId));
}

const LayeredWaveform* WavetableDoc::layeredFrameAt(int idx) const {
    return dynamic_cast<const LayeredWaveform*>(frameAt(idx));
}

// ---- Sparse-grid / mode-conversion helpers -----------------------------------

int WavetableDoc::gridCellCount() const {
    if (gridDims.empty()) return 0;
    int n = 1;
    for (int d : gridDims) {
        if (d <= 0) return 0;
        n *= d;
    }
    return n;
}

// === TEMP WAVETABLE DIAGNOSTIC (throwaway) ===
// Dump the grid/cell/scatter/library state to D:/temp/wt_diag.txt so we can see
// exactly when gridDims and cellWaveformIds desync, and whether cells point at
// the expected library entries. Called from the conversion + axis-edit paths.
void WavetableDoc::debugDumpState(const char* tag) const {
    juce::String s;
    s << "--- " << tag << " ---\n";
    s << "mode=" << (mode == WavetableMode::Grid ? "Grid" : "Scatter")
      << "  scatterDims=" << scatterDims << "\n";
    s << "gridDims=[";
    long long prod = gridDims.empty() ? 0 : 1;
    for (size_t d = 0; d < gridDims.size(); ++d) {
        s << gridDims[d] << (d + 1 < gridDims.size() ? "x" : "");
        prod *= gridDims[d];
    }
    s << "]  product=" << prod
      << "  cellWaveformIds.size()=" << (int)cellWaveformIds.size();
    if (prod != (long long)cellWaveformIds.size())
        s << "   *** MISMATCH ***";
    s << "\n";
    s << "library(" << (int)library.size() << "): ";
    for (const auto& e : library)
        s << "[" << e.id << ":" << (e.name.empty() ? "<unnamed>" : e.name) << "] ";
    s << "\n";
    s << "cells: ";
    for (size_t i = 0; i < cellWaveformIds.size(); ++i) {
        const int id = cellWaveformIds[i];
        const char* nm = "<empty>";
        if (id >= 0) {
            int li = findLibraryIndexById(id);
            nm = (li >= 0 && !library[(size_t)li].name.empty())
                     ? library[(size_t)li].name.c_str() : "<unnamed-or-missing>";
        }
        s << i << "=" << id << "(" << nm << ") ";
    }
    s << "\n";
    if (mode == WavetableMode::Scatter) {
        s << "scatterFrames(" << (int)scatterFrames.size() << "): ";
        for (const auto& sf : scatterFrames) {
            s << "id=" << sf.waveformId << "@(";
            for (size_t d = 0; d < sf.position.size(); ++d)
                s << juce::String(sf.position[d], 2) << (d + 1 < sf.position.size() ? "," : "");
            s << ") ";
        }
        s << "\n";
    }
    juce::File("D:/temp/wt_diag.txt").appendText(s);
}
// === END TEMP DIAGNOSTIC ===

std::vector<int> WavetableDoc::cellIdxToGridCoord(int idx) const {
    const int total = gridCellCount();
    if (idx < 0 || idx >= total) return {};
    std::vector<int> coord(gridDims.size(), 0);
    int rem = idx;
    // Row-major: last axis varies fastest. Matches gridToFlat() inverse.
    for (int d = (int)gridDims.size() - 1; d >= 0; --d) {
        const int sz = gridDims[d];
        coord[d] = rem % sz;
        rem /= sz;
    }
    return coord;
}

int WavetableDoc::gridCoordToCellIdx(const std::vector<int>& coord) const {
    if (coord.size() != gridDims.size()) return -1;
    int flat = 0, stride = 1;
    for (int d = (int)gridDims.size() - 1; d >= 0; --d) {
        if (coord[d] < 0 || coord[d] >= gridDims[d]) return -1;
        flat += coord[d] * stride;
        stride *= gridDims[d];
    }
    return flat;
}

std::vector<float> WavetableDoc::cellCenterPosition(int cellIdx) const {
    auto coord = cellIdxToGridCoord(cellIdx);
    if (coord.empty()) return {};
    std::vector<float> pos(coord.size(), 0.5f);
    for (size_t d = 0; d < coord.size(); ++d) {
        const int sz = gridDims[d];
        pos[d] = (sz > 0) ? ((float)coord[d] + 0.5f) / (float)sz : 0.5f;
    }
    return pos;
}

std::vector<int> WavetableDoc::effectiveAxes() const {
    std::vector<int> axes;
    if (mode == WavetableMode::Grid) {
        // An axis with a single cell is inert - a Position along it always
        // resolves to the only cell. Only axes of size >= 2 are traversable.
        for (int d = 0; d < (int)gridDims.size(); ++d)
            if (gridDims[d] >= 2) axes.push_back(d);
    } else {
        // Scatter traversability is per-dimension and purely positional,
        // exactly analogous to a grid axis needing >=2 cells: dimension d is
        // traversable iff the dots actually span a range along d (i.e. they
        // don't all share the same coordinate on d). Consequences:
        //   - 0 or 1 dot spans nothing (a single point has no extent) -> no
        //     axes at all, regardless of the blend mode (normalized vs
        //     "distance fades volume"). The blend mode only changes the gain
        //     math, never which axes exist.
        //   - 2 dots differing only in X -> just an X axis.
        //   - 2 dots differing only in Y -> just a Y axis (no X axis).
        //   - 2 dots differing in both -> both axes.
        // Drag a dot until it stops differing on an axis and that axis's
        // Position control disappears, just like shrinking a grid axis to one
        // cell. Each dimension is evaluated independently.
        constexpr float spanEps = 1e-6f;
        if ((int)scatterFrames.size() >= 2) {
            for (int d = 0; d < scatterDims; ++d) {
                float lo = 0.0f, hi = 0.0f;
                bool any = false;
                for (const auto& sf : scatterFrames) {
                    const float v = (d < (int)sf.position.size())
                                      ? sf.position[(size_t)d] : 0.5f;
                    if (!any) { lo = hi = v; any = true; }
                    else { lo = std::min(lo, v); hi = std::max(hi, v); }
                }
                if (any && (hi - lo) > spanEps) axes.push_back(d);
            }
        }
    }
    return axes;
}

void WavetableDoc::resizeGridAxis(int axisIdx, int newSize) {
    if (axisIdx < 0 || axisIdx >= (int)gridDims.size()) return;
    if (newSize < 1) newSize = 1;
    if (newSize == gridDims[axisIdx]) return;

    // Walk every old cell and copy its library id into the new flat layout if
    // its coordinate is still in range on the resized axis. We can't do this
    // in place because the row-major strides change with the axis size. Cells
    // that fall outside the new size are dropped (their library entries
    // remain in the library, just unreferenced).
    std::vector<int> newDims = gridDims;
    newDims[axisIdx] = newSize;

    auto coordToFlat = [](const std::vector<int>& coord, const std::vector<int>& dims) {
        int flat = 0, stride = 1;
        for (int d = (int)dims.size() - 1; d >= 0; --d) {
            flat += coord[d] * stride;
            stride *= dims[d];
        }
        return flat;
    };

    int newTotal = 1;
    for (int d : newDims) newTotal *= d;

    std::vector<int> newCells(newTotal, -1);
    const int oldTotal = (int)cellWaveformIds.size();
    for (int oldIdx = 0; oldIdx < oldTotal; ++oldIdx) {
        auto coord = cellIdxToGridCoord(oldIdx);
        if (coord.empty()) continue;
        if (coord[axisIdx] >= newSize) continue; // dropped by shrink
        const int newIdx = coordToFlat(coord, newDims);
        newCells[newIdx] = cellWaveformIds[oldIdx];
    }
    cellWaveformIds = std::move(newCells);
    gridDims = std::move(newDims);

    // Any prior scatter-revert snapshot is invalidated by a topology change.
    scatterFromGridSnapshot.reset();
    debugDumpState("after resizeGridAxis");
}

void WavetableDoc::convertGridToScatter() {
    // Build a fresh scatterFrames list from every non-empty grid cell.
    // Library entries are NOT moved - cells reference them by id, and the
    // library is shared across both modes. Record the source cell index so
    // canRevertScatterToGrid() can detect an unedited round-trip.
    ScatterFromGridSnapshot snap;
    snap.gridDims = gridDims;

    const int total = (int)cellWaveformIds.size();
    std::vector<ScatterFrame> newScatter;
    newScatter.reserve(total);

    // Preserve the grid's dimensionality (floored to 1 so a 1D grid becomes a
    // 1D line-view scatter, a 2D grid becomes a 2D square-view scatter, etc.).
    const int outDims = std::max((int)gridDims.size(), 1);

    for (int cellIdx = 0; cellIdx < total; ++cellIdx) {
        const int libId = cellWaveformIds[cellIdx];
        if (libId < 0) continue;
        ScatterFrame sf;
        sf.waveformId = libId;
        sf.position = cellCenterPosition(cellIdx);
        // Pad to outDims (e.g., a 1D grid becomes 2D with y=0.5).
        while ((int)sf.position.size() < outDims) sf.position.push_back(0.5f);
        // Colour now lives on the library entry (not per-dot), so we don't
        // assign it here - whatever the entry's colorIdx is at paint time
        // applies to every placement, grid cell or scatter dot alike.
        newScatter.push_back(std::move(sf));
        snap.originalCellIdx.push_back(cellIdx);
    }

    scatterFrames = std::move(newScatter);
    scatterDims = outDims;
    mode = WavetableMode::Scatter;
    cellWaveformIds.clear();
    scatterFromGridSnapshot = std::move(snap);
    debugDumpState("after convertGridToScatter");
}

bool WavetableDoc::canRevertScatterToGrid() const {
    if (mode != WavetableMode::Scatter) return false;
    if (!scatterFromGridSnapshot.has_value()) return false;
    const auto& snap = *scatterFromGridSnapshot;
    if (snap.originalCellIdx.size() != scatterFrames.size()) return false;
    if (snap.gridDims.empty()) return false;

    // Compute cell centers from the snapshot's grid shape (not the current
    // gridDims, which may be empty in scatter mode) and compare componentwise.
    auto snapCellCenter = [&](int idx) -> std::vector<float> {
        std::vector<float> pos(snap.gridDims.size(), 0.5f);
        int rem = idx;
        for (int d = (int)snap.gridDims.size() - 1; d >= 0; --d) {
            const int sz = snap.gridDims[d];
            if (sz <= 0) return {};
            const int c = rem % sz;
            rem /= sz;
            pos[d] = ((float)c + 0.5f) / (float)sz;
        }
        return pos;
    };

    constexpr float tol = 1e-4f;
    for (size_t i = 0; i < scatterFrames.size(); ++i) {
        const int srcCell = snap.originalCellIdx[i];
        auto expected = snapCellCenter(srcCell);
        if (expected.empty()) return false;
        const auto& got = scatterFrames[i].position;
        // The snapshot only constrains the first snap.gridDims.size() axes;
        // any extra padding axes (e.g., y when reverting a 1D-from-grid view)
        // must still sit at 0.5 to count as "unmoved".
        for (size_t d = 0; d < got.size(); ++d) {
            const float exp = (d < expected.size()) ? expected[d] : 0.5f;
            if (std::abs(got[d] - exp) > tol) return false;
        }
    }
    return true;
}

void WavetableDoc::revertScatterToGrid() {
    if (!canRevertScatterToGrid()) {
        jassertfalse;
        return;
    }
    const auto snap = *scatterFromGridSnapshot;  // copy before we mutate
    const int total = [&]{ int n = 1; for (int d : snap.gridDims) n *= d; return n; }();

    std::vector<int> newCells(total, -1);
    for (size_t i = 0; i < scatterFrames.size(); ++i) {
        const int cell = snap.originalCellIdx[i];
        if (cell < 0 || cell >= total) continue;
        newCells[cell] = scatterFrames[i].waveformId;
    }

    cellWaveformIds = std::move(newCells);
    gridDims = snap.gridDims;
    mode = WavetableMode::Grid;
    scatterFrames.clear();
    // The snapshot is consumed by a successful revert; a fresh
    // convertGridToScatter() will install a new one if the user converts again.
    scatterFromGridSnapshot.reset();
    debugDumpState("after revertScatterToGrid (lossless)");
}

void WavetableDoc::convertScatterToGrid() {
    // Lossy fallback for when canRevertScatterToGrid() is false (authored as
    // Scatter, or the snapshot was invalidated by an axis/dot edit). Unlike a
    // plain flatten-to-1D, this PRESERVES the scatter's dimensionality wherever
    // possible: it quantizes each axis's dot coordinates into distinct sorted
    // "tracks" and rebuilds the N-D grid those tracks imply. For dots that form
    // a clean Cartesian lattice (e.g. a grid converted to scatter, even after a
    // save/load drops the lossless snapshot) this reconstructs the original
    // grid shape exactly - a 2x2x2 comes back as a 2x2x2, not a 1x8 line, and
    // holes (empty cells) are preserved. Irregular/sparse scatters that would
    // blow up into a mostly-empty grid, or whose dots collide into one cell,
    // fall back to the simple 1D layout. The shared library is untouched.
    const int nFrames = (int)scatterFrames.size();

    auto flattenTo1D = [&]() {
        std::vector<int> newCells;
        newCells.reserve(scatterFrames.size());
        for (const auto& sf : scatterFrames)
            newCells.push_back(sf.waveformId);
        if (newCells.empty())
            newCells.push_back(-1);  // keep a single empty cell so the grid is valid
        cellWaveformIds = std::move(newCells);
        gridDims = { (int)cellWaveformIds.size() };
    };

    bool built = false;
    if (nFrames > 0) {
        // Geometric dimensionality of the scatter space, clamped to what the
        // dots actually carry.
        int D = std::max(1, scatterDims);
        for (const auto& sf : scatterFrames)
            D = std::min(D, std::max(1, (int)sf.position.size()));

        constexpr float tol = 1e-3f;
        auto coordOf = [&](const ScatterFrame& sf, int d) -> float {
            return (d < (int)sf.position.size()) ? sf.position[(size_t)d] : 0.5f;
        };

        // Per-axis sorted distinct coordinate values ("tracks").
        std::vector<std::vector<float>> tracks((size_t)D);
        for (int d = 0; d < D; ++d) {
            std::vector<float> vals;
            for (const auto& sf : scatterFrames) {
                const float v = coordOf(sf, d);
                bool found = false;
                for (float u : vals) if (std::abs(u - v) <= tol) { found = true; break; }
                if (!found) vals.push_back(v);
            }
            std::sort(vals.begin(), vals.end());
            tracks[(size_t)d] = std::move(vals);
        }

        std::vector<int> dims((size_t)D, 1);
        long long prod = 1;
        for (int d = 0; d < D; ++d) {
            dims[(size_t)d] = std::max(1, (int)tracks[(size_t)d].size());
            prod *= dims[(size_t)d];
        }

        // Row-major flatten (last axis fastest), matching gridCoordToCellIdx.
        auto flatten = [&](const std::vector<int>& c) {
            int flat = 0, stride = 1;
            for (int d = D - 1; d >= 0; --d) { flat += c[(size_t)d] * stride; stride *= dims[(size_t)d]; }
            return flat;
        };

        // Only reconstruct N-D when the grid stays reasonably dense (an
        // irregular scatter would explode into a mostly-empty grid).
        if (prod > 0 && prod <= (long long)nFrames * 4 + 4) {
            std::vector<int> ndCells((size_t)prod, -1);
            bool clean = true;
            for (const auto& sf : scatterFrames) {
                std::vector<int> c((size_t)D, 0);
                for (int d = 0; d < D; ++d) {
                    const float v = coordOf(sf, d);
                    int best = 0; float bestErr = 1e30f;
                    for (int i = 0; i < (int)tracks[(size_t)d].size(); ++i) {
                        const float e = std::abs(tracks[(size_t)d][(size_t)i] - v);
                        if (e < bestErr) { bestErr = e; best = i; }
                    }
                    c[(size_t)d] = best;
                }
                const int flat = flatten(c);
                if (flat < 0 || flat >= (int)ndCells.size() || ndCells[(size_t)flat] != -1) {
                    clean = false;  // collision / out of range -> not a clean lattice
                    break;
                }
                ndCells[(size_t)flat] = sf.waveformId;
            }
            if (clean) {
                cellWaveformIds = std::move(ndCells);
                gridDims = std::move(dims);
                built = true;
            }
        }
    }

    if (!built)
        flattenTo1D();

    mode = WavetableMode::Grid;
    scatterFrames.clear();
    scatterFromGridSnapshot.reset();
    debugDumpState(built ? "after convertScatterToGrid (N-D rebuild)"
                         : "after convertScatterToGrid (1D fallback)");
}

WavetableDoc WavetableDoc::defaultSingleSine() {
    WavetableDoc d;
    d.tableSize = 2048;
    d.gridDims = {1}; // 1D with 1 cell
    const int libId = d.addLibraryEntry(
        std::make_unique<LayeredWaveform>(LayeredWaveform::defaultSine()),
        "Sine");
    d.cellWaveformIds.push_back(libId);
    return d;
}

WavetableDoc WavetableDoc::defaultEmpty() {
    // 1D wavetable with a single empty cell. gridDims = {1} keeps the
    // gridDims-product invariant in sync with cellWaveformIds.size(), and
    // "+ Waveform" sparse-aware placement (see LayeredWaveEditorComponent's
    // add-frame handler) finds the empty cell and slots the first real
    // waveform in without growing the grid. The library starts empty.
    WavetableDoc d;
    d.tableSize = 2048;
    d.gridDims = {1};
    d.cellWaveformIds.push_back(-1);
    return d;
}

LayeredWaveform LayeredWaveform::defaultSine() {
    LayeredWaveform w;
    WaveLayer l;
    l.shape = WaveLayer::Sine;
    l.ratio = 1;
    l.phase = 0.0f;
    l.amp   = 1.0f;
    w.layers.push_back(l);
    return w;
}

// ==============================================================================
// WaveLayerEditor - one editor per layer (shared between the wavetable
// editor's stacked layer rows and the SignalShape editor's single layer view)
// ==============================================================================

// Sample one layer's contribution over one cycle (amp-scaled, not normalized).
// Used for the per-layer mini preview.
static void renderSingleLayer(const WaveLayer& layer, int tableSize,
                              std::vector<float>& out)
{
    out.assign(tableSize, 0.0f);
    std::mt19937 rng(1234u + (unsigned)layer.ratio * 31u + (unsigned)layer.shape * 7u);
    // For Drawn shapes, show one cycle in the preview so the control
    // points line up with the curve. The harmonic ratio still applies
    // in the final multi-layer render (LayeredWaveform::render).
    int r = (layer.shape == WaveLayer::Drawn) ? 1 : std::max(1, layer.ratio);
    for (int i = 0; i < tableSize; ++i) {
        float phase = (float)i / (float)tableSize;
        float x = phase * (float)r + layer.phase;
        out[i] = layer.amp * sampleLayer(layer, x, rng);
    }
}

// ---------------- WaveLayerEditor presets ----------------
//
// Preset = a snapshot of WaveLayer fields the user can stamp in as a
// starting point. Presets cover all shape modes so the user can drop in a
// "ready-made" waveform and tweak from there, rather than building every
// wave from scratch.
namespace {
struct WaveLayerPreset {
    const char* name;
    std::function<void(WaveLayer&)> apply;
};

static const std::vector<WaveLayerPreset>& wavePresets() {
    static const std::vector<WaveLayerPreset> presets = {
        { "Sine",         [](WaveLayer& l) { l.shape = WaveLayer::Sine; } },
        { "Saw",          [](WaveLayer& l) { l.shape = WaveLayer::Saw; } },
        { "Square",       [](WaveLayer& l) { l.shape = WaveLayer::Square; } },
        { "Triangle",     [](WaveLayer& l) { l.shape = WaveLayer::Triangle; } },
        { "Noise",        [](WaveLayer& l) { l.shape = WaveLayer::Noise; } },
        // Pulse 25%: drawn as a single rectangle: high for first quarter,
        // low for remainder. Two points isn't enough for cubic interpolation
        // to render a flat pulse, so use a denser set.
        { "Pulse 25%",    [](WaveLayer& l) {
            l.shape = WaveLayer::Drawn;
            l.freehandMode = false;
            l.drawnPoints = {
                {0.00f,  1.0f}, {0.24f,  1.0f},
                {0.25f, -1.0f}, {0.99f, -1.0f}
            };
        }},
        { "Pulse 75%",    [](WaveLayer& l) {
            l.shape = WaveLayer::Drawn;
            l.freehandMode = false;
            l.drawnPoints = {
                {0.00f,  1.0f}, {0.74f,  1.0f},
                {0.75f, -1.0f}, {0.99f, -1.0f}
            };
        }},
        // Half-sine: rectified sine, useful as an envelope curve.
        { "Half-sine",    [](WaveLayer& l) {
            l.shape = WaveLayer::Formula;
            l.formulaExpr = "if(sin(x) > 0, sin(x), 0)";
        }},
        // Ramp shapes via Drawn so the user can grab points to reshape.
        { "Ramp up",      [](WaveLayer& l) {
            l.shape = WaveLayer::Drawn;
            l.freehandMode = false;
            l.drawnPoints = { {0.0f, -1.0f}, {0.999f, 1.0f} };
        }},
        { "Ramp down",    [](WaveLayer& l) {
            l.shape = WaveLayer::Drawn;
            l.freehandMode = false;
            l.drawnPoints = { {0.0f, 1.0f}, {0.999f, -1.0f} };
        }},
        // Formula presets for common analog-flavored shapes.
        { "Soft saw",     [](WaveLayer& l) {
            l.shape = WaveLayer::Formula;
            l.formulaExpr = "tanh(2 * saw(x))";
        }},
        { "FM bell",      [](WaveLayer& l) {
            l.shape = WaveLayer::Formula;
            l.formulaExpr = "sin(x + 0.7 * sin(2.4 * x))";
        }},
        { "Organ-ish",    [](WaveLayer& l) {
            l.shape = WaveLayer::Formula;
            l.formulaExpr = "0.6*sin(x) + 0.3*sin(2*x) + 0.1*sin(4*x)";
        }},
    };
    return presets;
}
} // anonymous namespace

// ---------------- WaveLayerEditor implementation ----------------

WaveLayerEditor::WaveLayerEditor(WaveLayer* layerPtr, Callbacks cb)
    : layer(layerPtr), callbacks(std::move(cb))
{
    addAndMakeVisible(label);
    label.setJustificationType(juce::Justification::centredLeft);
    label.setFont(13.0f);

    auto addShapeBtn = [this](juce::TextButton& b, const char* name, WaveLayer::Shape s) {
        addAndMakeVisible(b);
        b.setButtonText(name);
        b.setClickingTogglesState(true);
        b.setRadioGroupId(0); // we'll handle toggling manually
        b.onClick = [this, s]() {
            if (!layer) return;
            layer->shape = s;
            // Seed a fresh Drawn layer with a few points so the user has
            // something grabable instead of an empty canvas.
            if (s == WaveLayer::Drawn && layer->drawnPoints.empty())
                layer->drawnPoints = defaultDrawnPoints();
            updateShapeButtons();
            refreshPreview();
            if (callbacks.onChanged) callbacks.onChanged();
        };
    };
    addShapeBtn(sineBtn,     "Sine",     WaveLayer::Sine);
    addShapeBtn(sawBtn,      "Saw",      WaveLayer::Saw);
    addShapeBtn(squareBtn,   "Square",   WaveLayer::Square);
    addShapeBtn(triangleBtn, "Triangle", WaveLayer::Triangle);
    addShapeBtn(noiseBtn,    "Noise",    WaveLayer::Noise);
    addShapeBtn(drawnBtn,    "Draw",     WaveLayer::Drawn);
    addShapeBtn(formulaBtn,  "Formula",  WaveLayer::Formula);

    addAndMakeVisible(freehandToggle);
    freehandToggle.setButtonText("Points");
    freehandToggle.setTooltip("Toggle between Points mode (click to place control points with smooth interpolation) "
                              "and Freehand mode (click and drag to draw the waveform shape directly).");
    freehandToggle.onClick = [this]() {
        if (!layer) return;
        layer->freehandMode = !layer->freehandMode;
        freehandToggle.setButtonText(layer->freehandMode ? "Freehand" : "Points");
        // Seed freehand samples if switching to freehand for the first time.
        if (layer->freehandMode && layer->drawnSamples.empty())
            layer->drawnSamples = defaultFreehandSamples();
        refreshPreview();
        if (callbacks.onChanged) callbacks.onChanged();
    };
    freehandToggle.setVisible(false); // only visible when shape == Drawn

    // Formula expression editor - only visible when shape == Formula.
    // Uses the same WaveExprParser vocabulary as the freq-domain editor:
    // sin, cos, tan, exp, log, sqrt, pow, abs, tanh, clamp,
    // saw(x), square(x), triangle(x), noise(), random, pi, e, + - * / ^,
    // floor, ceil, min(a,b), max(a,b), if(c,a,b), and conditions
    // (<, >, <=, >=, ==, !=, &&, ||, !, ternary ?:).
    // `x` ranges over [0, 2*pi) for one cycle.
    addAndMakeVisible(formulaEditor);
    formulaEditor.setMultiLine(false);
    formulaEditor.setReturnKeyStartsNewLine(false);
    formulaEditor.setTooltip("Expression in `x` (radians, 0 to 2*pi over one cycle). "
                             "Vocabulary: sin, cos, tan, exp, log, sqrt, pow, abs, tanh, clamp, "
                             "floor, ceil, min, max, if(c,a,b), c?a:b, saw(x), square(x), triangle(x), "
                             "noise(), random, pi, e. Output is clamped to -1..1.");
    formulaEditor.setText("sin(x)", juce::dontSendNotification);
    formulaEditor.onTextChange = [this]() {
        if (!layer) return;
        layer->formulaExpr = formulaEditor.getText().toStdString();
        layer->rebakeFormula();      // refresh Lua/Python bake (no-op for Built-in)
        refreshPreview();
        if (callbacks.onChanged) callbacks.onChanged();
    };
    formulaEditor.setVisible(false);

    // Language selector for the Formula expression (Built-in / Lua / Python).
    // Only visible when shape == Formula. Lua/Python let the user use loops and
    // variables (e.g. summing many harmonics) and are baked into the cycle when
    // edited - they do not run on the audio thread.
    addChildComponent(formulaLangCombo);
    formulaLangCombo.addItem("Built-in", 1);
    formulaLangCombo.addItem("Lua",      2);
    formulaLangCombo.addItem("Python",   3);
    formulaLangCombo.setItemEnabled(2, shapeLangAvailable(ShapeLang::Lua));
    formulaLangCombo.setItemEnabled(3, shapeLangAvailable(ShapeLang::Python));
    formulaLangCombo.setSelectedId(1, juce::dontSendNotification);
    formulaLangCombo.setTooltip("Language for the Formula expression. Built-in: fast math "
                                "expressions. Lua / Python: full languages with loops and "
                                "variables (e.g. sum many harmonics for additive synthesis). "
                                "All are baked into the wavetable when you edit, not run live. "
                                "Write a value in `x` (radians, 0..2*pi); multi-line bodies "
                                "must end with `return`.");
    formulaLangCombo.onChange = [this]() {
        if (!layer) return;
        layer->formulaLang = (ShapeLang)(formulaLangCombo.getSelectedId() - 1);
        layer->rebakeFormula();
        refreshPreview();
        if (callbacks.onChanged) callbacks.onChanged();
    };

    auto setupSlider = [this](juce::Slider& sl, double lo, double hi, double step, const char* suffix) {
        addAndMakeVisible(sl);
        sl.setSliderStyle(juce::Slider::LinearHorizontal);
        sl.setTextBoxStyle(juce::Slider::TextBoxRight, false, 55, 18);
        sl.setRange(lo, hi, step);
        sl.setTextValueSuffix(suffix);
        sl.onValueChange = [this]() {
            if (!layer) return;
            layer->ratio = (int)ratioSlider.getValue();
            layer->phase = (float)phaseSlider.getValue();
            layer->amp   = (float)ampSlider.getValue();
            refreshPreview();
            if (callbacks.onChanged) callbacks.onChanged();
        };
    };
    setupSlider(ratioSlider, 1.0, 16.0, 1.0, "x");
    setupSlider(phaseSlider, 0.0, 1.0, 0.01, "");
    setupSlider(ampSlider,   0.0, 1.0, 0.01, "");
    ratioSlider.setTooltip("Harmonic ratio: how many times faster this layer cycles than the fundamental. "
                           "1 = root pitch, 2 = one octave up, 3 = one octave + a fifth, etc. Higher numbers add brighter overtones.");
    phaseSlider.setTooltip("Phase offset (0 to 1): shifts where in its cycle this layer starts. "
                           "Affects how layers add up when summed - different phases give different timbres.");
    ampSlider.setTooltip("Amplitude (0 to 1): how loud this layer is in the final sum. 0 = silent, 1 = full volume. "
                         "Use to balance layers against each other.");

    addAndMakeVisible(ratioLabel);
    addAndMakeVisible(phaseLabel);
    addAndMakeVisible(ampLabel);
    ratioLabel.setText("Harmonic",  juce::dontSendNotification);
    phaseLabel.setText("Phase",     juce::dontSendNotification);
    ampLabel  .setText("Amplitude", juce::dontSendNotification);
    for (auto* l : { &ratioLabel, &phaseLabel, &ampLabel }) {
        l->setFont(11.0f);
        l->setJustificationType(juce::Justification::centredLeft);
    }

    addAndMakeVisible(presetBtn);
    presetBtn.setButtonText("Preset");
    presetBtn.setTooltip("Pick a starting waveform. Replaces the layer's shape with a preset; "
                         "you can edit it further from there.");
    presetBtn.onClick = [this]() { showPresetMenu(); };

    addAndMakeVisible(deleteBtn);
    deleteBtn.setButtonText("X");
    deleteBtn.setTooltip("Remove this layer.");
    deleteBtn.onClick = [this]() {
        if (callbacks.onDelete) callbacks.onDelete();
    };
    // No delete callback -> single-layer editor, hide the X.
    deleteBtn.setVisible((bool) callbacks.onDelete);
}

void WaveLayerEditor::setLayerPtr(WaveLayer* p) {
    layer = p;
    syncFromModel();
}

void WaveLayerEditor::syncFromModel() {
    if (!layer) return;
    auto& l = *layer;
    if (callbacks.indexForLabel) {
        label.setText("Layer " + juce::String(callbacks.indexForLabel()),
                      juce::dontSendNotification);
    } else {
        label.setText("Layer", juce::dontSendNotification);
    }
    ratioSlider.setValue(l.ratio, juce::dontSendNotification);
    phaseSlider.setValue(l.phase, juce::dontSendNotification);
    ampSlider  .setValue(l.amp,   juce::dontSendNotification);
    updateShapeButtons();
    freehandToggle.setVisible(l.shape == WaveLayer::Drawn);
    freehandToggle.setButtonText(l.freehandMode ? "Freehand" : "Points");
    formulaEditor.setVisible(l.shape == WaveLayer::Formula);
    formulaLangCombo.setVisible(l.shape == WaveLayer::Formula);
    formulaLangCombo.setSelectedId((int)l.formulaLang + 1, juce::dontSendNotification);
    if (l.shape == WaveLayer::Formula) {
        formulaEditor.setText(l.formulaExpr, juce::dontSendNotification);
        l.rebakeFormula();
    }
    refreshPreview();
}

void WaveLayerEditor::refreshPreview() {
    if (!layer) return;
    renderSingleLayer(*layer, 512, previewSamples);
    repaint();
}

void WaveLayerEditor::updateShapeButtons() {
    if (!layer) return;
    auto& l = *layer;
    sineBtn    .setToggleState(l.shape == WaveLayer::Sine,     juce::dontSendNotification);
    sawBtn     .setToggleState(l.shape == WaveLayer::Saw,      juce::dontSendNotification);
    squareBtn  .setToggleState(l.shape == WaveLayer::Square,   juce::dontSendNotification);
    triangleBtn.setToggleState(l.shape == WaveLayer::Triangle, juce::dontSendNotification);
    noiseBtn   .setToggleState(l.shape == WaveLayer::Noise,    juce::dontSendNotification);
    drawnBtn   .setToggleState(l.shape == WaveLayer::Drawn,    juce::dontSendNotification);
    formulaBtn .setToggleState(l.shape == WaveLayer::Formula,  juce::dontSendNotification);
    freehandToggle.setVisible(l.shape == WaveLayer::Drawn);
    freehandToggle.setButtonText(l.freehandMode ? "Freehand" : "Points");
    formulaEditor.setVisible(l.shape == WaveLayer::Formula);
    formulaLangCombo.setVisible(l.shape == WaveLayer::Formula);
    formulaLangCombo.setSelectedId((int)l.formulaLang + 1, juce::dontSendNotification);
    if (l.shape == WaveLayer::Formula
        && formulaEditor.getText().toStdString() != l.formulaExpr)
    {
        formulaEditor.setText(l.formulaExpr, juce::dontSendNotification);
    }
}

void WaveLayerEditor::showPresetMenu() {
    juce::PopupMenu m;
    const auto& presets = wavePresets();
    for (int i = 0; i < (int)presets.size(); ++i)
        m.addItem(i + 1, presets[i].name);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&presetBtn),
        [this](int result) {
            if (result <= 0 || !layer) return;
            const auto& presets = wavePresets();
            int idx = result - 1;
            if (idx < 0 || idx >= (int)presets.size()) return;
            presets[idx].apply(*layer);
            syncFromModel();
            if (callbacks.onChanged) callbacks.onChanged();
        });
}

void WaveLayerEditor::resized() {
    auto a = getLocalBounds().reduced(4);
    auto top = a.removeFromTop(22);
    label.setBounds(top.removeFromLeft(70));
    if (deleteBtn.isVisible())
        deleteBtn.setBounds(top.removeFromRight(22));
    // Preset button sits to the left of the (optional) delete button.
    presetBtn.setBounds(top.removeFromRight(72));

    // Shape button row - 7 buttons (sine/saw/square/triangle/noise/draw/formula)
    auto btnRow = a.removeFromTop(24);
    int bw = btnRow.getWidth() / 7;
    sineBtn    .setBounds(btnRow.removeFromLeft(bw));
    sawBtn     .setBounds(btnRow.removeFromLeft(bw));
    squareBtn  .setBounds(btnRow.removeFromLeft(bw));
    triangleBtn.setBounds(btnRow.removeFromLeft(bw));
    noiseBtn   .setBounds(btnRow.removeFromLeft(bw));
    drawnBtn   .setBounds(btnRow.removeFromLeft(bw));
    formulaBtn .setBounds(btnRow);

    // Sub-row: Freehand/Points toggle (Drawn) or Formula text editor (Formula).
    // Always reserve the height so the slider rows below don't jump when
    // toggling shape.
    auto subRow = a.removeFromTop(24);
    if (freehandToggle.isVisible()) {
        freehandToggle.setBounds(subRow.removeFromLeft(100));
    } else if (formulaEditor.isVisible()) {
        formulaLangCombo.setBounds(subRow.removeFromLeft(84));
        subRow.removeFromLeft(4);
        formulaEditor.setBounds(subRow);
    }

    // Reserve space for the mini preview (bottom of row)
    a.removeFromBottom(previewHeight);

    // Slider rows
    auto sliderRow = [&](juce::Label& lab, juce::Slider& sl) {
        auto r = a.removeFromTop(20);
        lab.setBounds(r.removeFromLeft(70));
        sl.setBounds(r);
    };
    sliderRow(ratioLabel, ratioSlider);
    sliderRow(phaseLabel, phaseSlider);
    sliderRow(ampLabel,   ampSlider);
}

void WaveLayerEditor::paint(juce::Graphics& g) {
    g.setColour(juce::Colour(40, 40, 50));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
    g.setColour(juce::Colour(70, 70, 90));
    g.drawRoundedRectangle(getLocalBounds().toFloat(), 4.0f, 1.0f);

    // Mini waveform preview for just this layer's contribution
    auto bounds = getLocalBounds().reduced(6).toFloat();
    auto previewArea = bounds.removeFromBottom((float)previewHeight).reduced(2.0f);

    g.setColour(juce::Colour(24, 24, 30));
    g.fillRoundedRectangle(previewArea, 3.0f);

    // Center line
    float cy = previewArea.getCentreY();
    g.setColour(juce::Colours::grey.withAlpha(0.25f));
    g.drawHorizontalLine((int)cy, previewArea.getX(), previewArea.getRight());

    if (!previewSamples.empty()) {
        float cx = previewArea.getX() + 2;
        float w  = previewArea.getWidth() - 4;
        float h  = previewArea.getHeight() - 4;

        juce::Path p;
        int n = (int)previewSamples.size();
        for (int i = 0; i < n; ++i) {
            float x = cx + (float)i / (float)(n - 1) * w;
            float y = cy - previewSamples[i] * h * 0.45f;
            if (i == 0) p.startNewSubPath(x, y);
            else p.lineTo(x, y);
        }
        g.setColour(juce::Colour(150, 200, 255));
        g.strokePath(p, juce::PathStrokeType(1.3f));

        // For Drawn layers in Points mode, overlay the control points so
        // the user can see and grab them.
        if (layer && layer->shape == WaveLayer::Drawn && !layer->freehandMode) {
            for (int i = 0; i < (int)layer->drawnPoints.size(); ++i) {
                const auto& pt = layer->drawnPoints[i];
                // Scale y by amp because the preview renders amp*shape,
                // so the visible curve is also amp-scaled.
                float x = cx + pt.first * w;
                float y = cy - (pt.second * layer->amp) * h * 0.45f;
                bool isDragged = (i == draggingIdx);
                g.setColour(isDragged ? juce::Colours::yellow : juce::Colours::white);
                g.fillEllipse(x - 3.0f, y - 3.0f, 6.0f, 6.0f);
                g.setColour(juce::Colour(60, 90, 140));
                g.drawEllipse(x - 3.0f, y - 3.0f, 6.0f, 6.0f, 1.0f);
            }
        }
    }

    // Surface a Lua/Python bake error over the preview so a broken script
    // isn't just silently flat.
    if (layer && layer->shape == WaveLayer::Formula && !layer->formulaError.empty()) {
        g.setColour(juce::Colours::red.withAlpha(0.9f));
        g.setFont(11.0f);
        g.drawFittedText("Script error: " + juce::String(layer->formulaError),
                         previewArea.toNearestInt().reduced(4),
                         juce::Justification::topLeft, 3);
    }
}

juce::Rectangle<float> WaveLayerEditor::getPreviewAreaBounds() const {
    auto bounds = getLocalBounds().reduced(6).toFloat();
    return bounds.removeFromBottom((float)previewHeight).reduced(2.0f);
}

bool WaveLayerEditor::mouseToPointXY(juce::Point<float> p, float& outX, float& outY) const {
    auto area = getPreviewAreaBounds();
    if (!area.contains(p)) return false;
    outX = (p.x - area.getX()) / juce::jmax(1.0f, area.getWidth());
    outY = 1.0f - 2.0f * (p.y - area.getY()) / juce::jmax(1.0f, area.getHeight());
    outX = juce::jlimit(0.0f, 0.999f, outX);
    outY = juce::jlimit(-1.0f, 1.0f, outY);
    return true;
}

int WaveLayerEditor::findPointNear(float x, float y, float radius) const {
    if (!layer) return -1;
    auto& pts = layer->drawnPoints;
    int best = -1;
    float bestD2 = radius * radius;
    for (int i = 0; i < (int)pts.size(); ++i) {
        float dx = pts[i].first - x;
        float dy = (pts[i].second - y) * 0.5f; // compress y to match x scale
        float d2 = dx * dx + dy * dy;
        if (d2 < bestD2) { bestD2 = d2; best = i; }
    }
    return best;
}

void WaveLayerEditor::sortPointsByX() {
    if (!layer) return;
    auto& pts = layer->drawnPoints;
    std::sort(pts.begin(), pts.end(),
              [](const std::pair<float,float>& a, const std::pair<float,float>& b) {
                  return a.first < b.first;
              });
}

void WaveLayerEditor::writeFreehandSample(float x, float y) {
    if (!layer) return;
    auto& samples = layer->drawnSamples;
    if (samples.empty()) samples = defaultFreehandSamples();
    int n = (int)samples.size();
    int idx = juce::jlimit(0, n - 1, (int)(x * (float)n));
    if (lastFreehandIdx >= 0 && lastFreehandIdx != idx) {
        // Interpolate between last and current to avoid gaps.
        int from = lastFreehandIdx;
        int to = idx;
        float fromY = lastFreehandY;
        float toY = y;
        int steps = std::abs(to - from);
        int dir = (to > from) ? 1 : -1;
        for (int s = 0; s <= steps; ++s) {
            int si = from + s * dir;
            if (si < 0 || si >= n) continue;
            float t = (steps > 0) ? (float)s / (float)steps : 1.0f;
            samples[si] = fromY + (toY - fromY) * t;
        }
    } else {
        samples[idx] = y;
    }
    lastFreehandIdx = idx;
    lastFreehandY = y;
}

void WaveLayerEditor::mouseDown(const juce::MouseEvent& e) {
    if (!layer) return;
    auto& l = *layer;
    if (l.shape != WaveLayer::Drawn) return;
    float x, y;
    if (!mouseToPointXY(e.position, x, y)) return;

    if (l.freehandMode) {
        // Freehand: start drawing samples
        freehandDrawing = true;
        lastFreehandIdx = -1;
        writeFreehandSample(x, y);
        refreshPreview();
        if (callbacks.onChanged) callbacks.onChanged();
        return;
    }

    // Points mode (original behavior)
    auto& pts = l.drawnPoints;
    int hit = findPointNear(x, y);
    if (e.mods.isShiftDown() && hit >= 0) {
        // Shift-click a point to delete it (keep at least 2 points so
        // interpolation has something to work with).
        if ((int)pts.size() > 2) {
            pts.erase(pts.begin() + hit);
            draggingIdx = -1;
            refreshPreview();
            if (callbacks.onChanged) callbacks.onChanged();
        }
        return;
    }
    if (hit >= 0) {
        draggingIdx = hit;
    } else {
        // Add a new point at the cursor, then sort by x so interpolation stays valid.
        pts.emplace_back(x, y);
        sortPointsByX();
        // After sorting, re-find the point we just added so we can continue
        // dragging it.
        draggingIdx = -1;
        for (int i = 0; i < (int)pts.size(); ++i)
            if (std::abs(pts[i].first - x) < 1e-5f && std::abs(pts[i].second - y) < 1e-5f)
                { draggingIdx = i; break; }
        refreshPreview();
        if (callbacks.onChanged) callbacks.onChanged();
    }
}

void WaveLayerEditor::mouseDrag(const juce::MouseEvent& e) {
    if (!layer) return;
    auto& l = *layer;
    if (l.shape != WaveLayer::Drawn) return;

    if (l.freehandMode && freehandDrawing) {
        auto area = getPreviewAreaBounds();
        auto cp = e.position;
        cp.x = juce::jlimit(area.getX(), area.getRight() - 1.0f, cp.x);
        cp.y = juce::jlimit(area.getY(), area.getBottom(), cp.y);
        float x, y;
        mouseToPointXY(cp, x, y);
        writeFreehandSample(x, y);
        refreshPreview();
        if (callbacks.onChanged) callbacks.onChanged();
        return;
    }

    // Points mode
    if (draggingIdx < 0) return;
    auto& pts = l.drawnPoints;
    if (draggingIdx >= (int)pts.size()) { draggingIdx = -1; return; }
    float x, y;
    // Use clamped conversion so dragging outside the area still moves the point.
    auto area = getPreviewAreaBounds();
    auto cp = e.position;
    cp.x = juce::jlimit(area.getX(), area.getRight() - 1.0f, cp.x);
    cp.y = juce::jlimit(area.getY(), area.getBottom(), cp.y);
    mouseToPointXY(cp, x, y);
    pts[draggingIdx] = { x, y };
    // Re-sort after movement since x may have changed order.
    // Remember old position so we can re-find after sort.
    float ox = x, oy = y;
    sortPointsByX();
    draggingIdx = -1;
    for (int i = 0; i < (int)pts.size(); ++i)
        if (std::abs(pts[i].first - ox) < 1e-5f && std::abs(pts[i].second - oy) < 1e-5f)
            { draggingIdx = i; break; }
    refreshPreview();
    if (callbacks.onChanged) callbacks.onChanged();
}

void WaveLayerEditor::mouseUp(const juce::MouseEvent&) {
    draggingIdx = -1;
    freehandDrawing = false;
    lastFreehandIdx = -1;
}

// ==============================================================================
// ScatterView - N-D scatter wavetable viewport
// ==============================================================================
//
// Renders frames as labeled colored dots projected to 2D, with the current
// Position cursor as a crosshair. The user can:
//   - Click empty space to move the Position cursor (the playhead) there.
//   - Click a frame dot to select it (so its layers become editable below).
//   - Drag a frame dot to move it.
//   - Right-button drag to orbit the camera (yaw/pitch) in any 3D mode.
//   - Pick which axis pair (or triple, in 3D) to view via the parent's
//     projection combo.
//   - Pick a 3D viewing mode via the parent's stereo-mode combo:
//       * Off            - flat 2D projection.
//       * Anaglyph       - one composite image with red/cyan eye channels;
//                          requires red/cyan paper glasses.
//       * Cross-eyed     - side-by-side stereoscope; the viewer crosses
//                          their eyes so the LEFT half is seen by the RIGHT
//                          eye and vice versa.
//       * Parallel       - side-by-side stereoscope viewed "wall-eyed" -
//                          the LEFT half is seen by the LEFT eye and the
//                          RIGHT half by the RIGHT eye (the opposite of
//                          cross-eyed; this is how a Holmes stereoscope or
//                          a VR headset presents the pair).
class LayeredWaveEditorComponent::ScatterView : public juce::Component,
                                                public juce::SettableTooltipClient,
                                                public juce::DragAndDropTarget {
public:
    explicit ScatterView(LayeredWaveEditorComponent& o) : owner(o) {}

    enum class StereoMode { Off, Anaglyph, CrossEyed, Parallel };

    // Which axes are projected to screen X / Y / Z (Z only used in 3D modes).
    int axisX = 0, axisY = 1, axisZ = 2;
    StereoMode stereoMode = StereoMode::Off;

    // Wireframe corner-count cap. The hypercube grows as 2^N edges and
    // becomes visually meaningless past about 6 dims (64 corners, 192
    // edges) — taller wavetables still render and snap to the cube, the
    // wireframe just stops adding further axes.
    static constexpr int kMaxRenderDims = 6;

    // Paint-scoped view transform. Each paintScene() begins by calling
    // computeViewTransform(area), which projects every N-hypercube corner
    // through the current rotation, finds the bbox, and picks a scale +
    // centre so the projected hypercube fills `area` with a small margin
    // regardless of rotation. projectPoint() / screenToPosition() then
    // read these cached values instead of the old fixed-0.48 *min(W,H)
    // formula. Without this, a rotated cube's projection shrinks below
    // the available view (the old code computed a scale that fit only the
    // un-rotated [-0.5..+0.5] axis range), and edges of the wireframe
    // wouldn't extend to where dots near the corners actually drew.
    // Mutable: paint state, not user-visible state — modified from const
    // helpers like screenToPosition / projectPoint indirectly.
    mutable float renderScale = 1.0f;
    mutable float renderCx = 0.0f;
    mutable float renderCy = 0.0f;

    // Apply the owner's N-D rotation to a position vector. The owner
    // owns the rotation state (`scatterPlaneAngles`) so the embedded
    // view and the pop-out wavetable-view window stay in lock-step -
    // dragging the embedded view or moving a slider in the pop-out both
    // update the same vector, and both views repaint together.
    // Rotations are composed in lexicographic plane order (see
    // LayeredWaveEditorComponent::planePairs). Centred at 0.5 because
    // the unit cube lives in [0, 1]^N.
    std::vector<float> rotateNd(std::vector<float> p) const;

    // Any of the stereo modes count as "3D" for projection / pin / hit-test
    // purposes - they all need yaw/pitch rotation, the Z axis read, etc.
    bool is3D() const { return stereoMode != StereoMode::Off; }
    bool isSplitView() const {
        return stereoMode == StereoMode::CrossEyed
            || stereoMode == StereoMode::Parallel;
    }

    // Real-world stereo geometry. The parent editor pushes these in from
    // its sliders so the parallax matches the user's actual eyes / screen.
    float ipdMm = 63.0f;          // interpupillary distance
    float viewingDistMm = 600.0f; // distance from eyes to screen
    float sceneDepthMm  = 60.0f;  // physical depth of the unit cube
    float dpi = 96.0f;            // screen DPI (auto-detected)

    void setProjection(int ax, int ay, int az) { axisX = ax; axisY = ay; axisZ = az; repaint(); }

    // A monoscopic projection (one camera, no parallax) plus the
    // post-rotation depth so applyParallax can compute disparity.
    struct Projected { float x, y; float worldZ; };

    // Project an N-D position to a 2D screen point - without stereo offset.
    // In any 3D mode this includes yaw/pitch rotation and perspective scaling
    // (size only - both eyes get the same shape).
    Projected projectPoint(const std::vector<float>& pos,
                           const juce::Rectangle<float>& area) const
    {
        (void)area;  // transform is cached in renderCx/Cy/Scale - see
                     // computeViewTransform(); area is read there, not here.
        // Apply the full N-D rotation first, then pick the projected
        // axes out of the rotated vector. Plane angles drive everything
        // - the screen-X/Y axes pull from rotated[axisX]/rotated[axisY],
        // and the screen-Z (used for perspective + parallax in any 3D
        // mode) pulls from rotated[axisZ]. With all plane angles at zero
        // the rotation is identity and the projection matches the
        // original "axis pick" behaviour exactly.
        auto rotated = rotateNd(pos);
        int nd = (int)rotated.size();
        float px = (axisX >= 0 && axisX < nd) ? rotated[axisX] : 0.5f;
        float py = (axisY >= 0 && axisY < nd) ? rotated[axisY] : 0.5f;
        float pz = (is3D() && axisZ >= 0 && axisZ < nd) ? rotated[axisZ] : 0.5f;

        float x = px - 0.5f, y = py - 0.5f, z = pz - 0.5f;

        // No perspective size-scaling in 3D modes - earlier revisions
        // applied pers = fov / (fov - z) with fov = 1.6, which produced a
        // very pronounced near/far warp (~1.45x at the near corners, 0.76x
        // at the far ones). For a small abstract wireframe this read as
        // distorting the geometry rather than adding depth. Parallax (in
        // applyParallax) still uses the raw z to drive stereo disparity,
        // so stereo modes still fuse with depth - we just keep the
        // monoscopic projection orthographic.

        // Axonometric contribution from every dim that isn't one of the
        // explicitly projected ones (axisX, axisY, and in 3D modes axisZ).
        // Without this, a rotation in a plane whose BOTH axes are non-
        // projected (e.g. Z-W in Flat 2D view with N=4) is mathematically
        // invisible: rotateNd modifies rotated[Z] and rotated[W], neither
        // of which feeds the projection. The fix gives each non-projected
        // dim a small screen-space direction, so any change to its rotated
        // value visibly nudges the projection. Directions are spread around
        // a half-circle so successive non-projected dims pull in distinct
        // diagonal directions; weight is small (0.22) to keep the view
        // feeling 2D rather than pseudo-3D.
        addAxonometric(rotated, x, y);

        // Map the centred coordinate via the fixed scale transform set up
        // by computeViewTransform(). renderCx/Cy are just the area centre,
        // so this is a single multiply-add to scale + recentre.
        return { renderCx + x * renderScale, renderCy + y * renderScale, z };
    }

    // See projectPoint comment for the rationale. Pulled out so
    // computeViewTransform's worst-case extent calculation can share the
    // exact same transform math.
    void addAxonometric(const std::vector<float>& rotated,
                        float& x, float& y) const
    {
        const int nd = (int)rotated.size();
        int kth = 0;
        for (int d = 0; d < nd; ++d) {
            if (d == axisX) continue;
            if (d == axisY) continue;
            if (is3D() && d == axisZ) continue;
            // Spread non-projected dims around a half-circle, starting at
            // -60deg (upper-right) and stepping -45deg per dim. These are
            // never axis-aligned, so they never coincide with axisX/axisY
            // direction and the axonometric contribution always has a
            // distinct visual signature.
            const float base = -juce::MathConstants<float>::pi / 3.0f;
            const float step = -juce::MathConstants<float>::pi / 4.0f;
            float angle = base + (float)kth * step;
            const float w = 0.22f;
            float c = std::cos(angle), s = std::sin(angle);
            float r = rotated[(size_t)d] - 0.5f;
            x += w * r * c;
            y += w * r * s;
            ++kth;
        }
    }

    // Compute the paint-scoped view transform (renderScale, renderCx,
    // renderCy). Picks a single constant scale that guarantees no rotation
    // of the projected cube can clip a corner, and that this scale stays
    // the same as the user adds more axes (the cube doesn't shrink when
    // you go from 3D to 4D to 5D...).
    //
    // Why constant instead of per-rotation fit: the old fit-bbox approach
    // made the wireframe "breathe" as you rotated - a square rotated 45deg
    // has a smaller axis-aligned bbox than the un-rotated square, so scale
    // grew, then shrank again as you kept rotating. The visual result was
    // a wireframe that resized constantly. Locking the scale to a single
    // worst-case extent means the cube can't grow under any rotation; it
    // shrinks slightly into the visible area at non-worst-case angles but
    // never gets cropped.
    //
    // Worst-case extent math (the user's hint of "sqrt(2)" was the right
    // intuition - generalized here):
    //   For a unit D-cube centred at the origin with vertices at +/- 0.5
    //   in each of its D axes, projected to one screen axis via a unit
    //   projection vector v of length D, the projected coordinate of any
    //   vertex is sum_i v_i * (+/- 0.5). The max over vertices is
    //   0.5 * sum_i |v_i|. Subject to sum_i v_i^2 = 1 (unit vector), this
    //   sum is maximized when v_i = +/- 1/sqrt(D) for all i, giving
    //   0.5 * sqrt(D). Full span across both vertex signs = sqrt(D).
    //   So an axis-aligned unit cube edge of length 1 projects to at most
    //   sqrt(D) on the screen under any rotation. D=2 -> sqrt(2). D=3 ->
    //   sqrt(3). Both screen axes can hit this bound simultaneously (any
    //   two orthonormal projection vectors can each be close to the
    //   diagonal), so we have to fit min(W,H), not max(W,H).
    //
    //   Crucially, D = the number of dims that project directly to screen
    //   (2 in 2D view, 3 in 3D view) - NOT the total N. Extra axes beyond
    //   D get axonometric offset bars (small "skew" lines off each base-
    //   cube corner) rather than full-dim rotation. That's why adding a
    //   4th, 5th, ... axis no longer shrinks the base cube - the bound is
    //   sqrt(2) or sqrt(3), constant in N. The axonometric protrusions
    //   for extra dims can technically poke a hair past the bound; the
    //   3% margin below absorbs them in practice, and at high N the user
    //   accepts that the protrusions visualize the extra dims and aren't
    //   required to stay strictly inside the view.
    void computeViewTransform(juce::Rectangle<float> area) const {
        // Number of dims that project directly to screen X / Y (/ Z).
        const int projDims = 2 + (is3D() ? 1 : 0);

        // Worst-case projected span of the unit cube along either screen axis.
        // Independent of total N - this is what keeps the base-cube size
        // constant as the user adds axes.
        const float worst = std::sqrt((float)projDims);

        // Margin: leave ~3% on each side so dot outlines, cell-coord
        // labels printed below dots, and the small axonometric
        // protrusions from extra (non-projected) dims don't clip at the
        // edge of the view rectangle.
        constexpr float marginFactor = 0.94f;

        // Fit the SMALLER dimension so the worst-case rotation fits in
        // BOTH screen axes simultaneously - no clipping under any rotation.
        const float fit = std::min(area.getWidth(), area.getHeight()) * marginFactor;
        renderScale = fit / std::max(1e-3f, worst);
        renderCx = area.getCentreX();
        renderCy = area.getCentreY();
    }

    // Compute the stereoscopic disparity in screen pixels for a point at
    // post-rotation depth `worldZ` (in [-0.5..+0.5] world units).
    //
    // Geometry: place the screen plane at z=0, viewer's eyes at -D (mm in
    // front), eye separation = IPD. A point at depth z_mm behind the screen
    // appears at parallax (IPD/2) * z_mm/(D+z_mm) per eye (positive = right
    // eye image moves right, left eye image moves left -> uncrossed disparity
    // -> object fuses behind the screen). For a point in front (z_mm<0) the
    // formula naturally yields crossed disparity.
    juce::Point<float> applyParallax(const Projected& pp, int eyeSign) const {
        if (!is3D() || eyeSign == 0) return { pp.x, pp.y };
        float z_mm = pp.worldZ * sceneDepthMm; // post-rotation z mapped to physical mm
        float denom = viewingDistMm + z_mm;
        if (std::abs(denom) < 1e-3f) return { pp.x, pp.y };
        float disparity_mm = (ipdMm * 0.5f) * z_mm / denom;
        float disparity_px = disparity_mm * dpi / 25.4f;
        return { pp.x + (float)eyeSign * disparity_px, pp.y };
    }

    // Backward-compat wrapper used by drawSquare2D / drawCube3D etc.
    juce::Point<float> projectToScreen(const std::vector<float>& pos,
                                       const juce::Rectangle<float>& area) const
    {
        auto p = projectPoint(pos, area);
        return { p.x, p.y };
    }

    // In side-by-side stereoscope modes the viewport is split into two
    // half-rectangles. Pick which half a mouse position lives in (and
    // return the corresponding sub-rectangle). For Off / Anaglyph this
    // returns the full rectangle.
    juce::Rectangle<float> sceneRectForPoint(juce::Point<float> p,
                                             juce::Rectangle<float> full) const
    {
        if (!isSplitView()) return full;
        float halfW = full.getWidth() * 0.5f;
        if (p.x < full.getCentreX())
            return full.withWidth(halfW);
        return full.withTrimmedLeft(halfW);
    }

    // Resolve a dot's display colour. Colour lives on the library entry
    // (WaveformLibraryEntry::colorIdx), so every placement of the same
    // waveform paints the same colour - the library list row swatch, the
    // grid cell dot, and any scatter dot referencing the same entry all
    // agree. For the Auto fallback we use the entry's LIBRARY position
    // (not the cell / scatter index), which is exactly the fallback the
    // library list swatch uses too. If the cell references a missing /
    // empty library id, we still want a colour, so we fall back to the
    // caller's index in that case (one missing-id slot = one palette
    // colour). The `cellFallbackIdx` parameter is only used in that
    // empty-cell path.
    juce::Colour colorForCellByLibId(int libId, int cellFallbackIdx) const {
        const int libIdx = owner.wave.findLibraryIndexById(libId);
        const WaveformLibraryEntry* entry = (libIdx >= 0)
            ? &owner.wave.library[libIdx]
            : nullptr;
        // Use libIdx for the Auto-fallback so a single library entry has
        // ONE auto colour everywhere; cellFallbackIdx is only meaningful
        // when the cell points at no library entry (libIdx < 0).
        const int fb = (libIdx >= 0) ? libIdx : cellFallbackIdx;
        return libraryEntryDisplayColor(entry, fb);
    }

    void paint(juce::Graphics& g) override {
        auto full = getLocalBounds().toFloat().reduced(2.0f);
        g.setColour(juce::Colour(18, 18, 24));
        g.fillRoundedRectangle(full, 4.0f);
        g.setColour(juce::Colour(60, 60, 78));
        g.drawRoundedRectangle(full, 4.0f, 1.0f);

        // Inline legend painted once over the full viewport (not per-pane in
        // stereoscope mode - it'd just duplicate noise).
        drawLegend(g, full);

        if (isSplitView()) {
            // Two physical panes side-by-side. Conventions:
            //   Cross-eyed: viewer crosses eyes, so LEFT pane = RIGHT eye image.
            //   Parallel:   viewer's eyes stay parallel (wall-eyed / VR style),
            //               so LEFT pane = LEFT eye image.
            float halfW = full.getWidth() * 0.5f;
            auto leftPane  = full.withWidth(halfW);
            auto rightPane = full.withTrimmedLeft(halfW);
            // Subtle separator line so the eye knows where the split is.
            g.setColour(juce::Colour(40, 40, 56));
            g.drawLine(full.getCentreX(), full.getY() + 4.0f,
                       full.getCentreX(), full.getBottom() - 4.0f, 1.0f);
            int leftEyeSign  = (stereoMode == StereoMode::Parallel) ? -1 : +1;
            int rightEyeSign = -leftEyeSign;
            paintScene(g, leftPane,  leftEyeSign);
            paintScene(g, rightPane, rightEyeSign);
        } else {
            // Off (eyeSign 0) and Anaglyph (handled internally by paintScene)
            // both render once over the whole viewport.
            paintScene(g, full, 0);
        }

        // Drop / cell-drag indicators painted on top so they sit above
        // the dots and wireframe.
        drawDragOverlays(g, full);
    }

    // Highlight rings for in-progress drags. Two cases:
    //   * Grid cell-drag in progress: paint a green ring around the
    //     destination cell so the user sees where the swap will land.
    //   * Library drop in progress: paint a green ring at the drop
    //     target (a grid cell in Grid mode, the cursor position in
    //     Scatter mode).
    // Both use the same green for visual consistency - drop = drop,
    // regardless of source.
    void drawDragOverlays(juce::Graphics& g, juce::Rectangle<float> full) {
        auto ringAtScreen = [&g](juce::Point<float> p, float radius) {
            g.setColour(juce::Colour(0xff5be36e).withAlpha(0.95f));
            g.drawEllipse(p.x - radius, p.y - radius,
                          radius * 2.0f, radius * 2.0f, 2.0f);
            g.setColour(juce::Colour(0xff5be36e).withAlpha(0.18f));
            g.fillEllipse(p.x - radius, p.y - radius,
                          radius * 2.0f, radius * 2.0f);
        };

        auto ringAtCell = [&](int cellIdx) {
            if (cellIdx < 0 || cellIdx >= owner.wave.gridCellCount()) return;
            auto area = sceneRectForPoint(full.getCentre(), full);
            computeViewTransform(area);
            auto cellPos = owner.wave.cellCenterPosition(cellIdx);
            int needN = std::max(2, owner.wave.numDimensions());
            while ((int)cellPos.size() < needN) cellPos.push_back(0.5f);
            auto sp = projectToScreen(cellPos, area);
            ringAtScreen(sp, 12.0f);
        };

        // 1. Grid cell-drag preview (destination cell).
        if (dragCellSrcIdx >= 0 && dragCellDstIdx >= 0
            && dragCellDstIdx != dragCellSrcIdx
            && owner.wave.mode == WavetableMode::Grid) {
            ringAtCell(dragCellDstIdx);
        }

        // 2. Library-row drop hover. Gate on the parent DragAndDropContainer's
        // global isDragAndDropActive() so the hover ring can't survive past
        // the end of the drag that produced it. JUCE drops itemDragExit() in
        // a handful of edge cases (Escape-cancelled drag, drag released
        // outside the application window, source row destroyed mid-drag by
        // a refresh) - without this defensive gate, hoverDropActive sticks
        // at true and the green ring persists at hoverDropCellIdx until the
        // user starts and ends another drag through the same target. The
        // observable failure was a phantom green ring around an empty grid
        // cell that the user couldn't dismiss or click into.
        if (hoverDropActive) {
            auto* container =
                juce::DragAndDropContainer::findParentDragContainerFor(this);
            const bool dragReallyActive =
                container && container->isDragAndDropActive();
            if (!dragReallyActive) {
                // Mutate-from-paint is normally a smell, but this is a
                // pure cleanup of stale state we know is invalid - no
                // visible behaviour changes other than the ring vanishing.
                hoverDropActive = false;
                hoverDropCellIdx = -1;
            } else if (owner.wave.mode == WavetableMode::Grid) {
                ringAtCell(hoverDropCellIdx);
            } else {
                ringAtScreen(hoverDropScreenPt, 10.0f);
            }
        }

        // 3. Same defensive gate for the in-flight grid cell-drag (mouseDown
        // arms dragCellSrcIdx/dragCellDstIdx; mouseUp clears them). The
        // gate above already handled the DnD-based library drop; this
        // covers the same class of failure for the mouse-drag path. We
        // can't use isDragAndDropActive() here (this drag isn't a JUCE
        // DnD, it's a raw mouse drag), but we can clear if the LEFT mouse
        // button isn't currently held - if no button is down, no drag can
        // possibly be in progress.
        if (dragCellSrcIdx >= 0
            && !juce::ModifierKeys::currentModifiers.isLeftButtonDown()) {
            dragCellSrcIdx = -1;
            dragCellDstIdx = -1;
        }
    }

    // Render one stereoscopic eye-view (or the flat monoscopic / anaglyph
    // composite view) into a sub-rectangle of the viewport.
    //   eyeSign  -1 = left eye image, +1 = right eye image, 0 = no parallax.
    //   In Anaglyph mode the eyeSign argument is ignored and both red+cyan
    //   passes are drawn into the same `area`.
    void paintScene(juce::Graphics& g, juce::Rectangle<float> area, int eyeSign) {
        // Compute the fit-scale transform first so the wireframe + dots +
        // gridlines all share the same per-paint scale & centre. Must
        // happen before any projectPoint/projectToScreen call inside this
        // pane.
        computeViewTransform(area);

        // Frame / unit-hypercube wireframe. Draws every corner of the
        // wavetable's actual N-dim cube (capped at kMaxRenderDims) and
        // every 1-bit-differ edge. Unrotated, a high-N cube projects
        // many edges on top of each other in 2D so it looks like a flat
        // square — rotate via the orbit drag or sidebar sliders to see
        // the extra dims pull apart.
        drawHypercube(g, area, eyeSign);

        // Axis labels (top of pane)
        g.setColour(juce::Colours::grey);
        g.setFont(11.0f);
        auto axName = [](int i) {
            const char* n[] = { "X", "Y", "Z", "W", "V", "U", "T", "S" };
            return juce::String((i >= 0 && i < 8) ? n[i] : "?");
        };
        // Low-dimensional scatter views (line / drop-target) get a tailored
        // label rather than the generic "axes: X x Y" string, which would
        // misrepresent how many axes actually exist.
        const bool lowDimScatter =
            (owner.wave.mode == WavetableMode::Scatter
             && owner.wave.numDimensions() <= 1);
        if (lowDimScatter) {
            if (owner.wave.numDimensions() == 1)
                g.drawText("axis: " + axName(axisX),
                           area.reduced(6, 4).toNearestInt(),
                           juce::Justification::topLeft);
            // numDimensions()==0: no axis to label (drop-target view).
        } else if (is3D()) {
            g.drawText("axes: " + axName(axisX) + " x " + axName(axisY) + " x " + axName(axisZ),
                       area.reduced(6, 4).toNearestInt(), juce::Justification::topLeft);
            const char* modeTag = nullptr;
            switch (stereoMode) {
                case StereoMode::Anaglyph:  modeTag = "(red-cyan glasses)"; break;
                case StereoMode::CrossEyed: modeTag = (eyeSign > 0 ? "(left eye)" : "(right eye)"); break;
                case StereoMode::Parallel:  modeTag = (eyeSign < 0 ? "(left eye)" : "(right eye)"); break;
                default: break;
            }
            if (modeTag)
                g.drawText(modeTag, area.reduced(6, 4).toNearestInt(),
                           juce::Justification::topRight);
        } else {
            g.drawText("axes: " + axName(axisX) + " x " + axName(axisY),
                       area.reduced(6, 4).toNearestInt(), juce::Justification::topLeft);
        }

        // ---- Scatter drop-target placeholder (0 axes) --------------------------
        // With scatterDims == 0 there's no line or square to draw - just an
        // affordance telling the user they can drop a waveform here to begin.
        // Any dots that exist project to the centre (empty position vectors),
        // so they're still drawn by the scatter dot loop below; this just adds
        // the "drag here" cue around them.
        if (owner.wave.mode == WavetableMode::Scatter
            && owner.wave.numDimensions() <= 0) {
            auto c = area.getCentre();
            // A small dashed circle, not a big square - just a compact "drop a
            // waveform here" target at the centre of the view.
            const float ringR = 20.0f;
            const float dashes[] = { 5.0f, 4.0f };
            juce::Path ring, dashed;
            ring.addEllipse(c.x - ringR, c.y - ringR, ringR * 2.0f, ringR * 2.0f);
            juce::PathStrokeType(1.6f).createDashedStroke(dashed, ring, dashes, 2);
            g.setColour(juce::Colour(0xff5be36e).withAlpha(0.6f));
            g.fillPath(dashed);
            g.setColour(juce::Colours::white.withAlpha(0.55f));
            g.setFont(11.0f);
            g.drawText("Drag a waveform here",
                       juce::Rectangle<float>(c.x - 110.0f, c.y + ringR + 4.0f,
                                              220.0f, 14.0f).toNearestInt(),
                       juce::Justification::centred);
        }

        bool useAnaglyph = (stereoMode == StereoMode::Anaglyph);

        // ---- Grid mode rendering -------------------------------------------------
        // Show one dot per non-null cell at the cell center. Cell centers come
        // from WavetableDoc::cellCenterPosition, padded to the rendering axis
        // count so projectPoint can read axisX/axisY/axisZ out of it.
        if (owner.wave.mode == WavetableMode::Grid) {
            // View dimension for the grid = number of grid axes, floored at 1.
            // A 1-axis grid renders as a segmented line (cells = segments
            // divided by tick marks), 2 as a checkerboard, 3 as a cube, etc.
            // scatterDims is irrelevant in Grid mode.
            const int viewDims = std::max(1, (int)owner.wave.gridDims.size());

            // Faint cell-boundary grid: for each dim d with more than one
            // cell, draw every interior cell-boundary slab as the wireframe
            // of an (N-1)-cube fixed at coord = i/dims[d] along d.
            //
            // Why "(N-1)-cube wireframe" not "lines on the front+back face":
            //   The previous implementation drew gridlines parallel to axisY
            //   on the Z=0 / Z=1 / W=0 / W=1 faces. Those lines stay parallel
            //   to the projected Y axis no matter how the cube is rotated
            //   (because we never vary axisZ/axisW along them), so they look
            //   "stuck" - rotation around X-Z visibly tilts the cube but the
            //   gridlines remain vertical. They also bunch onto the two outer
            //   faces of the cube with empty space in between.
            //   Drawing the full perpendicular slab wireframe instead:
            //     - spreads gridlines across the interior of the projection
            //       (each slab cuts THROUGH the cube, not along its faces),
            //     - includes edges parallel to every non-d axis, so rotation
            //       around any plane visibly tilts at least one set of edges,
            //     - is geometrically what "cell boundary" actually means in
            //       N-D: a hyperplane perpendicular to d, drawn at the
            //       interior cell partitions.
            //
            // The `!is3D()` gate stays in place because in stereo modes the
            // per-slab wireframes on top of the cube wireframe get noisy;
            // mono users can still read cell structure via dots.
            if (!is3D() && (int)owner.wave.gridDims.size() >= 1) {
                g.setColour(juce::Colour(60, 60, 80).withAlpha(0.55f));

                for (int d = 0; d < viewDims; ++d) {
                    const int dd = (d < (int)owner.wave.gridDims.size())
                                   ? owner.wave.gridDims[d] : 1;
                    if (dd <= 1) continue;  // no interior boundaries

                    // The (N-1) dims perpendicular to d. The slab at d=t
                    // is the unit (N-1)-cube spanning these dims, with d
                    // fixed at t. Wireframe = corners 0..2^(N-1)-1, edges
                    // connecting corner-pairs that differ in exactly 1 bit.
                    std::vector<int> otherDims;
                    otherDims.reserve((size_t)viewDims - 1);
                    for (int o = 0; o < viewDims; ++o)
                        if (o != d) otherDims.push_back(o);
                    const int nOther = (int)otherDims.size();
                    const int nCorners = (nOther > 0) ? (1 << nOther) : 1;

                    auto cornerPos = [&](int cornerMask, float t) {
                        std::vector<float> p(viewDims, 0.0f);
                        p[(size_t)d] = t;
                        for (int k = 0; k < nOther; ++k)
                            p[(size_t)otherDims[(size_t)k]]
                                = ((cornerMask >> k) & 1) ? 1.0f : 0.0f;
                        return p;
                    };

                    for (int i = 1; i < dd; ++i) {
                        const float t = (float)i / (float)dd;
                        if (nOther == 0) {
                            // 1-D grid: the cell boundary is a single point on
                            // the line, not a slab. Draw a short perpendicular
                            // tick so the line reads as a row of discrete cells
                            // (the 1-D analog of the 2-D checkerboard's grid
                            // lines). The tick direction is derived from the
                            // projected line direction so it stays perpendicular
                            // under any view rotation.
                            auto e0 = projectToScreen(std::vector<float>{0.0f}, area);
                            auto e1 = projectToScreen(std::vector<float>{1.0f}, area);
                            const float dx = e1.x - e0.x, dy = e1.y - e0.y;
                            const float len = std::sqrt(dx * dx + dy * dy);
                            const float px = (len > 1e-3f) ? -dy / len : 0.0f;
                            const float py = (len > 1e-3f) ?  dx / len : 1.0f;
                            const float h = 9.0f;
                            auto bp = projectToScreen(std::vector<float>{t}, area);
                            g.drawLine(bp.x - px * h, bp.y - py * h,
                                       bp.x + px * h, bp.y + py * h, 1.0f);
                            continue;
                        }
                        // Iterate unique edges of the (N-1)-cube: for each
                        // corner c0 and each bit, the edge goes to c0 ^ (1<<bit).
                        // Only emit when c0 < c1 to avoid drawing each edge twice.
                        for (int c0 = 0; c0 < nCorners; ++c0) {
                            for (int bit = 0; bit < nOther; ++bit) {
                                const int c1 = c0 ^ (1 << bit);
                                if (c1 <= c0) continue;
                                auto p0 = projectToScreen(cornerPos(c0, t), area);
                                auto p1 = projectToScreen(cornerPos(c1, t), area);
                                g.drawLine(p0.x, p0.y, p1.x, p1.y, 0.8f);
                            }
                        }
                    }
                }
            }

            // Cell dots
            for (int idx = 0; idx < (int)owner.wave.cellWaveformIds.size(); ++idx) {
                if (owner.wave.cellWaveformIds[idx] < 0) continue;
                auto cellPos = owner.wave.cellCenterPosition(idx);
                if (cellPos.empty()) continue;
                while ((int)cellPos.size() < viewDims) cellPos.push_back(0.5f);

                bool sel = (idx == owner.currentFrameIdx);
                auto pp = projectPoint(cellPos, area);

                // All dots are the same size. The selected dot is marked by
                // an amber outer ring; unselected dots get only the white
                // outline. (Older builds enlarged the selected dot and drew
                // amber rings on EVERY cell that referenced the right-pane's
                // current library entry - the resulting "rings on all dots,
                // plus a bigger ring on the selected one" was read by users
                // as visual noise that obscured per-cell identity. The
                // sel-only ring is the simpler, requested behaviour.)
                const float rr = 5.0f;

                if (useAnaglyph) {
                    auto pL = applyParallax(pp, -1);
                    auto pR = applyParallax(pp, +1);
                    g.setColour(juce::Colour::fromRGBA(255, 0, 0, sel ? 230 : 180));
                    g.fillEllipse(pL.x - rr, pL.y - rr, 2*rr, 2*rr);
                    g.setColour(juce::Colour::fromRGBA(0, 255, 255, sel ? 230 : 180));
                    g.fillEllipse(pR.x - rr, pR.y - rr, 2*rr, 2*rr);
                } else {
                    auto p = applyParallax(pp, eyeSign);
                    juce::Colour base = colorForCellByLibId(
                        owner.wave.cellWaveformIds[idx], idx);
                    g.setColour(base.withAlpha(sel ? 1.0f : 0.85f));
                    g.fillEllipse(p.x - rr, p.y - rr, 2*rr, 2*rr);
                    g.setColour(juce::Colours::white.withAlpha(sel ? 1.0f : 0.5f));
                    g.drawEllipse(p.x - rr, p.y - rr, 2*rr, 2*rr, sel ? 1.6f : 1.0f);
                    if (sel) {
                        // Amber outer ring marks the currently-selected
                        // dot only.
                        g.setColour(juce::Colour(0xffffc34a).withAlpha(0.9f));
                        const float pad = 3.0f;
                        g.drawEllipse(p.x - rr - pad, p.y - rr - pad,
                                      2 * (rr + pad), 2 * (rr + pad),
                                      1.8f);
                    }

                    // Cell coord label e.g. "(0,1)"
                    auto coord = owner.wave.cellIdxToGridCoord(idx);
                    juce::String txt;
                    if (coord.size() == 1) {
                        txt = "[" + juce::String(coord[0]) + "]";
                    } else if (!coord.empty()) {
                        juce::String inner;
                        for (size_t d = 0; d < coord.size(); ++d) {
                            if (d) inner << ",";
                            inner << coord[d];
                        }
                        txt = "(" + inner + ")";
                    }
                    if (!txt.isEmpty()) {
                        g.setColour(juce::Colours::white.withAlpha(0.85f));
                        g.setFont(10.0f);
                        g.drawText(txt, (int)p.x - 50, (int)p.y + (int)rr + 1,
                                   100, 12, juce::Justification::centred);
                    }
                }
            }

            return;
        }

        // Draw frames
        auto& frames = owner.wave.scatterFrames;
        for (int i = 0; i < (int)frames.size(); ++i) {
            auto base = colorForCellByLibId(frames[i].waveformId, i);
            bool sel = (i == owner.currentFrameIdx);
            auto pp = projectPoint(frames[i].position, area);

            // All dots are the same size; only the selected dot gets the
            // amber ring. See the matching change in the Grid path above for
            // the rationale.
            const float r = 6.0f;

            if (useAnaglyph) {
                auto pL = applyParallax(pp, -1);
                auto pR = applyParallax(pp, +1);
                // Red channel for left eye
                g.setColour(juce::Colour::fromRGBA(255, 0, 0, sel ? 230 : 180));
                g.fillEllipse(pL.x - r, pL.y - r, 2*r, 2*r);
                // Cyan channel for right eye
                g.setColour(juce::Colour::fromRGBA(0, 255, 255, sel ? 230 : 180));
                g.fillEllipse(pR.x - r, pR.y - r, 2*r, 2*r);

                if (sel) {
                    g.setColour(juce::Colours::white.withAlpha(0.7f));
                    g.drawEllipse(pL.x - r - 2, pL.y - r - 2, 2*r + 4, 2*r + 4, 1.2f);
                    g.drawEllipse(pR.x - r - 2, pR.y - r - 2, 2*r + 4, 2*r + 4, 1.2f);
                }
                if (!frames[i].label.empty() || sel) {
                    g.setColour(juce::Colours::white.withAlpha(0.85f));
                    g.setFont(10.0f);
                    juce::String txt = frames[i].label.empty()
                                       ? juce::String("Waveform " + juce::String(i + 1))
                                       : juce::String(frames[i].label);
                    auto mid = (pL + pR) * 0.5f;
                    g.drawText(txt, (int)mid.x - 40, (int)mid.y + (int)r + 2,
                               80, 12, juce::Justification::centred);
                }
            } else {
                // Off (eyeSign==0 -> applyParallax no-ops), CrossEyed or
                // Parallel (eyeSign==-1 or +1 applies horizontal disparity).
                auto p = applyParallax(pp, eyeSign);
                g.setColour(base.withAlpha(sel ? 1.0f : 0.85f));
                g.fillEllipse(p.x - r, p.y - r, 2*r, 2*r);
                g.setColour(juce::Colours::white.withAlpha(sel ? 1.0f : 0.5f));
                g.drawEllipse(p.x - r, p.y - r, 2*r, 2*r, sel ? 1.6f : 1.0f);
                if (sel) {
                    g.setColour(juce::Colour(0xffffc34a).withAlpha(0.9f));
                    const float pad = 3.0f;
                    g.drawEllipse(p.x - r - pad, p.y - r - pad,
                                  2 * (r + pad), 2 * (r + pad),
                                  1.8f);
                }

                g.setColour(juce::Colours::white.withAlpha(0.9f));
                g.setFont(10.5f);
                juce::String txt = frames[i].label.empty()
                                   ? juce::String("Waveform " + juce::String(i + 1))
                                   : juce::String(frames[i].label);
                g.drawText(txt, (int)p.x - 50, (int)p.y + (int)r + 1,
                           100, 12, juce::Justification::centred);
            }
        }

    }

    // Inline legend explaining what frame dots are. Both rows draw the
    // exact same dot+ring composite the viewport draws, so users can
    // map "this is what I'm looking at" to a meaningful description
    // without guessing. (The Position-cursor crosshair entry was
    // removed when the cursor concept went away - the synth's playback
    // point is driven entirely by the node's Position pins / params at
    // runtime, so there's no in-editor playhead to label.)
    void drawLegend(juce::Graphics& g, juce::Rectangle<float> area) {
        auto legend = area.reduced(8.0f);
        float lx = legend.getRight() - 220.0f;
        float ly = legend.getBottom() - 14.0f;

        // Row 1: coloured dot = "waveform". We use the actual editor-target
        // dot colour (or a neutral grey when there's no target) so the
        // legend's swatch matches what the user sees in the viewport.
        // Drawing one specific palette colour would only correctly
        // represent one of the user's waveforms; an editor-target match
        // means the legend's "= waveform" sample literally IS one of the
        // dots they're looking at, which is the strongest possible cue.
        juce::Colour sampleDotColor = juce::Colours::grey;
        if (owner.currentLibraryId >= 0) {
            const int libIdx = owner.wave.findLibraryIndexById(owner.currentLibraryId);
            if (libIdx >= 0) {
                sampleDotColor = libraryEntryDisplayColor(
                    &owner.wave.library[(size_t)libIdx], libIdx);
            }
        }
        g.setColour(sampleDotColor.withAlpha(0.9f));
        g.fillEllipse(lx, ly, 8.0f, 8.0f);
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.drawEllipse(lx, ly, 8.0f, 8.0f, 1.0f);
        g.setColour(juce::Colours::white.withAlpha(0.75f));
        g.setFont(10.0f);
        g.drawText("= waveform (colour matches Library)",
                   (int)(lx + 14), (int)(ly - 2), 220, 12,
                   juce::Justification::left);

        // Row 2: coloured dot WITH amber ring around it = "shown in
        // editor". Draw the same colored dot from row 1, then surround it
        // with an amber ring - exactly the layered composite the viewport
        // draws around an editor-target dot. Empty-ring legend was
        // confusing because the actual rendering has a colored dot inside
        // the ring, so users would look at the colored dot and think
        // "that's what the legend's ring is showing" - now there's no
        // ambiguity.
        if (owner.currentLibraryId >= 0) {
            float ly2 = ly - 16.0f;
            // Inner dot (same colour as row 1).
            g.setColour(sampleDotColor.withAlpha(0.9f));
            g.fillEllipse(lx, ly2, 8.0f, 8.0f);
            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.drawEllipse(lx, ly2, 8.0f, 8.0f, 1.0f);
            // Amber outer ring.
            g.setColour(juce::Colour(0xffffc34a).withAlpha(0.9f));
            g.drawEllipse(lx - 3.0f, ly2 - 3.0f, 14.0f, 14.0f, 1.4f);
            g.setColour(juce::Colours::white.withAlpha(0.75f));
            g.drawText("= shown in editor (amber ring)",
                       (int)(lx + 14), (int)(ly2 - 2), 220, 12,
                       juce::Justification::left);
        }
    }

    static void drawCrosshair(juce::Graphics& g, juce::Point<float> p, float r) {
        g.drawEllipse(p.x - r, p.y - r, 2*r, 2*r, 1.5f);
        g.drawLine(p.x - r - 3, p.y, p.x + r + 3, p.y, 1.0f);
        g.drawLine(p.x, p.y - r - 3, p.x, p.y + r + 3, 1.0f);
    }

    // Unified N-cube wireframe. Replaces the old drawSquare2D (only ever
    // a 2D face) and drawCube3D (only ever a 3D face) - both ignored
    // wavetable dims past their own depth, so a 4D wavetable's dots
    // could project to spots well outside the rendered outline once any
    // higher-dim rotation kicked in. Now we enumerate every corner of
    // the actual N-dim cube (capped at kMaxRenderDims) and connect
    // 1-bit-differ pairs, so:
    //   * dots are always inside the rendered hull (they live in
    //     [0,1]^N and the hull is the projection of [0,1]^N);
    //   * the hull always reaches the available view bounds because
    //     computeViewTransform() picks the scale from the same projected
    //     corner set;
    //   * the user sees the actual dim count once they rotate a higher
    //     plane (un-rotated, dims > 2 collapse on top of each other,
    //     which is mathematically correct).
    void drawHypercube(juce::Graphics& g, juce::Rectangle<float> area, int eyeSign) {
        // N is the geometric view dimension, capped at kMaxRenderDims. N>=2
        // draws a square/cube wireframe; N==1 draws a single line (the [0,1]
        // extent along axisX); N==0 (scatter drop-target placeholder) has no
        // outline to draw - paintScene paints the drop affordance instead.
        int N = std::min(kMaxRenderDims, owner.wave.numDimensions());
        if (N < 1) return;
        int nVerts = 1 << N;
        std::vector<Projected> pp((size_t)nVerts);
        for (int c = 0; c < nVerts; ++c) {
            std::vector<float> v((size_t)N, 0.0f);
            for (int b = 0; b < N; ++b)
                v[(size_t)b] = ((c >> b) & 1) ? 1.0f : 0.0f;
            pp[(size_t)c] = projectPoint(v, area);
        }

        // For an N-cube, edges connect corners that differ in exactly
        // one bit. Powers-of-two are the bit-pattern test: diff != 0 and
        // diff & (diff - 1) == 0 means a single bit set.
        auto drawEdges = [&](int eye, juce::Colour col, float thickness) {
            g.setColour(col);
            for (int a = 0; a < nVerts; ++a) {
                for (int b = a + 1; b < nVerts; ++b) {
                    int diff = a ^ b;
                    if (diff != 0 && (diff & (diff - 1)) == 0) {
                        auto p0 = applyParallax(pp[(size_t)a], eye);
                        auto p1 = applyParallax(pp[(size_t)b], eye);
                        g.drawLine(p0.x, p0.y, p1.x, p1.y, thickness);
                    }
                }
            }
        };

        bool useAnaglyph = (stereoMode == StereoMode::Anaglyph);
        if (useAnaglyph) {
            drawEdges(-1, juce::Colour::fromRGBA(180,   0,   0, 110), 1.0f);
            drawEdges(+1, juce::Colour::fromRGBA(  0, 180, 180, 110), 1.0f);
        } else {
            // Slightly dim higher-dim wireframes so the edge density
            // doesn't fight the dots. 2D is brighter because there's
            // only 4 edges to see.
            juce::Colour col = (N <= 2) ? juce::Colour(80, 80, 100)
                                        : juce::Colour(70, 70, 90);
            drawEdges(eyeSign, col, 1.0f);
        }
    }

    // ---- Mouse interaction ----
    int dragFrameIdx = -1;
    bool dragCursor = false;
    bool dragOrbit = false;             // left- or right-button camera orbit
    juce::Point<float> orbitStart;       // mouse position when orbit drag began

    // Grid-mode "drag this cell to another cell" state. Set on mouseDown
    // when a populated cell is left-clicked AND the wavetable has 1 or 2
    // grid dims (higher dims have axes that the view doesn't show, so the
    // drag would be ambiguous). dragCellDstIdx is the closest cell under
    // the cursor right now; on mouseUp, if src != dst, the two cells'
    // library refs are swapped (matching the axis-stepper drag path used
    // by handleSelFrameGridChange). -1 means no drag active / no target.
    int dragCellSrcIdx = -1;
    int dragCellDstIdx = -1;

    // Library-drag-over state. While a Library row is being dragged over
    // the view, this is the index of the cell (Grid) or -2 (Scatter, drop
    // at cursor) that the drop will land on. Used to paint a hover ring
    // so the user sees where the drop will go. -1 = no drag active.
    int hoverDropCellIdx = -1;
    bool hoverDropActive = false;
    juce::Point<float> hoverDropScreenPt;

    // Snapshots of the (axisX,axisZ) and (axisY,axisZ) plane angles when
    // an orbit drag begins. Horizontal mouse delta adds onto the
    // (axisX, axisZ) plane angle (visually "yaw" - swings the scene left
    // and right around the vertical axis); vertical mouse delta adds onto
    // the (axisY, axisZ) plane angle ("pitch"). All other plane angles
    // are left untouched - they're only adjustable from the pop-out
    // wavetable view's slider sidebar.
    float orbitStartYaw = 0.0f, orbitStartPitch = 0.0f;

    int hitTestFrame(juce::Point<float> p, juce::Rectangle<float> area) const {
        if (owner.wave.mode != WavetableMode::Scatter) return -1;
        // Refresh the cached transform so projection in the hit-test matches
        // the latest paint (mouse events may arrive between paints or in a
        // different sub-pane than the last paint, e.g. cross-eyed view).
        computeViewTransform(area);
        const auto& frames = owner.wave.scatterFrames;
        int best = -1; float bestD2 = 14.0f * 14.0f;
        for (int i = 0; i < (int)frames.size(); ++i) {
            auto sp = projectToScreen(frames[i].position, area);
            float dx = sp.x - p.x, dy = sp.y - p.y;
            float d2 = dx*dx + dy*dy;
            if (d2 < bestD2) { bestD2 = d2; best = i; }
        }
        return best;
    }

    // Grid-mode hit test: walk every populated cell, project its centre
    // position into the view, return the cell index (flat row-major) of the
    // closest non-null cell within the click threshold, or -1.
    int hitTestGridCell(juce::Point<float> p, juce::Rectangle<float> area) const {
        if (owner.wave.mode != WavetableMode::Grid) return -1;
        // Same reason as hitTestFrame - keep the inverse projection in sync
        // with whatever was painted last.
        computeViewTransform(area);
        const int total = owner.wave.gridCellCount();
        if (total <= 0) return -1;
        int best = -1; float bestD2 = 14.0f * 14.0f;
        for (int i = 0; i < total; ++i) {
            if (i >= (int)owner.wave.cellWaveformIds.size()
                || owner.wave.cellWaveformIds[i] < 0)
                continue; // empty cell - nothing to click on
            auto cellPos = owner.wave.cellCenterPosition(i);
            int needN = std::max(2, owner.wave.numDimensions());
            while ((int)cellPos.size() < needN) cellPos.push_back(0.5f);
            auto sp = projectToScreen(cellPos, area);
            float dx = sp.x - p.x, dy = sp.y - p.y;
            float d2 = dx*dx + dy*dy;
            if (d2 < bestD2) { bestD2 = d2; best = i; }
        }
        return best;
    }

    // Like hitTestGridCell, but considers EMPTY cells too and has no
    // distance cap - returns the closest cell regardless of how far away
    // the cursor is. Used as a drop target locator (cell-drag swap, and
    // Library-row drop into an empty cell). Returns -1 only when there
    // are no cells at all.
    int nearestGridCellAny(juce::Point<float> p, juce::Rectangle<float> area) const {
        if (owner.wave.mode != WavetableMode::Grid) return -1;
        computeViewTransform(area);
        const int total = owner.wave.gridCellCount();
        if (total <= 0) return -1;
        int best = -1;
        float bestD2 = std::numeric_limits<float>::max();
        for (int i = 0; i < total; ++i) {
            auto cellPos = owner.wave.cellCenterPosition(i);
            int needN = std::max(2, owner.wave.numDimensions());
            while ((int)cellPos.size() < needN) cellPos.push_back(0.5f);
            auto sp = projectToScreen(cellPos, area);
            float dx = sp.x - p.x, dy = sp.y - p.y;
            float d2 = dx*dx + dy*dy;
            if (d2 < bestD2) { bestD2 = d2; best = i; }
        }
        return best;
    }

    // Inverse projection: convert a screen point back into the projected
    // axes (axisX, axisY) of the N-D position. Other axes are left at the
    // current value of `current`. Only meaningful in 2D mode (any 3D mode
    // can't be unprojected unambiguously, so dragging is locked to the 2D
    // projected axes there too - Z stays at the current cursor value).
    std::vector<float> screenToPosition(juce::Point<float> p,
                                        juce::Rectangle<float> area,
                                        const std::vector<float>& current) const
    {
        // Refresh the cached transform so the inverse map matches the
        // currently displayed projection. Without this, a mousedrag that
        // arrives between two paints (or in a different sub-pane than the
        // last paint) would use stale renderScale/centre and feel like
        // the dot snapped wrong.
        computeViewTransform(area);

        std::vector<float> out = current;
        // Use numDimensions() so the cursor position vector is sized for the
        // active mode's view dimensions (Grid: gridDims.size(); Scatter:
        // scatterDims) - otherwise dragging in Grid mode with more grid axes
        // than scatterDims would leave high axes truncated.
        while ((int)out.size() < owner.wave.numDimensions()) out.push_back(0.5f);
        if (renderScale < 1e-3f) return out;
        // Inverse of projectPoint's centre+scale (we ignore the perspective
        // term and the rotation when mapping back - dragging is locked to
        // the projected axisX/axisY only, which matches the old behaviour).
        float nx = (p.x - renderCx) / renderScale + 0.5f;
        float ny = (p.y - renderCy) / renderScale + 0.5f;
        nx = juce::jlimit(0.0f, 1.0f, nx);
        ny = juce::jlimit(0.0f, 1.0f, ny);
        if (axisX >= 0 && axisX < (int)out.size()) out[axisX] = nx;
        if (axisY >= 0 && axisY < (int)out.size()) out[axisY] = ny;
        return out;
    }

    // Begin an orbit drag from the given start point. Used by both the
    // right-button-on-empty path (legacy gesture) and the left-button-on-
    // empty path. Operates in both 2D and 3D views - in a flat 2D view
    // the rotation still tips the wavetable's hidden axes into the
    // projection, so the user sees dots move and can keep tipping until
    // axes that were edge-on come into view.
    void beginOrbit(juce::Point<float> startPos) {
        dragOrbit = true;
        orbitStart = startPos;
        orbitStartYaw   = owner.getScatterPlaneAngle(axisX, axisZ);
        orbitStartPitch = owner.getScatterPlaneAngle(axisY, axisZ);
    }

    void mouseDown(const juce::MouseEvent& e) override {
        // Right-button (or any mod-popup): if it lands on a dot, show that
        // frame's context menu (Remove from wavetable). On empty space it
        // starts a camera orbit. Used to be 3D-only, but the X-Z / Y-Z
        // rotation it drives is also meaningful in a "flat" 2D view (it
        // tips hidden dims into the projection), so 2D gets it too now.
        if (e.mods.isRightButtonDown() || e.mods.isPopupMenu()) {
            auto fullR = getLocalBounds().toFloat().reduced(2.0f);
            auto areaR = sceneRectForPoint(e.position, fullR);
            int hit = (owner.wave.mode == WavetableMode::Scatter)
                          ? hitTestFrame(e.position, areaR)
                          : hitTestGridCell(e.position, areaR);
            if (hit >= 0) {
                showFrameContextMenu(hit, e.getScreenPosition());
                return;
            }
            beginOrbit(e.position);
            return;
        }

        auto full = getLocalBounds().toFloat().reduced(2.0f);
        auto area = sceneRectForPoint(e.position, full);

        // Grid mode left-click:
        //  - On a populated cell: select it (switchToFrame); if the
        //    wavetable has 1 or 2 grid dims, ALSO arm a cell-drag so the
        //    user can drag the populated cell onto another cell and the
        //    two get swapped on release.
        //  - On empty cell / empty space: start an orbit drag instead of
        //    no-opping. This is what makes the view rotatable by dragging
        //    anywhere in 2D / 3D.
        if (owner.wave.mode != WavetableMode::Scatter) {
            int cell = hitTestGridCell(e.position, area);
            if (cell >= 0) {
                // User selected on the View surface: Delete removes only this
                // placement (clears the cell), not the library waveform.
                owner.activeSelectionSurface =
                    LayeredWaveEditorComponent::SelectionSurface::View;
                // Route through switchToFrame() so the editor target
                // (currentLibraryId) is synced when the clicked cell holds
                // a library entry, matching the Cells list behaviour.
                owner.switchToFrame(cell);
                const bool dimsOK = ((int)owner.wave.gridDims.size() <= 2);
                if (dimsOK
                    && owner.wave.libraryIdForCell(cell) >= 0) {
                    dragCellSrcIdx = cell;
                    dragCellDstIdx = cell;
                }
                repaint();
            } else {
                // Empty cell or background - start orbit.
                beginOrbit(e.position);
            }
            return;
        }

        int hit = hitTestFrame(e.position, area);

        if (e.mods.isShiftDown() && hit >= 0) {
            // Shift-click frame to delete (keep at least one). Routes through
            // the same removal helper as the right-click "Remove from
            // wavetable" menu so the pop-out's frames list / tabs refresh.
            removeFrameFromWavetable(hit);
            return;
        }

        if (hit >= 0) {
            // User selected on the View surface: Delete removes only this
            // scatter dot (the placement), not the library waveform.
            owner.activeSelectionSurface =
                LayeredWaveEditorComponent::SelectionSurface::View;
            // Select + (conditionally) start drag. Route through
            // switchToFrame() so the editor target (currentLibraryId)
            // follows the click to the scatter dot's library entry. Only
            // arm the drag-to-move path when the wavetable has 1 or 2
            // scatter dims - higher dim counts would only update axisX /
            // axisY anyway (the inverse projection is ambiguous), which
            // tends to feel like the dot snaps unexpectedly.
            owner.switchToFrame(hit);
            const bool dimsOK = (owner.wave.scatterDims <= 2);
            dragFrameIdx = dimsOK ? hit : -1;
            dragCursor = false;
            repaint();
            return;
        }

        // No frame was hit and we're in Scatter mode: start an orbit drag.
        // The old behaviour was to move a yellow Position cursor here, but
        // the cursor concept has been removed - playback position is driven
        // by the synth node's Position params (cables / sliders) instead.
        // Use + Waveform in the sidebar to add a new dot, or drag a Library
        // row onto the view to drop one at the cursor.
        dragCursor = false;
        dragFrameIdx = -1;
        beginOrbit(e.position);
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        // Helper: apply an orbit step from current mouse position. Shared
        // by Grid and Scatter so the rotation feel is identical.
        auto applyOrbit = [this, &e]() {
            float dx = e.position.x - orbitStart.x;
            float dy = e.position.y - orbitStart.y;
            float newYaw   = orbitStartYaw + dx * 0.5f;
            float newPitch = juce::jlimit(-85.0f, 85.0f,
                                           orbitStartPitch + dy * 0.5f);
            // Orbit speed: ~0.5 degree per pixel. The pitch axis is
            // clamped to +/-85 degrees to avoid the gimbal-flip jolt at
            // the poles - that clamp doesn't generalise to the full N-D
            // rotation set, so we only apply it to the (axisY, axisZ)
            // angle that the drag actually writes (other planes from
            // the slider sidebar are unrestricted).
            owner.setScatterPlaneAngle(axisX, axisZ, newYaw);
            owner.setScatterPlaneAngle(axisY, axisZ, newPitch);
            owner.notifyScatterViewRotated();
        };

        // Grid mode: orbit OR cell-drag. Both are mutually exclusive (set
        // up in mouseDown), so a single if/else chain is enough.
        if (owner.wave.mode != WavetableMode::Scatter) {
            if (dragOrbit) { applyOrbit(); return; }

            if (dragCellSrcIdx >= 0) {
                auto full = getLocalBounds().toFloat().reduced(2.0f);
                auto area = sceneRectForPoint(e.position, full);
                int dst = nearestGridCellAny(e.position, area);
                if (dst != dragCellDstIdx) {
                    dragCellDstIdx = dst;
                    repaint();
                }
            }
            return;
        }

        if (dragOrbit) { applyOrbit(); return; }

        auto full = getLocalBounds().toFloat().reduced(2.0f);
        auto area = sceneRectForPoint(e.position, full);
        if (dragFrameIdx >= 0 && dragFrameIdx < (int)owner.wave.scatterFrames.size()) {
            // mouseDown already gated this on scatterDims <= 2.
            auto& sf = owner.wave.scatterFrames[dragFrameIdx];
            sf.position = screenToPosition(e.position, area, sf.position);
            owner.notifyPopoutFrameOrPositionChanged();
            owner.onLayerChanged();
            repaint();
        }
    }

    void mouseUp(const juce::MouseEvent&) override {
        // Commit any pending Grid cell-swap drag. If the user dropped
        // back onto the source cell (or never moved off it), this is a
        // no-op - the click already selected the cell in mouseDown.
        if (dragCellSrcIdx >= 0 && dragCellDstIdx >= 0
            && dragCellSrcIdx != dragCellDstIdx) {
            commitGridCellDrag(dragCellSrcIdx, dragCellDstIdx);
        }
        // A just-finished scatter-dot drag may have crossed an axis-span
        // threshold: a dot that now differs along Y where it didn't before
        // makes the Y axis traversable (its Position knob/pin + placement
        // slider appear), and vice-versa. Re-sync the Position params and
        // rebuild the sidebar ONCE here at the gesture endpoint - never per
        // drag tick, because syncPositionParams mutates node.params that the
        // audio thread reads without a lock.
        const bool wasScatterDotDrag = (dragFrameIdx >= 0
                                        && owner.wave.mode == WavetableMode::Scatter);
        dragFrameIdx = -1;
        dragCursor = false;
        dragOrbit = false;
        dragCellSrcIdx = -1;
        dragCellDstIdx = -1;
        if (wasScatterDotDrag)
            owner.notifyPopoutDocMutated();
    }

    // Swap the library refs of two grid cells. Mirrors the swap done by
    // handleSelFrameGridChange (axis-stepper drag): src lib id moves to
    // dst, dst lib id moves to src (so a move-onto-empty leaves the
    // source empty, and a move-onto-populated displaces the existing dot
    // back to the source cell). Selection follows the user's intent and
    // stays with the moved waveform at dst.
    void commitGridCellDrag(int srcIdx, int dstIdx) {
        if (owner.wave.mode != WavetableMode::Grid) return;
        if (srcIdx < 0 || dstIdx < 0) return;
        if (srcIdx >= (int)owner.wave.cellWaveformIds.size()
            || dstIdx >= (int)owner.wave.cellWaveformIds.size()) return;
        std::swap(owner.wave.cellWaveformIds[(size_t)srcIdx],
                  owner.wave.cellWaveformIds[(size_t)dstIdx]);
        owner.currentFrameIdx = dstIdx;
        const int destLibId = owner.wave.libraryIdForCell(dstIdx);
        if (destLibId >= 0) owner.currentLibraryId = destLibId;
        owner.wave.scatterFromGridSnapshot.reset();
        owner.updateHintText();
        owner.rebuildRows();
        owner.onLayerChanged();
        owner.notifyPopoutDocMutated();
    }

    // ---- DragAndDropTarget: accept Library-row drops --------------------
    //
    // A Library row in the sidebar can be dragged onto the view and
    // released to place that library entry into the wavetable. In Grid
    // mode the drop assigns the entry to whichever cell is nearest the
    // cursor. In Scatter mode it creates a new ScatterFrame at the drop
    // position (using screenToPosition's inverse projection - axisX /
    // axisY get the cursor's coordinates, any extra dims default to 0.5).
    // Description format is "libdrag:<entryId>"; see LibraryDragButton
    // in WavetableViewWindowContent.
    static int parseLibraryDragId(const juce::var& description) {
        const juce::String s = description.toString();
        const juce::String prefix("libdrag:");
        if (!s.startsWith(prefix)) return -1;
        return s.substring(prefix.length()).getIntValue();
    }

    bool isInterestedInDragSource(const SourceDetails& d) override {
        return parseLibraryDragId(d.description) >= 0;
    }

    void itemDragEnter(const SourceDetails& d) override {
        if (parseLibraryDragId(d.description) < 0) return;
        hoverDropActive = true;
        itemDragMove(d);
    }

    void itemDragMove(const SourceDetails& d) override {
        if (parseLibraryDragId(d.description) < 0) return;
        hoverDropActive = true;
        hoverDropScreenPt = d.localPosition.toFloat();
        if (owner.wave.mode == WavetableMode::Grid) {
            auto full = getLocalBounds().toFloat().reduced(2.0f);
            auto area = sceneRectForPoint(hoverDropScreenPt, full);
            hoverDropCellIdx = nearestGridCellAny(hoverDropScreenPt, area);
        } else {
            hoverDropCellIdx = -1;
        }
        repaint();
    }

    void itemDragExit(const SourceDetails& d) override {
        hoverDropActive = false;
        hoverDropCellIdx = -1;
        repaint();
    }

    void itemDropped(const SourceDetails& d) override {
        const int entryId = parseLibraryDragId(d.description);
        // Snapshot the hover state BEFORE we reset it - itemDropped's
        // chosen cell needs to match the cell the green hover ring was
        // last indicating, not whatever nearestGridCellAny recomputes
        // from d.localPosition. Those can diverge by one cell when the
        // cursor drifts a few pixels between the last itemDragMove and
        // mouseUp - and the dot the user saw highlighted is the one
        // they expect to be replaced. Reading the cached value makes
        // the drop strictly WYSIWYG: the ring shows the target, the
        // drop lands there.
        const int prevHoverCell = hoverDropCellIdx;
        hoverDropActive = false;
        hoverDropCellIdx = -1;
        if (entryId < 0) { repaint(); return; }
        // Verify the entry still exists - drag-and-drop is async, so the
        // user could in principle have deleted the source row mid-drag.
        if (owner.wave.findLibraryIndexById(entryId) < 0) { repaint(); return; }

        auto full = getLocalBounds().toFloat().reduced(2.0f);
        const juce::Point<float> pos = d.localPosition.toFloat();
        auto area = sceneRectForPoint(pos, full);

        if (owner.wave.mode == WavetableMode::Grid) {
            // Prefer the ring cell. Fall back to recomputing from the
            // release position only when no hover state was recorded -
            // that path is taken when itemDropped fires without a
            // preceding itemDragMove (rare: a drag that enters and
            // releases on this target in a single OS event). Either
            // way we still guard against the no-cells case.
            int cell = (prevHoverCell >= 0)
                ? prevHoverCell
                : nearestGridCellAny(pos, area);
            if (cell < 0) { repaint(); return; }
            owner.wave.assignCellToLibrary(cell, entryId);
            owner.currentFrameIdx = cell;
            owner.currentLibraryId = entryId;
        } else {
            // Scatter: place a new dot at the drop position. screenToPosition
            // pads the position vector to wave.numDimensions() with 0.5 for
            // any dims the view doesn't show, which is what the user spec
            // calls "automatically set upon release" for >2D wavetables.
            std::vector<float> seedPos(owner.wave.numDimensions(), 0.5f);
            std::vector<float> newPos = screenToPosition(pos, area, seedPos);
            ScatterFrame sf;
            sf.waveformId = entryId;
            sf.position = std::move(newPos);
            owner.wave.scatterFrames.push_back(std::move(sf));
            owner.currentFrameIdx = (int)owner.wave.scatterFrames.size() - 1;
            owner.currentLibraryId = entryId;
        }
        owner.wave.scatterFromGridSnapshot.reset();
        owner.updateHintText();
        owner.rebuildRows();
        owner.onLayerChanged();
        owner.notifyPopoutDocMutated();
        repaint();
    }

    // Double-click a frame dot (Scatter) or grid cell (Grid) to jump
    // straight into the per-waveform editor for that frame. The wavetable
    // view is the default landing; double-click is the discoverable
    // shortcut for "edit this one" without going through the frame-tabs
    // row above. (Single click still selects + starts a drag in scatter
    // mode, so the existing arrange-by-dragging gesture isn't broken.)
    void mouseDoubleClick(const juce::MouseEvent& e) override {
        if (e.mods.isRightButtonDown() || e.mods.isPopupMenu()) return;
        if (e.mods.isShiftDown()) return; // shift-double-click is delete-ish; let single handle it
        auto full = getLocalBounds().toFloat().reduced(2.0f);
        auto area = sceneRectForPoint(e.position, full);
        int hit = (owner.wave.mode == WavetableMode::Scatter)
                      ? hitTestFrame(e.position, area)
                      : hitTestGridCell(e.position, area);
        if (hit < 0) return;
        // Side-by-side layout: the per-waveform editor is always visible
        // on the right. Double-click is just an alternative way to select
        // the cell (in case the single-click selection was less precise).
        owner.switchToFrame(hit);
    }

    // Right-click on a dot pops this up. Grid mode: "Remove from wavetable"
    // clears the cell (keeping at least one cell filled). Scatter mode: it
    // erases the scatter entry (keeping at least one frame). Either way the
    // pop-out window's frames list and frame tabs refresh through
    // notifyPopoutDocMutated.
    // Human-readable label for a frame typeId(). Centralised so the
    // identify-header in the right-click menu and any future status-bar
    // readout use the same strings.
    static juce::String prettyFrameType(const std::string& tid) {
        if (tid == "layered")  return "Layered waveform";
        if (tid == "spectral") return "Spectral";
        if (tid == "wavelet")  return "Wavelet";
        if (tid == "sample")   return "Sampled audio";
        if (tid == "granular") return "Granular (captured)";
        if (tid.empty())       return juce::String();
        return juce::String(tid);  // unknown future type - show the raw tag
    }

    void showFrameContextMenu(int frameIdx, juce::Point<int> screenPos) {
        // Identify the library entry behind this dot/cell. In Scatter mode
        // every dot has a library ref; in Grid mode the right-click is
        // gated on a populated cell (hitTestGridCell skips empties), so
        // libId should be >= 0 here in practice. The nullptr-entry branch
        // below is defensive.
        const int libId = owner.wave.libraryIdForCell(frameIdx);
        const int libIdx = (libId >= 0) ? owner.wave.findLibraryIndexById(libId) : -1;
        const WaveformLibraryEntry* entry =
            (libIdx >= 0 && libIdx < (int)owner.wave.library.size())
                ? &owner.wave.library[(size_t)libIdx] : nullptr;

        juce::String header;
        if (entry) {
            juce::String name = entry->name.empty()
                ? juce::String("Waveform ") + juce::String(libIdx + 1)
                : juce::String(entry->name);
            const std::string tid = entry->wave ? entry->wave->typeId()
                                                 : std::string("layered");
            juce::String typeLabel = prettyFrameType(tid);
            header = typeLabel.isNotEmpty()
                ? name + "  -  " + typeLabel
                : name;
        } else {
            header = "(empty cell)";
        }

        juce::PopupMenu m;
        // Identify header: disabled section header showing this dot's
        // library entry name and its frame type. The user asked for an
        // "identify which waveform it is" affordance; this is it. It's
        // not clickable on purpose - it's a label, not an action.
        m.addSectionHeader(header);
        m.addSeparator();
        m.addItem(2, "Edit waveform",      entry != nullptr);
        m.addItem(4, "Rename waveform...",  entry != nullptr);
        m.addItem(3, "Duplicate waveform", entry != nullptr);
        m.addSeparator();
        // Always enabled - the wavetable may be cleared completely; the
        // library entries persist and the user can rebuild via drag from
        // the Library list or "+ Waveform".
        m.addItem(1, "Remove from wavetable", true);

        // Capture indices by value so they survive the async menu.
        juce::Component::SafePointer<ScatterView> self(this);
        // withTargetComponent(this) anchored the menu to the entire
        // ScatterView, so it landed at the bottom of the whole pane
        // instead of next to the clicked dot. Use a 1x1 target screen
        // rect at the actual cursor position so the menu pops up where
        // the user clicked.
        const juce::Rectangle<int> targetArea(screenPos, screenPos + juce::Point<int>(1, 1));
        m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(targetArea),
            [self, frameIdx, libId](int r) {
                if (!self) return;
                switch (r) {
                    case 1: self->removeFrameFromWavetable(frameIdx); break;
                    case 2:
                        if (libId >= 0) self->owner.setEditingLibraryEntry(libId);
                        break;
                    case 3:
                        if (libId >= 0) self->duplicateLibraryEntryFromCell(libId);
                        break;
                    case 4:
                        if (libId >= 0) self->owner.renameLibraryEntry(libId);
                        break;
                    default: break;
                }
            });
    }

    // "Duplicate waveform" from the dot context menu. Clones the library
    // entry the dot points at into a brand-new library entry named
    // "<name> (copy)" and focuses the editor on it. Like every other
    // creation path, the clone is added to the Library ONLY - it is NOT
    // placed beside the source dot, so duplicating never silently changes
    // the arrangement (no new dot, no grown grid). The user places it
    // explicitly via the Library list's "Add to grid" / drag-to-cell.
    // (Earlier builds dropped a placed copy next to the source; that was
    // removed for consistency with capture and the + Waveform menu.)
    void duplicateLibraryEntryFromCell(int srcLibId) {
        const int srcLibIdx = owner.wave.findLibraryIndexById(srcLibId);
        if (srcLibIdx < 0) return;
        auto* srcWavePtr = owner.wave.library[(size_t)srcLibIdx].wave.get();
        if (!srcWavePtr) return;

        std::string srcName = owner.wave.library[(size_t)srcLibIdx].name;
        if (srcName.empty())
            srcName = "Waveform " + std::to_string(srcLibIdx + 1);
        const std::string newName = srcName + " (copy)";

        const int newId = owner.wave.addLibraryEntry(srcWavePtr->clone(), newName);
        if (newId < 0) return;

        owner.currentLibraryId = newId;
        owner.wave.scatterFromGridSnapshot.reset();
        owner.updateHintText();
        owner.rebuildRows();
        owner.onLayerChanged();
        owner.notifyPopoutDocMutated();
        repaint();
    }

    void removeFrameFromWavetable(int frameIdx) {
        // The wavetable is allowed to go fully empty - the user may want
        // to clear it out and rebuild from a clean slate via drag-and-drop
        // from the Library or "+ Waveform". The synth handles an empty
        // wavetable as silence; library entries persist independently of
        // placements, so the user hasn't lost anything by clearing. The
        // old "keep at least one" gate left users stuck unable to remove
        // the last dot, which violated the symmetry users expect after
        // we let them remove the second-to-last dot.
        if (owner.wave.mode == WavetableMode::Scatter) {
            if (frameIdx < 0 || frameIdx >= (int)owner.wave.scatterFrames.size()) return;
            owner.wave.scatterFrames.erase(owner.wave.scatterFrames.begin() + frameIdx);
            // currentFrameIdx may now be out of range; -1 means "no
            // current frame" and the right-pane / preview paths all
            // handle that as a no-op.
            if (owner.currentFrameIdx >= (int)owner.wave.scatterFrames.size())
                owner.currentFrameIdx = (int)owner.wave.scatterFrames.size() - 1;
        } else {
            if (frameIdx < 0 || frameIdx >= (int)owner.wave.cellWaveformIds.size()) return;
            // Clear the cell reference; the library entry itself stays so the
            // user can re-place the waveform later via the library sidebar.
            // The selection deliberately stays on this (now empty) cell so
            // the user can immediately Assign a different library entry to
            // the same slot - matches the X button in the Cells list.
            owner.wave.cellWaveformIds[frameIdx] = -1;
        }
        owner.wave.scatterFromGridSnapshot.reset();
        owner.updateHintText();
        owner.rebuildRows();
        owner.onLayerChanged();
        owner.notifyPopoutDocMutated();
        repaint();
    }

    // Show a "grab" hand cursor when hovering over a frame dot (a discrete
    // selectable / draggable target). Empty space and the Position crosshair
    // both behave the same on click (move Position) - no per-pixel cursor
    // hint is needed there since the click-anywhere semantic is consistent.
    juce::MouseCursor getMouseCursor() override {
        if (owner.wave.mode != WavetableMode::Scatter)
            return juce::MouseCursor::NormalCursor;
        auto p = getMouseXYRelative().toFloat();
        auto full = getLocalBounds().toFloat().reduced(2.0f);
        auto area = sceneRectForPoint(p, full);
        if (hitTestFrame(p, area) >= 0) return juce::MouseCursor::PointingHandCursor;
        return juce::MouseCursor::NormalCursor;
    }

private:
    LayeredWaveEditorComponent& owner;
};

// Apply the owner's N-D plane-rotation stack to a position vector. The
// rotations are composed in lexicographic plane order: (0,1), (0,2), ...,
// (0,N-1), (1,2), ... With every plane angle at zero this is the identity,
// so the projection collapses to the original axis-pick behaviour. The
// angle for plane (i, j) rotates the components p[i] and p[j] of the
// centred vector around the origin (centre = 0.5 because the unit cube
// lives in [0, 1]^N).
std::vector<float> LayeredWaveEditorComponent::ScatterView::rotateNd(std::vector<float> p) const {
    int N = (int)p.size();
    if (N < 2) return p;
    auto pairs = LayeredWaveEditorComponent::scatterPlanePairs(N);
    for (const auto& ij : pairs) {
        int i = ij.first, j = ij.second;
        if (i < 0 || j < 0 || i >= N || j >= N) continue;
        float deg = owner.getScatterPlaneAngle(i, j);
        if (deg == 0.0f) continue;
        float rad = deg * (float)M_PI / 180.0f;
        float c = std::cos(rad), s = std::sin(rad);
        float a = p[i] - 0.5f;
        float b = p[j] - 0.5f;
        p[i] = 0.5f + c * a - s * b;
        p[j] = 0.5f + s * a + c * b;
    }
    return p;
}

// =========================================================================
// Pop-out wavetable view window. Hosts a second ScatterView that shares the
// owner's rotation/position state, plus one slider per N-D plane (i, j) so
// the user can rotate the view in planes the mouse drag doesn't cover. The
// embedded ScatterView in the main editor and the pop-out view are kept in
// lock-step by routing all rotation writes through
// LayeredWaveEditorComponent::setScatterPlaneAngle, and by calling
// notifyScatterViewRotated() from the embedded view's drag handler so the
// sliders here track the live drag.
// =========================================================================

class LayeredWaveEditorComponent::WavetableViewWindowContent
    : public juce::Component
{
public:
    // A Library row that doubles as a drag source: the user can grab the
    // row and drop it onto the arrangement view to place that waveform
    // into a cell (Grid) or as a new dot (Scatter). The button's onClick
    // (set in rebuildLibraryList) keeps working for plain clicks because
    // JUCE only routes mouseUp to the button when no drag was started.
    class LibraryDragButton : public juce::TextButton {
    public:
        explicit LibraryDragButton(int libId_) : libId(libId_) {}
        // Right-click on the row pops the per-waveform context menu (Add to
        // grid / Duplicate / Delete). Set in rebuildLibraryList. We intercept
        // mouseDown for the popup-menu case so the button never toggles its
        // selection state on a right-click.
        std::function<void(juce::Point<int>)> onRightClick;
        void mouseDown(const juce::MouseEvent& e) override {
            if (e.mods.isPopupMenu()) {
                if (onRightClick) onRightClick(e.getScreenPosition());
                return;
            }
            juce::TextButton::mouseDown(e);
        }
        void mouseDrag(const juce::MouseEvent& e) override {
            // Only kick off a drag once the user has moved a few pixels;
            // a tiny jitter on click shouldn't suddenly become a drop.
            if (e.getDistanceFromDragStart() < 5) {
                juce::TextButton::mouseDrag(e);
                return;
            }
            auto* dnd = juce::DragAndDropContainer::findParentDragContainerFor(this);
            if (dnd && !dnd->isDragAndDropActive()) {
                juce::var desc(juce::String("libdrag:") + juce::String(libId));
                dnd->startDragging(desc, this);
                // Remember we just kicked off a drag so the matching mouseUp
                // can suppress the button's click handler (see mouseUp below
                // for the full rationale).
                dragStartedThisInteraction = true;
            }
        }
        void mouseUp(const juce::MouseEvent& e) override {
            if (dragStartedThisInteraction) {
                // Suppress the click. Rationale: TextButton's mouseUp will
                // fire clicked()/onClick when the cursor is still considered
                // "over" the button - which it can be when JUCE's startDragging
                // path didn't generate the corresponding mouseExit (the cursor
                // is now over the drop target far away, but the button's
                // internal isOver state didn't get the memo because we
                // stopped delegating mouseDrag to the base class once we hit
                // the 5px threshold).
                //
                // Our onClick handler calls rebuildLibraryList(), which
                // deletes every LibraryDragButton including `this`. That
                // races with JUCE's DragImageComponent::mouseUp - the drag
                // image holds the source via a Component::SafePointer, and
                // if the button is deleted before the drag image processes
                // mouseUp, the drag image early-returns and itemDropped is
                // NEVER delivered to ScatterView. That is the intermittent
                // failure the diagnostic overlay surfaced: on a successful
                // drop the drag image processes mouseUp first (drop fires,
                // then click destroys the button), on a failed drop the
                // button's mouseUp processes first (click destroys the
                // button, then the drag image bails out silently).
                //
                // Skipping the base call here removes the race entirely.
                // The button will be destroyed by the drop's side effects
                // (rebuildLibraryList in itemDropped) on success, or stay
                // alive until the user does something else on a missed
                // drop - either way, click no longer fires from a drag.
                dragStartedThisInteraction = false;
                return;
            }
            juce::TextButton::mouseUp(e);
        }
    private:
        int libId;
        bool dragStartedThisInteraction = false;
    };

    explicit WavetableViewWindowContent(LayeredWaveEditorComponent& o)
        : owner(o)
    {
        // Accept keyboard focus so Del/Backspace can delete the highlighted
        // library entry (see keyPressed). The popup window grabs focus when
        // it opens; clicking anywhere in this content keeps it here.
        setWantsKeyboardFocus(true);

        view = std::make_unique<ScatterView>(owner);
        addAndMakeVisible(view.get());
        view->setTooltip(
            "Wavetable arrangement view. Click empty space to move the Position cursor; "
            "click a waveform dot to select it; Shift+click a dot to delete it. "
            "In any 3D viewing mode, right-drag orbits the camera around the (X,Z) "
            "and (Y,Z) planes - other plane angles are adjustable from the rotation "
            "sliders on the right.");

        // ---- View / stereo mode (top of sidebar) ----
        addAndMakeVisible(stereoModeCombo);
        stereoModeCombo.addItem("View: Flat 2D",                          1);
        stereoModeCombo.addItem("View: 3D Anaglyph (red-cyan glasses)",   2);
        stereoModeCombo.addItem("View: 3D Cross-eyed stereoscope",        3);
        stereoModeCombo.addItem("View: 3D Parallel stereoscope",          4);
        stereoModeCombo.setSelectedId(1, juce::dontSendNotification);
        stereoModeCombo.setTooltip(
            "Switch between 2D and 3D viewing modes for this pop-out window.");
        stereoModeCombo.onChange = [this]() {
            int id = stereoModeCombo.getSelectedId();
            ScatterView::StereoMode m = ScatterView::StereoMode::Off;
            if (id == 2) m = ScatterView::StereoMode::Anaglyph;
            if (id == 3) m = ScatterView::StereoMode::CrossEyed;
            if (id == 4) m = ScatterView::StereoMode::Parallel;
            view->stereoMode = m;
            view->repaint();
        };

        // ---- Library list (scrolling) ----
        // Lists every waveform that exists in this wavetable's library,
        // whether or not it's currently placed in a cell. This is where
        // the user creates / renames / deletes the actual waveform data;
        // placement on the arrangement view is a separate concern (Cells
        // list below).
        librarySectionLabel.setText("Library", juce::dontSendNotification);
        librarySectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        librarySectionLabel.setJustificationType(juce::Justification::centredLeft);
        librarySectionLabel.setTooltip(
            "All waveforms in this wavetable's library. Cells in the arrangement view "
            "reference these by id. Deleting a cell does NOT delete its library entry - "
            "use the X button here to remove an entry from the library (which clears any "
            "cells that referenced it).");
        addAndMakeVisible(librarySectionLabel);

        addAndMakeVisible(libraryListViewport);
        libraryListViewport.setViewedComponent(&libraryListContainer, false);
        libraryListViewport.setScrollBarsShown(true, false);

        addAndMakeVisible(addLibraryEntryBtn);
        addLibraryEntryBtn.setTooltip(
            "Add a new waveform to the wavetable. Click to choose the waveform type: "
            "Layered (time-domain layers), Frequency Domain (FFT magnitude+phase), "
            "Wavelet Space (DWT coefficient grid), or capture from audio (project song, "
            "microphone, or audio file). If a waveform is currently selected you can "
            "also duplicate it. The synth crossfades between waveforms as you sweep the "
            "Position parameter on the synth node in the graph, letting you morph "
            "between different waveform shapes during playback.");
        addLibraryEntryBtn.onClick = [this]() {
            // Defer to the editor's central add-waveform flow so this sidebar
            // button offers exactly the same 6+1 choices (Layered / Spectral /
            // Wavelet / capture from project / capture from mic / capture from
            // file, plus Duplicate when something is selected) as the editor
            // used to expose via its top-bar "+ Waveform" button. That top-bar
            // button has been removed - this is the single entry point now.
            owner.showAddWaveformMenu(&addLibraryEntryBtn);
        };

        addAndMakeVisible(assignToCellBtn);
        assignToCellBtn.setTooltip(
            "Place the waveform currently being edited (the highlighted row in the "
            "Library list) into the cell currently selected in the arrangement view "
            "(replaces whatever was there). Greyed out when no waveform is being "
            "edited or no cell is selected.");
        assignToCellBtn.onClick = [this]() {
            const int libId = owner.currentLibraryId;
            if (libId < 0) return;
            owner.wave.assignCellToLibrary(owner.currentFrameIdx, libId);
            owner.wave.scatterFromGridSnapshot.reset();
            owner.updateHintText();
            owner.rebuildRows();
            owner.onLayerChanged();
            refreshAfterDocMutation();
        };

        // ---- Cells list (scrolling) ----
        // Shows occupied placements. Click selects the cell; X clears it.
        // Library entries survive cell deletion.
        framesListLabel.setText("Cells", juce::dontSendNotification);
        framesListLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        framesListLabel.setJustificationType(juce::Justification::centredLeft);
        framesListLabel.setTooltip(
            "Cells in the arrangement view. In Grid mode each row shows the cell's "
            "coordinate plus the name of the waveform it holds (or \"- empty -\"); in "
            "Scatter mode each row shows its waveform name. Click to select the cell; "
            "X clears the cell (library entry survives).");
        addAndMakeVisible(framesListLabel);

        addAndMakeVisible(framesListViewport);
        framesListViewport.setViewedComponent(&framesListContainer, false);
        framesListViewport.setScrollBarsShown(true, false);

        // ---- Convert button (mode-dependent) ----
        addAndMakeVisible(convertBtn);
        convertBtn.onClick = [this]() { onConvertClicked(); };

        // ---- Scrollable settings container ----
        // Everything below has to be parented to settingsContainer (not
        // `this`) so it scrolls with the viewport when content overflows.
        addAndMakeVisible(settingsViewport);
        settingsViewport.setViewedComponent(&settingsContainer, false);
        settingsViewport.setScrollBarsShown(true, false);

        // ---- Grid axes section (Grid mode only) ----
        gridAxesLabel.setText("Grid axes (cells per axis):", juce::dontSendNotification);
        gridAxesLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        gridAxesLabel.setJustificationType(juce::Justification::centredLeft);
        settingsContainer.addAndMakeVisible(gridAxesLabel);

        settingsContainer.addAndMakeVisible(addAxisBtn);
        addAxisBtn.setTooltip("Add a new axis (dimension). Each *traversable* axis is "
                              "exposed to the synth as one Position knob. Max 8 axes. In "
                              "Grid mode the new axis starts with one cell - so it has "
                              "nothing to morph through yet and gets no Position knob "
                              "until you grow it to 2+ cells. In Scatter mode every "
                              "existing dot gets its new coordinate defaulted to the "
                              "midpoint (0.5).");
        addAxisBtn.onClick = [this]() {
            if (owner.wave.mode == WavetableMode::Grid) {
                if ((int)owner.wave.gridDims.size() >= 8) return;
                owner.wave.gridDims.push_back(1);
                // Adding a 1-size axis keeps the frame count constant (every
                // existing cell stays at coord[newAxis]=0). Reset any stale
                // revert snapshot; the topology changed.
                owner.wave.scatterFromGridSnapshot.reset();
                owner.wave.debugDumpState("after grid Add Axis");
            } else {
                if (owner.wave.scatterDims >= 8) return;
                ++owner.wave.scatterDims;
                // Pad every existing dot's position vector with a midpoint
                // value on the new axis. 0.5 keeps the dot visually centred
                // on the new dim so the user has something to drag, and
                // matches the load-path padding (lines ~1301 / ~1389 /
                // ~1478 / ~1555). The user can then move dots along the
                // new axis with the selected-frame slider strip.
                for (auto& sf : owner.wave.scatterFrames) {
                    while ((int)sf.position.size() < owner.wave.scatterDims)
                        sf.position.push_back(0.5f);
                }
                // Topology changed - the grid revert snapshot is no longer
                // valid (its dim count won't match).
                owner.wave.scatterFromGridSnapshot.reset();
            }
            rebuildAxisSteppers();
            owner.syncPositionParams();
            owner.updateHintText();
            // The view N now grew by one - regrow the rotation-angle vector,
            // rebuild this window's rotation slider strip + the selected-
            // frame position strip, and refresh the main editor's
            // projection combo.
            owner.rebuildScatterUI();
            rebuildSliders();
            rebuildSelFrameControls();
            owner.onLayerChanged();
            resized();
        };
        settingsContainer.addAndMakeVisible(removeAxisBtn);
        removeAxisBtn.setTooltip("Remove the last axis. In Grid mode, waveforms whose "
                                 "coord on the dropped axis is non-zero are deleted. In "
                                 "Scatter mode every dot's position vector is truncated "
                                 "(its coordinate on the dropped axis is discarded).");
        removeAxisBtn.onClick = [this]() {
            if (owner.wave.mode == WavetableMode::Grid) {
                if (owner.wave.gridDims.size() <= 1) return;
                // Drop frames whose coord on the last axis is > 0, then shrink.
                const int lastAxis = (int)owner.wave.gridDims.size() - 1;
                owner.wave.resizeGridAxis(lastAxis, 1);  // collapse last axis to 1
                owner.wave.gridDims.pop_back();
                // After popping, the surviving frames are still in the right
                // slots (their coord on the dropped axis was 0).
                owner.wave.scatterFromGridSnapshot.reset();
                if (owner.currentFrameIdx >= (int)owner.wave.cellWaveformIds.size())
                    owner.currentFrameIdx = 0;
            } else {
                // Scatter: the floor is 0 dims (a drop-target placeholder
                // view). 1 dim renders as a line view, 2+ as the square/cube
                // view. Truncate each dot's position vector to the new dim
                // count; no dots are deleted because every position is valid
                // in fewer dims.
                if (owner.wave.scatterDims <= 0) return;
                --owner.wave.scatterDims;
                for (auto& sf : owner.wave.scatterFrames) {
                    if ((int)sf.position.size() > owner.wave.scatterDims)
                        sf.position.resize((size_t)owner.wave.scatterDims);
                }
                owner.wave.scatterFromGridSnapshot.reset();
            }
            rebuildAxisSteppers();
            owner.syncPositionParams();
            owner.updateHintText();
            // View N shrank - shrink the rotation-angle vector and rebuild
            // the slider strip + selected-frame strip / projection combo
            // accordingly.
            owner.rebuildScatterUI();
            rebuildSliders();
            rebuildSelFrameControls();
            owner.onLayerChanged();
            resized();
        };

        // ---- Blend width (Scatter mode only) ----
        radiusLabel.setText("Blend width:", juce::dontSendNotification);
        radiusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        radiusLabel.setJustificationType(juce::Justification::centredLeft);
        settingsContainer.addAndMakeVisible(radiusLabel);

        settingsContainer.addAndMakeVisible(radiusSlider);
        radiusSlider.setRange(0.05, 1.5, 0.0);
        radiusSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        radiusSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 18);
        radiusSlider.setValue(owner.wave.scatterRadius, juce::dontSendNotification);
        radiusSlider.setTooltip("How sharply the blend snaps to the nearest scatter "
                                "waveform as you move the Position controls. Smaller = "
                                "sharper transitions between waveforms; larger = smoother, "
                                "wider blends. The Position always tracks smoothly at any "
                                "setting. In \"Distance fades volume\" mode this instead "
                                "sets the literal fade radius (normalized [0,1] units) "
                                "beyond which a waveform falls silent.");
        radiusSlider.onValueChange = [this]() {
            owner.wave.scatterRadius = (float)radiusSlider.getValue();
            owner.onLayerChanged();
            view->repaint();
        };

        // ---- "Distance fades volume" toggle (Grid + Scatter) ----
        // Button text and tooltip are set per-mode in the visibility-gating
        // block (updateModeUI), since the wording differs between the grid
        // "empty cells fade volume" framing and the scatter "distance fades
        // volume" framing. Here we just register it and wire the click.
        settingsContainer.addAndMakeVisible(absBlendToggle);
        absBlendToggle.setButtonText("Distance fades volume");
        absBlendToggle.setToggleState(owner.wave.absoluteBlend, juce::dontSendNotification);
        absBlendToggle.onClick = [this]() {
            owner.wave.absoluteBlend = absBlendToggle.getToggleState();
            // The blend mode only changes the gain math (normalized average vs
            // raw-weight gain); it does NOT change which axes are traversable -
            // that's purely the cells'/dots' spatial spread. So no Position-
            // param / placement-strip re-sync is needed, just rebuild the audio
            // render and repaint.
            owner.onLayerChanged();
            view->repaint();
        };

        // ---- Selected-frame position section header ----
        selFrameSectionLabel.setText("Selected waveform position:", juce::dontSendNotification);
        selFrameSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        selFrameSectionLabel.setJustificationType(juce::Justification::centredLeft);
        selFrameSectionLabel.setTooltip("Per-axis position of the currently selected waveform. "
                                        "In Scatter mode each slider sets that waveform's "
                                        "coordinate on the axis (0..1). In Grid mode each "
                                        "stepper picks which cell along the axis - moving "
                                        "onto an occupied cell swaps the two waveforms.");
        settingsContainer.addAndMakeVisible(selFrameSectionLabel);

        // ---- "Add to grid" button (shown instead of the position section
        //      when the highlighted library waveform isn't placed yet) ----
        addToGridBtn.setTooltip("This waveform exists in the library but isn't "
                                "placed in the arrangement view yet, so it has no "
                                "cell position. Click to drop it into the grid "
                                "(first empty cell, or a new cell if the grid is "
                                "full) - the position controls then appear.");
        addToGridBtn.onClick = [this]() {
            if (owner.currentLibraryId >= 0)
                addLibraryEntryToGrid(owner.currentLibraryId);
        };
        settingsContainer.addAndMakeVisible(addToGridBtn);

        // ---- Rotation section header ----
        rotationSectionLabel.setText("Rotation (per N-D plane):", juce::dontSendNotification);
        rotationSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        rotationSectionLabel.setJustificationType(juce::Justification::centredLeft);
        settingsContainer.addAndMakeVisible(rotationSectionLabel);

        // ---- Reset rotation (pinned bottom, NOT in the scroll area) ----
        addAndMakeVisible(resetRotBtn);
        resetRotBtn.setTooltip("Reset all rotation sliders to 0 (identity rotation). "
                               "The view returns to a pure axis-aligned projection.");
        resetRotBtn.onClick = [this]() {
            int N = std::max(2, owner.wave.numDimensions());
            auto pairs = LayeredWaveEditorComponent::scatterPlanePairs(N);
            for (const auto& ij : pairs)
                owner.setScatterPlaneAngle(ij.first, ij.second, 0.0f);
            refreshSliderValues();
        };

        // Top-bar buttons (Apply / Close / ?) and the discoverability
        // hint label live on LayeredWaveEditorComponent itself; this
        // panel is an embedded child of that editor, so duplicating them
        // here would just give the user two sets of controls that do the
        // same thing. The exception is "+ Waveform" - it used to be a
        // top-bar button on the owner but now lives here in the sidebar
        // (next to the library list it adds to), and delegates to the
        // owner's showAddWaveformMenu via the friend relationship.
        // updateHintText() is still called from refreshAfterDocMutation()
        // so the OWNER's hint stays accurate as the doc changes.

        rebuildSliders();
        rebuildAxisSteppers();
        rebuildSelFrameControls();
        rebuildLibraryList();
        rebuildFramesList();
        updateConvertButton();
        updateAssignButtonEnabled();

        // Default reasonable size; the owner overrides this on resize.
        setSize(940, 620);
    }

    void resized() override {
        auto r = getLocalBounds().reduced(0);

        // No top-bar or hint here - the owning LayeredWaveEditorComponent
        // owns those, and we're embedded into the area below them. We
        // fill our bounds with the sidebar (right) + scatter view (rest).

        const int sidebarW = 280;
        auto sidebar = r.removeFromRight(sidebarW);
        r.removeFromRight(8);

        // Sidebar layout, top to bottom:
        //   View combo (stereo / 3D mode)
        //   Library label + list (~120h) + "+ Waveform" button
        //   "Assign to selected cell" button
        //   Cells label + list (~110h)
        //   Convert button (mode-dependent)
        //   <scrollable settings viewport> (grid/radius + cursor pos +
        //                                   selected-frame pos + rotation)
        //   Reset rotation button (pinned bottom)
        stereoModeCombo.setBounds(sidebar.removeFromTop(28));
        sidebar.removeFromTop(8);

        // ----- Library section -----
        librarySectionLabel.setBounds(sidebar.removeFromTop(18));
        sidebar.removeFromTop(2);
        const int libListH = 120;
        libraryListViewport.setBounds(sidebar.removeFromTop(libListH));
        layoutLibraryListEntries();
        sidebar.removeFromTop(4);
        addLibraryEntryBtn.setBounds(sidebar.removeFromTop(24));
        sidebar.removeFromTop(4);
        assignToCellBtn.setBounds(sidebar.removeFromTop(24));
        sidebar.removeFromTop(10);

        // ----- Cells section -----
        framesListLabel.setBounds(sidebar.removeFromTop(18));
        sidebar.removeFromTop(2);
        const int listH = 110;
        framesListViewport.setBounds(sidebar.removeFromTop(listH));
        // Re-bound the rows to the new viewport width. At construction time
        // rebuildFramesList runs with viewport width 0, so the rows would
        // otherwise be invisibly narrow until something rebuilt them.
        layoutFramesListEntries();
        sidebar.removeFromTop(6);

        // Convert button row (Grid <-> Scatter mode toggle).
        convertBtn.setBounds(sidebar.removeFromTop(26));
        sidebar.removeFromTop(8);

        // Reset rotation pinned to the bottom of the sidebar (outside the
        // scroll area so it's always reachable).
        resetRotBtn.setBounds(sidebar.removeFromBottom(28));
        sidebar.removeFromBottom(6);

        // Everything else is in the scrollable viewport.
        settingsViewport.setBounds(sidebar);
        layoutSettingsContainer();

        view->setBounds(r);
    }

    // Discoverability hint is owned by LayeredWaveEditorComponent and
    // refreshed by its updateHintText(). Nothing to do here - this used
    // to update an embedded hint label that no longer exists.

    ~WavetableViewWindowContent() override = default;

    // (placeLibraryEntryIntoCell was used by the old pencil "focus on
    // orphan entry" flow, which forced auto-placement as a side-effect of
    // focusing the editor. With the library-id-addressed editor the
    // focus step doesn't need a cell anymore, so the helper is gone.
    // The +Waveform flow inlines its own placement logic.)

    // Lay out the per-axis / per-plane controls inside settingsContainer and
    // size the container so it scrolls when content overflows. Mode-dependent
    // sections (grid axes vs RBF radius) and Scatter-only sections (selected
    // frame editable sliders) are shown/hidden here.
    void layoutSettingsContainer() {
        const int innerW = std::max(0, settingsViewport.getWidth()
                                         - settingsViewport.getScrollBarThickness() - 2);
        const int contentX = 0;
        int contentY = 0;
        const int sectionGap = 10;
        const int rowH = 22;
        const int rowGap = 2;

        const bool isGrid = (owner.wave.mode == WavetableMode::Grid);

        // -- Axes section (always visible; label + buttons are mode-aware) --
        // In Grid mode the section also shows one cells-per-axis stepper per
        // dim. In Scatter mode there are no per-axis-size steppers (every
        // axis is continuous 0..1), so the section collapses to label +
        // buttons and is followed by the RBF radius row.
        gridAxesLabel.setVisible(true);
        addAxisBtn.setVisible(true);
        removeAxisBtn.setVisible(true);
        gridAxesLabel.setText(isGrid ? juce::String("Grid axes (cells per axis):")
                                     : juce::String("Scatter axes (dimensions):"),
                              juce::dontSendNotification);
        // Disable buttons at the floors / ceilings so the user gets a
        // tooltip-only hint that they can't go further (and so the visual
        // state matches the no-op handler).
        const int curN = isGrid ? (int)owner.wave.gridDims.size()
                                : owner.wave.scatterDims;
        const int floorN = isGrid ? 1 : 0;
        addAxisBtn.setEnabled(curN < 8);
        removeAxisBtn.setEnabled(curN > floorN);
        // Why-disabled tooltips so a greyed button explains itself (CLAUDE.md).
        if (curN >= 8)
            addAxisBtn.setTooltip("Already at the maximum of 8 axes.");
        else
            addAxisBtn.setTooltip(isGrid
                ? juce::String("Add a grid dimension (new axis starts with 1 cell).")
                : juce::String("Add a scatter dimension. 1 axis = line view, 2 = square, "
                               "3 = cube, and so on."));
        if (curN <= floorN)
            removeAxisBtn.setTooltip(isGrid
                ? juce::String("A grid always needs at least one axis - can't remove the last one.")
                : juce::String("Already at the minimum (0 axes - the drag-and-drop target view)."));
        else
            removeAxisBtn.setTooltip(isGrid
                ? juce::String("Remove the last axis. Waveforms whose coordinate on the dropped "
                               "axis is non-zero are deleted.")
                : juce::String("Remove the last axis. Every dot's position vector is truncated "
                               "(its coordinate on the dropped axis is discarded). At 1 axis this "
                               "gives a line view; at 0, the drag-and-drop target."));

        gridAxesLabel.setBounds(contentX, contentY, innerW, 18);
        contentY += 18 + 2;
        if (isGrid) {
            for (size_t k = 0; k < axisSizeSliders.size(); ++k) {
                if (k < axisSizeLabels.size())
                    axisSizeLabels[k]->setBounds(contentX, contentY, 48, rowH);
                axisSizeSliders[k]->setBounds(contentX + 48, contentY, innerW - 48, rowH);
                contentY += rowH + rowGap;
            }
        }
        addAxisBtn.setBounds(contentX, contentY, 70, 24);
        removeAxisBtn.setBounds(contentX + 76, contentY, 70, 24);
        contentY += 24;

        // The radius slider is scatter-only (it's the RBF cutoff). The
        // "fades volume" toggle is shown in BOTH modes, but its wording
        // differs: in scatter it's about distance to a dot, in grid it's
        // about morphing toward an empty cell.
        if (isGrid) {
            radiusLabel.setVisible(false);
            radiusSlider.setVisible(false);
            contentY += sectionGap;
            absBlendToggle.setButtonText("Empty cells fade volume");
            absBlendToggle.setTooltip(
                "Off (default): empty cells don't drain volume - the morph is "
                "renormalized over the filled cells, so the output stays full-"
                "volume even when some cells are empty.\n"
                "On: morphing toward an empty cell ducks the output toward "
                "silence (the older pre-renormalization grid behavior). Has no "
                "effect when every cell is filled.");
        } else {
            radiusLabel.setVisible(true);
            radiusSlider.setVisible(true);
            contentY += sectionGap;
            radiusLabel.setBounds(contentX, contentY, 80, rowH);
            radiusSlider.setBounds(contentX + 80, contentY, innerW - 80, rowH);
            contentY += rowH;
            absBlendToggle.setButtonText("Distance fades volume");
            absBlendToggle.setTooltip(
                "Off (default): the blend is volume-normalized, so the overall "
                "level stays constant as you morph and a lone waveform always "
                "plays at full volume.\n"
                "On: moving the Position toward a waveform makes it louder and "
                "away makes it quieter; a Position outside every waveform's RBF "
                "radius is silent. Lets a single scatter dot act as a loudness "
                "island you swell into.");
        }
        absBlendToggle.setToggleState(owner.wave.absoluteBlend, juce::dontSendNotification);
        absBlendToggle.setVisible(true);
        absBlendToggle.setBounds(contentX, contentY, innerW, rowH);
        contentY += rowH;
        contentY += sectionGap;

        // -- Selected-frame position controls (sliders or steppers) --
        // The position section only makes sense when the highlighted library
        // waveform is actually placed in the arrangement view. When it isn't
        // (a freshly-created entry that's still library-only), the cell
        // coordinates would be meaningless, so we hide the label + sliders and
        // show an "Add to grid" button instead. With nothing highlighted at
        // all, the whole section collapses.
        const bool placed  = highlightedWaveformPlaced();
        const bool haveLib = (owner.currentLibraryId >= 0);
        // A placed waveform on an arrangement with no geometric axes (a scatter
        // space dialled all the way down to 0 dims - the drop-target view) has
        // no position controls; hide the header too so it doesn't dangle over
        // an empty section.
        const bool showPos = placed && !selFrameSliders.empty();
        selFrameSectionLabel.setVisible(showPos);
        for (auto& s : selFrameSliders) s->setVisible(showPos);
        for (auto& l : selFrameLabels)  l->setVisible(showPos);
        addToGridBtn.setVisible(!placed && haveLib);
        if (showPos) {
            selFrameSectionLabel.setBounds(contentX, contentY, innerW, 18);
            contentY += 18 + 2;
            for (size_t k = 0; k < selFrameSliders.size(); ++k) {
                if (k < selFrameLabels.size())
                    selFrameLabels[k]->setBounds(contentX, contentY, 32, rowH);
                selFrameSliders[k]->setBounds(contentX + 32, contentY, innerW - 32, rowH);
                contentY += rowH + rowGap;
            }
            contentY += sectionGap;
        } else if (!placed && haveLib) {
            addToGridBtn.setBounds(contentX, contentY, std::min(innerW, 160), 26);
            contentY += 26 + sectionGap;
        }

        // -- Rotation sliders (one per N-D plane) --
        rotationSectionLabel.setBounds(contentX, contentY, innerW, 18);
        contentY += 18 + 2;
        const int rotRowH = 24;
        for (size_t k = 0; k < planeSliders.size(); ++k) {
            if (k < planeLabels.size())
                planeLabels[k]->setBounds(contentX, contentY, 72, rotRowH);
            planeSliders[k]->setBounds(contentX + 72, contentY, innerW - 72, rotRowH);
            contentY += rotRowH + rowGap;
        }

        settingsContainer.setSize(innerW, std::max(contentY, 10));
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(20, 20, 26));
    }

    // Called by owner when something changed the angle outside this window
    // (mouse drag in the embedded ScatterView, dim count change, etc.).
    // Pushes the current owner angles into the slider widgets WITHOUT
    // firing onValueChange (would otherwise re-write the owner and could
    // cascade).
    void refreshSliderValues() {
        int N = std::max(2, owner.wave.numDimensions());
        if ((int)planeSliders.size() != LayeredWaveEditorComponent::scatterPlaneCount(N)) {
            rebuildSliders();
            resized();
            return;
        }
        auto pairs = LayeredWaveEditorComponent::scatterPlanePairs(N);
        for (size_t k = 0; k < pairs.size() && k < planeSliders.size(); ++k) {
            float deg = owner.getScatterPlaneAngle(pairs[k].first, pairs[k].second);
            planeSliders[k]->setValue(deg, juce::dontSendNotification);
        }
        view->repaint();
    }

    // Called by owner whenever the frame set / mode / grid topology
    // changes (add/remove frame, convert mode, cell placement). Rebuilds
    // every list/stepper/button that depends on the doc's structure.
    void refreshAfterDocMutation() {
        // Grid axis resize / scatter frame add/remove / mode convert can cross
        // the "traversable" threshold (grid axis 1<->2, scatter frames 1<->2
        // under normalized blend), changing the effective dimension count. Re-
        // sync Position params/pins so a newly-traversable axis gains its control
        // and a now-inert one loses it. Guarded internally - no-op when unchanged.
        owner.maybeSyncPositionParams();
        rebuildLibraryList();
        rebuildFramesList();
        rebuildAxisSteppers();
        // Mode may have flipped (Grid<->Scatter) - rebuild the per-axis
        // selected-frame controls so they show the right widget kind.
        rebuildSelFrameControls();
        updateConvertButton();
        updateAssignButtonEnabled();
        // Mode may have flipped; the radius/axes sections swap visibility
        // inside layoutSettingsContainer().
        resized();
        view->repaint();
    }

    // Deferred variant of refreshAfterDocMutation(). MUST be used instead of
    // calling refreshAfterDocMutation() directly from a juce::Slider's
    // onValueChange handler when that refresh can destroy the very slider whose
    // callback is on the stack (the per-axis size steppers and the selected-
    // frame grid steppers both live in lists that refreshAfterDocMutation()
    // clears+rebuilds). JUCE's Slider::Pimpl::mouseWheelMove keeps touching the
    // Slider (and its ScopedDragNotification) as the stack unwinds AFTER the
    // value-change callback returns, so synchronously deleting the slider here
    // is a use-after-free (unlike JUCE Buttons, Sliders don't guard their
    // callbacks with a SafePointer). Deferring the rebuild to the next message-
    // loop tick lets the mouse-wheel/drag machinery finish unwinding against a
    // still-alive slider; the SafePointer guards against the window closing in
    // between. Model mutation stays synchronous at the call site - only the UI
    // rebuild is deferred.
    void scheduleRefreshAfterDocMutation() {
        juce::MessageManager::callAsync(
            [safe = juce::Component::SafePointer<WavetableViewWindowContent>(this)]() {
                if (auto* self = safe.getComponent())
                    self->refreshAfterDocMutation();
            });
    }

    // Called by owner whenever currentFrameIdx changes OR a scatter
    // position changes (frame click, drag, switchToFrame, +Frame, etc.).
    // Pushes the new selection's position into the per-axis sliders /
    // steppers, re-evaluates "Back to Grid" eligibility (any drag off a
    // snapshotted cell center should disable it), and repaints the view.
    void refreshFrameAndPositionValues() {
        refreshSelFrameValues();
        // The highlighted entry's placed/unplaced state may have flipped
        // (e.g. selecting a library-only entry), which swaps the position
        // section for the "Add to grid" button - relayout to apply.
        layoutSettingsContainer();
        updateConvertButton();
        // Cell selection moved - the Assign button's "cell selected" half
        // may have flipped.
        updateAssignButtonEnabled();
        // The Library row highlight tracks currentLibraryId. switchToFrame
        // syncs currentLibraryId to whichever entry the clicked cell holds,
        // so we need to refresh the row toggle states for the highlight to
        // follow dot clicks / drags (not just direct Library row clicks).
        refreshLibraryHighlight();
        view->repaint();
    }

    // Refresh the on/off state of each Library row button so the amber
    // highlight tracks owner.currentLibraryId. Lightweight: no rebuild,
    // just toggleState updates. Also scrolls the selected row into view
    // in the library viewport so the user can see which entry is now
    // the editor target after a click on a dot or cell.
    //
    // Falls back to a full rebuildLibraryList if the row vector got out
    // of sync with wave.library (only happens if something mutated the
    // library without going through refreshAfterDocMutation - shouldn't
    // happen, but the fallback keeps the UI from going stale silently).
    void refreshLibraryHighlight() {
        if (libraryRowButtons.size() != owner.wave.library.size()) {
            // Library changed shape (entry added/removed). Rebuild the rows
            // - which also sets each row's toggle state - then fall through
            // to the scroll-into-view logic below so a freshly-added entry
            // (which lands at the bottom of the list) is scrolled into view.
            rebuildLibraryList();
        } else {
            for (size_t i = 0; i < libraryRowButtons.size(); ++i) {
                const int entryId = owner.wave.library[i].id;
                libraryRowButtons[i]->setToggleState(
                    entryId == owner.currentLibraryId,
                    juce::dontSendNotification);
            }
        }
        juce::Component* selRow = nullptr;
        for (size_t i = 0; i < libraryRowButtons.size()
                          && i < owner.wave.library.size(); ++i) {
            if (owner.wave.library[i].id == owner.currentLibraryId) {
                selRow = libraryRowButtons[i].get();
                break;
            }
        }
        if (selRow) {
            // Scroll the selected row into view if it isn't already. Use
            // getBoundsInParent because the row lives inside
            // libraryListContainer, which is what the viewport scrolls.
            auto rowR = selRow->getBoundsInParent();
            auto vis  = libraryListViewport.getViewArea();
            if (!vis.contains(rowR.getCentre())) {
                int targetY = std::max(0, rowR.getY() - 4);
                libraryListViewport.setViewPosition(0, targetY);
            }
        }
    }

    // The embedded ScatterView is the editor's one source of truth for
    // arrangement-view rendering. Public so the owning
    // LayeredWaveEditorComponent can wire repaints / projection / stereo
    // settings through it. The owner does NOT own a separate ScatterView
    // - this one is the one.
    std::unique_ptr<ScatterView> view;

private:
    void onConvertClicked() {
        if (owner.wave.mode == WavetableMode::Grid) {
            owner.wave.convertGridToScatter();
            owner.currentPosition.assign(owner.wave.numDimensions(), 0.5f);
            owner.currentFrameIdx = 0;
        } else {
            // Reverse path. If every dot is still at its snapshotted cell
            // center we do a lossless revert (exact original grid layout).
            // Otherwise we fall back to a general flatten-to-1D-grid so the
            // user can always get back to Grid mode.
            if (owner.wave.canRevertScatterToGrid())
                owner.wave.revertScatterToGrid();
            else
                owner.wave.convertScatterToGrid();
            owner.currentFrameIdx = 0;
        }
        owner.ensureScatterPlaneAngles();
        owner.syncPositionParams();
        owner.updateHintText();
        owner.rebuildRows();
        owner.onLayerChanged();
        refreshAfterDocMutation();
        rebuildSliders();
        resized();
    }

public:
    // Public so the owning editor's onLayerChanged() can re-evaluate the
    // button after any mutation (keeps "Back to Grid" from going stale).
    void updateConvertButton() {
        // The placements list shows grid cells in Grid mode and free-positioned
        // dots in Scatter mode. Label it for what it actually contains so
        // "Cells" doesn't read as wrong terminology while in Scatter mode.
        framesListLabel.setText(owner.wave.mode == WavetableMode::Grid
                                    ? "Cells" : "Waveforms",
                                juce::dontSendNotification);
        if (owner.wave.mode == WavetableMode::Grid) {
            convertBtn.setButtonText(juce::String::fromUTF8("Convert \xE2\x86\x92 Scatter"));
            convertBtn.setEnabled(true);
            convertBtn.setTooltip("Convert this Grid wavetable to Scatter mode. Each "
                                  "non-empty cell becomes a scatter dot at the cell center. "
                                  "You can revert back to Grid as long as you don't drag "
                                  "any dot off its original cell center.");
        } else {
            const bool canRevert = owner.wave.canRevertScatterToGrid();
            // Always enabled - if a lossless revert isn't possible we fall back
            // to a general (lossy) flatten-to-1D-grid, so the user is never
            // stranded in Scatter mode.
            convertBtn.setEnabled(true);
            if (canRevert) {
                convertBtn.setButtonText(juce::String::fromUTF8("\xE2\x86\xA9 Back to Grid"));
                convertBtn.setTooltip("Revert to Grid mode. Every dot is still at its "
                                      "original cell center, so the conversion is lossless "
                                      "(the exact grid layout you started with comes back).");
            } else {
                convertBtn.setButtonText(juce::String::fromUTF8("Convert \xE2\x86\x92 Grid"));
                convertBtn.setTooltip("Convert to Grid mode. This wavetable can't be losslessly "
                                      "reverted (it was authored as Scatter, or you edited axes / "
                                      "moved dots), so the dots are laid out into a 1D grid - one "
                                      "cell per dot, in their current order. The free-form scatter "
                                      "positions are discarded; the waveforms themselves are kept. "
                                      "Once in Grid mode you can add axes and grow cells.");
            }
        }
    }

private:
    // Walk the existing frames-list buttons / delete-X widgets and re-bound
    // them based on the current viewport width. This is split out from
    // rebuildFramesList so resized() can apply correct widths after the
    // viewport finally has a non-zero size (at construction time the
    // viewport width is 0, so positions calculated there would clip).
    void layoutFramesListEntries() {
        const int rowH = 24;
        const int delW = 18;
        int contentW = std::max(0, framesListViewport.getWidth() - 14);
        int y = 0;
        for (size_t i = 0; i < frameListButtons.size(); ++i) {
            if (frameListButtons[i])
                frameListButtons[i]->setBounds(0, y, std::max(0, contentW - delW - 4), rowH - 2);
            if (i < frameListDeletes.size() && frameListDeletes[i])
                frameListDeletes[i]->setBounds(contentW - delW, y, delW, rowH - 2);
            y += rowH;
        }
        framesListContainer.setSize(contentW, std::max(y, 10));
    }

    // Re-bound the rows of the Library list to the current viewport width.
    // Same pattern as layoutFramesListEntries: at construction the viewport
    // width is 0, so rebuildLibraryList's positions are clipped until a
    // resized() pass re-runs this with the real width.
    //
    // Two-column layout: [label button stretches to fill] [red X delete].
    // The old three-column layout had a pencil column for "focus the editor
    // on this entry" - that's now done by clicking the row itself
    // (selection IS the editor target now), so the pencil is gone.
    void layoutLibraryListEntries() {
        const int rowH = 24;
        const int delW = 18;
        const int swatchW = 18;
        int contentW = std::max(0, libraryListViewport.getWidth() - 14);
        int y = 0;
        for (size_t i = 0; i < libraryRowButtons.size(); ++i) {
            int xRight = contentW;
            if (i < libraryRowDeletes.size() && libraryRowDeletes[i]) {
                xRight -= delW;
                libraryRowDeletes[i]->setBounds(xRight, y, delW, rowH - 2);
                xRight -= 4;
            }
            int xLeft = 0;
            if (i < libraryRowSwatches.size() && libraryRowSwatches[i]) {
                libraryRowSwatches[i]->setBounds(xLeft, y + (rowH - swatchW) / 2,
                                                 swatchW, swatchW);
                xLeft += swatchW + 4;
            }
            if (libraryRowButtons[i])
                libraryRowButtons[i]->setBounds(xLeft, y,
                                                std::max(0, xRight - xLeft),
                                                rowH - 2);
            y += rowH;
        }
        libraryListContainer.setSize(contentW, std::max(y, 10));
    }

public:
    // Rebuild the Library list from owner.wave.library. One row per entry,
    // even if the entry is currently orphaned (no cell references it).
    // Selection IS the editor target now: clicking a row focuses the
    // right-pane editor on that library entry. The selection highlight
    // tracks owner.currentLibraryId (the single source of truth - no
    // separate sidebar-selected state). Public because the owner editor
    // calls it directly after waveform-identity edits (colour pick, name
    // change) that only need the library list to repaint, not the whole
    // sidebar via refreshAfterDocMutation.
    void rebuildLibraryList() {
        libraryListContainer.removeAllChildren();
        libraryRowButtons.clear();
        libraryRowDeletes.clear();
        libraryRowSwatches.clear();

        for (size_t k = 0; k < owner.wave.library.size(); ++k) {
            const auto& entry = owner.wave.library[k];
            const int entryId = entry.id;
            int useCount = owner.wave.countCellsUsingLibrary(entryId);

            juce::String label = entry.name.empty()
                ? juce::String("Waveform ") + juce::String((int)k + 1)
                : juce::String(entry.name);
            if (useCount > 0)
                label += juce::String(" (used ") + juce::String(useCount)
                       + juce::String::fromUTF8("\xC3\x97)");

            auto btn = std::make_unique<LibraryDragButton>(entryId);
            btn->setButtonText(label);
            btn->setClickingTogglesState(true);
            btn->setToggleState(entryId == owner.currentLibraryId,
                                juce::dontSendNotification);
            // Explicit toggle-on color. JUCE's default buttonOnColourId is
            // visually almost identical to buttonColourId, so on/off
            // toggling reads as no change at all - the user clicks a row,
            // it never looks selected, and they assume the click did
            // nothing. We use the same amber as the arrangement-view
            // "shown in editor" ring so both views agree visually on
            // which library entry the right pane is editing.
            btn->setColour(juce::TextButton::buttonOnColourId,
                           juce::Colour(0xffffc34a).withAlpha(0.55f));
            btn->setColour(juce::TextButton::textColourOnId,
                           juce::Colours::black);
            btn->setTooltip(
                "Click to edit this waveform in the right pane. Edits flow into "
                "every cell that references it. Use 'Assign to selected cell' "
                "below to place this waveform in the cell currently selected "
                "in the arrangement view. Drag this row onto the wavetable view "
                "to drop it as a new waveform at the cursor (or into a cell in "
                "Grid mode).");
            btn->onClick = [this, entryId]() {
                // User selected on the Library surface: Delete now targets the
                // waveform DEFINITION (this entry), not a placement.
                owner.activeSelectionSurface =
                    LayeredWaveEditorComponent::SelectionSurface::Library;
                owner.setEditingLibraryEntry(entryId);
                rebuildLibraryList();
                updateAssignButtonEnabled();
                // setEditingLibraryEntry repoints currentFrameIdx at this
                // entry's first placement (if any); refresh the position
                // section visibility / values to match.
                refreshFrameAndPositionValues();
            };
            btn->onRightClick = [this, entryId](juce::Point<int> sp) {
                showLibraryRowMenu(entryId, sp);
            };
            libraryListContainer.addAndMakeVisible(btn.get());
            libraryRowButtons.push_back(std::move(btn));

            // Colour swatch on the left of the row. Same resolved colour
            // as the dots in the arrangement view (so visual identity
            // between sidebar row and viewport dot is exact). Click pops
            // the palette menu and writes back to library[k].colorIdx.
            auto sw = std::make_unique<LibraryColorSwatch>();
            sw->setSwatchColor(libraryEntryDisplayColor(&entry, (int)k));
            sw->setIsAuto(entry.colorIdx < 0);
            sw->setTooltip(
                "Click to colour-code this waveform. Auto picks a colour from "
                "the waveform's harmonic content. Every cell and dot "
                "referencing this waveform uses the picked colour.");
            sw->onPick = [this, entryId](int idx) {
                const int libIdx = owner.wave.findLibraryIndexById(entryId);
                if (libIdx < 0) return;
                owner.wave.library[libIdx].colorIdx = idx;
                owner.commitToNode();
                rebuildLibraryList();
                if (view) view->repaint();
                // If the editor is bound to this entry, sync its identity
                // row so the swatch and badge update too.
                if (owner.currentLibraryId == entryId)
                    owner.refreshIdentityRow();
                refreshAfterDocMutation();
            };
            libraryListContainer.addAndMakeVisible(sw.get());
            libraryRowSwatches.push_back(std::move(sw));

            auto del = std::make_unique<FrameDeleteX>();
            del->setTooltip(
                useCount > 0
                    ? juce::String("Remove this entry from the library. The ")
                          + juce::String(useCount)
                          + juce::String(useCount == 1 ? " cell" : " cells")
                          + juce::String(" referencing it will become empty.")
                    : juce::String("Remove this unplaced library entry."));
            del->onClick = [this, entryId]() { deleteLibraryEntry(entryId); };
            libraryListContainer.addAndMakeVisible(del.get());
            libraryRowDeletes.push_back(std::move(del));
        }
        layoutLibraryListEntries();
    }

    // True when the currently-highlighted library entry is actually placed
    // in the arrangement view (at least one Grid cell or Scatter dot
    // references it). Drives whether the "Selected waveform position"
    // section shows its cell-coordinate controls or the "Add to grid"
    // button instead.
    bool highlightedWaveformPlaced() const {
        return owner.currentLibraryId >= 0
            && owner.wave.countCellsUsingLibrary(owner.currentLibraryId) > 0;
    }

    // Remove a library entry (and clear every cell/dot that referenced it).
    // Shared by the row's X button, the right-click menu's Delete, and the
    // Del/Backspace key. Falls the editor target back to the first surviving
    // entry (or -1 when the library is now empty).
    void deleteLibraryEntry(int entryId) {
        if (owner.wave.findLibraryIndexById(entryId) < 0) return;
        owner.wave.removeLibraryEntry(entryId);
        if (owner.currentLibraryId == entryId) {
            owner.currentLibraryId = owner.wave.library.empty()
                ? -1
                : owner.wave.library.front().id;
        }
        owner.wave.scatterFromGridSnapshot.reset();
        owner.updateHintText();
        owner.rebuildRows();
        owner.onLayerChanged();
        refreshAfterDocMutation();
        refreshFrameAndPositionValues();
    }

    // Clone a library entry's waveform into a brand-new library entry WITHOUT
    // placing it in the arrangement view (the Library row's right-click
    // "Duplicate" action). ScatterView::duplicateLibraryEntryFromCell (the
    // dot's "Duplicate waveform") now behaves identically - all creation
    // paths are library-only; placement is always a separate explicit step.
    void duplicateLibraryEntry(int entryId) {
        const int srcIdx = owner.wave.findLibraryIndexById(entryId);
        if (srcIdx < 0) return;
        auto* srcWave = owner.wave.library[(size_t)srcIdx].wave.get();
        if (!srcWave) return;
        std::string srcName = owner.wave.library[(size_t)srcIdx].name;
        if (srcName.empty())
            srcName = "Waveform " + std::to_string(srcIdx + 1);
        const int newId = owner.wave.addLibraryEntry(srcWave->clone(),
                                                     srcName + " (copy)");
        if (newId < 0) return;
        owner.currentLibraryId = newId;
        owner.wave.scatterFromGridSnapshot.reset();
        owner.updateHintText();
        owner.rebuildRows();
        owner.onLayerChanged();
        refreshAfterDocMutation();
        refreshFrameAndPositionValues();
    }

    // Place an existing library entry into the arrangement view. Grid mode:
    // fills the first empty cell, growing the first axis by one if every cell
    // is occupied (capped at 64). Scatter mode: drops a new dot beside the
    // existing dots (offset along X), or dead-centre if it's the first dot.
    // Used by the "Add to grid" button and the right-click "Add to grid" item.
    void addLibraryEntryToGrid(int entryId) {
        if (owner.wave.findLibraryIndexById(entryId) < 0) return;
        if (owner.wave.mode == WavetableMode::Scatter) {
            ScatterFrame sf;
            sf.waveformId = entryId;
            // Size the position vector to the geometric dimension. 0 dims (the
            // drop-target view) yields an empty vector - all dots sit at the
            // same (empty) point and blend equally.
            sf.position.assign((size_t)std::max(0, owner.wave.scatterDims), 0.5f);
            // Offset along X from the existing dots so the new one lands beside
            // them instead of stacked invisibly on top - and so dropping a
            // second waveform in immediately yields a traversable X axis (the
            // dots now span a range). The user can drag it elsewhere (e.g. to
            // differ in Y instead) afterwards. A lone first dot stays centred
            // (it's inert until a second dot gives it something to span).
            // Guard on a non-empty position so the 0-dim case (no X axis) is
            // safe.
            if (!sf.position.empty() && !owner.wave.scatterFrames.empty()) {
                float maxX = 0.0f;
                for (const auto& f : owner.wave.scatterFrames)
                    if (!f.position.empty()) maxX = std::max(maxX, f.position[0]);
                sf.position[0] = (maxX < 0.85f)
                    ? (maxX + 0.15f)
                    : juce::jlimit(0.05f, 0.95f, maxX - 0.15f);
            }
            owner.wave.scatterFrames.push_back(std::move(sf));
            owner.currentFrameIdx = (int)owner.wave.scatterFrames.size() - 1;
        } else {
            int target = -1;
            for (size_t c = 0; c < owner.wave.cellWaveformIds.size(); ++c)
                if (owner.wave.cellWaveformIds[c] < 0) { target = (int)c; break; }
            if (target < 0) {
                // Every cell full - grow the first axis by one to make room,
                // unless we're already at the per-axis ceiling.
                if (!owner.wave.gridDims.empty()
                    && owner.wave.gridDims[0] < 64) {
                    owner.wave.resizeGridAxis(0, owner.wave.gridDims[0] + 1);
                    for (size_t c = 0; c < owner.wave.cellWaveformIds.size(); ++c)
                        if (owner.wave.cellWaveformIds[c] < 0) { target = (int)c; break; }
                }
            }
            if (target < 0) return;  // couldn't make room
            owner.wave.cellWaveformIds[(size_t)target] = entryId;
            owner.currentFrameIdx = target;
        }
        owner.currentLibraryId = entryId;
        owner.wave.scatterFromGridSnapshot.reset();
        owner.updateHintText();
        owner.rebuildRows();
        owner.onLayerChanged();
        refreshAfterDocMutation();
        refreshFrameAndPositionValues();
    }

    // Right-click context menu for a Library row: Add to grid / Duplicate /
    // Delete. Async + SafePointer-guarded so the callback no-ops if the
    // window is gone by the time the user picks an item.
    void showLibraryRowMenu(int entryId, juce::Point<int> screenPos) {
        const int idx = owner.wave.findLibraryIndexById(entryId);
        if (idx < 0) return;
        juce::String name = owner.wave.library[(size_t)idx].name.empty()
            ? juce::String("Waveform ") + juce::String(idx + 1)
            : juce::String(owner.wave.library[(size_t)idx].name);
        const bool placed = owner.wave.countCellsUsingLibrary(entryId) > 0;

        juce::PopupMenu m;
        m.addSectionHeader(name);
        // 1 = Add to grid (disabled when already placed at least once -
        // there's still a use for re-adding, but keep it enabled so the user
        // can drop additional copies). We keep it always enabled.
        m.addItem(1, "Add to grid");
        m.addItem(4, "Rename...");
        m.addItem(2, "Duplicate");
        m.addSeparator();
        m.addItem(3, "Delete");
        juce::ignoreUnused(placed);

        juce::Component::SafePointer<WavetableViewWindowContent> self(this);
        m.showMenuAsync(
            juce::PopupMenu::Options().withTargetScreenArea(
                juce::Rectangle<int>(screenPos, screenPos + juce::Point<int>(1, 1))),
            [self, entryId](int result) {
                if (self == nullptr || result == 0) return;
                if (result == 1) self->addLibraryEntryToGrid(entryId);
                else if (result == 2) self->duplicateLibraryEntry(entryId);
                else if (result == 3) self->deleteLibraryEntry(entryId);
                else if (result == 4) self->owner.renameLibraryEntry(entryId);
            });
    }

    // Del/Backspace deletes whatever the user last SELECTED, routed by the
    // active selection surface so the key matches the user's mental model:
    //   View surface (clicked a cell/dot in the arrangement view or Cells
    //     list) -> remove only that PLACEMENT (clears the grid cell / erases
    //     the scatter dot); the library waveform survives. This matches the
    //     Cells-list X button and the view's shift-click-to-delete gesture.
    //   Library surface (clicked a row in the Library list) -> remove the
    //     waveform DEFINITION and every placement of it, matching the row's X.
    // Without this split both gestures share currentLibraryId, so Delete
    // always nuked the library entry even when the user had just clicked a
    // dot in the view and expected only the dot to disappear.
    bool keyPressed(const juce::KeyPress& key) override {
        if (key == juce::KeyPress::deleteKey
            || key == juce::KeyPress::backspaceKey) {
            if (owner.activeSelectionSurface
                    == LayeredWaveEditorComponent::SelectionSurface::View) {
                // Only the placement. If the selected cell is empty there is
                // nothing to remove - return false so the key falls through
                // rather than surprise-deleting the library editor target.
                return removePlacementAt(owner.currentFrameIdx);
            }
            if (owner.currentLibraryId >= 0) {
                deleteLibraryEntry(owner.currentLibraryId);
                return true;
            }
        }
        return false;
    }

    // Remove a single PLACEMENT from the wavetable, leaving the underlying
    // library waveform intact. Shared by the keyboard Delete View-surface path
    // and the Cells-list X button so the two can't diverge. Returns true if
    // frameIdx referenced a removable placement (a real scatter dot, or a
    // populated grid cell); false for an out-of-range index or an already-empty
    // grid cell, so callers can decide whether to fall through.
    bool removePlacementAt(int frameIdx) {
        if (owner.wave.mode == WavetableMode::Scatter) {
            if (frameIdx < 0 || frameIdx >= (int)owner.wave.scatterFrames.size())
                return false;
            owner.wave.scatterFrames.erase(
                owner.wave.scatterFrames.begin() + frameIdx);
            // currentFrameIdx may now be past the end; clamp to the last dot
            // (-1 when the wavetable is now empty, handled as a no-op everywhere).
            if (owner.currentFrameIdx >= (int)owner.wave.scatterFrames.size())
                owner.currentFrameIdx = (int)owner.wave.scatterFrames.size() - 1;
        } else {
            if (frameIdx < 0 || frameIdx >= (int)owner.wave.cellWaveformIds.size())
                return false;
            if (owner.wave.cellWaveformIds[frameIdx] < 0)
                return false;  // already empty - nothing to clear
            // Clear the cell reference; the library entry stays so the user can
            // re-place it. Selection deliberately stays on this (now empty) cell.
            owner.wave.cellWaveformIds[frameIdx] = -1;
        }
        owner.wave.scatterFromGridSnapshot.reset();
        owner.updateHintText();
        owner.rebuildRows();
        owner.onLayerChanged();
        refreshAfterDocMutation();
        return true;
    }

private:
    // Assign button is enabled only when both ends of the operation are
    // present: an editor target (currentLibraryId, which IS the library
    // selection) AND a cell in the arrangement view (currentFrameIdx
    // valid and in range for the active mode). In Grid mode the cell can
    // be empty - that's the whole point of assign-to-empty-cell.
    void updateAssignButtonEnabled() {
        bool haveLib = (owner.currentLibraryId >= 0);
        bool haveCell = false;
        const int idx = owner.currentFrameIdx;
        if (owner.wave.mode == WavetableMode::Grid) {
            haveCell = idx >= 0 && idx < (int)owner.wave.cellWaveformIds.size();
        } else {
            haveCell = idx >= 0 && idx < (int)owner.wave.scatterFrames.size();
        }
        assignToCellBtn.setEnabled(haveLib && haveCell);
    }

    void rebuildFramesList() {
        framesListContainer.removeAllChildren();
        frameListButtons.clear();
        frameListDeletes.clear();

        // Grid mode lists EVERY cell (including empties) so a freshly-laid-out
        // grid is fully visible and "Assign to selected cell" can target an
        // empty slot. Scatter mode lists only its existing dots - scatter
        // points don't exist until placed, so there's no "empty scatter slot".
        auto addEntry = [&](const juce::String& label, int frameIdx,
                            bool isGridSparse, bool isEmpty) {
            auto btn = std::make_unique<juce::TextButton>();
            btn->setButtonText(label);
            btn->setClickingTogglesState(true);
            btn->setToggleState(frameIdx == owner.currentFrameIdx,
                                juce::dontSendNotification);
            // Explicit toggle-on background. Same reason as the Library
            // list: JUCE's default buttonOnColourId reads as no change.
            // We use the arrangement-view dot blue so the Cells-list
            // highlight and the (selected) blue dot ring agree on which
            // cell is selected. Distinct from the Library list's amber
            // (= "shown in editor") on purpose - selection and editor
            // target are now two separate states.
            btn->setColour(juce::TextButton::buttonOnColourId,
                           juce::Colour(0xff5fb3ff).withAlpha(0.55f));
            if (isEmpty) {
                // Dim the empty-cell label so the list reads as a sparse
                // sequence rather than a wall of indistinguishable rows.
                btn->setColour(juce::TextButton::textColourOffId,
                               juce::Colours::grey);
                btn->setColour(juce::TextButton::textColourOnId,
                               juce::Colours::black);
                btn->setTooltip("Empty cell. Click to select it, then pick a "
                                "Library entry and press \"Assign to selected "
                                "cell\" - or use +Waveform to create a new "
                                "entry placed here.");
            } else {
                btn->setColour(juce::TextButton::textColourOnId,
                               juce::Colours::black);
            }
            btn->onClick = [this, frameIdx]() {
                // User selected on the View surface (Cells list): Delete now
                // removes only this PLACEMENT, leaving the library waveform.
                owner.activeSelectionSurface =
                    LayeredWaveEditorComponent::SelectionSurface::View;
                owner.switchToFrame(frameIdx);
                rebuildFramesList();
            };
            framesListContainer.addAndMakeVisible(btn.get());
            frameListButtons.push_back(std::move(btn));

            auto del = std::make_unique<FrameDeleteX>();
            if (isEmpty) {
                // Nothing to clear on an already-empty cell. Keep the slot in
                // the layout so columns line up, but make the X inert + faded.
                del->setEnabled(false);
                del->setAlpha(0.0f);
            } else {
                del->setTooltip(isGridSparse ? juce::String("Clear this cell")
                                              : juce::String("Delete this waveform"));
                del->onClick = [this, frameIdx]() {
                    // Remove only this placement (clears a grid cell / erases a
                    // scatter dot); the library entry survives, so the user can
                    // re-place it via "Assign to selected cell" or a Library
                    // drag. Same helper the keyboard Delete View-path uses, so
                    // the X button and Del key can't diverge. The wavetable may
                    // go fully empty - the synth handles that as silence.
                    removePlacementAt(frameIdx);
                };
            }
            framesListContainer.addAndMakeVisible(del.get());
            frameListDeletes.push_back(std::move(del));
        };

        auto coordLabel = [](const std::vector<int>& coord, int fallback1Based) -> juce::String {
            if (coord.empty())        return juce::String(fallback1Based);
            if (coord.size() == 1)    return "[" + juce::String(coord[0]) + "]";
            juce::String inner;
            for (size_t d = 0; d < coord.size(); ++d) {
                if (d) inner << ",";
                inner << coord[d];
            }
            return "(" + inner + ")";
        };

        // Resolve a cell/dot's display name through the library via its
        // waveformId, matching the Library list's convention (entry.name,
        // else "Waveform <libIdx+1>"). Returns empty for an unresolved id.
        auto waveformName = [&](int wid) -> juce::String {
            const int libIdx = owner.wave.findLibraryIndexById(wid);
            if (libIdx < 0) return {};
            const auto& entry = owner.wave.library[(size_t)libIdx];
            return entry.name.empty()
                ? juce::String("Waveform ") + juce::String(libIdx + 1)
                : juce::String(entry.name);
        };

        if (owner.wave.mode == WavetableMode::Grid) {
            for (int i = 0; i < (int)owner.wave.cellWaveformIds.size(); ++i) {
                auto coord = owner.wave.cellIdxToGridCoord(i);
                juce::String label = coordLabel(coord, i + 1);
                const int wid = owner.wave.cellWaveformIds[i];
                const bool isEmpty = (wid < 0);
                if (isEmpty) {
                    label += "  - empty -";
                } else {
                    // Show the waveform that occupies this cell alongside its
                    // coordinate, so the Cells list reads e.g. "(0,0,0)  Bass"
                    // instead of just the coordinate. Falls back to the
                    // positional name for an orphaned id (no library match).
                    juce::String nm = waveformName(wid);
                    label += "  " + (nm.isNotEmpty()
                                         ? nm
                                         : juce::String("Waveform ") + juce::String(i + 1));
                }
                addEntry(label, i, true, isEmpty);
            }
        } else {
            for (int i = 0; i < (int)owner.wave.scatterFrames.size(); ++i) {
                // Resolve the dot's display name through the library via its
                // waveformId, mirroring the Library list's convention
                // (entry.name, else "Waveform <libIdx+1>"). The per-frame
                // `label` field is almost always empty, so the old code fell
                // back to a positional "Waveform <i+1>" that had nothing to do
                // with the actual waveform - e.g. a 2x2x2 grid converted to
                // scatter showed "Waveform 1..8" instead of the real names.
                const int wid = owner.wave.scatterFrames[i].waveformId;
                juce::String label = waveformName(wid);
                if (label.isEmpty()) {
                    // Orphaned dot (no matching library entry). Keep it visible
                    // with a positional fallback so it can still be selected/
                    // deleted rather than vanishing from the list.
                    label = juce::String("Waveform ") + juce::String(i + 1);
                }
                addEntry(label, i, false, false);
            }
        }
        layoutFramesListEntries();
    }

    void rebuildAxisSteppers() {
        for (auto& s : axisSizeSliders) settingsContainer.removeChildComponent(s.get());
        for (auto& l : axisSizeLabels)  settingsContainer.removeChildComponent(l.get());
        axisSizeSliders.clear();
        axisSizeLabels.clear();

        if (owner.wave.mode != WavetableMode::Grid) return;

        auto axName = [](int i) -> juce::String {
            const char* n[] = {"X","Y","Z","W","V","U","T","S"};
            if (i >= 0 && i < (int)(sizeof(n)/sizeof(n[0]))) return n[i];
            return "Axis " + juce::String(i);
        };

        for (size_t d = 0; d < owner.wave.gridDims.size(); ++d) {
            auto lab = std::make_unique<juce::Label>();
            lab->setText(axName((int)d) + ":", juce::dontSendNotification);
            lab->setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            lab->setJustificationType(juce::Justification::centredLeft);
            settingsContainer.addAndMakeVisible(lab.get());

            auto sl = std::make_unique<juce::Slider>(juce::Slider::IncDecButtons,
                                                     juce::Slider::TextBoxLeft);
            sl->setRange(1.0, 64.0, 1.0);
            sl->setIncDecButtonsMode(juce::Slider::incDecButtonsDraggable_AutoDirection);
            sl->setValue((double)owner.wave.gridDims[d], juce::dontSendNotification);
            sl->setTooltip("Number of cells along axis " + axName((int)d) + " (1..64). "
                           "Grow to add more positions along this axis; shrink to drop "
                           "waveforms whose coord on this axis is beyond the new size. "
                           "Growing from 1 to 2 makes the axis traversable, so its "
                           "Position knob and Mod input appear on the synth; shrinking "
                           "back to 1 removes them.");
            int axisIdx = (int)d;
            sl->onValueChange = [this, sl_raw = sl.get(), axisIdx]() {
                int newSize = (int)sl_raw->getValue();
                owner.wave.resizeGridAxis(axisIdx, newSize);
                if (owner.currentFrameIdx >= (int)owner.wave.cellWaveformIds.size()
                    || (owner.currentFrameIdx >= 0
                        && owner.wave.cellWaveformIds[owner.currentFrameIdx] < 0))
                {
                    int next = -1;
                    for (int k = 0; k < (int)owner.wave.cellWaveformIds.size(); ++k)
                        if (owner.wave.cellWaveformIds[k] >= 0) { next = k; break; }
                    owner.currentFrameIdx = std::max(0, next);
                }
                owner.updateHintText();
                owner.rebuildRows();
                owner.onLayerChanged();
                // Deferred: this rebuild deletes the slider we're inside the
                // onValueChange of (axisSizeSliders.clear()). See
                // scheduleRefreshAfterDocMutation() for the use-after-free.
                scheduleRefreshAfterDocMutation();
            };

            settingsContainer.addAndMakeVisible(sl.get());
            axisSizeLabels.push_back(std::move(lab));
            axisSizeSliders.push_back(std::move(sl));
        }
    }

    // Build (or rebuild after a dimension change) one slider per plane (i, j),
    // i < j, of the current view N-D space. Labels show the human-readable
    // axis names so a 4-D wavetable's "X-W" plane is identifiable at a glance.
    // Uses numDimensions() so Grid-mode axis adds produce the right plane set.
    void rebuildSliders() {
        for (auto& s : planeSliders) settingsContainer.removeChildComponent(s.get());
        for (auto& l : planeLabels)  settingsContainer.removeChildComponent(l.get());
        planeSliders.clear();
        planeLabels.clear();

        int N = std::max(2, owner.wave.numDimensions());
        auto pairs = LayeredWaveEditorComponent::scatterPlanePairs(N);

        auto axName = [](int i) -> juce::String {
            const char* n[] = {"X","Y","Z","W","V","U","T","S"};
            if (i >= 0 && i < (int)(sizeof(n)/sizeof(n[0]))) return n[i];
            return "Axis " + juce::String(i);
        };

        for (const auto& ij : pairs) {
            auto lab = std::make_unique<juce::Label>();
            lab->setText(axName(ij.first) + "-" + axName(ij.second), juce::dontSendNotification);
            lab->setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            lab->setJustificationType(juce::Justification::centredLeft);
            settingsContainer.addAndMakeVisible(lab.get());

            auto sl = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal,
                                                     juce::Slider::TextBoxRight);
            sl->setRange(-180.0, 180.0, 0.0);
            sl->setTextValueSuffix(juce::String::fromUTF8("\xC2\xB0")); // degrees
            sl->setNumDecimalPlacesToDisplay(1);
            sl->setValue(owner.getScatterPlaneAngle(ij.first, ij.second),
                         juce::dontSendNotification);
            int i = ij.first, j = ij.second;
            sl->setTooltip("Rotation angle in the " + axName(i) + "-" + axName(j) +
                           " plane (degrees). Drag to orbit the N-D view in this "
                           "plane. Visible regardless of which axes are currently "
                           "shown in the projection, so you can rotate axes out "
                           "of the projection plane and back in.");
            // Route slider drags through the owner's setter so the embedded
            // ScatterView in the main editor repaints too. We intentionally
            // do NOT call notifyScatterViewRotated() here - that refreshes
            // our own sliders, which would be redundant (we're the source
            // of this change) and could fight the live drag state.
            sl->onValueChange = [this, sl_raw = sl.get(), i, j]() {
                owner.setScatterPlaneAngle(i, j, (float)sl_raw->getValue());
            };

            settingsContainer.addAndMakeVisible(sl.get());
            planeLabels.push_back(std::move(lab));
            planeSliders.push_back(std::move(sl));
        }
    }

    // Which axes get a "Selected waveform position" control. This is the set
    // of GEOMETRIC axes of the active mode (numDimensions()), NOT the
    // traversable set (effectiveAxes):
    //   Grid: every axis (single-cell axes included, so the "+ at the end"
    //         grow affordance keeps working from this strip).
    //   Scatter: every scatterDims axis, so the selected dot can be placed
    //         along each visible axis - including a lone dot on a 1-D line.
    // Deliberately decoupled from the synth's Position knobs (which DO track
    // effectiveAxes): placement is an editor concern, and moving dots apart on
    // a geometric axis is exactly how the user *creates* a traversable axis.
    // Tying the strip to effectiveAxes produced a chicken-and-egg dead end - a
    // lone dot had no slider, so there was no way to position it (or to spread
    // a second dot away from it numerically) to make the axis traversable.
    std::vector<int> selFrameAxisList() const {
        const int N = std::max(0, owner.wave.numDimensions());
        std::vector<int> v((size_t)N);
        for (int d = 0; d < N; ++d) v[(size_t)d] = d;
        return v;
    }

    // -- Per-axis position controls for the currently selected frame.
    //    Scatter mode: continuous 0..1 sliders, one per traversable dimension.
    //    Grid mode: IncDec steppers, one per axis, picking the cell coord.
    //    Moving onto an occupied Grid cell swaps the two frames. --
    void rebuildSelFrameControls() {
        for (auto& s : selFrameSliders) settingsContainer.removeChildComponent(s.get());
        for (auto& l : selFrameLabels)  settingsContainer.removeChildComponent(l.get());
        selFrameSliders.clear();
        selFrameLabels.clear();

        const std::vector<int> axes = selFrameAxisList();
        const bool isGrid = (owner.wave.mode == WavetableMode::Grid);

        auto axName = [](int i) -> juce::String {
            const char* n[] = {"X","Y","Z","W","V","U","T","S"};
            if (i >= 0 && i < (int)(sizeof(n)/sizeof(n[0]))) return n[i];
            return juce::String(i);
        };

        for (int di = 0; di < (int)axes.size(); ++di) {
            const int d = axes[(size_t)di];  // geometric axis index
            auto lab = std::make_unique<juce::Label>();
            lab->setText(axName(d) + ":", juce::dontSendNotification);
            lab->setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            lab->setJustificationType(juce::Justification::centredLeft);
            settingsContainer.addAndMakeVisible(lab.get());
            selFrameLabels.push_back(std::move(lab));

            auto sl = std::make_unique<juce::Slider>();
            if (isGrid) {
                int axisSize = (d < (int)owner.wave.gridDims.size())
                                  ? owner.wave.gridDims[(size_t)d] : 1;
                axisSize = std::max(1, axisSize);
                // Allow stepping ONE past the current end of the axis: the
                // value-change handler grows the axis to fit. This is what
                // makes "I added an axis (size 1), now press + to use it"
                // work without the user having to discover the separate
                // axis-size stepper first.
                int stepperMax = std::min(64, axisSize + 1);
                sl->setSliderStyle(juce::Slider::IncDecButtons);
                sl->setIncDecButtonsMode(juce::Slider::incDecButtonsDraggable_Vertical);
                sl->setTextBoxStyle(juce::Slider::TextBoxLeft, false, 50, 18);
                sl->setRange(1.0, (double)stepperMax, 1.0);
                sl->setValue(1.0, juce::dontSendNotification);
                sl->setTooltip("Cell position of the selected waveform on axis " + axName(d) +
                               " (1.." + juce::String(axisSize) + "). Press + at the end "
                               "to grow the axis by one cell. Moving onto an occupied cell "
                               "swaps the two waveforms.");
                int axisIdx = d;
                sl->onValueChange = [this, sl_raw = sl.get(), axisIdx]() {
                    handleSelFrameGridChange(axisIdx, (int)sl_raw->getValue() - 1);
                };
            } else {
                sl->setSliderStyle(juce::Slider::LinearHorizontal);
                sl->setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 18);
                sl->setRange(0.0, 1.0, 0.001);
                sl->setValue(0.5, juce::dontSendNotification);
                sl->setTooltip("Selected waveform's coordinate on axis " + axName(d) +
                               " (0..1). Equivalent to dragging the dot in the view, "
                               "but works on axes that aren't currently projected.");
                int axisIdx = d;
                sl->onValueChange = [this, sl_raw = sl.get(), axisIdx]() {
                    handleSelFrameScatterChange(axisIdx, (float)sl_raw->getValue());
                };
            }
            settingsContainer.addAndMakeVisible(sl.get());
            selFrameSliders.push_back(std::move(sl));
        }
        refreshSelFrameValues();
    }

    // Push the selected frame's position into the slider widgets without
    // firing onValueChange (we'd just write back what we read).
    void refreshSelFrameValues() {
        const std::vector<int> axes = selFrameAxisList();
        const int N = (int)axes.size();
        if ((int)selFrameSliders.size() != N) {
            rebuildSelFrameControls();
            layoutSettingsContainer();
            return;
        }
        const int idx = owner.currentFrameIdx;
        selFrameSectionLabel.setText("Selected waveform position (#" +
                                     juce::String(idx + 1) + "):",
                                     juce::dontSendNotification);
        if (owner.wave.mode == WavetableMode::Scatter) {
            const ScatterFrame* sf = (idx >= 0 && idx < (int)owner.wave.scatterFrames.size())
                                       ? &owner.wave.scatterFrames[(size_t)idx] : nullptr;
            for (int di = 0; di < N; ++di) {
                const int d = axes[(size_t)di];  // geometric axis
                float v = (sf && d < (int)sf->position.size()) ? sf->position[(size_t)d] : 0.5f;
                selFrameSliders[(size_t)di]->setValue((double)v, juce::dontSendNotification);
            }
        } else {
            std::vector<int> coord;
            if (idx >= 0 && idx < owner.wave.gridCellCount())
                coord = owner.wave.cellIdxToGridCoord(idx);
            for (int di = 0; di < N; ++di) {
                const int d = axes[(size_t)di];  // geometric axis
                int sz = (d < (int)owner.wave.gridDims.size())
                            ? owner.wave.gridDims[(size_t)d] : 1;
                sz = std::max(1, sz);
                int c = (d < (int)coord.size()) ? coord[(size_t)d] : 0;
                // Update range too: the axis size may have changed since
                // the last build via the per-axis steppers. Same +1 trick
                // as in rebuildSelFrameControls so + at the end grows.
                int stepperMax = std::min(64, sz + 1);
                selFrameSliders[(size_t)di]->setRange(1.0, (double)stepperMax, 1.0);
                selFrameSliders[(size_t)di]->setValue((double)(c + 1),
                                                      juce::dontSendNotification);
            }
        }
    }

    void handleSelFrameScatterChange(int axisIdx, float v) {
        const int idx = owner.currentFrameIdx;
        if (idx < 0 || idx >= (int)owner.wave.scatterFrames.size()) return;
        auto& sf = owner.wave.scatterFrames[(size_t)idx];
        while ((int)sf.position.size() <= axisIdx) sf.position.push_back(0.5f);
        sf.position[(size_t)axisIdx] = juce::jlimit(0.0f, 1.0f, v);
        owner.onLayerChanged();
        // Moving a frame off its snapshotted cell center disqualifies the
        // lossless "Back to Grid" path - re-evaluate so the button greys
        // out the moment the user drags this slider.
        updateConvertButton();
        view->repaint();
    }

    void handleSelFrameGridChange(int axisIdx, int newCoord) {
        const int idx = owner.currentFrameIdx;
        if (idx < 0 || idx >= (int)owner.wave.cellWaveformIds.size()) return;
        if (axisIdx < 0 || axisIdx >= (int)owner.wave.gridDims.size()) return;
        if (newCoord < 0) return;

        auto srcCoord = owner.wave.cellIdxToGridCoord(idx);
        if ((int)srcCoord.size() <= axisIdx) return;
        if (srcCoord[(size_t)axisIdx] == newCoord) return;

        // If the user stepped past the current end of this axis, grow it
        // just enough so the requested coord becomes valid. This is what
        // makes a freshly-added (size-1) axis immediately usable - "+" at
        // the end means "extend the axis", matching how + Frame grows axis
        // 0 when every cell is full.
        if (newCoord >= owner.wave.gridDims[(size_t)axisIdx]) {
            int newSize = newCoord + 1;
            if (newSize > 64) return;  // hard cap matches axis-size stepper
            owner.wave.resizeGridAxis(axisIdx, newSize);
            // resizeGridAxis preserves frames by coord but row-major
            // indices change, so the size sliders need to refresh too.
            rebuildAxisSteppers();
        }

        // Recompute source / dest cell indices in the (possibly resized)
        // grid. Source coord didn't change; dest coord differs only on
        // axisIdx (set to newCoord).
        int srcIdx = owner.wave.gridCoordToCellIdx(srcCoord);
        auto destCoord = srcCoord;
        destCoord[(size_t)axisIdx] = newCoord;
        int destIdx = owner.wave.gridCoordToCellIdx(destCoord);
        if (srcIdx < 0 || destIdx < 0
            || srcIdx >= (int)owner.wave.cellWaveformIds.size()
            || destIdx >= (int)owner.wave.cellWaveformIds.size()) return;

        // Swap source and destination cell ids. This handles both move-to-
        // empty (dest was -1) and move-to-occupied (the displaced cell ref
        // ends up where the source used to be) uniformly. The selection
        // follows the user's intent and goes with the moved frame.
        std::swap(owner.wave.cellWaveformIds[(size_t)srcIdx],
                  owner.wave.cellWaveformIds[(size_t)destIdx]);
        owner.currentFrameIdx = destIdx;
        // Selection moved to the destination cell. If it now holds a
        // library entry, sync the editor target so the right pane stays
        // pointed at the waveform the user just dragged.
        {
            const int destLibId = owner.wave.libraryIdForCell(destIdx);
            if (destLibId >= 0) owner.currentLibraryId = destLibId;
        }
        owner.wave.scatterFromGridSnapshot.reset();
        owner.updateHintText();
        owner.rebuildRows();
        owner.onLayerChanged();
        // Deferred: refreshAfterDocMutation() rebuilds selFrameSliders, deleting
        // the stepper whose onValueChange invoked us. See
        // scheduleRefreshAfterDocMutation() for the use-after-free rationale.
        scheduleRefreshAfterDocMutation();
    }

    LayeredWaveEditorComponent& owner;
    juce::ComboBox stereoModeCombo;

    // Library list (scroll). Shows ALL library entries (used and unused),
    // each with a "(used N\u00D7)" suffix when placed. This is the canonical
    // list of waveform DATA in the wavetable - cells are placements that
    // reference these entries by id. Add / edit / delete operate here at
    // the data layer; placement on the arrangement view operates at the
    // cell layer (existing Cells list below).
    juce::Label     librarySectionLabel;
    juce::Viewport  libraryListViewport;
    juce::Component libraryListContainer;
    juce::TextButton addLibraryEntryBtn { "+ Waveform" };
    juce::TextButton assignToCellBtn { "Assign to selected cell" };
    std::vector<std::unique_ptr<juce::TextButton>> libraryRowButtons;
    std::vector<std::unique_ptr<FrameDeleteX>>     libraryRowDeletes;
    // Colour swatches, one per library row. Click pops the palette menu
    // and updates owner.wave.library[i].colorIdx. Same vector layout as
    // libraryRowButtons so indices line up.
    std::vector<std::unique_ptr<LibraryColorSwatch>> libraryRowSwatches;
    // The Library row selection IS the editor target (owner.currentLibraryId).
    // No separate sidebar-only state - clicking a row focuses the editor on
    // that entry, and the highlighted row is always the one being edited.

    // Frames list (scroll) - the placements list (cells in Grid mode,
    // scatter dots in Scatter mode). Distinct from the library: a row
    // here represents a cell, not a waveform.
    juce::Label     framesListLabel;
    juce::Viewport  framesListViewport;
    juce::Component framesListContainer;
    std::vector<std::unique_ptr<juce::TextButton>> frameListButtons;
    std::vector<std::unique_ptr<FrameDeleteX>>     frameListDeletes;

    juce::TextButton convertBtn;

    // Scrollable settings area. Everything below the Convert button row and
    // above the Reset rotation button lives inside settingsContainer so a
    // tall N-D wavetable (many axes, many rotation planes) can scroll
    // instead of being clipped.
    juce::Viewport  settingsViewport;
    juce::Component settingsContainer;

    // Grid axes section (lives inside settingsContainer)
    juce::Label gridAxesLabel;
    juce::TextButton addAxisBtn { "+ Axis" };
    juce::TextButton removeAxisBtn { "- Axis" };
    std::vector<std::unique_ptr<juce::Slider>> axisSizeSliders;
    std::vector<std::unique_ptr<juce::Label>>  axisSizeLabels;

    // Scatter section (lives inside settingsContainer)
    juce::Label  radiusLabel;
    juce::Slider radiusSlider;
    juce::ToggleButton absBlendToggle;

    // Selected-frame position section (lives inside settingsContainer).
    // In Scatter mode the sliders edit scatterFrames[currentFrameIdx].position
    // directly; in Grid mode they're IncDec steppers that move the selected
    // frame between cells (swapping with whatever is in the destination).
    juce::Label selFrameSectionLabel;
    juce::TextButton addToGridBtn { "Add to grid" };
    std::vector<std::unique_ptr<juce::Slider>> selFrameSliders;
    std::vector<std::unique_ptr<juce::Label>>  selFrameLabels;

    // Rotation section (lives inside settingsContainer)
    juce::Label rotationSectionLabel;
    std::vector<std::unique_ptr<juce::Slider>> planeSliders;
    std::vector<std::unique_ptr<juce::Label>>  planeLabels;

    // Reset rotation pinned to the bottom of the sidebar (NOT in the
    // scrollable area, so it's always reachable).
    juce::TextButton resetRotBtn { "Reset rotation" };
};

// ==============================================================================
// LayeredWaveEditorComponent
// ==============================================================================

// Registry of every live wavetable editor. The editors are non-modal
// DialogWindows the main window doesn't own (see launchNonModalToolDialog), so
// when an undo/redo snapshot restore rewrites a node's script the main window
// has no handle to the editor bound to it. Each editor registers itself here on
// construction and removes itself on destruction; reloadOpenEditorsAfterSnapshot
// walks the list and refreshes the matching editors. GUI-thread only, so a plain
// vector with no locking is safe.
static std::vector<LayeredWaveEditorComponent*>& openWaveEditors() {
    static std::vector<LayeredWaveEditorComponent*> s;
    return s;
}

void LayeredWaveEditorComponent::reloadOpenEditorsAfterSnapshot(NodeGraph& g) {
    // Copy the registry first: reloadFromNode() can close an editor (when its
    // node was undone away), which mutates openWaveEditors() mid-iteration.
    auto snapshot = openWaveEditors();
    for (auto* ed : snapshot)
        if (ed && &ed->graph == &g)
            ed->reloadFromNode();
}

LayeredWaveEditorComponent::LayeredWaveEditorComponent(NodeGraph& g, int nid, std::function<void()> apply)
    : graph(g), nodeId(nid), onApply(std::move(apply))
{
    openWaveEditors().push_back(this);
    // Decode existing state. Try wavetable first, then fall back to single
    // layered waveform (wrapped as a 1-frame wavetable), then default sine.
    auto* nd = graph.findNode(nodeId);
    std::string script = nd ? nd->script : "";
    if (!wave.decode(script)) {
        LayeredWaveform single;
        if (single.decode(script)) {
            wave.tableSize = single.tableSize;
            const int libId = wave.addLibraryEntry(
                std::make_unique<LayeredWaveform>(std::move(single)));
            wave.gridDims = {1};
            wave.cellWaveformIds.push_back(libId);
        } else {
            wave = WavetableDoc::defaultSingleSine();
        }
    }
    // Note: we no longer auto-seed a default sine into an empty Grid
    // wavetable. The "Wavetable" menu entry on the node graph opens the
    // editor with a deliberately empty WavetableDoc (one null cell), and
    // the user picks the first waveform's type via "+ Waveform". The
    // Scatter seed below is still kept for now because Scatter mode is
    // only reached by explicit user conversion, and an empty scatter
    // wavetable has no visible affordances yet.
    if (wave.mode == WavetableMode::Scatter && wave.scatterFrames.empty()) {
        // Seed with two frames so the user can immediately see the morph.
        // Both reference fresh library entries.
        const int idA = wave.addLibraryEntry(
            std::make_unique<LayeredWaveform>(LayeredWaveform::defaultSine()));
        const int idB = wave.addLibraryEntry(
            std::make_unique<LayeredWaveform>(LayeredWaveform::defaultSine()));
        ScatterFrame a; a.waveformId = idA;
        a.position.assign((size_t)std::max(0, wave.scatterDims), 0.5f);
        if (!a.position.empty()) a.position[0] = 0.25f;
        ScatterFrame b; b.waveformId = idB;
        b.position.assign((size_t)std::max(0, wave.scatterDims), 0.5f);
        if (!b.position.empty()) b.position[0] = 0.75f;
        wave.scatterFrames.push_back(std::move(a));
        wave.scatterFrames.push_back(std::move(b));
    }
    // Default to the first non-empty cell so currentFrameIdx points at a
    // real frame whenever one exists. For a fully-empty wavetable
    // (defaultEmpty()), leave currentFrameIdx at 0 - it points at the
    // single empty cell, which updateHintText / currentLayers /
    // updateFrameEditorEmbed all handle as "no current frame".
    currentFrameIdx = 0;
    if (wave.mode == WavetableMode::Grid) {
        for (int k = 0; k < (int)wave.cellWaveformIds.size(); ++k)
            if (wave.cellWaveformIds[k] >= 0) { currentFrameIdx = k; break; }
    }
    // Seed the editor target: prefer the library entry referenced by the
    // initially-selected cell. If the doc is mid-load with empty cells but
    // a populated library (e.g. orphaned entries), fall back to the first
    // library entry so the editor has SOMETHING to edit. -1 means "no
    // entry yet" - paint() shows a placeholder in that case.
    currentLibraryId = -1;
    {
        const int seedFromCell = wave.libraryIdForCell(currentFrameIdx);
        if (seedFromCell >= 0) currentLibraryId = seedFromCell;
        else if (!wave.library.empty()) currentLibraryId = wave.library.front().id;
    }
    currentPosition.assign(std::max(1, wave.numDimensions()), 0.5f);

    // Shared layer-stack widget (same code the Signal Shape editor uses).
    // Summation preview OFF: this editor renders its own multi-frame-type
    // preview strip below. New layers default to the next harmonic at half
    // amplitude, matching the old inline "+ Layer" behaviour.
    {
        LayerStackComponent::Options lsOpts;
        lsOpts.showSummationPreview = false;
        lsOpts.addLayerButtonText = "+ Layer";
        lsOpts.makeNewLayer = [](int count) {
            WaveLayer l;
            l.shape = WaveLayer::Sine;
            l.ratio = count + 1; // each new layer defaults to next harmonic
            l.phase = 0.0f;
            l.amp   = 0.5f;
            return l;
        };
        layerStack = std::make_unique<LayerStackComponent>(
            std::move(lsOpts), [this]() { onLayerChanged(); });
        addChildComponent(*layerStack); // visibility toggled in resized()
    }

    // The "+ Waveform" button used to live up here on the top toolbar.
    // It now lives in the arrangement-view sidebar (below the Library
    // list), so the user finds it next to the library it adds to instead
    // of detached up in the title bar. The popup-menu flow is identical;
    // see WavetableViewWindowContent::addLibraryEntryBtn and the call
    // to showAddWaveformMenu it makes.

    // Build the embedded arrangement view. It owns the ScatterView and
    // the sidebar (Library list, Cells list, mode-conversion, axis
    // steppers, RBF radius, per-axis position controls, and N-D rotation
    // sliders). Always visible - it occupies the left half of the dialog
    // body in the side-by-side layout.
    arrangementView = std::make_unique<WavetableViewWindowContent>(*this);
    addAndMakeVisible(arrangementView.get());

    // Auto-detect screen DPI from JUCE so the parallax math is calibrated
    // to the user's actual display.
    if (arrangementView && arrangementView->view) {
        auto& displays = juce::Desktop::getInstance().getDisplays();
        const auto* main = displays.getPrimaryDisplay();
        if (main && main->dpi > 1.0)
            arrangementView->view->dpi = (float)main->dpi;
    }

    addAndMakeVisible(helpBtn);
    helpBtn.setTooltip("Open the wavetable / layered waveform docs");
    helpBtn.onClick = []() { openHelpDocFile("wavetables.html"); };

    // Discoverability hint: appears below the toolbar when the wavetable
    // still has just one frame, since the "Waveform / Wavetable" menu name
    // doesn't make the multi-frame N-D arrangement capability obvious.
    // Wording aims to make clear that the Position control lives on the
    // synth node OUT in the graph, NOT inside this editor. Earlier
    // revisions buried "on the synth node in the graph" mid-sentence and
    // it kept reading as if Position were one of this editor's controls,
    // so this version puts the location up front ("Back in the node
    // graph, ...") on its own sentence.
    addAndMakeVisible(hintLabel);
    hintLabel.setText("Tip: this is a wavetable editor - click  + Waveform  to add more waveforms, "
                      "then arrange them in the grid / scatter view below. Use  + Axis  in the "
                      "sidebar to add more arrangement axes. "
                      "Back out in the node graph, the synth node itself has a Position knob "
                      "(or Position 1, 2, ... once you add more axes) that morphs between waveforms.",
                      juce::dontSendNotification);
    hintLabel.setJustificationType(juce::Justification::centredLeft);
    hintLabel.setColour(juce::Label::textColourId,
                        juce::Colours::white.withAlpha(0.55f));
    hintLabel.setFont(juce::Font(juce::FontOptions(12.0f).withStyle("italic")));
    hintLabel.setInterceptsMouseClicks(false, false);
    // Visibility is reapplied whenever frames change (in updateHintText)
    // and on first layout (in resized).
    hintLabel.setVisible(wave.cellWaveformIds.size() <= 1);

    // Compare (#9): flip the synth between two render modes that both
    // apply to a wavetable cycle, so the same waveform can be A/B
    // auditioned. Sets the Synth Mode param: 0 = Direct (SamplePerPoint),
    // 2 = Additive bank. The third Synth Mode (1 = AM-sine) is not exposed
    // here because it's a terrain-sonification mode for 2D/N-D terrains,
    // not a wavetable render mode.
    addAndMakeVisible(compareLabel);
    compareLabel.setFont(11.0f);
    compareLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.75f));
    compareLabel.setTooltip(
        "How this wavetable cycle is rendered to audio. Direct plays the "
        "cycle straight through one oscillator (classic wavetable - fast "
        "and faithful to what you authored). Additive bank FFTs the cycle "
        "into its harmonic series and re-synthesises it as a sum of "
        "independent sine partials - more CPU, but it doesn't alias when "
        "pitched up and lets each partial be modulated separately (planned).");

    addAndMakeVisible(compareDirectBtn);
    addAndMakeVisible(compareAdditiveBtn);
    compareDirectBtn.setTooltip(
        "Direct (classic wavetable): the cycle is read by one oscillator "
        "and the table value IS the audio sample. Fast and faithful to "
        "what you authored.");
    compareAdditiveBtn.setTooltip(
        "Additive bank: the cycle is FFT'd to extract up to 64 harmonic "
        "amplitudes and phases, and each partial is then synthesised as "
        "its own sine oscillator running at fundamental x harmonic number. "
        "For a static cycle this sounds nearly identical to Direct mode, "
        "but it doesn't alias when you pitch up, partials can be modulated "
        "individually (future feature), and it costs ~N times more CPU "
        "where N is the partial count.");

    auto setSynthMode = [this](float v) {
        if (auto* nd = graph.findNode(nodeId))
            for (auto& p : nd->params)
                if (p.name == "Synth Mode") { p.value = v; break; }
    };
    auto refreshCompareTint = [this]() {
        // Read the Synth Mode param and highlight whichever of the two
        // compare buttons currently matches. If it's AM-sine (1), neither
        // highlights - the buttons just look idle. That's fine: AM-sine is
        // selected from the node's main param row, not from this editor.
        float mv = 0.0f;
        if (auto* nd = graph.findNode(nodeId))
            for (auto& p : nd->params)
                if (p.name == "Synth Mode") { mv = p.value; break; }
        int m = juce::jlimit(0, 2, (int)std::round(mv));
        juce::Colour green = juce::Colour(60, 100, 60);
        juce::Colour blue  = juce::Colour(60, 60, 100);
        juce::Colour idle  = juce::Colour(55, 55, 60);
        compareDirectBtn  .setColour(juce::TextButton::buttonColourId,
                                     (m == 0) ? green : idle);
        compareAdditiveBtn.setColour(juce::TextButton::buttonColourId,
                                     (m == 2) ? blue  : idle);
    };
    compareDirectBtn.onClick = [setSynthMode, refreshCompareTint]() {
        setSynthMode(0.0f); // Direct (SamplePerPoint)
        refreshCompareTint();
    };
    compareAdditiveBtn.onClick = [setSynthMode, refreshCompareTint]() {
        setSynthMode(2.0f); // Additive bank
        refreshCompareTint();
    };
    refreshCompareTint();

    addAndMakeVisible(applyBtn);
    applyBtn.setButtonText("Apply");
    applyBtn.setTooltip("Save the current waveform edits to the synth without closing this editor");
    applyBtn.onClick = [this]() {
        commitToNode();
        if (onApply) onApply();
        commitUndoStep();
    };

    addAndMakeVisible(closeBtn);
    closeBtn.setButtonText("Close");
    closeBtn.onClick = [this]() {
        // Commit on close too so work isn't lost by accident. The undo step
        // must be pushed synchronously here - the timer can't fire after the
        // dialog is destroyed below.
        commitToNode();
        if (onApply) onApply();
        commitUndoStep();
        // Single-window editor: just delete our parent dialog. The
        // arrangement view is an embedded child of this component and
        // dies with us.
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
            juce::Component::SafePointer<juce::DialogWindow> safe(dw);
            juce::MessageManager::callAsync([safe]() {
                if (safe) delete safe.getComponent();
            });
        }
    };

    // ---- Per-waveform identity row (top of right pane) ----
    // Tiny colour swatch + name TextEditor for the library entry the
    // editor is currently bound to. Hidden when no library entry is
    // targeted (empty library) - paint() shows a placeholder in that case.
    addAndMakeVisible(identityLabel);
    identityLabel.setFont(11.0f);
    identityLabel.setColour(juce::Label::textColourId,
                            juce::Colours::white.withAlpha(0.75f));
    identityLabel.setJustificationType(juce::Justification::centredRight);

    nameColorSwatch = std::make_unique<LibraryColorSwatch>();
    addAndMakeVisible(nameColorSwatch.get());
    nameColorSwatch->setTooltip(
        "Click to colour-code this waveform. Auto picks a colour from "
        "the waveform's harmonic content. Every cell and dot referencing "
        "this waveform will use the picked colour.");
    nameColorSwatch->onPick = [this](int idx) {
        const int libIdx = wave.findLibraryIndexById(currentLibraryId);
        if (libIdx < 0) return;
        wave.library[libIdx].colorIdx = idx;
        // Push to node script so the colour persists across reloads and
        // ensure every observer (arrangement view, library list, popout)
        // repaints with the new colour.
        commitToNode();
        commitUndoStep();
        refreshIdentityRow();
        if (arrangementView) {
            arrangementView->rebuildLibraryList();
            arrangementView->view->repaint();
        }
        notifyPopoutDocMutated();
    };

    addAndMakeVisible(nameEditor);
    nameEditor.setMultiLine(false);
    nameEditor.setReturnKeyStartsNewLine(false);
    nameEditor.setFont(juce::Font(juce::FontOptions(13.0f)));
    nameEditor.setTextToShowWhenEmpty("(unnamed waveform)",
                                      juce::Colours::grey);
    nameEditor.setTooltip(
        "Rename this waveform. The name shows up in the Library list and "
        "in tooltips wherever this waveform is placed. Press Enter to commit.");
    // Match the app's other input fields: clicking in selects the whole name so
    // you can immediately type a replacement instead of editing character-by-char.
    nameEditor.setSelectAllWhenFocused(true);
    auto commitNameEdit = [this]() {
        setLibraryEntryName(currentLibraryId, nameEditor.getText().toStdString());
    };
    // Enter commits AND drops keyboard focus so the field visibly "quits editing"
    // (the focus-loss re-commit is a harmless no-op — setLibraryEntryName early-
    // outs when the name is unchanged). Clicking anywhere outside the field also
    // commits via onFocusLost.
    nameEditor.onReturnKey = [this, commitNameEdit]() {
        commitNameEdit();
        nameEditor.giveAwayKeyboardFocus();
    };
    nameEditor.onFocusLost  = commitNameEdit;

    // ---- Per-waveform gain knob ----
    addAndMakeVisible(gainLabel);
    gainLabel.setFont(11.0f);
    gainLabel.setColour(juce::Label::textColourId,
                        juce::Colours::white.withAlpha(0.75f));
    gainLabel.setJustificationType(juce::Justification::centredRight);

    addAndMakeVisible(gainSlider);
    // dragMax (4.0) is the slider's draggable ceiling; the value range goes
    // higher (up to 64x, a safety bound that prevents absurd / NaN values)
    // so the text box can accept typed figures above 4. Values over 4 pin the
    // thumb at the right end - see FreeEntrySlider.
    gainSlider.dragMax = 4.0;
    gainSlider.setRange(0.0, 64.0, 0.001);
    gainSlider.setDoubleClickReturnValue(true, 1.0);
    gainSlider.setNumDecimalPlacesToDisplay(2);
    gainSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 18);
    gainSlider.setTooltip(
        "Output volume of THIS waveform, multiplied on top of the cycle "
        "(1.00 = unchanged). Every waveform is peak-normalised to the same "
        "loudness, so this is how you make one frame louder or quieter than "
        "its neighbours. The slider drags from 0 to 4x; type a number in the "
        "box for higher or lower values. It changes the actual samples, so it "
        "shows in the preview and in morphs between frames. Double-click to "
        "reset to 1.00.");
    gainSlider.onValueChange = [this]() {
        auto* f = currentEditingFrame();
        if (!f) return;
        f->gain = (float)gainSlider.getValue();
        // Live: re-render preview + push to node so audio reflects it while
        // dragging.
        refreshPreview();
        commitToNode();
        notifyPopoutDocMutated();
        // Undo: during a drag, defer the snapshot to drag end (one Ctrl+Z per
        // sweep). For non-drag changes (textbox typing, double-click reset)
        // there is no drag end, so commit immediately.
        if (!gainSlider.isMouseButtonDown())
            commitUndoStep();
    };
    gainSlider.onDragEnd = [this]() { commitUndoStep(); };

    updateHintText();
    rebuildScatterUI();
    rebuildRows();
    refreshPreview();
    syncPositionParams();
    // Sized to fit a 1080p display with room for the OS taskbar and the
    // dialog's own non-native title bar (~30px). Side-by-side layout:
    // arrangement view (~960 px) on the left, per-waveform editor on
    // the right (~440 px), with margins and a gap.
    setSize(1500, 780);
}

// Library-name helpers defined later in this file (after the capture code).
// Forward-declared here so showAddWaveformMenu's "+ Waveform" handler can
// name freshly-inserted scratch/duplicate entries the same way captures are.
static std::string frameTypeName(const IWavetableFrame* frame);
static void applyLibraryIdSuffix(WavetableDoc& doc, int libId,
                                 const std::string& base);

void LayeredWaveEditorComponent::showAddWaveformMenu(juce::Component* anchor) {
    // Anchor must be on-screen - the popup menu attaches itself to its
    // bounds. The normal caller is the sidebar's "+ Waveform" button on
    // WavetableViewWindowContent; if the main editor is hidden (acting as
    // the state owner behind a pop-out wavetable view), the pop-out's own
    // "+ Waveform" button passes itself instead. There is no longer an
    // editor-owned fallback button to use when anchor is null - the
    // top-bar "+ Waveform" was removed in favour of the sidebar version.
    // Bail rather than crash if a future caller forgets to supply one.
    if (anchor == nullptr) {
        jassertfalse;
        return;
    }

    // Popup menu - pick the type of the new waveform. Item 1 is the
    // legacy duplicate-current behavior; items 2/3/4 insert a fresh
    // default of each editor-authored type; items 5/6/7 capture from
    // an audio source. Mixed-type wavetables are the whole point: you
    // can sit a spectral, wavelet or captured waveform alongside
    // layered waveforms inside one wavetable.
    juce::PopupMenu m;
    m.addSectionHeader("Edit from scratch");
    m.addItem(2, "Layered (time domain)");
    m.addItem(3, "Frequency Domain (FFT)");
    m.addItem(4, "Wavelet Space (DWT)");
    m.addSeparator();
    m.addSectionHeader("Capture from audio");
    // Items 5/6/7: open the capture dialog with one of three audio
    // sources. Each produces N captured waveforms (one per equally-
    // spaced position in a region) and lays them along Position so
    // the synth morphs through them.
    m.addItem(5, "From project song...");
    m.addItem(6, "From microphone / audio input...");
    m.addItem(7, "From audio file...");
    // Duplicate only makes sense when there's a current waveform to
    // duplicate - hide it on an empty wavetable.
    const bool haveCurrent = (wave.mode == WavetableMode::Grid)
        ? (currentFrameIdx >= 0
            && currentFrameIdx < (int)wave.cellWaveformIds.size()
            && wave.cellWaveformIds[currentFrameIdx] >= 0)
        : (currentFrameIdx >= 0
            && currentFrameIdx < (int)wave.scatterFrames.size());
    if (haveCurrent) {
        m.addSeparator();
        m.addItem(1, "Duplicate current waveform");
    }

    auto makeFreshFrame = [](int r) -> std::unique_ptr<IWavetableFrame> {
        if (r == 2) return std::make_unique<LayeredWaveform>(LayeredWaveform::defaultSine());
        if (r == 3) return std::make_unique<SpectralFrame>(SpectralDoc::defaultBuiltin());
        if (r == 4) return std::make_unique<WaveletFrame>(WaveletFrame::defaultEmpty());
        return nullptr;
    };

    m.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(anchor),
        [this, makeFreshFrame](int r) {
            if (r == 0) return;
            if (r == 5 || r == 6 || r == 7) {
                // Capture-from-audio entries. The capture UI lives inline
                // in the right pane (same screen real estate as the
                // layered / spectral / wavelet editors) rather than in a
                // separate window. It produces N frames on Save and we
                // then lay them out along Position; Close just tears the
                // panel down.
                showCapturePanelInline(r - 5);
                return;
            }
            // "+ Waveform" only adds an entry to the LIBRARY. It does NOT
            // automatically place it into a grid cell or create a scatter
            // dot - the user does that by dragging the library row onto
            // the arrangement view. Earlier versions auto-placed (first
            // empty cell in Grid mode, centre-of-cube ScatterFrame in
            // Scatter mode), but that conflated two distinct operations:
            // "I want this waveform to exist" vs "I want this waveform
            // to occupy this location". The library/arrangement split
            // is the whole point of the Library list being a separate
            // panel, so + Waveform respects it.
            std::unique_ptr<IWavetableFrame> nf;
            if (r == 1) {
                // Duplicate current: clone the waveform the editor is
                // currently focused on (the library entry, not the
                // selected cell). The clone becomes its own library
                // entry so the two can be edited independently.
                if (auto* f = currentEditingFrame())
                    nf = f->clone();
                else
                    nf = std::make_unique<LayeredWaveform>(LayeredWaveform::defaultSine());
            } else {
                nf = makeFreshFrame(r);
            }
            if (!nf) return;

            // Name the entry after its editor type ("Layered 5", "FFT 6",
            // "Wavelet 7", or "Granular N" for a duplicated capture) with the
            // same stable-id suffix the capture paths use, instead of the
            // generic "Waveform N".
            const std::string base = frameTypeName(nf.get());
            const int libId = wave.addLibraryEntry(std::move(nf), base);
            applyLibraryIdSuffix(wave, libId, base);
            // Sync the editor target to the freshly-added entry so the
            // user can immediately edit what they just created. We do
            // NOT change currentFrameIdx - the user's existing cell /
            // scatter dot selection (if any) is independent of which
            // library entry the right pane is bound to.
            currentLibraryId = libId;
            updateHintText();
            if (wave.mode == WavetableMode::Scatter) {
                // No scatter frames changed, but the library list got a
                // new row, so the sidebar view needs to repaint.
                repaintScatterViews();
            }
            // rebuildRows now triggers a layout pass at its tail, so the
            // + Layer button picks up the new haveFrame=true state and
            // appears even when the editor was opened on an empty
            // wavetable. Nothing to do here beyond the standard sequence.
            rebuildRows();
            onLayerChanged();
            refreshPreview();
            notifyPopoutFrameOrPositionChanged();
        });
}

// Display name for a Position axis's block-rate modulation pin. Every pin is
// labelled with its axis letter (X, Y, Z, W) so it always says which axis it
// drives - even a single-axis terrain reads "Mod: Position X" rather than a
// bare "Mod: Position" that leaves the user guessing. Falls back to a number
// past the four named axes. The prefix reflects the pin's mode: "Mod: " for a
// modulation (bipolar-around-knob) input, "Set: " for an absolute
// (cable-drives-the-value) input. Both prefixes are 5 chars so the suffix
// ("Position X") parses identically either way.
static std::string positionModPinName(int axisIndex, int numAxes,
                                      Node::ModPin::Mode mode = Node::ModPin::Mode::Modulate) {
    juce::ignoreUnused(numAxes);
    const char* prefix = (mode == Node::ModPin::Mode::Absolute) ? "Set: Position "
                                                                : "Mod: Position ";
    static const char* letters = "XYZW";
    if (axisIndex >= 0 && axisIndex < 4)
        return std::string(prefix) + letters[axisIndex];
    return std::string(prefix) + std::to_string(axisIndex + 1);
}

void LayeredWaveEditorComponent::syncPositionParams() {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;
    // Expose one Position param per *traversable* axis, not per geometric
    // axis: a single-cell grid axis (or a lone normalized scatter frame) has
    // nothing for a Position to do, so it gets no param/pin. Position params
    // are numbered contiguously over the effective axes; the synth maps the
    // k-th param back to its geometry axis via WavetableDoc::effectiveAxes().
    int n = wave.effectiveDimCount();

    auto isPosName = [](const std::string& s) {
        return s.rfind("Position", 0) == 0;
    };

    // Capture which axis each existing Position-modulation pin currently drives,
    // keyed by pinId, *before* the erase/re-add below shuffles param indices. A
    // modPin is a Position one if its paramIndex points at a Position-named
    // param; its axis index is that param's ordinal among the Position params.
    // Binding by axis index (not pin name) lets us freely rename the pins.
    std::map<int, int> pinToAxis;   // pinId -> axis index
    {
        std::map<int, int> paramIdxToAxis;
        int axis = 0;
        for (int i = 0; i < (int)nd->params.size(); ++i)
            if (isPosName(nd->params[i].name)) paramIdxToAxis[i] = axis++;
        for (const auto& mp : nd->modPins) {
            auto a = paramIdxToAxis.find(mp.paramIndex);
            if (a != paramIdxToAxis.end()) pinToAxis[mp.pinId] = a->second;
        }
    }

    // Remove existing Position params, but remember their *resting* value so an
    // axis add/remove doesn't reset the user's positions back to centre.
    //
    // Critically, for a param that's currently being signal-modulated we carry
    // over its baseValue (the user's resting setting), NOT the live `value`
    // (the modulated reading for the current block). The rebuilt param is left
    // un-modulated, so applySignalModulations re-snapshots its `value` as the
    // new baseValue on the next block; feeding it the resting value keeps that
    // base stable. Capturing the modulated value instead (the old bug) let the
    // base drift off-centre every time the grid changed - e.g. growing a grid
    // to 3D while a control fader held Position low would ratchet the base down,
    // and since the modulation model is `base + (signal-0.5)` a base below 0.5
    // can no longer sweep the axis across its full 0..1 (you'd hear both cells
    // blended at the fader extreme instead of the far cell alone).
    std::map<std::string, float> prevValues;
    for (const auto& p : nd->params)
        if (isPosName(p.name)) prevValues[p.name] = p.modulated ? p.baseValue : p.value;
    nd->params.erase(std::remove_if(nd->params.begin(), nd->params.end(),
        [&](const Param& p) { return isPosName(p.name); }), nd->params.end());

    // Add fresh Position params (1D = "Position", N-D = "Position 1".."Position N")
    for (int i = 0; i < n; ++i) {
        Param p;
        p.name = (n == 1) ? "Position" : ("Position " + std::to_string(i + 1));
        auto it = prevValues.find(p.name);
        p.value = (it != prevValues.end()) ? it->second : 0.5f;
        p.minVal = 0.0f;
        p.maxVal = 1.0f;
        nd->params.push_back(std::move(p));
    }

    // Keep one block-rate modulation input pin per Position axis: add a pin when
    // an axis is added, remove it (and any cables) when an axis is removed.
    syncPositionModPins(*nd, pinToAxis);
}

void LayeredWaveEditorComponent::maybeSyncPositionParams() {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;
    // Count current Position params and compare against how many *traversable*
    // axes the doc now has. Only when they diverge - e.g. a grid axis just grew
    // 1->2, or scatter frames crossed the 1<->2 boundary under normalized blend -
    // do we pay the param/pin rebuild. Keeps spurious churn (and the unlocked
    // audio-thread param read it races with) off the common no-op path.
    int posCount = 0;
    for (const auto& p : nd->params)
        if (p.name.rfind("Position", 0) == 0) ++posCount;
    if (posCount != wave.effectiveDimCount())
        syncPositionParams();
}

void LayeredWaveEditorComponent::syncPositionModPins(Node& nd,
                                                     const std::map<int, int>& pinToAxis) {
    auto isPosName = [](const std::string& s) {
        return s.rfind("Position", 0) == 0;
    };

    // Parse the Position axis a control-input pin refers to, INDEPENDENT of its
    // Mod:/Set: mode prefix. "Mod: Position X", "Set: Position 2", and the 1D
    // "Mod: Position" all parse; the axis token is a letter (X/Y/Z/W) or a
    // 1-based number. Returns -1 for anything that isn't a Position control pin.
    //
    // Mode-independence is the whole point: a single pin per axis must be
    // recognised as the SAME axis whether it's currently in Mod or Set mode.
    // The old code keyed orphan adoption on the literal "Mod: Position X" name,
    // so a pin switched to "Set: Position X" was invisible to the axis search
    // and a duplicate "Mod:" pin got appended - producing the 3-Set + 3-Mod
    // (six pins for three axes) corruption.
    auto axisOfPinName = [](const std::string& name) -> int {
        std::string s = name;
        if (s.rfind("Mod: ", 0) == 0 || s.rfind("Set: ", 0) == 0)
            s = s.substr(5);
        if (s.rfind("Position", 0) != 0) return -1;
        std::string tok = (s.size() > 9) ? s.substr(9) : std::string();
        if (tok.empty()) return 0;                       // 1D "Position" -> axis 0
        if (tok.size() == 1 && std::isalpha((unsigned char)tok[0])) {
            switch (std::toupper((unsigned char)tok[0])) {
                case 'X': return 0; case 'Y': return 1;
                case 'Z': return 2; case 'W': return 3;
            }
            return -1;
        }
        if (std::isdigit((unsigned char)tok[0]))
            return std::atoi(tok.c_str()) - 1;           // "Position 1" -> axis 0
        return -1;
    };

    // Map axis index -> current param index for the (freshly rebuilt) Position
    // params.
    std::vector<int> axisToParamIndex;
    for (int i = 0; i < (int)nd.params.size(); ++i)
        if (isPosName(nd.params[i].name)) axisToParamIndex.push_back(i);
    const int numAxes = (int)axisToParamIndex.size();

    auto hasLink = [&](int pinId) {
        for (const auto& lk : graph.links) if (lk.endPin == pinId) return true;
        return false;
    };
    auto findModPin = [&](int pinId) -> Node::ModPin* {
        for (auto& mp : nd.modPins) if (mp.pinId == pinId) return &mp;
        return nullptr;
    };

    // ---- Classify every input pin by the Position axis it belongs to ----
    // The axis comes from the captured pinToAxis binding first (survives
    // renames), then from parsing the pin's name (survives a lost/empty
    // binding). Mode never affects axis identity. Pins that resolve to a now-
    // removed axis (>= numAxes) are queued for deletion.
    std::vector<std::vector<int>> axisCandidates(numAxes);  // pinIds per axis
    std::vector<int> stalePositionPins;                     // axis removed
    for (const auto& pin : nd.pinsIn) {
        int axis = -1;
        auto pa = pinToAxis.find(pin.id);
        if (pa != pinToAxis.end()) axis = pa->second;
        else                       axis = axisOfPinName(pin.name);
        if (axis < 0) continue;                             // not a Position pin
        if (axis < numAxes) axisCandidates[axis].push_back(pin.id);
        else                stalePositionPins.push_back(pin.id);
    }

    // For each axis pick exactly ONE surviving pin, preferring one that already
    // carries an incoming cable so dedup never severs the user's connection.
    std::vector<int> chosenForAxis(numAxes, -1);
    std::vector<int> pinsToRemove = stalePositionPins;
    for (int axis = 0; axis < numAxes; ++axis) {
        auto& cands = axisCandidates[axis];
        if (cands.empty()) continue;
        int keep = cands.front();
        for (int id : cands) if (hasLink(id)) { keep = id; break; }
        chosenForAxis[axis] = keep;
        for (int id : cands) if (id != keep) pinsToRemove.push_back(id);
    }

    // Delete every duplicate / stale Position pin, its binding, and its cables.
    for (int pinId : pinsToRemove) {
        nd.pinsIn.erase(std::remove_if(nd.pinsIn.begin(), nd.pinsIn.end(),
            [pinId](const Pin& p) { return p.id == pinId; }), nd.pinsIn.end());
        nd.modPins.erase(std::remove_if(nd.modPins.begin(), nd.modPins.end(),
            [pinId](const Node::ModPin& mp) { return mp.pinId == pinId; }), nd.modPins.end());
        graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
            [pinId](const auto& lk) { return lk.endPin == pinId; }), graph.links.end());
    }

    // Ensure each axis ends up with exactly one bound, correctly-labelled pin.
    for (int axis = 0; axis < numAxes; ++axis) {
        int pinId = chosenForAxis[axis];
        if (pinId >= 0) {
            // An existing pin survives - guarantee it has a binding and that its
            // name matches its mode. Preserve the user's chosen mode: from the
            // live binding if present, else parsed from the pin name's prefix so
            // a re-adopted "Set: " orphan stays Set.
            Node::ModPin* mp = findModPin(pinId);
            Node::ModPin::Mode mode = Node::ModPin::Mode::Modulate;
            if (mp) {
                mode = mp->mode;
            } else {
                for (const auto& p : nd.pinsIn)
                    if (p.id == pinId) {
                        if (p.name.rfind("Set: ", 0) == 0)
                            mode = Node::ModPin::Mode::Absolute;
                        break;
                    }
            }
            if (!mp) {
                Node::ModPin nmp;
                nmp.pinId = pinId;
                nmp.depth = 1.0f;
                nmp.mode  = mode;
                nd.modPins.push_back(nmp);
                mp = &nd.modPins.back();
            }
            mp->paramIndex = axisToParamIndex[axis];
            mp->mode       = mode;
            for (auto& p : nd.pinsIn)
                if (p.id == pinId) { p.name = positionModPinName(axis, numAxes, mode); break; }
        } else {
            // No pin for this axis - create a fresh one. Position modulation is
            // block-rate (applySignalModulations reads sample 0), so the pin is
            // a Param (block-rate, orange) - NOT a Signal (audio-rate, amber).
            // They route identically at the cable level (#82); the kind is what
            // the user sees and reasons about.
            int newPinId = graph.allocId();
            nd.pinsIn.push_back({newPinId, positionModPinName(axis, numAxes),
                                 PinKind::Param, true, 1});
            Node::ModPin mp;
            mp.paramIndex = axisToParamIndex[axis];
            mp.pinId      = newPinId;
            mp.depth      = 1.0f;
            nd.modPins.push_back(mp);
        }
    }

    // Re-resolve genuine NON-Position control pins' paramIndex by exact name, a
    // safety net in case the param erase/re-add above shifted indices.
    for (auto& mp : nd.modPins) {
        const std::string* nm = nullptr;
        for (const auto& p : nd.pinsIn) if (p.id == mp.pinId) { nm = &p.name; break; }
        if (!nm) continue;
        if (axisOfPinName(*nm) >= 0) continue;   // Position pin, already handled
        if (nm->rfind("Mod: ", 0) == 0 || nm->rfind("Set: ", 0) == 0) {
            std::string target = nm->substr(5);
            for (int i = 0; i < (int)nd.params.size(); ++i)
                if (nd.params[i].name == target) { mp.paramIndex = i; break; }
        }
    }

    // Keep the Aftertouch pin after every Position pin so a freshly
    // appended Position axis never leaves Aftertouch wedged between two
    // position inputs. The graph builder enforces the same ordering at build
    // time; doing it here too means the node face updates immediately when
    // the user adds an axis, without waiting for a rebuild. No-op when no
    // Aftertouch pin exists yet.
    std::stable_partition(nd.pinsIn.begin(), nd.pinsIn.end(),
        [](const Pin& p) {
            return p.name != "Aftertouch";
        });
}

void LayeredWaveEditorComponent::rebuildScatterUI() {
    // Make sure the shared rotation-state vector has the right plane count
    // for the current dim count, then push the new count out to the
    // arrangement view (which rebuilds its slider strip if it changed).
    ensureScatterPlaneAngles();
    notifyScatterViewRotated();

    // Default to projecting axes 0 / 1 / 2 (the arrangement view's
    // rotation sliders handle viewing angles for higher-N wavetables).
    if (arrangementView && arrangementView->view)
        arrangementView->view->setProjection(0, 1, 2);

    resized();
}

// ---- Editor-target accessors (read currentLibraryId, not a cell) ----

IWavetableFrame* LayeredWaveEditorComponent::currentEditingFrame() {
    return wave.libraryFrameById(currentLibraryId);
}

const IWavetableFrame* LayeredWaveEditorComponent::currentEditingFrame() const {
    return wave.libraryFrameById(currentLibraryId);
}

LayeredWaveform* LayeredWaveEditorComponent::currentEditingLayeredFrame() {
    return wave.layeredFrameByLibrary(currentLibraryId);
}

const LayeredWaveform* LayeredWaveEditorComponent::currentEditingLayeredFrame() const {
    return wave.layeredFrameByLibrary(currentLibraryId);
}

std::vector<float> LayeredWaveEditorComponent::currentFramePosition() const {
    if (currentLibraryId < 0) return {};

    if (wave.mode == WavetableMode::Scatter) {
        // The dot's authored coord IS the Position the synth blends at.
        for (const auto& sf : wave.scatterFrames) {
            if (sf.waveformId == currentLibraryId) {
                std::vector<float> pos = sf.position;
                pos.resize(std::max((size_t)wave.scatterDims, pos.size()), 0.5f);
                return pos;
            }
        }
        return {};
    }

    // Grid mode: find the flat cell index that references this entry, then
    // decompose it into per-axis grid coords using the SAME row-major
    // ordering the synth uses when it builds wtGranularFrames[].position
    // (terrain_synth.cpp): innermost (last) dim varies fastest. Normalize
    // each coord by (dimSize-1) so a single-row axis maps to 0.
    const int nf = (int)wave.cellWaveformIds.size();
    int flat = -1;
    for (int f = 0; f < nf; ++f) {
        if (wave.cellWaveformIds[f] == currentLibraryId) { flat = f; break; }
    }
    if (flat < 0) return {};

    std::vector<int> gridCoord;
    int remaining = flat;
    for (int di = (int)wave.gridDims.size() - 1; di >= 0; --di) {
        const int dim = std::max(1, wave.gridDims[di]);
        gridCoord.push_back(remaining % dim);
        remaining /= dim;
    }
    std::reverse(gridCoord.begin(), gridCoord.end());

    std::vector<float> pos(gridCoord.size(), 0.0f);
    for (size_t d = 0; d < gridCoord.size(); ++d) {
        const int dim = std::max(1, wave.gridDims[d]);
        pos[d] = (dim <= 1) ? 0.0f
                            : (float)gridCoord[d] / (float)(dim - 1);
    }
    return pos;
}

std::vector<WaveLayer>& LayeredWaveEditorComponent::currentLayers() {
    if (auto* f = currentEditingLayeredFrame()) return f->layers;
    // Non-layered frames (spectral, wavelet, granular) have no layers. The
    // layered editor shows them as an empty layer list; their contents are
    // edited via the embedded sub-editor (SpectralEditorComponent etc.).
    static std::vector<WaveLayer> empty;
    return empty;
}

const std::vector<WaveLayer>& LayeredWaveEditorComponent::currentLayers() const {
    if (auto* f = currentEditingLayeredFrame()) return f->layers;
    static const std::vector<WaveLayer> empty;
    return empty;
}

void LayeredWaveEditorComponent::setEditingLibraryEntry(int libId) {
    if (libId == currentLibraryId) return;
    // -1 is the valid "no editing target" state (empty library). Any other
    // id must exist in the library; otherwise we silently keep the current
    // target rather than dropping to nothing (avoids accidental clears from
    // stale ids in deferred callbacks).
    if (libId != -1 && wave.findLibraryIndexById(libId) < 0) return;
    currentLibraryId = libId;
    // Point the cell/dot selection at this entry's first placement (if any)
    // so the "Selected waveform position" section describes THIS waveform
    // rather than whatever cell was previously selected. When the entry isn't
    // placed anywhere, currentFrameIdx is left as-is; the position section
    // hides itself (see highlightedWaveformPlaced) and offers "Add to grid".
    if (libId >= 0) {
        if (wave.mode == WavetableMode::Grid) {
            for (int c = 0; c < (int)wave.cellWaveformIds.size(); ++c)
                if (wave.cellWaveformIds[(size_t)c] == libId) { currentFrameIdx = c; break; }
        } else {
            for (int c = 0; c < (int)wave.scatterFrames.size(); ++c)
                if (wave.scatterFrames[(size_t)c].waveformId == libId) { currentFrameIdx = c; break; }
        }
    }
    updateHintText();
    rebuildRows();
    refreshPreview();
    refreshIdentityRow();
    notifyPopoutFrameOrPositionChanged();
}

void LayeredWaveEditorComponent::renameLibraryEntry(int libId) {
    const int libIdx = wave.findLibraryIndexById(libId);
    if (libIdx < 0) return;
    const juce::String oldName(wave.library[(size_t)libIdx].name);

    // Modal text-entry prompt, following the codebase rename pattern
    // (adsr_envelope_component.cpp). Parented to this component so Windows
    // groups it under the main window's taskbar icon instead of giving the
    // dialog its own entry.
    auto* aw = new juce::AlertWindow("Rename waveform", "New name:",
                                     juce::MessageBoxIconType::NoIcon, this);
    aw->addTextEditor("name", oldName);
    // Pre-select the existing name so typing replaces it immediately, matching
    // the app's other input dialogs. The AlertWindow grabs keyboard focus for its
    // first text editor on show, at which point this selects the whole contents.
    if (auto* te = aw->getTextEditor("name"))
        te->setSelectAllWhenFocused(true);
    aw->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    juce::Component::SafePointer<LayeredWaveEditorComponent> safe(this);
    aw->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [safe, aw, libId](int r) {
                std::unique_ptr<juce::AlertWindow> disposer(aw);  // free on return
                if (r != 1 || safe == nullptr) return;
                safe->setLibraryEntryName(
                    libId, aw->getTextEditorContents("name").toStdString());
            }), false);
}

void LayeredWaveEditorComponent::setLibraryEntryName(int libId,
                                                     const std::string& newName) {
    const int libIdx = wave.findLibraryIndexById(libId);
    if (libIdx < 0) return;
    if (wave.library[(size_t)libIdx].name == newName) return;
    wave.library[(size_t)libIdx].name = newName;
    commitToNode();
    commitUndoStep();
    if (arrangementView) arrangementView->rebuildLibraryList();
    // If the renamed entry is the editor's current target, refresh the inline
    // identity-row name field so the popup and the inline editor never disagree
    // (no-clobber: refreshIdentityRow only re-sets the text when it differs).
    if (libId == currentLibraryId) refreshIdentityRow();
    notifyPopoutDocMutated();
}

void LayeredWaveEditorComponent::refreshIdentityRow() {
    const int libIdx = wave.findLibraryIndexById(currentLibraryId);
    const bool have = (libIdx >= 0);
    identityLabel.setVisible(have);
    if (nameColorSwatch) nameColorSwatch->setVisible(have);
    nameEditor.setVisible(have);
    gainLabel.setVisible(have);
    gainSlider.setVisible(have);
    if (!have) return;

    // Sync the gain knob to the frame the editor is currently bound to.
    // currentEditingFrame() can differ from the library entry at libIdx in
    // theory, but in practice they track together; reading the frame's gain
    // directly keeps the knob honest for every frame type.
    if (auto* f = currentEditingFrame())
        gainSlider.setValue(f->gain, juce::dontSendNotification);

    const auto& entry = wave.library[libIdx];
    // Update the swatch colour to what the dots actually paint, so the
    // editor swatch matches the arrangement view at a glance. Auto badge
    // is shown when colorIdx < 0.
    if (nameColorSwatch) {
        nameColorSwatch->setSwatchColor(
            libraryEntryDisplayColor(&entry, libIdx));
        nameColorSwatch->setIsAuto(entry.colorIdx < 0);
    }
    // Only overwrite the editor text if it differs from the current name -
    // avoids clobbering an in-progress edit when the layer rows refresh
    // (which is frequent during dragging sliders / placing notes).
    const juce::String currentName(entry.name);
    if (nameEditor.getText() != currentName)
        nameEditor.setText(currentName, juce::dontSendNotification);
}

void LayeredWaveEditorComponent::switchToFrame(int idx) {
    // Now means: "user picked a cell in the arrangement view / Cells list /
    // (old) frame tab". The cell becomes the selected one (currentFrameIdx),
    // and if the cell holds a library entry we ALSO sync the editor target
    // (currentLibraryId) to it - selecting a non-empty cell almost always
    // means "I want to edit this one". Selecting an empty cell leaves the
    // editor target alone.
    //
    // Acceptable cell ranges are mode-dependent: Grid cells include empty
    // ones (so users can pick an empty cell as an Assign destination);
    // Scatter cells are always populated (a dot only exists if it holds a
    // waveform).
    int max = (wave.mode == WavetableMode::Grid)
        ? (int)wave.cellWaveformIds.size()
        : (int)wave.scatterFrames.size();
    if (idx < 0 || idx >= max) return;

    const bool cellChanged    = (idx != currentFrameIdx);
    currentFrameIdx = idx;

    // If this cell holds a library entry, sync the editor target to it.
    const int cellLibId = wave.libraryIdForCell(idx);
    bool editorChanged = false;
    if (cellLibId >= 0 && cellLibId != currentLibraryId) {
        currentLibraryId = cellLibId;
        editorChanged = true;
    }

    if (!cellChanged && !editorChanged) return;

    updateHintText();
    rebuildRows();
    refreshPreview();
    // Pop-out's per-axis "Selected frame position" strip tracks the
    // editor's selection in both modes.
    notifyPopoutFrameOrPositionChanged();
}

void LayeredWaveEditorComponent::FrameDeleteX::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat().reduced(2.0f);
    const float cx = r.getCentreX();
    const float cy = r.getCentreY();
    // Arm length: 35% of the shorter axis, so the X stays compact even in
    // taller tab rows.
    const float s = std::min(r.getWidth(), r.getHeight()) * 0.30f;
    const juce::Colour col = hover
        ? juce::Colour(0xffff5050)                       // bright red on hover
        : juce::Colour(0xffd04040).withAlpha(0.75f);     // muted red at rest
    const float thick = hover ? 2.2f : 1.6f;
    g.setColour(col);
    g.drawLine(cx - s, cy - s, cx + s, cy + s, thick);
    g.drawLine(cx - s, cy + s, cx + s, cy - s, thick);
}

void LayeredWaveEditorComponent::FrameDeleteX::mouseUp(const juce::MouseEvent& e) {
    if (getLocalBounds().contains(e.getPosition()) && onClick) onClick();
}

void LayeredWaveEditorComponent::updateHintText() {
    // Discoverability hint - shown while the library has 0 or 1 entries
    // so a fresh user sees the basic interactions (add a waveform, click
    // a cell, click a row). Hides once the library has a couple of
    // entries (the multi-waveform nature is visible at that point).
    const bool showHint = (wave.library.size() <= 1);
    if (wave.mode == WavetableMode::Grid) {
        hintLabel.setText(
            "Tip: this is a wavetable. Click  + Waveform  to add a waveform to the "
            "library, then place it in cells on the left. Click a Library row "
            "to edit a waveform; edits flow into every cell that uses it. "
            "Back out in the node graph, the synth node itself has a Position knob "
            "(or Position 1, 2, ... once you add more axes) that morphs between waveforms.",
            juce::dontSendNotification);
    } else {
        hintLabel.setText(
            "Tip: click  + Waveform  to add a waveform; click a dot to select it, "
            "or click a Library row to edit its waveform. "
            "Back out in the node graph, the synth node itself has a Position knob "
            "(or Position 1, 2, ... once you add more axes) that morphs between waveforms.",
            juce::dontSendNotification);
    }
    if (hintLabel.isVisible() != showHint) {
        hintLabel.setVisible(showHint);
        resized();
    }
}

LayeredWaveEditorComponent::~LayeredWaveEditorComponent() {
    // Drop out of the live-editor registry so a concurrent snapshot restore
    // never dereferences this dying editor.
    {
        auto& reg = openWaveEditors();
        reg.erase(std::remove(reg.begin(), reg.end(), this), reg.end());
    }
    // If there's a pending debounced apply, flush it now so the audio
    // engine picks up the last edits even if the editor closes quickly.
    if (isTimerRunning()) {
        stopTimer();
        if (onApply) onApply();
    }
    // arrangementView's unique_ptr destructs here after the member-clean-up
    // chain runs - it holds a ScatterView that references *this, but JUCE
    // tears children down before the parent's members, so by the time we
    // hit this dtor body the ScatterView is already gone.
}

// =========================================================================
// N-D rotation state (shared between the embedded ScatterView and the
// pop-out wavetable view's ScatterView). The owner is the source of truth
// so both views always show the same rotation - dragging in one updates
// the other, sliders in the pop-out update the embedded view.
// =========================================================================

int LayeredWaveEditorComponent::scatterPlaneIndexFor(int i, int j, int N) {
    // Lexicographic enumeration of (i, j) with i < j. The k-th plane is
    // the k-th such pair. The (i, j) -> k formula sums up the plane counts
    // for rows 0..i-1 (each row r contributes (N-1-r) planes) plus the
    // offset within row i.
    if (i > j) std::swap(i, j);
    if (i < 0 || j >= N || i == j) return -1;
    int base = i * N - i - i * (i - 1) / 2;
    return base + (j - i - 1);
}

std::vector<std::pair<int,int>> LayeredWaveEditorComponent::scatterPlanePairs(int N) {
    std::vector<std::pair<int,int>> out;
    out.reserve((size_t)scatterPlaneCount(N));
    for (int i = 0; i < N; ++i)
        for (int j = i + 1; j < N; ++j)
            out.emplace_back(i, j);
    return out;
}

void LayeredWaveEditorComponent::ensureScatterPlaneAngles() {
    // Rotation lives in the "view" N-D space, which is gridDims.size() in
    // Grid mode and scatterDims in Scatter mode. numDimensions() returns
    // exactly that, so we use it instead of scatterDims directly - otherwise
    // adding a Grid axis wouldn't grow the plane-angle vector.
    int N = std::max(2, wave.numDimensions());
    int want = scatterPlaneCount(N);
    if ((int)scatterPlaneAngles.size() != want)
        scatterPlaneAngles.resize((size_t)want, 0.0f);
}

float LayeredWaveEditorComponent::getScatterPlaneAngle(int i, int j) const {
    int N = std::max(2, wave.numDimensions());
    int k = scatterPlaneIndexFor(i, j, N);
    if (k < 0 || k >= (int)scatterPlaneAngles.size()) return 0.0f;
    return scatterPlaneAngles[(size_t)k];
}

void LayeredWaveEditorComponent::setScatterPlaneAngle(int i, int j, float deg) {
    ensureScatterPlaneAngles();
    int N = std::max(2, wave.numDimensions());
    int k = scatterPlaneIndexFor(i, j, N);
    if (k >= 0 && k < (int)scatterPlaneAngles.size()) {
        scatterPlaneAngles[(size_t)k] = deg;
        repaintScatterViews();
    }
}

void LayeredWaveEditorComponent::repaintScatterViews() {
    if (arrangementView && arrangementView->view) arrangementView->view->repaint();
}


std::function<void(double, int, double, int, int)>
LayeredWaveEditorComponent::makeCaptureMetadataSink() {
    return [this](double pitchHz, int freezeIdx, double crossfadeMs,
                  int grainCount, int fftSize) {
        auto* g = dynamic_cast<GranularFrame*>(
            wave.libraryFrameById(currentLibraryId));
        if (!g) return;
        g->embeddedPitchHz = (float)pitchHz;
        g->freezeMode = (GranularFreezeMode)juce::jlimit(0, 3, freezeIdx);
        const double sr = (g->sourceSampleRate > 0.0)
                              ? g->sourceSampleRate : 44100.0;
        g->crossfadeSamples =
            std::max(0, (int)std::round(crossfadeMs * 0.001 * sr));
        g->grainCount = juce::jlimit(kGranularMinGrains, kGranularMaxGrains,
                                     grainCount);
        g->fftSize = (fftSize <= 0) ? 0 : fftSize;
        // Same commit path the frame editor uses: encode to the node script
        // live + dirty flag, plus debounced undo.
        onLayerChanged();
    };
}

void LayeredWaveEditorComponent::showCapturePanelInline(int sourceKind,
                                                        bool replaceCurrentEntry) {
    // sourceKind: 0=Playback (project song), 1=Mic, 2=File.
    //
    // Playback uses CaptureFromSongDialog (#capV2): pre-renders the
    // whole project to PCM offline, shows a song-length timeline with
    // play / pause / stop transport and a draggable region (two start/
    // end handles) carrying N equally-spaced "section band" waveforms -
    // the same region/N-waveform selection model as the File source.
    // While Playing the engine plays back the rendered song at full
    // fidelity (with a moving playhead anchored at the region start);
    // while Paused or Scrubbing it loops a short grain on the Preview-
    // index-selected band so the user can audition before saving. One
    // Capture slices the region into N waveforms.
    //
    // Mic / File still use the original CaptureFromPlaybackDialog -
    // they don't need offline rendering since their data is either a
    // live ring buffer (mic) or a one-shot file load (file).
    //
    // The same Component classes that used to be hosted in a JUCE
    // DialogWindow are now embedded directly into the right pane via
    // capturePanel. They detect the inline mode by virtue of having
    // their onDismiss callback set: the Capture / Save and Close
    // buttons call onDismiss instead of walking up to a DialogWindow.
    // Save still fires onCapture (which adds frames to the Library);
    // Close just clears the panel.
    //
    // replaceCurrentEntry: see header doc. Routes onCapture to the
    // replace-in-place callback instead of the add-to-library one.
    // Silently falls back to add-to-library if no library entry is bound.
    const bool doReplace = replaceCurrentEntry && currentLibraryId >= 0;
    auto onCaptured = [this, doReplace, sourceKind]
        (std::vector<std::unique_ptr<IWavetableFrame>> frames) {
        // Stamp every captured granular frame with the source it came from so
        // its editor's "Re-capture from …" button names the right source and
        // re-opens the matching panel. Done here (the single point that knows
        // sourceKind) so both the add-to-library and replace-in-place paths
        // below inherit it.
        for (auto& fr : frames)
            if (auto* g = dynamic_cast<GranularFrame*>(fr.get()))
                g->captureSourceKind = sourceKind;
        if (doReplace) {
            replaceCurrentEntryWithCapturedFrame(std::move(frames));
            // In replace mode the capture is intentionally one-shot: the
            // user is fixing the source PCM of a SPECIFIC library entry,
            // not building a multi-frame wavetable. The append-mode default
            // of keeping the dialog open after Save would leave the panel
            // hiding the right-pane editor, so the user sees "where did my
            // controls go?" until they hunt for the Close button. Auto-
            // dismiss so the new GranularFrameEditorComponent reveals
            // itself the moment the wave is replaced.
            //
            // Deferred via callAsync because CaptureFromSongDialog's Save
            // handler continues to call methods on itself AFTER returning
            // from onCapture (updateStatusLabel) - destroying the panel
            // synchronously here would be use-after-free. Async runs after
            // the current message-thread event finishes. SafePointer
            // guards the case where our component is destroyed (e.g.
            // project closed) before the async fires.
            juce::Component::SafePointer<LayeredWaveEditorComponent> safe(this);
            juce::MessageManager::callAsync([safe]() {
                if (safe) safe->dismissCapturePanel();
            });
        } else {
            addCapturedFramesToLibrary(std::move(frames), sourceKind);
            // Append mode: the dialog stays open so the user can keep selecting
            // and capturing more regions. We deliberately do NOT bind a metadata
            // write-through sink here. Both capture dialogs now slice the
            // selected region into N equally-spaced waveforms per Capture, so
            // there is no single unambiguous frame for a post-Capture "As note"
            // / freeze / crossfade edit to target - the same reason the mic/file
            // dialog has never wired an append-mode sink. The write-through sink
            // is wired only in REPLACE mode below, where the panel is bound to
            // one specific library frame. (A post-Capture metadata tweak in
            // append mode should be made in that frame's editor instead.)
        }
    };

    std::unique_ptr<juce::Component> panel;
    if (sourceKind == 0) {
        auto* eng = AudioEngine::getInstance();
        NodeGraph* gp = eng ? eng->getGraph() : nullptr;
        Transport* tp = eng ? eng->getTransport() : nullptr;
        if (!gp || !tp) {
            // Engine not wired up yet (shouldn't happen once App has
            // run). Fall back to the legacy live-tap component so the
            // user at least gets *some* path through.
            auto p = std::make_unique<CaptureFromPlaybackDialog>(
                CaptureSource::Playback, wave.tableSize, onCaptured);
            p->onDismiss = [this]() { dismissCapturePanel(); };
            panel = std::move(p);
        } else {
            auto p = std::make_unique<CaptureFromSongDialog>(
                *gp, *tp, wave.tableSize, onCaptured);
            p->onDismiss = [this]() { dismissCapturePanel(); };
            // Re-capture / replace mode is where the two save models unify.
            // The capture panel becomes bound to the SPECIFIC existing library
            // frame it's replacing: we (1) SEED its editable controls from that
            // frame so the panel opens reflecting reality, and (2) install a
            // live write-through sink so any metadata edit (pitch / freeze /
            // crossfade) commits to the frame the instant it changes - exactly
            // like GranularFrameEditorComponent. Result: closing the panel can
            // no longer silently revert a metadata change; only the PCM grab
            // stays an explicit Save (it depends on the region selection and
            // per-waveform width - capture-time params).
            if (doReplace) {
                if (auto* g = dynamic_cast<GranularFrame*>(currentEditingFrame())) {
                    const double sr = (g->sourceSampleRate > 0.0)
                                          ? g->sourceSampleRate : 44100.0;
                    const double xfadeMs = (double)g->crossfadeSamples / sr * 1000.0;
                    p->seedFromExistingFrame((double)g->embeddedPitchHz,
                                             (int)g->freezeMode, xfadeMs,
                                             g->grainCount, g->fftSize);
                }
                // Write-through sink tracks currentLibraryId (== the frame
                // being replaced throughout this session), re-looked-up live so
                // a graph.nodes / wave.library move can never dangle.
                p->onMetadataEdited = makeCaptureMetadataSink();
            }
            panel = std::move(p);
        }
    } else {
        CaptureSource src = (sourceKind == 1) ? CaptureSource::Mic
                                              : CaptureSource::File;
        auto p = std::make_unique<CaptureFromPlaybackDialog>(
            src, wave.tableSize, onCaptured);
        p->onDismiss = [this]() { dismissCapturePanel(); };
        // Re-capture / replace mode unifies the save model here exactly as it
        // does for the song dialog above: bind the panel to the SPECIFIC frame
        // being replaced, (1) SEED its metadata controls from that frame so it
        // opens reflecting reality, and (2) install the live write-through sink
        // so any metadata edit (pitch / freeze / grains / FFT) commits to the
        // frame the instant it changes. This dialog has no crossfade control, so
        // we seed the frame's crossfade ms and the sink echoes it back unchanged
        // (a no-op write that never clobbers the editor-set crossfade).
        if (doReplace) {
            if (auto* g = dynamic_cast<GranularFrame*>(currentEditingFrame())) {
                const double sr = (g->sourceSampleRate > 0.0)
                                      ? g->sourceSampleRate : 44100.0;
                const double xfadeMs = (double)g->crossfadeSamples / sr * 1000.0;
                p->seedFromExistingFrame((double)g->embeddedPitchHz,
                                         (int)g->freezeMode, xfadeMs,
                                         g->grainCount, g->fftSize);
            }
            p->onMetadataEdited = makeCaptureMetadataSink();
        }
        panel = std::move(p);
    }

    // Tear down any existing panel first (re-entering capture while one
    // is already up). Lifetimes are clean since this method only runs on
    // the message thread.
    if (capturePanel) {
        removeChildComponent(capturePanel.get());
        capturePanel.reset();
    }
    capturePanel = std::move(panel);
    addAndMakeVisible(capturePanel.get());
    resized();
}

void LayeredWaveEditorComponent::dismissCapturePanel() {
    if (!capturePanel) return;
    removeChildComponent(capturePanel.get());
    capturePanel.reset();
    // Rebind the per-frame editor before bringing it back. While the capture
    // panel was open it write-through-edited the bound frame's metadata (the
    // "As note" pitch, freeze mode, crossfade) via makeCaptureMetadataSink,
    // but the embedded GranularFrameEditorComponent created at Save time
    // seeded its controls ONCE in its constructor and was hidden behind the
    // capture panel - so those edits never reached its sliders. Just calling
    // resized() would re-show that STALE editor (e.g. the octave reverting to
    // its save-time value, and worse, a later interaction committing the stale
    // value back over the edited one). rebuildRows() tears down and recreates
    // the embed bound to the current frame, re-seeding every control from the
    // live (edited) data, then lays out. It ends in resized(), so the
    // visibility/bounds toggle still happens now that capturePanel is null.
    rebuildRows();
    repaint();
}

// Build the BASE Library display name for a captured frame - reflecting its
// source (mic / file / song) and, for granular captures, the freeze method
// baked into it - replacing the generic "Waveform N". sourceKind: 0 =
// project song, 1 = mic, 2 = file (matches showCapturePanelInline). No index
// number here; the caller appends the entry's stable creation id once it's
// known (see applyLibraryIdSuffix in addCapturedFramesToLibrary). Returns
// an empty string for sourceKind < 0 so addLibraryEntry falls back to its
// own auto-generated default (used by the non-capture insert paths).
static std::string captureEntryName(int sourceKind, const IWavetableFrame* frame) {
    if (sourceKind < 0) return {};
    juce::String src;
    switch (sourceKind) {
        case 0:  src = "Song"; break;  // project song
        case 1:  src = "Mic";  break;  // microphone input
        case 2:  src = "File"; break;  // audio file
        default: src = "Capture"; break;
    }
    juce::String name = src;
    if (auto* g = dynamic_cast<const GranularFrame*>(frame)) {
        switch (g->freezeMode) {
            case GranularFreezeMode::CrossfadeLoop:   name << " - Crossfade loop";   break;
            case GranularFreezeMode::AsyncGranular:   name << " - Async granular";   break;
            case GranularFreezeMode::PitchSyncGrains: name << " - Pitch-sync grains";break;
            case GranularFreezeMode::SpectralFreeze:  name << " - Spectral freeze";  break;
        }
    }
    return name.toStdString();
}

// Build the BASE Library display name for a frame created from scratch or by
// duplicating an existing one (the "+ Waveform" menu's "Edit from scratch"
// items and "Duplicate current"). Reflects the editor type the frame belongs
// to - "Layered", "FFT", "Wavelet" - instead of the generic "Waveform N". A
// duplicated capture keeps its granular type as "Granular". As with
// captureEntryName the caller appends the entry's stable creation id.
static std::string frameTypeName(const IWavetableFrame* frame) {
    if (dynamic_cast<const SpectralFrame*>(frame))  return "FFT";
    if (dynamic_cast<const WaveletFrame*>(frame))   return "Wavelet";
    if (dynamic_cast<const GranularFrame*>(frame))  return "Granular";
    if (dynamic_cast<const LayeredWaveform*>(frame)) return "Layered";
    return {};  // unknown type - let addLibraryEntry use its own default
}

// Append an entry's stable creation id (library id - monotonic, reload-safe,
// never renumbered on deletion) to its display name so otherwise-identical
// auto-named entries stay distinguishable (e.g. "Layered 5", "Mic 7"). No-op
// when base is empty, so non-auto-named paths keep addLibraryEntry's default.
static void applyLibraryIdSuffix(WavetableDoc& doc, int libId,
                                 const std::string& base) {
    if (base.empty() || libId < 0) return;
    const int idx = doc.findLibraryIndexById(libId);
    if (idx >= 0)
        doc.library[(size_t)idx].name = base + " " + std::to_string(libId);
}

void LayeredWaveEditorComponent::addCapturedFramesToLibrary(
    std::vector<std::unique_ptr<IWavetableFrame>> frames,
    int sourceKind)
{
    if (frames.empty()) return;
    const int N = (int)frames.size();

    // All captured waveforms - whether the user grabbed one spot or sliced
    // out N - go to the Library list ONLY. They are NOT auto-placed into
    // the arrangement: capturing never grows the grid's cell count, never
    // resizes an axis, and never adds a scatter dot. The user's expectation
    // when capturing is "give me these waveforms to work with", not "rebuild
    // my wavetable arrangement around them". Placement is a separate,
    // explicit step: select a cell (or scatter slot) and use the Library
    // list's "Assign to selected cell" button to drop an entry in. This
    // mirrors how the "+ Waveform" menu's insert items behave - they add a
    // library entry without touching gridDims / cellWaveformIds / the
    // current cell selection.
    int firstNewLibId = -1;
    for (int i = 0; i < N; ++i) {
        // Base name (source + freeze method for captures) plus the entry's
        // stable creation id, appended post-add via applyLibraryIdSuffix
        // because the id isn't known until addLibraryEntry assigns it
        // (e.g. "Mic - Crossfade loop 7").
        auto base = captureEntryName(sourceKind, frames[(size_t)i].get());
        const int libId = wave.addLibraryEntry(std::move(frames[(size_t)i]), base);
        applyLibraryIdSuffix(wave, libId, base);
        if (i == 0) firstNewLibId = libId;
    }

    // Bind the right-pane editor to the first newly-captured entry so the
    // user immediately sees what they grabbed. Deliberately leave
    // currentFrameIdx (the cell/scatter-dot selection) untouched - nothing
    // was placed, so the user's existing arrangement selection stands.
    if (firstNewLibId >= 0) currentLibraryId = firstNewLibId;
    wave.scatterFromGridSnapshot.reset();
    updateHintText();
    rebuildRows();
    onLayerChanged();
    notifyPopoutFrameOrPositionChanged();
    notifyPopoutDocMutated();
}

void LayeredWaveEditorComponent::replaceCurrentEntryWithCapturedFrame(
    std::vector<std::unique_ptr<IWavetableFrame>> frames)
{
    // Replace the wave on the currently-edited library entry. The "right
    // pane" stays bound to currentLibraryId throughout, so as soon as the
    // wave swaps the editor re-binds itself to the new frame via the
    // standard rebuildRows + onLayerChanged flow.
    //
    // We deliberately preserve the library entry's id, name, and colour -
    // the user's mental model of "this is Waveform 3" survives a re-
    // capture; only the source PCM (and embedded granular params, if
    // the new frame is also granular) change.
    if (frames.empty()) return;
    const int libIdx = wave.findLibraryIndexById(currentLibraryId);
    if (libIdx < 0) return;
    auto& entry = wave.library[(size_t)libIdx];
    entry.wave = std::move(frames[0]);
    // Any additional captured frames are dropped on the floor. Both capture
    // dialogs can now slice a region into N waveforms per Capture, but for the
    // re-capture path "replace with N frames" doesn't map to a single library
    // entry, so we take the first slice and let the user re-add the others
    // manually if they want them.

    // Same downstream sync as append path: re-render preview, push to
    // node, refresh sidebar list (so the row's "used Nx" count and
    // thumbnail update), and notify the pop-out view.
    rebuildRows();
    onLayerChanged();
    notifyPopoutFrameOrPositionChanged();
    notifyPopoutDocMutated();
}

void LayeredWaveEditorComponent::notifyScatterViewRotated() {
    if (arrangementView) arrangementView->refreshSliderValues();
}

void LayeredWaveEditorComponent::notifyPopoutFrameOrPositionChanged() {
    if (arrangementView) arrangementView->refreshFrameAndPositionValues();
}

void LayeredWaveEditorComponent::notifyPopoutDocMutated() {
    if (arrangementView) arrangementView->refreshAfterDocMutation();
}

void LayeredWaveEditorComponent::rebuildRows() {
    // Bind the shared layer stack to whichever layered frame is currently
    // targeted (nullptr for non-layered frames -> the stack shows an empty
    // list, and updateFrameEditorEmbed() hides it in favour of the embed).
    // setTarget internally rebuilds its rows + re-lays-out, with the same
    // realloc-safe "rebuild all rows on any add/delete" invariant.
    if (layerStack) {
        layerStack->setTarget(currentEditingLayeredFrame());
        // Force a rebuild even when the target object/count is unchanged: a
        // frame's layer DATA may have been mutated in place (e.g. preset load,
        // capture replace) without changing the count, and callers rely on
        // rebuildRows() refreshing the visible row controls from the model.
        layerStack->refreshFromModel();
    }

    // For spectral / wavelet frames, the layers area becomes the seat for
    // the matching frame editor instead. Doing this after the layer stack is
    // re-bound (and hidden, for non-layered frames) means we don't show stale
    // rows under the embedded editor for a flicker frame.
    updateFrameEditorEmbed();

    // The per-waveform name+colour row at the top of the right pane
    // tracks whichever library entry the editor is bound to. Rebuilding
    // rows is the universal "the editor just (re)bound to a frame" path,
    // so the identity row is refreshed here for every binding flow
    // (initial load, switchToFrame, library mutations, ...).
    refreshIdentityRow();

    // Trigger a layout pass. rebuildRows() materially changes what the
    // right pane should show (layer rows appearing/disappearing, embed
    // type swapping, identity row visibility, + Layer button visibility),
    // and the layout function (resized) computes all of those visibility
    // flags from the live state. Without this call, paths like the
    // showAddWaveformMenu callback or setEditingLibraryEntry mutate the
    // state but leave the layout in whichever state the previous resized
    // pass left it - notably the + Layer button stays hidden if the
    // editor was opened on an empty wavetable (haveFrame=false hid it).
    //
    // The recursion-from-resized case is bounded: resized()'s staleness
    // check (line "if (embedStale || rowsStale) rebuildRows()") only
    // fires when rows/embed are inconsistent with the live state, and
    // we've just made them consistent. The inner resized() therefore
    // skips the rebuildRows path and just lays out, which is correct.
    resized();
}

void LayeredWaveEditorComponent::updateFrameEditorEmbed() {
    // Bind by LIBRARY ENTRY now, not by cell. Editing the entry
    // automatically flows into every cell that references it.
    IWavetableFrame* f = currentEditingFrame();
    const std::string tid = f ? f->typeId() : std::string();

    // Layered frames use the inline WaveLayerEditor widgets - tear down any embed
    // and surface the viewport. Every other frame type either gets a
    // dedicated embedded editor below, or gets a "no editor for this type"
    // fallback in resized() (which hides + Layer so the user isn't offered
    // an action that doesn't apply).
    if (tid == "layered") {
        if (embeddedFrameEditor) {
            removeChildComponent(embeddedFrameEditor.get());
            embeddedFrameEditor.reset();
            embeddedFrameType.clear();
        }
        if (layerStack) layerStack->setVisible(true);
        return;
    }

    // Non-layered frame. Rebuild the embed whenever it points at a
    // different frame than we want (different type OR same type but a
    // different concrete frame instance, since each sub-editor binds to a
    // specific frame in its ctor). Frame switching isn't a hot path, so
    // unconditionally rebuild on type mismatch is fine.
    if (embeddedFrameEditor) {
        removeChildComponent(embeddedFrameEditor.get());
        embeddedFrameEditor.reset();
        embeddedFrameType.clear();
    }

    auto onSubApply = [this]() { onLayerChanged(); };

    if (tid == "spectral") {
        auto* sf = dynamic_cast<SpectralFrame*>(f);
        if (sf)
            embeddedFrameEditor = std::make_unique<SpectralEditorComponent>(*sf, onSubApply);
    } else if (tid == "wavelet") {
        auto* wf = dynamic_cast<WaveletFrame*>(f);
        if (wf)
            embeddedFrameEditor = std::make_unique<WaveletPainterComponent>(*wf, onSubApply);
    } else if (tid == "granular") {
        auto* gf = dynamic_cast<GranularFrame*>(f);
        if (gf) {
            // Re-capture replaces the source PCM of the current library entry
            // in place, re-opening the SAME source the frame was captured from
            // (song / mic / file) - a mic frame must re-capture from the mic,
            // not the song. The frame remembers its origin in captureSourceKind;
            // -1 (unknown, e.g. a pre-field project or a from-scratch frame)
            // falls back to song, the historical default.
            auto onRecap = [this]() {
                int kind = 0;
                if (auto* g = dynamic_cast<GranularFrame*>(currentEditingFrame()))
                    if (g->captureSourceKind >= 0) kind = g->captureSourceKind;
                showCapturePanelInline(kind, /*replaceCurrentEntry=*/true);
            };
            // Audition through the owning synth node's pendingAudition queue.
            // TerrainSynthProcessor::processBlock drains the queue and emits
            // real note-ons, so the Play button auditions through the same
            // voice / envelope / Volume path a wired-up MIDI note would hit.
            // Capturing `this` is safe because the editor's lifetime is bounded
            // by the LayeredWaveEditorComponent, which itself outlives the
            // embeddedFrameEditor. We look the node up fresh on every call via
            // graph.findNode(nodeId) so a graph.nodes reallocation can never
            // leave us holding a stale Node*.
            auto onAudition = [this](bool noteOn, int pitch, int velocity) {
                auto* nd = graph.findNode(nodeId);
                if (!nd) return;

                // Stop / refresh-with-no-frame: clear the sustained audition so
                // the synth releases its held voice on the next block.
                if (!noteOn) {
                    std::lock_guard<std::mutex> lock(*nd->auditionMutex);
                    nd->heldAudition.reset();
                    return;
                }

                // Tag the note with THIS frame's wavetable Position so the synth
                // auditions the edited frame, not whatever the live Position knob
                // selects. Computed fresh each call so moving the scatter dot (or
                // editing a param) is reflected. Done before taking the lock.
                std::vector<float> pos = currentFramePosition();

                // Ship the edited frame's actual data so the synth can render it
                // even when it isn't placed into the grid/scatter (a freshly-
                // captured library-only frame is otherwise absent from the
                // synth's wtGranularFrames table and would be silent). Looked up
                // fresh via currentEditingFrame() so we never hold a stale frame
                // pointer across a doc edit.
                auto* g = dynamic_cast<GranularFrame*>(currentEditingFrame());
                if (!g) {
                    // No granular frame to audition - nothing to hold.
                    std::lock_guard<std::mutex> lock(*nd->auditionMutex);
                    nd->heldAudition.reset();
                    return;
                }

                auto gpayload = std::make_shared<Node::AuditionGranularFrame>();
                // Reuse the in-flight audition's PCM when this is a live REFRESH
                // (band/grain/pitch edit mid-audition) rather than a fresh Play:
                // the source bytes don't change on a param edit, so sharing the
                // shared_ptr avoids deep-copying multi-MB of PCM on every slider
                // tick. A fresh Play (no held audition yet) copies it once. The
                // copy stays OUTSIDE the audio mutex so a long copy never stalls
                // the audio thread.
                {
                    std::lock_guard<std::mutex> lock(*nd->auditionMutex);
                    if (nd->heldAudition && nd->heldAudition->granularFrame
                        && nd->heldAudition->granularFrame->source)
                        gpayload->source = nd->heldAudition->granularFrame->source;
                }
                if (!gpayload->source)
                    gpayload->source =
                        std::make_shared<std::vector<float>>(g->source);
                gpayload->sourceSampleRate = g->sourceSampleRate;
                gpayload->grainLength      = g->grainLength;
                gpayload->windowStart      = g->windowStart;
                gpayload->windowLen        = g->windowLen;
                gpayload->embeddedPitchHz  = g->embeddedPitchHz;
                gpayload->freezeMode       = (int)g->freezeMode;
                gpayload->grainCount       = g->grainCount;
                gpayload->fftSize          = g->fftSize;
                gpayload->crossfadeSamples = g->crossfadeSamples;
                gpayload->gain             = g->gain;

                // Publish as the sustained, level-triggered audition. The synth
                // (re)establishes a voice from this whenever it starts, so the
                // preview survives the debounced graph rebuild an edit fires.
                auto ev = std::make_shared<Node::AuditionEvent>();
                ev->isNoteOn      = true;
                ev->pitch         = pitch;
                ev->velocity      = velocity;
                ev->position      = std::move(pos);
                ev->granularFrame = std::move(gpayload);

                std::lock_guard<std::mutex> lock(*nd->auditionMutex);
                nd->heldAudition = std::move(ev);
            };
            embeddedFrameEditor = std::make_unique<GranularFrameEditorComponent>(
                *gf, onSubApply, onRecap, onAudition);
        }
    }
    // tid == "sample" deliberately falls through with no embeddedFrameEditor;
    // resized() shows a "no editor for this type" placeholder and hides
    // + Layer. A dedicated SampleFrameEditorComponent is a future task.

    if (!embeddedFrameEditor) {
        // No embed for this frame type. We still want to hide the layer
        // stack (it's not meaningful for non-layered frames). resized()
        // detects "haveFrame && !embeddedFrameEditor && tid != layered"
        // and draws the placeholder + hides + Layer.
        if (layerStack) layerStack->setVisible(false);
        return;
    }

    embeddedFrameType = tid;
    addAndMakeVisible(embeddedFrameEditor.get());
    if (layerStack) layerStack->setVisible(false);

    // Re-lay-out so the embed gets the layer stack's rectangle. resized()
    // checks for the embed and prefers it over the viewport when present.
    resized();
}

void LayeredWaveEditorComponent::refreshPreview() {
    // Render the LIBRARY ENTRY the editor is currently targeting (NOT the
    // selected cell - they can be different now). Every concrete frame type
    // knows how to produce a tableSize-sample cycle.
    if (auto* f = currentEditingFrame()) {
        f->render(wave.tableSize, previewSamples);
    } else {
        previewSamples.clear();
    }
    repaint();
}

void LayeredWaveEditorComponent::commitToNode() {
    if (auto* nd = graph.findNode(nodeId)) {
        // Synchronised write: the audio thread (TerrainSynthProcessor) polls
        // this node's script live, and the granular wavetable script is multi-
        // megabyte, so a raw assignment races the audio read and crashes mid-
        // copy. encode() builds the string outside the lock; the helper only
        // holds the per-node mutex for the (cheap) move-assignment.
        setNodeScriptSynced(*nd, wave.encode());
        // Bump the project-dirty flag so quit-without-save prompts and
        // autosave both pick up wavetable-editor edits. Without this, the
        // user can spend a session sculpting waveforms, close SEANCE, and
        // lose everything silently because no other code path knows the
        // node's script changed.
        graph.dirty = true;
    }
}

void LayeredWaveEditorComponent::commitUndoStep() {
    // commitSnapshot() de-dups against the previous snapshot, so calling this
    // on every settled edit (debounced apply, Apply/Close, rename, recolour)
    // is cheap when nothing actually changed and pushes exactly one undo step
    // per distinct wavetable state otherwise.
    graph.commitSnapshot("Edit wavetable");
}

bool LayeredWaveEditorComponent::keyPressed(const juce::KeyPress& key) {
    if (!key.getModifiers().isCtrlDown()) return false;
    const int code = key.getKeyCode();
    bool redo;
    if (code == 'Z')      redo = key.getModifiers().isShiftDown();
    else if (code == 'Y') redo = true;
    else                  return false;  // not an undo/redo key - let it pass

    // Materialize any pending debounced edit as a real undo step BEFORE we
    // undo. Without this, a Ctrl+Z fired inside the 150 ms debounce window (the
    // common case right after a capture) would revert the LAST committed
    // snapshot and skip past the just-made edit entirely - the edit lives only
    // in the node script and would be lost with no matching redo. Flushing it
    // now turns it into a proper, reversible step.
    if (isTimerRunning()) {
        stopTimer();
        commitToNode();
        if (onApply) onApply();
        commitUndoStep();
    }

    // Defer the actual undo/redo to the next message tick. doUndo()/doRedo()
    // fire the graph's onLoadSnapshot, which reparses the graph and (via
    // reloadOpenEditorsAfterSnapshot) rebuilds THIS editor's child components -
    // unsafe to run synchronously from inside a child's key handler still on the
    // call stack. The graph outlives every editor, so capturing it by pointer is
    // safe even if this editor closes in the meantime.
    NodeGraph* g = &graph;
    juce::MessageManager::callAsync([g, redo]() {
        if (redo) g->undoTree.doRedo();
        else      g->undoTree.doUndo();
    });
    return true;
}

void LayeredWaveEditorComponent::reloadFromNode() {
    // Cancel any pending debounce first: timerCallback() would otherwise fire
    // after we reload and commitUndoStep() the OLD wave doc, corrupting both the
    // node script and the undo tree (re-applying the change we just undid).
    stopTimer();

    auto* nd = graph.findNode(nodeId);
    if (!nd) {
        // The node itself was undone away - nothing left to edit. Close our
        // pop-out window (async + SafePointer, mirroring the Close button).
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
            juce::Component::SafePointer<juce::DialogWindow> safe(dw);
            juce::MessageManager::callAsync([safe]() {
                if (safe) delete safe.getComponent();
            });
        }
        return;
    }

    // Preserve the editor target / arrangement selection across the reload when
    // they survive in the restored doc.
    const int prevLibraryId = currentLibraryId;
    const int prevFrameIdx  = currentFrameIdx;
    const std::vector<float> prevPosition = currentPosition;

    WavetableDoc fresh;
    if (!fresh.decode(nd->script)) {
        // Node script is no longer an editable wavetable (shouldn't happen for a
        // synth we had open). Leave the editor as-is rather than blanking it -
        // the stale state is at least self-consistent.
        return;
    }
    wave = std::move(fresh);

    // Re-seed selection: keep the previous library entry if it still exists,
    // else fall back to the first entry the way the constructor does.
    if (wave.findLibraryIndexById(prevLibraryId) >= 0)
        currentLibraryId = prevLibraryId;
    else
        currentLibraryId = wave.library.empty() ? -1 : wave.library.front().id;

    const int cellCount = (wave.mode == WavetableMode::Grid)
        ? (int)wave.cellWaveformIds.size()
        : (int)wave.scatterFrames.size();
    currentFrameIdx = (cellCount > 0)
        ? juce::jlimit(0, cellCount - 1, prevFrameIdx) : 0;

    currentPosition.assign((size_t)std::max(1, wave.numDimensions()), 0.5f);
    for (size_t d = 0; d < currentPosition.size() && d < prevPosition.size(); ++d)
        currentPosition[d] = prevPosition[d];

    // A reload invalidates any in-progress capture context and the lossless
    // "Back to Grid" round-trip snapshot.
    if (capturePanel) dismissCapturePanel();
    wave.scatterFromGridSnapshot.reset();

    // Rebuild the UI from the restored doc. Deliberately NOT onLayerChanged():
    // that path commits to the node and pushes a snapshot, which would clobber
    // the very undo we are responding to. refreshPreview() + the arrangement-
    // view refreshers rebuild everything visible without writing anything back.
    updateHintText();
    rebuildRows();
    refreshPreview();
    if (arrangementView) {
        arrangementView->refreshAfterDocMutation();
        arrangementView->refreshFrameAndPositionValues();
    }
    notifyPopoutFrameOrPositionChanged();
    notifyPopoutDocMutated();
    repaint();
}

void LayeredWaveEditorComponent::onLayerChanged() {
    // Live visual preview every tick; audio rebuild is debounced so we don't
    // race JUCE's AudioProcessorGraph async rebuild (which crashes on rapid
    // concurrent rebuilds).
    refreshPreview();
    commitToNode();
    // Any doc mutation can invalidate the lossless "Back to Grid" round-trip
    // (a dragged dot, an added/removed axis, a placed/deleted frame all reset
    // or break the scatter->grid snapshot). onLayerChanged() is the universal
    // "something changed" notifier, so re-evaluating the Convert button here
    // guarantees it never sits stale-enabled regardless of which handler fired
    // - the button greys the instant the round-trip stops being possible.
    if (arrangementView) arrangementView->updateConvertButton();
    // (Re)start the debounce: fire 150ms after the last change.
    startTimer(150);
}

void LayeredWaveEditorComponent::timerCallback() {
    stopTimer();
    if (onApply) onApply();
    // The debounce has settled - this is the natural commit point for the
    // bulk of wavetable edits (waveform sculpting, cell placement, grid
    // resize, deletes all route through onLayerChanged -> this timer).
    commitUndoStep();
}

void LayeredWaveEditorComponent::resized() {
    auto a = getLocalBounds().reduced(8);

    // Discoverability hint - shown only when the wavetable has just one
    // frame (so people who picked "Waveform / Wavetable" see immediately
    // that this editor is more than a single-waveform editor). Sits
    // ABOVE the toolbar so it reads as a header rather than getting
    // sandwiched between the top buttons and the frame-tabs row.
    if (hintLabel.isVisible()) {
        hintLabel.setBounds(a.removeFromTop(18));
        a.removeFromTop(4);
    }

    // Top button row spans the whole window:
    //   [Render mode: Direct] [Additive bank]              [?] [Apply] [Close]
    // "+ Waveform" used to sit on the left here; it has been moved into
    // the arrangement-view sidebar (next to the Library list) so it lives
    // beside the data structure it mutates. "Render mode" sets the synth
    // node's Synth Mode param, which applies to the WHOLE wavetable (not
    // per-waveform), so it belongs in the global toolbar rather than the
    // right-pane per-waveform editor where it used to sit.
    auto top = a.removeFromTop(28);
    closeBtn.setBounds(top.removeFromRight(60));
    top.removeFromRight(4);
    applyBtn.setBounds(top.removeFromRight(60));
    top.removeFromRight(4);
    helpBtn.setBounds(top.removeFromRight(26));
    top.removeFromRight(12); // separator gap from the right-side cluster

    compareLabel.setVisible(true);
    compareDirectBtn.setVisible(true);
    compareAdditiveBtn.setVisible(true);
    compareLabel.setBounds(top.removeFromLeft(80));
    top.removeFromLeft(4);
    compareDirectBtn.setBounds(top.removeFromLeft(60));
    top.removeFromLeft(2);
    compareAdditiveBtn.setBounds(top.removeFromLeft(100));

    a.removeFromTop(6);

    // (The old "Frame tabs" row that used to sit here has been removed:
    // it listed one tab per library entry, which exactly duplicated the
    // Library list in the arrangement-view sidebar. With the sidebar
    // already always visible, the tab strip was redundant and ate ~32 px
    // of vertical space that the editor body now reclaims.)

    // ---- Body split: LEFT = arrangement view, RIGHT = per-waveform editor.
    // Both are always visible. Right pane is sized to give the editor enough
    // room (~440 px) without starving the arrangement view; on narrow dialogs
    // it falls back to half-and-half.
    const int rightPaneW = juce::jlimit(360, 560, a.getWidth() / 2);
    auto right = a.removeFromRight(rightPaneW);
    a.removeFromRight(8); // gap between the two halves

    if (arrangementView) {
        arrangementView->setVisible(true);
        arrangementView->setBounds(a);
    }

    // ---- Right pane: capture flow OR (editor body + preview) ----
    // While a capture is in progress, the capture component occupies the
    // entire right pane in place of the normal per-frame editor stack.
    // Hide the per-frame widgets so they don't poke through, and skip the
    // rest of the right-pane layout. The Render mode buttons stay visible
    // - they live in the top toolbar now and apply to the whole node.
    if (capturePanel) {
        if (embeddedFrameEditor) embeddedFrameEditor->setVisible(false);
        if (layerStack) layerStack->setVisible(false);
        // Identity row also hidden during capture - the right pane is
        // entirely owned by the capture panel until the user closes it.
        identityLabel.setVisible(false);
        if (nameColorSwatch) nameColorSwatch->setVisible(false);
        nameEditor.setVisible(false);
        gainLabel.setVisible(false);
        gainSlider.setVisible(false);
        previewBounds = juce::Rectangle<int>();  // suppresses preview paint
        capturePanel->setBounds(right);
        return;
    }

    // Preview strip at the bottom of the right pane. Bounds are stashed for
    // paint() so the geometry stays in one place.
    int previewH = 150;
    previewBounds = right.removeFromBottom(previewH);
    right.removeFromBottom(6);

    // Identity row at the very top of the right pane (above the editor
    // body): [Waveform: label] [colour swatch] [name TextEditor]. Stays
    // visible across editor types (layered / spectral / wavelet) so the
    // name and colour live in one stable spot. Hidden when no library
    // entry is targeted - refreshIdentityRow controls visibility.
    {
        const int idH = 24;
        auto idRow = right.removeFromTop(idH);
        right.removeFromTop(6);
        identityLabel.setBounds(idRow.removeFromLeft(70));
        idRow.removeFromLeft(2);
        if (nameColorSwatch) {
            nameColorSwatch->setBounds(idRow.removeFromLeft(idH).reduced(2));
            idRow.removeFromLeft(4);
        }
        nameEditor.setBounds(idRow);
    }

    // Gain row directly under the identity row: [Gain: label][slider+box].
    // Kept on its own line so the horizontal slider + value text box have room
    // without squeezing the name editor. Same visibility gating as the
    // identity row.
    {
        const int gH = 26;
        auto gRow = right.removeFromTop(gH);
        right.removeFromTop(6);
        gainLabel.setBounds(gRow.removeFromLeft(70));
        gRow.removeFromLeft(2);
        // Horizontal slider + attached text box; cap the width so it doesn't
        // sprawl across the whole pane on wide windows.
        gainSlider.setBounds(gRow.removeFromLeft(juce::jmin(gRow.getWidth(), 240)));
    }

    // Middle: either the layered-frame layer rows + viewport, the embedded
    // spectral / wavelet / granular sub-editor, or a placeholder for frame
    // types that don't yet have a dedicated editor (currently SampleFrame).
    // + Layer is meaningful ONLY for LayeredWaveform; we hide it for every
    // other type so the user isn't offered an action that doesn't apply.
    const bool haveFrame = (currentEditingFrame() != nullptr);
    const auto* fr = currentEditingFrame();
    const std::string tid = fr ? fr->typeId() : std::string();
    const bool isLayered = (tid == "layered");

    // Defense in depth: every mutator that re-targets the editor (frame
    // switch, library entry click, drag-drop, capture replace, ...) is
    // supposed to call rebuildRows(), which calls updateFrameEditorEmbed()
    // and rebuilds the layer rows. resized() being a pure function of the
    // current state means that even if some path forgets, the user-visible
    // layout is still correct.
    //
    // Two flavours of staleness are possible:
    //   1. embeddedFrameEditor is set, but the current frame is layered
    //      (or has no editor type) - the embed is left over from a
    //      previous binding and would hide the +Layer button + rows.
    //   2. embeddedFrameEditor is null (or wrong type), but the current
    //      frame is spectral/wavelet/granular - we'd fall through to the
    //      "no editor for this type" placeholder.
    // Either way, kick a rebuildRows() to reconcile, which itself calls
    // updateFrameEditorEmbed().
    const bool embedNeeded   = (tid == "spectral" || tid == "wavelet" || tid == "granular");
    const bool embedMatches  = embeddedFrameEditor && embeddedFrameType == tid;
    const bool embedStale    = haveFrame &&
                               ((embedNeeded != (embeddedFrameEditor != nullptr))
                                || (embedNeeded && !embedMatches)
                                || (isLayered && embeddedFrameEditor != nullptr));
    const bool rowsStale     = haveFrame && isLayered && layerStack &&
                               layerStack->getTarget() != currentEditingLayeredFrame();
    if (embedStale || rowsStale) {
        rebuildRows();
    }

    if (!haveFrame) {
        if (embeddedFrameEditor) embeddedFrameEditor->setVisible(false);
        if (layerStack) layerStack->setVisible(false);
        placeholderBounds = juce::Rectangle<int>();
    } else if (embeddedFrameEditor) {
        embeddedFrameEditor->setVisible(true);
        if (layerStack) layerStack->setVisible(false);
        embeddedFrameEditor->setBounds(right);
        placeholderBounds = juce::Rectangle<int>();
    } else if (isLayered) {
        // The shared layer stack owns its own "+ Layer" header + scrolling
        // row list, so it just takes the whole right-pane body rectangle.
        if (layerStack) {
            layerStack->setVisible(true);
            layerStack->setBounds(right);
        }
        placeholderBounds = juce::Rectangle<int>();
    } else {
        // Frame type has no dedicated embedded editor yet (currently
        // SampleFrame: tid == "sample"). Hide all the layered controls
        // and stash a placeholder rect for paint() to draw the
        // "no editor for this type yet" message into. This is strictly
        // better than the old fall-through where + Layer appeared above
        // an empty viewport - the button suggested layered semantics on
        // a non-layered frame.
        if (layerStack) layerStack->setVisible(false);
        placeholderBounds = right;
    }
}

void LayeredWaveEditorComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(22, 22, 28));

    // "No editor for this frame type" placeholder. resized() sets this
    // bounds rect only when haveFrame && !embeddedFrameEditor && frame
    // type isn't layered - i.e., a frame type we don't have a dedicated
    // editor for yet. Currently only SampleFrame hits this branch; if a
    // future frame type is added without an editor, the placeholder is
    // what the user sees instead of an empty right pane or a misleading
    // + Layer button.
    if (!placeholderBounds.isEmpty()) {
        auto pr = placeholderBounds.toFloat();
        g.setColour(juce::Colour(28, 28, 36));
        g.fillRoundedRectangle(pr, 4.0f);
        g.setColour(juce::Colour(70, 70, 90));
        g.drawRoundedRectangle(pr, 4.0f, 1.0f);
        const auto* fr = currentEditingFrame();
        const juce::String tid = fr ? juce::String(fr->typeId())
                                    : juce::String("?");
        juce::String msg;
        msg << "Captured " << tid << " waveform.\n"
            << "This frame type doesn't have an in-editor view yet.\n"
            << "You can replace it via + Waveform, or delete it from the "
            << "Library list and re-capture.";
        g.setColour(juce::Colours::white.withAlpha(0.75f));
        g.setFont(12.0f);
        g.drawText(msg, pr.reduced(12.0f).toNearestInt(),
                   juce::Justification::centred, true);
    }

    // Side-by-side layout: the preview strip lives at the bottom of the
    // RIGHT pane. resized() stashes its bounds in previewBounds so we
    // can draw without re-deriving the geometry here.
    if (previewBounds.isEmpty()) return;
    auto previewArea = previewBounds.toFloat();

    g.setColour(juce::Colour(30, 30, 38));
    g.fillRoundedRectangle(previewArea, 4.0f);
    g.setColour(juce::Colour(70, 70, 90));
    g.drawRoundedRectangle(previewArea, 4.0f, 1.0f);

    // If the editor has no library entry targeted (empty library), show a
    // placeholder over the right pane editor area (and skip the curve).
    const bool haveFrame = (currentEditingFrame() != nullptr);
    if (!haveFrame) {
        // Editor body area is everything in the right pane above the preview.
        // We can't easily reconstruct it here without re-running the resized()
        // geometry, but the layer stack / embed are hidden anyway, so the
        // placeholder just goes in the preview rect.
        g.setColour(juce::Colours::grey.withAlpha(0.75f));
        g.setFont(12.0f);
        g.drawText("No waveform to edit - add one with + Waveform, or "
                   "click an existing waveform in the Library list.",
                   previewArea.reduced(8.0f).toNearestInt(),
                   juce::Justification::centred, true);
        return;
    }

    // Title. We deliberately don't put the current frame index in this
    // label - the frame tabs above already show which frame is selected,
    // and the old "Grid frame N/M" wording read like a frame identifier
    // rather than a section heading for the preview strip. Calling it
    // "Single-cycle waveform" matches what it actually shows (one cycle
    // of the resulting sample at the currently-selected wavetable frame).
    g.setColour(juce::Colours::grey);
    g.setFont(11.0f);
    juce::String title = "Single-cycle waveform"
                         "  -  baked on edit; Position morphs the active blend";
    g.drawText(title,
               previewArea.reduced(6, 2).toNearestInt(),
               juce::Justification::topLeft, true);

    // Reserve a strip at the bottom of the preview for the X-axis labels
    // and duration anchor.  Without this, the curve sat on a 0..1 phase
    // axis with no indication of what time it actually represents - a
    // wavetable synth reads one cycle per played-note period, so one
    // cycle's duration is literally 1/note_freq seconds.  We show the
    // duration at common reference pitches so the user can see "this
    // sub-cycle wiggle = ~0.5 ms".
    const float axisStripH = 28.0f;
    auto curveArea = previewArea.reduced(4.0f, 14.0f);
    curveArea.removeFromBottom(axisStripH - 4.0f);

    // Waveform curve
    if (!previewSamples.empty()) {
        float cx = curveArea.getX();
        float cy = curveArea.getCentreY();
        float w  = curveArea.getWidth();
        float h  = curveArea.getHeight();

        g.setColour(juce::Colours::grey.withAlpha(0.3f));
        g.drawHorizontalLine((int)cy, cx, cx + w);

        juce::Path p;
        int n = (int)previewSamples.size();
        for (int i = 0; i < n; ++i) {
            float x = cx + (float)i / (float)(n - 1) * w;
            float y = cy - previewSamples[i] * h * 0.45f;
            if (i == 0) p.startNewSubPath(x, y);
            else p.lineTo(x, y);
        }
        g.setColour(juce::Colours::cornflowerblue);
        g.strokePath(p, juce::PathStrokeType(1.5f));

        // Phase tick marks at 0, 1/4, 1/2, 3/4, 1.  Anchors the curve to
        // a clear "one full cycle" mental model - without these, the
        // user has no way to gauge how wide a feature is relative to the
        // whole cycle.
        float tickTop    = cy + h * 0.45f + 2.0f;
        float tickBot    = curveArea.getBottom() + 2.0f;
        float labelY     = curveArea.getBottom() + 4.0f;
        float labelH     = 11.0f;
        g.setColour(juce::Colours::grey.withAlpha(0.5f));
        g.setFont(9.5f);
        const char* phaseLabels[] = {"0", "1/4", "1/2", "3/4", "1 cycle"};
        for (int i = 0; i < 5; ++i) {
            float t = (float)i / 4.0f;
            float x = cx + t * w;
            g.drawVerticalLine((int)x, tickTop, tickBot);
            auto justification = (i == 0)              ? juce::Justification::centredLeft
                              : (i == 4)              ? juce::Justification::centredRight
                                                       : juce::Justification::centred;
            juce::Rectangle<float> lbl(x - 30, labelY, 60, labelH);
            if (i == 0)      lbl.setX(x);
            else if (i == 4) lbl.setX(x - 60);
            g.drawText(phaseLabels[i], lbl.toNearestInt(), justification, false);
        }

        // Duration anchor: a wavetable synth's cycle plays at the played
        // note's frequency, so 1 cycle = 1/freq seconds.  We anchor with
        // two common reference pitches so the user can ballpark how
        // wide their features are in real time.  We don't tie this to
        // the actual playing note (which can change per keystroke) -
        // fixed anchors are more legible.
        const float a4Hz = 440.0f;
        const float c4Hz = 261.626f;
        auto fmtMs = [](float ms) {
            return ms >= 10.0f ? juce::String(ms, 1) + " ms"
                              : juce::String(ms, 2) + " ms";
        };
        juce::String anchor = "1 cycle = " + fmtMs(1000.0f / a4Hz)
                            + " at A4 (440 Hz)   |   "
                            + fmtMs(1000.0f / c4Hz)
                            + " at C4 (262 Hz)";
        g.setColour(juce::Colours::grey.withAlpha(0.85f));
        g.setFont(10.0f);
        juce::Rectangle<float> anchorR(
            curveArea.getX(),
            labelY + labelH + 1.0f,
            curveArea.getWidth(),
            12.0f);
        g.drawText(anchor, anchorR.toNearestInt(),
                   juce::Justification::centred, false);
    }
}

} // namespace SoundShop

#include "capture_from_playback.h"
#include "audio_engine.h"
#include "audio_cache.h"
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

CaptureFromPlaybackDialog::CaptureFromPlaybackDialog(CaptureSource src, int ts, OnCapture onCap)
    : onCapture(std::move(onCap)),
      source(src),
      tableSize(std::max(64, ts)) {
    setSize(720, 440);

    addAndMakeVisible(numFramesLabel);
    numFramesLabel.setText("Number of waveforms:", juce::dontSendNotification);
    numFramesLabel.setJustificationType(juce::Justification::centredRight);

    addAndMakeVisible(numFramesSlider);
    numFramesSlider.setRange(1.0, 32.0, 1.0);
    numFramesSlider.setValue(8.0, juce::dontSendNotification);
    numFramesSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    numFramesSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
    numFramesSlider.setTooltip(
        "How many captured waveforms to extract across the selected region. "
        "Each one takes a short audio window at one position; the synth "
        "morphs through them as the Position parameter sweeps from 0 to 1. "
        "More waveforms = finer evolution but more memory.");
    numFramesSlider.onValueChange = [this]() { updateRegionInfoLabel(); };

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
                       "sweeps through them.";
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

    // Live-source controls.
    if (isLiveSource()) {
        addAndMakeVisible(freezeToggle);
        freezeToggle.setTooltip(
            "Pause the auto-refresh so the waveform display stays still while you "
            "drag the region handles. Turn it off to see new audio.");
    }

    // File-source controls.
    if (source == CaptureSource::File) {
        addAndMakeVisible(loadFileBtn);
        loadFileBtn.setTooltip("Pick a .wav/.mp3/.aiff/.flac/.ogg file. The whole "
                                "file is decoded and shown; drag handles to pick a "
                                "region inside it.");
        loadFileBtn.onClick = [this]() { chooseAndLoadFile(); };
    }

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

    // Initial state per source.
    if (isLiveSource()) {
        refreshSnapshot();
        startTimerHz(20);
    } else {
        // File: disable capture until a file is loaded. The button gets
        // re-enabled in chooseAndLoadFile() on success.
        captureBtn.setEnabled(false);
    }

    updateSourceInfoLabel();
    updateRegionInfoLabel();
}

CaptureFromPlaybackDialog::~CaptureFromPlaybackDialog() {
    stopTimer();
}

void CaptureFromPlaybackDialog::timerCallback() {
    if (!isLiveSource()) return;
    if (!freezeToggle.getToggleState())
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
        updateRegionInfoLabel();
    } else {
        regionStart = juce::jlimit(0, sz, regionStart);
        regionEnd   = juce::jlimit(regionStart, sz, regionEnd);
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
    if (isLiveSource())
        freezeToggle.setBounds(bottomRow.removeFromRight(120));
    if (source == CaptureSource::File)
        loadFileBtn.setBounds(bottomRow.removeFromLeft(140));

    r.removeFromBottom(8);
    auto controlsRow = r.removeFromBottom(26);
    numFramesLabel.setBounds(controlsRow.removeFromLeft(140));
    numFramesSlider.setBounds(controlsRow.removeFromLeft(260));
    controlsRow.removeFromLeft(12);
    regionInfoLabel.setBounds(controlsRow);

    r.removeFromBottom(4);
    sourceInfoLabel.setBounds(r.removeFromBottom(20));

    r.removeFromBottom(2);
    hintLabel.setBounds(r.removeFromBottom(36));
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
    juce::String msg;
    msg << "Region: " << juce::String(spanSec, 2) << " s   |   "
        << n << " frames, "
        << juce::String(perFrameMs, 1) << " ms between frame centers";
    regionInfoLabel.setText(msg, juce::dontSendNotification);
}

void CaptureFromPlaybackDialog::mouseDown(const juce::MouseEvent& e) {
    if (!waveRect.contains(e.getPosition())) return;
    if (tap.empty()) return;

    const int xs = xForIdx(regionStart);
    const int xe = xForIdx(regionEnd);
    const int mx = e.x;

    const int dStart = std::abs(mx - xs);
    const int dEnd   = std::abs(mx - xe);
    if (std::min(dStart, dEnd) <= kHandleHitRadius) {
        dragHandle = (dStart <= dEnd) ? 0 : 1;
        dragAnchorSampleOffset = 0;
        return;
    }
    if (mx > xs && mx < xe) {
        dragHandle = 2;
        dragAnchorSampleOffset = idxForX(mx) - regionStart;
        return;
    }
    regionStart = idxForX(mx);
    regionEnd   = regionStart;
    dragHandle  = 1;
    updateRegionInfoLabel();
    repaint(waveRect);
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
    updateRegionInfoLabel();
    repaint(waveRect);
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

    // Frame-center markers (vertical guides showing where each captured
    // frame's window center will sit). Helps the user see what they're
    // grabbing without re-reading the info label.
    const int n = (int)std::round(numFramesSlider.getValue());
    if (n > 0 && xe > xs) {
        g.setColour(juce::Colour::fromFloatRGBA(1.0f, 0.85f, 0.5f, 0.5f));
        for (int i = 0; i < n; ++i) {
            const float t = (n == 1) ? 0.5f : (float)i / (float)(n - 1);
            const int cx  = xs + (int)std::round(t * (xe - xs));
            g.drawVerticalLine(cx, (float)waveRect.getY() + 4.0f,
                                   (float)waveRect.getBottom() - 4.0f);
        }
    }

    auto drawHandle = [&](int x) {
        g.setColour(juce::Colour(255, 165, 60));
        g.fillRect(juce::Rectangle<int>(x - 2, waveRect.getY(), 4, H));
        g.fillRect(juce::Rectangle<int>(x - 6, waveRect.getY(), 12, 8));
        g.fillRect(juce::Rectangle<int>(x - 6, waveRect.getBottom() - 8, 12, 8));
    };
    drawHandle(xs);
    drawHandle(xe);
}

std::vector<std::unique_ptr<IWavetableFrame>>
CaptureFromPlaybackDialog::buildFrames(int n) const {
    std::vector<std::unique_ptr<IWavetableFrame>> out;
    if (n <= 0 || tap.empty()) return out;
    n = std::min(n, 32);
    out.reserve((size_t)n);

    // Mic/file capture mirrors the song-capture path: each emitted frame
    // is a GranularFrame holding a multi-sample window of the source PCM
    // plus a default ~100 ms grain length. The synth plays it back via
    // 4-voice OLA so the timbre evolves over time instead of being
    // collapsed to one cycle. embeddedPitchHz=440 (A4) until YIN pitch
    // detection lands - so MIDI note 69 plays at 1:1, others get pitch
    // shifted the same way as cycle frames.
    const int   tapLen     = (int)tap.size();
    const int   regionLen  = std::max(0, regionEnd - regionStart);
    const double sr        = (tapSampleRate > 0.0) ? tapSampleRate : 48000.0;

    // Grain length: 100 ms is a musically neutral default - long enough
    // to preserve formant character, short enough that pitched material
    // still tracks MIDI cleanly. Floor at 64 samples for the OLA math.
    const int grainLenSamples = std::max(64, (int)std::round(0.1 * sr));

    // Source window: aim for ~1 second around each frame's center, or
    // 4x grain length (whichever is larger). Clamp to the user-selected
    // region so a small region produces correspondingly small sources -
    // the user picked that span on purpose.
    const int srcWanted  = std::max((int)std::llround(sr), grainLenSamples * 4);
    const int srcLen     = std::min(srcWanted, regionLen > 0 ? regionLen : tapLen);

    for (int i = 0; i < n; ++i) {
        const float t = (n == 1) ? 0.5f : (float)i / (float)(n - 1);
        const int centerIdx = regionStart + (int)std::round(t * regionLen);
        int startIdx = centerIdx - srcLen / 2;
        startIdx = juce::jlimit(0, std::max(0, tapLen - srcLen), startIdx);

        std::vector<float> source((size_t)srcLen, 0.0f);
        for (int s = 0; s < srcLen; ++s) {
            const int src = startIdx + s;
            if (src >= 0 && src < tapLen)
                source[(size_t)s] = tap[(size_t)src];
        }

        auto frame = std::make_unique<GranularFrame>(
            std::move(source), sr, grainLenSamples, 440.0f);
        out.push_back(std::move(frame));
    }
    return out;
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
    setSize(820, 552);

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
    // ComboBox IDs are 1-based; we map them to enum values via -1.
    // Only Crossfade loop is implemented in the audio engine today; the
    // other three are stubs that silently fall back to CrossfadeLoop
    // (see audio_engine.cpp's freeze-mode dispatch). Until their
    // implementations land, present them disabled with a "(coming soon)"
    // suffix so the user doesn't waste time toggling them expecting an
    // audible change.
    freezeModeCombo.addItem("Crossfade loop",
                            (int)GranularFreezeMode::CrossfadeLoop + 1);
    freezeModeCombo.addItem("Async granular (blur) - coming soon",
                            (int)GranularFreezeMode::AsyncGranular + 1);
    freezeModeCombo.addItem("Pitch-sync grains - coming soon",
                            (int)GranularFreezeMode::PitchSyncGrains + 1);
    freezeModeCombo.addItem("Spectral freeze - coming soon",
                            (int)GranularFreezeMode::SpectralFreeze + 1);
    freezeModeCombo.setItemEnabled(
        (int)GranularFreezeMode::AsyncGranular + 1, false);
    freezeModeCombo.setItemEnabled(
        (int)GranularFreezeMode::PitchSyncGrains + 1, false);
    freezeModeCombo.setItemEnabled(
        (int)GranularFreezeMode::SpectralFreeze + 1, false);
    freezeModeCombo.setSelectedId(
        (int)GranularFreezeMode::CrossfadeLoop + 1,
        juce::dontSendNotification);
    freezeModeCombo.setTooltip(
        "How the loop sustains the marker spot while a captured note is "
        "held. Currently only Crossfade loop is implemented; the other "
        "three are reserved slots for upcoming algorithms and will be "
        "enabled when their engine code lands.\n"
        " - Crossfade loop: faithful tape loop with a short fade across "
        "the seam. The source pitch is baked in; held notes are this loop "
        "resampled to your MIDI pitch (sampler-style).\n"
        " - Async granular (coming soon): many short grains at randomised "
        "positions in a small range. Frozen blur / GRM-Freeze texture.\n"
        " - Pitch-sync grains (coming soon): grain hop locked to the "
        "detected pitch period so the loop is one cycle. Requires a "
        "pitched source.\n"
        " - Spectral freeze (coming soon): keeps the FFT magnitudes and "
        "regenerates phases each frame. Ethereal pad sustain.\n"
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
    };

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

    // Freeze-mode row: dropdown + brief space (status label stays on
    // the width row above, so this row is the picker alone).
    auto freezeRow = r.removeFromBottom(26);
    freezeModeLabel.setBounds(freezeRow.removeFromLeft(70));
    freezeModeCombo.setBounds(freezeRow.removeFromLeft(220));

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

    // YIN pitch detection: planned follow-up. Until then we use the
    // GranularFrame default (A4 = 440 Hz) so MIDI note 69 plays at 1:1
    // and other notes pitch-shift by grain stride. Users can dial in
    // the right pitch by ear via MIDI key choice for now.
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
        440.0f, mode, xfadeSamples);
    out.push_back(std::move(frame));
    return out;
}

} // namespace SoundShop

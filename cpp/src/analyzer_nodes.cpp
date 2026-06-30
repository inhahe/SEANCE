#include "analyzer_nodes.h"
#include "fft_util.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace SoundShop {

// ==============================================================================
// AnalyzerCaptureRegistry
// ==============================================================================
std::mutex AnalyzerCaptureRegistry::mutex;
std::map<int, std::weak_ptr<AnalyzerCapture>> AnalyzerCaptureRegistry::table;

std::shared_ptr<AnalyzerCapture> AnalyzerCaptureRegistry::getOrCreate(int nodeId) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = table.find(nodeId);
    if (it != table.end()) {
        if (auto sp = it->second.lock()) return sp;
        // Expired weak_ref - fall through and create a fresh one.
    }
    auto cap = std::make_shared<AnalyzerCapture>();
    table[nodeId] = cap;
    return cap;
}

// ==============================================================================
// AnalyzerProcessor
// ==============================================================================
AnalyzerProcessor::AnalyzerProcessor(Node& n) : node(n) {
    capture = AnalyzerCaptureRegistry::getOrCreate(node.id);
}

void AnalyzerProcessor::prepareToPlay(double sr, int) {
    sampleRate = sr;
    if (capture) capture->sampleRate.store(sr, std::memory_order_relaxed);
}

void AnalyzerProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) {
    int numSamples = buf.getNumSamples();
    if (numSamples == 0 || !capture) return;

    int numCh = buf.getNumChannels();
    const float* in0 = numCh > 0 ? buf.getReadPointer(0) : nullptr;
    const float* in1 = numCh > 1 ? buf.getReadPointer(1) : in0;

    int wp = capture->writePos.load(std::memory_order_relaxed);
    for (int s = 0; s < numSamples; ++s) {
        capture->bufL[wp] = in0 ? in0[s] : 0.0f;
        capture->bufR[wp] = in1 ? in1[s] : 0.0f;
        wp = (wp + 1) % AnalyzerCapture::kBufSize;
    }
    capture->writePos.store(wp, std::memory_order_release);
    // Audio passes through unchanged on channels 0+1.
}

// ==============================================================================
// SpectrumAnalyzerComponent
// ==============================================================================
SpectrumAnalyzerComponent::SpectrumAnalyzerComponent(NodeGraph& g, int nid)
    : graph(g), nodeId(nid)
{
    capture = AnalyzerCaptureRegistry::getOrCreate(nodeId);

    // Legacy "Bins" param (from when the bar count was a fixed dropdown
    // 16/32/.../512) is no longer used - the current analyzer derives
    // bar count from the visible width. Strip the stale param so projects
    // don't carry it forever.
    if (auto* nd = graph.findNode(nodeId)) {
        for (size_t i = 0; i < nd->params.size(); ) {
            if (nd->params[i].name == "Bins") nd->params.erase(nd->params.begin() + i);
            else ++i;
        }
    }

    setSize(640, 280);
    startTimerHz(20);
}

int SpectrumAnalyzerComponent::currentNumBars() const {
    // Roughly one bar per 4 pixels of canvas width. Clamp to a useful
    // visible range so a very narrow window still shows something and
    // a huge one doesn't drown the FFT in sub-pixel bars.
    auto area = getLocalBounds().reduced(6).withTrimmedBottom(14);
    int w = std::max(0, area.getWidth());
    return juce::jlimit(8, 1024, w / 4);
}

void SpectrumAnalyzerComponent::timerCallback() {
    if (!capture) return;
    int n = 2048; // FFT size
    std::vector<float> L, R;
    double sr = capture->snapshot(L, R, n);

    // Hann window over mono sum.
    std::vector<float> windowed(n);
    for (int i = 0; i < n; ++i) {
        float w = 0.5f * (1.0f - std::cos(6.28318530718f * i / n));
        windowed[i] = 0.5f * (L[i] + R[i]) * w;
    }

    FFT fft(n);
    std::vector<std::complex<float>> spectrum;
    fft.forwardReal(windowed, spectrum);
    int numBins = n / 2;

    int numBars = currentNumBars();
    if ((int)magnitudes.size() != numBars) magnitudes.assign(numBars, 0.0f);

    float nyquist = (float)(sr * 0.5);
    if (nyquist < 1.0f) nyquist = 22050.0f;
    float logMin = std::log(20.0f);
    float logMax = std::log(nyquist);

    for (int bar = 0; bar < numBars; ++bar) {
        float logLo = logMin + (logMax - logMin) * bar / numBars;
        float logHi = logMin + (logMax - logMin) * (bar + 1) / numBars;
        float hzLo = std::exp(logLo);
        float hzHi = std::exp(logHi);
        int binLo = juce::jlimit(0, numBins - 1, (int)(hzLo / nyquist * numBins));
        int binHi = juce::jlimit(binLo, numBins - 1, (int)(hzHi / nyquist * numBins));

        float maxMag = 0.0f;
        for (int b = binLo; b <= binHi; ++b)
            maxMag = std::max(maxMag, std::abs(spectrum[b]));
        // Smooth decay.
        magnitudes[bar] = magnitudes[bar] * 0.7f + maxMag * 0.3f;
    }

    repaint();
}

void SpectrumAnalyzerComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(18, 20, 28));

    auto area = getLocalBounds().toFloat().reduced(6).withTrimmedTop(34).withTrimmedBottom(14);
    int numBars = (int)magnitudes.size();
    if (numBars == 0) return;

    float barW = area.getWidth() / (float)numBars;
    float maxDb = 0.0f, minDb = -80.0f;

    for (int i = 0; i < numBars; ++i) {
        float db = 20.0f * std::log10(std::max(1e-6f, magnitudes[i]));
        float frac = juce::jlimit(0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
        float barH = frac * area.getHeight();
        auto col = juce::Colour::fromHSV(0.6f - frac * 0.6f, 0.8f, 0.5f + frac * 0.5f, 1.0f);
        g.setColour(col);
        g.fillRect(area.getX() + i * barW, area.getBottom() - barH, barW - 1, barH);
    }

    // Frequency labels (log axis).
    g.setColour(juce::Colours::grey);
    g.setFont(9.0f);
    double sr = capture ? capture->sampleRate.load(std::memory_order_relaxed) : 44100.0;
    float nyquist = (float)(sr * 0.5);
    if (nyquist < 1.0f) nyquist = 22050.0f;
    for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f }) {
        if (hz > nyquist) break;
        float logFrac = std::log(hz / 20.0f) / std::log(nyquist / 20.0f);
        float x = area.getX() + logFrac * area.getWidth();
        g.setColour(juce::Colour(40, 44, 56));
        g.drawVerticalLine((int)x, area.getY(), area.getBottom());
        g.setColour(juce::Colours::grey);
        juce::String label = (hz >= 1000) ? juce::String((int)(hz / 1000)) + "k" : juce::String((int)hz);
        g.drawText(label, (int)x - 14, (int)area.getBottom() + 1, 28, 12, juce::Justification::centred);
    }
}

// ==============================================================================
// OscilloscopeComponent
// ==============================================================================
OscilloscopeComponent::OscilloscopeComponent(NodeGraph& g, int nid)
    : graph(g), nodeId(nid)
{
    capture = AnalyzerCaptureRegistry::getOrCreate(nodeId);

    // Legacy "Window" param (from when the sample count was a fixed dropdown
    // 256/512/1024/2048/4096) is no longer used - the current oscilloscope
    // derives sample count from the visible width, so drag the dialog wider
    // to see more samples per frame. Strip the stale param so projects
    // don't carry it forever.
    if (auto* nd = graph.findNode(nodeId)) {
        for (size_t i = 0; i < nd->params.size(); ) {
            if (nd->params[i].name == "Window") nd->params.erase(nd->params.begin() + i);
            else ++i;
        }
    }

    // --- Control strip widgets -------------------------------------------
    // Mode selector: Triggered acquisition vs Roll (free-running strip chart).
    addAndMakeVisible(modeBox);
    modeBox.addItem("Triggered", 1);
    modeBox.addItem("Roll", 2);
    modeBox.setTooltip(
        "Triggered: aligns the trace to a level crossing so a repeating "
        "waveform looks frozen on screen (classic oscilloscope sync).\n"
        "Roll: free-running strip chart - newest samples scroll in at the "
        "right edge, no alignment.");
    modeBox.onChange = [this] { commitScopeSettings(); };

    // Trigger slope: Rising / Falling (only meaningful in Triggered mode).
    addAndMakeVisible(slopeBtn);
    slopeBtn.setClickingTogglesState(false);
    slopeBtn.onClick = [this] {
        if (auto* nd = findNode()) {
            nd->scopeTrigRising = !nd->scopeTrigRising;
            loadControlsFromNode();
            commitScopeSettings();
        }
    };

    // Trigger level: -1..1 (0 = zero crossing).
    addAndMakeVisible(levelLabel);
    levelLabel.setText("Level", juce::dontSendNotification);
    levelLabel.setJustificationType(juce::Justification::centredRight);
    levelLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible(levelSlider);
    levelSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    levelSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 20);
    levelSlider.setRange(-1.0, 1.0, 0.001);
    levelSlider.setTooltip(
        "Trigger level (-1..1, 0 = zero crossing). The trace starts where the "
        "signal crosses this level with the chosen slope.");
    levelSlider.onValueChange = [this] {
        if (auto* nd = findNode())
            nd->scopeTrigLevel = (float)levelSlider.getValue();
    };
    // Commit one undo step when the drag finishes, not per tick.
    levelSlider.onDragEnd = [this] { commitScopeSettings(); };

    loadControlsFromNode();
    updateControlEnablement();

    setSize(640, 240 + kControlStripH);
    startTimerHz(30);
}

void OscilloscopeComponent::resized() {
    auto strip = getLocalBounds().removeFromTop(kControlStripH).reduced(6, 4);
    modeBox.setBounds(strip.removeFromLeft(110));
    strip.removeFromLeft(8);
    slopeBtn.setBounds(strip.removeFromLeft(80));
    strip.removeFromLeft(12);
    levelLabel.setBounds(strip.removeFromLeft(40));
    strip.removeFromLeft(4);
    levelSlider.setBounds(strip.removeFromLeft(juce::jmin(220, strip.getWidth())));
}

void OscilloscopeComponent::loadControlsFromNode() {
    auto* nd = findNode();
    if (!nd) return;
    modeBox.setSelectedId(nd->scopeTriggered ? 1 : 2, juce::dontSendNotification);
    slopeBtn.setButtonText(nd->scopeTrigRising ? "Rising" : "Falling");
    levelSlider.setValue(nd->scopeTrigLevel, juce::dontSendNotification);
}

void OscilloscopeComponent::updateControlEnablement() {
    auto* nd = findNode();
    const bool triggered = nd ? nd->scopeTriggered : true;
    slopeBtn.setEnabled(triggered);
    levelSlider.setEnabled(triggered);
    levelLabel.setEnabled(triggered);

    const juce::String why =
        "Disabled in Roll mode - roll is a free-running strip chart with no "
        "edge alignment. Switch the mode to Triggered to set the slope/level.";
    if (triggered) {
        slopeBtn.setTooltip("Trigger slope: which direction the signal must "
                            "cross the level to start the trace. Click to "
                            "toggle Rising / Falling.");
        levelSlider.setTooltip(
            "Trigger level (-1..1, 0 = zero crossing). The trace starts where "
            "the signal crosses this level with the chosen slope.");
    } else {
        slopeBtn.setTooltip(why);
        levelSlider.setTooltip(why);
    }
}

void OscilloscopeComponent::commitScopeSettings() {
    auto* nd = findNode();
    if (!nd) return;
    nd->scopeTriggered  = (modeBox.getSelectedId() != 2);
    nd->scopeTrigLevel  = (float)levelSlider.getValue();
    // slopeBtn state is mirrored into nd->scopeTrigRising by its onClick.
    updateControlEnablement();
    graph.commitSnapshot("Oscilloscope settings");
}

void OscilloscopeComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(18, 20, 28));

    // Reserve the control strip at the top; the scope draws below it.
    auto full = getLocalBounds();
    full.removeFromTop(kControlStripH);
    auto area = full.toFloat().reduced(6);

    // Center line.
    g.setColour(juce::Colour(40, 44, 56));
    g.drawHorizontalLine((int)area.getCentreY(), area.getX(), area.getRight());

    if (!capture) return;

    auto* nd = findNode();
    const bool triggered = nd ? nd->scopeTriggered : true;
    const float trigLevel = nd ? nd->scopeTrigLevel : 0.0f;
    const bool trigRising = nd ? nd->scopeTrigRising : true;

    // Number of samples shown across the width (one sample per pixel),
    // saturating at the capture buffer size.
    int n = juce::jlimit(16,
                         (int)AnalyzerCapture::kBufSize,
                         (int)std::round(area.getWidth()));

    std::vector<float> L, R;
    if (triggered) {
        // Snapshot a larger window than we display, then search it for a
        // level crossing of the chosen slope. Drawing `n` samples starting
        // at the crossing makes a periodic waveform appear stationary.
        int snap = juce::jlimit(n, (int)AnalyzerCapture::kBufSize, n * 2);
        std::vector<float> sL, sR;
        capture->snapshot(sL, sR, snap);

        // Search the earliest portion so we still have `n` samples to draw
        // after the trigger index. Default to the most recent `n` samples
        // (= the tail) if no crossing is found.
        int trig = snap - n;   // fallback: newest n samples
        int searchEnd = snap - n;
        for (int i = 1; i <= searchEnd; ++i) {
            float prev = sL[i - 1], cur = sL[i];
            bool cross = trigRising ? (prev < trigLevel && cur >= trigLevel)
                                    : (prev > trigLevel && cur <= trigLevel);
            if (cross) { trig = i; break; }
        }

        L.assign(sL.begin() + trig, sL.begin() + trig + n);
        R.assign(sR.begin() + trig, sR.begin() + trig + n);
    } else {
        // Roll: free-running, newest n samples each frame (newest at right).
        capture->snapshot(L, R, n);
    }

    // Draw L and R overlaid.
    juce::Path pathL, pathR;
    float xScale = (n > 1) ? area.getWidth() / (float)(n - 1) : 0.0f;
    float yCenter = area.getCentreY();
    float yHalf = area.getHeight() * 0.45f;

    for (int i = 0; i < n; ++i) {
        float x = area.getX() + i * xScale;
        float yL = yCenter - juce::jlimit(-1.0f, 1.0f, L[i]) * yHalf;
        float yR = yCenter - juce::jlimit(-1.0f, 1.0f, R[i]) * yHalf;
        if (i == 0) { pathL.startNewSubPath(x, yL); pathR.startNewSubPath(x, yR); }
        else        { pathL.lineTo(x, yL);          pathR.lineTo(x, yR);          }
    }

    g.setColour(juce::Colour(80, 200, 255).withAlpha(0.8f));
    g.strokePath(pathL, juce::PathStrokeType(1.2f));
    g.setColour(juce::Colour(255, 140, 80).withAlpha(0.6f));
    g.strokePath(pathR, juce::PathStrokeType(1.2f));

    // Faint trigger-level guide line (Triggered mode only).
    if (triggered) {
        float yTrig = yCenter - juce::jlimit(-1.0f, 1.0f, trigLevel) * yHalf;
        g.setColour(juce::Colour(120, 200, 120).withAlpha(0.35f));
        float dashes[] = { 4.0f, 3.0f };
        g.drawDashedLine(juce::Line<float>(area.getX(), yTrig, area.getRight(), yTrig),
                         dashes, 2, 1.0f);
    }

    // Border.
    g.setColour(juce::Colour(50, 55, 70));
    g.drawRect(area, 1.0f);

    // Sample-count overlay so the user can see how the resize is mapping
    // to samples. Show the capture's sample rate to make the time span
    // concrete (e.g. "1024 samples (~23 ms)").
    double sr = capture->sampleRate.load(std::memory_order_relaxed);
    double ms = (sr > 0.0) ? 1000.0 * (double)n / sr : 0.0;
    juce::String info = juce::String(n) + " samples";
    if (ms > 0.0)
        info += " (~" + juce::String(ms, ms < 10.0 ? 2 : 1) + " ms)";
    g.setColour(juce::Colours::lightgrey.withAlpha(0.7f));
    g.setFont(11.0f);
    g.drawText(info,
               juce::Rectangle<int>((int)area.getX() + 6, (int)area.getY() + 4,
                                    (int)area.getWidth() - 12, 14),
               juce::Justification::topRight, false);
    if (n >= AnalyzerCapture::kBufSize) {
        // Make the cap visible so a user dragging the window past 4096px
        // doesn't quietly stop seeing more samples.
        g.setColour(juce::Colours::orange.withAlpha(0.85f));
        g.drawText("(buffer cap reached)",
                   juce::Rectangle<int>((int)area.getX() + 6, (int)area.getY() + 20,
                                        (int)area.getWidth() - 12, 14),
                   juce::Justification::topRight, false);
    }
}

// ==============================================================================
// SpectrogramComponent
// ==============================================================================
SpectrogramComponent::SpectrogramComponent(NodeGraph& g, int nid)
    : graph(g), nodeId(nid)
{
    capture = AnalyzerCaptureRegistry::getOrCreate(nodeId);
    waterfall = juce::Image(juce::Image::RGB, waterfallW, waterfallH, true);
    currentFftSize = chooseFftSizeForHeight(waterfallH);
    setSize(640, 280);
    startTimerHz(15);
}

int SpectrogramComponent::chooseFftSizeForHeight(int h) const {
    // The waterfall maps log-frequency along the Y axis, so taller dialogs
    // benefit from finer frequency resolution. Aim for roughly two FFT
    // bins per pixel of height (FFT size = 2*H), rounded up to the next
    // power of two. Clamp to a sensible range and to the capture buffer
    // size (we can't FFT more samples than the ring buffer holds).
    int target = std::max(1, h) * 2;
    int n = 256;
    while (n < target && n < AnalyzerCapture::kBufSize) n *= 2;
    return juce::jlimit(256, (int)AnalyzerCapture::kBufSize, n);
}

void SpectrogramComponent::resized() {
    int newW = std::max(64, getWidth());
    int newH = std::max(64, getHeight() - 6);
    if (newW != waterfallW || newH != waterfallH) {
        waterfallW = newW;
        waterfallH = newH;
        waterfall = juce::Image(juce::Image::RGB, waterfallW, waterfallH, true);
    }
    currentFftSize = chooseFftSizeForHeight(waterfallH);
}

void SpectrogramComponent::timerCallback() {
    if (!capture) return;
    int n = currentFftSize; // FFT size per frame, scales with dialog height
    std::vector<float> L, R;
    double sr = capture->snapshot(L, R, n);

    // Hann window over mono sum.
    std::vector<float> windowed(n);
    for (int i = 0; i < n; ++i) {
        float w = 0.5f * (1.0f - std::cos(6.28318530718f * i / n));
        windowed[i] = 0.5f * (L[i] + R[i]) * w;
    }

    FFT fft(n);
    std::vector<std::complex<float>> spectrum;
    fft.forwardReal(windowed, spectrum);
    int numBins = n / 2;

    float nyquist = (float)(sr * 0.5);
    if (nyquist < 1.0f) nyquist = 22050.0f;
    float logMin = std::log(20.0f);
    float logMax = std::log(nyquist);

    // Scroll waterfall left by 1 pixel column, then draw the new spectrum
    // into the rightmost column. Log-frequency axis on Y (low at bottom).
    juce::Image::BitmapData bm(waterfall, juce::Image::BitmapData::readWrite);
    int W = waterfall.getWidth();
    int H = waterfall.getHeight();

    // Shift left by 1px (last column gets overwritten next).
    for (int y = 0; y < H; ++y) {
        juce::uint8* row = bm.getLinePointer(y);
        // RGB = 3 bytes per pixel.
        std::memmove(row, row + 3, (size_t)(W - 1) * 3);
    }

    // Compute new rightmost column.
    int col = W - 1;
    for (int y = 0; y < H; ++y) {
        float frac = 1.0f - (float)y / (float)(H - 1); // bottom = 0 (low Hz), top = 1
        float logHz = logMin + frac * (logMax - logMin);
        float hz = std::exp(logHz);
        int bin = juce::jlimit(0, numBins - 1, (int)(hz / nyquist * numBins));
        float mag = std::abs(spectrum[bin]);
        float db = 20.0f * std::log10(std::max(1e-6f, mag));
        float t = juce::jlimit(0.0f, 1.0f, (db + 80.0f) / 80.0f);

        // Magma-ish colormap: black -> purple -> red -> yellow -> white.
        juce::uint8 r = (juce::uint8) juce::jlimit(0, 255, (int)(std::pow(t, 0.6f) * 255));
        juce::uint8 g = (juce::uint8) juce::jlimit(0, 255, (int)(std::pow(std::max(0.0f, t - 0.3f), 1.5f) * 255 * 1.4f));
        juce::uint8 b = (juce::uint8) juce::jlimit(0, 255, (int)(std::pow(std::max(0.0f, t - 0.05f), 0.5f) * 255 * 0.6f));

        juce::uint8* px = bm.getLinePointer(y) + col * 3;
        // BitmapData with RGB format stores as BGR on most platforms via PixelRGB
        // (LittleEndian). Use setPixelColour for safety.
        bm.setPixelColour(col, y, juce::Colour(r, g, b));
        (void)px;
    }

    repaint();
}

void SpectrogramComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(10, 12, 18));
    auto area = getLocalBounds().toFloat().reduced(3);
    if (waterfall.isValid())
        g.drawImage(waterfall, area, juce::RectanglePlacement::stretchToFit);
    g.setColour(juce::Colour(50, 55, 70));
    g.drawRect(area, 1.0f);
}

} // namespace SoundShop

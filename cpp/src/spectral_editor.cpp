#define _USE_MATH_DEFINES
#include "spectral_editor.h"
#include "fft_util.h"        // FFT for the preview
#include "help_utils.h"
#include <cmath>
#include <sstream>
#include <algorithm>
#include <complex>

namespace SoundShop {

// SpectralCurve evaluation, encoding, the SpectralCurvePanel UI, and all
// the helper functions used to be defined here. They now live in
// curve_editor.{h,cpp} so SpectrumTap and any future user of the same
// 3-mode curve editor can share the implementation.

// Helper used by SpectralDoc::encode / decode: split on a separator
// character. (Same as the one in curve_editor.cpp - duplicated here to
// keep that one file-local.)
static std::vector<std::string> splitChar(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= s.size()) {
        size_t n = s.find(sep, p);
        if (n == std::string::npos) n = s.size();
        out.push_back(s.substr(p, n - p));
        p = n + 1;
    }
    return out;
}

std::string SpectralDoc::encode() const {
    std::ostringstream o;
    o << "__spectral2__:" << fftSize << "|" << mag.encode() << "|" << phase.encode();
    return o.str();
}

bool SpectralDoc::decode(const std::string& s) {
    // ---- New format ----
    if (s.rfind("__spectral2__:", 0) == 0) {
        std::string rest = s.substr(std::string("__spectral2__:").size());
        auto parts = splitChar(rest, '|');
        if (parts.size() < 3) return false;
        try { fftSize = std::stoi(parts[0]); } catch (...) { fftSize = 2048; }
        if (!SpectralCurve::decode(parts[1], mag))   return false;
        if (!SpectralCurve::decode(parts[2], phase)) return false;
        return true;
    }

    // ---- Legacy format: __spectral__:<fftSize>:<phaseMode>:<magExpr>|<phaseExpr> ----
    if (s.rfind("__spectral__:", 0) == 0) {
        std::string rest = s.substr(std::string("__spectral__:").size());
        auto c1 = rest.find(':');
        auto c2 = (c1 != std::string::npos) ? rest.find(':', c1 + 1)
                                            : std::string::npos;
        if (c1 == std::string::npos || c2 == std::string::npos) return false;
        try { fftSize = std::stoi(rest.substr(0, c1)); } catch (...) { fftSize = 2048; }
        int phaseMode = 1;
        try { phaseMode = std::stoi(rest.substr(c1 + 1, c2 - c1 - 1)); } catch (...) {}
        std::string body = rest.substr(c2 + 1);
        std::string magExpr, phaseExpr;
        auto bar = body.find('|');
        if (bar != std::string::npos) {
            magExpr   = body.substr(0, bar);
            phaseExpr = body.substr(bar + 1);
        } else {
            magExpr = body;
        }
        mag.mode = SpectralCurve::Equation;
        mag.expression = magExpr;
        phase.mode = SpectralCurve::Equation;
        switch (phaseMode) {
            case 0: phase.expression = phaseExpr.empty() ? "0" : phaseExpr; break;
            case 1: phase.expression = "noise()*pi"; break;
            case 2: phase.expression = "0"; break;
            case 3: phase.expression = "-pi*f"; break;
            default: phase.expression = "0"; break;
        }
        return true;
    }
    return false;
}

SpectralDoc SpectralDoc::defaultBuiltin() {
    SpectralDoc d;
    d.fftSize = 2048;
    d.mag.mode = SpectralCurve::Equation;
    d.mag.expression = "exp(-f/20)";
    d.phase.mode = SpectralCurve::Equation;
    d.phase.expression = "noise()*pi";
    return d;
}

// =============================================================================
// SpectralDoc -> time-domain waveform
// =============================================================================
//
// Shared by Terrain::fillFromSpectralDoc and SpectralFrame::render so the
// rendering rules (power-of-two rounding, DC kill, peak normalisation) live
// in one place. The caller picks the table size; the SpectralCurves are
// evaluated at the matching halfBins regardless of doc.fftSize.
void renderSpectralToWaveform(const SpectralDoc& doc,
                              int tableSize,
                              std::vector<float>& out)
{
    int n = 2;
    while (n < tableSize && n < 16384) n <<= 1;
    if (n < 2) n = 2;

    int halfBins = n / 2 + 1;
    auto mags   = doc.mag.evaluate(halfBins);
    auto phases = doc.phase.evaluate(halfBins);
    for (auto& m : mags) if (m < 0.0f) m = 0.0f;

    std::vector<std::complex<float>> spectrum(halfBins);
    for (int k = 0; k < halfBins; ++k) {
        float m = mags[k];
        float p = phases[k];
        if (k == 0 || k == halfBins - 1)
            spectrum[k] = std::complex<float>(m, 0.0f);
        else
            spectrum[k] = std::complex<float>(m * std::cos(p),
                                              m * std::sin(p));
    }
    spectrum[0] = 0.0f; // kill DC

    FFT fft(n);
    fft.inverseReal(spectrum, out);

    float peak = 0.0f;
    for (float v : out) peak = std::max(peak, std::abs(v));
    if (peak > 1e-9f) {
        float inv = 1.0f / peak;
        for (float& v : out) v *= inv;
    }
}

// =============================================================================
// SpectralFrame - IWavetableFrame adapter
// =============================================================================

void SpectralFrame::renderRaw(int tableSize, std::vector<float>& out) const {
    renderSpectralToWaveform(doc, tableSize, out);
}

std::string SpectralFrame::encodeBody() const {
    // Strip the "__spectral2__:" prefix; the container length-prefixes the
    // body so embedded ':' and '|' are safe.
    auto full = doc.encode();
    const std::string prefix = "__spectral2__:";
    if (full.rfind(prefix, 0) == 0) return full.substr(prefix.size());
    return full;
}

bool SpectralFrame::decodeBody(const std::string& body) {
    // SpectralDoc::decode accepts only prefixed input; reattach the prefix
    // so the existing parser is happy.
    return doc.decode("__spectral2__:" + body);
}

std::unique_ptr<IWavetableFrame> SpectralFrame::clone() const {
    return std::make_unique<SpectralFrame>(*this);
}


// ==============================================================================
// SpectralEditorComponent
// ==============================================================================

SpectralEditorComponent::SpectralEditorComponent(NodeGraph& g, int id,
                                                  std::function<void()> apply)
    : graph(&g), nodeId(id), externalFrame(nullptr), onApply(std::move(apply))
{
    if (auto* nd = graph->findNode(nodeId)) {
        if (!doc.decode(nd->script))
            doc = SpectralDoc::defaultBuiltin();
    } else {
        doc = SpectralDoc::defaultBuiltin();
    }
    initUI();
}

SpectralEditorComponent::SpectralEditorComponent(SpectralFrame& frame,
                                                  std::function<void()> apply)
    : graph(nullptr), nodeId(0), externalFrame(&frame), onApply(std::move(apply))
{
    // Seed the editor from the frame's existing doc. No NodeGraph: commits
    // mirror straight back into externalFrame->doc.
    doc = frame.doc;
    initUI();
}

void SpectralEditorComponent::initUI() {
    // FFT size combo
    addAndMakeVisible(fftSizeLabel);
    fftSizeLabel.setText("FFT size:", juce::dontSendNotification);
    fftSizeLabel.setFont(13.0f);
    addAndMakeVisible(fftSizeCombo);
    fftSizeCombo.addItem("512",  1);
    fftSizeCombo.addItem("1024", 2);
    fftSizeCombo.addItem("2048", 3);
    fftSizeCombo.addItem("4096", 4);
    int sel = (doc.fftSize <= 512) ? 1 : (doc.fftSize <= 1024) ? 2
            : (doc.fftSize <= 2048) ? 3 : 4;
    fftSizeCombo.setSelectedId(sel, juce::dontSendNotification);
    fftSizeCombo.setTooltip("How many FFT bins to use to render this wavetable. "
                            "More bins = finer spectral detail and a longer single-cycle waveform. "
                            "Higher values cost more memory but the synth's anti-aliased mip pyramid keeps CPU cost similar.");
    fftSizeCombo.onChange = [this]() {
        int s = fftSizeCombo.getSelectedId();
        doc.fftSize = (s == 1) ? 512 : (s == 2) ? 1024 : (s == 3) ? 2048 : 4096;
        onCurveChanged();
    };

    // Curve panels
    phasePanel = std::make_unique<SpectralCurvePanel>(doc.phase, "Phase",
        -3.14159265f, 3.14159265f, juce::Colour(255, 180, 100),
        [this]() { onCurveChanged(); });
    magPanel = std::make_unique<SpectralCurvePanel>(doc.mag, "Magnitude",
        0.0f, 1.0f, juce::Colour(150, 200, 255),
        [this]() { onCurveChanged(); });
    addAndMakeVisible(phasePanel.get());
    addAndMakeVisible(magPanel.get());

    // Apply/Close belong to the standalone-dialog mode where this editor
    // owns its own DialogWindow. When the editor is frame-backed (embedded
    // into the wavetable editor as the per-frame editing slot), commits
    // happen continuously through the onApply callback and there's no
    // owning dialog to close, so both buttons are hidden.
    const bool standaloneDialogMode = (externalFrame == nullptr);
    if (standaloneDialogMode) {
        addAndMakeVisible(applyBtn);
        applyBtn.setTooltip("Push the current spectrum to the node now (it is also auto-applied a half-second after edits stop).");
        applyBtn.onClick = [this]() { commitToNode(); if (onApply) onApply(); };
        addAndMakeVisible(closeBtn);
        closeBtn.setTooltip("Close this editor. Unapplied edits are committed first.");
        closeBtn.onClick = [this]() {
            commitToNode(); if (onApply) onApply();
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState(0);
        };
    }
    addAndMakeVisible(helpBtn);
    helpBtn.setTooltip("Open the help page for the frequency-domain editor.");
    helpBtn.onClick = []() { openHelpDocFile("wavetables.html"); };

    refreshPreview();
    setSize(720, 520);
}

SpectralEditorComponent::~SpectralEditorComponent() {
    stopTimer();
}

void SpectralEditorComponent::resized() {
    auto a = getLocalBounds().reduced(8);

    // Top row: FFT size + buttons. Apply/Close only appear in standalone
    // dialog mode; in embedded mode they're not parented at all, so we
    // skip their slots to give the FFT-size combo more breathing room.
    auto top = a.removeFromTop(28);
    fftSizeLabel.setBounds(top.removeFromLeft(70));
    fftSizeCombo.setBounds(top.removeFromLeft(80));
    if (externalFrame == nullptr) {
        closeBtn.setBounds(top.removeFromRight(72));
        top.removeFromRight(4);
        applyBtn.setBounds(top.removeFromRight(72));
        top.removeFromRight(4);
    }
    helpBtn.setBounds(top.removeFromRight(24));

    a.removeFromTop(4);

    // Bottom: time-domain preview strip. Only shown in standalone mode -
    // when this editor is embedded inside the wavetable shell as the
    // per-frame editor for a Spectral frame, the shell already paints a
    // larger single-cycle preview at the bottom of its own area, so we'd
    // just be duplicating it (and at a smaller size). Skipping the strip
    // here also gives the mag / phase panels the full vertical space.
    if (externalFrame == nullptr) {
        auto previewArea = a.removeFromBottom(80);
        (void)previewArea; // drawn in paint()
        a.removeFromBottom(4);
    }

    // Two stacked curve panels: phase on top, mag on bottom.
    int half = a.getHeight() / 2;
    phasePanel->setBounds(a.removeFromTop(half - 2));
    a.removeFromTop(4);
    magPanel->setBounds(a);
}

void SpectralEditorComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(22, 22, 28));

    // Embedded mode: the wavetable shell already paints a "Single-cycle
    // waveform" strip below us, so we skip our own to avoid duplicating it.
    if (externalFrame != nullptr) return;

    // Time-domain preview strip at the bottom
    auto bounds = getLocalBounds().reduced(8);
    bounds.removeFromTop(28 + 4);
    auto previewArea = bounds.removeFromBottom(80).toFloat().reduced(2.0f);

    g.setColour(juce::Colour(18, 18, 24));
    g.fillRoundedRectangle(previewArea, 3.0f);
    g.setColour(juce::Colour(70, 70, 90));
    g.drawRoundedRectangle(previewArea, 3.0f, 1.0f);

    float cy = previewArea.getCentreY();
    g.setColour(juce::Colours::grey.withAlpha(0.25f));
    g.drawHorizontalLine((int)cy, previewArea.getX(), previewArea.getRight());

    if (!previewSamples.empty()) {
        juce::Path p;
        int n = (int)previewSamples.size();
        float w = previewArea.getWidth() - 4;
        float h = previewArea.getHeight() - 4;
        float cx = previewArea.getX() + 2;
        for (int i = 0; i < n; ++i) {
            float x = cx + (float)i / (float)(n - 1) * w;
            float y = cy - previewSamples[i] * h * 0.45f;
            if (i == 0) p.startNewSubPath(x, y);
            else p.lineTo(x, y);
        }
        g.setColour(juce::Colour(180, 220, 255));
        g.strokePath(p, juce::PathStrokeType(1.3f));
    }

    // Label sits *inside* the preview rectangle's top-left corner. Drawing
    // it above (the old behaviour) put the label 14 px up into the
    // magnitude panel, which painted over it and made the strip look
    // unlabeled.
    g.setColour(juce::Colours::lightgrey);
    g.setFont(11.0f);
    g.drawText("Resulting waveform (one cycle, IFFT of magnitude * phase above)",
               previewArea.reduced(6.0f, 3.0f).toNearestInt(),
               juce::Justification::topLeft);
}

void SpectralEditorComponent::timerCallback() {
    stopTimer();
    commitToNode();
    if (onApply) onApply();
}

void SpectralEditorComponent::refreshPreview() {
    // Round fftSize up to power of two, matching the synth-side logic.
    int n = 2;
    while (n < doc.fftSize && n < 16384) n <<= 1;
    int halfBins = n / 2 + 1;

    auto mags   = doc.mag.evaluate(halfBins);
    auto phases = doc.phase.evaluate(halfBins);
    // Clamp magnitudes to >= 0 (mag must be non-negative; freehand could
    // produce a slight negative from drawing below the baseline).
    for (auto& m : mags) if (m < 0.0f) m = 0.0f;

    std::vector<std::complex<float>> spectrum(halfBins);
    for (int k = 0; k < halfBins; ++k) {
        float m = mags[k];
        float ph = phases[k];
        if (k == 0 || k == halfBins - 1)
            spectrum[k] = std::complex<float>(m, 0.0f);
        else
            spectrum[k] = std::complex<float>(m * std::cos(ph), m * std::sin(ph));
    }
    spectrum[0] = 0.0f;  // kill DC

    FFT fft(n);
    fft.inverseReal(spectrum, previewSamples);

    float peak = 0.0f;
    for (float v : previewSamples) peak = std::max(peak, std::abs(v));
    if (peak > 1e-9f) {
        float inv = 1.0f / peak;
        for (float& v : previewSamples) v *= inv;
    }
    repaint();
}

void SpectralEditorComponent::commitToNode() {
    if (externalFrame) {
        // Frame-backed: mirror the working doc into the owned SpectralFrame.
        // The hosting wavetable editor will re-encode the parent wavetable
        // and push it through via onApply().
        externalFrame->doc = doc;
    } else if (graph) {
        if (auto* nd = graph->findNode(nodeId)) {
            // Synchronised: a standalone spectral node classifies as a
            // wavetable source, so TerrainSynthProcessor polls its script on
            // the audio thread. Lock the per-node mutex around the write.
            setNodeScriptSynced(*nd, doc.encode());
            // Mark the project dirty so quit-without-save prompts and
            // autosave both notice these edits. (The externalFrame branch
            // above is inside a layered-wave parent that bumps the flag
            // through its own commitToNode.)
            graph->dirty = true;
        }
    }
}

void SpectralEditorComponent::onCurveChanged() {
    refreshPreview();
    if (externalFrame != nullptr) {
        // Embedded inside the layered-wave editor: commit immediately so the
        // parent's f->render() sees fresh data, and call onApply right away
        // so the parent re-paints its single-cycle preview every drag tick.
        // The parent already debounces its own audio-graph rebuild (150ms),
        // so we don't add extra latency by doing this. Without this the
        // parent preview only refreshed 500ms after the user stopped editing,
        // which looked like "phase edits don't affect the resulting wave".
        commitToNode();
        if (onApply) onApply();
        return;
    }
    // Standalone (node-graph) mode: onApply triggers an audio-graph rebuild
    // (requestRebuild()), which is expensive — keep the 500ms debounce so
    // typing in the equation field doesn't fight the audio thread.
    startTimer(500);
}

} // namespace SoundShop

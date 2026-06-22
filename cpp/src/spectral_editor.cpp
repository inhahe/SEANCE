#define _USE_MATH_DEFINES
#include "spectral_editor.h"
#include "fft_util.h"        // FFT for the preview
#include "help_utils.h"
#include "node_graph_component.h"  // launchAhdsrEnvelopeDialog for the Envelope... button
#include "layered_wave_editor.h"   // WavetableDoc - resolve SpectralFrames nested in wavetables
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
    // Optional 4th field: per-bin warp chain. Omitted when empty so files made
    // before warp existed (and old binaries reading new files) round-trip
    // unchanged - the decoder only looks for parts[3] when present.
    if (!warpChain.empty())
        o << "|warp:" << encodeWarpChain(warpChain);
    // Optional field: FrequencyGraph live-reference ids, emitted AFTER the warp
    // block so an old decoder (which only checked parts[3] for a "warp:" prefix)
    // still finds its warp and harmlessly ignores this trailing field. Present
    // only when at least one curve is linked.
    if (magAssetId >= 0 || phaseAssetId >= 0)
        o << "|refs:" << magAssetId << ":" << phaseAssetId;
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
        // Optional trailing fields (4th onward), each self-identified by a
        // prefix so order is flexible and unknown fields are skipped:
        //   "warp:<chain>"      - per-bin warp chain (absent in pre-warp files)
        //   "refs:<mag>:<phase>" - FrequencyGraph live-reference asset ids
        warpChain.clear();
        magAssetId = -1;
        phaseAssetId = -1;
        for (size_t i = 3; i < parts.size(); ++i) {
            if (parts[i].rfind("warp:", 0) == 0) {
                warpChain = decodeWarpChain(parts[i].substr(5));
            } else if (parts[i].rfind("refs:", 0) == 0) {
                auto ids = splitChar(parts[i].substr(5), ':');
                if (ids.size() >= 1) { try { magAssetId   = std::stoi(ids[0]); } catch (...) {} }
                if (ids.size() >= 2) { try { phaseAssetId = std::stoi(ids[1]); } catch (...) {} }
            }
        }
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
// FrequencyGraph live-reference resolution
// =============================================================================

// Resolve one linked curve. assetId is updated in place (detached to -1 if the
// referenced asset is gone). Returns true if anything changed. *refreshed is
// incremented only when a live asset was actually re-decoded into the curve
// (not when the id was detached because the asset vanished).
static bool resolveSpectralCurveRef(NodeGraph& graph, int& assetId,
                                    SpectralCurve& curve, int& refreshed) {
    if (assetId < 0) return false;
    const AssetEntry* e = graph.assets.find(assetId);
    if (e && e->kind == AssetKind::FrequencyGraph) {
        SpectralCurve c;
        if (SpectralCurve::decode(e->payload, c)) {  // decode() rebakes Lua/Python
            curve = std::move(c);
            ++refreshed;
            return true;
        }
        return false;  // malformed payload - leave the cached curve untouched
    }
    assetId = -1;      // referenced asset gone -> detach, keep last cached curve
    return true;
}

// Resolve both curves of one doc. Returns true if either changed.
static bool resolveSpectralDocRefs(NodeGraph& graph, SpectralDoc& doc, int& refreshed) {
    bool changed = false;
    changed |= resolveSpectralCurveRef(graph, doc.magAssetId,   doc.mag,   refreshed);
    changed |= resolveSpectralCurveRef(graph, doc.phaseAssetId, doc.phase, refreshed);
    return changed;
}

int resolveSpectralReferences(NodeGraph& graph) {
    int refreshed = 0;
    for (auto& n : graph.nodes) {
        // Standalone Frequency Domain node - script is the SpectralDoc directly.
        if (n.script.rfind("__spectral2__", 0) == 0 ||
            n.script.rfind("__spectral__", 0) == 0) {
            SpectralDoc doc;
            if (!doc.decode(n.script)) continue;
            if (resolveSpectralDocRefs(graph, doc, refreshed))
                setNodeScriptSynced(n, doc.encode());
            continue;
        }
        // Wavetable node - SpectralFrames can be nested in the frame library.
        if (n.script.rfind("__wavetable", 0) == 0) {
            WavetableDoc wt;
            if (!wt.decode(n.script)) continue;
            bool any = false;
            for (auto& entry : wt.library) {
                auto* sf = dynamic_cast<SpectralFrame*>(entry.wave.get());
                if (sf && resolveSpectralDocRefs(graph, sf->doc, refreshed))
                    any = true;
            }
            if (any)
                setNodeScriptSynced(n, wt.encode());
        }
    }
    return refreshed;
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

    // Bucket C per-bin warp: treat the magnitude-vs-bin array as a buffer and
    // run the warp chain over it. Amplitude-domain ops reshape the magnitude
    // envelope; phase-domain ops stretch / shift the spectrum along the bin
    // axis. Re-clamp to >= 0 afterwards since some transfers (fold/flip) can
    // drive a magnitude negative, which is meaningless for a magnitude.
    if (!doc.warpChain.empty()) {
        applyWarpChain(doc.warpChain, mags);
        for (auto& m : mags) if (m < 0.0f) m = -m;  // |mag|
    }

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
    assetGraph = &g;  // node-backed: library links resolve against this graph
    if (auto* nd = graph->findNode(nodeId)) {
        if (!doc.decode(nd->script))
            doc = SpectralDoc::defaultBuiltin();
    } else {
        doc = SpectralDoc::defaultBuiltin();
    }
    initUI();
}

SpectralEditorComponent::SpectralEditorComponent(SpectralFrame& frame,
                                                  std::function<void()> apply,
                                                  NodeGraph* ag)
    : graph(nullptr), nodeId(0), externalFrame(&frame), onApply(std::move(apply)),
      assetGraph(ag)
{
    // Seed the editor from the frame's existing doc. No owning NodeGraph for
    // commits (those mirror into externalFrame->doc), but `assetGraph` - when
    // the wavetable shell passes its project graph - still drives FrequencyGraph
    // library linking.
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

    // Read-only badge → break the library link and fork an independent copy
    // (the only unlink path; see SpectralCurvePanel::onUnlink). Only meaningful
    // when an asset store backs the links.
    if (assetGraph != nullptr) {
        phasePanel->onUnlink = [this]() { unlinkCurveLib(false); };
        magPanel->onUnlink   = [this]() { unlinkCurveLib(true); };
    }

    // Per-curve FrequencyGraph library link affordances. Only meaningful when
    // there's a project graph whose asset store backs the links.
    if (assetGraph != nullptr) {
        auto setupLib = [this](juce::TextButton& btn, juce::Label& lbl,
                               bool isMag, const char* which) {
            addAndMakeVisible(btn);
            btn.setTooltip(juce::String("Publish this ") + which +
                " curve to the project's Frequency Graphs library, load a copy "
                "of a library curve (independent), or sync to one as a live, "
                "read-only mirror. A synced curve is edited only in the library; "
                "click the panel's read-only badge to fork an editable copy.");
            btn.onClick = [this, isMag]() { openCurveLibrary(isMag); };
            addAndMakeVisible(lbl);
            lbl.setFont(11.0f);
            lbl.setColour(juce::Label::textColourId, juce::Colour(0xFFAAAAAA));
        };
        setupLib(phaseLibraryBtn, phaseLinkLabel, false, "phase");
        setupLib(magLibraryBtn,   magLinkLabel,   true,  "magnitude");
        refreshLinkLabels();
        refreshReadOnly();
    }

    // Per-bin warp chain (Bucket C). Sits in a strip below the curve panels.
    {
        WarpChainEditor::Callbacks wcb;
        wcb.onChanged = [this]() { onCurveChanged(); };
        wcb.onStructureChanged = [this]() { resized(); onCurveChanged(); };
        warpEditor = std::make_unique<WarpChainEditor>(std::move(wcb));
        warpEditor->setChain(&doc.warpChain);
        addAndMakeVisible(*warpEditor);
    }

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
        // Amplitude-envelope editor (node-backed mode only - graph is set).
        if (graph != nullptr) {
            addAndMakeVisible(envelopeBtn);
            envelopeBtn.setTooltip("Edit the amplitude envelope (Attack/Hold/Decay/Sustain/Release "
                                   "shape, per-stage curve, velocity sensitivity) that controls how "
                                   "each note fades in and out. Opens in a separate window.");
            envelopeBtn.onClick = [this]() {
                if (graph) launchAhdsrEnvelopeDialog(this, *graph, nodeId);
            };

            // Held audition (Preview): plays the edited spectrum as a sustained
            // note through this node's voice, refreshing live as you edit.
            addAndMakeVisible(playBtn);
            playBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(60, 110, 70));
            playBtn.setTooltip("Audition this waveform: holds a sustained A4 note through this "
                               "node's voice so you hear edits live. The node must reach an Output "
                               "to be heard. Click again to stop.");
            playBtn.onClick = [this]() { togglePreview(); };
        }
    }
    addAndMakeVisible(helpBtn);
    helpBtn.setTooltip("Open the help page for the frequency-domain editor.");
    helpBtn.onClick = []() { openHelpDocFile("wavetables.html"); };

    refreshPreview();
    setSize(720, 520);
}

SpectralEditorComponent::~SpectralEditorComponent() {
    stopTimer();
    // Release any held audition so the synth voice doesn't keep sounding after
    // the editor closes.
    if (framePlaying && graph)
        if (auto* nd = graph->findNode(nodeId)) clearNodeHeldAudition(*nd);
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
        if (graph != nullptr) {
            envelopeBtn.setBounds(top.removeFromRight(90));
            top.removeFromRight(8);
            playBtn.setBounds(top.removeFromRight(80));
            top.removeFromRight(8);
        }
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

    // Per-bin warp chain strip, just below the curve panels. Height tracks the
    // op count (an empty chain only shows the header + Add affordance).
    if (warpEditor) {
        warpEditor->setBounds(a.removeFromBottom(warpEditor->preferredHeight()));
        a.removeFromBottom(4);
    }

    // Two stacked curve panels: phase on top, mag on bottom. When library
    // linking is available (assetGraph set), each panel gets a thin row above
    // it carrying the "Library..." button and the link-status label.
    const int linkRowH = (assetGraph != nullptr) ? 22 : 0;
    int half = a.getHeight() / 2;
    {
        auto top = a.removeFromTop(half - 2);
        if (assetGraph != nullptr) {
            auto row = top.removeFromTop(linkRowH);
            phaseLibraryBtn.setBounds(row.removeFromLeft(80));
            row.removeFromLeft(6);
            phaseLinkLabel.setBounds(row);
            top.removeFromTop(2);
        }
        phasePanel->setBounds(top);
    }
    a.removeFromTop(4);
    {
        auto bot = a;
        if (assetGraph != nullptr) {
            auto row = bot.removeFromTop(linkRowH);
            magLibraryBtn.setBounds(row.removeFromLeft(80));
            row.removeFromLeft(6);
            magLinkLabel.setBounds(row);
            bot.removeFromTop(2);
        }
        magPanel->setBounds(bot);
    }
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
    // Render through the shared path so the preview matches the synth exactly,
    // including the per-bin warp chain (Bucket C). doc.fftSize is rounded up to
    // a power of two inside renderSpectralToWaveform, same as the synth side.
    renderSpectralToWaveform(doc, doc.fftSize, previewSamples);
    repaint();
    // Keep a running Preview in sync: the on-screen waveform IS the cycle we
    // audition, so ship the freshly-rendered one whenever it changes.
    refreshPreviewAudition();
}

void SpectralEditorComponent::togglePreview() {
    if (graph == nullptr) return;   // node-backed only
    auto* nd = graph->findNode(nodeId);
    if (!nd) return;
    if (framePlaying) {
        framePlaying = false;
        clearNodeHeldAudition(*nd);
        playBtn.setButtonText("Preview");
        playBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(60, 110, 70));
    } else {
        framePlaying = true;
        playBtn.setButtonText("Stop");
        playBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(140, 70, 70));
        refreshPreviewAudition();   // ships the held note
    }
}

void SpectralEditorComponent::refreshPreviewAudition() {
    if (!framePlaying || graph == nullptr) return;
    auto* nd = graph->findNode(nodeId);
    if (!nd) return;
    // previewSamples is the final single cycle (peak-normalised IFFT, with the
    // per-bin warp baked in) - exactly what the synth bakes into its terrain.
    setNodeHeldAuditionCycle(*nd, previewSamples);
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
    // No write-back: a linked curve is read-only, so an edit can only happen on
    // an independent (unlinked) curve, which has no asset to propagate to. The
    // only way to change a shared library item is to edit it in the library.
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

void SpectralEditorComponent::refreshReadOnly() {
    if (assetGraph == nullptr) return;
    if (phasePanel) phasePanel->setReadOnly(doc.phaseAssetId >= 0);
    if (magPanel)   magPanel->setReadOnly(doc.magAssetId >= 0);
}

void SpectralEditorComponent::refreshLinkLabels() {
    if (assetGraph == nullptr) return;
    auto describe = [this](int assetId, juce::Label& lbl) {
        if (assetId < 0) {
            lbl.setText("Independent curve (not in library)",
                        juce::dontSendNotification);
        } else {
            const AssetEntry* e = assetGraph->assets.find(assetId);
            juce::String nm = e ? juce::String(e->name) : juce::String("(missing)");
            lbl.setText("Linked: " + nm + " (#" + juce::String(assetId) +
                        ")  - read only (click the badge to edit a copy)",
                        juce::dontSendNotification);
        }
    };
    describe(doc.phaseAssetId, phaseLinkLabel);
    describe(doc.magAssetId,   magLinkLabel);
}

void SpectralEditorComponent::openCurveLibrary(bool isMag) {
    if (assetGraph == nullptr) return;
    SpectralCurve&  curve     = isMag ? doc.mag : doc.phase;
    int             currentId = isMag ? doc.magAssetId : doc.phaseAssetId;
    juce::Component* anchor   = isMag ? (juce::Component*)&magLibraryBtn
                                      : (juce::Component*)&phaseLibraryBtn;
    juce::String name = juce::String(isMag ? "Magnitude" : "Phase") + " curve";

    showFrequencyGraphLibraryMenu(anchor, *assetGraph, curve, currentId, name,
        [this, isMag](int newId) {
            (isMag ? doc.magAssetId : doc.phaseAssetId) = newId;
            // The menu may have replaced the curve (load-copy / link case);
            // reflect it in the panel's text/toggles.
            (isMag ? magPanel : phasePanel)->syncFromModel();
            // Persist the new link state. No write-back: linking mirrors the
            // asset INTO our curve (read-only); a copy/unlink is independent.
            commitToNode();
            refreshLinkLabels();
            refreshReadOnly();
            refreshPreview();
            if (onApply) onApply();
        });
}

void SpectralEditorComponent::unlinkCurveLib(bool isMag) {
    if (assetGraph == nullptr) return;
    // Break the live link and keep the current (now independent) curve. Mirrors
    // the onChanged(-1) path of openCurveLibrary; invoked from the panel badge.
    (isMag ? doc.magAssetId : doc.phaseAssetId) = -1;
    (isMag ? magPanel : phasePanel)->syncFromModel();
    commitToNode();
    refreshLinkLabels();
    refreshReadOnly();
    refreshPreview();
    if (onApply) onApply();
}

// ==============================================================================
// CurveEQEditorComponent - editor for a Curve EQ node (__curveeq__:).
// A single magnitude-response curve panel + FrequencyGraph library linking.
// ==============================================================================

CurveEQEditorComponent::CurveEQEditorComponent(NodeGraph& g, int nId,
                                               std::function<void()> apply)
    : graph(g), nodeId(nId), onApply(std::move(apply))
{
    if (auto* nd = graph.findNode(nodeId)) {
        if (!CurveEq::decode(nd->script, curve, assetId)) {
            curve = SpectralCurve();
            curve.expression = "1";
            assetId = -1;
        }
    } else {
        curve = SpectralCurve();
        curve.expression = "1";
    }

    addAndMakeVisible(titleLabel);
    titleLabel.setText("Frequency response (gain multiplier vs. frequency)",
                       juce::dontSendNotification);
    titleLabel.setFont(13.0f);
    titleLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFCCCCCC));

    // y range 0..2: 1.0 = unity (flat), <1 cuts, >1 boosts. The processor
    // clamps to [0,8] so a steep drawn curve can still boost hard if wanted.
    curvePanel = std::make_unique<SpectralCurvePanel>(curve, "Gain",
        0.0f, 2.0f, juce::Colour(150, 230, 170),
        [this]() { onCurveChanged(); });
    addAndMakeVisible(curvePanel.get());
    // Read-only badge → break the link and fork an independent copy.
    curvePanel->onUnlink = [this]() {
        assetId = -1;
        if (curvePanel) curvePanel->syncFromModel();
        commitToNode();
        refreshLinkLabel();
        refreshReadOnly();
        if (onApply) onApply();
    };

    addAndMakeVisible(libraryBtn);
    libraryBtn.setTooltip("Publish this response curve to the project's Frequency "
        "Graphs library, load a copy of a library curve (independent), or sync to "
        "one as a live, read-only mirror. A synced curve is edited only in the "
        "library; click the panel's read-only badge to fork an editable copy.");
    libraryBtn.onClick = [this]() { openLibrary(); };

    addAndMakeVisible(linkLabel);
    linkLabel.setFont(11.0f);
    linkLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFAAAAAA));

    addAndMakeVisible(closeBtn);
    closeBtn.setTooltip("Close this editor. Unapplied edits are committed first.");
    closeBtn.onClick = [this]() {
        commitToNode();
        if (onApply) onApply();
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState(0);
    };

    refreshLinkLabel();
    refreshReadOnly();
    setSize(560, 360);
}

CurveEQEditorComponent::~CurveEQEditorComponent() {
    stopTimer();
}

void CurveEQEditorComponent::resized() {
    auto a = getLocalBounds().reduced(8);

    auto top = a.removeFromTop(26);
    closeBtn.setBounds(top.removeFromRight(72));
    titleLabel.setBounds(top);
    a.removeFromTop(4);

    auto row = a.removeFromTop(22);
    libraryBtn.setBounds(row.removeFromLeft(80));
    row.removeFromLeft(6);
    linkLabel.setBounds(row);
    a.removeFromTop(2);

    if (curvePanel) curvePanel->setBounds(a);
}

void CurveEQEditorComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(22, 22, 28));
}

void CurveEQEditorComponent::timerCallback() {
    stopTimer();
    commitToNode();
    if (onApply) onApply();
}

void CurveEQEditorComponent::onCurveChanged() {
    // No write-back: a linked curve is read-only, so edits only happen on an
    // independent curve with no asset to propagate to. A shared library item is
    // changed only by editing it in the library.
    // Debounce the audio-graph rebuild, same as the spectral editor.
    startTimer(500);
}

void CurveEQEditorComponent::commitToNode() {
    if (auto* nd = graph.findNode(nodeId)) {
        setNodeScriptSynced(*nd, CurveEq::encode(curve, assetId));
        graph.dirty = true;
        graph.commitSnapshot("Edit Curve EQ");
    }
}

void CurveEQEditorComponent::refreshReadOnly() {
    if (curvePanel) curvePanel->setReadOnly(assetId >= 0);
}

void CurveEQEditorComponent::refreshLinkLabel() {
    if (assetId < 0) {
        linkLabel.setText("Independent curve (not in library)",
                          juce::dontSendNotification);
    } else {
        const AssetEntry* e = graph.assets.find(assetId);
        juce::String nm = e ? juce::String(e->name) : juce::String("(missing)");
        linkLabel.setText("Linked: " + nm + " (#" + juce::String(assetId) +
                          ")  - read only (click the badge to edit a copy)",
                          juce::dontSendNotification);
    }
}

void CurveEQEditorComponent::openLibrary() {
    showFrequencyGraphLibraryMenu(&libraryBtn, graph, curve, assetId,
        "Curve EQ response",
        [this](int newId) {
            assetId = newId;
            if (curvePanel) curvePanel->syncFromModel();
            commitToNode();
            refreshLinkLabel();
            refreshReadOnly();
            if (onApply) onApply();
        });
}

} // namespace SoundShop

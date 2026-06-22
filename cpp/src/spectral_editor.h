#pragma once
#include "node_graph.h"
#include "wavetable_frame.h"
#include "warp.h"           // WarpOp (per-bin element warp)
#include "warp_editor.h"    // WarpChainEditor (per-bin warp UI)
#include "curve_editor.h"   // SpectralCurve + SpectralCurvePanel (reusable)
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <functional>
#include <string>
#include <memory>

namespace SoundShop {

// SpectralCurve (mag/phase authoring with Equation / Points / Freehand
// modes) lives in curve_editor.h and is reused by other parts of the
// codebase (e.g. SpectrumTap's per-bin frequency-response curves).

// One full frequency-domain wavetable frame: a magnitude curve and a
// phase curve combined into a real spectrum, then IFFTed to a single-cycle
// waveform on the synth side. This is the spectral analogue of one
// LayeredWaveform frame.
//
// Format note: the old `__spectral__:<fftSize>:<phaseMode>:<mag>|<phase>`
// string is still recognised on decode (one of the four phase modes
// converts to an equivalent Equation expression). The new format is
// emitted as `__spectral2__:<fftSize>|<magCurve>|<phaseCurve>`.
struct SpectralDoc {
    SpectralCurve mag;
    SpectralCurve phase;
    int fftSize = 2048;

    // Live references to project FrequencyGraph assets, mirroring the AHDSR /
    // MorphAlgorithm / SpectrumTap "live reference" model. -1 = independent
    // (the curve above is this doc's own). When >= 0, the corresponding curve
    // is a cache of the asset's content, refreshed by resolveSpectralReferences()
    // at edit/load; editing a linked curve writes back to the asset and
    // propagates to every consumer sharing the id. Serialized in encode()/
    // decode() under the "refs" key (after the optional "warp" block, so old
    // decoders that only look for "warp:" at parts[3] still round-trip).
    int magAssetId   = -1;
    int phaseAssetId = -1;

    // Bucket C element warp: a chain of shape-bending ops applied to the
    // per-bin magnitude array (over the bin axis) before the IFFT. Amplitude-
    // domain ops reshape the spectral envelope (fold / clip / saturate the
    // magnitudes); phase-domain ops stretch / shift the spectrum along the bin
    // axis. Baked at render (not modulatable), like the per-layer warp.
    std::vector<WarpOp> warpChain;

    std::string encode() const;                  // produces __spectral2__: form
    bool        decode(const std::string& s);    // accepts new or legacy

    static SpectralDoc defaultBuiltin();         // exp(-f/20) mag, random phase
};

// Resolve every SpectralDoc FrequencyGraph live reference in the graph. Walks
// both standalone Frequency Domain nodes (script "__spectral2__:...") and
// SpectralFrames nested inside wavetable nodes (script "__wavetable...:"). For
// each linked curve (magAssetId / phaseAssetId >= 0): if the asset still exists
// and is a FrequencyGraph, re-decode its payload into the cached curve; if the
// asset is gone, detach the id to -1 (keeping the last cached curve so nothing
// dangles). Re-encodes the node script when anything changed. Returns the number
// of curves refreshed. Call after readProject() and after any edit that may have
// changed an asset a curve points at (mirrors resolveAhdsr/Warp/SpectrumTap).
int resolveSpectralReferences(NodeGraph& graph);

// Render a SpectralDoc to a single-cycle time-domain waveform at the
// requested table size (rounded up internally to the nearest power of two,
// capped at 16384). Output is peak-normalised to 1.0. Used by both the
// terrain fill path and the SpectralFrame wavetable-frame adapter so the
// rendering rules stay consistent in both places.
void renderSpectralToWaveform(const SpectralDoc& doc,
                              int tableSize,
                              std::vector<float>& out);

// SpectralFrame wraps a SpectralDoc as an IWavetableFrame so a frequency-
// domain frame can sit alongside layered and wavelet frames in a single
// mixed-type wavetable. The wire format used in __wavetable2__ is the body
// of SpectralDoc::encode() (i.e. without the "__spectral2__:" prefix).
struct SpectralFrame : public IWavetableFrame {
    SpectralDoc doc;

    SpectralFrame() = default;
    explicit SpectralFrame(SpectralDoc d) : doc(std::move(d)) {}

    const char* typeId() const override { return "spectral"; }
    void renderRaw(int tableSize, std::vector<float>& out) const override;
    std::string encodeBody() const override;
    bool decodeBody(const std::string& body) override;
    std::unique_ptr<IWavetableFrame> clone() const override;
};

// Editor window contents (paired with juce::DialogWindow launched by the
// caller). Edits `node.script` directly. onApply() is called on a debounce
// timer to request a graph rebuild, same as the waveform editor.
//
// Layout: phase curve on top, magnitude curve on bottom (the user spec
// says "phase one above the amplitude one"). Each curve panel has a
// mode toggle (Equation / Draw), and the Drawn mode has a Points /
// Freehand sub-toggle and a 2D canvas you can click-drag in.
class SpectralEditorComponent : public juce::Component, private juce::Timer {
public:
    // Node-backed mode: read/write the SpectralDoc through node.script. Used
    // when the editor opens directly on a Frequency Domain node.
    SpectralEditorComponent(NodeGraph& graph, int nodeId,
                            std::function<void()> onApply);

    // Frame-backed mode: read/write an external SpectralFrame's SpectralDoc
    // directly. Used when this editor is launched as a sub-dialog from the
    // wavetable shell to edit a single SpectralFrame inside a mixed-type
    // wavetable. onApply is invoked after every commit so the owning wavetable
    // editor can re-render its preview / push the updated wavetable through to
    // its host node. `assetGraph` (the host's project graph) is optional: when
    // supplied it enables the per-curve FrequencyGraph library link buttons
    // (publish / link / detach + live propagation), exactly as in node mode;
    // when null those buttons are hidden.
    SpectralEditorComponent(SpectralFrame& externalFrame,
                            std::function<void()> onApply,
                            NodeGraph* assetGraph = nullptr);

    ~SpectralEditorComponent() override;

    void resized() override;
    void paint(juce::Graphics& g) override;
    void timerCallback() override;

private:
    // Either (graph, nodeId) or externalFrame is active, never both.
    NodeGraph* graph = nullptr;
    int nodeId = 0;
    SpectralFrame* externalFrame = nullptr;
    std::function<void()> onApply;

    // Project graph used for FrequencyGraph library linking. In node-backed
    // mode this is `graph`; in frame-backed mode it's the optional assetGraph
    // passed by the wavetable shell. Null => library link buttons are hidden.
    NodeGraph* assetGraph = nullptr;

    void initUI();

    SpectralDoc doc;
    std::vector<float> previewSamples;   // resulting time-domain waveform

    juce::ComboBox fftSizeCombo;
    juce::Label    fftSizeLabel;
    juce::TextButton applyBtn { "Apply" };
    juce::TextButton closeBtn { "Close" };
    juce::TextButton helpBtn  { "?" };
    // Opens the shared AHDSR amplitude-envelope editor for this node in a
    // separate dialog. Only shown in node-backed mode (graph != nullptr);
    // in frame-backed sub-editor mode there is no node envelope to edit.
    juce::TextButton envelopeBtn { "Envelope..." };
    // Sustained held audition (the generic editor "Preview" button - same role
    // as the wavetable shell's playBtn). Node-backed mode only; in frame-backed
    // mode the wavetable shell owns the Preview button instead.
    juce::TextButton playBtn { "Preview" };
    bool framePlaying = false;

    std::unique_ptr<SpectralCurvePanel> phasePanel;  // top
    std::unique_ptr<SpectralCurvePanel> magPanel;    // bottom

    // Per-curve FrequencyGraph library link affordances. Shown only when
    // assetGraph != nullptr. Each "Library..." button opens the shared
    // publish/link/detach menu; the label shows the current link status.
    juce::TextButton phaseLibraryBtn { "Library..." };
    juce::TextButton magLibraryBtn   { "Library..." };
    juce::Label      phaseLinkLabel;
    juce::Label      magLinkLabel;

    // Open the shared library menu for the mag (isMag) or phase curve.
    void openCurveLibrary(bool isMag);
    // Break the live library link for the mag (isMag) or phase curve and keep an
    // independent copy. Wired to the panel's read-only "edit a copy" badge.
    void unlinkCurveLib(bool isMag);
    // Refresh both "Independent / Linked to ..." status lines from doc ids.
    void refreshLinkLabels();
    // Lock each panel for editing iff its curve is a live library link. A linked
    // curve is read-only (edit it in the library, or click the panel badge to
    // fork an editable copy) - consumers
    // never write back to the asset. No-op when there's no assetGraph.
    void refreshReadOnly();

    // Per-bin warp chain editor (Bucket C). Bound to doc.warpChain; baked into
    // the spectrum at render, so its callbacks only re-render + commit (never
    // touch node params - this warp is not modulatable).
    std::unique_ptr<WarpChainEditor> warpEditor;

    void refreshPreview();
    void commitToNode();
    void onCurveChanged();

    // Held-audition (Preview) control. togglePreview starts/stops; ship re-sends
    // the freshly-rendered cycle whenever an edit changes it (no-op unless a
    // Preview is active). Both no-op in frame-backed mode (graph == nullptr).
    void togglePreview();
    void refreshPreviewAudition();
};

// =============================================================================
// CurveEQEditorComponent - editor for a Curve EQ node (__curveeq__:).
//
// Edits the single magnitude-response SpectralCurve stored in the node script,
// with the same FrequencyGraph library linking as the spectral editor (publish
// / link existing / detach, with live propagation to every consumer sharing the
// asset). Just one curve panel + a Library... button; FFT Size and Mix stay as
// node params shown in the node body. onApply() is debounced (500 ms) to request
// a graph rebuild, same as the spectral / waveform editors.
// =============================================================================
class CurveEQEditorComponent : public juce::Component, private juce::Timer {
public:
    CurveEQEditorComponent(NodeGraph& graph, int nodeId,
                           std::function<void()> onApply);
    ~CurveEQEditorComponent() override;

    void resized() override;
    void paint(juce::Graphics& g) override;
    void timerCallback() override;

private:
    NodeGraph& graph;
    int nodeId;
    std::function<void()> onApply;

    SpectralCurve curve;       // working copy, mirrored to node.script on commit
    int assetId = -1;          // FrequencyGraph link (-1 = independent)

    juce::Label      titleLabel;
    std::unique_ptr<SpectralCurvePanel> curvePanel;
    juce::TextButton libraryBtn { "Library..." };
    juce::Label      linkLabel;
    juce::TextButton closeBtn   { "Close" };

    void onCurveChanged();
    void commitToNode();          // re-encode node.script + snapshot
    void openLibrary();
    void refreshReadOnly();       // lock the panel iff the curve is a live link
    void refreshLinkLabel();
};

} // namespace SoundShop

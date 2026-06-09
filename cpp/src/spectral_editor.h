#pragma once
#include "node_graph.h"
#include "wavetable_frame.h"
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

    std::string encode() const;                  // produces __spectral2__: form
    bool        decode(const std::string& s);    // accepts new or legacy

    static SpectralDoc defaultBuiltin();         // exp(-f/20) mag, random phase
};

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
    // directly, no NodeGraph involved. Used when this editor is launched as
    // a sub-dialog from the wavetable shell to edit a single SpectralFrame
    // inside a mixed-type wavetable. onApply is invoked after every commit
    // so the owning wavetable editor can re-render its preview / push the
    // updated wavetable through to its host node.
    SpectralEditorComponent(SpectralFrame& externalFrame,
                            std::function<void()> onApply);

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

    void initUI();

    SpectralDoc doc;
    std::vector<float> previewSamples;   // resulting time-domain waveform

    juce::ComboBox fftSizeCombo;
    juce::Label    fftSizeLabel;
    juce::TextButton applyBtn { "Apply" };
    juce::TextButton closeBtn { "Close" };
    juce::TextButton helpBtn  { "?" };

    std::unique_ptr<SpectralCurvePanel> phasePanel;  // top
    std::unique_ptr<SpectralCurvePanel> magPanel;    // bottom

    void refreshPreview();
    void commitToNode();
    void onCurveChanged();
};

} // namespace SoundShop

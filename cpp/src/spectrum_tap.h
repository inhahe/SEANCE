#pragma once
#include "node_graph.h"
#include "curve_editor.h"
#include "fft_util.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <complex>
#include <functional>
#include <memory>
#include <vector>
#include <cmath>

namespace SoundShop {

// A frequency bin: bandpass filter + envelope follower extracting amplitude
// at a chosen frequency range. Updated per sample, output is a smooth 0..1
// amplitude envelope usable as a control signal.
//
// Two response modes coexist:
//   - Default (useCustomResponse=false): a 2nd-order biquad bandpass at
//     centerFreq with the requested bandwidth, followed by a one-pole
//     envelope follower. Per-sample, low latency.
//   - Custom (useCustomResponse=true): the bin's amplitude is computed
//     in the frequency domain as a weighted sum of the input's magnitude
//     spectrum, with `responseCurve` providing the per-FFT-bin weight
//     across the bin's [centerFreq - bw/2, centerFreq + bw/2] range. The
//     curve x-axis is normalised so x=0 is the left edge of the bin and
//     x=1 is the right edge. Updated once per FFT hop (block latency).
struct FrequencyBin {
    float centerFreq = 1000.0f;  // Hz
    float bandwidth  = 200.0f;   // Hz (width of the band)
    uint32_t color   = 0xFFFF8800;

    bool useCustomResponse = false;
    SpectralCurve responseCurve;

    // Biquad bandpass state (2nd-order IIR), used when !useCustomResponse
    float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0; // coefficients
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;           // state

    // Envelope follower state (one-pole lowpass on rectified signal)
    float envelope = 0.0f;
    float smoothCoeff = 0.0f; // computed from sample rate

    // Recompute coefficients for the current freq/bandwidth/sampleRate
    void computeCoeffs(double sampleRate) {
        double Q = std::max(0.1, (double)centerFreq / std::max(1.0, (double)bandwidth));
        double w0 = 2.0 * 3.14159265358979 * centerFreq / sampleRate;
        double sinW0 = std::sin(w0);
        double cosW0 = std::cos(w0);
        double alpha = sinW0 / (2.0 * Q);

        double a0 = 1.0 + alpha;
        b0 = (float)(alpha / a0);
        b1 = 0.0f;
        b2 = (float)(-alpha / a0);
        a1 = (float)(-2.0 * cosW0 / a0);
        a2 = (float)((1.0 - alpha) / a0);

        // Envelope smoothing: ~10ms attack, ~50ms release
        double smoothTime = 0.02; // 20ms average
        smoothCoeff = (float)(1.0 - std::exp(-1.0 / (smoothTime * sampleRate)));
    }

    // Process one sample through the bandpass + envelope follower.
    // Returns the current envelope amplitude (0..~1).
    float process(float input) {
        // Biquad bandpass
        float filtered = b0 * input + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = input;
        y2 = y1; y1 = filtered;

        // Rectify + smooth
        float rectified = std::abs(filtered);
        envelope += smoothCoeff * (rectified - envelope);
        return envelope;
    }

    void reset() {
        x1 = x2 = y1 = y2 = 0;
        envelope = 0;
    }
};

// Audio processor inserted inline on a wire. Passes audio through unchanged
// while running frequency bins on it. Each bin's amplitude is available as
// a named param on the node (readable by downstream signal connections).
class SpectrumTapProcessor : public juce::AudioProcessor {
public:
    SpectrumTapProcessor(Node& node);

    const juce::String getName() const override { return "SpectrumTap"; }
    void prepareToPlay(double sr, int bs) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    // Default and bounds for the user-settable FFT size. The FFT runs only
    // when at least one bin is in custom-response mode, so biquad-only taps
    // pay no cost. Smaller sizes mean lower latency and CPU, but coarser
    // frequency resolution per FFT bin.
    static constexpr int kDefaultFftSize = 1024;
    static constexpr int kMinFftSize = 128;
    static constexpr int kMaxFftSize = 8192;

    // Encode the per-bin custom response curves into node.script. Result
    // has the form `__spectrumtap__|<fftSize>|<curve1>|<curve2>|...`
    // where each <curveK> is either empty (biquad default) or a
    // SpectralCurve::encode() payload. When `fftSize` is the default
    // AND there are no custom curves, the result collapses to plain
    // `__spectrumtap__` so legacy / biquad-only taps round-trip cleanly.
    static std::string encodeScript(const std::vector<FrequencyBin>& bins,
                                    int fftSize);

    // Decode a node.script value (prefix `__spectrumtap__`) into the FFT
    // size and a parallel pair of vectors: a curve per bin (default-
    // constructed where omitted) and a useCustomResponse flag per bin.
    // The plain `__spectrumtap__` value returns empty vectors and the
    // default FFT size. The first field after the tag is parsed as an
    // int FFT size when it is purely numeric; otherwise it is treated
    // as a curve slot for backwards compatibility with the very first
    // iteration of this format (which had no fftSize header).
    static void decodeScript(const std::string& script,
                             int& outFftSize,
                             std::vector<SpectralCurve>& outCurves,
                             std::vector<bool>& outUseCustom);

private:
    Node& node;
    double sampleRate = 44100;
    std::vector<FrequencyBin> bins;

    // FFT machinery for custom-response bins. Allocated lazily; only used
    // when at least one bin has useCustomResponse == true.
    std::unique_ptr<FFT> fft;
    int fftSize = 1024;
    std::vector<float> fftInputRing;       // size fftSize, sliding buffer
    int fftWriteIdx = 0;
    std::vector<std::complex<float>> fftScratchTime;  // size fftSize
    std::vector<std::complex<float>> fftScratchFreq;  // size fftSize/2+1
    std::vector<float> fftMag;             // size fftSize/2+1
    // Per-bin FFT-bin weights (rows: bins, cols: halfBins+1). Only the
    // rows for custom bins are filled; biquad rows are left empty.
    std::vector<std::vector<float>> binFftWeights;
    bool anyCustomBin = false;

    void rebuildBins();
    void rebuildBinFftWeights();
    void ensureFFTReady();
};

// UI for the spectrum tap: shows bins on a frequency axis, lets user
// add/remove/resize them, displays live amplitude levels.
class SpectrumTapComponent : public juce::Component, private juce::Timer {
public:
    // onTopologyChanged: invoked whenever a bin is added or removed.
    // The host should call requestRebuild() on the GraphProcessor and
    // repaint the node graph component so the new Signal Out pin is
    // visible and routable. May be null for previewing in isolation.
    SpectrumTapComponent(NodeGraph& graph, int nodeId,
                         std::function<void()> onTopologyChanged = {});

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void timerCallback() override { repaint(); }

private:
    NodeGraph& graph;
    int nodeId;
    std::function<void()> onTopologyChanged;
    int dragBinIdx = -1;
    enum DragWhat { DragCenter, DragLeft, DragRight };
    DragWhat dragWhat = DragCenter;

    juce::TextButton addBinBtn{"+ Add Bin"};
    juce::Label      fftSizeLabel { {}, "FFT size:" };
    juce::ComboBox   fftSizeCombo;
    int              fftSize = SpectrumTapProcessor::kDefaultFftSize;

    // Per-bin custom response curve state, kept in lock-step with the
    // node's "Bin " params. binCurveStates[i].first is whether the i-th
    // bin uses a custom response curve, .second is the curve itself
    // (only meaningful when .first is true). Mirrored to node.script
    // via syncCurvesToScript() so the audio processor and persistence
    // layers see the same view.
    std::vector<std::pair<bool, SpectralCurve>> binCurveStates;
    int countBinParams() const;
    void syncCurveStateCount();        // pad/truncate to match bin count
    void loadCurveStatesFromScript();  // read node.script
    void syncCurvesToScript();         // write node.script + dirty + rebuild
    void openResponseEditor(int binSlotIdx);

    // Convert between Hz and screen x (log scale)
    float hzToX(float hz) const;
    float xToHz(float x) const;
    juce::Rectangle<float> getSpectrumArea() const;

    // Reconcile the node's Signal Out pins with its "Bin " params:
    // ensure there is exactly one Signal Out pin per bin, named after
    // the bin's frequency, in the same order. Adds missing pins (for
    // projects saved before per-bin pins existed), removes orphans
    // (bins deleted while the editor was closed), and renames pins
    // whose bin frequency drifted. Returns true if pinsOut changed.
    bool syncBinPins();
};

} // namespace SoundShop

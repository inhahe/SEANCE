#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <map>
#include <vector>

namespace SoundShop {

// ==============================================================================
// Shared per-node audio capture ring buffer for the analyzer nodes
// (Spectrum Analyzer / Oscilloscope / Spectrogram). The processor pushes
// the input audio into the ring each block; the editor component polls a
// snapshot at UI rate to drive its display. One AnalyzerCapture instance
// lives per-node, shared via std::shared_ptr between the processor (which
// fills it on the audio thread) and the editor (which reads on the UI
// thread). Torn reads are acceptable - the worst case is a brief visual
// glitch, never a crash.
// ==============================================================================
struct AnalyzerCapture {
    static constexpr int kBufSize = 4096;
    float bufL[kBufSize] = {};
    float bufR[kBufSize] = {};
    std::atomic<int> writePos{0};
    std::atomic<double> sampleRate{44100.0};

    // Read the most recent `count` samples (oldest-first), or up to kBufSize.
    // Returns the device sample rate.
    double snapshot(std::vector<float>& outL, std::vector<float>& outR, int count) const {
        count = juce::jlimit(1, kBufSize, count);
        outL.resize(count);
        outR.resize(count);
        int wp = writePos.load(std::memory_order_relaxed);
        // The oldest sample sits at (wp - count + kBufSize) % kBufSize.
        int start = ((wp - count) % kBufSize + kBufSize) % kBufSize;
        for (int i = 0; i < count; ++i) {
            int idx = (start + i) % kBufSize;
            outL[i] = bufL[idx];
            outR[i] = bufR[idx];
        }
        return sampleRate.load(std::memory_order_relaxed);
    }
};

// Registry mapping node IDs to their AnalyzerCapture. Both the processor
// and the editor look up the capture by node ID and hold a shared_ptr.
// A capture stays alive until the last shared_ptr is released
// (processor destroyed AND editor closed), so the editor can keep showing
// the last buffer briefly after the user disconnects the cable.
class AnalyzerCaptureRegistry {
public:
    static std::shared_ptr<AnalyzerCapture> getOrCreate(int nodeId);

private:
    static std::mutex mutex;
    // Weak refs - the registry doesn't own the buffers, the processor
    // and editor do. When both let go the entry self-cleans on next
    // getOrCreate.
    static std::map<int, std::weak_ptr<AnalyzerCapture>> table;
};

// ==============================================================================
// AnalyzerProcessor: a single processor that backs all three analyzer
// nodes (Spectrum Analyzer, Oscilloscope, Spectrogram). Audio passes
// through unchanged; the input is copied into the node's AnalyzerCapture
// each block so the editor can read it for display.
// ==============================================================================
class AnalyzerProcessor : public juce::AudioProcessor {
public:
    AnalyzerProcessor(Node& node);

    const juce::String getName() const override { return "Analyzer"; }
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

private:
    Node& node;
    double sampleRate = 44100;
    std::shared_ptr<AnalyzerCapture> capture;
};

// ==============================================================================
// SpectrumAnalyzerComponent: FFT bar display. The dialog window is
// resizable - drag wider to add more frequency bars (more detail across
// the spectrum), drag taller to make the bars taller. Bar count is
// derived from the visible width (~one bar per 4 pixels, clamped to a
// readable range).
// ==============================================================================
class SpectrumAnalyzerComponent : public juce::Component, private juce::Timer {
public:
    SpectrumAnalyzerComponent(NodeGraph& graph, int nodeId);
    void paint(juce::Graphics& g) override;
    void resized() override {}
    void timerCallback() override;

private:
    NodeGraph& graph;
    int nodeId;
    std::shared_ptr<AnalyzerCapture> capture;
    std::vector<float> magnitudes;       // smoothed bar magnitudes
    int currentNumBars() const;          // derived from getWidth()
};

// ==============================================================================
// OscilloscopeComponent: time-domain waveform display.
//
// Two acquisition modes (persisted per-node, see Node::scopeTriggered):
//   - Triggered: the trace is aligned to a level crossing of the chosen slope
//     so a periodic waveform appears stationary (classic oscilloscope sync).
//   - Roll: free-running strip chart - the most recent samples are drawn each
//     frame with the newest at the right edge, no edge alignment.
// A top control strip exposes the mode, the trigger slope (Rising/Falling), and
// the trigger level; the slope/level controls grey out in Roll mode.
// ==============================================================================
class OscilloscopeComponent : public juce::Component, private juce::Timer {
public:
    OscilloscopeComponent(NodeGraph& graph, int nodeId);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override { repaint(); }

private:
    NodeGraph& graph;
    int nodeId;
    std::shared_ptr<AnalyzerCapture> capture;

    // Control strip widgets. Their state mirrors the persisted Node fields;
    // edits push back to the node and commit one undo snapshot.
    juce::ComboBox      modeBox;      // Triggered / Roll
    juce::TextButton    slopeBtn;     // Rising / Falling (triggered only)
    juce::Slider        levelSlider;  // trigger level -1..1 (triggered only)
    juce::Label         levelLabel;   // "Level"
    juce::TooltipWindow tooltips { this };

    Node* findNode() const { return graph.findNode(nodeId); }
    void loadControlsFromNode();      // widget state  <- node fields
    void updateControlEnablement();   // grey slope/level in Roll mode (+ why tooltip)
    void commitScopeSettings();       // node fields <- widgets, then commitSnapshot

    static constexpr int kControlStripH = 30;
};

// ==============================================================================
// SpectrogramComponent: time-frequency waterfall display. New FFT frames
// scroll in from the right; older frames fade out at the left edge.
// ==============================================================================
class SpectrogramComponent : public juce::Component, private juce::Timer {
public:
    SpectrogramComponent(NodeGraph& graph, int nodeId);
    ~SpectrogramComponent() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override;

private:
    NodeGraph& graph;
    int nodeId;
    std::shared_ptr<AnalyzerCapture> capture;
    juce::Image waterfall;              // scrolling image
    int waterfallW = 512;
    int waterfallH = 256;
    // FFT size scales with display height (next power of two >= 2*H,
    // clamped to AnalyzerCapture::kBufSize). The user-visible effect:
    // dragging the window taller increases the number of FFT bins, so
    // each row of the waterfall corresponds to a finer slice of the
    // frequency axis. Updated in resized() and read in timerCallback().
    int currentFftSize = 1024;
    int chooseFftSizeForHeight(int h) const;
};

} // namespace SoundShop

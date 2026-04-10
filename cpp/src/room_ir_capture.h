#pragma once
#include "node_graph.h"
#include "fft_util.h"
#include "convolution_processor.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <complex>
#include <atomic>
#include <functional>

namespace SoundShop {

// Generates test signals for IR capture
class IRCaptureSignal {
public:
    // Generate a single click (impulse)
    static std::vector<float> generateClick(double sampleRate, float durationSec = 0.001f);

    // Generate a logarithmic sine sweep (20 Hz → 20 kHz)
    static std::vector<float> generateSweep(double sampleRate, float durationSec = 3.0f);

    // Generate the inverse sweep filter for deconvolution
    static std::vector<float> generateInverseSweep(double sampleRate, float durationSec = 3.0f);

    // Deconvolve a recorded sweep response to extract the IR
    // result = IFFT(FFT(recording) * FFT(inverseSweep))
    static std::vector<float> deconvolve(const std::vector<float>& recording,
                                          const std::vector<float>& inverseSweep,
                                          double sampleRate);
};

// Handles the actual audio I/O for capture: plays a signal through the
// output device while simultaneously recording from the input device.
class IRCaptureSession : public juce::AudioIODeviceCallback {
public:
    IRCaptureSession();

    void startCapture(juce::AudioDeviceManager& deviceManager,
                      const std::vector<float>& playbackSignal,
                      float recordDurationSec,
                      double sampleRate);
    void stopCapture();

    bool isCapturing() const { return capturing.load(); }
    bool isComplete() const { return complete.load(); }
    const std::vector<float>& getRecording() const { return recording; }

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData, int numInputChannels,
        float* const* outputChannelData, int numOutputChannels,
        int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

private:
    std::vector<float> playbackSignal;
    std::vector<float> recording;
    int64_t playPos = 0;
    int64_t recordPos = 0;
    int64_t maxRecordSamples = 0;
    std::atomic<bool> capturing{false};
    std::atomic<bool> complete{false};
};

// Dialog UI for room IR capture
class RoomIRCaptureComponent : public juce::Component, private juce::Timer {
public:
    RoomIRCaptureComponent(NodeGraph& graph, juce::AudioDeviceManager& deviceManager,
                            std::function<void()> onComplete);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override;

private:
    NodeGraph& graph;
    juce::AudioDeviceManager& deviceManager;
    std::function<void()> onComplete;

    juce::ComboBox methodCombo;
    juce::Label methodLabel;
    juce::Slider sweepDurationSlider;
    juce::Label sweepDurLabel;
    juce::Slider recordDurationSlider;
    juce::Label recordDurLabel;
    juce::TextButton captureBtn{"Start Capture"};
    juce::TextButton closeBtn{"Close"};
    juce::Label statusLabel;
    juce::Label instructionLabel;

    IRCaptureSession session;
    std::vector<float> capturedIR;
    bool hasResult = false;

    // For deconvolution after sweep capture
    std::vector<float> inverseSweep;
    double captureSampleRate = 48000;

    void startCapture();
    void processResult();
    void createConvolutionNode();
};

} // namespace SoundShop

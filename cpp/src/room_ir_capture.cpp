#define _USE_MATH_DEFINES
#include "room_ir_capture.h"
#include <cmath>
#include <algorithm>
#include <complex>

namespace SoundShop {

// ==============================================================================
// Signal generation
// ==============================================================================

std::vector<float> IRCaptureSignal::generateClick(double sampleRate, float durationSec) {
    int len = (int)(durationSec * sampleRate);
    std::vector<float> signal(len, 0.0f);
    // Short impulse: a few samples at full amplitude
    int clickSamples = std::max(1, (int)(0.0005 * sampleRate)); // 0.5ms
    for (int i = 0; i < clickSamples && i < len; ++i)
        signal[i] = 0.9f;
    return signal;
}

std::vector<float> IRCaptureSignal::generateSweep(double sampleRate, float durationSec) {
    int len = (int)(durationSec * sampleRate);
    std::vector<float> signal(len);
    double f1 = 20.0, f2 = 20000.0;
    double lnRatio = std::log(f2 / f1);

    for (int i = 0; i < len; ++i) {
        double t = (double)i / sampleRate;
        double T = durationSec;
        // Logarithmic sine sweep: f(t) = f1 * exp(t/T * ln(f2/f1))
        // phase(t) = 2*pi * f1 * T / ln(f2/f1) * (exp(t/T * ln(f2/f1)) - 1)
        double phase = 2.0 * M_PI * f1 * T / lnRatio * (std::exp(t / T * lnRatio) - 1.0);
        signal[i] = (float)(0.8 * std::sin(phase));

        // Fade in/out to avoid clicks
        double fadeLen = 0.01; // 10ms
        if (t < fadeLen) signal[i] *= (float)(t / fadeLen);
        if (t > T - fadeLen) signal[i] *= (float)((T - t) / fadeLen);
    }
    return signal;
}

std::vector<float> IRCaptureSignal::generateInverseSweep(double sampleRate, float durationSec) {
    // The inverse filter for a log sweep is the time-reversed sweep with
    // amplitude envelope compensation: amplitude decays at 6dB/octave
    // (because log sweep spends more time at low frequencies).
    auto sweep = generateSweep(sampleRate, durationSec);
    int len = (int)sweep.size();
    std::vector<float> inverse(len);

    double f1 = 20.0, f2 = 20000.0;
    double lnRatio = std::log(f2 / f1);

    for (int i = 0; i < len; ++i) {
        // Time-reverse
        inverse[i] = sweep[len - 1 - i];
        // Apply amplitude envelope: 6dB/octave decay = exponential in time
        double t = (double)(len - 1 - i) / sampleRate;
        double T = durationSec;
        double envelope = std::exp(-t / T * lnRatio);
        inverse[i] *= (float)envelope;
    }

    // Normalize
    float peak = 0;
    for (float s : inverse) peak = std::max(peak, std::abs(s));
    if (peak > 1e-6f)
        for (float& s : inverse) s /= (peak * len * 0.5f);

    return inverse;
}

std::vector<float> IRCaptureSignal::deconvolve(const std::vector<float>& recording,
                                                const std::vector<float>& inverseSweep,
                                                double sampleRate) {
    // Convolution of the recording with the inverse sweep = the IR
    return ConvolutionProcessor::convolveIRs(recording, inverseSweep);
}

// ==============================================================================
// Capture session (audio I/O callback)
// ==============================================================================

IRCaptureSession::IRCaptureSession() {}

void IRCaptureSession::startCapture(juce::AudioDeviceManager& dm,
                                     const std::vector<float>& signal,
                                     float recordDurationSec,
                                     double sampleRate) {
    playbackSignal = signal;
    maxRecordSamples = (int64_t)(recordDurationSec * sampleRate);
    recording.clear();
    recording.reserve(maxRecordSamples);
    playPos = 0;
    recordPos = 0;
    complete = false;
    capturing = true;

    // Temporarily take over the audio callback
    dm.addAudioCallback(this);
}

void IRCaptureSession::stopCapture() {
    capturing = false;
}

void IRCaptureSession::audioDeviceIOCallbackWithContext(
    const float* const* inputChannelData, int numInputChannels,
    float* const* outputChannelData, int numOutputChannels,
    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    if (!capturing.load()) {
        // Silence output
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch])
                std::memset(outputChannelData[ch], 0, sizeof(float) * numSamples);
        return;
    }

    for (int s = 0; s < numSamples; ++s) {
        // Play the test signal
        float out = 0.0f;
        if (playPos < (int64_t)playbackSignal.size())
            out = playbackSignal[playPos];
        playPos++;

        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch])
                outputChannelData[ch][s] = out;

        // Record from input
        float in = 0.0f;
        if (numInputChannels > 0 && inputChannelData[0])
            in = inputChannelData[0][s];
        if (recordPos < maxRecordSamples)
            recording.push_back(in);
        recordPos++;

        // Done when we've recorded enough
        if (recordPos >= maxRecordSamples) {
            capturing = false;
            complete = true;
            break;
        }
    }
}

// ==============================================================================
// Dialog UI
// ==============================================================================

RoomIRCaptureComponent::RoomIRCaptureComponent(NodeGraph& g,
                                                juce::AudioDeviceManager& dm,
                                                std::function<void()> onComp)
    : graph(g), deviceManager(dm), onComplete(std::move(onComp))
{
    addAndMakeVisible(instructionLabel);
    instructionLabel.setText(
        "Capture a room's impulse response by playing a test signal through your speakers\n"
        "and recording the room's response with your microphone.\n\n"
        "1. Place your microphone where the listener would be\n"
        "2. Choose a method (Sine Sweep is more accurate, Click is faster)\n"
        "3. Click 'Start Capture' — the test signal will play, then the room response is recorded\n"
        "4. The captured IR is automatically loaded into a new Convolution Filter node",
        juce::dontSendNotification);
    instructionLabel.setFont(11.0f);
    instructionLabel.setJustificationType(juce::Justification::topLeft);

    addAndMakeVisible(methodCombo); addAndMakeVisible(methodLabel);
    methodLabel.setText("Method:", juce::dontSendNotification);
    methodLabel.setFont(11.0f);
    methodCombo.addItem("Sine Sweep (recommended)", 1);
    methodCombo.addItem("Click / Impulse (faster)", 2);
    methodCombo.setSelectedId(1);
    methodCombo.setTooltip("Sine Sweep slowly sweeps through every audible frequency for cleaner results "
                            "but is louder and slower. Click/Impulse plays a sharp pop and records the response — "
                            "faster but more affected by background noise.");

    addAndMakeVisible(sweepDurationSlider); addAndMakeVisible(sweepDurLabel);
    sweepDurLabel.setText("Sweep length:", juce::dontSendNotification);
    sweepDurLabel.setFont(11.0f);
    sweepDurationSlider.setRange(1.0, 10.0, 0.5);
    sweepDurationSlider.setValue(3.0);
    sweepDurationSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 20);
    sweepDurationSlider.setTextValueSuffix("s");
    sweepDurationSlider.setTooltip("How long the sine sweep test signal plays. Longer sweeps give cleaner "
                                    "results in noisy environments at the cost of capture time.");

    addAndMakeVisible(recordDurationSlider); addAndMakeVisible(recordDurLabel);
    recordDurLabel.setText("Record length:", juce::dontSendNotification);
    recordDurLabel.setFont(11.0f);
    recordDurationSlider.setRange(1.0, 15.0, 0.5);
    recordDurationSlider.setValue(5.0);
    recordDurationSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 20);
    recordDurationSlider.setTextValueSuffix("s");
    recordDurationSlider.setTooltip("Total recording time. Make this longer than the sweep so the room's "
                                     "tail (reverb decay) gets fully captured.");

    addAndMakeVisible(captureBtn);
    captureBtn.setTooltip("Begin the capture: play the test signal through your speakers, record the room's response "
                          "with your microphone, and create a Convolution node loaded with the result");
    captureBtn.onClick = [this]() { startCapture(); };

    addAndMakeVisible(closeBtn);
    closeBtn.onClick = [this]() {
        session.stopCapture();
        deviceManager.removeAudioCallback(&session);
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
            juce::Component::SafePointer<juce::DialogWindow> safe(dw);
            juce::MessageManager::callAsync([safe]() {
                if (safe) delete safe.getComponent();
            });
        }
    };

    addAndMakeVisible(statusLabel);
    statusLabel.setFont(juce::Font(12.0f, juce::Font::bold));
    statusLabel.setColour(juce::Label::textColourId, juce::Colours::limegreen);

    setSize(500, 380);
}

void RoomIRCaptureComponent::startCapture() {
    auto* device = deviceManager.getCurrentAudioDevice();
    if (!device) {
        statusLabel.setText("Error: No audio device available", juce::dontSendNotification);
        return;
    }

    captureSampleRate = device->getCurrentSampleRate();
    float sweepDur = (float)sweepDurationSlider.getValue();
    float recordDur = (float)recordDurationSlider.getValue();

    std::vector<float> signal;
    if (methodCombo.getSelectedId() == 1) {
        // Sine sweep
        signal = IRCaptureSignal::generateSweep(captureSampleRate, sweepDur);
        inverseSweep = IRCaptureSignal::generateInverseSweep(captureSampleRate, sweepDur);
        // Record for sweep duration + extra tail for room reverb
        recordDur = sweepDur + recordDur;
    } else {
        // Click
        signal = IRCaptureSignal::generateClick(captureSampleRate);
        inverseSweep.clear(); // no deconvolution needed for click
    }

    statusLabel.setText("Capturing... (playing test signal + recording room response)",
                        juce::dontSendNotification);
    captureBtn.setEnabled(false);

    // Need input enabled on the device
    auto deviceSetup = deviceManager.getAudioDeviceSetup();
    if (deviceSetup.inputChannels.isZero()) {
        // Enable first input channel
        deviceSetup.inputChannels.setBit(0);
        deviceManager.setAudioDeviceSetup(deviceSetup, true);
    }

    session.startCapture(deviceManager, signal, recordDur, captureSampleRate);
    startTimerHz(10); // poll for completion
}

void RoomIRCaptureComponent::timerCallback() {
    if (session.isComplete()) {
        stopTimer();
        deviceManager.removeAudioCallback(&session);
        statusLabel.setText("Processing captured audio...", juce::dontSendNotification);

        // Process on a slight delay so the UI updates
        juce::MessageManager::callAsync([this]() { processResult(); });
    }
}

void RoomIRCaptureComponent::processResult() {
    auto& recording = session.getRecording();
    if (recording.empty()) {
        statusLabel.setText("Error: No audio was recorded. Check your microphone.",
                            juce::dontSendNotification);
        captureBtn.setEnabled(true);
        return;
    }

    if (!inverseSweep.empty()) {
        // Sine sweep method: deconvolve to extract the pure IR
        capturedIR = IRCaptureSignal::deconvolve(recording, inverseSweep, captureSampleRate);
    } else {
        // Click method: the recording IS the IR (just trim leading silence)
        capturedIR = recording;
        // Find the first sample above a threshold (the click arrival)
        int start = 0;
        for (int i = 0; i < (int)capturedIR.size(); ++i) {
            if (std::abs(capturedIR[i]) > 0.01f) { start = std::max(0, i - 10); break; }
        }
        if (start > 0)
            capturedIR.erase(capturedIR.begin(), capturedIR.begin() + start);
    }

    // Normalize the IR to peak 1.0
    float peak = 0;
    for (float s : capturedIR) peak = std::max(peak, std::abs(s));
    if (peak > 1e-6f)
        for (float& s : capturedIR) s /= peak;

    // Trim trailing near-silence (below -60dB)
    float threshold = 0.001f;
    int end = (int)capturedIR.size();
    while (end > 1 && std::abs(capturedIR[end - 1]) < threshold) --end;
    capturedIR.resize(end);

    hasResult = true;
    createConvolutionNode();

    float durMs = (float)capturedIR.size() / (float)captureSampleRate * 1000.0f;
    statusLabel.setText("Done! Captured IR: " + juce::String((int)capturedIR.size())
                        + " samples (" + juce::String(durMs, 0) + " ms). "
                        + "A new Convolution Filter node has been created.",
                        juce::dontSendNotification);
    captureBtn.setEnabled(true);
    repaint();
}

void RoomIRCaptureComponent::createConvolutionNode() {
    // Create a Convolution Filter node pre-loaded with the captured IR
    auto& n = graph.addNode("Room IR", NodeType::Effect,
        {Pin{0, "Audio In", PinKind::Audio, true}},
        {Pin{0, "Audio Out", PinKind::Audio, false}},
        {200, 200});
    n.script = ConvolutionProcessor::encodeIR(capturedIR);
    graph.dirty = true;
    if (onComplete) onComplete();
}

void RoomIRCaptureComponent::resized() {
    auto a = getLocalBounds().reduced(10);

    instructionLabel.setBounds(a.removeFromTop(100));
    a.removeFromTop(8);

    auto row1 = a.removeFromTop(26);
    methodLabel.setBounds(row1.removeFromLeft(55));
    methodCombo.setBounds(row1.removeFromLeft(220).reduced(0, 2));

    auto row2 = a.removeFromTop(26);
    sweepDurLabel.setBounds(row2.removeFromLeft(85));
    sweepDurationSlider.setBounds(row2.removeFromLeft(180).reduced(0, 2));

    auto row3 = a.removeFromTop(26);
    recordDurLabel.setBounds(row3.removeFromLeft(85));
    recordDurationSlider.setBounds(row3.removeFromLeft(180).reduced(0, 2));

    a.removeFromTop(8);
    auto row4 = a.removeFromTop(28);
    captureBtn.setBounds(row4.removeFromLeft(120).reduced(0, 2));
    row4.removeFromLeft(8);
    closeBtn.setBounds(row4.removeFromLeft(60).reduced(0, 2));

    a.removeFromTop(8);
    statusLabel.setBounds(a.removeFromTop(40));

    // Remaining = waveform preview of captured IR (if available)
}

void RoomIRCaptureComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(22, 22, 28));

    // Show captured IR waveform if available
    if (hasResult && !capturedIR.empty()) {
        auto a = getLocalBounds().reduced(10);
        a.removeFromTop(280); // skip controls
        auto area = a.toFloat();
        if (area.getHeight() > 20) {
            g.setColour(juce::Colour(18, 20, 28));
            g.fillRoundedRectangle(area, 4.0f);
            g.setColour(juce::Colour(50, 55, 70));
            g.drawRoundedRectangle(area, 4.0f, 1.0f);

            float cy = area.getCentreY();
            g.setColour(juce::Colours::grey.withAlpha(0.3f));
            g.drawHorizontalLine((int)cy, area.getX(), area.getRight());

            g.setColour(juce::Colours::cornflowerblue);
            juce::Path p;
            int n = (int)capturedIR.size();
            int step = std::max(1, n / (int)area.getWidth());
            for (int i = 0; i < n; i += step) {
                float x = area.getX() + (float)i / n * area.getWidth();
                float y = cy - capturedIR[i] * area.getHeight() * 0.45f;
                if (i == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
            }
            g.strokePath(p, juce::PathStrokeType(1.0f));

            g.setColour(juce::Colours::grey);
            g.setFont(9.0f);
            g.drawText("Captured Impulse Response", area.reduced(4, 2).toNearestInt(),
                       juce::Justification::topLeft);
        }
    }
}

} // namespace SoundShop

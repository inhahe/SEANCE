#pragma once
#include "audio_engine.h"
#include "fft_util.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <cmath>

namespace SoundShop {

// ==============================================================================
// Spectrum visualizer pane (#10): real-time FFT display of the master
// output. Shows frequency content as colored bars (log frequency axis,
// dB amplitude) updated at ~20 Hz from the audio engine's ring buffer.
// ==============================================================================
class SpectrumVisualizerComponent : public juce::Component, public juce::Timer {
public:
    SpectrumVisualizerComponent(AudioEngine& ae) : engine(ae) {
        startTimerHz(20);
        setSize(600, 200);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(18, 20, 28));

        auto area = getLocalBounds().toFloat().reduced(4);
        int numBars = (int)magnitudes.size();
        if (numBars == 0) return;

        float barW = area.getWidth() / (float)numBars;
        float maxDb = 0.0f, minDb = -80.0f;

        for (int i = 0; i < numBars; ++i) {
            float db = 20.0f * std::log10(std::max(1e-6f, magnitudes[i]));
            float frac = juce::jlimit(0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
            float barH = frac * area.getHeight();

            // Color: blue -> cyan -> green -> yellow -> red based on magnitude.
            auto col = juce::Colour::fromHSV(0.6f - frac * 0.6f, 0.8f, 0.5f + frac * 0.5f, 1.0f);
            g.setColour(col);
            g.fillRect(area.getX() + i * barW, area.getBottom() - barH, barW - 1, barH);
        }

        // Frequency labels.
        g.setColour(juce::Colours::grey);
        g.setFont(9.0f);
        float nyquist = (float)(engine.getSampleRate() * 0.5);
        float freqs[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
        for (float hz : freqs) {
            if (hz > nyquist) break;
            float logFrac = std::log(hz / 20.0f) / std::log(nyquist / 20.0f);
            float x = area.getX() + logFrac * area.getWidth();
            g.drawVerticalLine((int)x, area.getY(), area.getBottom());
            juce::String label = (hz >= 1000) ? juce::String(hz / 1000, 0) + "k" : juce::String((int)hz);
            g.drawText(label, (int)x - 12, (int)area.getBottom() - 12, 24, 12, juce::Justification::centred);
        }
    }

    void timerCallback() override {
        // Grab the latest audio snapshot and compute FFT magnitudes.
        std::vector<float> snapshot;
        engine.getSpectrumSnapshot(snapshot, 0);

        int n = (int)snapshot.size();
        if (n == 0) return;

        // Apply Hann window.
        for (int i = 0; i < n; ++i)
            snapshot[i] *= 0.5f * (1.0f - std::cos(6.28318f * i / n));

        FFT fft(n);
        std::vector<std::complex<float>> spectrum;
        fft.forwardReal(snapshot, spectrum);

        int numBins = n / 2;
        // Map to log-frequency bars (64 bars covering 20 Hz to Nyquist).
        int numBars = 64;
        magnitudes.resize(numBars, 0.0f);
        float nyquist = (float)(engine.getSampleRate() * 0.5);
        float logMin = std::log(20.0f);
        float logMax = std::log(nyquist);

        for (int bar = 0; bar < numBars; ++bar) {
            float logLo = logMin + (logMax - logMin) * bar / numBars;
            float logHi = logMin + (logMax - logMin) * (bar + 1) / numBars;
            float hzLo = std::exp(logLo);
            float hzHi = std::exp(logHi);
            int binLo = (int)(hzLo / nyquist * numBins);
            int binHi = (int)(hzHi / nyquist * numBins);
            binLo = juce::jlimit(0, numBins - 1, binLo);
            binHi = juce::jlimit(binLo, numBins - 1, binHi);

            float maxMag = 0;
            for (int b = binLo; b <= binHi; ++b)
                maxMag = std::max(maxMag, std::abs(spectrum[b]));
            // Smooth decay.
            magnitudes[bar] = magnitudes[bar] * 0.7f + maxMag * 0.3f;
        }

        repaint();
    }

private:
    AudioEngine& engine;
    std::vector<float> magnitudes;
};

} // namespace SoundShop

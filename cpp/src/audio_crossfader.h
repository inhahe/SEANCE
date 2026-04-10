#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>

namespace SoundShop {

// Smooths instantaneous gain changes (mute, solo, plugin bypass, future
// child-track entry/exit, etc.) into a short ramp to prevent clicks.
//
// Use one instance per "gain decision" you want to smooth. Each block:
//   1. Call setCrossfadeDuration(graph.globalCrossfadeSec) so the user's
//      preferred ramp length is honored.
//   2. Call setTarget(0.0f or 1.0f) for the desired state.
//   3. Call process(buf) to apply the in-place ramp toward target.
//
// The ramp is linear in amplitude. Linear is fine for short (<100ms) fades
// because the ear doesn't perceive non-linearity at that timescale; longer
// fades would benefit from an equal-power curve but the use case here is
// click suppression, not musical fades.
class AudioCrossfader {
public:
    void prepare(double sr, float crossfadeSec) {
        sampleRate = sr;
        setCrossfadeDuration(crossfadeSec);
    }

    void setCrossfadeDuration(float crossfadeSec) {
        // Per-sample step that takes us 0..1 over `crossfadeSec` seconds.
        // Tiny floor avoids div-by-zero and lets a value of 0 effectively
        // mean "snap instantly" (one-block ramp).
        float secs = std::max(0.0001f, crossfadeSec);
        ratePerSample = (float)(1.0 / (secs * sampleRate));
    }

    void setTarget(float t) { target = juce::jlimit(0.0f, 1.0f, t); }

    // Snap immediately to a value, skipping the ramp. Use this on
    // initialization or after a graph rebuild so we don't fade in from 0
    // when nothing changed.
    void snapTo(float v) {
        v = juce::jlimit(0.0f, 1.0f, v);
        current = v;
        target = v;
    }

    // Apply the in-place ramp toward target. Fast paths for fully on / off.
    void process(juce::AudioBuffer<float>& buf) {
        if (current == target) {
            if (current >= 1.0f) return;       // pass-through
            if (current <= 0.0f) { buf.clear(); return; } // silence
        }
        int n = buf.getNumSamples();
        int ch = buf.getNumChannels();
        for (int i = 0; i < n; ++i) {
            if (current < target)      current = std::min(target, current + ratePerSample);
            else if (current > target) current = std::max(target, current - ratePerSample);
            for (int c = 0; c < ch; ++c)
                buf.getWritePointer(c)[i] *= current;
        }
    }

    float getCurrent() const { return current; }
    float getTarget()  const { return target; }
    bool isFullyOff() const { return current <= 0.0f && target <= 0.0f; }
    bool isFullyOn()  const { return current >= 1.0f && target >= 1.0f; }
    bool isRamping()  const { return current != target; }

private:
    double sampleRate = 44100.0;
    float ratePerSample = 1.0f / (0.05f * 44100.0f);
    float current = 1.0f;
    float target  = 1.0f;
};

} // namespace SoundShop

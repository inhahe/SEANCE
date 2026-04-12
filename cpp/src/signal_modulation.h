#pragma once
#include "node_graph.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>

namespace SoundShop {

// Apply incoming Signal-cable modulations to the node's params at the
// start of a processBlock. For each ModPin on the node, reads the signal
// value from the corresponding audio channel (2 + control-slot index)
// and writes the modulated result into node.params[].value, keeping
// baseValue as the unmodulated reference.
//
// Call this at the TOP of every processor's processBlock. It's safe to
// call on nodes with no modPins — the function returns immediately.
//
// Modulation model: bipolar additive.
//   modulated = baseValue + signal * depth * (maxVal - minVal) / 2
// A signal of +1 with depth 1 pushes the param to the top of its range;
// a signal of -1 pushes to the bottom. The result is clamped to
// [minVal, maxVal]. The `depth` per modPin defaults to 1.0.
//
// Resolution: block-rate (reads channel at sample 0). For per-sample
// modulation of time-critical params like filter cutoff, the processor
// can read the channel directly inside its sample loop — this helper
// is just the convenient default.

inline void applySignalModulations(Node& node,
                                    const juce::AudioBuffer<float>& buf) {
    if (node.modPins.empty()) return;

    // Build a quick map: for each Signal/Param pin in pinsIn, which
    // control-slot index is it? (matches graph_processor.cpp's routing
    // logic that puts control signals on channels 2, 3, 4, ...)
    // We only need to iterate once — most nodes have 0-3 modPins.
    for (auto& mp : node.modPins) {
        if (mp.paramIndex < 0 || mp.paramIndex >= (int)node.params.size())
            continue;

        // Find the control-slot index for this pin — count Signal/Param
        // pins in pinsIn that come before the one with id == mp.pinId.
        int slotIdx = -1;
        int sigCount = 0;
        for (auto& pin : node.pinsIn) {
            if (pin.id == mp.pinId) { slotIdx = sigCount; break; }
            if (pin.kind == PinKind::Signal || pin.kind == PinKind::Param)
                ++sigCount;
        }
        if (slotIdx < 0) continue;

        int chIdx = 2 + slotIdx;
        if (chIdx >= buf.getNumChannels()) continue;

        // Read the signal value at sample 0 (block-rate modulation).
        float sigVal = buf.getSample(chIdx, 0);

        auto& p = node.params[mp.paramIndex];
        // On the first modulated block, snapshot the current value as
        // baseValue. On subsequent blocks, baseValue is already stable.
        if (!p.modulated) {
            p.baseValue = p.value;
            p.modulated = true;
        }

        float range = p.maxVal - p.minVal;
        float modVal = p.baseValue + sigVal * mp.depth * range * 0.5f;
        p.value = std::clamp(modVal, p.minVal, p.maxVal);
    }
}

// Reset modulation flags on all params. Called when a node's modPins
// change (pin removed, cable disconnected) so values return to their
// base state.
inline void clearSignalModulations(Node& node) {
    for (auto& p : node.params) {
        if (p.modulated) {
            p.value = p.baseValue;
            p.modulated = false;
        }
    }
}

} // namespace SoundShop

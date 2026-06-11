#pragma once
#include "node_graph.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>

namespace SoundShop {

// ---------------------------------------------------------------------------
// Control-signal range convention (the ONE place this is documented in code).
//
// Every control signal carried on a Signal/Param pin (or a control channel,
// 2+, of an audio buffer) is UNIPOLAR 0..1. 0.5 is the neutral "no change"
// resting value. This is deliberately uniform across ALL signal producers
// (Signal Shape, Control Bank, XY Pad, MIDI Mod, scripts) so a consumer never
// has to know who fed it. AUDIO (channels 0/1) is still bipolar -1..1; this
// convention is only about CONTROL signals.
//
// A consumer that wants a bipolar swing (e.g. additive modulation around a
// base value) converts at the read site with toBipolar(); a producer that
// computed a bipolar value (a raw sin(), say) maps it onto the wire with
// toUnipolar(). The two cancel for an LFO, so LFO behaviour is identical to
// the old all-bipolar world while fixing producers (like a Control Bank fader
// at 0) that used to emit 0 and get misread as "-1 / param minimum".
inline float toBipolar(float x)  { return x * 2.0f - 1.0f; }   // 0..1  -> -1..1
inline float toUnipolar(float x) { return x * 0.5f + 0.5f; }   // -1..1 -> 0..1

// Apply incoming Signal-cable modulations to the node's params at the
// start of a processBlock. For each ModPin on the node, reads the signal
// value from the corresponding audio channel (2 + control-slot index)
// and writes the modulated result into node.params[].value, keeping
// baseValue as the unmodulated reference.
//
// Call this at the TOP of every processor's processBlock. It's safe to
// call on nodes with no modPins - the function returns immediately.
//
// Modulation model: bipolar additive around the base value.
//   modulated = baseValue + toBipolar(signal) * depth * (maxVal - minVal) / 2
// The incoming signal is the unipolar 0..1 wire value; toBipolar() maps it to
// -1..1 so that a signal of 1.0 pushes the param to the top of its range, 0.0
// to the bottom, and 0.5 (the neutral resting value) leaves it unchanged. The
// result is clamped to [minVal, maxVal]. The `depth` per modPin defaults 1.0.
//
// Resolution: block-rate (reads channel at sample 0). For per-sample
// modulation of time-critical params like filter cutoff, the processor
// can read the channel directly inside its sample loop - this helper
// is just the convenient default.

inline void applySignalModulations(Node& node,
                                    const juce::AudioBuffer<float>& buf) {
    if (node.modPins.empty()) return;

    // === TEMP MODPIN DIAGNOSTIC (throwaway) ===
    // Only for a node that owns a "Position" param (the wavetable synth), and
    // only every Nth call so we don't hammer the disk. Dumps, per modPin: pin
    // name, connected flag, Set/Mod mode, computed chIdx, the raw sample[0] on
    // THAT channel, plus every control channel's sample[0] so we can see if the
    // LFO signal is landing on a different channel than the apply side reads.
    {
        bool hasPos = false;
        for (const auto& p : node.params)
            if (p.name.rfind("Position", 0) == 0) { hasPos = true; break; }
        static int sCallCount = 0;
        if (hasPos && ((sCallCount++ % 64) == 0)) {
            juce::String line;
            line << "nCh=" << buf.getNumChannels() << "  chans[";
            for (int c = 2; c < buf.getNumChannels(); ++c)
                line << "ch" << c << "=" << juce::String(buf.getSample(c, 0), 3) << " ";
            line << "]  modPins{";
            for (const auto& mp : node.modPins) {
                std::string pinName = "?";
                for (const auto& pin : node.pinsIn)
                    if (pin.id == mp.pinId) { pinName = pin.name; break; }
                int slot = -1, sc = 0;
                for (const auto& pin : node.pinsIn) {
                    if (pin.id == mp.pinId) { slot = sc; break; }
                    if (pin.kind == PinKind::Signal || pin.kind == PinKind::Param) ++sc;
                }
                int ch = 2 + slot;
                float sv = (ch >= 0 && ch < buf.getNumChannels()) ? buf.getSample(ch, 0) : -999.0f;
                line << "[" << pinName.c_str()
                     << " conn=" << (mp.connected ? 1 : 0)
                     << " mode=" << (mp.mode == Node::ModPin::Mode::Absolute ? "Set" : "Mod")
                     << " ch=" << ch << " sig=" << juce::String(sv, 3)
                     << " pIdx=" << mp.paramIndex << "] ";
            }
            line << "}\n";
            juce::File("D:/temp/modpin_diag.txt").appendText(line);
        }
    }
    // === END TEMP DIAGNOSTIC ===

    // Build a quick map: for each Signal/Param pin in pinsIn, which
    // control-slot index is it? (matches graph_processor.cpp's routing
    // logic that puts control signals on channels 2, 3, 4, ...)
    // We only need to iterate once - most nodes have 0-3 modPins.
    for (auto& mp : node.modPins) {
        if (mp.paramIndex < 0 || mp.paramIndex >= (int)node.params.size())
            continue;

        // Skip bindings with no cable actually feeding the pin. Pins like the
        // wavetable "Mod: Position" inputs are created eagerly (so the user has
        // somewhere to plug a cable in), and their modPin binding exists even
        // when idle. Without this guard we'd read the pin's silent control
        // channel as a genuine 0.0 modulation and force the bound param to its
        // minimum every block - which manifested as the Position sliders doing
        // nothing (the param was pinned to 0 regardless of the slider). The
        // `connected` flag is recomputed from graph.links at every graph
        // rebuild (GraphProcessor::buildGraph), so it tracks connect/disconnect.
        if (!mp.connected) {
            // A previously-connected binding that just lost its cable must
            // release the param back to the user's manual resting value.
            auto& p = node.params[mp.paramIndex];
            if (p.modulated) { p.value = p.baseValue; p.modulated = false; }
            continue;
        }

        // Find the control-slot index for this pin - count Signal/Param
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
        // baseValue (the user's manual resting setting). On subsequent blocks,
        // baseValue is already stable. This applies to BOTH modes: in Absolute
        // mode baseValue preserves the manual value so disconnecting the cable
        // (clearSignalModulations) restores it.
        if (!p.modulated) {
            p.baseValue = p.value;
            p.modulated = true;
        }

        float range = p.maxVal - p.minVal;
        float modVal;
        if (mp.mode == Node::ModPin::Mode::Absolute) {
            // "Set": the cable's value IS the param value, mapped edge-to-edge
            // across [min,max]. The knob's baseValue is ignored (preserved only
            // for restore-on-disconnect). 0 -> min, 1 -> max. depth is unused.
            modVal = p.minVal + sigVal * range;
        } else {
            // "Mod": bipolar-additive around baseValue. 0.5 -> no change.
            modVal = p.baseValue + toBipolar(sigVal) * mp.depth * range * 0.5f;
        }
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

// ==============================================================================
// MPE voice-level expression (#78)
//
// For synths with a Voice struct that has `note`, `active`, and float
// fields for pitch bend / pressure / timbre, this helper scans a MIDI
// buffer for per-channel messages and distributes them to the voice
// whose MIDI channel matches. In non-MPE mode (channel 1 only), the
// messages apply to ALL active voices.
//
// Each synth can call this at the top of processBlock after handling
// note-on/off, then read v.mpe.pitchBend / v.mpe.pressure /
// v.mpe.polyPressure / v.mpe.timbre inside its per-sample loop (or use
// effectivePressure(v.mpe) for the combined channel+poly pressure).
// ==============================================================================

struct MpeVoiceState {
    float pitchBend    = 0.0f;   // semitones, +/- pitchBendRange
    float pressure     = 0.0f;   // 0..1 (channel aftertouch - one value per channel)
    float polyPressure = 0.0f;   // 0..1 (polyphonic key pressure - per note)
    float timbre       = 0.5f;   // 0..1 (CC74)
};

// Combined pressure a voice should respond to: channel aftertouch (applies to
// every note on the channel) plus polyphonic key pressure (this note only),
// clamped to 0..1. A controller might send either, both, or neither; both
// fields default to 0 so this is a no-op until pressure actually arrives.
// Use this as the single "how hard is this note being pressed right now" value
// for amplitude / brightness swells.
inline float effectivePressure(const MpeVoiceState& m) {
    return std::clamp(m.pressure + m.polyPressure, 0.0f, 1.0f);
}

// Distribute MPE messages from `midi` into a voice array. Each voice
// must have: bool active, int note, int mpeChannel, MpeVoiceState mpe.
// `pitchBendRange` is the per-note PB range in semitones (48 default).
template<typename VoiceArray>
inline void distributeMpeMessages(const juce::MidiBuffer& midi,
                                   VoiceArray& voices,
                                   float pitchBendRange = 48.0f) {
    for (auto meta : midi) {
        auto msg = meta.getMessage();
        int ch = msg.getChannel();

        if (msg.isPitchWheel()) {
            float pbNorm = (msg.getPitchWheelValue() - 8192) / 8192.0f;
            // The pitch-bend range depends on which channel the wheel arrives
            // on. A *member* channel (MPE, 2..16) carries one note, so it uses
            // the wide per-note range (default 48 semis). Channel 1 is either a
            // non-MPE controller (every note on ch 1) or the MPE *master*
            // channel - in both cases the wheel is a global bend that should
            // use the standard GM range (+/-2 semis), matching TerrainSynth's
            // kPitchBendRangeSemis. Using the wide range here would slam a
            // normal pitch wheel +/-48 semitones (4 octaves).
            constexpr float kLegacyPitchBendSemis = 2.0f;
            for (auto& v : voices) {
                if (!v.active) continue;
                if (ch != 1 && v.mpeChannel == ch)
                    v.mpe.pitchBend = pbNorm * pitchBendRange;
                else if (ch == 1)
                    v.mpe.pitchBend = pbNorm * kLegacyPitchBendSemis;
            }
        } else if (msg.isChannelPressure()) {
            float p = msg.getChannelPressureValue() / 127.0f;
            for (auto& v : voices) {
                if (!v.active) continue;
                if (v.mpeChannel == ch || ch == 1)
                    v.mpe.pressure = p;
            }
        } else if (msg.isAftertouch()) {
            // Polyphonic key pressure: carries a note number, so it's routed
            // only to the voice(s) currently holding that note (on the
            // matching channel, or any channel in non-MPE mode where
            // everything arrives on channel 1). This is how per-note pressure
            // reaches the right voice without ever touching a mono signal
            // cable - the note number is the disambiguator a single signal
            // can't carry.
            float p = msg.getAfterTouchValue() / 127.0f;
            int n = msg.getNoteNumber();
            for (auto& v : voices) {
                if (!v.active) continue;
                if ((v.mpeChannel == ch || ch == 1) && v.note == n)
                    v.mpe.polyPressure = p;
            }
        } else if (msg.isController() && msg.getControllerNumber() == 74) {
            float t = msg.getControllerValue() / 127.0f;
            for (auto& v : voices) {
                if (!v.active) continue;
                if (v.mpeChannel == ch || ch == 1)
                    v.mpe.timbre = t;
            }
        }
    }
}

} // namespace SoundShop

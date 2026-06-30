#include "poly_voice_processor.h"
#include <cmath>

namespace SoundShop {

PolyVoiceProcessor::PolyVoiceProcessor(Node& containerNode_, NodeGraph& graph_,
                                       Transport& transport_)
    : containerNode(containerNode_), graph(graph_), transport(transport_),
      containerId(containerNode_.id) {
    // One MIDI input bus, one stereo output bus. enableAllBuses() in rebuildGraph
    // plus widenForControl set the channel layout; default stereo out here.
    setPlayConfigDetails(0, 2, sampleRate, blockSize);
}

VoiceInProcessor* PolyVoiceProcessor::findVoiceIn(GraphProcessor& gp) {
    auto* g = gp.getGraph();
    if (!g) return nullptr;
    for (auto* node : g->getNodes()) {
        if (!node) continue;
        if (auto* vi = dynamic_cast<VoiceInProcessor*>(node->getProcessor()))
            return vi;
    }
    return nullptr;
}

void PolyVoiceProcessor::buildVoices() {
    voices.clear();
    const int n = juce::jlimit(1, 64, containerNode.voicePolyphony);
    for (int i = 0; i < n; ++i) {
        Voice v;
        v.gp = std::make_unique<GraphProcessor>();
        v.gp->setBuildScope(containerId);
        // prepare() sets the GraphProcessor's sampleRate/blockSize members that
        // widenForControl reads during rebuildGraph; it also prepares the (still
        // empty) graph - harmless. rebuildGraph then populates the inner subgraph
        // (only nodes whose voiceContainerId == containerId), and we prepare the
        // now-populated graph for playback.
        v.gp->prepare(graph, sampleRate, blockSize);
        v.gp->rebuildGraph(graph, transport);
        if (v.gp->getGraph())
            v.gp->getGraph()->prepareToPlay(sampleRate, blockSize);
        v.voiceIn = findVoiceIn(*v.gp);
        v.scratch.setSize(2, blockSize);
        voices.push_back(std::move(v));
    }
    alloc.resize((int) voices.size());
    built = true;
}

void PolyVoiceProcessor::prepareToPlay(double sr, int bs) {
    sampleRate = sr;
    blockSize = bs;
    buildVoices();
}

void PolyVoiceProcessor::processBlock(juce::AudioBuffer<float>& buf,
                                      juce::MidiBuffer& midi) {
    const int n = buf.getNumSamples();
    buf.clear();
    if (!built || voices.empty()) return;

    // Refresh the steal policy from the container each block so a live menu
    // change (Voice stealing submenu) takes effect immediately without a
    // graph rebuild. jlimit guards against a stale/garbage serialized value.
    alloc.stealMode = juce::jlimit(0, 2, containerNode.voiceStealMode);

    // Apply an MPE expression update to every voice the message addresses. A
    // master/non-MPE channel (1) broadcasts to ALL active voices; a member
    // channel (2-16) targets only the voice(s) allocated on that channel.
    // `note >= 0` further restricts to a specific note (poly key pressure).
    auto routeExpr = [this](int chan, int note, auto&& fn) {
        for (int i = 0; i < alloc.size(); ++i) {
            const auto& s = alloc.slots[(size_t) i];
            if (!s.active) continue;
            if (chan != 1 && s.channel != chan) continue;
            if (note >= 0 && s.note != note) continue;
            if (voices[(size_t) i].voiceIn) fn(*voices[(size_t) i].voiceIn);
        }
    };

    // 1. Translate incoming MIDI into voice allocation + gate events. The policy
    //    (free slot / steal-oldest / note matching) lives in VoiceAllocator; here
    //    we just apply its decisions to the matching voice's audio objects.
    for (const auto meta : midi) {
        const auto m = meta.getMessage();
        const int off = meta.samplePosition;
        if (m.isNoteOn() && m.getVelocity() > 0) {
            // Allocate a whole UNISON STACK for this note (1 slot when unison is
            // off). Every slot of the stack shares a group id so the note-off
            // releases them together; each is detuned and panned across the stack.
            const int uni = juce::jlimit(1, VoiceAllocator::GroupResult::kMaxUnison,
                                         containerNode.voiceUnison);
            const auto grp = alloc.noteOnGroup(m.getNoteNumber(), m.getChannel(), uni);
            const float detune = juce::jmax(0.0f, containerNode.voiceUnisonDetune);
            const float spread = juce::jlimit(0.0f, 1.0f, containerNode.voiceUnisonSpread);
            // Equal RMS across the stack: detuned voices are decorrelated, so the
            // sum grows ~sqrt(count); 1/sqrt(count) keeps perceived level steady.
            const float norm = grp.count > 0 ? 1.0f / std::sqrt((float) grp.count) : 1.0f;
            for (int k = 0; k < grp.count; ++k) {
                const int slot = grp.slot[k];
                auto& v = voices[(size_t) slot];
                // Symmetric position in [-1, 1] for this slot in the stack.
                const float t = grp.count > 1 ? (2.0f * k / (grp.count - 1) - 1.0f) : 0.0f;
                if (v.voiceIn) {
                    v.voiceIn->setUnisonDetune(t * detune);
                    if (grp.stole[k]) v.voiceIn->reset();
                    const float glideMs = grp.stole[k]
                        ? juce::jmax(0.0f, containerNode.voiceGlideMs) : 0.0f;
                    v.voiceIn->noteOn(off, m.getNoteNumber(), m.getFloatVelocity(), glideMs);
                }
                // Stereo balance: centre unity, full pan kills the far channel.
                const float pan = t * spread;
                v.panGainL = (1.0f - juce::jmax(0.0f, pan)) * norm;
                v.panGainR = (1.0f + juce::jmin(0.0f, pan)) * norm;
            }
        } else if (m.isPitchWheel()) {
            // Per-note pitch bend. A member-channel (>1) bend bends only the
            // voice(s) on that channel using the MPE default +/-48 semitone
            // range; a channel-1 (master / non-MPE) bend bends ALL voices with
            // the conventional +/-2 semitones. routeExpr applies the matching set.
            const int chan = m.getChannel();
            const float norm = (m.getPitchWheelValue() - 8192) / 8192.0f; // -1..~1
            const float semis = norm * (chan == 1 ? 2.0f : 48.0f);
            routeExpr(chan, -1, [&](VoiceInProcessor& vi){ vi.setPitchBend(semis); });
        } else if (m.isChannelPressure()) {
            const int chan = m.getChannel();
            const float z = m.getChannelPressureValue() / 127.0f;
            routeExpr(chan, -1, [&](VoiceInProcessor& vi){ vi.setPressure(z); });
        } else if (m.isAftertouch()) {
            // Polyphonic key pressure: note-specific, so also match the note.
            const int chan = m.getChannel();
            const float z = m.getAfterTouchValue() / 127.0f;
            routeExpr(chan, m.getNoteNumber(), [&](VoiceInProcessor& vi){ vi.setPressure(z); });
        } else if (m.isController() && m.getControllerNumber() == 74) {
            // CC74 = MPE timbre ("slide" / Y axis).
            const int chan = m.getChannel();
            const float y = m.getControllerValue() / 127.0f;
            routeExpr(chan, -1, [&](VoiceInProcessor& vi){ vi.setTimbre(y); });
        } else if (m.isNoteOff() || (m.isNoteOn() && m.getVelocity() == 0)) {
            // Release the whole unison stack struck for this note on this channel.
            const auto rel = alloc.noteOffGroup(m.getNoteNumber(), m.getChannel());
            for (int k = 0; k < rel.count; ++k) {
                auto& v = voices[(size_t) rel.slot[k]];
                if (v.voiceIn) v.voiceIn->noteOff(off, m.getNoteNumber());
            }
        } else if (m.isAllNotesOff() || m.isAllSoundOff()) {
            for (int i = 0; i < alloc.size(); ++i) {
                if (!alloc.slots[(size_t) i].active) continue;
                if (voices[(size_t) i].voiceIn)
                    voices[(size_t) i].voiceIn->noteOff(0, alloc.slots[(size_t) i].note);
            }
            alloc.allNotesOff();
        }
    }

    // 2. Render each active voice into its scratch buffer and sum.
    const float blockMs = sampleRate > 0 ? 1000.0f * (float)n / (float)sampleRate : 0.0f;
    for (int i = 0; i < (int) voices.size(); ++i) {
        if (!alloc.slots[(size_t) i].active) continue;
        auto& v = voices[(size_t) i];
        if (v.scratch.getNumSamples() < n)
            v.scratch.setSize(2, n, false, false, true);
        v.scratch.clear();
        juce::MidiBuffer emptyMidi;
        if (v.gp->getGraph())
            v.gp->getGraph()->processBlock(v.scratch, emptyMidi);

        // Sum with the voice's unison balance (unity 1,1 when unison is off).
        if (buf.getNumChannels() > 0) buf.addFrom(0, 0, v.scratch, 0, 0, n, v.panGainL);
        if (buf.getNumChannels() > 1) buf.addFrom(1, 0, v.scratch, 1, 0, n, v.panGainR);

        // Voice-free detection: a released voice whose output has decayed below
        // the floor for kFreeMs is done (envelope tail finished).
        const float rms = v.scratch.getRMSLevel(0, 0, n);
        if (alloc.postRender(i, rms, blockMs, kFloorRms, kFreeMs)) {
            if (v.voiceIn) v.voiceIn->reset();
        }
    }
}

} // namespace SoundShop

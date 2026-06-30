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

    // 1. Translate incoming MIDI into voice allocation + gate events. The policy
    //    (free slot / steal-oldest / note matching) lives in VoiceAllocator; here
    //    we just apply its decisions to the matching voice's audio objects.
    for (const auto meta : midi) {
        const auto m = meta.getMessage();
        const int off = meta.samplePosition;
        if (m.isNoteOn() && m.getVelocity() > 0) {
            const auto res = alloc.noteOn(m.getNoteNumber());
            if (res.slot < 0) continue;
            auto& v = voices[(size_t) res.slot];
            if (res.stole && v.voiceIn) v.voiceIn->reset();
            if (v.voiceIn) {
                // Portamento applies only when this voice was STOLEN mid-note
                // (the pitch slides from the old note to the new one). A fresh /
                // freed voice has no meaningful "previous pitch", so it starts on
                // pitch with no glide. glide time is a live container field.
                const float glideMs = res.stole
                    ? juce::jmax(0.0f, containerNode.voiceGlideMs) : 0.0f;
                v.voiceIn->noteOn(off, m.getNoteNumber(), m.getFloatVelocity(), glideMs);
            }
        } else if (m.isNoteOff() || (m.isNoteOn() && m.getVelocity() == 0)) {
            const int slot = alloc.noteOff(m.getNoteNumber());
            if (slot >= 0 && voices[(size_t) slot].voiceIn)
                voices[(size_t) slot].voiceIn->noteOff(off, m.getNoteNumber());
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

        for (int c = 0; c < 2 && c < buf.getNumChannels(); ++c)
            buf.addFrom(c, 0, v.scratch, c, 0, n);

        // Voice-free detection: a released voice whose output has decayed below
        // the floor for kFreeMs is done (envelope tail finished).
        const float rms = v.scratch.getRMSLevel(0, 0, n);
        if (alloc.postRender(i, rms, blockMs, kFloorRms, kFreeMs)) {
            if (v.voiceIn) v.voiceIn->reset();
        }
    }
}

} // namespace SoundShop

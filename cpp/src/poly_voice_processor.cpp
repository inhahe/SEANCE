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
    built = true;
}

void PolyVoiceProcessor::prepareToPlay(double sr, int bs) {
    sampleRate = sr;
    blockSize = bs;
    buildVoices();
}

int PolyVoiceProcessor::allocVoice() {
    for (int i = 0; i < (int)voices.size(); ++i)
        if (!voices[i].active) return i;
    // All busy - steal the oldest.
    int oldest = 0;
    long long minAge = voices.empty() ? 0 : voices[0].age;
    for (int i = 1; i < (int)voices.size(); ++i)
        if (voices[i].age < minAge) { minAge = voices[i].age; oldest = i; }
    if (!voices.empty() && voices[oldest].voiceIn)
        voices[oldest].voiceIn->reset();
    return oldest;
}

void PolyVoiceProcessor::processBlock(juce::AudioBuffer<float>& buf,
                                      juce::MidiBuffer& midi) {
    const int n = buf.getNumSamples();
    buf.clear();
    if (!built || voices.empty()) return;

    // 1. Translate incoming MIDI into voice allocation + gate events.
    for (const auto meta : midi) {
        const auto m = meta.getMessage();
        const int off = meta.samplePosition;
        if (m.isNoteOn() && m.getVelocity() > 0) {
            const int idx = allocVoice();
            auto& v = voices[idx];
            v.active = true;
            v.gateHeld = true;
            v.note = m.getNoteNumber();
            v.age = ++ageCounter;
            v.silenceMs = 0.0f;
            if (v.voiceIn)
                v.voiceIn->noteOn(off, m.getNoteNumber(), m.getFloatVelocity());
        } else if (m.isNoteOff() || (m.isNoteOn() && m.getVelocity() == 0)) {
            for (auto& v : voices)
                if (v.active && v.gateHeld && v.note == m.getNoteNumber()) {
                    v.gateHeld = false;
                    if (v.voiceIn) v.voiceIn->noteOff(off, m.getNoteNumber());
                    break;
                }
        } else if (m.isAllNotesOff() || m.isAllSoundOff()) {
            for (auto& v : voices) {
                if (!v.active) continue;
                v.gateHeld = false;
                if (v.voiceIn) v.voiceIn->noteOff(0, v.note);
            }
        }
    }

    // 2. Render each active voice into its scratch buffer and sum.
    const float blockMs = sampleRate > 0 ? 1000.0f * (float)n / (float)sampleRate : 0.0f;
    for (auto& v : voices) {
        if (!v.active) continue;
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
        if (!v.gateHeld && rms < kFloorRms) {
            v.silenceMs += blockMs;
            if (v.silenceMs >= kFreeMs) {
                v.active = false;
                if (v.voiceIn) v.voiceIn->reset();
            }
        } else {
            v.silenceMs = 0.0f;
        }
    }
}

} // namespace SoundShop

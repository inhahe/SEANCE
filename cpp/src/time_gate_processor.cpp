#include "time_gate_processor.h"
#include <cmath>
#include <algorithm>

namespace SoundShop {

TimeGateProcessor::TimeGateProcessor(int lid, Node& src, NodeGraph& g, Transport& t)
    : linkId(lid), sourceNode(src), graph(g), transport(t)
{}

float TimeGateProcessor::computeWet(float beat) const {
    float wet = 0.0f;

    // Collect all group IDs that contain this link.
    std::vector<int> myGroupIds;
    for (const auto& grp : graph.effectGroups)
        for (int id : grp.linkIds)
            if (id == linkId) { myGroupIds.push_back(grp.id); break; }

    // The default crossfade comes from the project-wide setting; a group can
    // override it with a non-zero crossfadeSec of its own. Hardcoded zero on
    // a group means "fall through to the global default" (so users can set a
    // single global value and have it apply everywhere).
    const float globalDefault = std::max(0.0f, graph.globalCrossfadeSec);

    auto applyRegion = [&](const EffectRegion& region) {
        bool applies = false;
        float crossfadeSec = globalDefault;

        if (region.linkId == linkId && region.linkId >= 0) {
            applies = true;
        } else if (region.groupId >= 0) {
            for (int gid : myGroupIds) {
                if (gid == region.groupId) {
                    applies = true;
                    if (auto* grp = graph.findEffectGroup(gid))
                        if (grp->crossfadeSec > 0.0f)
                            crossfadeSec = grp->crossfadeSec;
                    break;
                }
            }
        }
        if (!applies) return;

        // Region is active if beat is inside [startBeat, endBeat].
        // Crossfade ramps in at the start edge and out at the end edge.
        if (beat < region.startBeat || beat > region.endBeat) return;
        float crossfadeBeats = std::max(0.001f,
            crossfadeSec * (float)(transport.bpm / 60.0));
        float fadeIn  = (beat - region.startBeat) / crossfadeBeats;
        float fadeOut = (region.endBeat - beat) / crossfadeBeats;
        float w = std::min(1.0f, std::min(fadeIn, fadeOut));
        wet = std::max(wet, w);
    };

    // Effect regions can live on any node (the source node, or a different
    // track), so scan the whole graph in one pass. Cheap; the graph is tiny
    // and `applies` rejects mismatches early.
    for (const auto& node : graph.nodes)
        for (const auto& region : node.effectRegions)
            applyRegion(region);

    return wet;
}

void TimeGateProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) {
    float beat = (float)transport.positionBeats();
    float wet = computeWet(beat);

    if (wet <= 0.0f) {
        // Fully closed: silence audio, clear MIDI
        buf.clear();
        midi.clear();
        return;
    }

    if (wet >= 1.0f) {
        // Fully open: pass through unchanged
        return;
    }

    // Crossfading: scale audio by wet amount
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        buf.applyGain(ch, 0, buf.getNumSamples(), wet);

    // Scale MIDI velocity by wet (optional — crude but prevents loud note-ons
    // during crossfade. Could be omitted if MIDI gating feels unnatural.)
    // For v1, just pass MIDI through unchanged during crossfade.
}

} // namespace SoundShop

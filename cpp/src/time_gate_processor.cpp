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

    // Scan effect regions on the source node. A region applies if it
    // directly references our linkId, or references a group we belong to.
    for (const auto& region : sourceNode.effectRegions) {
        bool applies = false;
        float crossfadeSec = 0.05f;

        if (region.linkId == linkId && region.linkId >= 0) {
            applies = true;
        } else if (region.groupId >= 0) {
            for (int gid : myGroupIds) {
                if (gid == region.groupId) {
                    applies = true;
                    // Use the group's crossfade duration if available
                    if (auto* grp = graph.findEffectGroup(gid))
                        crossfadeSec = grp->crossfadeSec;
                    break;
                }
            }
        }
        if (!applies) continue;

        // Region is active if beat is inside [startBeat, endBeat].
        // Compute crossfade ramp at both edges.
        if (beat >= region.startBeat && beat <= region.endBeat) {
            float crossfadeBeats = crossfadeSec * (transport.bpm / 60.0f);
            crossfadeBeats = std::max(0.001f, crossfadeBeats);
            float fadeIn  = (beat - region.startBeat) / crossfadeBeats;
            float fadeOut = (region.endBeat - beat) / crossfadeBeats;
            float w = std::min(1.0f, std::min(fadeIn, fadeOut));
            wet = std::max(wet, w);
        }
    }

    // Also scan ALL nodes (not just sourceNode) for regions that reference
    // our link or our groups, in case the region lives on a different track.
    for (const auto& node : graph.nodes) {
        if (&node == &sourceNode) continue; // already scanned above
        for (const auto& region : node.effectRegions) {
            bool applies = false;
            float crossfadeSec = 0.05f;

            if (region.linkId == linkId && region.linkId >= 0) {
                applies = true;
            } else if (region.groupId >= 0) {
                for (int gid : myGroupIds) {
                    if (gid == region.groupId) {
                        applies = true;
                        if (auto* grp = graph.findEffectGroup(gid))
                            crossfadeSec = grp->crossfadeSec;
                        break;
                    }
                }
            }
            if (!applies) continue;

            if (beat >= region.startBeat && beat <= region.endBeat) {
                float crossfadeBeats = crossfadeSec * (transport.bpm / 60.0f);
                crossfadeBeats = std::max(0.001f, crossfadeBeats);
                float fadeIn  = (beat - region.startBeat) / crossfadeBeats;
                float fadeOut = (region.endBeat - beat) / crossfadeBeats;
                float w = std::min(1.0f, std::min(fadeIn, fadeOut));
                wet = std::max(wet, w);
            }
        }
    }

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

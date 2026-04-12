#include "node_graph.h"
#include "project_file.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace SoundShop {

void NodeGraph::commitSnapshot(const std::string& description) {
    // Serialize the current graph (graph-only — plugin state excluded).
    auto text = ProjectFile::serializeForUndo(*this);

    // De-dup against the previous step's snapshot. memcmp on the underlying
    // strings (length check first, then byte compare with early exit) — fast
    // for the no-change case, which is the common case for defensive calls.
    const auto& prev = undoTree.currentSnapshot();
    if (text.size() == prev.size() && text == prev)
        return; // nothing changed since the previous step

    undoTree.pushSnapshot(std::move(text), description);
    dirty = true;
}

static const char* channelLabel(int ch) {
    switch (ch) {
        case 1: return "mono";
        case 2: return "stereo";
        case 6: return "5.1";
        case 8: return "7.1";
        default: return "?ch";
    }
}

NodeGraph::NodeGraph() {}

float NodeGraph::getTimelineBeats(const Node& node) const {
    if (node.clips.empty()) return 4.0f;
    float end = 0;
    for (auto& c : node.clips)
        end = std::max(end, c.startBeat + c.lengthBeats);
    return std::max(4.0f, std::ceil(end / 4.0f) * 4.0f);
}

Node& NodeGraph::addNode(const std::string& name, NodeType type,
                          std::vector<Pin> ins, std::vector<Pin> outs,
                          Vec2 pos) {
    Node node;
    node.id = newId();

    // Auto-number if a node with this exact name already exists
    int count = 0;
    for (auto& n : nodes)
        if (n.name == name || (n.name.rfind(name + " ", 0) == 0))
            count++;
    node.name = count > 0 ? name + " " + std::to_string(count + 1) : name;

    node.type = type;
    node.pos = pos;
    for (auto& p : ins) { p.id = newId(); node.pinsIn.push_back(p); }
    for (auto& p : outs) { p.id = newId(); node.pinsOut.push_back(p); }
    nodes.push_back(std::move(node));
    dirty = true;
    return nodes.back();
}

void NodeGraph::addLink(int outPin, int inPin) {
    links.push_back({newId(), outPin, inPin});
    dirty = true;
}

Node* NodeGraph::findNode(int id) {
    for (auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

Node& NodeGraph::createGroup(const std::string& name, Vec2 pos) {
    return addNode(name, NodeType::Group, {}, {}, pos);
}

void NodeGraph::addToGroup(int groupId, int childId) {
    auto* group = findNode(groupId);
    auto* child = findNode(childId);
    if (!group || !child || group->type != NodeType::Group) return;
    if (child->parentGroupId == groupId) return;
    if (child->parentGroupId >= 0)
        removeFromGroup(childId);
    group->childNodeIds.push_back(childId);
    child->parentGroupId = groupId;
    dirty = true;
}

void NodeGraph::removeFromGroup(int childId) {
    auto* child = findNode(childId);
    if (!child || child->parentGroupId < 0) return;
    auto* group = findNode(child->parentGroupId);
    if (group) {
        auto& ids = group->childNodeIds;
        ids.erase(std::remove(ids.begin(), ids.end(), childId), ids.end());
    }
    child->parentGroupId = -1;
    dirty = true;
}

void NodeGraph::insertTime(float atBeat, float duration, int nodeId) {
    auto process = [&](Node& node) {
        // Shift clips and their contents
        if (node.type == NodeType::MidiTimeline || node.type == NodeType::AudioTimeline) {
            for (auto& clip : node.clips) {
                if (clip.startBeat >= atBeat) {
                    clip.startBeat += duration;
                } else if (clip.startBeat + clip.lengthBeats > atBeat) {
                    clip.lengthBeats += duration;
                    float clipInsert = atBeat - clip.startBeat;
                    for (auto& n : clip.notes)
                        if (n.offset >= clipInsert) n.offset += duration;
                    for (auto& cc : clip.ccEvents)
                        if (cc.offset >= clipInsert) cc.offset += duration;
                }
            }
        }
        // Shift automation points on all params
        for (auto& param : node.params)
            for (auto& pt : param.automation.points)
                if (pt.beat >= atBeat) pt.beat += duration;
    };

    if (nodeId >= 0) {
        auto* n = findNode(nodeId);
        if (n) process(*n);
    } else {
        for (auto& n : nodes) process(n);
    }

    // Also shift markers
    if (nodeId < 0) {
        for (auto& m : markers)
            if (m.beat >= atBeat) m.beat += duration;
    }
    dirty = true;
}

void NodeGraph::deleteTime(float fromBeat, float toBeat, int nodeId) {
    float duration = toBeat - fromBeat;
    if (duration <= 0) return;

    auto process = [&](Node& node) {
        // Shift automation points on all params
        for (auto& param : node.params) {
            auto& pts = param.automation.points;
            // Remove points in the deleted range
            pts.erase(std::remove_if(pts.begin(), pts.end(),
                [&](auto& pt) { return pt.beat >= fromBeat && pt.beat < toBeat; }), pts.end());
            // Shift points after the deleted range
            for (auto& pt : pts)
                if (pt.beat >= toBeat) pt.beat -= duration;
        }

        if (node.type != NodeType::MidiTimeline && node.type != NodeType::AudioTimeline) return;
        for (auto it = node.clips.begin(); it != node.clips.end(); ) {
            auto& clip = *it;
            float clipEnd = clip.startBeat + clip.lengthBeats;

            if (clip.startBeat >= toBeat) {
                // Entirely after the deleted region — shift back
                clip.startBeat -= duration;
            } else if (clipEnd <= fromBeat) {
                // Entirely before — no change
            } else if (clip.startBeat >= fromBeat && clipEnd <= toBeat) {
                // Entirely within the deleted region — remove
                it = node.clips.erase(it);
                continue;
            } else if (clip.startBeat < fromBeat && clipEnd > toBeat) {
                // Straddles the entire deleted region — shrink it
                float clipFrom = fromBeat - clip.startBeat;
                float clipTo = toBeat - clip.startBeat;
                // Remove notes/CC in the deleted range, shift those after
                for (auto ni = clip.notes.begin(); ni != clip.notes.end(); ) {
                    if (ni->offset >= clipFrom && ni->offset < clipTo)
                        ni = clip.notes.erase(ni);
                    else {
                        if (ni->offset >= clipTo) ni->offset -= duration;
                        ++ni;
                    }
                }
                for (auto ci = clip.ccEvents.begin(); ci != clip.ccEvents.end(); ) {
                    if (ci->offset >= clipFrom && ci->offset < clipTo)
                        ci = clip.ccEvents.erase(ci);
                    else {
                        if (ci->offset >= clipTo) ci->offset -= duration;
                        ++ci;
                    }
                }
                clip.lengthBeats -= duration;
            } else if (clip.startBeat < fromBeat) {
                // Overlaps start of deleted region — trim end
                clip.lengthBeats = fromBeat - clip.startBeat;
                clip.notes.erase(std::remove_if(clip.notes.begin(), clip.notes.end(),
                    [&](auto& n) { return n.offset >= clip.lengthBeats; }), clip.notes.end());
                clip.ccEvents.erase(std::remove_if(clip.ccEvents.begin(), clip.ccEvents.end(),
                    [&](auto& cc) { return cc.offset >= clip.lengthBeats; }), clip.ccEvents.end());
            } else {
                // Overlaps end of deleted region — trim start and shift
                float trimAmount = toBeat - clip.startBeat;
                for (auto ni = clip.notes.begin(); ni != clip.notes.end(); ) {
                    ni->offset -= trimAmount;
                    if (ni->offset < 0) ni = clip.notes.erase(ni);
                    else ++ni;
                }
                for (auto ci = clip.ccEvents.begin(); ci != clip.ccEvents.end(); ) {
                    ci->offset -= trimAmount;
                    if (ci->offset < 0) ci = clip.ccEvents.erase(ci);
                    else ++ci;
                }
                clip.lengthBeats -= trimAmount;
                clip.startBeat = fromBeat;
            }
            ++it;
        }
    };

    if (nodeId >= 0) {
        auto* n = findNode(nodeId);
        if (n) process(*n);
    } else {
        for (auto& n : nodes) process(n);
    }

    // Also shift markers
    if (nodeId < 0) {
        markers.erase(std::remove_if(markers.begin(), markers.end(),
            [&](auto& m) { return m.beat >= fromBeat && m.beat < toBeat; }), markers.end());
        for (auto& m : markers)
            if (m.beat >= toBeat) m.beat -= duration;
    }
    dirty = true;
}

float NodeGraph::getAbsoluteBeatOffset(int nodeId) {
    float total = 0;
    int current = nodeId;
    int depth = 0;
    while (current >= 0 && depth < 20) { // depth limit to prevent infinite loops
        auto* node = findNode(current);
        if (!node) break;
        total += node->groupBeatOffset;
        current = node->parentGroupId;
        depth++;
    }
    return total;
}

void NodeGraph::resolveAnchors() {
    // Resolve marker anchors
    for (auto& node : nodes) {
        if (!node.anchorMarker.empty()) {
            float beat = resolveMarkerBeat(node.anchorMarker);
            if (beat >= 0)
                node.groupBeatOffset = beat;
        }
    }
    // Compute cascading absolute offsets
    for (auto& node : nodes)
        node.absoluteBeatOffset = getAbsoluteBeatOffset(node.id);
}

void NodeGraph::setupDefaultGraph() {
    nodes.reserve(16);

    // Computer Keyboard Input — represents the on-screen / typing input
    // device. Live MIDI from the computer keyboard is pushed into this
    // node's output buffer by AudioEngine::keyboardNoteOn. Users wire
    // this node's MIDI Out to whatever they want to play.
    auto& keyIn = addNode("Computer Keyboard", NodeType::MidiInput,
        {}, {Pin{0, "MIDI Out", PinKind::Midi, false}}, {80, 120});
    keyIn.midiInputSourceId = "keyboard";

    // A starter MIDI Track ready to record into, with its MIDI In pin
    // pre-wired from the keyboard so typing Just Works on a fresh project.
    auto& track = addNode("MIDI Track", NodeType::MidiTimeline,
        {Pin{0, "MIDI In", PinKind::Midi, true}},
        {Pin{0, "MIDI", PinKind::Midi, false}}, {380, 120});
    track.clips.push_back({"Clip 1", 0, 4, juce::Colours::cornflowerblue.getARGB()});

    // Master Out lives well to the right so freshly-created nodes (which
    // appear near the visible top-left) don't immediately overlap it.
    addNode("Master Out", NodeType::Output,
        {Pin{0, "In", PinKind::Audio, true}}, {}, {1400, 300});

    // Auto-wire Computer Keyboard -> MIDI Track so the user can play the
    // instant they pick a synth and drop it after the track.
    if (!keyIn.pinsOut.empty() && !track.pinsIn.empty())
        addLink(keyIn.pinsOut[0].id, track.pinsIn[0].id);

    dirty = false; // don't count initial setup as a change
}

} // namespace SoundShop

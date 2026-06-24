#include "node_graph.h"
#include "project_file.h"
#include "warp.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace SoundShop {

void NodeGraph::commitSnapshot(const std::string& description) {
    // Serialize the current graph (graph-only - plugin state excluded).
    auto text = ProjectFile::serializeForUndo(*this);

    // De-dup against the previous step's snapshot. memcmp on the underlying
    // strings (length check first, then byte compare with early exit) - fast
    // for the no-change case, which is the common case for defensive calls.
    const auto& prev = undoTree.currentSnapshot();
    if (text.size() == prev.size() && text == prev)
        return; // nothing changed since the previous step

    undoTree.pushSnapshot(std::move(text), description);
    dirty = true;
}

void NodeGraph::resolveAhdsrReferences() {
    for (auto& n : nodes) {
        if (n.ahdsrAssetId < 0) continue;
        const AssetEntry* e = assets.find(n.ahdsrAssetId);
        if (e && e->kind == AssetKind::AhdsrCurve) {
            AHDSREnvelope::decode(e->payload, n.ahdsrEnvelope);
        } else {
            // The referenced curve is gone (hard-deleted in another project /
            // by import). Fall back to "independent" so the node keeps its last
            // mirrored envelope instead of silently dangling.
            n.ahdsrAssetId = -1;
        }
    }
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

double NodeGraph::effectiveSongLengthBeats() const {
    // Explicit override wins.
    if (songLengthBeats > 0) return songLengthBeats;

    // Auto-derive: max getTimelineBeats() across all timeline nodes that
    // actually have clips. getTimelineBeats() returns 4.0 for empty
    // timelines, which would falsely make an empty project "4 beats long";
    // skip them so a project with no clips at all returns 0 (= no end).
    double maxEnd = 0.0;
    for (const auto& n : nodes) {
        if (n.type != NodeType::AudioTimeline &&
            n.type != NodeType::MidiTimeline) continue;
        if (n.clips.empty()) continue;
        double e = (double) getTimelineBeats(n);
        if (e > maxEnd) maxEnd = e;
    }
    return maxEnd;
}

double NodeGraph::growSongLengthToContent() {
    double prior = songLengthBeats;
    if (songLengthBeats <= 0) return prior;  // auto mode follows clips already
    double contentEnd = 0.0;
    for (const auto& n : nodes) {
        if (n.type != NodeType::AudioTimeline &&
            n.type != NodeType::MidiTimeline) continue;
        if (n.clips.empty()) continue;
        contentEnd = std::max(contentEnd, (double) getTimelineBeats(n));
    }
    if (contentEnd > songLengthBeats) {
        songLengthBeats = contentEnd;
        dirty = true;
    }
    return prior;
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
    // Lock the structural mutation against the audio thread. push_back may
    // reallocate `nodes`, move-constructing every existing Node and nulling
    // the moved-from shared_ptr members; the audio callback holding a Node&
    // into the old storage would then crash on a null mutex (see the
    // mutationLock comment in node_graph.h). recursive_mutex so batch callers
    // that already hold the lock (setupDefaultGraph, project/MOD load) nest
    // safely.
    {
        std::lock_guard<std::recursive_mutex> lk(mutationLock);
        nodes.push_back(std::move(node));
    }
    dirty = true;
    return nodes.back();
}

void NodeGraph::addLink(int outPin, int inPin) {
    // Same reallocation race as addNode: a push_back that grows `links` can
    // tear the audio thread's iteration in rebuildGraph. Lock it.
    {
        std::lock_guard<std::recursive_mutex> lk(mutationLock);
        links.push_back({newId(), outPin, inPin});
    }
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
                // Entirely after the deleted region - shift back
                clip.startBeat -= duration;
            } else if (clipEnd <= fromBeat) {
                // Entirely before - no change
            } else if (clip.startBeat >= fromBeat && clipEnd <= toBeat) {
                // Entirely within the deleted region - remove
                it = node.clips.erase(it);
                continue;
            } else if (clip.startBeat < fromBeat && clipEnd > toBeat) {
                // Straddles the entire deleted region - shrink it
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
                // Overlaps start of deleted region - trim end
                clip.lengthBeats = fromBeat - clip.startBeat;
                clip.notes.erase(std::remove_if(clip.notes.begin(), clip.notes.end(),
                    [&](auto& n) { return n.offset >= clip.lengthBeats; }), clip.notes.end());
                clip.ccEvents.erase(std::remove_if(clip.ccEvents.begin(), clip.ccEvents.end(),
                    [&](auto& cc) { return cc.offset >= clip.lengthBeats; }), clip.ccEvents.end());
            } else {
                // Overlaps end of deleted region - trim start and shift
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

    // Seed the curated built-in morph chains into the project's asset library so
    // the morph picker (and the Asset Library panel) are never empty on a fresh
    // project. Idempotent + code-owned: see seedBuiltinMorphLibrary in warp.h.
    seedBuiltinMorphLibrary(assets);

    // Reset song-end / repeat state to defaults. Without this, switching
    // from a loaded project (e.g. a MOD import with songLengthBeats=352
    // and SongRepeat::Forever) to File -> New would leave those values in
    // place, and the auto-derive from clips would be silently overridden
    // by a stale explicit value from the previous project.
    songLengthBeats = 0;                  // 0 = auto-derive from clips
    songRepeatMode  = SongRepeat::None;   // halt at song end by default
    songRepeatCount = 1;

    // Computer Keyboard Input - represents the on-screen / typing input
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

// ---------------------------------------------------------------------------
// On-demand modulation pins (#88) - graph-level helpers. Pure data-model: they
// mutate `graph` and leave commit/undo/rebuild to the caller. NodeGraphComponent
// wraps these with commitSnapshot()+onNodeEdited()+repaint(); the warp/morph
// editor folds them into its settled-edit commit. Addressing is by stable
// (nodeId, paramIndex) so nothing dangles across an async menu/checkbox callback.
// ---------------------------------------------------------------------------

bool hasParamModPin(const NodeGraph& graph, int nodeId, int paramIndex) {
    // findNode has no const overload; we only read, so iterate the const vector.
    for (const Node& nd : graph.nodes) {
        if (nd.id != nodeId) continue;
        for (const auto& mp : nd.modPins)
            if (mp.paramIndex == paramIndex) return true;
        return false;
    }
    return false;
}

int addParamModPin(NodeGraph& graph, int nodeId, int paramIndex, bool absolute) {
    Node* nd = graph.findNode(nodeId);
    if (!nd || paramIndex < 0 || paramIndex >= (int)nd->params.size()) return -1;
    // Idempotent: if a pin already exists for this param, return it unchanged.
    for (const auto& mp : nd->modPins)
        if (mp.paramIndex == paramIndex) return mp.pinId;
    // Consumed block-rate (applySignalModulations reads sample 0), so the pin is
    // a Param (block-rate, orange) - NOT a Signal. The receiver decides the rate.
    std::string pinName = (absolute ? "Set: " : "Mod: ") + nd->params[paramIndex].name;
    int newPinId = graph.allocId();
    nd->pinsIn.push_back({newPinId, pinName, PinKind::Param, true, 1});
    Node::ModPin mp;
    mp.paramIndex = paramIndex;
    mp.pinId      = newPinId;
    mp.depth      = 1.0f;
    mp.mode       = absolute ? Node::ModPin::Mode::Absolute
                             : Node::ModPin::Mode::Modulate;
    nd->modPins.push_back(mp);
    graph.dirty = true;
    return newPinId;
}

bool removeParamModPin(NodeGraph& graph, int nodeId, int paramIndex) {
    Node* nd = graph.findNode(nodeId);
    if (!nd) return false;
    bool removed = false;
    for (auto it = nd->modPins.begin(); it != nd->modPins.end(); ++it) {
        if (it->paramIndex != paramIndex) continue;
        int pinId = it->pinId;
        nd->pinsIn.erase(
            std::remove_if(nd->pinsIn.begin(), nd->pinsIn.end(),
                [pinId](const Pin& p) { return p.id == pinId; }),
            nd->pinsIn.end());
        {
            std::lock_guard<std::recursive_mutex> lk(graph.mutationLock);
            graph.links.erase(
                std::remove_if(graph.links.begin(), graph.links.end(),
                    [pinId](const auto& l) { return l.endPin == pinId; }),
                graph.links.end());
        }
        nd->modPins.erase(it);
        removed = true;
        break;
    }
    // Clear modulation state on the param so it returns to its resting value.
    if (removed && paramIndex >= 0 && paramIndex < (int)nd->params.size()) {
        auto& p = nd->params[paramIndex];
        if (p.modulated) { p.value = p.baseValue; p.modulated = false; }
    }
    if (removed) graph.dirty = true;
    return removed;
}

int pruneOrphanModPins(NodeGraph& graph, int nodeId) {
    Node* nd = graph.findNode(nodeId);
    if (!nd) return 0;
    int removed = 0;

    auto pinExists = [&](int pinId) {
        for (const auto& p : nd->pinsIn) if (p.id == pinId) return true;
        return false;
    };
    auto dropPinAndLinks = [&](int pinId) {
        {
            std::lock_guard<std::recursive_mutex> lk(graph.mutationLock);
            graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
                [&](const Link& l) { return l.startPin == pinId || l.endPin == pinId; }),
                graph.links.end());
        }
        nd->pinsIn.erase(std::remove_if(nd->pinsIn.begin(), nd->pinsIn.end(),
            [&](const Pin& p) { return p.id == pinId; }), nd->pinsIn.end());
    };

    // Invariant: every "Mod:"/"Set:" Param input pin is backed by exactly one
    // modPin, and every modPin references an in-range param + a live pin.
    // addParamModPin / removeParamModPin maintain both sides in lockstep, but a
    // historical reconcile/remap bug could strand a pin without its modPin (a
    // dangling "Mod: X" input pin) - which then round-trips through save/load as
    // a ghost modulation input the user can't explain or remove. This restores
    // the invariant on both sides; it's a no-op (returns 0) on a consistent node,
    // which is the common case.

    // 1) Drop modPins whose param is out of range (dead binding). The pin they
    //    point at is meaningless without a param, so drop it too when present.
    //    A modPin whose pin is already missing just loses the stale modPin.
    for (auto it = nd->modPins.begin(); it != nd->modPins.end(); ) {
        const bool badParam = it->paramIndex < 0
                            || it->paramIndex >= (int)nd->params.size();
        const bool noPin = !pinExists(it->pinId);
        if (badParam || noPin) {
            if (!noPin) dropPinAndLinks(it->pinId);
            it = nd->modPins.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }

    // 2) Drop "Mod:"/"Set:" Param input pins that no surviving modPin backs.
    std::set<int> backed;
    for (const auto& mp : nd->modPins) backed.insert(mp.pinId);
    std::vector<int> orphanPins;
    for (const auto& p : nd->pinsIn) {
        if (p.kind != PinKind::Param) continue;
        const bool modLabel = p.name.rfind("Mod: ", 0) == 0
                           || p.name.rfind("Set: ", 0) == 0;
        if (modLabel && !backed.count(p.id)) orphanPins.push_back(p.id);
    }
    for (int pid : orphanPins) { dropPinAndLinks(pid); ++removed; }

    if (removed) graph.dirty = true;
    return removed;
}

// ----------------------------------------------------------------------------
// FM operator envelopes - seed / migrate.
// ----------------------------------------------------------------------------
void ensureFmOpEnvelopes(Node& node) {
    if (node.script != "__fmsynth__") return;
    if (node.opEnvelopes.size() == 4) return;   // already seeded / migrated

    // Legacy per-operator linear-ADSR defaults (the values the old inline
    // FMSynthProcessor used when a param was absent). When a project predates
    // node.opEnvelopes it still carries "Op{i} A/D/S/R" params; we read those
    // so the migrated AHDSR reproduces the old sound. For a brand-new node the
    // params are absent and these defaults apply.
    const float defA = 0.01f, defD = 0.1f, defS = 0.7f, defR = 0.3f;
    auto paramVal = [&](const std::string& nm, float def) -> float {
        for (const auto& p : node.params) if (p.name == nm) return p.value;
        return def;
    };

    node.opEnvelopes.assign(4, AHDSREnvelope{});   // 4x default linear curves
    for (int i = 0; i < 4; ++i) {
        const std::string pre = "Op" + std::to_string(i + 1) + " ";
        AHDSREnvelope& e = node.opEnvelopes[(size_t)i];
        e.attackMs  = std::max(0.001f, paramVal(pre + "A", defA)) * 1000.0f;
        e.holdMs    = 0.0f;
        e.decayMs   = std::max(0.001f, paramVal(pre + "D", defD)) * 1000.0f;
        e.sustain   = paramVal(pre + "S", defS);
        e.releaseMs = std::max(0.001f, paramVal(pre + "R", defR)) * 1000.0f;
        // The FM master output applies note velocity once (out *= v.vel), so the
        // per-operator envelopes must NOT also velocity-scale or velocity would
        // be applied twice. The user can raise this per operator in the editor
        // to get the velocity->brightness behaviour that's idiomatic for FM.
        e.velocitySensitivity = 0.0f;
        e.attackTension = e.decayTension = e.releaseTension = 0.0f;
    }

    // Strip the now-migrated legacy params (keep "Op{i} Ratio" / "Op{i} Level").
    // Matches exactly the names "Op<n> A", "Op<n> D", "Op<n> S", "Op<n> R".
    node.params.erase(std::remove_if(node.params.begin(), node.params.end(),
        [](const Param& p) {
            if (p.name.size() < 4 || p.name.rfind("Op", 0) != 0) return false;
            const char last = p.name.back();
            const char sep  = p.name[p.name.size() - 2];
            return sep == ' ' &&
                   (last == 'A' || last == 'D' || last == 'S' || last == 'R');
        }), node.params.end());
}

} // namespace SoundShop

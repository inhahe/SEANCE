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

double NodeGraph::contentEndBeats() const {
    // Exact end of the last clip across all timeline nodes with clips, NOT
    // rounded up to a bar (so it can land mid-bar). Empty timelines are
    // skipped so a project with no clips returns 0 (= no end).
    //
    // Clip beats are node-local, so a track nested under another track (audio
    // or MIDI) plays absoluteBeatOffset beats later than its clips claim. That
    // offset has to be added here or the song would end -- and an export would
    // stop -- before a nested track's tail had played.
    double maxEnd = 0.0;
    for (const auto& n : nodes) {
        if (n.type != NodeType::AudioTimeline &&
            n.type != NodeType::MidiTimeline) continue;
        if (n.clips.empty()) continue;
        for (const auto& c : n.clips)
            maxEnd = std::max(maxEnd,
                (double) (n.absoluteBeatOffset + c.startBeat + c.lengthBeats));
    }
    return maxEnd;
}

double NodeGraph::effectiveSongLengthBeats() const {
    // Explicit override wins.
    if (songLengthBeats > 0) return songLengthBeats;

    // Auto-derive: exact (un-rounded) end of the last clip across all timeline
    // nodes. We deliberately use contentEndBeats() rather than the bar-rounded
    // getTimelineBeats() so the audible song end matches where the song-end
    // marker is drawn and where content actually stops - the song may end
    // mid-bar. The timeline grid still rounds up for a clean display width.
    return contentEndBeats();
}

double NodeGraph::growSongLengthToContent() {
    double prior = songLengthBeats;
    if (songLengthBeats <= 0) return prior;  // auto mode follows clips already
    // Use the exact (un-rounded) content end so growing the override matches
    // the mid-bar playback end, not a bar-rounded value.
    double contentEnd = contentEndBeats();
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

Node& NodeGraph::addAudioTrack(const std::string& name, Vec2 pos) {
    auto& n = addNode(name, NodeType::AudioTimeline,
        { Pin{0, "Audio In", PinKind::Audio, true} },   // recording / monitoring
        { Pin{0, "Audio", PinKind::Audio, false} }, pos);
    n.clips.push_back({"Clip 1", 0, 4, juce::Colours::forestgreen.getARGB()});
    n.params.push_back({"Input Channel", -1.0f, -1.0f, 31.0f});  // -1 = none, 0-31 = channel
    n.params.push_back({"Volume", 1.0f, 0.0f, 1.0f});
    n.params.push_back({"Pan", 0.0f, -1.0f, 1.0f});
    return n;
}

Node* NodeGraph::findNode(int id) {
    for (auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

Node& NodeGraph::createGroup(const std::string& name, Vec2 pos) {
    return addNode(name, NodeType::Group, {}, {}, pos);
}

// Is `ancestorId` somewhere in `nodeId`'s parent chain? Used to refuse
// parent/child links that would form a cycle. A `visited` set makes the walk
// terminate on any pre-existing corrupt cycle (each node is examined at most
// once), so there is no arbitrary cap on how deeply timelines/groups may nest.
bool NodeGraph::isAncestorOf(int ancestorId, int nodeId) {
    std::set<int> visited;
    int current = nodeId;
    while (current >= 0) {
        if (!visited.insert(current).second) break; // already seen -> cycle
        auto* n = findNode(current);
        if (!n) break;
        if (n->parentGroupId == ancestorId) return true;
        current = n->parentGroupId;
    }
    return false;
}

void NodeGraph::addToGroup(int groupId, int childId) {
    auto* group = findNode(groupId);
    auto* child = findNode(childId);
    if (!group || !child) return;
    // A parent may be a dedicated Group container OR a timeline track
    // (track-of-track parenting: a MIDI/Audio track can host children whose
    // start beat is relative to it).
    bool validParent = group->type == NodeType::Group
                     || group->type == NodeType::MidiTimeline
                     || group->type == NodeType::AudioTimeline;
    if (!validParent) return;
    // Never create a cycle: refuse self-parenting or parenting to one of this
    // node's own descendants.
    if (groupId == childId || isAncestorOf(childId, groupId)) return;
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
        // Time-gated effect layers live in the same local beat space as clips,
        // so they have to ripple too - otherwise inserting a bar leaves every
        // layer gating the wrong music.
        for (auto& region : node.effectRegions) {
            if (region.startBeat >= atBeat) {
                region.startBeat += duration;
                region.endBeat += duration;
            } else if (region.endBeat > atBeat) {
                region.endBeat += duration;   // straddles the insert: stretch it
            }
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
        for (auto& m : markers)
            if (m.beat >= atBeat) m.beat += duration;
    }
    // Markers just moved, so any child timeline anchored to a marker must have
    // its groupBeatOffset (and the cascading absoluteBeatOffset cache) re-read.
    // This is what makes anchored children ripple left/right when time is
    // inserted upstream. Safe to call in single-node scope too: it only rewrites
    // offsets for anchored nodes and recomputes the derived absolute cache.
    resolveAnchors();
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

        // Time-gated effect layers ripple with the music they gate. A layer
        // wholly inside the removed span disappears with it; one that straddles
        // or trails the span is shortened / pulled back.
        {
            auto& regs = node.effectRegions;
            regs.erase(std::remove_if(regs.begin(), regs.end(), [&](const EffectRegion& r) {
                return r.startBeat >= fromBeat && r.endBeat <= toBeat;
            }), regs.end());
            for (auto& r : regs) {
                auto pull = [&](float b) {
                    if (b <= fromBeat) return b;
                    if (b >= toBeat)   return b - duration;
                    return fromBeat;            // inside the removed span
                };
                r.startBeat = pull(r.startBeat);
                r.endBeat = std::max(r.startBeat, pull(r.endBeat));
            }
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
    // Markers just moved/were removed, so re-resolve any child timeline anchored
    // to a marker (this makes anchored children ripple left when time is cut
    // upstream) and refresh the cascading absoluteBeatOffset cache. If a child
    // was anchored to a marker that fell inside the deleted range, that marker
    // is gone, resolveMarkerBeat returns <0, and the child keeps its last offset.
    resolveAnchors();
    dirty = true;
}

float NodeGraph::getAbsoluteBeatOffset(int nodeId) {
    // Sum groupBeatOffset up the parent chain. A `visited` set stops the walk if
    // the chain is ever corrupt/cyclic (each node contributes at most once),
    // which removes any need for an arbitrary nesting-depth cap - timelines can
    // be nested arbitrarily deep.
    float total = 0;
    std::set<int> visited;
    int current = nodeId;
    while (current >= 0) {
        if (!visited.insert(current).second) break; // already seen -> cycle
        auto* node = findNode(current);
        if (!node) break;
        total += node->groupBeatOffset;
        current = node->parentGroupId;
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
    // (No reserve: `nodes` is a std::deque for stable element addresses - see
    // the declaration comment in node_graph.h. deque has no reserve(), and
    // needs none: it never reallocates existing elements.)

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

// ----------------------------------------------------------------------------
// Voice (polyphonic) factory presets. See the header for the contract. addNode
// reallocates graph.nodes, so we capture stable node/pin IDs right after each
// creation and NEVER hold a Node& across the next addNode (no-dangling-refs).
// ----------------------------------------------------------------------------
int buildVoicePreset(NodeGraph& graph, Vec2 pos, int preset) {
    struct Spec {
        const char* name; int poly; int steal; float glideMs;
        int unison; float detune; float spread;
    };
    // steal: 0=oldest 1=quietest 2=round-robin (matches Node::voiceStealMode)
    Spec spec;
    switch (preset) {
        case 1:  spec = {"Warm Pad",      8, 0,   0.0f, 3, 8.0f,  0.6f}; break;
        case 2:  spec = {"Pluck",         8, 0,   0.0f, 1, 12.0f, 0.5f}; break;
        case 3:  spec = {"Supersaw Lead", 1, 0,  50.0f, 7, 25.0f, 1.0f}; break;
        case 4:  spec = {"Noise Perc",    8, 0,   0.0f, 1, 12.0f, 0.5f}; break;
        default: spec = {"Voice",         8, 0,   0.0f, 1, 12.0f, 0.5f}; preset = 0; break;
    }

    const float px = pos.x, py = pos.y;

    // 1. The container itself (MIDI in -> Audio out), at the top level.
    int containerId;
    {
        auto& c = graph.addNode(spec.name, NodeType::VoiceContainer,
            {Pin{0, "MIDI", PinKind::Midi, true}},
            {Pin{0, "Audio", PinKind::Audio, false}}, {px, py});
        c.voicePolyphony     = spec.poly;
        c.voiceStealMode     = spec.steal;
        c.voiceGlideMs       = spec.glideMs;
        c.voiceUnison        = spec.unison;
        c.voiceUnisonDetune  = spec.detune;
        c.voiceUnisonSpread  = spec.spread;
        containerId = c.id;
    }

    // 2. Inner VoiceIn puck: per-note context source. Emits raw per-voice MIDI
    //    plus Pitch(Hz)/Gate(0/1)/Velocity(0..1) and the MPE expression signals
    //    Pressure(0..1)/Timbre(0..1).
    int viMidiPin, viPitchPin, viGatePin, viVelPin;
    {
        auto& vi = graph.addNode("Voice In", NodeType::VoiceIn, {},
            {Pin{0, "MIDI",     PinKind::Midi,   false},
             Pin{0, "Pitch",    PinKind::Signal, false, 1},
             Pin{0, "Gate",     PinKind::Signal, false, 1},
             Pin{0, "Velocity", PinKind::Signal, false, 1},
             Pin{0, "Pressure", PinKind::Signal, false, 1},
             Pin{0, "Timbre",   PinKind::Signal, false, 1}},
            {px - 240.0f, py + 170.0f});
        vi.voiceContainerId = containerId;
        if (vi.pinsOut.size() >= 6) {
            vi.pinsOut[1].tooltip = "Pitch (Hz): this voice's note frequency, including pitch bend.";
            vi.pinsOut[2].tooltip = "Gate (0/1): 1 while the key is held, 0 after release.";
            vi.pinsOut[3].tooltip = "Velocity (0..1): how hard this note was struck.";
            vi.pinsOut[4].tooltip = "Pressure (0..1): per-note pressure (MPE / aftertouch); 0 at rest.";
            vi.pinsOut[5].tooltip = "Timbre (0..1): per-note timbre slide (MPE CC74); 0.5 = centre.";
        }
        viMidiPin  = vi.pinsOut[0].id;
        viPitchPin = vi.pinsOut[1].id;
        viGatePin  = vi.pinsOut[2].id;
        viVelPin   = vi.pinsOut[3].id;
    }

    // 3. Inner VoiceOut sink: mapped to the inner graph's audio output.
    int voAudioInPin;
    {
        auto& vo = graph.addNode("Voice Out", NodeType::VoiceOut,
            {Pin{0, "Audio", PinKind::Audio, true}}, {},
            {px + 240.0f, py + 170.0f});
        vo.voiceContainerId = containerId;
        voAudioInPin = vo.pinsIn[0].id;
    }

    // 4. Preset-specific inner instrument chain, wired VoiceIn -> instrument ->
    //    VoiceOut. Each instrument carries voiceContainerId so it builds inside
    //    every voice clone (one oscillator/synth per simultaneous note).
    const Vec2 instPos{px, py + 170.0f};
    if (preset == 0) {
        // Basic: FM Synth, same defaults as the standalone FM Synth menu entry.
        int synthMidiInPin, synthAudioOutPin;
        {
            auto& s = graph.addNode("FM Synth", NodeType::Instrument,
                {Pin{0, "MIDI", PinKind::Midi, true}},
                {Pin{0, "Audio", PinKind::Audio, false}}, instPos);
            s.voiceContainerId = containerId;
            s.script = "__fmsynth__";
            s.params.push_back({"Algorithm", 0.0f, 0.0f, 7.0f});
            s.params.push_back({"Feedback",  0.3f, 0.0f, 1.0f});
            s.params.push_back({"Volume",    0.5f, 0.0f, 1.0f});
            for (int i = 1; i <= 4; ++i) {
                auto pp = "Op" + std::to_string(i) + " ";
                s.params.push_back({pp + "Ratio", (float)i, 0.1f, 16.0f});
                s.params.push_back({pp + "Level", i == 1 ? 1.0f : 0.5f, 0.0f, 1.0f});
            }
            ensureFmOpEnvelopes(s);
            synthMidiInPin   = s.pinsIn[0].id;
            synthAudioOutPin = s.pinsOut[0].id;
        }
        graph.addLink(viMidiPin, synthMidiInPin);
        graph.addLink(synthAudioOutPin, voAudioInPin);
    } else if (preset == 4) {
        // Noise Perc: gated noise burst with a short percussive envelope.
        int nGatePin, nVelPin, nAudioOutPin;
        {
            auto& n = graph.addNode("Signal Noise", NodeType::Instrument,
                {Pin{0, "Gate",     PinKind::Signal, true, 1},
                 Pin{0, "Velocity", PinKind::Signal, true, 1}},
                {Pin{0, "Audio", PinKind::Audio, false}}, instPos);
            n.voiceContainerId = containerId;
            n.script = "__signalnoise__";
            if (n.pinsIn.size() >= 2) {
                n.pinsIn[0].tooltip = "Gate (0/1): starts the note while >= 0.5, releases on the falling edge.";
                n.pinsIn[1].tooltip = "Velocity (0..1): how hard the note is struck; scales the envelope.";
            }
            n.params.push_back({"Type",   0.0f, 0.0f, 2.0f}); // 0=White 1=Pink 2=Brown
            n.params.push_back({"Volume", 0.5f, 0.0f, 1.0f});
            n.ahdsrEnvelope.attackMs  = 1.0f;
            n.ahdsrEnvelope.decayMs   = 130.0f;
            n.ahdsrEnvelope.sustain   = 0.0f;
            n.ahdsrEnvelope.releaseMs = 80.0f;
            nGatePin     = n.pinsIn[0].id;
            nVelPin      = n.pinsIn[1].id;
            nAudioOutPin = n.pinsOut[0].id;
        }
        graph.addLink(viGatePin, nGatePin);
        graph.addLink(viVelPin,  nVelPin);
        graph.addLink(nAudioOutPin, voAudioInPin);
    } else {
        // Warm Pad / Pluck / Supersaw Lead: a Signal Oscillator driven by the
        // voice's Pitch/Gate/Velocity, with a preset-tuned amplitude envelope.
        // waveform: 0=sine 1=saw 2=square 3=tri.
        float waveform = 1.0f, atk = 5.0f, dec = 100.0f, sus = 0.7f, rel = 300.0f;
        if (preset == 1) { waveform = 3.0f; atk = 400.0f; dec = 200.0f; sus = 0.85f; rel = 900.0f; } // Warm Pad: mellow triangle, slow swell
        else if (preset == 2) { waveform = 1.0f; atk = 2.0f; dec = 200.0f; sus = 0.0f; rel = 150.0f; } // Pluck: fast saw decay, no sustain
        else if (preset == 3) { waveform = 1.0f; atk = 8.0f; dec = 100.0f; sus = 0.9f; rel = 200.0f; } // Supersaw Lead: sustained saw

        int oPitchPin, oGatePin, oVelPin, oAudioOutPin;
        {
            auto& n = graph.addNode("Signal Osc", NodeType::Instrument,
                {Pin{0, "Pitch",    PinKind::Signal, true, 1},
                 Pin{0, "Gate",     PinKind::Signal, true, 1},
                 Pin{0, "Velocity", PinKind::Signal, true, 1}},
                {Pin{0, "Audio", PinKind::Audio, false}}, instPos);
            n.voiceContainerId = containerId;
            n.script = "__signalosc__";
            if (n.pinsIn.size() >= 3) {
                n.pinsIn[0].tooltip = "Pitch (Hz): oscillator frequency, read every sample.";
                n.pinsIn[1].tooltip = "Gate (0/1): starts the note while >= 0.5, releases on the falling edge.";
                n.pinsIn[2].tooltip = "Velocity (0..1): how hard the note is struck; scales the envelope.";
            }
            n.params.push_back({"Waveform", waveform, 0.0f, 4.0f}); // 0=sine 1=saw 2=square 3=tri 4=pulse
            n.params.push_back({"Volume",   0.5f, 0.0f, 1.0f});
            n.params.push_back({"Pulse Width", 0.5f, 0.05f, 0.95f, "%.2f"}); // only audible on Pulse; modulatable (#88)
            n.ahdsrEnvelope.attackMs  = atk;
            n.ahdsrEnvelope.decayMs   = dec;
            n.ahdsrEnvelope.sustain   = sus;
            n.ahdsrEnvelope.releaseMs = rel;
            oPitchPin    = n.pinsIn[0].id;
            oGatePin     = n.pinsIn[1].id;
            oVelPin      = n.pinsIn[2].id;
            oAudioOutPin = n.pinsOut[0].id;
        }
        graph.addLink(viPitchPin, oPitchPin);
        graph.addLink(viGatePin,  oGatePin);
        graph.addLink(viVelPin,   oVelPin);
        graph.addLink(oAudioOutPin, voAudioInPin);
    }

    return containerId;
}

// ---------------------------------------------------------------------------
// Wire / gate naming (see the declarations in node_graph.h)
// ---------------------------------------------------------------------------

juce::String NodeGraph::wireLabel(int linkId) const {
    static const juce::String arrow = juce::String::fromUTF8(" \xe2\x86\x92 ");

    // Resolve one link to (source node, source plug, dest node, dest plug).
    auto endpoints = [this](const Link& l, juce::String& sN, juce::String& sP,
                            juce::String& dN, juce::String& dP) {
        for (const auto& n : nodes) {
            for (const auto& p : n.pinsOut)
                if (p.id == l.startPin) { sN = n.name; sP = p.name; }
            for (const auto& p : n.pinsIn)
                if (p.id == l.endPin)   { dN = n.name; dP = p.name; }
        }
        return sN.isNotEmpty() && dN.isNotEmpty();
    };

    const Link* self = nullptr;
    for (const auto& l : links)
        if (l.id == linkId) { self = &l; break; }
    if (!self) return {};

    juce::String sN, sP, dN, dP;
    if (!endpoints(*self, sN, sP, dN, dP)) return {};
    const juce::String plain = sN + arrow + dN;

    // Qualify with plug names only when some OTHER wire produces the same plain
    // label - adding them unconditionally makes every entry noisy for no gain.
    for (const auto& l : links) {
        if (l.id == linkId) continue;
        juce::String oN, oP, oD, oDP;
        if (!endpoints(l, oN, oP, oD, oDP)) continue;
        if (oN + arrow + oD == plain)
            return plain + "  (" + sP + arrow + dP + ")";
    }
    return plain;
}

juce::String NodeGraph::gateLabel(const EffectRegion& r) const {
    if (r.groupId >= 0) {
        if (auto* g = findEffectGroup(r.groupId))
            return g->name.empty() ? ("Group #" + juce::String(g->id))
                                   : juce::String(g->name);
        return "(missing group)";
    }
    if (r.linkId >= 0) {
        auto s = wireLabel(r.linkId);
        return s.isEmpty() ? juce::String("(missing wire)") : s;
    }
    return "(unassigned)";
}

} // namespace SoundShop

#pragma once
#include "node_graph.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <algorithm>

namespace SoundShop {

// =============================================================================
// TrackNestingMenu  -  shared "make a track a child of another" context menu
// =============================================================================
//
// A timeline track can be nested under another track so its clips start at an
// offset within the parent (groupBeatOffset, cascaded into absoluteBeatOffset).
// Historically this was only reachable by right-clicking the *track-header
// strip inside the piano roll* - undiscoverable for anyone who never opened the
// editor and right-clicked exactly there. To fix discoverability the SAME menu
// is now offered from the node-graph node right-click too. Rather than copy the
// menu (which would drift), both call sites use these helpers.
//
// IDs are namespaced into a high range so they can be merged into a host menu
// that has its own ids without colliding. owns() tells the host whether a given
// result belongs to this submenu; handle() applies it.
namespace TrackNestingMenu {

// "Make child of <track>" entries are kMakeChildBase + candidate node id.
inline constexpr int kMakeChildBase = 90000;
inline constexpr int kClearParent   = 89001;
inline constexpr int kSetStartBeat  = 89002;

// True when this node type can participate in track nesting.
inline bool appliesTo(const Node& node) {
    return node.type == NodeType::MidiTimeline
        || node.type == NodeType::AudioTimeline
        || node.type == NodeType::Group;
}

// True when `result` came from this submenu (so the host can defer to handle()).
inline bool owns(int result) {
    return result == kClearParent
        || result == kSetStartBeat
        || result >= kMakeChildBase;
}

// Populate `menu` with the nesting items for `node`:
//   - "Make child of >"  submenu of every other timeline/group track. Tracks
//                        that would form a cycle (this track's own descendants)
//                        are shown DISABLED with the reason inline rather than
//                        hidden, so the user isn't left wondering why a track is
//                        missing from the list (the "grayed-out controls must
//                        explain themselves" rule).
//   - "Clear parent"     (enabled only when currently nested).
//   - "Set start beat..." (the child's offset, in beats, within its parent).
inline void addItems(juce::PopupMenu& menu, NodeGraph& graph, const Node& node) {
    const int myId = node.id;

    juce::PopupMenu parentSub;
    bool anyCandidate = false;
    for (auto& cand : graph.nodes) {
        if (cand.id == myId) continue;
        if (cand.type != NodeType::MidiTimeline && cand.type != NodeType::AudioTimeline
            && cand.type != NodeType::Group)
            continue;
        if (graph.isAncestorOf(myId, cand.id)) {
            // `cand` is already nested somewhere inside this track, so making
            // this track a child of `cand` would create a parent/child loop.
            // Show it disabled with the reason baked into the label - PopupMenu
            // items can't carry hover tooltips, so the explanation lives inline.
            // id -1 => not selectable (JUCE asserts on id 0); owns()/handle()
            // ignore it anyway since it's below kMakeChildBase.
            parentSub.addItem(-1, juce::String(cand.name)
                                  + "  \xe2\x80\x94 already inside this track",
                              /*enabled*/ false, /*ticked*/ false);
            anyCandidate = true;
            continue;
        }
        const bool isCurrentParent = (node.parentGroupId == cand.id);
        parentSub.addItem(kMakeChildBase + cand.id, juce::String(cand.name),
                          /*enabled*/ !isCurrentParent, /*ticked*/ isCurrentParent);
        anyCandidate = true;
    }
    if (!anyCandidate)
        parentSub.addItem(-1, "(no other tracks)", false, false);
    menu.addSubMenu("Make child of", parentSub);

    menu.addItem(kClearParent, "Clear parent", node.parentGroupId >= 0, false);
    menu.addSeparator();
    // fromUTF8 is required: juce::String(const char*) routes through
    // CharPointer_ASCII, which widens each byte to its own character, so a raw
    // UTF-8 literal renders as mojibake ("Set start beat" + three junk glyphs).
    menu.addItem(kSetStartBeat, juce::String::fromUTF8("Set start beat\xe2\x80\xa6"));
}

// Apply a chosen `result`. `refresh(desc)` is invoked after any data change so
// the caller can resolveAnchors / repaint / notify editors / commit the undo
// snapshot with its own description text. The "set start beat" item shows a
// modal text dialog (parented to `owner` for correct stacking/centering) and
// calls `refresh` only when the user confirms. Returns true if consumed.
inline bool handle(int result, NodeGraph& graph, int myId,
                   juce::Component* owner,
                   std::function<void(const char* desc)> refresh) {
    if (!owns(result)) return false;
    auto* n = graph.findNode(myId);
    if (!n) return true;

    if (result == kClearParent) {
        // Detach but keep the absolute start where it is so the track doesn't
        // visually jump (fold the inherited offset back into our own).
        if (n->parentGroupId >= 0) {
            const float inherited = n->absoluteBeatOffset - n->groupBeatOffset;
            graph.removeFromGroup(myId);
            n->anchorMarker.clear();
            n->groupBeatOffset = std::max(0.0f, n->groupBeatOffset + inherited);
            refresh("Clear track parent");
        }
    } else if (result == kSetStartBeat) {
        auto* aw = new juce::AlertWindow("Set Start Beat",
            "Start offset for \"" + juce::String(n->name) + "\" (in beats):",
            juce::MessageBoxIconType::NoIcon,
            owner);
        aw->addTextEditor("beat", juce::String(n->groupBeatOffset, 3));
        aw->addButton("Set", 1, juce::KeyPress(juce::KeyPress::returnKey));
        aw->addButton("Cancel", 0);
        aw->enterModalState(true, juce::ModalCallbackFunction::create(
            [aw, &graph, myId, refresh](int res) {
                if (res == 1) {
                    if (auto* nn = graph.findNode(myId)) {
                        const float v = aw->getTextEditorContents("beat").getFloatValue();
                        nn->groupBeatOffset = std::max(0.0f, v);
                        nn->anchorMarker.clear();
                        refresh("Set track start beat");
                    }
                }
                delete aw;
            }), true);
    } else if (result >= kMakeChildBase) {
        const int parentId = result - kMakeChildBase;
        if (parentId != myId && !graph.isAncestorOf(myId, parentId)) {
            graph.addToGroup(parentId, myId);
            refresh("Make track a child");
        }
    }
    return true;
}

} // namespace TrackNestingMenu
} // namespace SoundShop

#pragma once
#include "node_graph.h"
#include <set>

namespace SoundShop {

// Piano roll editor state (per-node)
struct PianoRollState {
    int scrollPitch = 60;   // center pitch
    int visibleRange = 36;  // 3 octaves
    float snap = 0.25f;     // 16th note
    float hZoom = 1.0f;     // horizontal zoom (1.0 = show full timeline)
    float hScroll = 0.0f;   // horizontal scroll (0.0 = start, in beats)

    // Dragging
    struct DragInfo {
        int clipIdx = -1;
        int noteIdx = -1;
        enum Mode { None, Move, ResizeLeft, ResizeRight, Detune } mode = None;
        float startBeat = 0;
        int startPitch = 0;
    } drag;

    // Selection
    std::set<std::pair<int, int>> selected; // (clip_idx, note_idx)

    // Box selection / empty click
    struct BoxSelect {
        bool active = false;
        float startBeat = 0, startPitch = 0;
        float endBeat = 0, endPitch = 0;
    } box;
    float emptyClickBeat = 0;
    int emptyClickPitch = 0;

    // Key/mode/scale — each stores its own last-selected value
    int keyRoot = 0;
    std::string activeCategory = "key"; // which one is active: "key", "mode", "scale"
    std::string keyName = "Major";
    std::string modeName = "Ionian";
    std::string scaleName = "Chromatic";
    // Helper to get the active name
    const std::string& activeName() const {
        if (activeCategory == "key") return keyName;
        if (activeCategory == "mode") return modeName;
        return scaleName;
    }

    // Compact mode — hides toolbar, everything via context menus
    bool compactMode = false;

    // Focused clip (for per-clip key display and editing)
    int focusedClipIdx = 0;

    // Key detection results
    bool showDetectResults = false;
    std::vector<MusicTheory::KeyMatch> detectResults;

    // Audition — keys being previewed by clicking the piano column
    struct AuditionNote {
        int pitch = -1;
        double startTime = 0;   // ImGui time when pressed
        double duration = 0.5;  // seconds
        bool noteOnSent = false;
        bool noteOffSent = false;
    };
    std::vector<AuditionNote> auditionNotes;
};

// TODO: port full piano roll drawing and interaction from Python prototype
// This will be the most complex UI component

} // namespace SoundShop

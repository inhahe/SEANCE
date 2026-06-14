#pragma once
#include "node_graph.h"
#include <set>

namespace SoundShop {

// Piano roll editor state (per-node)
struct PianoRollState {
    int scrollPitch = 60;   // center pitch (C4)
    // 18 semitones (1.5 octaves) is a comfortable starting view: at typical
    // editor heights each row is ~25-30px, so note labels are readable and
    // notes are easy to click without surgical precision. The user can zoom
    // out for more range or use Fit All to auto-fit existing notes.
    int visibleRange = 18;
    float snap = 0.25f;     // 16th note
    float hZoom = 0.4f;     // horizontal zoom (<1.0 = zoomed out past timeline, showing room to expand)
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

    // Scale selection for piano-roll snapping / "in key" highlighting / degree
    // analysis. Two regimes, tracked by activeCategory:
    //   "keymode" - Root + Key (parent scale) + Mode (rotation degree of the
    //               parent). keyName and modeName BOTH matter; the active
    //               interval set is rotateScale(keys()[keyName], mode index).
    //   "scale"   - Root + a fixed non-diatonic set named by scaleName; Key and
    //               Mode are ignored (and greyed in the UI).
    // The actual interval set is computed by MusicTheory::activeIntervals().
    int keyRoot = 0;
    std::string activeCategory = "keymode"; // "keymode" or "scale"
    std::string keyName = "Major";
    std::string modeName = "Ionian (Major)";
    std::string scaleName = "Chromatic";

    // Compact mode - hides toolbar, everything via context menus
    bool compactMode = false;

    // Focused clip (for per-clip key display and editing)
    int focusedClipIdx = 0;

    // Key detection results
    bool showDetectResults = false;
    std::vector<MusicTheory::KeyMatch> detectResults;

    // Audition - keys being previewed by clicking the piano column
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

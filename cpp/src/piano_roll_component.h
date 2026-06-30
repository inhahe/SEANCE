#pragma once
#include "node_graph.h"
#include "piano_roll.h"
#include "transport.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace SoundShop {

class PianoRollComponent : public juce::Component,
                            public juce::ScrollBar::Listener,
                            public juce::TooltipClient {
public:
    PianoRollComponent(NodeGraph& graph, Node& node, Transport* transport = nullptr);
    ~PianoRollComponent() override {
        rootCombo.setLookAndFeel(nullptr);
        keyCombo.setLookAndFeel(nullptr);
        modeCombo.setLookAndFeel(nullptr);
        scaleCombo.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

    bool compactMode = false;

    // Trigger piano roll actions from external hotkeys
    void triggerAction(const std::string& action);
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseEnter(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    bool keyPressed(const juce::KeyPress& key) override;

    // Hover popup: when the mouse rests over a note in the grid, show every
    // note under the cursor (name, scale degree, velocity, start beat,
    // duration, detune). Multiple stacked/overlapping notes are all listed.
    // Returns empty when not over a note so no tooltip appears. Implemented
    // for juce::TooltipClient; the shared TooltipWindow lives on
    // MainContentComponent.
    juce::String getTooltip() override;

    // Refresh the node pointer from the graph. Call at the start of every
    // public entry point (paint, mouseDown, etc.) because graph.nodes can
    // reallocate when nodes are added, invalidating old pointers.
    void refreshNode() { node = graph.findNode(nodeId); }

    // Wired by MainContentComponent: invoked when the user drags the
    // resize handle at the top of this panel. `deltaPx` is the cursor's
    // vertical delta since the last callback (positive = handle moved
    // down). The host adjusts this panel's heightPx by -deltaPx (drag UP
    // = grow) and re-lays-out the editor stack; panels above just shift.
    std::function<void(int deltaPx)> onResizeDrag;

    // Height of the drag strip at the very top of the panel. Visible as
    // a thin grip bar; mouseDown there starts the resize gesture and the
    // event is consumed before reaching the toolbar. Exposed publicly so
    // toolbarHeight() can shift the toolbar down by this amount.
    static constexpr int RESIZE_HANDLE_H = 6;

    // Per-track header strip: a horizontal band just above the note grid that
    // shows the track as a clip-block sitting at its start beat. Drag it
    // left/right to set this track's time offset; right-click it to set/clear
    // its parent track. Folded into toolbarHeight() so every grid offset in
    // paint()/mouseDown() accounts for it automatically.
    static constexpr int TRACK_HEADER_H = 20;

    // Wired by MainContentComponent: invoked after this track's time offset or
    // parent changes, so the host can repaint ALL editor panels (a parent's
    // move shifts its children, which live in other panels) and the node graph.
    std::function<void()> onTimingChanged;

private:
    NodeGraph& graph;
    int nodeId;
    Node* node = nullptr;   // refreshed via refreshNode(); never cache across calls
    Transport* transport = nullptr;
    PianoRollState state;

    static constexpr float KEY_WIDTH = 40.0f;

    // Expression / automation lane
    enum ExprLane { ExprNone, ExprVelocity, ExprPitchBend, ExprSlide, ExprPressure, ExprAutomation };
    ExprLane exprLane = ExprNone;
    static constexpr float EXPR_LANE_HEIGHT = 80.0f;
    juce::ComboBox exprLaneCombo;  // select which MPE expression curve to view/edit
    juce::ComboBox autoParamCombo; // select which parameter's automation to view/edit
    int autoParamIndex = -1;       // index into node.params, -1 = none

    // Drag state
    enum DragModeEnum { DragNone, DragNote, DragResizeLeft, DragResizeRight, DragBox, DragExprPoint, DragSongEnd,
                        DragLoopStart, DragLoopEnd, DragTrackOffset };
    DragModeEnum dragMode = DragNone;
    // Grab tolerance (px) for the A-B loop region's start/end boundary lines.
    static constexpr float LOOP_EDGE_GRAB_PX = 5.0f;
    int dragNoteCI = -1, dragNoteNI = -1;

    // Song-end resize handle. A draggable orange border is drawn at the end of
    // this timeline's content (the rightmost clip's end). Dragging it left
    // shortens the song - trimming any notes past the new end on release -
    // while dragging right adds empty beats. songEndDragVisBeats/Scroll capture
    // the horizontal mapping at gesture start so live resizing stays smooth even
    // as the derived timeline length (which feeds the cursor->beat conversion)
    // changes underneath the drag.
    int   songEndDragClipIdx = -1;
    float songEndDragVisBeats = 0;
    float songEndDragScroll = 0;
    // Node-local end beat of the rightmost clip (0 and clipIdx=-1 if no clips).
    float songEndBeatLocal(int* clipIdxOut = nullptr) const;
    // Node-local beat -> on-screen x (mirrors the beatToX used in paint).
    float beatToScreenX(float beatLocal) const;
    static constexpr float SONG_END_GRAB_PX = 5.0f;
    // Track-offset drag (header strip). Captures the horizontal mapping and the
    // node's own groupBeatOffset at gesture start so live dragging stays stable
    // even as the derived timeline length (which feeds the beat<->x mapping)
    // changes underneath the drag.
    float trackOffsetStartOffset = 0;  // node.groupBeatOffset at mouseDown
    float trackOffsetDownAbsBeat = 0;  // absolute beat under cursor at mouseDown
    float trackOffsetDragVisBeats = 0; // captured visibleBeats
    float trackOffsetDragScroll = 0;   // captured scrollBeat
    // Map a panel x (absolute component coords) to an absolute beat using the
    // current (or, mid-drag, captured) horizontal mapping. capturedVisBeats<=0
    // means "compute the mapping live from node/state".
    float panelXToAbsBeat(float x, float capturedVisBeats = -1.0f, float capturedScroll = 0.0f) const;
    bool isInTrackHeader(juce::Point<float> pos) const; // pos in component coords
    void showTrackHeaderMenu();
    void paintTrackHeader(juce::Graphics& g);

    float dragStartBeat = 0;
    int dragStartPitch = 0;
    juce::Point<float> dragStartScreen, dragCurrentScreen;
    float lastClickBeat = 0;
    int lastClickPitch = 60;

    // Live paste target. Tracks the grid cell under the cursor (updated in
    // mouseMove) so Ctrl+V pastes where the mouse is pointing and paint() can
    // draw a ghost preview of the clipboard there. hoverValid is false when the
    // cursor is off the grid (over toolbar / keys / scrollbars / outside the
    // component) so the ghost only appears over editable space.
    float hoverBeat = 0;
    int hoverPitch = 60;
    bool hoverValid = false;

    // Deferred right-click context menu. Showing a juce::PopupMenu synchronously
    // from mouseDown while the right button is still held lets the button-release
    // land on (and auto-select) whatever item sits under the cursor - which is
    // the first item, "Place Note Here", so a plain right-click dropped a stray
    // note. We arm the menu on right mouseDown and actually show it on mouseUp,
    // when no button is held, so selecting an item requires a deliberate click.
    // (This also matches the Windows convention of opening context menus on
    // right-button release.)
    bool rightClickArmed = false;
    juce::Point<float> rightClickDownPos;

    // Marquee (box) selection. The selection is recomputed live on every drag
    // tick so notes highlight as the rectangle sweeps over them and unhighlight
    // when they fall back out. marqueeBase is the selection that existed when the
    // drag began (empty for a plain drag, the prior selection for Shift+drag) so
    // each tick can rebuild selection = base + notes-currently-in-box.
    std::set<std::pair<int, int>> marqueeBase;
    void updateMarqueeSelection();

    // Undo snapshots for drag operations
    struct NoteSnapshot {
        int ci, ni;
        float offset, duration, detune;
        int pitch, velocity;
    };
    std::vector<NoteSnapshot> dragBeforeSnapshot;
    void captureSelectedSnapshot(std::vector<NoteSnapshot>& snap);
    void pushDragUndo(const std::string& desc, const std::vector<NoteSnapshot>& before);

    // Expression editing state
    int exprDragCI = -1, exprDragNI = -1, exprDragPtIdx = -1;
    bool isInExprLane(juce::Point<float> pos) const;
    std::pair<float, float> screenToExprBeatValue(juce::Point<float> pos) const;
    std::vector<ExpressionPoint>* getExprCurve(MidiNote& note);
    const std::vector<ExpressionPoint>* getExprCurveConst(const MidiNote& note) const;

    // Close callback
public:
    std::function<void(int)> onClose; // called with node.id

private:
    // Helpers
    std::pair<float, int> screenToBeatPitch(juce::Point<float> pos) const;

    struct NoteHit {
        int ci = -1, ni = -1;
        enum Edge { Body, Left, Right } edge = Body;
        bool valid() const { return ci >= 0; }
    };
    NoteHit findNoteAt(juce::Point<float> screenPos) const;
    void showNoteMenu();
    void showEmptyMenu();

    // Clipboard (static - shared across all piano roll instances).
    // Notes are stored relative to the copied block's TOP-LEFT corner so paste
    // can re-anchor the whole block to the paste point (the click / marquee-drag
    // origin): offsetFromFirst is beats from the earliest note, pitchBelowTop is
    // semitones below the highest note. On paste the top-left note lands at the
    // anchor and the rest keep their relative positions.
    struct ClipboardNote {
        float offsetFromFirst; // beats from the earliest copied note (>= 0)
        int   pitchBelowTop;   // semitones below the highest copied note (>= 0)
        float duration;
        int velocity;
        float detune;
        NoteExpression expression;
    };
    static std::vector<ClipboardNote> clipboard;

    // Clip-level clipboard (static - shared across all piano rolls)
    static std::unique_ptr<Clip> clipClipboard;
    void copyClipAtCursor();
    void pasteClipAtCursor();

    void copySelected();
    void cutSelected();
    void pasteAtCursor();
    void deleteSelected();
    void selectAll();
    void zoomToSelection();
    void scrollBarMoved(juce::ScrollBar* bar, double newRangeStart) override;
    void updateScrollBars();

    // Scrollbars
    juce::ScrollBar hScrollBar{false}; // horizontal
    juce::ScrollBar vScrollBar{true};  // vertical (pitch)
    juce::Slider hZoomSlider;
    // Vertical zoom (rows-per-octave). Backed by state.visibleRange:
    // smaller visibleRange = fewer rows = taller note lanes. Slider runs
    // 12..120 semitones; we invert the display so dragging right = thicker
    // rows (matches the user's mental model of "zoom in"). Ctrl+Shift+
    // scroll on the grid also drives this.
    juce::Slider vZoomSlider;
    static constexpr int SCROLLBAR_SIZE = 20;

    // Audition tracking
    std::map<int, double> auditionKeys; // pitch -> time started

    // Toolbar
    juce::TextButton compactBtn{"--"}, closeBtn{"X"};
    juce::TextButton transpUpOctBtn{"+Octave"}, transpDownOctBtn{"-Octave"};
    juce::TextButton transpUpSemiBtn{"+Semitone"}, transpDownSemiBtn{"-Semitone"};
    juce::TextButton timeLeftBtn{"Nudge Left"}, timeRightBtn{"Nudge Right"};
    juce::TextButton selectAllBtn{"Select All"}, deselectBtn{"Deselect"};
    juce::TextButton dblDurBtn{"x2 Duration"}, halfDurBtn{"/2 Duration"};
    juce::TextButton reverseBtn{"Reverse"};
    juce::TextButton detuneResetBtn{"Reset"};
    juce::Slider detuneSlider;
    juce::Label detuneLbl;
    juce::TextButton quantizeBtn{"Quantize"};
    juce::Slider quantizeStrSlider;
    juce::Label  quantizeStrLbl;
    juce::TextButton snap14Btn{"1/4"}, snap12Btn{"1/2"}, snap1Btn{"1"}, snapOffBtn{"Off"};
    juce::TextButton snapScaleBtn{"Snap to Scale"}, detectKeyBtn{"Detect Key"};
    juce::ComboBox rootCombo, keyCombo, modeCombo, scaleCombo;
    juce::Label rootLbl, keyLbl, modeLbl, scaleLbl;
    // Push state.{keyRoot,keyName,modeName,scaleName,activeCategory} into the
    // Root/Key/Mode/Scale dropdowns and refresh their enabled/tooltip state.
    // Call after any code path that changes the scale selection WITHOUT going
    // through a combo's own onChange (context menus, key detection).
    void syncScaleUI();
    juce::Label titleLabel, helpLabel;
    juce::TextButton muteBtn{"Mute"}, soloBtn{"Solo"};
    juce::Slider panSlider;
    juce::Label panLbl;
    // toolbarHeight includes the RESIZE_HANDLE_H strip at the very top
    // so every "below toolbar" offset in paint() / mouseDown() / etc.
    // automatically accounts for the handle. The toolbar buttons are
    // laid out with a matching removeFromTop(RESIZE_HANDLE_H) at the
    // start of resized() so they sit below the handle, not under it.
    int toolbarHeight() const { return (compactMode ? 28 : 82) + RESIZE_HANDLE_H + TRACK_HEADER_H; }
    // Top y of the track-header strip (it occupies the bottom TRACK_HEADER_H of
    // the toolbar region, directly above the note grid).
    int trackHeaderTop() const { return toolbarHeight() - TRACK_HEADER_H; }

    // Resize-handle gesture state. mouseDown in the top RESIZE_HANDLE_H
    // strip sets resizingHeight=true; subsequent mouseDrag events fire
    // onResizeDrag with the per-frame deltaY. Reset on mouseUp.
    bool resizingHeight = false;
    int  resizeLastY = 0;

    // Custom LookAndFeel for smaller combo box fonts
    struct SmallComboLookAndFeel : public juce::LookAndFeel_V4 {
        juce::Font getComboBoxFont(juce::ComboBox&) override { return juce::Font(15.0f); }
        juce::Font getPopupMenuFont() override { return juce::Font(15.0f); }
    };
    SmallComboLookAndFeel smallComboLF;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollComponent)
};

} // namespace SoundShop

#pragma once
#include "node_graph.h"
#include "piano_roll.h"
#include "transport.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace SoundShop {

class PianoRollComponent : public juce::Component,
                            public juce::ScrollBar::Listener {
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
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    NodeGraph& graph;
    Node& node;
    Transport* transport = nullptr;
    PianoRollState state;

    static constexpr float KEY_WIDTH = 40.0f;

    // Expression / automation lane
    enum ExprLane { ExprNone, ExprVelocity, ExprPitchBend, ExprSlide, ExprPressure, ExprAutomation };
    ExprLane exprLane = ExprNone;
    static constexpr float EXPR_LANE_HEIGHT = 80.0f;
    juce::TextButton exprVelBtn{"Vel"}, exprPBBtn{"PB"}, exprSlideBtn{"Slide"}, exprPressBtn{"Press"}, exprOffBtn{"None"};
    juce::ComboBox autoParamCombo; // select which parameter's automation to view/edit
    int autoParamIndex = -1;       // index into node.params, -1 = none

    // Drag state
    enum DragModeEnum { DragNone, DragNote, DragResizeLeft, DragResizeRight, DragBox, DragExprPoint };
    DragModeEnum dragMode = DragNone;
    int dragNoteCI = -1, dragNoteNI = -1;
    float dragStartBeat = 0;
    int dragStartPitch = 0;
    juce::Point<float> dragStartScreen, dragCurrentScreen;
    float lastClickBeat = 0;
    int lastClickPitch = 60;

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

    // Clipboard (static — shared across all piano roll instances)
    struct ClipboardNote {
        float offsetFromFirst; // beat offset relative to the earliest copied note
        int pitch;
        float duration;
        int velocity;
        float detune;
        NoteExpression expression;
    };
    static std::vector<ClipboardNote> clipboard;

    // Clip-level clipboard (static — shared across all piano rolls)
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
    static constexpr int SCROLLBAR_SIZE = 14;

    // Audition tracking
    std::map<int, double> auditionKeys; // pitch → time started

    // Toolbar
    juce::TextButton compactBtn{"--"}, closeBtn{"X"};
    juce::TextButton transpUpOctBtn{"+Octave"}, transpDownOctBtn{"-Octave"};
    juce::TextButton transpUpSemiBtn{"+Semitone"}, transpDownSemiBtn{"-Semitone"};
    juce::TextButton timeLeftBtn{"<< Time"}, timeRightBtn{"Time >>"};
    juce::TextButton selectAllBtn{"All"}, deselectBtn{"None"};
    juce::TextButton dblDurBtn{"x2 Duration"}, halfDurBtn{"/2 Duration"};
    juce::TextButton reverseBtn{"Reverse"};
    juce::TextButton detuneResetBtn{"Reset"};
    juce::Slider detuneSlider;
    juce::Label detuneLbl;
    juce::TextButton snap14Btn{"1/4"}, snap12Btn{"1/2"}, snap1Btn{"1"}, snapOffBtn{"Off"};
    juce::TextButton snapScaleBtn{"Snap to Scale"}, detectKeyBtn{"Detect Key"};
    juce::ComboBox rootCombo, keyCombo, modeCombo, scaleCombo;
    juce::Label rootLbl, keyLbl, modeLbl, scaleLbl;
    juce::Label titleLabel, helpLabel;
    juce::TextButton muteBtn{"M"}, soloBtn{"S"};
    juce::Slider panSlider;
    juce::Label panLbl;
    int toolbarHeight() const { return compactMode ? 28 : 82; }

    // Custom LookAndFeel for smaller combo box fonts
    struct SmallComboLookAndFeel : public juce::LookAndFeel_V4 {
        juce::Font getComboBoxFont(juce::ComboBox&) override { return juce::Font(15.0f); }
        juce::Font getPopupMenuFont() override { return juce::Font(15.0f); }
    };
    SmallComboLookAndFeel smallComboLF;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollComponent)
};

} // namespace SoundShop

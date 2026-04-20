#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <map>
#include <string>
#include <functional>
#include <vector>

namespace SoundShop {

// All actions that can be assigned a hotkey.
enum class HotkeyAction {
    // Transport
    Play, Stop, Record, ToggleLoop, ToggleMetronome,
    // File
    NewProject, OpenProject, SaveProject, SaveProjectAs, ExportAudio,
    // Edit
    Undo, Redo, DeleteSelected, SelectAll,
    // View
    FitAll, ZoomIn, ZoomOut,
    // Node
    MuteSelected, SoloSelected,
    // Automation
    WriteAutoToSelection, ArmAllParams, DisarmAllParams,
    // Tools
    Capture, ToggleKeyboardMidi, OpenXYPad,
    // Tracks
    AddMidiTrack, AddAudioTrack,
    // Piano roll
    TransposeUpSemi, TransposeDownSemi, TransposeUpOctave, TransposeDownOctave,
    NudgeLeft, NudgeRight, DoubleDuration, HalveDuration, ReverseNotes,
    // System
    AssignHotkeys, // the assign-hotkeys dialog itself

    COUNT // fixed action count; dynamic node actions stored separately
};

// Dynamic per-node action: references a specific node by name.
// If the node is renamed or deleted, the action goes inert (does nothing).
enum class NodeActionType {
    ToggleMute,
    ToggleSolo,
    OpenEditor,
};

// A binding: either a keyboard key combo or a MIDI message.
struct HotkeyBinding {
    // Keyboard binding
    int keyCode = 0;       // JUCE key code (0 = unbound)
    int modifiers = 0;     // JUCE modifier flags (ctrl, shift, alt)

    // MIDI binding (alternative to keyboard)
    bool isMidi = false;
    int midiType = 0;      // 0=note, 1=CC
    int midiChannel = 0;   // 0-15
    int midiNumber = 0;    // note number or CC number

    bool isUnbound() const { return keyCode == 0 && !isMidi; }

    juce::String toString() const {
        if (isUnbound()) return "(none)";
        if (isMidi) {
            if (midiType == 0) return "MIDI Note " + juce::String(midiNumber);
            return "MIDI CC#" + juce::String(midiNumber);
        }
        juce::String s;
        if (modifiers & juce::ModifierKeys::ctrlModifier) s += "Ctrl+";
        if (modifiers & juce::ModifierKeys::shiftModifier) s += "Shift+";
        if (modifiers & juce::ModifierKeys::altModifier) s += "Alt+";
        if (keyCode >= 'A' && keyCode <= 'Z')
            s += (char)keyCode;
        else if (keyCode >= juce::KeyPress::F1Key && keyCode <= juce::KeyPress::F12Key)
            s += "F" + juce::String(keyCode - juce::KeyPress::F1Key + 1);
        else if (keyCode == juce::KeyPress::spaceKey) s += "Space";
        else if (keyCode == juce::KeyPress::returnKey) s += "Enter";
        else if (keyCode == juce::KeyPress::escapeKey) s += "Escape";
        else if (keyCode == juce::KeyPress::deleteKey) s += "Delete";
        else if (keyCode == juce::KeyPress::upKey) s += "Up";
        else if (keyCode == juce::KeyPress::downKey) s += "Down";
        else if (keyCode == juce::KeyPress::leftKey) s += "Left";
        else if (keyCode == juce::KeyPress::rightKey) s += "Right";
        else s += juce::String::charToString((juce::juce_wchar)keyCode);
        return s;
    }

    bool matchesKey(const juce::KeyPress& key) const {
        if (isMidi || keyCode == 0) return false;
        return key.getKeyCode() == keyCode &&
               (key.getModifiers().getRawFlags() & 0x7) == (modifiers & 0x7);
    }

    bool matchesMidi(int type, int channel, int number) const {
        if (!isMidi) return false;
        // Match type (note/CC) and number. Channel is ignored for matching
        // so a binding works regardless of which MIDI channel the controller
        // sends on (many controllers default to different channels).
        return midiType == type && midiNumber == number;
    }
};

struct DynamicNodeBinding {
    std::string nodeName;       // matches Node::name
    NodeActionType actionType;
    HotkeyBinding binding;

    juce::String displayName() const {
        const char* typeStr = (actionType == NodeActionType::ToggleMute) ? "Mute"
                            : (actionType == NodeActionType::ToggleSolo) ? "Solo"
                            : "Open";
        return juce::String(typeStr) + ": " + juce::String(nodeName);
    }
};

class HotkeyManager {
public:
    HotkeyManager();

    // Get/set binding for an action
    HotkeyBinding getBinding(HotkeyAction action) const;
    void setBinding(HotkeyAction action, const HotkeyBinding& binding);
    void clearBinding(HotkeyAction action);

    // Lookup: find which action a key press or MIDI message triggers
    HotkeyAction findActionForKey(const juce::KeyPress& key) const;
    HotkeyAction findActionForMidi(int type, int channel, int number) const;

    // Action metadata
    static const char* actionName(HotkeyAction action);
    static const char* actionCategory(HotkeyAction action);

    // Save/load to JSON file
    void saveToFile(const juce::File& file) const;
    void loadFromFile(const juce::File& file);
    void resetToDefaults();

    // Register action callbacks (called by MainContentComponent)
    void setCallback(HotkeyAction action, std::function<void()> cb) { callbacks[(int)action] = std::move(cb); }
    bool executeAction(HotkeyAction action);

    // Dynamic per-node bindings
    std::vector<DynamicNodeBinding> nodeBindings;
    void addNodeBinding(const std::string& nodeName, NodeActionType type, const HotkeyBinding& binding);
    void removeNodeBinding(int index);
    DynamicNodeBinding* findNodeBindingForKey(const juce::KeyPress& key);
    DynamicNodeBinding* findNodeBindingForMidi(int type, int channel, int number);

    // Callback for dynamic node actions (set by MainContentComponent)
    std::function<void(const std::string& nodeName, NodeActionType type)> onNodeAction;

private:
    HotkeyBinding bindings[(int)HotkeyAction::COUNT];
    std::function<void()> callbacks[(int)HotkeyAction::COUNT];

    void setDefaults();
};

// Settings dialog for viewing/editing hotkey assignments.
class NodeGraph; // forward declare for node listing

class HotkeySettingsComponent : public juce::Component {
public:
    HotkeySettingsComponent(HotkeyManager& manager, NodeGraph* graph = nullptr);

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

    // Called by AudioEngine when MIDI is received during capture mode
    void onMidiReceived(int type, int channel, int number);

private:
    HotkeyManager& manager;

    juce::ListBox listBox;
    struct ActionListModel : public juce::ListBoxModel {
        HotkeyManager& mgr;
        int captureIdx = -1; // which action is being captured (-1 = none)
        HotkeySettingsComponent* owner = nullptr;

        ActionListModel(HotkeyManager& m) : mgr(m) {}
        int getNumRows() override { return (int)HotkeyAction::COUNT + (int)mgr.nodeBindings.size(); }
        void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override;
        void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    };
    ActionListModel model;

    NodeGraph* graph = nullptr;
    juce::TextButton addNodeActionBtn{"+ Add Node Action..."};
    juce::TextButton resetBtn{"Reset to Defaults"};
    juce::TextButton closeBtn{"Close"};
    juce::Label captureLabel;
};

} // namespace SoundShop

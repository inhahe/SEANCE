#include "hotkey_manager.h"
#include "node_graph.h"
#include <fstream>

namespace SoundShop {

// ==============================================================================
// Action metadata
// ==============================================================================

const char* HotkeyManager::actionName(HotkeyAction a) {
    switch (a) {
        case HotkeyAction::Play:                return "Play";
        case HotkeyAction::Stop:                return "Stop";
        case HotkeyAction::Record:              return "Record";
        case HotkeyAction::ToggleLoop:          return "Toggle Loop";
        case HotkeyAction::ToggleMetronome:     return "Toggle Metronome";
        case HotkeyAction::NewProject:          return "New Project";
        case HotkeyAction::OpenProject:         return "Open Project";
        case HotkeyAction::SaveProject:         return "Save Project";
        case HotkeyAction::SaveProjectAs:       return "Save Project As";
        case HotkeyAction::ExportAudio:         return "Export Audio";
        case HotkeyAction::Undo:                return "Undo";
        case HotkeyAction::Redo:                return "Redo";
        case HotkeyAction::DeleteSelected:      return "Delete Selected";
        case HotkeyAction::SelectAll:           return "Select All";
        case HotkeyAction::FitAll:              return "Fit All";
        case HotkeyAction::ZoomIn:              return "Zoom In";
        case HotkeyAction::ZoomOut:             return "Zoom Out";
        case HotkeyAction::MuteSelected:        return "Mute Selected";
        case HotkeyAction::SoloSelected:        return "Solo Selected";
        case HotkeyAction::WriteAutoToSelection:return "Write Automation to Selection";
        case HotkeyAction::ArmAllParams:        return "Arm All Params";
        case HotkeyAction::DisarmAllParams:     return "Disarm All Params";
        case HotkeyAction::Capture:             return "Capture / Bounce";
        case HotkeyAction::ToggleKeyboardMidi:  return "Toggle Keyboard MIDI";
        case HotkeyAction::OpenXYPad:           return "Open XY Pad";
        case HotkeyAction::AddMidiTrack:        return "Add MIDI Track";
        case HotkeyAction::AddAudioTrack:       return "Add Audio Track";
        case HotkeyAction::TransposeUpSemi:     return "Transpose Up Semitone";
        case HotkeyAction::TransposeDownSemi:   return "Transpose Down Semitone";
        case HotkeyAction::TransposeUpOctave:   return "Transpose Up Octave";
        case HotkeyAction::TransposeDownOctave: return "Transpose Down Octave";
        case HotkeyAction::NudgeLeft:           return "Nudge Notes Left";
        case HotkeyAction::NudgeRight:          return "Nudge Notes Right";
        case HotkeyAction::DoubleDuration:      return "Double Note Duration";
        case HotkeyAction::HalveDuration:       return "Halve Note Duration";
        case HotkeyAction::ReverseNotes:        return "Reverse Notes";
        case HotkeyAction::AssignHotkeys:       return "Assign Hotkeys";
        default: return "Unknown";
    }
}

const char* HotkeyManager::actionCategory(HotkeyAction a) {
    switch (a) {
        case HotkeyAction::Play: case HotkeyAction::Stop: case HotkeyAction::Record:
        case HotkeyAction::ToggleLoop: case HotkeyAction::ToggleMetronome:
            return "Transport";
        case HotkeyAction::NewProject: case HotkeyAction::OpenProject:
        case HotkeyAction::SaveProject: case HotkeyAction::SaveProjectAs:
        case HotkeyAction::ExportAudio:
            return "File";
        case HotkeyAction::Undo: case HotkeyAction::Redo:
        case HotkeyAction::DeleteSelected: case HotkeyAction::SelectAll:
            return "Edit";
        case HotkeyAction::FitAll: case HotkeyAction::ZoomIn: case HotkeyAction::ZoomOut:
            return "View";
        case HotkeyAction::MuteSelected: case HotkeyAction::SoloSelected:
            return "Node";
        case HotkeyAction::AddMidiTrack: case HotkeyAction::AddAudioTrack:
            return "Tracks";
        case HotkeyAction::WriteAutoToSelection: case HotkeyAction::ArmAllParams:
        case HotkeyAction::DisarmAllParams:
            return "Automation";
        case HotkeyAction::Capture: case HotkeyAction::ToggleKeyboardMidi:
        case HotkeyAction::OpenXYPad:
            return "Tools";
        case HotkeyAction::TransposeUpSemi: case HotkeyAction::TransposeDownSemi:
        case HotkeyAction::TransposeUpOctave: case HotkeyAction::TransposeDownOctave:
        case HotkeyAction::NudgeLeft: case HotkeyAction::NudgeRight:
        case HotkeyAction::DoubleDuration: case HotkeyAction::HalveDuration:
        case HotkeyAction::ReverseNotes:
            return "Piano Roll";
        case HotkeyAction::AssignHotkeys:
            return "System";
        default: return "Other";
    }
}

// ==============================================================================
// HotkeyManager
// ==============================================================================

HotkeyManager::HotkeyManager() { setDefaults(); }

void HotkeyManager::setDefaults() {
    for (auto& b : bindings) b = {};
    // Set some sensible defaults
    bindings[(int)HotkeyAction::Play]       = {juce::KeyPress::spaceKey, 0};
    bindings[(int)HotkeyAction::Undo]       = {'Z', juce::ModifierKeys::ctrlModifier};
    bindings[(int)HotkeyAction::Redo]       = {'Z', juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier};
    bindings[(int)HotkeyAction::SaveProject]= {'S', juce::ModifierKeys::ctrlModifier};
    bindings[(int)HotkeyAction::OpenProject]= {'O', juce::ModifierKeys::ctrlModifier};
    bindings[(int)HotkeyAction::NewProject] = {'N', juce::ModifierKeys::ctrlModifier};
    bindings[(int)HotkeyAction::DeleteSelected] = {juce::KeyPress::deleteKey, 0};
}

void HotkeyManager::resetToDefaults() { setDefaults(); }

HotkeyBinding HotkeyManager::getBinding(HotkeyAction action) const {
    return bindings[(int)action];
}

void HotkeyManager::setBinding(HotkeyAction action, const HotkeyBinding& binding) {
    bindings[(int)action] = binding;
}

void HotkeyManager::clearBinding(HotkeyAction action) {
    bindings[(int)action] = {};
}

HotkeyAction HotkeyManager::findActionForKey(const juce::KeyPress& key) const {
    for (int i = 0; i < (int)HotkeyAction::COUNT; ++i)
        if (bindings[i].matchesKey(key))
            return (HotkeyAction)i;
    return HotkeyAction::COUNT; // no match
}

HotkeyAction HotkeyManager::findActionForMidi(int type, int channel, int number) const {
    for (int i = 0; i < (int)HotkeyAction::COUNT; ++i)
        if (bindings[i].matchesMidi(type, channel, number))
            return (HotkeyAction)i;
    return HotkeyAction::COUNT;
}

bool HotkeyManager::executeAction(HotkeyAction action) {
    if (action >= HotkeyAction::COUNT) return false;
    auto& cb = callbacks[(int)action];
    if (cb) { cb(); return true; }
    return false;
}

void HotkeyManager::addNodeBinding(const std::string& nodeName, NodeActionType type,
                                    const HotkeyBinding& binding) {
    nodeBindings.push_back({nodeName, type, binding});
}

void HotkeyManager::removeNodeBinding(int index) {
    if (index >= 0 && index < (int)nodeBindings.size())
        nodeBindings.erase(nodeBindings.begin() + index);
}

DynamicNodeBinding* HotkeyManager::findNodeBindingForKey(const juce::KeyPress& key) {
    for (auto& nb : nodeBindings)
        if (nb.binding.matchesKey(key)) return &nb;
    return nullptr;
}

DynamicNodeBinding* HotkeyManager::findNodeBindingForMidi(int type, int channel, int number) {
    for (auto& nb : nodeBindings)
        if (nb.binding.matchesMidi(type, channel, number)) return &nb;
    return nullptr;
}

void HotkeyManager::saveToFile(const juce::File& file) const {
    juce::var root(new juce::DynamicObject());
    auto* obj = root.getDynamicObject();
    for (int i = 0; i < (int)HotkeyAction::COUNT; ++i) {
        auto& b = bindings[i];
        if (b.isUnbound()) continue;
        auto* entry = new juce::DynamicObject();
        if (b.isMidi) {
            entry->setProperty("midi", true);
            entry->setProperty("midiType", b.midiType);
            entry->setProperty("midiChannel", b.midiChannel);
            entry->setProperty("midiNumber", b.midiNumber);
        } else {
            entry->setProperty("key", b.keyCode);
            entry->setProperty("mods", b.modifiers);
        }
        obj->setProperty(juce::String(actionName((HotkeyAction)i)), juce::var(entry));
    }
    file.replaceWithText(juce::JSON::toString(root, true));
}

void HotkeyManager::loadFromFile(const juce::File& file) {
    if (!file.existsAsFile()) return;
    auto root = juce::JSON::parse(file.loadFileAsString());
    if (!root.isObject()) return;
    auto* obj = root.getDynamicObject();
    if (!obj) return;

    for (int i = 0; i < (int)HotkeyAction::COUNT; ++i) {
        auto name = juce::String(actionName((HotkeyAction)i));
        if (!obj->hasProperty(name)) continue;
        auto entry = obj->getProperty(name);
        if (!entry.isObject()) continue;
        auto* eo = entry.getDynamicObject();
        if (!eo) continue;

        HotkeyBinding b;
        if ((bool)eo->getProperty("midi")) {
            b.isMidi = true;
            b.midiType = (int)eo->getProperty("midiType");
            b.midiChannel = (int)eo->getProperty("midiChannel");
            b.midiNumber = (int)eo->getProperty("midiNumber");
        } else {
            b.keyCode = (int)eo->getProperty("key");
            b.modifiers = (int)eo->getProperty("mods");
        }
        bindings[i] = b;
    }
}

// ==============================================================================
// Settings dialog
// ==============================================================================

void HotkeySettingsComponent::ActionListModel::paintListBoxItem(
    int row, juce::Graphics& g, int w, int h, bool selected)
{
    int fixedCount = (int)HotkeyAction::COUNT;
    int totalRows = fixedCount + (int)mgr.nodeBindings.size();
    if (row < 0 || row >= totalRows) return;

    if (selected)
        g.fillAll(juce::Colour(50, 60, 80));
    else
        g.fillAll(row % 2 == 0 ? juce::Colour(30, 30, 38) : juce::Colour(25, 25, 32));

    g.setFont(12.0f);

    bool isCapturing = (captureIdx == row);
    HotkeyBinding binding;
    juce::String category, actionName;

    if (row < fixedCount) {
        auto action = (HotkeyAction)row;
        category = HotkeyManager::actionCategory(action);
        actionName = HotkeyManager::actionName(action);
        binding = mgr.getBinding(action);
    } else {
        int dynIdx = row - fixedCount;
        auto& nb = mgr.nodeBindings[dynIdx];
        category = "Node";
        actionName = nb.displayName();
        binding = nb.binding;
    }

    // Category
    g.setColour(juce::Colours::grey);
    g.drawText(category, 8, 0, 90, h, juce::Justification::centredLeft);

    // Action name
    g.setColour(juce::Colours::white);
    g.drawText(actionName, 100, 0, 200, h, juce::Justification::centredLeft);

    // Current binding
    if (isCapturing) {
        g.setColour(juce::Colours::yellow);
        g.drawText("Press a key or MIDI button...", 310, 0, 250, h,
                   juce::Justification::centredLeft);
    } else {
        g.setColour(binding.isUnbound() ? juce::Colours::darkgrey : juce::Colours::cornflowerblue);
        g.drawText(binding.toString(), 310, 0, 150, h,
                   juce::Justification::centredLeft);
        if (!binding.isUnbound()) {
            g.setColour(juce::Colours::grey.withAlpha(0.5f));
            g.drawText("[click to change | right-click to remove]", 460, 0, 220, h,
                       juce::Justification::centredLeft);
        } else {
            g.setColour(juce::Colours::grey.withAlpha(0.3f));
            g.drawText("[click to assign]", 460, 0, 120, h,
                       juce::Justification::centredLeft);
        }
    }
}

void HotkeySettingsComponent::ActionListModel::listBoxItemClicked(
    int row, const juce::MouseEvent& e)
{
    int fixedCount = (int)HotkeyAction::COUNT;
    int totalRows = fixedCount + (int)mgr.nodeBindings.size();
    if (row < 0 || row >= totalRows) return;

    if (e.mods.isRightButtonDown()) {
        // Right-click: clear binding
        if (row < fixedCount)
            mgr.clearBinding((HotkeyAction)row);
        else
            mgr.removeNodeBinding(row - fixedCount);
        captureIdx = -1;
    } else {
        // Left-click: enter capture mode for this action
        captureIdx = row;
        if (owner) {
            owner->captureLabel.setText(
                "Press any key combo (with or without Ctrl/Shift/Alt), "
                "or press a MIDI button/pad on your controller. "
                "Assigning to: " + juce::String(HotkeyManager::actionName((HotkeyAction)row)),
                juce::dontSendNotification);
            owner->grabKeyboardFocus();
        }
    }
    if (owner) owner->listBox.repaint();
}

HotkeySettingsComponent::HotkeySettingsComponent(HotkeyManager& mgr, NodeGraph* g)
    : manager(mgr), model(mgr), graph(g)
{
    model.owner = this;
    listBox.setModel(&model);
    listBox.setRowHeight(24);
    addAndMakeVisible(listBox);

    addAndMakeVisible(captureLabel);
    captureLabel.setFont(11.0f);
    captureLabel.setColour(juce::Label::textColourId, juce::Colours::yellow);
    captureLabel.setText(
        "Click any action to assign a keyboard shortcut or MIDI controller button.",
        juce::dontSendNotification);

    addAndMakeVisible(addNodeActionBtn);
    addNodeActionBtn.setTooltip("Add a hotkey binding for a specific node — toggle its mute/solo or open its editor. "
                                "Pick the node and action from the popup, then click the new row to assign a keyboard shortcut.");
    addNodeActionBtn.onClick = [this]() {
        if (!graph) return;
        juce::PopupMenu menu;
        int id = 1;
        for (auto& node : graph->nodes) {
            juce::PopupMenu sub;
            sub.addItem(id++, "Mute: " + juce::String(node.name));
            sub.addItem(id++, "Solo: " + juce::String(node.name));
            sub.addItem(id++, "Open: " + juce::String(node.name));
            menu.addSubMenu(node.name, sub);
        }
        menu.showMenuAsync({}, [this](int result) {
            if (result <= 0 || !graph) return;
            int idx = (result - 1) / 3;
            int actionIdx = (result - 1) % 3;
            if (idx >= (int)graph->nodes.size()) return;
            auto& node = graph->nodes[idx];
            NodeActionType type = (actionIdx == 0) ? NodeActionType::ToggleMute
                                : (actionIdx == 1) ? NodeActionType::ToggleSolo
                                : NodeActionType::OpenEditor;
            manager.addNodeBinding(node.name, type, {});
            listBox.updateContent();
            listBox.repaint();
        });
    };

    addAndMakeVisible(resetBtn);
    resetBtn.setTooltip("Discard all custom hotkey bindings and restore the default set");
    resetBtn.onClick = [this]() {
        manager.resetToDefaults();
        model.captureIdx = -1;
        listBox.repaint();
    };

    addAndMakeVisible(closeBtn);
    closeBtn.onClick = [this]() {
        // Save on close
        auto configFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                              .getSiblingFile("hotkeys.json");
        manager.saveToFile(configFile);
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
            juce::Component::SafePointer<juce::DialogWindow> safe(dw);
            juce::MessageManager::callAsync([safe]() {
                if (safe) delete safe.getComponent();
            });
        }
    };

    setWantsKeyboardFocus(true);
    setSize(620, 500);
}

void HotkeySettingsComponent::resized() {
    auto a = getLocalBounds().reduced(8);
    auto bottom = a.removeFromBottom(28);
    closeBtn.setBounds(bottom.removeFromRight(60).reduced(2));
    bottom.removeFromRight(4);
    resetBtn.setBounds(bottom.removeFromRight(120).reduced(2));
    bottom.removeFromRight(4);
    addNodeActionBtn.setBounds(bottom.removeFromRight(140).reduced(2));
    captureLabel.setBounds(bottom.reduced(2));

    listBox.setBounds(a);
}

void HotkeySettingsComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(22, 22, 28));
}

bool HotkeySettingsComponent::keyPressed(const juce::KeyPress& key) {
    if (model.captureIdx < 0) return false;

    // Escape cancels capture
    if (key.getKeyCode() == juce::KeyPress::escapeKey) {
        model.captureIdx = -1;
        captureLabel.setText("", juce::dontSendNotification);
        listBox.repaint();
        return true;
    }

    // Capture the key combo
    HotkeyBinding binding;
    binding.keyCode = key.getKeyCode();
    binding.modifiers = key.getModifiers().getRawFlags() & 0x7; // ctrl, shift, alt only
    // Uppercase letter keys for consistency
    if (binding.keyCode >= 'a' && binding.keyCode <= 'z')
        binding.keyCode -= 32;

    int fixedCount = (int)HotkeyAction::COUNT;
    if (model.captureIdx < fixedCount)
        manager.setBinding((HotkeyAction)model.captureIdx, binding);
    else {
        int dynIdx = model.captureIdx - fixedCount;
        if (dynIdx >= 0 && dynIdx < (int)manager.nodeBindings.size())
            manager.nodeBindings[dynIdx].binding = binding;
    }
    captureLabel.setText("Assigned: " + binding.toString(), juce::dontSendNotification);
    model.captureIdx = -1;
    listBox.repaint();
    return true;
}

void HotkeySettingsComponent::onMidiReceived(int type, int channel, int number) {
    if (model.captureIdx < 0) return;

    HotkeyBinding binding;
    binding.isMidi = true;
    binding.midiType = type;
    binding.midiChannel = channel;
    binding.midiNumber = number;

    int fixedCount = (int)HotkeyAction::COUNT;
    if (model.captureIdx < fixedCount)
        manager.setBinding((HotkeyAction)model.captureIdx, binding);
    else {
        int dynIdx = model.captureIdx - fixedCount;
        if (dynIdx >= 0 && dynIdx < (int)manager.nodeBindings.size())
            manager.nodeBindings[dynIdx].binding = binding;
    }
    captureLabel.setText("Assigned: " + binding.toString(), juce::dontSendNotification);
    model.captureIdx = -1;

    juce::MessageManager::callAsync([this]() { listBox.repaint(); });
}

} // namespace SoundShop

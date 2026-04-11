#include "midi_device_wizard.h"
#include <algorithm>

namespace SoundShop {

MidiDeviceWizardComponent::MidiDeviceWizardComponent(NodeGraph& g,
                                                     AudioEngine& e,
                                                     std::function<void()> apply)
    : graph(g), engine(e), onApply(std::move(apply))
{
    addAndMakeVisible(headerLabel);
    headerLabel.setText("Select MIDI input devices to add to the graph",
                        juce::dontSendNotification);
    headerLabel.setFont(juce::Font(14.0f, juce::Font::bold));

    addAndMakeVisible(hintLabel);
    hintLabel.setText(
        "Each selected device becomes a node you can wire to tracks or "
        "synths. You can add or remove devices later via the menu.",
        juce::dontSendNotification);
    hintLabel.setFont(11.0f);
    hintLabel.setColour(juce::Label::textColourId, juce::Colours::grey);

    addAndMakeVisible(viewport);
    viewport.setViewedComponent(&content, false);
    viewport.setScrollBarsShown(true, false);

    addAndMakeVisible(addBtn);
    addBtn.setTooltip("Add a MIDI Input node to the graph for each device checked above. "
                      "You can wire those nodes to MIDI tracks or synths to play them live.");
    addBtn.onClick = [this]() {
        // Add a MidiInput node for each checked row that isn't already in.
        int newCount = 0;
        float nextY = 100.0f;
        for (auto& n : graph.nodes)
            nextY = std::max(nextY, n.pos.y); // stack new ones below existing
        float placeY = nextY + 80.0f;
        float placeX = 80.0f;
        for (auto& r : rows) {
            if (r.alreadyAdded) continue;
            if (!r.toggle->getToggleState()) continue;
            auto& node = graph.addNode(r.name.toStdString(), NodeType::MidiInput,
                {}, {Pin{0, "MIDI Out", PinKind::Midi, false}},
                {placeX, placeY + newCount * 60.0f});
            node.midiInputSourceId = r.identifier.toStdString();
            ++newCount;
        }
        if (newCount > 0) {
            engine.syncMidiDeviceEnablement();
            if (onApply) onApply();
        }
        closeSelf();
    };

    addAndMakeVisible(selectAllBtn);
    selectAllBtn.setTooltip("Check every detected device that isn't already in the graph");
    selectAllBtn.onClick = [this]() {
        for (auto& r : rows)
            if (!r.alreadyAdded)
                r.toggle->setToggleState(true, juce::dontSendNotification);
    };

    addAndMakeVisible(skipBtn);
    skipBtn.setTooltip("Close this dialog without adding any devices. You can re-open it later from "
                       "the Options menu if you change your mind.");
    skipBtn.onClick = [this]() { closeSelf(); };

    rebuildList();
    setSize(520, 380);
}

void MidiDeviceWizardComponent::rebuildList() {
    rows.clear();
    content.removeAllChildren();

    auto devices = engine.listMidiInputDevices();
    int y = 0;
    for (auto& d : devices) {
        Row r;
        r.identifier = d.identifier;
        r.name = d.name;
        r.alreadyAdded = d.presentInGraph;
        r.toggle = std::make_unique<juce::ToggleButton>(
            d.name + (r.alreadyAdded ? juce::String(" (already added)") : juce::String()));
        r.toggle->setEnabled(!r.alreadyAdded);
        // MIDI devices default to UNCHECKED — a user might have many
        // controllers plugged in and not want them all populating the
        // graph. They explicitly pick the ones they want.
        // (When audio I/O sections are added in Phase 2b/2c, the OS-
        //  active audio in + audio out devices WILL default to checked.)
        r.toggle->setToggleState(false, juce::dontSendNotification);
        r.toggle->setBounds(0, y, 480, 24);
        content.addAndMakeVisible(r.toggle.get());
        rows.push_back(std::move(r));
        y += 28;
    }
    if (rows.empty()) {
        // Show a helpful message when nothing is detected
        auto* lbl = new juce::Label();
        lbl->setText("No MIDI input devices detected.",
                     juce::dontSendNotification);
        lbl->setColour(juce::Label::textColourId, juce::Colours::grey);
        lbl->setBounds(0, 0, 480, 24);
        content.addAndMakeVisible(lbl);
        y = 30;
        // leaks — acceptable for this one-shot case; dialog closes fast
    }
    content.setSize(500, std::max(y, 30));
}

void MidiDeviceWizardComponent::closeSelf() {
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
        juce::Component::SafePointer<juce::DialogWindow> safe(dw);
        juce::MessageManager::callAsync([safe]() {
            if (safe) delete safe.getComponent();
        });
    }
}

void MidiDeviceWizardComponent::resized() {
    auto a = getLocalBounds().reduced(12);
    headerLabel.setBounds(a.removeFromTop(22));
    hintLabel.setBounds(a.removeFromTop(34));
    a.removeFromTop(6);

    auto buttons = a.removeFromBottom(30);
    skipBtn.setBounds(buttons.removeFromLeft(70));
    buttons.removeFromLeft(6);
    selectAllBtn.setBounds(buttons.removeFromLeft(90));
    addBtn.setBounds(buttons.removeFromRight(110));

    a.removeFromBottom(8);
    viewport.setBounds(a);
    content.setSize(viewport.getWidth() - 4, content.getHeight());
}

void MidiDeviceWizardComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(28, 28, 36));
}

} // namespace SoundShop

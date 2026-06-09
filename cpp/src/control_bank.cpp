#include "control_bank.h"
#include <algorithm>

namespace SoundShop {

ControlBankComponent::ControlBankComponent(NodeGraph& g, int nid,
                                           std::function<void()> changed)
    : graph(g), nodeId(nid), onChanged(std::move(changed))
{
    if (auto* nd = node())
        vertical = !(nd->script.find(":h") != std::string::npos);

    titleLabel.setText("Control Bank - drag a fader to set its output (0..1). "
                       "Stretch the window to make sliders longer and finer.",
                       juce::dontSendNotification);
    titleLabel.setFont(juce::Font(12.0f));
    titleLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    titleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(titleLabel);

    addBtn.setTooltip("Add a slider. Each slider gets its own control-signal "
                      "output pin you can wire to any param.");
    addBtn.onClick = [this]() { addSlider(); };
    addAndMakeVisible(addBtn);

    orientationToggle.setTooltip("Lay the faders out horizontally (stacked, "
                                 "window width sets their length) instead of "
                                 "vertically (in a row, window height sets "
                                 "their length).");
    orientationToggle.setToggleState(!vertical, juce::dontSendNotification);
    orientationToggle.onClick = [this]() {
        vertical = !orientationToggle.getToggleState();
        writeOrientationToScript();
        applyOrientationStyles();
        resized();
        repaint();
        graph.commitSnapshot("Control bank orientation");
    };
    addAndMakeVisible(orientationToggle);

    rebuildRows();
    setSize(520, 380);
}

void ControlBankComponent::writeOrientationToScript() {
    auto* nd = node();
    if (!nd) return;
    nd->script = vertical ? "__controlbank__" : "__controlbank__:h";
}

void ControlBankComponent::applyOrientationStyles() {
    for (auto& r : rows) {
        if (!r.slider) continue;
        if (vertical) {
            r.slider->setSliderStyle(juce::Slider::LinearVertical);
            r.slider->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 52, 16);
        } else {
            r.slider->setSliderStyle(juce::Slider::LinearHorizontal);
            r.slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 16);
        }
    }
}

void ControlBankComponent::rebuildRows() {
    rows.clear();
    auto* nd = node();
    if (!nd) return;

    // Defensive: a Control Bank should always have at least one slider. If a
    // malformed file or an empty creation slipped through, seed one so the UI
    // isn't blank and the node still emits something.
    if (nd->params.empty()) {
        nd->params.push_back({"Slider 1", 0.5f, 0.0f, 1.0f});
        Pin p; p.id = graph.allocId(); p.name = "Slider 1";
        p.kind = PinKind::Signal; p.isInput = false; p.channels = 1;
        nd->pinsOut.push_back(p);
    }

    const int n = (int)nd->params.size();
    for (int i = 0; i < n; ++i) {
        Row row;
        row.pinId = (i < (int)nd->pinsOut.size()) ? nd->pinsOut[(size_t)i].id : -1;

        row.name = std::make_unique<juce::Label>();
        row.name->setText(nd->params[(size_t)i].name, juce::dontSendNotification);
        row.name->setJustificationType(juce::Justification::centred);
        row.name->setColour(juce::Label::textColourId, juce::Colours::white);
        row.name->setFont(juce::Font(12.0f));
        row.name->setEditable(false, true, false); // edit on double-click
        row.name->setTooltip("Double-click to rename this slider and its "
                             "output pin.");
        {
            juce::Label* lbl = row.name.get();
            int idx = i;
            lbl->onTextChange = [this, lbl, idx]() {
                renameSlider(idx, lbl->getText());
            };
        }
        addAndMakeVisible(row.name.get());

        row.slider = std::make_unique<juce::Slider>();
        row.slider->setRange(0.0, 1.0, 0.0);
        row.slider->setValue((double)nd->params[(size_t)i].value,
                             juce::dontSendNotification);
        row.slider->setDoubleClickReturnValue(true, 0.5);
        {
            juce::Slider* sl = row.slider.get();
            int idx = i;
            sl->onValueChange = [this, sl, idx]() {
                auto* n2 = node();
                if (!n2 || idx >= (int)n2->params.size()) return;
                n2->params[(size_t)idx].value = (float)sl->getValue();
                // The processor reads params[idx].value live every block, so
                // no graph rebuild is needed for a value move. Snapshot only
                // for non-drag edits (wheel / typed value); drag gestures
                // snapshot once on release via onDragEnd.
                if (!sl->isMouseButtonDown())
                    graph.commitSnapshot("Set control value");
            };
            sl->onDragEnd = [this]() {
                graph.commitSnapshot("Set control value");
            };
        }
        addAndMakeVisible(row.slider.get());

        row.removeBtn = std::make_unique<juce::TextButton>("X");
        row.removeBtn->setTooltip("Remove this slider and its output pin.");
        {
            int idx = i;
            row.removeBtn->onClick = [this, idx]() { removeSlider(idx); };
        }
        addAndMakeVisible(row.removeBtn.get());

        rows.push_back(std::move(row));
    }

    applyOrientationStyles();

    const bool canRemove = (int)rows.size() > kMinSliders;
    for (auto& r : rows) {
        r.removeBtn->setEnabled(canRemove);
        if (!canRemove)
            r.removeBtn->setTooltip("A Control Bank needs at least one slider.");
    }
    addBtn.setEnabled((int)rows.size() < kMaxSliders);
    if (!addBtn.isEnabled())
        addBtn.setTooltip("Maximum of " + juce::String(kMaxSliders) + " sliders reached.");

    resized();
    repaint();
}

void ControlBankComponent::addSlider() {
    auto* nd = node();
    if (!nd || (int)nd->params.size() >= kMaxSliders) return;

    const int next = (int)nd->params.size() + 1;
    juce::String name = "Slider " + juce::String(next);
    nd->params.push_back({name.toStdString(), 0.5f, 0.0f, 1.0f});

    Pin p; p.id = graph.allocId(); p.name = name.toStdString();
    p.kind = PinKind::Signal; p.isInput = false; p.channels = 1;
    nd->pinsOut.push_back(p);

    rebuildRows();
    if (onChanged) onChanged();          // pin count changed -> rebuild graph
    graph.commitSnapshot("Add control slider");
}

void ControlBankComponent::removeSlider(int idx) {
    auto* nd = node();
    if (!nd) return;
    if (idx < 0 || idx >= (int)nd->params.size()) return;
    if ((int)nd->params.size() <= kMinSliders) return;

    // Drop any cables wired to this slider's output pin so no dangling link
    // survives (graph rebuild skips orphans, but a saved project shouldn't
    // carry links to pins that no longer exist).
    if (idx < (int)nd->pinsOut.size()) {
        const int pinId = nd->pinsOut[(size_t)idx].id;
        graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
            [pinId](const Link& l) {
                return l.startPin == pinId || l.endPin == pinId;
            }), graph.links.end());
        nd->pinsOut.erase(nd->pinsOut.begin() + idx);
    }
    nd->params.erase(nd->params.begin() + idx);

    rebuildRows();
    if (onChanged) onChanged();          // pin count changed -> rebuild graph
    graph.commitSnapshot("Remove control slider");
}

void ControlBankComponent::renameSlider(int idx, const juce::String& newName) {
    auto* nd = node();
    if (!nd || idx < 0 || idx >= (int)nd->params.size()) return;
    juce::String trimmed = newName.trim();
    if (trimmed.isEmpty()) trimmed = "Slider " + juce::String(idx + 1);
    nd->params[(size_t)idx].name = trimmed.toStdString();
    if (idx < (int)nd->pinsOut.size())
        nd->pinsOut[(size_t)idx].name = trimmed.toStdString();
    if (idx < (int)rows.size() && rows[(size_t)idx].name)
        rows[(size_t)idx].name->setText(trimmed, juce::dontSendNotification);
    if (onChanged) onChanged();          // pin label changed -> refresh graph
    graph.commitSnapshot("Rename control slider");
}

void ControlBankComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(25, 25, 32));
}

void ControlBankComponent::resized() {
    auto area = getLocalBounds().reduced(8);

    titleLabel.setBounds(area.removeFromTop(34));
    area.removeFromTop(4);

    auto topRow = area.removeFromTop(26);
    addBtn.setBounds(topRow.removeFromLeft(90));
    orientationToggle.setBounds(topRow.removeFromRight(170));
    area.removeFromTop(8);

    if (rows.empty()) return;
    const int n = (int)rows.size();

    if (vertical) {
        // Faders in a row. Column = [name][slider fills][X].
        const int colW = std::max(40, area.getWidth() / n);
        for (int i = 0; i < n; ++i) {
            auto col = area.removeFromLeft(colW).reduced(3, 0);
            auto& r = rows[(size_t)i];
            r.name->setBounds(col.removeFromTop(20));
            r.removeBtn->setBounds(col.removeFromBottom(20).withSizeKeepingCentre(24, 18));
            r.slider->setBounds(col);
        }
    } else {
        // Sliders stacked. Row = [name][slider fills][X].
        const int rowH = std::max(28, area.getHeight() / n);
        for (int i = 0; i < n; ++i) {
            auto rowArea = area.removeFromTop(rowH).reduced(0, 3);
            auto& r = rows[(size_t)i];
            r.name->setBounds(rowArea.removeFromLeft(90));
            r.removeBtn->setBounds(rowArea.removeFromRight(28).withSizeKeepingCentre(24, 18));
            r.slider->setBounds(rowArea);
        }
    }
}

} // namespace SoundShop

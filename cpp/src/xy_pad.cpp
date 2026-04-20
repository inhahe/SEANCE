#include "xy_pad.h"
#include <cmath>

namespace SoundShop {

XYPadComponent::XYPadComponent(NodeGraph& g, int nid) : graph(g), nodeId(nid) {
    addAndMakeVisible(xCombo); addAndMakeVisible(yCombo); addAndMakeVisible(zCombo);
    addAndMakeVisible(xLabel); addAndMakeVisible(yLabel); addAndMakeVisible(zLabel);
    xLabel.setText("X axis (left/right):", juce::dontSendNotification);
    yLabel.setText("Y axis (up/down):", juce::dontSendNotification);
    zLabel.setText("Z axis (scroll wheel):", juce::dontSendNotification);
    xCombo.setTooltip("Pick which parameter on which node the X axis (horizontal mouse position) controls");
    yCombo.setTooltip("Pick which parameter on which node the Y axis (vertical mouse position) controls");
    zCombo.setTooltip("Pick which parameter on which node the Z axis (mouse scroll wheel) controls");
    for (auto* l : {&xLabel, &yLabel, &zLabel}) {
        l->setFont(11.0f);
        l->setJustificationType(juce::Justification::centredRight);
    }

    rebuildCombos();

    auto onCombo = [this](juce::ComboBox& combo, XYAxisBinding& bind) {
        int id = combo.getSelectedId();
        if (id <= 0) { bind.nodeId = -1; bind.paramIdx = -1; }
        else parseComboId(id, bind.nodeId, bind.paramIdx);
    };
    xCombo.onChange = [this, onCombo]() { onCombo(xCombo, xBind); };
    yCombo.onChange = [this, onCombo]() { onCombo(yCombo, yBind); };
    zCombo.onChange = [this, onCombo]() { onCombo(zCombo, zBind); };

    startTimerHz(30); // repaint for visual feedback
    setSize(340, 420);
}

void XYPadComponent::rebuildCombos() {
    for (auto* combo : {&xCombo, &yCombo, &zCombo}) {
        combo->clear();
        combo->addItem("None", 1);
        for (auto& node : graph.nodes) {
            for (int pi = 0; pi < (int)node.params.size(); ++pi) {
                auto label = juce::String(node.name) + ": " + juce::String(node.params[pi].name);
                combo->addItem(label, makeComboId(node.id, pi));
            }
        }
        combo->setSelectedId(1, juce::dontSendNotification);
    }
}

int XYPadComponent::makeComboId(int nodeId, int paramIdx) {
    return nodeId * 1000 + paramIdx + 2; // +2 because 1 = "None"
}

void XYPadComponent::parseComboId(int itemId, int& nodeId, int& paramIdx) {
    itemId -= 2;
    nodeId = itemId / 1000;
    paramIdx = itemId % 1000;
}

void XYPadComponent::applyBinding(const XYAxisBinding& bind, float value01) {
    if (bind.nodeId < 0 || bind.paramIdx < 0) return;
    auto* node = graph.findNode(bind.nodeId);
    if (!node || bind.paramIdx >= (int)node->params.size()) return;
    auto& p = node->params[bind.paramIdx];
    p.value = p.minVal + value01 * (p.maxVal - p.minVal);
}

juce::Rectangle<float> XYPadComponent::getPadArea() const {
    auto bounds = getLocalBounds().toFloat();
    bounds.removeFromTop(90); // space for dropdowns
    bounds.removeFromBottom(16); // space for value labels
    return bounds.reduced(12);
}

std::pair<float, float> XYPadComponent::mouseToXY(juce::Point<float> pos) const {
    auto area = getPadArea();
    float x = juce::jlimit(0.0f, 1.0f, (pos.x - area.getX()) / area.getWidth());
    float y = juce::jlimit(0.0f, 1.0f, 1.0f - (pos.y - area.getY()) / area.getHeight());
    return {x, y};
}

void XYPadComponent::updateNodeParams() {
    // Write X/Y/Z to the XY Pad node's own params so the signal outputs
    // (which read from the node's params in processBlock) reflect the pad.
    auto* nd = graph.findNode(nodeId);
    if (nd) {
        for (auto& p : nd->params) {
            if (p.name == "X") p.value = padX;
            else if (p.name == "Y") p.value = padY;
            else if (p.name == "Z") p.value = padZ;
        }
    }
}

void XYPadComponent::mouseDown(const juce::MouseEvent& e) {
    if (!getPadArea().contains(e.position)) return;
    auto [x, y] = mouseToXY(e.position);
    padX = x; padY = y;
    applyBinding(xBind, padX);
    applyBinding(yBind, padY);
    updateNodeParams();
    repaint();
}

void XYPadComponent::mouseDrag(const juce::MouseEvent& e) {
    auto [x, y] = mouseToXY(e.position);
    padX = x; padY = y;
    applyBinding(xBind, padX);
    applyBinding(yBind, padY);
    updateNodeParams();
    repaint();
}

void XYPadComponent::mouseUp(const juce::MouseEvent&) {}

void XYPadComponent::mouseWheelMove(const juce::MouseEvent&,
                                      const juce::MouseWheelDetails& w) {
    padZ = juce::jlimit(0.0f, 1.0f, padZ + w.deltaY * 0.1f);
    applyBinding(zBind, padZ);
    updateNodeParams();
    repaint();
}

void XYPadComponent::timerCallback() { repaint(); }

void XYPadComponent::resized() {
    auto area = getLocalBounds().reduced(8);
    int rowH = 24;
    auto placeRow = [&](juce::Label& lbl, juce::ComboBox& combo) {
        auto row = area.removeFromTop(rowH);
        lbl.setBounds(row.removeFromLeft(140));
        combo.setBounds(row.reduced(2, 2));
    };
    placeRow(xLabel, xCombo);
    placeRow(yLabel, yCombo);
    placeRow(zLabel, zCombo);
}

void XYPadComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(25, 25, 32));
    auto area = getPadArea();

    // Pad background
    g.setColour(juce::Colour(20, 22, 30));
    g.fillRoundedRectangle(area, 6.0f);
    g.setColour(juce::Colour(50, 55, 70));
    g.drawRoundedRectangle(area, 6.0f, 1.0f);

    // Grid lines
    g.setColour(juce::Colour(40, 42, 55));
    for (int i = 1; i < 4; ++i) {
        float fx = area.getX() + area.getWidth() * (i / 4.0f);
        float fy = area.getY() + area.getHeight() * (i / 4.0f);
        g.drawVerticalLine((int)fx, area.getY(), area.getBottom());
        g.drawHorizontalLine((int)fy, area.getX(), area.getRight());
    }

    // Center crosshair
    g.setColour(juce::Colour(60, 60, 80));
    g.drawVerticalLine((int)area.getCentreX(), area.getY(), area.getBottom());
    g.drawHorizontalLine((int)area.getCentreY(), area.getX(), area.getRight());

    // Handle position
    float hx = area.getX() + padX * area.getWidth();
    float hy = area.getBottom() - padY * area.getHeight();

    // Crosshair through handle
    g.setColour(juce::Colours::cornflowerblue.withAlpha(0.4f));
    g.drawVerticalLine((int)hx, area.getY(), area.getBottom());
    g.drawHorizontalLine((int)hy, area.getX(), area.getRight());

    // Handle dot
    float r = 8.0f;
    g.setColour(juce::Colours::cornflowerblue);
    g.fillEllipse(hx - r, hy - r, r * 2, r * 2);
    g.setColour(juce::Colours::white);
    g.drawEllipse(hx - r, hy - r, r * 2, r * 2, 1.5f);

    // Value labels
    g.setColour(juce::Colours::grey);
    g.setFont(10.0f);
    float lblY = area.getBottom() + 2;
    g.drawText("X: " + juce::String(padX, 2), (int)area.getX(), (int)lblY, 70, 14,
               juce::Justification::centredLeft);
    g.drawText("Y: " + juce::String(padY, 2), (int)area.getX() + 75, (int)lblY, 70, 14,
               juce::Justification::centredLeft);
    g.drawText("Z: " + juce::String(padZ, 2), (int)area.getX() + 150, (int)lblY, 70, 14,
               juce::Justification::centredLeft);
}

} // namespace SoundShop

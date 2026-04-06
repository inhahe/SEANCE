#include "node_graph_component.h"
#include "music_theory.h"
#include <cmath>

namespace SoundShop {

static const float NODE_WIDTH = 180.0f;
static const float PIN_ROW_HEIGHT = 20.0f;
static const float PIN_RADIUS = 5.0f;
static const float HEADER_HEIGHT = 24.0f;

NodeGraphComponent::NodeGraphComponent(NodeGraph& g) : graph(g) {
    setWantsKeyboardFocus(true);
}

// Coordinate transforms
juce::Point<float> NodeGraphComponent::screenToCanvas(juce::Point<float> screen) const {
    return (screen - panOffset) / zoom;
}

juce::Point<float> NodeGraphComponent::canvasToScreen(juce::Point<float> canvas) const {
    return canvas * zoom + panOffset;
}

// Node bounds in canvas coordinates
juce::Rectangle<float> NodeGraphComponent::getNodeBounds(const Node& node) const {
    int numRows = std::max((int)node.pinsIn.size(), (int)node.pinsOut.size());
    if (!node.params.empty()) numRows += (int)node.params.size();
    float h = HEADER_HEIGHT + std::max(numRows, 1) * PIN_ROW_HEIGHT + 8;
    return {node.pos.x, node.pos.y, NODE_WIDTH, h};
}

// Pin position in canvas coordinates
juce::Point<float> NodeGraphComponent::getPinPosition(const Node& node, const Pin& pin) const {
    auto bounds = getNodeBounds(node);
    float y = bounds.getY() + HEADER_HEIGHT;

    if (pin.isInput) {
        for (auto& p : node.pinsIn) {
            y += PIN_ROW_HEIGHT;
            if (p.id == pin.id) return {bounds.getX(), y - PIN_ROW_HEIGHT / 2};
        }
    } else {
        for (auto& p : node.pinsOut) {
            y += PIN_ROW_HEIGHT;
            if (p.id == pin.id) return {bounds.getRight(), y - PIN_ROW_HEIGHT / 2};
        }
    }
    return bounds.getCentre();
}

juce::Colour NodeGraphComponent::getNodeColor(const Node& node) const {
    switch (node.type) {
        case NodeType::AudioTimeline: return juce::Colour(80, 40, 120);
        case NodeType::MidiTimeline:  return juce::Colour(40, 60, 120);
        case NodeType::Instrument:    return juce::Colour(100, 60, 40);
        case NodeType::Effect:        return juce::Colour(40, 80, 120);
        case NodeType::Mixer:         return juce::Colour(80, 100, 40);
        case NodeType::Output:        return juce::Colour(120, 50, 50);
        case NodeType::Script:        return juce::Colour(60, 100, 80);
        case NodeType::Group:         return juce::Colour(70, 70, 90);
        case NodeType::TerrainSynth:  return juce::Colour(120, 60, 100);
        case NodeType::SignalShape:   return juce::Colour(180, 120, 40);
        default:                      return juce::Colour(80, 80, 80);
    }
}

// ==============================================================================
// Drawing
// ==============================================================================

void NodeGraphComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(25, 25, 30));
    drawGrid(g);

    // Draw parent-child group lines
    for (auto& node : graph.nodes) {
        if (node.parentGroupId >= 0) {
            auto* parent = graph.findNode(node.parentGroupId);
            if (parent) {
                auto childCenter = canvasToScreen(getNodeBounds(node).getCentre());
                auto parentCenter = canvasToScreen(getNodeBounds(*parent).getCentre());
                g.setColour(juce::Colours::white.withAlpha(0.15f));
                float dash[] = {4.0f, 4.0f};
                g.drawDashedLine(juce::Line<float>(parentCenter, childCenter), dash, 2, 1.0f);
            }
        }
    }

    // Draw links
    for (auto& link : graph.links)
        drawLink(g, link);

    // Draw pending link
    if (dragMode == DragMode::DragLink)
        drawPendingLink(g);

    // Draw nodes
    for (auto& node : graph.nodes)
        drawNode(g, node);
}

void NodeGraphComponent::drawGrid(juce::Graphics& g) {
    float gridSize = 50.0f * zoom;
    if (gridSize < 10) gridSize *= 5;

    float startX = std::fmod(panOffset.x, gridSize);
    float startY = std::fmod(panOffset.y, gridSize);

    g.setColour(juce::Colour(40, 40, 45));
    for (float x = startX; x < getWidth(); x += gridSize)
        g.drawVerticalLine((int)x, 0, (float)getHeight());
    for (float y = startY; y < getHeight(); y += gridSize)
        g.drawHorizontalLine((int)y, 0, (float)getWidth());
}

void NodeGraphComponent::drawNode(juce::Graphics& g, Node& node) {
    auto bounds = getNodeBounds(node);
    auto screenBounds = juce::Rectangle<float>(
        canvasToScreen(bounds.getTopLeft()),
        canvasToScreen(bounds.getBottomRight()));

    if (!screenBounds.expanded(50).intersects(getLocalBounds().toFloat()))
        return; // off-screen culling

    auto col = getNodeColor(node);
    bool isSelected = (node.id == selectedNodeId);
    bool isActiveEditor = (node.id == graph.activeEditorNodeId);

    // Node body
    g.setColour(col.withAlpha(0.85f));
    g.fillRoundedRectangle(screenBounds, 6.0f * zoom);

    // Border
    g.setColour(isActiveEditor ? juce::Colours::cornflowerblue
                : isSelected ? juce::Colours::white
                : col.brighter(0.3f));
    g.drawRoundedRectangle(screenBounds, 6.0f * zoom,
                            (isSelected || isActiveEditor) ? 2.5f : 1.0f);

    // Title
    float fontSize = std::max(10.0f, 14.0f * zoom);
    g.setFont(juce::Font(fontSize));
    g.setColour(juce::Colours::white);
    auto titleArea = screenBounds.removeFromTop(HEADER_HEIGHT * zoom);
    g.drawText(node.name, titleArea.reduced(6 * zoom, 0), juce::Justification::centredLeft);

    // Group/parent indicator for child nodes
    if (node.parentGroupId >= 0 && zoom > 0.4f) {
        auto* parent = graph.findNode(node.parentGroupId);
        juce::String info = parent ? parent->name : "?";
        info += " @" + juce::String(node.absoluteBeatOffset, 1);
        g.setColour(juce::Colours::cyan.withAlpha(0.6f));
        g.setFont(juce::Font(std::max(7.0f, 9.0f * zoom)));
        g.drawText(info,
                    titleArea.reduced(6 * zoom, 0).translated(0, HEADER_HEIGHT * zoom * 0.4f),
                    juce::Justification::centredLeft);
    }

    // Mute/Solo indicator
    if (node.muted) {
        g.setColour(juce::Colours::red.withAlpha(0.4f));
        g.fillRoundedRectangle(screenBounds, 6.0f * zoom); // dim overlay
        g.setColour(juce::Colours::red);
        g.setFont(juce::Font(std::max(8.0f, 10.0f * zoom)));
        g.drawText("M", titleArea.removeFromRight(16 * zoom), juce::Justification::centred);
    }
    if (node.soloed) {
        g.setColour(juce::Colours::yellow);
        g.setFont(juce::Font(std::max(8.0f, 10.0f * zoom), juce::Font::bold));
        g.drawText("S", titleArea.removeFromRight(16 * zoom), juce::Justification::centred);
    }

    // Pan indicator (small bar in title)
    if (node.pan != 0.0f && zoom > 0.4f) {
        float barW = 30 * zoom;
        float barH = 3 * zoom;
        float barX = titleArea.getCentreX() - barW / 2;
        float barY = titleArea.getBottom() - barH - 1;
        g.setColour(juce::Colours::grey.withAlpha(0.4f));
        g.fillRect(barX, barY, barW, barH);
        float panPos = (node.pan + 1.0f) / 2.0f; // 0..1
        float dotX = barX + panPos * barW;
        g.setColour(juce::Colours::orange);
        g.fillEllipse(dotX - 2 * zoom, barY - 1 * zoom, 4 * zoom, barH + 2 * zoom);
    }

    // Cache indicator (top-right of title)
    if (node.cache.valid) {
        float indR = 4 * zoom;
        float indX = titleArea.getRight() - indR * 2 - 4 * zoom;
        float indY = titleArea.getCentreY() - indR;
        g.setColour(node.cache.useDisk ? juce::Colours::cyan : juce::Colours::limegreen);
        g.fillEllipse(indX, indY, indR * 2, indR * 2);
    } else if (!node.cache.deterministic) {
        float indR = 4 * zoom;
        float indX = titleArea.getRight() - indR * 2 - 4 * zoom;
        float indY = titleArea.getCentreY() - indR;
        g.setColour(juce::Colours::orange);
        g.fillEllipse(indX, indY, indR * 2, indR * 2);
    }

    // Pins
    float pinY = bounds.getY() + HEADER_HEIGHT;
    auto drawPin = [&](const Pin& pin, bool isInput) {
        auto pos = canvasToScreen({isInput ? bounds.getX() : bounds.getRight(), pinY + PIN_ROW_HEIGHT / 2});
        float r = PIN_RADIUS * zoom;

        // Pin circle
        g.setColour(pin.kind == PinKind::Midi   ? juce::Colours::limegreen
                  : pin.kind == PinKind::Param  ? juce::Colours::orange
                  : pin.kind == PinKind::Signal ? juce::Colours::red
                  : juce::Colours::cornflowerblue);
        g.fillEllipse(pos.x - r, pos.y - r, r * 2, r * 2);

        // Label
        float labelFontSize = std::max(8.0f, 11.0f * zoom);
        g.setFont(juce::Font(labelFontSize));
        g.setColour(juce::Colours::white.withAlpha(0.8f));
        auto labelRect = juce::Rectangle<float>(
            isInput ? pos.x + r + 3 * zoom : pos.x - r - 80 * zoom,
            pos.y - labelFontSize / 2,
            75 * zoom, labelFontSize + 2);
        g.drawText(pin.name,
                    labelRect,
                    isInput ? juce::Justification::centredLeft : juce::Justification::centredRight);
    };

    int maxPins = std::max((int)node.pinsIn.size(), (int)node.pinsOut.size());
    for (int i = 0; i < maxPins; ++i) {
        if (i < (int)node.pinsIn.size()) drawPin(node.pinsIn[i], true);
        if (i < (int)node.pinsOut.size()) drawPin(node.pinsOut[i], false);
        pinY += PIN_ROW_HEIGHT;
    }
}

void NodeGraphComponent::drawLink(juce::Graphics& g, Link& link) {
    // Find source and destination pin positions
    juce::Point<float> start, end;
    PinKind kind = PinKind::Audio;
    bool found = false;

    for (auto& node : graph.nodes) {
        for (auto& pin : node.pinsOut) {
            if (pin.id == link.startPin) {
                start = canvasToScreen(getPinPosition(node, pin));
                kind = pin.kind;
                found = true;
                break;
            }
        }
        for (auto& pin : node.pinsIn) {
            if (pin.id == link.endPin) {
                end = canvasToScreen(getPinPosition(node, pin));
                break;
            }
        }
    }
    if (!found) return;

    // Bézier curve
    juce::Path path;
    float dx = std::abs(end.x - start.x) * 0.5f;
    dx = std::max(dx, 30.0f * zoom);
    path.startNewSubPath(start);
    path.cubicTo(start.x + dx, start.y, end.x - dx, end.y, end.x, end.y);

    auto linkColour = kind == PinKind::Midi   ? juce::Colours::limegreen.withAlpha(0.7f)
                    : kind == PinKind::Param  ? juce::Colours::orange.withAlpha(0.7f)
                    : kind == PinKind::Signal ? juce::Colours::red.withAlpha(0.7f)
                    : juce::Colours::cornflowerblue.withAlpha(0.7f);
    // Dim the cable if gain is very low
    if (link.gainDb < -10.0f)
        linkColour = linkColour.withAlpha(0.3f);
    g.setColour(linkColour);
    float thickness = ((link.id == selectedLinkId) ? 3.0f : 2.0f) * zoom;
    g.strokePath(path, juce::PathStrokeType(thickness));

    // Show gain label on cable if not unity
    if (link.gainDb != 0.0f && zoom > 0.4f) {
        float midX = (start.x + end.x) / 2;
        float midY = (start.y + end.y) / 2;
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(std::max(8.0f, 10.0f * zoom)));
        g.drawText(juce::String(link.gainDb, 1) + " dB",
                    (int)(midX - 20), (int)(midY - 8), 40, 16,
                    juce::Justification::centred);
    }
}

void NodeGraphComponent::drawPendingLink(juce::Graphics& g) {
    juce::Path path;
    float dx = std::abs(dragCurrent.x - dragStart.x) * 0.5f;
    dx = std::max(dx, 30.0f);
    path.startNewSubPath(dragStart);
    if (dragPinIsOutput)
        path.cubicTo(dragStart.x + dx, dragStart.y, dragCurrent.x - dx, dragCurrent.y, dragCurrent.x, dragCurrent.y);
    else
        path.cubicTo(dragStart.x - dx, dragStart.y, dragCurrent.x + dx, dragCurrent.y, dragCurrent.x, dragCurrent.y);

    g.setColour(juce::Colours::yellow.withAlpha(0.6f));
    g.strokePath(path, juce::PathStrokeType(2.0f));
}

// ==============================================================================
// Hit testing
// ==============================================================================

Node* NodeGraphComponent::nodeAtPoint(juce::Point<float> canvasPos) {
    // Iterate in reverse so topmost node (drawn last) is found first
    for (int i = (int)graph.nodes.size() - 1; i >= 0; --i) {
        if (getNodeBounds(graph.nodes[i]).contains(canvasPos))
            return &graph.nodes[i];
    }
    return nullptr;
}

int NodeGraphComponent::pinAtPoint(juce::Point<float> canvasPos, bool& isOutput) {
    float hitRadius = PIN_RADIUS * 2;
    for (auto& node : graph.nodes) {
        for (auto& pin : node.pinsOut) {
            if (getPinPosition(node, pin).getDistanceFrom(canvasPos) < hitRadius) {
                isOutput = true;
                return pin.id;
            }
        }
        for (auto& pin : node.pinsIn) {
            if (getPinPosition(node, pin).getDistanceFrom(canvasPos) < hitRadius) {
                isOutput = false;
                return pin.id;
            }
        }
    }
    return -1;
}

int NodeGraphComponent::linkAtPoint(juce::Point<float> canvasPos) {
    auto screenPos = canvasToScreen(canvasPos);
    for (auto& link : graph.links) {
        juce::Point<float> start, end;
        for (auto& node : graph.nodes) {
            for (auto& pin : node.pinsOut)
                if (pin.id == link.startPin) start = canvasToScreen(getPinPosition(node, pin));
            for (auto& pin : node.pinsIn)
                if (pin.id == link.endPin) end = canvasToScreen(getPinPosition(node, pin));
        }
        // Simple distance check to the line
        float dx = std::abs(end.x - start.x) * 0.5f;
        dx = std::max(dx, 30.0f * zoom);
        juce::Path path;
        path.startNewSubPath(start);
        path.cubicTo(start.x + dx, start.y, end.x - dx, end.y, end.x, end.y);

        juce::PathFlatteningIterator it(path, {}, 2.0f);
        float minDist = 999999;
        while (it.next())
            minDist = std::min(minDist, screenPos.getDistanceFrom({it.x2, it.y2}));

        if (minDist < 8.0f) return link.id;
    }
    return -1;
}

// ==============================================================================
// Mouse interaction
// ==============================================================================

void NodeGraphComponent::mouseDown(const juce::MouseEvent& e) {
    auto canvasPos = screenToCanvas(e.position);

    if (e.mods.isRightButtonDown()) {
        // Check link hit first for right-click
        int linkId = linkAtPoint(canvasPos);
        if (linkId >= 0) {
            selectedLinkId = linkId;
            selectedNodeId = -1;
            showLinkMenu(linkId);
            return;
        }
        auto* node = nodeAtPoint(canvasPos);
        if (node)
            showNodeMenu(*node);
        else
            showBackgroundMenu(canvasPos);
        return;
    }

    // Check pin hit first (for link dragging)
    bool isOut;
    int pinId = pinAtPoint(canvasPos, isOut);
    if (pinId >= 0) {
        dragMode = DragMode::DragLink;
        dragPinId = pinId;
        dragPinIsOutput = isOut;
        // Find pin position for start
        for (auto& node : graph.nodes) {
            auto& pins = isOut ? node.pinsOut : node.pinsIn;
            for (auto& pin : pins) {
                if (pin.id == pinId) {
                    dragStart = canvasToScreen(getPinPosition(node, pin));
                    break;
                }
            }
        }
        dragCurrent = e.position;
        return;
    }

    // Check node hit
    auto* node = nodeAtPoint(canvasPos);
    if (node) {
        dragMode = DragMode::MoveNode;
        dragNodeId = node->id;
        selectedNodeId = node->id;
        dragStart = e.position;
        repaint();
        return;
    }

    // Check link hit
    int linkId = linkAtPoint(canvasPos);
    if (linkId >= 0) {
        selectedLinkId = linkId;
        selectedNodeId = -1;
        repaint();
        return;
    }

    // Empty space — pan
    dragMode = DragMode::Pan;
    dragStart = e.position;
    selectedNodeId = -1;
    selectedLinkId = -1;
    repaint();
}

void NodeGraphComponent::mouseDrag(const juce::MouseEvent& e) {
    if (dragMode == DragMode::Pan) {
        panOffset += e.position - dragStart;
        dragStart = e.position;
        repaint();
    } else if (dragMode == DragMode::MoveNode) {
        auto* node = graph.findNode(dragNodeId);
        if (node) {
            auto delta = (e.position - dragStart) / zoom;
            node->pos = {node->pos.x + delta.x, node->pos.y + delta.y};
            graph.dirty = true;
            dragStart = e.position;
            repaint();
        }
    } else if (dragMode == DragMode::DragLink) {
        dragCurrent = e.position;
        repaint();
    }
}

void NodeGraphComponent::mouseUp(const juce::MouseEvent& e) {
    if (dragMode == DragMode::DragLink) {
        // Check if dropped on a pin
        auto canvasPos = screenToCanvas(e.position);
        bool isOut;
        int targetPin = pinAtPoint(canvasPos, isOut);
        if (targetPin >= 0 && isOut != dragPinIsOutput) {
            // Create link
            int outPin = dragPinIsOutput ? dragPinId : targetPin;
            int inPin = dragPinIsOutput ? targetPin : dragPinId;
            graph.addLink(outPin, inPin);
        }
    }
    dragMode = DragMode::None;
    dragNodeId = -1;
    repaint();
}

void NodeGraphComponent::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) {
    float oldZoom = zoom;
    zoom *= (1.0f + wheel.deltaY * 0.3f);
    zoom = juce::jlimit(0.1f, 4.0f, zoom);

    // Zoom toward mouse position
    auto mousePos = e.position;
    panOffset = mousePos - (mousePos - panOffset) * (zoom / oldZoom);

    repaint();
}

void NodeGraphComponent::mouseDoubleClick(const juce::MouseEvent& e) {
    auto canvasPos = screenToCanvas(e.position);
    auto* node = nodeAtPoint(canvasPos);
    if (!node) return;

    if (node->type == NodeType::TerrainSynth) {
        // Open terrain visualizer
        if (onShowPluginUI) onShowPluginUI(node->id); // reuse plugin UI callback for now
        return;
    }

    if (node->type == NodeType::MidiTimeline || node->type == NodeType::AudioTimeline) {
        // Open piano roll editor
        bool already = false;
        for (auto* ed : graph.openEditors)
            if (ed->id == node->id) { already = true; break; }
        if (!already)
            graph.openEditors.insert(graph.openEditors.begin(), node);
        graph.activeEditorNodeId = node->id;
        if (onOpenEditor) onOpenEditor(*node);
    } else if (node->plugin || node->type == NodeType::Instrument || node->type == NodeType::Effect) {
        // Open plugin UI
        if (onShowPluginUI) onShowPluginUI(node->id);
    }
}

void NodeGraphComponent::fitAll() {
    if (graph.nodes.empty()) return;

    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (auto& node : graph.nodes) {
        auto b = getNodeBounds(node);
        minX = std::min(minX, b.getX());
        minY = std::min(minY, b.getY());
        maxX = std::max(maxX, b.getRight());
        maxY = std::max(maxY, b.getBottom());
    }

    float contentW = maxX - minX + 100;
    float contentH = maxY - minY + 100;
    float zoomX = getWidth() / contentW;
    float zoomY = getHeight() / contentH;
    zoom = juce::jlimit(0.1f, 1.5f, std::min(zoomX, zoomY));

    float cx = (minX + maxX) / 2;
    float cy = (minY + maxY) / 2;
    panOffset = {getWidth() / 2.0f - cx * zoom, getHeight() / 2.0f - cy * zoom};

    repaint();
}

void NodeGraphComponent::resized() {}

// ==============================================================================
// Context menus
// ==============================================================================

void NodeGraphComponent::showBackgroundMenu(juce::Point<float> canvasPos) {
    juce::PopupMenu menu;
    menu.addItem(1, "MIDI Timeline");
    menu.addItem(2, "Audio Timeline");
    menu.addSeparator();

    juce::PopupMenu instMenu;
    juce::PopupMenu synthMenu;
    synthMenu.addItem(110, "Sine");
    synthMenu.addItem(111, "Saw");
    synthMenu.addItem(112, "Square");
    synthMenu.addItem(113, "Triangle");
    synthMenu.addItem(114, "Custom Expression...");
    instMenu.addSubMenu("Built-in Synth", synthMenu);
    juce::PopupMenu terrainMenu;
    terrainMenu.addItem(120, "2D Terrain (sin*cos)");
    terrainMenu.addItem(121, "2D Terrain (noise)");
    terrainMenu.addItem(122, "2D Terrain (custom expression...)");
    terrainMenu.addItem(123, "From Image...");
    terrainMenu.addItem(124, "From Audio File...");
    instMenu.addSubMenu("Terrain Synth", terrainMenu);
    instMenu.addSeparator();
    instMenu.addItem(100, "Piano");
    instMenu.addItem(101, "Synth");
    instMenu.addItem(102, "Sampler");
    instMenu.addItem(103, "Drum Machine");
    menu.addSubMenu("Instruments", instMenu);

    juce::PopupMenu fxMenu;
    fxMenu.addItem(206, "Pitch Shift / Time Stretch");
    fxMenu.addSeparator();
    fxMenu.addItem(200, "Reverb");
    fxMenu.addItem(201, "Compressor");
    fxMenu.addItem(202, "EQ");
    fxMenu.addItem(203, "Delay");
    fxMenu.addItem(204, "Distortion");
    fxMenu.addItem(205, "Chorus");
    menu.addSubMenu("Effects", fxMenu);

    menu.addSeparator();
    menu.addItem(3, "Mixer");
    menu.addItem(4, "Output");
    menu.addItem(5, "Group");
    menu.addItem(6, "WASM Script...");

    juce::PopupMenu sigMenu;
    sigMenu.addItem(130, "LFO (sine)");
    sigMenu.addItem(131, "LFO (custom expression...)");
    sigMenu.addItem(132, "Envelope (custom expression...)");
    menu.addSubMenu("Signal Shape", sigMenu);

    // Plugin instruments/effects
    auto* host = graph.pluginHost;
    if (host) {
        auto& plugins = host->getAvailablePlugins();
        if (!plugins.empty()) {
            menu.addSeparator();
            juce::PopupMenu piMenu, pfxMenu;
            for (int i = 0; i < (int)plugins.size(); ++i) {
                auto& pi = plugins[i];
                auto label = pi.name + " (" + pi.manufacturer + ")";
                if (pi.isInstrument)
                    piMenu.addItem(1000 + i, label);
                else
                    pfxMenu.addItem(1000 + i, label);
            }
            menu.addSubMenu("Plugin Instruments", piMenu);
            menu.addSubMenu("Plugin Effects", pfxMenu);
        }
    }

    auto pos = canvasPos;
    menu.showMenuAsync(juce::PopupMenu::Options(), [this, pos](int result) {
        if (result <= 0) return;

        auto p = juce::Point<float>{pos.x, pos.y};
        if (result == 1) {
            auto& n = graph.addNode("MIDI Track", NodeType::MidiTimeline,
                {}, {Pin{0, "MIDI", PinKind::Midi, false}}, {p.x, p.y});
            n.clips.push_back({"Clip 1", 0, 4, juce::Colours::cornflowerblue.getARGB()});
        } else if (result == 2) {
            graph.addNode("Audio Track", NodeType::AudioTimeline,
                {}, {Pin{0, "Audio", PinKind::Audio, false}}, {p.x, p.y});
        } else if (result == 3) {
            graph.addNode("Mixer", NodeType::Mixer,
                {Pin{0, "In 1", PinKind::Audio, true}, Pin{0, "In 2", PinKind::Audio, true}},
                {Pin{0, "Out", PinKind::Audio, false}}, {p.x, p.y});
        } else if (result == 4) {
            graph.addNode("Output", NodeType::Output,
                {Pin{0, "In", PinKind::Audio, true}}, {}, {p.x, p.y});
        } else if (result == 5) {
            graph.createGroup("Group", {p.x, p.y});
        } else if (result == 6) {
            // WASM Script — open file chooser
            auto chooser = std::make_shared<juce::FileChooser>(
                "Load WASM Script", juce::File(), "*.wasm");
            auto canvasPos = p;
            chooser->launchAsync(juce::FileBrowserComponent::openMode,
                [this, canvasPos, chooser](const juce::FileChooser& fc) {
                    auto file = fc.getResult();
                    if (file.existsAsFile()) {
                        auto name = file.getFileNameWithoutExtension().toStdString();
                        auto& n = graph.addNode(name, NodeType::Script,
                            {Pin{0, "Audio In", PinKind::Audio, true}},
                            {Pin{0, "Audio Out", PinKind::Audio, false}},
                            {canvasPos.x, canvasPos.y});
                        n.script = file.getFullPathName().toStdString();
                        repaint();
                    }
                });
            return; // don't repaint yet, async
        } else if (result >= 110 && result <= 114) {
            // Built-in synth (unified: uses TerrainSynthProcessor)
            // Simple waveforms create 1D terrains; custom expressions auto-detect dimension
            const char* waveNames[] = {"Sine Synth", "Saw Synth", "Square Synth", "Triangle Synth", "Custom Synth"};
            const char* waveExprs[] = {"sin(x)", "saw(x)", "square(x)", "triangle(x)", ""};
            int wi = result - 110;
            auto& n = graph.addNode(waveNames[wi], NodeType::Instrument,
                {Pin{0, "MIDI", PinKind::Midi, true},
                 Pin{0, "Sig X", PinKind::Signal, true, 1},
                 Pin{0, "Sig Y", PinKind::Signal, true, 1}},
                {Pin{0, "Audio", PinKind::Audio, false}}, {p.x, p.y});
            n.script = waveExprs[wi];
            // Unified synth parameters (same as terrain synth)
            n.params.push_back({"Attack",       0.01f, 0.001f, 2.0f});   // 0
            n.params.push_back({"Decay",        0.1f,  0.001f, 2.0f});   // 1
            n.params.push_back({"Sustain",      0.7f,  0.0f,   1.0f});   // 2
            n.params.push_back({"Release",      0.3f,  0.001f, 5.0f});   // 3
            n.params.push_back({"Volume",       0.5f,  0.0f,   1.0f});   // 4
            n.params.push_back({"Speed",        1.0f,  0.01f, 20.0f});   // 5
            n.params.push_back({"Radius X",     0.3f,  0.0f,   0.5f});   // 6
            n.params.push_back({"Radius Y",     0.3f,  0.0f,   0.5f});   // 7
            n.params.push_back({"Center X",     0.5f,  0.0f,   1.0f});   // 8
            n.params.push_back({"Center Y",     0.5f,  0.0f,   1.0f});   // 9
            n.params.push_back({"Rad Mod Spd",  0.0f,  0.0f,  10.0f});   // 10
            n.params.push_back({"Rad Mod Amt",  0.0f,  0.0f,   0.3f});   // 11
            n.params.push_back({"Traversal",    0.0f,  0.0f,   3.0f});   // 12
            n.params.push_back({"Synth Mode",   0.0f,  0.0f,   1.0f});   // 13
            n.params.push_back({"LFO1 Rate",    0.5f,  0.01f, 20.0f});   // 14
            n.params.push_back({"LFO2 Rate",    0.2f,  0.01f, 20.0f});   // 15
            n.params.push_back({"LFO1 Amount",  0.0f,  0.0f,   1.0f});   // 16
            n.params.push_back({"LFO2 Amount",  0.0f,  0.0f,   1.0f});   // 17
            n.params.push_back({"Grain Size",   0.0f,  0.0f,   0.5f});   // 18 (seconds, 0=off)
            n.params.push_back({"Freeze",       0.0f,  0.0f,   1.0f});   // 19 (0=off, 1=freeze)
            n.params.push_back({"Grain Jitter", 0.0f,  0.0f,   1.0f});   // 20

            if (wi == 4) {
                // Custom: prompt for expression
                auto nodeId = n.id;
                auto* aw = new juce::AlertWindow("Synth Expression",
                    "Enter a math expression. Variables: x, y, z, w\n"
                    "Dimensions auto-detected from variables used.\n\n"
                    "1D (waveform): sin(x) + 0.3*sin(3*x)\n"
                    "2D (terrain): sin(x) * cos(y)\n"
                    "3D (volume): sin(x) * cos(y) * sin(z)\n\n"
                    "Functions: sin, cos, abs, sqrt, pow, tanh, noise\n"
                    "Shapes: saw(x), square(x), triangle(x)",
                    juce::MessageBoxIconType::NoIcon);
                aw->addTextEditor("expr", "sin(x) + 0.3*sin(3*x)", "Expression:");
                aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
                aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
                aw->enterModalState(true, juce::ModalCallbackFunction::create(
                    [this, nodeId, aw](int result) {
                        if (result == 1) {
                            auto expr = aw->getTextEditorContents("expr").toStdString();
                            if (auto* nd = graph.findNode(nodeId))
                                nd->script = expr;
                        }
                        delete aw;
                        repaint();
                    }), true);
                return;
            }
        } else if (result >= 130 && result <= 132) {
            // Signal Shape nodes
            auto makeSignalNode = [&](const std::string& name, const std::string& expr,
                                      float modeVal) -> Node& {
                auto& n = graph.addNode(name, NodeType::SignalShape,
                    {Pin{0, "MIDI In", PinKind::Midi, true}},      // trigger input for envelope
                    {Pin{0, "Param Out", PinKind::Param, false},
                     Pin{0, "Signal Out", PinKind::Signal, false, 1}},  // both UI-rate and audio-rate
                    {p.x, p.y});
                n.script = expr;
                n.params.push_back({"Mode",       modeVal, 0.0f, 1.0f});    // 0=LFO, 1=Envelope
                n.params.push_back({"Rate",       1.0f,    0.01f, 50.0f});   // Hz or beats
                n.params.push_back({"Min",        0.0f,   -1.0f, 1.0f});
                n.params.push_back({"Max",        1.0f,   -1.0f, 1.0f});
                n.params.push_back({"Beat Sync",  0.0f,    0.0f, 1.0f});    // 0=free, 1=synced
                n.params.push_back({"Phase",      0.0f,    0.0f, 1.0f});
                n.params.push_back({"Output",     0.0f,   -1.0f, 1.0f});    // read-only, current value
                return n;
            };

            if (result == 130) {
                makeSignalNode("LFO (sine)", "sin(x)", 0.0f);
            } else if (result == 131) {
                auto nodeId = makeSignalNode("LFO", "sin(x)", 0.0f).id;
                auto* aw = new juce::AlertWindow("LFO Expression",
                    "x = 0..2pi, output -1..1\nFunctions: sin, cos, abs, saw, square, triangle",
                    juce::MessageBoxIconType::NoIcon);
                aw->addTextEditor("expr", "sin(x) + 0.3*sin(3*x)", "Expression:");
                aw->addButton("OK", 1); aw->addButton("Cancel", 0);
                aw->enterModalState(true, juce::ModalCallbackFunction::create(
                    [this, nodeId, aw](int res) {
                        if (res == 1)
                            if (auto* nd = graph.findNode(nodeId))
                                nd->script = aw->getTextEditorContents("expr").toStdString();
                        delete aw; repaint();
                    }), true);
                return;
            } else if (result == 132) {
                auto nodeId = makeSignalNode("Envelope", "sin(x)", 1.0f).id;
                auto* aw = new juce::AlertWindow("Envelope Expression",
                    "x = 0..2pi (start to end of envelope)\n"
                    "Example: (1 - cos(x)) * 0.5  (fade in then out)\n"
                    "Example: sin(x/2)^2  (smooth attack, hold, release)",
                    juce::MessageBoxIconType::NoIcon);
                aw->addTextEditor("expr", "(1 - cos(x)) * 0.5", "Expression:");
                aw->addButton("OK", 1); aw->addButton("Cancel", 0);
                aw->enterModalState(true, juce::ModalCallbackFunction::create(
                    [this, nodeId, aw](int res) {
                        if (res == 1)
                            if (auto* nd = graph.findNode(nodeId))
                                nd->script = aw->getTextEditorContents("expr").toStdString();
                        delete aw; repaint();
                    }), true);
                return;
            }
        } else if (result >= 120 && result <= 124) {
            // Terrain Synth
            auto makeTerrainNode = [&](const std::string& name, const std::string& script) -> Node& {
                auto& n = graph.addNode(name, NodeType::TerrainSynth,
                    {Pin{0, "MIDI", PinKind::Midi, true},
                     Pin{0, "Sig X", PinKind::Signal, true, 1},
                     Pin{0, "Sig Y", PinKind::Signal, true, 1}},
                    {Pin{0, "Audio", PinKind::Audio, false}}, {p.x, p.y});
                n.script = script;
                n.params.push_back({"Attack",   0.01f, 0.001f, 2.0f});
                n.params.push_back({"Decay",    0.1f,  0.001f, 2.0f});
                n.params.push_back({"Sustain",  0.7f,  0.0f,   1.0f});
                n.params.push_back({"Release",  0.3f,  0.001f, 5.0f});
                n.params.push_back({"Volume",   0.5f,  0.0f,   1.0f});
                n.params.push_back({"Speed",        1.0f,  0.01f, 20.0f});  // 5
                n.params.push_back({"Radius X",     0.3f,  0.0f,   0.5f}); // 6
                n.params.push_back({"Radius Y",     0.3f,  0.0f,   0.5f}); // 7
                n.params.push_back({"Center X",     0.5f,  0.0f,   1.0f}); // 8
                n.params.push_back({"Center Y",     0.5f,  0.0f,   1.0f}); // 9
                n.params.push_back({"Rad Mod Spd",  0.0f,  0.0f,  10.0f}); // 10
                n.params.push_back({"Rad Mod Amt",  0.0f,  0.0f,   0.3f}); // 11
                n.params.push_back({"Traversal",    0.0f,  0.0f,   3.0f}); // 12: 0=Orbit,1=Linear,2=Lissajous,3=Physics
                n.params.push_back({"Synth Mode",   0.0f,  0.0f,   1.0f}); // 13: 0=SamplePerPoint,1=WaveformPerPoint
                n.params.push_back({"LFO1 Rate",    0.5f,  0.01f, 20.0f}); // 14
                n.params.push_back({"LFO2 Rate",    0.2f,  0.01f, 20.0f}); // 15
                n.params.push_back({"LFO1 Amount",  0.0f,  0.0f,   1.0f}); // 16
                n.params.push_back({"LFO2 Amount",  0.0f,  0.0f,   1.0f}); // 17
                n.params.push_back({"Grain Size",   0.0f,  0.0f,   0.5f}); // 18
                n.params.push_back({"Freeze",       0.0f,  0.0f,   1.0f}); // 19
                n.params.push_back({"Grain Jitter", 0.0f,  0.0f,   1.0f}); // 20
                return n;
            };

            if (result == 120) {
                makeTerrainNode("Terrain (sin*cos)", "sin(x) * cos(y)");
            } else if (result == 121) {
                makeTerrainNode("Terrain (noise)", "noise(0)");
            } else if (result == 122) {
                auto nodeId = makeTerrainNode("Terrain", "sin(x)*cos(y)").id;
                auto* aw = new juce::AlertWindow("Terrain Expression",
                    "Enter a 2D expression. Variables: x, y (0..2pi)\n"
                    "Functions: sin, cos, abs, sqrt, pow, tanh, noise\n\n"
                    "Examples:\n"
                    "  sin(x) * cos(y)\n"
                    "  sin(x*3) + cos(y*2) * 0.5\n"
                    "  tanh(sin(x) * sin(y) * 3)",
                    juce::MessageBoxIconType::NoIcon);
                aw->addTextEditor("expr", "sin(x) * cos(y)", "Expression:");
                aw->addButton("OK", 1); aw->addButton("Cancel", 0);
                aw->enterModalState(true, juce::ModalCallbackFunction::create(
                    [this, nodeId, aw](int res) {
                        if (res == 1)
                            if (auto* nd = graph.findNode(nodeId))
                                nd->script = aw->getTextEditorContents("expr").toStdString();
                        delete aw; repaint();
                    }), true);
                return;
            } else if (result == 123) {
                auto nodeId = makeTerrainNode("Terrain (image)", "").id;
                if (auto* nd = graph.findNode(nodeId)) nd->script = "__image__";
                auto chooser = std::make_shared<juce::FileChooser>("Load Image", juce::File(), "*.png;*.jpg;*.bmp");
                chooser->launchAsync(juce::FileBrowserComponent::openMode,
                    [this, nodeId, chooser](const juce::FileChooser& fc) {
                        auto file = fc.getResult();
                        if (file.existsAsFile())
                            if (auto* nd = graph.findNode(nodeId))
                                nd->script = "__image__:" + file.getFullPathName().toStdString();
                        repaint();
                    });
                return;
            } else if (result == 124) {
                auto nodeId = makeTerrainNode("Terrain (audio)", "").id;
                auto chooser = std::make_shared<juce::FileChooser>("Load Audio", juce::File(), "*.wav;*.mp3;*.aiff;*.flac");
                chooser->launchAsync(juce::FileBrowserComponent::openMode,
                    [this, nodeId, chooser](const juce::FileChooser& fc) {
                        auto file = fc.getResult();
                        if (file.existsAsFile())
                            if (auto* nd = graph.findNode(nodeId))
                                nd->script = "__audio__:" + file.getFullPathName().toStdString();
                        repaint();
                    });
                return;
            }
        } else if (result == 102) {
            // Sampler: load a sound file as a pitched instrument
            auto canvasPos = p;
            auto chooser = std::make_shared<juce::FileChooser>(
                "Load Sample", juce::File(), "*.wav;*.mp3;*.aiff;*.flac;*.ogg");
            chooser->launchAsync(juce::FileBrowserComponent::openMode,
                [this, canvasPos, chooser](const juce::FileChooser& fc) {
                    auto file = fc.getResult();
                    if (!file.existsAsFile()) return;
                    auto name = file.getFileNameWithoutExtension().toStdString();
                    auto& n = graph.addNode(name, NodeType::Instrument,
                        {Pin{0, "MIDI", PinKind::Midi, true},
                         Pin{0, "Sig X", PinKind::Signal, true, 1},
                         Pin{0, "Sig Y", PinKind::Signal, true, 1}},
                        {Pin{0, "Audio", PinKind::Audio, false}},
                        {canvasPos.x, canvasPos.y});
                    n.script = "__audio__:" + file.getFullPathName().toStdString();
                    // Sampler params (same as unified terrain synth)
                    n.params.push_back({"Attack",       0.005f, 0.001f, 2.0f});
                    n.params.push_back({"Decay",        0.05f,  0.001f, 2.0f});
                    n.params.push_back({"Sustain",      1.0f,   0.0f,   1.0f});
                    n.params.push_back({"Release",      0.1f,   0.001f, 5.0f});
                    n.params.push_back({"Volume",       0.5f,   0.0f,   1.0f});
                    n.params.push_back({"Speed",        1.0f,   0.01f, 20.0f});
                    n.params.push_back({"Radius X",     0.0f,   0.0f,   0.5f});
                    n.params.push_back({"Radius Y",     0.0f,   0.0f,   0.5f});
                    n.params.push_back({"Center X",     0.5f,   0.0f,   1.0f});
                    n.params.push_back({"Center Y",     0.5f,   0.0f,   1.0f});
                    n.params.push_back({"Rad Mod Spd",  0.0f,   0.0f,  10.0f});
                    n.params.push_back({"Rad Mod Amt",  0.0f,   0.0f,   0.3f});
                    n.params.push_back({"Traversal",    1.0f,   0.0f,   3.0f}); // Linear
                    n.params.push_back({"Synth Mode",   0.0f,   0.0f,   1.0f}); // SamplePerPoint
                    n.params.push_back({"LFO1 Rate",    0.5f,   0.01f, 20.0f});
                    n.params.push_back({"LFO2 Rate",    0.2f,   0.01f, 20.0f});
                    n.params.push_back({"LFO1 Amount",  0.0f,   0.0f,   1.0f});
                    n.params.push_back({"LFO2 Amount",  0.0f,   0.0f,   1.0f});
                    n.params.push_back({"Grain Size",   0.02f,  0.0f,   0.5f}); // sampler default: small grains
                    n.params.push_back({"Freeze",       0.0f,   0.0f,   1.0f});
                    n.params.push_back({"Grain Jitter", 0.0f,   0.0f,   1.0f});
                    repaint();
                });
            return;
        } else if (result >= 100 && result < 110 && result != 102) {
            // Generic built-in instruments (placeholders)
            const char* names[] = {"Piano", "Synth", "Sampler", "Drum Machine"};
            auto& n = graph.addNode(names[result-100], NodeType::Instrument,
                {Pin{0, "MIDI", PinKind::Midi, true}},
                {Pin{0, "Audio", PinKind::Audio, false}}, {p.x, p.y});
            (void)n;
        } else if (result == 206) {
            auto& n = graph.addNode("Pitch Shift", NodeType::Effect,
                {Pin{0, "Audio In", PinKind::Audio, true}},
                {Pin{0, "Audio Out", PinKind::Audio, false}}, {p.x, p.y});
            n.script = "__pitchshift__";
            n.params.push_back({"Pitch (semi)", 0.0f, -24.0f, 24.0f});
            n.params.push_back({"Time Ratio",   1.0f, 0.25f,  4.0f});
            n.params.push_back({"Formant",       1.0f, 0.0f,   1.0f});
        } else if (result >= 200 && result < 206) {
            const char* names[] = {"Reverb", "Compressor", "EQ", "Delay", "Distortion", "Chorus"};
            graph.addNode(names[result-200], NodeType::Effect,
                {Pin{0, "In", PinKind::Audio, true}},
                {Pin{0, "Out", PinKind::Audio, false}}, {p.x, p.y});
        } else if (result >= 1000 && graph.pluginHost) {
            int idx = result - 1000;
            auto& plugins = graph.pluginHost->getAvailablePlugins();
            if (idx < (int)plugins.size()) {
                auto& pi = plugins[idx];
                std::vector<Pin> ins, outs;
                if (pi.hasMidiInput) ins.push_back({0, "MIDI In", PinKind::Midi, true});
                if (pi.hasAudioInput) ins.push_back({0, "Audio In", PinKind::Audio, true, pi.numAudioInputChannels});
                if (pi.hasAudioOutput) outs.push_back({0, "Audio Out", PinKind::Audio, false, pi.numAudioOutputChannels});
                if (pi.hasMidiOutput) outs.push_back({0, "MIDI Out", PinKind::Midi, false});
                auto type = pi.isInstrument ? NodeType::Instrument : NodeType::Effect;
                auto& n = graph.addNode(pi.name, type, ins, outs, {p.x, p.y});
                auto loaded = graph.pluginHost->loadPlugin(idx, 44100.0, 512);
                if (loaded) { n.plugin = std::move(loaded); n.pluginIndex = idx; }
            }
        }
        repaint();
    });
}

void NodeGraphComponent::showNodeMenu(Node& node) {
    juce::PopupMenu menu;
    menu.addItem(5, "Rename...");
    menu.addSeparator();
    menu.addItem(1, "Delete");
    menu.addItem(2, "Duplicate");
    if (node.type == NodeType::MidiTimeline || node.type == NodeType::AudioTimeline) {
        menu.addItem(3, "Open Editor");
        menu.addItem(9, node.mpeEnabled ? "Disable MPE" : "Enable MPE", true, node.mpeEnabled);
    }
    if (node.plugin || node.type == NodeType::Instrument || node.type == NodeType::Effect) {
        menu.addItem(4, "Show Plugin UI");
        menu.addItem(7, "Presets...");
        menu.addItem(8, "MIDI Map...");
        if (node.pluginIndex >= 0)
            menu.addItem(6, "Plugin Info...");
    }
    // Mute / Solo
    menu.addItem(160, node.muted ? "Unmute" : "Mute", true, node.muted);
    menu.addItem(161, node.soloed ? "Unsolo" : "Solo", true, node.soloed);
    menu.addItem(162, "Run Script...");

    // Pan submenu
    {
        juce::PopupMenu panMenu;
        panMenu.addItem(150, "Hard Left",  true, node.pan <= -0.95f);
        panMenu.addItem(151, "Left",       true, std::abs(node.pan - (-0.5f)) < 0.1f);
        panMenu.addItem(152, "Center",     true, std::abs(node.pan) < 0.05f);
        panMenu.addItem(153, "Right",      true, std::abs(node.pan - 0.5f) < 0.1f);
        panMenu.addItem(154, "Hard Right", true, node.pan >= 0.95f);
        menu.addSubMenu("Pan (" + juce::String(node.pan > 0 ? "R " : node.pan < 0 ? "L " : "") +
            juce::String(std::abs((int)(node.pan * 100))) + "%)", panMenu);
    }

    menu.addSeparator();
    if (node.cache.enabled)
        menu.addItem(10, "Unfreeze (disable cache)");
    else
        menu.addItem(10, "Freeze (cache audio)");
    menu.addItem(11, node.cache.autoCache ? "Disable auto-cache" : "Enable auto-cache",
                 true, node.cache.autoCache);
    if (node.cache.valid)
        menu.addItem(-1, juce::String("Cache: valid (") +
            juce::String((int)(node.cache.numSamples / std::max(1.0, node.cache.sampleRate))) +
            "s)", false);

    int nodeId = node.id;
    menu.showMenuAsync(juce::PopupMenu::Options(), [this, nodeId](int result) {
        auto* node = graph.findNode(nodeId);
        if (!node) return;

        if (result == 1) {
            // Delete node and connected links
            std::set<int> pinIds;
            for (auto& p : node->pinsIn) pinIds.insert(p.id);
            for (auto& p : node->pinsOut) pinIds.insert(p.id);
            graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
                [&](auto& l) { return pinIds.count(l.startPin) || pinIds.count(l.endPin); }),
                graph.links.end());
            graph.openEditors.erase(std::remove_if(graph.openEditors.begin(), graph.openEditors.end(),
                [nodeId](auto* e) { return e->id == nodeId; }), graph.openEditors.end());
            graph.nodes.erase(std::remove_if(graph.nodes.begin(), graph.nodes.end(),
                [nodeId](auto& n) { return n.id == nodeId; }), graph.nodes.end());
            graph.dirty = true;
        } else if (result == 2) {
            auto& dup = graph.addNode(node->name, node->type, {}, {},
                {node->pos.x + 50, node->pos.y + 50});
            for (auto& p : node->pinsIn) dup.pinsIn.push_back({graph.getNextId(), p.name, p.kind, true, p.channels});
            for (auto& p : node->pinsOut) dup.pinsOut.push_back({graph.getNextId(), p.name, p.kind, false, p.channels});
            dup.params = node->params;
            dup.clips = node->clips;
        } else if (result == 3) {
            bool already = false;
            for (auto* e : graph.openEditors)
                if (e->id == nodeId) { already = true; break; }
            if (!already)
                graph.openEditors.insert(graph.openEditors.begin(), node);
            graph.activeEditorNodeId = nodeId;
        } else if (result == 4) {
            if (onShowPluginUI) onShowPluginUI(nodeId);
        } else if (result == 6) {
            if (onShowPluginInfo) onShowPluginInfo(nodeId);
        } else if (result == 7) {
            if (onShowPluginPresets) onShowPluginPresets(nodeId);
        } else if (result == 8) {
            if (onShowMidiMap) onShowMidiMap(nodeId);
        } else if (result == 9) {
            node->mpeEnabled = !node->mpeEnabled;
            graph.dirty = true;
        } else if (result == 10) {
            if (node->cache.enabled) {
                node->cache.enabled = false;
                node->cache.clear();
            } else {
                node->cache.enabled = true;
                node->cache.valid = false;
                if (onFreezeNode) onFreezeNode(nodeId);
            }
        } else if (result == 11) {
            node->cache.autoCache = !node->cache.autoCache;
            if (!node->cache.autoCache) node->cache.invalidate();
        } else if (result == 160) {
            node->muted = !node->muted;
            graph.dirty = true;
        } else if (result == 161) {
            node->soloed = !node->soloed;
            graph.dirty = true;
        } else if (result == 162) {
            if (onRunScript) onRunScript(nodeId);
        } else if (result >= 150 && result <= 154) {
            float pans[] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
            node->pan = pans[result - 150];
            graph.dirty = true;
        } else if (result == 5) {
            auto* aw = new juce::AlertWindow("Rename Node", "Enter a new name:",
                                              juce::MessageBoxIconType::NoIcon);
            aw->addTextEditor("name", node->name, "Name:");
            aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
            aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            aw->enterModalState(true, juce::ModalCallbackFunction::create(
                [this, nodeId, aw](int result) {
                    if (result == 1) {
                        auto newName = aw->getTextEditorContents("name").toStdString();
                        if (!newName.empty()) {
                            if (auto* n = graph.findNode(nodeId)) {
                                n->name = newName;
                                graph.dirty = true;
                            }
                        }
                    }
                    delete aw;
                    repaint();
                }), true);
            return; // don't repaint yet — modal dialog handles it
        }
        repaint();
    });
}

bool NodeGraphComponent::keyPressed(const juce::KeyPress& key) {
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) {
        if (selectedLinkId >= 0) {
            deleteSelectedLink();
            return true;
        }
        if (selectedNodeId >= 0) {
            deleteSelectedNode();
            return true;
        }
    }
    return false;
}

void NodeGraphComponent::deleteSelectedLink() {
    if (selectedLinkId < 0) return;
    graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
        [this](auto& l) { return l.id == selectedLinkId; }), graph.links.end());
    graph.dirty = true;
    selectedLinkId = -1;
    repaint();
}

void NodeGraphComponent::deleteSelectedNode() {
    if (selectedNodeId < 0) return;
    auto* node = graph.findNode(selectedNodeId);
    if (!node) return;

    // Remove connected links
    std::set<int> pinIds;
    for (auto& p : node->pinsIn) pinIds.insert(p.id);
    for (auto& p : node->pinsOut) pinIds.insert(p.id);
    graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
        [&](auto& l) { return pinIds.count(l.startPin) || pinIds.count(l.endPin); }),
        graph.links.end());

    // Remove from open editors
    int nid = selectedNodeId;
    graph.openEditors.erase(std::remove_if(graph.openEditors.begin(), graph.openEditors.end(),
        [nid](auto* e) { return e->id == nid; }), graph.openEditors.end());

    // Remove node
    graph.nodes.erase(std::remove_if(graph.nodes.begin(), graph.nodes.end(),
        [nid](auto& n) { return n.id == nid; }), graph.nodes.end());

    graph.dirty = true;
    selectedNodeId = -1;
    repaint();
}

void NodeGraphComponent::showLinkMenu(int linkId) {
    // Find the link
    Link* link = nullptr;
    for (auto& l : graph.links)
        if (l.id == linkId) { link = &l; break; }

    juce::PopupMenu menu;
    menu.addItem(1, "Delete Connection");
    menu.addSeparator();

    // Gain presets
    float currentGain = link ? link->gainDb : 0.0f;
    juce::PopupMenu gainMenu;
    gainMenu.addItem(10, "0 dB (unity)", true, std::abs(currentGain) < 0.1f);
    gainMenu.addItem(11, "-3 dB",  true, std::abs(currentGain - (-3.0f)) < 0.1f);
    gainMenu.addItem(12, "-6 dB",  true, std::abs(currentGain - (-6.0f)) < 0.1f);
    gainMenu.addItem(13, "-12 dB", true, std::abs(currentGain - (-12.0f)) < 0.1f);
    gainMenu.addItem(14, "-20 dB", true, std::abs(currentGain - (-20.0f)) < 0.1f);
    gainMenu.addItem(15, "+3 dB",  true, std::abs(currentGain - 3.0f) < 0.1f);
    gainMenu.addItem(16, "+6 dB",  true, std::abs(currentGain - 6.0f) < 0.1f);
    gainMenu.addItem(17, "Custom...");
    menu.addSubMenu("Gain (" + juce::String(currentGain, 1) + " dB)", gainMenu);

    menu.showMenuAsync(juce::PopupMenu::Options(), [this, linkId](int result) {
        // Find the link again (async)
        Link* lk = nullptr;
        for (auto& l : graph.links)
            if (l.id == linkId) { lk = &l; break; }
        if (!lk && result != 1) return;

        if (result == 1) {
            graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
                [linkId](auto& l) { return l.id == linkId; }), graph.links.end());
            graph.dirty = true;
            selectedLinkId = -1;
        } else if (result >= 10 && result <= 16) {
            float gains[] = {0, -3, -6, -12, -20, 3, 6};
            lk->gainDb = gains[result - 10];
            graph.dirty = true;
        } else if (result == 17 && lk) {
            auto* aw = new juce::AlertWindow("Connection Gain",
                "Enter gain in dB (0 = unity, negative = quieter):",
                juce::MessageBoxIconType::NoIcon);
            aw->addTextEditor("gain", juce::String(lk->gainDb, 1), "dB:");
            aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
            aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            auto lid = linkId;
            aw->enterModalState(true, juce::ModalCallbackFunction::create(
                [this, lid, aw](int res) {
                    if (res == 1) {
                        auto val = aw->getTextEditorContents("gain").getFloatValue();
                        for (auto& l : graph.links)
                            if (l.id == lid) { l.gainDb = juce::jlimit(-60.0f, 24.0f, val); break; }
                        graph.dirty = true;
                    }
                    delete aw;
                    repaint();
                }), true);
            return;
        }
        repaint();
    });
}

} // namespace SoundShop

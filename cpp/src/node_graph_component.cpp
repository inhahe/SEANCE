#include "node_graph_component.h"
#include "music_theory.h"
#include "layered_wave_editor.h"
#include "trigger_node.h"
#include "midi_mod_node.h"
#include "xy_pad.h"
#include "convolution_processor.h"
#include "soundfont_processor.h"
#include "builtin_effects.h"
#include "drum_synth.h"
#include <cmath>

namespace SoundShop {

static const float NODE_WIDTH = 180.0f;
static const float PIN_ROW_HEIGHT = 20.0f;
static const float PIN_RADIUS = 5.0f;
static const float HEADER_HEIGHT = 24.0f;

// Central color definitions for each pin/wire kind. Used by both drawPin
// (dots) and drawLink (cables) so the cable matches the pin it's attached
// to. Param and Signal are intentionally in the same warm (orange/amber)
// family because they're conceptually related — Param = block-rate control,
// Signal = audio-rate control — while Audio (blue) and MIDI (green) are in
// clearly different hue families.
static juce::Colour colourForPinKind(PinKind k) {
    switch (k) {
        case PinKind::Audio:  return juce::Colour(100, 149, 237); // cornflower blue
        case PinKind::Midi:   return juce::Colour( 85, 205,  85); // lime green
        case PinKind::Param:  return juce::Colour(255, 140,  40); // orange (block-rate)
        case PinKind::Signal: return juce::Colour(255, 205,  55); // amber (audio-rate)
    }
    return juce::Colour(200, 200, 200);
}

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
        case NodeType::MidiInput:     return juce::Colour(50, 130, 70); // green — matches MIDI wire color
        default:                      return juce::Colour(80, 80, 80);
    }
}

// ==============================================================================
// Drawing
// ==============================================================================

void NodeGraphComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(25, 25, 30));

    // If we somehow paint before resized() (e.g. unusual layout cascade), do
    // the initial fit here so we never draw nodes at the default zoom/pan.
    if (pendingInitialFit && getWidth() > 0 && getHeight() > 0) {
        if (graph.nodes.size() > 1) fitAll();
        pendingInitialFit = false;
    }

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

        // Pin circle. Normally colored by kind; while a wire-drag is in
        // flight and this pin is the current valid drop target, draw it in
        // bright yellow with an outer halo so the user knows the cursor is
        // close enough to drop.
        bool isHoverDropTarget = (dragMode == DragMode::DragLink)
                              && (pin.id == dragHoverPinId);
        if (isHoverDropTarget) {
            // Outer halo
            g.setColour(juce::Colours::yellow.withAlpha(0.35f));
            g.fillEllipse(pos.x - r * 2, pos.y - r * 2, r * 4, r * 4);
            g.setColour(juce::Colours::yellow);
            g.fillEllipse(pos.x - r * 1.4f, pos.y - r * 1.4f, r * 2.8f, r * 2.8f);
            g.setColour(juce::Colours::white);
            g.drawEllipse(pos.x - r * 1.4f, pos.y - r * 1.4f, r * 2.8f, r * 2.8f, 1.5f);
        } else {
            g.setColour(colourForPinKind(pin.kind));
            g.fillEllipse(pos.x - r, pos.y - r, r * 2, r * 2);
        }

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

    // Parameter rows: drawn below the pins. Each row shows name + value plus
    // a horizontal fill bar indicating position within [min, max]. Drag the
    // row horizontally to change the value (handled in mouseDown/mouseDrag).
    // Signal-controlled params are drawn dimmed and locked.
    bool nodeSignalLocked = graph.hasSignalInput(node.id);
    if (!node.params.empty() && zoom > 0.4f) {
        float paramFontSize = std::max(8.0f, 10.0f * zoom);
        g.setFont(juce::Font(paramFontSize));
        for (int pi = 0; pi < (int)node.params.size(); ++pi) {
            const auto& p = node.params[pi];
            float rowTop    = pinY + 2;
            float rowBottom = pinY + PIN_ROW_HEIGHT - 2;
            auto rowTL = canvasToScreen({bounds.getX() + 6, rowTop});
            auto rowBR = canvasToScreen({bounds.getRight() - 6, rowBottom});
            juce::Rectangle<float> rowRect(rowTL.x, rowTL.y, rowBR.x - rowTL.x, rowBR.y - rowTL.y);

            // Background fill bar showing the param's position within its range.
            float range = std::max(1e-6f, p.maxVal - p.minVal);
            float frac = juce::jlimit(0.0f, 1.0f, (p.value - p.minVal) / range);
            auto fillRect = rowRect;
            fillRect.setWidth(rowRect.getWidth() * frac);

            // Signal-locked params are dimmed (orange fill, no handle)
            if (nodeSignalLocked) {
                g.setColour(juce::Colour(160, 100, 40).withAlpha(0.35f));
                g.fillRoundedRectangle(fillRect, 2.0f);
                g.setColour(juce::Colour(120, 80, 40).withAlpha(0.5f));
                g.drawRoundedRectangle(rowRect, 2.0f, 1.0f);
            } else {
                g.setColour(juce::Colour(80, 110, 160).withAlpha(0.55f));
                g.fillRoundedRectangle(fillRect, 2.0f);
                g.setColour(juce::Colour(60, 80, 120));
                g.drawRoundedRectangle(rowRect, 2.0f, 1.0f);

                // Slider handle: a thin vertical bar at the current value position,
                // brighter so it stands out as the draggable element.
                // Hidden when signal-locked (not draggable).
                float handleX = rowRect.getX() + rowRect.getWidth() * frac;
                float handleW = std::max(2.0f, 3.0f * zoom);
                juce::Rectangle<float> handleRect(handleX - handleW * 0.5f,
                                                  rowRect.getY() - 1.0f,
                                                  handleW,
                                                  rowRect.getHeight() + 2.0f);
                g.setColour(juce::Colours::white);
                g.fillRoundedRectangle(handleRect, 1.0f);
            }

            // Armed indicator: red dot next to the name when armed for auto-write
            if (p.autoWriteArmed) {
                float dotX = rowRect.getX() + 2;
                float dotY = rowRect.getCentreY() - 2;
                g.setColour(juce::Colours::red);
                g.fillEllipse(dotX, dotY, 5.0f, 5.0f);
            }

            // Name (left) and value (right)
            g.setColour(nodeSignalLocked ? juce::Colours::grey : juce::Colours::white);
            auto labelRect = rowRect.reduced(p.autoWriteArmed ? 10.0f : 4.0f, 0.0f);
            g.drawText(p.name, labelRect, juce::Justification::centredLeft, false);
            juce::String valueStr = juce::String(p.value, 2);
            g.drawText(valueStr, rowRect.reduced(4, 0), juce::Justification::centredRight, false);

            pinY += PIN_ROW_HEIGHT;
        }
    }
}

void NodeGraphComponent::drawLink(juce::Graphics& g, Link& link) {
    // Find source and destination pin positions, plus their kinds.
    // The two kinds may differ when an implicit Param↔Signal conversion is
    // in effect — in that case the wire is drawn in two halves, source
    // colour up front and destination colour at the tail, so the user can
    // see the conversion happening visually.
    juce::Point<float> start, end;
    PinKind srcKind = PinKind::Audio;
    PinKind dstKind = PinKind::Audio;
    bool foundSrc = false, foundDst = false;

    for (auto& node : graph.nodes) {
        for (auto& pin : node.pinsOut) {
            if (pin.id == link.startPin) {
                start = canvasToScreen(getPinPosition(node, pin));
                srcKind = pin.kind;
                foundSrc = true;
                break;
            }
        }
        for (auto& pin : node.pinsIn) {
            if (pin.id == link.endPin) {
                end = canvasToScreen(getPinPosition(node, pin));
                dstKind = pin.kind;
                foundDst = true;
                break;
            }
        }
    }
    if (!foundSrc || !foundDst) return;

    // Bézier curve (cubic) with horizontal handles
    float dx = std::abs(end.x - start.x) * 0.5f;
    dx = std::max(dx, 30.0f * zoom);
    juce::Point<float> ctrl1{start.x + dx, start.y};
    juce::Point<float> ctrl2{end.x - dx,   end.y};

    juce::Path path;
    path.startNewSubPath(start);
    path.cubicTo(ctrl1, ctrl2, end);

    // Base alpha — much dimmer when the link is heavily attenuated.
    float baseAlpha = (link.gainDb < -10.0f) ? 0.3f : 0.8f;
    float thickness = ((link.id == selectedLinkId) ? 3.0f : 2.0f) * zoom;

    if (srcKind == dstKind) {
        // Single-kind cable: stroke the full bezier in one colour.
        g.setColour(colourForPinKind(srcKind).withAlpha(baseAlpha));
        g.strokePath(path, juce::PathStrokeType(thickness));
    } else {
        // Mixed-kind cable (currently only Param↔Signal). Stroke the whole
        // bezier in the source colour, then re-stroke a polyline that
        // approximates the tail half in the destination colour. The two
        // halves meet at the bezier midpoint (t=0.5), giving a clean colour
        // change without a gradient. We use de Casteljau / direct evaluation
        // to sample the curve so the polyline tracks the bezier exactly.
        auto bezAt = [&](float t) -> juce::Point<float> {
            float u = 1.0f - t;
            float x = u*u*u*start.x + 3*u*u*t*ctrl1.x + 3*u*t*t*ctrl2.x + t*t*t*end.x;
            float y = u*u*u*start.y + 3*u*u*t*ctrl1.y + 3*u*t*t*ctrl2.y + t*t*t*end.y;
            return {x, y};
        };

        g.setColour(colourForPinKind(srcKind).withAlpha(baseAlpha));
        g.strokePath(path, juce::PathStrokeType(thickness));

        // Sample the tail half (t in [0.5, 1.0]) as a smooth polyline.
        const int tailSegments = 20;
        juce::Path tail;
        tail.startNewSubPath(bezAt(0.5f));
        for (int i = 1; i <= tailSegments; ++i) {
            float t = 0.5f + 0.5f * (float)i / (float)tailSegments;
            tail.lineTo(bezAt(t));
        }
        g.setColour(colourForPinKind(dstKind).withAlpha(baseAlpha));
        g.strokePath(tail, juce::PathStrokeType(thickness));
    }

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

    // Wire tags: colored shapes along the cable to identify individual wires
    // and show group membership. Only drawn when zoomed in enough to read.
    if (zoom > 0.35f) {
        // Evaluate a point on the cubic Bézier at parameter t.
        // Control points: P0=start, P1=(start.x+dx, start.y),
        //                 P2=(end.x-dx, end.y), P3=end.
        auto bezierAt = [&](float t) -> juce::Point<float> {
            float u = 1.0f - t;
            float x = u*u*u*start.x + 3*u*u*t*(start.x+dx) + 3*u*t*t*(end.x-dx) + t*t*t*end.x;
            float y = u*u*u*start.y + 3*u*u*t*start.y      + 3*u*t*t*end.y       + t*t*t*end.y;
            return {x, y};
        };

        float tagR = std::max(4.0f, 5.0f * zoom); // tag radius
        float t = 0.35f; // starting position along the cable

        // --- Circle tag: individual wire identity ---
        {
            uint32_t col = getDistinctColor(link.id);
            auto pos = bezierAt(t);
            g.setColour(juce::Colour((uint8_t)((col >> 16) & 0xFF),
                                     (uint8_t)((col >> 8) & 0xFF),
                                     (uint8_t)(col & 0xFF)));
            g.fillEllipse(pos.x - tagR, pos.y - tagR, tagR * 2, tagR * 2);
            g.setColour(juce::Colours::white.withAlpha(0.8f));
            g.drawEllipse(pos.x - tagR, pos.y - tagR, tagR * 2, tagR * 2, 1.0f);
            t += 0.12f;
        }

        // --- Diamond tags: one per group this link belongs to ---
        for (const auto& grp : graph.effectGroups) {
            bool inGroup = false;
            for (int lid : grp.linkIds)
                if (lid == link.id) { inGroup = true; break; }
            if (!inGroup) continue;

            auto pos = bezierAt(std::min(t, 0.85f));
            uint32_t col = grp.color;
            g.setColour(juce::Colour((uint8_t)((col >> 16) & 0xFF),
                                     (uint8_t)((col >> 8) & 0xFF),
                                     (uint8_t)(col & 0xFF)));
            // Diamond: rotated square
            juce::Path diamond;
            diamond.startNewSubPath(pos.x, pos.y - tagR);
            diamond.lineTo(pos.x + tagR, pos.y);
            diamond.lineTo(pos.x, pos.y + tagR);
            diamond.lineTo(pos.x - tagR, pos.y);
            diamond.closeSubPath();
            g.fillPath(diamond);
            g.setColour(juce::Colours::white.withAlpha(0.8f));
            g.strokePath(diamond, juce::PathStrokeType(1.0f));

            // Optional: group name label next to the diamond (if named)
            if (!grp.name.empty() && zoom > 0.6f) {
                g.setColour(juce::Colours::white);
                g.setFont(juce::Font(std::max(7.0f, 9.0f * zoom)));
                g.drawText(grp.name, (int)(pos.x + tagR + 2), (int)(pos.y - 6),
                           80, 12, juce::Justification::centredLeft, false);
            }

            t += 0.12f;
        }
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
        if (node) {
            // Check if right-click is on a param row — show arm/disarm menu
            if (!node->params.empty()) {
                auto bounds = getNodeBounds(*node);
                int maxPins = std::max((int)node->pinsIn.size(), (int)node->pinsOut.size());
                float paramRowsTop = bounds.getY() + HEADER_HEIGHT + maxPins * PIN_ROW_HEIGHT;
                if (canvasPos.y >= paramRowsTop) {
                    int idx = (int)((canvasPos.y - paramRowsTop) / PIN_ROW_HEIGHT);
                    if (idx >= 0 && idx < (int)node->params.size()) {
                        auto& p = node->params[idx];
                        juce::PopupMenu pm;
                        pm.addItem(1, p.autoWriteArmed ? "Disarm for Auto-Write" : "Arm for Auto-Write");
                        pm.addItem(2, "Arm All on This Node");
                        pm.addItem(3, "Disarm All on This Node");
                        pm.addItem(4, "Reset to Default (double-click)");
                        int nodeId = node->id;
                        int paramIdx = idx;
                        pm.showMenuAsync({}, [this, nodeId, paramIdx](int r) {
                            auto* nd = graph.findNode(nodeId);
                            if (!nd) return;
                            if (r == 1 && paramIdx < (int)nd->params.size())
                                nd->params[paramIdx].autoWriteArmed = !nd->params[paramIdx].autoWriteArmed;
                            else if (r == 2)
                                graph.armNodeParams(nodeId, true);
                            else if (r == 3)
                                graph.armNodeParams(nodeId, false);
                            else if (r == 4 && paramIdx < (int)nd->params.size()) {
                                auto& p2 = nd->params[paramIdx];
                                p2.value = (p2.minVal + p2.maxVal) * 0.5f;
                            }
                            repaint();
                        });
                        return;
                    }
                }
            }
            showNodeMenu(*node);
        } else
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
        // Check if click landed on a param row inside the node — if so,
        // start a horizontal slider interaction (jump-to-click + drag).
        // Signal-controlled nodes have their params locked — no dragging.
        if (!node->params.empty() && !graph.hasSignalInput(node->id)) {
            auto bounds = getNodeBounds(*node);
            int maxPins = std::max((int)node->pinsIn.size(), (int)node->pinsOut.size());
            float paramRowsTop = bounds.getY() + HEADER_HEIGHT + maxPins * PIN_ROW_HEIGHT;
            float paramRowsLeft  = bounds.getX() + 6;
            float paramRowsRight = bounds.getRight() - 6;
            if (canvasPos.x >= paramRowsLeft && canvasPos.x <= paramRowsRight
                && canvasPos.y >= paramRowsTop)
            {
                int idx = (int)((canvasPos.y - paramRowsTop) / PIN_ROW_HEIGHT);
                if (idx >= 0 && idx < (int)node->params.size()) {
                    auto& p = node->params[idx];
                    dragMode = DragMode::DragParam;
                    dragNodeId = node->id;
                    dragParamIdx = idx;
                    dragParamStartValue = p.value;
                    dragParamLeftX = paramRowsLeft;
                    dragParamWidth = paramRowsRight - paramRowsLeft;
                    dragStart = e.position;
                    selectedNodeId = node->id;
                    // Jump to the clicked position immediately.
                    float frac = juce::jlimit(0.0f, 1.0f,
                                              (canvasPos.x - dragParamLeftX) / std::max(1.0f, dragParamWidth));
                    p.value = p.minVal + frac * (p.maxVal - p.minVal);
                    graph.dirty = true;
                    repaint();
                    return;
                }
            }
        }
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
        // Track which pin we're hovering over so drawPin() can highlight it.
        // Valid drop target requires:
        //  1. opposite direction from the source (output→input or vice versa)
        //  2. not the same pin we started dragging from
        //  3. compatible pin kinds (audio↔audio, MIDI↔MIDI, or any control↔
        //     control mix; see arePinKindsCompatible). Param↔Signal is
        //     deliberately treated as compatible — implicit conversion lets
        //     either control kind drive either control input.
        auto canvasPos = screenToCanvas(e.position);
        bool isOut = false;
        int hovered = pinAtPoint(canvasPos, isOut);
        bool valid = false;
        if (hovered >= 0 && hovered != dragPinId && isOut != dragPinIsOutput) {
            // Look up both pins' kinds and check compatibility
            PinKind srcKind = PinKind::Audio, dstKind = PinKind::Audio;
            bool gotSrc = false, gotDst = false;
            for (auto& node : graph.nodes) {
                for (auto& pin : node.pinsIn) {
                    if (pin.id == dragPinId)  { srcKind = pin.kind; gotSrc = true; }
                    if (pin.id == hovered)    { dstKind = pin.kind; gotDst = true; }
                }
                for (auto& pin : node.pinsOut) {
                    if (pin.id == dragPinId)  { srcKind = pin.kind; gotSrc = true; }
                    if (pin.id == hovered)    { dstKind = pin.kind; gotDst = true; }
                }
            }
            valid = gotSrc && gotDst && arePinKindsCompatible(srcKind, dstKind);
        }
        dragHoverPinId = valid ? hovered : -1;
        repaint();
    } else if (dragMode == DragMode::DragParam) {
        auto* node = graph.findNode(dragNodeId);
        if (node && dragParamIdx >= 0 && dragParamIdx < (int)node->params.size()) {
            auto& p = node->params[dragParamIdx];
            // Horizontal drag: map the cursor's absolute canvas-x onto the
            // slider's track. The cursor can travel anywhere; we clamp to the
            // track's range.
            auto canvasPos = screenToCanvas(e.position);
            float frac = juce::jlimit(0.0f, 1.0f,
                                      (canvasPos.x - dragParamLeftX) / std::max(1.0f, dragParamWidth));
            p.value = p.minVal + frac * (p.maxVal - p.minVal);
            graph.dirty = true;
            // No graph rebuild here — processBlock reads param values fresh
            // every callback via getParam, so the new value is picked up on
            // the next audio block automatically. Calling requestRebuild on
            // every drag tick races JUCE's async graph rebuild and crashes.
            repaint();
        }
    }
}

void NodeGraphComponent::mouseUp(const juce::MouseEvent& e) {
    if (dragMode == DragMode::DragLink) {
        // Check if dropped on a pin. Same compatibility rules as the hover
        // highlight (mouseDrag): direction must flip, kinds must be
        // compatible (Param↔Signal counts as compatible).
        auto canvasPos = screenToCanvas(e.position);
        bool isOut;
        int targetPin = pinAtPoint(canvasPos, isOut);
        if (targetPin >= 0 && isOut != dragPinIsOutput && targetPin != dragPinId) {
            PinKind srcKind = PinKind::Audio, dstKind = PinKind::Audio;
            bool gotSrc = false, gotDst = false;
            for (auto& node : graph.nodes) {
                for (auto& pin : node.pinsIn) {
                    if (pin.id == dragPinId) { srcKind = pin.kind; gotSrc = true; }
                    if (pin.id == targetPin) { dstKind = pin.kind; gotDst = true; }
                }
                for (auto& pin : node.pinsOut) {
                    if (pin.id == dragPinId) { srcKind = pin.kind; gotSrc = true; }
                    if (pin.id == targetPin) { dstKind = pin.kind; gotDst = true; }
                }
            }
            if (gotSrc && gotDst && arePinKindsCompatible(srcKind, dstKind)) {
                int outPin = dragPinIsOutput ? dragPinId : targetPin;
                int inPin  = dragPinIsOutput ? targetPin : dragPinId;
                graph.addLink(outPin, inPin);
            }
        }
    }
    dragMode = DragMode::None;
    dragNodeId = -1;
    dragParamIdx = -1;
    dragHoverPinId = -1;
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

    // Double-click a param row = reset to default (midpoint of range).
    // Standard DAW convention for "return to center."
    if (!node->params.empty() && !graph.hasSignalInput(node->id)) {
        auto bounds = getNodeBounds(*node);
        int maxPins = std::max((int)node->pinsIn.size(), (int)node->pinsOut.size());
        float paramRowsTop = bounds.getY() + HEADER_HEIGHT + maxPins * PIN_ROW_HEIGHT;
        float paramRowsLeft  = bounds.getX() + 6;
        float paramRowsRight = bounds.getRight() - 6;
        if (canvasPos.x >= paramRowsLeft && canvasPos.x <= paramRowsRight
            && canvasPos.y >= paramRowsTop)
        {
            int idx = (int)((canvasPos.y - paramRowsTop) / PIN_ROW_HEIGHT);
            if (idx >= 0 && idx < (int)node->params.size()) {
                auto& p = node->params[idx];
                // Reset to midpoint of range (center for Pan, default for others)
                p.value = (p.minVal + p.maxVal) * 0.5f;
                graph.dirty = true;
                repaint();
                return;
            }
        }
    }

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

void NodeGraphComponent::resized() {
    // Run the initial fit-all the first time we get a real (non-zero) size,
    // so the very first paint already shows the graph centered at the right
    // zoom — no visible zoom-in jitter on project load. Subsequent resizes
    // (window-resize, panel splits, etc.) leave the user's view alone so we
    // don't clobber any manual pan/zoom they've done.
    if (pendingInitialFit && getWidth() > 0 && getHeight() > 0) {
        if (graph.nodes.size() > 1) {
            fitAll();
        }
        pendingInitialFit = false;
    }
}

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
    synthMenu.addItem(110, "Waveform");
    synthMenu.addItem(115, "Frequency Domain...");
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
    instMenu.addItem(102, "Sampler");
    instMenu.addItem(104, "SoundFont (.sf2)...");
    instMenu.addItem(105, "SFZ Instrument (.sfz)...");
    instMenu.addItem(103, "Drum Machine");
    instMenu.addItem(106, "Analog Drum Synth");
    menu.addSubMenu("Instruments", instMenu);

    juce::PopupMenu fxMenu;
    fxMenu.addItem(206, "Pitch Shift / Time Stretch");
    fxMenu.addSeparator();
    fxMenu.addItem(207, "Convolution Filter");
    fxMenu.addSeparator();
    fxMenu.addItem(208, "Tremolo");
    fxMenu.addItem(209, "Vibrato");
    fxMenu.addItem(210, "Flanger");
    fxMenu.addItem(211, "Phaser");
    fxMenu.addItem(212, "Echo");
    fxMenu.addSeparator();
    fxMenu.addItem(213, "Compressor");
    fxMenu.addItem(214, "Limiter");
    fxMenu.addItem(215, "Gate");
    fxMenu.addSeparator();
    fxMenu.addItem(216, "Arpeggiator");
    fxMenu.addItem(218, "Mixture (organ harmonics)");
    fxMenu.addItem(219, "Trigger (MIDI / signal)");
    fxMenu.addItem(220, "MIDI Modulator (signal -> MIDI)");
    fxMenu.addSeparator();
    fxMenu.addItem(217, "3D Spatializer (binaural)");
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
    sigMenu.addSeparator();
    sigMenu.addItem(133, "XY Pad");
    sigMenu.addItem(134, "Spectrum Tap");
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

        // Nudge `p` if it overlaps an existing node. Uses actual node bounds
        // (which vary by param count) and nudges by full node width + padding.
        {
            const float PAD = 30.0f;
            auto overlapsAnyNode = [&](float x, float y) {
                // Estimate the new node's height (header + 1 pin row + rough param guess)
                float newH = HEADER_HEIGHT + PIN_ROW_HEIGHT + 8;
                for (auto& n : graph.nodes) {
                    auto bounds = getNodeBounds(n);
                    // Check if the proposed rect overlaps this node's rect
                    if (x < bounds.getRight() + PAD && x + NODE_WIDTH + PAD > bounds.getX() &&
                        y < bounds.getBottom() + PAD && y + newH + PAD > bounds.getY())
                        return true;
                }
                return false;
            };
            int safety = 50;
            while (overlapsAnyNode(p.x, p.y) && safety-- > 0) {
                p.x += NODE_WIDTH + PAD;
            }
        }

        if (result == 1) {
            auto& n = graph.addNode("MIDI Track", NodeType::MidiTimeline,
                {Pin{0, "MIDI In", PinKind::Midi, true}},
                {Pin{0, "MIDI", PinKind::Midi, false}}, {p.x, p.y});
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
        } else if (result == 110 || result == 115) {
            // Built-in synth (unified: uses TerrainSynthProcessor)
            // 110 = Layered Waveform editor, 115 = Frequency Domain (spectral)
            const char* nodeName = (result == 110) ? "Waveform Synth" : "Spectral Synth";
            // The Waveform Synth is strictly 1D, so Sig X / Sig Y inputs
            // (which modulate a 2D terrain traversal position) are meaningless
            // and omitted. Spectral Synth also produces a 1D waveform, so
            // same logic. Only Terrain Synth nodes still expose them.
            auto& n = graph.addNode(nodeName, NodeType::Instrument,
                {Pin{0, "MIDI", PinKind::Midi, true}},
                {Pin{0, "Audio", PinKind::Audio, false}}, {p.x, p.y});
            // Default script depends on which menu item fired
            if (result == 110)
                n.script = WavetableDoc::defaultSingleSine().encode();
            else
                n.script = ""; // spectral will be filled in by the dialog below

            // Compact param list: only the controls that actually do something
            // for a 1D Waveform/Spectral synth. Terrain-traversal params (Speed,
            // Radius, Center, Traversal mode, LFOs, Grain) are omitted since
            // they're meaningless for 1D playback. TerrainSynthProcessor reads
            // missing params via getParam's default-fallback path, and the
            // Position param is now looked up by name so its index doesn't
            // matter.
            n.params.push_back({"Attack",   0.01f, 0.001f, 2.0f});
            n.params.push_back({"Decay",    0.1f,  0.001f, 2.0f});
            n.params.push_back({"Sustain",  0.7f,  0.0f,   1.0f});
            n.params.push_back({"Release",  0.3f,  0.001f, 5.0f});
            n.params.push_back({"Volume",   0.5f,  0.0f,   1.0f});
            n.params.push_back({"Pan",      0.0f, -1.0f,   1.0f});
            // Velocity sensitivity: 0 = ignore velocity (every note at full
            // volume), 1 = linear response (default, v/127 gain).
            n.params.push_back({"Vel Sens", 1.0f,  0.0f,   1.0f});
            // Mod-wheel vibrato depth (0 = disable the default behavior so
            // the user can MIDI-Learn CC1 to a different param instead).
            n.params.push_back({"Vibrato",  1.0f,  0.0f,   1.0f});
            // Wavetable Position — meaningful when there are multiple frames.
            // Looked up by name in TerrainSynthProcessor, so list order is free.
            n.params.push_back({"Position", 0.0f,  0.0f,   1.0f});

            if (result == 110) {
                // Waveform — open the layered waveform editor immediately.
                auto nodeId = n.id;
                auto* editor = new LayeredWaveEditorComponent(graph, nodeId, [this]() {
                    if (onNodeEdited) onNodeEdited();
                    repaint();
                });
                juce::DialogWindow::LaunchOptions opts;
                opts.content.setOwned(editor);
                opts.dialogTitle = "Waveform: " + juce::String(n.name);
                opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
                opts.escapeKeyTriggersCloseButton = true;
                opts.useNativeTitleBar = true;
                opts.resizable = true;
                opts.launchAsync();
                (void)nodeId;
                return;
            } else if (result == 115) {
                // Frequency Domain (spectral) — prompt for mag and phase expressions.
                auto nodeId = n.id;
                auto* aw = new juce::AlertWindow("Frequency Domain Synth",
                    "Define the sound's spectrum. `f` is the frequency bin index.\n\n"
                    "Magnitude examples:\n"
                    "  exp(-f/20)                  — dark, natural decay\n"
                    "  1/(f+1)                     — sawtooth-like\n"
                    "  exp(-((f-30)^2)/40)         — single formant bump\n"
                    "  sin(f*0.3) + 0.5*cos(f*0.1) — layered slow ripples\n\n"
                    "Phase defaults to random (natural noise-like) — change to `0`\n"
                    "for an impulsive clicky attack, or write an expression in `f`.\n\n"
                    "Functions: sin, cos, exp, log, sqrt, pow, abs, tanh, clamp, noise",
                    juce::MessageBoxIconType::NoIcon);
                aw->addTextEditor("mag",   "exp(-f/20)", "Magnitude mag(f):");
                aw->addTextEditor("phase", "random",     "Phase phase(f):");
                aw->addComboBox("fftsize", {"512", "1024", "2048", "4096"}, "FFT size:");
                if (auto* cb = aw->getComboBoxComponent("fftsize")) cb->setSelectedItemIndex(2);
                aw->addComboBox("phasemode", {"Expression", "Random", "Zero (clicky)", "Linear"}, "Phase mode:");
                if (auto* cb = aw->getComboBoxComponent("phasemode")) cb->setSelectedItemIndex(1);
                aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
                aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
                aw->enterModalState(true, juce::ModalCallbackFunction::create(
                    [this, nodeId, aw](int result) {
                        if (result == 1) {
                            auto mag   = aw->getTextEditorContents("mag").toStdString();
                            auto phase = aw->getTextEditorContents("phase").toStdString();
                            int fftIdx = 2, phaseIdx = 1;
                            if (auto* cb = aw->getComboBoxComponent("fftsize"))
                                fftIdx = cb->getSelectedItemIndex();
                            if (auto* cb = aw->getComboBoxComponent("phasemode"))
                                phaseIdx = cb->getSelectedItemIndex();
                            int fftSize = (fftIdx == 0) ? 512 : (fftIdx == 1) ? 1024
                                        : (fftIdx == 2) ? 2048 : 4096;
                            if (auto* nd = graph.findNode(nodeId)) {
                                nd->script = "__spectral__:" + std::to_string(fftSize) +
                                             ":" + std::to_string(phaseIdx) +
                                             ":" + mag + "|" + phase;
                            }
                        }
                        delete aw;
                        repaint();
                    }), true);
                return;
            }
        } else if (result == 133) {
            // XY Pad: a signal-generating node with X/Y/Z outputs that can
            // be wired through the graph. Also has a fast-path dropdown to
            // directly control any param without wiring.
            auto& n = graph.addNode("XY Pad", NodeType::SignalShape,
                {},  // no inputs
                {Pin{0, "X Out", PinKind::Signal, false, 1},
                 Pin{0, "Y Out", PinKind::Signal, false, 1},
                 Pin{0, "Z Out", PinKind::Signal, false, 1}},
                {p.x, p.y});
            n.script = "__xypad__";
            n.params.push_back({"X", 0.5f, 0.0f, 1.0f});
            n.params.push_back({"Y", 0.5f, 0.0f, 1.0f});
            n.params.push_back({"Z", 0.5f, 0.0f, 1.0f});
            // Open the pad window immediately
            {
                auto* pad = new XYPadComponent(graph, n.id);
                juce::DialogWindow::LaunchOptions opts;
                opts.content.setOwned(pad);
                opts.dialogTitle = "XY Pad";
                opts.dialogBackgroundColour = juce::Colour(25, 25, 32);
                opts.escapeKeyTriggersCloseButton = true;
                opts.useNativeTitleBar = true;
                opts.resizable = true;
                opts.launchAsync();
            }
            return;
        } else if (result == 134) {
            // Spectrum Tap — insert inline on audio for frequency analysis.
            // Has audio in/out (passthrough) and user-defined frequency bins.
            auto& n = graph.addNode("Spectrum Tap", NodeType::Effect,
                {Pin{0, "Audio In", PinKind::Audio, true}},
                {Pin{0, "Audio Out", PinKind::Audio, false}},
                {p.x, p.y});
            n.script = "__spectrumtap__";
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
                n.params.push_back({"Pan",      0.0f, -1.0f,   1.0f});
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
                    n.params.push_back({"Pan",          0.0f,  -1.0f,   1.0f});
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
        } else if (result == 104 || result == 105) {
            // SoundFont (.sf2) or SFZ instrument — file chooser
            juce::String filter = (result == 104) ? "*.sf2" : "*.sfz";
            juce::String title = (result == 104) ? "Load SoundFont (.sf2)" : "Load SFZ Instrument (.sfz)";
            auto canvasPos = p;
            auto chooser = std::make_shared<juce::FileChooser>(title, juce::File(), filter);
            chooser->launchAsync(juce::FileBrowserComponent::openMode,
                [this, canvasPos, chooser, result](const juce::FileChooser& fc) {
                    auto file = fc.getResult();
                    if (!file.existsAsFile()) return;
                    auto name = file.getFileNameWithoutExtension().toStdString();
                    auto& n = graph.addNode(name, NodeType::Instrument,
                        {Pin{0, "MIDI", PinKind::Midi, true}},
                        {Pin{0, "Audio", PinKind::Audio, false}},
                        {canvasPos.x, canvasPos.y});
                    if (result == 104)
                        n.script = "__sf2__:" + file.getFullPathName().toStdString();
                    else
                        n.script = "__sfz__:" + file.getFullPathName().toStdString();
                    n.params.push_back({"Attack",   0.01f, 0.001f, 2.0f});
                    n.params.push_back({"Decay",    0.1f,  0.001f, 2.0f});
                    n.params.push_back({"Sustain",  0.7f,  0.0f,   1.0f});
                    n.params.push_back({"Release",  0.3f,  0.001f, 5.0f});
                    n.params.push_back({"Volume",   0.5f,  0.0f,   1.0f});
                    n.params.push_back({"Pan",      0.0f, -1.0f,   1.0f});
                    n.params.push_back({"Vel Sens", 1.0f,  0.0f,   1.0f});
                    if (result == 104)
                        n.params.push_back({"Preset", 0.0f, 0.0f, 127.0f});
                    repaint();
                });
            return;
        } else if (result == 106) {
            // Analog Drum Synth
            auto& n = graph.addNode("Drum Synth", NodeType::Instrument,
                {Pin{0, "MIDI", PinKind::Midi, true}},
                {Pin{0, "Audio", PinKind::Audio, false}}, {p.x, p.y});
            n.script = "__drumsynth__";
            n.params.push_back({"Volume",   0.5f, 0.0f, 1.0f});
            n.params.push_back({"Pan",      0.0f, -1.0f, 1.0f});
            n.params.push_back({"Vel Sens", 1.0f, 0.0f, 1.0f});
            // The DrumSynthProcessor will populate per-sound params on construction
        } else if (result == 100 || result == 103) {
            // Piano and Drum Machine: functional defaults that route through
            // TerrainSynthProcessor, so they don't crash and give the user
            // something playable immediately. Each gets the full param list.
            const char* nodeName = (result == 100) ? "Piano" : "Drum Machine";
            // 1D layered waveforms — no Sig X/Y (meaningless for 1D).
            auto& n = graph.addNode(nodeName, NodeType::Instrument,
                {Pin{0, "MIDI", PinKind::Midi, true}},
                {Pin{0, "Audio", PinKind::Audio, false}}, {p.x, p.y});

            // Default scripts — layered waveforms with per-instrument character
            if (result == 100) {
                // Piano: fundamental + a few decaying harmonics for a mellow tone
                LayeredWaveform lw;
                WaveLayer l1; l1.shape = WaveLayer::Sine;     l1.ratio = 1; l1.amp = 1.0f;  lw.layers.push_back(l1);
                WaveLayer l2; l2.shape = WaveLayer::Sine;     l2.ratio = 2; l2.amp = 0.5f;  lw.layers.push_back(l2);
                WaveLayer l3; l3.shape = WaveLayer::Sine;     l3.ratio = 3; l3.amp = 0.25f; lw.layers.push_back(l3);
                WaveLayer l4; l4.shape = WaveLayer::Triangle; l4.ratio = 1; l4.amp = 0.2f;  lw.layers.push_back(l4);
                n.script = lw.encode();
            } else {
                // Drum Machine: noise layer + low-frequency body
                LayeredWaveform lw;
                WaveLayer l1; l1.shape = WaveLayer::Noise; l1.ratio = 1; l1.amp = 1.0f; lw.layers.push_back(l1);
                WaveLayer l2; l2.shape = WaveLayer::Sine;  l2.ratio = 1; l2.amp = 0.6f; lw.layers.push_back(l2);
                n.script = lw.encode();
            }

            // Compact param list (same rationale as Waveform Synth above).
            n.params.push_back({"Attack",  (result == 100) ? 0.005f : 0.001f, 0.001f, 2.0f});
            n.params.push_back({"Decay",   (result == 100) ? 0.3f   : 0.1f,   0.001f, 2.0f});
            n.params.push_back({"Sustain", (result == 100) ? 0.5f   : 0.0f,   0.0f,   1.0f});
            n.params.push_back({"Release", (result == 100) ? 0.5f   : 0.1f,   0.001f, 5.0f});
            n.params.push_back({"Volume",  0.5f, 0.0f, 1.0f});
            n.params.push_back({"Pan",     0.0f, -1.0f, 1.0f});
        } else if (result == 206) {
            auto& n = graph.addNode("Pitch Shift", NodeType::Effect,
                {Pin{0, "Audio In", PinKind::Audio, true}},
                {Pin{0, "Audio Out", PinKind::Audio, false}}, {p.x, p.y});
            n.script = "__pitchshift__";
            n.params.push_back({"Pitch (semi)", 0.0f, -24.0f, 24.0f});
            n.params.push_back({"Time Ratio",   1.0f, 0.25f,  4.0f});
            n.params.push_back({"Formant",       1.0f, 0.0f,   1.0f});
        } else if (result == 207) {
            // Convolution Filter
            auto& n = graph.addNode("Convolution", NodeType::Effect,
                {Pin{0, "Audio In", PinKind::Audio, true}},
                {Pin{0, "Audio Out", PinKind::Audio, false}}, {p.x, p.y});
            n.script = ConvolutionProcessor::encodeIR({1.0f}); // identity (passthrough)
        } else if (result >= 208 && result <= 216) {
            // Built-in effects with real DSP
            auto makeEffect = [&](const char* name, const char* script,
                                   std::vector<Param> params, bool midiIO = false) -> Node& {
                auto& n = graph.addNode(name, NodeType::Effect,
                    {Pin{0, midiIO ? "MIDI In" : "Audio In",
                         midiIO ? PinKind::Midi : PinKind::Audio, true}},
                    {Pin{0, midiIO ? "MIDI Out" : "Audio Out",
                         midiIO ? PinKind::Midi : PinKind::Audio, false}},
                    {p.x, p.y});
                if (midiIO) {
                    // Arpeggiator needs both MIDI and audio pins
                    n.pinsIn.clear(); n.pinsOut.clear();
                    n.pinsIn.push_back({graph.getNextId(), "MIDI In", PinKind::Midi, true});
                    n.pinsOut.push_back({graph.getNextId(), "MIDI Out", PinKind::Midi, false});
                }
                n.script = script;
                n.params = std::move(params);
                return n;
            };
            switch (result) {
                case 208: makeEffect("Tremolo", "__tremolo__", {
                    {"Rate", 4.0f, 0.1f, 20.0f},
                    {"Depth", 0.5f, 0.0f, 1.0f},
                    {"Shape", 0.0f, 0.0f, 2.0f}, // 0=sine, 1=square, 2=triangle
                }); break;
                case 209: makeEffect("Vibrato", "__vibrato__", {
                    {"Rate", 5.0f, 0.1f, 15.0f},
                    {"Depth", 0.3f, 0.0f, 2.0f}, // semitones
                }); break;
                case 210: makeEffect("Flanger", "__flanger__", {
                    {"Rate", 0.3f, 0.01f, 5.0f},
                    {"Depth", 0.7f, 0.0f, 1.0f},
                    {"Feedback", 0.5f, 0.0f, 0.95f},
                    {"Mix", 0.5f, 0.0f, 1.0f},
                }); break;
                case 211: makeEffect("Phaser", "__phaser__", {
                    {"Rate", 0.5f, 0.01f, 5.0f},
                    {"Depth", 0.7f, 0.0f, 1.0f},
                    {"Feedback", 0.3f, 0.0f, 0.95f},
                    {"Stages", 6.0f, 2.0f, 12.0f},
                }); break;
                case 212: makeEffect("Echo", "__echo__", {
                    {"Delay", 300.0f, 10.0f, 2000.0f},
                    {"Feedback", 0.5f, 0.0f, 0.95f},
                    {"Mix", 0.4f, 0.0f, 1.0f},
                }); break;
                case 213: makeEffect("Compressor", "__compressor__", {
                    {"Threshold", -20.0f, -60.0f, 0.0f},
                    {"Ratio", 4.0f, 1.0f, 20.0f},
                    {"Attack", 10.0f, 0.1f, 100.0f},
                    {"Release", 100.0f, 10.0f, 1000.0f},
                    {"Makeup Gain", 0.0f, 0.0f, 30.0f},
                }); break;
                case 214: makeEffect("Limiter", "__limiter__", {
                    {"Ceiling", -0.3f, -20.0f, 0.0f},
                    {"Release", 50.0f, 5.0f, 500.0f},
                }); break;
                case 215: makeEffect("Gate", "__gate__", {
                    {"Threshold", -40.0f, -80.0f, 0.0f},
                    {"Attack", 1.0f, 0.1f, 50.0f},
                    {"Release", 50.0f, 5.0f, 500.0f},
                }); break;
                case 218: makeEffect("Mixture", "__mixture__", {
                {"Octaves", 2.0f, 1.0f, 4.0f},
                {"Include Fifths", 1.0f, 0.0f, 1.0f},
                {"Include Thirds", 0.0f, 0.0f, 1.0f},
                {"Level Decay", 0.5f, 0.0f, 0.95f},
            }, true); break;
            case 216: makeEffect("Arpeggiator", "__arpeggiator__", {
                    {"Rate", 8.0f, 1.0f, 32.0f},
                    {"Pattern", 0.0f, 0.0f, 3.0f}, // 0=up, 1=down, 2=updown, 3=random
                    {"Octaves", 1.0f, 1.0f, 4.0f},
                }, true); break;
            case 220: {
                // MIDI Modulator — MIDI in + N Signal ins -> MIDI out.
                // Default: one velocity-scaling rule with one Signal input.
                // The editor lets the user add more inputs, each targeting
                // a different MIDI attribute.
                auto& n = graph.addNode("MIDI Mod", NodeType::Effect,
                    {}, {}, {p.x, p.y});
                n.pinsIn.clear();
                n.pinsOut.clear();
                n.pinsIn.push_back({graph.getNextId(),  "MIDI In",  PinKind::Midi,   true});
                n.pinsIn.push_back({graph.getNextId(),  "Sig 1",    PinKind::Signal, true, 1});
                n.pinsOut.push_back({graph.getNextId(), "MIDI Out", PinKind::Midi,   false});
                n.script = MidiModDoc::defaultDoc().encode();
                break;
            }
            case 219: {
                // Trigger node — MIDI in, MIDI out + Signal out.
                // Uses the Effect node type but has two distinct output pins
                // (one MIDI, one Signal) rather than the usual MIDI-only or
                // audio-only effect layout.
                auto& n = graph.addNode("Trigger", NodeType::Effect,
                    {}, {}, {p.x, p.y});
                n.pinsIn.clear();
                n.pinsOut.clear();
                n.pinsIn.push_back({graph.getNextId(),  "MIDI In",    PinKind::Midi,   true});
                n.pinsOut.push_back({graph.getNextId(), "MIDI Out",   PinKind::Midi,   false});
                n.pinsOut.push_back({graph.getNextId(), "Signal Out", PinKind::Signal, false});
                // Seed with a sensible default doc so the node does something
                // on first placement.
                n.script = TriggerDoc::defaultDoc().encode();
                break;
            }
            }
        } else if (result == 217) {
            // 3D Spatializer
            auto& n = graph.addNode("3D Spatializer", NodeType::Effect,
                {Pin{0, "Audio In", PinKind::Audio, true}},
                {Pin{0, "Audio Out", PinKind::Audio, false}}, {p.x, p.y});
            n.script = "__spatializer3d__";
            n.params.push_back({"Azimuth", 0.0f, -180.0f, 180.0f});
            n.params.push_back({"Elevation", 0.0f, -90.0f, 90.0f});
            n.params.push_back({"Distance", 0.5f, 0.0f, 1.0f});
        } else if (result >= 200 && result < 206) {
            // Legacy stub effects (kept for backward compatibility with old projects)
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
            if (onNodeDeleted) onNodeDeleted(nodeId);
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

    // Close any live editor panels for this node before removing it,
    // so they don't hold dangling references.
    int nid = selectedNodeId;
    if (onNodeDeleted) onNodeDeleted(nid);

    // Remove from open editors
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

    // Effect Group membership
    {
        juce::PopupMenu grpMenu;
        grpMenu.addItem(30, "New Group...");
        if (!graph.effectGroups.empty()) grpMenu.addSeparator();
        for (auto& grp : graph.effectGroups) {
            bool inGroup = false;
            for (int lid : grp.linkIds)
                if (lid == linkId) { inGroup = true; break; }
            juce::String label = grp.name.empty()
                ? "Group #" + juce::String(grp.id)
                : juce::String(grp.name);
            grpMenu.addItem(100 + grp.id, (inGroup ? "Remove from " : "Add to ") + label);
        }
        grpMenu.addSeparator();
        grpMenu.addItem(31, "Help: Effect Groups...");
        menu.addSubMenu("Effect Group", grpMenu);
    }

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
        } else if (result == 31) {
            // Help: Effect Groups → open the docs page
            if (onOpenHelpDoc) onOpenHelpDoc("layers-and-groups.html");
            return;
        } else if (result == 30) {
            // New effect group — prompt for optional name
            auto* aw = new juce::AlertWindow("New Effect Group",
                "Name is optional — the group is always identified by its colored diamond tag.",
                juce::MessageBoxIconType::NoIcon);
            aw->addTextEditor("name", "", "Name (optional):");
            aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
            aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            auto lid = linkId;
            aw->enterModalState(true, juce::ModalCallbackFunction::create(
                [this, lid, aw](int res) {
                    if (res == 1) {
                        auto name = aw->getTextEditorContents("name").toStdString();
                        auto& grp = graph.addEffectGroup(name);
                        grp.linkIds.push_back(lid);
                        graph.dirty = true;
                    }
                    delete aw;
                    repaint();
                }), true);
            return;
        } else if (result >= 100 && result < 5000) {
            // Toggle link membership in an existing group
            int groupId = result - 100;
            if (auto* grp = graph.findEffectGroup(groupId)) {
                auto it = std::find(grp->linkIds.begin(), grp->linkIds.end(), linkId);
                if (it != grp->linkIds.end())
                    grp->linkIds.erase(it); // remove
                else
                    grp->linkIds.push_back(linkId); // add
                graph.dirty = true;
            }
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

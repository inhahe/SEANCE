#include "node_graph_component.h"
#include "dialog_helpers.h"
#include "music_theory.h"
#include "layered_wave_editor.h"
#include "spectral_editor.h"
#include "wavelet_painter.h"
#include "wavelet_paint.h"
#include "trigger_node.h"
#include "midi_mod_node.h"
#include "xy_pad.h"
#include "control_bank.h"
#include "signal_shape_node.h"
#include "midi_script_editor.h"
#include "convolution_processor.h"
#include "soundfont_processor.h"
#include "builtin_effects.h"
#include "drum_synth.h"
#include "multi_sampler.h"
#include "terrain_synth.h" // classifySynthSource for Synth Mode picker
#include "adsr_envelope_component.h"
#include "envelope_presets.h"
#include <cmath>

namespace SoundShop {

static const float NODE_WIDTH = 180.0f;
static const float PIN_ROW_HEIGHT = 20.0f;
static const float PIN_RADIUS = 5.0f;
static const float HEADER_HEIGHT = 24.0f;

// Central color definitions for each pin/wire kind. Used by both drawPin
// (dots) and drawLink (cables) so the cable matches the pin it's attached
// to. Param and Signal are intentionally in the same warm (orange/amber)
// family because they're conceptually related - Param = block-rate control,
// Signal = audio-rate control - while Audio (blue) and MIDI (green) are in
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

// Human-readable name for a pin/wire kind. The bare enum names ("Param",
// "Signal") mean nothing to a non-musician, so each is described in plain
// language by what it actually carries and how often it updates. The Param and
// Signal update rates depend on the live sample rate / block size, so pass them
// in (sampleRate / blockSize); when unknown (<=0) a generic phrasing is used.
// Shown as the header of the cable right-click menu.
static juce::String nameForPinKind(PinKind k, double sampleRate, int blockSize) {
    bool haveFmt = sampleRate > 0.0 && blockSize > 0;
    switch (k) {
        case PinKind::Audio:  return "Audio - the sound itself";
        case PinKind::Midi:   return "MIDI - notes & controllers";
        case PinKind::Param: {
            if (haveFmt) {
                int perSec = (int)std::lround(sampleRate / (double)blockSize);
                return "Param - smooth control values (updates "
                       + juce::String(perSec) + "x/sec, once per "
                       + juce::String(blockSize) + "-sample block)";
            }
            return "Param - smooth control values (once per audio block)";
        }
        case PinKind::Signal: {
            if (haveFmt) {
                // Every sample => the sample rate. Show in kHz to keep it short.
                juce::String khz = juce::String(sampleRate / 1000.0, 1);
                return "Signal - fast control values (updates every sample, "
                       + khz + "k/sec)";
            }
            return "Signal - fast control values (updates every sample)";
        }
    }
    return "Unknown";
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
        case NodeType::MidiInput:     return juce::Colour(50, 130, 70); // green - matches MIDI wire color
        case NodeType::MidiScript:    return juce::Colour(40, 140, 90); // green family - a MIDI generator
        default:                      return juce::Colour(80, 80, 80);
    }
}

// ==============================================================================
// Drawing
// ==============================================================================

void NodeGraphComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(25, 25, 30));

    // If we somehow paint before resized() (e.g. unusual layout cascade),
    // apply the initial view here so we never draw nodes at the default
    // zoom/pan. Prefer the saved pan/zoom (loaded from the project file)
    // when present, otherwise fit-all so newly-created/imported projects
    // still get centered.
    if (pendingInitialFit && getWidth() > 0 && getHeight() > 0) {
        if (graph.viewZoom > 0.0f) {
            zoom = graph.viewZoom;
            panOffset = {graph.viewPanX, graph.viewPanY};
        } else if (graph.nodes.size() > 1) {
            fitAll();
        }
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

    // Draw links. The hovered cable is deferred and drawn AFTER the nodes
    // (below) so its highlight/glow is never occluded - cables route under node
    // bodies, and a short cable between two adjacent nodes would otherwise have
    // its entire highlight hidden behind the nodes, making it look like nothing
    // lit up even though the hover hit-test fired.
    auto emphasised = [&](const Link& l) {
        return l.id == hoveredLinkId || l.id == selectedLinkId;
    };
    for (auto& link : graph.links)
        if (!emphasised(link))
            drawLink(g, link);

    // Draw pending link
    if (dragMode == DragMode::DragLink)
        drawPendingLink(g);

    // Draw nodes
    for (auto& node : graph.nodes)
        drawNode(g, node);

    // Emphasised cables (selected and/or hovered) on top of everything, so the
    // highlight stays fully visible - traceable end-to-end and never occluded
    // by nodes. Selected first, hovered last so the hovered cable wins when a
    // different cable is selected. (Selection is also set by a right-click, so
    // the targeted cable stays lit while its context menu is open.)
    for (auto& link : graph.links)
        if (link.id == selectedLinkId && link.id != hoveredLinkId)
            drawLink(g, link);
    for (auto& link : graph.links)
        if (link.id == hoveredLinkId)
            drawLink(g, link);
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
    if (node.recordArmed) {
        g.setColour(juce::Colours::red);
        g.setFont(juce::Font(std::max(8.0f, 10.0f * zoom), juce::Font::bold));
        g.drawText("R", titleArea.removeFromRight(16 * zoom), juce::Justification::centred);
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
            // Enum-typed params get their numeric value translated into a
            // readable label, so the user sees the meaning rather than a
            // float like "1.00". Everything else falls through to a 2-dp
            // numeric display.
            juce::String valueStr;
            if (p.name == "Synth Mode") {
                // Display the *effective* mode after clamping against the
                // source's applicability set. Legacy projects with a stale
                // mode value (e.g. AM-sine on a wavetable) silently snap to
                // a valid mode in the audio thread, and we mirror that here
                // so the row reads what the synth is actually doing.
                int m = juce::jlimit(0, 2, (int)std::round(p.value));
                TerrainSynthMode requested = (m == 1) ? TerrainSynthMode::WaveformPerPoint
                                           : (m == 2) ? TerrainSynthMode::AdditiveBank
                                                      : TerrainSynthMode::SamplePerPoint;
                auto avail = synthModeAvailabilityFor(classifySynthSource(node.script));
                TerrainSynthMode effective = avail.clamp(requested);
                valueStr = (effective == TerrainSynthMode::SamplePerPoint)   ? "Direct"
                         : (effective == TerrainSynthMode::WaveformPerPoint) ? "AM-sine"
                                                                             : juce::String("Additive bank");
            } else if (p.name == "Traversal") {
                int m = juce::jlimit(0, 3, (int)std::round(p.value));
                valueStr = (m == 0) ? "Orbit"
                         : (m == 1) ? "Linear"
                         : (m == 2) ? "Lissajous"
                         : juce::String("Physics");
            } else {
                valueStr = juce::String(p.value, 2);
            }
            g.drawText(valueStr, rowRect.reduced(4, 0), juce::Justification::centredRight, false);

            // Modulation indicators (#29): small colored dots after the
            // param name showing what's driving this param.
            if (zoom > 0.4f) {
                float indX = labelRect.getX() + g.getCurrentFont().getStringWidthFloat(p.name) + 4;
                float indY = labelRect.getCentreY() - 2;
                float indSz = 4.0f;
                // Orange dot = has automation points
                if (!p.automation.points.empty()) {
                    g.setColour(juce::Colours::orange);
                    g.fillEllipse(indX, indY, indSz, indSz);
                    indX += indSz + 2;
                }
                // Cyan dot = signal modulation pin attached
                if (p.modulated) {
                    g.setColour(juce::Colours::cyan);
                    g.fillEllipse(indX, indY, indSz, indSz);
                    indX += indSz + 2;
                }
                // Green dot = MIDI Learn (CC mapping) targets this param
                for (auto& cc : graph.ccMappings) {
                    if (cc.nodeId == node.id && cc.paramIdx == pi) {
                        g.setColour(juce::Colours::limegreen);
                        g.fillEllipse(indX, indY, indSz, indSz);
                        break;
                    }
                }
            }

            pinY += PIN_ROW_HEIGHT;
        }
    }

    // Peak meter bars (#99) - two thin horizontal bars (L/R) at the
    // bottom of the node, showing the current audio level. Green
    // below -6 dB, yellow up to -1 dB, red above. Only drawn when
    // there's actually signal flowing (peak > 0.001) and zoom > 0.35.
    if (zoom > 0.35f) {
        float pkL = node.meterPeakL;
        float pkR = node.meterPeakR;
        if (pkL > 0.001f || pkR > 0.001f) {
            auto meterBounds = getNodeBounds(node);
            auto sb = juce::Rectangle<float>(
                canvasToScreen(meterBounds.getBottomLeft()),
                canvasToScreen(meterBounds.getBottomRight() + juce::Point<float>(0, 6)));
            float mw = sb.getWidth();
            float mh = sb.getHeight() * 0.45f;
            float my = sb.getY();

            auto drawBar = [&](float peak, float y) {
                float db = 20.0f * std::log10(std::max(1e-6f, peak));
                float frac = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f); // -60..0 dB -> 0..1
                auto col = (db > -1.0f) ? juce::Colours::red
                         : (db > -6.0f) ? juce::Colours::yellow
                         : juce::Colours::limegreen;
                g.setColour(juce::Colour(30, 30, 35));
                g.fillRect(sb.getX(), y, mw, mh);
                g.setColour(col.withAlpha(0.8f));
                g.fillRect(sb.getX(), y, mw * frac, mh);
            };
            drawBar(pkL, my);
            drawBar(pkR, my + mh + 1);
        }
    }
}

void NodeGraphComponent::drawLink(juce::Graphics& g, Link& link) {
    // Find source and destination pin positions, plus their kinds.
    // The two kinds may differ when an implicit Param↔Signal conversion is
    // in effect - in that case the wire is drawn in two halves, source
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

    // Base alpha - much dimmer when the link is heavily attenuated.
    // "Emphasised" = the cable the user is targeting: either hovered, or
    // selected (which is also set by a right-click, so the cable stays lit up
    // while its context menu is open). Both get the full glow treatment.
    bool isSelected = (link.id == selectedLinkId);
    bool isHovered  = (link.id == hoveredLinkId);
    bool emphasise  = isSelected || isHovered;
    float baseAlpha = (link.gainDb < -10.0f) ? 0.3f : 0.8f;
    if (emphasise) baseAlpha = 1.0f; // full opacity when targeted
    float thickness = 2.0f * zoom;
    if (emphasise) thickness = 3.5f * zoom;

    // Glow: a soft halo of progressively wider, low-alpha strokes drawn
    // underneath the cable so the connection the cursor will target reads as
    // lit up. Drawn in the source-kind colour (a single-colour halo is fine
    // even for a two-tone Param<->Signal cable).
    if (emphasise) {
        juce::Colour glowCol = colourForPinKind(srcKind);
        for (int i = 3; i >= 1; --i)
            g.setColour(glowCol.withAlpha(0.13f)),
            g.strokePath(path, juce::PathStrokeType(thickness + (float)i * 4.0f * zoom,
                         juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Cable colour: brighten when emphasised so it stands out above its neighbours.
    auto strokeColour = [&](PinKind k) {
        auto c = colourForPinKind(k).withAlpha(baseAlpha);
        return emphasise ? c.brighter(0.5f) : c;
    };

    if (srcKind == dstKind) {
        // Single-kind cable: stroke the full bezier in one colour.
        g.setColour(strokeColour(srcKind));
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

        g.setColour(strokeColour(srcKind));
        g.strokePath(path, juce::PathStrokeType(thickness));

        // Sample the tail half (t in [0.5, 1.0]) as a smooth polyline.
        const int tailSegments = 20;
        juce::Path tail;
        tail.startNewSubPath(bezAt(0.5f));
        for (int i = 1; i <= tailSegments; ++i) {
            float t = 0.5f + 0.5f * (float)i / (float)tailSegments;
            tail.lineTo(bezAt(t));
        }
        g.setColour(strokeColour(dstKind));
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

        // Count how many tags this cable will draw: one circle for wire
        // identity, plus one diamond per effect group the link belongs to.
        // Knowing the total up front lets us centre the whole cluster on the
        // wire's midpoint (t=0.5) instead of starting at a fixed offset.
        int tagCount = 1;
        for (const auto& grp : graph.effectGroups)
            for (int lid : grp.linkIds)
                if (lid == link.id) { ++tagCount; break; }

        const float tagSpacing = 0.12f; // gap between consecutive tags in t
        // Centre the run of tags on t=0.5: a run of N tags spans
        // (N-1)*spacing, so the first sits half a span before the midpoint.
        float t = 0.5f - (tagCount - 1) * tagSpacing * 0.5f;

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
            t += tagSpacing;
        }

        // --- Diamond tags: one per group this link belongs to ---
        for (const auto& grp : graph.effectGroups) {
            bool inGroup = false;
            for (int lid : grp.linkIds)
                if (lid == link.id) { inGroup = true; break; }
            if (!inGroup) continue;

            auto pos = bezierAt(juce::jlimit(0.0f, 1.0f, t));
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

            t += tagSpacing;
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

int NodeGraphComponent::pinAtPoint(juce::Point<float> canvasPos, bool& isOutput, int wantInput) {
    // Return the CLOSEST pin to the cursor, not merely the first one found in
    // iteration order. The old "first within radius, outputs before inputs"
    // logic had two failure modes: (1) when two pins were both in range it
    // returned whichever was iterated first rather than the nearer one, and
    // (2) it always preferred outputs, so when dropping a cable onto an input
    // pin that happened to sit near some output pin (e.g. the source node's own
    // output, or an adjacent node's output), it returned that output instead -
    // making the drop's direction check fail and silently refusing the
    // connection. That's exactly why dragging "Signal Out" onto a synth's
    // bottom-left "Aftertouch" input could fail while a higher input succeeded.
    //
    // wantInput: -1 = accept either direction (starting a drag), 0 = only
    // output pins, 1 = only input pins. Drag/drop pass the opposite of the
    // source pin's direction so a target pin can never resolve to the wrong
    // side.
    float hitRadius = PIN_RADIUS * 2;
    int   bestPin = -1;
    bool  bestIsOut = false;
    float bestDist = hitRadius;
    for (auto& node : graph.nodes) {
        if (wantInput != 1) {
            for (auto& pin : node.pinsOut) {
                float d = getPinPosition(node, pin).getDistanceFrom(canvasPos);
                if (d < bestDist) { bestDist = d; bestPin = pin.id; bestIsOut = true; }
            }
        }
        if (wantInput != 0) {
            for (auto& pin : node.pinsIn) {
                float d = getPinPosition(node, pin).getDistanceFrom(canvasPos);
                if (d < bestDist) { bestDist = d; bestPin = pin.id; bestIsOut = false; }
            }
        }
    }
    if (bestPin >= 0) isOutput = bestIsOut;
    return bestPin;
}

int NodeGraphComponent::linkAtPoint(juce::Point<float> canvasPos) {
    auto screenPos = canvasToScreen(canvasPos);
    // Return the CLOSEST link within tolerance, not merely the first one in
    // iteration order. Overlapping cables (e.g. an audio cable and a Signal
    // modulation cable running between the same pair of nodes) would otherwise
    // always resolve to whichever appears first in graph.links, making the
    // other one impossible to right-click / select / delete.
    int   bestLink = -1;
    float bestDist = 13.0f; // hit tolerance in px (generous so thin cables are
                            // easy to hover/click, esp. when zoomed out)
    for (auto& link : graph.links) {
        juce::Point<float> start, end;
        bool foundSrc = false, foundDst = false;
        for (auto& node : graph.nodes) {
            for (auto& pin : node.pinsOut)
                if (pin.id == link.startPin) { start = canvasToScreen(getPinPosition(node, pin)); foundSrc = true; }
            for (auto& pin : node.pinsIn)
                if (pin.id == link.endPin) { end = canvasToScreen(getPinPosition(node, pin)); foundDst = true; }
        }
        // Skip dangling links whose endpoints no longer exist - otherwise their
        // default {0,0} endpoints create a phantom hot-spot at the canvas origin.
        if (!foundSrc || !foundDst) continue;

        // Simple distance check to the line
        float dx = std::abs(end.x - start.x) * 0.5f;
        dx = std::max(dx, 30.0f * zoom);
        juce::Path path;
        path.startNewSubPath(start);
        path.cubicTo(start.x + dx, start.y, end.x - dx, end.y, end.x, end.y);

        // Measure distance to the line SEGMENTS between consecutive flattened
        // points, not to the points themselves. PathFlatteningIterator only
        // subdivides where the curve bends, so the straight stretches where the
        // cable exits each pin horizontally get just their two endpoints - tens
        // of px apart. Measuring point-to-sample-point distance there would miss
        // a cursor sitting right on the straight part of the wire (exactly the
        // region near a node), which is why hover/right-click failed within a
        // short distance of a node. juce::Line::getDistanceFromPoint clamps to
        // the segment, so this is the true distance to the drawn cable.
        juce::PathFlatteningIterator it(path, {}, 2.0f);
        float minDist = 999999.0f;
        juce::Point<float> dummy;
        while (it.next()) {
            juce::Line<float> seg(it.x1, it.y1, it.x2, it.y2);
            minDist = std::min(minDist, seg.getDistanceFromPoint(screenPos, dummy));
        }

        if (minDist < bestDist) { bestDist = minDist; bestLink = link.id; }
    }
    return bestLink;
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
            // Check if right-click is on a param row - show arm/disarm menu
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
                        // Signal modulation pin (#88): offer to add or remove
                        // a Signal input pin that drives this specific param.
                        bool hasModPin = false;
                        for (auto& mp : node->modPins)
                            if (mp.paramIndex == idx) { hasModPin = true; break; }
                        pm.addSeparator();
                        if (hasModPin)
                            pm.addItem(10, "Remove Modulation Input");
                        else
                            pm.addItem(10, "Add Modulation Input");
                        int nodeId = node->id;
                        int paramIdx = idx;
                        pm.showMenuAsync({}, [this, nodeId, paramIdx, hasModPin](int r) {
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
                            else if (r == 10) {
                                if (hasModPin) {
                                    // Remove the modulation pin + binding.
                                    for (auto it = nd->modPins.begin(); it != nd->modPins.end(); ++it) {
                                        if (it->paramIndex == paramIdx) {
                                            int pinId = it->pinId;
                                            // Remove pin from pinsIn.
                                            nd->pinsIn.erase(
                                                std::remove_if(nd->pinsIn.begin(), nd->pinsIn.end(),
                                                    [pinId](const Pin& p) { return p.id == pinId; }),
                                                nd->pinsIn.end());
                                            // Remove any links connected to this pin.
                                            graph.links.erase(
                                                std::remove_if(graph.links.begin(), graph.links.end(),
                                                    [pinId](const auto& lk) { return lk.endPin == pinId; }),
                                                graph.links.end());
                                            nd->modPins.erase(it);
                                            break;
                                        }
                                    }
                                    // Clear modulation state on the param.
                                    if (paramIdx < (int)nd->params.size()) {
                                        auto& p2 = nd->params[paramIdx];
                                        if (p2.modulated) {
                                            p2.value = p2.baseValue;
                                            p2.modulated = false;
                                        }
                                    }
                                    graph.dirty = true;
                                    graph.commitSnapshot("Remove modulation input");
                                } else {
                                    // Add a new modulation input pin and bind it to this param.
                                    // Modulation is consumed block-rate (applySignalModulations
                                    // reads sample 0), so the pin is a Param (block-rate, orange) -
                                    // NOT a Signal (sample-rate, amber). The receiver decides the
                                    // consumption rate; Param/Signal cables are interchangeable.
                                    if (paramIdx >= (int)nd->params.size()) return;
                                    std::string pinName = "Mod: " + nd->params[paramIdx].name;
                                    int newPinId = graph.allocId();
                                    nd->pinsIn.push_back({newPinId, pinName, PinKind::Param, true, 1});
                                    Node::ModPin mp;
                                    mp.paramIndex = paramIdx;
                                    mp.pinId = newPinId;
                                    mp.depth = 1.0f;
                                    nd->modPins.push_back(mp);
                                    graph.dirty = true;
                                    graph.commitSnapshot("Add modulation input");
                                }
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
        // Check if click landed on a param row inside the node - if so,
        // start a horizontal slider interaction (jump-to-click + drag).
        // Signal-controlled nodes have their params locked - no dragging.
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
                    // Enum params (Synth Mode) get a popup picker instead
                    // of a continuous slider: drag-through-values is clunky
                    // when the values are discrete labels rather than
                    // continuous numbers, and lets users park between
                    // states. For Synth Mode we additionally filter the
                    // menu to the modes that make sense for the current
                    // terrain source, with the rest disabled and
                    // explained inline so users see why they can't pick
                    // them. Right-click still opens the standard
                    // arm/disarm menu (handled above), so no UX gets lost.
                    if (p.name == "Synth Mode") {
                        selectedNodeId = node->id;
                        SynthModeAvailability avail =
                            synthModeAvailabilityFor(classifySynthSource(node->script));
                        int currentInt = juce::jlimit(0, 2, (int)std::round(p.value));
                        TerrainSynthMode current = (currentInt == 1) ? TerrainSynthMode::WaveformPerPoint
                                                 : (currentInt == 2) ? TerrainSynthMode::AdditiveBank
                                                                     : TerrainSynthMode::SamplePerPoint;
                        TerrainSynthMode effective = avail.clamp(current);

                        juce::PopupMenu pm;
                        auto addModeItem = [&](int itemId, const juce::String& label,
                                               const juce::String& whyDisabled,
                                               bool isAvailable, bool isCurrent) {
                            juce::PopupMenu::Item item;
                            item.itemID = itemId;
                            item.text = isAvailable ? label
                                                    : label + "  -  " + whyDisabled;
                            item.isEnabled = isAvailable;
                            item.isTicked = isCurrent && isAvailable;
                            pm.addItem(item);
                        };
                        addModeItem(1, "Direct",
                                    "needs a 1D wavetable cycle or audio sample",
                                    avail.direct,
                                    effective == TerrainSynthMode::SamplePerPoint);
                        addModeItem(2, "AM-sine",
                                    "only meaningful for 2D+ terrains "
                                    "(images, math 2D+, fractal noise)",
                                    avail.amSine,
                                    effective == TerrainSynthMode::WaveformPerPoint);
                        addModeItem(3, "Additive bank",
                                    "needs a 1D wavetable cycle to FFT into partials",
                                    avail.additiveBank,
                                    effective == TerrainSynthMode::AdditiveBank);

                        int nodeId = node->id;
                        int paramIdx = idx;
                        pm.showMenuAsync({}, [this, nodeId, paramIdx](int r) {
                            if (r == 0) return;
                            auto* nd = graph.findNode(nodeId);
                            if (!nd || paramIdx >= (int)nd->params.size()) return;
                            // Menu IDs are 1/2/3 (cleared 0 = cancel);
                            // Synth Mode param values are 0/1/2.
                            nd->params[paramIdx].value = (float)(r - 1);
                            graph.dirty = true;
                            graph.commitSnapshot("Change Synth Mode");
                            repaint();
                        });
                        return;
                    }
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

    // Empty space - pan
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
        publishViewState();
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
        //  1. opposite direction from the source (output->input or vice versa)
        //  2. not the same pin we started dragging from
        //  3. compatible pin kinds (audio↔audio, MIDI↔MIDI, or any control↔
        //     control mix; see arePinKindsCompatible). Param↔Signal is
        //     deliberately treated as compatible - implicit conversion lets
        //     either control kind drive either control input.
        auto canvasPos = screenToCanvas(e.position);
        bool isOut = false;
        // Only consider pins on the opposite side from the source: dragging
        // from an output looks for an input target and vice-versa. This stops a
        // nearby output pin from shadowing the input the user is aiming at.
        int hovered = pinAtPoint(canvasPos, isOut, dragPinIsOutput ? 1 : 0);
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
            // No graph rebuild here - processBlock reads param values fresh
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
        // Match the highlight logic: only accept a target on the opposite side
        // from the source pin, so a nearby output can't shadow the intended
        // input (which silently refused the connection - the Aftertouch bug).
        int targetPin = pinAtPoint(canvasPos, isOut, dragPinIsOutput ? 1 : 0);
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
                // Graph-topology change: snapshot it so undo/redo can revert
                // the new cable AND so the persisted-undo-tree's current
                // snapshot stays in sync with graph.links. Without this,
                // tryRestoreUndoTree() at next startup would reparse a
                // pre-cable snapshot over the freshly-loaded .ssp and
                // silently drop the cable the user just made.
                graph.commitSnapshot("Connect pins");
            }
        }
    }
    dragMode = DragMode::None;
    dragNodeId = -1;
    dragParamIdx = -1;
    dragHoverPinId = -1;
    repaint();
}

void NodeGraphComponent::mouseMove(const juce::MouseEvent& e) {
    // Highlight the cable under the cursor (within right-click distance) so the
    // user can see exactly which connection a click / right-click will target.
    // Uses the same hit-test as selection, so the highlighted cable is always
    // the one that would actually be picked.
    int over = linkAtPoint(screenToCanvas(e.position));
    if (over != hoveredLinkId) {
        hoveredLinkId = over;
        repaint();
    }
}

void NodeGraphComponent::mouseExit(const juce::MouseEvent&) {
    if (hoveredLinkId != -1) {
        hoveredLinkId = -1;
        repaint();
    }
}

void NodeGraphComponent::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) {
    float oldZoom = zoom;
    zoom *= (1.0f + wheel.deltaY * 0.3f);
    zoom = juce::jlimit(0.1f, 4.0f, zoom);

    // Zoom toward mouse position
    auto mousePos = e.position;
    panOffset = mousePos - (mousePos - panOffset) * (zoom / oldZoom);

    publishViewState();
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

    // Double-click a MIDI Script node opens its program editor.
    if (node->type == NodeType::MidiScript) {
        int captured = node->id;
        auto* editor = new MidiScriptEditorComponent(graph, captured,
            [this]() {
                if (onNodeEdited) onNodeEdited();
                repaint();
            });
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(editor);
        opts.dialogTitle = "MIDI Script: " + juce::String(node->name);
        opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = false;
        opts.resizable = true;
        opts.componentToCentreAround = this;
        SoundShop::launchToolDialog(opts);
        return;
    }

    // Double-click a Signal Shape node opens its editor. XY Pad nodes
    // share NodeType::SignalShape but use a different dedicated editor
    // (xy_pad.h) - keep that opening on double-click too. The
    // distinguishing tag is the "__xypad__" script.
    if (node->type == NodeType::SignalShape) {
        int captured = node->id;
        if (node->script == "__xypad__") {
            auto* pad = new XYPadComponent(graph, captured);
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned(pad);
            opts.dialogTitle = "XY Pad";
            opts.dialogBackgroundColour = juce::Colour(25, 25, 32);
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar = false;
            opts.resizable = true;
            opts.componentToCentreAround = this;
            if (auto* dlg = SoundShop::launchToolDialog(opts))
                dlg->setResizeLimits(320, 400, 6000, 6000);
        } else if (node->script.rfind("__controlbank__", 0) == 0) {
            auto* bank = new ControlBankComponent(graph, captured,
                [this]() {
                    if (onNodeEdited) onNodeEdited();
                    repaint();
                });
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned(bank);
            opts.dialogTitle = "Control Bank: " + juce::String(node->name);
            opts.dialogBackgroundColour = juce::Colour(25, 25, 32);
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar = false;
            opts.resizable = true;
            opts.componentToCentreAround = this;
            SoundShop::launchToolDialog(opts);
        } else {
            auto* editor = new SignalShapeEditorComponent(graph, captured,
                [this]() {
                    if (onNodeEdited) onNodeEdited();
                    repaint();
                },
                [this, captured]() {
                    if (onSignalShapeManualTrigger) onSignalShapeManualTrigger(captured);
                });
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned(editor);
            opts.dialogTitle = "Signal Shape: " + juce::String(node->name);
            opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar = false;
            opts.resizable = true;
            opts.componentToCentreAround = this;
            SoundShop::launchToolDialog(opts);
        }
        return;
    }

    if (node->type == NodeType::MidiTimeline || node->type == NodeType::AudioTimeline) {
        // Open piano roll editor
        bool already = false;
        for (int edId : graph.openEditors)
            if (edId == node->id) { already = true; break; }
        if (!already)
            graph.openEditors.insert(graph.openEditors.begin(), node->id);
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

    publishViewState();
    repaint();
}

void NodeGraphComponent::publishViewState() {
    graph.viewZoom = zoom;
    graph.viewPanX = panOffset.x;
    graph.viewPanY = panOffset.y;
}

void NodeGraphComponent::notifyProjectLoaded() {
    // A new project's view fields have just been populated (or left at 0).
    // Defer the actual restore to the next paint/resized once we have a
    // real size - matches the existing first-paint contract and avoids
    // racing with whatever layout pass triggered the load.
    pendingInitialFit = true;
    repaint();
}

void NodeGraphComponent::resized() {
    // Apply the initial view the first time we get a real (non-zero) size,
    // so the very first paint already shows the graph at the right
    // zoom - no visible zoom-in jitter on project load. Prefer the saved
    // pan/zoom (graph.viewZoom > 0) when available, otherwise fit-all.
    // Subsequent resizes (window-resize, panel splits, etc.) leave the
    // user's view alone so we don't clobber any manual pan/zoom they've
    // done.
    if (pendingInitialFit && getWidth() > 0 && getHeight() > 0) {
        if (graph.viewZoom > 0.0f) {
            zoom = graph.viewZoom;
            panOffset = {graph.viewPanX, graph.viewPanY};
        } else if (graph.nodes.size() > 1) {
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
    // Built-in Synth is a single wavetable-based oscillator. The wavetable
    // can mix layered (time-domain), frequency-domain (FFT), and wavelet-
    // domain (DWT) frames - those used to be three separate menu items, but
    // since any of them lets you author any of the three frame types from
    // inside the wavetable editor (via + Frame), the three options were
    // redundant. Collapsed into one entry; pick frame types after the
    // editor opens.
    instMenu.addItem(110, "Wavetable");
    juce::PopupMenu terrainMenu;
    terrainMenu.addItem(120, "2D Terrain (sin*cos)");
    terrainMenu.addItem(121, "2D Terrain (noise)");
    terrainMenu.addItem(122, "2D Terrain (custom expression...)");
    terrainMenu.addItem(125, "N-D Terrain (custom expression, 1-8D)...");
    terrainMenu.addItem(123, "From Image...");
    terrainMenu.addItem(124, "From Audio File...");
    instMenu.addSubMenu("Terrain Synth", terrainMenu);
    instMenu.addSeparator();
    instMenu.addItem(100, "Piano");
    instMenu.addItem(102, "Sampler");
    instMenu.addItem(107, "FM Synth");
    instMenu.addItem(108, "Phase Distortion Synth");
    instMenu.addItem(109, "Particle Cloud Synth");
    // Bugfix: this used to be ID 110, which collided with the Built-in Synth
    // entry above, so clicking "Additive Synth" actually created a Waveform
    // Synth (the first matching branch in the result chain). Moved to 112.
    instMenu.addItem(112, "Additive Synth");
    instMenu.addItem(111, "Spectral Grain Synth");
    instMenu.addItem(104, "SoundFont (.sf2)...");
    instMenu.addItem(105, "SFZ Instrument (.sfz)...");
    instMenu.addItem(103, "Drum Machine");
    instMenu.addItem(106, "Analog Drum Synth");
    menu.addSubMenu("Instruments", instMenu);

    juce::PopupMenu fxMenu;
    fxMenu.addItem(206, "Pitch Shift / Time Stretch");
    fxMenu.addSeparator();
    fxMenu.addItem(207, "Convolution Filter");
    fxMenu.addItem(221, "Reverb");
    fxMenu.addItem(222, "Parametric EQ");
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
    fxMenu.addItem(223, "Ring Modulator");
    fxMenu.addItem(226, "Transient/Sustain Split");
    fxMenu.addItem(227, "Wavelet Denoiser");
    fxMenu.addItem(228, "Wavelet Bitcrush");
    fxMenu.addItem(229, "Octave Shift (wavelet)");
    fxMenu.addItem(230, "Wavelet Multiband Comp");
    fxMenu.addItem(231, "Wavelet Pitch Shift");
    fxMenu.addItem(232, "Wavelet Reverb (1/f)");
    fxMenu.addItem(233, "Independent Pitch Shift");
    fxMenu.addItem(234, "Wavelet Complexity");
    fxMenu.addItem(235, "Asymmetric Filter");
    fxMenu.addItem(236, "Wavelet Pitch Tracker");
    fxMenu.addItem(237, "Wavelet Vocoder");
    fxMenu.addItem(238, "Formant Pitch Shift");
    fxMenu.addItem(239, "SMS (harmonic/noise split)");
    fxMenu.addSeparator();
    fxMenu.addItem(224, "M/S Encode (stereo -> mid+side)");
    fxMenu.addItem(225, "M/S Decode (mid+side -> stereo)");
    fxMenu.addSeparator();
    fxMenu.addItem(217, "3D Spatializer (binaural)");
    menu.addSubMenu("Effects", fxMenu);

    menu.addSeparator();
    menu.addItem(3, "Mixer");
    menu.addItem(4, "Output");
    menu.addItem(5, "Group");
    menu.addItem(6, "WASM Script...");

    juce::PopupMenu sigMenu;
    // Single unified "Signal Shape" entry. Replaces the old separate LFO
    // sine / LFO custom-expression / Envelope custom-expression items
    // (#107) - those were just preset starting points for the same
    // underlying node, with no behavior the user can't reach by editing
    // the shape + trigger expression inside the SignalShape editor.
    sigMenu.addItem(130, "Signal Shape (LFO / Envelope)");
    sigMenu.addItem(131, "MIDI Script (algorithmic MIDI)");
    sigMenu.addSeparator();
    sigMenu.addItem(133, "XY Pad");
    sigMenu.addItem(135, "Control Bank");
    sigMenu.addItem(134, "Spectrum Tap");
    sigMenu.addSeparator();
    sigMenu.addItem(140, "Spectrum Analyzer");
    sigMenu.addItem(141, "Oscilloscope");
    sigMenu.addItem(142, "Spectrogram");
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
            // WASM Script - open file chooser
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
        } else if (result == 110) {
            // Wavetable node - uses TerrainSynthProcessor with a 1D wavetable
            // script. The wavetable editor lets the user mix layered
            // (time-domain) / frequency-domain (FFT) / wavelet (DWT) /
            // captured waveforms freely via its "+ Waveform" popup, so we
            // don't need separate top-level menu items for the three
            // synthesizable waveform types. Starts EMPTY (no waveforms) so
            // the very first user action is picking the type of the first
            // waveform via "+ Waveform" - skips the dance of deleting an
            // auto-created sine that wasn't asked for.
            auto& n = graph.addNode("Wavetable", NodeType::Instrument,
                {Pin{0, "MIDI", PinKind::Midi, true}},
                {Pin{0, "Audio", PinKind::Audio, false}}, {p.x, p.y});
            n.script = WavetableDoc::defaultEmpty().encode();

            // Compact param list: only the controls that actually do something
            // for a 1D Waveform synth. Terrain-traversal params (Speed,
            // Radius, Center, Traversal mode, LFOs, Grain) are omitted since
            // they're meaningless for 1D playback. TerrainSynthProcessor reads
            // missing params via getParam's default-fallback path, and the
            // Position param is now looked up by name so its index doesn't
            // matter.
            // Amplitude envelope lives on the shared node AHDSR (single
            // source of truth, edited via right-click "Envelope (AHDSR)..."),
            // not as inline params. Seed it with this synth's classic ADSR
            // character. Velocity sensitivity is part of the envelope too.
            n.ahdsrEnvelope.attackMs  = 10.0f;
            n.ahdsrEnvelope.decayMs   = 100.0f;
            n.ahdsrEnvelope.sustain   = 0.7f;
            n.ahdsrEnvelope.releaseMs = 300.0f;
            n.params.push_back({"Volume",   1.0f,  0.0f,   1.0f});
            n.params.push_back({"Pan",      0.0f, -1.0f,   1.0f});
            // Mod-wheel vibrato depth (0 = disable the default behavior so
            // the user can MIDI-Learn CC1 to a different param instead).
            n.params.push_back({"Vibrato",  1.0f,  0.0f,   1.0f});
            // Wavetable Position - meaningful when there are multiple frames.
            // Looked up by name in TerrainSynthProcessor, so list order is free.
            n.params.push_back({"Position", 0.0f,  0.0f,   1.0f});

            // Open the wavetable editor immediately. MUST use the
            // launchNonModalToolDialog path, not launchToolDialog: the
            // arrangement view's library list uses JUCE's
            // DragAndDropContainer to drop library entries onto cells /
            // scatter positions, and a modal parent dialog blocks the
            // DragImageComponent (which lives on the desktop, outside the
            // modal hierarchy) from receiving mouseUp via the source-
            // component listener forwarding chain - so itemDropped never
            // fires and the drag silently leaves a stranded drag-image
            // bitmap with no placement. This mirrors the rationale at the
            // double-click reopen path in main_window.cpp; both entry
            // points into the wavetable editor must be non-modal for DnD
            // to work.
            auto nodeId = n.id;
            auto* editor = new LayeredWaveEditorComponent(graph, nodeId, [this]() {
                if (onNodeEdited) onNodeEdited();
                repaint();
            });
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned(editor);
            opts.dialogTitle = "Wavetable: " + juce::String(n.name);
            opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar = false;
            opts.resizable = true;
            opts.componentToCentreAround = this;
            SoundShop::launchNonModalToolDialog(opts);
            (void)nodeId;
            return;
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
                opts.useNativeTitleBar = false;
                opts.resizable = true;
                opts.componentToCentreAround = this;
                if (auto* dlg = SoundShop::launchToolDialog(opts))
                    dlg->setResizeLimits(320, 400, 6000, 6000);
            }
            return;
        } else if (result == 135) {
            // Control Bank: a bank of manual macro faders. Each slider emits
            // one control-signal output (0..1) you can wire to any param. Same
            // NodeType::SignalShape family as XY Pad / Signal Shape, tagged by
            // the "__controlbank__" script and handled by SignalShapeProcessor's
            // control-bank branch. Starts with 4 sliders; add/remove in the
            // editor (which opens immediately, like XY Pad / Wavetable).
            const int kStartSliders = 4;
            std::vector<Pin> outs;
            for (int i = 0; i < kStartSliders; ++i)
                outs.push_back(Pin{0, "Slider " + std::to_string(i + 1),
                                   PinKind::Signal, false, 1});
            auto& n = graph.addNode("Control Bank", NodeType::SignalShape,
                                    {}, outs, {p.x, p.y});
            n.script = "__controlbank__"; // vertical by default
            for (int i = 0; i < kStartSliders; ++i)
                n.params.push_back({"Slider " + std::to_string(i + 1), 0.5f, 0.0f, 1.0f});

            int newNodeId = n.id;
            auto* bank = new ControlBankComponent(graph, newNodeId,
                [this]() {
                    if (onNodeEdited) onNodeEdited();
                    repaint();
                });
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned(bank);
            opts.dialogTitle = "Control Bank: " + juce::String(n.name);
            opts.dialogBackgroundColour = juce::Colour(25, 25, 32);
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar = false;
            opts.resizable = true;
            opts.componentToCentreAround = this;
            SoundShop::launchToolDialog(opts);
            return;
        } else if (result == 134) {
            // Spectrum Tap - insert inline on audio for frequency analysis.
            // Has audio in/out (passthrough) and user-defined frequency bins.
            auto& n = graph.addNode("Spectrum Tap", NodeType::Effect,
                {Pin{0, "Audio In", PinKind::Audio, true}},
                {Pin{0, "Audio Out", PinKind::Audio, false}},
                {p.x, p.y});
            n.script = "__spectrumtap__";
        } else if (result == 140 || result == 141 || result == 142) {
            // Audio analyzer nodes: pure visualizers that pass audio
            // through and display the signal. All three are Effect nodes
            // with Audio In + Audio Out; the script tag selects which
            // editor opens on double-click.
            const char* nodeName    = (result == 140) ? "Spectrum Analyzer"
                                    : (result == 141) ? "Oscilloscope"
                                                      : "Spectrogram";
            const char* scriptTag   = (result == 140) ? "__spectrumanalyzer__"
                                    : (result == 141) ? "__oscilloscope__"
                                                      : "__spectrogram__";
            auto& n = graph.addNode(nodeName, NodeType::Effect,
                {Pin{0, "Audio In", PinKind::Audio, true}},
                {Pin{0, "Audio Out", PinKind::Audio, false}},
                {p.x, p.y});
            n.script = scriptTag;
            if (result == 140)
                n.params.push_back({"Bins",   64.0f,   16.0f, 512.0f});
            else if (result == 141)
                n.params.push_back({"Window", 1024.0f, 256.0f, 4096.0f});
        } else if (result == 131) {
            // MIDI Script node - algorithmic MIDI generator. Runs a small
            // program (statements + persistent state + MIDI emit functions)
            // once per sample and outputs MIDI live. Default pins: one merged
            // "MIDI In" and one "MIDI Out 1"; the editor lets the user add
            // Signal inputs (s1..sN) and more MIDI outputs. See
            // midi_script_node.h.
            auto& n = graph.addNode("MIDI Script", NodeType::MidiScript,
                {Pin{0, "MIDI In", PinKind::Midi, true}},
                {Pin{0, "MIDI Out 1", PinKind::Midi, false}},
                {p.x, p.y});

            MidiScriptDoc seed = MidiScriptDoc::defaultDoc();
            n.script = seed.encode();

            int newNodeId = n.id;
            auto* editor = new MidiScriptEditorComponent(graph, newNodeId,
                [this]() {
                    if (onNodeEdited) onNodeEdited();
                    repaint();
                });
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned(editor);
            opts.dialogTitle = "MIDI Script: " + juce::String(n.name);
            opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar = false;
            opts.resizable = true;
            opts.componentToCentreAround = this;
            SoundShop::launchToolDialog(opts);
            return;
        } else if (result == 130) {
            // Signal Shape node. Single menu item replacing the old
            // LFO / LFO-expression / Envelope-expression trio - the
            // underlying processor is the same in all three cases,
            // distinguished only by trigger expression and repeat mode
            // (both editable inside the SignalShape editor that opens
            // immediately on creation).
            //
            // Pins:
            //   In:  "MIDI In" only. The OLD code documented its lone
            //        MIDI input as "trigger input for envelope" but
            //        the new processor uses MIDI for gate/freq/note/vel
            //        VARIABLES; triggering is done via the trigger
            //        expression (e.g. "gate"). Additional Signal input
            //        pins (s1..sN) appear as the user dials up
            //        signalInputCount in the editor.
            //   Out: Param Out + Signal Out (both carry the same value;
            //        Param Out exists for orange-cable connections to
            //        param-arming inputs, Signal Out for audio-rate
            //        consumers).
            auto& n = graph.addNode("Signal Shape", NodeType::SignalShape,
                {Pin{0, "MIDI In", PinKind::Midi, true}},
                {Pin{0, "Param Out", PinKind::Param, false},
                 Pin{0, "Signal Out", PinKind::Signal, false, 1}},
                {p.x, p.y});

            // Seed node.script with a NEUTRAL default: a free-running
            // (Forever) shape with ZERO layers. A layer-less shape renders to
            // a flat 0, so a brand-new node does NOT sweep anything - its
            // output sits at 0 until the user adds a layer in the editor.
            // (A default Sine would start modulating downstream params the
            // instant the node is created, which surprised users.) The editor
            // opens showing an empty layer stack with a "+ Layer" button and a
            // hint explaining the node stays at 0 until a layer is added.
            SignalShapeDoc seed = SignalShapeDoc::defaultLFO();
            n.script = seed.encode();

            n.params.push_back({"Rate",       1.0f,    0.01f, 50.0f});   // Hz, or beats/cycle when Beat Sync = 1
            n.params.push_back({"Beat Sync",  0.0f,    0.0f, 1.0f});      // 0=free, 1=synced
            n.params.push_back({"Phase",      0.0f,    0.0f, 1.0f});      // phase offset
            n.params.push_back({"Output",     0.0f,   -1.0f, 1.0f});      // read-only current value

            // Open the SignalShape editor immediately - matches the
            // "wavetable opens on create" pattern above. Non-modal so the
            // user can leave it up while interacting with the graph.
            int newNodeId = n.id;
            auto* editor = new SignalShapeEditorComponent(graph, newNodeId,
                [this]() {
                    if (onNodeEdited) onNodeEdited();
                    repaint();
                },
                [this, newNodeId]() {
                    if (onSignalShapeManualTrigger) onSignalShapeManualTrigger(newNodeId);
                });
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned(editor);
            opts.dialogTitle = "Signal Shape: " + juce::String(n.name);
            opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar = false;
            opts.resizable = true;
            opts.componentToCentreAround = this;
            SoundShop::launchToolDialog(opts);
            return;
        } else if (result >= 120 && result <= 125) {
            // Terrain Synth. The terrain engine and visualizer both support
            // N-dimensional terrains (1..8 axes); the visualizer's + Dim /
            // - Dim buttons add/remove axes at runtime, and
            // makeTerrainNode() takes a numDims arg so callers can also
            // create higher-D terrains directly. Image (2D) and audio (1D)
            // sources have fixed dimensionality - the only path that
            // varies N at create time is the formula path (result 125).
            if (result == 120) {
                makeTerrainNode("Terrain (sin*cos)", "sin(x) * cos(y)", p);
            } else if (result == 121) {
                // Fractal value noise: smooth, 1/f-ish bumpy terrain. The
                // orbit reads varying noisy texture as it moves, rather than
                // independent random samples per cell (which is what the old
                // expression-based "noise(0)" produced and was effectively
                // equivalent to a noise oscillator with the terrain abstraction
                // adding nothing). Script format documented in terrain_synth.cpp.
                makeTerrainNode("Terrain (noise)",
                                "__valuenoise__:256,256:4:0.55:42", p);
            } else if (result == 122) {
                auto nodeId = makeTerrainNode("Terrain", "sin(x)*cos(y)", p).id;
                auto* aw = new juce::AlertWindow("Terrain Expression",
                    "Enter a 2D expression. Variables: x, y (each ranges 0..2pi)\n"
                    "Functions: sin, cos, abs, sqrt, pow, tanh, noise\n\n"
                    "Examples:\n"
                    "  sin(x) * cos(y)\n"
                    "  sin(x*3) + cos(y*2) * 0.5\n"
                    "  tanh(sin(x) * sin(y) * 3)\n\n"
                    "For 3D-8D terrains, use the N-D Terrain menu item instead.",
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
                auto nodeId = makeTerrainNode("Terrain (image)", "", p).id;
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
                auto nodeId = makeTerrainNode("Terrain (audio)", "", p).id;
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
            } else if (result == 125) {
                // N-D Terrain (custom expression). Open a dialog with a
                // dim-count combo and an expression editor. Default is 3D
                // so the option is meaningfully different from the 2D
                // preset above; user can pick anything in 1..8.
                auto canvasPos = p;
                auto* aw = new juce::AlertWindow(
                    "N-D Terrain Expression",
                    "Variables per axis: x, y, z, w, v, u, s, t (each ranges 0..2pi)\n"
                    "Functions: sin, cos, abs, sqrt, pow, tanh, noise\n"
                    "\n"
                    "Pick the number of dimensions (1-8). Each dimension creates one\n"
                    "Sig input pin and one Center/Radius parameter pair on the synth.\n"
                    "Axes you don't reference in the expression are constant along\n"
                    "that axis but still exist as inputs you can modulate.\n"
                    "\n"
                    "Examples:\n"
                    "  1D:  sin(x)\n"
                    "  2D:  sin(x) * cos(y)\n"
                    "  3D:  sin(x) * cos(y) * sin(z)\n"
                    "  4D:  tanh(sin(x) + cos(y) + sin(z) * cos(w))",
                    juce::MessageBoxIconType::NoIcon);
                aw->addComboBox("dims",
                    {"1", "2", "3", "4", "5", "6", "7", "8"},
                    "Dimensions:");
                if (auto* cb = aw->getComboBoxComponent("dims"))
                    cb->setSelectedItemIndex(2, juce::dontSendNotification); // default 3D
                aw->addTextEditor("expr",
                    "sin(x) * cos(y) * sin(z)",
                    "Expression:");
                aw->addButton("OK", 1);
                aw->addButton("Cancel", 0);
                aw->enterModalState(true, juce::ModalCallbackFunction::create(
                    [this, aw, canvasPos](int res) {
                        if (res == 1) {
                            int numDims = 2;
                            if (auto* cb = aw->getComboBoxComponent("dims"))
                                numDims = juce::jlimit(1, 8, cb->getSelectedItemIndex() + 1);
                            std::string expr = aw->getTextEditorContents("expr").toStdString();
                            std::string nodeName = juce::String(numDims).toStdString() + "D Terrain";
                            makeTerrainNode(nodeName, expr, canvasPos, numDims);
                            if (onNodeEdited) onNodeEdited();
                        }
                        delete aw;
                        repaint();
                    }), true);
                return;
            }
        } else if (result == 102) {
            // Sampler: creates a MultiSampler node. The file chooser is
            // a convenience - if the user picks a file, it becomes the
            // instrument's first (and only) zone covering the full MIDI
            // range. The sampler editor can add more zones later.
            auto canvasPos = p;
            auto chooser = std::make_shared<juce::FileChooser>(
                "Load Sample", juce::File(), "*.wav;*.mp3;*.aiff;*.flac;*.ogg");
            chooser->launchAsync(juce::FileBrowserComponent::openMode,
                [this, canvasPos, chooser](const juce::FileChooser& fc) {
                    auto file = fc.getResult();
                    if (!file.existsAsFile()) return;
                    auto name = file.getFileNameWithoutExtension().toStdString();
                    auto& n = graph.addNode(name, NodeType::Instrument,
                        {Pin{0, "MIDI", PinKind::Midi, true}},
                        {Pin{0, "Audio", PinKind::Audio, false}},
                        {canvasPos.x, canvasPos.y});
                    MultiSamplerDoc doc;
                    MultiSamplerZone z;
                    z.samplePath = file.getFullPathName().toStdString();
                    z.loNote = 0; z.hiNote = 127;
                    z.loVel = 1; z.hiVel = 127;
                    z.baseNote = 60;
                    doc.zones.push_back(z);
                    n.script = doc.encode();
                    // Global Volume/Pan live as real params so automation
                    // lanes and Signal cables can target them.
                    n.params.push_back({"Volume", 0.5f,  0.0f, 1.0f});
                    n.params.push_back({"Pan",    0.0f, -1.0f, 1.0f});
                    repaint();
                });
            return;
        } else if (result == 104 || result == 105) {
            // SoundFont (.sf2) or SFZ instrument - file chooser
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
                    // No Attack/Decay/Sustain/Release params: the SoundFont /
                    // SFZ player's amplitude envelope comes from the sound-
                    // bank file itself (SF2 preset / SFZ region ampeg), so
                    // node-level ADSR sliders would be inert.
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
        } else if (result == 107) {
            // FM Synth: 4-operator FM synthesis
            auto& n = graph.addNode("FM Synth", NodeType::Instrument,
                {Pin{0, "MIDI", PinKind::Midi, true}},
                {Pin{0, "Audio", PinKind::Audio, false}}, {p.x, p.y});
            n.script = "__fmsynth__";
            n.params.push_back({"Algorithm",  0.0f, 0.0f, 7.0f});
            n.params.push_back({"Feedback",   0.3f, 0.0f, 1.0f});
            n.params.push_back({"Volume",     0.5f, 0.0f, 1.0f});
            for (int i = 1; i <= 4; ++i) {
                auto p2 = "Op" + std::to_string(i) + " ";
                n.params.push_back({p2 + "Ratio", (float)i, 0.1f, 16.0f});
                n.params.push_back({p2 + "Level", i == 1 ? 1.0f : 0.5f, 0.0f, 1.0f});
                n.params.push_back({p2 + "A",     0.01f, 0.001f, 2.0f});
                n.params.push_back({p2 + "D",     0.1f,  0.001f, 5.0f});
                n.params.push_back({p2 + "S",     0.7f,  0.0f,   1.0f});
                n.params.push_back({p2 + "R",     0.3f,  0.001f, 10.0f});
            }
            repaint();
        } else if (result == 111) {
            // Spectral Grain Synth
            auto& n = graph.addNode("Spectral Grain", NodeType::Instrument,
                {Pin{0, "MIDI", PinKind::Midi, true}},
                {Pin{0, "Audio", PinKind::Audio, false}}, {p.x, p.y});
            n.script = "__spectralgrain__:exp(-f/10)";
            n.params.push_back({"Density",    20.0f, 1.0f, 200.0f});
            n.params.push_back({"Grain Size", 40.0f, 1.0f, 200.0f});
            n.params.push_back({"Volume",      0.5f, 0.0f, 1.0f});
            // Amplitude envelope on the shared node AHDSR (see Wavetable above).
            n.ahdsrEnvelope.attackMs  = 10.0f;
            n.ahdsrEnvelope.decayMs   = 100.0f;
            n.ahdsrEnvelope.sustain   = 0.7f;
            n.ahdsrEnvelope.releaseMs = 300.0f;
            repaint();
        } else if (result == 112) {
            // Additive Synth (ID changed from 110 to 112 to break a
            // pre-existing collision with the Built-in Synth menu entry).
            auto& n = graph.addNode("Additive", NodeType::Instrument,
                {Pin{0, "MIDI", PinKind::Midi, true}},
                {Pin{0, "Audio", PinKind::Audio, false}}, {p.x, p.y});
            n.script = "__additivesynth__";
            n.params.push_back({"Preset",      0.0f, 0.0f,  4.0f}); // 0=Custom 1=Bell 2=Drum 3=Piano 4=Organ
            n.params.push_back({"Partials",   16.0f, 1.0f, 64.0f});
            n.params.push_back({"Stretch",     0.0f, 0.0f,  2.0f});
            n.params.push_back({"Brightness",  1.0f, 0.0f,  3.0f});
            n.params.push_back({"Volume",      0.5f, 0.0f, 1.0f});
            // Amplitude envelope on the shared node AHDSR (see Wavetable above).
            n.ahdsrEnvelope.attackMs  = 10.0f;
            n.ahdsrEnvelope.decayMs   = 100.0f;
            n.ahdsrEnvelope.sustain   = 0.7f;
            n.ahdsrEnvelope.releaseMs = 300.0f;
            repaint();
        } else if (result == 109) {
            // Particle Cloud Synth
            auto& n = graph.addNode("Particle", NodeType::Instrument,
                {Pin{0, "MIDI", PinKind::Midi, true}},
                {Pin{0, "Audio", PinKind::Audio, false}}, {p.x, p.y});
            n.script = "__particlesynth__";
            n.params.push_back({"Density",    30.0f,  1.0f, 200.0f});
            n.params.push_back({"Spread",      7.0f,  0.0f,  24.0f});
            n.params.push_back({"Grain Size", 50.0f,  1.0f, 500.0f});
            n.params.push_back({"Attack",      0.1f,  0.0f,   1.0f});
            n.params.push_back({"Release",     0.3f,  0.0f,   1.0f});
            n.params.push_back({"Shape",       0.0f,  0.0f,   3.0f}); // 0=sine 1=saw 2=sq 3=noise
            n.params.push_back({"Volume",      0.5f,  0.0f,   1.0f});
            repaint();
        } else if (result == 108) {
            // Phase Distortion Synth
            auto& n = graph.addNode("PD Synth", NodeType::Instrument,
                {Pin{0, "MIDI", PinKind::Midi, true}},
                {Pin{0, "Audio", PinKind::Audio, false}}, {p.x, p.y});
            n.script = "__pdsynth__";
            n.params.push_back({"Waveform",    0.0f, 0.0f, 3.0f}); // 0=saw 1=sq 2=pulse 3=reso
            n.params.push_back({"Depth",       0.8f, 0.0f, 1.0f});
            n.params.push_back({"DCW Attack",  0.01f, 0.001f, 2.0f});
            n.params.push_back({"DCW Decay",   0.3f, 0.001f, 5.0f});
            n.params.push_back({"DCW Sustain", 0.3f, 0.0f, 1.0f});
            n.params.push_back({"Volume",      0.5f, 0.0f, 1.0f});
            // Amplitude envelope on the shared node AHDSR (the DCW envelope
            // above is a separate timbral envelope and stays as params).
            n.ahdsrEnvelope.attackMs  = 5.0f;
            n.ahdsrEnvelope.decayMs   = 100.0f;
            n.ahdsrEnvelope.sustain   = 0.7f;
            n.ahdsrEnvelope.releaseMs = 300.0f;
            repaint();
        } else if (result == 100 || result == 103) {
            // Piano and Drum Machine: functional defaults that route through
            // TerrainSynthProcessor, so they don't crash and give the user
            // something playable immediately. Each gets the full param list.
            const char* nodeName = (result == 100) ? "Piano" : "Drum Machine";
            // 1D layered waveforms - no Sig X/Y (meaningless for 1D).
            auto& n = graph.addNode(nodeName, NodeType::Instrument,
                {Pin{0, "MIDI", PinKind::Midi, true}},
                {Pin{0, "Audio", PinKind::Audio, false}}, {p.x, p.y});

            // Default scripts - layered waveforms with per-instrument character
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

            // Amplitude envelope on the shared node AHDSR, seeded with each
            // preset's character (Piano: slow decay + sustain + long release;
            // Drum Machine: snappy, no sustain). Edited via "Envelope (AHDSR)...".
            n.ahdsrEnvelope.attackMs  = (result == 100) ? 5.0f   : 1.0f;
            n.ahdsrEnvelope.decayMs   = (result == 100) ? 300.0f : 100.0f;
            n.ahdsrEnvelope.sustain   = (result == 100) ? 0.5f   : 0.0f;
            n.ahdsrEnvelope.releaseMs = (result == 100) ? 500.0f : 100.0f;
            // Compact param list (same rationale as Waveform Synth above).
            n.params.push_back({"Volume",  1.0f, 0.0f, 1.0f});
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
                    n.pinsIn.push_back({graph.allocId(), "MIDI In", PinKind::Midi, true});
                    n.pinsOut.push_back({graph.allocId(), "MIDI Out", PinKind::Midi, false});
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
                case 221: makeEffect("Reverb", "__reverb__", {
                    {"Mix",       0.3f,  0.0f, 1.0f},
                    {"Size",      0.6f,  0.0f, 1.0f},
                    {"Damping",   0.5f,  0.0f, 1.0f},
                    {"Width",     1.0f,  0.0f, 1.0f},
                    {"Pre-Delay", 0.0f,  0.0f, 200.0f},
                }); break;
                case 222: makeEffect("EQ", "__eq__", {
                    // Band 1 defaults to HP at 80 Hz
                    {"B1 Type", 3.0f, 0.0f, 4.0f},
                    {"B1 Freq", 80.0f, 20.0f, 20000.0f},
                    {"B1 Gain", 0.0f, -24.0f, 24.0f},
                    {"B1 Q",    0.707f, 0.1f, 10.0f},
                    // Band 2 defaults to Peak at 400 Hz
                    {"B2 Type", 0.0f, 0.0f, 4.0f},
                    {"B2 Freq", 400.0f, 20.0f, 20000.0f},
                    {"B2 Gain", 0.0f, -24.0f, 24.0f},
                    {"B2 Q",    0.707f, 0.1f, 10.0f},
                    // Band 3 defaults to Peak at 2500 Hz
                    {"B3 Type", 0.0f, 0.0f, 4.0f},
                    {"B3 Freq", 2500.0f, 20.0f, 20000.0f},
                    {"B3 Gain", 0.0f, -24.0f, 24.0f},
                    {"B3 Q",    0.707f, 0.1f, 10.0f},
                    // Band 4 defaults to LP at 8000 Hz
                    {"B4 Type", 4.0f, 0.0f, 4.0f},
                    {"B4 Freq", 8000.0f, 20.0f, 20000.0f},
                    {"B4 Gain", 0.0f, -24.0f, 24.0f},
                    {"B4 Q",    0.707f, 0.1f, 10.0f},
                }); break;
                case 239: makeEffect("SMS", "__sms__", {
                    {"Threshold",     0.1f, 0.0f,  1.0f},
                    {"Harmonic Gain", 1.0f, 0.0f,  3.0f},
                    {"Noise Gain",    1.0f, 0.0f,  3.0f},
                    {"FFT Size",     10.0f, 8.0f, 12.0f}, // 2^10=1024
                    {"Mix",           1.0f, 0.0f,  1.0f},
                }); break;
                case 238: makeEffect("Formant Pitch", "__formantpitch__", {
                    {"Semitones",    0.0f, -24.0f, 24.0f},
                    {"Formant Lock", 0.8f,   0.0f,  1.0f},
                    {"Levels",       5.0f,   1.0f,  8.0f},
                    {"Mix",          1.0f,   0.0f,  1.0f},
                }); break;
                case 237: {
                    // Wavelet Vocoder: carrier (Audio In) + modulator (Signal In)
                    auto& vcn = graph.addNode("Vocoder", NodeType::Effect,
                        {Pin{0, "Audio In", PinKind::Audio, true}},
                        {Pin{0, "Audio Out", PinKind::Audio, false}}, {p.x, p.y});
                    vcn.pinsIn.push_back({graph.allocId(), "Modulator", PinKind::Signal, true, 1});
                    vcn.script = "__waveletvocoder__";
                    vcn.params.push_back({"Bands", 5.0f, 1.0f, 8.0f});
                    vcn.params.push_back({"Mix",   1.0f, 0.0f, 1.0f});
                    break;
                }
                case 236: {
                    // Pitch Tracker: Audio In -> Signal Out (detected pitch)
                    auto& ptn = graph.addNode("Pitch Tracker", NodeType::Effect,
                        {Pin{0, "Audio In", PinKind::Audio, true}},
                        {Pin{0, "Audio Out", PinKind::Audio, false}}, {p.x, p.y});
                    ptn.pinsOut.push_back({graph.allocId(), "Pitch Out", PinKind::Signal, false});
                    ptn.script = "__pitchtracker__";
                    ptn.params.push_back({"Min Hz",      50.0f,  20.0f, 5000.0f});
                    ptn.params.push_back({"Max Hz",    2000.0f,  20.0f, 5000.0f});
                    ptn.params.push_back({"Detected Hz",  0.0f,   0.0f, 5000.0f});
                    break;
                }
                case 235: makeEffect("Asymmetric Filter", "__asymfilter__", {
                    {"Pre-Attack", 20.0f, 0.0f, 100.0f},
                    {"Post-Decay", 50.0f, 0.0f, 200.0f},
                    {"Pre Gain",    2.0f, 0.0f,   4.0f},
                    {"Post Gain",   0.5f, 0.0f,   2.0f},
                    {"Levels",      4.0f, 1.0f,   8.0f},
                    {"Mix",         1.0f, 0.0f,   1.0f},
                }); break;
                case 234: makeEffect("Complexity", "__waveletcomplexity__", {
                    {"Complexity", 0.5f, 0.0f, 1.0f},
                    {"Levels",     4.0f, 1.0f, 8.0f},
                    {"Mix",        1.0f, 0.0f, 1.0f},
                }); break;
                case 233: makeEffect("Ind. Pitch Shift", "__indpitchshift__", {
                    {"Semitones",  0.0f, -24.0f, 24.0f},
                    {"Threshold",  0.3f,   0.0f,  1.0f},
                    {"Trans Gain", 1.0f,   0.0f,  2.0f},
                    {"Levels",     4.0f,   1.0f,  8.0f},
                    {"Mix",        1.0f,   0.0f,  1.0f},
                }); break;
                case 232: makeEffect("Wavelet Reverb", "__waveletreverb__", {
                    {"Decay",  0.7f, 0.0f, 1.0f},
                    {"Color",  1.0f, 0.0f, 3.0f}, // 0=white 1=pink 2=brown
                    {"Levels", 5.0f, 1.0f, 8.0f},
                    {"Mix",    0.3f, 0.0f, 1.0f},
                }); break;
                case 231: makeEffect("Wavelet Pitch", "__waveletpitch__", {
                    {"Semitones", 0.0f, -24.0f, 24.0f},
                    {"Mix",       1.0f,   0.0f,  1.0f},
                }); break;
                case 230: makeEffect("Wavelet MB Comp", "__waveletmbcomp__", {
                    {"Threshold", -20.0f, -60.0f, 0.0f},
                    {"Ratio",       4.0f,   1.0f, 20.0f},
                    {"Levels",      4.0f,   1.0f,  6.0f},
                    {"Low Gain",    0.0f, -12.0f, 12.0f},
                    {"High Gain",   0.0f, -12.0f, 12.0f},
                    {"Mix",         1.0f,   0.0f,  1.0f},
                }); break;
                case 229: makeEffect("Octave Shift", "__octaveshift__", {
                    {"Shift", -1.0f, -2.0f, 2.0f},
                    {"Mix",    0.5f,  0.0f, 1.0f},
                }); break;
                case 228: makeEffect("Wavelet Bitcrush", "__waveletbitcrush__", {
                    {"Bits",    4.0f, 1.0f, 16.0f},
                    {"Band Lo", 0.0f, 0.0f, 7.0f},
                    {"Band Hi", 7.0f, 0.0f, 7.0f},
                    {"Levels",  4.0f, 1.0f, 8.0f},
                    {"Mix",     1.0f, 0.0f, 1.0f},
                }); break;
                case 227: makeEffect("Denoiser", "__denoiser__", {
                    {"Threshold", 0.1f, 0.0f, 1.0f},
                    {"Levels",    4.0f, 1.0f, 8.0f},
                    {"Mix",       1.0f, 0.0f, 1.0f},
                }); break;
                case 226: makeEffect("Transient Split", "__transientsplit__", {
                    {"Transient", 1.0f, 0.0f, 2.0f},
                    {"Sustain",   1.0f, 0.0f, 2.0f},
                    {"Threshold", 0.3f, 0.0f, 1.0f},
                    {"Levels",    4.0f, 1.0f, 8.0f},
                }); break;
                case 223: makeEffect("Ring Mod", "__ringmod__", {
                    {"Mix",       0.5f,   0.0f, 1.0f},
                    {"Int Freq",  440.0f, 20.0f, 20000.0f},
                    {"Int Shape", 0.0f,   0.0f, 2.0f}, // 0=sine 1=square 2=tri
                }); break;
                case 224: makeEffect("M/S Encode", "__msencode__", {
                    {"Mode", 0.0f, 0.0f, 1.0f}, // 0=encode
                }); break;
                case 225: makeEffect("M/S Decode", "__msdecode__", {
                    {"Mode", 1.0f, 0.0f, 1.0f}, // 1=decode
                }); break;
                case 213: {
                    auto& cn = makeEffect("Compressor", "__compressor__", {
                        {"Threshold", -20.0f, -60.0f, 0.0f},
                        {"Ratio", 4.0f, 1.0f, 20.0f},
                        {"Attack", 10.0f, 0.1f, 100.0f},
                        {"Release", 100.0f, 10.0f, 1000.0f},
                        {"Makeup Gain", 0.0f, 0.0f, 30.0f},
                    });
                    // Add a Signal input pin for sidechain detection.
                    // Wire any audio source (kick drum track, etc.) into
                    // this pin and the compressor's envelope follower
                    // triggers from that signal instead of the main input.
                    cn.pinsIn.push_back({graph.allocId(), "Sidechain",
                                         PinKind::Signal, true, 1});
                    break;
                }
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
                // MIDI Modulator - MIDI in + N Signal ins -> MIDI out.
                // Default: one velocity-scaling rule with one Signal input.
                // The editor lets the user add more inputs, each targeting
                // a different MIDI attribute.
                auto& n = graph.addNode("MIDI Mod", NodeType::Effect,
                    {}, {}, {p.x, p.y});
                n.pinsIn.clear();
                n.pinsOut.clear();
                n.pinsIn.push_back({graph.allocId(),  "MIDI In",  PinKind::Midi,   true});
                n.pinsIn.push_back({graph.allocId(),  "Sig 1",    PinKind::Signal, true, 1});
                n.pinsOut.push_back({graph.allocId(), "MIDI Out", PinKind::Midi,   false});
                n.script = MidiModDoc::defaultDoc().encode();
                break;
            }
            case 219: {
                // Trigger node - MIDI in + Audio in, MIDI out + Signal out.
                // Uses the Effect node type but has two distinct output pins
                // (one MIDI, one Signal) rather than the usual MIDI-only or
                // audio-only effect layout.
                //
                // The Audio In pin feeds the AudioThreshold firing mode:
                // TriggerProcessor::processBlock scans audio buffer channel 0
                // for level crossings against the rule's thresholdDb (see
                // trigger_node.cpp:431-456). Audio is optional - rules using
                // NoteOn / NoteOff don't need anything wired here.
                auto& n = graph.addNode("Trigger", NodeType::Effect,
                    {}, {}, {p.x, p.y});
                n.pinsIn.clear();
                n.pinsOut.clear();
                n.pinsIn.push_back({graph.allocId(),  "MIDI In",    PinKind::Midi,   true});
                n.pinsIn.push_back({graph.allocId(),  "Audio In",   PinKind::Audio,  true});
                n.pinsOut.push_back({graph.allocId(), "MIDI Out",   PinKind::Midi,   false});
                n.pinsOut.push_back({graph.allocId(), "Signal Out", PinKind::Signal, false});
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
        // "Record Here" (#77): arm this specific node for recording
        // so the next Record action captures into it instead of the
        // default active editor. Toggleable.
        menu.addItem(163, node.recordArmed ? "Disarm Recording" : "Record Here",
                     true, node.recordArmed);
    }
    if (node.plugin || node.type == NodeType::Instrument || node.type == NodeType::Effect) {
        menu.addItem(4, "Show Plugin UI");
        menu.addItem(7, "Presets...");
        menu.addItem(8, "MIDI Map...");
        if (node.pluginIndex >= 0)
            menu.addItem(6, "Plugin Info...");
        // MPE handshake for hosted plugins that take MIDI: emits the MPE
        // Configuration Message so an MPE-capable plugin interprets incoming
        // channels 2..16 as per-note member channels. Harmless on non-MPE
        // plugins (they ignore the RPN). Built-in synths read MPE natively
        // and don't get this entry.
        //
        // The label carries the caveat inline because JUCE PopupMenu items
        // can't show hover tooltips, and the warning matters at the decision
        // point: turning this on only helps when the MIDI feeding this plugin
        // is actually MPE (an MPE-enabled timeline or an MPE controller wired
        // in). With a non-MPE source every note lands on the master channel
        // (ch 1), which a strict MPE plugin may play flat or not voice at all.
        if (node.plugin) {
            bool hasMidiIn = false;
            for (auto& p : node.pinsIn)
                if (p.kind == PinKind::Midi) { hasMidiIn = true; break; }
            if (hasMidiIn)
                menu.addItem(181,
                             node.mpeEnabled ? "Disable MPE"
                                             : "Enable MPE (needs an MPE source)",
                             true, node.mpeEnabled);
        }
    }
    // Envelope editor on synths whose amplitude envelope IS the shared node
    // AHDSR. These read node.ahdsrEnvelope directly through the shared
    // AHDSREnvelopeRuntime: the Terrain/wavetable engine plus the Additive,
    // PD, and Spectral Grain synths. Synths that supply their own amplitude
    // envelope - FM (per-operator), Particle (per-grain), Drum (per-sound),
    // and the sample/region-file players (SoundFont, SFZ, Sfizz,
    // MultiSampler) - are NOT offered the editor, because editing it would be
    // inert (a silent lie). Extending a shared master-VCA to those synths is
    // tracked as future work in known-issues.md. Raw plugin-hosting
    // Instruments (pluginIndex >= 0) have their envelope inside the plugin.
    bool isTonalSynth = false;
    if (node.type == NodeType::TerrainSynth) {
        isTonalSynth = true;
    } else if (node.type == NodeType::Instrument && node.pluginIndex < 0) {
        auto isScript = [&](const char* tag) {
            return node.script.rfind(tag, 0) == 0;
        };
        bool ownEnvelope =
            isScript("__fmsynth__") || isScript("__particlesynth__") ||
            isScript("__drumsynth__") || isScript("__sf2__") ||
            isScript("__sfz__") || isScript("__sfizz__") ||
            isScript(MultiSamplerDoc::kPrefix);
        isTonalSynth = !ownEnvelope;
    }
    if (isTonalSynth)
        menu.addItem(180, "Envelope (AHDSR)...");

    // Signal Shape gets an "Edit Shape" entry (and we hide it for the
    // sibling XY Pad / Control Bank nodes, which share NodeType::SignalShape
    // but have their own dedicated editors opened via double-click).
    if (node.type == NodeType::SignalShape && node.script != "__xypad__"
        && node.script.rfind("__controlbank__", 0) != 0)
        menu.addItem(190, "Edit Shape...");
    if (node.type == NodeType::SignalShape && node.script.rfind("__controlbank__", 0) == 0)
        menu.addItem(191, "Edit Control Bank...");
    if (node.type == NodeType::MidiScript)
        menu.addItem(192, "Edit Program...");
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

    // Convolution auto-merge (#33): offer to merge with downstream convolution.
    if (node.script.rfind("__convolution__:", 0) == 0) {
        // Find a downstream convolution node (connected via this node's output).
        int downstreamConvId = -1;
        for (auto& link : graph.links) {
            for (auto& pin : node.pinsOut) {
                if (pin.id == link.startPin) {
                    for (auto& other : graph.nodes) {
                        if (other.id == node.id) continue;
                        for (auto& dstPin : other.pinsIn)
                            if (dstPin.id == link.endPin && other.script.rfind("__convolution__:", 0) == 0)
                                downstreamConvId = other.id;
                    }
                }
            }
        }
        if (downstreamConvId >= 0)
            menu.addItem(170, "Merge with downstream convolution");
    }

    int nodeId = node.id;
    menu.showMenuAsync(juce::PopupMenu::Options(), [this, nodeId](int result) {
        auto* node = graph.findNode(nodeId);
        if (!node) return;

        if (result == 1) {
            // Delete node (and, if it's a Group container, every node
            // inside the group - this is the entry point used when the
            // user right-clicks the grey container left behind by a
            // tracker import and chooses Delete to nuke the whole
            // import in one shot).
            deleteNodeAndDescendants(nodeId);
        } else if (result == 2) {
            auto& dup = graph.addNode(node->name, node->type, {}, {},
                {node->pos.x + 50, node->pos.y + 50});
            for (auto& p : node->pinsIn) dup.pinsIn.push_back({graph.allocId(), p.name, p.kind, true, p.channels});
            for (auto& p : node->pinsOut) dup.pinsOut.push_back({graph.allocId(), p.name, p.kind, false, p.channels});
            dup.params = node->params;
            dup.clips = node->clips;
        } else if (result == 3) {
            bool already = false;
            for (int edId : graph.openEditors)
                if (edId == nodeId) { already = true; break; }
            if (!already)
                graph.openEditors.insert(graph.openEditors.begin(), nodeId);
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
        } else if (result == 181) {
            // Plugin MPE toggle: adds/removes the parallel MCM generator node,
            // so it needs a graph rebuild (unlike the timeline toggle above,
            // which only changes how notes are emitted inside processBlock).
            node->mpeEnabled = !node->mpeEnabled;
            graph.commitSnapshot(node->mpeEnabled ? "Enable plugin MPE"
                                                  : "Disable plugin MPE");
            if (onNodeEdited) onNodeEdited();
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
        } else if (result == 163) {
            // "Record Here" toggle (#77)
            node->recordArmed = !node->recordArmed;
            graph.dirty = true;
        } else if (result == 180) {
            // Open the shared AHDSR envelope editor on this node.
            // The dialog hosts the reusable AHDSREnvelopeComponent editing
            // node->ahdsrEnvelope by reference. node->ahdsrEnvelope is the
            // single source of truth that every tonal synth reads directly,
            // so the onChanged callback only needs to mark the graph dirty
            // (the undo snapshot is committed once when the dialog closes).
            int captured = nodeId;
            auto* content = new AHDSREnvelopeComponent(node->ahdsrEnvelope,
                [this, captured]() {
                    if (auto* n = graph.findNode(captured))
                        graph.dirty = true;
                });
            content->setSize(700, 420);
            juce::DialogWindow::LaunchOptions opt;
            opt.dialogTitle = "Envelope - " + juce::String(node->name);
            opt.content.setOwned(content);
            opt.escapeKeyTriggersCloseButton = true;
            opt.useNativeTitleBar = true;
            opt.resizable = true;
            opt.componentToCentreAround = this;
            opt.launchAsync();
            // Snapshot is committed when the dialog closes via the
            // surrounding "Edit envelope" undo step; the per-keystroke
            // onChanged calls only mark dirty so undo doesn't fragment
            // into one step per slider tick. We commit a single
            // snapshot here so an undo right after closing the dialog
            // reverts the whole edit.
            graph.commitSnapshot("Edit envelope");
        } else if (result == 190) {
            // Open the Signal Shape editor for an existing node. Same
            // launch flow as the "create + open" path in the menu above,
            // including the manual-trigger lookup callback.
            int captured = nodeId;
            auto* editor = new SignalShapeEditorComponent(graph, captured,
                [this]() {
                    if (onNodeEdited) onNodeEdited();
                    repaint();
                },
                [this, captured]() {
                    if (onSignalShapeManualTrigger) onSignalShapeManualTrigger(captured);
                });
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned(editor);
            opts.dialogTitle = "Signal Shape: " + juce::String(node->name);
            opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar = false;
            opts.resizable = true;
            opts.componentToCentreAround = this;
            SoundShop::launchToolDialog(opts);
        } else if (result == 191) {
            // Open the Control Bank editor for an existing node.
            int captured = nodeId;
            auto* bank = new ControlBankComponent(graph, captured,
                [this]() {
                    if (onNodeEdited) onNodeEdited();
                    repaint();
                });
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned(bank);
            opts.dialogTitle = "Control Bank: " + juce::String(node->name);
            opts.dialogBackgroundColour = juce::Colour(25, 25, 32);
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar = false;
            opts.resizable = true;
            opts.componentToCentreAround = this;
            SoundShop::launchToolDialog(opts);
        } else if (result == 192) {
            // Open the MIDI Script editor for an existing node.
            int captured = nodeId;
            auto* editor = new MidiScriptEditorComponent(graph, captured,
                [this]() {
                    if (onNodeEdited) onNodeEdited();
                    repaint();
                });
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned(editor);
            opts.dialogTitle = "MIDI Script: " + juce::String(node->name);
            opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar = false;
            opts.resizable = true;
            opts.componentToCentreAround = this;
            SoundShop::launchToolDialog(opts);
        } else if (result == 170) {
            // Convolution auto-merge (#33): convolve this node's IR with
            // the downstream convolution's IR, put the result in this node,
            // and remove the downstream node + link.
            if (node->script.rfind("__convolution__:", 0) == 0) {
                // Find the downstream convolution node.
                int downId = -1;
                for (auto& link : graph.links) {
                    for (auto& pin : node->pinsOut)
                        if (pin.id == link.startPin) {
                            for (auto& other : graph.nodes) {
                                if (other.id == nodeId) continue;
                                for (auto& dp : other.pinsIn)
                                    if (dp.id == link.endPin &&
                                        other.script.rfind("__convolution__:", 0) == 0)
                                        downId = other.id;
                            }
                        }
                }
                if (downId >= 0) {
                    auto irA = ConvolutionProcessor::decodeIR(node->script);
                    auto* downNode = graph.findNode(downId);
                    if (downNode) {
                        auto irB = ConvolutionProcessor::decodeIR(downNode->script);
                        if (!irA.empty() && !irB.empty()) {
                            auto merged = ConvolutionProcessor::convolveIRs(irA, irB);
                            node->script = ConvolutionProcessor::encodeIR(merged);
                            // Rewire downstream's outputs to this node's output.
                            for (auto& link : graph.links)
                                for (auto& dp : downNode->pinsOut)
                                    if (dp.id == link.startPin)
                                        for (auto& myOut : node->pinsOut)
                                            link.startPin = myOut.id;
                            // Remove the downstream node + its links.
                            // Collect pin IDs first, then erase.
                            std::vector<int> downPinIds;
                            for (auto& p : downNode->pinsIn)  downPinIds.push_back(p.id);
                            for (auto& p : downNode->pinsOut) downPinIds.push_back(p.id);
                            // Guard the structural edit against the audio
                            // callback iterating graph.nodes/links (see the
                            // mutationLock comment in deleteNodeAndDescendants).
                            {
                                std::lock_guard<std::mutex> graphLk(graph.mutationLock);
                                graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
                                    [&downPinIds](const Link& l) {
                                        for (int pid : downPinIds)
                                            if (l.startPin == pid || l.endPin == pid) return true;
                                        return false;
                                    }), graph.links.end());
                                graph.nodes.erase(std::remove_if(graph.nodes.begin(), graph.nodes.end(),
                                    [downId](const Node& nn) { return nn.id == downId; }), graph.nodes.end());
                            }
                            graph.dirty = true;
                            graph.commitSnapshot("Merge convolutions");
                        }
                    }
                }
            }
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
            return; // don't repaint yet - modal dialog handles it
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
    // Topology change - keep undo tree and graph.links in sync (see
    // mouseUp's matching commitSnapshot for why this matters at quit/restart).
    graph.commitSnapshot("Delete connection");
    repaint();
}

void NodeGraphComponent::deleteSelectedNode() {
    if (selectedNodeId < 0) return;
    if (!graph.findNode(selectedNodeId)) return;
    deleteNodeAndDescendants(selectedNodeId);
    selectedNodeId = -1;
    repaint();
}

void NodeGraphComponent::deleteNodeAndDescendants(int rootId) {
    auto* root = graph.findNode(rootId);
    if (!root) return;

    // If this root is a MOD-import root group that overrode the global song
    // settings on import, capture the stashed PRE-import values now (before
    // the node is erased and `root` dangles). We restore them after the
    // deletion so removing the whole module backs out its loop contribution
    // and returns the song settings to whatever the user had before import.
    // This only fires for the import's root group node — single child nodes
    // never carry modImportSavedSong, so deleting one node of a mod leaves
    // the song settings untouched, as required.
    const bool   restoreModSong   = root->modImportSavedSong;
    const int    rmRepeatMode      = root->modImportPrevRepeatMode;
    const int    rmRepeatCount      = root->modImportPrevRepeatCount;
    const double rmSongLength      = root->modImportPrevSongLength;
    const bool   rmLoopEnabled      = root->modImportPrevLoopEnabled;
    const double rmLoopStart        = root->modImportPrevLoopStart;
    const double rmLoopEnd          = root->modImportPrevLoopEnd;

    // Collect every node to delete: the root plus, if it's a Group, every
    // descendant via childNodeIds (recursively, so a group-of-groups
    // cascades fully). Set guards against accidental cycles in malformed
    // childNodeIds data.
    std::set<int> victims;
    std::vector<int> stack { rootId };
    while (!stack.empty()) {
        int id = stack.back();
        stack.pop_back();
        if (!victims.insert(id).second) continue;
        auto* n = graph.findNode(id);
        if (!n) continue;
        if (n->type == NodeType::Group) {
            for (int childId : n->childNodeIds)
                if (!victims.count(childId))
                    stack.push_back(childId);
        }
    }

    // Gather all pin IDs across the victim set so we can sweep matching
    // links in one pass instead of N passes. Also remember whether any
    // victim was a timeline node — we use that below to decide whether to
    // reset the explicit song-length override.
    std::set<int> pinIds;
    bool anyTimelineDeleted = false;
    for (int id : victims) {
        if (auto* n = graph.findNode(id)) {
            for (auto& p : n->pinsIn)  pinIds.insert(p.id);
            for (auto& p : n->pinsOut) pinIds.insert(p.id);
            if (n->type == NodeType::AudioTimeline ||
                n->type == NodeType::MidiTimeline)
                anyTimelineDeleted = true;
        }
    }
    // Hold the graph mutation lock across the entire structural edit (links
    // erase + nodes erase + the scalar song-setting fixups + commitSnapshot's
    // serialization read). The audio callback iterates graph.nodes and
    // graph.links under a try-lock (audio_engine.cpp ~210); without pairing
    // the lock here, erasing nodes/links while the audio thread was mid-
    // iteration produced torn reads / use-after-free - the same race that
    // crashed tracker import (.63000.dmp) and, more recently, deleting the
    // wavetable node (SEANCE.exe.80308.dmp). The lock_guard lives to end of
    // function; commitSnapshot only serializes (reads) the graph and never
    // takes mutationLock, so holding it across the snapshot is deadlock-free
    // and additionally prevents an audio-thread rebuild mid-serialization.
    std::lock_guard<std::mutex> graphLk(graph.mutationLock);

    graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
        [&](auto& l) { return pinIds.count(l.startPin) || pinIds.count(l.endPin); }),
        graph.links.end());

    // Notify upstream (editor panels, audio engine, etc.) so they can
    // drop any dangling references before the nodes vanish.
    for (int id : victims)
        if (onNodeDeleted) onNodeDeleted(id);

    // Close open editor panels for every victim.
    graph.openEditors.erase(std::remove_if(graph.openEditors.begin(), graph.openEditors.end(),
        [&](int id) { return victims.count(id) > 0; }), graph.openEditors.end());

    // Unlink the root from any parent group's childNodeIds (descendants
    // are members of `root`, which is going away with them, so they don't
    // need separate parent-group cleanup).
    if (root->parentGroupId >= 0)
        graph.removeFromGroup(rootId);

    // Drop all victims from graph.nodes in one pass.
    graph.nodes.erase(std::remove_if(graph.nodes.begin(), graph.nodes.end(),
        [&](auto& n) { return victims.count(n.id) > 0; }), graph.nodes.end());

    // Clear stale selections if the user had a victim selected.
    if (victims.count(selectedNodeId)) selectedNodeId = -1;

    // If any deleted node was a timeline, revert songLengthBeats to "auto"
    // (0) so the effective length recomputes from the remaining timelines.
    // An explicit override that referenced a now-deleted track would
    // otherwise keep the song playing past the actual content.
    if (anyTimelineDeleted && graph.songLengthBeats > 0)
        graph.songLengthBeats = 0;

    // Restore the pre-import song settings if this root group node had
    // overridden them on import. Done after the timeline-reset above so the
    // user's original choice wins over the auto-reset (deleting the module
    // should return to the pre-import state, not a half-reset one).
    if (restoreModSong) {
        graph.songRepeatMode  = (NodeGraph::SongRepeat)rmRepeatMode;
        graph.songRepeatCount = rmRepeatCount;
        graph.songLengthBeats = rmSongLength;
        graph.loopEnabled     = rmLoopEnabled;
        graph.loopStartBeat   = rmLoopStart;
        graph.loopEndBeat     = rmLoopEnd;
    }

    graph.dirty = true;
    graph.commitSnapshot(victims.size() > 1
        ? "Delete group (" + std::to_string(victims.size()) + " nodes)"
        : "Delete node");
}

void NodeGraphComponent::showLinkMenu(int linkId) {
    // Find the link
    Link* link = nullptr;
    for (auto& l : graph.links)
        if (l.id == linkId) { link = &l; break; }

    juce::PopupMenu menu;

    // Header: show the wire's signal type so the user knows what they're
    // looking at (the pin colour alone isn't self-explanatory). When the two
    // endpoints differ (an implicit Param<->Signal conversion), show both.
    if (link) {
        PinKind srcKind = PinKind::Audio, dstKind = PinKind::Audio;
        bool foundSrc = false, foundDst = false;
        for (auto& node : graph.nodes) {
            for (auto& pin : node.pinsOut)
                if (pin.id == link->startPin) { srcKind = pin.kind; foundSrc = true; }
            for (auto& pin : node.pinsIn)
                if (pin.id == link->endPin)   { dstKind = pin.kind; foundDst = true; }
        }
        if (foundSrc && foundDst) {
            double sr = 0.0; int bs = 0;
            if (getAudioFormat) { auto fmt = getAudioFormat(); sr = fmt.first; bs = fmt.second; }
            if (srcKind == dstKind) {
                menu.addSectionHeader(nameForPinKind(srcKind, sr, bs));
            } else {
                // Implicit Param<->Signal conversion: keep the header short by
                // naming the two kinds with a plain-language note about what the
                // conversion does to the update rate.
                auto shortName = [](PinKind k) -> juce::String {
                    switch (k) {
                        case PinKind::Audio:  return "Audio";
                        case PinKind::Midi:   return "MIDI";
                        case PinKind::Param:  return "Param";
                        case PinKind::Signal: return "Signal";
                    }
                    return "?";
                };
                juce::String rateTxt = (sr > 0.0 && bs > 0)
                    ? juce::String((int)std::lround(sr / (double)bs)) + "x/sec"
                    : "block-rate";
                juce::String note = (dstKind == PinKind::Param)
                    ? " (resampled to " + rateTxt + ")"
                    : (dstKind == PinKind::Signal ? " (upsampled to every sample)" : "");
                menu.addSectionHeader(shortName(srcKind) + " \xe2\x86\x92 "
                                      + shortName(dstKind) + note);
            }
        }
    }

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
            // See mouseUp's commitSnapshot - topology changes need to
            // commit, otherwise the deletion is undone at next startup
            // when the persisted undo tree's current snapshot is reapplied
            // over the freshly-loaded .ssp.
            graph.commitSnapshot("Delete connection");
        } else if (result >= 10 && result <= 16) {
            float gains[] = {0, -3, -6, -12, -20, 3, 6};
            lk->gainDb = gains[result - 10];
            graph.dirty = true;
            // A gain change doesn't alter node/link counts, so the audio graph
            // won't auto-rebuild. Without a rebuild the GainProcessor (only
            // inserted when gainDb != 0) is never created/removed, so the new
            // gain has no audible effect. Force a rebuild.
            if (onNodeEdited) onNodeEdited();
            graph.commitSnapshot("Set connection gain");
        } else if (result == 31) {
            // Help: Effect Groups -> open the docs page
            if (onOpenHelpDoc) onOpenHelpDoc("layers-and-groups.html");
            return;
        } else if (result == 30) {
            // New effect group - prompt for optional name
            auto* aw = new juce::AlertWindow("New Effect Group",
                "Name is optional - the group is always identified by its colored diamond tag.",
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
                        graph.commitSnapshot("New effect group");
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
                graph.commitSnapshot("Toggle effect group membership");
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
                        // Force an audio-graph rebuild so the GainProcessor is
                        // inserted/updated (gain change alone won't trigger one).
                        if (onNodeEdited) onNodeEdited();
                        graph.commitSnapshot("Set connection gain");
                    }
                    delete aw;
                    repaint();
                }), true);
            return;
        }
        repaint();
    });
}

// ==============================================================================
// Terrain node factory (#? - N-D terrain creation)
// ==============================================================================

Node& NodeGraphComponent::makeTerrainNode(const std::string& name,
                                          const std::string& script,
                                          juce::Point<float> canvasPos,
                                          int numDims) {
    static const char* axisNames[] = {"X", "Y", "Z", "W", "V", "U", "S", "T"};
    numDims = juce::jlimit(1, 8, numDims);

    std::vector<Pin> inPins;
    inPins.push_back(Pin{0, "MIDI", PinKind::Midi, true});
    for (int d = 0; d < numDims; ++d)
        inPins.push_back(Pin{0, std::string("Sig ") + axisNames[d],
                             PinKind::Signal, true, 1});

    auto& n = graph.addNode(name, NodeType::TerrainSynth,
        inPins,
        {Pin{0, "Audio", PinKind::Audio, false}},
        {canvasPos.x, canvasPos.y});
    n.script = script;
    // Amplitude envelope lives in node.ahdsrEnvelope (single source of truth),
    // not as Attack/Decay/Sustain/Release params. Seed a sensible default.
    n.ahdsrEnvelope.attackMs  = 10.0f;
    n.ahdsrEnvelope.decayMs   = 100.0f;
    n.ahdsrEnvelope.sustain   = 0.7f;
    n.ahdsrEnvelope.releaseMs = 300.0f;
    n.params.push_back({"Volume",   0.5f,  0.0f,   1.0f});
    n.params.push_back({"Pan",      0.0f, -1.0f,   1.0f});
    n.params.push_back({"Speed",        1.0f,  0.01f, 20.0f});
    // All radii first, then all centres - matches the original 2D ordering.
    for (int d = 0; d < numDims; ++d)
        n.params.push_back({std::string("Radius ") + axisNames[d],
                            0.3f, 0.0f, 0.5f});
    for (int d = 0; d < numDims; ++d)
        n.params.push_back({std::string("Center ") + axisNames[d],
                            0.5f, 0.0f, 1.0f});
    n.params.push_back({"Rad Mod Spd",  0.0f,  0.0f,  10.0f});
    n.params.push_back({"Rad Mod Amt",  0.0f,  0.0f,   0.3f});
    n.params.push_back({"Traversal",    0.0f,  0.0f,   3.0f}); // 0=Orbit,1=Linear,2=Lissajous,3=Physics
    // Synth Mode: 0=Direct (SamplePerPoint), 1=AM-sine (WaveformPerPoint),
    // 2=Additive bank (per-partial sines). Direct is natural for 1D
    // wavetable/audio terrains; AM-sine is the only meaningful mode for
    // 2D/N-D terrains (image, math expression, fractal noise); Additive
    // bank applies only to 1D wavetable cycles.
    n.params.push_back({"Synth Mode",   0.0f,  0.0f,   2.0f});
    n.params.push_back({"LFO1 Rate",    0.5f,  0.01f, 20.0f});
    n.params.push_back({"LFO2 Rate",    0.2f,  0.01f, 20.0f});
    n.params.push_back({"LFO1 Amount",  0.0f,  0.0f,   1.0f});
    n.params.push_back({"LFO2 Amount",  0.0f,  0.0f,   1.0f});
    n.params.push_back({"Grain Size",   0.0f,  0.0f,   0.5f});
    n.params.push_back({"Freeze",       0.0f,  0.0f,   1.0f});
    n.params.push_back({"Grain Jitter", 0.0f,  0.0f,   1.0f});
    return n;
}

} // namespace SoundShop

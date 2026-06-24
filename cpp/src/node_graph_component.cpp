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
#include "video_import_dialog.h"
#include "generate_dialog.h"
#include "script_runtime.h" // ScriptLang for generate-terrain default
#include "adsr_envelope_component.h"
#include "envelope_presets.h"
#include "audio_export.h" // WAV export of 1D generated terrains
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

// ---------------------------------------------------------------------------
// Node row layout helpers.
//
// A node body (below the header) has two stacked regions:
//   1. TOP PIN REGION  - structural I/O pins (audio / MIDI / signal that aren't
//      bound to a param), interleaved input-left / output-right, one per row.
//   2. PARAM REGION    - one row per param. A param that owns an on-demand
//      modulation pin (#88) renders that pin IN PLACE on the left edge of its
//      own row, rather than as a separate top-region row. So pinning a param
//      no longer shoves a pin to the top and grows the node - the slider just
//      grows a connector on its own row, and the node stays the same height.
//
// These helpers are the single source of truth shared by getNodeBounds /
// getPinPosition / drawNode and every hit-test, so the drawn layout and the
// click targets can never drift apart.
// ---------------------------------------------------------------------------

// True if pinId is a param-modulation pin (folded into its param's row) rather
// than a structural top-region pin. A "Mod:/Set:" pin with no live modPin
// binding (legacy / orphaned) returns false and stays a structural pin until
// its binding is repaired on right-click (showPinMenu self-heal).
static bool isParamModPinId(const Node& node, int pinId) {
    for (const auto& mp : node.modPins)
        if (mp.pinId == pinId) return true;
    return false;
}

// Structural (non-param-mod) input pins, in pinsIn order.
static std::vector<const Pin*> structuralInputPins(const Node& node) {
    std::vector<const Pin*> v;
    v.reserve(node.pinsIn.size());
    for (const auto& p : node.pinsIn)
        if (!isParamModPinId(node, p.id)) v.push_back(&p);
    return v;
}

// Rows in the top pin region = max(structural inputs, outputs).
static int numTopPinRows(const Node& node) {
    return std::max((int)structuralInputPins(node).size(),
                    (int)node.pinsOut.size());
}

// Node bounds in canvas coordinates
juce::Rectangle<float> NodeGraphComponent::getNodeBounds(const Node& node) const {
    int numRows = numTopPinRows(node) + (int)node.params.size();
    float h = HEADER_HEIGHT + std::max(numRows, 1) * PIN_ROW_HEIGHT + 8;
    return {node.pos.x, node.pos.y, NODE_WIDTH, h};
}

// Pin position in canvas coordinates
juce::Point<float> NodeGraphComponent::getPinPosition(const Node& node, const Pin& pin) const {
    auto bounds = getNodeBounds(node);
    const float topY = bounds.getY() + HEADER_HEIGHT;

    if (pin.isInput) {
        // A param-modulation pin lives ON its bound param's row, not in the top
        // region. Compute that row's centre on the node's left edge.
        for (const auto& mp : node.modPins) {
            if (mp.pinId == pin.id && mp.paramIndex >= 0
                && mp.paramIndex < (int)node.params.size()) {
                float y = topY + (numTopPinRows(node) + mp.paramIndex) * PIN_ROW_HEIGHT
                          + PIN_ROW_HEIGHT / 2;
                return {bounds.getX(), y};
            }
        }
        // Structural input pin: row index among structural inputs (mod pins skipped).
        int idx = 0;
        for (const auto& p : node.pinsIn) {
            if (isParamModPinId(node, p.id)) continue;
            if (p.id == pin.id)
                return {bounds.getX(), topY + idx * PIN_ROW_HEIGHT + PIN_ROW_HEIGHT / 2};
            ++idx;
        }
    } else {
        int idx = 0;
        for (const auto& p : node.pinsOut) {
            if (p.id == pin.id)
                return {bounds.getRight(), topY + idx * PIN_ROW_HEIGHT + PIN_ROW_HEIGHT / 2};
            ++idx;
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
        case NodeType::MidiBreakout:  return juce::Colour(40, 140, 110); // MIDI green, control-signal tint
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

    // Pre-compute on-face latency badges. The common case (nothing in the graph
    // reports latency) bails immediately, so a zero-latency graph pays nothing
    // and shows no badges. Otherwise compute each node's accumulated "to here"
    // latency once (shared memo across nodes), cached for drawNode to read.
    latencyBadgeTotals.clear();
    latencyBadgeSampleRate = (getAudioFormat ? getAudioFormat().first : 0.0);
    if (getNodeLatencies) {
        auto own = getNodeLatencies();
        bool anyNonZero = false;
        for (auto& kv : own) if (kv.second > 0) { anyNonZero = true; break; }
        if (anyNonZero) {
            std::unordered_map<int, int> memo;
            for (auto& node : graph.nodes) {
                std::unordered_set<int> visiting;
                int tot = cumulativeLatencyTo(node.id, own, memo, visiting);
                if (tot > 0) latencyBadgeTotals[node.id] = tot;
            }
        }
    }

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

    // Latency badge: a small pill on the node face showing the delay the signal
    // has accumulated by the time it leaves this node (own latency + the longest
    // upstream path). Only present when non-zero - basically only when a hosted
    // latency-bearing plugin is in the chain - so zero-latency graphs stay clean.
    // Drawn at the node's bottom-left; hidden when zoomed too far out to read.
    // Right-click the node for the full breakdown (own vs. to-here).
    if (zoom > 0.45f) {
        auto lit = latencyBadgeTotals.find(node.id);
        if (lit != latencyBadgeTotals.end() && lit->second > 0) {
            int samples = lit->second;
            juce::String txt = (latencyBadgeSampleRate > 0.0)
                ? juce::String(1000.0 * samples / latencyBadgeSampleRate, 1) + " ms"
                : juce::String(samples) + " smp";
            float fs = std::max(7.0f, 9.0f * zoom);
            g.setFont(juce::Font(fs));
            float padX = 4 * zoom, padY = 2 * zoom;
            float tw = g.getCurrentFont().getStringWidthFloat(txt) + padX * 2;
            float th = fs + padY * 2;
            auto bl = canvasToScreen(bounds.getBottomLeft());
            juce::Rectangle<float> pill(bl.x + 4 * zoom, bl.y - th - 4 * zoom, tw, th);
            g.setColour(juce::Colours::black.withAlpha(0.55f));
            g.fillRoundedRectangle(pill, 3 * zoom);
            g.setColour(juce::Colours::orange);
            g.drawRoundedRectangle(pill, 3 * zoom, 1.0f);
            g.setColour(juce::Colours::white);
            g.drawText(txt, pill, juce::Justification::centred);
        }
    }

    // Pins
    float pinY = bounds.getY() + HEADER_HEIGHT;
    auto drawPin = [&](const Pin& pin, bool isInput, bool hasOpposite, bool withLabel = true) {
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

        // A param-modulation pin folded onto its param row needs no label: the
        // param row already shows the param name. Drawing "Mod: <param>" again
        // would just collide with it. So the param-row caller passes withLabel
        // = false and we draw only the connector dot.
        if (!withLabel) return;

        // Label. Give it the full width from the pin to the node's far edge so a
        // long control-pin name ("Mod: Tape Saturate") shows in full and only
        // ellipsizes when it actually reaches the node's edge (matching the param
        // sliders below). When the row carries BOTH an input and an output pin,
        // split at the node centre so the two labels never overlap.
        float labelFontSize = std::max(8.0f, 11.0f * zoom);
        g.setFont(juce::Font(labelFontSize));
        g.setColour(juce::Colours::white.withAlpha(0.8f));
        float nodeLeftX  = canvasToScreen({bounds.getX(),     pinY + PIN_ROW_HEIGHT / 2.0f}).x;
        float nodeRightX = canvasToScreen({bounds.getRight(), pinY + PIN_ROW_HEIGHT / 2.0f}).x;
        float margin     = 4 * zoom;
        float centreX    = (nodeLeftX + nodeRightX) * 0.5f;
        float labelX, labelW;
        if (isInput) {
            labelX = pos.x + r + 3 * zoom;
            float rightLimit = hasOpposite ? (centreX - margin) : (nodeRightX - margin);
            labelW = std::max(10.0f * zoom, rightLimit - labelX);
        } else {
            float leftLimit  = hasOpposite ? (centreX + margin) : (nodeLeftX + margin);
            float rightEdge  = pos.x - r - 3 * zoom;
            labelX = leftLimit;
            labelW = std::max(10.0f * zoom, rightEdge - leftLimit);
        }
        auto labelRect = juce::Rectangle<float>(
            labelX, pos.y - labelFontSize / 2, labelW, labelFontSize + 2);
        g.drawText(pin.name,
                    labelRect,
                    isInput ? juce::Justification::centredLeft : juce::Justification::centredRight);
    };

    // Top pin region: structural input pins (left) + output pins (right). Param-
    // modulation pins are NOT drawn here - they render on their param's row below.
    auto structIns = structuralInputPins(node);
    int topRows = std::max((int)structIns.size(), (int)node.pinsOut.size());
    for (int i = 0; i < topRows; ++i) {
        bool inHas  = i < (int)structIns.size();
        bool outHas = i < (int)node.pinsOut.size();
        if (inHas)  drawPin(*structIns[(size_t)i], true,  outHas);
        if (outHas) drawPin(node.pinsOut[(size_t)i], false, inHas);
        pinY += PIN_ROW_HEIGHT;
    }

    // Parameter rows: drawn below the pins. Each row shows name + value plus
    // a horizontal fill bar indicating position within [min, max]. Drag the
    // row horizontally to change the value (handled in mouseDown/mouseDrag).
    // Signal-controlled params are drawn dimmed and locked - but only the
    // specific param a cable actually drives, not every param on the node.
    if (!node.params.empty() && zoom > 0.4f) {
        float paramFontSize = std::max(8.0f, 10.0f * zoom);
        g.setFont(juce::Font(paramFontSize));
        for (int pi = 0; pi < (int)node.params.size(); ++pi) {
            const auto& p = node.params[pi];
            // Lock visual is reserved for ABSOLUTE-driven params: the cable
            // sets the value edge-to-edge and the knob can't be touched. A
            // Mod-driven param stays editable (you drag its resting/base
            // value while the cable modulates around it), so it renders like
            // a normal editable row.
            bool paramLocked = graph.paramHasAbsoluteInput(node.id, pi);
            // Does this param own an on-demand modulation pin? If so its
            // connector is drawn IN PLACE on this row's left edge (below),
            // not in the top pin region.
            const Pin* modPinPin = nullptr;
            bool modPinIsAbsolute = false;
            for (const auto& mp : node.modPins)
                if (mp.paramIndex == pi) {
                    for (const auto& ip : node.pinsIn)
                        if (ip.id == mp.pinId) { modPinPin = &ip; break; }
                    modPinIsAbsolute = (mp.mode == Node::ModPin::Mode::Absolute);
                    break;
                }
            // A folded-in control pin shows a compact "Set"/"Mod" tag (drawn
            // right-aligned just left of the value, below) so the user can still
            // tell the pin's mode at a glance even though the full "Set:/Mod:
            // <param>" pin label isn't drawn on the row (the param name already
            // occupies the left). Measure its width here for that placement.
            juce::String modTag = modPinPin ? (modPinIsAbsolute ? "Set" : "Mod")
                                            : juce::String();
            float modTagW = 0.0f;
            if (modPinPin) {
                g.setFont(juce::Font(paramFontSize));
                modTagW = g.getCurrentFont().getStringWidthFloat(modTag) + 4.0f;
            }
            float rowTop    = pinY + 2;
            float rowBottom = pinY + PIN_ROW_HEIGHT - 2;
            auto rowTL = canvasToScreen({bounds.getX() + 6, rowTop});
            auto rowBR = canvasToScreen({bounds.getRight() - 6, rowBottom});
            juce::Rectangle<float> rowRect(rowTL.x, rowTL.y, rowBR.x - rowTL.x, rowBR.y - rowTL.y);

            // Background fill bar showing the param's position within its range.
            // For an absolute-locked param we show the LIVE value the cable is
            // driving. For an editable param (manual or Mod-driven) we show the
            // resting/base value the user controls - a Mod cable swings the
            // live value around but the handle should sit at what the knob is
            // set to, not jitter with the modulation.
            float range = std::max(1e-6f, p.maxVal - p.minVal);
            float dispValue = (!paramLocked && p.modulated) ? p.baseValue : p.value;
            float frac = juce::jlimit(0.0f, 1.0f, (dispValue - p.minVal) / range);
            auto fillRect = rowRect;
            fillRect.setWidth(rowRect.getWidth() * frac);

            // Signal-locked params are dimmed (orange fill, no draggable handle)
            if (paramLocked) {
                g.setColour(juce::Colour(160, 100, 40).withAlpha(0.35f));
                g.fillRoundedRectangle(fillRect, 2.0f);
                g.setColour(juce::Colour(120, 80, 40).withAlpha(0.5f));
                g.drawRoundedRectangle(rowRect, 2.0f, 1.0f);

                // Live-value marker (Set / Absolute mode): the cable's signal IS
                // the param value, so there's a single value to show and it moves
                // every block. The graph repaints at 30Hz, so this marker tracks
                // the incoming signal in real time. It's drawn as a dimmed orange
                // bar (not white) to read as "driven, not grabbable" - distinct
                // from the bright white draggable handle on a manual param.
                float liveX = rowRect.getX() + rowRect.getWidth() * frac;
                float liveW = std::max(2.0f, 3.0f * zoom);
                juce::Rectangle<float> liveRect(liveX - liveW * 0.5f,
                                                rowRect.getY() - 1.0f,
                                                liveW,
                                                rowRect.getHeight() + 2.0f);
                g.setColour(juce::Colour(230, 150, 70).withAlpha(0.9f));
                g.fillRoundedRectangle(liveRect, 1.0f);
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

                // Mod-mode second indicator: in "Mod" (bipolar-additive) mode the
                // white handle stays at the user's resting/base value (which they
                // can still drag), while the incoming signal swings the LIVE value
                // around it. Without a separate marker the user has no way to see
                // what the modulation is actually doing. Draw a cyan marker at the
                // live modulated value (p.value) - cyan matches the "signal
                // modulation attached" dot drawn after the param name, so the two
                // read as the same concept. Updates at 30Hz with the repaint tick.
                if (p.modulated) {
                    float liveFrac = juce::jlimit(0.0f, 1.0f,
                                                  (p.value - p.minVal) / range);
                    float liveX = rowRect.getX() + rowRect.getWidth() * liveFrac;
                    // Thin translucent full-height line so it's visible even when
                    // it sits right on top of the white base handle.
                    g.setColour(juce::Colours::cyan.withAlpha(0.55f));
                    g.fillRect(liveX - 0.5f, rowRect.getY(),
                               1.0f, rowRect.getHeight());
                    // Solid caret at the bottom edge pointing up at the value, so
                    // the live marker stays legible against the fill bar.
                    float cs = std::max(2.5f, 3.0f * zoom);
                    juce::Path caret;
                    caret.addTriangle(liveX,        rowRect.getBottom() - cs,
                                      liveX - cs,    rowRect.getBottom() + 1.0f,
                                      liveX + cs,    rowRect.getBottom() + 1.0f);
                    g.setColour(juce::Colours::cyan);
                    g.fillPath(caret);
                }
            }

            // Armed indicator: red dot next to the name when armed for auto-write
            if (p.autoWriteArmed) {
                float dotX = rowRect.getX() + 2;
                float dotY = rowRect.getCentreY() - 2;
                g.setColour(juce::Colours::red);
                g.fillEllipse(dotX, dotY, 5.0f, 5.0f);
            }

            // Name (left) and value (right). The in-place modulation connector
            // dot sits in the row's 6px left margin (rowRect starts at
            // bounds.getX()+6, the dot is drawn at bounds.getX()), so it never
            // overlaps the label - the param name stays left-aligned with every
            // other row, pinned or not. The Set/Mod mode tag is drawn separately,
            // right-aligned just left of the value (below), so it doesn't push the
            // name in either.
            g.setColour(paramLocked ? juce::Colours::grey : juce::Colours::white);
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
            } else if (p.name == "Algorithm" && node.script == "__pitchdetector__") {
                valueStr = ((int)std::round(p.value) == 1) ? "Autocorr"
                                                           : juce::String("YIN");
            } else if (p.name == "Mapping" && node.script == "__pitchdetector__") {
                valueStr = ((int)std::round(p.value) == 1) ? "Linear"
                                                           : juce::String("Log");
            } else {
                valueStr = juce::String(dispValue, 2);
            }
            g.drawText(valueStr, rowRect.reduced(4, 0), juce::Justification::centredRight, false);

            // Set/Mod tag: when this row carries a modulation pin, label which
            // mode it's in (Set = an Absolute cable fully owns the value; Mod =
            // bipolar modulation around the base). Drawn in the pin's wire colour,
            // right-aligned just left of the value text so it reads as part of the
            // row without indenting the param name.
            if (modPinPin) {
                g.setFont(juce::Font(paramFontSize));
                float valW = g.getCurrentFont().getStringWidthFloat(valueStr);
                auto tagRect = rowRect.reduced(4, 0)
                                   .withTrimmedRight(valW + 6.0f)
                                   .removeFromRight(modTagW);
                g.setColour(colourForPinKind(modPinPin->kind)
                                .withAlpha(paramLocked ? 0.6f : 0.95f));
                g.drawText(modTag, tagRect, juce::Justification::centredRight, false);
            }

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

            // In-place modulation connector: the param's mod pin renders on the
            // left edge of its own row (drawPin reads the current pinY, which is
            // this row's top). No label - the param name above already names it.
            // Drawn after the row content so the dot/halo sit on top.
            if (modPinPin)
                drawPin(*modPinPin, /*isInput=*/true, /*hasOpposite=*/false,
                        /*withLabel=*/false);

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
    // bottom-left "Pressure" input could fail while a higher input succeeded.
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
        // Resolve any pin directly under the cursor up front. The dot is drawn
        // centred on the node's left/right edge, so half of it hangs OUTSIDE
        // the node bounds - nodeAtPoint() (a getNodeBounds().contains() test)
        // would miss a click on the outer half. pinAtPoint() uses the real pin
        // positions with a generous radius, so it catches the dot on either
        // side of the edge.
        bool pinIsOut = false;
        int  hitPinId = pinAtPoint(canvasPos, pinIsOut, /*wantInput=*/-1);
        Node* pinNode = nullptr;
        const Pin* hitPin = nullptr;
        bool pinIsControlInput = false;
        if (hitPinId >= 0) {
            for (auto& nd : graph.nodes) {
                auto& pins = pinIsOut ? nd.pinsOut : nd.pinsIn;
                for (auto& p : pins)
                    if (p.id == hitPinId) { pinNode = &nd; hitPin = &p; break; }
                if (pinNode) break;
            }
            // Recognise a control-input pin by its "Mod: " / "Set: " NAME, not
            // only by an existing modPin binding. Some nodes (e.g. wavetables
            // loaded from older projects saved before modPin serialisation, or
            // whose bindings were otherwise lost) have orphan "Mod: Position X"
            // pins with no modPin entry. showPinMenu repairs the binding from
            // the pin name on demand; detecting by name here means the pin
            // still takes priority over its cable so the repair is reachable.
            if (pinNode && !pinIsOut && hitPin
                && (hitPin->name.rfind("Mod: ", 0) == 0
                 || hitPin->name.rfind("Set: ", 0) == 0))
                pinIsControlInput = true;
        }

        // A control-input pin (Mod/Set) takes priority over the cable plugged
        // into it. The cable terminates exactly at the pin, so the link
        // hit-test below would otherwise always win and there'd be no way to
        // right-click the pin itself to switch Set<->Mod or remove it. (Its
        // menu's "Remove Input Cable Pin" deletes the cable too, so nothing is
        // lost.) Regular pins fall through to the normal link-first order so a
        // cable is still right-clickable at its endpoint.
        if (pinIsControlInput && pinNode && hitPin) {
            showPinMenu(*pinNode, *hitPin, /*isInput=*/true);
            return;
        }

        // Check link hit for right-click
        int linkId = linkAtPoint(canvasPos);
        if (linkId >= 0) {
            selectedLinkId = linkId;
            selectedNodeId = -1;
            showLinkMenu(linkId);
            return;
        }
        // Non-control pin (or a control pin with no cable): the cursor is right
        // on the dot but no cable intercepted it. Open the pin menu (which
        // falls back to the node menu for non-control pins).
        if (pinNode && hitPin) {
            showPinMenu(*pinNode, *hitPin, !pinIsOut);
            return;
        }
        auto* node = nodeAtPoint(canvasPos);
        if (node) {
            // Check if right-click landed on a pin's ROW first - the whole
            // horizontal band of a pin (its circle AND its label text), not
            // just the small circle. This makes the Mod/Set switch (and other
            // pin actions) reachable by right-clicking the readable label,
            // which is what users aim at, instead of the tiny edge dot.
            //
            // Each row may carry an input pin (drawn on the left) and/or an
            // output pin (drawn on the right). When the row has BOTH, split at
            // the node's horizontal centre. When it has only ONE, the WHOLE row
            // hits that pin - never split. The split-by-centre rule alone was
            // the bug: a long input label like "Mod: Position 1" extends past
            // centreX, so clicking its right half looked for a (non-existent)
            // output pin on that row and fell through to the node menu.
            {
                auto bounds = getNodeBounds(*node);
                // Top region holds structural input pins (mod pins live on their
                // param rows below), so index against the structural list.
                auto structIns = structuralInputPins(*node);
                int topRows = std::max((int)structIns.size(),
                                       (int)node->pinsOut.size());
                float pinRowsTop   = bounds.getY() + HEADER_HEIGHT;
                float paramRowsTop = pinRowsTop + topRows * PIN_ROW_HEIGHT;
                if (canvasPos.y >= pinRowsTop && canvasPos.y < paramRowsTop) {
                    int row = (int)((canvasPos.y - pinRowsTop) / PIN_ROW_HEIGHT);
                    const Pin* inPin  = (row < (int)structIns.size())
                                            ? structIns[(size_t)row]  : nullptr;
                    const Pin* outPin = (row < (int)node->pinsOut.size())
                                            ? &node->pinsOut[(size_t)row] : nullptr;
                    const Pin* pin = nullptr;
                    bool isInput = true;
                    if (inPin && outPin) {
                        bool leftHalf = canvasPos.x < bounds.getCentreX();
                        pin = leftHalf ? inPin : outPin;
                        isInput = leftHalf;
                    } else if (inPin) {
                        pin = inPin;  isInput = true;   // input-only row
                    } else if (outPin) {
                        pin = outPin; isInput = false;  // output-only row
                    }
                    if (pin) { showPinMenu(*node, *pin, isInput); return; }
                }
            }
            // Check if right-click is on a param row - show arm/disarm menu
            if (!node->params.empty()) {
                auto bounds = getNodeBounds(*node);
                int topRows = numTopPinRows(*node);
                float paramRowsTop = bounds.getY() + HEADER_HEIGHT + topRows * PIN_ROW_HEIGHT;
                if (canvasPos.y >= paramRowsTop) {
                    int idx = (int)((canvasPos.y - paramRowsTop) / PIN_ROW_HEIGHT);
                    if (idx >= 0 && idx < (int)node->params.size()) {
                        // Right-click landed ON the in-place modulation connector
                        // (left edge of the row)? Show the pin menu (disconnect /
                        // switch Set-Mod / remove), same as a structural pin.
                        for (auto& mp : node->modPins) {
                            if (mp.paramIndex != idx) continue;
                            const Pin* mpin = nullptr;
                            for (auto& ip : node->pinsIn)
                                if (ip.id == mp.pinId) { mpin = &ip; break; }
                            if (!mpin) break;
                            auto pinPos = getPinPosition(*node, *mpin);
                            if (canvasPos.getDistanceFrom(pinPos) <= PIN_RADIUS * 2) {
                                showPinMenu(*node, *mpin, /*isInput=*/true);
                                return;
                            }
                            break;
                        }
                        auto& p = node->params[idx];
                        juce::PopupMenu pm;
                        pm.addItem(1, p.autoWriteArmed ? "Disarm for Auto-Write" : "Arm for Auto-Write");
                        pm.addItem(2, "Arm All on This Node");
                        pm.addItem(3, "Disarm All on This Node");
                        pm.addItem(4, "Reset to Default (double-click)");
                        // Signal control pin (#88): offer to add or remove a
                        // control input pin that drives this specific param.
                        // Two flavours (per-pin mode on Node::ModPin):
                        //   Set (Absolute) - the cable sets the value directly,
                        //       edge-to-edge; the knob locks while connected.
                        //   Mod (Modulate) - the cable swings the value around
                        //       the knob's setting; the knob stays editable.
                        // "Set" is the default (listed first) since a cable
                        // wired to a param usually reads as "drive this value".
                        bool hasModPin = false;
                        Node::ModPin::Mode curMode = Node::ModPin::Mode::Modulate;
                        for (auto& mp : node->modPins)
                            if (mp.paramIndex == idx) { hasModPin = true; curMode = mp.mode; break; }
                        pm.addSeparator();
                        if (hasModPin) {
                            pm.addItem(10, "Remove Input Cable Pin");
                            if (curMode == Node::ModPin::Mode::Absolute)
                                pm.addItem(11, "Switch to Modulation (Mod) - swing around the knob");
                            else
                                pm.addItem(11, "Switch to Absolute (Set) - cable sets the value");
                        } else {
                            pm.addItem(12, "Add Absolute Input (Set) - cable sets this value directly");
                            pm.addItem(13, "Add Modulation Input (Mod) - cable swings around the knob");
                        }
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
                            else if (r == 10) removeControlInput(nodeId, paramIdx);
                            else if (r == 11) switchControlInputMode(nodeId, paramIdx);
                            else if (r == 12) addControlInput(nodeId, paramIdx, /*absolute=*/true);
                            else if (r == 13) addControlInput(nodeId, paramIdx, /*absolute=*/false);
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
        // A signal-controlled param is locked - no dragging - but only that
        // specific param, not the rest of the node's params.
        if (!node->params.empty()) {
            auto bounds = getNodeBounds(*node);
            int topRows = numTopPinRows(*node);
            float paramRowsTop = bounds.getY() + HEADER_HEIGHT + topRows * PIN_ROW_HEIGHT;
            float paramRowsLeft  = bounds.getX() + 6;
            float paramRowsRight = bounds.getRight() - 6;
            if (canvasPos.x >= paramRowsLeft && canvasPos.x <= paramRowsRight
                && canvasPos.y >= paramRowsTop)
            {
                int idx = (int)((canvasPos.y - paramRowsTop) / PIN_ROW_HEIGHT);
                // Only ABSOLUTE-driven params are locked from manual drag. A
                // Mod-driven param stays draggable: the drag edits its base
                // (resting) value while the cable keeps modulating around it.
                if (idx >= 0 && idx < (int)node->params.size()
                    && !graph.paramHasAbsoluteInput(node->id, idx)) {
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
                    // Other discrete enum params (Pitch Detector's Algorithm
                    // and Mapping) also get a popup picker rather than a
                    // slider, for the same reason: discrete labelled states
                    // shouldn't be drag-scrubbed or parked between values.
                    if (p.name == "Algorithm" || p.name == "Mapping") {
                        selectedNodeId = node->id;
                        std::vector<const char*> labels = (p.name == "Algorithm")
                            ? std::vector<const char*>{"YIN (robust, default)", "Autocorrelation"}
                            : std::vector<const char*>{"Logarithmic (musical)", "Linear"};
                        int cur = juce::jlimit(0, (int)labels.size() - 1, (int)std::round(p.value));
                        juce::PopupMenu pm;
                        for (int i = 0; i < (int)labels.size(); ++i)
                            pm.addItem(i + 1, labels[i], true, i == cur);
                        int nodeId = node->id;
                        int paramIdx = idx;
                        std::string desc = "Change " + p.name;
                        pm.showMenuAsync({}, [this, nodeId, paramIdx, desc](int r) {
                            if (r == 0) return;
                            auto* nd = graph.findNode(nodeId);
                            if (!nd || paramIdx >= (int)nd->params.size()) return;
                            nd->params[paramIdx].value = (float)(r - 1);
                            graph.dirty = true;
                            graph.commitSnapshot(desc);
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
                    // Jump to the clicked position immediately. When a Mod
                    // cable is in place (p.modulated) the drag edits the base
                    // value the modulation swings around, not the live value
                    // (which applySignalModulations recomputes each block from
                    // baseValue). A non-modulated param writes value directly.
                    float frac = juce::jlimit(0.0f, 1.0f,
                                              (canvasPos.x - dragParamLeftX) / std::max(1.0f, dragParamWidth));
                    float newVal = p.minVal + frac * (p.maxVal - p.minVal);
                    if (p.modulated) p.baseValue = newVal;
                    else             p.value = newVal;
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
            float newVal = p.minVal + frac * (p.maxVal - p.minVal);
            // Mod-driven param: edit the base value the modulation swings
            // around (the live value is recomputed each block from baseValue).
            if (p.modulated) p.baseValue = newVal;
            else             p.value = newVal;
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
                // Force a graph rebuild so the new cable is routed immediately.
                // The processBlock link-COUNT delta would also catch this, but
                // relying on the count heuristic is fragile (it misses an
                // add-one/remove-one in the same frame) - request the rebuild
                // explicitly on the topology change, the same as every other
                // edit path.
                if (onNodeEdited) onNodeEdited();
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

juce::String NodeGraphComponent::getTooltip() {
    // Resolve the pin under the current mouse position directly (rather than
    // caching hover state) so the text is always accurate. pinAtPoint with
    // wantInput = -1 accepts either an input or an output pin.
    if (!isMouseOverOrDragging()) return {};
    auto canvasPos = screenToCanvas(getMouseXYRelative().toFloat());
    bool isOut = false;
    int pinId = pinAtPoint(canvasPos, isOut, -1);
    if (pinId < 0) return {};
    for (auto& node : graph.nodes) {
        for (auto& p : node.pinsIn)
            if (p.id == pinId) return juce::String(p.tooltip);
        for (auto& p : node.pinsOut)
            if (p.id == pinId) return juce::String(p.tooltip);
    }
    return {};
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
    // Standard DAW convention for "return to center." A signal-driven param is
    // locked (per-param), so it can't be reset by double-click either.
    if (!node->params.empty()) {
        auto bounds = getNodeBounds(*node);
        int topRows = numTopPinRows(*node);
        float paramRowsTop = bounds.getY() + HEADER_HEIGHT + topRows * PIN_ROW_HEIGHT;
        float paramRowsLeft  = bounds.getX() + 6;
        float paramRowsRight = bounds.getRight() - 6;
        if (canvasPos.x >= paramRowsLeft && canvasPos.x <= paramRowsRight
            && canvasPos.y >= paramRowsTop)
        {
            int idx = (int)((canvasPos.y - paramRowsTop) / PIN_ROW_HEIGHT);
            // Absolute-driven params are locked; Mod-driven stay resettable
            // (the reset targets the base value).
            if (idx >= 0 && idx < (int)node->params.size()
                && !graph.paramHasAbsoluteInput(node->id, idx)) {
                auto& p = node->params[idx];
                // Reset to midpoint of range (center for Pan, default for
                // others). For a Mod-driven param this resets the base value
                // the modulation swings around.
                float mid = (p.minVal + p.maxVal) * 0.5f;
                if (p.modulated) p.baseValue = mid;
                else             p.value = mid;
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
            // Non-modal: a user-input surface (XY pad) must stay usable
            // alongside the main window, the transport, and other input-node
            // editors while the song plays. The editor is node-id-safe (looks
            // up via findNode each access), so it survives its node being
            // edited or deleted out from under it.
            if (auto* dlg = SoundShop::launchNonModalToolDialog(opts))
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
            // Non-modal: a Control Bank is a live macro-fader surface meant to
            // be played while the song runs - and you may want several open at
            // once. Modal would block the transport and every other node. The
            // editor is node-id-safe, so deleting its node mid-session is safe.
            SoundShop::launchNonModalToolDialog(opts);
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
            opts.dialogTitle = "Script: " + juce::String(node->name);
            opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar = false;
            opts.resizable = true;
            opts.componentToCentreAround = this;
            // Non-modal: a Script node has a manual-trigger button and drives
            // params live, so it belongs in the same play-while-open family as
            // the XY Pad and Control Bank above.
            SoundShop::launchNonModalToolDialog(opts);
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
    // Focused single-oscillator instruments, one per wavetable frame type (all
    // the per-frame controls, none of the multi-frame wavetable machinery - no
    // grid, no Position morph, no library). These used to live under a
    // "Single-Frame Instruments" submenu, but the name only made sense in
    // contrast to wavetable frames (a concept the UI no longer surfaces), and
    // since they're standalone instruments now they belong directly in the
    // Instruments list - placed right after Wavetable so the wavetable-family
    // synths stay adjacent.
    //
    // The old "Sample (single cycle)" instrument (id 254) was removed: a single
    // cycle extracted from captured audio is now available as the Granular
    // node's "Single cycle" freeze mode (autocorrelation period detect + clean
    // crossfaded loop), which is strictly more robust than the SampleFrame's
    // zero-crossing collapse. Id 254 is intentionally left as a gap (the
    // SampleFrame *type* itself stays for project/back-compat decode).
    instMenu.addItem(250, "Layered Waveform");
    instMenu.addItem(251, "Frequency Domain");
    instMenu.addItem(252, "Wavelet Space");
    instMenu.addItem(253, "Inharmonic");
    instMenu.addItem(255, "Granular");
    juce::PopupMenu terrainMenu;
    terrainMenu.addItem(120, "2D Terrain (sin*cos)");
    terrainMenu.addItem(122, "2D Terrain (custom expression...)");
    terrainMenu.addItem(125, "N-D Terrain (custom expression, 1-8D)...");
    terrainMenu.addItem(124, "1D Terrain from Audio File...");
    terrainMenu.addItem(123, "2D Terrain from Image...");
    terrainMenu.addItem(126, "3D Terrain from Video...");
    terrainMenu.addItem(127, "Terrain from Program (Generate)...");
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
    fxMenu.addItem(241, "Curve EQ (draw response)");
    // Waveshaper submenu: one entry per amplitude-domain morph method. Built
    // from the shared registry (warp.h) so labels/tooltips stay in sync with
    // the synth's morph picker. IDs 260 + index (260..269); see the matching
    // creation handler. These apply warpAmpValue() to the audio stream - the
    // same transfer the synth's amplitude morphs use.
    {
        juce::PopupMenu wsMenu;
        const auto& wm = waveshaperMethods();
        for (int i = 0; i < (int)wm.size(); ++i)
            wsMenu.addItem(260 + i, warpMethodName(wm[i]));
        fxMenu.addSubMenu("Waveshaper (amplitude morph)", wsMenu);
    }
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
    fxMenu.addItem(240, "Pitch Detector (YIN / autocorrelation)");
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
    // Single unified "Script" entry. One scriptable node now covers BOTH signal
    // generation (LFO / Envelope, continuous o1..oP outputs) AND algorithmic
    // MIDI (note()/cc()/bend() emit on MIDI Out pins). The old separate
    // "Signal Shape" and "MIDI Script" items collapsed into this - choose how
    // many signal outputs vs MIDI outputs the node has inside the editor. (A
    // node with 0 MIDI outputs is the classic Signal Shape; 0 continuous
    // outputs + a MIDI-emitting program is the classic MIDI Script.)
    sigMenu.addItem(130, "Script (signal + MIDI)");
    sigMenu.addItem(143, "MIDI Breakout (MIDI -> signals)");
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
        } else if (result >= 250 && result <= 255) {
            // Standalone single-frame instrument nodes. One per wavetable frame
            // type, but presented as a distinct instrument (not a 1-frame
            // wavetable): the script is a __framesynth__ wrapper, and the editor
            // opens in focused mode (no grid / library / Position morph). They
            // reuse the entire wavetable render + sub-editor path under the hood.
            // Explicit id -> type map (NOT result-250 indexing) so the removed
            // "Sample" instrument leaves a harmless gap at id 254 without
            // shifting Granular (255) out of bounds.
            struct FrameInst { int id; const char* typeId; const char* name; };
            const FrameInst kFrameInst[] = {
                { 250, "layered",    "Layered Waveform" },
                { 251, "spectral",   "Frequency Domain" },
                { 252, "wavelet",    "Wavelet Space" },
                { 253, "inharmonic", "Inharmonic" },
                { 255, "granular",   "Granular" },
            };
            const FrameInst* fip = nullptr;
            for (const auto& e : kFrameInst) if (e.id == result) { fip = &e; break; }
            if (!fip) return;   // gap id (254, ex-Sample) or unknown
            const FrameInst& fi = *fip;
            std::string script = SoundShop::defaultFrameSynthScriptForType(fi.typeId);
            if (script.empty()) return;  // unknown type id (shouldn't happen)

            auto& n = graph.addNode(fi.name, NodeType::Instrument,
                {Pin{0, "MIDI", PinKind::Midi, true}},
                {Pin{0, "Audio", PinKind::Audio, false}}, {p.x, p.y});
            n.script = std::move(script);

            // Same generic synth voice character + param set as the Wavetable
            // node, MINUS the Position param: a single-frame instrument has no
            // morph axis, so a Position knob would do nothing (and the
            // wavetable-only controls are exactly what these nodes drop).
            n.ahdsrEnvelope.attackMs  = 10.0f;
            n.ahdsrEnvelope.decayMs   = 100.0f;
            n.ahdsrEnvelope.sustain   = 0.7f;
            n.ahdsrEnvelope.releaseMs = 300.0f;
            n.params.push_back({"Volume",  1.0f,  0.0f, 1.0f});
            n.params.push_back({"Pan",     0.0f, -1.0f, 1.0f});
            n.params.push_back({"Vibrato", 1.0f,  0.0f, 1.0f});

            // Commit the new node before opening its editor so undo/redo and
            // save/load see a consistent graph.
            graph.commitSnapshot("Add instrument");

            auto nodeId = n.id;
            auto* editor = new LayeredWaveEditorComponent(graph, nodeId, [this]() {
                if (onNodeEdited) onNodeEdited();
                repaint();
            });
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned(editor);
            opts.dialogTitle = "Instrument: " + juce::String(n.name);
            opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar = false;
            opts.resizable = true;
            opts.componentToCentreAround = this;
            SoundShop::launchNonModalToolDialog(opts);
            if (onNodeEdited) onNodeEdited();
            repaint();
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
                // Non-modal (see the double-click reopen path above): a live
                // input surface must coexist with the rest of the UI.
                if (auto* dlg = SoundShop::launchNonModalToolDialog(opts))
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
            // Non-modal (see the reopen path): live macro faders.
            SoundShop::launchNonModalToolDialog(opts);
            return;
        } else if (result == 143) {
            // MIDI Breakout: taps a MIDI stream and re-emits its expression
            // controllers as block-rate control signals so they can be wired
            // anywhere a control cable is accepted. One MIDI input, four Signal
            // outputs in the order MidiBreakoutProcessor writes them (Velocity,
            // Pressure, Mod Wheel, Pitch Bend). No editor - falls through to the
            // common node-creation finalization below.
            auto& n = graph.addNode("MIDI Breakout", NodeType::MidiBreakout,
                { Pin{0, "MIDI In",    PinKind::Midi,   true } },
                { Pin{0, "Velocity",   PinKind::Signal, false, 1},
                  Pin{0, "Pressure",   PinKind::Signal, false, 1},
                  Pin{0, "Mod Wheel",  PinKind::Signal, false, 1},
                  Pin{0, "Pitch Bend", PinKind::Signal, false, 1} },
                {p.x, p.y});
            n.script = "__midibreakout__";
            // Per-output hover tooltips. A synth that receives the same MIDI
            // already applies these controllers itself, so feeding one back into
            // that synth is redundant. Two distinct redundancy modes:
            //   - Pressure: the synth's Pressure INPUT pin OVERWRITES (replaces)
            //     the keyboard's own pressure, so looping it back is harmless but
            //     pointless (and downgrades it to a once-per-block value). Not a
            //     double-application.
            //   - Mod wheel / pitch bend: the synth has no input pin for these -
            //     it bends pitch and vibratos straight from MIDI. Wiring one into
            //     a modulation pin that drives the SAME thing stacks on top of
            //     the synth's own handling, so it genuinely applies twice.
            // Either way the useful move is to route these somewhere new.
            const std::string elsewhere =
                " Route it somewhere new instead - a filter cutoff, a wavetable "
                "position, a different synth, an effect knob.";
            const std::string dblMod =
                " A synth that gets the same MIDI already bends pitch / vibratos "
                "from it directly, so wiring this into a modulation pin driving "
                "the same thing applies it twice." + elsewhere;
            if (n.pinsOut.size() >= 4) {
                n.pinsOut[0].tooltip =
                    "Last note-on velocity, 0..1 (how hard the key was struck), "
                    "held until the next note.";
                n.pinsOut[1].tooltip =
                    "Key pressure / aftertouch, 0..1 (channel pressure, or the "
                    "most recent polyphonic key-pressure). A synth's Pressure "
                    "input pin OVERWRITES the keyboard's own pressure with this, "
                    "so feeding a synth its own pressure back is just redundant." +
                    elsewhere;
                n.pinsOut[2].tooltip =
                    "Mod wheel (MIDI CC 1), 0..1." + dblMod;
                n.pinsOut[3].tooltip =
                    "Pitch-bend wheel, 0..1 with 0.5 = centre (full down = 0, "
                    "full up = 1). In a param's Modulate mode 0.5 = no change; "
                    "use Absolute/Set mode to map it edge-to-edge." + dblMod;
            }
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
        } else if (result == 130) {
            // Unified Script node. One scriptable node covers signal generation
            // (LFO / Envelope via the drawn shape + per-sample expression) AND
            // algorithmic MIDI (note()/cc()/bend() emit). The editor opens
            // immediately so the user can pick I/O counts and write the program.
            //
            // Pins (the default I/O matches SignalShapeDoc::defaultLFO):
            //   In:  "MIDI In" - drives the gate/freq/note/vel VARIABLES
            //        (NOT a trigger; trigger is the trigger expression). The
            //        editor's "MIDI inputs" count sets how many MIDI In pins
            //        (0 = none, 1 = "MIDI In", >1 = "MIDI In 1..N", each event
            //        tagged with its 1-based input index). Signal inputs s1..sN
            //        appear as the user dials up signalInputCount.
            //   Out: "o1" - the single default continuous output (Signal kind).
            //        The editor adds o2..oP, MIDI Out pins, or flips the
            //        continuous pins to Param kind. syncPins() keeps node pins
            //        in sync with the doc from then on.
            auto& n = graph.addNode("Script", NodeType::SignalShape,
                {Pin{0, "MIDI In", PinKind::Midi, true}},
                {Pin{0, "o1", PinKind::Signal, false}},
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
            opts.dialogTitle = "Script: " + juce::String(n.name);
            opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar = false;
            opts.resizable = true;
            opts.componentToCentreAround = this;
            // Non-modal (live input surface - see the double-click path).
            SoundShop::launchNonModalToolDialog(opts);
            return;
        } else if (result >= 120 && result <= 127) {
            // Terrain Synth. The terrain engine and visualizer both support
            // N-dimensional terrains (1..8 axes); the visualizer's + Dim /
            // - Dim buttons add/remove axes at runtime, and
            // makeTerrainNode() takes a numDims arg so callers can also
            // create higher-D terrains directly. Image (2D) and audio (1D)
            // sources have fixed dimensionality - the only path that
            // varies N at create time is the formula path (result 125).
            if (result == 120) {
                makeTerrainNode("Terrain (sin*cos)", "sin(x) * cos(y)", p);
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
            } else if (result == 126) {
                // Terrain from video: a 3D terrain (frames x height x width).
                // The node starts empty; the import dialog decodes the grid and
                // bakes it into the node script (see VideoImportDialogComponent).
                auto nodeId = makeTerrainNode("Terrain (video)", "", p, 3).id;
                auto* editor = new VideoImportDialogComponent(
                    graph, nodeId,
                    [this] {
                        if (onNodeEdited) onNodeEdited();
                        graph.commitSnapshot("Import video terrain");
                        repaint();
                    });
                editor->setSize(760, 660);
                juce::DialogWindow::LaunchOptions opts;
                opts.content.setOwned(editor);
                opts.dialogTitle = "Import Video";
                opts.dialogBackgroundColour = juce::Colour(0xff2b2b30);
                opts.escapeKeyTriggersCloseButton = true;
                opts.useNativeTitleBar = false;
                opts.resizable = true;
                opts.componentToCentreAround = this;
                SoundShop::launchNonModalToolDialog(opts);
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
            } else if (result == 127) {
                // Terrain from Program (Generate): the user writes a script that
                // returns one value in [0,1] per cell; the terrain is rebuilt
                // from it on every load (see makeGenerateTerrainScript). Create
                // mode - the dialog's chosen rank decides the new node's shape.
                auto canvasPos = p;
                GenerateTerrainParams seed;
                seed.lang = (int) ScriptLang::Lua;   // dialog falls back if absent
                seed.dims = { 512, 512 };
                auto* editor = new GenerateDialogComponent(
                    seed, /*lockRank=*/false,
                    [this, canvasPos](const GenerateTerrainParams& gp) {
                        int nd = (int) gp.dims.size();
                        std::string nm = juce::String(nd).toStdString() + "D Terrain (generated)";
                        auto& n = makeTerrainNode(nm, makeGenerateTerrainScript(gp, &graph.contentStore),
                                                  canvasPos, nd);
                        (void) n;
                        if (onNodeEdited) onNodeEdited();
                        graph.commitSnapshot("Generate terrain");
                        repaint();
                    });
                juce::DialogWindow::LaunchOptions opts;
                opts.content.setOwned(editor);
                opts.dialogTitle = "Generate Terrain";
                opts.dialogBackgroundColour = juce::Colour(0xff2b2b30);
                opts.escapeKeyTriggersCloseButton = true;
                opts.useNativeTitleBar = false;
                opts.resizable = true;
                opts.componentToCentreAround = this;
                SoundShop::launchNonModalToolDialog(opts);
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
            }
            // Per-operator A/D/S/R is now a full AHDSR envelope per operator
            // (hold stage, per-segment curves, tension, velocity sensitivity),
            // edited via the multi-tab operator-envelope dialog. Seed the 4
            // default envelopes here (replaces the old "Op{i} A/D/S/R" params).
            ensureFmOpEnvelopes(n);
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
        } else if (result >= 260 && result <= 269) {
            // Waveshaper (amplitude morph): one node per Bucket-A warp method.
            // The DSP is WaveshaperProcessor, which reads its method from the
            // "__waveshaper:<token>__" script. A single amount param, labelled
            // with the method's named morph parameter ("Drive"/"Fold"/"Crush"/
            // ...) so it reads naturally and the on-demand modulation pin (#88)
            // inherits the right name; WaveshaperProcessor derives the same
            // label from the method to read it back.
            const auto& wm = waveshaperMethods();
            const int idx = result - 260;
            if (idx >= 0 && idx < (int)wm.size()) {
                const WarpMethod method = wm[idx];
                const juce::String label =
                    juce::String("Waveshaper: ") + warpMethodName(method);
                auto& n = graph.addNode(label.toStdString(), NodeType::Effect,
                    {Pin{0, "Audio In", PinKind::Audio, true}},
                    {Pin{0, "Audio Out", PinKind::Audio, false}}, {p.x, p.y});
                n.script = waveshaperScriptFor(method);
                const char* pl = warpParamLabel(method);
                n.params.push_back({ (pl && *pl) ? pl : "Amount",
                                     0.5f, 0.0f, 1.0f });
            }
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
                case 241: {
                    // Curve EQ: draw-the-response equaliser. Default curve is
                    // flat (unity gain at every frequency). The magnitude curve
                    // lives in the script; FFT Size + Mix are node params.
                    SpectralCurve c;
                    c.expression = "1";
                    auto& n = makeEffect("Curve EQ", "__curveeq__:", {
                        {"FFT Size", 11.0f, 8.0f, 12.0f}, // 2^11 = 2048-bin resolution
                        {"Mix",       1.0f, 0.0f,  1.0f},
                    });
                    n.script = CurveEq::encode(c, -1);
                    break;
                }
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
                case 240: {
                    // Pitch Detector: Audio In -> Audio Out (passthrough) +
                    // Pitch Out (Signal). Insert inline like the Spectrum Tap:
                    // the audio continues downstream unchanged while the Signal
                    // pin emits the detected pitch.
                    auto& pdn = graph.addNode("Pitch Detector", NodeType::Effect,
                        {Pin{0, "Audio In", PinKind::Audio, true}},
                        {Pin{0, "Audio Out", PinKind::Audio, false}}, {p.x, p.y});
                    pdn.pinsOut.push_back({graph.allocId(), "Pitch Out", PinKind::Signal, false});
                    pdn.script = "__pitchdetector__";
                    pdn.params.push_back({"Algorithm",     0.0f,     0.0f,     1.0f});  // 0=YIN,1=Autocorr
                    pdn.params.push_back({"Hop",           0.0f,     0.0f, 16384.0f});  // 0=per block; update rate
                    pdn.params.push_back({"Min Hz",       50.0f,    20.0f, 20000.0f});  // also sets window/latency
                    pdn.params.push_back({"Max Hz",     2000.0f,    20.0f, 20000.0f});
                    pdn.params.push_back({"Mapping",       0.0f,     0.0f,     1.0f});  // 0=Log,1=Linear
                    pdn.params.push_back({"Detected Hz",   0.0f,     0.0f, 20000.0f});
                    pdn.pinsIn[0].tooltip =
                        "Audio to analyse. The node measures the fundamental "
                        "pitch of this signal and emits it on Pitch Out, updating "
                        "every block (or every Hop samples). Min Hz sets the lowest "
                        "note it can detect and, with it, the analysis latency - "
                        "lower Min Hz needs a longer analysis window.";
                    pdn.pinsOut[0].tooltip =
                        "Detected pitch as a 0..1 signal across [Min Hz, Max Hz]. "
                        "0 = Min Hz, 1 = Max Hz. Mapping = Logarithmic spaces the "
                        "range musically (an octave is the same distance "
                        "everywhere); Linear spaces it by raw Hz. Wire this into "
                        "any param's Modulate/Absolute input to pitch-follow.";
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

// ----------------------------------------------------------------------------
// Control-input (#88) operations. Shared by the param-row right-click menu and
// the per-pin right-click menu so both surfaces behave identically. Each looks
// the node up by id and addresses the binding by stable paramIndex, so nothing
// dangles across the async menu callback even if the pin/param vectors moved.
// ----------------------------------------------------------------------------
void NodeGraphComponent::addControlInput(int nodeId, int paramIdx, bool absolute) {
    // Pure data-model mutation lives in the shared graph helper (#88) so the
    // node right-click menu and the warp/morph editor's per-param "modulate"
    // checkbox behave identically. This surface owns the commit + rebuild.
    if (addParamModPin(graph, nodeId, paramIdx, absolute) < 0) return;
    graph.commitSnapshot(absolute ? "Add absolute input" : "Add modulation input");
    // Topology changed: the node gained an input pin (and needs a wider input
    // bus to carry the new control channel). Force a rebuild now - the
    // node/link COUNT is unchanged, so the processBlock count-delta check would
    // otherwise miss this edit and keep the synth on its old (too-narrow) bus,
    // silently dropping any signal later cabled to this pin.
    if (onNodeEdited) onNodeEdited();
    repaint();
}

void NodeGraphComponent::removeControlInput(int nodeId, int paramIdx) {
    // Shared graph helper does the data-model removal (#88); this surface owns
    // the commit + rebuild. No-op (no commit) if there was no pin to remove.
    if (!removeParamModPin(graph, nodeId, paramIdx)) return;
    graph.commitSnapshot("Remove control input");
    if (onNodeEdited) onNodeEdited();
    repaint();
}

void NodeGraphComponent::switchControlInputMode(int nodeId, int paramIdx) {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;
    for (auto& mp : nd->modPins) {
        if (mp.paramIndex != paramIdx) continue;
        mp.mode = (mp.mode == Node::ModPin::Mode::Absolute)
                    ? Node::ModPin::Mode::Modulate
                    : Node::ModPin::Mode::Absolute;
        // Relabel the pin's Set:/Mod: prefix to match the new mode.
        for (auto& pin : nd->pinsIn)
            if (pin.id == mp.pinId
                && (pin.name.rfind("Mod: ", 0) == 0
                 || pin.name.rfind("Set: ", 0) == 0))
                pin.name = (mp.mode == Node::ModPin::Mode::Absolute
                              ? "Set: " : "Mod: ") + pin.name.substr(5);
        break;
    }
    // Reset to the resting value so the param doesn't keep the last driven
    // reading while the new mode takes over on the next audio block.
    if (paramIdx >= 0 && paramIdx < (int)nd->params.size()) {
        auto& p = nd->params[paramIdx];
        if (p.modulated) { p.value = p.baseValue; p.modulated = false; }
    }
    graph.dirty = true;
    graph.commitSnapshot("Switch control input mode");
    if (onNodeEdited) onNodeEdited();
    repaint();
}

void NodeGraphComponent::showPinMenu(Node& node, const Pin& pin, bool isInput) {
    // Is this an input pin bound to a control-input ModPin? If so, offer the
    // Set/Mod switch and removal. (Only input pins carry ModPins.)
    int paramIdx = -1;
    Node::ModPin::Mode curMode = Node::ModPin::Mode::Modulate;
    if (isInput) {
        for (auto& mp : node.modPins)
            if (mp.pinId == pin.id) { paramIdx = mp.paramIndex; curMode = mp.mode; break; }
    }

    // Repair path: the pin is named like a control input ("Mod: <param>" /
    // "Set: <param>") but has NO modPin binding. This happens with wavetable
    // "Mod: Position X" pins from projects saved before modPin serialisation
    // existed, or whose bindings were otherwise lost - the pin shows on the
    // node face but the Set/Mod menu had nothing to act on. Rebuild the
    // binding from the pin name so the feature self-heals on first right-click.
    if (paramIdx < 0 && isInput
        && (pin.name.rfind("Mod: ", 0) == 0 || pin.name.rfind("Set: ", 0) == 0)) {
        bool absolute = (pin.name.rfind("Set: ", 0) == 0);
        std::string suffix = pin.name.substr(5);   // text after "Mod: "/"Set: "

        // Resolve which param this pin drives. First try an exact param-name
        // match ("Mod: Volume" -> param "Volume"). Then handle the wavetable
        // Position quirk: the PIN is labelled by axis letter ("Position X/Y/Z/W")
        // while the PARAMS are numbered ("Position 1".."Position N"), so map the
        // axis letter/number to its ordinal among the Position-named params.
        int resolved = -1;
        for (int i = 0; i < (int)node.params.size(); ++i)
            if (node.params[(size_t)i].name == suffix) { resolved = i; break; }
        if (resolved < 0 && suffix.rfind("Position", 0) == 0) {
            int axis = -1;
            // suffix is "Position X" / "Position 1" etc - read the token after
            // "Position ".
            std::string tok = (suffix.size() > 9) ? suffix.substr(9) : std::string();
            if (tok.size() == 1 && std::isalpha((unsigned char)tok[0])) {
                switch (std::toupper((unsigned char)tok[0])) {
                    case 'X': axis = 0; break; case 'Y': axis = 1; break;
                    case 'Z': axis = 2; break; case 'W': axis = 3; break;
                }
            } else if (!tok.empty() && std::isdigit((unsigned char)tok[0])) {
                axis = std::atoi(tok.c_str()) - 1;   // "Position 1" -> axis 0
            }
            if (axis >= 0) {
                int count = 0;
                for (int i = 0; i < (int)node.params.size(); ++i) {
                    if (node.params[(size_t)i].name.rfind("Position", 0) != 0) continue;
                    if (count == axis) { resolved = i; break; }
                    ++count;
                }
            }
        }

        if (resolved >= 0) {
            Node::ModPin mp;
            mp.paramIndex = resolved;
            mp.pinId      = pin.id;
            mp.depth      = 1.0f;
            mp.mode       = absolute ? Node::ModPin::Mode::Absolute
                                     : Node::ModPin::Mode::Modulate;
            node.modPins.push_back(mp);
            graph.dirty = true;
            graph.commitSnapshot("Repair control input binding");
            // An orphaned binding also meant the cable into this pin wasn't
            // modulating anything (applySignalModulations iterates modPins).
            // Rebuild so the restored binding takes effect in the audio graph.
            if (onNodeEdited) onNodeEdited();
            paramIdx = resolved;
            curMode  = mp.mode;
        }
    }

    if (paramIdx < 0) {
        // Not a control-input pin (or its binding couldn't be resolved) - no
        // pin-specific actions. Fall back to the node menu so right-clicking a
        // plain pin/label still does something.
        showNodeMenu(node);
        return;
    }

    juce::PopupMenu pm;
    juce::String paramName = (paramIdx < (int)node.params.size())
                                 ? juce::String(node.params[(size_t)paramIdx].name)
                                 : juce::String();
    pm.addSectionHeader(
        (curMode == Node::ModPin::Mode::Absolute ? "Set: " : "Mod: ") + paramName);
    if (curMode == Node::ModPin::Mode::Absolute)
        pm.addItem(1, "Switch to Modulation (Mod) - swing around the knob");
    else
        pm.addItem(1, "Switch to Absolute (Set) - cable sets the value");
    pm.addSeparator();
    pm.addItem(2, "Remove Input Cable Pin");

    int nodeId = node.id;
    int pIdx   = paramIdx;
    pm.showMenuAsync({}, [this, nodeId, pIdx](int r) {
        if (r == 1)      switchControlInputMode(nodeId, pIdx);
        else if (r == 2) removeControlInput(nodeId, pIdx);
    });
}

int NodeGraphComponent::cumulativeLatencyTo(
        int nodeId,
        const std::unordered_map<int, int>& ownLatency,
        std::unordered_map<int, int>& memo,
        std::unordered_set<int>& visiting) const {
    if (auto m = memo.find(nodeId); m != memo.end()) return m->second;
    if (visiting.count(nodeId)) return 0; // feedback cycle - break it
    visiting.insert(nodeId);

    int self = 0;
    if (auto it = ownLatency.find(nodeId); it != ownLatency.end()) self = it->second;

    int maxUpstream = 0;
    if (Node* n = graph.findNode(nodeId)) {
        // A node feeds `n` when one of its OUTPUT pins drives a link whose
        // END pin is one of n's INPUT pins. Walk every link once.
        for (auto& link : graph.links) {
            bool feedsN = false;
            for (auto& pin : n->pinsIn)
                if (pin.id == link.endPin) { feedsN = true; break; }
            if (!feedsN) continue;
            int srcId = -1;
            for (auto& other : graph.nodes) {
                for (auto& op : other.pinsOut)
                    if (op.id == link.startPin) { srcId = other.id; break; }
                if (srcId >= 0) break;
            }
            if (srcId >= 0)
                maxUpstream = std::max(maxUpstream,
                    cumulativeLatencyTo(srcId, ownLatency, memo, visiting));
        }
    }

    visiting.erase(nodeId);
    int total = maxUpstream + self;
    memo[nodeId] = total;
    return total;
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
        // "MPE mode" for a hosted plugin: a user-asserted flag telling SEANCE
        // this plugin is itself running in MPE mode. MPE capability can't be
        // detected reliably, so the user states it. When on, SEANCE emits the
        // MPE Configuration Message (zone handshake) and the cable-level tuning
        // adapter spreads each note onto its own member channel (2..16) with a
        // full per-note tuning bend - this is what lets unequal temperaments and
        // per-note expression reach the plugin. It also spreads a plain single-
        // channel source automatically, so an MPE source is NOT required.
        //
        // The caveat is inline because JUCE PopupMenu items can't show hover
        // tooltips and it matters at the decision point: only turn this on if
        // the plugin really is in MPE mode. Enabling it for a plugin that is
        // NOT in MPE mode scatters one voice's notes across channels it treats
        // as independent, which typically makes it misbehave or go silent.
        if (node.plugin) {
            bool hasMidiIn = false;
            for (auto& p : node.pinsIn)
                if (p.kind == PinKind::Midi) { hasMidiIn = true; break; }
            if (hasMidiIn)
                menu.addItem(181,
                             node.mpeEnabled ? "Disable MPE mode"
                                             : "Enable MPE mode (only if plugin is in MPE mode)",
                             true, node.mpeEnabled);
        }
    }
    // Envelope editor on synths whose amplitude envelope IS the shared node
    // AHDSR. These read node.ahdsrEnvelope directly through the shared
    // AHDSREnvelopeRuntime: the Terrain/wavetable engine plus the Additive,
    // PD, Spectral Grain, and Particle Cloud synths. (Particle's per-grain
    // attack/release shapes each grain; the node AHDSR is its separate
    // note-level VCA - a true addition, not a double of the grain envelope.)
    // Synths that supply their own integral amplitude envelope - FM
    // (per-operator, edited via its own operator-envelope menu), Drum
    // (one-shot per-sound decay, no sustain stage), and the sample/region-file
    // players (SoundFont, SFZ, Sfizz, MultiSampler) - are NOT offered this
    // editor, because a generic AHDSR can't subsume what they already have.
    // Raw plugin-hosting Instruments (pluginIndex >= 0) have their envelope
    // inside the plugin.
    bool isTonalSynth = false;
    if (node.type == NodeType::TerrainSynth) {
        isTonalSynth = true;
    } else if (node.type == NodeType::Instrument && node.pluginIndex < 0) {
        auto isScript = [&](const char* tag) {
            return node.script.rfind(tag, 0) == 0;
        };
        bool ownEnvelope =
            isScript("__fmsynth__") ||
            isScript("__drumsynth__") || isScript("__sf2__") ||
            isScript("__sfz__") || isScript("__sfizz__") ||
            isScript(MultiSamplerDoc::kPrefix);
        isTonalSynth = !ownEnvelope;
    }
    if (isTonalSynth)
        menu.addItem(180, "Envelope (AHDSR)...");

    // FM synth: 4 per-operator AHDSR envelopes (one per operator), edited in
    // a single tabbed dialog. FM is excluded from the generic single-envelope
    // item above because its amplitude shape is per-operator, not node-global.
    if (node.type == NodeType::Instrument && node.pluginIndex < 0 &&
        node.script.rfind("__fmsynth__", 0) == 0)
        menu.addItem(182, "Operator Envelopes (AHDSR)...");

    // Video terrains can be re-cropped / re-scaled by re-opening the import
    // dialog, which re-seeds its controls from the node's baked __video__ script.
    if (node.type == NodeType::TerrainSynth && node.script.rfind("__video__:", 0) == 0)
        menu.addItem(193, "Edit Video...");

    // Generated terrains can be re-opened in the Generate dialog to tweak the
    // program or dimension sizes (rank is locked - see GenerateDialogComponent).
    if (node.type == NodeType::TerrainSynth && node.script.rfind("__generate__:", 0) == 0) {
        menu.addItem(194, "Edit Source...");
        // Export the baked grid in external-tool formats. .npz (NumPy) works for
        // any rank and preserves full float precision; .wav is offered only for
        // 1D grids (a mono waveform) and .png only for 2D grids (an 8-bit
        // grayscale image). Rank is read from the baked script's dims field so
        // the irrelevant formats are hidden rather than greyed out.
        juce::PopupMenu exportMenu;
        exportMenu.addItem(195, "NumPy .npz (any rank, full precision)...");
        int genRank = generateScriptRank(node.script);
        if (genRank == 1)
            exportMenu.addItem(196, "WAV (1D waveform)...");
        if (genRank == 2)
            exportMenu.addItem(197, "PNG (2D grayscale image)...");
        menu.addSubMenu("Export grid as", exportMenu);
    }

    // Parametric EQ: add/remove bands. The band count is variable - each band
    // is one cascaded biquad. "Add" pushes the four B<n> Type/Freq/Gain/Q
    // params; "Remove" pops the highest band's four params. The generic param
    // panel then shows exactly the active bands (no orphan sliders).
    if (node.type == NodeType::Effect && node.script.rfind("__eq__", 0) == 0) {
        int nb = ParametricEQProcessor::countBands(node);
        menu.addItem(200, "Add EQ Band", nb < ParametricEQProcessor::kMaxBands);
        menu.addItem(201, "Remove Last EQ Band", nb > 1);
    }

    // The unified Script node gets an "Edit Script" entry (hidden for the
    // sibling XY Pad / Control Bank nodes, which share NodeType::SignalShape
    // but have their own dedicated editors opened via double-click).
    if (node.type == NodeType::SignalShape && node.script != "__xypad__"
        && node.script.rfind("__controlbank__", 0) != 0)
        menu.addItem(190, "Edit Script...");
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

    // Latency readout (disabled info items). Most built-in nodes report 0; a
    // hosted plugin's lookahead/linear-phase processing reports its delay, which
    // the audio graph compensates automatically. Two lines, ALWAYS both shown so
    // the combined figure is never ambiguous by its absence:
    //   "Latency (this node)"      - the node's own added delay.
    //   "Latency (combined here)"  - the largest delay accumulated along any path
    //                                of nodes feeding it, plus this node's own;
    //                                i.e. how far behind real time the signal is
    //                                by the time it leaves this node. This is the
    //                                same max-over-input-paths figure the graph's
    //                                delay compensation aligns every branch to.
    //                                Equals the node's own value when nothing
    //                                upstream adds delay (e.g. a source, or an
    //                                all-zero-latency chain) - still shown, so the
    //                                user can see it's been accounted for.
    if (getNodeLatencies) {
        auto lat = getNodeLatencies();
        int ownSamples = 0;
        if (auto it = lat.find(node.id); it != lat.end()) ownSamples = it->second;
        std::unordered_map<int, int> memo;
        std::unordered_set<int> visiting;
        int totalSamples = cumulativeLatencyTo(node.id, lat, memo, visiting);
        double sr = (getAudioFormat ? getAudioFormat().first : 0.0);
        auto fmt = [sr](int s) -> juce::String {
            juce::String t = juce::String(s) + (s == 1 ? " sample" : " samples");
            if (sr > 0.0) t += " (" + juce::String(1000.0 * s / sr, 1) + " ms)";
            return t;
        };
        menu.addSeparator();
        menu.addItem(-1, "Latency (this node): " + fmt(ownSamples), false);
        menu.addItem(-1, "Latency (combined here): " + fmt(totalSamples), false);
    }

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
        } else if (result == 200) {
            // Add an EQ band: a new peaking band at 1 kHz, flat (0 dB).
            int nb = ParametricEQProcessor::countBands(*node);
            if (nb < ParametricEQProcessor::kMaxBands) {
                std::string pfx = "B" + std::to_string(nb + 1) + " ";
                node->params.push_back({pfx + "Type", 0.0f, 0.0f, 4.0f});
                node->params.push_back({pfx + "Freq", 1000.0f, 20.0f, 20000.0f});
                node->params.push_back({pfx + "Gain", 0.0f, -24.0f, 24.0f});
                node->params.push_back({pfx + "Q",    0.707f, 0.1f, 10.0f});
                graph.commitSnapshot("Add EQ band");
                if (onNodeEdited) onNodeEdited();
            }
        } else if (result == 201) {
            // Remove the highest-numbered EQ band (its four params).
            int nb = ParametricEQProcessor::countBands(*node);
            if (nb > 1) {
                std::string pfx = "B" + std::to_string(nb) + " ";
                node->params.erase(
                    std::remove_if(node->params.begin(), node->params.end(),
                        [&](const Param& p) {
                            return juce::String(p.name).startsWith(pfx);
                        }),
                    node->params.end());
                graph.commitSnapshot("Remove EQ band");
                if (onNodeEdited) onNodeEdited();
            }
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
            // Open the shared AHDSR envelope editor on this node via the
            // single shared launch path (also used by the instrument
            // editors' "Envelope..." buttons).
            launchAhdsrEnvelopeDialog(this, graph, nodeId);
        } else if (result == 182) {
            // FM: open the multi-tab operator-envelope editor (one AHDSR per
            // operator). Reuses the same AHDSREnvelopeComponent, one per tab.
            launchOpEnvelopesDialog(this, graph, nodeId,
                                    {"Op 1", "Op 2", "Op 3", "Op 4"});
        } else if (result == 190) {
            // Open the Script editor for an existing node. Same launch flow
            // as the "create + open" path in the menu above, including the
            // manual-trigger lookup callback.
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
            opts.dialogTitle = "Script: " + juce::String(node->name);
            opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar = false;
            opts.resizable = true;
            opts.componentToCentreAround = this;
            // Non-modal (live input surface - see the double-click path).
            SoundShop::launchNonModalToolDialog(opts);
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
            // Non-modal (live macro faders - see the double-click path).
            SoundShop::launchNonModalToolDialog(opts);
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
        } else if (result == 193) {
            // Re-open the Import Video dialog on an existing video terrain. It
            // re-seeds its crop / scale controls from the node's __video__
            // script, so the user can re-crop without re-picking the file.
            int captured = nodeId;
            auto* editor = new VideoImportDialogComponent(graph, captured,
                [this]() {
                    if (onNodeEdited) onNodeEdited();
                    graph.commitSnapshot("Edit video terrain");
                    repaint();
                });
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned(editor);
            opts.dialogTitle = "Import Video: " + juce::String(node->name);
            opts.dialogBackgroundColour = juce::Colour(0xff2b2b30);
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar = false;
            opts.resizable = true;
            opts.componentToCentreAround = this;
            SoundShop::launchNonModalToolDialog(opts);
        } else if (result == 194) {
            // Re-open the Generate dialog on an existing generated terrain. The
            // dialog seeds from the node's __generate__ script and edits it in
            // place; rank is locked so the node's pins/params stay valid.
            int captured = nodeId;
            GenerateTerrainParams seed;
            parseGenerateTerrainScript(node->script, seed, &graph.contentStore);
            auto* editor = new GenerateDialogComponent(
                seed, /*lockRank=*/true,
                [this, captured](const GenerateTerrainParams& gp) {
                    if (auto* nd = graph.findNode(captured)) {
                        nd->script = makeGenerateTerrainScript(gp, &graph.contentStore);
                        if (onNodeEdited) onNodeEdited();
                        graph.commitSnapshot("Edit generated terrain");
                        repaint();
                    }
                });
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned(editor);
            opts.dialogTitle = "Edit Source: " + juce::String(node->name);
            opts.dialogBackgroundColour = juce::Colour(0xff2b2b30);
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar = false;
            opts.resizable = true;
            opts.componentToCentreAround = this;
            SoundShop::launchNonModalToolDialog(opts);
        } else if (result == 195) {
            // Export the baked grid of a generated terrain to a NumPy .npz
            // file (a ZIP of the canonical .npy payload - what np.savez writes),
            // so the data can be loaded straight into NumPy / SciPy / etc.
            GenerateTerrainParams gp;
            bool haveGrid = parseGenerateTerrainScript(node->script, gp,
                                                       &graph.contentStore)
                            && !gp.data.empty();
            if (!haveGrid) {
                juce::NativeMessageBox::showAsync(
                    juce::MessageBoxOptions()
                        .withIconType(juce::MessageBoxIconType::WarningIcon)
                        .withTitle("Export grid as .npz")
                        .withMessage("This terrain has no baked grid to export.\n"
                                     "Open \"Edit Source...\" and Generate first.")
                        .withButton("OK")
                        .withAssociatedComponent(this),
                    nullptr);
            } else {
                auto base = juce::File::createLegalFileName(node->name);
                if (base.isEmpty()) base = "terrain";
                auto chooser = std::make_shared<juce::FileChooser>(
                    "Export grid as .npz",
                    juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                        .getChildFile(base + ".npz"),
                    "*.npz");
                // gp (grid data + dims) is captured by value so the async
                // callback never touches the node - safe if the node is gone.
                chooser->launchAsync(
                    juce::FileBrowserComponent::saveMode
                        | juce::FileBrowserComponent::warnAboutOverwriting,
                    [this, chooser, gp](const juce::FileChooser& fc) {
                        auto file = fc.getResult();
                        if (file == juce::File()) return; // cancelled
                        if (file.getFileExtension().isEmpty())
                            file = file.withFileExtension("npz");
                        auto npy = ContentStore::makeNpy(gp.data, gp.dims);
                        auto npz = ContentStore::makeNpz(
                            { { "terrain.npy", npy } });
                        bool ok = file.replaceWithData(npz.data(), npz.size());
                        if (!ok)
                            juce::NativeMessageBox::showAsync(
                                juce::MessageBoxOptions()
                                    .withIconType(juce::MessageBoxIconType::WarningIcon)
                                    .withTitle("Export grid as .npz")
                                    .withMessage("Could not write:\n"
                                                 + file.getFullPathName())
                                    .withButton("OK")
                                    .withAssociatedComponent(this),
                                nullptr);
                    });
            }
        } else if (result == 196 || result == 197) {
            // Domain-native grid export: WAV for a 1D terrain (treat the grid as
            // a mono waveform) or PNG for a 2D terrain (an 8-bit grayscale image,
            // float [-1,1] mapped to [0,255]). Both gate on the grid's actual
            // rank, which the menu already checked, but re-verify here so a stale
            // menu can't mis-export.
            const bool wantWav = (result == 196);
            GenerateTerrainParams gp;
            bool haveGrid = parseGenerateTerrainScript(node->script, gp,
                                                       &graph.contentStore)
                            && !gp.data.empty();
            const int rank = (int)gp.dims.size();
            const char* fmtName = wantWav ? "WAV" : "PNG";
            if (!haveGrid || (wantWav ? rank != 1 : rank != 2)) {
                juce::NativeMessageBox::showAsync(
                    juce::MessageBoxOptions()
                        .withIconType(juce::MessageBoxIconType::WarningIcon)
                        .withTitle(juce::String("Export grid as ") + fmtName)
                        .withMessage(juce::String(
                            haveGrid ? (wantWav
                                ? "WAV export needs a 1D grid (a waveform)."
                                : "PNG export needs a 2D grid (an image).")
                                     : "This terrain has no baked grid to export.\n"
                                       "Open \"Edit Source...\" and Generate first."))
                        .withButton("OK")
                        .withAssociatedComponent(this),
                    nullptr);
            } else {
                auto base = juce::File::createLegalFileName(node->name);
                if (base.isEmpty()) base = "terrain";
                const char* ext = wantWav ? "wav" : "png";
                // Capture the device sample rate now for WAV (the project rate,
                // or 44100 if it's the 0 "follow device" sentinel).
                int sr = (int)graph.projectSampleRate;
                if (sr <= 0) sr = 44100;
                auto chooser = std::make_shared<juce::FileChooser>(
                    juce::String("Export grid as ") + fmtName,
                    juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                        .getChildFile(base + "." + ext),
                    juce::String("*.") + ext);
                // gp captured by value: the async callback never touches the node.
                chooser->launchAsync(
                    juce::FileBrowserComponent::saveMode
                        | juce::FileBrowserComponent::warnAboutOverwriting,
                    [this, chooser, gp, wantWav, sr, ext, fmtName](const juce::FileChooser& fc) {
                        auto file = fc.getResult();
                        if (file == juce::File()) return; // cancelled
                        if (file.getFileExtension().isEmpty())
                            file = file.withFileExtension(ext);
                        bool ok = false;
                        if (wantWav) {
                            // Mono buffer straight from the float grid (already
                            // bipolar [-1,1]); WAV writer clamps on its own.
                            juce::AudioBuffer<float> buf(1, (int)gp.data.size());
                            std::copy(gp.data.begin(), gp.data.end(),
                                      buf.getWritePointer(0));
                            ExportOptions opt;
                            opt.format = ExportFormat::WAV;
                            opt.sampleRate = sr;
                            opt.bitsPerSample = 24;
                            opt.numChannels = 1;
                            ok = AudioExporter::exportToFile(file, buf, opt);
                        } else {
                            // 2D grid -> 8-bit grayscale PNG. dims[0]=rows (height),
                            // dims[1]=cols (width), row-major; float [-1,1] -> [0,255].
                            const int h = gp.dims[0], w = gp.dims[1];
                            juce::Image img(juce::Image::RGB, w, h, false);
                            {
                                juce::Image::BitmapData bmp(
                                    img, juce::Image::BitmapData::writeOnly);
                                for (int y = 0; y < h; ++y)
                                    for (int x = 0; x < w; ++x) {
                                        float v = gp.data[(size_t)y * w + x];
                                        int g = juce::jlimit(0, 255,
                                            (int)std::lround((v * 0.5f + 0.5f) * 255.0f));
                                        bmp.setPixelColour(x, y,
                                            juce::Colour((juce::uint8)g,
                                                         (juce::uint8)g,
                                                         (juce::uint8)g));
                                    }
                            }
                            juce::FileOutputStream os(file);
                            if (os.openedOk()) {
                                os.setPosition(0);
                                os.truncate();
                                ok = juce::PNGImageFormat().writeImageToStream(img, os);
                            }
                        }
                        if (!ok)
                            juce::NativeMessageBox::showAsync(
                                juce::MessageBoxOptions()
                                    .withIconType(juce::MessageBoxIconType::WarningIcon)
                                    .withTitle(juce::String("Export grid as ") + fmtName)
                                    .withMessage("Could not write:\n"
                                                 + file.getFullPathName())
                                    .withButton("OK")
                                    .withAssociatedComponent(this),
                                nullptr);
                    });
            }
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
                                std::lock_guard<std::recursive_mutex> graphLk(graph.mutationLock);
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
    {
        std::lock_guard<std::recursive_mutex> graphLk(graph.mutationLock);
        graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
            [this](auto& l) { return l.id == selectedLinkId; }), graph.links.end());
    }
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
    std::lock_guard<std::recursive_mutex> graphLk(graph.mutationLock);

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
            {
                std::lock_guard<std::recursive_mutex> graphLk(graph.mutationLock);
                graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
                    [linkId](auto& l) { return l.id == linkId; }), graph.links.end());
            }
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

// ----------------------------------------------------------------------------
// Shared AHDSR envelope dialog launcher (declared in node_graph_component.h).
// ----------------------------------------------------------------------------
void launchAhdsrEnvelopeDialog(juce::Component* parent, NodeGraph& graph,
                               int nodeId) {
    Node* node = graph.findNode(nodeId);
    if (!node) return;
    // The dialog hosts the reusable AHDSREnvelopeComponent editing
    // node->ahdsrEnvelope by reference. node->ahdsrEnvelope is the single
    // source of truth that every tonal synth reads directly, so the
    // onChanged callback only needs to mark the graph dirty; the undo
    // snapshot is committed once below (an undo right after the edit
    // reverts the whole gesture rather than fragmenting per slider tick).
    auto* content = new AHDSREnvelopeComponent(node->ahdsrEnvelope,
        [&graph, nodeId]() {
            if (graph.findNode(nodeId)) graph.dirty = true;
        });
    // Wire the project asset-library AHDSR-curve store so the dialog can
    // reference / publish a shared curve. Closures look the node up by id each
    // time (never capture Node* - graph.nodes can reallocate).
    AHDSREnvelopeComponent::LibraryContext libCtx;
    libCtx.lib = &graph.assets;
    libCtx.getAssetId = [&graph, nodeId]() -> int {
        Node* n = graph.findNode(nodeId);
        return n ? n->ahdsrAssetId : -1;
    };
    libCtx.setAssetId = [&graph, nodeId](int id) {
        if (Node* n = graph.findNode(nodeId)) n->ahdsrAssetId = id;
    };
    libCtx.propagate = [&graph]() { graph.resolveAhdsrReferences(); };
    content->setLibraryContext(libCtx);
    content->setSize(700, 516);
    juce::DialogWindow::LaunchOptions opt;
    opt.dialogTitle = "Envelope - " + juce::String(node->name);
    opt.content.setOwned(content);
    opt.escapeKeyTriggersCloseButton = true;
    opt.useNativeTitleBar = true;
    opt.resizable = true;
    opt.componentToCentreAround = parent;
    launchToolDialog(opt);   // no separate taskbar entry
    graph.commitSnapshot("Edit envelope");
}

// ----------------------------------------------------------------------------
// Multi-tab AHDSR editor (declared in node_graph_component.h). Hosts one
// AHDSREnvelopeComponent per tab, each editing node.opEnvelopes[i] by
// reference. Modal, so the references stay valid for the dialog's lifetime
// (the user can't restructure the graph while it's up) and one undo snapshot
// is committed after it closes - mirroring launchAhdsrEnvelopeDialog.
// ----------------------------------------------------------------------------
namespace {
class OpEnvelopesContent : public juce::Component {
public:
    OpEnvelopesContent(NodeGraph& graph, int nodeId,
                       const std::vector<juce::String>& tabNames)
        : tabs(juce::TabbedButtonBar::TabsAtTop) {
        Node* node = graph.findNode(nodeId);
        jassert(node != nullptr);
        // Guarantee one envelope per requested tab.
        if (node->opEnvelopes.size() < tabNames.size())
            node->opEnvelopes.resize(tabNames.size());

        auto tabColour = getLookAndFeel().findColour(
            juce::ResizableWindow::backgroundColourId);
        for (size_t i = 0; i < tabNames.size(); ++i) {
            // Bind by reference to the i-th envelope; the onChanged callback
            // looks the node up by id (never captures Node*) and only marks
            // the graph dirty - the undo snapshot is committed by the caller.
            auto* editor = new AHDSREnvelopeComponent(
                node->opEnvelopes[i],
                [&graph, nodeId]() {
                    if (graph.findNode(nodeId)) graph.dirty = true;
                });
            tabs.addTab(tabNames[i], tabColour, editor, true);
        }
        addAndMakeVisible(tabs);
    }
    void resized() override { tabs.setBounds(getLocalBounds()); }
private:
    juce::TabbedComponent tabs;
};
} // anonymous namespace

void launchOpEnvelopesDialog(juce::Component* parent, NodeGraph& graph,
                             int nodeId,
                             const std::vector<juce::String>& tabNames) {
    Node* node = graph.findNode(nodeId);
    if (!node || tabNames.empty()) return;
    auto* content = new OpEnvelopesContent(graph, nodeId, tabNames);
    // Single editor is 700x516; add tab-bar height for the tabbed wrapper.
    content->setSize(700, 552);
    juce::DialogWindow::LaunchOptions opt;
    opt.dialogTitle = "Operator Envelopes - " + juce::String(node->name);
    opt.content.setOwned(content);
    opt.escapeKeyTriggersCloseButton = true;
    opt.useNativeTitleBar = true;
    opt.resizable = true;
    opt.componentToCentreAround = parent;
    launchToolDialog(opt);   // modal, no separate taskbar entry
    graph.commitSnapshot("Edit operator envelopes");
}

} // namespace SoundShop

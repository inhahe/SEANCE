#include "routing_strip.h"
#include <algorithm>
#include <cmath>

namespace SoundShop {

RoutingStrip::RoutingStrip(NodeGraph& g, Transport& t) : graph(g), transport(t) {}

void RoutingStrip::setHorizontalView(float scrollBeat, float visibleBeats,
                                      float totalBeats, float gridX, float gridW)
{
    // Idempotent on purpose: MainContentComponent polls this every timer tick
    // (there is no scroll callback on the piano roll), so repainting
    // unconditionally would mean a 30 Hz repaint of a strip that never changed.
    auto same = [](float a, float b) { return std::abs(a - b) < 0.001f; };
    if (same(scrollBeat, hScroll) && same(visibleBeats, hVisible)
        && same(totalBeats, hTotal) && same(gridX, gX) && same(gridW, gW))
        return;
    hScroll   = scrollBeat;
    hVisible  = visibleBeats;
    hTotal    = totalBeats;
    gX        = gridX;
    gW        = gridW;
    repaint();
}

float RoutingStrip::gridWidth() const {
    // gW is 0 until a piano roll has told us its geometry; until then just use
    // everything right of the gutter so the strip is never zero-width.
    return gW > 0.0f ? gW : std::max(1.0f, (float)getWidth() - gX);
}

float RoutingStrip::beatToX(float absBeat) const {
    return gX + ((absBeat - hScroll) / std::max(0.1f, hVisible)) * gridWidth();
}

int RoutingStrip::getDesiredHeight() const {
    auto links = collectGatedLinks();
    if (links.empty()) return 0;
    return (int)std::ceil(rowTop((int)links.size()) + 1.0f);
}

std::vector<RoutingStrip::GatedLinkInfo> RoutingStrip::collectGatedLinks() const {
    // Find all links referenced by any EffectRegion on any node.
    std::map<int, GatedLinkInfo> byLink;

    for (const auto& node : graph.nodes) {
        // Regions are stored in node-LOCAL beats, exactly like clips and notes,
        // so they slide when the track's start position moves. The strip's axis
        // is absolute, so fold the offset in here.
        const float off = node.absoluteBeatOffset;
        for (const auto& region : node.effectRegions) {
            const std::pair<float, float> span{ region.startBeat + off,
                                                region.endBeat + off };
            if (region.linkId >= 0) {
                auto& info = byLink[region.linkId];
                info.linkId = region.linkId;
                info.spans.push_back(span);
                if (info.color == 0) info.color = region.color;
            }
            // Also expand groups: each link in the group gets an entry
            if (region.groupId >= 0) {
                if (auto* grp = graph.findEffectGroup(region.groupId)) {
                    for (int lid : grp->linkIds) {
                        auto& info = byLink[lid];
                        info.linkId = lid;
                        info.spans.push_back(span);
                        if (info.color == 0) info.color = grp->color;
                    }
                }
            }
        }
    }

    // Fill in labels from the actual link data
    const juce::String arrow = juce::String::fromUTF8(" \xe2\x86\x92 ");
    for (auto& [id, info] : byLink) {
        for (const auto& link : graph.links) {
            if (link.id == id) {
                juce::String src, dst;
                for (const auto& n : graph.nodes) {
                    for (const auto& pin : n.pinsOut)
                        if (pin.id == link.startPin) src = n.name;
                    for (const auto& pin : n.pinsIn)
                        if (pin.id == link.endPin) dst = n.name;
                }
                info.label = src + arrow + dst;
                break;
            }
        }
        if (info.color == 0)
            info.color = getDistinctColor(id);
    }

    std::vector<GatedLinkInfo> result;
    for (auto& [_, info] : byLink) result.push_back(std::move(info));
    return result;
}

juce::String RoutingStrip::getTooltip() {
    auto links = collectGatedLinks();
    if (links.empty()) return {};
    const float my = (float)getMouseXYRelative().y;
    if (my < headerH)
        return "Routing overview: every wire that's switched on and off over time, "
               "across the whole project. Edit these in a track's Effects lane.";
    for (int i = 0; i < (int)links.size(); ++i)
        if (my >= rowTop(i) && my < rowTop(i) + wireH)
            return links[(size_t)i].label + " - active where the bar is drawn";
    return {};
}

void RoutingStrip::paint(juce::Graphics& g) {
    auto links = collectGatedLinks();
    if (links.empty()) return;

    float w = (float)getWidth();
    float h = (float)getHeight();

    // Background
    g.setColour(juce::Colour(18, 18, 24));
    g.fillRect(0.0f, 0.0f, w, h);

    // Caption gets a band of its own across the full width. Sharing the left
    // gutter with the per-wire labels made both illegible on a one-wire strip.
    g.setColour(juce::Colours::grey.withAlpha(0.75f));
    g.setFont(11.0f);
    g.drawText("Routing", 4, 0, (int)w - 8, (int)headerH,
               juce::Justification::centredLeft, false);

    // Separator line at bottom
    g.setColour(juce::Colour(50, 50, 60));
    g.drawHorizontalLine((int)h - 1, 0, w);

    const float gridW = gridWidth();
    const float gridR = gX + gridW;

    for (int i = 0; i < (int)links.size(); ++i) {
        const auto& info = links[(size_t)i];
        float wy = rowTop(i);

        auto wireColor = juce::Colour((uint8_t)((info.color >> 16) & 0xFF),
                                      (uint8_t)((info.color >> 8) & 0xFF),
                                      (uint8_t)(info.color & 0xFF));

        // Row identity in the gutter. The gutter is only as wide as the piano
        // roll's key ruler (~40 px), which is far too narrow for "Synth -> Reverb"
        // at any readable font size - so it carries a colour chip instead,
        // matching the wire in the graph and the tube in the Effects lane. The
        // name itself goes on the tube (below), with the tooltip as the fallback.
        {
            juce::Rectangle<float> chip(4.0f, wy + wireH * 0.5f - 2.5f,
                                        std::max(6.0f, gX - 10.0f), 5.0f);
            g.setColour(wireColor.withAlpha(0.85f));
            g.fillRoundedRectangle(chip, 2.5f);
        }

        // Draw each active region as a wire segment
        for (const auto& [startBeat, endBeat] : info.spans) {
            float rx1 = beatToX(startBeat);
            float rx2 = beatToX(endBeat);
            if (rx2 < gX || rx1 > gridR) continue;
            const bool clipL = rx1 < gX, clipR = rx2 > gridR;
            rx1 = std::max(rx1, gX);
            rx2 = std::min(rx2, gridR);

            // Wire-like tube appearance using Lambert cosine shading on a
            // cylinder cross-section. Light direction tilted slightly from
            // above-left (~20 degrees from top normal).
            {
                const float lightAngle = 0.35f; // radians from top-center
                int iH = (int)wireH;
                for (int row = 0; row < iH; ++row) {
                    // Map row to angle on the semicircular cross-section:
                    // row 0 = top (theta = -pi/2), row iH = bottom (theta = +pi/2)
                    float t = (float)row / (float)std::max(1, iH - 1); // 0..1
                    float theta = (t - 0.5f) * 3.14159265f; // -pi/2 .. +pi/2
                    // Lambert diffuse: brightness = cos(theta - lightAngle)
                    float brightness = std::cos(theta - lightAngle);
                    brightness = std::max(0.0f, brightness);
                    // Add a small ambient so the dark edges aren't pure black
                    float lum = 0.15f + 0.85f * brightness;
                    auto rowColor = wireColor.interpolatedWith(
                        juce::Colours::white, (lum - 0.5f) * 0.5f);
                    if (lum < 0.5f)
                        rowColor = wireColor.interpolatedWith(
                            juce::Colours::black, (0.5f - lum) * 0.8f);
                    g.setColour(rowColor);
                    g.drawHorizontalLine((int)(wy + row), rx1, rx2);
                }
            }

            // End caps: rounded where the layer really ends, flat and bright
            // where it just runs off the edge of the view - same convention as
            // the Effects lane, so a clipped tube never reads as a short one.
            float capR = wireH * 0.5f;
            g.setColour(wireColor);
            if (!clipL) g.fillEllipse(rx1 - capR * 0.3f, wy, capR * 0.6f, wireH);
            if (!clipR) g.fillEllipse(rx2 - capR * 0.3f, wy, capR * 0.6f, wireH);
            g.setColour(wireColor.brighter(0.7f));
            if (clipL) g.fillRect(rx1, wy, 2.0f, wireH);
            if (clipR) g.fillRect(rx2 - 2.0f, wy, 2.0f, wireH);

            // Name on the tube, like a clip name - the only place there's room
            // for it at a legible size. Contrast is picked against the tube's
            // own brightness so it stays readable on any wire colour.
            if (rx2 - rx1 >= labelMinTubeW && info.label.isNotEmpty()) {
                juce::Graphics::ScopedSaveState clipState(g);
                g.reduceClipRegion(juce::Rectangle<int>((int)rx1 + 3, (int)wy,
                                                        (int)(rx2 - rx1) - 6, (int)wireH));
                g.setColour(wireColor.getPerceivedBrightness() > 0.55f
                                ? juce::Colours::black.withAlpha(0.8f)
                                : juce::Colours::white.withAlpha(0.9f));
                g.setFont(10.0f);
                g.drawText(info.label, (int)rx1 + 4, (int)wy,
                           (int)(rx2 - rx1) - 8, (int)wireH,
                           juce::Justification::centredLeft, false);
            }
        }
    }
}

} // namespace SoundShop

#include "routing_strip.h"
#include <algorithm>

namespace SoundShop {

RoutingStrip::RoutingStrip(NodeGraph& g, Transport& t) : graph(g), transport(t) {}

void RoutingStrip::setHorizontalView(float scrollBeat, float visibleBeats,
                                      float totalBeats, float gridX)
{
    hScroll   = scrollBeat;
    hVisible  = visibleBeats;
    hTotal    = totalBeats;
    gX        = gridX;
    repaint();
}

int RoutingStrip::getDesiredHeight() const {
    auto links = collectGatedLinks();
    if (links.empty()) return 0;
    return (int)(links.size() * (wireH + wireGap) + wireGap + 4);
}

std::vector<RoutingStrip::GatedLinkInfo> RoutingStrip::collectGatedLinks() const {
    // Find all links referenced by any EffectRegion on any node.
    std::map<int, GatedLinkInfo> byLink;

    for (const auto& node : graph.nodes) {
        for (const auto& region : node.effectRegions) {
            if (region.linkId >= 0) {
                auto& info = byLink[region.linkId];
                info.linkId = region.linkId;
                info.regions.push_back(&region);
                if (info.color == 0) info.color = region.color;
            }
            // Also expand groups: each link in the group gets an entry
            if (region.groupId >= 0) {
                if (auto* grp = graph.findEffectGroup(region.groupId)) {
                    for (int lid : grp->linkIds) {
                        auto& info = byLink[lid];
                        info.linkId = lid;
                        info.regions.push_back(&region);
                        if (info.color == 0) info.color = grp->color;
                    }
                }
            }
        }
    }

    // Fill in labels from the actual link data
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
                info.label = src + " > " + dst;
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

void RoutingStrip::paint(juce::Graphics& g) {
    auto links = collectGatedLinks();
    if (links.empty()) return;

    float w = (float)getWidth();
    float h = (float)getHeight();

    // Background
    g.setColour(juce::Colour(18, 18, 24));
    g.fillRect(0.0f, 0.0f, w, h);

    // "Routing" label
    g.setColour(juce::Colours::grey.withAlpha(0.5f));
    g.setFont(9.0f);
    g.drawText("Routing", 4, 0, (int)gX - 8, (int)h,
               juce::Justification::centredLeft, false);

    // Separator line at bottom
    g.setColour(juce::Colour(50, 50, 60));
    g.drawHorizontalLine((int)h - 1, 0, w);

    float gridW = w - gX;
    auto beatToX = [&](float beat) -> float {
        return gX + ((beat - hScroll) / std::max(0.1f, hVisible)) * gridW;
    };

    for (int i = 0; i < (int)links.size(); ++i) {
        const auto& info = links[i];
        float wy = wireGap + i * (wireH + wireGap);

        auto wireColor = juce::Colour((uint8_t)((info.color >> 16) & 0xFF),
                                      (uint8_t)((info.color >> 8) & 0xFF),
                                      (uint8_t)(info.color & 0xFF));

        // Draw each active region as a wire segment
        for (const auto* region : info.regions) {
            float rx1 = beatToX(region->startBeat);
            float rx2 = beatToX(region->endBeat);
            if (rx2 < gX || rx1 > w) continue;
            rx1 = std::max(rx1, gX);
            rx2 = std::min(rx2, w);

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

            // Rounded end caps (semicircles at left and right edges)
            float capR = wireH * 0.5f;
            g.setColour(wireColor);
            g.fillEllipse(rx1 - capR * 0.3f, wy, capR * 0.6f, wireH);
            g.fillEllipse(rx2 - capR * 0.3f, wy, capR * 0.6f, wireH);
        }

        // Label at the left margin
        g.setColour(wireColor.withAlpha(0.7f));
        g.setFont(8.0f);
        g.drawText(info.label, 2, (int)wy, (int)gX - 4, (int)wireH,
                   juce::Justification::centredLeft, false);
    }
}

} // namespace SoundShop

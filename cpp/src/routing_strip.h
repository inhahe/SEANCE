#pragma once
#include "node_graph.h"
#include "transport.h"
#include "effect_regions.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace SoundShop {

// A shared timeline strip drawn above all piano roll panels, showing
// time-gated link connections across the whole project. Each gated link
// appears as a wire-like bar with 3D shading, with small gaps between
// wires to reinforce that they're separate connections.
//
// The horizontal axis matches the piano rolls below (same beat range
// and scroll). Collapsible — hidden when no link gates exist.
class RoutingStrip : public juce::Component {
public:
    RoutingStrip(NodeGraph& graph, Transport& transport);

    void paint(juce::Graphics& g) override;
    void resized() override {}

    // Sync horizontal scroll/zoom with the piano roll. Called by
    // MainContentComponent whenever the piano roll scrolls.
    void setHorizontalView(float scrollBeat, float visibleBeats, float totalBeats, float gridX);

    // Returns the desired height based on how many gated links exist.
    // Returns 0 if no link gates → the strip should be hidden.
    int getDesiredHeight() const;

private:
    NodeGraph& graph;
    Transport& transport;

    float hScroll = 0.0f;
    float hVisible = 16.0f;
    float hTotal = 32.0f;
    float gX = 40.0f; // left margin matching piano roll's KEY_WIDTH

    static constexpr float wireH = 10.0f;
    static constexpr float wireGap = 3.0f;

    // Collect all links that have time-gated regions anywhere in the graph.
    struct GatedLinkInfo {
        int linkId;
        juce::String label; // "From > To"
        uint32_t color;
        std::vector<const EffectRegion*> regions; // pointers into graph nodes
    };
    std::vector<GatedLinkInfo> collectGatedLinks() const;
};

} // namespace SoundShop

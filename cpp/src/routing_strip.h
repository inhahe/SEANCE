#pragma once
#include "node_graph.h"
#include "transport.h"
#include "effect_regions.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace SoundShop {

// A shared timeline strip drawn above all piano roll panels, showing every
// time-gated wire in the WHOLE project on one axis. Each gated wire gets a row;
// the beats where it's switched on appear as a shaded tube in that wire's (or
// its effect group's) colour.
//
// This complements, rather than duplicates, the per-track Effects lane inside
// each piano roll: the lane shows and edits ONE track's layers, the strip is the
// read-only project-wide overview, so you can see that the reverb send opens at
// bar 9 on one track while the delay closes at bar 11 on another.
//
// The horizontal axis is in ABSOLUTE beats and is driven by
// setHorizontalView(), which MainContentComponent feeds from the topmost editor
// panel - the one physically underneath the strip - so a tube always sits above
// the beats it gates. Hidden entirely when no layers exist.
class RoutingStrip : public juce::Component,
                     public juce::TooltipClient {
public:
    RoutingStrip(NodeGraph& graph, Transport& transport);

    void paint(juce::Graphics& g) override;
    void resized() override {}

    // Full wire name for the row under the mouse. Row labels are drawn on the
    // tubes themselves and a short tube has no room for one, so the tooltip is
    // the reliable way to identify a wire. Implemented for juce::TooltipClient;
    // the shared TooltipWindow lives on MainContentComponent.
    juce::String getTooltip() override;

    // Sync horizontal scroll/zoom with the piano roll below. Called by
    // MainContentComponent every timer tick; a no-op (no repaint) when nothing
    // actually moved, which is what makes polling instead of a scroll callback
    // acceptable.
    void setHorizontalView(float scrollBeat, float visibleBeats, float totalBeats,
                           float gridX, float gridW);

    // Returns the desired height based on how many gated wires exist.
    // Returns 0 if there are none -> the strip should be hidden.
    int getDesiredHeight() const;

private:
    NodeGraph& graph;
    Transport& transport;

    float hScroll = 0.0f;
    float hVisible = 16.0f;
    float hTotal = 32.0f;
    float gX = 40.0f;   // left gutter, matching the piano roll's key ruler
    float gW = 0.0f;    // grid width; 0 = "not told yet", fall back to the rest

    // A caption band of its own. The previous layout drew "Routing" and the
    // per-wire labels into the same left gutter, so on a one-wire strip they
    // landed on top of each other and neither was legible.
    static constexpr float headerH = 14.0f;
    static constexpr float wireH   = 14.0f;
    static constexpr float wireGap = 3.0f;
    // A tube narrower than this has no room for its name; the tooltip covers it.
    static constexpr float labelMinTubeW = 34.0f;

    // Collect all wires that have time-gated regions anywhere in the graph.
    struct GatedLinkInfo {
        int linkId = -1;
        juce::String label;        // "From -> To"
        uint32_t color = 0;
        // Beat spans in ABSOLUTE beats. Regions are stored node-locally, so the
        // owning node's absoluteBeatOffset is folded in here at collection time
        // - that also avoids holding pointers into node.effectRegions.
        std::vector<std::pair<float, float>> spans;
    };
    std::vector<GatedLinkInfo> collectGatedLinks() const;

    float rowTop(int i) const { return headerH + wireGap + (float)i * (wireH + wireGap); }
    float beatToX(float absBeat) const;
    float gridWidth() const;
};

} // namespace SoundShop

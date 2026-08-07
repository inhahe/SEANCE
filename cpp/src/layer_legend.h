#pragma once
#include "node_graph.h"
#include "transport.h"
#include "effect_regions.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace SoundShop {

// A thin band above the stack of track editors listing, as coloured chips,
// every wire or effect group that is time-gated ANYWHERE in the project.
//
// It is a LEGEND, not a timeline. That distinction is the whole design:
//
//   * A layer's identity, once drawn, is nothing but a colour. Collapsed
//     Effects lanes are unlabelled tubes, and the graph tags wires with colour
//     too, so the project needs one place that says which colour is which wire.
//     Nowhere else does.
//   * It deliberately does NOT draw bars over a time axis. It used to, and the
//     axis was a fiction: every piano roll scrolls and zooms independently, so
//     there is no project-wide time axis for a project-wide band to plot
//     against. Borrowing one panel's axis produced a "universal" strip that
//     moved when you scrolled one track, and that simply restated - larger -
//     what that track's own Effects lane already showed correctly. When it's
//     timing you want, the Effects lane is the honest answer; when it's
//     identity you want, this is.
//
// The per-track spans survive as the chip's tooltip, which lists which tracks
// gate the wire and over what beats - the cross-track information the bars were
// reaching for, stated instead of drawn.
class LayerLegend : public juce::Component,
                    public juce::TooltipClient {
public:
    LayerLegend(NodeGraph& graph, Transport& transport);

    void paint(juce::Graphics& g) override;
    void resized() override { rebuild(); }

    // Full name plus per-track beat spans for the chip under the mouse.
    juce::String getTooltip() override;

    // Height needed to flow every chip at `width` px, or 0 when nothing in the
    // project is gated -> the band should be hidden. Takes the width because
    // the caller lays this band out before it has a width of its own.
    int getDesiredHeight(int width) const;

    // Re-read the graph; returns true (having repainted) if what should be on
    // display actually changed, so the host can re-run its layout - a new chip
    // can push the band onto a second row.
    //
    // Polled rather than pushed: layers and groups are created and deleted from
    // the piano roll's Effects lane, the graph's wire menus, project load, undo
    // and redo, and requiring every one of those to notify a band they don't
    // otherwise know about is exactly the kind of coupling that goes stale. The
    // check is a string compare over a handful of entries.
    bool refresh();

private:
    NodeGraph& graph;
    Transport& transport;

    static constexpr int   padY    = 3;
    static constexpr int   rowH    = 18;
    static constexpr int   chipGap = 6;
    static constexpr int   swatchW = 10;
    static constexpr float fontH   = 11.5f;

    // One entry per distinct gated THING. A group stays a single entry rather
    // than being expanded into its member wires: the group is the identity the
    // user chose when they made the layer, and expanding it is precisely what
    // made this band look like a duplicate of everything else.
    struct Entry {
        bool isGroup = false;
        int  id = -1;                       // group id or link id
        juce::String name;                  // group name, or "Src -> Dst"
        juce::String detail;                // member wires (groups only)
        juce::Colour colour;
        std::vector<juce::String> gatedOn;  // "Track (beats a-b)", one per region
        juce::Rectangle<int> bounds;        // filled in by flow()
    };
    std::vector<Entry> entries;
    juce::String lastSignature;   // what refresh() compares against

    // Width reserved for the "Layers:" caption on the first row. Recomputed
    // rather than cached so getDesiredHeight() stays const and can be called
    // before the band has ever been painted.
    int captionWidth() const;

    std::vector<Entry> collect() const;
    static juce::String signatureOf(const std::vector<Entry>& es);
    // Re-collect and flow the chips into the current width.
    void rebuild();
    int  flow(std::vector<Entry>& es, int width) const; // -> total height
    int  chipWidth(const Entry& e) const;
    static juce::Font legendFont() { return juce::Font{juce::FontOptions(fontH)}; }
};

} // namespace SoundShop

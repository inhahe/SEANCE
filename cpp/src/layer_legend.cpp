#include "layer_legend.h"
#include <algorithm>
#include <cmath>

namespace SoundShop {

LayerLegend::LayerLegend(NodeGraph& g, Transport& t) : graph(g), transport(t) {}

// Beat numbers in the tooltip are rounded to 2 dp and stripped of trailing
// zeros: layer edges are pixel-precise so they are rarely whole numbers, but
// "beats 4 - 8" reads better than "beats 4.00 - 8.00" for the common case where
// they landed on the grid.
static juce::String beatStr(float b) {
    juce::String s = juce::String(b, 2);
    if (s.contains(".")) {
        while (s.endsWithChar('0')) s = s.dropLastCharacters(1);
        if (s.endsWithChar('.')) s = s.dropLastCharacters(1);
    }
    return s;
}

static juce::Colour entryColour(const NodeGraph& graph, const EffectRegion& r) {
    uint32_t col = r.color;
    if (col == 0 && r.groupId >= 0)
        if (auto* grp = graph.findEffectGroup(r.groupId)) col = grp->color;
    if (col == 0 && r.linkId >= 0) col = getDistinctColor(r.linkId);
    if (col == 0) col = 0xFF808080;
    return juce::Colour((uint8_t)((col >> 16) & 0xFF),
                        (uint8_t)((col >> 8) & 0xFF),
                        (uint8_t)(col & 0xFF));
}

std::vector<LayerLegend::Entry> LayerLegend::collect() const {
    std::vector<Entry> out;

    auto find = [&out](bool isGroup, int id) -> Entry* {
        for (auto& e : out)
            if (e.isGroup == isGroup && e.id == id) return &e;
        return nullptr;
    };

    for (const auto& node : graph.nodes) {
        // Regions are stored in node-LOCAL beats, exactly like clips and notes,
        // so they slide when the track's start position moves. The tooltip
        // compares spans across tracks, so it needs absolute beats.
        const float off = node.absoluteBeatOffset;
        for (const auto& region : node.effectRegions) {
            const bool isGroup = region.groupId >= 0;
            const int  id      = isGroup ? region.groupId : region.linkId;
            if (id < 0) continue;

            Entry* e = find(isGroup, id);
            if (!e) {
                Entry fresh;
                fresh.isGroup = isGroup;
                fresh.id      = id;
                fresh.name    = graph.gateLabel(region);
                fresh.colour  = entryColour(graph, region);
                if (isGroup) {
                    // A group's chip says only the group's name, because that is
                    // the identity the user made. The member wires belong in the
                    // tooltip - listing them as chips of their own is exactly
                    // what made this band look like a copy of everything else.
                    if (auto* grp = graph.findEffectGroup(id)) {
                        juce::StringArray members;
                        for (int lid : grp->linkIds) {
                            auto w = graph.wireLabel(lid);
                            if (w.isNotEmpty()) members.add(w);
                        }
                        fresh.detail = members.isEmpty()
                                           ? juce::String("no wires in this group yet")
                                           : members.joinIntoString("\n");
                    }
                }
                out.push_back(std::move(fresh));
                e = &out.back();
            }
            e->gatedOn.push_back(juce::String(node.name) + "  ("
                                 + beatStr(region.startBeat + off) + " - "
                                 + beatStr(region.endBeat + off) + ")");
        }
    }

    // Groups first, then wires; alphabetical inside each. Stable ordering matters
    // more than it looks: the chips are the only place a colour is named, so a
    // chip that jumps around when an unrelated track gains a layer would make the
    // legend hard to use as a reference.
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.isGroup != b.isGroup) return a.isGroup;
        return a.name.compareIgnoreCase(b.name) < 0;
    });
    return out;
}

int LayerLegend::chipWidth(const Entry& e) const {
    // getStringWidth is deprecated in JUCE 8 and returns 0; GlyphArrangement is
    // the supported measurement, and only reports real metrics for a Font built
    // through FontOptions (see legendFont()).
    const int textW = juce::GlyphArrangement::getStringWidthInt(legendFont(), e.name);
    return swatchW + 5 + textW + 10;
}

int LayerLegend::captionWidth() const {
    return juce::GlyphArrangement::getStringWidthInt(legendFont(), "Layers:") + 12;
}

int LayerLegend::flow(std::vector<Entry>& es, int width) const {
    if (es.empty()) return 0;
    const int left = captionWidth();
    int x = left;
    int y = padY;
    int rows = 1;
    for (auto& e : es) {
        const int w = chipWidth(e);
        // Wrap rather than clip or scroll: the band is a reference the user reads
        // at a glance, and a chip cut in half names no colour at all.
        if (x > left && x + w > width - padY) {
            x = left;
            y += rowH + 2;
            ++rows;
        }
        e.bounds = { x, y, w, rowH };
        x += w + chipGap;
    }
    return padY * 2 + rows * rowH + (rows - 1) * 2;
}

juce::String LayerLegend::signatureOf(const std::vector<Entry>& es) {
    juce::String s;
    for (const auto& e : es) {
        s << (e.isGroup ? 'g' : 'w') << e.id << '|' << e.name << '|'
          << e.colour.toString() << '|' << e.detail << '|';
        for (const auto& t : e.gatedOn) s << t << ',';
        s << ';';
    }
    return s;
}

void LayerLegend::rebuild() {
    entries = collect();
    lastSignature = signatureOf(entries);
    flow(entries, std::max(getWidth(), 80));
}

bool LayerLegend::refresh() {
    if (signatureOf(collect()) == lastSignature) return false;
    rebuild();
    repaint();
    return true;
}

int LayerLegend::getDesiredHeight(int width) const {
    auto es = collect();
    if (es.empty()) return 0;   // nothing gated anywhere -> the band hides itself
    return flow(es, std::max(width, 80));
}

juce::String LayerLegend::getTooltip() {
    const auto p = getMouseXYRelative();
    for (const auto& e : entries) {
        if (!e.bounds.contains(p)) continue;
        juce::String t = e.name;
        if (e.detail.isNotEmpty()) t += "\n" + e.detail;
        t += "\n\nSwitched on by:";
        for (const auto& s : e.gatedOn) t += "\n  " + s;
        t += "\n\nEdit these in the track's Effects lane.";
        return t;
    }
    return "Every wire or effect group that is switched on and off over time, "
           "anywhere in this project, and the colour it is drawn in. "
           "Edit the timing in a track's Effects lane.";
}

void LayerLegend::paint(juce::Graphics& g) {
    const int w = getWidth(), h = getHeight();
    g.setColour(juce::Colour(18, 18, 24));
    g.fillRect(0, 0, w, h);
    g.setColour(juce::Colour(50, 50, 60));
    g.drawHorizontalLine(h - 1, 0.0f, (float)w);

    g.setFont(legendFont());
    g.setColour(juce::Colours::grey.withAlpha(0.75f));
    g.drawText("Layers:", 4, padY, captionWidth() - 8, rowH,
               juce::Justification::centredLeft, false);

    for (const auto& e : entries) {
        auto r = e.bounds.toFloat();

        // Chip body is a wash of the layer's own colour so the whole chip - not
        // just the swatch - reads as that colour from across the room, while the
        // label stays on a dark enough ground to be legible at 11.5 px.
        g.setColour(e.colour.withAlpha(0.16f));
        g.fillRoundedRectangle(r, 3.0f);
        g.setColour(e.colour.withAlpha(0.55f));
        g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);

        // The swatch is the part that has to match exactly: it is the same solid
        // fill the Effects-lane tube and the graph's wire tag use.
        juce::Rectangle<float> sw(r.getX() + 3.0f, r.getY() + 3.0f,
                                  (float)swatchW - 3.0f, r.getHeight() - 6.0f);
        g.setColour(e.colour);
        g.fillRoundedRectangle(sw, 2.0f);
        // Groups are diamonds elsewhere in the app; here the swatch gets a
        // brighter inner bar instead so a group still reads differently from a
        // single wire at this size, where a diamond would be four grey pixels.
        if (e.isGroup) {
            g.setColour(e.colour.contrasting(0.6f).withAlpha(0.9f));
            g.fillRect(sw.reduced(1.5f).withHeight(1.5f)
                         .withY(sw.getCentreY() - 0.75f));
        }

        g.setColour(juce::Colours::white.withAlpha(0.88f));
        g.drawText(e.name, e.bounds.getX() + swatchW + 5, e.bounds.getY(),
                   e.bounds.getWidth() - swatchW - 9, e.bounds.getHeight(),
                   juce::Justification::centredLeft, false);
    }
}

} // namespace SoundShop

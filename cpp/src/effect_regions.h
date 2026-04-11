#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <cmath>

namespace SoundShop {

// ==============================================================================
// Maximally distinct color palette (Trubetskoy 20 + extended)
// ==============================================================================

// First 20 colors are maximally perceptually distinct (Sasha Trubetskoy).
// Extended tier adds lighter/darker variants for overflow.
// Colors are consumed in order: first group gets color 0, second gets 1, etc.
// Tier 1: 20 maximally distinct (Trubetskoy). Tier 2: 10 less-distinct
// variants. Beyond that, getDistinctColor() generates deterministic
// pseudo-random colors using the golden-angle hue walk, so there's never
// a hard limit — just diminishing distinctiveness.
inline const uint32_t kFixedPalette[] = {
    // Tier 1: 20 maximally distinct (alpha 0xFF prefix)
    0xFFE6194B, // red
    0xFF3CB44B, // green
    0xFFFFE119, // yellow
    0xFF0082C8, // blue
    0xFFF58231, // orange
    0xFF911EB4, // purple
    0xFF46F0F0, // cyan
    0xFFF032E6, // magenta
    0xFFD2F53C, // lime
    0xFFFABEBE, // pink
    0xFF008080, // teal
    0xFFE6BEFF, // lavender
    0xFFAA6E28, // brown
    0xFFFFFAC8, // beige
    0xFF800000, // maroon
    0xFFAAFFC3, // mint
    0xFF808000, // olive
    0xFFFFD8B1, // coral
    0xFF000080, // navy
    0xFF808080, // grey
    // Tier 2: slightly less distinct (darkened/lightened variants)
    0xFFCC4444, // dark red
    0xFF44AA44, // dark green
    0xFFAAAA00, // dark yellow
    0xFF4488CC, // light blue
    0xFFCC8844, // dark orange
    0xFFBB66DD, // light purple
    0xFF66BBBB, // dark cyan
    0xFFCC66CC, // dark magenta
    0xFF88BB44, // dark lime
    0xFF996666, // dark pink
};
inline constexpr int kNumFixedColors = sizeof(kFixedPalette) / sizeof(kFixedPalette[0]);

// Get a distinct color by index. Uses the fixed palette for the first 30,
// then generates deterministic pseudo-random colors via the golden-angle
// hue walk (irrational spacing in HSL that avoids clustering). Never repeats
// within the first ~200 indices and stays reasonably distinct much further.
inline uint32_t getDistinctColor(int index) {
    if (index >= 0 && index < kNumFixedColors)
        return kFixedPalette[index];

    // Golden-angle hue walk: each step rotates by ~137.508 degrees in hue,
    // which maximally avoids nearby hues. Saturation and lightness alternate
    // between two bands so adjacent indices also differ in brightness.
    int k = index - kNumFixedColors;
    float hue = std::fmod((float)k * 137.508f, 360.0f) / 360.0f; // 0..1
    float sat = (k % 2 == 0) ? 0.7f : 0.5f;
    float lit = (k % 3 == 0) ? 0.55f : (k % 3 == 1) ? 0.40f : 0.70f;

    // HSL → RGB (simplified)
    auto hueToRgb = [](float p, float q, float t) -> float {
        if (t < 0.0f) t += 1.0f;
        if (t > 1.0f) t -= 1.0f;
        if (t < 1.0f/6.0f) return p + (q - p) * 6.0f * t;
        if (t < 1.0f/2.0f) return q;
        if (t < 2.0f/3.0f) return p + (q - p) * (2.0f/3.0f - t) * 6.0f;
        return p;
    };
    float q = (lit < 0.5f) ? lit * (1.0f + sat) : lit + sat - lit * sat;
    float p = 2.0f * lit - q;
    int r = (int)(hueToRgb(p, q, hue + 1.0f/3.0f) * 255.0f);
    int g = (int)(hueToRgb(p, q, hue) * 255.0f);
    int b = (int)(hueToRgb(p, q, hue - 1.0f/3.0f) * 255.0f);
    r = (r < 0) ? 0 : (r > 255) ? 255 : r;
    g = (g < 0) ? 0 : (g > 255) ? 255 : g;
    b = (b < 0) ? 0 : (b > 255) ? 255 : b;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// ==============================================================================
// Effect Groups — bundles of links that activate/deactivate together
// ==============================================================================

struct EffectGroup {
    int id = 0;
    std::string name;       // optional — empty = identified by color/shape only
    uint32_t color = 0;     // ARGB, auto-assigned from palette if 0
    std::vector<int> linkIds; // which Link IDs belong to this group
    // Per-group crossfade override. 0 = inherit NodeGraph::globalCrossfadeSec.
    // Non-zero overrides the global so a single group can have a longer or
    // shorter ramp than the rest of the project.
    float crossfadeSec = 0.0f;
};

// ==============================================================================
// Effect Regions — time-bounded activation of a link or group on a track
// ==============================================================================

struct EffectRegion {
    int linkId  = -1;       // gate a single link (-1 if using a group)
    int groupId = -1;       // gate a whole group (-1 if using a single link)
    float startBeat = 0;
    float endBeat = 4;
    uint32_t color = 0;     // ARGB, auto from link's effect node or group color
};

// Tag shapes drawn on wires in the graph view
enum class WireTagShape {
    Circle,     // individual wire identity
    Diamond     // group membership
};

} // namespace SoundShop

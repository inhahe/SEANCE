// terrain_ripple.c — Whole-grid TERRAIN generator for SoundShop2 (Terrain Synth
// "Terrain from Program (Generate)…", language = WASM).
//
// A terrain WASM module is the odd one out among WASM scripts: it has NO audio
// path. Instead of ss_process(), it exports ss_generate(), which the host calls
// ONCE, offline, on the message thread to fill an N-dimensional grid. Each cell
// is one value in [0,1] (a grayscale height); the host maps it to the terrain's
// bipolar [-1,1] as v*2-1. The grid is HOST-owned — you poke cells through the
// ss_grid_* host imports (see soundshop_wasm.h), not through linear memory.
//
// This example is rank-agnostic: it produces a radial ripple from the centre of
// however many axes the terrain has, so it works on a {512,512} image, a
// {64,128,128} "video", or a 1-D {44100} sound alike.
//
// Compile (clang, no libm needed — uses the header's libm-free helpers):
//   clang --target=wasm32 -O2 -nostdlib -Wl,--no-entry -Wl,--export-all \
//         -o terrain_ripple.wasm terrain_ripple.c
//
// Then in SEANCE: Add Node → Terrain → Terrain from Program (Generate)…,
// set Language = "WASM (.wasm module)", click "Browse .wasm…", pick the file.

#include "soundshop_wasm.h"

// A cheap, libm-free sine (good enough for terrain shaping). Argument in radians.
// Bhaskara I's approximation, range-reduced to [-pi, pi].
static float fast_sin(float x) {
    const float PI  = 3.14159265f;
    const float TAU = 6.28318531f;
    // range-reduce to [-pi, pi]
    x = ss_mod(x + PI, TAU) - PI;       // ss_mod = GLSL floored modulo (header)
    float ax = (x < 0.0f) ? -x : x;
    // Bhaskara: sin(x) ≈ 16x(pi-x) / (5pi^2 - 4x(pi-x)) for x in [0, pi]
    float num = 16.0f * ax * (PI - ax);
    float den = 5.0f * PI * PI - 4.0f * ax * (PI - ax);
    float s = num / den;
    return (x < 0.0f) ? -s : s;
}

// Terrain modules still need ss_init (re-seed hook); nothing to set up here.
int32_t ss_init(void) {
    return 0;
}

void ss_generate(void) {
    int32_t total = ss_grid_total();   // product(dims)
    int32_t nd    = ss_grid_nd();      // number of axes (rank)

    for (int32_t i = 0; i < total; ++i) {
        // Distance of this cell from the centre of the grid, in normalised units
        // (each axis coordinate is in [0,1], centre at 0.5).
        float d2 = 0.0f;
        for (int32_t a = 0; a < nd; ++a) {
            float c = ss_grid_coord(i, a) - 0.5f;   // [-0.5, 0.5]
            d2 += c * c;
        }
        // sqrt without libm: one Newton step is plenty for a height field.
        float r = d2;
        if (r > 0.0f) { r = 0.5f * (r + d2 / r); r = 0.5f * (r + d2 / r); }

        // Concentric ripples that fade out toward the edges.
        float ripple = fast_sin(r * 40.0f);         // 0-frequency at centre
        float fade   = 1.0f - ss_clamp(r * 1.6f, 0.0f, 1.0f);
        float v      = 0.5f + 0.5f * ripple * fade;  // back into [0,1]

        ss_grid_set(i, ss_clamp(v, 0.0f, 1.0f));
    }
}

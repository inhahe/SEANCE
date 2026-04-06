// tremolo.c — Beat-synced tremolo effect
// Compile: clang --target=wasm32 -O2 -nostdlib -Wl,--no-entry -Wl,--export-all -o tremolo.wasm tremolo.c
#include "soundshop_wasm.h"

// Simple sin approximation (no libm in freestanding WASM)
static float fsin(float x) {
    // Normalize to [-pi, pi]
    while (x >  3.14159265f) x -= 6.28318530f;
    while (x < -3.14159265f) x += 6.28318530f;
    // Bhaskara I approximation
    float x2 = x * x;
    return (16.0f * x * (3.14159265f - x)) /
           (5.0f * 3.14159265f * 3.14159265f - 4.0f * x * (3.14159265f - x));
}

static int param_depth;
static int param_rate;

int32_t ss_init(void) {
    param_depth = ss_declare_param("Depth", 0.5f, 0.0f, 1.0f);
    param_rate  = ss_declare_param("Rate (beats)", 1.0f, 0.25f, 8.0f);
    return 0;
}

void ss_process(void) {
    float depth = ss_param_value(param_depth);
    float rateBeats = ss_param_value(param_rate);
    uint32_t bs = SS_BLOCK_SIZE;
    float sr = SS_SAMPLE_RATE;
    float bpm = SS_BPM;
    double beatPos = SS_BEAT_POS;

    float* inL  = ss_audio_in(0, 0);
    float* inR  = ss_audio_in(0, 1);
    float* outL = ss_audio_out(0, 0);
    float* outR = ss_audio_out(0, 1);

    float beatsPerSample = bpm / (60.0f * sr);

    for (uint32_t i = 0; i < bs; i++) {
        float beat = (float)beatPos + i * beatsPerSample;
        float phase = beat / rateBeats;
        float lfo = 0.5f + 0.5f * fsin(phase * 6.28318530f);
        float gain = 1.0f - depth * (1.0f - lfo);
        outL[i] = inL[i] * gain;
        outR[i] = inR[i] * gain;
    }
}

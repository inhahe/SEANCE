// gain.c — Simple gain plugin for SoundShop2 WASM scripting
// Compile: clang --target=wasm32 -O2 -nostdlib -Wl,--no-entry -Wl,--export-all -o gain.wasm gain.c
#include "soundshop_wasm.h"

static int param_gain;

int32_t ss_init(void) {
    param_gain = ss_declare_param("Gain", 0.5f, 0.0f, 1.0f);
    return 0;
}

void ss_process(void) {
    float gain = ss_param_value(param_gain);
    uint32_t bs = SS_BLOCK_SIZE;
    float* inL  = ss_audio_in(0, 0);
    float* inR  = ss_audio_in(0, 1);
    float* outL = ss_audio_out(0, 0);
    float* outR = ss_audio_out(0, 1);

    for (uint32_t i = 0; i < bs; i++) {
        outL[i] = inL[i] * gain;
        outR[i] = inR[i] * gain;
    }
}

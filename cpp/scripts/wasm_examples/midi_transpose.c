// midi_transpose.c — Transpose incoming MIDI notes by a configurable amount
// Compile: clang --target=wasm32 -O2 -nostdlib -Wl,--no-entry -Wl,--export-all -o midi_transpose.wasm midi_transpose.c
#include "soundshop_wasm.h"

static int param_semitones;

int32_t ss_init(void) {
    param_semitones = ss_declare_param("Semitones", 0.0f, -24.0f, 24.0f);
    return 0;
}

// This script processes MIDI only (no audio)
int32_t ss_num_audio_inputs(void)  { return 0; }
int32_t ss_num_audio_outputs(void) { return 0; }

void ss_process(void) {
    int semitones = (int)ss_param_value(param_semitones);
    uint32_t count = SS_MIDI_IN_COUNT;
    ss_midi_event_t* events = ss_midi_in_events();

    for (uint32_t i = 0; i < count; i++) {
        uint8_t status = events[i].status;
        uint8_t d1 = events[i].data1;
        uint8_t d2 = events[i].data2;

        // Note on/off: transpose pitch
        if ((status & 0xF0) == 0x90 || (status & 0xF0) == 0x80) {
            int pitch = d1 + semitones;
            if (pitch < 0) pitch = 0;
            if (pitch > 127) pitch = 127;
            d1 = (uint8_t)pitch;
        }

        ss_midi_out(events[i].sample_offset, status, d1, d2);
    }
}

// soundshop_wasm.h — SoundShop2 WASM Script API
// Compile: clang --target=wasm32 -O2 -nostdlib -Wl,--no-entry -Wl,--export-all -o script.wasm script.c
//
// Scripts must export:
//   int32_t ss_init(void)    — declare params, return 0 on success
//   void    ss_process(void) — process one audio block
//
// Scripts may optionally export:
//   void    ss_prepare(void)              — called when sample rate / block size changes
//   int32_t ss_num_audio_inputs(void)     — default 1 (stereo pair)
//   int32_t ss_num_audio_outputs(void)    — default 1 (stereo pair)

#ifndef SOUNDSHOP_WASM_H
#define SOUNDSHOP_WASM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Host-provided imports (linked at runtime by the WASM host)
// ============================================================================

// Declare a parameter during ss_init(). Returns parameter index (0-based).
__attribute__((import_module("env"), import_name("ss_declare_param")))
int32_t ss_declare_param(const char* name, float default_val, float min_val, float max_val);

// Debug logging (no-op in release builds to stay real-time safe).
__attribute__((import_module("env"), import_name("ss_log")))
void ss_log(const char* msg);

// Read current value of a parameter.
__attribute__((import_module("env"), import_name("ss_get_param")))
float ss_get_param(int32_t index);

// Emit a MIDI event to the output buffer.
__attribute__((import_module("env"), import_name("ss_midi_out")))
void ss_midi_out(int32_t sample_offset, uint8_t status, uint8_t d1, uint8_t d2);

// ============================================================================
// Shared memory layout
// ============================================================================
// The host writes inputs into WASM linear memory before calling ss_process(),
// and reads outputs after it returns. All offsets are byte offsets from 0.

#define SS_HEADER       0x0000

// Header fields (256 bytes)
#define SS_MAGIC            (*(volatile uint32_t*)(SS_HEADER + 0x00)) // 0x57415343 "WASC"
#define SS_VERSION          (*(volatile uint32_t*)(SS_HEADER + 0x04)) // 1
#define SS_BLOCK_SIZE       (*(volatile uint32_t*)(SS_HEADER + 0x08))
#define SS_SAMPLE_RATE      (*(volatile float*)   (SS_HEADER + 0x0C))
#define SS_BPM              (*(volatile float*)   (SS_HEADER + 0x10))
#define SS_BEAT_POS         (*(volatile double*)  (SS_HEADER + 0x14))
#define SS_TRANSPORT_FLAGS  (*(volatile uint32_t*)(SS_HEADER + 0x1C))
#define SS_NUM_AUDIO_IN     (*(volatile uint32_t*)(SS_HEADER + 0x20))
#define SS_NUM_AUDIO_OUT    (*(volatile uint32_t*)(SS_HEADER + 0x24))
#define SS_NUM_PARAMS       (*(volatile uint32_t*)(SS_HEADER + 0x28))
#define SS_MIDI_IN_COUNT    (*(volatile uint32_t*)(SS_HEADER + 0x2C))
#define SS_MIDI_OUT_COUNT   (*(volatile uint32_t*)(SS_HEADER + 0x30))
#define SS_AUDIO_IN_OFF     (*(volatile uint32_t*)(SS_HEADER + 0x34))
#define SS_AUDIO_OUT_OFF    (*(volatile uint32_t*)(SS_HEADER + 0x38))
#define SS_PARAM_OFF        (*(volatile uint32_t*)(SS_HEADER + 0x3C))
#define SS_MIDI_IN_OFF      (*(volatile uint32_t*)(SS_HEADER + 0x40))
#define SS_MIDI_OUT_OFF     (*(volatile uint32_t*)(SS_HEADER + 0x44))
#define SS_LATENCY          (*(volatile uint32_t*)(SS_HEADER + 0x48))
#define SS_TAIL             (*(volatile uint32_t*)(SS_HEADER + 0x4C))

// Transport flag bits
#define SS_FLAG_PLAYING     0x01
#define SS_FLAG_RECORDING   0x02

#define SS_IS_PLAYING       (SS_TRANSPORT_FLAGS & SS_FLAG_PLAYING)
#define SS_IS_RECORDING     (SS_TRANSPORT_FLAGS & SS_FLAG_RECORDING)

// ============================================================================
// Audio helpers
// ============================================================================
// Audio is stored non-interleaved: L channel then R channel per stereo pair.
// Each channel is block_size floats.

static inline float* ss_audio_in(int pair, int channel) {
    return (float*)(uintptr_t)(SS_AUDIO_IN_OFF +
        (pair * 2 + channel) * SS_BLOCK_SIZE * sizeof(float));
}

static inline float* ss_audio_out(int pair, int channel) {
    return (float*)(uintptr_t)(SS_AUDIO_OUT_OFF +
        (pair * 2 + channel) * SS_BLOCK_SIZE * sizeof(float));
}

// ============================================================================
// Parameter helpers
// ============================================================================
// Each param is 16 bytes: [value, min, max, default] as f32.

static inline float ss_param_value(int idx) {
    float* p = (float*)(uintptr_t)(SS_PARAM_OFF + idx * 16);
    return p[0];
}

// ============================================================================
// MIDI helpers
// ============================================================================

typedef struct {
    uint32_t sample_offset;
    uint8_t  status;
    uint8_t  data1;
    uint8_t  data2;
    uint8_t  _reserved;
} ss_midi_event_t;

static inline ss_midi_event_t* ss_midi_in_events(void) {
    return (ss_midi_event_t*)(uintptr_t)SS_MIDI_IN_OFF;
}

// ============================================================================
// Latency / tail reporting
// ============================================================================

static inline void ss_set_latency(uint32_t samples) { SS_LATENCY = samples; }
static inline void ss_set_tail(uint32_t samples)    { SS_TAIL = samples; }

#ifdef __cplusplus
}
#endif
#endif // SOUNDSHOP_WASM_H

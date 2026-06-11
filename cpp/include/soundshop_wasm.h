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
//   int32_t ss_num_midi_outputs(void)     — default 1; >1 exposes "MIDI Out 1..N"
//                                           pins, each an independent cable.
//                                           Emit to a specific one with
//                                           ss_midi_out_n(out_index, ...).

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

// Frequency (Hz) of a MIDI note using the PROJECT tuning system (Equal
// Temperament / Pythagorean / Just Intonation / Meantone) and concert pitch —
// NOT a hardcoded 12-TET A440. This is the one note helper that needs the host,
// because only the host knows the project's tuning. Returns 0 for a note
// outside 0..127. (Name <-> number conversion is pure and lives in this header
// as ss_notenum / ss_notename below — no host round-trip needed.)
__attribute__((import_module("env"), import_name("ss_note_to_freq")))
float ss_note_to_freq(int32_t midinote);

// Emit a MIDI event to the output buffer (MIDI output 0).
__attribute__((import_module("env"), import_name("ss_midi_out")))
void ss_midi_out(int32_t sample_offset, uint8_t status, uint8_t d1, uint8_t d2);

// Emit a MIDI event to a specific MIDI output pin (0-based out_index). Only
// meaningful when the script exports ss_num_midi_outputs() > 1; out_index is
// clamped to the declared count. Each output is an independent MIDI cable in
// the graph — the host routes by re-stamping the event's channel internally,
// so do NOT rely on the channel nibble of `status` surviving when there is
// more than one output. With a single output, ss_midi_out (or out_index 0)
// passes the status byte through unchanged.
__attribute__((import_module("env"), import_name("ss_midi_out_n")))
void ss_midi_out_n(int32_t out_index, int32_t sample_offset,
                   uint8_t status, uint8_t d1, uint8_t d2);

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
// Note names <-> MIDI numbers (pure, host-independent)
// ============================================================================
// Octave convention matches the rest of SoundShop: C4 = MIDI 60, A4 = 69, so
// midi = (octave + 1) * 12 + pitch_class. These run entirely inside the module
// (no host call) because they don't depend on any project state — only
// ss_note_to_freq()/ss_notefreq() need the host, for the tuning.

// Parse a note-name string like "C4", "c#4", "Bb3" to a MIDI number. Letter
// A-G (case-insensitive), any run of '#'/'+' (sharp) or 'b' (flat) which may
// spill into the adjacent octave (so "B#4" = C5, "Cb4" = B3), optional signed
// octave (defaults to 4 when absent). Returns -1 on a parse error or a result
// outside 0..127, so callers can detect "not a valid note name".
static inline int32_t ss_notenum(const char* s) {
    if (!s) return -1;
    static const int8_t base[7] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
    const char* p = s;
    while (*p == ' ' || *p == '\t') ++p;
    char c = *p;
    if (c >= 'a' && c <= 'g') c = (char)(c - 'a' + 'A');
    if (c < 'A' || c > 'G') return -1;
    int semi = base[c - 'A'];
    ++p;
    for (;;) {
        if (*p == '#' || *p == '+') { semi += 1; ++p; }
        else if (*p == 'b' || *p == 'B') { semi -= 1; ++p; }
        else break;
    }
    int octave = 4;
    int neg = 0;
    if (*p == '-' || *p == '+') { neg = (*p == '-'); ++p; }
    const char* digits = p;
    int val = 0;
    while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); ++p; }
    if (p != digits) octave = neg ? -val : val;
    else if (neg) return -1; // lone '-' with no digits
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '\0') return -1; // trailing junk -> not a note name
    int midi = (octave + 1) * 12 + semi;
    if (midi < 0 || midi > 127) return -1;
    return midi;
}

// Format a MIDI number as a note name into `out` (needs >= 5 bytes), e.g.
// 60 -> "C4". Returns `out`. Out-of-range notes produce "?".
static inline char* ss_notename(int32_t midi, char* out) {
    static const char* names[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    if (!out) return out;
    if (midi < 0 || midi > 127) { out[0] = '?'; out[1] = '\0'; return out; }
    const char* nm = names[midi % 12];
    int octave = midi / 12 - 1;
    int i = 0;
    out[i++] = nm[0];
    if (nm[1]) out[i++] = nm[1];
    if (octave < 0) { out[i++] = '-'; octave = -octave; }
    if (octave >= 10) out[i++] = (char)('0' + octave / 10);
    out[i++] = (char)('0' + octave % 10);
    out[i] = '\0';
    return out;
}

// Frequency (Hz) of a note name like "C4" via the project tuning — convenience
// wrapper combining ss_notenum() with the host's ss_note_to_freq().
static inline float ss_notefreq(const char* name) {
    int32_t n = ss_notenum(name);
    return (n < 0) ? 0.0f : ss_note_to_freq(n);
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

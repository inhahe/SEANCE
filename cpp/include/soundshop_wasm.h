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
//   int32_t ss_num_midi_inputs(void)      — default 1; >1 exposes "MIDI In 1..N"
//                                           pins, each an independent cable. The
//                                           host merges them into the single
//                                           MIDI-input event buffer, tagging each
//                                           event with the 0-based pin it arrived
//                                           on in ss_midi_event_t.input_index.

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

// Read one of the shared FACTORY-WAVEFORM cycles — the exact same bank the
// Built-in / Lua / Python / GLSL formula languages expose as waveform(id, phase).
// `id` is the stable entry index (the integer shown in the factory-waveform
// browser; identical across every language). `phase` is normalised [0,1) and
// wraps; the cycle is read with linear interpolation. Returns the sample in
// [-1,1], or 0 for an out-of-range id. RT-safe (a plain table read host-side).
__attribute__((import_module("env"), import_name("ss_waveform")))
float ss_waveform(int32_t id, float phase);

// Resolve a factory-waveform NAME (case-insensitive, e.g. "AKWF oboe 0001") to
// its stable entry index for ss_waveform(), or -1 if there is no such waveform.
// Call this once in ss_init() and cache the result: the first lookup builds an
// internal name->index map host-side, so it is not guaranteed allocation-free.
__attribute__((import_module("env"), import_name("ss_waveform_id")))
int32_t ss_waveform_id(const char* name);

// ----------------------------------------------------------------------------
// Wavetable shape-bending ("warp") algorithms
// ----------------------------------------------------------------------------
// The same warps the Layered Wave editor and synth voice apply, exposed as pure
// scalar functions so an in-script clip/fold/bend matches the node bit-for-bit.
//   ss_warpamp(method, x, amount)     - amplitude-domain transfer on a sample
//                                       value x in [-1,1] (clip/fold/saturate/...)
//   ss_warpphase(method, phase, amount) - phase-domain remap of a read position
//                                       in [0,1) (bend/asym/PWM-skew/PD/...)
// `method` is an integer WarpMethod id - use the SS_WARP_* constants below, or
// ss_warp_method("name") to resolve a readable name once. `amount` is the 0..1
// morph knob (0 = identity). An unknown method id is treated as identity.
__attribute__((import_module("env"), import_name("ss_warpamp")))
float ss_warpamp(int32_t method, float x, float amount);
__attribute__((import_module("env"), import_name("ss_warpphase")))
float ss_warpphase(int32_t method, float phase, float amount);
__attribute__((import_module("env"), import_name("ss_warp_method")))
int32_t ss_warp_method(const char* name);

// Stable WarpMethod ids (mirror enum WarpMethod in cpp/src/warp.h; serialized, so
// these values never change). Amplitude-domain shapers feed ss_warpamp; phase-
// domain remaps feed ss_warpphase.
#define SS_WARP_NONE              0
#define SS_WARP_SOFTCLIP         1   // amplitude
#define SS_WARP_HARDCLIP         2
#define SS_WARP_WAVEFOLD         3
#define SS_WARP_WAVEWRAP         4
#define SS_WARP_RECTIFY          5
#define SS_WARP_QUANTIZE         6
#define SS_WARP_TUBESAT          7
#define SS_WARP_TAPESAT          8
#define SS_WARP_FLIP            10
#define SS_WARP_CHEBYSHEV       11
#define SS_WARP_BENDPLUS        20   // phase
#define SS_WARP_BENDMINUS       21
#define SS_WARP_ASYMPLUS        22
#define SS_WARP_ASYMMINUS       23
#define SS_WARP_PWMSKEW         24
#define SS_WARP_PHASEQUANTIZE   25
#define SS_WARP_PHASEDISTORTION 26
#define SS_WARP_VPS             27
#define SS_WARP_REMAP           28
#define SS_WARP_SELFSYNC        29

// ----------------------------------------------------------------------------
// Bucket C: representation-bound whole-buffer warps.
//   ss_spectralwarp(buf, len, method, amount)
//       - FFT the buffer, warp its per-bin MAGNITUDE envelope through the same
//         amplitude transfer as ss_warpamp (phase preserved), inverse FFT.
//   ss_waveletwarp(buf, len, method, amount, filter, levels)
//       - forward DWT, warp every coefficient, inverse DWT. `filter` is a
//         wavelet name ("db4", "db2", "sym4", "db1"/"haar"); pass 0/NULL for the
//         "db4" default. `levels` is the decomposition depth (e.g. 5).
// Both operate IN PLACE on `len` floats starting at `buf` and preserve length.
// These only make sense over a whole buffer (offline bake / block streaming), so
// there is no per-sample equivalent. `amount` is the 0..1 morph knob (0 =
// identity); an unknown method is identity. Not for the per-sample hot path -
// each call allocates a scratch buffer host-side.
__attribute__((import_module("env"), import_name("ss_spectralwarp")))
void ss_spectralwarp(float* buf, int32_t len, int32_t method, float amount);
__attribute__((import_module("env"), import_name("ss_waveletwarp")))
void ss_waveletwarp(float* buf, int32_t len, int32_t method, float amount,
                    const char* filter, int32_t levels);

// ============================================================================
// Whole-grid TERRAIN generation (the Terrain Synth "generate" feature)
// ============================================================================
// A module used to GENERATE a terrain is a different shape from an audio module:
// instead of ss_process(), it exports
//
//     void ss_generate(void);
//
// which the host calls ONCE (offline, on the message thread - never the audio
// thread) to fill an N-D grid. The grid is HOST-owned, not in your linear memory:
// you read/write cells by FLAT row-major index through the ss_grid_* imports
// below. A generator module still needs an ss_init() (it may be empty) but does
// NOT need ss_process(); an audio module is unchanged (ss_process, no
// ss_generate). The result is one value per cell in [0,1] (a grayscale height),
// baked and mapped to the terrain's bipolar [-1,1] as v*2-1, exactly like every
// other generation language.
//
// WASM is block-only, so terrain WASM is WHOLE-GRID ONLY (no per-cell mode): the
// whole program owns the array, which is what makes cross-cell work - blur,
// cellular automata, FFT, global normalisation - possible. These imports mirror
// the Lua whole-grid API (set/get/coord/coordAxis/neighbor) one-for-one.
//
// Minimal generator:
//     int32_t ss_init(void) { return 0; }
//     void ss_generate(void) {
//         int32_t total = ss_grid_total();
//         for (int32_t i = 0; i < total; ++i) {
//             float x = ss_grid_coord(i, 0) * 6.2831853f;
//             float y = ss_grid_coord(i, 1) * 6.2831853f;
//             ss_grid_set(i, 0.5f + 0.5f * sinf(x) * cosf(y));
//         }
//     }
__attribute__((import_module("env"), import_name("ss_grid_total")))
int32_t ss_grid_total(void);                       // total cell count = product(dims)
__attribute__((import_module("env"), import_name("ss_grid_nd")))
int32_t ss_grid_nd(void);                          // number of dimensions (rank)
__attribute__((import_module("env"), import_name("ss_grid_dim")))
int32_t ss_grid_dim(int32_t axis);                 // size of `axis` (0 if out of range)
__attribute__((import_module("env"), import_name("ss_grid_set")))
void ss_grid_set(int32_t i, float v);              // write flat cell i (v clamped [0,1])
__attribute__((import_module("env"), import_name("ss_grid_get")))
float ss_grid_get(int32_t i);                      // read flat cell i (0 outside range)
__attribute__((import_module("env"), import_name("ss_grid_coord")))
float ss_grid_coord(int32_t i, int32_t axis);      // normalised [0,1] position along axis
__attribute__((import_module("env"), import_name("ss_grid_coord_axis")))
int32_t ss_grid_coord_axis(int32_t i, int32_t axis); // INTEGER coord of cell i along axis
__attribute__((import_module("env"), import_name("ss_grid_neighbor")))
int32_t ss_grid_neighbor(int32_t i, int32_t axis, int32_t delta); // flat idx delta steps, edge-clamped

// Convenience N-D helpers (header-only; compose the imports above). `coords` is
// an array of `nd` integer per-axis coordinates. flatten/getat clamp each coord
// to [0,dim-1] (edge replicate); setat ignores an out-of-range write, matching
// the Lua flatten/getAt/setAt twins. Define SS_NO_GRID_HELPERS to omit.
#ifndef SS_NO_GRID_HELPERS
static inline int32_t ss_grid_flatten(const int32_t* coords, int32_t nd) {
    int32_t idx = 0;
    for (int32_t a = 0; a < nd; ++a) {
        int32_t sz = ss_grid_dim(a); if (sz < 1) sz = 1;
        int32_t c = coords[a]; if (c < 0) c = 0; else if (c > sz - 1) c = sz - 1;
        idx = idx * sz + c;                 // Horner form, last axis fastest
    }
    return idx;
}
static inline float ss_grid_getat(const int32_t* coords, int32_t nd) {
    return ss_grid_get(ss_grid_flatten(coords, nd));
}
static inline void ss_grid_setat(const int32_t* coords, int32_t nd, float v) {
    for (int32_t a = 0; a < nd; ++a) {
        int32_t sz = ss_grid_dim(a); if (sz < 1) sz = 1;
        if (coords[a] < 0 || coords[a] > sz - 1) return;   // OOB coord => no-op
    }
    ss_grid_set(ss_grid_flatten(coords, nd), v);
}
#endif // SS_NO_GRID_HELPERS

// ============================================================================
// GLSL-style shaping helpers (header-only, no libm)
// ============================================================================
// C's <math.h> has the transcendentals (sinf, cosf, expf, logf, tanhf, ...) but
// NOT GLSL's shaping builtins (mix, clamp, step, smoothstep, fract, mod, sign,
// the wave shapers). These inline versions give a WASM script the same shaping
// vocabulary the Built-in/Lua/Python/GLSL formula languages share, spelled with
// the `ss_` prefix to match the rest of this ABI. They use compiler builtins
// that lower to native WASM float ops, so they work even under -nostdlib.
//
// For the transcendentals, include <math.h> and link a wasm libm (or drop
// -nostdlib). `ss_mod` uses GLSL's *floored* semantics (a - b*floor(a/b)), which
// differ from C's fmodf for negative operands. Define SS_NO_SHAPING_HELPERS
// before including this header to omit this section.
#ifndef SS_NO_SHAPING_HELPERS
static inline float ss_fract(float x)            { return x - __builtin_floorf(x); }
static inline float ss_sign(float x)             { return (float)((x > 0.0f) - (x < 0.0f)); }
static inline float ss_mod(float a, float b)     { return b == 0.0f ? 0.0f : a - b * __builtin_floorf(a / b); }
static inline float ss_clamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float ss_min(float a, float b)     { return a < b ? a : b; }
static inline float ss_max(float a, float b)     { return a > b ? a : b; }
static inline float ss_mix(float a, float b, float t)     { return a + (b - a) * t; }
static inline float ss_step(float edge, float x)          { return x < edge ? 0.0f : 1.0f; }
static inline float ss_smoothstep(float e0, float e1, float x) {
    float t = e1 == e0 ? 0.0f : ss_clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
static inline float ss_radians(float deg)        { return deg * 0.01745329252f; }
static inline float ss_degrees(float rad)        { return rad * 57.2957795131f; }
// Bipolar wave shapers over a phase p in turns (1.0 = one cycle), matching the
// formula languages' saw/square/triangle. Range [-1,1].
static inline float ss_saw(float p)      { p -= __builtin_floorf(p); return 2.0f * p - 1.0f; }
static inline float ss_square(float p)   { p -= __builtin_floorf(p); return p < 0.5f ? 1.0f : -1.0f; }
static inline float ss_triangle(float p) { p -= __builtin_floorf(p); return p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p; }
// Polarity converters: -1..1 <-> 0..1 (audio cables carry bipolar, control 0..1).
static inline float ss_unipolar(float x) { return x * 0.5f + 0.5f; }
static inline float ss_bipolar(float x)  { return x * 2.0f - 1.0f; }
#endif // SS_NO_SHAPING_HELPERS

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
    // For MIDI-INPUT events this is the 0-based MIDI-input pin the event arrived
    // on (the same index the Lua stream pollmidi() exposes as its 1-based `idx`).
    // Today every node has a single MIDI input, so it is 0. For events the module
    // EMITS, the output pin is chosen by ss_midi_out_n(out_idx, ...) instead.
    uint8_t  input_index;
} ss_midi_event_t;

static inline ss_midi_event_t* ss_midi_in_events(void) {
    return (ss_midi_event_t*)(uintptr_t)SS_MIDI_IN_OFF;
}
// Number of MIDI-input events delivered to this block (read SS_MIDI_IN_COUNT).
static inline uint32_t ss_midi_in_count(void) { return SS_MIDI_IN_COUNT; }

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

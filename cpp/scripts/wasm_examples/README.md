# SoundShop2 WASM Script Nodes

Write real-time audio/MIDI effects in C, C++, Rust, AssemblyScript, Go, or Zig — any language that compiles to WebAssembly. Scripts run at audio sample rate on the audio thread as nodes in the signal graph.

## Quick Start

1. Write your script in any supported language
2. Compile to `.wasm`
3. In SoundShop2, right-click the graph > "WASM Script..." and pick the `.wasm` file
4. Connect it like any other node — audio in, audio out, MIDI, parameters

---

## Language Setup Guides

### C / C++

**Install:** [LLVM/Clang](https://releases.llvm.org/) version 11 or later. The `wasm32` target is built in.

**Windows:** Download from https://github.com/nicholasgasior/llvm-project/releases or install via `winget install LLVM.LLVM`

**Compile:**
```bash
clang --target=wasm32 -O2 -nostdlib \
      -Wl,--no-entry -Wl,--export-all \
      -I path/to/soundshop/include \
      -o myscript.wasm myscript.c
```

For C++:
```bash
clang++ --target=wasm32 -O2 -nostdlib -fno-exceptions -fno-rtti \
        -Wl,--no-entry -Wl,--export-all \
        -I path/to/soundshop/include \
        -o myscript.wasm myscript.cpp
```

**Include:** `#include "soundshop_wasm.h"` — provides all macros, types, and host function declarations.

**Notes:**
- No standard library available (`-nostdlib`). No `printf`, `malloc`, `math.h`.
- Write your own math functions or use the approximations in the examples.
- All memory is pre-allocated in WASM linear memory (256KB).

---

### Rust

**Install:** [rustup](https://rustup.rs/), then add the WASM target:
```bash
rustup target add wasm32-unknown-unknown
```

**Project setup:**
```bash
cargo init --lib my_effect
cd my_effect
```

Edit `Cargo.toml`:
```toml
[lib]
crate-type = ["cdylib"]

[profile.release]
opt-level = "z"     # optimize for size
lto = true
```

Edit `src/lib.rs`:
```rust
#![no_std]

// Host imports
extern "C" {
    fn ss_declare_param(name: *const u8, default: f32, min: f32, max: f32) -> i32;
    fn ss_get_param(index: i32) -> f32;
    fn ss_midi_out(sample_offset: i32, status: u8, d1: u8, d2: u8);
    fn ss_log(msg: *const u8);
}

// Shared memory layout (same offsets as soundshop_wasm.h)
const HEADER: *mut u8 = 0x0000 as *mut u8;

unsafe fn read_u32(off: usize) -> u32 { core::ptr::read_unaligned((HEADER.add(off)) as *const u32) }
unsafe fn read_f32(off: usize) -> f32 { core::ptr::read_unaligned((HEADER.add(off)) as *const f32) }

unsafe fn block_size() -> u32 { read_u32(0x08) }
unsafe fn sample_rate() -> f32 { read_f32(0x0C) }
unsafe fn audio_in_off() -> u32 { read_u32(0x34) }
unsafe fn audio_out_off() -> u32 { read_u32(0x38) }

static mut GAIN_IDX: i32 = 0;

#[no_mangle]
pub unsafe extern "C" fn ss_init() -> i32 {
    GAIN_IDX = ss_declare_param(b"Gain\0".as_ptr(), 0.5, 0.0, 1.0);
    0
}

#[no_mangle]
pub unsafe extern "C" fn ss_process() {
    let bs = block_size() as usize;
    let gain = ss_get_param(GAIN_IDX);
    let inp = audio_in_off() as *const f32;
    let out = audio_out_off() as *mut f32;
    for i in 0..bs {
        // Left channel
        *out.add(i) = *inp.add(i) * gain;
        // Right channel
        *out.add(bs + i) = *inp.add(bs + i) * gain;
    }
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! { loop {} }
```

**Compile:**
```bash
cargo build --target wasm32-unknown-unknown --release
```

Output: `target/wasm32-unknown-unknown/release/my_effect.wasm`

---

### AssemblyScript (TypeScript-like)

**Install:** [Node.js](https://nodejs.org/), then:
```bash
npm install -g assemblyscript
```

**Project setup:**
```bash
mkdir my_effect && cd my_effect
npx asinit .
```

Edit `assembly/index.ts`:
```typescript
// Host imports
@external("env", "ss_declare_param")
declare function ss_declare_param(name: usize, def: f32, min: f32, max: f32): i32;

@external("env", "ss_get_param")
declare function ss_get_param(index: i32): f32;

@external("env", "ss_midi_out")
declare function ss_midi_out(offset: i32, status: u8, d1: u8, d2: u8): void;

// Shared memory offsets
const H_BLOCK_SIZE: usize = 0x08;
const H_SAMPLE_RATE: usize = 0x0C;
const H_AUDIO_IN_OFF: usize = 0x34;
const H_AUDIO_OUT_OFF: usize = 0x38;

let gainIdx: i32 = 0;

export function ss_init(): i32 {
    // "Gain" as null-terminated string in memory
    const name = memory.data(4);
    store<u32>(name, 0x6E696147); // "Gain"
    store<u8>(name + 4, 0);
    gainIdx = ss_declare_param(name, 0.5, 0.0, 1.0);
    return 0;
}

export function ss_process(): void {
    const bs = load<u32>(H_BLOCK_SIZE);
    const gain = ss_get_param(gainIdx);
    const inOff = load<u32>(H_AUDIO_IN_OFF);
    const outOff = load<u32>(H_AUDIO_OUT_OFF);

    for (let i: u32 = 0; i < bs; i++) {
        // Left
        const inL = load<f32>(inOff + i * 4);
        store<f32>(outOff + i * 4, inL * gain);
        // Right
        const inR = load<f32>(inOff + (bs + i) * 4);
        store<f32>(outOff + (bs + i) * 4, inR * gain);
    }
}
```

**Compile:**
```bash
npx asc assembly/index.ts -O3 --exportRuntime -o build/my_effect.wasm
```

---

### Go

**Install:** [Go](https://go.dev/) 1.21+ (has built-in WASM support, but targets `wasm_exec.js` by default). For bare WASM without a JS runtime, use [TinyGo](https://tinygo.org/):

```bash
# Install TinyGo: https://tinygo.org/getting-started/install/
tinygo version
```

**Write `main.go`:**
```go
package main

// Host imports via //go:wasmimport
//go:wasmimport env ss_declare_param
func ssDeclareParam(name *byte, def, min, max float32) int32

//go:wasmimport env ss_get_param
func ssGetParam(index int32) float32

var gainIdx int32

//export ss_init
func ssInit() int32 {
    name := []byte("Gain\x00")
    gainIdx = ssDeclareParam(&name[0], 0.5, 0.0, 1.0)
    return 0
}

//export ss_process
func ssProcess() {
    // Read from shared memory at known offsets
    // (requires unsafe pointer arithmetic)
}

func main() {}
```

**Compile:**
```bash
tinygo build -o my_effect.wasm -target wasm -no-debug main.go
```

**Note:** Go/TinyGo WASM support for bare-metal (no JS) is less mature. C or Rust are recommended for complex DSP.

---

### Zig

**Install:** [Zig](https://ziglang.org/download/) 0.11+

**Write `effect.zig`:**
```zig
const std = @import("std");

extern "env" fn ss_declare_param(name: [*]const u8, def: f32, min: f32, max: f32) i32;
extern "env" fn ss_get_param(index: i32) f32;

var gain_idx: i32 = 0;

export fn ss_init() i32 {
    gain_idx = ss_declare_param("Gain", 0.5, 0.0, 1.0);
    return 0;
}

export fn ss_process() void {
    const bs = @as(*const u32, @ptrFromInt(0x08)).*;
    const gain = ss_get_param(gain_idx);
    const in_off = @as(*const u32, @ptrFromInt(0x34)).*;
    const out_off = @as(*const u32, @ptrFromInt(0x38)).*;

    var i: u32 = 0;
    while (i < bs) : (i += 1) {
        const in_ptr = @as(*const f32, @ptrFromInt(in_off + i * 4));
        const out_ptr = @as(*f32, @ptrFromInt(out_off + i * 4));
        out_ptr.* = in_ptr.* * gain;
    }
}
```

**Compile:**
```bash
zig build-lib effect.zig -target wasm32-freestanding -O ReleaseFast
```

---

### JavaScript

JavaScript doesn't compile directly to WASM, but there are two options:

**Option A: AssemblyScript (recommended)** — see the AssemblyScript section above. It's TypeScript syntax that compiles to efficient WASM. This is the best path for JS developers.

**Option B: Javy** — [Javy](https://github.com/nicholasgasior/nicholasgasior-nicholasgasior-nicholasgasior-nicholasgasior-nicholasgasior-javy) by Bytecode Alliance embeds a JS engine (QuickJS) into WASM. It works but has significant overhead — the entire JS runtime is bundled into the `.wasm` file (~500KB+), and performance is much slower than native WASM.

**Install Javy:**
```bash
# Download from https://github.com/nicholasgasior/nicholasgasior-nicholasgasior-javy/releases
javy compile my_effect.js -o my_effect.wasm
```

**Write `my_effect.js`:**
```javascript
// Note: Javy provides a different API than our direct WASM interface.
// You'd need to read/write shared memory manually via ArrayBuffer views.
// This is awkward — AssemblyScript is strongly recommended instead.

// Javy scripts communicate via stdin/stdout by default,
// which doesn't map well to our real-time audio API.
```

**Recommendation:** Use AssemblyScript for JS/TS developers. It compiles to clean, fast WASM with full access to our shared memory API. Javy adds too much overhead for real-time audio.

---

### Lua

Lua doesn't have a mainstream WASM compiler, but there are two approaches:

**Option A: Compile Lua interpreter to WASM** — embed a Lua VM (like [Wasm-Lua](https://nicholasgasior.github.io/nicholasgasior-nicholasgasior-nicholasgasior-nicholasgasior-nicholasgasior-wasm-lua)) that runs Lua scripts inside WASM. Adds overhead but works.

**Option B: Use Fengari or Wasmoon** — these are Lua-in-WASM runtimes, but they're designed for browser use, not bare WASM embedding.

**Practical approach:** Write a C wrapper that embeds a minimal Lua interpreter and exposes our API:

**`lua_wrapper.c`:**
```c
#include "soundshop_wasm.h"
// Embed a minimal Lua 5.4 compiled to WASM alongside your script.
// The C wrapper calls ss_init/ss_process which invoke Lua functions.
// This requires compiling Lua's source (about 30 .c files) to wasm32.
```

**Compile Lua + wrapper:**
```bash
# Compile all Lua source files to WASM objects
clang --target=wasm32 -O2 -c lua/*.c
# Compile your wrapper
clang --target=wasm32 -O2 -c lua_wrapper.c
# Link together
wasm-ld --no-entry --export-all *.o -o lua_effect.wasm
```

**Recommendation:** Lua-in-WASM works but adds complexity and ~100KB overhead. For simple scripts, writing directly in C is easier. For complex logic where you prefer Lua syntax, the embedded approach works. Consider whether Python scripting (which runs on the UI thread, not audio thread) covers your use case instead.

---

### Language Comparison

| Language | Install Complexity | WASM Size | Performance | Ease of Use |
|----------|-------------------|-----------|-------------|-------------|
| **C** | Low (just clang) | Tiny (~1KB) | Best | Medium |
| **Rust** | Medium (rustup) | Small (~5KB) | Best | Medium |
| **Zig** | Low (single binary) | Tiny (~1KB) | Best | Medium |
| **AssemblyScript** | Low (npm) | Small (~5KB) | Good | Easy (TS syntax) |
| **Go (TinyGo)** | Medium | Medium (~50KB) | Good | Easy |
| **Lua (embedded)** | High | Large (~100KB) | Fair | Easy (Lua syntax) |
| **JavaScript (Javy)** | Medium | Large (~500KB) | Poor | Easy (JS syntax) |

**For audio DSP:** C, Rust, or Zig give the best performance with minimal overhead.
**For JS/TS developers:** AssemblyScript is the clear choice.

**Note:** SoundShop also has Python scripting (Scripts menu) for project manipulation, batch operations, and analysis. Python runs on the UI thread at 30Hz — it can't process audio in real time but is great for adding notes, automating parameters, and other non-audio-rate tasks. WASM is specifically for real-time audio/MIDI processing at sample rate.

---

## SoundShop WASM API Reference

### Required Exports (your script must provide)

| Function | Signature | Description |
|----------|-----------|-------------|
| `ss_init` | `() -> i32` | Called once on load. Declare parameters. Return 0 = success. |
| `ss_process` | `() -> void` | Called every audio block. Read inputs, write outputs. |

### Optional Exports

| Function | Signature | Default | Description |
|----------|-----------|---------|-------------|
| `ss_prepare` | `() -> void` | — | Called when sample rate or buffer size changes. |
| `ss_num_audio_inputs` | `() -> i32` | 1 | Number of stereo input pairs. Return 0 for MIDI-only. |
| `ss_num_audio_outputs` | `() -> i32` | 1 | Number of stereo output pairs. |

### Host Functions (callable from your script)

| Function | Signature | Description |
|----------|-----------|-------------|
| `ss_declare_param` | `(name: *u8, def: f32, min: f32, max: f32) -> i32` | Declare a parameter. Call only during `ss_init()`. Returns param index. |
| `ss_get_param` | `(index: i32) -> f32` | Read current parameter value. |
| `ss_midi_out` | `(sample_offset: i32, status: u8, d1: u8, d2: u8) -> void` | Emit a MIDI event. |
| `ss_log` | `(msg: *u8) -> void` | Debug print (no-op in release). |

All host functions are imported from module `"env"`.

### Shared Memory Layout

All data exchange happens through WASM linear memory at fixed offsets:

```
HEADER (256 bytes at offset 0x0000)
  0x00  u32   magic (0x57415343 = "WASC")
  0x04  u32   version (1)
  0x08  u32   block_size (samples per block, e.g. 480)
  0x0C  f32   sample_rate (e.g. 48000.0)
  0x10  f32   bpm
  0x14  f64   beat_position
  0x1C  u32   transport_flags (bit 0 = playing, bit 1 = recording)
  0x20  u32   num_audio_inputs
  0x24  u32   num_audio_outputs
  0x28  u32   num_params
  0x2C  u32   midi_in_count
  0x30  u32   midi_out_count (write this)
  0x34  u32   audio_in_offset (byte offset to audio input data)
  0x38  u32   audio_out_offset (byte offset to audio output data)
  0x3C  u32   param_offset (byte offset to parameter block)
  0x40  u32   midi_in_offset
  0x44  u32   midi_out_offset
  0x48  u32   reported_latency (write this, in samples)
  0x4C  u32   reported_tail (write this, in samples)

PARAMETERS (at param_offset, 16 bytes each, max 32)
  +0x00  f32  current value
  +0x04  f32  min value
  +0x08  f32  max value
  +0x0C  f32  default value

AUDIO (at audio_in_offset / audio_out_offset)
  Non-interleaved: L channel (block_size floats), then R channel.
  Per stereo pair: block_size * 4 * 2 bytes.

MIDI EVENTS (at midi_in_offset / midi_out_offset, 8 bytes each, max 256)
  +0x00  u32  sample_offset (within block)
  +0x04  u8   status byte
  +0x05  u8   data1
  +0x06  u8   data2
  +0x07  u8   reserved
```

### Constraints

- **No allocations** in `ss_process()`. All memory is pre-allocated.
- **No file I/O, no syscalls.** This runs on the real-time audio thread.
- **No `memory.grow`.** The host pre-allocates 256KB.
- **No standard library** in C (use `-nostdlib`). Write your own math or use examples.
- **Max limits:** 32 parameters, 8 stereo I/O pairs, 256 MIDI events per block.
- **Deterministic timing required.** No GC, no JIT warmup, no unbounded loops.

### Tips

- **Math without libm:** See `tremolo.c` for a `sin()` approximation. For `exp`, `log`, `pow`, `sqrt`, write Taylor series approximations or lookup tables.
- **State between blocks:** Use `static` variables (C) or `static mut` (Rust). WASM linear memory persists between calls.
- **Debugging:** Use `ss_log("message")` during development. It prints to the DAW console.
- **Performance:** A 480-sample block at 48kHz gives you ~10ms. Simple DSP loops complete in <0.1ms. You have plenty of headroom.

---

## C Examples

### gain.c — Simple gain

```c
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
```

### midi_transpose.c — MIDI-only transpose

```c
#include "soundshop_wasm.h"

static int param_semi;

int32_t ss_init(void) {
    param_semi = ss_declare_param("Semitones", 0.0f, -24.0f, 24.0f);
    return 0;
}

int32_t ss_num_audio_inputs(void)  { return 0; }
int32_t ss_num_audio_outputs(void) { return 0; }

void ss_process(void) {
    int semi = (int)ss_param_value(param_semi);
    uint32_t count = SS_MIDI_IN_COUNT;
    ss_midi_event_t* ev = ss_midi_in_events();
    for (uint32_t i = 0; i < count; i++) {
        uint8_t st = ev[i].status, d1 = ev[i].data1, d2 = ev[i].data2;
        if ((st & 0xF0) == 0x90 || (st & 0xF0) == 0x80) {
            int p = d1 + semi;
            d1 = (uint8_t)(p < 0 ? 0 : p > 127 ? 127 : p);
        }
        ss_midi_out(ev[i].sample_offset, st, d1, d2);
    }
}
```

### tremolo.c — Beat-synced tremolo

```c
#include "soundshop_wasm.h"

static float fsin(float x) {
    while (x >  3.14159265f) x -= 6.28318530f;
    while (x < -3.14159265f) x += 6.28318530f;
    return (16.0f * x * (3.14159265f - x)) /
           (5.0f * 3.14159265f * 3.14159265f -
            4.0f * x * (3.14159265f - x));
}

static int p_depth, p_rate;

int32_t ss_init(void) {
    p_depth = ss_declare_param("Depth", 0.5f, 0.0f, 1.0f);
    p_rate  = ss_declare_param("Rate (beats)", 1.0f, 0.25f, 8.0f);
    return 0;
}

void ss_process(void) {
    float depth = ss_param_value(p_depth);
    float rate  = ss_param_value(p_rate);
    uint32_t bs = SS_BLOCK_SIZE;
    float bps   = SS_BPM / (60.0f * SS_SAMPLE_RATE);
    double beat = SS_BEAT_POS;
    float* iL = ss_audio_in(0,0), *iR = ss_audio_in(0,1);
    float* oL = ss_audio_out(0,0), *oR = ss_audio_out(0,1);
    for (uint32_t i = 0; i < bs; i++) {
        float b = (float)beat + i * bps;
        float lfo = 0.5f + 0.5f * fsin(b / rate * 6.2831853f);
        float g = 1.0f - depth * (1.0f - lfo);
        oL[i] = iL[i] * g;
        oR[i] = iR[i] * g;
    }
}
```

### lowpass.c — Simple one-pole low-pass filter

```c
#include "soundshop_wasm.h"

static int p_cutoff;
static float prevL = 0, prevR = 0;

int32_t ss_init(void) {
    p_cutoff = ss_declare_param("Cutoff", 0.5f, 0.0f, 1.0f);
    return 0;
}

void ss_process(void) {
    float cutoff = ss_param_value(p_cutoff);
    // Map 0-1 to coefficient (higher = more treble passes through)
    float alpha = cutoff * cutoff; // quadratic for more musical feel
    uint32_t bs = SS_BLOCK_SIZE;
    float* inL  = ss_audio_in(0, 0);
    float* inR  = ss_audio_in(0, 1);
    float* outL = ss_audio_out(0, 0);
    float* outR = ss_audio_out(0, 1);
    for (uint32_t i = 0; i < bs; i++) {
        prevL += alpha * (inL[i] - prevL);
        prevR += alpha * (inR[i] - prevR);
        outL[i] = prevL;
        outR[i] = prevR;
    }
}
```

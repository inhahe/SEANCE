# Scripting Languages × Execution Contexts — Rationale Reference

A cheat-sheet for *which* scripting language to reach for in *which* part of SEANCE,
and **why each combination is allowed or forbidden**. This exists because the
allowed combinations are not arbitrary — they fall out of one hard constraint
(the audio thread must never block, allocate, or call an interpreter it can't
bound) and a few capability facts about each language.

If you only remember one thing: **there are two worlds.**

| World | When it runs | What's allowed | Why |
|---|---|---|---|
| **Real-time** (Script node) | On the audio thread, every block forever | Builtin, Lua, WASM | Must be bounded, allocation-light, deterministic. No Python, no GLSL. |
| **Offline / bake** (terrain, wavetable, ADHSR/spectral curves) | Once, on the UI thread, result frozen into the project | Builtin, Lua, Python, GLSL | Latency doesn't matter; result is a frozen lookup table. Anything goes. |

Everything below is just the consequences of that split.

---

## The five languages at a glance

| Language | Where it lives | Real-time safe? | Why you'd pick it |
|---|---|---|---|
| **Builtin** (the custom expression language) | Script node, terrain, curves | **Yes** — allocation-free expression walker, no GC | Zero-dependency, guaranteed-safe one-liners. The default. |
| **Lua** (5.4, embedded) | Script node, terrain, curves | **Per-block: yes. Per-sample: risky** (GC/alloc) | Full language — loops, tables, state, coroutines — when a one-liner isn't enough. |
| **WASM** (wasm3, user-supplied `.wasm`) | Script node only | **Yes** — compiled, deterministic, persistent linear memory | Heavy DSP at native-ish speed, many instances, no GC. |
| **Python** (CPython, optional DLL) | **Offline bake only** (terrain, curves) | **No** — never on the audio thread | numpy/scipy-class algorithms to *generate* tables offline. |
| **GLSL** (headless GL 4.3 compute) | **Offline bake only** (terrain grids, wavetable/curve formulas) | **No** — GPU, UI thread | Massively-parallel grid compute (convolution, CA, diffusion); the most capable waveshaping language for curves. |

Build-time / runtime availability:
- **Builtin** — always present.
- **Lua** — compiled in when `HAS_LUA` is set (it is, by default). Greys out in the language picker if absent.
- **WASM** — needs wasm3; greys out if absent.
- **Python** — optional `find_package(Python3)`; DLL is delay-loaded and *runtime*-probed (`ScriptEngine::pythonAvailable()`). If the DLL isn't there, the Python option simply disappears — the app still runs.
- **GLSL** — needs a GL 4.3 driver; probed by `glslComputeAvailable()`. Greys out otherwise.

---

## The full matrix

Rows are the languages you named; columns are the execution contexts you named.
`✓` = supported, `—` = not supported / not applicable. The "why" for every cell
is expanded in the sections below.

| | Expression mode¹ | Sample-rate mode² (per-sample) | Block mode (per-block) | Program-loop mode (streaming) | Terrain synthesis | Wavetable / ADHSR curves |
|---|:--:|:--:|:--:|:--:|:--:|:--:|
| **Builtin (custom)** | ✓ *(this **is** expression mode)* | ✓ *(the only per-sample language)* | — | — | ✓ *(per-cell only)* | ✓ |
| **Lua** | — *(use Builtin for one-liners)* | ✓ *(with a stutter warning)* | ✓ | ✓ *(`stream()`)* | ✓ *(per-cell + whole-grid)* | ✓ |
| **WASM** | — | —³ *(but already sample-accurate — see below)* | ✓ | ✓ *(morally — module owns its loop)* | — | — |
| **Python** | — | — | — | — | ✓ *(per-cell + whole-grid, offline)* | ✓ *(offline)* |
| **GLSL** | — | — | — | — | ✓ *(per-cell + whole-grid, GPU)* | ✓ *(per-sample bake, GPU)* |

¹ **Expression mode is not a separate language** — it's the Builtin language running its single-expression-per-sample evaluator. It's listed as its own column because that's how you think about it in the UI ("just type a formula"), but mechanically it's the Builtin × per-sample cell.

² **Sample-rate mode and expression mode overlap for Builtin.** Builtin *only* runs per-sample, so "expression mode" and "Builtin in sample-rate mode" are the same thing. The distinction matters once you add Lua, which can *also* run per-sample but is a full program, not a single expression.

³ **WASM's "—" here is about the *calling convention*, not the *signal*.** This is the point that trips everyone up, so it gets its own section ([WASM and "sample-rate": resolving the apparent contradiction](#wasm-and-sample-rate-resolving-the-apparent-contradiction)). Short version: a WASM module already produces a fully sample-accurate signal and can react per-sample — it just does that *inside* its own block loop rather than by being re-entered once per sample. There is no capability missing; the per-sample/per-block toggle simply doesn't apply to a program that owns its loop.

Enforced in code by `scriptLangSupportsRate()` (`script_runtime.cpp:49-56`):
```
Builtin -> PerSample only
Lua     -> PerSample and PerBlock
WASM    -> PerBlock only
```
The editor's **Run** dropdown (Per sample / Per block) is greyed out and forced
for Builtin (→ Per sample) and WASM (→ Per block); only Lua leaves it live.

---

## Real-time contexts (the Script node)

These four "modes" are all the *same* Script node — they differ in how often
your code runs and who owns the loop. Pick the cheapest one that can express
your idea.

### Expression mode — Builtin, per-sample

**What it is:** one formula, re-evaluated every sample, returning one number
(or, in MIDI role, calling `note()`/`cc()`/`bend()`). Persistent variables and
`init:`/`start:`/`loop:` sections are available, but no loops or tables.

| Language | Verdict | Rationale |
|---|---|---|
| **Builtin** | ✅ **Use it** | This is what expression mode *is*. Allocation-free, no GC, safe to run 44.1k×/sec. The right default for LFOs, envelopes, simple CC automation, four-on-the-floor kicks. |
| **Lua** | 🚫 wrong tool | Lua *can* run per-sample, but if all you have is a one-liner, Builtin is strictly cheaper and safer. Reach for Lua only when you outgrow a single expression. |
| **WASM / Python / GLSL** | ⛔ N/A | WASM doesn't use the per-sample *invocation* mode (it owns its own loop — see the [WASM section](#wasm-and-sample-rate-resolving-the-apparent-contradiction)). Python/GLSL never run in the audio path. |

### Sample-rate mode (per-sample) — Builtin or Lua

**What it is:** the *host* owns the loop and calls your code **once per audio
sample**, and you return/process one sample per call. Sample-accurate by nature.
(Note the emphasis on *invocation* — this is a calling convention, not the only
way to get a sample-accurate signal; see the WASM section.)

| Language | Verdict | Rationale |
|---|---|---|
| **Builtin** | ✅ Ideal | Bounded, allocation-free. The reason per-sample is even safe. |
| **Lua** | ⚠️ Allowed, with a warning | The interpreter is called 44,100+ times/second. A script that builds tables/strings each sample generates garbage → GC → **audible stutter**. The editor shows an inline ⚠ warning when Lua + per-sample is selected and suggests Per-block. Use it only for genuinely light per-sample logic that needs more than a formula. |
| **WASM** | ➖ Toggle N/A (not a limitation) | WASM isn't *re-entered* once per sample — but it doesn't need to be. Its `ss_process()` already loops over every sample inside one block call, so the output is fully sample-accurate and it can react per-sample. Forcing per-sample *invocation* would be the identical computation with 512× the call-boundary overhead, so the Run dropdown is simply fixed to Per block. See the dedicated section below. |
| **Python / GLSL** | ⛔ Never | Not in the audio path. |

### Block mode (per-block) — Lua or WASM

**What it is:** your code runs once per audio block (e.g. once per 512 samples)
and fills the whole output buffer / emits all MIDI for the block, stamping each
event with a sample offset for accuracy.

| Language | Verdict | Rationale |
|---|---|---|
| **Builtin** | 🚫 Not supported | Builtin is a single-expression evaluator; it has no notion of "fill a buffer." Forced to per-sample. |
| **Lua** | ✅ The scalable choice | One interpreter call per block instead of per sample → no per-sample GC pressure. Still sample-accurate if you write `for i=0,n-1 do out(i, f(i)) end`. Globals persist across blocks for free. Event-driven MIDI via `midiin()` / `midievent(k)`. **This is where heavy Lua belongs.** |
| **WASM** | ✅ Native fit | WASM's `ss_process()` *is* a per-block function. Compiled speed, deterministic, linear memory persists between blocks. Best for high instance counts or tight DSP. |
| **Python / GLSL** | ⛔ Never | Not in the audio path. |

### Program-loop mode (streaming) — Lua `stream()`, or WASM

**What it is:** *your program owns the loop.* Instead of being called per
sample/block, you write an infinite loop that `pull()`s input and `out()`s
output; the host suspends/resumes you at block boundaries. Cross-block state is
just ordinary locals — a filter's running value, a delay line, a phase counter —
no manual global bookkeeping.

| Language | Verdict | Rationale |
|---|---|---|
| **Builtin** | 🚫 Can't | **The language has no loop constructs at all** — no `for`, no `while`, no coroutines. It is a pure *expression* evaluator: the host re-invokes it once per sample and it returns one value. A program can't "own the loop" because there is no way to *write* a loop in it (the `init:`/`start:`/`loop:` section names are host-invocation phases, not iteration — `loop:` is "the body run each sample," not a loop you control). Owning the sample loop fundamentally requires a language with iteration and persistent local state across iterations, which is exactly what Lua coroutines / a compiled WASM module provide and Builtin does not. |
| **Lua** | ✅ The clean way to do stateful DSP | Define `function stream()` instead of `loop()`. The coroutine is created off the audio thread once, then resumed each block. Inherently sample-accurate, so the **Run dropdown is ignored**. `pull()`/`poll()` for one sample, `pullblock()`/`pollblock()` for a block, `pollmidi()` for event-driven MIDI *inside* the loop. Ideal for filters, delays, anything where carrying state across blocks via globals would be ugly. |
| **WASM** | ✅ Morally streaming already | A WASM module *is* a loop-owning program: `ss_process()` fills the block, linear memory persists, the Run dropdown is ignored. Same shape as Lua streaming, just compiled. |
| **Python / GLSL** | ⛔ Never | Not in the audio path. |

**Lua streaming vs WASM:** pick Lua when you want to iterate quickly in-editor
with full language ergonomics and the perf is fine; pick WASM when you need
native speed, many instances, or you already have C/Rust/Zig DSP to drop in.

### WASM and "sample-rate": resolving the apparent contradiction

This is the one that reliably causes confusion, because the word "per-sample"
means two different things and they get conflated. Untangling them:

**Two different "per-sample"s:**

1. **Per-sample *resolution* (the signal):** does the output change every
   sample? Can the code react to each incoming sample / MIDI event at the exact
   sample it happens?
2. **Per-sample *invocation* (the calling convention):** does the *host* enter
   and exit your code once per sample — 44,100 separate calls per second?

The "Run: Per sample / Per block" dropdown is about **#2, the invocation**. It
is *not* a knob for signal resolution.

**What WASM actually does:** a WASM module exports `ss_process()`, which the host
calls **once per block**, and inside that one call the module runs *its own loop
over every sample in the block*. So:

- Its output is **fully sample-accurate** (#1 ✓) — it writes every sample, can
  hold per-sample state, and stamps MIDI events at their exact sample offset.
- It is **not re-entered per sample** (#2 ✗) — and that's a feature, not a limit.

So when I said earlier "WASM works in sample rate," I meant **#1**: a WASM module
does genuine sample-by-sample work and produces a sample-accurate signal. When
the doc says "WASM is per-block only," it means **#2**: the host calls it once
per block, so the per-sample/per-block *toggle* doesn't apply. **Both are true.**
They're describing different axes. My earlier phrasing blurred them — apologies.

**"But you agreed sample-rate and block-rate signals are basically the same at
different rates — so if it's fast enough, WASM should do per-sample too."** The
key realization: there's **nothing to add**, because per-sample *invocation*
would produce the *exact same signal* as what WASM already does, just slower.
Calling `ss_process()` with `blockSize = 1`, 44,100 times a second, computes
bit-for-bit the same output as calling it once with `blockSize = 512` — but pays
the host↔WASM call-boundary cost 512× as often for zero benefit. The module's
internal loop *is* the per-sample loop; moving that loop out into the host buys
nothing and costs overhead. So "make WASM work in sample-rate mode" isn't a
withheld capability — it's a strictly-worse way to get the identical result, and
that's why the toggle is fixed rather than offered.

**The clean mental model — there are only two kinds of real-time program:**

| Kind | Who owns the per-sample loop | Languages | The Run dropdown |
|---|---|---|---|
| **Host-driven** | The host. You're a callback. | Builtin, Lua-`loop()` | Meaningful: per-sample vs per-block invocation is a real choice (simplicity vs cost). |
| **Self-driven** | Your program. You contain the loop. | Lua-`stream()`, **WASM** | Ignored: you already do per-sample work inside one resume/call. There's no toggle to make. |

WASM and Lua `stream()` are the **same category** — both own their loop, both are
sample-accurate, both ignore the Run dropdown. WASM isn't a limited version of a
per-sample language; it's a self-driven one, exactly like streaming Lua.

(Aside: per-sample *host* invocation also wouldn't unlock cross-*node* sample
feedback in this engine, because the JUCE `AudioProcessorGraph` processes the
whole graph a block at a time regardless — even a "per-sample" Builtin node is
per-sample only *within* its own block. So nothing about the graph's behaviour
changes either. Sample-accurate feedback inside a single WASM module already
works, because the module owns its loop.)

### Self-driven, two ways: Lua `stream()` *yields*, WASM *returns*

The table above lumps Lua `stream()` and WASM into one "self-driven" bucket, and
for picking a language that's the right level of detail. But they reach the same
place by **two different mechanisms**, and the difference explains a question
that otherwise looks like a contradiction: *if WASM can't suspend/yield, why
isn't the audio thread stuck waiting on it at block rate too?*

The trap is treating "hand control back to the host" as one operation. It's two:

- **Return** — the function *finishes* and exits. Its call stack is destroyed.
  The host gets control because the call **ended**. To do more work later, the
  host calls the function **again from the top**, and any state that must survive
  has to live *outside* the call stack.
- **Yield / suspend** — the function *pauses in the middle*, keeps its entire
  call stack **frozen and alive**, hands control back, and is later **resumed
  from that exact instruction**.

WASM (under wasm3) can **return** but cannot **yield** — a WASM call runs to
completion on one stack; the MVP has no coroutines, continuations, or stack
switching. Lua **can** yield, because Lua coroutines suspend across the C
boundary.

Now the two streaming styles fall out cleanly:

| | Lua `stream()` | WASM |
|---|---|---|
| Form you write | one *never-returning* loop spanning all blocks | a per-block function that fills the buffer and **returns** |
| Block boundary | **yields** — coroutine frozen mid-loop, resumed next block | **returns** — call ends; re-entered from the top next block |
| Cross-block state lives in | the suspended stack (ordinary `local`s) | **persistent linear memory** (survives the return; module never re-instantiated) |
| Needs suspension? | **yes** (that's what the coroutine is for) | **no** |
| Sample-accurate? | yes | yes |
| Signal produced | — identical — | — identical — |

So **"WASM can't yield" never bites at block rate, because the block model is
built on *return*, not yield.** The host calls `ss_process()`, WASM does the
whole block's work and *finishes*, the host regains control because the call
*completed* — nobody is blocked, nothing is paused mid-flight. Cross-block
continuity doesn't need a frozen stack; it comes from linear memory plus
re-calling from the top.

Yield is only required for the one thing WASM *can't* express: a **single
continuous loop that spans block boundaries** (the `while true do … pull() … end`
form). And that's pure **syntactic sugar** — it produces the identical signal to
a per-block fill function, just spelled as one loop instead of a re-entered
function. You *could* give WASM that form via Asyncify (rewrites the binary to
unwind/rewind its own stack), a dedicated OS thread with a real blocking
primitive, or the stack-switching proposal — but all three add real cost
(code-size/runtime overhead, thread-per-node sync, runtime support wasm3 lacks)
to buy *only* the loop syntax. Persistent linear memory already delivers the
actual capability, so the per-block ABI is the right call.

The same return-vs-yield split also re-frames "WASM can't run at sample rate"
precisely: WASM **could** be invoked per-sample in *return* style (call
`ss_process()` with `blockSize = 1`, return after each sample) — that's possible,
just strictly wasteful (identical signal, 512× the call overhead). What WASM
**can't** do is the *streaming* style of per-sample work (one continuous loop
pulling per sample), because that needs yield. The Run dropdown is fixed to Per
block because per-block return is the only option that's neither impossible (the
stream style) nor pointless (per-sample return).

### Why GLSL can't do real-time audio — in *any* mode

GLSL is offered for offline bakes (terrain, wavetable/curve shapes) but **never**
in the Script node, in *either* program-owns-loop or per-sample form. This isn't
an oversight or a "not wired up yet" — a GPU is the wrong kind of machine for the
real-time audio path, and the reasons hold for every mode:

| Obstacle | Why it's fatal in real time |
|---|---|
| **Context thread-affinity** | SEANCE's headless GL context is bound to the UI/message thread. The audio callback runs on a separate, driver-owned real-time thread. You can't safely make a GL context current on the audio thread (and release it from the UI thread) per block — GL drivers aren't built for hard-real-time callers. |
| **Dispatch + readback round-trip** | Even a trivial compute dispatch costs a CPU→GPU submit, GPU scheduling + execution, then a CPU stall on a fence/`glFinish` until the result is back. That round-trip is routinely hundreds of µs to several ms — comparable to or larger than an entire audio block period (512 @ 48k ≈ 10.7 ms; at a 64-sample buffer ≈ 1.3 ms). One GPU round-trip per block can blow the deadline → dropouts. |
| **No real-time guarantee** | The driver can stall unboundedly for VRAM eviction, the desktop compositor, another process's GPU work, or a context switch. That's the same forbidden category as malloc/GC on the audio thread — except *out of our control*, in the driver/OS. |
| **GPU is throughput, not latency** | A GPU's advantage is thousands of lanes running in parallel. Audio needs low latency and modest per-step compute on a deadline — the opposite profile. |

Walking the two modes the question asks about:

- **Per-sample GLSL** is the worst case: dispatching a shader to compute *one*
  scalar wastes the entire parallel machine and pays the full round-trip latency
  44,100×/sec. A GPU doing one sample at a time is all latency and no throughput.
- **Program-owns-loop / streaming GLSL** doesn't fit the hardware model at all.
  GPU compute is batch-parallel — "run this kernel over this whole array at
  once." There is no coroutine-style `pull()` one sample, suspend, `out()` one
  sample interleaved with a CPU producer. The closest a GPU gets to "owning its
  loop" is processing a whole buffer in parallel — i.e. the *block* model — and
  the block model is exactly what the round-trip latency above kills.

So GLSL's strengths (massive parallel batch compute, high throughput, latency
irrelevant) line up perfectly with **offline baking** — run once, freeze the
result into the project, never touch the GPU again at playback — and line up
against everything real-time audio needs. It's not strictly *impossible* to
imagine a high-latency bulk-processing GPU audio path, but for an interactive
real-time node graph it's the wrong tool, so it's deliberately not offered. The
audio thread only ever reads GLSL's *baked* output, exactly like a hand-drawn
curve.

**"Then run GLSL on the CPU to dodge the GPU concerns?"** Tempting — the latency
and thread-affinity problems come from the GPU *execution model*, not from GLSL
the language, so CPU execution would sidestep them. But there's no "CPU mode"
switch on our GL backend (`glsl_compute` is fundamentally an OpenGL-context
wrapper), and the two ways to get CPU-GLSL are both large, with a punchline that
makes the whole exercise pointless:

- **A software GL driver (llvmpipe / SwiftShader).** Heavy dependency to vendor
  and ship, and *still not real-time-safe* — a software driver allocates, spawns
  worker threads, JIT-compiles shaders, and owns a thread-affine context, so
  calling it from the audio callback per block is the same forbidden behaviour.
  You lose the PCIe round-trip and keep almost every other problem; realistically
  you'd still only run it offline, which is what we already do on the real GPU.
- **Compile GLSL ourselves (glslang → SPIR-V → CPU).** Translate to SPIR-V, then
  either emit C++/LLVM and compile it or interpret SPIR-V on a small VM. To be
  real-time-safe the result must be bounded, allocation-free, and compiled
  **offline** (UI thread), so the audio thread only calls a fixed routine. But
  that artifact — *an expressive language, compiled ahead-of-time off the audio
  thread into a bounded allocation-free per-block routine* — **is exactly what
  WASM already is.** Route 2 is "build a new compiler/VM to arrive at a more
  awkward WASM."

So a real-time-safe CPU-GLSL is architecturally *the same thing as the WASM path
we already have*; the GPU was the only thing GLSL added over WASM/Lua/Builtin,
and CPU execution throws that away. If the real want is GLSL's *ergonomics* in
real time (vector math, and especially `waveform(id, phase)` bank access), the
proper move is to enrich the languages already in the audio path — not to port
GLSL to CPU. And anyone who genuinely needs a GLSL algorithm running live can go
**GLSL → SPIR-V → WASM offline** and load the `.wasm` as a Script node. Building
CPU-GLSL for real time would duplicate WASM at high cost, so it's deliberately
not done.

**What was actually enriched.** Following the "enrich the languages already in
the path" plan, the **complete scalar slice** of GLSL's
`Trigonometry`/`Exponential`/`Common` builtins is now shared across **all three**
custom-expression dialects — the Built-in parser (`builtin_synth.cpp`
`WaveExprParser`), the Lua prelude (`lua_prelude.h`), and the Python bake
preludes (`scripting.cpp`):
`asin acos atan(y[,x]) sinh cosh tanh asinh acosh atanh exp2 log2 inversesqrt
sign round roundEven trunc fract mod mix step smoothstep fma radians degrees`
(on top of the pre-existing `sin cos tan sqrt exp log pow abs floor ceil min max
clamp`), with `mod` using GLSL's floored semantics and domain guards
(`acosh`/`atanh`) matching across the three. The only scalar GLSL builtins
deliberately *omitted* are the ones that don't fit a float-only single-return
evaluator: bit-reinterpret (`floatBitsToInt` & friends — no integer type), the
two-output `modf`/`frexp` (their pieces are already reachable via `trunc`/`fract`
and `log2`), and `ldexp`/`isnan`/`isinf` (niche). One subtlety worth recording:
**Lua 5.3 removed `math.tanh`/`math.sinh`/`math.cosh`**, so on Lua 5.4 the single
most common waveshaping saturator (`tanh`) was silently absent until the prelude
re-defined all three by hand (with an overflow guard, `±20 → ±1`) — a real gap the
"don't forget the other languages" pass closed.

The `waveform(id_or_name, phase)` factory-bank accessor — once present only in
Built-in, Lua, Python and GLSL — is **now in WASM too**, closing the last
cross-language gap. The Script node's WASM ABI gains two host imports,
`ss_waveform(int id, float phase)` (raw `[-1,1]` sample, identical wrap+interpolate
to the other languages) and `ss_waveform_id(const char* name)` (resolve a name to
its stable integer id once, then reuse the integer — the WASM analogue of Lua's
`waveforms[name]`), with the ~4000-entry bank warmed off the audio thread at
module-link time. Because WASM modules compile `-nostdlib` (no libm), the same pass
added header-only GLSL-parity shaping helpers to `soundshop_wasm.h`
(`ss_fract`/`ss_sign`/`ss_mod`/`ss_clamp`/`ss_mix`/`ss_step`/`ss_smoothstep`/
`ss_radians`/`ss_degrees`/`ss_saw`/`ss_square`/`ss_triangle`/`ss_unipolar`/
`ss_bipolar`, built on `__builtin_floorf` so they need no libm); the
transcendentals still require `<math.h>` + a linked libm.

### Waveshaping warps — the same transfer functions, every language

SEANCE's warp framework (`warp.h`/`warp.cpp`) is the single source of truth for
the catalogue of amplitude/phase shaping transfers — soft-clip, hard-clip,
wavefold, wavewrap, rectify, quantize, tube/tape saturation, flip, Chebyshev,
the bend/asym/PWM phase shapers, etc. (the same `WarpMethod` enum used by the
node-graph Warp effect and the wavetable painter). Those transfers are now
**callable directly from scripts** so a bake or a live Script node can reach the
exact same shaping math the rest of the app uses, instead of re-deriving a
saturator by hand.

**Per-sample scalar warps — every language, real-time-safe.** Two scalar
primitives are exposed in all custom-expression dialects (Built-in, Lua, Python,
WASM):

- `warpamp(method, x, amount)` — amplitude transfer: shapes one signal sample.
- `warpphase(method, phase, amount)` — phase transfer: shapes one phase value.

The **method comes first** (consistent with the rest of the warp API); the
whole-buffer calls below put their buffer first instead.
`method` is either the stable integer id or a name string
(`"soft clip"`, `"bend+"`, … — case/space/punctuation-tolerant, resolved by
`warpMethodFromName`). `amount` is `0..1` where **0 is an exact identity** and an
unknown method is identity too, so they're safe to sweep. Because they're pure
`float→float` (no allocation, no state), they're allowed in *every* mode
including per-sample real time. In WASM they're the host imports
`ss_warpamp`/`ss_warpphase` plus `ss_warp_method(const char*)` to resolve a name
to its id once (the `warpMethodFromName` analogue, same pattern as
`ss_waveform_id`). The `SS_WARP_*` id constants are in `soundshop_wasm.h`.

**Whole-buffer warps — offline/streaming only.** Two transforms operate on a
whole buffer rather than a single sample, because they warp a *representation* of
the signal, not its instantaneous amplitude:

- `spectralwarp(buf, method, amount)` — FFT the buffer, run the amplitude
  transfer over the per-bin **magnitude** envelope (phase preserved, DC bin left
  alone), inverse-FFT back.
- `waveletwarp(buf, method, amount[, filter="db4", levels=5])` — multi-level DWT,
  warp every wavelet **coefficient** through the transfer, inverse-DWT back. The
  round trip is exact perfect-reconstruction, so `amount` 0 returns the buffer
  unchanged.

Both route every value through the same `warpAmpValue` transfer as `warpamp`, so
the catalogue stays a single source of truth. They take a whole buffer (a Lua/
Python list, or a `(ptr,len)` pair in WASM) and are therefore **bake/offline or
block/stream context only** — never per-sample. There is deliberately **no**
`granularwarp` binding: a per-grain amplitude warp collapses to `warpamp` applied
sample-by-sample, so adding one would just duplicate the scalar primitive over a
loop (documented in `buffer_warp.h`). Implementations live in
`buffer_warp.cpp`; the FFT is `fft_util.h`, the wavelet filters are
`getWaveletFilter()` from `wavelet.h` (the synthesis here is a local
perfect-reconstruction transform — wavelet.h's own `idwt` is frozen for painter
compatibility and is not perfect-reconstruction for multi-tap filters).

**Composition ("multiple wave objects").** Lua/Python/GLSL bakes could always
define local intermediates and combine them; the Built-in language now does too —
a Formula/curve source with newline/`;`-separated statements and `name = expr`
assignments runs in *program mode* (`WaveExprParser::runProgram`), so
`a = sin(x)` / `b = 0.5*sin(3*x)` / `a + b` builds several waves and sums them.
Each baked sample is an independent pure function of the sweep position (fresh
state per sample), matching the other languages' per-call-local semantics.

So the practical ergonomics gap between "our expression language" and GLSL is now
just the **type system** — vectors, matrices, and swizzles — not the math
vocabulary or value composition. Closing *that* gap is a compiler project, scoped
in the next note.

### How big is "make our expression language *exactly* GLSL"?

Honest sizing, because the temptation ("just keep adding functions until it's
GLSL") hides a cliff:

- **Scalar parity — done, cheap.** Adding named `float→float` (and a couple of
  `float,float→float`/`float,float,float→float`) builtins to a recursive-descent
  evaluator is an afternoon each; the complete scalar GLSL set described above is
  now in. Plus newline/assignment composition. This is the 90% of what
  hand-authored wavetable/curve formulas actually use.
- **Full GLSL — a from-scratch compiler, person-months.** "Exactly matches
  GLSL" means a real type system (`float/int/uint/bool` × `vec2/3/4` × `mat2/3/4`),
  swizzles (`v.xyzw`, `v.rgba`, write-masks), ~100+ overloaded builtins with the
  component-wise/per-type overload rules, implicit conversions, user-defined
  functions with overload resolution, `const`/`uniform`/control flow, the preprocessor,
  and array types. Our current evaluator re-parses the source string per sample
  and only knows `float`; none of that scaffolding generalises. A *robust subset*
  (scalars + `vecN` + swizzle + the common builtins, no preprocessor) is still
  weeks of focused work plus a serious test suite; genuine spec conformance is
  the kind of thing `glslang` represents — not something to hand-roll.
- **Therefore the recommended split:** keep growing the **scalar** vocabulary in
  lockstep across the three dialects whenever a useful function is missing (it's
  cheap and instantly available in real time), and for anyone who truly needs
  GLSL's *vector* expressiveness running live, point them at the documented
  **GLSL → SPIR-V → WASM (offline)** toolchain rather than reimplementing GLSL's
  type system inside our parser. Building our own GLSL compiler would land us, at
  great expense, on a worse-tooled version of a path (SPIR-V) the ecosystem
  already maintains.

---

## Offline / bake contexts (no audio-thread constraint)

Here the script runs **once, on the UI thread**, and its output is **frozen
into the project file** (a grid, or a 512-sample lookup table). At playback the
audio thread just reads the baked data — *no interpreter ever runs in real
time.* That's why Python and GLSL are allowed here and nowhere else, and why
"is it real-time safe?" is simply not a question that applies.

### Terrain synthesis — Builtin, Lua, Python, GLSL

Generates an N-dimensional grid. `GenLang { Builtin, Lua, Python, Glsl }`
(`terrain_synth.h:81`). Two sub-modes: **per-cell** (a function returning one
value per cell) and **whole-grid** (the program sees the whole array and can do
cross-cell work — convolution, cellular automata, diffusion — optionally over
multiple passes).

| Language | Per-cell | Whole-grid | Rationale |
|---|:--:|:--:|---|
| **Builtin** | ✓ | — | Fast, zero-dependency, but a single per-cell expression — no cross-cell access, so no whole-grid. Use for clean analytic surfaces (`sin(x)*cos(y)`). |
| **Lua** | ✓ | ✓ | Full scripting with neighbour access for whole-grid algorithms. The go-to when Builtin's one expression isn't enough and you don't need a GPU. |
| **Python** | ✓ | ✓ | Same role as Lua but with the scientific stack (numpy/scipy/FFT). Heaviest startup, but it's offline so who cares. Use for genuinely complex math. |
| **GLSL** | ✓ | ✓ | Runs on the GPU via a headless GL 4.3 compute context. Massively parallel — the right call for big grids and iterative ping-pong passes (blur, CA, diffusion). Whole-grid uses two alternating SSBOs with `prevAt()`/`neighbor()` helpers. |

**Why no WASM here?** Terrain has its own bake path and WASM was never wired
into it. Two separate reasons, one per sub-mode:

- **Per-cell is *fundamentally* impossible for WASM.** Per-cell generation
  re-invokes the program once per grid cell (the `PerSample` path), but a WASM
  module *owns its own loop* — it is never re-entered per element (this is the
  exact same property that makes it a streaming audio node; see the [WASM
  section](#wasm-and-sample-rate-resolving-the-apparent-contradiction)). So
  per-cell WASM is correctly rejected, and always will be (the self-test asserts
  this: `fillFromScript(ScriptLang::Wasm, …)` returns false). It is the mirror
  image of Builtin's "can't stream" — Builtin has *no* loop, WASM *is* the loop.
- **Whole-grid is feasible but not built.** The whole-grid path runs the program
  *once* and hands it the array, which fits WASM's model exactly (WASM is
  `PerBlock`-capable). The clean extension point already exists —
  `IScriptRuntime::runGenerate(dims, data, error)` (the Lua runtime overrides it
  with a `generate()` entry + `set/get/coord` host calls); the WASM runtime just
  returns the default "can't generate a whole grid." Wiring it up means: a WASM
  override of `runGenerate` with grid host-imports (`generate` export +
  `ss_grid_set/get/coord/dims`), a `GenLang::Wasm` value (kept off the
  `Python==2`/`ScriptLang::Wasm==2` cross-cast trap), and generate-dialog UX for
  picking a **binary `.wasm` file** rather than typing source (the dialog is a
  text editor today; WASM modules load by filesystem path, same as Script-node
  WASM). It's a real feature, not a config flag — and the WASM execution path
  can't be self-tested without a checked-in `.wasm` fixture (the repo ships only
  `.c` example sources, no binaries).

**Pick order:** Builtin for simple analytic surfaces → Lua for scripted /
cross-cell → GLSL when the grid is big or the algorithm is iterative and
parallel → Python when you specifically want numpy/scipy.

### Wavetable layers & ADHSR / spectral curves — Builtin, Lua, Python, GLSL

A **Formula** wavetable layer (sample space), or any ADHSR-envelope segment
curve / spectral magnitude/phase curve set to **Equation** mode (frequency/phase
space), is a text expression baked to a sample buffer.
`ShapeLang { Builtin, Lua, Python, Glsl }` (`shape_expr.h:28`). The same four
languages cover **both** the time-domain waveshape (loop var `x` in radians,
clamped to [-1,1]) and the frequency/phase-domain curves (var `f`/`x` normalized
[0,1], unclamped) — the `domainRadians` flag is the only difference.

| Language | Verdict | Rationale |
|---|---|---|
| **Builtin** | ✅ Default | Pure C++ `WaveExprParser`, thread-safe, instant. `sin(x) + 0.5*sin(3*x)` and friends. |
| **Lua** | ✅ When you need logic | Sandboxed local state; loops/conditionals to build a cycle procedurally. Baked offline so the per-sample cost is irrelevant. |
| **Python** | ✅ For heavy math | Shared CPython on the UI thread; reach for it when you want library math to shape the curve. |
| **GLSL** | ✅ Most capable waveshaping | A GPU compute shader (same backend as terrain) bakes the curve in parallel — one thread per sample. Native GLSL math plus `waveform(id, phase)` for the factory bank make it the strongest tool for rich waveshaping. Works for both waveshapes and spectral curves. Greyed out without a GL 4.3 driver; the curve bakes to flat zero on a machine that can't run it (same fallback as a Python-baked curve where Python is absent). |
| **WASM** | 🚫 Not offered | No ABI for "return one cycle of N samples," and you would compile a whole `.wasm` module just to define one short curve — the open-a-binary flow buys nothing for a one-shot offline bake. (Could be wired later if ever wanted; it's a real feature, not a config flag.) |

**Why these never need to be real-time safe:** the formula is evaluated once at
edit/load time into a lookup table; during playback the voice just samples the
table. So even Python and GLSL — which can never touch the audio thread — are
perfectly fine here. (GLSL bakes on the UI thread, same thread-affine GL context
as terrain generation.)

---

## Quick-pick decision guide

**"I want to drive a parameter / make an LFO or envelope."**
→ Builtin (expression mode). Only escalate if a one-liner can't say it.

**"My idea needs loops/tables/state but stays light."**
→ Lua. Prefer **per-block**; use per-sample only if the logic is trivial and
heed the stutter warning.

**"I'm writing a filter / delay / anything with state that flows across blocks."**
→ Lua **streaming** (`stream()`). Locals persist for free.

**"It's heavy DSP, or I'm running many instances, or I have C/Rust code."**
→ **WASM** (per-block, owns its loop).

**"I'm generating a terrain grid."**
→ Builtin (simple analytic) → Lua (scripted) → **GLSL** (big/iterative/parallel)
→ Python (numpy/scipy).

**"I'm defining a wavetable Formula layer or an ADHSR/spectral curve."**
→ Builtin (simple) → Lua (logic) → **GLSL** (richest waveshaping, GPU) → Python
(library math). Baked offline, so pick purely on expressiveness — GLSL works for
both the time-domain waveshape and frequency/phase-space curves.

**"Should I use Python for sound generation in real time?"**
→ **No.** Python only ever bakes offline tables/grids on the UI thread. It is
never in the audio path. If the DLL is missing the feature just disappears.

**"Can I run GLSL on the GPU as a real-time audio node (per-sample or
streaming)?"**
→ **No** — in *any* mode. A GPU is a high-latency, batch-parallel throughput
machine; real-time audio needs low latency on a hard deadline. The context is
UI-thread-bound, and a dispatch+readback round-trip per block can exceed the
block period with no driver-level real-time guarantee. GLSL earns its place
*offline*, where its parallelism shines and latency is irrelevant. See
[Why GLSL can't do real-time audio](#why-glsl-cant-do-real-time-audio--in-any-mode).

---

## The one rule that explains every "no"

Almost every `—` in the matrix traces back to a single principle:

> **The audio thread runs forever, can't block, and can't tolerate
> unbounded allocation or GC.**

- Builtin is allowed per-sample because it's allocation-free.
- Lua is *allowed but warned* per-sample because its GC can stutter; it's *safe*
  per-block where it's called ~100×/sec instead of ~44,100×/sec.
- WASM is per-block-only because its whole-block ABI is the only shape that fits,
  and it's safe because it's compiled with persistent linear memory and no GC.
- Python and GLSL are **banned from real time entirely** and live only in the
  bake world, where their output is frozen and the audio thread never calls them.

Get that, and you can re-derive the whole table.

---

### Source-of-truth pointers (for when this drifts)

- Real-time capability matrix: `cpp/src/script_runtime.cpp:49-56`,
  `cpp/src/script_runtime.h` (enum + doc comment).
- UI gating (Language / Run dropdowns, greying, per-sample warning):
  `cpp/src/script_lang_bar.h`.
- Lua per-sample / per-block / `stream()`: `cpp/src/script_runtime_lua.cpp`.
- WASM ABI (per-block, owns loop): `cpp/src/script_runtime_wasm.cpp`,
  `cpp/include/soundshop_wasm.h`.
- Terrain languages: `cpp/src/terrain_synth.h:81` (`GenLang`),
  `cpp/src/generate_dialog.cpp`, `cpp/src/glsl_compute.h/.cpp`.
- Curve / wavetable formula languages: `cpp/src/shape_expr.h:28` (`ShapeLang`),
  `cpp/src/layered_wave_editor.h`, `cpp/src/adsr_envelope.h`,
  `cpp/src/curve_editor.h`.
- Python embedding (offline only): `cpp/src/scripting.h/.cpp`,
  `cpp/CMakeLists.txt` (optional `find_package(Python3)`).
- Warp transfers (single source of truth): `cpp/src/warp.h/.cpp`
  (`WarpMethod`, `warpAmpValue`/`warpPhaseValue`, `warpMethodFromName`).
- Scalar warp bindings (`warpamp`/`warpphase`): `cpp/src/builtin_synth.cpp`
  (Built-in parser), `cpp/src/script_runtime_lua.cpp`, `cpp/src/scripting.cpp`,
  `cpp/src/script_runtime_wasm.cpp` + `cpp/src/wasm_script_processor.cpp`
  (`ss_warpamp`/`ss_warpphase`/`ss_warp_method`), `cpp/include/soundshop_wasm.h`.
- Whole-buffer warp bindings (`spectralwarp`/`waveletwarp`):
  `cpp/src/buffer_warp.h/.cpp` (FFT `cpp/src/fft_util.h`, wavelet
  `cpp/src/wavelet.h`), same four binding files as above.

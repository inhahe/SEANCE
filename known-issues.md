# Known Issues

Running log of bugs and tech debt that aren't fixed yet. New entries go at the
top. When something is fixed, delete the entry (git history is the archive).

---

## BUG: legacy Wavelet Pitch Tracker writes its signal to the wrong channel

**Found:** 2026-06-14, while building the new precise **Pitch Detector** node
(`__pitchdetector__`). The legacy **Wavelet Pitch Tracker** node
(`__pitchtracker__`, `WaveletPitchTrackerProcessor` in `builtin_effects.h`)
writes its normalized detected-pitch value to **channel 0** (`buf.clear();
auto* out = buf.getWritePointer(0); ...`) instead of the Signal-output channel
(`2 + outputPinIndex`). Every other signal-producing node writes to channel
`2 + index` (see `spectrum_tap.cpp`, the new `PitchDetectorProcessor`). Because
the node also exposes an "Audio Out" pin on channel 0, the pitch value leaks
onto the audio bus while the actual "Pitch Out" Signal pin (channel 2) carries
silence — so the node's signal output is effectively dead.

**Why not fixed now:** the node is superseded by the new Pitch Detector, which
is more accurate (true YIN/autocorrelation with parabolic interpolation vs.
octave-band wavelet resolution), wired correctly, and offers algorithm choice,
window/hop control, min/max band, and log/linear mapping. The legacy node is
kept only for backward-compatibility with old projects. **Proper fix (when
touched):** change the output write in `WaveletPitchTrackerProcessor::processBlock`
to target channel 2 (`if (buf.getNumChannels() > 2) { auto* out =
buf.getWritePointer(2); ... }`) and stop clearing the whole buffer, matching the
Pitch Detector. Audio passthrough on 0/1 can then stay intact. Consider also
adding a deprecation hint in the node's tooltip pointing users at the new node.

---

## PLANNED: generated-terrain grid export — EXR format + traversal/expression dim cap >8

**Found:** 2026-06-13, after shipping `.npz` / WAV / PNG grid export for
generated (`__generate__`) terrains (right-click node → **Export grid as ▸**).
Two parts of the originally-scoped "Export Source" task were deliberately left
for later because each is a substantial change with marginal value, and a
partial version would be a stop-gap (against the project's no-stop-gap rule).

**1. EXR export (HDR float image).** Currently 2D grids export as 8-bit
grayscale PNG (lossy) or full-precision `.npz`. A 32-bit float `.exr` would let
2D heightmaps go losslessly into VFX/image tools (Nuke, Photoshop, Blender)
that read EXR but not `.npz`. **Why deferred:** there is *no* OpenEXR/tinyexr
support anywhere in the tree, so this requires vendoring a new dependency
(tinyexr is a single-header, Apache-2.0 ~10k-line lib) — a dependency-addition
decision worth making explicitly rather than slipping in. Its value also
overlaps `.npz`, which already preserves full float precision. **Proper fix:**
vendor `tinyexr.h` under `cpp/third_party/`, add a `#include` + one `.cpp`
translation unit defining `TINYEXR_IMPLEMENTATION`, gate the export item on
rank==2 in `node_graph_component.cpp` (the `result == 196/197` block, add a
`198`), and map the float grid → single-channel (or RGB-replicated) EXR scanline.

**2. Traversal / expression axis cap >8.** Three places cap N-D terrains at 8
axes: the manual-terrain **+Dim** button (`main_window.cpp` `changeDimCount`,
`juce::jlimit(1, 8, ...)` ~line 1890, plus its `axisNames[8]` = X,Y,Z,W,V,U,S,T),
the **Lissajous** traversal (`terrain_synth.cpp` ~line 864 `d < 8` indexing
`std::array<AxisParams, 8> axes` in `terrain_synth.h` ~line 287), and the
**Builtin expression** evaluator (`terrain_synth.cpp` `fillFromExpression`,
`float vars[8]` ~line 192, 8 named axis letters, loop `d < std::min(nd, 8)`
~line 266). Note the *generate-script* path (Lua/Builtin per-cell `fillFromScript`)
is already **unbounded** — it binds `c0..c{nd-1}`, so high-rank *generated*
grids already work; only the manual-pin flow, Lissajous, and the legacy
expression fill are capped. **Why deferred:** the proper fix spans all three
subsystems at once (a partial raise — e.g. UI to 16 while Lissajous still
indexes an 8-array — would be an out-of-bounds bug), and the value is marginal
(>8-axis manual terrains are exotic; Lissajous per-axis params aren't even
exposed in any UI yet, and aren't persisted). **Proper fix:** change
`std::array<AxisParams, 8>` → `std::vector<AxisParams>` sized to `numDims`
(it isn't serialized, so no save/load impact), make the Lissajous loop run to
`numDims`, extend the Builtin parser to index `c0..cN` (or numeric var names)
past the 8 letters, and raise the `changeDimCount` cap. Also reconcile with the
*separate* "Max 8 axes" limit in the Layer/wavetable arrangement editor
(`layered_wave_editor.cpp` ~lines 6213/6520), which is its own subsystem.

---

## TECH DEBT (partially resolved): baked blobs copied into undo snapshots — video/wavetable still inline

**Found:** 2026-06-13, implementing "bake the generated terrain data into the
node" for the Terrain-Synth *Generate from program* feature.
**Generated-terrain case fixed:** 2026-06-13 via the `ContentStore`.

**Original problem:** baking a computed grid into the node's `__generate__`
script (and likewise `__video__` / `__wavetable*__`) meant
`NodeGraph::commitSnapshot` → `serializeForUndo(gp=nullptr)` copied every
node's multi-MB blob into a new undo snapshot string on every structural edit.
O(total cells) per snapshot per edit; heavy editing ballooned in-memory undo
history. Debt, not a live bug — snapshots are plain RAM strings and save/load
stayed correct.

**Fix shipped for generated terrain:** a content-addressed side store
(`content_store.{h,cpp}`, `NodeGraph::contentStore`). The canonical form is a
`.npy` payload hashed with a 128-bit content hash (two FNV-1a lanes +
splitmix64 avalanche → 32 hex); the stored form is a 4-byte-plane shuffle +
DEFLATE (JUCE gzip). `__generate__` scripts now carry `#<hash>` in their 4th
field instead of the inline blob; `TerrainSynthProcessor` resolves the grid
from the store at construction. `serializeForUndo` passes `includeBlobs=false`
so snapshots carry only the hash, never the bytes — the bytes live once in the
in-memory store across undo/redo. Real saves (`writeProject`,
`includeBlobs=true`) emit `[Blob]` sections; `readProject` loads them back.
Legacy projects with inline gzip+base64 blobs still decode (null-store path).
Identical grids dedup to one entry. Covered by the `store:` self-tests (all
161 pass).

**Still outstanding (the remaining debt):** **video (`__video__`) and
wavetable (`__wavetable*__`) blobs are NOT yet migrated** — they still inline
their bytes into the script and therefore still bloat undo snapshots. The
mechanism to fix them now exists; the work is to route their bake/parse paths
through `contentStore.putFloatGrid` / `getFloatGrid` (or a bytes-oriented
`insertRaw`/`get` for non-float PCM) and emit `#hash` refs the same way.
`ContentStore` is float-grid-oriented today; decoded video is `uint8` RGBA and
wavetable PCM may want a raw-bytes put/get pair — add those overloads when
migrating. Until then this entry stays open.

---

## TECH DEBT: Python can't be truly launch-optional (load-time-linked, not dynamic)

**Found:** 2026-06-13, making Python an optional dependency.

**What it is:** A build configured *with* Python links the interpreter at
**load time** (the normal `python3XY.lib` import library) and copies
`pythonXY.dll` next to the exe. This means that specific build needs that DLL
present at process start: if the bundled `pythonXY.dll` is *deleted*, SEANCE.exe
fails to launch with a Windows loader error. The `ScriptEngine::pythonAvailable()`
probe (`LoadLibrary`, cached) only protects against Python *features* being used
when the interpreter is stale/mismatched/broken — it cannot save a launch when
the load-time-linked DLL is gone. (A build configured *without* Python is
genuinely launch-optional — it compiles `scripting.cpp` to stubs and never
references the DLL.)

**Why not just delay-load it:** MSVC `/DELAYLOAD:pythonXY.dll` fails at link
time with `LNK1194` — CPython's public API imports *data* symbols
(`Py_None`→`__imp__Py_NoneStruct`, `PyFloat_Type`, the `PyExc_*` exception
objects), and the delay-load helper can only thunk *function* imports.

**Proper fix (substantial):** to make a Python-enabled build also launch when
its DLL is absent, resolve every Python symbol by hand via `GetProcAddress`
(including the data exports, which `GetProcAddress` *can* return) behind a thin
wrapper layer, OR split the interpreter into a separately-`LoadLibrary`-d
plugin DLL that the core never load-time-links. Both are a meaningful chunk of
work given how widely the Python C-API is used across `scripting.cpp`. Until
then the practical answer is "ship the DLL," which the build does.

---

## REQUIRED EXTERNAL PATCH: JUCE WASAPI plain-IEEE-float capture fix

**Found:** 2026-06-11, diagnosing a Logitech C615 webcam mic that recorded as
square-wave garbage under Windows Audio (WASAPI) while DirectSound was clean.

**What it is:** stock JUCE 8.0.12 misclassifies any capture device that reports
its shared-mode mix format with the **plain** `WAVE_FORMAT_IEEE_FLOAT` tag
(0x0003) — instead of the `WAVE_FORMAT_EXTENSIBLE` + IEEE_FLOAT subformat — as
integer PCM. It then decodes genuine 32-bit float samples through the Int32
converter, corrupting every sample (square-wave garbage that also bakes into a
frozen capture). `juce_WASAPI_windows.cpp` `tryInitialisingWithBufferSize`,
`isFloat` detection (~line 919).

**Status:** FIXED via patch, but the fix lives **outside** our repo (it edits
the shared JUCE install at `D:/JUCE-8.0.12`), so it is **lost on any JUCE
reinstall/upgrade and must be re-applied.** The patch and apply instructions are
in `patches/juce-wasapi-ieee-float-tag.diff` and `patches/README.md`. This entry
stays until the fix is upstreamed into JUCE itself (then delete it). Ideally
also submit the fix upstream to the JUCE project.

---

## FEATURE GAP: project tuning (microtuning) never reaches VST3/AU instruments

**Found:** 2026-06-10, while adding note-name support to MIDI scripting (the
scale/frequency discussion).

**What it is:** SEANCE has a project-global **tuning system** (`tuning.h`:
Equal12 / Pythagorean / JustIntonation / Meantone, each a per-pitch-class cents
table) plus concert pitch, surfaced via `Transport::noteToFreq`. The **built-in
terrain synth honors it** (`terrain_synth.cpp` ~1401 calls
`transport.noteToFreq(noteNumber)`, applying the cents). But **third-party
VST3/AU instruments** are fed plain `MidiMessage::noteOn` events and play
standard 12-TET — they never see the project's cents offsets. So selecting a
non-equal temperament has no effect on hosted plugins.

**Not to be confused with scales.** Scales/keys (Major/Minor/Dorian/…,
`music_theory.*`) are a *compositional* constraint (piano-roll snapping, degree
system, Change-Key) and correctly do NOT change frequencies. This entry is
strictly about *tuning/temperament* reaching instruments, not scales. A scale
selection working without instrument awareness is by design and is fine.

**Proper fix (not yet done):** a per-instrument tuning bridge that detunes each
note to the project tuning, preferring the cleanest path the target supports:
1. **VST3 native** — `NoteOnEvent::tuning` (cents) / note-expression tuning. No
   channel juggling; honored by plugins that implement it (Surge, Vital, …).
2. **MPE-style per-note pitch bend** — one note per MIDI channel + 14-bit bend
   (≈0.024 cents/step at the default ±2-semitone range, so precision is a
   non-issue; the real cost is channel allocation / 15-voice polyphony). Works
   with essentially any plugin that responds to pitch bend.
3. **MTS-ESP (ODDSound)** — broadcast tuning table for clients that support it.
No path is universally honored, so this is best-effort per plugin. Sizable
feature; deferred. The note-name/notefreq scripting work uses `noteToFreq`
directly and is unaffected by this gap.

---

## TECH DEBT: graph builder can leave audio connections on a node that doesn't reach Output

**Found:** 2026-06-10, while diagnosing the now-fixed "unconnected wavetable
plays on transport Play" bug (issues 1 & 2 in that session).

**What it is:** `GraphProcessor::rebuildGraph` builds JUCE
`AudioProcessorGraph` connections directly from `graph.links`, then separately
computes `Node::reachesOutput` via a backward audio-only BFS from Output nodes
(`graph_processor.cpp` ~1001-1046). These two are derived from the same data
but are not cross-checked. A diagnostic (`[CONN-DBG]`, since removed) captured a
wavetable node with `reachesOutput == false` that *still* had live audio
connections into an intermediate pan/gain node — i.e. the node fed an audio
branch that dead-ends before Output. That's internally consistent (a dead-end
branch legitimately has connections but no path to the sink), but it means the
node's `processBlock` still runs and its audio is still routed into the graph;
the only thing keeping it silent is that the branch never reaches the sink.

**Why it mattered (the original bug, now fixed):** when the count-delta rebuild
heuristic missed a connection change, the `AudioProcessorGraph` could hold
connections that no longer matched `graph.links` while `reachesOutput` was
stale, producing audio from a node the user believed was disconnected. The root
cause — rebuilds not firing on cable connect/disconnect and mod-pin add/remove —
was fixed by forcing an explicit `requestRebuild` on those paths
(`node_graph_component.cpp`). So this is no longer a live bug.

**Proper defensive fix (not yet done):** make the connection builder and
`reachesOutput` share a single source of truth, OR have the builder skip audio
output connections for any node whose `reachesOutput` is false (a node that
can't reach the sink has no reason to emit audio into the graph). Either removes
the class of bug where a routing inconsistency makes a "disconnected" node
audible, rather than relying on rebuild timing to keep the two views in sync.
Low priority — the timing fix closed the observed symptom; this is hardening.

**Where to look:** `graph_processor.cpp` `rebuildGraph` connection loop
(~860-999) and the `reachesOutput` BFS (~1001-1046); the rebuild trigger in
`processBlock` (~1080-1086).

---

## BUG (needs repro): mod-import loop not reverted after deleting the module

**Reported:** 2026-06-09. User loaded a tracker module, deleted it, and the
song was left "on loop" even though they never enabled looping. Expectation:
deleting the module reverts loop/repeat settings to the pre-import state, and
the user feels a module should express looping via Song Repeat (the "different
kind of loop"), never via the user's manual transport loop region.

**Code analysis (could not reproduce from inspection):**
- `mod_import.cpp` ~1851-1888: a genuine module loop is detected as either
  whole-song (`wholeSongLoopTarget == 0` → Song Repeat = Forever, does NOT touch
  the transport loop) or partial-section (`wholeSongLoopTarget > 0` → sets
  `graph.loopEnabled = true` + a loop region). Both paths call
  `stashPreImportSong()`, which records the pre-import song/loop settings on the
  import's root **group** node (`modImportSavedSong` + `modImportPrev*`).
- `node_graph_component.cpp` `deleteNodeAndDescendants` ~2992-2999: when the root
  group is deleted, it restores all stashed `modImportPrev*` values. The two
  general delete entry points (Delete key → `deleteSelectedNode`, context-menu)
  both funnel here.
- `project_file.cpp` round-trips all seven `modImport*` fields symmetrically.
- `main_window.cpp:905` syncs `transport.loopEnabled = graph.loopEnabled` every
  timer tick, so a restored `graph.loopEnabled = false` propagates to audio.

The restore path is correct in isolation. Likely culprits to confirm with the
user: (a) the module was removed by deleting individual child nodes rather than
the **group root** (only the group root carries `modImportSavedSong`, so
deleting children leaves song settings untouched — by design); or (b) the
module hit the partial-section branch and the real objection is design intent
(#2): module imports should never enable the transport loop at all.

**Possible proper fix (pending decision):** stop using the transport loop region
for partial-section module loops — express looping only via Song Repeat. Since
Song Repeat Forever can't represent "play intro once, then loop from beat X,"
this needs either a loop-start beat added to the Song Repeat system or an
accepted approximation (partial loops degrade to play-once). Do NOT rip out the
current behavior blindly; confirm repro + desired semantics with the user first.

## FUTURE WORK: shared master-VCA so own-envelope synths can honor node AHDSR

**Context:** 2026-06-09, AHDSR consolidation. The shared envelope runtime
(`AHDSREnvelopeRuntime` + `AHDSRCurveTables`, `adsr_envelope.{h,cpp}`) is now
the single amplitude-envelope source for Terrain Synth (incl. wavetable /
frequency-domain / wavelet-space variants), Additive, Phase Distortion, and
Spectral Grain. Those nodes read `node.ahdsrEnvelope` directly and have **no**
A/D/S/R params on the node.

The remaining synths — FM (per-operator envelopes), Particle Cloud (per-grain
mini-envelopes), Drum (per-sound envelopes), and the sample/region players
(SoundFont, SFZ, Sfizz, MultiSampler) — generate their own amplitude shaping
internally, so the node-level *Envelope (AHDSR)…* editor is deliberately **not**
offered for them (gating at `node_graph_component.cpp` ~line 2513-2534). Editing
it would be inert, which the "no silent lies" rule forbids.

**Proper fix:** add an optional shared *master-VCA* stage driven by
`node.ahdsrEnvelope` that multiplies the final per-voice output of any synth,
applied **after** the synth's own internal envelope. Then the editor can be
offered universally, with a note that it stacks on top of the engine's native
shaping. Until then, those synths intentionally show no AHDSR editor.

## FIXED 2026-06-09: Terrain Pan param off-by-one

**Was:** the full Terrain node (`makeTerrainNode`) created params in the order
Attack/Decay/Sustain/Release/Volume/Pan/Speed/…, but the processor's
index-based `getParam(idx)` reads treated index 5 as **Speed**, not **Pan** —
a pre-existing off-by-one where Pan was created but never read and Speed was
read from the Pan slot. Converting all Terrain reads to name-based
`getParamByName` (terrain_synth.cpp) and removing the A/D/S/R params during the
AHDSR consolidation fixed this: every traversal/LFO/grain param is now matched
by name, so creation order and read order can no longer drift.

---

## TECH DEBT: unify the single-curve and layer-stack editors (two-tier plan)

**Observed:** 2026-06-09, while adding Lua/Python authoring to the wavetable
**Formula** layer (`WaveLayer` in `layered_wave_editor`) and the **spectral
curve** (`SpectralCurve`/`SpectralCurvePanel` in `curve_editor`). Several
features reimplement the same two ideas with divergent code/representations:

- **A single authored 1-D curve** — Equation (Built-in / Lua / Python) or Drawn
  (points / freehand), baked to samples. Used by: spectral magnitude/phase
  curves, ADSR segment curves, and the *shape part* of each wavetable/LFO/
  MIDI-gen layer. `SpectralCurve` already is this object; `WaveLayer` reinvents
  it with a `Shape` enum (named primitives baked in) + freehand-only Drawn.
- **A stack of those curves with positioning metadata** — `Layer = curve +
  {harmonic, phase, amplitude}`, summed + normalized. Shared (conceptually)
  between the layered waveform editor and the LFO node (and MIDI generation).
  ADSR and spectral curves do NOT use this tier — they're a single curve.

**Why it matters:** Lua/Python (via the already-shared `bakeShapeExpr`) would
come "for free" to ADSR curves once they sit on the shared single-curve widget.
Right now the per-feature forks mean each new authoring capability has to be
wired N times.

**Proper fix (staged):**
- *Stage A (self-contained, do first):* generalize `SpectralCurve`/
  `SpectralCurvePanel` into a context-configured `ShapeCurve`/`ShapeCurvePanel`
  driven by a `CurveContext { domain (periodic radians vs normalized [0,1]),
  valueRange + clampMode, primitivePalette, defaultExpr }`. The
  `domainRadians` flag on `bakeShapeExpr` already is the periodicity axis.
  Point the ADSR editor at it — gets Lua/Python in ADSR at no extra cost. The
  primitive-button difference the user noted is just `primitivePalette` being
  empty/easing-only for ADSR (Linear/Exp/Log/S-curve) vs waveforms for
  periodic uses.
- *Stage B (invasive, touches audio path):* re-express `WaveLayer` on top of
  `ShapeCurve`, then factor sum/normalize into a `LayerStack` shared by the
  layered waveform, LFO node, and MIDI-gen. Requires reconciling the two
  divergent curve representations (pick `SpectralCurve`'s cleaner Equation/
  Drawn-points/Drawn-freehand model; express named primitives as palette
  presets) — this is the real refactor cost.

**Axes that must become CurveContext config (not code forks):** domain/
periodicity (periodic radians `[0,2π)` wrap vs non-periodic `[0,1]` once),
output range/clamp (`[-1,1]` bipolar audio vs `[0,1]` unipolar ADSR/spectral),
primitive palette, and (Tier-2-only) stacking metadata + normalization.

**Status:** current Lua/Python work (wavetable Formula + spectral curves) is
shipped as the Tier-1 proof. The AHDSR editor (`adsr_envelope_component.cpp`)
already embeds `SpectralCurvePanel` directly, so Built-in/Lua/Python authoring
is *already live* on every AHDSR segment curve (Attack/Hold/Decay/Release) —
the "ADSR gets Lua/Python for free" goal is effectively met without the rename.
A **Hold** segment curve (a `0..1` multiplier on peak, default flat `"1"`) was
added 2026-06-09 alongside this. Stage A (the `CurveContext` struct +
`SpectralCurve`→`ShapeCurve` rename) remains deliberately deferred: it is a
mostly-cosmetic generalization whose value is preventing future per-site forks,
not fixing a current bug, and the rename touches ~16 files with byte-identical
serialization constraints. Do it as one deliberate unit, not slipped in
piecemeal. Stage B is a tracked refactor, not to be slipped in as a partial
(no-stop-gap rule).

## Lua / Wasm script load errors aren't shown in the editor

**Observed:** 2026-06-09, while adding the Language dropdown to MIDI Script /
Signal Shape. If a Lua program has a syntax error or omits the required
`loop()` function (e.g. right after switching the language from Built-in,
when the text is still a built-in expression), or a chosen `.wasm` file
fails to load/link, the runtime captures the message via
`IScriptRuntime::getError()` but the **editor never surfaces it** — the node
just goes silent. The user gets no feedback about *why*.

**Where:** `MidiScriptEditorComponent` (`midi_script_editor.cpp`) and
`SignalShapeEditorComponent` (`signal_shape_node.cpp`). The live error lives
on the runtime owned by the processor (`MidiScriptProcessor` /
`SignalShapeProcessor`), not on the doc, so the editor can't read it directly.

**Proper fix:** plumb a way for the editor to poll the live processor's
`runtime->getError()` (similar to how SignalShape's `onManualTrigger` reaches
the live processor via the GraphProcessor lookup), then show the message in a
red status line under the program editor, clearing it when the script
compiles clean. A `juce::Timer` on the editor that queries once or twice a
second is sufficient (load happens on the audio thread on the next rebuild).

**Workaround for now:** the in-editor Lua reference documents that a `loop()`
function is required, and the inline warning covers the per-sample perf risk.

## Control Bank editor may not push undo steps / mark dirty (audit)

**Observed:** 2026-06-09, noted while fixing the same gap in the Signal Shape
editor (now resolved via a destructor `commitSnapshot`). Other modeless
script-style editors that followed the old SignalShape pattern — **Control
Bank** in particular — may still lack a close-time `graph.commitSnapshot()`,
so edits there leave no undo step and don't set `graph.dirty` (silent loss on
quit). Audit each; the MIDI Script and Signal Shape editor destructors are the
reference for the correct one-snapshot-per-session pattern.

---

## MOD import: suspiciously short (2-beat) transport loop on a long module

**Observed:** 2026-06-08. A user imported a module much longer than two beats
but ended up with a transport A-B loop only ~2 beats long, sitting near the end
of the song. The piano-roll loop overlay made it visible; playback wrapped
inside that tiny region.

**Where:** `mod_import.cpp` loop-target detection. When the order-list walk
revisits an order (`orderVisited[order]` true at ~L1493), it records
`wholeSongLoopTarget = order`, and for `wholeSongLoopTarget > 0` sets the
transport loop to `[orderStartBeat[wholeSongLoopTarget], currentBeat]`
(~L1872-1887). A 2-beat loop means the detected loop-back target started only
~2 beats before the song end — i.e. the walk decided the module loops back to
its final, very short order/pattern.

**Hypotheses (need the actual module to confirm):**
1. *Genuine:* the module really does end by looping a tiny trailing
   pattern/outro (some modules do). Then the import is faithful and this is not
   a bug — just surprising.
2. *Bug:* `wholeSongLoopTarget` resolves to a trailing order it shouldn't
   (e.g. a `Bxx`/pattern-break path that lands on the last order, or an
   `orderStartBeat` recorded at the wrong point), producing a degenerate loop
   instead of the module's real loop point — or instead of "no loop / play
   once."

**Repro need:** the specific imported module file (.mod/.xm/.it/.s3m). Without
it we can't tell hypothesis 1 from 2. **Next step when a repro arrives:** log
`wholeSongLoopTarget`, `orderStartBeat[...]`, and `currentBeat` at the loop-set
site and compare against the module's order list / `Bxx` targets.

**Mitigation already in place:** the loop is now clearly drawn (framed overlay,
not a wash), editable (drag the loop edges), and removable (right-click →
*Disable Loop Region*, or Clip → *Clear Loop Region*). Deleting the import's
root group node also restores the exact pre-import loop state (stashed in
`mod_import.cpp` `stashPreImportSong()` → restored in
`node_graph_component.cpp` `deleteNodeAndDescendants()`).

---

## Python interpreter run on the audio thread → `python314.dll` crash — FIXED

**Observed:** 2026-06-12. Two crash dumps (`SEANCE.exe.1776.dmp`,
`SEANCE.exe.56136.dmp`) with the faulting RIP inside `python314.dll` and an
access violation **reading `0x10`** (a near-null `PyObject*` dereference =
CPython interpreter-state corruption). These were the most recent of nine
recovery-prompt-triggering crashes the user hit "lately." The older dumps in
that batch are the separate `node.script` / `graph.nodes` data races documented
below (`-1` sentinel reads, heap ops); this entry is the *new* signature.

**Root cause:** the embedded CPython interpreter is a single process-global,
GIL-held resource that may **only** be touched from the message thread (see
`scripting.h`). But a Terrain Synth (or Signal Shape) whose source is a layered
waveform containing a **Lua/Python/GLSL Formula layer** re-ran the interpreter
to bake that layer's one-cycle buffer **on the audio thread**:

- `GraphProcessor::rebuildGraph` runs **inside** the audio callback
  (`processBlock`, graph_processor.cpp:1380), so the `TerrainSynthProcessor`
  **constructor** (terrain_synth.cpp:1358) and `reloadIfScriptChanged`
  (terrain_synth.cpp:1211) both run on the audio thread.
- Both call `LayeredWaveform::decode → parseLayer → WaveLayer::rebakeFormula →
  bakeShapeExpr → ScriptEngine::bakeShapeExpr`, which executes CPython.
- Running Python off the message thread (no GIL/thread-state handoff) corrupts
  interpreter state; the next interpreter access faults near-null inside
  `python314.dll`. Timing matched the user's "recovery dialog often lately"
  with no consistent repro — it only bit when a graph rebuild coincided with a
  Terrain/Signal node that had a non-Built-in Formula layer.

**Fix shipped (2026-06-14):** the audio thread no longer runs an interpreter to
reconstruct a formula cycle. Three coordinated changes (all in
`layered_wave_editor.{h,cpp}`, the shared layered-waveform codec used by Terrain
Synth, Signal Shape, and wavetable frames):

1. **Embed the baked cycle in the script.** `encodeLayer` now writes a
   `bake=<count>;<s0>;<s1>;…` field for non-Built-in Formula layers (`;`-
   separated so the comma-split field parser keeps it as one field). `parseLayer`
   loads it straight into `formulaSamples`, so the audio thread renders the
   formula from data — never the interpreter. This mirrors the `__generate__`
   terrain path, which already embeds baked grid data for the same reason.
2. **Thread-guard the baker.** `WaveLayer::rebakeFormula()` checks
   `juce::MessageManager` and, when called off the message thread, refuses to
   bake (leaves `formulaSamples` untouched) instead of running Python. This is
   the crash-safety net: any unmigrated/edge-case script that reaches the audio
   thread renders silent rather than corrupting the interpreter.
3. **Migrate old projects on load.** Projects saved before the embed have no
   `bake=` field. `decode()` sets `decodedNeedsBakeEmbed`, and
   `migrateLayeredScriptEmbedBake` (plus a Signal-Shape sibling loop) re-bakes on
   the message thread and re-encodes in `openProjectFile` (under `mutationLock`,
   audio thread parked) so the embed is present before the graph goes live. The
   undo baseline snapshot is taken after this, so undo/redo also carry the embed.

Covered by six self-tests (`bake:` prefix in `--self-test`). **Residual gap:**
the migration only walks `__layered__` (Terrain Synth) and `__signalshape__`
nodes; a Python/Lua Formula layer embedded in an old **wavetable frame**
(`__wavetable*__`) is crash-safe via the thread-guard but renders silent until
re-saved. Low risk (rare combination, no crash), documented here as a follow-up.

---

## `node.script` live-edit data race — fixed for Terrain Synth, still raw elsewhere

**Observed:** 2026-06-08. A crash (`SEANCE.exe.3596.dmp`) ~1 second after
editing a waveform and closing the wavetable editor. The fault is an access
violation inside a `std::string` copy: `memcpy` of ~1.4 MB (`R8=0x150176`)
from a garbage source pointer (`Rdx=0xfffffffffcbfa000`). The 1.4 MB matches a
granular wavetable node's `script` (PCM-as-text, format `__wavetable4__:…`).

**Root cause:** `TerrainSynthProcessor::reloadIfScriptChanged()` runs on the
audio thread every block and reads/copies `node.script`, while UI-thread
editors rewrite `node.script` in place (the #23 "commit to node.script without
a full rebuild" path). A `std::string` assignment is not atomic, so the audio
thread can observe the new size with a stale/freed data pointer and `memcpy`
from garbage — guaranteed to crash for a multi-MB granular script. This is
*not* the same as the `mutationLock` issue below: that guards `graph.nodes`
reallocation; this is concurrent mutation of a single stable node's string.

**Fix shipped:** added `setNodeScriptSynced(Node&, std::string)` in
`node_graph.h`, which locks the node's existing `auditionMutex` around the
assignment. `reloadIfScriptChanged()` now takes a locked snapshot under the
same mutex and works off the audio-thread-owned `cachedScript` (also used at
the `classifySynthSource` call later in `processBlock`, instead of re-reading
`node.script`). All live writers to Terrain-Synth-classified nodes route
through the helper: `layered_wave_editor.cpp` (wavetable/layered/granular),
`spectral_editor.cpp` (standalone spectral), `wavelet_painter.cpp` (standalone
wavelet paint), and `scripting.cpp` (Python `set_script`). Release builds also
now emit PDBs (`/Zi` + `/DEBUG /OPT:REF /OPT:ICF` in `cpp/CMakeLists.txt`) so a

**Second audio-thread reader was missed — fixed 2026-06-11
(`SEANCE.exe.17316.dmp`):** the same crash recurred while dragging the freeze
*selection band* in the granular editor (grain size and selection both at
minimum). The fault was an access violation in `AudioCacheManager::hashString`
copying `node.script`, reached via `computeNodeHash` → `isCacheValid` →
`GraphProcessor::rebuildGraph`, which runs **on the audio thread** inside the
audio callback. So besides `reloadIfScriptChanged`, the cache hasher is a
*second* audio-thread reader of `node.script`, and it had never been paired
with `auditionMutex` — the band drag fires `onLayerChanged → commitToNode →
setNodeScriptSynced` on every drag tick, and a rebuild landing inside that
write window read a torn size/pointer of the multi-MB granular script. Fixed by
wrapping the `hashString(node.script)` read in `computeNodeHash`
(`audio_cache.cpp` ~L117) in a `std::lock_guard<std::mutex>` on
`node.auditionMutex`, pairing with the writer. The lock is released before the
recursive upstream-hash calls, so no two per-node mutexes are ever co-held (no
deadlock), and the writer never takes `mutationLock`, so there's no cycle with
the audio callback's `mutationLock → auditionMutex` order.
future crash dump is symbolizable.

**What's still unprotected:** the identical poll-`node.script`-on-the-audio-
thread pattern exists in other processors — `midi_mod_node.cpp` (~L88),
`trigger_node.cpp` (~L294), `signal_shape_node.cpp` (~L128),
`multi_sampler.cpp` (~L342), `soundfont_processor.cpp`, and the
`spectrum_tap.cpp` write path (~L440). Their editors still do raw
`nd->script = …` (`midi_mod_node.cpp:412`, `trigger_node.cpp:812`,
`signal_shape_node.cpp:721`, `multi_sampler_editor.cpp:264`,
`convolution_editor.cpp:135`, `spectrum_tap.cpp:441`). These scripts are small
(bytes to KB), so the realloc/copy window is orders of magnitude smaller than
the granular case and the race is effectively never observed — but it is the
same bug class. **Proper finishing fix:** route every one of those writers
through `setNodeScriptSynced`, and wrap each processor's `node.script`
compare/copy in a `std::lock_guard` on that node's `auditionMutex` (snapshot
into the processor's `cachedScript` exactly as Terrain Synth now does).

---

## Audio-thread graph access is only partially synchronised

**Observed:** 2026-06-07. A tracker-import crash
(`SEANCE.exe.63000.dmp`) was traced to the audio callback iterating
`graph.nodes` (both for per-event MIDI routing and inside
`GraphProcessor::rebuildGraph` when it observes a node-count change)
concurrently with `mod_import.cpp` calling `graph.addNode()`. A
`std::vector` reallocation mid-iteration produced torn reads of
`Node::id` — the post-rebuild `nodeMap` contained entries with `id=0`
and `id=1132382734` (an obvious freed-memory read), and the JUCE graph
wiring then crashed downstream.

**Partial fix shipped:** added `NodeGraph::mutationLock` (a
`std::mutex`). The audio callback takes a non-blocking try-lock at the
top of `AudioEngine::audioDeviceIOCallbackWithContext` and outputs
silence if it can't acquire it. The batch-mutation entry points hold a
`std::lock_guard` for the duration of the batch: MOD import
(`importModFile` callback in `main_window.cpp`), project file load
(`openProjectFile` + the constructor-time auto-load), autosave recovery,
and undo snapshot restore (`onLoadSnapshot`). `Node::id` and `Pin::id`
also got `= -1` default initialisers so an uninitialised read is
recognisable rather than wild memory.

**Node-deletion paths now guarded (2026-06-09):** the predicted "nonzero
chance" crash actually fired — `SEANCE.exe.80308.dmp`, a crash on deleting
the wavetable node. The UI-thread `graph.nodes` / `graph.links` erase in
`NodeGraphComponent::deleteNodeAndDescendants` ran concurrently with the
audio callback iterating those vectors, so the multi-element `erase`
(which moves/frees elements) produced a torn read / use-after-free —
exactly the `.63000.dmp` mechanism at a delete site instead of an add
site. Fixed by holding `graph.mutationLock` across the structural edit in:
`deleteNodeAndDescendants` (lock to end of function, covers links+nodes
erase and `commitSnapshot`'s serialization read),
the "Merge convolutions" context-menu erase (`node_graph_component.cpp`),
`MainContentComponent::newProject` (clear+rebuild), and the Python
`remove_node` scripting API (`scripting.cpp`). The same edits also removed
two dangling `Node* defaultTrack` pointers held across `addNode()`
(constructor + `newProject` MIDI-input auto-wire) by resolving a stable
MIDI input pin ID up front instead of caching a `Node*`.

**What's still unprotected:** single-node *additive* menu actions (Add
Node, drag-link to create a Link, inline rename) still mutate
`graph.nodes` / `graph.links` from the UI thread without taking the lock.
Each of these is one `push_back` (or a single small field write), so the
race window is tiny relative to the audio block period and the chance of
hitting it is correspondingly low — but it's nonzero and the failure mode
is the same crash class. The proper finishing fix is to hold the lock
around every UI-side graph mutation. Best place to do it is at the
boundary in `NodeGraphComponent` (the context-menu / drag handlers in
`node_graph_component.cpp` that call `graph.addNode`, `graph.addLink`,
etc.). A single audit pass + RAII guard at each remaining call site
should be enough.

---

## Sample-frame captures still have no in-editor view

**Observed:** 2026-06-07. After capturing a single-cycle SampleFrame via the
capture flow, the right-pane editor falls through to the "no editor for this
frame type yet" placeholder (added when the granular editor was built).
The user can see what they captured and replace it via + Waveform, but
there's no way to view the captured cycle, re-do the FFT round-trip
conditioning, or display the embedded pitch readout.

**Where to look:** `updateFrameEditorEmbed()` in `layered_wave_editor.cpp`.
The proper fix mirrors `GranularFrameEditorComponent` (added in the same
session): a `SampleFrameEditorComponent` that shows the single cycle as a
waveform display, a pitch readout, and a "Re-capture..." button that
replaces in place via `showCapturePanelInline(0, /*replaceCurrent=*/true)`.
Add a branch in `updateFrameEditorEmbed()` for `tid == "sample"` that
instantiates it. The plumbing for in-place replacement is already there
(`replaceCurrentEntryWithCapturedFrame`); only the editor component is
missing.

---

## MOD re-import is non-deterministic: sometimes replaces, sometimes duplicates

**Observed:** 2026-06-07. Re-importing the same MOD file via File → Import
MOD on an existing project sometimes ADDS a second set of MidiTimeline +
MultiSampler nodes (one per channel) and sometimes REPLACES the existing
set. Surfaced while diagnosing the (now-fixed) silent-playback-on-restart
bug — re-import was the workaround, and the non-determinism made the
workaround unreliable. Once a user noticed two sets in the graph; other
times only one.

**Why this matters:** when re-import duplicates, the new nodes may be
unwired (not connected to Output) and the old broken ones still hold the
audio path, so the user sees "I re-imported" but hears no change. When
re-import replaces, the new nodes inherit the wiring and the song plays.

**Where to look:** `ModImporter::import` in `mod_import.cpp`. The decision
of whether to clear / overwrite existing imported nodes should be either
"always replace by source-file fingerprint" or "always append + leave it
to the user to delete the old ones". Current behaviour is neither.


---

## Incremental build silently leaves ABI-skewed object files (crashed editor Play)

**Observed:** 2026-06-09. Pressing Play on a previously-created granular
waveform in the layered wave editor crashed SEANCE with an access violation
in `TerrainSynthProcessor::processBlock` at `terrain_synth.cpp:1477`
(`!ev.granularFrame->source->empty()` — reading a garbage `shared_ptr`).

**Root cause:** a structural ABI skew, NOT a source bug. The
`AuditionEvent` struct in `node_graph.h` had a 5th field
(`std::shared_ptr<AuditionGranularFrame> granularFrame`) added at the end,
growing `sizeof(AuditionEvent)` from 40 → 56 bytes. The incremental build
recompiled only a handful of TUs (terrain_synth, layered_wave_editor,
capture_from_playback, graph_processor, audio_engine) against the new
header, but left **65 of 86** SEANCE object files — including
`node_graph.obj` and `piano_roll_component.obj`, both of which construct /
push to `node.pendingAudition` (a `std::vector<AuditionEvent>`) — compiled
against the OLD 40-byte layout. With producers/managers using a 40-byte
element stride and the consumer reading a 56-byte stride, the consumer's
read of the trailing `granularFrame` field landed in the next element's
bytes (interpreted-as-float-data garbage), so the first 0x28 bytes
(isNoteOn / pitch / velocity / position) were valid but `granularFrame`
was corrupt. Classic "first fields fine, last field garbage" skew
signature.

**Fix applied:** deleted all 68 of our own app `.obj` files under
`build/SEANCE.dir/Release` (kept the JUCE / third-party objs, which don't
include `node_graph.h`) and did a full recompile so the whole binary
agrees on every `node_graph.h` layout. The source is correct — the
3-field aggregate inits like `{true, pitch, 100}` in the piano-roll
producers correctly zero-fill `position` and `granularFrame`.

**Recurred 2026-06-13.** Adding `ContentStore contentStore;` as a member of
`NodeGraph` in `node_graph.h` grew `sizeof(NodeGraph)`, but the incremental
build left `main_window.obj` compiled against the old layout. This time the
skew surfaced as a **crash at destruction time**: a stack-local `NodeGraph`
constructed in the self-test printed its "constructed" marker, then
segfaulted when it went out of scope — the stale TU's view of the object's
member offsets put the destructor's frees on the wrong addresses, corrupting
the heap. Deleting `main_window.obj` (then a full `--clean-first` rebuild)
fixed it; all self-tests pass. **Signature to recognise:** a destructor-
time / free-time crash that appears right after adding or reordering a member
in a widely-included header is almost always this, not a logic bug in the new
code. Reach for a clean rebuild *first* before auditing the new code.

**Root cause corrected 2026-06-13 (was previously misattributed to a broken
dependency tracker).** An earlier version of this entry claimed "MSBuild's
incremental dependency tracking did not reliably recompile header dependents."
That is **wrong** — it was disproven empirically:

- **The tracker records the dependency.** `build/SEANCE.dir/Release/SEANCE.tlog/CL.read.1.tlog`
  (UTF-16; decode with Python, not `iconv`) lists `NODE_GRAPH.H` under
  `MAIN_WINDOW.CPP`'s 946-entry read group (and `CONTENT_STORE.H`,
  `TERRAIN_SYNTH.H`). MSBuild's FileTracker knows `main_window.cpp` depends on
  `node_graph.h`.
- **An ordinary incremental build honors it.** `touch src/node_graph.h` →
  `cmake --build build --config Release --target SEANCE` recompiled
  `main_window.cpp` (build log shows it; `main_window.obj` mtime advanced).
  So a clean, uninterrupted incremental build DOES rebuild dependents on a
  header change.

So the dependency tracker is sound. The stale objects came from a **desynced
incremental-build state**, not a tracker defect. The realistic triggers in
this workflow:

1. **Interrupted / killed builds.** These full rebuilds take minutes; in the
   agentic loop they're frequently run under a Bash timeout, `run_in_background`,
   or get cancelled. A long MSBuild killed mid-flight can leave a subset of
   objs recompiled and others stale, with `.lastbuildstate`/tlog and obj
   timestamps no longer in agreement — and the project can still *link* against
   the mixed set on a later partial build.
2. **Timestamp anomalies that defeat the mtime up-to-date check.** MSBuild
   decides "up to date" by comparing mtimes. Anything that makes the edited
   header's mtime NOT advance past an obj — a git checkout/stash/restore that
   resets mtimes, a file-sync or editor that preserves mtime, or clock skew —
   makes MSBuild skip a TU that genuinely needs recompiling. (The reported
   "`node_graph.obj` older than `node_graph.h`" from the 2026-06-09 incident is
   exactly this class of anomaly.)

**How to avoid (unchanged, still correct):** after any change to a struct
layout in a widely-included header (`node_graph.h` especially), do a clean
rebuild rather than trusting the incremental build — `cmake --build build
--config Release --clean-first` (or delete the app `.obj` files under
`build/SEANCE.dir/Release`). At minimum, **let the incremental build run to
completion uninterrupted**, and if anything looks off, verify no
`SEANCE.dir/Release/*.obj` is older than the edited header. The mitigation
targets the desynced-state symptom; the build tooling itself is not broken,
so this is a rare/transient hazard, not a guaranteed recurrence on every
layout-changing header edit.

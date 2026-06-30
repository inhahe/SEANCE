# Per-Voice Polyphony — the Voice container ("Poly Grid") architecture

Status: **design approved, implementation in progress.** This document is the
canonical spec for the per-voice polyphony feature. Keep it in sync as the
implementation lands.

## Problem

SEANCE's node graph is already a modular patching environment: nodes wired with
audio / MIDI / param(signal) cables, where **Signal pins carry per-sample
control as extra audio-buffer channels** (Param pins carry a block-rate scalar).
What it *cannot* do is build a **polyphonic instrument voice out of graph
primitives**. Polyphony today lives sealed *inside* monolithic instrument
processors (Waveform, FM, Additive, …). You cannot take an oscillator node + a
filter node + an envelope node and have that little subgraph instantiated *per
MIDI note* and summed.

That per-voice-instantiation capability is the one genuinely non-redundant thing
a Bitwig-Grid-style "module system" offers. The patching UX itself is already
covered by the existing graph, so we are **not** building a second modular
system — we are adding the single missing capability (per-voice polyphony) to
the graph we already have.

## Architecture facts this builds on

- The graph is a `juce::AudioProcessorGraph`. Every `Node` becomes a
  `juce::AudioProcessor`, built by one big `switch (node.type)` factory inside
  `GraphProcessor::rebuildGraph` (`graph_processor.cpp`). Links become
  `processorGraph->addConnection(...)`.
- Signal/Param pins are carried as **extra channels** in the JUCE buffer: every
  Signal/Param input pin gets a dedicated control input channel (2, 3, …) and
  every Signal/Param output pin a control output channel
  (`graph_processor.cpp` rebuild logic). Signal = per-sample (audio-rate),
  Param = block-rate value.
- Polyphony is internal to instrument processors; the graph has no per-voice
  concept (`graph_processor.cpp` "voice" only appears in the MPE tuning
  adapter's channel allocator).
- A parent/child container membership model already exists on `Node`
  (`parentGroupId` / `childNodeIds`), currently used for **timeline** grouping.
  The Voice container gets its **own** membership field to keep the two meanings
  separate.

## The core idea: a voiced container node

A new container node type — **Voice** (working name; UI label e.g.
`Voice (8)`). On the main canvas it is a single node with one **MIDI input** and
one **audio output**. Internally it owns an **inner graph** (the per-note patch)
and runs **N independent realizations** of that patch — the voices — summing
their outputs.

```
   main canvas                         inner graph (one Voice container)
   ┌───────────┐                       ┌──────────────────────────────────────┐
   │ MidiTrack │──MIDI──▶┌─────────┐   │  [Voice In]                 [Voice Out]│
   └───────────┘         │ Voice(8)│──▶│   Pitch ─▶ Osc ─▶ Filter ─▶ ╳ ─▶ out  │
                         └────┬────┘   │   Gate ─▶ Env ─────────────▶ ╱        │
                              audio    └──────────────────────────────────────┘
                              to Output                (×N voices, summed)
```

Because color is inferred from pins (`getVisualCategory`), a MIDI-in/audio-out
container reads as an instrument automatically.

## Engine: `PolyVoiceProcessor`

A new `juce::AudioProcessor` wrapping the container, inserted as **one node** in
the main JUCE graph (so main-graph PDC, mixing, and routing treat it normally).
Internally:

1. **N clones of the inner subgraph.** v1 builds N separate inner
   `juce::AudioProcessorGraph` instances, each constructed by the *same*
   node→processor factory `rebuildGraph` uses — refactored into a standalone
   `createProcessorForNode(node, transport, graph, …)` so it can target any
   graph, not just the main one. JUCE then handles inner execution order,
   control-channel wiring, and inner PDC for free. (Later optimization: one
   topological order run N times, if N graphs proves heavy.) All N clones read
   the **same `Node`** for parameters; they differ only in DSP state (phase,
   filter memory) and per-voice context.

2. **Voice-context source modules.** Inside the patch, per-note signals come
   from special source nodes living on the left boundary:
   - **Pitch In** — note pitch (Hz and/or note number) as a Signal out.
   - **Gate In** — 1.0 while held, 0.0 after note-off (the envelope's trigger).
   - **Velocity In** — note-on velocity (0..1) as a Signal out.
   - *(later: MPE per-note Pressure / Slide / Bend.)*
   These are realized as tiny processors whose Signal-out channel the container
   **drives per voice**: before running voice *v*'s block, the container writes
   that voice's pitch/gate/velocity into voice *v*'s context-source processors.

3. **Voice allocation & lifecycle.** The container parses its incoming
   `MidiBuffer` (it does **not** forward raw notes into the patch — the patch is
   driven by the context signals):
   - note-on → allocate a free voice (or steal); set pitch/velocity; raise gate
     at the event's sample offset.
   - note-off → drop that voice's gate at the sample offset; the voice keeps
     running until silent (envelope tail).
   - **voice-free detection (v1):** voice is freed when gate-released **and**
     output RMS has been below a floor for K ms. A designated amp-envelope
     module may later report "finished" explicitly for a cleaner signal.
   - **voice stealing:** when all N busy, steal oldest (v1); quietest/round-robin
     later.

4. **Sum & out.** Mix active voices into the container's output buffer; apply
   voice/master gain. Idle voices are skipped, so CPU scales with *active*
   polyphony, not N.

## Visual model: drill-in with breadcrumb

- **Collapsed by default** on the main canvas: one node, MIDI-in left, audio-out
  right, labelled `Voice (N)`. An optional non-interactive *peek* (greyed mini
  preview of the inner patch) can use the existing `groupExpanded` bool.
- **Double-click to drill in.** `node_graph_component` gains a "current
  container" scope: it renders only the nodes belonging to the container you're
  inside (root = top level). A **breadcrumb bar** (`Main ▸ Voice`) shows depth
  and clicks to walk back out. Tint/frame the inner canvas so inside-vs-outside
  is unambiguous. This matches Bitwig's Grid editor, Max/PD subpatches, and
  SEANCE's own stated "richer internal view when you expand/click into it."
- **Boundary pucks.** Inside, pin two boundary nodes to the canvas edges:
  - **left "Voice In" puck** — origin of the Pitch/Gate/Velocity context (+ raw
    MIDI for inner modules that want it).
  - **right "Voice Out" puck** — where patch audio leaves to be summed across
    voices.
  These mirror JUCE's `AudioGraphIOProcessor` I/O nodes and Max inlets/outlets,
  so the boundary is visible, not a black box.
- **Pop-out option.** Like the Layered Waveform editor already pops out a
  window, the inner graph can optionally open in a detached window (same scoped
  component) so both levels are visible at once. Offered as an alternative way
  to open the same view, not a separate model.
- **Avoid inline nesting** (drawing inner nodes inside an expanded box on the
  main canvas) as the primary model — it gets cramped fast and complicates
  pan/zoom and hit-testing. The collapsed-node + drill-in model is the lead.

## Integration with required systems (per CLAUDE.md checklist)

- **Save/Load** — inner nodes and links already serialize generically; add
  container fields (membership id, polyphony count N, steal mode, glide) to
  `project_file.cpp`. Round-trip test.
- **Dirty tracking / Undo** — container creation, inner-graph edits, and param
  changes go through the normal `commitSnapshot()` path (topology changes), same
  as any node add/remove.
- **Graph processor** — the container is one node type with one case in the
  factory; the inner graph is owned and driven by `PolyVoiceProcessor`.
- **Node color** — inferred from MIDI-in/audio-out pins; no new visual category
  needed (reads as instrument).
- **Docs (three surfaces)** — README bullet (new top-level instrument concept),
  REFERENCE section (full spec: container, voice context, drill-in, voice
  stealing, file format), and a `docs/*.html` tutorial ("build a synth voice")
  added to the Help menu + landing page.

## Honest hard parts

- **Voice-end detection** — the RMS/envelope-done rule must not cut tails or
  waste voices. Needs tuning + a self-test.
- **CPU** — N× the patch cost; needs active-voice-only execution and a sane cap
  (8/16/32).
- **Editing the patch while voices sound** — rebuilding inner graphs without
  clicks; v1 can defer rebuilds to silence like the main graph defers rebuilds.
- **Sample-accurate gate timing, glide, unison, per-voice analog drift** — all
  *supported* by the architecture (each voice has its own state, RNG, and
  context) but are M2/M3 polish, not v1.

## Build order

- **Phase 0 (enabling refactor):** extract the node→processor factory from
  `rebuildGraph` into a standalone `createProcessorForNode(...)` that targets an
  arbitrary graph. Pure refactor; self-test stays green. *(Low risk, do first.)*
- **M1 (proof of concept):** Voice container node + membership data model +
  `PolyVoiceProcessor` running N clones; Pitch/Gate/Velocity context modules;
  one existing oscillator + one envelope wired inside; RMS voice-free; fixed N.
  Goal: play a chord into it and hear N summed voices. Self-test for voice
  allocation/stealing.
- **M2:** steal modes, glide, sample-accurate gates, edit-while-playing safety,
  full save/load + dirty/undo, scoped inner editor + breadcrumb + boundary
  pucks.
- **M3:** real module kit (math, LFO, sample & hold, logic, more osc/filter
  types), MPE per-note expression into the context, unison.
- **M4:** presets, modular-familiarity niceties, docs across the three surfaces.

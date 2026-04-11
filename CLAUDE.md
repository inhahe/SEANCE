# SoundShop2

A node-based digital audio workstation (DAW) designed to be intuitive for people without a music background.

## Vision

Traditional DAWs are designed by musicians for musicians and are full of cryptic knobs and buttons. SoundShop takes a node-graph approach where signal flow is visible and composable: every building block (timeline, effect, mixer, output) is a node, and you wire them together with cables.

## Architecture Decisions

### Node-based everything
- Every audio element is a node: timelines, effects, mixers, outputs.
- Connections between nodes are explicit cables — no hidden routing.
- Signal flow is always visible in the graph.

### Timeline as a node
- Each timeline is its own node with a single audio output, keeping graph connections consistent (one output, one cable).
- A timeline node has a richer internal view when you expand/click into it (clip arrangement, etc.).
- Multiple timeline nodes can **snap together** to view them side-by-side, reconstructing a multi-track view on demand.
- All timelines share a single global transport (play/pause/stop/BPM). No independent timeline clocks.

### Pin types
- **Audio** — audio signal flow (blue)
- **MIDI** — MIDI event flow (green)
- **Param** — parameter automation (orange)

### Two-phase development
- **Phase 1 (complete): Python prototype** — `main.py`, imgui_bundle. Used to prove the UX concept.
- **Phase 2 (current): C++ production** — Dear ImGui + imgui-node-editor + SDL2 for UI, JUCE 8.0.12 for audio engine, plugin hosting (VST3/AU), MIDI.

## Tech Stack

- **C++20**, Visual Studio 2022, CMake
- **Dear ImGui** + **imgui-node-editor** (develop branch) — UI and node graph
- **SDL2** + OpenGL 3.3 — window and rendering backend
- **JUCE 8.0.12** (`D:/JUCE-8.0.12`) — audio devices, plugin hosting, MIDI, audio formats
- **Python prototype** still in `main.py` for reference

## Project Structure

- `main.py` — Python prototype (reference)
- `cpp/` — C++ production code
  - `cpp/CMakeLists.txt` — build system
  - `cpp/src/main.cpp` — SDL2 window, ImGui setup, main loop
  - `cpp/src/app.h/cpp` — top-level app, menu bar, file dialogs, preferences
  - `cpp/src/node_graph.h/cpp` — node/link data model, graph drawing, transport bar, context menus
  - `cpp/src/piano_roll.h/cpp` — piano roll state and drawing/interaction
  - `cpp/src/audio_engine.h/cpp` — JUCE audio device callback
  - `cpp/src/graph_processor.h/cpp` — walks node graph, routes audio/MIDI between nodes
  - `cpp/src/plugin_host.h/cpp` — VST3/AU scanning, loading, plugin editor hosting
  - `cpp/src/plugin_settings.h/cpp` — plugin directory management UI, blocklist
  - `cpp/src/project_file.h/cpp` — save/load projects
  - `cpp/src/music_theory.h/cpp` — scales, modes, keys, degree analysis
  - `cpp/src/transport.h/cpp` — shared transport state (beats↔samples)
  - `cpp/src/undo.h/cpp` — command-based undo tree with branching redo

## Building

```
cd cpp
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\SoundShop2.exe
```

## MIDI Note Degree System

Each MIDI note stores its scale degree (1st-7th), octave, and chromatic offset alongside the raw pitch. This enables:
- **Analyze**: tag notes with their degree in the current key/scale
- **Change Key**: recompute pitches from degrees in a new key/scale (e.g., C Major → D Minor preserves melody shape)
- **Degree display**: notes show their degree in the piano roll (e.g., "C4 (1)", "E4 (3)")
- **Chromatic offset**: notes outside the scale are marked with sharps/flats relative to the nearest degree

Workflow: Analyze in original key → change key/scale dropdown → Change Key button.

## When Adding New Features — Checklist

Every new feature that modifies project state must integrate with these systems:

1. **Save/Load** — any new data (new node fields, new clip types, etc.) must be serialized in `project_file.cpp` save/load. Test that a round-trip save→load preserves the data.
2. **Dirty tracking** — modifications must cause `projectDirty = true` so the user gets prompted to save on quit. Currently tracked via undo tree growth.
3. **Undo/Redo** — every state-changing operation must end with either an `exec()`/`LambdaCommand` (fast-path) or a `commitSnapshot("description")` call (snapshot path). See the policy below.
4. **README** — if the feature adds or changes any user-visible behavior (new node, new editor, new menu item, new keyboard shortcut, new concept, changed UX, etc.), update `README.md` in the same change. The README is the user-facing description of what the project does — letting it drift behind the codebase makes it actively misleading. Even small additions get a mention. The "Core concepts" section (Signals / Wavetable arrangement / Waveterrain / Layers / Groups) and the categorized "Features" section are the right places for most additions; new top-level concepts may warrant new sections. Treat the README the same as save/load and undo: a non-optional integration step, not an afterthought.

Other things to keep in mind:
- **Node color** is inferred from I/O pins (`getVisualCategory`), not hardcoded per node type. If you add a new I/O pattern, check if it needs a new visual category.
- **Plugin I/O** — when a plugin is loaded, its actual bus layout should determine the node's pins.
- **Graph processor** — new node types need a case in `GraphProcessor::processBlock` to handle audio/MIDI routing.

## Undo Strategy — Decide at the Moment You Add the Feature

Two coexisting mechanisms feed the same undo tree:

- **`exec()` + `LambdaCommand`** (the fast path) — captures a do/undo closure pair. Reverting is a tiny in-place data tweak. Used for high-frequency operations where the user expects per-edit Ctrl+Z granularity and a snapshot revert would be visibly slow.
- **`commitSnapshot("description")`** (the default path) — serializes the full graph (excluding plugin internal state) and pushes it as a new step if it differs from the previous snapshot. Reverting reparses the snapshot text and rebuilds the audio graph, which is fine for infrequent structural changes but would be visibly sluggish for things like dragging a note pitch.

When adding a mutating function, ask three questions:

1. **Is it called frequently?** — per drag tick, per keystroke, per individual edit. Not per dialog or per menu item.
2. **Does it have a trivial in-place inverse?** — flipping a bool, restoring a number, swapping a small struct. No object instantiation, no graph rebuild, no plugin reload, no editor recreation.
3. **Does the user expect fine-grained undo?** — each individual edit gets its own Ctrl+Z step, not bundled with neighbours.

If yes to **all three**, use `exec()` with a `LambdaCommand` and add the operation to the list below. Otherwise, default to `commitSnapshot("description")` at the end of the handler. Default to snapshot when unsure — its failure mode is much milder (a missed snapshot rolls into the next one; a missed `LambdaCommand` makes the change unrecoverable).

Continuous gestures (slider drag, marker drag) skip undo during the drag and create one undo step on release — the gesture endpoint is the natural commit point.

### Operations using `LambdaCommand` (fast path)

This list is the canonical inventory. **If you add a new operation that meets all three criteria above, add it here at the same time so future maintainers know it's in the fast path.**

- **Place note** — left-click in piano roll empty space (`piano_roll_component.cpp` `pushDone("Place note")`)
- **Delete notes** — Delete key on selection (`piano_roll_component.cpp` `pushDone("Delete N notes")`)
- **Move/resize notes** — committed on drag end (`piano_roll.cpp` `exec("Move/resize notes")`)
- **Bulk note transformations** — transpose, change velocity, change pitch, etc. via the `execNoteEdit` wrapper (`piano_roll.cpp:56`) and the `applyToSelUndo` path in `piano_roll_component.cpp:2205`

Plausible *future* operations that should join this list when implemented:

- Cable drag to a different pin
- Inline node rename
- Mute/solo toggle on a track
- Marker drag on the timeline
- Tempo-map and time-sig point drag
- Clip move/resize (drag end)
- Param slider drag (drag end)
- Automation point edit (drag end)
- Pin reordering on a node
- Knob right-click → "set to default"

Everything else — adding/removing nodes, adding/removing links, dialog-driven edits, plugin loads, effect-group changes, anything touching graph topology — uses `commitSnapshot()`.

## Code Quality Rules

- **Always do the proper fix.** Never put off a correct fix in favor of a quick hack "for now." Quick fixes accumulate as tech debt and create bugs that are harder to diagnose later. If a fix requires a large refactor, do the refactor. The reserve(512) approach to prevent vector reallocation crashes is an example of what NOT to do — the proper fix is to store node IDs and look them up, not to hope the vector never grows past a magic number.
- **No stop-gap solutions that conflict with the planned proper solution.** When you can already see the right architecture for something but it's a substantial chunk of work to build, do NOT add a small partial fix that delivers a fraction of the value but goes against the eventual design. Stop-gaps in this category accumulate as code that has to be undone, cause user confusion when the proper design lands and the old way disappears, and often create dependencies in the codebase that make the proper fix harder to do later. If the user has agreed on the target architecture, the only acceptable answers are: (1) build the proper solution now, or (2) leave the gap untouched and document it as a planned task. "Build a smaller version of the wrong thing in the meantime" is not an option. **Example:** the user explicitly rejected adding default Volume/Pan signal pins to Audio Track as a quick fix for parameter modulation, in favor of waiting for the on-demand-modulation-pin mechanism (#88) that closes the gap properly across all nodes. Quick fixes "just for the most-wanted cases" are exactly what this rule prohibits.
- **No dangling references.** Never store `Node&` or `Node*` across call boundaries where `graph.nodes` could reallocate. Store `int nodeId` and look up via `graph.findNode(nodeId)` at each entry point.

## UX Principles

- No assumed music knowledge — every control should be self-explanatory.
- Parameters show names and values in plain text, not just unlabeled knobs.
- Signal flow is always visible, never buried in menus.
- Simple by default, powerful when needed.
- **Tooltips where they help.** Add a `setTooltip()` call to any control whose function isn't fully self-evident from its label. Skip the tooltip only when ALL of these hold: (1) the label clearly describes the function, (2) there's no hidden behavior (modifiers, secondary actions, state-dependent meaning), AND (3) the function is universally understood — not just to people already fluent in DAWs / music software. Plain "Play" and "Stop" buttons skip the tooltip; "Mon", "Metro", "Fit", "Capture" do not. **Lean toward keeping the tooltip whenever the label uses music terminology (BPM, time signature, pitch bend, mod wheel, sustain, scale, key, etc.) — this DAW is built for non-musicians, so things obvious to a musician aren't necessarily obvious to the target user.** Knobs and sliders where the units matter (Hz, dB, ms, semitones) should always have a tooltip stating the units. If the tooltip would be more than ~2 sentences, link to the relevant docs page instead: "Do X. Shift+click for Y. See Help → Foo for details." A `juce::TooltipWindow` lives on `MainContentComponent` so `setTooltip` calls anywhere in the component hierarchy actually display.
- **Grayed-out controls must explain themselves.** If a control is disabled/greyed out for any reason (wrong mode, waiting on something, missing prerequisite), its tooltip MUST explain *why* it's disabled and, when possible, *how* to enable it. Silently disabled controls are a usability trap — the user has no way to know whether the feature is missing, broken, or just not applicable right now. Example: "Signal-locked — this param is being driven by an incoming Param/Signal cable. Disconnect the cable to edit manually." If the control is disabled for more than one reason, the tooltip should state whichever reason currently applies.

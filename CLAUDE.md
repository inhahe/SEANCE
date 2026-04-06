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
3. **Undo/Redo** — state-changing operations should go through `exec()` (the command-based undo system) so they can be undone. Use `applyToSelUndo` for note modifications, or create `LambdaCommand` for other operations. Continuous operations (slider drags) can skip undo during the drag but should create a command on release.

Other things to keep in mind:
- **Node color** is inferred from I/O pins (`getVisualCategory`), not hardcoded per node type. If you add a new I/O pattern, check if it needs a new visual category.
- **Plugin I/O** — when a plugin is loaded, its actual bus layout should determine the node's pins.
- **Graph processor** — new node types need a case in `GraphProcessor::processBlock` to handle audio/MIDI routing.

## UX Principles

- No assumed music knowledge — every control should be self-explanatory.
- Parameters show names and values in plain text, not just unlabeled knobs.
- Signal flow is always visible, never buried in menus.
- Simple by default, powerful when needed.

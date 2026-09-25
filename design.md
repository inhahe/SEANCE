# SEANCE — Design Overview

SEANCE (the SoundShop2 project) is a node-based digital audio workstation for
people without a music background: every building block — timelines, synths,
effects, mixers, the output — is a node, wired with visible cables. It is a
C++20 JUCE 8 application (`cpp/`); the Python prototype in `main.py` is kept for
reference only.

This file is a short, living map of the architecture: what the parts are, which
thread owns what, and the invariants that hold them together. It deliberately
doesn't repeat the detail, which lives in:

| Document | What it's for |
|---|---|
| `README.md` | The pitch and feature tour. |
| `REFERENCE.md` | The granular reference: every feature, exact UI, on-disk formats, undo behaviour, and the *why* behind design choices. The authority on detail. |
| `docs/*.html` | Task-oriented tutorials, opened from the in-app Help menu. |
| `poly-voice-architecture.md` | Design and milestones of Voice containers (per-voice polyphony). |
| `known-issues.md` | Open bugs, tech debt and feature gaps. |
| `CLAUDE.md` | Working rules: undo policy, dialogs, tooltips, versioning, the new-feature checklist. |
| `patches/README.md` | Fixes SEANCE needs in the shared JUCE install. |

Keep this file in step when the architecture changes (a new subsystem, a new
thread, a changed invariant); feature-level detail goes in `REFERENCE.md`.

## Code layout (`cpp/src`)

- **App shell** — `main.cpp` (`juce::JUCEApplication`; `--self-test`,
  `--plugin-sandbox`, `--ephemeral` and a project path on the command line),
  `main_window.*` (`MainContentComponent`: menus, dialogs, the UI timer,
  autosave, crash recovery, the plugin loader), `dialog_helpers.*`.
- **Model** — `node_graph.*` (`NodeGraph`: nodes, links, the undo tree, the graph
  lock), `project_file.*` (the `.ssp` text format, also used for undo
  snapshots), `undo.*`, `content_store.*` (large baked blobs, by hash),
  `asset_library.*`, `transport.*`, `music_theory.*`.
- **Canvas and editors** — `node_graph_component.*` (the node graph view, its
  menus and scopes), `piano_roll_component.*`, the wavetable / spectral /
  wavelet / sampler / convolution editors, capture dialogs.
- **Audio** — `audio_engine.*` (device callback, recording, previews),
  `graph_processor.*` (node graph → JUCE `AudioProcessorGraph`), one processor
  per node type (built-in synths and effects, timelines, script nodes),
  `poly_voice_processor.*` (Voice containers), `audio_export.*` (offline
  render + encoders), `audio_cache.*` (freeze / auto-cache).
- **Plugins** — `plugin_host.*` (scanning, blocklist, identity, loading),
  `plugin_settings.*` (folders; also compiled into PresetRecorder),
  `plugin_copies.*` (copies for renders and Voice containers),
  `plugin_window.h` (editor windows), `plugin_sandbox.*` (an out-of-process
  hosting proxy and its `--plugin-sandbox` child mode; not wired into the graph
  yet).
- **Scripting** — `scripting.*` (the embedded CPython Script Console,
  `import soundshop`), `script_runtime*` (Built-in / Lua / WebAssembly for script
  nodes), `glsl_*`.
- **Tests** — `self_test.cpp` (`--self-test <dir>`), `cpp/test_plugins/`
  (LV2 plugins built only for the self-tests), `cmake/check_dialog_patterns.cmake`
  (a build-time lint).
- **PresetRecorder** (`PresetRecorder/`) — a companion tool that records a
  preview of every plugin preset; it shares `plugin_settings` with SEANCE.

## Threads

| Thread | Does |
|---|---|
| **Message thread** | All UI; every structural edit of the node graph; loading and destroying plugins (they must be made there); rebuilding the live audio graph; the undo tree; project load/save serialization; the Script Console; Freeze, Bounce and script renders. |
| **Audio thread** | `AudioEngine::audioDeviceIOCallbackWithContext`: try-locks the graph, routes MIDI, runs the live `GraphProcessor`, mixes previews. Must not block and should not allocate (a few processors still do — `known-issues.md`); never rebuilds the graph (it asks the message thread to). |
| **Offline render threads** | Export (`ThreadWithProgressWindow`) and capture from playback (`RenderJob`) run their own `GraphProcessor` over the same node graph. |
| **Autosave worker** | Writes already-serialized autosave files to disk; never touches the graph. |

## The node graph

`NodeGraph` owns `std::deque<Node> nodes` (a deque, so adding a node never moves
the others) and `std::vector<Link> links`. Processors hold a `Node&` and read
their node's settings every block — so anything that destroys or moves nodes
must keep every graph that holds processors from running meanwhile (an open
tech-debt item in `known-issues.md`).

**The graph lock** (`NodeGraph::mutationLock`, a `GraphMutex` — `graph_mutex.h`):
a recursive mutex every structural mutation holds. The audio callback only
try-locks it (a silent block if it can't). Offline renders hold it one block at a
time; anyone waiting goes first between blocks, and the audio callback waits
briefly for a render's block instead of going silent.
`NodeGraph::nodeStorageVersion` is bumped whenever nodes are destroyed or
replaced, so a holder of `Node&`s that lets go of the lock between uses (capture
from playback) can tell.

**Voice containers**: a node's `voiceContainerId` puts it inside a container's
per-note patch rather than the main graph.

## The audio graph

`GraphProcessor::rebuildGraph` turns the node graph into a JUCE
`AudioProcessorGraph`: one processor per node (plus a pan stage after built-in
audio producers), and per-cable adapters (time gates, cable gain, MIDI channel
filters and stamps, the tuning adapter and MPE handshake for plugins). JUCE
handles execution order and plugin-delay compensation; a latency listener
triggers a rebuild when a processor's latency changes. The live graph is rebuilt
on the message thread, holding the graph lock, when the node or link count
changes or a rebuild is requested; the audio callback plays silence until then.
A `buildScope` makes the same builder build a Voice container's inner patch.

Graphs that exist: the **live graph** (`AudioEngine`), one per **voice** inside
each Voice container (`PolyVoiceProcessor` builds N inner graphs and sums them),
and one per **offline render** (Export / script `render()`, Freeze, Bounce,
capture from playback).

## Hosted plugins

- **Identity**: a plugin node records its plugin's `juce::PluginDescription`;
  `PluginHost::resolvePlugin` finds it again on load (moved, updated, another
  computer). Blocked plugins are never loaded.
- **Loading** is asynchronous and serial on the message thread
  (`beginAsyncPluginLoad` / `processNextPluginLoad`), with a per-node badge.
- **The live graph hosts each plugin node's own instance** (JUCE owns it from
  then on). Rebuilds keep it; undo carries a node's plugin over by id and
  identity; a plugin whose node went is retired alive and disposed of on the
  message thread (window closed, state kept for undo).
- **Every other graph plays copies** (`PluginCopies` + `PluginCopyProcessor`):
  an offline render loads a copy of each plugin as it starts, with its current
  settings; a Voice container plays one copy per voice, following the node's own
  instance (the "master": never played, its window the one edited, its state the
  one saved). Copies are made on the message thread before the graph that plays
  them is built.

## Undo

A branching undo tree (`undo.*`) fed two ways: `commitSnapshot` (the whole graph
serialized without plugin state — the default for anything structural) and
`exec()` with a `LambdaCommand` (a small in-place inverse, for frequent
fine-grained edits such as note moves). `NodeGraph::restoreSnapshot` reparses a
snapshot and carries live plugins across. The policy for choosing between them
is in `CLAUDE.md`. A new or opened project starts a fresh history
(`UndoTree::reset`); the main window remembers the step the project file matches
(`savedUndoStep`), and any other step means unsaved changes.

## Persistence and recovery

Projects are `.ssp` text files (`ProjectFile`); frozen audio is stored beside
them in `soundshop_cache/`; baked blobs live in the content store. Autosave
writes the graph and, separately, each changed plugin's state; after a crash the
next start offers to recover them. `--ephemeral` redirects all of that to a
throwaway folder for testing.

## Testing, versioning, release

`SEANCE.exe --self-test <dir>` runs the headless self-tests and writes a report.
The version lives only in `project(SEANCE VERSION …)` in `cpp/CMakeLists.txt`
(bumped on every rebuild); `release.bat` packages and publishes a build.

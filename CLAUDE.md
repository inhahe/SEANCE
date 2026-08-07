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
- **Phase 2 (current): C++ production** — **JUCE 8.0.12 for everything**: GUI (`juce_gui_basics`/`juce_gui_extra`, software-rendered), node graph, audio engine, plugin hosting (VST3/LV2), MIDI, audio formats.

> **NOTE (corrected 2026-06):** An earlier plan called for Dear ImGui + imgui-node-editor + SDL2 + OpenGL for the UI. **That plan was never built.** The production app is a pure `juce::JUCEApplication` (`main.cpp`) using JUCE's software-rendered GUI. There is **no SDL2, no OpenGL context, and no live ImGui** in the build — `cpp/CMakeLists.txt` links none of them, and the only ImGui-using file (`cpp/src/piano_roll.cpp`) is dead/backup code excluded from the build (the live piano roll is `piano_roll_component.cpp`). Anything needing a GPU/GL context must stand one up from scratch.

## Tech Stack

- **C++20**, Visual Studio 2022, CMake
- **JUCE 8.0.12** (`D:/JUCE-8.0.12`) — UI (software-rendered `juce_gui_basics`/`juce_gui_extra`), node-graph rendering, audio devices, plugin hosting, MIDI, audio formats
- **No GPU/GL/SDL/ImGui** in the production build (see note above)
- **Python prototype** still in `main.py` for reference

## Project Structure

- `main.py` — Python prototype (reference)
- `cpp/` — C++ production code
  - `cpp/CMakeLists.txt` — build system
  - `cpp/src/main.cpp` — `juce::JUCEApplication` entry point, logging, `--self-test`/`--plugin-sandbox` dispatch
  - `cpp/src/main_window.h/cpp` — main JUCE window, menu bar, top-level component
  - `cpp/src/node_graph_component.h/cpp` — live JUCE node-graph editor (drawing + interaction)
  - `cpp/src/piano_roll_component.h/cpp` — live JUCE piano roll (the ImGui `piano_roll.cpp` is dead/backup code, not built)
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

**Testing the GUI: always launch with `--ephemeral`.** When you open the full
app to verify a feature (not `--self-test`/`--plugin-sandbox`, which never make a
window), pass `--ephemeral`. It redirects all crash-recovery state to a wiped
throwaway temp dir, so killing the process to rebuild can't leave a stale
`autosave.ssp` in the user's real `%APPDATA%/SoundShop` — which would otherwise
make their next normal launch falsely prompt "didn't shut down cleanly". The
window title shows `[ephemeral session]`. See `REFERENCE.md` →
[Ephemeral session](#ephemeral-session---ephemeral). Also: kill any leftover
`SEANCE.exe` before rebuilding (the linker can't overwrite a running exe →
`LNK1104`).

**Required JUCE patches:** SEANCE depends on fixes to the shared JUCE install
(`D:/JUCE-8.0.12`) that live in `patches/` and are **lost on any JUCE
reinstall/upgrade**. Re-apply them (see `patches/README.md`) whenever JUCE is
reinstalled — currently `juce-wasapi-ieee-float-tag.diff` (WASAPI capture: stock
JUCE misreads devices that report the plain `WAVE_FORMAT_IEEE_FLOAT` tag, e.g.
the Logitech C615 mic, as integer → square-wave-garbage mic). These should be
upstreamed to JUCE; until then they're tracked in `known-issues.md`.

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
4. **Three doc surfaces — consider all three on every user-visible change.** SEANCE has three places user-visible behavior is documented, each aimed at a different audience. Every change or addition that's user-visible needs to be evaluated against all three, even if the answer for some of them is "nothing to do". They are:
   - **`README.md`** — elevator pitch + feature inventory, read by people browsing the repo on GitHub. Update when the change is significant enough to belong in the high-level tour: a whole new node type, a new editor, a new top-level concept, a new built-in instrument or effect, a new file format, a new keyboard shortcut, a meaningful change to how an existing feature is used. **Don't** update for small refinements — a new button inside an existing dialog, an extra option in a menu, a tooltip rewording, an internal refactor. Treat README as elevator pitch, not change log.
   - **`REFERENCE.md`** at the project root — granular feature reference, read by anyone who wants the full picture (devs, power users, future me). Exact button labels, dialog semantics, edge cases, file locations, on-disk formats, undo behavior, the "why" behind each design choice. Update for **any** user-visible change or addition, even ones too small for the README. If the README mentions a feature, REFERENCE.md should have the full breakdown of how it works. When you add a new section, add an entry to the table of contents at the top, and if the README also covers the feature, link the README summary into the relevant REFERENCE.md anchor.
   - **`docs/*.html`** — task-oriented tutorials opened from the in-app Help menu, read by users actually trying to do something ("how do I make a pad", "what's a Layer"). Update when the change affects how a user would *do* a workflow: new steps to follow, changed UI flow, new shortcut they should know about while working through the tutorial. Don't bloat tutorials with spec-style detail (that's REFERENCE.md's job); keep them readable. If you add a feature significant enough for a new tutorial page, also add it to `MainContentComponent`'s Help menu and to `docs/index.html`'s landing-page list.
   The three exist deliberately, with overlap: tutorials teach, the reference specs, the README pitches. None of them is the change log — the change log is git. **A useful default question on any change is "which of the three need updating?"** rather than "do I need to update docs?".

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
- **Dialogs must not create their own taskbar entry.** When showing any modal/popup window (alerts, message boxes, confirmation dialogs, custom `DialogWindow`s, file choosers' result popups, etc.), it must be parented to the main SEANCE window's HWND so Windows groups it under the existing taskbar icon. **Concretely:**
  - For simple OK/Yes-No popups: use `juce::NativeMessageBox::showAsync(MessageBoxOptions().withAssociatedComponent(this)...)`. NOT `juce::AlertWindow::showAsync` and NOT `juce::AlertWindow::showMessageBoxAsync` — both of those create a JUCE top-level desktop component without an owner HWND, which Windows treats as a separate app and gives its own taskbar entry. `withAssociatedComponent` on the *AlertWindow* path only affects positioning, not taskbar parentage; on the *NativeMessageBox* path it controls the HWND owner that's passed to the OS `MessageBox()` call, which IS what suppresses the taskbar entry.
  - For custom JUCE dialogs (`DialogWindow::LaunchOptions`): use `launchToolDialog()` / `launchManagedToolDialog()` from `cpp/src/dialog_helpers.h`. They wrap the dialog in `ToolDialogWindow`, which overrides `getDesktopWindowStyleFlags()` to drop `windowAppearsOnTaskbar` (yielding `WS_EX_TOOLWINDOW`), and centre it on the main window.
  - For **text/combo entry** dialogs (rename prompts, "Set start beat…"): a plain `juce::AlertWindow` is fine and needs no special handling — `SoundShop::AppLookAndFeel` (installed from `main.cpp`'s `initialise()`) overrides `LookAndFeel::getAlertBoxWindowFlags()` to strip `windowAppearsOnTaskbar`, which fixes every AlertWindow in the app at once. **Do NOT try to fix an AlertWindow by subclassing it and overriding `getDesktopWindowStyleFlags()`** — that's the `DialogWindow` recipe and it silently fails here, because AlertWindow builds its peer during construction (before the subclass vtable is live); the late flag change then re-creates the peer out from under `enterModalState()` and the dialog never appears at all. Measured, not theorised.
  - **Result codes differ between the two APIs.** `AlertWindow` reports button X of N as `(X + 1) % N` (first button 1, last/Escape 0); `NativeMessageBox::showAsync` reports a plain 0-based index. A name-for-name swap compiles cleanly and silently inverts every `if (result == 1)`. Use `SoundShop::showAlertAsync` / `showAlert` (`dialog_helpers.h`), which remap to the AlertWindow convention.
  - **Two automated guards exist — don't disable them.** `cpp/cmake/check_dialog_patterns.cmake` (the `seance_dialog_lint` build target) fails the build on an `AlertWindow` subclass or a `getDesktopWindowStyleFlags` override outside `dialog_helpers.cpp`; `testDialogTaskbarFlags()` in `self_test.cpp` asserts the taskbar flag is really absent from a live `AlertWindow`'s and `ToolDialogWindow`'s peers. `installAppLookAndFeel()` must stay at the very top of `initialise()`, above the `--self-test` dispatch, or the test stops covering the real wiring.
  - **Always physically test** that a new dialog doesn't add a second SEANCE icon to the Windows taskbar. The guards above cover the known traps, but the default JUCE patterns get this wrong in ways no lint predicts. `cpp/build/uitest/enumall.ps1 -ProcId <pid>` prints each window's ex-style: only the main window may be `APP=True`; a fixed JUCE dialog reads `ex=0x180 TOOL=True APP=False`, a native message box `cls=#32770 APP=False owner=<main hwnd>`. See REFERENCE.md → *Dialogs and the Windows taskbar*.

## Version Control

- **Commit to git freely whenever it makes sense; never `git push`.** You don't need to ask before committing — create commits at natural checkpoints (a feature done, a known-good buildable/tested state, before a risky change). Scope commits sensibly and keep build/render artifacts (release archives, `*.wav` renders, `captures/`, self-test scratch) and secrets out of history. Pushing to a remote (GitHub, etc.) is always the user's action — never run `git push` unless explicitly told to in that moment.

## Versioning

- **Bump the version on every rebuild.** `project(SEANCE VERSION x.y.z ...)` on line 2 of `cpp/CMakeLists.txt` is the **single source of truth** for the version. It is aligned with the **published release series on GitHub** (which reached v0.9.0 before CMake started tracking it — the line sat at its `0.1.0` default while releases were numbered by hand). Never "correct" it downward to match that old default: the next version must always be ahead of the newest tag at `github.com/inhahe/seance/releases`. Whenever you rebuild the project after making changes, increment it in the same change — patch (`0.1.0` → `0.1.1`) for fixes and small additions, minor (`0.1.0` → `0.2.0`) for a new node/editor/format or anything that changes how an existing feature is used, major for a break in the project-file format or a redesign that invalidates existing muscle memory. Don't batch several rebuilds under one version: the number is what a bug report will quote, so it has to distinguish builds.
- **Never write the version anywhere else.** CMake feeds `PROJECT_VERSION` to `juce_add_gui_app` → `JUCE_VERSION` → `JUCE_APPLICATION_VERSION_STRING`, which lands in the exe's file-version resource *and* in the Help → About box. `release.bat` reads the same line to pick its tag and refuses to publish if the built exe's version resource disagrees. One number, four consumers — a second hardcoded copy anywhere would drift and defeat all of them.
- **Releasing:** `release.bat` in the project root packages the current build (exe + DLLs + `docs/` + `resources/` + `scripts/` + the markdown docs) into one zip and publishes it to `github.com/inhahe/seance` tagged `v<version>`. It refuses to publish a version that already has a release, so running it without bumping first is a harmless no-op. `release.bat --dry-run` stages and zips without touching GitHub. Publishing is the **user's** action — don't run `release.bat` unless explicitly asked to in that moment, same rule as `git push`.
- **This working copy is not the GitHub repo.** `D:\visual studio projects\SoundShop2` has **no git remote**; the source published to `inhahe/seance` lives in a separate clone that the user syncs with `copy_to_seance.bat` and pushes by hand. So local `HEAD` is normally a commit GitHub has never seen. `release.bat` handles this — it asks GitHub whether it has `HEAD` and only then pins the tag to it, otherwise tagging the default branch — but the ideal order is **sync + push the source first, then release**, so the tag names the exact commit the binary was built from. Don't `git push` from this directory to "fix" it; that's the user's separate workflow.

## UX Principles

- No assumed music knowledge — every control should be self-explanatory.
- Parameters show names and values in plain text, not just unlabeled knobs.
- Signal flow is always visible, never buried in menus.
- Simple by default, powerful when needed.
- **Tooltips where they help.** Add a `setTooltip()` call to any control whose function isn't fully self-evident from its label. Skip the tooltip only when ALL of these hold: (1) the label clearly describes the function, (2) there's no hidden behavior (modifiers, secondary actions, state-dependent meaning), AND (3) the function is universally understood — not just to people already fluent in DAWs / music software. Plain "Play" and "Stop" buttons skip the tooltip; "Mon", "Metro", "Fit", "Capture" do not. **Lean toward keeping the tooltip whenever the label uses music terminology (BPM, time signature, pitch bend, mod wheel, sustain, scale, key, etc.) — this DAW is built for non-musicians, so things obvious to a musician aren't necessarily obvious to the target user.** Knobs and sliders where the units matter (Hz, dB, ms, semitones) should always have a tooltip stating the units. If the tooltip would be more than ~2 sentences, link to the relevant docs page instead: "Do X. Shift+click for Y. See Help → Foo for details." A `juce::TooltipWindow` lives on `MainContentComponent` so `setTooltip` calls anywhere in the component hierarchy actually display.
- **Grayed-out controls must explain themselves.** If a control is disabled/greyed out for any reason (wrong mode, waiting on something, missing prerequisite), its tooltip MUST explain *why* it's disabled and, when possible, *how* to enable it. Silently disabled controls are a usability trap — the user has no way to know whether the feature is missing, broken, or just not applicable right now. Example: "Signal-locked — this param is being driven by an incoming Param/Signal cable. Disconnect the cable to edit manually." If the control is disabled for more than one reason, the tooltip should state whichever reason currently applies.

# SEANCE Reference

Detailed feature documentation for SEANCE. The README gives the high-level
tour of what the app does; this file is where the granular behaviour lives —
exact button labels, edge cases, file locations, dialog semantics, etc.

If you're looking for "what is SEANCE and what can it do", start with the
README. If you're looking for "exactly what happens when I click X", start
here.

---

## Table of contents

- [Graph fundamentals](#graph-fundamentals)
- [Transport bar](#transport-bar)
- [Recording live audio (mic / line-in)](#recording-live-audio-mic--line-in)
- [Automation recording (knob-drag capture)](#automation-recording-knob-drag-capture)
- [Computer Keyboard node](#computer-keyboard-node)
- [MIDI input and routing](#midi-input-and-routing)
- [Piano roll](#piano-roll)
- [MPE (per-note expression)](#mpe-per-note-expression)
- [Microtuning hosted plugins](#microtuning-hosted-plugins)
- [Layered Waveform editor](#layered-waveform-editor)
- [Waveform warp (shape-bending)](#waveform-warp-shape-bending)
- [Waveshaper (amplitude morph) effects](#waveshaper-amplitude-morph-effects)
- [Standalone single-cycle oscillators (frame synths)](#standalone-single-cycle-oscillators-frame-synths)
- [Frequency-domain (spectral) synth](#frequency-domain-spectral-synth)
- [Granular synths (Particle Cloud, Spectral Grain)](#granular-synths-particle-cloud-spectral-grain)
- [Terrain Synth](#terrain-synth)
- [Effect layers and groups](#effect-layers-and-groups)
- [Wavelet effects](#wavelet-effects)
- [Pitch Detector](#pitch-detector)
- [Pitch Shift node](#pitch-shift-node)
- [Convolution Filter](#convolution-filter)
- [3D Spatializer (binaural / holophonic)](#3d-spatializer-binaural--holophonic)
- [MIDI Modulator](#midi-modulator)
- [Trigger Node](#trigger-node)
- [Analyzer / visualizer nodes (Spectrum Analyzer, Oscilloscope, Spectrogram)](#analyzer--visualizer-nodes-spectrum-analyzer-oscilloscope-spectrogram)
- [Script (signal + MIDI)](#script-signal--midi)
- [Control Bank](#control-bank)
- [Shared AHDSR envelope](#shared-ahdsr-envelope)
- [Voice container (per-voice polyphony)](#voice-container-per-voice-polyphony)
- [Asset library (project stores)](#asset-library-project-stores)
- [Terrain-synth self-test (`--self-test`)](#terrain-synth-self-test---self-test)
- [Ephemeral session (`--ephemeral`)](#ephemeral-session---ephemeral)
- [Opening a project from the command line](#opening-a-project-from-the-command-line)
- [Asynchronous plugin loading](#asynchronous-plugin-loading)

---

## Graph fundamentals

### Audio block format

- **Sample format**: 32-bit float, stereo (2 channels), nominal range `-1.0..+1.0`.
- **Block size**: defaults to 512 samples (`AudioEngine::blockSize`, `GraphProcessor::blockSize`). The audio device may negotiate a different size at startup — the engine adopts whatever `device->getCurrentBufferSizeSamples()` returns.
- **Sample rate**: whatever the audio device reports; SEANCE doesn't pick. Most internal time-based parameters (envelope times, crossfades, IR lengths) are stored in seconds and converted per-block.

### Audio device selection & persistence

- **Settings dialog**: **Settings → Audio Device…** opens JUCE's `AudioDeviceSelectorComponent` (driver type, input/output device, sample rate, buffer size, and MIDI input enablement). The microphone-capture dialog reaches the same dialog via its **"Audio device…"** button — both routes call the shared `SoundShop::launchAudioDeviceSettings()` helper (`dialog_helpers.cpp`).
- **Persistence**: SEANCE remembers your audio device choice across restarts. The full device setup (driver type, device names, sample rate, buffer size, channel selection) is saved to `soundshop_audio_settings.xml` next to the executable, written automatically whenever the device configuration changes (i.e. you pick something in the settings dialog) and on shutdown. On launch the saved XML is handed to `AudioDeviceManager::initialise()`, so your exact last-used device is restored. Implemented in `AudioEngine::saveAudioSettings()` / `changeListenerCallback()` (the engine registers as a `ChangeListener` on the `AudioDeviceManager`) and the restore path in `AudioEngine::init()`.
- **First-run default = Windows Audio (WASAPI).** When no settings file exists yet (a fresh install), SEANCE explicitly selects the **Windows Audio** (WASAPI shared-mode) driver type before initialising. This is JUCE's out-of-the-box default anyway, but it's set explicitly so the intent is clear and survives any change to JUCE's type ordering. Plain shared-mode WASAPI is the right default on **both** counts:
  - **Output** — clean and low-latency. Earlier builds forced DirectSound on first run, but its polling-based output path **crackles/glitches under load** — audible "vinyl-dust" clicks that get worse on a busy graph (e.g. a held piano-synth note with its release tail) — and that hit **every** user by default, including the large majority who only play notes, play songs, or capture from a file.
  - **Input** — a mismatched mic+speakers pair (different physical devices, e.g. a USB-webcam mic and an HDMI output) works on WASAPI *without* DirectSound, because **plain shared mode is the one WASAPI mode JUCE runs with the `AUTOCONVERTPCM | SRC_DEFAULT_QUALITY` stream flags set** (`supportsSampleRateConversion()` in `juce_WASAPI_windows.cpp` returns true only for `WASAPIDeviceMode::shared`, false for exclusive/low-latency). Those flags turn on WASAPI's built-in **per-endpoint** resampler/reformatter, so a mic running at its own native rate and format is converted independently of the output clock. That per-endpoint conversion is exactly what was missing when the old "welded default device" corrupted the captured input into square-wave garbage. To make sure each endpoint gets it, `ensureAudioInputEnabled()` opens the input **and** output as two *explicitly named* shared-mode endpoints rather than leaning on the auto-combined default device.
  - **Net:** clean output and a working mismatched-device mic on a single driver, no DirectSound tradeoff. DirectSound remains reachable as a **last resort** — via the **"Audio device…"** button — for the rare device whose format even the shared-mode SRC can't reconcile, but it is no longer the default. This is *only* a first-run default; the saved-settings path always takes precedence, so any deliberate driver choice (including DirectSound) sticks on subsequent runs.
- **Mic-capture troubleshooting note.** The microphone-capture dialog's hint text tells the user that if the input still sounds wrong (garbled, noisy, or like the computer's own audio) on the default Windows Audio driver, they can open **"Audio device…"** and switch the driver type to DirectSound as a last-resort fallback for a device the shared-mode resampler can't reconcile.

### Pin kinds (exact colors)

Four kinds only. RGB values are defined in `node_graph_component.cpp:colourForPinKind`:

| Kind   | Color         | RGB                | Rate                    | Carries                       |
|--------|---------------|--------------------|-----|-------------------------------|
| Audio  | Cornflower blue | `(100, 149, 237)` | per-sample audio        | stereo float audio            |
| MIDI   | Lime green    | `( 85, 205,  85)` | event-driven            | note-on/off, CC, pitch bend, aftertouch, program change |
| Param  | Orange        | `(255, 140,  40)` | one value per audio block (~700 Hz at 44.1 kHz / 512-sample blocks) | knob-modulation control signal |
| Signal | Amber         | `(255, 205,  55)` | per-sample              | audio-rate control signal (FM, sample-accurate envelopes) |

Cables inherit the color of the pin they're attached to (drawn by the same `colourForPinKind` lookup), so the wire color identifies the kind end-to-end. Param and Signal are intentionally in the same warm hue family — they're both control signals at different rates — while Audio (cool blue) and MIDI (cool green) sit in distinct hue families.

Connections are kind-checked: an Audio output won't connect to a MIDI input, etc. Adapter nodes (e.g. **MIDI Modulator**, **Trigger**) are how you cross kinds.

### Control signal range (0..1)

Every **control signal** — anything carried on a **Param** or **Signal** pin, or on the control channels (2+) of an audio buffer — is **unipolar `0..1`**. This is uniform across *all* control-signal producers: Signal Shape, Control Bank faders, XY Pad, MIDI Modulator, the Spectrum tap's bin outputs, and Lua / Wasm / Built-in scripts in the Signal role. **`0.5` is the neutral "no change" resting value**, `0` the minimum, `1` the maximum. (This is distinct from **Audio**, channels 0/1, which is legitimately bipolar `−1..1` — the convention only governs control signals.)

- **Why uniform.** A consumer never has to know which node fed it. Previously some sources emitted `−1..1` and some `0..1`, which meant e.g. a Control Bank fader dragged to `0` was misread as "−1 / push the target param to its minimum" instead of "no modulation" — the bug this convention fixes.
- **How consumers use it.** A node that modulates a parameter applies *bipolar-additive* modulation around the param's current base value: `modulated = base + toBipolar(signal) · depth · (max − min)/2`, where `toBipolar(x) = x·2 − 1`. So `0.5` → unchanged, `1` → `+depth` toward max, `0` → `−depth` toward min. A consumer that genuinely wants the raw `0..1` (e.g. a wavetable position axis) uses the signal directly. The conversions are centralised as `toBipolar()` / `toUnipolar()` in `signal_modulation.h`.
- **For expression / script authors.** The value you compute *is* the wire value, clamped to `0..1`. Math functions keep their natural range — `sin`/`cos`/`tan` (and `saw`/`square`/`triangle`/`noise`) return `−1..1`, so a bare `sin(...)` **clips its negative half**. Reach for the unipolar aliases `usin`/`ucos`/`utan` (and `usaw`/`usquare`/`utriangle`/`unoise`), or wrap a bipolar sub-expression in `unipolar(...)`; `bipolar(x)` converts the other way. These helpers exist in both the Built-in DSL and the Lua prelude.
- **Back-compat.** Projects saved before this change still load. Signal Shape *shape layers* pass through the shape pipeline unchanged (they're remapped to `0..1` automatically), so a legacy LFO behaves the same. The only manual exception is a Signal Shape **composition `expr`** someone hand-typed as raw bipolar math (e.g. `expr = sin(x*6.28)`); on the `0..1` wire its negative half now clips — re-wrap it as `usin(...)` / `unipolar(...)`. The default `expr = curve` is unaffected.

### Control inputs on parameters (Set vs Mod)

Any parameter shown in a node's body can be given an **on-demand control input pin** so a Param/Signal cable drives it, without cluttering nodes that don't need it. Right-click the node and the menu offers, per param:

- **Add Absolute Input (Set)…** — adds an input pin named **`Set: <param>`**. The cable's value *is* the parameter value, mapped edge-to-edge across the param's range: `0` → minimum, `1` → maximum (`depth` is unused). The knob is then **locked** — it greys out and can't be dragged, because there's no resting value left for the user to set; the cable owns it. This is how a host normally drives a plugin/VST3 parameter (always an absolute normalized value), and it's the **default** choice (listed first).
- **Add Modulation Input (Mod)…** — adds an input pin named **`Mod: <param>`**. The cable *modulates* the knob's resting value rather than replacing it: `modulated = base + toBipolar(signal) · depth · (max − min)/2`, so `0.5` (the neutral resting signal) leaves the param unchanged, `1` pushes toward max, `0` toward min. The knob **stays editable** — dragging it sets the **base** (the centre the modulation swings around), and the handle/number on the row track that base value, not the jittering live value. Double-clicking the row resets the base to the range midpoint.

Once a control input exists, the right-click menu also offers, for that pin:

- **Switch to Modulation… / Switch to Absolute…** — flips the existing pin between the two modes, relabelling its `Set:`/`Mod:` prefix and resetting the param to its resting value. Any cable already plugged in stays connected (pins are referenced by id).
- **Remove Input Cable Pin** — deletes the pin, any cable plugged into it, and the binding, then restores the param to its base value.

These two operations are reachable two ways: from the **param row's** right-click menu (as above), and by **right-clicking the control-input pin directly**. The pin right-click hit-test covers both the **pin circle itself** and the **whole pin row** (its label text):

- **The circle** is matched first via `pinAtPoint` (a generous-radius test against the real pin position) — important because the dot is drawn centred on the node's edge, so its outer half sits *outside* the node's bounding box and a plain bounds-contains test (`nodeAtPoint`) would miss a click there. A control-input pin also takes priority over the **cable plugged into it** (the cable terminates exactly at the pin, so the link hit-test would otherwise always win and there'd be no way to right-click the pin); regular pins still let the cable menu win at the endpoint.
- **The label row** is matched second, inside the node body. A row can carry an input pin (drawn left) and/or an output pin (drawn right). When the row has **both**, the click is split at the node's horizontal centre. When it has **only one** (e.g. a wavetable's input-only `Mod: Position 1` row), the **entire row width** hits that pin — *not* split at centre. This matters because a long input label like `Mod: Position 1` extends past the node's centre; splitting at centre would make clicks on the right half of the label look for a non-existent output pin and fall through to the node menu.

So you can aim at the dot, the `Mod:`/`Set:` prefix, or the axis name — anywhere on the row — and get the pin menu. Right-clicking a control-input pin opens a focused menu headed by the pin's name with just **Switch to…** and **Remove Input Cable Pin**; right-clicking any *other* (non-control) pin falls back to the node menu so a plain-pin right-click still does something useful. Both surfaces call the same shared `addControlInput` / `removeControlInput` / `switchControlInputMode` helpers, so they stay in sync and each commits one undo snapshot plus a graph rebuild.

**Pin hover highlight.** Moving the cursor over a pin lights up its *whole interactive region* — the dot **and** the readable label text, i.e. exactly the area a right-click resolves — with a faint white wash and a thin outline in the pin's wire colour. This previews "this is what you'll hit" the same way the cable hover highlight does. The highlight uses the **same** geometry as the right-click hit-test: both the dot (`pinAtPoint`, matched first, including the half that overhangs the node edge) and the label **row band** are resolved by the shared `pinInRowBand` helper, so the lit region can never disagree with what a click targets. On a row carrying both an input (left) and output (right) pin the highlight covers just the hovered pin's half (split at node centre); a single-pin row lights up its full width. A folded param-row control connector (`Set:`/`Mod:`, drawn with no separate label) highlights just its dot. The highlight is suppressed while a cable drag is in flight (the drop-target pin already gets its own enlarged glow) and clears on mouse-exit. Tracked by `hoveredPinId`, updated in `mouseMove` via `pinUnderCursor`.

When you **drag a cable** near a valid drop-target pin, that pin glows: it grows to ~1.4× and gains a soft outer halo, both drawn in the pin's **own kind colour** (audio blue / MIDI green / param-signal orange) with a white rim — *not* a generic yellow. Earlier this glow was always yellow regardless of pin type; now it reads as "this exact pin, lit up," keeping the wire-colour meaning consistent with everything else in the graph.

**In-place pin layout — a control pin renders on its own param's row.** A node body has two stacked regions below the header: a **top pin region** of *structural* I/O pins (audio / MIDI / signal that aren't bound to a param, interleaved input-left / output-right, one per row) and a **param region** of one row per param. A param's on-demand control pin (`Set:`/`Mod:`) is **not** drawn as a separate top-region row — it folds onto the **left edge of its own param's row**, so the pin dot sits directly beside the slider it drives. The full `Set:/Mod: <param>` label isn't repeated (the param name already occupies the row), but a compact **`Set`/`Mod` tag** is drawn in the pin's wire colour so the mode is still readable at a glance. The tag is **right-aligned just left of the param's value** (not after the dot), and the param name is **not** indented at all — the dot lives in the row's narrow left margin (the slider area starts a few pixels inset, so the dot never overlaps the label), so a pinned row's name stays perfectly aligned with every other row's. (Earlier the tag sat right after the dot and pushed the name right, which made pinned rows' labels visibly indented relative to their neighbours.) Consequences: **pinning a param no longer grows the node taller** (no extra top row is added) and **no longer reorders anything** (the slider stays put and simply grows a connector in place); the spatial pin↔param relationship is unambiguous even on a tall node. The layout is computed by a single source of truth — `numTopPinRows` (= `max(structural inputs, outputs)`) plus `structuralInputPins` / `isParamModPinId` in `node_graph_component.cpp` — shared by `getNodeBounds`, `getPinPosition`, `drawNode`, and every hit-test, so the drawn dot and its click target can't drift. Cabling to/from the in-place pin works unchanged (`pinAtPoint` reads `getPinPosition`, which returns the row-centre on the node's left edge); right-clicking the in-place dot opens the same pin menu; and the slider-drag zone begins just inside the left edge so a click on the dot grabs the pin while a click on the slider edits the value. This applies to **all** on-demand control pins, including the wavetable's `Mod: Position …` pins, which now sit on their Position param rows rather than at the top.

**Orphan-binding self-heal.** A control-input pin is recognised by its `Mod:`/`Set:` **name**, not only by the presence of a matching `modPin` entry. Some nodes can carry the pin on their face with no binding behind it — most commonly a wavetable's `Mod: Position X/Y/Z` pins loaded from a project saved before `modPin=` serialisation existed (or whose bindings were otherwise lost). For such a pin the binding also meant the incoming cable wasn't actually modulating anything, since `applySignalModulations` iterates `modPins`. When you right-click an orphan `Mod:`/`Set:` pin, `showPinMenu` **rebuilds the binding from the pin name on the spot**: it resolves which param the pin drives — an exact param-name match first (`Mod: Volume` → param `Volume`), then the wavetable Position quirk where the *pin* is labelled by axis letter (`Position X/Y/Z/W`) but the *params* are numbered (`Position 1…N`), mapping the letter/number to its ordinal among the `Position`-named params. It then creates the `modPin` (mode taken from the `Set:`/`Mod:` prefix), commits an undo snapshot, and requests a graph rebuild so the restored binding immediately drives audio. After this one right-click the pin behaves like any other control input.

**Lock semantics.** Only an *Absolute* ("Set") input locks the knob; a *Modulate* ("Mod") input leaves it draggable (you're editing the base). Locking is per-parameter — a node with one Set-driven param keeps every other param fully editable. The lock is computed by `Graph::paramHasAbsoluteInput`; the looser `paramHasSignalInput` (any connected control pin, either mode) is no longer used for the UI lock.

**Live value display on the slider.** Because the node body repaints at 30 Hz, a modulated param's slider shows what the incoming signal is doing in real time, and the two modes draw it differently because they mean different things:

- **Set (Absolute):** the cable's value *is* the param value, so there's a single number to show. The fill bar and a **dimmed-orange marker** track the live driven value every frame (the knob isn't draggable while Set-locked, so the marker is orange, not the bright-white grabbable handle). Watching the orange marker sweep is the quickest confirmation that a signal is actually reaching the param.
- **Mod (Modulate):** there are *two* values — the **resting/base** value you set (still draggable) and the **live modulated** value the signal swings it to. The bright-white handle stays at the base value; a separate **cyan marker** (a thin full-height line plus a caret at the bottom edge) shows the live modulated value. Cyan matches the "signal-modulation attached" dot drawn after the param name, so the marker and the dot read as one concept.

**Idle eager pins don't clobber the value.** Some control pins are created *eagerly* even when nothing is plugged in — most notably the wavetable's `Mod: Position …` pins, which always exist so you have somewhere to drop a cable. A `modPin` binding can therefore exist with no cable feeding it. `applySignalModulations` skips any binding whose pin has no incoming cable (tracked by a runtime `ModPin::connected` flag recomputed from `graph.links` on every graph rebuild, the same recompute-on-build discipline as `Node::reachesOutput`). Without this guard the apply step would read the pin's *silent* control channel as a genuine `0.0` modulation and force the bound param to its minimum every block — which is exactly why an unconnected Position axis used to peg to 0 and make its manual slider do nothing. With the guard, an unconnected pin leaves the slider in manual control and a connected one modulates as normal; disconnecting restores the param to its base value.

**Persistence.** Each binding serializes as `modPin=paramIdx,pinId,depth,mode` where `mode` is `0` (Modulate) or `1` (Absolute). The mode field is optional on load — projects saved before Set/Mod existed have no mode field and default to **Modulate**, matching the original always-modulate behaviour. These lines are written immediately after the node's `[Param]` blocks and carry no section header of their own, so on load the active section is `[Param]`, not `[Node]`, when they are read. The loader therefore parses `modPin=` *before* the per-section dispatch (it binds to the most recently seen node); a regression in which the handler lived only in the `[Node]` branch silently dropped every binding — and its pin — on reload. The `pin round-trip` self-tests round a frame-scope warp param, its `modPin`, and its `Mod:` pin through `writeProject`→`readProject` to guard against this.

The wavetable's auto-created `Mod: Position …` pins (see [Position parameters](#position-parameters)) are exactly these on-demand pins, kept in sync with the axis count; they default to Modulate but can be switched to `Set:` per axis.

### Cable interaction (hover, select, gain, delete)

- **Hover highlight** — moving the cursor within ~13 px of a cable lights it up: full opacity, a thicker stroke, brightened colour, and a soft glow halo drawn underneath. The same soft halo is also drawn around the cable's three circles — the two **end pin dots** and the **middle identity circle** — so the whole connection reads as one lit-up object rather than a glowing line with un-lit beads on it. The highlighted cable is the exact one a click or right-click will act on, so it doubles as a "this is what you'll hit" preview. The emphasised cables (hovered *and* selected) are repainted **after the nodes**, so the highlight stays visible even where a cable runs underneath or close to a node box (short cables between adjacent nodes used to be fully occluded). The highlight also persists while the right-click cable menu is open, because emphasis is `selected || hovered` and a right-click selects the cable it targets.
- **Closest-cable targeting** — when several cables overlap (common where an audio cable and a Signal/Param modulation cable run between the same pair of nodes), the hit-test (`linkAtPoint`) picks the cable whose curve passes *closest* to the cursor, not merely the first in draw order. This is what makes an overlapped modulation cable reachable for selecting/right-clicking/deleting. Dangling cables whose endpoint pins no longer exist are skipped (no phantom hot-spot at the canvas origin).
- **Pin connection targeting** — when you drag a cable, the drop hit-test (`pinAtPoint`) returns the *closest* pin to the cursor and only considers pins on the **opposite side** from where the drag started (dragging from an output looks only for inputs, and vice-versa). This fixes a class of silent connection failures: previously the hit-test returned the first pin in iteration order and always preferred outputs, so dropping onto an input that happened to sit near some output pin (the source node's own output, or an adjacent node's output) resolved to that output and the direction check quietly refused the connection. It was most visible dragging `Signal Out` onto a synth's bottom-left `Aftertouch` input.
- **Left-click** selects a cable (3 px stroke). **Delete**/**Backspace** removes the selected cable.
- **Right-click** opens the cable menu, headed by the wire's signal type in plain language so non-musicians can tell what they're acting on: **Audio – the sound itself**, **MIDI – notes & controllers**, **Param – smooth control values**, or **Signal – fast control values**. The Param and Signal headers spell out the *exact* update rate computed from the project's live sample rate and block size — e.g. at 44.1 kHz with a 64-sample block, Param reads `updates 689x/sec, once per 64-sample block` and Signal reads `updates every sample, 44.1k/sec`. When the audio device hasn't started yet (rate unknown) a generic phrasing is used. When the two endpoints differ (an implicit Param↔Signal conversion) the header names both kinds with the rate change, e.g. `Signal → Param (resampled to 689x/sec)`. The values are sourced via the `getAudioFormat` callback wired from `main_window` to the audio engine (`getSampleRate()` / `getBlockSize()`). Below the header: **Delete Connection**; **Gain** submenu (0/−3/−6/−12/−20/+3/+6 dB presets + **Custom…**, range −60..+24 dB); **Effect Group** membership. Cable right-click takes priority over the node under it.
- **Connection gain** inserts a gain stage on that cable. Because changing a cable's gain doesn't alter node/link counts, the gain handler explicitly requests an audio-graph rebuild so the change is audible immediately (the gain stage is only present in the rebuilt graph when `gainDb != 0`; setting it back to 0 dB removes the stage). Cables attenuated below −10 dB are drawn dimmer as a visual cue.

### Navigating the graph (zoom, pan, Fit All, Auto-Fit Graph)

The node-graph canvas has a free `zoom` (0.1×–4×) and `panOffset`. You **zoom** with the mouse wheel (centred on the cursor) and **pan** by dragging empty canvas. Two menu helpers frame the view automatically:

- **View → Fit All** — a one-shot reframe that zooms/pans so the **whole graph** fits on screen with a 100 px margin (`applyFitBounds` clamps the fit zoom to `0.1×–1.5×` and centres the bounding box). Use it any time the graph has wandered off-screen or you've zoomed into a corner.
- **View → Auto-Fit Graph (always show whole graph)** — a checkable, persistent toggle (off by default) that keeps Fit All applied **continuously**. While on, SEANCE re-frames the canvas whenever the graph's overall bounding box changes — when you add or remove nodes (including snapping MIDI/Timeline tracks in or out of the view), when a node is moved (it re-frames once on drag release), and when the window is resized — so the entire graph always stays visible without manual zooming.
  - **Implementation.** `NodeGraphComponent` runs a 15 Hz `Timer` that compares the all-nodes bounding box (`computeAllNodesBounds`) against the last fitted box and only re-fits when it actually changed — a single centralised change-detector rather than a `fitAll()` call sprinkled at every add/remove site. The timer skips re-fitting during an in-progress drag (so dragging a node doesn't fight the view) and before the initial project-load fit (`pendingInitialFit`). Window resizes re-fit immediately from `resized()` since the timer only watches node bounds, not the viewport.
  - **Manual override releases the lock.** Any **manual zoom** (mouse wheel) or **empty-space pan** (drag on blank canvas) immediately turns the toggle **off** (`releaseAutoFitForManualView`) — the moment you frame the view yourself, SEANCE stops overriding you, unticks the menu item, and persists the off state. **Moving a node does not** release the lock (that's editing the graph, not the view), so the view re-frames around the moved node on release and auto-fit stays on.
  - **Persistence.** The toggle is saved to `soundshop_prefs.xml` (`autoFitGraph` attribute) next to the executable and restored on launch, applied to the graph after preferences load.

### Node visual category

Node color in the graph is **inferred from the node's pins** by `getVisualCategory`, not stored per-node-type. Add a new I/O pattern and you may need a new visual category. The categorisations seen by the user (Input / Timeline / Instrument / Effect / Signal shape / Output) are presentation; internally each is a function of pin shape.

### Graph mutation threading

All graph mutations (add/remove node, add/remove link, clip add) must hold `NodeGraph::mutationLock` for the duration of the change so the audio callback never iterates `graph.nodes` or `graph.links` mid-mutation. The audio callback takes a non-blocking try-lock and outputs silence if it can't acquire it; batch entry points hold a `std::lock_guard`. Single-node UI actions (right-click Add/Delete, drag-link, inline rename) are still on the to-do list — see `known-issues.md`.

### Plugin-delay compensation (PDC)

SEANCE compensates for per-node processing latency so a processed branch stays time-aligned with parallel/dry branches when they re-converge. This is **handled by JUCE's `juce::AudioProcessorGraph`**, which SEANCE uses as its render engine: when the graph builds its render sequence it reads each node's `getLatencySamples()`, computes the maximum-latency path to every mix point, and inserts compensating delay lines on the shorter branches. Both hosted third-party plugins (added straight to the graph in `graph_processor.cpp`, their `getLatencySamples()` read automatically) and any built-in processor that reports a latency are covered for free. The graph itself reports the total path latency via its own `getLatencySamples()`.

JUCE only recomputes this when the graph is **rebuilt**, so SEANCE must rebuild whenever a node's latency changes *at runtime* (e.g. a plugin switching modes, or a future built-in toggling a high-quality latency-bearing mode). `GraphProcessor` registers a `LatencyChangeListener` (`graph_processor.h`) as an `AudioProcessorListener` on every node during `rebuildGraph`; its `audioProcessorChanged` checks `details.latencyChanged` and sets the `rebuildRequested` flag, so the next block rebuilds the render sequence and re-derives the compensation. (Re-adding the listener on each rebuild is safe — `AudioProcessor::addListener` dedups.) A `pdc:` self-test in `self_test.cpp` guards this by wiring a latency-reporting node against a parallel dry path through a real `AudioProcessorGraph` and asserting the dry path is delayed to match (and that the graph reports the max-path latency).

Because PDC works, latency-bearing DSP designs are now viable in SEANCE. The shipping **Curve EQ** is still zero-latency by construction (a deliberate simplicity choice, not a PDC limitation); a latency-bearing high-quality EQ mode is a noted backlog item.

**Seeing a node's latency.** Right-clicking any node shows a disabled readout near the bottom of its menu:

- **Latency (this node)** — the node's own added delay, in samples and (when the audio device is running) milliseconds. Most built-in nodes report **0**; the figure is non-zero mainly for hosted plugins doing lookahead or linear-phase processing.
- **Latency (combined here)** — the cumulative delay the signal has accumulated by the time it leaves this node: the **largest** summed latency along any path of nodes feeding its inputs, **plus** this node's own. (Max, not sum, of the incoming paths — that's the value PDC aligns every branch to.) **Both lines are always shown**, even when they're equal (a source node, or an all-zero-latency chain) — so the combined figure is never ambiguous by its absence. When equal, it's confirming nothing upstream adds delay.

**On the node face.** When a node's accumulated "to here" latency is non-zero, a small orange-bordered pill at its **bottom-left** shows that delay in ms (or samples if the audio device isn't running yet) — so latency is visible at a glance without opening the menu. The badge is drawn **only** for non-zero nodes (so a normal all-built-in graph shows none) and is hidden when zoomed out too far to read; right-click for the full own-vs-to-here breakdown.

The numbers come from `GraphProcessor::snapshotNodeLatencies()` — a thread-safe copy of the `LatencyChangeListener`'s settled per-node latencies, keyed by stable node id. The listener tracks each node's **real** processor (the `nodeInputMap` side), not the trailing pan node on the `nodeMap` side, so a panned node still reports its true delay rather than the pan processor's zero. `NodeGraphComponent::cumulativeLatencyTo()` does the "to here" computation as a memoised, cycle-guarded walk up `graph.links` (pure UI-side graph traversal, no audio-thread access); `paint()` precomputes the badge totals once per frame and only when something actually reports latency, so zero-latency graphs do no extra work.

### Node freeze and caching

Any node (except the **Output** sink) can be **frozen** — SEANCE renders its output to a PCM buffer once and plays that buffer back instead of re-running the node's DSP every block. This saves CPU on an expensive branch (a heavy synth, a plugin chain, a convolution) and lets you commit a "take" so later upstream edits can't change it. Freeze state lives in each node's `AudioCache` struct (`node_graph.h`).

**Freezing a node.** Right-click a node → **Freeze (cache audio)**. SEANCE does a single offline render of the whole project (length = the last clip's end + a 4-beat tail), taps that node's *own* output, and stores the captured left/right PCM in the node's cache (`enabled = valid = true`). While frozen the node shows a bold **FROZEN** tag at the top-right of its title — **cyan** if the cache is backed by an on-disk file, **limegreen** if it's in memory. Right-click → **Unfreeze (disable cache)** clears it and the node goes live again.

**What "tap its own output" means.** The render adds a hidden sink processor (`FreezeTapProcessor`) to the JUCE render graph as an extra fan-out from the node's output pin, then renders once. Each tap records exactly the audio that node emits — not the full mix bus — so a freeze isolates the node's signal correctly. (This replaced an earlier implementation that stored the whole output mix regardless of which node you froze.)

**Batch freeze (arm-then-render).** Freezing N nodes one at a time costs N full-project renders. Instead you can **arm** any number of nodes and freeze them all in **one** pass: right-click a node → **Arm for batch freeze** (toggles; re-selecting it is **Disarm from batch freeze**). Armed nodes show a bold amber **ARMED** tag at their title's top-right. Once at least one node is armed, every node's menu also offers **Freeze N armed node(s) (one pass)**, which renders the project a single time and captures each armed node through its own tap simultaneously (`MainContentComponent::freezeNodes`; single-node freeze is just `freezeNodes({id})`). Arming is **transient** — it is never saved, and a reload starts with nothing armed (`Node::armedForFreeze`, not serialized).

**Auto-cache (automatic memoization).** Separate from manual freeze, each node has an **auto-cache** flag (on by default, right-click → **Disable/Enable auto-cache**). When a node is **deterministic** (no live/unpredictable inputs — no live MIDI, no hardware, nothing whose output can't be reproduced from saved state), the graph processor may transparently reuse a cached render keyed by an **input hash** (`AudioCacheManager::computeNodeHash`), re-rendering only when the hash changes. A node that isn't deterministic shows a small **orange dot** instead of a FROZEN/ARMED tag. Disabling auto-cache forces the node to always render live.

**Persistence.** Manual freezes survive save/reload. On save, each frozen node's PCM is written to a sibling `soundshop_cache/node_<id>.cache` file (node-id-keyed, so freezes never collide), and the cache **metadata** (enabled/valid/useDisk/hash/sampleRate/numSamples, plus the auto-cache preference) is serialized into the project file (`project_file.cpp`, gated on `includeBlobs` so undo snapshots omit the heavy payload but keep the preference). On load, `MainContentComponent::rehydrateNodeCaches` points the cache manager at that folder and re-attaches each freeze to its file **lazily** — the audio thread pages the samples in on first playback. If a cache file is missing (project copied without its `soundshop_cache` folder), the freeze is dropped so the node renders live rather than playing silence.

**Playback.** A frozen node is substituted in the render graph by a `CachePlaybackProcessor` (`cache_processor.h`) that plays `cache.left/right` at `transport.positionSamples - startSample`, returning silence past the ends. The **Output** node is never cached (it's the live mix bus). Freezing/unfreezing requests a graph rebuild so the change takes effect immediately.

---

## Transport bar

### Position / song-length display

The green monospaced readout on the transport bar shows the playback position **and** the total song length, each in two forms separated by `   ` (three spaces):

```
0:00.0/15:30.0   Bar 1:1.0/20
```

- **Left of each pair — current position:** elapsed wall-clock time as `minutes:seconds.tenths` (starts at `0:00.0`), and musical position as `Bar:Beat` (both 1-based, so a song starts at Bar 1, Beat 1.0).
- **Right of each `/` — total length:** total time as `minutes:seconds.tenths`, and the total bar count.

The total is computed from `NodeGraph::effectiveSongLengthBeats()`: the explicit length set via the **Song** button if one is set (`songLengthBeats > 0`), otherwise auto-derived from the last clip across all timeline nodes. **When the project has no clips and no explicit length, there is nothing to total, so the `/total` halves are omitted** and the readout falls back to just the current position (`0:00.0   Bar 1:1.0`).

Total time is derived by converting the total beat count through the tempo map, so it respects tempo-map changes (it is not a naive `beats × 60/BPM`). The total bar count is the bar the song's end falls in; when the length lands exactly on a downbeat (beat 1.0) the song fills the *previous* bar, so an 80-beat 4/4 song reads `/20`, not `/21`.

### Lit button states

Two transport buttons light up with a shared blue accent (`RGB(64,132,223)`) to show an active state:

- **Play** is lit blue **while audio is actually playing** and reverts to its normal look the moment playback stops — including programmatic stops, e.g. the song auto-stopping at its end. (It tracks the engine's real playing state, not just the Play/Stop clicks.)
- **Loop** is lit blue **while looping is enabled** and unlit otherwise. The state follows every way looping can change — the button itself, a project load, undo/redo, the piano-roll loop-region menu, or a tracker import.
- **Auto** turns **red** (`RGB(200,60,60)`) while the global automation-record mode is armed (Touch or Latch) and shows its default look when Off. See [Automation recording](#automation-recording-knob-drag-capture) below.

The blue is deliberately distinct from the green/red used by the **Metro**, **Mon** (monitor), and computer-keyboard-MIDI toggles. An un-lit transport button uses the default button colour (no hardcoded grey), so it matches every other un-lit button.

### Spacebar toggles play/stop

Pressing **Space** toggles the transport, matching the universal DAW convention: press once to start playing from the playhead, press again to stop. The spacebar is the default binding for the **Play** hotkey (`HotkeyManager`); the callback checks `transport.playing` and routes to the same `onStop()` / `onPlay()` handlers the on-screen **Play/Pause** and **Stop** buttons use. Because it goes through those handlers (not the raw engine calls), the play-from-top-when-parked-at-end logic, recording teardown, the Play/Pause button label, and the stop-silences-everything behaviour below all stay in sync no matter whether you click or use the key.

### Stop silences all trailing sound immediately

When the transport stops, **all trailing sound is cut at once** rather than ringing out — synth amp-envelope release tails, reverb / echo / delay / convolution buffers, sustained voices, and hosted-plugin tails all go silent immediately. This matches what every mainstream DAW does on Stop.

Mechanism: `AudioEngine::stop()` calls `panic()`, which sets a one-shot `std::atomic<bool> panicRequested` flag on the `GraphProcessor`. On the very next audio callback the flag is consumed and `juce::AudioProcessorGraph::reset()` is called, which propagates `reset()` to **every** node processor in the graph. Each tail-bearing built-in processor implements `reset()` to wipe its state:

- **Synths** — clear their voice containers / hard-reset per-voice amp envelopes (FM, PD, Additive, Particle, Spectral Grain, Built-in, Terrain, Drum, MultiSampler, Signal Oscillator, Signal Noise, SoundFont/SFZ, sfizz).
- **Time-based effects** — zero their delay/feedback/tail buffers (Echo, Reverb, Wavelet Reverb, Vibrato, Flanger, Phaser, Convolution).
- **Hosted VST3/plugins** — receive the standard `reset()` that JUCE forwards to the plugin.

The reset is a **one-shot state wipe, not a permanent mute**: the graph keeps processing afterwards, so audition / musical-typing while stopped is unaffected. Memoryless processors (filters, EQ, compressor, distortion, etc.) need no `reset()` since they produce no audible tail.

---

## Recording live audio (mic / line-in)

SEANCE can capture live audio while the song plays, and the take **sticks**: it lands on the timeline as an ordinary audio clip backed by a WAV on disk, so it plays back with everything else on the next pass and survives save/load.

### Arming tracks

Recording is **per Audio Track node**, and you arm as many as you like:

1. Create an **Audio Track** node for each input you want to capture.
2. Set that node's **Input Channel** param to the hardware input channel it should listen to (0 = the first input on the audio device, 1 = the second, and so on).
3. Right-click the node → **Record Here** to arm it.
4. Press **Play & Record**.

Every armed track with a valid input channel records **simultaneously and independently**, each to its own file and its own clip. There is no fixed limit on how many — the ceiling is however many input channels the audio device actually presents. Two mics into a 2-in interface means two armed Audio Tracks on channels 0 and 1, and you get two separate tracks of audio, not one interleaved blob.

Each track's **Input Monitor** toggle mixes its input channel into the output while you play, so you can hear yourself; it's applied with the node's own Volume and Pan.

### Tracks are created for you (Settings → Auto-Create Tracks for Audio Inputs)

You do not actually have to do step 1 and 2 by hand. With **Auto-Create Tracks for Audio Inputs** on (the default), pressing **Play & Record** first walks every **active input channel on the current audio device** and makes sure each one has somewhere to land:

- An input that **already has an Audio Track** pointed at it re-uses that track — it is simply re-armed. Nothing is duplicated, ever, no matter how many times you press record. (The "is this input covered?" test is on the track's **Input Channel** param, not on whether it happens to be armed right now: a finished take disarms its track, so an armed-only test would spawn a fresh duplicate on every second press.)
- An input with **no track yet** gets a new Audio Track, which is:
  - **placed where it fits** — the same free-slot search the **+ Audio Track** button uses, so it never lands on top of an existing node, and the canvas re-fits if the new node fell outside the current view;
  - **named after the input**: the kind (**Mic** / **Line In** / **Input**, inferred from the device and channel names), the channel number (only when there's more than one input, so a single-mic setup just says `Mic`), and the shortened device name — e.g. `Mic 1 (C615)`, `Line In 2 (Focusrite)`. A name already taken by another node gets a numeric suffix (`Mic 1 (C615) 2`);
  - **armed** on that channel, with its **Input Channel** param set;
  - **cabled to the Output node**, so the take is audible on the next pass without you dragging anything. Several tracks feeding one input pin is fine — the audio graph sums connections that share a destination;
  - **empty** — no placeholder clip, unlike a hand-made Audio Track.
- Creating tracks is a single undo step (**"Add track(s) for audio input(s)"**), separate from the **"Record audio"** step the take itself produces.

Turn the setting off if you want the old fully-manual behaviour, where only tracks you built and armed yourself are ever recorded into.

### Hearing the track you're recording into (Settings → Play Tracks Back While Recording Them)

While a take is in progress, the track being recorded into is **muted by default**. Otherwise you'd hear the previous take on that same track playing back underneath the one you're performing, which is confusing and — with input monitoring on — a feedback risk.

The mute applies only when **both** conditions hold:

1. the track is **mid-take right now** (`Node::recordingNow`, set by `MultitrackRecorder::startRecording` and cleared by `stopRecording`), **and**
2. **Settings → Play Tracks Back While Recording Them** is **off**.

Every other track in the project keeps playing normally — you still hear the song you're performing along to. Turning the setting on mid-take takes effect immediately, because the two facts are stored separately and recombined every block rather than baked into one flag.

Implementation notes: the rule lives in `PanProcessor::currentlyMuted()`, alongside the ordinary mute and solo rules, so the record mute inherits `muteFader`'s declicking for free and never clicks when a take starts or ends. `Node::recordingNow` is **transient session state** — never serialized, never part of an undo snapshot — and `MultitrackRecorder::clearRecordingFlags()` resets it across the whole graph on any path that swaps the graph wholesale (new project, open project, undo), so a node can't be left silent with nothing in the UI to explain why.

`testAutoInputTracks` in the self-test covers all of this offline: creation, naming, wiring, the no-duplicate re-use path, the numeric suffix, and all four states of the two-condition mute.

### What happens on stop

Pressing **Stop** finalizes every armed take:

- Each track's audio is written to a **24-bit mono WAV** in a `recordings/` folder next to the executable, named `<track name>_<timestamp>.wav`.
- A clip pointing at that file is appended to the track, colored red, starting **at the beat the playhead was on when you hit record** — not at beat 0.
- The track is **disarmed**, so an accidental second pass can't overwrite the take.
- The whole thing is committed as a single undo step, **"Record audio"**, so <kbd>Ctrl</kbd>+<kbd>Z</kbd> removes the take and the project is correctly marked dirty.

Because the clip is a normal audio clip, everything that works on audio clips works on it: slip, trim, move, nest the track under another track, bounce, export.

**Clip position is stored node-local.** The playhead reports an *absolute* beat, but `Clip::startBeat` is relative to its node, and `AudioTimelineProcessor` adds the track's `absoluteBeatOffset` back on at playback. Both recorders therefore subtract that offset when placing the clip — otherwise recording into a [nested track](#track-header-strip-parenting--time-offset) would double-count the nesting offset and the take would play late by exactly the parent's offset. Both directions are regression-tested by `testMultitrackRecording`, which records two inputs into nested tracks and plays the result back.

### Disk overruns are reported, never silent

Samples go from the audio callback straight into a `juce::AudioFormatWriter::ThreadedWriter` per track, which hands them to one shared background thread that does the file I/O. The audio thread never allocates and never blocks on the disk, so **takes have no length limit** — they run as long as the drive has room.

Each track's FIFO holds **four seconds** of headroom, so an ordinary disk or antivirus stall doesn't cost you audio. If the drive still falls behind and the FIFO overruns, the lost samples are **counted, logged, and reported in a dialog on stop** ("Recording dropped audio", with the number of seconds lost). A take with gaps in it is never handed back silently — only the performer can decide whether to keep it or redo it.

---

## Automation recording (knob-drag capture)

SEANCE can **record the knob/slider moves you make during playback** straight into the target parameter's automation lane, so the move plays back the next time — the classic DAW "automation write" workflow. This is distinct from **Freeze** (which renders a node's *sound* to audio to reproduce it exactly, including free-running LFO phase and internal randomness): automation recording captures **authored parameter motion**, not the rendered signal.

### The three record modes

The **Auto** button on the transport bar cycles the **global record mode**: **Off → Touch → Latch → Off**.

- **Off** — nothing is recorded, ever. This is a **hard gate**: no per-node or per-param override can record while global is Off, so loading a project or hitting Play can never silently overwrite your automation. (This is why "Write" is not offered globally — see below.)
- **Touch** — a param records **only while you hold it**. Grab the knob, drag, release — the points under the drag are (over)written; everywhere you *didn't* touch keeps its existing automation. This is punch-in/punch-out per gesture.
- **Latch** — a param starts recording the moment you **first grab it** and keeps recording (at the last value) **until the playback pass ends** (transport stop), even after you let go. Use it to overwrite from a point onward.

A fourth mode, **Write**, exists only as a **per-node or per-param override**, never globally: it records the **entire pass** at the control's current value whether or not you touch it — the "flatten this param to a static value across the whole song" tool. Because a single global switch set to Write could wipe every lane in the project on the next Play, Write is deliberately scope-limited.

### The record cascade (global → node → param)

Every param resolves its effective record mode from three levels, most-specific first:

1. **Param override** (`Param::armMode`) — right-click a param row → **Automation** submenu → Inherit / Off / Touch / Latch / Write.
2. **Node override** (`Node::armMode`) — right-click the node → **Automation** submenu → same choices; the default for every param on that node.
3. **Global** (the **Auto** button).

`resolveArmMode(global, node, param)` (in `node_graph.h`) implements this: **Off/Inherit global is a hard gate returning Off**; otherwise a param override wins, else a node override, else the global mode. The param-row **Automation** submenu shows the *resolved effective mode* in its header so you can see what will actually happen. **Global arm is session-only** (never saved); node and param overrides **are** saved with the project.

### The read (playback) axis — muting lanes without deleting them

Independently of recording, you can **mute automation on playback** non-destructively:

- **Node → Automation → Ignore automation on this node** (`Node::ignoreAutomation`) — mutes *every* lane on the node.
- **Param row → Automation → Bypass this lane** (`Param::bypassAutomation`) — mutes one param's lane.

`automationReadEnabled(node, param)` gates the read path; the points stay on disk and resume driving the param the moment you un-mute. Both toggles are saved with the project. There is deliberately **no global read-mute** — muting is always node- or param-scoped.

### How a pass works (capture pipeline)

All point-writing happens in **one place** — the UI playback timer (`MainContentComponent::timerCallback`). The graph editor's knob-drag only flips a per-param `recWriting` flag via `onParamGesture → handleParamGesture`; the timer does the rest:

- **Play-start** (`beginAutomationPass`) arms every param whose resolved mode is **Write** (they record for the whole pass) and resets each param's sweep cursor.
- **During playback**, for each param with `recWriting` set, `recordAutomationPoint` **sweeps out** any existing points in `(lastBeat, currentBeat]` and inserts a new point at the playhead — so re-recording over a region cleanly replaces it. Absolute-cable-driven ("Set") params are never recorded (the cable owns them).
- **Touch release** clears `recWriting` and resets the cursor, so the untouched remainder of the pass keeps its old automation (punch-out). **Latch** keeps `recWriting` set until stop.
- **Stop** (`endAutomationPass`) runs a collinear-point **simplify** on each recorded lane (epsilon = 0.5 % of the param's range) and commits **one undo snapshot** ("Record automation") for the whole pass, marking the project dirty.

### On-screen feedback

The graph makes capture state visible ("signal flow is always visible"):

- An **armed** param (resolved mode ≠ Off under the current cascade) shows a **hollow red dot** after its name.
- A param **recording right now** gets a **solid red outline** around its whole row.
- A param whose lane is **muted** (node-ignore or per-param bypass) shows a **grey dot** (only when it actually has points).
- A node with a **record-mode override** shows an **`A:<mode>`** badge in its title; a node with **ignore-automation** shows a **slashed-A** badge.

### Hosted VST3/AU plugin knobs

Dragging a knob **inside a hosted plugin's own editor window** during playback **is** recorded, the same way native knobs are. Because a plugin's parameters aren't SEANCE native param rows (they live behind JUCE's `AudioProcessorParameter` interface), their recorded lanes are stored separately on the node (`pluginParamAutomation`, keyed by plugin-param index, normalized 0..1) rather than in a `Param` row — but from the user's side it behaves identically: arm **Auto → Touch/Latch** (or a per-node **Write** override), play, wiggle a knob in the plugin window, stop, and it plays back. While a plugin knob is recording, the **whole node is outlined in red** (plugin params have no on-body rows to glow individually).

Under the hood SEANCE listens for the plugin's own `AudioProcessorListener` gesture/value callbacks (queued off the audio thread, drained by the UI timer), samples the live normalized value each tick, and writes back on playback via `setValue` — which does **not** notify listeners, so playback can't feed back into the recorder.

**Caveat — gesture-less plugins.** Some plugins move a parameter without sending `begin/endChangeGesture` (only a bare value-change). Touch still works for these: it arms on the first change and ends after a short (~250 ms) idle gap instead of on gesture-release. Latch and Write are unaffected (they run to Stop). If a particular plugin's Touch captures feel clipped, use **Latch** or a per-node **Write** override instead.

**MIDI-learned CC moves are captured too.** If you've MIDI-Learned a hardware CC to a plugin parameter (right-click the knob → MIDI Learn), moving that controller during playback records into automation exactly like dragging the plugin knob directly — arm Touch/Latch/Write, play, twist the physical knob, stop, and it replays. Because the learned CC is applied on the audio thread (bypassing the plugin's listener callbacks), SEANCE captures the matched CC target separately and samples the resulting live value each tick. The same gesture-less Touch caveat applies (arms on the first CC move, ends on the ~250 ms idle gap).

---

## Computer Keyboard node

The Computer Keyboard node turns the QWERTY rows into a MIDI controller. SEANCE auto-creates one at the top-left of every new project, pre-wired to the default MIDI Track.

### Key mapping

Implemented in `main_window.cpp:keyToMidiNote()` (around line 5171):

| Keys                | Role                       |
|---------------------|----------------------------|
| `A S D F G H J K L` | white keys, starting at C  |
| `W E   T Y U   O P` | black keys (gaps where there's no black key) |
| `Z`                 | octave down                |
| `X`                 | octave up                  |

Octave range is clamped to **0..8**. The displayed octave is the C-row's octave; `A` plays C, `S` plays D, … `K` plays B, `L` plays the next C up.

### Velocity from modifiers

Defined in `main_window.cpp:5222-5224`:

- Plain key — **velocity 90** (medium)
- `Shift + key` — **velocity 120** (loud)
- `Alt + key` — **velocity 50** (soft)

These three discrete steps replace continuous velocity sensitivity; for real velocity expression, plug a hardware controller into a MIDI Input node.

### Toggle and focus

The **Keyboard MIDI** toggle in the main toolbar enables/disables keyboard input globally. The main window must also have keyboard focus — clicking the graph area is enough. When the toggle is off, A–Z are free for other shortcuts.

---

## MIDI input and routing

Every input device — computer keyboard, hardware MIDI keyboards, drum pads, network/virtual MIDI clients — is a **node in the graph**. There is no separate "MIDI routing matrix"; the cable is the routing.

### Hardware MIDI

- On a fresh project, SEANCE pops a wizard that lists every MIDI input device currently detected. Checking devices and clicking *Add Selected* creates a MIDI Input node per device.
- Re-open the wizard any time via **Options → Add MIDI Input Device…** — useful after plugging in a new device or after deleting an input node.
- **Hotplug**: connecting a new MIDI device while SEANCE is running pops a small dialog asking *Add* or *Ignore*. Ignored devices stay ignored until next restart or until you re-open the wizard.

### MIDI Track input

Every MIDI Timeline node has a MIDI In pin on its left side. Wire an input node's MIDI Out to it and live events are merged with the track's clip playback into a single MIDI Out. To audition without recording, wire the input node directly to a synth.

### MIDI Learn (CC mapping)

Right-click any knob/slider → **MIDI Learn** → move the controller's knob → mapping captured. Stored per-project in `project_file.cpp` so mappings persist across save/load.

**Filtering rule**: a CC that has been learned to a control is **removed from the cable stream** so it only affects the mapped control. Notes, pitch bend, aftertouch, and unmapped CCs pass through cables normally. The MIDI Modulator's outgoing CCs are *not* subject to this filter — see the [MIDI Modulator](#midi-modulator) section.

### Built-in controller responses

Every tonal synth is its own standalone `juce::AudioProcessor` (constructed by node type in `graph_processor.cpp:533-558`) — there is **no** single shared synth backend. Controller handling therefore lives in two places, and the coverage differs by synth:

- **`TerrainSynth`** (`terrain_synth.cpp`) implements the full controller set — pitch bend, mod-wheel vibrato, sustain pedal, channel + poly aftertouch — with per-MIDI-channel state arrays (`pitchBendFactor[16]`, `modWheel[16]`, `sustainPedal[16]`, `channelAftertouch[16]`). The **Piano, Waveform, and Sampler** node types are TerrainSynth configurations, so they inherit all of it. **Drum Machine** uses its own `DrumSynthProcessor` with an analogous (freeze-the-decay) sustain path.
- The **standalone synths** — Wavetable (`BuiltinSynthProcessor`), FM, Phase Distortion, Additive, Spectral Grain, Particle Cloud — handle the *per-note expression* subset (note on/off, pitch bend, channel pressure, polyphonic key pressure, CC#74 timbre) via the shared `distributeMpeMessages()` helper in `signal_modulation.h`. They do **not** implement mod-wheel vibrato or the sustain pedal; "Sustain" on these synths is the ADSR sustain *level*, not the CC#64 pedal.

The rows below describe each controller's behavior; the "Implemented by" notes call out which path applies.

| Control                 | Behavior                                                                      |
|-------------------------|-------------------------------------------------------------------------------|
| **Pitch bend**          | **±2 semitones** for a normal pitch wheel (channel-1 / non-MPE, and the MPE *master* channel), applied smoothly to all sustained voices. On an MPE *member* channel (2–16) the bend is per-note and uses the wide MPE range (default 48 semitones). Both `TerrainSynth` (`kPitchBendRangeSemis = 2.0f`) and the standalone synths via `distributeMpeMessages()` honor this split, so a regular pitch wheel never slams ±48 semitones. |
| **Mod wheel (CC#1)**    | Drives a default 6 Hz vibrato; depth scales with wheel position. Set the synth's *Vibrato* param to 0 to free CC#1 for MIDI Learn elsewhere. **Implemented only in `TerrainSynth`** (`modWheel[16]`) — the standalone synths (Wavetable, FM, Phase Distortion, Additive, Spectral Grain, Particle Cloud) ignore CC#1. |
| **Sustain pedal (CC#64)** | Holds notes through their release stage until pedal-up. Implemented by `TerrainSynth::sustainPedal[16]` (per-MIDI-channel state) in `terrain_synth.cpp:1519, 1571`. Drum Machine implements an analogous `DrumSynthProcessor::sustainPedal[16]` that *freezes the decay envelope* on held voices (drums have no note-off release stage to defer), so the drum rings at its current amplitude until pedal-up. **The standalone synths (Wavetable, FM, Phase Distortion, Additive, Spectral Grain, Particle Cloud) do not implement CC#64.** |
| **Channel pressure / aftertouch** | Multiplies per-voice volume by the aftertouch sensitivity (default 0.5, saved per node). Also exposed as a **Param** (block-rate, orange) input pin — wire any Param or Signal source into the synth's Aftertouch pin to drive the same swell. It's a Param rather than a Signal pin because the synth consumes it as the **block mean** (averaged across the whole block) so a slow LFO drives a smooth swell instead of bleeding its audio shape into the amplitude. The pin is system-managed: it's auto-created on tonal/note-triggered synths and any project that saved it as the old amber Signal pin is silently normalized to Param on load (safe because the type was never the user's choice). |
| **Polyphonic key pressure** | Per-*note* aftertouch (each held key can be pushed independently). Routed to the matching voice by note number, then **added** to channel pressure and clamped to 0..1 before the sensitivity multiply (`effectivePressure()` in `signal_modulation.h`). This is the one MIDI dimension that *cannot* ride a mono Signal cable — a single signal value can't say which note it belongs to — so it's consumed inside the synth voice allocator keyed by note number, never as a cable. Implemented via the shared `distributeMpeMessages()` helper across the standalone synths (Wavetable, FM, Phase Distortion, Additive, Spectral Grain); `TerrainSynth` handles it with its own equivalent inline path. |
| **Velocity**            | Scaled by the per-synth *Vel Sens* param (0..1). 0 ignores velocity, 1 maps full range. |

Drum Machine uses the separate `DrumSynthProcessor`, which also handles CC#64 — but with the freeze-the-decay semantics described above, since drum voices have no note-off-driven release stage to defer.

---

## MPE (per-note expression)

MPE (MIDI Polyphonic Expression) is the convention that lets *each held note*
carry its own continuous pitch bend, pressure, and timbre — so you can bend one
note of a chord while the others stay put. Standard MIDI can't do this because
those messages are per-channel, and a single channel holds the whole chord. MPE
solves it by spreading notes across **member channels 2–16** (one note per
channel) with **channel 1 as the master**, so each note's expression rides its
own channel.

SEANCE handles MPE at three points: the timeline *emits* it, hardware
controllers are *recorded* into it, and it's *consumed* by both the built-in
synths and (via a handshake) hosted plugins.

### Timeline MPE output

Right-click a **MIDI Timeline** or **Audio Timeline** node → **Enable MPE**
(context-menu item; toggles `node.mpeEnabled`, saved per node). With MPE on, the
`MidiTimelineProcessor` (`graph_processor.cpp`):

- Allocates each played note to a free **member channel 2–16** round-robin
  (`allocMpeChannel`, 15 channels = `kMpeChannels`; steals the oldest when all
  are busy), instead of sending everything on channel 1.
- Emits the note's stored **per-note expression curves** (`NoteExpression`:
  `pitchBend`, `slide`→CC#74, `pressure`→channel pressure) on that note's
  channel — an initial value just before note-on, then updates every ~32 samples
  while the note is held (`emitExpression`).
- Per-note pitch-bend range is `node.mpePitchBendRange` (default **48**
  semitones), so the recorded normalized bend maps to a wide expressive range.

With MPE off, every note goes out on channel 1 and the expression curves are not
emitted (a non-MPE synth would smear one note's bend across the whole chord).

### Recording MPE from a hardware controller

`AudioEngine::handleIncomingMidiMessage` (`audio_engine.cpp:776`) watches
channels 2–16 when `mpeRecordTargetNodeId` is armed. A note-on opens an
`MpeRecordNote`; subsequent pitch-wheel / CC#74 / channel-pressure messages on
that channel append timestamped `ExpressionPoint`s; note-off finalizes the note
into the target clip's `notes` with its `expression` curves filled in. So
playing an MPE controller (ROLI Seaboard, LinnStrument, etc.) into an armed
track captures the per-note gestures, which then play back through the timeline
MPE output above.

### Plugin MPE handshake

Built-in synths read MPE channels natively, but a **hosted VST3/AU plugin** must
first be told to interpret channels 2–16 as an MPE zone. That's done with the
**MPE Configuration Message (MCM)** — an RPN handshake (CC 6/38/100/101) defined
by the MPE spec.

Right-click a hosted-plugin node that has a MIDI input pin → **Enable MPE mode
(only if plugin is in MPE mode)** (context-menu item id 181; gated on
`node.plugin` + a `PinKind::Midi` input; toggles `node.mpeEnabled`, commits an
undo snapshot, and rebuilds the graph). This flag is a **user assertion that the
plugin itself is running in MPE mode** — MPE capability can't be detected
reliably, so SEANCE can't infer it. The label spells out the caveat inline
because JUCE `PopupMenu` items can't show hover tooltips. On rebuild,
`GraphProcessor::rebuildGraph` wires a small parallel MIDI generator —
`MpeConfigProcessor` (`graph_processor.h`) — into the plugin's MIDI input
(`nodeInputMap[node.id]`). That generator emits
`juce::MPEMessages::setLowerZone(15, mpePitchBendRange, 2)` (15 member channels,
per-note bend range from the node, master bend range 2).

Design notes:

- **Why a parallel generator rather than transforming the note stream:** it only
  *adds* the RPN and never touches notes, so the handshake itself is byte-for-
  byte safe; the actual note spreading is done separately by the cable-level
  tuning adapter (see [Microtuning hosted plugins](#microtuning-hosted-plugins)).
- **Why injected at the plugin's graph input** rather than on a cable: it
  bypasses the cable-level MIDI-Learn CC filter that could otherwise strip the
  RPN's CC 6/38/100/101 bytes.
- **Re-emit lifecycle:** emits for the first `kEmitBlocks` (4) blocks after
  construction (covers plugin load, graph rebuild, and the MPE toggle — all of
  which recreate the node) and re-arms on every transport play-start edge (covers
  plugins that reset their zone layout when playback stops).
- **Source-agnostic:** because the handshake rides the plugin's input, it works
  whether notes come from an MPE-enabled timeline, a hardware MPE controller, or
  a plain single-channel source (the cable adapter spreads the latter onto member
  channels automatically — an MPE *source* is not required).

**Why the toggle warns.** Turning MPE mode on for a plugin that is **not**
actually in MPE mode is harmful: the cable adapter scatters one voice's notes
across channels 2–16, which a non-MPE plugin treats as independent monophonic
channels — so it typically misbehaves (wrong voicing, dropped polyphony) or goes
silent. That's the opposite failure of leaving it off, so the toggle is opt-in
and the label says **only if plugin is in MPE mode**. Rule of thumb: match this
flag to the plugin's own MPE setting.

**Authoring a plugin that responds to this.** There is *nothing SEANCE-specific*
to implement. SEANCE hosts standard VST3/AU; the MCM it emits is the regular
MIDI-MPE 1.0 handshake (the RPN 0x0006 zone-layout message), identical to what
Bitwig, Logic, Cubase, GarageBand, etc. send. A plugin "supports MPE in SEANCE"
purely by supporting standard MPE — i.e. honoring the lower-zone RPN and reading
per-note channels 2–16. The toggle lives on the *host* side and decides only
whether SEANCE sends that standard handshake; it is not a proprietary protocol a
plugin must opt into. (This is why there's no SEANCE plugin-authoring guide —
making a plugin for SEANCE is just making a normal VST3/AU plugin.)

Both toggles persist via `project_file.cpp` (`mpeEnabled`,
`mpePitchBendRange`), so saved projects reload with MPE state intact.

## Microtuning hosted plugins

The project-global **tuning system** (Equal Temperament / Pythagorean / Just
Intonation / Quarter-Comma Meantone) and **concert pitch** (A4 = 440 Hz by
default), set in *Settings → Tuning System / Concert Pitch*, change the actual
Hz of every note. Built-in synths read this directly via `Transport::noteToFreq`.
A **hosted VST3/AU plugin**, though, renders the raw MIDI note number at standard
12-TET / A440 and has no idea about the project tuning — so SEANCE has to bend it
into tune from the outside, with pitch bend.

**Where the bend is applied: the cable.** A MIDI track and the instrument it
drives are *separate nodes joined by a MIDI cable*. The cable is the only place
that knows **both** the global tuning **and** whether the destination is a native
synth (tunes itself) or a hosted plugin (must be bent). So a cable-level
processor, `MidiTuningAdapterProcessor` (`graph_processor.{h,cpp}`), is spliced
onto every MIDI cable whose destination has a loaded `node.plugin`, as the last
hop before the plugin. Native-synth cables and non-MIDI cables never get one. At
**default tuning (12-TET / A440)** the adapter is a pure pass-through, so it costs
nothing until you actually pick a non-standard tuning.

The split between the two tuning components decides *how* it bends:

- **Concert-pitch component** — `1200·log2(concertPitch/440)`, the *same* for
  every note. Representable on a single shared channel.
- **Temperament component** — `tuningCentsOffset[note%12]`, *differs per pitch
  class* (zero for Equal Temperament). Needs a separate channel per note, i.e.
  MPE.

The adapter therefore has two modes, chosen per-destination from the plugin's
**MPE mode** toggle (see [Plugin MPE handshake](#plugin-mpe-handshake)):

- **Plugin in MPE mode** — each incoming note is given its own member channel
  (2–16, allocated round-robin, spreading a single-channel source on the fly) and
  receives the **full** per-note tuning bend (concert + temperament), summed with
  any expression pitch-bend already on the note. This is the only mode that can
  deliver unequal temperaments correctly. The member-channel bend range is
  `node.mpePitchBendRange` (default **±48 semitones** — symmetric, fixed, not
  user-exposed; tuning only needs ≤½ semitone since we bend from the nearest
  note, and ±48 is the MPE-spec headroom that also leaves room for expression).
- **Plugin not in MPE mode** — everything collapses to channel 1 with only the
  **uniform concert-pitch bend** (temperament can't be delivered per-note on one
  shared channel). The adapter sends an RPN pinning channel 1's bend range to ±2
  semitones so the sub-semitone concert bend lands correctly; a plugin that
  ignores the RPN and keeps its own fixed range is the "may not honor tuning"
  case the menu warns about. Any user pitch bend on the stream is summed in.

**The Tuning System menu warns** when an unequal temperament is selected (a
disabled multi-line note at the bottom of the submenu): the temperament only
reaches a hosted plugin in MPE mode, plugins with a fixed bend range may still be
off, and built-in synths are always correct.

**Bend-only, deliberately no MTS-ESP.** SEANCE does *not* use MTS-ESP (the
MIDI Tuning Standard sysex-server protocol) as a fallback. MTS is process-global,
so it would double-tune any plugin that is both MPE- and MTS-capable, and it
can't be set per-plugin. Pitch bend is per-cable and composes cleanly with
expression, so it's the only mechanism used.

**Overflow is not specially handled.** If expression bend + tuning bend exceeded
the member range the adapter would just clamp, but at ±48 semitones (four
octaves of headroom over a ≤½-semitone tuning bend) this is a non-issue in
practice. Note-renumbering was rejected because it breaks sample-based
instruments' key-zones and per-key timbre.

### Editing recorded MPE in the piano roll

When a timeline has MPE enabled, the piano roll exposes an **expression lane**
below the grid for viewing and editing the per-note curves after the fact.

- **MPE Lane dropdown** (toolbar, second row; visible only when
  `node.mpeEnabled`): pick **Pitch Bend**, **Slide (timbre)**, or **Pressure**
  to show that curve for the selected notes in the bottom lane. "MPE Lane: off"
  hides it. It is mutually exclusive with the **Automate Param** dropdown next to
  it — choosing an MPE lane clears the automation selection and vice-versa.
- **In the lane:** click empty space to add a breakpoint, drag a point to move
  it, right-click a point to delete it. Each gesture (add / drag-release /
  delete) commits a `commitSnapshot` undo step ("Edit MPE expression", "Delete
  MPE expression point"), so every edit is independently undoable and is
  serialized into the project.
- **Note tint:** with MPE on, each note body is tinted from its clip color toward
  hot orange in proportion to its recorded **mean pressure**, so heavily-pressed
  notes read at a glance without opening the lane.

The note right-click menu gains an **MPE Expression** submenu (only when
`node.mpeEnabled` and notes are selected):

- **Smooth / Thin Curves** — runs a Ramer–Douglas–Peucker simplification
  (`simplifyExprCurve`, tolerance 0.02 in normalized value units) over all three
  curves of every selected note, collapsing the dense stream captured from a
  controller into a handful of editable control points while preserving shape.
  Commits "Smooth MPE expression".
- **Clear Expression** — drops all pitch-bend / slide / pressure points on the
  selected notes ("Clear MPE expression").
- **Bake Pressure to Automation →** *(param list)* — copies each selected note's
  pressure points onto the chosen parameter's automation lane, mapping the
  normalized 0..1 pressure to the param's `[minVal, maxVal]` range at absolute
  beat positions, then sorts the lane and opens it ("Bake pressure to
  automation"). Useful for driving, say, a filter cutoff from how hard you
  pressed each note.

---

## Piano roll

Opens by double-clicking a MIDI Track node; the editor docks at the bottom of the window.

### Snap

The toolbar offers four snap divisions:

- **1/4** — quarter-beat granularity (the fine setting)
- **1/2** — half-beat
- **1** — whole-beat
- **Off** — no snap

`Alt` held during a drag temporarily disables snapping regardless of mode. The **Snap to Scale** button further restricts dragged-note pitches to the chosen Key/Scale.

### Key/Scale/Mode/Root

Four dropdowns set the musical context. They determine which rows the piano roll background highlights as in-scale, what Snap-to-Scale snaps to, and the reference scale for degree analysis. There are **two regimes**, and the dropdowns reflect which one is active:

- **Root** (C, C#, …) — the tonic, the "home" pitch. Always relevant, and the one axis that's independent of everything else.
- **Key + Mode regime** (the default). **Key** picks a *parent scale* (Major, Natural/Harmonic/Melodic Minor, and other 7-note parents) — the set of seven notes to use. **Mode** picks which of those seven notes becomes "home" by *rotating* the parent: Mode 1 **Ionian (Major)** is the Key itself, Dorian starts on the 2nd degree, Phrygian on the 3rd, and so on through Locrian. Key and Mode work **together** — the highlighted scale is the parent rotated to the chosen mode, anchored at Root. This is why e.g. Root A + Key Major + Mode Dorian gives A Dorian, and why a non-major parent like Harmonic Minor + Mode 5 produces Phrygian Dominant. (Mechanically, the interval set is `MusicTheory::activeIntervals()` = `rotateScale(keys()[Key], modeIndex(Mode))`.)
- **Scale regime**. **Scale** is a list of fixed, non-diatonic note sets — Pentatonic, Blues, Whole-Tone, Augmented, the octatonics, exotic scales, plus named modes of non-major parents (Phrygian Dominant, Acoustic, …), and **Chromatic** (which, covering all 12 notes, turns scale highlighting off). Picking a Scale **overrides** Key + Mode: the two dropdowns grey out (their tooltip explains why), and the highlight comes straight from the chosen set anchored at Root. Pick a Key or Mode again to return to the Key+Mode regime.

> **Why Key and Mode are split this way.** A *mode* is already a complete interval pattern over a root, so "Key" here is **not** a note letter (that's Root) and **not** redundant with Mode — it's the *parent scale* that Mode rotates. The label **Ionian (Major)** makes the identity explicit: the 1st mode of the major scale *is* the major scale. Note that a *scale* (an interval set, what these dropdowns choose) is distinct from a *tuning system* (the cents/frequencies each pitch maps to, set project-wide; see the Tuning section) — "Pythagorean" is a tuning, not a scale, which is why it is deliberately absent from the Scale list.

**Detect Key** runs an analyzer over the existing notes and snaps the dropdowns to the most likely match: a "key" match sets Key with Mode reset to Ionian; a "mode" match sets Mode with Major as the parent; a "scale" match switches to the Scale regime.

### Selecting, copying & pasting notes

- **Marquee select** — drag a selection rectangle over empty grid space to highlight every note it touches. Highlighting updates **live** as you drag: notes light up the moment the rectangle sweeps over them and un-highlight if they fall back out, so you can see exactly what you're about to select before releasing. Hold `Shift` while dragging to add the marquee'd notes to the existing selection rather than replacing it. The drag origin (beat + pitch under the cursor where the marquee began) is also remembered as the *paste anchor*.
- **Select All** — `Ctrl+A` (or the toolbar button) selects every note in the clip.
- **Copy** — `Ctrl+C` or right-click → *Copy*. Copies the selection to a clipboard shared across all piano-roll instances.
- **Cut** — `Ctrl+X` or right-click → *Cut*. Copies then deletes the selection (the delete is undoable).
- **Paste** — re-anchors the whole copied block by its **top-left corner** (earliest beat, highest pitch): that corner lands at the paste target and every other note keeps its relative position. The paste target depends on how you paste:
  - **`Ctrl+V`** pastes at the **grid cell under the mouse cursor**. While the clipboard is non-empty, a translucent aqua **ghost preview** of the block follows the cursor over the grid, showing exactly where the notes will land before you commit — so you just move the mouse to the spot and press `Ctrl+V`. (This replaces relying on the marquee rectangle, which disappears on mouse-release and so can't serve as a persistent target.)
  - **Right-click → *Paste*** pastes at the point where the context menu was opened.
  - In both cases the paste start beat is snapped to the current Snap division, the notes land in the clip covering the target (or the first clip if none does), the clip auto-extends to fit, and the pasted notes become the new selection. If the project carries an explicit song-length override (see **Grid regions and song length** below) and the pasted notes land past it, the override grows to cover them (part of the same `Paste N notes` undo step). This extends the *song-length* boundary only — if a transport A-B loop is active, that still bounds playback until it's disabled or its end is dragged out.

Clipboard notes are stored corner-relative — each note records its beat offset from the earliest copied note and its semitones below the highest copied note — so paste is position-independent in both axes. Both copy and paste are single undo steps (`Paste N notes`). The clipboard is `static`, shared across every open piano-roll instance, so you can copy from one MIDI Timeline and paste into another.

### MIDI note degree fields

Every `MidiNote` stores, alongside the raw pitch:

- **Scale degree** (1st..7th)
- **Octave**
- **Chromatic offset** (semitones from the nearest scale degree — non-zero for accidentals)

This enables the *Change Key* workflow: Analyze in the original key → change the Key/Scale dropdowns → click *Change Key* and pitches are recomputed from degrees in the new context, preserving melodic shape (C Major → D Minor keeps the contour, just transposed and re-coloured).

Notes display their degree in the piano roll (e.g. `C4 (1)`, `E4 (3)`). Out-of-scale notes display sharps/flats relative to the nearest degree.

### Keyboard column note names

The keyboard column down the left edge labels **every** row with its note name (`C4`, `C#4`, `D4`, …), not just the C-of-octave rows. The label font shrinks to fit the row height (floored at 6.5px — the smallest size that still rasterises legibly), so at the default zoom and panel height every row is readable. The default vertical zoom (`visibleRange = 15` semitones ≈ 1¼ octaves) and default editor-panel height (220px) are tuned together so rows are ≈7.5px tall — enough for a per-row label. In-scale rows use brighter text (root = brightest), out-of-scale rows are dimmed. If you zoom out far enough that rows drop below ~4px, the column falls back to one large `C3`/`C4`/`C5`… anchor label per octave so the names stay legible.

### Note hover popup

Hovering the mouse over a note pops up a tooltip with that note's full detail: name and scale degree (`C#4 (3rd)`), velocity, start beat, length, and detune in cents (if non-zero), plus the clip name when the timeline holds more than one clip. If several notes overlap at the cursor (stacked pitches / chords), the popup lists **all** of them, prefixed with a count (`3 notes:`).

### Lanes (bottom strip)

One lane is shown at a time, selected from the lane buttons. Available lane types:

- **Off** — hidden; maximizes note grid space.
- **Velocity** — one vertical bar per note, bar height encodes 0..127; drag up/down to edit.
- **Pitch Bend** — per-clip pitch-bend automation curve.
- **Slide** — per-note legato/portamento control.
- **Pressure** — per-clip channel pressure curve.
- **Automate Param** — pick any parameter on the parent synth from a dropdown; the lane becomes an automation curve for that param. Click to add control points, drag to shape. Interpolation is **Catmull-Rom** between points.

The automation curve is read by the synth during playback whenever the clip is playing; outside the clip the param falls back to its slider value (or to any incoming Param/Signal cable).

### Toolbar transformations

Operate on the selection if there is one, otherwise the whole clip:

- **+Octave / -Octave** — ±12 semitones
- **+Semitone / -Semitone** — ±1 semitone
- **Nudge Left / Nudge Right** — ±1 snap unit in time
- **x2 Duration / /2 Duration** — double/halve note lengths
- **Reverse** — flip the time order of selected notes
- **Detune** — slider in cents (1/100 of a semitone) for fine pitch offsets

All of these go through `execNoteEdit` / `applyToSelUndo` (the `LambdaCommand` fast path — see CLAUDE.md).

### Compact mode

The `--` button at the top-left collapses most of the toolbar to free vertical space; `++` brings it back.

### Mouse / keyboard

- Wheel — scroll pitch axis
- Shift+wheel — scroll time axis
- Ctrl+wheel — zoom horizontally toward cursor
- Bottom horizontal zoom slider — precise zoom value

### Grid regions and song length

The note grid can show **up to three** brightness levels. They are three *independent* overlays that happen to stack, not three degrees of one "activation" setting — which is why they can line up or diverge:

1. **Plain grid (darkest)** — empty space with no clip over it.
2. **Clip tint (middle)** — every clip fills its span with a faint wash of the clip's colour (≈6% alpha; drawn in `piano_roll_component.cpp` "Clip boundaries"). This is where notes live; the band marks the extent of actual content. Placing or pasting notes past the current clip end auto-extends the clip, so this band grows to follow your notes.
3. **Loop region (brightest)** — when the transport A-B loop is enabled (`transport->loopEnabled`, `loopStartBeat`..`loopEndBeat`), that span is overlaid with a translucent blue wash (`RGB(60,60,150)` @ 15% alpha) plus brighter blue edge lines. Because it paints *on top of* the clip tint, the looped portion of a clip reads as "extra highlighted." This overlay is the **loop region**, not the song length.

**Which one actually bounds playback?** Two separate mechanisms, and the loop wins:

- **Transport A-B loop** (overlay 3). If `loopEnabled` and `loopEndBeat > loopStartBeat`, playback wraps from `loopEndBeat` back to `loopStartBeat` every pass and `applySongRepeat` is skipped for that block (`audio_engine.cpp` — "the user-region loop above takes precedence"). So whenever the blue overlay is present, **that** is the boundary the playhead obeys; any content past `loopEndBeat` (middle-tint clip with no blue over it) never plays until the loop is turned off or its end dragged out. The loop is a deliberate user-controlled cycler, so content-extending edits **do not** move it.
- **Song length** (no overlay of its own — marked only by the orange **END** bar). Used only when no transport loop is active. Governed by `NodeGraph::effectiveSongLengthBeats()`: an explicit override (`songLengthBeats > 0`, via the **Song** button or a tracker/MOD import) else auto-derived from the **exact, un-rounded** end of the last clip across all timeline nodes (`contentEndBeats()` — the song may end mid-bar, matching the END bar). In **auto** mode it tracks the clips; in **override** mode it's independent and can sit short of the content (deliberate early stop / trailing silence).

A **MOD/tracker import** sets one of these depending on the module's loop target (`mod_import.cpp`): a whole-song self-repeat (`target == 0`) becomes `songRepeatMode = Forever` + a song-length override with **no** transport loop (so only two levels appear); an "intro then loop a later section" (`target > 0`) becomes a **transport A-B loop** from the section start to the song end (so all three levels appear, and the blue region is what repeats). This is the usual reason an imported project shows the third, brightest level and won't play past it.

To keep the *song-length* boundary honest, every content-extending edit — note placement, paste-at-cursor, clip paste, right-click *Add Note*, and dragging the **END** bar right — calls `NodeGraph::growSongLengthToContent()`, a no-op in auto mode that in override mode pushes the override out to cover any content now extending past it (grow-only, so deliberate early-stop/trailing-silence setups survive). **Note this only affects the song-length boundary, not the transport loop** — if a blue loop overlay is bounding playback, pasting past it still won't be heard until the loop is disabled or extended. (Known gap: dragging the **END** bar *left* in override mode shortens the clip but leaves the longer override in place; the orange END bar is drawn from clip extents, so in override mode the bar and the true playback end can diverge.)

#### Defining and editing the loop region from the grid

The transport A-B loop can be created, adjusted, and removed entirely from the piano-roll grid (it's the same `transport->loopStartBeat`/`loopEndBeat` the transport toolbar's **Loop** button drives — they stay in sync).

- **Visual** — the loop span is drawn as a thin non-occluding frame (top/bottom hairlines plus a 2px vertical handle line with inward ticks at each edge, labelled **LOOP**), *not* a filled wash, so notes inside the loop stay fully visible.
- **Right-click → Loop Region** submenu collects every loop action:
  - **Loop Selected Notes** — sets the loop to span the beat extent of the current note selection (start of the earliest selected note to end of the latest). Disabled when nothing is selected.
  - **Loop Clip Under Cursor** — sets the loop to the clip the right-click landed in. Disabled when the cursor isn't over a clip.
  - **Loop Whole Song** — sets the loop to the full project length (beat 0 to the last clip end), mirroring the transport **Loop** button.
  - **Set Loop Start Here** / **Set Loop End Here** — place one edge at the clicked beat (the other edge is nudged to keep start < end if needed).
  - **Clear Loop Region** — disables the loop. Disabled when no loop is active.
- **Right-click inside an active loop** also surfaces a top-level **Disable Loop Region** item (above the submenus) for one-click removal without hunting through the submenu.
- **Drag the loop edges** — hovering either edge line shows a left-right resize cursor; dragging moves that edge (snapped to the grid; a minimum gap is enforced so the loop can't invert). Released as a single `commitSnapshot("Move loop edge")` undo step.

All loop edits commit an undo snapshot, so any of them is a single Ctrl+Z.

### Song-end resize handle

A draggable orange **END** bar is drawn at the end of the timeline's content — at the right edge of the rightmost clip. It spans the full height of the note grid with a small labelled tab at the top.

- **Drag it left** to shorten the song. On release, any notes that now start past the new end are deleted, and notes that merely overhang the end are trimmed to fit (down to a minimum length).
- **Drag it right** to lengthen the song, adding empty beats.
- The drag snaps to the current Snap division; hold `Alt` to drop it at an arbitrary beat. The clip can't be dragged shorter than one snap unit (or a quarter-beat, whichever is larger).

Internally the handle adjusts the rightmost clip's `lengthBeats`; because the auto-derived song length follows the clips, shrinking the clip shortens the whole song. The gesture captures the horizontal beat↔pixel mapping at drag start so live resizing stays smooth even as the derived timeline length changes underneath the drag. The resize is a single `commitSnapshot("Resize song end")` undo step.

**The song ends exactly where the END bar sits — including mid-bar.** Playback length is the *un-rounded* end of the last clip (`NodeGraph::contentEndBeats()`), so if you drag the END bar to beat 1 of a 4-beat bar, the song stops at beat 1, not the end of the bar. The visible **grid** still draws out to the next full bar (`getTimelineBeats()` rounds up) for a clean ruler, but that trailing grid is silent. (Previously the two disagreed: the marker sat at the exact clip end while playback ran on to the bar-rounded end.)

### Track header strip (parenting & time offset)

Every piano-roll panel has a thin **track-header strip** between the toolbar and the note grid (its own `TRACK_HEADER_H = 20`px band, folded into `toolbarHeight()` so the grid below shifts down automatically). The strip shows this track as a clip-block positioned along the same horizontal beat axis as the grid, with a bright left edge marking its start beat. Its label reads the track name, and — when the track is a child — `◂ child of <parent>` plus `@<n> beats` (the track's own start offset).

The strip exposes the two halves of timeline parenting:

- **Drag the strip left/right to retime the track.** Left-dragging anywhere in the strip changes the track's own start offset (`groupBeatOffset`). The drag snaps to the current Snap division (hold `Alt` for free placement), clamps at beat 0, and captures the beat↔pixel mapping at drag start so the motion stays stable even as the derived timeline length changes underneath. Because a node's absolute offset is the sum of its own offset and every ancestor's (`getAbsoluteBeatOffset`), **moving a parent moves all its children with it** — and if the child is shown in another stacked panel, that panel repaints live (`onTimingChanged`). Setting an offset clears any anchor-marker binding (an explicit drag wins). No undo step is created during the drag; one `commitSnapshot("Move track in time")` is pushed on release.

- **Right-click the strip for the parent menu.** It offers:
  - **Make child of ▸** — a submenu of every *other* timeline/group node. Candidates that would create a cycle (this track's own descendants) are **shown disabled with the reason inline** — e.g. `MyChild — already inside this track` — rather than silently omitted, so you're never left guessing why a track is missing from the list (PopupMenu items can't carry hover tooltips, so the explanation lives in the label). The current parent is shown ticked and disabled. Picking an enabled one calls `addToGroup`, so **any** MIDI/Audio timeline (not just a dedicated Group node) can be a parent. Undo step: `commitSnapshot("Make track a child")`.
  - **Clear parent** — detaches the track. The inherited offset is folded into the track's own offset so it doesn't visually jump. Undo step: `commitSnapshot("Clear track parent")`.
  - **Set start beat…** — a dialog for typing an exact offset in beats (parented to the main window). Undo step: `commitSnapshot("Set track start beat")`.

**Also reachable from the node graph (discoverability).** The same three items appear under a **"Track nesting & timing ▸"** submenu when you **right-click a timeline node on the main graph canvas** — so you no longer have to know to open the editor and right-click the (easy-to-miss) header strip. Both entry points share one implementation (`TrackNestingMenu::addItems` / `handle` in `track_nesting_menu.h`) so they can never drift apart; the node-menu path runs the same `resolveAnchors` + `commitSnapshot` refresh (timing offsets are read live during playback, so no audio-graph rebuild is needed).

`addToGroup` was relaxed from Group-only parents to also accept `MidiTimeline`/`AudioTimeline` parents, guarded by `isAncestorOf` so no parent/child cycle can form. When a parent timeline is deleted, any surviving child whose parent was the deleted node is detached to top-level (keeping its own offset) rather than left with a dangling `parentGroupId`. The relationship and offset are serialized generically (`parentGroupId` + `groupBeatOffset` in the project file), so save→load round-trips it.

**Audio tracks nest exactly like MIDI tracks — including in playback.** Nesting is a *node*-level relationship, so it applies uniformly to `AudioTimeline` and `MidiTimeline`: an audio track can be a child of another audio track, of a MIDI track, or of a Group, and its audio clips shift by the cascading `absoluteBeatOffset` just as a child MIDI track's notes do. Three places consume the offset and all three must agree, or you get a silent desync:

- **Drawing** — the piano roll's `beatToX` adds `absoluteBeatOffset` once, so every clip/note drawn from node-local beats lands at the offset position automatically.
- **Playback** — `AudioTimelineProcessor::processBlock` adds it to each clip's start/end before the block-overlap test, matching what the MIDI generator does. (Earlier builds read `clip.startBeat` raw here, so a nested audio track *drew* at its offset but *played* at beat 0 — audible only as things being in the wrong place, with nothing on screen to explain it. Regression-tested by `testAudioTrackNesting`.)
- **Song length** — `contentEndBeats()` adds it, so the auto-derived song end, the default A-B loop range, **Export Audio**, **Bounce to Audio Track** and the playback-capture render all run long enough to include a nested track's tail. The offline-render cache key (`computeNodeHash`) also folds in `absoluteBeatOffset`, so re-nesting a track invalidates its cached render instead of replaying the old timing.

**Nesting depth is unbounded.** There is no cap on how deeply timelines may nest. Both parent-chain walks — `isAncestorOf` (cycle check on link) and `getAbsoluteBeatOffset` (sum of every ancestor's `groupBeatOffset`) — use a `visited`-set loop detector rather than an arbitrary depth limit, so a chain of any depth accumulates its full offset, and even a corrupt/cyclic `parentGroupId` chain (e.g. from a hand-edited project file) terminates cleanly with each node counted once instead of hanging. (Earlier builds capped `getAbsoluteBeatOffset` at 20 levels, which silently truncated the offset past that depth.)

**Anchoring a child offset to a marker (ripple on insert/cut time).** A nested timeline's start offset can be *bound to a named marker* instead of a fixed beat: the node's `anchorMarker` field holds the marker name, and `resolveAnchors()` rewrites its `groupBeatOffset` to the marker's current beat (via `resolveMarkerBeat`) and rebuilds every node's cascading `absoluteBeatOffset`. The point of anchoring is retiming: when you **Insert time** or **Delete time** in all-tracks scope, markers past the edit point shift, and any child anchored to one of them **ripples left/right to stay locked to its marker**. This is wired in at the model level — `NodeGraph::insertTime` and `deleteTime` both call `resolveAnchors()` after shifting clips/notes/CC/automation/markers, so *every* caller (piano-roll Insert/Delete Time menu, the Python `insert_time`/`delete_time` bindings, MOD import) gets the ripple for free with no per-call-site plumbing. If a child is anchored to a marker that falls *inside* a deleted range, that marker is removed, `resolveMarkerBeat` returns < 0, and the child simply keeps its last offset. Any explicit offset edit (dragging the header strip or **Set start beat…**) clears `anchorMarker`, so a manual retime always wins over the anchor.

---

## Layered Waveform editor

The editor for the Waveform synth. Builds single-cycle waveforms as a sum of layered shapes, optionally stacks cycles into a multi-frame wavetable, and supports N-dimensional grids (Grid mode) or scattered free positioning (Scatter mode).

### Library and Cells panels (pop-out window)

The pop-out wavetable window has two list panels on its sidebar that separate *waveform existence* from *waveform placement*:

- **Library** — every waveform that exists in this wavetable, whether or not it's placed in the arrangement view. This is where waveforms are created, edited, coloured, and deleted. `+ Waveform` adds a new library entry (Layered / Frequency-domain / Wavelet / capture-from-audio, plus *Duplicate current* when a waveform is selected). The newly-added entry is highlighted and, if the list scrolls, the viewport scrolls to the bottom so the new row is visible. Clicking a row makes that entry the **editor target** (`currentLibraryId`, amber highlight) — edits flow into every cell that references it — and repoints the cell/dot selection at the entry's first placement so the position section describes the right waveform.
- **Cells / Waveforms** — the occupied placements in the arrangement view. The panel is titled **Cells** in Grid mode (one row per cell, including empty cells) and **Waveforms** in Scatter mode (one row per dot). In **Grid** mode each row shows the cell's **coordinate followed by the name of the waveform it holds** — e.g. `(0,0,0)  Bass` — or `- empty -` for an unoccupied cell; the name is resolved through the cell's `waveformId` the same way Scatter does, falling back to `Waveform N` only when the entry is unnamed or orphaned. In **Scatter** mode each row is labelled with the **library name of the waveform that dot holds** (resolved through the dot's `waveformId`), falling back to `Waveform N` only when the entry is unnamed or orphaned — so converting a named grid to Scatter keeps the real names instead of showing generic `Waveform 1…N`. Clicking a row selects that cell/dot (blue highlight); the `X` clears it (the library entry survives).

**Library row interactions:**

- **Click** — set as editor target.
- **Drag onto the arrangement view** — place that waveform at the cursor (Grid: into a cell; Scatter: as a new dot).
- **Right-click** — context menu: **Add to grid** (place into the arrangement — first empty Grid cell, growing the first axis by one if every cell is full and the axis is under 64; or a centre-of-cube Scatter dot), **Rename…** (see [Renaming a waveform](#renaming-a-waveform)), **Duplicate** (clone the waveform into a new *library-only* entry, no placement — the arrangement view's dot right-click *Duplicate waveform* behaves identically; all creation paths are library-only. The clone is named `<source> (copy N)` with the lowest free `N` (so duplicating *Saw* twice gives *Saw (copy 1)* then *Saw (copy 2)*); duplicating an existing copy strips the old suffix and continues the run rather than nesting (*Saw (copy 1)* → *Saw (copy 3)* when 1 and 2 already exist)), **Delete**.
- **Del / Backspace** (with the pop-out focused) — **routed by the surface you last selected on**, because clicking a populated cell/dot updates both the editor target and the cell selection, so the key alone can't tell which you mean. If your last selection was a **Library** row, Delete removes that **waveform definition** and every placement of it (same as the row's `X`). If your last selection was a **cell/dot in the arrangement view or a Cells/Waveforms-list row**, Delete removes only **that placement** (clears the Grid cell / erases the Scatter dot) and the library waveform survives — identical to the placement's `X` and to shift-clicking a dot. Deleting a placement when the selected Grid cell is already empty is a no-op. *(This split fixes the old behaviour where selecting a dot in the view and pressing Delete silently deleted the underlying library waveform.)*
- **X button** — remove the entry from the library; every cell/dot that referenced it becomes empty. The editor target falls back to the first surviving entry, or none if the library is now empty.

**"Selected waveform position" section gating.** This section's per-axis sliders/steppers only appear when the highlighted library entry is actually placed in the arrangement (at least one Grid cell or Scatter dot references it). A freshly-created library entry isn't placed yet, so showing cell coordinates for it would be meaningless — instead the section is replaced by an **Add to grid** button that places the entry (then the position controls reappear). With nothing highlighted, the section collapses entirely.

### Auditioning the frame (Preview button)

A green **Preview** button sits in a button row **near the bottom of the right pane** (just above the frame-warp strip and the summation preview), at the bottom-left. This placement and label match every other audition control in the app — the granular and inharmonic frame editors and the mic/file/song capture dialogs all use a bottom **Preview** button that toggles to **Stop** — so the audition control is in the same spot and named the same wherever you are. (It was briefly in the top identity row labelled "Play"; it was moved and renamed for that consistency.) Pressing it sustains a held note (A4, MIDI 69, full velocity) through the **owning Waveform synth's own voice path** — its AHDSR envelope, Volume, and any downstream effects/pan — so you hear the single cycle you're editing **on its own**, even before it's placed into any grid cell or scatter dot. Click again (the button turns red and reads **Stop**) to release the note. The button is **hidden when there's no editor target** (nothing to play) and while the capture panel is showing.

- **What you see is what you hear.** Unlike the granular editor's frame-specific override, the layered Preview button uses a **generic single-cycle audition** (`Node::AuditionCycleFrame`, carried on the audition note-on as `AuditionEvent::cycleFrame`). The editor renders the on-screen frame to its final single cycle via `IWavetableFrame::render(tableSize, out)` — which bakes in the per-waveform **gain** and every layer/summation warp — and ships *that* exact buffer. The voice plays the supplied cycle **exclusively**, bypassing the cycle terrain, the placed-frame morph, the granular layer, and the inharmonic layer, so the audition is byte-for-byte the preview waveform. Because every frame type (Layered, Frequency-domain, Wavelet, Granular sample) renders down to one final cycle, this mechanism is **frame-type-agnostic** — it's the shared path that drives the Preview button in *every* frame editor, not just the layered one. The boilerplate (build the `AuditionCycleFrame` + held A4 note-on, write `node.heldAudition` under the mutex) is factored into two free helpers, `setNodeHeldAuditionCycle(node, cycle)` / `clearNodeHeldAudition(node)` (`node_graph.h`), so any editor can ship a held audition in one call.
- **Edits are heard live.** The held audition is re-published on every audible change while Preview is held — layer edits (`onLayerChanged`), Gain drag (`gainSlider.onValueChange`), switching the editor target / library entry (`setEditingLibraryEntry`), and a snapshot reload (`reloadFromNode`) all call `refreshHeldFrameAudition()`, which re-renders the current cycle and re-arms the note under the `auditionMutex`. So dragging a layer's amplitude while playing morphs the sustained note in real time. This rides the same **level-triggered held-audition channel** (`Node::heldAudition`) the granular editor uses, so it survives the debounced (~150 ms) graph rebuild that every wavetable edit triggers — the fresh post-rebuild processor re-arms the note from `heldAudition` rather than going silent.
- **Audible even when the synth isn't wired to Output.** As with the granular editor, when the owning synth node has no audio path to an Output node (`!node.reachesOutput`) the audition voice is diverted to the `AudioEngine` audition-monitor bus, so a freshly-added or disconnected Waveform synth still previews. When it *is* routed, the audition flows through the normal graph (downstream effects included).
- **Always released on exit.** Closing the editor (destructor), losing the editor target (`refreshIdentityRow` no-target branch), and pressing Stop all clear `heldAudition`, so a held preview never leaks past the editor.

### Renaming a waveform

A waveform's name is a property of the **library entry**, shared by every cell/dot that references it (exactly like its colour and [gain](#per-waveform-gain)). It shows in the Library list and in the tooltips/headers wherever the waveform is placed. There are three ways to rename, all of which write the same field, commit to the node script, push one undo step, and refresh every view (Library list, identity row, arrangement-view tooltips):

- **Inline name box** — the name text box in the right pane's identity row (top of the editor), preceded by a **colour swatch** and a **"Name:"** label. It's present and editable for all four frame-content editors — Layered, Frequency-domain (FFT), Wavelet, and Granular — so you can rename whatever waveform you're currently editing without leaving the editor. Type and press Enter (or click away) to commit. Empty shows the placeholder "(unnamed waveform)".
- **Library row → Rename…** — right-click any row in the pop-out window's **Library** list. Opens a small modal text-entry dialog seeded with the current name; Enter commits, Esc cancels.
- **Arrangement-view cell/dot → Rename waveform…** — right-click a populated Grid cell or a Scatter dot (the same menu that carries *Edit waveform* / *Duplicate waveform* / *Remove from wavetable*, under the identify header). Opens the same modal text-entry dialog.

The **mic/file and song capture dialogs are creation tools, not renamers**: captured waveforms are auto-named after their source and freeze method (the *Library entry names* note in the mic/file capture section below). Rename the result afterward via any of the three paths above — the new entry lands in the Library highlighted, so it's a right-click (or an Enter in the inline box) away.

### Per-waveform gain

Every waveform in a wavetable is **peak-normalised** — whatever you build (a single sine, a stack of layers, a captured sample, a frequency-domain spectrum) is scaled so its loudest sample hits 1.0. That keeps frames at a consistent level, but it also means there's no way, from the shape controls alone, to make one waveform quieter or louder than its neighbours: every finished cycle has the same peak. Morphing between two frames therefore carries no volume contour.

The **Gain** slider on the per-waveform identity row (directly under the **Waveform:** name/colour row, right pane) fixes that. It's a horizontal slider whose drag spans **0 – 4×** (1.00 = unchanged, double-click to reset); the attached text box accepts typed values higher or lower than the drag range, up to a 64× safety ceiling, with figures above 4 pinning the thumb at the right end. The value is multiplied onto the cycle **after** normalisation, so it:

- changes the actual rendered samples (not just a playback trim),
- shows immediately in the waveform preview strip,
- is baked into the synth's wavetable terrain, so a 0.5-gain frame really is half as loud in the morph as a 1.0-gain frame, giving morphs an honest volume contour.

Gain is a property of the **waveform** (the library entry / frame), not of a cell or dot — so every placement that references the same waveform shares its gain, exactly like name and colour. It's stored per frame and travels with the waveform through duplicate, Grid↔Scatter conversion, save/load, and undo. Dragging the slider is one undo step per sweep (committed on release); typing a value or double-click-reset commits immediately.

> Gain does **not** change a waveform's *shape* — it's a pure post-normalisation level scaler. It also does not address per-cell volume gradients in the morph blend (those are controlled by the *Empty cells fade volume* / *Distance fades volume* toggles, above).
>
> Granular frames honour Gain too. Their grain playback is streamed from the source PCM (bypassing the normalised cycle), so the synth's granular layer multiplies the rendered grain by the frame's gain directly — a 0.5-gain granular frame is half as loud in held-note playback and in the wavetable morph, matching the cycle-frame behaviour. (Earlier builds applied Gain only to a granular frame's preview/cycle approximation, not its audible grain output; that gap is closed.)

On disk this is the `__wavetable5__` script format (a per-entry gain float added to the v4 library+cell layout); older `__wavetable4__` and earlier projects load with every gain defaulting to 1.00.

<a name="per-layer-wave-source-picker"></a>
### Wave source (per layer)

Each layer has **exactly one wave source** — what generates its cycle — chosen from a single **wave-source picker** button (showing the current source's name). One menu consolidates every way to define the cycle; picking one **replaces** the previous source (this single picker replaced the old grid of shape buttons + separate *Preset* pulldown + *Use Library…* button). The menu groups:

- **Custom shapes** — user-authored sources whose cycle you define directly:
  - **Draw your own** — freehand canvas; a **Points / Freehand** toggle appears (Points = draggable control points with smooth interpolation; Freehand = drag to paint the shape directly).
  - **Formula** — math expression over one cycle, variable `x` in radians `[0, 2π)`, result clamped to `[-1, 1]`. A language dropdown (Built-in / Lua / Python / GLSL) appears next to the field. See [Formula authoring language](#formula-authoring-language-built-in--lua--python--glsl) and [Terrain Synth](#terrain-synth) for the grammar.
  - **Use Library…** — pull a shape from the project's waveform library into this layer (only when the host wired it; omitted in the LFO / Signal-Shape editor). Opens the waveform browser; a built-in single cycle loads as an independent **copy** (a live *factory reference* until you edit the cycle), and a **saved** waveform loads as a one-time copy *unless* you tick **Sync to library** in the picker, which **live-links** the layer to the asset (later edits to either propagate — the per-layer analogue of the frame-scope Use Library, backed by `WaveLayer::assetId`). The Sync checkbox is enabled only for saved entries (built-ins are immutable templates).
  - **Save to Library…** — publish this layer as a reusable single-layer **Waveform asset** and live-link the layer to it, so further edits propagate to every layer/frame that references it. The per-layer **Save** half of the Use/Save pair (mirrors the frame-scope and Summation-Morph Save buttons).
  - **Unlink from Library** — detach this layer's live link to a saved waveform (`WaveLayer::assetId = -1`) while keeping the current cycle as an independent editable copy, so edits stop propagating to/from the asset. Shown **disabled** (greyed) while the layer isn't currently linked. The per-layer member of the three Unlink affordances (frame / layer / morph).
- **Presets** — a submenu of ready-made starting cycles you can then edit further, split into two sections so you can see at a glance which ones carry extra knobs:
  - **Simple** — the five basic static shapes (**Sine, Saw, Square, Triangle, Noise** — moved here from the former top-level *Static shapes* section, since they're just the simplest ready-made cycles) followed by baked drawn/formula cycles with no extra controls beyond the standard harmonic/phase/amplitude (Half-sine, Ramp up/down, Soft saw, FM bell, Organ-ish, Pulse 25%/75%).
  - **With parameters** — the **wave-defining generators (Type 1)**, where the morph parameter **is** the wave (see [the two types](#one-mechanism-two-user-facing-types)): **Pulse (PWM)**, **Hard Sync**, **FM**, **Phase Distortion**. Picking one is mutually exclusive with a static shape (a generator *is* the shape source) and reveals its **named morph knob(s)**: *Duty* (Pulse), *Amount* (Sync / Phase Distortion), *Index* + *Ratio* (FM). Each of those extra knobs carries its own opt-in **Pin** checkbox (below) so it can be modulated live. The per-layer arbitrary-wave (Type-2) "+ Add" chain was removed (see the note under the knobs below); Type-2 reshaping lives only at the frame-scope [Summation Morph](#where-a-warp-chain-can-live).

> The five bare static shapes (Sine, Saw, Square, Triangle, Noise) now live under **Presets → Simple** rather than a top-level section — they're just the simplest ready-made cycles, so they sit with the other presets instead of in their own list. The top-level **Custom shapes** section holds only the make-it-yourself sources (Draw, Formula, Use/Save Library). The generators are **not** a separate top-level menu section either — they're under **Presets → With parameters**. So everything that's a ready-made starting cycle (with or without extra knobs) is under **Presets**, and everything you author yourself is under **Custom shapes**.

Knobs (all sources):

- **Harmonic** — integer or fractional multiple of the played pitch. 1 = fundamental; 2 = octave up; non-integer ratios produce bell/metallic inharmonic textures.
- **Phase** — 0..1; where in the cycle the layer starts. Matters when multiple layers sum (cancellation and reshaping). Carries an opt-in **Pin** checkbox (see below).
- **Amplitude** — 0..1; loudness contribution to the sum. Carries an opt-in **Pin** checkbox (see below).

Every parameter row is laid out in aligned columns — the sliders are the same length and the **Pin** checkboxes line up in a single column, which simply stays blank for rows that have no Pin option (only **Harmonic**, which isn't modulable here).

**The "Pin" checkbox (was "Mod").** The per-parameter checkbox is labelled **Pin** rather than *Mod*: ticking it adds a control-**input pin** to the node, and that pin can run in either **Mod** or **Set** mode (the two [pin types](#control-inputs-on-parameters-set-vs-mod)) — so "Mod" was a misleading name for the checkbox itself. The checkbox just exposes the input; the pin's mode is chosen on the pin like any other.

**Per-layer Phase / Amplitude / generator-param modulation.** On a **single-frame** layered wavetable each layer's **Phase** and **Amplitude** slider — and, for a generator layer (Pulse / Sync / FM / Phase Distortion), its extra morph knob(s) **Duty / Amount / Index / Ratio** — has a small **Pin** checkbox. Ticking it exposes an on-demand modulation pin on the node so a cable (LFO, envelope, another signal) can drive that value live as the note sustains; the slider then shows **signal-locked** (greyed, with a tooltip) because the synth rewrites it each block. Unticking removes the pin and returns to the baked value. As with op modulation this is **single-frame only** — on a multi-frame table the box is disabled with a tooltip explaining why (the synth re-bakes the lone frame in place per voice; it can't re-bake one frame of a multi-frame grid). Under the hood each pinned field becomes an on-demand **layer-field** `Param` (keyed by layer index and a field code: 0 Phase, 1 Amplitude, 2 the generator's primary param, 3 the FM ratio) that the synth reads via `getParamByLayerField` and feeds into `LayeredWaveform::renderWithLiveOverrides`. See [Modulating an op](#modulating-an-op-the-pin-checkbox) for the shared mechanism. *(A non-harmonic layered mode where each layer's frequency is also modulable is planned but not yet built.)*

> **Per-layer arbitrary-wave morph removed.** Earlier builds put a Type-2 "+ Add" **Layer Morph** chain under each layer (soft-clip / fold / bend / saturate the layer's own cycle before it sums). That was removed: a layer's per-layer morph is now **only its wave-defining (Type-1) generator** (Pulse / Sync / FM / Phase Distortion, chosen in the wave-source picker). Arbitrary-wave (Type-2) reshaping lives **only** at the frame-scope [Summation Morph](#where-a-warp-chain-can-live). The removed chain is kept dormant behind the `enablePerLayerWarp` flag (`layered_wave_editor.cpp`, set `false`) so it can be restored wholesale if wanted.

`+ Add Layer` stacks more; `X` removes one. The waveform preview at the top updates live.

### Formula authoring language (Built-in / Lua / Python / GLSL)

A **Formula** layer has a small language dropdown next to its expression field with four choices. The same dropdown (and the same baking machinery) is reused by the [frequency-domain spectral curves](#frequency-domain-spectral-synth) and the [AHDSR per-segment curves](#per-segment-shape-curves); the per-context differences are the domain of the variable and whether the result is clamped.

- **Built-in** (default) — the `WaveExprParser` expression language (same vocabulary as the Terrain Synth math grammar: `sin cos tan asin acos sinh cosh atan(y[,x]) asinh acosh atanh sqrt inversesqrt exp exp2 log log2 pow abs sign floor ceil round roundEven trunc fract min max mod clamp mix step smoothstep fma saw square triangle noise radians degrees pi e`, `+ - * / ^`, ternary `? :`). The hyperbolic/inverse-hyperbolic, `exp2 log2`, `roundEven fma`, and the `mix step smoothstep fract sign mod atan(y,x) radians degrees inversesqrt` group were added for vocabulary parity with the GLSL shape dialect, so the same waveshaping idioms port between Builtin and GLSL — the Built-in language now covers the complete *scalar* slice of GLSL's Trigonometry/Exponential/Common builtins. Evaluated **live** in pure C++ at the exact resolution requested by the synth — safe from any thread, allocation-free. For a wavetable layer the variable is `x` in radians over one cycle.
  **Multiple "wave objects" in one formula.** A Built-in formula isn't limited to a single expression: write several statements separated by newlines or `;`, assign named intermediates, and let the last statement be the result — e.g. `a = sin(x)` / `b = 0.5*sin(3*x)` / `a + b` builds two waves and sums them. This is the Built-in equivalent of using `local` variables in Lua/Python or `float` temporaries in GLSL, and works in every Formula/curve context (the bake routes a multi-statement source through program mode, evaluating each sample as an independent pure function of the sweep position — assignments don't carry over between samples).
- **Lua** — a sandboxed Lua 5.4 program (base/table/string/math only; `dofile`/`loadfile`/`load`/`require`/`collectgarbage` stripped). The same math helpers are pre-aliased so `sin(x)`, `saw(3*x)`, `clamp(v,lo,hi)` etc. work without `math.` prefixes. Available only in builds compiled with Lua.
- **Python** — an embedded CPython program. `math` symbols and the same helper set (`saw square triangle clamp noise pow fract`) are injected; `random` is available as the `noise()` source. Requires a Python interpreter — see [Python is optional](#python-is-optional-runtime-detection--graceful-disable). When none is present this option is greyed out (with a tooltip explaining why).
- **GLSL** — a GPU **compute shader**: the body you write becomes the inside of `float shapeValue(float x, float f, int i, int n)`, dispatched one thread per output sample on a headless OpenGL 4.3 core context. This is **the most capable waveshaping option** — native GLSL math (`sin cos pow mix smoothstep clamp` …), `TAU`, and the same integer-indexed `waveform(int id, float phase)` factory-bank access the Terrain Synth GLSL dialect uses. The variable contract matches the other languages: `x` is radians `[0, 2π)` for a wavetable layer (normalized `[0,1]` for a spectral/AHDSR curve), `f` is always normalized `[0,1]`, `i`/`n` are the sample index and count. Requires an OpenGL 4.3 compute-capable driver; greyed out (with a reason tooltip) on machines without one. Like Lua/Python it **bakes offline** — there is no live GLSL on the audio thread.

**Why bake to samples.** Lua, Python and GLSL are **not** evaluated on the audio thread. When you edit the expression (or load a project), the script is run once on the UI thread (or the GPU, for GLSL) across a fixed-resolution sweep and the result is cached as a sample buffer (`formulaSamples`, 2048 points for a wavetable layer); the audio path linearly resamples that buffer, exactly like a hand-drawn layer. Built-in stays live because it's pure C++. This is why Python and GLSL are allowed here even though they're forbidden for the real-time script *nodes* — an offline bake has no GIL/GPU-stall/real-time hazard. WebAssembly is intentionally **not** offered for static shapes (it exists for the live DSP nodes, where ahead-of-time native speed matters; an offline bake gains nothing from it, and nobody wants to compile a `.wasm` binary just to define a short curve).

**Source form.** A bare expression (no newline, no `return`) is wrapped automatically — `sin(x)` becomes `return (sin(x))`. A multi-line body is used verbatim and must `return` a value (the wavetable per-layer field is single-line in practice; the spectral editor's field grows to a multi-line box when Lua/Python/GLSL is selected).

**Errors.** A Lua/Python/GLSL compile or runtime error during the bake is shown as a red `Script error: …` overlay on the layer/curve preview, and the baked buffer is zero-filled until the error is fixed. An unavailable language (e.g. Lua in a build without it, or GLSL with no 4.3 driver) is greyed out in the dropdown with an explanatory tooltip.

For **Python** bakes the overlay message is `Type: message (line N)` — the exception class (e.g. `ZeroDivisionError`, `SyntaxError`), its message, and the line number **in your source** (the bake wraps your code in generated scaffolding, but the reported line is remapped back to the line you actually typed, 1-based; an error that lands inside the generated wrapper is reported without a line number). The full Python traceback (the un-remapped, scaffolded form) is written to `seance.log` so the on-screen message stays short. **GLSL** bakes are likewise compiled on demand (the offline headless GL 4.3 compute context — never a per-keystroke check) and the driver's compile/link info log is shown verbatim, except that its line numbers are remapped the same way: an error inside your body reports your source line, not the line inside the generated compute shader (both the NVIDIA `0(L)` and AMD/Intel/Mesa `0:L:` log formats are handled; numbers that fall inside the wrapper are left as-is). The raw un-remapped GLSL log is also written to `seance.log`. For the **built-in** language a lightweight structural check catches unbalanced parentheses, unterminated string literals, and out-of-grammar characters with a line number, but — by design — does not flag unknown identifiers (they evaluate to `0`, matching the tolerant runtime).

**Serialization.** The chosen language is saved alongside the expression (`builtin`/`lua`/`python`/`glsl`). Projects written before a given language existed decode as Built-in (older files never wrote `glsl`). For a **wavetable Formula layer** the baked cycle is now **embedded** in the encoded layer as a `bake=<count>;<s0>;<s1>;…` field (`;`-separated so it survives the comma-split layer parser; written only for non-Built-in languages — Built-in evaluates live and needs no embed). This is a deliberate departure from "store only the source text": the layered codec is decoded **on the audio thread** (a graph rebuild reconstructs the Terrain Synth / Signal Shape processor inside the audio callback), and re-running Lua/Python/GLSL there to reproduce the cycle would touch the message-thread-only interpreters — which corrupts the CPython interpreter and crashes deep in `python3xx.dll`. Embedding the baked cycle means the audio thread renders from data, never an interpreter (`WaveLayer::rebakeFormula()` additionally refuses to bake off the message thread as a safety net). Projects saved before this change have no `bake=` field; they are detected (`LayeredWaveform::decodedNeedsBakeEmbed`) and re-baked + re-encoded on the message thread at load (`migrateLayeredScriptEmbedBake` in `openProjectFile`, plus the Signal-Shape sibling loop) so the embed is present before the graph goes live. (Spectral magnitude/phase and AHDSR segment curves are still re-baked on load via `SpectralCurve::rebake()` — they are not decoded on the audio thread, so they keep the source-only form.) See `known-issues.md` ("Python interpreter run on the audio thread").

### Python is optional (runtime detection & graceful disable)

Python is **not** required to build or run SEANCE. Everything else — the built-in expression language, Lua, and WebAssembly — is independent of it. Where Python fits in, and how it degrades when absent:

**What uses Python.** Three surfaces embed CPython: the **Script Console** (`Tools → Script Console`, the algorithmic project-manipulation API — `import soundshop`), **Python signal evaluation** (live param-binding scripts), and the **Python shape baker** (the `Python` option in every Formula/Equation language dropdown — wavetable Formula layers, spectral magnitude/phase curves, AHDSR segment curves). Lua and Built-in never touch Python.

**Can scripts import libraries?** Yes. The console runs a normal CPython interpreter, so `import` works for the standard library and any third-party package installed in the interpreter SEANCE links against (`sys.path` is also seeded with a `scripts/` folder next to the exe and relative to the working dir, so SEANCE's own `soundshop_tools` / `soundshop_music` helper modules import cleanly). There is no separate "start" call — running code in the console *is* the entry point; just `import` what you need at the top. The offline shape baker is a sandboxed subset (a fixed math vocabulary + `random`) and is not meant for arbitrary imports.

**Which Python does it use?** SEANCE links one specific CPython ABI chosen at **build** time (`find_package(Python3)`, or a hardcoded fallback path on the dev machine). It does **not** scan the system for Python installs at runtime and cannot mix-and-match versions — the interpreter must match the ABI the build linked (e.g. a 3.14 build needs `python314.dll`). The DLL is located by the normal Windows loader search (its install on `PATH`, or the copy placed next to the exe — see below); there's no custom install-detection logic.

**Runtime graceful-disable.** Every entry into the interpreter first checks `ScriptEngine::pythonAvailable()`, which probes the Python DLL with `LoadLibrary` (cached). If the probe fails, the Python C-API is never called, so a missing DLL can never crash a Python feature — Python scripting is simply switched off. `shapeLangAvailable(Python)` returns the same probe, so the **Python** option greys out in every language dropdown (with a tooltip telling you to install Python and restart), and the Script Console opens with an explanatory message and a disabled **Run** button instead of silently doing nothing.

**Why the DLL is shipped, not delay-loaded.** SEANCE links Python at **load time** (the normal import library), and a post-build step copies `pythonXY.dll` next to the exe, so the produced build always has a matching interpreter without relying on a system-`PATH` Python (the interpreter still finds its standard library via its install/registry). We deliberately do **not** use MSVC `/DELAYLOAD` for the Python DLL: CPython's public API pulls in *data*-symbol imports (`Py_None` → `__imp__Py_NoneStruct`, `PyFloat_Type`, the `PyExc_*` exception objects, …), and the MSVC delay-load helper can only thunk *function* imports — linking with `/DELAYLOAD:pythonXY.dll` fails at link time with `LNK1194` ("cannot delay-load … due to import of data symbol"). Because the DLL is load-time-linked, the OS resolves it during process start, so a build whose `pythonXY.dll` is genuinely *deleted* will fail to launch with a loader error — the `pythonAvailable()` probe protects against Python features being *used* when the interpreter is broken, not against the bundled DLL being removed. (True launch-without-the-DLL would require resolving every Python symbol by hand via `GetProcAddress`, including the data exports, or splitting the interpreter into a separately-loaded plugin DLL; see `known-issues.md`.) The `pythonAvailable()` probe still matters: it keeps a stale/mismatched/relocated DLL or a half-broken install from taking down a Python feature, and it's the single switch the UI reads to grey things out.

**Build-time gate.** If no Python is found at configure time at all, the build defines no `HAS_PYTHON`; `scripting.cpp` then compiles to stubs and the whole app still builds and runs with Python disabled (Lua/Built-in/Wasm unaffected) — *this* is the configuration that launches with no Python anywhere on the machine. When Python *is* found at configure time, the load-time link + DLL copy above applies.

**Licensing.** CPython is distributed under the **PSF License Agreement**, which permits redistribution (including shipping `pythonXY.dll` alongside an application) provided Python's copyright notice is retained; it is GPL-compatible and imposes no copyleft on SEANCE. Bundling the DLL is therefore fine.

**Lua and WebAssembly, by contrast, are statically linked** — Lua 5.4 and wasm3 are compiled into the SEANCE binary, so there is no external DLL to be missing and nothing to detect at runtime. They're optional only at **build** time: a build without Lua vendored (`HAS_LUA` undefined) or without wasm3 reports them unavailable via `scriptLangAvailable()` / `wasmRuntimeAvailable()`, and the corresponding dropdown options grey out exactly like Python does — but a shipped build that *was* compiled with them can never lose them on an end-user machine the way a missing Python DLL would.

### Multi-frame wavetables

`+ Add Frame` stacks distinct cycles. The synth gains a **Position** parameter that crossfades between frames; automate / modulate Position for timbral morphing. New frames duplicate the active frame as a starting point.

### N-dimensional grids

A grid starts **one-dimensional** (a single axis of cells) and renders as a **segmented line**: the `[0,1]` track is divided by short perpendicular tick marks at each cell boundary (`i/N`), with the cell's waveform shown as a coloured dot at the segment centre and a `[i]` index label below it. The cells *are* the segments — it's the 1D slice of the 2D checkerboard, so empty cells stay invisible in the viewport just as they do in higher dimensions (use the Cells list to select/assign them). The tick direction follows the projected line so the dividers stay perpendicular under any view rotation.

`+ Dim` adds another axis, up to **8 dimensions** maximum (the floor is 1 — a grid always has at least one axis of cells). Two axes render as the familiar checkerboard, three as a cube, and so on. Each *traversable* axis becomes an independent Position parameter on the synth, so a 3D wavetable has Position X, Y, and Z. Most users won't go past 2D; the cap exists because higher-dimensional grids quickly become impossible to author.

**Only *traversable* axes get a Position parameter.** An axis is traversable once it holds more than one cell — a grid axis that's still 1-wide has nothing for a Position to crossfade between, so it gets no slider/param/pin until you grow it to 2+. Position params are numbered contiguously over the traversable axes (so a grid that's 1×5 exposes a single `Position`, not `Position 2`). Grow an axis from 1→2 and its Position control appears; shrink it back to 1 and the control (and any cable wired into its mod pin) is removed. This keeps the synth's parameter list free of dead knobs that can't do anything.

**Empty cells fade volume** (`absoluteBlend`) — a per-wavetable toggle below the axis-size sliders, the Grid-mode counterpart to Scatter's *Distance fades volume* (it's the same persisted field, relabeled). It controls what happens when the Position morph lands on or near an **empty** cell:

- **Off (default)** — the morph is **renormalized over the filled cells**. The synth builds an occupancy mask (1.0 where a cell holds a frame, cycle or granular; 0.0 where empty) parallel to the wavetable terrain, samples it at the current morph coordinate to get the fraction of N-linear interpolation weight that landed on filled cells, and divides the blended sample by that fraction. The result: empty cells don't drain volume — morphing across a gap keeps the output at full level, carried by whichever filled cells are still in range. A grid with **no** empty cells is unaffected (the mask is all-ones, so the gain is exactly 1).
- **On** — no renormalization. Empty cells are baked as silence and contribute zero to the weighted sum, so morphing toward an empty cell **ducks the output toward silence** (the original pre-renormalization grid behavior). Useful when you *want* gaps in the grid to read as rhythmic/dynamic dropouts rather than being papered over.

Like the scatter toggle, this only changes the gain math — it never adds or removes a Position axis. Both the cycle and granular layers get the same renormalization gain so a granular frame next to empty cells stays full-volume too. Serialized as an optional trailing field on the grid mode spec (`g;numDims;dim0;…;flag`); files written before the toggle existed load as `false` = **renormalized** (the new full-volume default), so an old project with empty cells plays louder than it used to — flip the toggle on to restore the prior fade behavior.

### Scatter mode

`Mode: Grid` ↔ `Mode: Scatter` toggle in the toolbar. In Scatter mode each frame has a free position vector (X, Y, optionally Z, …) and frames are dragged in the viewport. Frames closer to the current Position blend in, distant frames contribute less — smoother and more organic than grid interpolation when frames don't lie on grid intersections.

**Blend kernel.** The default normalized blend uses **scale-free inverse-distance (Shepard) weighting**: `w_i = (d_min / d_i)^p`, normalized to sum-1, where `p` (sharpness) is derived from the **Blend width** slider as `p = 2 / width`. This kernel *always* tracks Position smoothly — there is no setting that hard-switches to the nearest frame or collapses to a static average. (The earlier Wendland-C²-RBF kernel had no usable radius for this: a small radius left every frame outside each other's support so the blend fell back to a nearest-neighbour *hard switch*, while a large radius made every weight near-equal so the blend became a *static uniform average* that ignored the Position sliders — the slider had a razor-thin sweet spot that depended on frame spacing. Shepard removes the radius/scale entirely, so the slider becomes a pure sharpness control that behaves the same regardless of how the dots are spread.) The compact-support **Wendland C²** kernel is retained *only* for *Distance fades volume* mode (below), where a literal cutoff radius — silence beyond it — is the intended behaviour.

**Geometric view dimension (`scatterDims`).** The `+ Dim` / `− Dim` buttons set how many coordinate axes the scatter space has, which drives what the viewport looks like — this is the *geometric* dimension, separate from how many axes are actually *traversable* (below). Scatter starts at **one axis = a line view**: dots sit on a horizontal line and drag left/right along it. `− Dim` down to **zero axes** replaces the line with a small **dashed-circle drop-target** ("Drag a waveform here") at the centre of the view — there's no line or square, just a compact affordance that you can drop a waveform to begin (dots that already exist pile at centre and blend equally). `+ Dim` to **two axes** gives the familiar square view, three the cube, and so on up to 8. The floor is 0 (Grid mode's floor stays 1, since a grid always has at least one axis of cells).

Per-frame coordinate sliders below the viewport allow numeric placement when dragging isn't precise enough.

**Converting between modes.** The sidebar conversion button switches a wavetable between Grid and Scatter. From Grid it reads **`Convert → Scatter`** (always available): every non-empty cell becomes a dot at its cell center, and a snapshot of the original grid layout is recorded. From Scatter the button is **always enabled** and takes one of two forms:

- **`↩ Back to Grid`** (lossless) — shown when every dot is still sitting on its snapshotted cell center *and* the snapshot still matches (you converted from Grid and haven't edited axes or moved dots off-center). Restores the exact original grid layout.
- **`Convert → Grid`** (lossy fallback) — shown otherwise: when the wavetable was authored as Scatter, or you added/removed an axis, or dragged a dot off its cell center (any axis edit resets the snapshot), or the lossless snapshot was dropped (it isn't persisted across save/load). Rather than blindly flattening, it **reconstructs the N-D grid the dots imply, preserving dimensionality**: each axis's dot coordinates are quantized into distinct sorted "tracks", and the grid is sized to those tracks (track-count per axis = cells along that axis). Each dot lands in the cell its quantized coordinates select. For dots that still form a clean Cartesian lattice — e.g. a 2×2×2 grid that was converted to scatter and never moved off-center, even after a save/load — this rebuilds the **original 2×2×2 shape exactly** (not a 1×8 line), and any holes (empty cells) survive too. The flatten-to-1D behaviour is now only a *last-resort* fallback, used when the scatter is irregular enough that the implied grid would be badly sparse (`cell count > 4·dots + 4`) or when two dots quantize into the same cell (not a clean lattice). Once back in Grid mode you can re-add axes and grow cells. This guarantees you're never stranded in Scatter mode.

**Which Scatter axes are traversable is purely positional, and evaluated per-dimension** — exactly analogous to a grid axis needing ≥2 cells. A scatter dimension *d* gets a Position param **iff the dots actually span a range along *d*** — i.e. they don't all share the same coordinate on *d*. Consequences:

- **0 or 1 dot → no Position params at all.** A single point has no extent to traverse. This holds **regardless of the blend mode** (normalized vs *Distance fades volume*) — the blend mode only changes the gain math, never which axes exist.
- **2 dots differing only in X → just an X axis** (`Position`).
- **2 dots differing only in Y → just a Y axis** (`Position`), with *no* X axis. Each dimension is judged independently and symmetrically.
- **2 dots differing in both → both axes** (`Position 1` = X, `Position 2` = Y).

Drag a dot until it stops differing on an axis and that axis's Position control (and any cable wired into its mod pin) disappears, just like shrinking a grid axis back to one cell. The re-sync happens once on **mouse-up** at the end of the drag gesture (never per drag tick — adding/removing params touches state the audio thread reads lock-free). On the synth side the query point pins every inert axis to the dots' shared coordinate on that axis, so an inert axis contributes zero to the blend distances no matter where the dots sit.

The **Selected waveform position** strip in the sidebar shows one slider per **geometric** axis (`scatterDims`), *not* per traversable axis — it's an editor placement control, deliberately decoupled from the synth's Position knobs (which track the traversable set above). This is on purpose: a lone dot on a 1-D line has no traversable axis yet, but you still need a slider to place it — and moving dots apart along a geometric axis is exactly how you *create* a traversable one. Tying the strip to the traversable set instead produced a chicken-and-egg dead end (no slider on a lone dot → no way to spread a second dot away from it numerically). Dropping a second waveform via **Add to grid** offsets it along X from the existing dots (so it lands beside them and immediately yields a traversable X axis rather than stacking invisibly); drag it elsewhere afterward (e.g. to differ in Y instead) to change which axes exist.

**Distance fades volume** (`absoluteBlend`) — a per-wavetable toggle below the **Blend width** slider. By default the blend weights are **normalized** (sum-to-1) Shepard weights, so the blend always equals a full-volume weighted average of the frames — moving Position around changes *which* frames you hear but never the overall loudness. With *Distance fades volume* on, the blend switches to **compact-support Wendland C²** weights used directly as **gain** (no normalization): each scatter dot becomes a "loudness island" that's loudest at its center and fades to silence at the edge of its radius (here the Blend-width slider is the literal fade radius in normalized [0,1] units), and a Position sitting in the gap outside every frame's radius is **intentionally silent**. This only affects the gain math — it does **not** create or remove any Position axis (that's purely the dots' spatial spread, above), so it's only meaningful once you have two or more dots to span an axis. The same `absoluteBlend` flag drives the Grid-mode **Empty cells fade volume** toggle (below) — the two modes share one persisted field, relabeled per mode.

### Stereoscopic 3D viewport

<a name="stereoscopic-3d-viewport"></a>The arrangement view can render the frame positions with real stereoscopic depth. The **View** combo at the top of the arrangement view's sidebar picks the mode (`ScatterView::StereoMode`):

| View mode | What it draws | What you need |
|---|---|---|
| **Flat 2D** (default) | One monoscopic projection, no rotation applied. | Nothing. |
| **3D Anaglyph (red-cyan glasses)** | One composite image with the left eye in the red channel and the right eye in cyan. | Red/cyan paper glasses. |
| **3D Cross-eyed stereoscope** | Side-by-side pair; the **left** half carries the **right** eye's image. You cross your eyes until the two halves fuse into a third image in the middle. | Nothing — but it takes practice. |
| **3D Parallel stereoscope** | The same side-by-side pair with the eye assignment swapped, so the left half is the left eye. Viewed "wall-eyed" (diverging, as if looking through the screen). This is how a Holmes stereoscope or a VR headset presents a pair. | Nothing, or a stereoscope. |

Cross-eyed and parallel are the same two images in the opposite order — the only code difference is the sign of `leftEyeSign`. Pick whichever fusing technique you can do; if a mode looks *inverted* (near things reading as far), you're using the wrong one for your technique, so switch to the other.

**All three 3D modes** (not just anaglyph) count as `is3D()`, which turns on the yaw/pitch rotation, reads the Z projection axis, and enables perspective size scaling. **Right-drag orbits the camera** around the (X,Z) and (Y,Z) planes; other plane angles come from the rotation sliders in the sidebar. In a split mode the two halves each carry a corner label (`(left eye)` / `(right eye)`; anaglyph labels itself `(red-cyan glasses)`), and mouse hit-testing resolves against whichever half the pointer is in (`sceneRectForPoint`), so clicking and dragging dots keeps working in stereo.

**Parallax is computed from real-world geometry**, not a fudge factor (`applyParallax`). The screen plane sits at z=0 with the viewer's eyes at distance *D* in front; a point at depth *z* gets a per-eye disparity of `(IPD/2) · z/(D+z)`, converted from mm to pixels via the display DPI. Points behind the screen get uncrossed disparity and fuse behind it; points in front fall out of the same formula with crossed disparity. The inputs are currently fixed at **IPD 63 mm**, **viewing distance 600 mm** and **scene depth 60 mm** (the physical depth the unit cube maps to), with **DPI auto-detected** from the JUCE display at construction. They are not yet exposed as sliders — see `known-issues.md`.

The projection combo selects which axes drive screen X/Y/Z when the space has more than 3 dimensions. Visualization aid only; none of this affects the audio.

### Frame types

Most frames are layered-waveform single cycles. Two specialized frame types are supported:

- **Wavelet frame** — edited in `WaveletPainter`; paints wavelets at chosen time/frequency positions and reconstructs the cycle.
- **Granular frame** — a multi-second slice of recorded audio (e.g. captured from a region via "capture from song") that the synth sustains as a held note. Edited in `GranularFrameEditorComponent`. The frame stores a **freeze mode**, a **grain length** (the size of each overlapping grain — used only by the two grain-cloud modes; set by the Width slider at capture time), a **grain count** (`grainCount` — how many overlapping grains form the cloud in the two grain-cloud modes; default 4), an **FFT size** (`fftSize` — the SpectralFreeze analysis size; `0` = auto), a **freeze-window position and width** (`windowStart` / `windowLen` — which sub-slice of the capture to freeze, and how wide; in Crossfade mode the window *is* the loop), a **crossfade length** (the seam blend), and an **embedded pitch** (the note at which the window plays back 1:1). See [Grain count & FFT size](#granular-grain-fft) for the two texture controls.
  - **Freeze window (draggable, resizable selection band).** <a name="granular-freeze-window"></a>The capture is typically longer than one grain — **both** capture paths (song and mic/file/playback) now store one freeze **window** (`buildFrames` → `buildGrainSource`) plus a **half-window lookahead tail** for the seam, the window auto-sized per freeze mode — `autoWindowMultiplier(mode) × grain` (4×) for the grain-cloud modes, a fixed `kNonGrainAutoWindowSamples` (≈ 100 ms, **decoupled from the grain**) for Crossfade/Spectral — until you override it — leaving room to choose *where in the capture* the freeze sits and *how wide* a slice it uses. The frame editor draws an **amber selection band** over the source thumbnail: **drag the middle to move it, drag either edge to resize it**. The band is resizable in **every** mode and always sets the freeze **window** (`windowStart`/`windowLen`) — it never edits the grain length. What that window *means* depends on the mode, and only the two grain-cloud modes actually use a grain:
    - **Async / Pitch-synced grains** — the band is the **window the grains roam over**. Its floor is the **grain length** (a grain must fit) and its ceiling is the **whole capture**. The window width is **decoupled from the grain**: the grain is the size of each overlapping Hann grain, the window is the region the grains roam over. A window *wider* than the grain gives these two modes the multi-grain roam room that makes them sound different; pinning the window to exactly one grain wide collapses both to the same near-static single grain. These are the **only** modes where the Grain-length slider does anything.
    - **Crossfade loop** — the loop **is the whole band**, so resizing sets the **loop length** directly (a wide band = a long evolving loop; a narrow one = a short pitched buzz, down to ~5 ms). There are no grains, so the **Grain-length slider is greyed out** with a tooltip explaining why (per the "grayed-out controls must explain themselves" rule).
    - **Spectral freeze** — the band sets the **FFT analysis region** (floor ≈ one FFT block, 256 samples). Grain length has no effect, so the **Grain-length slider is greyed out** here too.
    - **Single cycle** — the band's **start** picks where in the capture the one cycle is detected from; the mode auto-detects a single pitch period there and loops it. There are no grains, so the **Grain-length and Grain-count sliders are greyed out** with explanatory tooltips.
    
    The **Grain length slider** (active only in the two cloud modes) is capped at the captured duration (`min(`[`kGranularMaxGrainMs`](#granular-freeze-window)` = 500 ms, capturedMs)`) since the grain must fit inside the window which fits inside the capture; growing the grain raises the cloud window's floor. A **dashed** band means *auto* (the stored `windowStart`/`windowLen` are both the sentinel `-1`); the instant you drag or resize it the band becomes **solid** (explicit `windowStart`/`windowLen ≥ 0`). A **size read-out** under the thumbnail (`updateSourceInfoLabel()`) shows the source length, sample rate, and **freeze-window width in ms** — plus the **grain length in ms** in the two grain-cloud modes — so you can directly compare *window vs grain* (e.g. `820 ms @ 44 kHz · window 240 ms · grain 60 ms`). It refreshes live as you drag the band, change the grain length, or switch freeze mode (the grain term appears only in Async / Pitch-sync, where the grain is meaningful). Dragging/resizing drives the live audition and commits on the editor's debounced undo path (one *Edit wavetable* step per gesture), exactly like the param sliders. The editor's `minWindowSamples()` is the per-mode resize floor (grain for the cloud modes, 256 for Spectral, ~5 ms for Crossfade) and mirrors the voice's `bandLen` floor in `granular_freeze.cpp`; `effectiveWindowLen()` returns the clamped `windowLen` (or one grain when auto). In Crossfade mode the **crossfade-seam cap tracks the window** (`crossfadeMaxSamples()` = window/2), so shrinking the loop shrinks the crossfade slider's range with it.
    - **`windowStart` / `windowLen` semantics & back-compat (two auto sentinels).** `windowStart` defaults to `-1`; `windowLen` defaults to `-2` (`kWindowAutoPerMethod`). `windowStart == -1` = auto-centre: the voice positions the band with the historical centred-with-lookahead formula `max(0, (srcLen − (grain + grain/2)) / 2)`. The **width** has *two* auto markers, resolved by the shared `resolveAutoWindowLen(windowLen, mode, grain)` (`granular_frame.h`) so the voice, the editor, and all three capture dialogs agree:
      - **`kWindowLegacyAuto` (`-1`)** = "auto = exactly one grain wide" — the original one-grain window. **Every frame captured or saved before these fields existed decodes as `-1`** and MUST keep resolving to `grain` so those frames stay **byte-for-byte identical** on reload. Never repurpose this value.
      - **`kWindowAutoPerMethod` (`-2`)** = the per-mode auto width, recomputed whenever the grain length or freeze mode changes, resolved by `resolveAutoWindowLen`: the **grain-cloud modes** (AsyncGranular / PitchSyncGrains) resolve to `autoWindowMultiplier(mode) × grain` (**4×**, so the cloud modes get several grain-periods of roam room out of the box — a 1× window collapses them into a one-grain loop), while the **non-grain modes** (CrossfadeLoop / SpectralFreeze) resolve to a **fixed `kNonGrainAutoWindowSamples`** (4800 samples ≈ 100 ms @ 48 k, ≈ 109 ms @ 44.1 k) that is **independent of the grain length**. This decoupling is what lets the grain default drop to ~5 ms (for a smooth constant async cloud) **without** collapsing the default Crossfade loop: the Crossfade/Spectral window keeps its historical ~100 ms framing instead of shrinking to one tiny grain. (`resolveAutoWindowLen` has no sample-rate parameter — the editor, voice, and capture dialogs all share it — so the non-grain default is expressed in **samples**, not ms.) **Freshly created and freshly captured frames default to `-2`**, so a new Async/PitchSync capture no longer degenerates into a loop. Retuning `autoWindowMultiplier` is **forward-only**: it only affects new frames carrying `-2`; frames already saved keep whatever width they resolved to, so old projects never shift.
      Either sentinel only becomes an explicit (`≥ 0`) width once the user drags/resizes the band (editor) or moves the window slider (capture). On disk the auto state rides a **dedicated flag field** so `-2` survives a round-trip without breaking the `+1`-biased width encoding (the width token itself can't represent `-2` once biased to `-1`). In auto mode each freeze mode keeps its *original* per-mode framing (e.g. Async granular roams the whole source, Spectral freeze analyses the loop centre); once banded, **all four modes confine their reads to `[windowStart, windowStart+windowLen)`** (the grain cloud roams within it, **Crossfade loops the whole window**, Spectral analyses its centre). `windowLen`'s floor is **mode-dependent**: the two grain-cloud modes clamp it to `[grainLength, srcLen]` (a grain must fit), while CrossfadeLoop and SpectralFreeze floor it at 16 samples — they don't granulate, so the window can be shorter than the (greyed-out, inert) grain. Both persist as optional header ints in the wire format (written biased by `+1` so `-1` stays a non-negative all-digit token; decode back via `token − 1`). When the band sits at the source's far edge there may be little or no lookahead tail, so the Crossfade-loop seam is clamped to whatever lookahead remains before `srcLen` (a zero-lookahead band gets a hard, un-crossfaded loop rather than a thump).
  - **"As note" picker retunes the live audition.** Both the capture dialog and the frame editor expose a Note/Octave "As note" picker that sets the embedded pitch. The preview marker audition (and the frame editor's fallback Play) resample the grain to **`(440 Hz / embedded pitch) × (sourceSampleRate / projectRate)`** so picking a note is *immediately audible* and matches what the synth voice will produce when the frame is triggered at the editor's reference note A4 (MIDI 69). At the default A4 *and* when the capture rate equals the graph rate the ratio is 1 (native rate). The capture preview is driven by `AudioEngine::setPreviewGrainRatio` (a fractional, linearly-interpolated playhead in the GrainLoop preview).
    - **Sample-rate reconciliation (`srRatio`).** The synth's grain reader (`terrain_synth.cpp` `renderGrainSample`) always folds a `srcSampleRate / projectRate` term into its read ratio, because captured playback frames store PCM at the **tap rate** (the device/loopback rate, e.g. 44.1 kHz) without resampling, while the synth renders at the **project/graph rate** (e.g. 48 kHz). The two previews — the capture dialog's marker audition (`regenerateAuditionGrain`) and the editor's fallback (engine-preview) Play — now fold the **same `srRatio`** in. Without it, a frame captured at 44.1 kHz and rendered at 48 kHz auditioned a few semitones **higher** in the preview than the placed synth note played (`44100/48000 ≈ 0.92`, ~1.5 semitones), which is the "plays back a few notes lower in the editor than in the capture preview" symptom (the editor/synth path was correct; the preview was sharp). The song-capture source is pre-rendered at the project rate, so its `srRatio` is naturally 1.
  - **Unified save model — re-capture writes metadata through live.** The frame editor (`GranularFrameEditorComponent`) and the capture panels used to disagree about *when* an edit sticks: the editor commits every control change instantly (it holds the live frame by reference), while the capture panel staged everything and only committed on the **Save / Capture** button — so closing it discarded any pitch/freeze/crossfade change you'd made. They're now reconciled across **all three sources**. When you **"Re-capture from song / mic / file…"** to replace an existing frame, the capture panel — `CaptureFromSongDialog` for song, `CaptureFromPlaybackDialog` for mic/file — is **bound to that specific library frame**:
    - Its editable controls (the "As note" picker, **Freeze** mode, the **Grains** / **FFT size** texture controls, and the **Crossfade** slider) are **seeded from the frame's current values** on open (`seedFromExistingFrame`), so the panel reflects reality instead of resetting to defaults (A4 / Crossfade loop / default grains / FFT / crossfade). Both dialogs now have a Crossfade slider, so this is symmetric across all three sources.
    - Any change to those **commits straight through to the frame the instant you make it** — same path the editor uses (`onMetadataEdited` → frame mutation → `onLayerChanged()` → node-script commit + dirty flag + debounced undo). Closing the panel can no longer silently revert a metadata edit.
    - Only the **PCM grab itself** still requires the explicit Save / Capture button, because it depends on capture-time params (both dialogs' region selection + per-waveform window + waveform count) — re-grabbing audio is inherent to "capture", not a save-model quirk. Closing without it keeps your committed metadata but leaves the original audio in place. (In **append** mode, where no pre-existing frame is bound, there's nothing to write through to, so creating a brand-new frame stays an explicit Save and closing legitimately creates nothing. **Neither** dialog wires an append-mode write-through sink now: both slice the selection into N waveforms per Capture, so there's no single unambiguous frame to bind to. Write-through is wired only in **replace** mode, where exactly one frame is bound.)
    - Crossfade crosses the boundary in **milliseconds** (rate-independent); the host converts to samples against the frame's own `sourceSampleRate`, so a render rate ≠ frame rate can't misinterpret the seam length. **Both** capture dialogs now expose a Crossfade slider, so the mic/file/playback dialog seeds the frame's crossfade ms into its slider on open and writes any change back through `onMetadataEdited` — the same write-through path as pitch / freeze / grains / FFT, no longer a no-op echo.
    - **Re-capture targets the frame's original source.** Each granular frame remembers which source produced it (`GranularFrame::captureSourceKind`: song / mic / file), so the editor's button reads **"Re-capture from song…"**, **"Re-capture from mic…"**, or **"Re-capture from file…"** to match, and clicking it re-opens that same capture panel — a mic-captured frame re-captures from the mic, not the song. The source is stamped on at capture time and **persisted** in the frame's wire format as an optional header int (written biased by +1 so it stays an all-digit token; `0` decodes to "unknown"). Frames from projects saved before this field existed — or built from scratch / duplicated — have an **unknown** source and fall back to the historical **song** default for both the label and the panel.
  - **Freeze modes** — `CrossfadeLoop` (default), `AsyncGranular`, `PitchSyncGrains`, `SpectralFreeze`, `SingleCycle`. **All five are implemented.** Crucially, the capture/freeze audition and the held synth note share **one** reader — `GrainFreezeVoice` (`granular_freeze.h`/`.cpp`) — so **what you audition is exactly what you get**: there is no separate "audition" algorithm that could drift from playback. The audition engine (`AudioEngine`) owns a single `GrainFreezeVoice`; the synth owns one per voice per granular frame (so a 3-way Position morph runs three independent streams). All five modes sustain the captured window and resample their output by a `ratio` so a held note tracks MIDI pitch — at the note matching the embedded pitch the ratio is 1 (the audition reproduced 1:1), other notes transpose. The voice re-anchors itself whenever the source, length, grain length, [freeze-window position or width](#granular-freeze-window), or mode changes, and is allocation-free in steady state (buffers and the SpectralFreeze FFT are built only on (re)initialisation, mirroring the lazy-on-note-on pattern). The five characters:
    - **Crossfade loop** — a faithful tape loop of the captured window with a Hann crossfade across the seam (so the wrap doesn't click). The loop **is the whole selection band**, so its length is set by **resizing the amber band** (not the grain slider, which is greyed out here): a wide band is a long, evolving loop; a narrow one is a short, pitched buzz. Moving the band repositions the loop. In the voice, `loopLen = bandLen` (the band width), and an auto window (`windowLen == -1`) resolves `bandLen` to `grainLen` so pre-band frames still loop exactly one grain. *"What does this spot literally sound like."*
    - **Async granular** — a bank of [**grain-count**](#granular-grain-fft) overlapping Hann grains (default 4, 75 % overlap at 4) that re-trigger at randomised positions jittered across the source, summed and gain-normalised for unity (the `2/N` Hann-COLA factor, = the historical `×0.5` at N = 4). A frozen blur / GRM-Freeze texture rather than an exact loop. In the legacy (un-banded) path the grain is a **short (~80 ms) "blur" grain** — capped at `min(80 ms, srcLen/2)` — *not* the full loop width, because the un-banded capture path passes the whole loop length as the grain size, which is far too long to overlap into a blur. **With an explicit [freeze window](#granular-freeze-window) the grain is the user's chosen grain length** (fit inside the band), and the grains roam over the band's width — so a band wider than the grain produces a real moving cloud, while a band pinned to the grain gives no roam room (a near-static single grain). The roam range is bounded so **every grain read stays inside the window**: un-banded `aStart ∈ [0, srcLen − grain]`; banded `aStart ∈ [bandStart, bandStart + windowLen − grain]`. *(Bug fix: the old code used a full-loop-length grain centred at `srcLen/2`, so every grain read ~25 % past the end of the 1.5×-loop capture source and clamped to the DC tail — the audition came out ~20 dB below the other modes. Now in-bounds and at a comparable level.)* **Ratio-aware roam clamp (pitched-up grains).** A grain advances its source read by the playback `ratio` per envelope sample, so over its `grain`-sample life it sweeps `grain × ratio` source samples. The init-time `aRoamHi` (`= aWindowHi − grain`) only leaves room for a `ratio = 1` sweep, so a held note played **above** the embedded pitch (`ratio > 1`) used to let grains start high enough that their sped-up sweep ran `grain × (ratio − 1)` samples **past the window's upper edge** into the half-window lookahead tail (decorrelated content the user never selected) — a contributor to the "doesn't sound constant" blur. `asyncSample` now tightens the re-trigger high bound to `aWindowHi − ⌈grain × max(1, ratio)⌉` so the whole sweep stays inside the band; at `ratio ≤ 1` it equals the original `aRoamHi`, so native-and-below playback is byte-for-byte unchanged. **Async constancy is still fundamentally governed by grain length and grain count** — a 4-grain cloud of long grains over a wide roam window is inherently a decorrelated blur, so **shorter grains** and/or **more grains** (the *Grains* control) sound smoother/more constant; that's the texture's nature, not a bug.
    - **Pitch-sync grains** — the **pitch-coherent twin of Async granular**. It runs the *same* machinery — a bank of [**grain-count**](#granular-grain-fft) overlapping Hann grains (default 4, 75 % overlap at 4, summed and gain-normalised for unity) forming a **stationary random scatter** over the captured window (a freeze, **not** a playhead sweeping forward through the source) — with one change: every grain origin is **snapped to the source's pitch-period grid** (origin = an integer multiple of the detected period). Because overlapping grains then sit a whole number of periods apart, they read the **same waveform phase** and sum **coherently**, so the cloud has a definite, stable pitch instead of Async granular's random-phase comb-filtered blur. The many period-aligned grains are the "grains" (plural) the mode is named for; the slight timbral variation between grains drawn from different parts of the note keeps it a living grain cloud rather than a dead single cycle, while the pitch stays rock-solid. **This only differs audibly from Async granular when the [freeze window](#granular-freeze-window) is wider than the grain** — the snapping only matters when the window spans several pitch periods that grains can roam across; with the window pinned to one grain wide both modes collapse to the same near-static single grain. Grain length is a **whole number of periods** nearest the grain target — the user's grain length when banded, ~80 ms in the legacy un-banded path — (≥ two periods) so the snap grid stays clean and the Hann seams are smooth. The period is **detected from the source itself** by autocorrelation (`detectPeriodSamples` in `granular_freeze.cpp`, middle ≤100 ms, 50 Hz–5 kHz), *not* taken from the embedded-pitch label; it falls back to `sampleRate / embeddedPitch` only when no confident pitch is found (noisy / inharmonic source). A window too short to hold whole-period grains falls back to the single-cycle crossfade loop (`pitchSyncSample` with `psGrain == 0`). *(Bug fix: it previously (a) trusted the embedded pitch, which defaults to A4 and is usually wrong, so the mode buzzed at 440 Hz regardless of what was captured, and (b) merely looped one cycle — a single static tone — contradicting the "grains" name; it now detects the real period and overlap-adds a stationary cloud of period-snapped grains, coherent and full-level where the same cloud without snapping (Async granular) comb-cancels to a much quieter blur.)*
    - **Spectral freeze** — a phase-vocoder freeze: one Hann-windowed FFT captures the magnitude spectrum, then synthesis re-randomises the phases every hop (specN/4), IFFTs in place, and overlap-adds into a streaming ring the per-sample reader resamples by `ratio`. An ethereal pad sustain decoupled from the source's time-domain identity. The FFT size follows the [**FFT-size** control](#granular-grain-fft): **Auto** (`fftSize == 0`) keeps the historical pick — the largest power-of-two ≤ min(analysis window, 2048), ≥ 256, else silent — while an explicit size requests the largest power-of-two ≤ min(chosen size, analysis window, 8192), floored at 256 (so a window smaller than the request still caps to what fits). Larger sizes give finer frequency resolution (more bins) at a coarser time window. The amber band sets the **analysis region** (resizable, like the cloud window), but **grain length has no effect in this mode, so the Grain-length slider is greyed out** with an explanatory tooltip. All FFT scratch buffers are reused across hops, so steady-state synthesis allocates nothing. The analysis window is centred on the **centre of the loop region** (`grainLen/2`), not the centre of the whole 1.5×-loop source — the extra half is a lookahead tail past the loop (the decayed end of the captured note), so centring there made the freeze far quieter than the loop modes. *(Bug fix: re-anchored to a representative spot and the OLA gain raised from 0.8 to 1.0 so the freeze sits at roughly the Crossfade-loop reference level.)*
    - **Single cycle** — detect **one** pitch period and loop just that single cycle, turning the captured spot into a **static single-cycle-oscillator tone**: one dead, perfectly-periodic cycle (the fixed-timbre counterpart to Pitch-sync grains' *living* period-snapped cloud — no grain scatter, no timbral evolution, just one repeating cycle at the source's pitch). The period is detected by the **same autocorrelation** `detectPeriodSamples` used by Pitch-sync grains — **not** zero-crossing, so it locks cleanly onto complex / inharmonic material; it falls back to `sampleRate / embeddedPitch` only when no confident pitch is found. In the voice this is the `loopSample` reader with `loopLen` = one detected period (it reuses the Crossfade-loop machinery): the seam is **Hann-crossfaded against the adjacent (one-period-further-on, same-waveform-phase) period**, so any small period-detection error blends out instead of clicking — directly addressing "just repeating where it crosses zero wouldn't be good enough". The amber band positions where in the capture the cycle is taken from (band start); like the other non-grain modes, **the Grain-length and Grain-count sliders are greyed out** (there are no grains — the loop is exactly one period), with explanatory tooltips. The **Crossfade** slider still applies (the seam blend), clamped at use time to half the detected period. Existing nodes are unaffected — this is purely an additional mode; new captures keep defaulting to Crossfade loop.
  - **Grain count & FFT size — per-mode texture controls.** <a name="granular-grain-fft"></a>Two controls set the *density / resolution* of the freeze, each one relevant to a different subset of modes. They appear in the frame editor (`GranularFrameEditorComponent`) and in all three capture dialogs, and bake into the frame (`GranularFrame::grainCount` / `fftSize`).
    - **Grains** (`grainCount`, range 2–16, default **4**) — how many overlapping Hann grains make up the cloud in the two grain-cloud modes (**Async granular**, **Pitch-sync grains**). More grains = a denser, smoother cloud; fewer = sparser and more granular. The level stays constant as you change it because the voice gain-normalises by the Hann constant-overlap-add factor `2/N` (= the historical `×0.5` at N = 4, so old frames are byte-for-byte identical). The floor is **2**: at N = 1 a single Hann grain pulses in amplitude (COLA fails), so one grain isn't offered. **Greyed out / hidden** in Crossfade loop and Spectral freeze (no grains); the editor greys the slider with a tooltip, the capture dialogs hide it (it shares a row slot with the FFT-size control).
    - **FFT size** (`fftSize`, **Auto** + {256, 512, 1024, 2048, 4096, 8192}, default **Auto** = `0`) — the FFT analysis size for **Spectral freeze** only. Auto reproduces the historical pick (largest power-of-two ≤ min(window, 2048)); an explicit size requests the largest power-of-two ≤ min(chosen size, window, 8192), floored at 256. Larger = finer frequency detail (more bins) but a coarser time window. **Greyed out / hidden** in every non-spectral mode.
    - **In the capture dialogs** the two controls read **"Grains per waveform:"** and **"FFT size per waveform:"** — the "per waveform" wording disambiguates because a multi-waveform capture turns one dialog selection into N successive frames, each baked with the chosen count / size. In the single-frame editor they're plain **"Grains"** / **"FFT size"**. Both capture dialogs ([`CaptureFromPlaybackDialog`](#frame-types) for mic/file/playback, `CaptureFromSongDialog` for song) put the active control to the right of the **Freeze:** picker, show only the one that applies to the current mode (`refreshFreezeExtras()` toggles visibility), and drive the live **Preview / audition** through the engine's preview atomics (`AudioEngine::setPreviewGrainCount` / `setPreviewFftSize`) so what you hear before capturing matches the saved frame.
    - **Re-capture write-through.** In **every** capture dialog's re-capture (replace) mode — song (`CaptureFromSongDialog`) **and** mic/file (`CaptureFromPlaybackDialog`) — the two controls are **seeded from the existing frame** (`seedFromExistingFrame`) and any change **commits straight through** to the bound frame (`onMetadataEdited` now carries `grainCount` / `fftSize` alongside pitch/freeze/crossfade), matching the unified save model used for the other metadata.
    - **Wire format.** Both persist as **optional trailing header ints** in the `__wavetable2__` granular body, appended after `windowLen`: `…<windowStart+1>;<windowLen+1>;<grainCount>;<fftSize>;<s0,s1,…>`. They're plain positive ints (no `+1` bias — `grainCount ≥ 2`, `fftSize ≥ 0` are already non-negative). A frame saved before these fields existed simply lacks the tokens and decodes to the defaults (`grainCount = 4`, `fftSize = 0` = auto), so old projects sound identical. Decode clamps `grainCount` to [2, 16] and `fftSize` to `0` or [256, 8192].
  - **Preview button auditions the frame being edited.** The editor's **Preview**/Stop button (a bottom-left button row, the same label and placement as every other frame editor and capture dialog) routes through the owning synth node's audition queue and plays a held note (A4) at the **edited frame's wavetable Position** — not wherever the live Position knob currently sits. So pressing Preview in the "waveform 2" editor plays waveform 2, even though the synth's default Position would otherwise select waveform 1. This is implemented as a per-voice Position override carried on the audition note-on: the audition voice computes its own granular morph weights from the edited frame's normalized grid/scatter coordinate, while concurrent real MIDI/timeline notes keep following the live Position. In scatter mode the override reproduces the natural blend *at that dot's position* (i.e. exactly what you'd hear by moving Position to the dot), so neighbouring frames still contribute as they would in normal playback.
    - **Unplaced (library-only) frames also audition.** A freshly-captured single frame lands in the **Library** but isn't placed into any grid cell / scatter dot, so it has no Position and isn't in the synth's placed-frame table. To make its Preview button audible (and faithful), the audition note-on carries the **frame's actual PCM + grain params directly** (`AuditionEvent::granularFrame`, a shared copy of the on-screen bytes). When present, the voice renders *only* that frame — full envelope/Volume path, bypassing both the cycle terrain and the placed-frame morph — so the capture you just grabbed plays immediately and exactly as edited, with no wait for the ~150 ms graph rebuild. The same CrossfadeLoop reader serves both the placed-frame morph and this direct path, so audition and playback stay identical.
    - **Audition survives a graph rebuild (live edits while playing).** Resizing the freeze-window band — or changing grain length / count / FFT size / crossfade / pitch / freeze mode — while Preview is held used to **stop the preview** until you pressed Start again. The reason: every wavetable edit fires a debounced (~150 ms) `onNodeEdited → requestRebuild → GraphProcessor::rebuildGraph`, which calls `processorGraph->clear()` and **destroys every live voice**, including the held audition note. The momentary `pendingAudition` queue is **edge-triggered** (consumed once), so nothing re-established the note in the fresh post-rebuild processor. Fixed with a **level-triggered** held-audition channel: `Node::heldAudition` (a `shared_ptr<AuditionEvent>`, guarded by `auditionMutex`) means "a voice should be sounding with this data" for as long as it's non-null. `TerrainSynthProcessor` **reconciles** it each block (`heldAuditionActive` / `heldAuditionPitch` per processor): a fresh processor sees `heldAuditionActive == false` and re-arms the note from `heldAudition`, so the audition seamlessly continues across the rebuild. The editor's `applyEdit()` wrapper **re-publishes the snapshot on every audible edit** while playing (sharing the source PCM `shared_ptr` rather than deep-copying the multi-MB buffer), so the re-armed post-rebuild voice reflects the **new** band / grain / mode — you hear the change instead of silence. Stop (or closing the editor, via `stopPlay()` in the destructor) clears `heldAudition`, releasing the held note. This is separate from and additive to `pendingAudition`, which is still used for momentary piano-roll note clicks. In the **fallback** (engine-preview) Preview path there is no graph rebuild and the preview atomics were already live-updated by `pushPreview*()`, so `applyEdit()` is a cheap no-op there (the freeze-mode change still goes straight to the engine preview).
    - **Preview is audible even when the synth node isn't wired to an Output.** A freshly-added synth node (or one you've disconnected) has no audio path to the speakers, so its rendered audition would normally dead-end in the graph and you'd hear nothing. To keep the preview reliable, `GraphProcessor::rebuildGraph` flags each node's `reachesOutput` (an audio-link reachability walk back from every Output node), and when a synth node *can't* reach output its audition voices are diverted to the `AudioEngine` **audition-monitor bus** — a side buffer the audio callback sums straight into the device output, independent of graph wiring. When the node *is* routed to output, the audition stays in the normal graph path so it still flows through your downstream effects/pan exactly like a played note. Only the editor-Preview audition is diverted; ordinary MIDI/timeline notes on an unrouted node remain (correctly) silent.
  - **Song capture dialog (`CaptureFromSongDialog`).** The **"From project song…"** entry pre-renders the whole project to PCM offline, then exposes the **same region / N-waveform selection model** as the mic/file dialog (two draggable start/end handles, **Waveforms to slice out**, per-waveform **Window length** (ms) window with a **Fit width to selection** button, **Preview waveform** index picker, Gain, Freeze, Grain length, Crossfade, and the embedded-pitch picker — all documented under "all sources" below) **plus a full-song Play / Pause / Stop transport**. The capture model is identical to the file dialog's: `buildFrames(n)` slices the region into N banded `GranularFrame`s using the shared `bandStartForIndex()` geometry, and **Capture waveforms** adds them to the Library. What's unique is how the transport reconciles with the region audition:
    - **Play** = full-fidelity playback of the rendered song with a moving **playhead** (a thin vertical line) anchored at the **region start** (`setPreviewMode(SongPlay)` / `setPreviewSongPosSamples`). The playhead is drawn **only while Playing**; the region handles are left alone (not view-pegged) so the playhead can sweep freely.
    - **Pause / Scrub** = audition the **Preview-index-selected** region band as a looping grain (`setPreviewMode(GrainLoop)` → `regenerateAuditionGrain`), the same GrainLoop mechanism the mic/file dialog uses — so what you hear before capturing matches the saved frame. Grabbing a handle while Playing drops the transport into **Scrubbing**; releasing returns it to **Paused**.
    - **Stop** = silence (`setPreviewMode(Off)`), playhead reset to the region start.
    - **Default region.** Because a song can be minutes long, the dialog opens with a region of ≈ 1 second at the song start (rather than the whole song) so an n≤1 capture doesn't build a multi-minute audition buffer.
  - **Mic / file capture dialog (`CaptureFromPlaybackDialog`).** The **"From microphone / audio input…"** and **"From audio file…"** entries use a simpler shared dialog (no transport, unlike the song dialog): drag the orange handles to pick a region, set **Waveforms to slice out** (1–32, default 8 — the region is split into that many equal slots, one waveform centred in each; `1` = a single frozen snapshot), pick an embedded pitch via the Note/Octave picker, and press **Capture waveforms**.
    - **Edge handles stay fully grabbable.** The two region handles are drawn centred on the selection's start/end sample, so when a handle sits at the very start or end of the buffer its outer half would normally be clipped to the wave-view edge — leaving a thin half-bar that's hard to grab. Instead, both the handle **rendering** and **hit-testing** use a *handle zone* (`handleZone()` = the wave rectangle widened by the `kHandleHitRadius` = 8 px hit radius on each side), and the drag/create repaints cover that same zone so the overhang never ghosts. The result: the full vertical bar (plus its ±6 px end caps) is always drawn and grabbable, even when the selection edge is pegged to the buffer start/end. The wave-view layout's 12 px side margin gives the 8 px overhang room to render and receive clicks. Body-drag and click-to-create still require the click to land inside the wave area proper, so the slop margin only ever grabs an existing handle, never starts a new selection. (Applies to all three sources of this dialog — Mic, File, Playback — and the song dialog, which now uses the same two-handle region model with the identical `handleZone()` / `kHandleHitRadius` geometry.)
    - **Captured waveforms go to the Library only — never auto-placed.** Pressing **Capture waveforms** adds every sliced waveform to the **Library list** and binds the right-pane editor to the first one. It does **not** touch the arrangement: it never grows the grid's cell count, never resizes an axis, and never adds a scatter dot, and the current cell/scatter selection is left as-is. Placement is a separate, explicit step — select a cell (or scatter slot) and press the Library list's **Assign to selected cell**. This matches the **+ Waveform** menu's insert items, which likewise only add a library entry. (Earlier builds appended multi-slice captures along the Position axis automatically; that was removed so capturing can't silently rebuild your wavetable.) Handled by `addCapturedFramesToLibrary()` in `layered_wave_editor.cpp`.
      - **Capturing is undoable.** A capture mutates the editor's doc (new Library entries) on the same debounced path as any other edit, so it lands in the graph undo tree as an *Edit wavetable* step — **Ctrl+Z removes the captured waveforms, Ctrl+Y restores them**. Two things make this work that previously didn't: (1) the editor is a pop-out `DialogWindow`, so its key events never reach `MainContentComponent::keyPressed`; `LayeredWaveEditorComponent::keyPressed` now handles Ctrl+Z / Ctrl+Y itself (first flushing any pending debounce as a real undo step so the capture is committed before the undo runs), and (2) on every snapshot restore the open editor re-decodes its doc from the restored `node->script` via `reloadFromNode()` — without that the editor would keep showing its stale pre-undo library even though the graph reverted. An editor registry (`reloadOpenEditorsAfterSnapshot`) is walked from the main window's `onLoadSnapshot`, so undo/redo triggered from *anywhere* (the editor or the main window) refreshes every open wavetable editor; if the editor's node was deleted by the restore, the window closes itself.
    - **Remembered dialog settings (session-scoped).** The dialog re-opens with the **waveform count**, **Gain**, **grain length** (ms), **crossfade** (ms), and **labelled pitch** (Note + Octave) you last used, so a workflow of capturing many waveforms at the same settings doesn't reset every open. Whether you've **manually moved the grain slider** (`grainUserSet`) is remembered too, so a remembered grain value isn't silently overwritten by the mode-snap default on the next open (see [Grain length](#capture-grain-length)). The values are held in a file-static `captureDialogPrefs()` and written on **every** close — including **Cancel** — so even an aborted open updates the remembered state to whatever you'd dialled in. The prefs are **session-scoped only** (not persisted to disk), and **Window length (the freeze window, ms) is deliberately excluded** because it auto-tracks `autoWindowMultiplier(mode) × grain` for the current selection on each open (remembering a fixed value would fight the auto-default). Defaults on first open: 8 waveforms, 1.00× gain, a **mode-dependent grain** (**5 ms** in Async granular, **40 ms** in Pitch-sync grains — see [Grain length](#capture-grain-length)), 50 ms crossfade, A4.
    - **Reversed slider fill.** The dialog's sliders use a **reversed two-tone fill** — the bright track colour sits to the **right** of the thumb and the dim colour to the **left**, the opposite of JUCE's default. This reads the slider's current `trackColourId` / `backgroundColourId` and swaps them (the same pattern used in `control_bank.cpp`), so the bright portion represents remaining **headroom** above the current value rather than the consumed amount.
    - **Gain (all sources).** A horizontal **Gain** slider on the right half of the embedded-pitch row sets the output level applied to every captured waveform (**0–4×**, 1.00 = the raw recorded level, double-click to reset). It writes the produced `GranularFrame`'s per-frame `gain` (`IWavetableFrame::gain`) — the **same scalar the wave editor's [Gain knob](#per-waveform-gain) drives** — so it's not baked into the PCM: the recording stays at its captured level and the gain is a separate, reversible multiplier you can fine-tune later in the editor. Because granular frames now honour `gain` in the synth's granular layer (the grain reader multiplies by it), the captured level is audible in held-note playback and in the wavetable morph, not just in the preview. The **Preview reflects it live** — `regenerateAuditionGrain` bakes the gain into its throwaway preview buffer (the engine's preview reader has no per-frame gain knob), and moving the slider while a preview runs re-publishes the loop. Use it to level a quiet mic take up or a hot file down at capture time.
    - **Freeze mode (all sources).** A **Freeze:** dropdown (its own row above the embedded-pitch row) picks which granular sustain algorithm the captured frames use — **Crossfade loop** / **Async granular** / **Pitch-sync grains** / **Spectral freeze** (see [Freeze modes](#frame-types) above for what each does). It is baked into every produced `GranularFrame` (`buildFrames` reads `selectedFreezeMode()`) and drives the live **Preview** through the same shared `GrainFreezeVoice` the synth uses, so you can **A/B the four characters before capturing** and what you hear is what a held note will play. Changing it while a preview runs re-anchors and re-publishes the audition live. Mirrors the song dialog's picker — the two were unified so all three capture types expose the full mode set rather than the song dialog alone. To its right sit the mode-specific **"Grains per waveform:"** slider and **"FFT size per waveform:"** combo — see [Grain count & FFT size](#granular-grain-fft).
    - **Grain length (all sources, cloud modes only).** <a name="capture-grain-length"></a>A **Grain length** slider (its own row, **1–500 ms**, 0.5-ms step, seeded from the remembered prefs) sets the length of **each overlapping Hann grain** inside a captured waveform — the same quantity the wave editor's grain control drives. **Short grains make the Async-granular cloud sound constant/steady; longer grains roam over a proportionally wider window and sound more evolving / less constant.** The slider reaches **below 5 ms** (down to 1 ms) for the smoothest possible cloud, though `effectiveGrainLen()`'s 64-sample floor (`max(64, ms × sr)`, ≈ 1.33 ms @ 48 k) means the smallest few values all map to that floor (and the voice enforces a further 16-sample engine floor). It is **only used by the two grain-cloud modes** (Async / Pitch-sync grains): the ms is converted to samples, baked into every produced `GranularFrame`'s `grainLength`, and published to the live preview (`setPreviewGrainLength`).
      - **Mode-dependent default + mode-snap.** The two grain-cloud modes want *different* grain lengths to sound constant, so the default is **per-mode** (`defaultGrainMsForMode()` in `granular_frame.h`): **5 ms for Async granular**, **40 ms for Pitch-sync grains**. Async is a decorrelated blur where many tiny grains blend into a smooth cloud, so a very short grain sounds steady; Pitch-sync snaps each grain origin to the pitch-period grid and needs a few *whole periods* per grain (~40 ms) before the periods lock and the pitch sounds coherent — at 5 ms it warbles. **While you haven't manually moved the grain slider** (`grainUserSet == false`), switching the **Freeze** mode between the two cloud modes snaps the grain to that mode's default, so each mode opens at its good value. **The instant you drag the grain slider** it sets `grainUserSet = true` and the slider **stays where you put it** — no more mode-snapping — and that manual value (plus the `grainUserSet` flag) persists across opens via the session prefs. The non-grain modes (Crossfade / Spectral) don't trigger a snap (the grain is inert there). The frame editor (which edits an existing frame, not a from-scratch default) is unaffected — only the capture dialogs apply the mode-snap. In **Crossfade loop** and **Spectral freeze** the slider is **greyed out** (not hidden) with a tooltip explaining that those modes loop / analyse the whole freeze window instead and pointing at "Window length" for the loop/analysis length — per the "grayed-out controls must explain themselves" rule. In those non-cloud modes the auto window no longer follows the grain at all — it uses the fixed `kNonGrainAutoWindowSamples` default — so a short grain doesn't shrink the loop/analysis window. This is the capture-dialog half of **Option B**: a grain slider *and* a window slider, mirroring the editor, so the two are decoupled exactly as they are in the frame editor.
    - **Crossfade (all sources).** A **Crossfade (ms)** slider (its own row, **1 ms – half the freeze window**, default **50 ms**, seeded from the remembered prefs and double-click to reset) sets the length of the equal-power seam each captured frame uses to hide its loop boundary — the **same quantity the wave editor's and song dialog's Crossfade sliders drive**, so all three sources now expose it symmetrically. It is baked into every produced `GranularFrame`'s `crossfadeSamples` (`buildFrames` reads `crossfadeSlider.getValue() × 0.001 × sr`, still hard-clamped to `windowLen/2` so the two seam halves can't overrun the loop). The cap **tracks half the freeze window live**: `syncCrossfadeMaxToWindow()` re-runs whenever the window changes (window-slider drag, Fit, mode/grain/count change, re-capture seed) and re-ranges the slider to `effectiveSrcLen()/2`, because in the playback model the crossfade loop *is* the freeze window — matching the editor's `crossfadeMaxSamples()` for Crossfade loop and the engine's `xfade = min(req, window/2)` clamp. There is **no 500 ms ceiling** (unlike the song dialog) because the playback window can be many seconds long. The slider stores its value as `crossfadeDesiredMs` (the **intent**, surviving any temporary clamp by a narrow window) and persists/reports *that*, not the visibly-clamped value, so shrinking and re-widening the window restores the seam length you dialled. It is **only used by Crossfade loop** (the other three modes don't loop a seam), so it is **greyed out in every mode except Crossfade loop** — symmetric with how the Grain-length slider greys out in the non-grain modes, and per the "grayed-out controls must explain themselves" rule (its disabled tooltip says the seam only applies to Crossfade loop and to switch the freeze mode to set it). Its `crossfadeDesiredMs` value is **retained while greyed** and still **re-baked into every captured frame** (so a frame captured in another mode keeps a sensible seam if you later switch it to Crossfade loop in the wave editor); in re-capture mode the value still writes through `onMetadataEdited` whenever you do change it in Crossfade mode. (`refreshFreezeExtras()` owns the enable/tooltip for both grain and crossfade; in the song dialog it ANDs the mode test with the render-ready gate so neither lights up before the offline render finishes.) The **Preview reflects it live** (`regenerateAuditionGrain` bakes the same seam into its throwaway loop and re-publishes on drag).
    - **Window length (the freeze window, all sources) + section bands.** Each captured waveform spans a *freeze window* of audio — the region the mode loops (Crossfade), roams (Async / Pitch-sync grains), or analyses (Spectral). All three sources expose it as a **Window length (ms)** slider (`kWindowMinMs` = 1 ms – `kWindowMaxMs` = 12000 ms, **1-ms step**). **It is expressed in milliseconds — the same unit as Grain length and Crossfade — so the window:grain ratio reads off directly** (the old "Samples per waveform" sample-count was hard to compare against the ms grain/crossfade controls). The slider value is converted to samples against the source rate (`windowFromSlider()` = `round(ms / 1000 × sr)`, with the song dialog using `songSampleRate` and the mic/file dialog `tapSampleRate`, fallback 48000). **Until you drag it, the slider auto-tracks the per-mode auto width** (`syncWindowToAuto()`): the grain-cloud modes track `autoWindowMultiplier(mode) × grain` (4×, so they have roam room — a plain `mult × grainMs` multiply since both window and grain are ms), while Crossfade/Spectral track the fixed `kNonGrainAutoWindowSamples` default (converted to ms against the source rate), so their window is **independent of the grain** and a short grain default doesn't collapse the loop/analysis window. It re-runs whenever the grain length, freeze mode, or waveform count changes — exactly mirroring the editor's `kWindowAutoPerMethod` per-method default. Dragging it (or pressing **Fit width to selection**) sets `windowUserSet = true`, after which it stays the explicit ms width you chose across grain/mode changes. The slider is the **source of truth** for the window length when there are **2+ waveforms**: `effectiveSrcLen()` returns `windowFromSlider()` (capped to the selection), and that span is shared by the captured frames, the region audition, and the **shaded orange section bands** drawn over the waveform. (Note the floor is **mode-dependent**: `windowFromSlider()` / `effectiveSrcLen()` floor the cloud modes at one grain — a grain must fit — and Crossfade/Spectral at 256 samples.) `effectiveSrcLen()` also **caps the window to the current selection length** (`min(window, regionLen)`): a per-waveform window can never be wider than the selection it sits in, so the section bands always stay between the two handles — this is what stops the bands from spilling outside the handles after zooming the view or when the handles are squeezed tighter than the window.
      - **Minimum selection = one window length.** While dragging a resize handle, the selection **can't be shrunk below one window length** (`enforceMinSelectionDuringDrag()` pushes the dragged edge back out so the selection stays ≥ `minSelectionLen()` = `windowFromSlider()`). This guarantees at least one whole section band always fits inside the selection, so the bands never get clipped by an over-tight selection. The clamp applies only to the two resize handles; a body drag (which keeps a fixed span) is exempt, and a single-waveform capture (whose window *is* the selection) imposes no minimum.
      - **Zoom can't go tighter than one window.** The view-zoom is clamped so the visible window is never smaller than one freeze window (with 2+ waveforms): `minViewLenSamples()` raises the view's lower bound from the default `kMinViewSamples` floor to `max(kMinViewSamples, windowFromSlider())`, and `reconfigureZoomForWindow()` re-ranges the zoom slider's maximum to `bufferLen / minViewLenSamples()` whenever the window changes (count, grain, mode, window-slider drag, or Fit). So you **physically can't zoom in far enough to make a whole captured waveform not fit on screen** — widening the window pulls an over-zoomed view back out, and the zoom slider simply stops at that limit. This is why the minimum-selection clamp above needs no "window bigger than the view" escape hatch: the window is always ≤ the view. (A single-waveform capture has no independent window, so its zoom keeps the plain `kMinViewSamples` floor and can zoom into a long selection freely. The re-range uses a re-entrancy guard, since `setRange` can fire the zoom slider's change handler, which itself refreshes the window controls.)
      - **Single waveform = the whole selection.** When **Waveforms to slice out is 1**, there is no per-waveform spacing to honour: the one window simply *is* the selection. `effectiveSrcLen()` returns the selection length directly, and the **Window length slider and the Fit button are both disabled** (greyed, with a tooltip explaining the lone window spans the whole selection); the slider still tracks the selection size (shown in ms) for display as you drag the handles. Shrinking the selection shrinks the orange highlight with it.
      - **Band geometry (2+ waveforms) — piecewise, continuous at the tiling point.** The bands are placed by `bandStartForIndex()` in two regimes that meet continuously where the windows exactly tile the selection (`freeSpace = regionLen − n·srcLen == 0`):
        - **Windows fit (`freeSpace ≥ 0`) — uniform-gap.** The leftover space is split into **n+1 equal gaps**: one before the first band, one between each adjacent pair, and one after the last (`gap = freeSpace / (n+1)`). So the two **end margins** (leftmost band → left handle, rightmost band → right handle) always **equal the inter-band gaps** and grow/shrink together as the selection is resized.
        - **Windows overlap (`freeSpace < 0`) — contained.** When the windows are wider than their share they must overlap; rather than let a negative end margin push the outer bands **past** the handles, the row stays **contained**: band 0 flush to the left handle, band n−1 flush to the right, the overlap distributed evenly between (`step = (regionLen − srcLen)/(n−1)`). So the selection never visibly spills its bounds even at heavy overlap.
      - **Constant size on resize.** Because the window is a fixed sample count, **resizing the selection re-spaces the bands without rescaling them**: each band's pixel width is `srcLen × pxPerSample` (independent of region length), so growing the region opens the (uniform) gaps and shrinking it forces overlap, while the bands themselves stay the same size. `paint()` draws the bands from the same `bandStartForIndex()` geometry `buildFrames()` captures, so the drawn bands and the captured audio agree.
      - **Changing the waveform count resizes the selection, not the windows.** When you change **Waveforms to slice out**, the **selection grows/shrinks in proportion** (`resizeSelectionForCountChange()` in both dialogs: `newLen = oldLen × newN / oldN`) so each waveform's window length **and** the inter-band slot spacing (`regionLen / n`) stay **constant** — adding waveforms extends the selection to make room for them rather than cramming more bands into a fixed span (and removing waveforms shrinks it back). The window control itself is count-independent (it tracks `autoWindowMultiplier × grain` or your fixed value), so it doesn't move on a count change — only the **two region handles** do. After the resize the dialog re-clamps the selection to the view, re-syncs the auto window, and refreshes the zoom/preview controls.
        - **Centred on the selected preview waveform.** The resize is **centred on the band the **Preview-waveform picker** currently points at** — that band's centre (computed from the *old* count/region, since the preview slider is re-ranged only after the resize) is held fixed and the new length is laid symmetrically around it. So the waveform you're auditioning stays put under the handles while the others spread out (growing) or close in (shrinking) around it, instead of the selection always growing rightward from a fixed start.
        - **Shrinking stays inside the old bounds.** When the new count is **smaller** (shorter selection), the result is additionally constrained to remain within the **old** selection's `[start, end]`, so a smaller count **never pushes a handle farther out than it already was** — if centring on the preview band would push an edge past where it used to be, that edge is pulled back in (the smaller window always fits inside the old one). Growing has no such cap: a larger count legitimately moves the handles outward, clamped only to the buffer ends (shifted left to fit if it would overrun, never collapsing below ~256 samples).
      - **Per-method auto-default + Fit button.** Whenever the **grain length, freeze mode, or waveform count** changes (and when the region is first established), `syncWindowToAuto()` sets the slider to the per-method default the wave editor uses — `autoWindowMultiplier(mode) × grain` (4×) for the grain-cloud modes, the fixed `kNonGrainAutoWindowSamples` (≈ 100 ms) for Crossfade/Spectral. At that window the bands generally leave gaps or overlap rather than tiling exactly; that's intended, because the window is anchored to the *grain* (cloud modes, so they get roam room) or to a fixed default (non-grain modes) rather than to the slot spacing. The **Fit width to selection** button (next to the slider, disabled at a single waveform) instead sets the slider to exactly the slot spacing (`regionLen / n`, `syncWindowToFitSlots()`); at that window `freeSpace == 0` so all gaps are zero and the bands tile the selection edge-to-edge — handy when you want the windows to abut. Pressing Fit, like dragging the slider, marks the window as user-set so it then stays fixed across grain/mode changes until re-fit or re-dragged.

      Setting the slider by hand (or pressing Fit) overrides the auto-default and is the way to deliberately dial in overlap or gaps. Every band is the same orange as the region tint with thin orange edge separators (no center line; the earlier amber center lines and blue fills were removed). The region-info line reads e.g. `Region: 2.0 s | 8 waveforms, 250 ms apart, each 1000 ms wide`.
    - **Library entry names.** Captured entries are named after their source and freeze method rather than a generic "Waveform N": **Mic / File / Song** + the granular freeze mode (e.g. `Mic - Crossfade loop`, `File - Spectral freeze`, `Song - Crossfade loop`). The word "waveform" is dropped to keep the Library list compact. Every captured entry also gets its **stable library id appended** as a numeric suffix (e.g. `Mic - Crossfade loop 7`, `File - Spectral freeze 12`) so otherwise-identical entries stay distinguishable. The id is the monotonic creation counter (`nextLibraryId`), which is reload-safe and is **never renumbered when other entries are deleted** — so the number you see on an entry is permanent, not a positional index that shifts around as the list changes. The **"Edit from scratch"** and **"Duplicate current"** items in the **+ Waveform** menu are named the same way, after their editor type plus the id suffix: `Layered N` (time-domain), `FFT N` (frequency-domain), `Wavelet N` (wavelet-space), and `Granular N` for a duplicated capture — so all six creation paths produce a self-describing, stably-numbered name rather than the legacy generic `Waveform N`. Re-capture (replace) keeps the existing entry's name; only freshly-added entries are auto-named. Names remain user-editable. Built by `captureEntryName()` (capture base label) / `frameTypeName()` (scratch & duplicate base label) plus the shared id suffix `applyLibraryIdSuffix()` in `layered_wave_editor.cpp`. The old generic `Waveform N` default in `addLibraryEntry()` now only fires for paths that pass no name (e.g. project load fallbacks).
    - **Mic live monitoring + Pause.** While the mic dialog is open the input is routed through the output (`AudioEngine::inputMonitoring`) so you *hear* what you're about to capture, and the display sweeps a ~10 s ring buffer at 20 Hz. A single prominent **Pause** button mutes monitoring **and** holds the display still so you can drag the region handles on a steady waveform; it toggles to **Go live** to resume both (amber while live, blue while paused). The dialog snapshots the engine's prior `inputMonitoring` state on construction and restores it in its destructor, so opening/closing it never clobbers the main-window **Mon** toggle. The source-info line shows **LIVE — use headphones to avoid feedback** while monitoring and **PAUSED** once held. The playback-tap source keeps the older plain **"Pause view"** checkbox (no monitoring — the project output is already audible via the main transport). **Why "Pause" and not "Freeze":** the word *freeze* is reserved for the granular **freeze methods** (CrossfadeLoop / AsyncGranular / PitchSyncGrains / SpectralFreeze) that sustain a captured waveform — the freeze-method picker (a **Freeze:** dropdown) now lives in **all three** capture dialogs (song, mic, file), so a "Freeze" button sitting next to a "Freeze:" method picker would be ambiguous. The pause control therefore reads **Pause** everywhere.
      - **Input-device enablement.** On Windows the default input and output are frequently different physical devices, and JUCE's default-device init can land on an output-only setup (input channels zeroed / input device name empty) — which silently disables the mic, so capture/IR/monitoring get no signal even though the device "opened" (a freshly-plugged mic that's the default *recording* device but not the same device as the default *playback* device hits this exactly). `AudioEngine::ensureAudioInputEnabled()` detects that, picks the device type's default input device and enables its first channel, and is called both at engine `init()` **and** every time the mic capture dialog opens (so a mic plugged in mid-session is picked up). It restarts the device only when input is actually off — the common "input already live" path is a no-op with no audio glitch. Mirrors the older `inputChannels.isZero()` guard that already existed in `room_ir_capture.cpp`, hoisted to apply app-wide.
      - **"Audio device…" button + DirectSound troubleshooting.** The mic dialog has an **"Audio device…"** button (bottom-left of the button row) that opens the standard Audio Device Settings dialog, plus a note in the hint text: if the input sounds wrong (garbled, noisy, or like the computer's own audio), switch the driver type to **DirectSound** there. This is the guided fallback for the WASAPI combined-device input-corruption bug — see [Audio device selection & persistence](#audio-device-selection--persistence) for the full explanation and why DirectSound is the first-run default.
    - **Region audition (Mic + File).** A **Preview** button loops the selected slice through the engine's GrainLoop preview so you can *hear* the exact region under the handles before committing it to the library — the same mechanism the song dialog uses for its Pause/Scrub audition. It auditions a **representative captured frame** using the **exact geometry `buildFrames()` bakes** — the same **banded** source, grain, per-method window, and (for the cloud modes) banded preview atomics — so the preview is honest: what you hear is byte-for-byte what the synth will play:
      - **Banded frames, not cropped grains.** Each captured frame is now a **banded `GranularFrame`**: its source PCM is one freeze **window** (`effectiveSrcLen()`) plus a half-window lookahead tail, with `windowStart = 0` / `windowLen = window`, and a **separate** `grainLength` taken from the Grain-length slider. So the voice runs its clean per-mode banded math — Crossfade loops the whole window, the **grain-cloud modes roam their grain inside the window** (a window wider than the grain = real moving cloud, which is exactly why the window auto-defaults to 4× grain), and Spectral analyses inside it. With a **single waveform** the window is the whole selection, so previewing plays back the **entire recorded sound** on repeat — record yourself saying "one", fit the selection around it, and Preview plays "one… one… one". *(This replaces the old `loopLenForWindow` model, which cropped every frame's source to a ~100 ms grain and left frames non-banded, so the cloud modes had only ~1 grain of roam room — collapsing Async/PitchSync to a near-static single grain regardless of window. The crop function is gone.)*
      - **"Preview waveform" picker chooses which frame to audition.** A **Preview waveform** slider (its own row above the button row, 1-based) selects which of the N captured waveforms Preview plays — `1` is the first (earliest in the selection), `N` the last. `regenerateAuditionGrain` reads `value − 1` as the band index and auditions exactly that frame via `bandStartForIndex`, so what you hear is precisely the frame you'll get at that Position. Changing it **while a preview is running re-publishes the loop live** (no stop/start). The slider re-ranges and clamps its pick whenever the waveform count changes, and is **disabled at a single waveform** (nothing to choose — Preview plays the whole selection), with a tooltip saying so. Present for both Mic and File.
      - **Loop / banded mechanics.** The source buffer is the freeze window **plus a window/2 lookahead tail** drawn from the audio past the band end (`buildGrainSource(startIdx, windowLen)`), which the `CrossfadeLoop` seam crossfade (~50 ms, clamped to window/2) reads so the wrap doesn't click. For the cloud modes the same tail gives grains roaming near the band end somewhere to read. The preview publishes the band via the engine's **banded preview atomics** — `setPreviewWindowStart(0)` / `setPreviewWindowLen(window)` alongside `setPreviewGrainLength(min(grain, window))` — so the engine's single `GrainFreezeVoice` runs the **same banded path** as the captured frame. **The published grain is clamped to the window, not just the source** (`min(grain, windowLen)`): a grain must fit inside the freeze window, because the voice floors the band width *up* to the grain length (`bandLen ≥ grain` for the cloud modes), so a grain wider than the window would swallow the whole `1.5×window` source and trip the voice's `srcLen < aGrain + 4` "band too short to roam" viability gate — zeroing the grain and producing **silence**. This bit when the selection (and thus the window) was smaller than the requested grain — e.g. a 63 ms selection with a 100 ms grain in Async granular went silent. Capping the published grain to the window keeps the cloud modes sounding (the grain just collapses to the window with no roam room). `buildFrames` applies the **same** `min(effectiveGrainLen(), windowLen)` clamp when baking each `GranularFrame`, so a captured frame is never silent for the same reason and the audition matches the capture. The non-cloud modes (Crossfade/Spectral) ignore the grain entirely and use the band. Playback ratio is `440 / capturedPitchHz` so it sounds at the editor's A4 reference. The bake (`buildFrames`) writes the same window, source layout, grain, and crossfade into the saved `GranularFrame`, and `terrain_synth`'s `renderGrainSample` plays it back with identical geometry — so audition and synth playback stay 1:1.
      - Dragging the region handles plays it automatically and respins the source as the handles move (**scrub-to-audition**); changing the embedded-pitch picker re-pitches a running preview live. Preview toggles to **Stop**, and the engine preview is torn down (`clearPreview`) when the dialog closes. **Mic** can only audition once the display is **Paused** (a sweeping ring buffer can't loop stably) — until then the Preview button is disabled with a tooltip telling you to press Pause first; **File** can audition as soon as a file is loaded. Going **Go live** on the mic stops any running audition.
    - **Zoom / scroll the waveform view (File source + the song dialog).** <a name="capture-zoom-scroll"></a>In a long file or song the whole buffer is mapped across the wave rectangle, so a useful selection is a one-pixel sliver. Two horizontal sliders **below the waveform** fix that: **Zoom** shrinks the visible window around the current scroll position (1× = whole buffer; the max lets the view shrink to ≈ `kMinViewSamples` = 256 samples, on a log-skewed travel) and **Scroll** slides that window left/right along the buffer. The view is a `[viewStart, viewLen]` window driven by the two slider values (`viewZoom` / `viewScroll`); `xForIdx`/`idxForX` (file) and `xForSamplePos`/`samplePosForX` (song) and the waveform render loop all map that window to the wave rectangle, so at 1× they collapse to the old whole-buffer map — **the live Mic / Playback sources keep zoom 1 / scroll 0 and hide both sliders**, so only the File and Song views show them. Zooming **stays at the current scroll value** (the scroll fraction is preserved; only `viewLen` changes), and **Scroll is disabled until you zoom in** (nothing to slide at 1×). The sliders are re-ranged for the buffer on load/render (`configureViewSlidersForBuffer`) and disabled until a file is loaded / the render completes.
      - **Selection follows the audio, pegs at the view edges, scales with zoom (File).** The selection is a fixed audio sample range, so panning scrolls it visually with the audio. `clampSelectionToView()` keeps it usable at every zoom: a selection that would fall off a view edge is **translated to sit flush against that edge** (it "stays still, pegged" while you keep scrolling past it), and one longer than the visible window is **shortened to the window** ("can't select more than you can see") — so zooming in **scales the selection down with the view** but never larger than it. The section bands and orange region shading draw through the same view mapping, so they zoom/scroll with the waveform automatically.
      - **Selection pegs into the view (Song).** The song dialog now uses the same two-handle region model as File, so it shares the same view-pegging logic: `clampSelectionToView()` keeps the region inside the visible window after a zoom/scroll (translating a region that would fall off an edge to sit flush against it, and shortening one longer than the visible window to fit), then re-anchors the Pause/Scrub band audition on the pegged region. The region is left alone while **Playing** — the playhead genuinely sweeps the song, so the view isn't disturbed during playback.

- **Inharmonic frame** <a name="inharmonic-frame"></a>— an additive stack of **partials** with **arbitrary (non-integer) frequency ratios**, played as a **live multi-oscillator voice** (one sine per partial, per voice) rather than a baked single cycle. This is what makes bells, mallets, gongs, and metallic / detuned / stretched tones, whose overtones do **not** fall on the integer harmonic series. Edited in `InharmonicFrameEditorComponent` (`InharmonicFrame` in `inharmonic_frame.h/.cpp`). Distinct from the synth-wide **Additive bank** render mode (which FFTs one authored cycle and is therefore harmonic-by-construction): the inharmonic frame authors the partials *directly* and never constrains their ratios to integers.
  - **Partials list.** Each partial has a **ratio** (its frequency as a multiple of the played note — `1.0` = the fundamental, `2.76` = a typical bell's first overtone; range **0.01–32**, the slider skewed around `4.0` so the musically dense low ratios get most of the travel), an **amplitude** (`0..1`), and a **phase** (`0..1` of a cycle). Up to **`kInharmonicMaxPartials` = 64** partials. **+ Partial** adds one (disabled at the cap), the **X** on a row removes it, and **Bell** resets the stack to the built-in `defaultBell()` (5 partials at the classic bell ratios). Each control has a units tooltip per the non-musician UX rule (ratio → "× the played note", etc.).
  - **Live voice (what you hear).** When a note plays, the synth runs **one sine oscillator per partial** at `noteHz × ratio_k`, sums them, and mixes the result into the wavetable Position morph exactly like the [granular frame](#granular-freeze-window)'s live grain stream — the inharmonic frame bakes a **zero cycle** into the terrain and registers an `InharmonicLayerEntry` side-table entry that a per-voice oscillator bank (`Voice::InhStream`) reads live, weighted by the same Position blend the cycle layer would get (scatter Shepard/Wendland RBF or grid N-linear hat). Each partial whose frequency would exceed Nyquist is dropped, so the stack never aliases when pitched up. Phases re-seed from each partial's authored initial phase on note-on, so reused voices don't click.
  - **What-you-see-equals-what-you-hear loudness.** The editor thumbnail (`renderRaw`) peak-normalises a representative cycle to 1.0, but the live additive voice has no per-sample normaliser, so it would otherwise be louder/quieter than the thumbnail suggests. `InharmonicFrame::normGainFor()` computes `1 / peak-of-the-summed-partials-over-one-period` once at build time, stored on the side-table entry and multiplied into the live voice — so the played level matches the drawn cycle. `renderRaw` uses the **same** helper for its own normalisation, so the two can't drift.
  - **Amplitude-domain warp only.** The frame carries its own [warp chain](#waveform-warp-shape-bending), but — like the granular grain stream — a continuous live oscillator bank has **no periodic phase axis to remap**, so its warp editor is restricted to **amplitude-domain** methods (clip / fold / saturate the summed output). The picker hides the phase-domain methods and shows an explanatory empty-hint.
  - **Save/load & undo.** Partials and the warp chain serialise into the frame body (a `;warp:` section appended only when the chain is non-empty, so old decoders never see it); a round-trip preserves every ratio/amp/phase and both warp ops. Edits commit on the editor's debounced *Edit wavetable* undo step like every other wavetable edit.

- **Factory waveform library** <a name="factory-waveform-library"></a>— a built-in collection of **thousands of ready-made single-cycle oscillator shapes**, browsable from the **+ Waveform → Factory waveform...** menu item. The shapes are sourced from the public-domain [Adventure Kid Waveforms (AKWF)](https://github.com/KristofferKarlAxelEkstrand/AKWF-FREE) set (CC0) — basic geometric waves, tuned-instrument cycles (piano, electric piano, organ, guitars, strings, brass, woodwinds, theremin, voice), and character/algorithmic banks (FM, overtone, distorted, bit-reduced, chip, video-game) plus large assorted banks. Importing one is **not** a separate frame type: the chosen cycle is dropped into a **Drawn/Freehand `WaveLayer`** of a one-layer `LayeredWaveform` (`makeFactoryFrame()` in `layered_wave_editor.cpp`), so the result is a **fully editable, serialisable layered frame** — you can draw over it, stack more layers on it, apply a per-layer warp chain, everything a hand-built layered waveform supports. The new entry lands in the **Library list** (named after its source waveform plus the standard stable-id suffix) exactly like an edit-from-scratch entry; it is **not** auto-placed into a cell (use **Assign to selected cell**).
  - **The browser.** A modal **Factory Waveforms** dialog (`FactoryWaveformBrowser` in `layered_wave_editor.cpp`, opened via `launchToolDialog` so it shares the main window's taskbar entry): a **category list** down the left (each row shows its waveform count, with an **All categories** row at the top), a **search box** across the top (filters by waveform name *or* category), a **Curated only** toggle, the filtered **waveform list** in the middle, and a **live cycle preview** along the bottom. Pick a waveform (single-click to preview, double-click or **Insert** to add) — Insert is disabled until something is selected. The dialog centres on the wavetable editor and closes on Insert / Cancel / Esc.
  - **Curated "best of" subset (★).** A hand-picked subset of the library (the clean fundamental shapes plus a representative or two from each instrument and a sparse sampling of the character banks) is flagged **curated**. Curated waveforms show a **gold ★** in the list and are **sorted to the top of their category** (then alphabetical), mirroring the ★-and-sort-to-top treatment the warp method picker gives its recommended algorithms. The **Curated only** toggle hides everything else (and drops any category that has no curated entries), so a user who just wants a tight, vetted set never has to wade through all ~4000. (Curation is rule-based in the packer, not per-waveform auditioned — see below.)
  - **On disk: one packed asset, not 4000 files.** The whole library ships as a single binary, `cpp/resources/waveforms.bin` (~4.4 MB), copied next to the executable by CMake (same mechanism as `docs/`) and loaded at runtime by the process-wide `WaveformBank` singleton (`waveform_bank.h/.cpp`), lazily on first browser open. We deliberately do **not** commit thousands of loose `.wav` files or embed them in the binary: a single ~4 MB read beats opening 4000 files and keeps the build fast. Each cycle is stored DC-removed, resampled to **512 samples** (the Freehand layer's native size, so import is a straight copy), peak-normalised, as `int16` (so the asset stays ~4 MB rather than ~8 MB float). The file is little-endian with a `SSWB` magic + version; the loader validates and fails cleanly (the browser shows an explanatory message) if the asset is missing or malformed. The asset is **regenerated by `cpp/tools/pack_waveforms.py`** from a local AKWF checkout (folder name → display category via a name map; curation via the rules above); a fresh repo checkout without the asset simply shows an empty browser, and the self-test treats a missing asset as a soft skip.
  - **User single-cycle import.** `makeFactoryFrame()` is the shared import path for both a bank entry and an arbitrary user-loaded single-cycle `.wav` (any length is linearly resampled to 512), so the two stay in sync.
  - **Stored by reference, not by copy (`factoryRef`).** When you pick a *factory* waveform, the layer keeps a live **reference** to it by the bank entry's **stable name** (`WaveLayer::factoryRef`, set from `WaveformBank::entry(i).name`) — the samples are loaded into the layer for rendering, but the project saves only the **name** (a compact `factory=<name>` field on the layer), *not* the 512 samples. On load the cycle is re-resolved from the bank (`WaveLayer::resolveFactoryRef()` → `WaveformBank::indexForName`). A session that uses stock waves is therefore much smaller, and the bank itself is never embedded in a project. **Keying by name (not by index)** means you can freely reorder/insert/curate the bank in a future build without breaking saved references; only renaming or removing an entry would. A user-loaded `.wav` carries no name, so it always embeds its samples.
    - **Fork on first content edit.** The reference survives level/tuning edits (amp, phase, ratio, per-layer warp), but the moment you change the **cycle content** — drawing over it, switching Points/Freehand, picking a different shape, or applying a preset — the layer **forks**: `factoryRef` is cleared and the now-independent cycle is embedded in full. This is belt-and-suspenders: the editor clears `factoryRef` on those edits, **and** the serializer is **content-addressed** — `encodeLayer` only writes `factory=<name>` while the layer's `drawnSamples` still match the bank entry byte-for-byte (otherwise it embeds the samples), so even a missed UI trigger can never silently save the wrong cycle. If the name can't be resolved on load (e.g. `waveforms.bin` missing, or — more likely — the project was saved by a *newer* SEANCE whose bank added that cycle), the layer renders silent but keeps the reference so a re-save still points at it. Built-ins remain immutable and uneditable in place — customizing one is always *pick → fork-on-edit*, never an edit of the shared factory shape.
    - **Warning on unresolved references.** Silently zeroing a waveform the current build doesn't have would make a loaded song untrue to the original with no indication, so opening a project surfaces a warning. A `FactoryRefResolutionScope` (`layered_wave_editor.h`) on the stack around `ProjectFile::load` in `MainContentComponent::openProjectFile` collects every factory name that fails to resolve (deduplicated, in encounter order); when the load finishes with a non-empty set, a single warning dialog ("Missing built-in waveforms", parented to the main window per the taskbar rule) lists the names (capped at 12, "+N more" beyond that) and explains that the project was likely saved with a newer SEANCE, the affected layers were silenced, and updating SEANCE should restore them. The scope is installed only on the user-facing project open — undo restore, library previews, and other internal decodes resolve silently as before.

A `SampleFrame` type exists for captured single-cycle samples but has no in-editor view yet — see `known-issues.md`.

---

## Waveform warp (shape-bending)

<a name="waveform-warp-shape-bending"></a>**Warp** is real-time, modulatable *shape-bending* of a waveform — folding, clipping, bending, saturating, phase-distorting, and (for the generator morphs) pulse-width / sync / FM / phase-distortion shaping. The framework lives in `warp.h/.cpp` (pure std, unit-tested from `--self-test`); the reusable editor is `WarpChainEditor` (`warp_editor.h/.cpp`). The design goal is that the *amount* of each warp is a node parameter, so an LFO / oscillator / envelope wired in via the [on-demand modulation pins (#88)](#control-inputs-on-parameters-set-vs-mod) can **morph the waveform live** as a note sustains.

### One mechanism, two user-facing types

In code there is a single shaping mechanism: a *shaping parameter* (a chain op's amount, or a generator's shape param) is a node `Param` that can **opt into** a modulation pin. **"Morph" is the user-facing word** for an animated/shaping operation; **"warp" is the umbrella/mechanism term**. There is no separate morph data type — a warp *is* a morph the moment its amount param has a pin and is being driven. But the UI keeps **two functionally distinct types** visible, because they are different things and must not be conflated:

| | **Type 1 — wave-DEFINING morph** | **Type 2 — arbitrary-wave morph** |
|---|---|---|
| What it does | generates the cycle from scratch; the morph param **is** the wave | reshapes whatever wave already exists |
| Code bucket | **B** (generator, inside the oscillator) | **A** (transfer on the output) |
| Cardinality | exactly **one** per layer | an **ordered "+ Add" chain** |
| Relationship to shape | **mutually exclusive** with a static shape/preset (it *is* the shape source) | needs an existing shape to act on |
| Where offered | per layer (one option in the [wave-source picker](#per-layer-wave-source-picker)) | frame-scope summation only (the per-layer Type-2 chain was removed) |
| UI control | single-select pulldown | ordered list built with **+ Add** |
| Examples | PWM, Hard Sync, FM, Phase Distortion | Soft Clip, Wavefold, Bend, Saturate |

A **Type 1** generator is *not* a `WarpMethod` enum value and never appears in a chain editor — it is the layer's **shape + `shapeParam`** (Pulse/Sync/FM/PD), selected from the per-layer wave-source picker. The chain editors (`WarpChainEditor`) only ever offer **Type 2** (Bucket A) methods, so a wave-defining generator can never leak into a transfer/summation stage.

**Pin explosion is solved by opt-in.** Every shaping param defaults to **no pin** (baked, zero live cost). A per-op **"Pin" checkbox** adds/removes the `ModPin` (the #88 mechanism). Checking it makes that one param live-modulatable; unchecking removes the pin (and, for per-layer ops, the on-demand param itself). See [Modulating an op](#modulating-an-op-the-pin-checkbox) below.

### The three buckets

Warps split into three buckets by *where* they apply:

- **Bucket A — transform warps.** A function of an arbitrary cycle, applied to whatever waveform a frame produces. Two sub-domains matter per sample:
  - **Amplitude-domain** — a nonlinear transfer on the sample value *after* the table lookup: **Soft Clip, Hard Clip, Wavefold, Wavewrap, Rectify, Quantize** (bitcrush), **Tube** (asymmetric / even-harmonic saturation), **Tape** (symmetric / odd-harmonic), **Flip, Chebyshev**.
  - **Phase-domain** — remaps the read position *before* the lookup: **Bend +/−, Asym +/−, PWM-skew, Phase quantize, Phase distortion** (Casio-CZ style), **Vector phase shaping, Remap** (parameterised S-curve), **Self-sync** (read-restart formant sync).
- **Bucket B — generator morphs.** The morph parameter is part of the wave's *definition*, not a post-process: PWM pulse width, hard-sync ratio, FM index, CZ phase-distortion. Implemented as **layer-shape primitives** inside the layered-waveform layer stack, not as chain ops.
- **Bucket C — representation-bound element warps.** Applied *inside* a frame's render, per element of its representation: per-FFT-bin (spectral), per-wavelet-coefficient (wavelet), per-grain (granular), per-partial (inharmonic). These are **baked** into the rendered cycle / stream, not modulated live.

### Where a warp chain can live

The same `WarpChainEditor` widget drives every chain; it does **not** own the chain (the host points it at a `std::vector<WarpOp>` via `setChain`). Each host gives the editor a context-appropriate header via `setHeaderText` — so the frame-scope editor reads **"Summation Morph"** (the baked element editors keep the generic *"Warp"* header). The **per-layer Type-2 "Layer Morph" chain was removed** — a layer reshapes its own cycle only through its wave-defining (Type-1) generator; arbitrary-wave (Type-2) reshaping happens only at the frame-scope summation. Host scopes:

| Scope (header) | Storage | Modulatable? | Domains offered |
|---|---|---|---|
| **Frame-scope** — *"Summation Morph"* (Bucket A) | `IWavetableFrame::morphChain` (**per frame**) | **Yes** — each op opts into a positional `Warp N` node param keyed to that frame | Phase + Amplitude |
| **Spectral / Wavelet element** (Bucket C) | per-doc element chain | No (baked) | restricted |
| **Granular / Inharmonic element** (Bucket C) | per-frame chain | No (baked) | **Amplitude only** (a live stream has no periodic phase axis) |

A host that only supports some domains calls `setAllowedDomains(...)`, which filters both the picker and the "+ Add" method list and can supply an `emptyHint` explaining the restriction (per the "grayed-out controls must explain themselves" rule).

**The Summation Morph chain is per-frame.** Each frame (each library entry's `IWavetableFrame`) carries its **own** `morphChain` + `morphAssetId` — the chain shapes *that frame's* cycle, **before** the cross-frame Position blend. Two frames in the same table hold genuinely independent chains: setting frame A to *Soft Clip* doesn't touch frame B. The editor binds to the **currently-edited frame's** chain; switching frames re-points the `WarpChainEditor` at the new frame's vector. (Earlier builds stored a single `WavetableDoc::warpChain` shared across all frames, which is why picking a morph for one frame appeared to mirror onto the others — that bug is the reason this is now per-frame.)

- **Per-frame param keying.** Each frame-scope op's node param carries the owning frame's library id in `Param::warpFrameId`, so frame A's *Warp 1* and frame B's *Warp 1* are distinct params with distinct pins. `syncWarpParamsForNode` walks every morphing frame in `doc.library` and reconciles `(warpFrameId, warpSlot)`-keyed params; the synth reads each frame's live amounts via `getParamByWarpSlot(node, frameId, slot, …)`.
- **Per-frame name prefix.** When **more than one** frame in a table carries a non-empty morph chain, each frame's morph params get a disambiguating name prefix (`frameWarpPrefix`, e.g. `"A: "`, `"B: "`) so the node sliders read *"A: Soft Clip Drive"* vs *"B: Wavefold Fold"*. With only one morphing frame the prefix is empty (no clutter).
- **Synth bake.** The static terrain/scatter bake renders each frame through `IWavetableFrame::renderMorphed` (render + `applyWarpChain(morphChain)` at resting amounts). When a frame's morph (or per-layer field) is modulated, the synth re-bakes **just that frame's** cycle per block via `rebakeFramesIfNeeded` / the `wtRebakeFrames` table (each entry records the frame's destination — a Scatter slot or a Grid stride/offset — and only re-renders when a modulated value changes vs its cache).

**Per-layer Phase / Amplitude modulation (the engine fork).** A layer's **Phase** and **Amplitude** can each opt into an on-demand modulation pin via a **Mod** checkbox next to its slider. With **no pinned field**, the layer is **baked** into the table at edit time — the common case, zero live cost. The moment a layer's Phase or Amp is pinned, the synth re-bakes that frame's cycle at block rate from the current modulated values (`LayeredWaveform::renderWithLiveOverrides`, driven by per-layer **layer-field** params the synth reads via `getParamByLayerField`). This now works on **multi-frame tables too** — the per-frame `wtRebakeFrames` infrastructure re-bakes each affected frame in place (Scatter slot or Grid stride/offset), so the per-layer **Mod box is enabled regardless of frame count**. (Earlier builds gated this behind a single-frame-only `perLayerWarpModSupported()` check and disabled the box on multi-frame tables; lifting that gate is part of the per-frame re-bake work.) The on-demand layer-field params exist **only while pinned** — checking "Mod" creates the `Param` (keyed by `warpFrameId` = owning frame id, `warpLayer` = layer index, `layerField` = 0 Phase / 1 Amplitude, `warpSlot` = −1) + pin; unchecking removes the pin *and* erases the param.

> The dormant per-layer warp re-bake path (`renderWithLiveWarp` → `getParamByWarpLayerSlot`, `(warpLayer, warpSlot)` params) is retained behind the disabled `enablePerLayerWarp` flag for a possible wholesale restore of the removed per-layer Type-2 chain; `renderWithLiveWarp` now just delegates to `renderWithLiveOverrides` with no phase/amp overrides.

### Editing a chain

The editor shows a context header (**"Summation Morph"** frame-scope, **"Warp"** for baked element chains) and a **+ Add** button, then one row per op. Clicking **+ Add** opens the same domain-grouped method picker the per-row Method button uses; the new op is appended only once you choose a method (cancelling adds nothing). This lets you pick *which* shape-bender to add each time — earlier builds instead appended an op pre-set to the first recommended method, so repeated Add clicks just stacked copies of that default. Each row:

- **Enable** checkbox — bypass this stage without deleting it (a bypassed op greys its amount slider).
- **Method** button — opens a `PopupMenu` grouped by domain, with a **★ star badge** on the recommended (higher-quality) methods. Restricted hosts only list their allowed domains.
- **Amount** slider — the `0..1` morph amount (`0` = identity). On a modulatable chain this slider mirrors into the op's node param, so a wired LFO picks up the new resting value.
- **Pin** checkbox — see [Modulating an op](#modulating-an-op-the-pin-checkbox) below.
- **▲ / ▼ reorder arrows** — move this stage earlier / later in the chain. **Order is part of the sound** — fold-then-clip is a different transfer curve than clip-then-fold — so the chain is processed strictly top-to-bottom and you can reorder freely. The **▲ is disabled on the first row and ▼ on the last** (each with a tooltip saying why, per the grayed-out-controls rule). Reordering is undoable on the same debounced *Edit wavetable* step as any other warp edit.
  - **Modulation follows the op, not the slot.** On the frame-scope chain each slot is exposed as a positional `Warp 1`..`Warp N` node param (so the synth reads slot *k*'s live amount from `Warp k+1`). When you move an op, a wired LFO/oscillator stays driving **that op**, not the slot it vacated: `WarpChainEditor::moveOp` fires an `onReorder(a, b)` callback that the host (`LayeredWaveEditorComponent::swapWarpParamNames`) uses to swap the two affected params' **names** — the param objects (and the modulation pins that reference them by index) stay put, so each op keeps its own (possibly modulated) param after the move.
- **X** — remove this stage.

Adding or removing an op fires `onStructureChanged`, which on the frame-scope host re-syncs the warp params (adding a param + on-demand pin for a new op, dropping the param/pin/cables for a removed one, and remapping the surviving pins' param indices). A reorder fires `onReorder` + `onChanged` but **not** `onStructureChanged` (the op count is unchanged).

### Named morph parameters

A pinned op's node param is named after the morph **method itself** — *Soft Clip*, *Tape Saturate*, *Wavefold*, *PWM Skew*, *Bend +*, … — i.e. the method's full display name from the `warp.h` registry (`warpMethodName`), **not** the abstract amount-label (*Drive* / *Fold* / *Width*) it used to show. So a pinned Tape-Saturate op reads as **"Tape Saturate"** (and, on a multi-frame table where >1 frame morphs, with the frame prefix, e.g. **"Layered 53: Tape Saturate"**) rather than an opaque "Warp 1" or an amount-label "Drive 1". The label-building helpers are `warpSlotParamName(chain, slot)` (frame-scope) and `perLayerWarpParamName(chain, layer, slot)` (per-layer, prefixed `L<n>`), both in `layered_wave_editor.cpp`. **Disambiguation is occurrence-based:** a number is appended only when the *same method* appears more than once in the chain — a lone Wavefold reads "Wavefold", but two of them read "Wavefold 1" / "Wavefold 2". Internally the param is still keyed by a stable **`warpSlot`** (its position in the chain) decoupled from the label, so re-picking a method relabels the param without breaking a wired pin.

### Modulating an op (the "Pin" checkbox)

> The per-row checkbox is labelled **Pin** (it used to read *Mod*): ticking it adds a control-**input pin** whose mode can be either **Mod** or **Set**, so labelling the checkbox "Mod" was misleading. The anchor name keeps the old slug for stable links.

Every op defaults to **baked** (no pin, no live cost). Ticking a row's **Pin** box opts that op's amount into an [on-demand modulation pin (#88)](#control-inputs-on-parameters-set-vs-mod): a `Param`/`ModPin` is created so an LFO, oscillator, or envelope can drive the morph live as the note sustains; unticking removes it. The checkbox reflects the current pin state.

- **Frame-scope (Summation Morph):** the op maps to its positional `warpSlot` param (created up-front by `syncWarpParams`, labelled by method name per *Named morph parameters* above); checking adds the pin, unchecking removes it.
- **Per-layer Phase / Amplitude:** the same opt-in mechanism drives each layer's **Phase** and **Amplitude** slider (the per-layer arbitrary-wave chain itself was removed). The param exists **only while pinned**. Checking creates a layer-field param named e.g. *"Layer 2 Phase"* (keyed by `warpFrameId` = owning frame id, `warpLayer` = layer index, `layerField` = 0 Phase / 1 Amplitude) and adds the pin; unchecking removes the pin and **erases the param**, and the slider unlocks. This works on **single- and multi-frame** tables alike — the synth re-bakes the affected frame in place per voice via the per-frame `wtRebakeFrames` path (see the engine fork above). (The box used to be disabled on multi-frame tables; that gate was lifted with the per-frame re-bake work.)
- **Baked element chains** (spectral / wavelet / granular / inharmonic) hide the Pin box entirely — they have no node params to pin.

**Who owns a pinned op's amount, and which slider locks.** Pinning an op gives its amount a **node param** (the slider on the node face + any cable wired to its pin) in addition to the editor's in-frame slider. Pinning alone does **not** freeze either slider: a pinned-but-uncabled op, or one driven by a **Mod** cable, stays draggable in *both* the editor and on the node — dragging sets the **base** (resting) amount the modulation swings around, exactly like every other modulatable param. Only an active **Absolute ("Set")** cable, which fully owns the value, locks the sliders (they grey out with a tooltip explaining how to re-enable: disconnect the Set cable or switch the pin to Mod). The editor enforces this via `WarpChainEditor::Callbacks::isAmountLocked` (frame-scope and per-layer warp ops) and `WaveLayerEditor::Callbacks::isFieldAmountLocked` (per-layer Phase / Amplitude / generator sliders), both resolving to `Graph::paramHasAbsoluteInput` — the same predicate that locks the node slider, so the two surfaces always agree. (An earlier iteration froze the editor slider whenever pinned, which stranded a pinned-but-uncabled value and made a Mod base un-editable from the editor; that lock-on-pin rule was replaced by this lock-on-Set rule.) Two more pieces make the node param actually take effect:
> - The editor mirrors the op amount into the node param **unless an active *Set* (Absolute) cable owns it**. `pushWarpAmountsToParams` overwrites the param's value on every edit *except* when `Graph::paramHasAbsoluteInput` is true (an Absolute cable is driving it edge-to-edge) — there the cable owns the value, so the editor leaves it frozen as the resting fallback. A **pinned-but-not-Set** op (no cable, or a *Mod* cable) therefore stays fully editable from the editor: dragging its editor slider updates the node param, so the sound *and* the preview change. (Earlier the gate was `!hasParamModPin`, which froze the param for **any** pinned op — so a pinned morph slider in the editor moved but changed nothing; that was the reported bug.) The **reverse direction** — a node-side slider drag, a *Mod* cable, or an undo moving the param underneath the editor — is mirrored back into the editor's `chain.amount` and its on-screen amount slider by `pollNodeParamChanges` (20 Hz), so the two surfaces stay in sync and neither clobbers the other.
> - The editor's **preview and held "Preview" audition render at the live node-param amount**, not the frozen editor amount (`renderEditingFrameLiveCycle`). So dragging *either* the editor slider or the node-graph slider of a pinned morph op is both **visible** in the preview and **audible** in the sustained audition. Because the held audition overrides the synth terrain with a static cycle, a low-rate poll (`pollNodeParamChanges`, 20 Hz while the editor is open) re-ships that cycle whenever a node-param amount changes underneath the editor, so a node-side drag — or a modulation cable, or an undo — tracks live rather than appearing to do nothing.

**Continuous, click-free Preview while dragging.** The sustained "Preview" voice captures its single cycle at note-on, and the editor re-ships a freshly rendered cycle whenever a morph amount changes. The synth updates the **live audition voice's cycle in place** each block when a newer cycle is shipped (`TerrainSynthProcessor::processBlock`, the `wantHeld && heldAuditionActive` branch) rather than waiting for the debounced graph rebuild, so the audio tracks edits continuously. A hard pointer swap would **click**, though — the phase accumulator is mid-cycle, so reading a freshly reshaped table at that phase steps the output — so the swap is **crossfaded**: the voice keeps the previous cycle (`Voice::auditionCyclePrev`) and ramps `auditionCycleFade` 0→1 over ~6 ms, blending `prev`→`current` per output sample.

The crossfade alone isn't enough, because it only de-clicks *one* swap: if cycles are re-shipped faster than ~6 ms (a fast slider drag firing on every pixel) each new swap interrupts the previous fade and the residual steps stack back up into audible breaks. The fix is to **re-ship at a throttled rate**, which is exactly why a modulation **signal** never breaks: a cable changes the node param, and only the 20 Hz `pollNodeParamChanges` re-ships the cycle — so swaps land ≥50 ms apart and every 6 ms crossfade finishes first. So the **editor amount slider now takes the same path**: its drag is routed through a dedicated lighter callback (`WarpChainEditor::Callbacks::onAmountChanged` → `LayeredWaveEditorComponent::onFrameWarpAmountDragged`) that updates the live `Warp N` node param and the cheap on-screen preview *every* tick but does **not** re-ship the audition or rewrite the node script per tick — the throttled 20 Hz poll re-ships the audio (de-clicked), and the settled node-script commit + undo step happen once when the 150 ms debounce fires (`timerCallback` now calls `commitToNode()` before `onApply()`). The amount flows to the synth through the modulatable param, so no per-tick script rewrite (and its terrain re-bake) is needed at all — the editor drag is now byte-for-byte the same smooth path as a cable. Method / enable / structural changes still take the full `onChanged` → `onLayerChanged` path (those *are* baked into the script), but they're single clicks, not drags, so a one-off re-ship is fine.

### Built-in & saved morphs (the Library row)

The frame-scope Summation Morph editor shows a **Library row** (the per-layer and element editors don't): a status label (showing *Independent* or the live-linked morph's name) plus an **Unlink** / **Use Library…** / **Save to Library** button trio — the same Load/Save layout the frame and per-layer waveform rows use, with an added Unlink (it replaced the old inline combo so the picker can host a *Sync to library* choice). **Use Library…** opens a modal **morph picker** (`MorphLibraryBrowser`); **Save to Library** publishes the current stack; **Unlink** detaches a live link while keeping the chain (see below); "+ Add" instead builds a stack one stage at a time. The picker **sources every chain from the one project asset library** — the curated built-ins are *seeded* into that library, not listed from a separate code path — and partitions it under section headings. Every row in the picker **loads** a chain — there is deliberately **no** no-op "Independent" row (an earlier version had one; selecting it loaded nothing, which looked like a broken picker). To go back to an independent, unlinked chain you use the **Unlink** button on the Library row, not a picker entry. The sections are:

- **Built-in** — the curated Type-2 chains, **seeded into the project's [asset library](#asset-library-project-stores) as Morph Algorithm entries** (`seedBuiltinMorphLibrary` in `warp.h`, from `builtinMorphChains()`): *Warm Saturation, West Coast Fold, Lo-Fi Crush, Tape Glue, Pulse Width, Soft Bend, Formant Sync, Rectify Octave*. They appear in both this picker and the **Asset Library panel** (flagged ★ starred). They remain **templates, not live references** — picking one **copies** its ops into the chain and detaches to *(Independent)*, exactly as picking a factory waveform copies it into a layer (so an edit can never silently mutate a shared built-in). **Code-owned and not serialized:** their ids sit in a reserved range (`kBuiltinMorphIdBase = 200000`, below the user id base `1000000`, so `isBuiltinMorphAssetId` tells the two apart) and are **skipped by project-file save/export**; they are **re-seeded idempotently on every new project and every project load**, which keeps them improvable across app versions, keeps project files free of boilerplate, and makes them effectively undeletable (a deletion is undone by the next re-seed) — matching the asset library's *disjoint id space* + *divergence = duplicate* design.
- **Saved** — user-published [Morph Algorithm assets](#asset-library-project-stores) (ids ≥ `1000000`). Picking one applies its ops to the chain; tick **Sync to library** in the picker to **live-link** it (editing the chain then updates every frame that points at it) or leave it off to load a one-time independent copy. The Sync checkbox is enabled only for Saved entries (built-ins are immutable templates that always copy). **Save to Library** publishes the current chain as a new Morph Algorithm asset.

**Unlink** (the Library-row button) detaches the frame's live link to a Saved morph (`IWavetableFrame::morphAssetId = -1`) while keeping the current chain exactly as-is — an independent editable copy whose edits no longer propagate to/from the asset. It's **disabled** (greyed, with an explaining tooltip) while the frame's morph is already independent. Because the link is per-frame, unlinking one frame's morph leaves any other frame's live link intact. This is the morph member of the three **Unlink from Library** affordances (frame waveform / per-layer / morph), all of which set their respective `assetId` to `-1` and freeze the current content.

### Per-sample primitives vs the buffer helper

The live synth voice applies warps with the per-sample primitives `warpPhaseValue(method, phase, amount)` and `warpAmpValue(method, x, amount)` directly, so `amount` can be modulated every sample. The editor preview and the per-element bake instead call `applyWarpChain(ops, cycle)`, which composes the same primitives over a whole single-cycle buffer (phase-domain ops resample the cycle through `warpReadCycle`; amplitude-domain ops map each sample). Both paths share the same primitives, so preview, bake, and live playback agree.

### Scripting the warp transfers

The same warp catalogue is reachable from **scripts** (Script node, terrain/curve/wavetable bakes), so a script can shape a signal with the *exact* transfer the node-graph Warp effect uses instead of hand-rolling a saturator. Every binding routes through the `warp.h` primitives, keeping the catalogue a single source of truth. Two tiers, matching the bucket split:

- **Scalar warps — every language, real-time-safe (Bucket A).** `warpamp(method, x, amount)` (amplitude transfer on one sample) and `warpphase(method, phase, amount)` (phase transfer on one phase value) are exposed in the **Built-in** parser, **Lua**, **Python**, and **WASM**. The **method comes first** (matching the rest of the warp API; the whole-buffer calls below put their buffer first instead). `method` is the stable integer id **or** a name string (`"soft clip"`, `"bend+"`, …), resolved by `warpMethodFromName` (case/space/punctuation-tolerant). `amount` is `0..1`, **0 = exact identity**, unknown method = identity, so they're safe to sweep. Being pure `float→float` they're legal in *every* mode, per-sample real time included. WASM exposes them as host imports `ss_warpamp` / `ss_warpphase` plus `ss_warp_method(const char* name)` (resolve a name to its id once, like `ss_waveform_id`); the `SS_WARP_*` id constants live in `soundshop_wasm.h`.
- **Whole-buffer warps — bake/stream only (Bucket C).** `spectralwarp(buf, method, amount)` FFTs the buffer, warps the per-bin **magnitude** envelope (phase preserved, DC bin left alone), and inverse-FFTs back. `waveletwarp(buf, method, amount[, filter="db4", levels=5])` runs a multi-level DWT, warps every **coefficient**, and inverse-DWTs back (an exact perfect-reconstruction round trip — `amount` 0 returns the buffer unchanged). Both take a *whole buffer* (a Lua/Python list, or a `(ptr,len)` pair `ss_spectralwarp` / `ss_waveletwarp` in WASM), so they're **offline-bake or block/stream context only — never per-sample**. Both still funnel every value through `warpAmpValue`, so the catalogue stays unified. There is deliberately **no** `granularwarp` binding: a per-grain amplitude warp collapses to `warpamp` applied sample-by-sample, so a binding would just duplicate the scalar primitive over a loop (noted in `buffer_warp.h`). Implementations: `buffer_warp.cpp` (FFT `fft_util.h`, wavelet filters `getWaveletFilter()` from `wavelet.h`). The wavelet synthesis here is a **local perfect-reconstruction transform** — wavelet.h's own `idwt` is frozen for painter/saved-project compatibility and is *not* perfect-reconstruction for multi-tap filters, so a forward→warp→inverse round trip must not use it.

See [SCRIPTING-LANGUAGES.md](SCRIPTING-LANGUAGES.md#waveshaping-warps--the-same-transfer-functions-every-language) for the cross-language rationale.

### Save / load

A warp chain serialises with `encodeWarpChain` / `decodeWarpChain`. Grammar: `<count>:<op>:<op>…` where an op is `<method>;<amount>;<aux>;<enabled>`. `:` separates ops and `;` separates fields within an op, so it never collides with the `,`/`|` field separators the host docs use. The leading count is advisory (decode trusts the actual op tokens). An empty chain encodes as `0`, and callers **omit the section entirely** when the chain is empty — so a doc saved before warp existed (or with no warp) has no warp token and old decoders never trip on it. `WarpMethod` ids are **stable** (serialized in project files) — never renumber existing values; the enum has an intentional hole at `9` (a planned-but-never-shipped "Mirror" method) to keep the surviving ids fixed.

**Per-frame morph blocks (`__wavetable5__`).** Because the chain is per-frame, a wavetable doc serialises the morph data as two trailing, length-tagged blocks rather than a single doc-level chain:

- **`:morph:<count>:<libId>:<len>:<chainStr>…`** — one `(libId, len, chainStr)` triple per frame that has a non-empty chain. `libId` is the frame's library entry id; `chainStr` is the `encodeWarpChain` output (which itself contains `:`), so it's **length-prefixed** (`<len>` = its character count) and the decoder slices exactly that many characters rather than splitting on `:`.
- **`:morphAsset:<libId>=<assetId>:…`** — the per-frame morph live-links (`morphAssetId`), one `libId=assetId` pair per linked frame.

**Backward-compatible migration.** A project saved under the old single-chain model carries doc-level `:warp:` / `:warpAsset:` blocks. On decode these are **migrated into every frame** of the table (each frame adopts a copy of the legacy chain and the legacy asset link), so old projects open with the same audible result and then re-save in the per-frame format. Decoders that predate the per-frame blocks ignore the unknown `:morph:` / `:morphAsset:` tokens.

### Undo

Frame-scope and per-layer warp edits (add / remove / reorder / amount / enable / method) all route through the editor's debounced commit, landing as a single *Edit wavetable* snapshot step. Bucket-C element warps commit through their own editor's apply path. Continuous amount-slider drags follow the usual "commit on release / debounce" rule rather than one step per tick.

---

## Waveshaper (amplitude morph) effects

A family of ten **amplitude-domain waveshaping effect nodes** — one per Bucket-A amplitude-domain `WarpMethod` — that let you apply the wavetable synth's per-frame shaping curves as a standalone effect anywhere on the wire. They exist because the same shaping a user can dial into a wavetable frame's [morph chain](#waveform-warp-shape-bending) is also useful as a plain insert effect on an arbitrary audio signal, and building one node per curve (rather than one node with a method dropdown) keeps each node self-describing in the graph — the node's title says exactly what it does, and its single knob is labelled for that curve.

### The ten methods

Added from the node menu under **Waveshaper (amplitude morph)**, which lists (in this order): **Soft Clip**, **Hard Clip**, **Wavefold**, **Wavewrap**, **Rectify**, **Quantize (bitcrush)**, **Tube Saturate**, **Tape Saturate**, **Flip (invert)**, **Chebyshev**. These are exactly the `WarpMethod` values returned by `waveshaperMethods()` (`warp.h`) — the amplitude-domain sub-bucket of [Bucket A](#the-three-buckets), and the same primitives the synth uses for live per-frame amplitude morphing.

### One processor, keyed by script token

Identity rides the node's `script` string, **not** a per-method C++ class. `waveshaperScriptFor(method)` produces `"__waveshaper:<token>__"` (e.g. `__waveshaper:wavefold__`); `isWaveshaperScript()` / `waveshaperMethodFromScript()` recover the method. A single `WaveshaperProcessor` (`builtin_effects.h`) reads the method from its node's script in the constructor and applies `warpAmpValue(method, x, amount)` sample-by-sample across every channel — so the effect node and a wavetable frame's morph chain produce **byte-identical** shaping for the same method and amount. `GraphProcessor::rebuildGraph` instantiates it via `isWaveshaperScript(node.script)` before the rest of the built-in-effect if-else chain.

### The amount knob

Each node carries **one** parameter whose label is the method's own warp-parameter name from `warpParamLabel(method)` — **Drive** (Soft/Hard Clip, Tube, Tape), **Fold** (Wavefold), **Wrap** (Wavewrap), **Amount** (Rectify, Flip, Chebyshev), or **Crush** (Quantize) — ranged `0..1`, default `0.5`. The processor looks the value up by that same label, so the creation handler and the DSP can never disagree about the param name. At `amount == 0` (or method `None`) the node is a pass-through. Like every built-in effect it calls `applySignalModulations(node, buf)` first, so the knob can be Param/Signal-cable driven via the standard [Set/Mod control-input](#control-inputs-on-parameters-set-vs-mod) mechanism.

### Save / load, undo

No special serialization — the node persists through the normal `node.script` + `node.params` path in `project_file.cpp`, and `waveshaperMethodFromScript` re-derives the method on load. Adding/removing the node is a graph-topology change, so it commits via `commitSnapshot()` like any other node; the amount knob follows the standard slider-drag undo rule (commit on release). A self-test (`--self-test`, "fs/ws save-load") round-trips a Wavefold waveshaper through `serializeForUndo` → `loadFromString` and checks the script identity and param value survive.

---

## Standalone single-cycle oscillators (frame synths)

Each of these ways to author a single wavetable frame is also available as a **standalone instrument node** that plays that one cycle as a fixed-timbre oscillator: **Layered**, **Spectral** (frequency-domain), **Wavelet Space**, **Inharmonic**, and **Granular**. They are the wavetable's per-frame authoring methods promoted to first-class one-shot synths — for when you want a single fixed timbre authored your favourite way, without the multi-frame morph machinery.

> **Removed: the standalone "Sample (single cycle)" oscillator (menu id 254).** It only ever played a default sine — the live single-cycle capture/import path was never wired up. The need it was meant to fill (turn a recording into one repeating cycle) is now served properly by the **Granular** node's **[Single cycle](#granular-freeze-window) freeze mode**, which autocorrelation-detects one pitch period and loops it with a crossfaded seam. The `SampleFrame` *type* itself stays (project/back-compat decode, capture-from-playback frames); only the standalone instrument node is gone. Id 254 is intentionally left as a gap in the Instruments menu so the other ids (Granular = 255) stay stable.

### Not a one-frame wavetable in disguise — but it reuses the engine

The design constraint was *"don't just make 1-frame wavetables in disguise"* while *"sharing code between the frames and the independent instruments as much as reasonable"*. The resolution: a frame-synth node's `script` is `kFrameSynthPrefix` (`__framesynth__:`) followed by a complete single-frame WavetableDoc encode (one library entry, a 1×1 grid). The synth strips the prefix via `effectiveSynthScript()` and runs the **entire existing wavetable render path** on the lone frame, so there's zero duplicated DSP — but the node presents as its own instrument type with a focused editor, no Position axis, and no "number of waveforms" control. Helpers live in `warp.h`-adjacent code: `defaultFrameSynthScriptForType(typeId)` builds the default script for each frame type (it still recognises `"sample"` for back-compat decode even though no menu item creates one), `isFrameSynthScript()` / `decodeFrameSynthScript()` recover the frame type, and `effectiveSynthScript()` yields the wavetable-engine view.

### Focused editor

Opening a frame-synth node opens `LayeredWaveEditorComponent` in **`frameSynthMode`** — it detects the prefix and hides everything that only makes sense for a multi-frame wavetable (the Library/Cells panels, the Position/morph arrangement, the "number of waveforms" control). What remains is the focused authoring surface for that one cycle plus a toolbar with **Gain**, **Preview** (holds a sustained A4 through the node's own voice; the on-screen cycle *is* the audition, re-shipped on every edit — see [Auditioning the frame](#auditioning-the-frame-preview-button)), **Envelope…** (the shared [AHDSR envelope](#shared-ahdsr-envelope)), and the **Morph** (warp-chain) panel.

### Envelope on all of them

All five route through the wavetable/`TerrainSynth` voice path, so they get the **shared node AHDSR envelope** exactly like the wavetable synth: right-click → *Envelope (AHDSR)…* (gated by `isTonalSynth`) or the editor toolbar's **Envelope…** button. This is what satisfied the request to put the envelope feature on every one of them.

### Single-cycle from a recording — use Granular's Single cycle freeze mode

The standalone **Sample** oscillator (which never had a working capture path) was removed; turning a recording into one repeating cycle is now the **Granular** node's **[Single cycle](#granular-freeze-window) freeze mode**. It does properly what the Sample oscillator only promised: autocorrelation pitch-detect → one period → loop with a crossfaded (against the adjacent same-phase period) seam — the robust extraction pipeline, ear-validated by reusing the same `detectPeriodSamples` the Pitch-sync mode already relies on. Capture into a Granular node (capture-from-song / mic / file), then set Freeze mode → Single cycle.

### Save / load, undo

The node persists through the standard `node.script` path (the script *is* the full frame encode), so round-tripping a project preserves the instrument and its authored cycle. Adding the node is a graph-topology change (`commitSnapshot()`); edits inside the focused editor land on the editor's debounced *Edit wavetable* path, identical to editing a frame inside a real wavetable. A self-test (`--self-test`, "fs/ws save-load") round-trips a Granular frame-synth through `serializeForUndo` → `loadFromString` and checks the frame-type identity and Volume param survive.

---

## Frequency-domain (spectral) synth

Authors a single-cycle waveform directly in the frequency domain: a **magnitude** curve and a **phase** curve over the FFT half-spectrum, IFFTed to one cycle. A `SpectralDoc` holds an `fftSize` plus two `SpectralCurve`s (`mag`, `phase`); both are evaluated at `halfBins = fftSize/2 + 1` and combined into a complex spectrum (DC and Nyquist forced real), then inverse-FFTed and peak-normalised. The phase canvas sits above the magnitude canvas; a live time-domain preview updates as you edit.

**Editor toolbar parity (standalone node editors).** When the Frequency Domain node's editor is opened standalone (double-click the node), its top toolbar carries the same generic controls as the wavetable editor: an **Envelope…** button (edits the node's AHDSR amplitude envelope) and a green **Preview** button. Preview holds a sustained A4 through the node's own voice — the on-screen waveform *is* the audition cycle (shipped via `setNodeHeldAuditionCycle`), and it re-ships on every edit so you hear changes live; click again to **Stop**. As with all held auditions the node must reach an Output to be heard. Both controls appear only in node-backed mode; when a Spectral frame is edited *inside* a wavetable the shell already provides them, so the sub-editor hides its own. The standalone **Wavelet Space** node editor (`WaveletPainter`) gained the identical Envelope… + Preview pair for the same reason.

### Spectral curves

Each of the two curves is a `SpectralCurve` — the same reusable 1-D curve type used by the [AHDSR per-segment shapes](#per-segment-shape-curves) — with two authoring modes:

- **Equation** — a formula, in one of four languages (see below).
- **Drawn** — graphical, with **Points** (Catmull-Rom through draggable control points) and **Freehand** (per-sample painting) sub-modes, identical to the waveform editor's drawn layers.

The **same Built-in / Lua / Python / GLSL dropdown and bake machinery documented under [Formula authoring language](#formula-authoring-language-built-in--lua--python--glsl)** drives the Equation mode here, with one domain difference that matters:

- **Built-in:** the variable is **`f` = the integer bin index `[0, halfBins)`**, evaluated live in C++ at the synth's actual `halfBins`. So `exp(-f/20)` rolls off over ~20 bins regardless of resolution — a *bin-count-relative* contour. (The default magnitude is `exp(-f/20)`; the default phase is `noise()*pi` for a randomised phase.)
- **Lua / Python / GLSL:** the script bakes once over a **normalized `[0, 1]`** sweep (1024 points) on the UI thread (or the GPU, for GLSL) and is resampled to `halfBins` on evaluate — it can't see the live bin count. The variable is `x`, with `f` provided as an alias of `x` (both normalized). A scripted `exp(-f/20)` is therefore *near-flat* (f ∈ [0,1]), not a 20-bin rolloff — the same text means different things in Built-in vs script mode. This seam is intrinsic to the offline bake and is tracked for the `CurveContext` unification in `known-issues.md`. (Spectral/AHDSR curve bakes are **unclamped** — the `[-1,1]` clamp applies only to wavetable waveshapes.)

When a script language is selected the equation field grows into a multi-line editor (additive-style bodies that `return` a value are common for spectra). Bake errors show as a red `Script error: …` overlay on the curve canvas.

### Library-linked mag/phase curves

Each of the two curves has its own **Library…** button (above its canvas) that ties it into the project's [Frequency Graphs library](#frequency-graph-library-curves), using the exact same publish / link / detach popup as the Spectrum Tap (the shared `showFrequencyGraphLibraryMenu` helper):

- **Add this curve to library** — publishes the magnitude (or phase) curve as a new `FrequencyGraph` asset (default name `Magnitude curve` / `Phase curve`). The node **stays independent** (a copy is deposited in the library; the node is not auto-linked).
- **Load a copy from library** — mirrors a chosen asset's curve in as an **independent copy** (no link). The default, frictionless way to reuse a library curve.
- **Sync with library curve (read-only)** — mirrors a chosen asset's curve in as a **live, read-only mirror**. The curve panel locks; you edit it only in the library, or break the link from the panel badge (below).

Unlinking is **not** a menu item — the library popup only does library operations (publish / load / sync). To diverge from a synced curve, **click the panel's read-only badge** (it reads *linked – click to edit a copy*): that breaks the link and forks an independent, editable copy. A status line under each button shows **Independent curve** or **Linked: <name> (#id) — read only (click the badge to edit a copy)**. Hard-deleting the asset makes the curve fall back to its last cached shape as independent. This works both for a **standalone Frequency Domain node** and for a **Spectral frame nested inside a wavetable** (the wavetable shell passes its project graph into the sub-editor so library linking is available there too). Resolution is `resolveSpectralReferences()` (`spectral_editor.cpp`), called after `readProject()` and after each edit; it walks both standalone `__spectral2__` nodes and `__wavetable…` nodes (resolving any `SpectralFrame` in their frame library). *(See the [model-in-transition note](#asset-library-project-stores); a library-side editor for these assets is still to come.)*

### SpectrumTap

The **SpectrumTap** effect reuses `SpectralCurve` for a per-bin custom frequency response, so the same Equation/Drawn authoring and Built-in/Lua/Python/GLSL language choice applies there too. Right-click a bin → **Edit response curve…** opens the curve editor.

**Library-linked response curves.** The response editor has a **Library…** button that ties a bin's response into the project's [Frequency Graphs library](#frequency-graph-library-curves):

- **Add this curve to library** — publishes the bin's current curve as a new `FrequencyGraph` asset (default name `Response <bin label>`; rename later in the library browser). The bin **stays independent** (not auto-linked).
- **Load a copy from library** — picks a `FrequencyGraph` asset and mirrors it into the bin as an **independent copy** (no link).
- **Sync with library curve (read-only)** — picks an asset and makes the bin a **live, read-only mirror** of it; the curve panel locks. Break the link from the panel badge (below).

As with the spectral curves, unlinking isn't a menu item — **click the panel's read-only badge** (*linked – click to edit a copy*) to break the link and fork an editable copy. A status line under the canvas shows **Independent curve** or **Linked to library curve: <name> (#id) — read only (click the badge to edit a copy)**. **Use Default (bandpass)** and the bin's **Delete** also detach any link. Hard-deleting the referenced asset (from the library browser) makes the bin fall back to its last cached curve as an independent curve — references never dangle. Resolution is done by `resolveSpectrumTapReferences()` (`spectrum_tap.cpp`), called after `readProject()` and after each edit, mirroring `resolveAhdsrReferences()`. *(See the [model-in-transition note](#asset-library-project-stores).)*

### <a name="curve-eq"></a>Curve EQ

The **Curve EQ** effect is the third `FrequencyGraph` consumer (alongside the spectral synth's mag/phase curves and the Spectrum Tap). It applies a single user-drawn **frequency-response curve** — a gain *multiplier* vs. frequency — to any audio passing through it. It's the draw-a-shape complement to the biquad **Parametric EQ**: the biquad EQ is the low-CPU, minimum-phase, surgical tool (a handful of bands with type/freq/gain/Q); the Curve EQ lets you sketch an arbitrary response and applies it linearly across the whole spectrum.

**Created from** the node-graph right-click → **Add effect** → **Curve EQ (draw response)** menu item. Two node params show in the body: **FFT Size** (8–12, i.e. 256–4096 bins; default 11 = 2048) and **Mix** (0–1 dry/wet). **Double-click** the node to open the curve editor — one `SpectralCurvePanel` (gain axis `0..2`, where `1.0` = unity/flat, `<1` cuts, `>1` boosts; the processor clamps the applied gain to `[0,8]`). The same Equation/Drawn authoring and Built-in/Lua/Python/GLSL language choice as every other `SpectralCurve` applies; the default curve is the flat equation `1`.

**Library linking** is identical to the spectral editor: a **Library…** button + status line tie the response into the [Frequency Graphs library](#frequency-graph-library-curves) (publish / link / detach via the shared `showFrequencyGraphLibraryMenu`), so one drawn EQ shape can be live-shared with FFT wavetable frames, the Spectrum Tap, and other Curve EQ nodes. Resolution is `resolveCurveEqReferences()` (`curve_editor.cpp`), called after `readProject()` and after each edit; a hard-deleted asset detaches the node to its last cached curve.

**DSP — zero latency.** `CurveEQProcessor` (`builtin_effects.h`) runs a *block-local* Hann-windowed overlap-add STFT (75% overlap, `fftSize` clamped down to fit the block), multiplies each bin's **magnitude** by the curve value (phase untouched → linear / zero-phase), and normalises each output sample by the summed synthesis window. It introduces **no latency** by construction — a deliberate simplicity choice (not a PDC limitation; SEANCE's graph *does* delay-compensate, see [Plugin-delay compensation](#plugin-delay-compensation-pdc)). A latency-bearing high-quality mode is a noted backlog item. Edits commit through `commitToNode()` → `setNodeScriptSynced` + `commitSnapshot("Edit Curve EQ")` (snapshot undo; the editor debounces the audio-graph rebuild 500 ms).

**Serialization:** `__curveeq__:<curve.encode()>` with an optional trailing `|refs:<assetId>` field (emitted only when linked). The `CurveEq` namespace helpers (`curve_editor.h`) are the single source of truth shared by the processor, the editor, and the resolver.

### <a name="signal-eq"></a>Signal EQ (modulatable points)

The **Signal EQ** effect is the **signal-controllable** equaliser: a set of **curve points**, each a peaking bell, where every point's **centre frequency** *and* **gain** is an individual node param — and therefore individually **signal-modulatable** via the on-demand [Param-pin mechanism (#88)](#on-demand-modulation-pins). Drive a point's frequency from an LFO to make a sweeping notch; drive its gain from an envelope follower for dynamic EQ; wire an audio-derived control into either coordinate for anything in between. It's the third member of the EQ family: the [Parametric EQ](#parametric-eq) is the static surgical biquad tool, the [Curve EQ](#curve-eq) is the draw-an-arbitrary-shape tool, and the Signal EQ is the *moving-target* tool whose shape is meant to be modulated in real time.

**Model — a point *is* a biquad.** Each point at *(freq, gain)* is exactly one RBJ-cookbook **peaking biquad**: a bump (gain > 0) or dip (gain < 0) of that height centred on that frequency, with a bandwidth set by the shared global **Width** (the filter Q). The node's response is the product of every point's bell. This unifies "bins of arbitrary width and offset" and "draggable curve points" into a single model — moving a point slides/raises its bump; raising Width makes every bump narrower.

**Created from** the node-graph right-click → **Add effect** → **Signal EQ (modulatable points)**. Global params (shown in the node body): **Mode** (0 = Zero-latency, 1 = FFT exact), **Width** (shared Q, 0.1–24), **Mix** (0–1 dry/wet), **FFT Size** (8–12, used only in FFT mode). Then two params **per point**: `P<n> Freq` (20–20000 Hz) and `P<n> Gain` (−24…+24 dB). The default node has **3 flat points** (200 Hz / 1 kHz / 5 kHz, all 0 dB) so it passes audio unchanged until you move a point or wire a control input. Points are added/removed from the **node context menu** (*Add EQ Point* / *Remove Last EQ Point*, 1–16 points) or inside the editor.

**Modulating a point** needs no special UI: because each coordinate is a node param, right-click its slider in the node body → **Add control input** (the standard #88 affordance) to grow a `Mod:`/`Set:` Param input pin, then wire any Signal/Param cable into it. The editor offers the same via right-click on a point handle.

**Editor (`SignalEQEditorComponent`, `signal_eq_editor.{h,cpp}`).** Double-click the node to open a **log-frequency / dB canvas**. Drag a point handle to set its Freq (x) + Gain (y) — committed to the undo tree on mouse-up; the live audio follows the drag immediately (params are read per block). **Double-click empty space** adds a point there; **right-click a handle** → delete it, or add a frequency-(X)/gain-(Y) control input. A handle turns **orange** when that point has a live modulation cable. **Mode** / **Width** / **Mix** controls sit along the bottom. The editor is pure sugar over the params, so save/load, undo and automation all work without editor-specific code.

**DSP — two engines (`SignalEQProcessor`, `builtin_effects.h`).** Both share `updateCoefficients()` (the biquad bank built from the points + Width), so the **magnitude** response is identical between them; only phase/CPU/transient character differ:
- **Mode 0 — Zero-latency:** cascade the peaking biquads, processed per sample. Truly zero added latency; minimum-phase (the classic analog-EQ phase character around each bump).
- **Mode 1 — FFT exact:** a *block-local* Hann overlap-add STFT (75% overlap, like the Curve EQ) multiplies each bin by the **exact** magnitude the biquad cascade would produce (computed from the same coefficients via the digital biquad transfer function), but phase-preserved → linear/zero-phase. Costs an FFT per block and softens sharp transients slightly; still **block-local**, so it adds no inter-block latency either. Falls back to the biquad path when the block is too short to FFT.

`applySignalModulations()` runs first each block, so modulated points are evaluated before the coefficients are rebuilt — i.e. the response tracks the control signals at block rate. Both modes also apply the **Mix** dry/wet blend.

**Serialization:** none beyond the generic node fields — the script is the bare tag `__signaleq__`, and **all** state (global params, per-point Freq/Gain params, modulated `baseValue`s, and the modPin bindings) round-trips through the standard `[Node]`/`[Param]`/`modPin=` serialization in `project_file.cpp`. Undo is `commitSnapshot(...)` on every structural edit (add/remove point, add control input) and on drag-end.

### <a name="sms"></a>SMS (harmonic / noise split)

The **SMS** effect (*spectral modeling synthesis*) splits any incoming sound into two halves and gives each its own volume control:

- **Deterministic (harmonic)** — the strong spectral peaks. The tonal, pitched part: the note itself, the bowed string, the vowel.
- **Stochastic (noise)** — everything left over once the peaks are subtracted. Breath, bow scrape, consonants, cymbal wash, room hiss.

Turn **Noise Gain** up and **Harmonic Gain** down to make a voice all breath and no pitch; do the reverse to get an unnaturally pure, "de-aired" tone. It's also the cheapest way to make a pad sound airier without touching its pitch or EQ.

**Created from** the node-graph right-click → **Add effect** → **SMS (harmonic/noise split)**. Params:

| Param | Range | Meaning |
|---|---|---|
| **Threshold** | 0–1 | How loud a bin has to be, *relative to the loudest bin in that frame*, to count as harmonic. `0` = every bin is harmonic (so the noise half is empty and the node is a bypass); `1` = only the single loudest bin is harmonic. Default `0.1`. |
| **Harmonic Gain** | 0–3 | Level of the deterministic half. |
| **Noise Gain** | 0–3 | Level of the stochastic half. |
| **FFT Size** | 8–12 | Transform size as a power of two (256–4096). Default `10` = 1024. Bigger = finer frequency resolution (better peak isolation on low, slow material), smaller = better time resolution (less smearing of transients). |
| **Mix** | 0–1 | Dry/wet. |

Because the split is relative to each frame's own loudest bin, Threshold behaves consistently as the input level changes — you don't have to re-tune it when you turn the source up.

**DSP — zero latency (`SMSProcessor`, `builtin_effects.h`).** A *block-local* Hann overlap-add STFT at 50% overlap (`FFT Size` clamped **down to a power of two** that fits the host block). Per frame: window → forward FFT → find each bin's magnitude and the frame maximum → bins at or above `Threshold × max` are copied into a peaks-only spectrum → inverse FFT gives the harmonic signal → the residual is `windowed − harmonic`. The two are scaled by their gains, summed, re-windowed, and overlap-added; the sum is then **normalised by the accumulated window power**, so with `Threshold = 0` and both gains at 1 the node is *exactly* unity gain and bit-for-bit transparent. Samples that no frame covered (the first/last partial window of a block) pass the dry signal through rather than fading to silence. Like the two EQs it is block-local and therefore adds **no latency**.

> Before 2026-08 the overlap-add was **not** normalised. Hann² at 50% overlap sums to `0.5·(1 + cos²)`, which ripples between 0.5 and 1.0 — so SMS imposed an audible tremolo at the frame rate (≈43 Hz at FFT Size 1024 / 44.1 kHz) and ran about 2.5 dB quiet. Existing projects will hear SMS as slightly louder and no longer wobbling. The same change also fixed a crash: the transform size used to be clamped with `fftSize = n`, handing a **non-power-of-two** size to the FFT whenever the host block wasn't a power of two (480 samples is common), which indexed its bit-reversal table past its end and corrupted the heap.

**Serialization:** none beyond the generic node fields — the script is the bare tag `__sms__` and all five params round-trip through the standard `[Node]`/`[Param]` serialization. All params are signal-modulatable via the on-demand [Param-pin mechanism (#88)](#on-demand-modulation-pins).

### Serialization

`__spectral2__:<fftSize>|<mag.encode()>|<phase.encode()>` — each curve's `encode()` embeds its mode, expression (with `,`→`;` and `|`→`\x1F` escaping so it's safe inside the `|`-delimited blob), and language key. Two optional trailing `|`-fields follow, each self-identified by a prefix so order is flexible and unknown fields are skipped: `warp:<chain>` (the per-bin warp chain) and `refs:<magAssetId>:<phaseAssetId>` (the FrequencyGraph live-reference ids, emitted only when a curve is linked). `refs:` is written *after* `warp:` so an old decoder — which only checked `parts[3]` for a `warp:` prefix — still finds its warp and harmlessly ignores the ref field. The older `__spectral__:<fftSize>:<phaseMode>:<magExpr>|<phaseExpr>` format still decodes (as Built-in equations). Baked sample buffers are transient and re-created via `SpectralCurve::rebake()` on load.

The SpectrumTap script is `__spectrumtap__|<fftSize>|<curve0>|<curve1>|…` where each `<curveK>` is empty (biquad default) or a `SpectralCurve::encode()` payload. Bins linked to a `FrequencyGraph` asset append a trailing `|#refs|<slot>:<assetId>|…` section carrying the reference ids; the cached curve still persists in its slot so a deleted asset degrades gracefully. Pre-`#refs` decoders treat the `#refs` token as an un-decodable (hence biquad) slot and ignore it, so the format is backward compatible.

---

## Granular synths (Particle Cloud, Spectral Grain)

Two instrument nodes build sound out of **grains** — short bursts, tens of
milliseconds each, sprayed on top of one another fast enough to fuse into a
continuous texture. They share the idea and almost nothing else: **Particle
Cloud** synthesises each grain from a plain waveform, and **Spectral Grain**
plays grains cut from a bank of IFFTs of a spectrum you define.

Both take MIDI in and audio out, both are added from *Add Node → Instrument*,
and both use the [shared AHDSR envelope](#shared-ahdsr-envelope) as a
note-level VCA over the whole cloud — that envelope is *not* a set of params on
the node, it lives on `node.ahdsrEnvelope` and is edited by right-clicking the
node → *Envelope (AHDSR)…*.

### The two rates that decide everything

The single most useful thing to know about either node is how its two main
knobs interact. Grains are spawned at **Density** per second and each one lives
for **Grain Size** milliseconds, so the number sounding at once settles at:

```
grains = Density × Grain Size (in seconds) × (voices sounding)
```

That product, not either knob alone, is what you hear as "thickness":

- **Density high, Grain Size low** — many short grains. Crisp, granular,
  pointillist; individual grains are audible as texture.
- **Density low, Grain Size high** — few long grains. Smooth and pad-like,
  because each grain's own fade-in/fade-out is long enough to be a slow swell.
- **Both low** — you drop below the fusion threshold and start hearing
  individual events. This is where the glitchy/sparse sounds live.
- **Both high** — a dense wash. Note that this is also where the output gets
  loudest; see *Level* below.

### Particle Cloud (`__particlesynth__`)

Monophonic (one held note at a time). Each grain is an independently
randomised burst of a chosen waveform.

| Param | Default | Range | Meaning |
|---|---|---|---|
| **Density** | 30 | 1 – 200 | Grains spawned per second. |
| **Spread** | 7 | 0 – 24 | Pitch randomisation, in **semitones**, applied per grain as a symmetric ± range around the played note. 0 = every grain on pitch; 24 = ±2 octaves of scatter. |
| **Grain Size** | 50 | 1 – 500 ms | How long each grain lasts. |
| **Attack** | 0.1 | 0 – 1 | Each grain's own fade-in, as a **fraction of that grain's length** — not a time. So it scales automatically when you move Grain Size. |
| **Release** | 0.3 | 0 – 1 | Same, for each grain's fade-out. Attack + Release above 1.0 simply means the grain never reaches full level. |
| **Shape** | 0 (sine) | 0 – 3 | Grain waveform: **0 = sine, 1 = saw, 2 = square, 3 = noise**. |
| **Volume** | 0.5 | 0 – 1 | Output level. |

*Spread* is what separates this node from just playing a note: with Spread at 0
and a sine Shape it is a slightly grainy oscillator, and everything interesting
comes from widening it. Because the randomisation is per grain and uniform, a
wide Spread over a dense cloud reads as a **noise band** centred on the played
note rather than as a chord.

Each grain's envelope is a straight **linear** ramp up over *Attack* and down
over *Release*, with a flat top in between — deliberately not a smooth window,
because the corner is part of the node's character.

Grains are also **panned randomly** across the stereo field, one position per
grain, using an equal-power (sin/cos) law. This is not a param — it's always
on, and it's the main reason a dense Particle Cloud sounds wide without any
external stereo processing. A sparse cloud, by contrast, scatters audibly
across the image.

Grain spawning continues through the AHDSR **release** stage, so a long release
gives a cloud that thins out gradually rather than one that stops spawning at
note-off and leaves only its tails.

### Spectral Grain (`__spectralgrain__:<magnitude expression>`)

**8-voice polyphonic.** Instead of a waveform, you define a *magnitude
spectrum* as an expression in `f` (the FFT bin index), and the node pre-renders
a bank of **16 grains**, each an inverse FFT of that same spectrum with
different random phases. Playback picks a bank entry at random per grain, so
the timbre is fixed but the phase relationships keep shifting — that's what
produces the shimmering, never-quite-repeating character.

The default expression is `exp(-f/10)`, a steep low-pass roll-off.

| Param | Default | Range | Meaning |
|---|---|---|---|
| **Density** | 20 | 1 – 200 | Grains spawned per second, **per voice**. |
| **Grain Size** | 40 | 1 – 200 ms | How long each grain lasts. |
| **Volume** | 0.5 | 0 – 1 | Output level. |

Notes on the model:

- The grain bank is generated at a fixed **1024-point** FFT, which is what
  fixes the meaning of `f`: bin `f` is `f × sampleRate / 1024` Hz at the
  reference pitch A4. Changing the expression regenerates the bank.
- MIDI note number sets a **playback rate** (`2^((note − 69)/12)`), i.e. the
  bank waveform is read faster or slower. So the whole spectrum transposes with
  the note, as with a sampler, rather than each partial being retuned.
- A bank entry is one *period* of the defined spectrum and therefore exactly
  periodic, so grains longer than 1024 samples simply loop through it. Grain
  Size is honoured across its whole range and is independent of the note
  played. (Before 2026-08 grain length was clamped to the bank length — 21 ms
  at 48 kHz — so most of the Grain Size range was inert and a grain played
  above A4 ran off the end of the bank and died early, making grain duration
  depend on the note. See `known-issues.md`.)
- Each grain belongs to the voice that spawned it and is rendered under that
  voice's envelope, so a chord really is several distinct pitches. Grains
  outlive their voice's note-off (that is what `getTailLengthSeconds` accounts
  for), but a voice slot being recycled for a new note drops its leftovers so
  the new note can't inherit the old cloud.

### Level

Both nodes divide the summed cloud by `√(grain count)` before *Volume*, which is
the right *statistical* correction for summing uncorrelated signals — but it's
an average, not a bound. A cloud whose grains happen to line up in phase can
still come out well above the nominal level. The two nodes then differ:

- **Spectral Grain** hard-clips its output to ±1.0. Past the point where the
  clipper engages, raising Density makes the cloud *dirtier* rather than
  louder, so pull Volume down as you raise Density if you want it clean.
- **Particle Cloud has no output limiter at all** and can exceed 0 dBFS on
  dense settings. Watch the meter on whatever it feeds; adding a limiter or
  soft-clip here is tracked in `known-issues.md`.

Either way, **Volume is not a linear trim at high Density** on these nodes.

### Grain ceiling

Each node caps its simultaneously-sounding grains at **1024**
(`ParticleSynthProcessor::kMaxGrains`, `SpectralGrainProcessor::kMaxActiveGrains`).
The cap exists so the grain list can be reserved once in `prepareToPlay` and
never reallocate on the audio thread; when it's hit, the **newest** grain is
dropped rather than the oldest being stolen, so a saturated cloud keeps
sounding instead of stuttering.

**You will not reach it by turning knobs.** At the param maxima the ceiling is
100 grains for Particle Cloud (200/s × 0.5 s, monophonic) and 320 for Spectral
Grain (200/s × 0.2 s × 8 voices) — three to ten times under the cap. Signal
cables can't get you there either: `applySignalModulations` clamps a modulated
param to its declared range, so a cable can't push Density past 200. The cap is
a safety net for the audio thread, not a knob you can run into.

### Transport Stop

Both nodes implement the transport-panic hook, so pressing **Stop** cuts the
cloud dead — every in-flight grain is dropped and every voice silenced —
rather than letting hundreds of grain tails ring out. Neither node responds to
MIDI *All Notes Off*, so panic is the only thing that stops a held note on
them.

### Serialization

Nothing bespoke. Particle Cloud's script is the bare tag `__particlesynth__`;
Spectral Grain's is `__spectralgrain__:` followed by the magnitude expression.
All params and the node AHDSR round-trip through the standard node
serialisation. All params are signal-modulatable via the on-demand
[Param-pin mechanism (#88)](#on-demand-modulation-pins).

---

## Terrain Synth

The shared engine behind Waveform / Sampler / Piano / SoundFont and the explicit Terrain Synth node. Treats the sound source as an N-dimensional **terrain** (an array of samples in 1D-or-higher) and a **traversal** (a curve that walks the terrain over time, reading out samples as audio).

### Terrain sources

- **1D waveform** — a single-cycle waveform (e.g. Layered Waveform output)
- **1D audio file** — any recorded sample; the whole file is the terrain
- **2D image** — grayscale image where pixel brightness becomes sample amplitude; wide → long sound, tall → more vertical headroom
- **3D video** — a clip imported via **Add Node → Terrain → From Video…**; each frame's grayscale brightness becomes a 2D amplitude surface and the time axis becomes the third dimension. See [Video import](#video-import-3d-terrain) below.
- **N-D wavetable** — multiple frames in Grid or Scatter layout (see above)
- **Math expression** — procedural; evaluated at every grid coordinate
- **Programmatic (Generate)** — a terrain built by running a user **program** (Builtin math, Lua, Python, a **GLSL compute shader** on the GPU, or a pre-compiled **WASM module**), either once per grid cell (per-cell) or once over the whole array (whole-grid, for cross-cell effects like blur/CA/FFT — on the CPU languages, on GLSL via **multi-pass** ping-pong, and on WASM via its own loop), at any rank and resolution you choose. The computed grid is **baked into the project** so it never re-runs on load. See [Programmatic source](#programmatic-source-generate) below.

The traversal doesn't care which source produced the terrain.

### Video import (3D terrain)

**Add Node → Terrain → From Video…** opens the **Import Video** dialog, which decodes a video clip into a 3-dimensional terrain `{frames, height, width}`: each frame contributes a 2D brightness surface (pixel luminance → sample amplitude, exactly like the 2D image source) and the frames stack along the time axis. The result is a 3-axis terrain you traverse like any other (Orbit/Lissajous/Path through the `{time, y, x}` volume).

**Runtime requirement: ffmpeg.** Decoding is done by shelling out to the **ffmpeg / ffprobe command-line tools** via `juce::ChildProcess` (`video_decoder.cpp`) rather than linking a per-OS native decoder or vendoring libav. This is the same code path on Windows, macOS and Linux — the only requirement is that `ffmpeg` (and ideally `ffprobe`) are installed and on `PATH`. When ffmpeg is missing the **From Video…** dialog still opens but the **Choose Video…** button is disabled with a tooltip explaining how to enable it. ffmpeg is only needed at import / re-crop time; see "Baked terrain data" below.

The dialog (`video_import_dialog.cpp`) gives you:

- **Frame preview** with a draggable **crop rectangle** (XY spatial crop). Drag the interior to move it, the edges/corners to resize; a live `W × H px` readout shows the cropped source size and the area outside the crop is dimmed.
- **Transport** — Play/Pause plus a **timeline bar** below the preview. The bar has a **playhead** (drag or click anywhere to scrub) and two **in/out handles** that set the **time crop**; the kept span is highlighted and playback loops within it.
- **Output grid size** — three controls:
  - **Output W (px)** and **Output H (px)** — the spatial resolution of the terrain grid. They are **aspect-linked** to the crop: editing one recomputes the other from the cropped region's source-pixel aspect ratio (resizing the crop rectangle also re-derives the height from the current width). This is the "scale down the selected XY region" control — one effective scale for both axes via the linked aspect.
  - **Output frames** — the number of evenly-spaced time slices sampled across the time crop (the depth of the 3D terrain).
  - A live `Grid: W × H × frames = N cells` readout.
- **Import** decodes the final grid from the **original** video at full source quality (`VideoDecoder::decodeGrid`, background thread): it time-trims to the in/out range, crops the source rectangle, scales each frame to Output W × H, temporally resamples to exactly Output frames, and converts to grayscale — all in one ffmpeg filter chain.

**Performance / threading.** On load the dialog extracts a low-resolution JPEG **proxy** frame sequence once (`VideoDecoder::extractProxy`) so scrubbing and playback never spawn ffmpeg per displayed frame. Both the proxy extraction and the final decode run on detached `std::thread`s writing into `std::shared_ptr` job structs with atomic flags; a `juce::Timer` polls them on the message thread, so closing the dialog mid-decode is safe (the shared_ptr outlives the component).

**Baked terrain data.** Like wavetables bake their PCM into the node script, the decoded 8-bit grayscale grid is base64-encoded and stored **in the node script** itself. The script format is:

```
__video__:<path>|<t0>,<t1>|<cx>,<cy>,<cw>,<ch>|<outW>,<outH>,<outFrames>|<base64 gray>
```

(`|` is illegal in Windows paths and absent from the base64 alphabet, so the path is reassembled from all leading split fields for POSIX safety.) Because the grid is baked in, **reloading a project needs neither ffmpeg nor the original video file** — the terrain is rebuilt straight from the script in `TerrainSynthProcessor`'s constructor (`Terrain::fillFromVideoData`). The leading source fields (path / time / crop / output dims) are also what re-seed the Import Video dialog's controls when you re-open an already-imported node, so you can re-crop without re-picking the file. Right-click a video terrain node → **Edit Video…** re-opens the dialog seeded from the node's script (re-cropping does need ffmpeg and the source file again, since the final decode reads the original). Re-opening only parses those header fields (it skips the large base64 blob) and re-decodes from the source on the next Import.

A video terrain classifies as a **Surface** source (`SynthSourceClass::Surface`), so its default Synth Mode is AM-sine (WaveformPerPoint), the same as the 2D image source.

### Programmatic source (Generate)

**Add Node → Terrain → Terrain from Program (Generate)…** opens the **Generate Terrain** dialog (`generate_dialog.cpp`), where you build a terrain of **any rank and resolution** by writing a short program that is run **once per grid cell**. This is the counterpart to importing audio/image/video: instead of decoding a file, the terrain is *computed*, which lets you reach bit depths a video/image source can't (the data is 32-bit float in memory) and dimensionalities a file can't represent.

**Generation mode.** The dialog's **Mode** picker chooses how the program is run:

- **Per-cell (one call per cell)** — the program runs once for every grid cell and returns that cell's value. Simple and the only mode some languages support. Each call sees the cell's coordinate (below).
- **Whole-grid (one call, full array)** — the program runs **once** and fills the entire array itself. For CPU languages (Lua, Python) this unlocks **cross-cell algorithms a per-cell program physically cannot express** — convolution / blur, cellular automata, FFT, iterative relaxation, global normalisation — because the program can read every cell while writing every cell. Available for languages that can run block-at-a-time (**Lua**, **Python**, **GLSL**, **WASM**); the option is greyed for per-cell-only languages (Builtin), with a tooltip explaining why and how to enable it. **WASM is whole-grid *only*** — its mirror-image constraint (it owns its own loop, so it can't be re-entered per cell) is explained under the Languages list below. **GLSL whole-grid is different**: it's a GPU "raw compute" mode where the shader's `main()` owns the writes — within a single pass every invocation runs in parallel, so it does *parallel per-output-cell* work (manual N-D indexing, multiple writes, scatter), but **true cross-cell reads of other output cells are not safe in one pass** (that needs multi-pass ping-pong — a documented future enhancement). For convolution/CA/relaxation today, use Lua or Python whole-grid.

**Per-cell program model.** In per-cell mode, for each cell the program sees:

- `c0 … c{nd-1}` — the cell's normalized coordinate along each axis, each in `[0,1]`. (In **GLSL** these are an array `c[16]` indexed `c[0] … c[nd-1]`, since GLSL has no dynamic global names.)
- `x, y, z, w` — aliases of `c0 … c3` scaled to `[0, 2*pi]`, so a math expression written for the **Math expression** source (`sin(x)*cos(y)`) works unchanged here.
- `nd` — the number of dimensions.
- **GLSL only**, additionally: `coord[d]` — the integer cell index along axis `d`; `dims[d]` — the axis size; `TAU` — 2π. The per-cell GLSL body must `return` a `float` (it's the body of a `cellValue(…)` function).

**Whole-grid program model (Lua / Python).** In whole-grid mode, the program defines a `generate()` function (rather than `loop()`) that fills the array. It sees these globals and helpers:

- `dims` — a 1-based table (Lua) / 0-based list (Python) of the axis sizes; `nd` — the rank; `total` — the cell count (`product(dims)`).
- **`getAt(c0, c1, …)` / `setAt(c0, c1, …, v)` — direct N-D pixel access (the recommended way).** Read or write a cell by its **per-axis integer coordinates**, with no manual flattening. `getAt` **edge-clamps** each coordinate to `[0, dim-1]`, so a stencil that reads past a border replicates the nearest edge cell (the useful default for blur/convolution) and never goes out of bounds; missing trailing coordinates count as `0`. `setAt` takes the value as the argument **right after the `nd` coordinates** (`setAt(r, c, v)` on a 2-D grid), clamps it to `[0,1]`, and **ignores the write if any coordinate is out of range** (unlike `getAt`'s clamped reads — this mirrors `set` so an off-by-one loop can't silently clobber an edge cell). With these you write N-D generators as nested coordinate loops — `for r…: for c…: setAt(r, c, blur_of(getAt(r-1,c), getAt(r+1,c), …))` — which is why the flat-index helpers below are rarely needed on the CPU.
- `set(i, v)` — write **flat** cell `i` (0-based, row-major: the last axis varies fastest) to `v` (clamped to `[0,1]`). The flat-index counterpart to `setAt`, for the `for i = 0, total-1` iteration style.
- `get(i)` — read **flat** cell `i` back (0 before it's written). Flat-index counterpart to `getAt` (note `get` zero-pads out of range whereas `getAt` edge-clamps).
- `coord(i, axis)` — the normalized `[0,1]` position of flat cell `i` along `axis` (0-based), so you can recover coordinates from a flat index.
- `coordAxis(i, axis)` — the *integer* coordinate of flat cell `i` along `axis` (the inverse companion to `flatten`; `coord` is just this divided by `dim-1`).
- `flatten(c0, c1, …)` — per-axis **integer** coordinates → a flat index (row-major, last axis fastest), each coordinate **edge-clamped** to `[0, dim-1]`; missing trailing arguments count as `0`. On the CPU this is **rarely needed** now that `getAt`/`setAt` exist — it's mainly here for parity with GLSL, where the SSBO is a flat array and there is no `getAt`/`setAt`, so `data[flatten(…)]` is the *only* way to address a cell by coordinate. (`getAt`/`setAt` are literally `get`/`set` composed with this clamp.)
- `neighbor(i, axis, delta)` — the flat index of the cell `delta` steps from `i` along `axis`, **clamped to the edge**; the flat-index way to find a neighbour (on the CPU `getAt(r, c±1)` usually reads better). Matches the GLSL whole-grid `neighbor`.

`coord` / `coordAxis` / `flatten` / `neighbor` mirror the GLSL whole-grid index vocabulary exactly, so flat-index code ports between CPU and GPU unchanged; `getAt` / `setAt` are the CPU-only ergonomic layer on top (GLSL can't offer them because each shader invocation owns one output cell and cross-cell reads must come from the `prev[]` ping-pong snapshot — see the GLSL model below).

**Python only — a native numpy `grid`.** When numpy is importable (the usual case), the whole-grid Python program is *additionally* handed **`grid`**: a `float64` numpy ndarray shaped exactly like the terrain (`grid.shape == tuple(dims)`, row-major). This is the canonical store, so you can skip flat indices entirely and write idiomatic numpy — `grid[r, c] = …`, slicing (`grid[0, :] = ramp`, `grid[mask] = 0`), or fully vectorized field math (`grid[:] = np.sin(xx) * np.cos(yy)`). The result is read straight out of the array's buffer (the buffer protocol — no per-cell Python round-trips), which also makes numpy generators dramatically faster than a Python `for`-loop over `setAt`. The `set` / `get` / `getAt` / `setAt` helpers operate on the **same live `grid`**, so mixing helper calls with numpy slicing is fine, and reassigning `grid` wholesale (`grid = grid + 1`, `grid = np.roll(grid, 1, axis=0)`) is honoured — readback always reads whatever `grid` points to at the end of `generate()`. One semantic note: the helpers clamp each write to `[0,1]`, but **raw numpy writes do not** — out-of-range values are clamped once, at readback, via `np.clip(grid, 0, 1)` (then mapped to bipolar `[-1,1]`). If numpy is **not** installed, `grid is None` and the cells live in a flat list reached only through the helpers; a generator that should run in either environment can branch on `if grid is not None:`. *(Lua and WASM get no equivalent: Lua's only container is the nested table — 1-indexed, allocation/GC-heavy, no stdlib ndarray — and WASM exposes only flat linear memory, so for those two the flat host buffer plus `getAt`/`setAt` is the correct model. numpy is the only scripting stack here with a true native N-D array, so `grid` is Python-exclusive.)*

**Whole-grid program model (GLSL).** GLSL whole-grid is the body of the compute shader's `main()` (not a `generate()` function). It runs once per GPU invocation (one per cell) and sees:

- `data[]` — the output SSBO at binding 0 (`float`, length `total`); the shader writes `data[gid]` itself.
- `gl_GlobalInvocationID.x` — the flat cell index for this invocation; `uTotal` — the cell count; `nd` — the rank; `uDims[16]` — the axis sizes.
- `coordOf(idx, axis)` — the normalized `[0,1]` position of flat cell `idx` along `axis` (the GPU analogue of Lua's `coord`); `coordAxis(idx, axis)` — the *integer* coordinate along `axis`; and `TAU` (= 2π).
- `flatten(c0, c1, …)` — the inverse of `coordAxis`: integer per-axis coordinates → a flat `data[]`/`prev[]` index. **This helper is emitted *specialised to the current terrain*:** its parameter count equals the rank `nd`, and each axis size is baked in as a *literal constant* with edge-clamping, so there's no `uDims[]` loop. A `{4,128,128}` terrain gets `int flatten(int c0,int c1,int c2)` returning `(clamp(c0,0,3)*128 + clamp(c1,0,127))*128 + clamp(c2,0,127)` (row-major: last axis varies fastest). The per-axis sizes are also exposed as literal constants `DIM0 … DIM{nd-1}`. (Generic `neighbor(idx,axis,delta)` covers single-axis stepping; `flatten` is for jumping to an arbitrary computed coordinate.)

Always guard with `if (gl_GlobalInvocationID.x >= uint(uTotal)) return;` — dispatch is rounded up to a multiple of the 64-wide workgroup, so some invocations run past the end.

**Multi-pass "ping-pong" (GLSL whole-grid).** A single compute pass writes each output cell **in parallel and independently**, so a cell cannot reliably read another cell's *new* value within the same pass — which rules out convolution/blur, cellular automata, diffusion/erosion, and any stencil that reads neighbours. The **Passes** field (GLSL whole-grid only; default 1) solves this with classic ping-pong: the shader runs `Passes` times over **two alternating buffers**, and on every pass it reads the **previous pass's complete output** through a read-only `prev[]` buffer (binding 1) while writing `data[]`. Because `prev[]` is a stable snapshot of the prior pass, neighbour reads are well-defined. The whole-grid model gains, in multi-pass:

- `prev[]` — the previous pass's full output (binding 1); on **pass 0** it is **all zeros**.
- `prevAt(idx)` — `prev[idx]`, returning `0` if `idx` is out of range (safe reads).
- `neighbor(idx, axis, delta)` — the flat index of the cell `delta` steps from `idx` along `axis`, **clamped to the edge**. Compose it for diagonals/N-D stencils: `neighbor(neighbor(i, 0, dy), 1, dx)`.
- `uPass` — the current pass index (`0 … uNumPasses-1`); `uNumPasses` — the total pass count.

The idiom is **seed on `uPass == 0`, then iterate**: pass 0 writes the initial field (ignoring the all-zero `prev[]`), and later passes transform `prev[]` into `data[]`. After the last pass the most-recently-written buffer is read back and baked. `Passes = 1` is exactly the original single-pass behaviour (one pass, `prev[]` unused). Each pass is a full GPU dispatch; the pass count is capped at **4096** (`kGlslMaxPasses`). The dispatch primitive is `glslDispatchComputePingPong` (`glsl_compute.h`); `uPass` is set automatically per pass, the two buffers are zero-initialised with `glClearBufferData`, and a `GL_SHADER_STORAGE_BARRIER_BIT` memory barrier between dispatches makes each pass's writes visible to the next pass's `prev[]` reads. *(Per-cell GLSL is always single-pass and uses the single-buffer `glslDispatchCompute` path — Passes is hidden for it.)*

**Whole-grid program model (WASM).** A WASM module is a **pre-compiled binary**, not source you edit in the dialog — author it in C / Rust / Zig against the ABI in `cpp/include/soundshop_wasm.h` and pick the `.wasm` with **Browse .wasm…**. The runtime (`script_runtime_wasm.cpp`, overriding `IScriptRuntime::runGenerate`) makes the host-owned grid available through fixed-arity host imports in module `"env"` — there is no `getAt`/`setAt` ergonomic layer (those are language-side conveniences), but the header ships inline `ss_grid_flatten` / `ss_grid_getat` / `ss_grid_setat` helpers built on the flat imports:

- `ss_grid_total()` — cell count (`product(dims)`); `ss_grid_nd()` — rank; `ss_grid_dim(axis)` — one axis size. (These mirror Lua's `total` / `nd` / `dims`.)
- `ss_grid_set(flat, v)` — write **flat** cell (0-based, row-major: last axis varies fastest); `v` is clamped to `[0,1]` (NaN→0) and an out-of-range index is ignored — identical semantics to Lua's `set`.
- `ss_grid_get(flat)` — read flat cell back (0 before written, like Lua `get`).
- `ss_grid_coord(flat, axis)` — normalized `[0,1]` position of a flat cell along an axis (Lua `coord`); `ss_grid_coord_axis(flat, axis)` — the *integer* index (Lua `coordAxis`).
- `ss_grid_neighbor(flat, axis, delta)` — flat index of a neighbour `delta` steps along `axis`, **edge-clamped** to `[0, dim-1]` (identical to Lua's `neighbor`; an out-of-range `axis`/`flat` returns the input index unchanged).

The module exports `void ss_init(void)` (re-seed hook, called before generate) and `void ss_generate(void)` (fills the grid). Output contract is the same as every other language — each cell a `[0,1]` value, mapped to bipolar `[-1,1]` as `v*2-1`. Because the bake is **offline** (once, on the message thread), per-cell host-call overhead is irrelevant, so the grid stays host-owned and poked via imports rather than shared through the module's linear memory. The same `WasmRuntime` load path serves audio Script-node modules (which export `ss_process`) and terrain modules (which export `ss_generate`); a module needs `ss_init` plus at least one of the two.

**Factory waveforms — `waveform(id_or_name, phase)`.** Every language and mode can sample the ~4000 bundled single-cycle factory waveforms (the same AKWF bank the Layered Wave editor and the waveform browser draw from — see [Factory waveform library](#factory-waveform-library)). Every waveform has a **stable integer id** — the same integer in every language, shown as the dim **`#N`** in the factory-waveform browser and returned by `waveforms["name"]` in Lua/Python. The integer is the canonical key; names are a convenience.

```
waveform(42, c0)                 // raw sample of waveform #42 at phase c0, in [-1,1]
waveform("AKWF_sin", c0)         // by name (Builtin/Lua/Python only)
waveform(42, c0)*0.5 + 0.5       // mapped into the [0,1] output contract
local w = waveforms["AKWF_sin"]  // Lua: resolve a name to its id once…
waveform(w, c0)                  // …then reuse the fast integer in the hot loop
```

- The **first argument is the waveform id** (an integer entry index) **or a name** (where supported). An out-of-range id / unknown name resolves to "silence" (the call returns `0` for every phase) rather than erroring, so a typo degrades gracefully. There is **no source-rewriting parser** anywhere anymore — the entire bank is made available to each language and the lookup is an ordinary runtime/GPU index:
  - **GLSL:** GPUs have no strings, so GLSL is **integer-only** — pass the id, e.g. `waveform(42, phase)`. The whole bank is uploaded **once per session** to a cached read-only std430 SSBO at **binding 2** (`glslSetCachedBuffer`, keyed by `kWaveformBankBufferId`), globally indexed (`id*512 + sampleIndex`), and the generated `float waveform(int id, float phase)` indexes straight into it with the same wrap+interpolate as the CPU path. No `waveform("…")` lexical scan, no per-bake repacking, no rewriting of your source. The function + SSBO are emitted only when the shader source mentions `waveform` (a cheap substring check, not a parse), so a shader that never calls it pays nothing. Works in per-cell and whole-grid (including multi-pass) GLSL — the cached buffer is re-bound before every dispatch and stays valid across all passes.
  - **Lua / Python:** `waveform` is an ordinary registered runtime function (`l_waveform` / `py_waveform`) that accepts **either** an integer id **or** a name string (resolved by `WaveformBank::indexForName`, case-insensitive, leading/trailing whitespace ignored). Because it's a normal argument, the name **need not be a literal** — `waveform("AKWF_" .. n, c0)` works. Both languages also expose **`waveforms`**, a name→id map you can consult to avoid per-call name hashing: `waveforms["AKWF_sin"]` returns the same integer GLSL uses. The map **caches** each lookup (Lua `rawset`s the result into the table; Python's `dict.__missing__` stores it), so a given name hits `indexForName` only once even when read from a per-cell loop. The fast idiom is **resolve once, reuse the integer**: in Lua cache it in a `local` (at top level or in `start()` for a streaming script); in whole-grid Python cache it at top level; in per-cell Python `waveform(waveforms["name"], c0)` is already fast after the first cell (the dict hit replaces the hash).
  - **Builtin:** the math-expression language is the *only* one SEANCE parses itself (`WaveExprParser`); the `waveform` atom reads a quoted **name literal** straight from the source (or a numeric id). The name must be a literal (the language has no string variables). The terrain path re-parses per cell, so the lookup runs per cell — fine for the simple language; use Lua/Python/GLSL for large grids that need a cached integer.
- The **second argument is a phase in `[0,1)`** that wraps (so `1.0` ≡ `0.0`, `2.5` ≡ `0.5`); it indexes the 512-sample cycle with **linear interpolation** between adjacent samples. Passing a normalized coordinate (`c0`, `c1`, …) plays exactly one cycle of the waveform across that axis.
- The return value is the **raw `[-1,1]` sample**. Because the terrain output contract is unipolar `[0,1]`, map it yourself when the waveform *is* the output: `waveform(id, c0)*0.5 + 0.5` (or `unipolar(waveform(id, c0))` in Builtin). Used as an *ingredient* inside a larger bipolar expression, no mapping is needed.
- **Finding an id:** open the factory-waveform browser (Layered Wave editor → factory library) and read the dim `#N` on the right of each row, or — in Lua/Python — print/inspect `waveforms["name"]`. The id is stable as long as `waveforms.bin` doesn't change, so it's safe to type into GLSL source and save in a project.

**Output contract.** Either mode produces **one value in `[0,1]`** per cell — think of it as a grayscale height / brightness — which is mapped to the terrain's bipolar `[-1,1]` as `v*2-1`, exactly like the image and video sources. (Per-cell returns the value; whole-grid writes it via `set()`.) This unipolar contract, rather than the Math-expression source's bipolar one, is dictated by the script runtime: the Signal role clamps Lua output to `[0,1]`. Builtin output is clamped to `[0,1]` here too so both languages behave identically.

**Languages.** The dialog's **Language** picker lists only languages that can generate **and** are compiled into the build:

- **Builtin (math expression)** — a single math expression (the same parser/functions as the Math-expression source: `sin`, `cos`, `abs`, `sqrt`, `pow`, `tanh`, `noise`). Always available, **per-cell only**.
- **Lua** — a full function (`loop()` per-cell, or `generate()` whole-grid), so you can use locals, conditionals and loops. Listed only when Lua is vendored in the build. Supports **both** modes.
- **Python** — a full Python program with the embedded-CPython interpreter (the same one the script console / signal scripting / shape baker use). Per-cell: a bare expression or a body that `return`s a value, seeing `c0…c7` / `x,y,z,w` / `nd` exactly like the other languages. Whole-grid: a `def generate():` that fills the array via `set(i, v)` / `get(i)` / `coord(i, axis)` with `dims` / `nd` / `total` globals (mirrors the Lua whole-grid API). Listed only when the Python DLL is available at runtime (it's delay-loaded; a missing DLL just hides the option). Supports **both** modes. **Python can't run on the audio thread** (where the terrain node is built during graph rebuild), which is the original reason the generated grid is now baked for *every* language — see "Baked data" below.
- **GLSL (compute shader, GPU)** — the program is a GLSL **compute shader** dispatched on an **offscreen OpenGL 4.3 core context** that SEANCE stands up on demand (the app's UI is otherwise 100% software-rendered — there is no live GL context; the bake context is created lazily and used only at Generate time). Per-cell: the body of a `float cellValue(…)` that `return`s `[0,1]`, with the coordinate vocabulary above plus `coord[d]`/`dims[d]`/`TAU`. Whole-grid: the body of `main()`, which writes `data[gid]` itself, optionally **multi-pass** via the **Passes** field for cross-cell convolution/CA/diffusion (see the GLSL whole-grid model above). Listed only when a GPU/driver capable of a headless 4.3 core context is present (`glsl_compute.cpp` → `glslComputeAvailable()`); otherwise it's hidden, like a missing Python DLL. Supports **both** modes. **GLSL bakes like Python** — it can't run on the audio thread (no GL context there), so the grid is computed once at Generate time and stored. **GPU caveat:** float results can differ slightly across GPUs/drivers, so the same shader may bake to different bytes (and thus a different content-store hash) on different machines — fine for dedup, not bit-reproducible. Dimension cap is **16** (GLSL fixed-array limit); use Lua/Python for higher rank.
- **WASM (.wasm module)** — a **pre-compiled** WebAssembly module run under wasm3, picked as a **binary file** (a **Browse .wasm…** button replaces the code editor) rather than typed source. **Whole-grid only** — there is no code-editor / per-cell mode for it. The module exports `void ss_init(void)` + `void ss_generate(void)` and fills the host-owned grid through fixed-arity host imports (`ss_grid_total/nd/dim/set/get/coord/coord_axis/neighbor`, module `"env"`) that mirror the Lua whole-grid API; the full ABI and a generator example live in `cpp/include/soundshop_wasm.h`. Listed only when wasm3 is compiled into the build (`wasmRuntimeAvailable()`); otherwise hidden, like a missing Python DLL. **Why no per-cell?** Per-cell generation re-invokes the program once per cell, but a WASM module *owns its own loop* and is never re-entered per element (the same property that makes it a streaming audio node) — so per-cell WASM is correctly rejected (the dialog greys the per-cell mode with an explaining tooltip; the self-test asserts a per-cell WASM request fails). It's the mirror image of Builtin, which has *no* loop and so is per-cell only. Like Python/GLSL, WASM **bakes** — the grid is computed once at Generate time and stored, never re-run on load.

The `GenLang` enum used by this dialog is **distinct** from `ScriptLang`, and the two only agree on Builtin (0) / Lua (1). `GenLang::Python` (value 2) is **not** `ScriptLang::Wasm` (value 2) — Python here goes through the embedded `ScriptEngine`, not an `IScriptRuntime`; `GenLang::Glsl` (value 3) is **not** a `ScriptLang` at all (it dispatches a compute shader via `Terrain::fillFromGlsl` / `glsl_compute.h`, bypassing the runtime layer); and `GenLang::Wasm` (value **4**) maps to `ScriptLang::Wasm` (value 2) — the values differ, so a blind `(ScriptLang)(int)lang` cross-cast would mis-route. **Never cross-cast `GenLang` to `ScriptLang`** — `generate_dialog.cpp` maps explicitly via `genLangToScriptLang()`, and a self-test (`gen: GenLang/ScriptLang values diverge`) pins the value divergence so the trap can't silently reappear.

**Dimensions.** The **Dimensions** field is a comma-separated list of axis sizes, so the rank is just how many numbers you type: `44100` = 1D audio, `512, 512` = a 2D image, `64, 128, 128` = 3D video `{frames, h, w}`. Any rank **up to 8** is accepted (the cell-generation backend `Terrain::fillFromScript` is itself unbounded; the 8-axis ceiling is the current node-wide limit on Sig pins / Center-Radius params, lifted everywhere at once when that cap is generalized). The total cell count is capped at 1G to avoid runaway grids.

**Generate** validates the program by running it over a tiny probe grid (same rank, sizes clamped to ≤3) so syntax/runtime errors surface instantly in the status line without paying for the full grid. On success it then computes the **full-resolution** grid right there **on the message thread** (`generateGrid()` → `Terrain::fillFromScript` / `Terrain::fillFromScriptWholeGrid` for Builtin/Lua, `ScriptEngine::bakeTerrain` for Python, `Terrain::fillFromGlsl` — with the **Passes** count for whole-grid — for GLSL) and stores both the program *and* the computed float data into the node. (The probe validation runs GLSL with a single pass — it only needs to catch compile/link errors, not run the whole ping-pong iteration.)

**Baked data, not just the program.** A generated terrain stores the **program + language + mode + passes + dimensions *and* the computed grid**. On load the node's constructor just deserializes the data — it never re-runs the generator. The script format is:

```
__generate__:<langInt>[:<modeInt>[:<passes>]]|<dim0>,<dim1>,…,<dimN-1>|<base64 source>|<dataField>
```

`<langInt>` is the `GenLang` enum value (Builtin 0 / Lua 1 / Python 2 / GLSL 3). The optional `:<modeInt>` is the generation mode (`0` per-cell, `1` whole-grid); when absent it defaults to per-cell, so projects saved before the mode field still load. The further-optional `:<passes>` is the GLSL whole-grid ping-pong pass count, written **only when it's `> 1`** (so non-GLSL and single-pass tags are byte-identical to before, and old projects load as `passes = 1`); a bare `<langInt>:<modeInt>:<passes>` is parsed by splitting field 0 on `:`. The program source is base64-encoded so it can contain newlines, `|`, braces, etc. without breaking the pipe-delimited layout. The **4th field** (`<dataField>`) holds the baked grid in one of two forms:

- **Content-store reference (current):** `#<32-hex-hash>` — a reference into the project's content-addressed blob store ([Content store](#content-store) below). The actual bytes live once in the store, keyed by a hash of the grid's canonical `.npy` payload. The leading `#` is unambiguous because it appears in neither the base64 alphabet nor anywhere else in the pipe-delimited layout.
- **Legacy inline blob:** the final bipolar `[-1,1]` floats (`product(dims)` of them), gzip-compressed (level 9) then base64-encoded — bit-exact. Projects written before the content store embedded the blob directly here, and still load.

**Why bake for every language, not just Python?** `TerrainSynthProcessor`'s constructor runs during **graph rebuild on the audio thread**. The embedded Python interpreter is illegal there (single GIL-held interpreter, message-thread only), so a Python generator *cannot* re-run on load — it must be baked. Rather than special-case Python, **all** languages bake: this also removes a latent glitch where a large Builtin/Lua grid would re-run its per-cell loop on the audio thread during every load/undo and stall audio. Generation always happens exactly once, on the message thread, at Generate time. (The one-time cost is unavoidable regardless — the data has to be computed once either way.)

**Backward compatibility.** Old projects whose tag has **no 4th field** still load: for Builtin/Lua the constructor falls back to regenerating from the program (safe, deterministic); a Python **or GLSL** node with no baked data can't regenerate on the audio thread (CPython and the GL context are both message-thread-only), so it loads as a flat grid until re-generated via Edit Source. Inline-blob (legacy) and `#hash` (current) 4th fields both decode. New saves always write the `#hash` form.

<a name="content-store"></a>
#### Content store (content-addressed blob side-store)

Large immutable baked payloads — currently generated-terrain grids — live in a per-project **content store** (`content_store.{h,cpp}`, `NodeGraph::contentStore`) rather than inline in each node's script. The motivation is undo memory: `commitSnapshot` serializes the whole graph on every structural edit, and inlining multi-MB blobs into every snapshot string ballooned the in-memory undo tree (see `known-issues.md`). Nodes now carry only a short hash; the bytes are stored once and shared across every undo step that references them.

- **Canonical form (what's hashed):** a NumPy `.npy` payload — `descr='<f4'`, C-order, the node's shape tuple, then the raw little-endian float32 cells. This is deterministic and compression-independent, so the same grid always hashes the same regardless of how it's stored. The hash is a 128-bit content hash (two decorrelated FNV-1a lanes + a splitmix64 avalanche), rendered as 32 lowercase hex chars. Not cryptographic — it's for addressing/dedup; 128 bits is birthday-safe far past any realistic blob count.
- **Stored form (what's kept in RAM/on disk):** the `.npy` bytes passed through a **4-byte-plane shuffle** (groups the four bytes of each float32 into separate planes so DEFLATE sees long low-entropy runs) then **DEFLATE** (JUCE gzip, level 9). Pure permutation + lossless compression, so round-trips are bit-exact.
- **Dedup:** identical grids hash identically and are stored once, no matter how many nodes or undo snapshots reference them.
- **Undo exclusion:** `serializeForUndo` writes snapshots with `includeBlobs=false` — the `#hash` travels in the snapshot, the bytes never do, and they persist in the live in-memory store across undo/redo. A **real save** (`writeProject`, `includeBlobs=true`) emits the referenced blobs as `[Blob]` sections (`hash=…` + base64 `bytes=…`) just before `[End]`; `readProject` loads them back into a fresh graph's store. Only blobs actually referenced by a node are written, so orphaned entries are dropped at save time.

The generated terrain classifies by rank: 1D → **Sample** source, 2D+ → **Surface** source (default Synth Mode AM-sine), matching the audio/image/video sources.

**Editing.** Right-click a generated terrain node → **Edit Source…** re-opens the dialog seeded from the node's script. In edit mode the **axis count is locked** (changing the number of axes would rewire the node's Sig pins and Center/Radius params and drop cables) — you can still change dimension **sizes**, the language, the **mode**, and the program. To use a different rank, create a new generated terrain.

**Export grid as.** Right-click a generated terrain node → **Export grid as ▸** offers up to three formats, depending on the grid's rank:

- **NumPy `.npz` (any rank, full precision).** Writes the baked grid to a NumPy **`.npz`** file (a ZIP archive containing one `.npy` member named `terrain`, float32, C-order, at the node's full rank/shape). This is exactly the container `numpy.savez` produces (members are STORED/uncompressed), so it loads straight into the scientific-Python stack: `np.load("name.npz")["terrain"]` returns the array with its original shape. The grid is the final bipolar `[-1, 1]` data, bit-exact with what Generate produced — the same canonical `.npy` payload the [content store](#content-store) hashes (`ContentStore::makeNpy` / `makeNpz` in `content_store.cpp`). Use it to take a terrain you sculpted in SEANCE into NumPy/SciPy/Matplotlib, or any tool that reads `.npz`. Always offered.
- **WAV (1D waveform).** Offered only when the grid is **1D**. Treats the row as a mono waveform and writes a 24-bit PCM `.wav` via the same `AudioExporter` used for project export. The sample rate is the project sample rate (or 44100 Hz when the project is set to follow the audio device). The float data is bipolar `[-1, 1]`, so it maps directly to full-scale audio — open it in any audio editor or sampler.
- **PNG (2D grayscale image).** Offered only when the grid is **2D**. Maps each cell's float `[-1, 1]` value to 8-bit grayscale `[0, 255]` (`v*0.5+0.5` then ×255) and writes an RGB `.png` (`dims[0]` = height, `dims[1]` = width, row-major). Note this is **lossy** — 8-bit quantises the float grid; use `.npz` if you need the exact values. Handy for previewing a heightmap or pulling it into an image editor.

All three appear only on generated (`__generate__`) terrains, and the menu shows a warning if there's no baked grid (a Python node that was loaded without baked data — re-run Generate first). The WAV/PNG items are hidden when the rank doesn't match (rather than greyed out), since they're meaningless for the wrong rank; `.npz` is the universal fallback.

### Traversal modes

Seven built-in modes:

- **Linear** — sweeps a single axis at constant speed. The default for 1D waveform (= standard wavetable playback) and 1D audio file (= sample player).
- **Orbit** — circles a center point in 2D-or-higher; radius and speed are knobs. Periodically revisits each region.
- **Lissajous** — two-axis independent oscillators with different rates; classic figure-8/lemniscate patterns drawn through the terrain. Complex periodic timbres.
- **Path** — user-defined polyline through 2D-or-higher space. Click points in the visualizer (Click Points draw mode) or drag (Freehand). Playback modes: **Loop** (jump back at end) or **Bounce** (ping-pong).
- **Physics** — particle-with-forces traversal.
- **Static** — no automatic motion. The playback point is held at **Center X/Y**, so the circling oscillators (Radius, Speed, Rad Mod) are bypassed entirely. This is the "turn the circling oscillators off" option: pick Static when you'd rather place the point yourself — set it with the Center sliders, or drive **Center X**/**Center Y** with a signal cable (LFO, XY pad, envelope, automation, or another node) by right-clicking those rows to add a modulation input (see [Control inputs on parameters](#control-inputs-on-parameters-set-vs-mod)). With no modulation the timbre is frozen at one spot, like a single-cycle wavetable position.
- **Custom** — user-defined math expression.

The **Traversal** parameter row is a discrete picker: click it to choose a mode from a popup (it does not drag-scrub between values). The terrain visualizer (the colored grid in the synth editor) shows the current 2D slice with the traversal path overlaid.

### Math expression grammar (Formula traversals and terrain)

Variables available in expressions: `x`, `y`, `z`, `w`, `v`, `u`, `s`, `t`. Their meaning depends on context — `t` is typically time, `x..z` are spatial coordinates, etc. Standard math operators and the usual set of functions (`sin`, `cos`, `exp`, `log`, `sqrt`, `pow`, `abs`, `min`, `max`, `^` for power, …) are supported.

### Voice allocation

Note-on allocates a voice and starts a traversal. The note's pitch sets the traversal advance rate (higher notes traverse faster). Velocity scales amplitude through the *Vel Sens* parameter (0..1). Chords run independent traversals at different speeds, summed.

### Position parameters

Each *traversable* axis of a wavetable terrain becomes a **Position** parameter on the synth (a grid axis with only one cell, or a single normalized scatter frame, is not traversable and gets no param — see [N-dimensional grids](#n-dimensional-grids) and [Scatter mode](#scatter-mode)). Position params can be:

- Set with the slider
- Automated from a piano-roll automation lane
- Driven by a Param cable (block-rate, ~700 Hz)
- Driven by a Signal cable (audio-rate, per-sample) — for FM-style timbral effects

Every traversable Position axis automatically gets its own block-rate modulation input pin labelled with its axis letter — **`Mod: Position X`**, **`Mod: Position Y`**, **`Mod: Position Z`**, **`Mod: Position W`** (and `Mod: Position 5`, `6`, … past the four named axes). Even a single-axis terrain reads `Mod: Position X` so the pin always says which axis it drives rather than leaving a bare `Mod: Position`. Adding an axis (`+ Dim`) or growing a 1-wide axis to 2+ creates the matching pin; removing an axis (or shrinking it back to a single cell) deletes the pin and any cable plugged into it. These are the same on-demand control pins you can add to any parameter (see [Control inputs on parameters](#control-inputs-on-parameters-set-vs-mod)), just kept in sync with the axis count for you — wire a slow LFO or envelope into `Mod: Position X` to morph timbre over time without touching the piano-roll automation lane. They default to **Mod** (modulate around the slider's resting position); right-click the pin and **Switch to Absolute** to make a cable drive the axis edge-to-edge instead (`Set: Position X`), which locks the slider and side-steps the resting-value-drift caveat below entirely. The pins are created when the wavetable editor is opened, so older projects gain them (and get relabelled with the axis letter) automatically on next edit.

When the axis count changes, each surviving Position param keeps its **resting value** (the slider setting), not the momentary modulated reading. This matters when a Position is being signal-driven while the grid is edited: `syncPositionParams()` rebuilds the param list, and for a modulated param it carries over `baseValue` (the resting setting) rather than the live `value` (that block's modulated reading). The rebuilt param is left un-modulated, so `applySignalModulations` re-snapshots its `value` as the new base on the next block — feeding it the resting value keeps that base stable. Capturing the modulated value instead (the old bug) let the base drift off-centre every time the grid changed: because the modulation model is `base + (signal − 0.5)`, a base below 0.5 can no longer sweep the axis across its full 0..1, so e.g. growing a wavetable to 3D while a control fader held a Position low would leave that axis stuck — you'd hear both cells blended at the fader extreme instead of the far cell alone. Preserving the resting value keeps the full morph sweep intact across `+ Dim`/grid-resize edits.

Note that this only prevents *new* drift. A wavetable whose Position base was already corrupted (built with the buggy code, in-memory) won't self-heal — reset it by rebuilding the grid, or by momentarily disconnecting the `Mod: Position` cable and dragging the Position slider back to centre.

### MIDI controller behavior

Same as the [MIDI input](#midi-input-and-routing) section's "Built-in controller responses" table. Sustain pedal, mod-wheel vibrato (6 Hz default), pitch bend (±2 semitones), velocity sensitivity, and the per-voice [Pressure input pin](#pressure-input-pin) all apply.

---

## Pitch Shift node

Changes the pitch of an audio signal **without changing its duration**. Two
params, both on the node face:

| Param | Range | Meaning |
|---|---|---|
| **Pitch (semi)** | −24 … +24 | How far to transpose, in semitones. 12 = up an octave, −12 = down an octave, 0 = unity. |
| **Formant** | 0 / 1 | Off: the whole spectrum scales, so a voice pitched up sounds like a chipmunk. On: the original **spectral envelope** is restored after shifting, so the pitch moves but the timbre stays put — the same voice, singing higher. |

### How it works

A **phase vocoder** (`PhaseVocoderShifter`, `cpp/src/pitch_core.h`), not
stretch-then-resample. A 2048-point FFT at 75% overlap; each frame's spectral
peaks are found and every peak is translated *together with its whole mainlobe*,
with the surrounding bins' phases locked to the peak's. Moving whole peaks
rather than individual bins is what keeps the level right — mapping each bin
independently tears mainlobes apart and costs about 4.5 dB on an octave shift.

A peak's *synthesis* frequency comes from its true frequency (recovered from the
phase advance between frames) times the ratio, not from its bin index, so
resolution is not limited to whole bins: a one-semitone shift really is one
semitone. Measured accuracy is under 8 cents across ±12 semitones.

**Formant** preservation uses cepstral liftering: the log-magnitude spectrum is
transformed to the cepstrum, only the low-quefrency part is kept (1.2 ms, which
separates the slowly-varying vocal-tract envelope from the harmonic comb), and
transformed back to give a smooth envelope. After shifting, bin *k* carries what
came from bin *k*/ratio, so multiplying by env(*k*)/env(*k*/ratio) puts the
original envelope back. The correction is clamped to ±20 dB, because in deep
spectral valleys the raw ratio explodes and would amplify numerical noise into
an audible artefact. It costs two extra FFTs per frame, so it is skipped
entirely when the switch is off *and* when the pitch sits at unity.

### Latency

The node reports **1536 samples** of latency (32 ms at 48 kHz) — the FFT size
minus one hop. The graph's plugin delay compensation uses that to keep the node
aligned against every other path, so you never compensate by hand.

The node deliberately does **not** short-circuit at 0 semitones. A fixed-latency
node that early-outs would jump forward by its full latency the instant the knob
crossed zero: an audible click, and a phase discontinuity against everything the
graph has already delay-compensated.

### What happened to Time Ratio

Earlier versions of this node wrapped the **Rubber Band** library and exposed a
third param, **Time Ratio**, for time stretching. Both are gone.

Rubber Band is GPL v2 and was statically linked, which made the whole binary
GPL-encumbered — incompatible with shipping SEANCE as a commercial plugin.
Before removing it all three params were measured, and only Pitch worked:

- **Time Ratio** could not work, and the reason is structural rather than a bug:
  a `processBlock` must emit exactly as many samples as it is handed, so a
  change of duration has nowhere to go. At ratio 2.0 the surplus backed up
  inside the stretcher indefinitely (latency growing half a block per block); at
  ratio 0.5 it starved and zero-filled, leaving **50% of a continuous tone as
  exact silence** while the tempo stayed unchanged.
- **Formant** was never read at all — the stretcher was constructed with formant
  preservation permanently on, so the knob rendered bit-identical output in
  either position.

Formant preservation was therefore rebuilt into the in-house core, so nothing
was lost and the knob now genuinely works. Time Ratio was removed outright. **A
real time-stretch feature has to be offline/clip-based** — rendering a clip to a
new length — rather than a live effect node.

Projects saved with the old three-param node load fine: params are read by name,
and the obsolete `Time Ratio` entry is stripped on load so it cannot linger as a
dead knob.

---

## Effect layers and groups

Lets you mark a wire as **active only during certain beat ranges** — the routing itself becomes time-gated rather than always-on. Implemented in `effect_regions.h`, `time_gate_processor.cpp`, and surfaced in the piano roll as colored bars above the notes.

### Layers (per-wire regions)

A **layer** is one time range during which one specific cable is active. Stored on `Node::effectRegions`. Outside the region the wire is muted; at the region edges the wire crossfades to/from silence to prevent clicks.

Create one via right-click on a wire → **Time-gate…**, then drag in the piano roll layer bar area to set start/end. The region is tied to that specific wire only.

Each layer is colored to match its wire (or its effect group) so you can read routing at a glance.

### Effect groups

A named bag of wires that activate together. Built via right-click → **Effect Group → New Group…** then right-click → **Effect Group → Add to [group name]** on additional wires.

Once layered, a group's gate opens/closes all member wires simultaneously. Member-wire visual indicators: a **circle** for individual-wire layers, a **diamond** for group membership.

### Crossfade duration

Global default is **50 ms** (`NodeGraph::globalCrossfadeSec = 0.05f` in `node_graph.h:624`), set via **Options → Crossfade Duration**.

Per-group override: `EffectGroup::crossfadeSec`. Zero (the default) means "inherit the global value"; any positive value overrides. Stored in the project file only when non-zero (`project_file.cpp:92-93`).

### Routing strip

Above the piano roll, a narrow strip appears when any layers exist. Shows each gated wire as a horizontal "wire" bar with 3D shading, colored to match the wire/group. The horizontal axis matches the piano roll below — read across to see which wires are active at which beats. The strip auto-hides when there are no layers.

---

## Wavelet effects

Twelve `Effect` nodes built on the **discrete wavelet transform (DWT)** rather than the FFT. All of them live in right-click → **Effects**, in one run between *Ring Modulator* and *SMS*.

**Why wavelets, in plain language.** An FFT chops audio into fixed-length windows and asks "how much of each frequency is in this window?". That is a fair question for a sustained note and a bad one for a drum hit — a window long enough to resolve bass is long enough to smear the attack. A wavelet transform asks a *scale*-shaped question instead: it looks at low frequencies with long windows and high frequencies with short ones, automatically. The practical consequence is that a wavelet effect can be surgical about the sustained part of a sound while leaving transients alone (or vice versa), which is what most of the nodes below trade on.

Contents:

- [Shared behaviour](#shared-behaviour-all-wavelet-effects)
- [Transient/Sustain Split](#transientsustain-split)
- [Wavelet Denoiser](#wavelet-denoiser)
- [Wavelet Bitcrush](#wavelet-bitcrush)
- [Octave Shift (wavelet)](#octave-shift-wavelet)
- [Wavelet Multiband Comp](#wavelet-multiband-comp)
- [Wavelet Reverb (1/f)](#wavelet-reverb-1f)
- [Independent Pitch Shift](#independent-pitch-shift)
- [Wavelet Complexity](#wavelet-complexity)
- [Asymmetric Filter](#asymmetric-filter)
- [Wavelet Vocoder](#wavelet-vocoder)
- [Formant Pitch Shift](#formant-pitch-shift)
- [Wavelet Pitch Tracker (legacy)](#wavelet-pitch-tracker-legacy)
- [Neutral settings at a glance](#neutral-settings-at-a-glance)

### Shared behaviour (all wavelet effects)

Every node in this family has the same skeleton, so learning it once covers all twelve:

1. Take the current audio block, zero-pad it to the next power of two.
2. Forward DWT to **Levels** depth.
3. Do something to the coefficients — that "something" is the whole difference between the nodes.
4. Inverse DWT.
5. Crossfade against the untouched dry copy using **Mix**.

Consequences worth knowing:

- **Processing is per-block and stereo is per-channel.** Left and right are transformed independently (at most two channels are touched). Nothing except the Reverb carries wavelet state across block boundaries, so any threshold that is expressed as "a fraction of the largest coefficient" is measured **against the current block only**. That makes those thresholds automatically program-adaptive — they follow the material without a knob — but it also means a very aggressive setting can produce a seam at block boundaries.
- **`Levels` is the decomposition depth**, and it is what turns the effect from broadband to surgical. With `Levels = L` at sample rate `SR`, detail **band `b`** (band 0 = coarsest / lowest, band `L-1` = finest / highest) covers roughly `SR/2^(L-b+1) … SR/2^(L-b)` Hz, and the leftover **approximation band** holds everything below `SR/2^(L+1)`. At `Levels = 4`, 48 kHz: approximation ≈ 0–1.5 kHz, band 0 ≈ 1.5–3 kHz, band 1 ≈ 3–6 kHz, band 2 ≈ 6–12 kHz, band 3 ≈ 12–24 kHz. Raising Levels pushes the split points down and gives finer control in the bass at the cost of more transform work. Levels is capped by the block length — a short block cannot support eight levels, so the node silently uses as many as fit (`actualLevels`).
- **`Mix` is a plain dry/wet crossfade** on the node's output; `Mix = 0` bypasses the wavelet path entirely.
- **Wavelet family per node.** SEANCE ships four filter banks (`db1`/Haar, `db2`, `db4`, `sym4`); each effect picks the one that suits its job and doesn't expose the choice as a knob (the *Wavelet Space* waveform editor does — see the README). **`db4`** is the default across the family: 8-tap Daubechies, smooth enough that reconstruction artifacts stay inaudible on tonal material. **Bitcrush uses `db2`** (4-tap) because a shorter filter localises the quantisation grit tightly in time instead of spreading it. **Denoiser uses `sym4`**, a symmetric Symlet with near-linear phase, which matters when you are shrinking coefficients and don't want the surviving ones to shift.
- **Neutral settings are guaranteed, and tested.** Every effect below with a neutral parameter position is covered by a self-test (`wavelet-fx:` in `--self-test`) asserting that at that setting the output is bit-for-bit the input to within `1e-4`, **at full wet**, so the full forward + inverse transform still runs. This is a stronger claim than "Mix = 0 is clean": it proves the round-trip is lossless. (It was added because it wasn't — an earlier inverse transform dropped over half the signal's energy, which made every "bypass" setting a heavy colouration. See the note in `known-issues.md`.) Each of those tests is paired with a check that moving one knob off neutral *does* change the output, so the guarantee can't be satisfied by an effect that does nothing.
- **Latency.** All of these are zero-latency except **Independent Pitch Shift** and **Formant Pitch Shift**, which run a phase vocoder (2048-point FFT, 75% overlap → **1536 samples ≈ 32 ms** at 48 kHz). Both report that to the graph's plugin-delay compensation, so parallel branches stay aligned. Both also deliberately keep running at 0 semitones rather than bypassing, because a node that reports fixed latency has to honour it — bypassing at zero would make the signal jump 32 ms forward the moment the knob crossed zero.

---

### Transient/Sustain Split

`__transientsplit__`. Separates the **attack** of a sound from its **body** and lets you rebalance them. Large wavelet coefficients are short-lived spikes — the pick, the stick, the consonant; small ones are the sustained tone underneath. The node sorts every coefficient into one of two complementary streams by magnitude, reconstructs each separately, and sums them back with independent gains.

Turn **Sustain** down to get a dry, percussive version of anything (drums lose their room, a guitar loses its ring). Turn **Transient** down for the opposite — the pad hiding inside a plucked instrument. Turn **Transient** *up* past 1 to re-add attack to a sound that a compressor flattened.

| Param | Default | Range | Meaning |
|---|---|---|---|
| **Transient** | 1.0 | 0 – 2 | Gain on the transient stream. 1 = untouched, 0 = removed, 2 = +6 dB. |
| **Sustain** | 1.0 | 0 – 2 | Gain on the sustain stream, same scale. |
| **Threshold** | 0.3 | 0 – 1 | Where the split happens, as a fraction of the block's largest coefficient. Lower = more material counts as transient. |
| **Levels** | 4 | 1 – 8 | DWT depth. |

**Neutral:** Transient = 1, Sustain = 1. The two streams are complementary by construction, so summing them at unity must give the input back regardless of Threshold. This node has no Mix param — the two gains already span dry.

Wavelet family: `db4`.

---

### Wavelet Denoiser

`__denoiser__`. Broadband noise reduction by **wavelet shrinkage** (the classic Donoho–Johnstone method): forward DWT, pull every detail coefficient toward zero by a fixed amount, inverse DWT. Noise is spread thinly across all coefficients, so it falls below the threshold and vanishes; real signal is concentrated in a few large coefficients, which survive (reduced by the threshold, not zeroed — "soft" shrinkage, which avoids the musical-noise chirping that hard gating produces).

The advantage over spectral gating is that transients keep their edge. A gate has to pick a window length and smears anything shorter than it; shrinkage has no window to smear across.

The approximation band is never thresholded — the lowest band is left alone so the fundamental of a bass note can't be shrunk away.

| Param | Default | Range | Meaning |
|---|---|---|---|
| **Threshold** | 0.1 | 0 – 1 | Noise floor estimate, as a fraction of the block's largest coefficient. Raise until the hiss goes, then back off — too high and sustained tones start to sound gated. |
| **Levels** | 4 | 1 – 8 | DWT depth. |
| **Mix** | 1.0 | 0 – 1 | Dry/wet. Blending some dry back in is a good way to soften over-aggressive settings. |

**Neutral:** Threshold = 0 (nothing falls below zero, so nothing is shrunk).

Wavelet family: `sym4` — symmetric and near-linear-phase, so the coefficients that survive shrinkage stay where they were in time.

---

### Wavelet Bitcrush

`__waveletbitcrush__`. Bit-reduction that only hits **the frequencies you choose**. A conventional bitcrusher quantises the waveform, so the grit lands everywhere at once. This one quantises wavelet coefficients within a selected band range, so you can have crunchy lo-fi bass under clean highs, sizzle on top of a clean low end, or the full 8-bit treatment.

| Param | Default | Range | Meaning |
|---|---|---|---|
| **Bits** | 4 | 1 – 16 | Quantisation resolution: step size is `1 / 2^Bits`. 16 is effectively transparent; 1–3 is destructive. |
| **Band Lo** | 0 | 0 – 7 | Lowest detail band to crush. Band 0 is the coarsest (lowest-frequency) detail band; see [Shared behaviour](#shared-behaviour-all-wavelet-effects) for the Hz mapping. |
| **Band Hi** | 7 | 0 – 7 | Highest detail band to crush. Bands above `Levels-1` don't exist and are ignored, so with the default `Levels = 4` only bands 0–3 do anything. |
| **Levels** | 4 | 1 – 8 | DWT depth — also how many bands there are to select between. |
| **Mix** | 1.0 | 0 – 1 | Dry/wet. |

The **approximation band is never crushed**, only detail bands, so the very bottom of the spectrum stays clean no matter what Band Lo is set to.

**Neutral:** Bits = 16 — a step of 1/65536, which every coefficient survives to within about 8·10⁻⁶.

Wavelet family: `db2` — a short 4-tap filter keeps each quantisation error localised in time, which is what makes the artifact read as *grit* rather than as a smeared buzz.

---

### Octave Shift (wavelet)

`__octaveshift__`. Shifts pitch by whole octaves by moving energy between wavelet bands. Because octave bands *are* the DWT's natural decomposition levels, no resampling or time-stretching is involved — there is no window to smear, so transients keep their timing and their edge.

This is the node for sub-bass generation (Shift = −1 blended under the dry signal) and for cheap octave-doubling, not for musical intervals — for anything that isn't a whole octave use [Independent Pitch Shift](#independent-pitch-shift) or the [Pitch Shift node](#pitch-shift-node).

| Param | Default | Range | Meaning |
|---|---|---|---|
| **Shift** | −1 | −2 … +2 | Octaves. 0 = off, −1 = one octave down (sub), +2 = two octaves up. |
| **Mix** | 0.5 | 0 – 1 | Dry/wet. The default of 0.5 is deliberate: a sub-octave is normally wanted *underneath* the original, not instead of it. |

**Neutral:** Shift = 0. Note that this is an **early-out** — the node returns before the transform runs, so at Shift = 0 the Mix knob does nothing and the audio is untouched by construction rather than by round-trip.

Implementation caveat: the shift is done by **reassigning each band's coefficients to a neighbouring band** and reconstructing, which is an approximation to a true octave shift rather than an exact resampling. Bands that fall off either end are dropped, and the copy truncates when the source and destination band lengths differ. In practice it sounds like a clean, slightly hollow octave — good for sub reinforcement, characterful rather than transparent for an octave-up lead. A true scale-shift implementation is on the roadmap.

Wavelet family: `db4`. Levels is fixed internally at 6 (not exposed) — enough bands for a two-octave move at any supported block size.

---

### Wavelet Multiband Comp

`__waveletmbcomp__`. Multiband compression where the bands are **DWT decomposition levels** instead of crossover filters. A conventional multiband compressor splits with filters, and those filters have phase responses that don't perfectly cancel when the bands are summed — the classic "phasey" multiband sound. Wavelet levels are perfectly reconstructing by construction, so the bands sum back to the original exactly when no gain is applied.

Also doubles as a **spectral tilt** tool via the two gain knobs, which interpolate a per-band gain ramp from the lowest band to the highest.

| Param | Default | Range | Meaning |
|---|---|---|---|
| **Threshold** | −20 dB | −60 … 0 dB | Level above which a band starts being compressed. Shared across all bands. |
| **Ratio** | 4 | 1 – 20 | Compression ratio. 1 = off, 20 ≈ limiting. Shared across all bands. |
| **Levels** | 4 | 1 – 6 | Number of octave bands. |
| **Low Gain** | 0 dB | −12 … +12 dB | Post-compression trim on the **lowest** band. |
| **High Gain** | 0 dB | −12 … +12 dB | Post-compression trim on the **highest** band. Bands in between get a linear interpolation of the two, so Low −6 / High +6 is a bright tilt. |
| **Mix** | 1.0 | 0 – 1 | Dry/wet — set below 1 for parallel ("New York") compression. |

**Neutral:** Ratio = 1 with both gains at 0 dB. At Ratio = 1 the gain computer is an identity (`dbReduction = dbOver · (1 − 1/1) = 0`) whatever the band peak is.

Behaviour caveat: gain reduction is computed **per band, per block, from that block's peak**, and applied as a single static gain to the whole block. There is no attack or release — so this behaves like a fast per-band leveller rather than a classic compressor with a time constant, and it can't be used for pumping or for shaping attack. Attack/Release params are a planned addition; for envelope-shaped dynamics use the ordinary **Compressor** node (right-click → Effects → Compressor), which has Attack/Release and a sidechain input.

Wavelet family: `db4`.

---

### Wavelet Reverb (1/f)

`__waveletreverb__`. A short, diffuse ambience built by weighting wavelet bands with a `1/f^Color` curve — i.e. a **self-similar / fractal** tail rather than a simulated room. There are no delay lines, no early reflections and no room model: the "space" comes from the statistics of the weighting, which is why it sounds smooth and grainless in a way delay-network reverbs don't.

Best used as a wash / bloom on pads and textures. It is not a room simulator — for that, capture or load an impulse response into the [Convolution Filter](#convolution-filter).

| Param | Default | Range | Meaning |
|---|---|---|---|
| **Decay** | 0.7 | 0 – 1 | How much of the tail survives each buffer shift. 1 = no attenuation, 0 = dry. |
| **Color** | 1.0 | 0 – 3 | Spectral slope of the tail: each band is weighted `1/(band+1)^Color`. **0 = white** (all bands equal, bright and hissy), **1 = pink** (natural, the default), **2 = brown** (dark and rounded), 3 = very dark. |
| **Levels** | 5 | 1 – 8 | DWT depth on the tail buffer — how many bands the colour curve is spread over. |
| **Mix** | 0.3 | 0 – 1 | Dry/wet. Reverb is normally a send-style effect, hence the low default. |

**Tail length is hard-bounded at 8192 samples** (≈ 186 ms at 44.1 kHz, ≈ 171 ms at 48 kHz) regardless of Decay — that's the size of the internal buffer the whole thing operates on, and it's reported to the graph via `getTailLengthSeconds()`. Pressing **Stop** zeroes the tail buffers, so the wash cuts immediately rather than ringing on after the transport stops.

**Neutral:** Decay = 1, Color = 0, Mix = 1. This is the least obvious neutral in the family: with every band weight at 1 and no attenuation on the shift, the samples that come out of the tail buffer are exactly the ones just written into it — and it is still a full 8192-point round-trip, which makes it the most demanding of the transform tests.

**Decay is defined as the attenuation across a full traversal of the tail buffer**, not as a per-block multiplier. The tail is aged once per `processBlock`, so a sample is attenuated `8192 / blockSize` times over its life — 16 times at a 512-sample buffer but 128 times at a 64-sample one. Applying the knob value verbatim each block therefore made the same preset a usable bloom on one audio device and effectively dry on another (at Decay = 0.7, 0.003 vs 1.4 × 10⁻⁶ surviving). The per-block coefficient is instead derived as `Decay^(16 · blockSize / 8192)`, so the total across the buffer is `Decay¹⁶` at every buffer size. The exponent 16 is chosen so that at the common 512-sample buffer the coefficient is exactly `Decay` — projects made before this fix sound unchanged, and every other buffer size now matches them instead of diverging. Guarded by the `wavelet-fx: Reverb decay is independent of the audio buffer size` self-test.

Wavelet family: `db4`.

---

### Independent Pitch Shift

`__indpitchshift__`. Pitch-shifts **only the tonal part** of a sound and leaves the transients at their original pitch and timing. Pitch-shifting a full drum loop normally destroys it — the shifter smears the hits and the whole thing goes soft. This node splits transient from tonal in the wavelet domain first (the same split as [Transient/Sustain Split](#transientsustain-split)), shifts just the tonal stream through a phase vocoder, and recombines, so the drums stay punchy while the melodic content moves.

| Param | Default | Range | Meaning |
|---|---|---|---|
| **Semitones** | 0 | −24 … +24 | Shift amount applied to the tonal stream. |
| **Threshold** | 0.3 | 0 – 1 | Transient/tonal split point, as a fraction of the block's largest coefficient. |
| **Trans Gain** | 1.0 | 0 – 2 | Gain on the (unshifted) transient stream. Push above 1 to keep attacks prominent against a heavily shifted body. |
| **Levels** | 4 | 1 – 8 | DWT depth for the split. |
| **Mix** | 1.0 | 0 – 1 | Dry/wet. |

**Latency: 1536 samples (≈ 32 ms at 48 kHz)**, reported to plugin-delay compensation. Both the transient stream and the dry copy are internally delayed by the same amount, so all three paths stay sample-aligned. There is **no neutral bypass** — at 0 semitones the shifter still runs (measured at −0.003 dB, i.e. transparent) rather than early-outing, because the node has to keep honouring its reported latency.

Wavelet family: `db4`. Shifting is done by a 2048-point / 75%-overlap phase vocoder shared with the [Pitch Shift node](#pitch-shift-node) (`pitch_core.h`) — constant-duration by construction, which an in-block resampler is not.

---

### Wavelet Complexity

`__waveletcomplexity__`. A single "how much detail?" knob. The node keeps only the **N largest** wavelet coefficients and zeroes the rest, so turning it down progressively strips a sound back to its essentials — like a resolution dial for audio. At 100% nothing is discarded; at very low settings only a handful of coefficients survive and the sound reduces to a rough sketch of itself.

Useful for lo-fi textures, for gradually dissolving a sound into an ambience, and (automated) for build-ups where a part gains detail as it arrives.

| Param | Default | Range | Meaning |
|---|---|---|---|
| **Complexity** | 0.5 | 0 – 1 | Fraction of coefficients kept. 1 = keep everything, 0 = keep just one. |
| **Levels** | 4 | 1 – 8 | DWT depth. |
| **Mix** | 1.0 | 0 – 1 | Dry/wet. |

**Neutral:** Complexity = 1.0 (the keep-set is the entire coefficient set).

Wavelet family: `db4`.

---

### Asymmetric Filter

`__asymfilter__`. A filter that reacts **before** a transient happens. Every causal filter — every analogue filter, every conventional plugin — can only respond after the fact, because it can't know what's coming. Working in the wavelet domain on a whole block at once removes that constraint: the node locates transients first, then applies a gain envelope that is *shaped differently ahead of each onset than behind it*. The result is a swell into an attack or a duck ahead of it, which no causal processor can do.

| Param | Default | Range | Meaning |
|---|---|---|---|
| **Pre-Attack** | 20 ms | 0 – 100 ms | How far ahead of each detected onset the pre-region starts. |
| **Post-Decay** | 50 ms | 0 – 200 ms | How far behind each onset the post-region extends. |
| **Pre Gain** | 2.0 | 0 – 4 | Gain reached immediately before the onset. > 1 swells into the hit; < 1 ducks ahead of it. |
| **Post Gain** | 0.5 | 0 – 2 | Gain immediately after the onset, recovering to 1 over Post-Decay. < 1 tightens/gates the tail. |
| **Sensitivity** | 0.5 | 0 – 1 | Onset threshold, as a fraction of the block's loudest finest-band coefficient. 0 treats everything as an onset; 1 keeps only the single loudest. |
| **Levels** | 4 | 1 – 8 | DWT depth. |
| **Mix** | 1.0 | 0 – 1 | Dry/wet. |

Onsets are found as peaks in the finest detail band. The threshold is **program-adaptive** — a fraction of that band's own peak within the block, not an absolute level — so the node behaves the same on a quiet take and a loud one without re-dialling. Sensitivity's default of 0.5 is the value that used to be hardcoded, so projects saved before the param existed are unchanged.

**The gain envelope is built along the time axis, in samples, and then resampled onto each band by that band's stride.** This matters more than it sounds. The concatenated coefficient array is *not* a time axis: one step in the approximation band is worth `2^Levels` input samples while one step in the finest detail band is worth 2. An envelope laid out directly across that array would put the "20 ms before the onset" region in a different place, and at a different width, in every band. Each coefficient takes the **mean** of the envelope across the span of time it represents rather than a point sample, because the coarse bands stride up to 256 samples at a time and point sampling would alias a short pre-attack ramp into them — or step straight over it. Guarded by the `wavelet-fx: Asymmetric Filter's pre-attack lands before the onset, not at the start of the block` self-test.

**Neutral:** Pre Gain = 1 and Post Gain = 1. Both ramps collapse to a constant 1 (`1 + (1−1)·frac`), so the envelope stays flat even where transients are detected. Detection still runs — this is not an early-out.

Remaining limitation, logged in `known-issues.md`: the analysis window is **one zero-padded audio block**, so neither time region can be longer than that however the knob is set, and both are clamped to it. At 48 kHz a 512-sample block is ≈ 10.7 ms, so with a small device buffer the top of the Pre-Attack range flattens out — the *shape* keeps changing as the envelope is redistributed, but the region can't extend past the block. Lifting this needs a fixed-size analysis frame with reported latency, decoupled from the device buffer, rather than a bigger clamp.

Wavelet family: `db4`.

---

### Wavelet Vocoder

`__waveletvocoder__`. A vocoder using **wavelet bands instead of fixed FFT bins**. The classic use is making an instrument talk: feed a pad or a saw into the audio input (the *carrier*) and a voice into the Modulator input, and the pad takes on the voice's spectral shape. The wavelet version differs from an FFT vocoder in that its bands are octave-wide and its time resolution scales with frequency, so consonants stay crisp instead of being smeared to the length of an FFT window.

The node has an extra **Modulator** input pin (a Signal-kind input on channel 2) alongside the usual Audio In.

| Param | Default | Range | Meaning |
|---|---|---|---|
| **Bands** | 5 | 1 – 8 | Number of wavelet bands the modulator's envelope is measured over. Fewer = coarser, more robotic; more = more intelligible. |
| **Mix** | 1.0 | 0 – 1 | Dry/wet against the unprocessed carrier. |

Behaviour notes:

- **No modulator connected = clean passthrough.** The node checks for the modulator channel and returns the carrier untouched if it isn't there, so an unwired vocoder is silent-by-omission rather than silent outright.
- **The output is mono.** After processing, channel 0 is copied over channel 1. Place any stereo widening after this node, not before.
- Per-band gain is capped at 10× to stop a near-silent carrier band from exploding.

**Neutral:** this is the one effect in the family with **no neutral knob position** — imposing the modulator's spectral envelope is the entire job, and no parameter setting makes that an identity. Its identity is a property of the *signals* instead: feed the same audio into both the carrier and the modulator and every band's scale factor becomes `sqrt(modE/carE) = 1`, so the carrier comes back untouched. That is how the self-test pins it.

Wavelet family: `db4`.

---

### Formant Pitch Shift

`__formantpitch__`. Pitch shifting **without the chipmunk effect**. Ordinary pitch shifting moves the formants — the fixed resonances of a throat or a body — along with the pitch, which is exactly what makes a shifted voice sound like a different, smaller creature. This node shifts the pitch, then measures the *original* signal's per-band energy profile and re-imposes it on the result, putting the spectral envelope back where it was while leaving the pitch moved.

| Param | Default | Range | Meaning |
|---|---|---|---|
| **Semitones** | 0 | −24 … +24 | Pitch shift amount. |
| **Formant Lock** | 0.8 | 0 – 1 | How much of the original formant envelope is restored. 0 = none (a plain pitch shift, chipmunk included), 1 = full restoration. The default leaves a little of the natural shift in, which usually sounds less processed. Sweeping this knob *on its own* is a gender/size morph. |
| **Levels** | 5 | 1 – 8 | DWT depth — how finely the formant envelope is sampled. |
| **Mix** | 1.0 | 0 – 1 | Dry/wet. |

**Latency: 1536 samples (≈ 32 ms at 48 kHz)**, reported to plugin-delay compensation. The dry copy is internally delayed to match, which matters more here than elsewhere: the dry path is also where the formant envelope is measured, and on speech an un-delayed measurement would be sampling a phoneme from 32 ms earlier. As with Independent Pitch Shift there is **no bypass at 0 semitones** — the node must keep honouring its reported latency.

Wavelet family: `db4`; the shift itself uses the shared phase vocoder from `pitch_core.h`.

---

### Wavelet Pitch Tracker (legacy)

`__pitchtracker__`. Detects the fundamental pitch of incoming audio by finding the wavelet level with the strongest energy, and emits it as a normalised Signal.

> **Superseded — use the [Pitch Detector](#pitch-detector) instead.** This node only resolves pitch to octave-band accuracy, and it has a signal-routing bug: it writes the detected value to channel 0 (the audio bus) instead of channel 2, so the "Pitch Out" pin actually carries silence and the node's signal output is effectively dead. It is kept only so that old projects still load. The Pitch Detector is accurate (true YIN / autocorrelation with parabolic interpolation), correctly wired, and adds algorithm choice, hop control, and log/linear mapping. Both bugs are written up in `known-issues.md`.

| Param | Default | Range | Meaning |
|---|---|---|---|
| **Min Hz** | 50 | 20 – 5000 | Bottom of the detection/output range. |
| **Max Hz** | 2000 | 20 – 5000 | Top of the detection/output range. |
| **Detected Hz** | 0 | read-only | Most recent detection, for display. |

Pins: Audio In, Audio Out, and a **Pitch Out** Signal output.

Wavelet family: `db4`.

---

### Neutral settings at a glance

The parameter position at which each effect is mathematically an identity. Everything in this table is asserted by the `wavelet-fx:` self-tests at **full wet**, paired with a check that moving off it does change the sound.

| Effect | Neutral setting |
|---|---|
| Transient/Sustain Split | Transient = 1, Sustain = 1 |
| Wavelet Denoiser | Threshold = 0 |
| Wavelet Bitcrush | Bits = 16 |
| Octave Shift | Shift = 0 *(early-out — does not exercise the transform)* |
| Wavelet Multiband Comp | Ratio = 1, Low Gain = High Gain = 0 dB |
| Wavelet Reverb | Decay = 1, Color = 0 |
| Wavelet Complexity | Complexity = 1.0 |
| Asymmetric Filter | Pre Gain = 1, Post Gain = 1 |
| Wavelet Vocoder | *(no neutral knob)* modulator signal == carrier signal |
| Independent Pitch Shift | *(none — always runs, reports 32 ms latency)* |
| Formant Pitch Shift | *(none — always runs, reports 32 ms latency)* |
| Every effect with a **Mix** param | Mix = 0 (bypasses the wavelet path entirely) |

---

## Pitch Detector

`__pitchdetector__` (an `Effect` node). Measures the **fundamental pitch** of incoming audio and emits it as a unipolar **Signal** (`0..1`) you can wire into any param's Modulate/Absolute input — pitch-to-cutoff, pitch-to-wavetable-position, auto-tune scaffolding, etc. Add via right-click → **Effects → Pitch Detector (YIN / autocorrelation)**.

It is the precise successor to the legacy **Wavelet Pitch Tracker** (`__pitchtracker__`), which only resolved pitch to octave-band accuracy and had a signal-routing bug (see `known-issues.md`). The old node is kept for backward compatibility; new graphs should use this one.

### Pins

- **Audio In** (channel 0/1) — audio to analyse.
- **Audio Out** (channel 0/1) — the input passes through unchanged, so the node sits inline like a Spectrum Tap; you don't have to branch the signal.
- **Pitch Out** (Signal, channel 2) — the detected pitch normalised to `0..1` across `[Min Hz, Max Hz]`.

### How it works

Incoming mono audio is accumulated into a ring buffer. Every **Hop** samples (or once per audio block when Hop = 0), the most recent **window** of samples is handed to the selected detector. The result is mapped to `0..1` and held on the Signal output until the next detection (so the signal is smooth between hops). If a hop finds no confident pitch, the previous value is held rather than dropping to zero.

The **analysis window is not a user knob** — it's derived automatically from **Min Hz**. Pitch detection needs roughly `kAnalysisPeriods` (= **3**) full periods of the lowest note to lock on, so the window is computed as `ceil(3 · sampleRate / minHz)` and clamped to `[64, kMaxWindow]` (`kMaxWindow = 65536`). This makes Min Hz the single honest control: lowering it lets the node detect deeper notes but proportionally lengthens the window and therefore the latency. (Earlier builds exposed a redundant manual **Window** param; it was removed because the window length is physically dictated by the lowest frequency you ask it to detect — setting both independently let you pick a window that couldn't actually resolve the chosen Min Hz.)

The two detectors live in `pitch_detect.h` and are shared with the self-test:

- **YIN** (default) — cumulative-mean-normalised difference function (de Cheveigné & Kawahara 2002). Robust against octave errors; the best general choice.
- **Autocorrelation** — classic lag-correlation with **first-strong-peak** picking (it deliberately takes the earliest peak reaching 85% of the maximum, so a pure tone reports its fundamental, not a subharmonic) and parabolic interpolation for sub-sample accuracy.

### Params (on-node rows)

- **Algorithm** — `YIN` / `Autocorr`. Click the row to pick from a popup (it's a discrete choice, not a slider).
- **Hop** — re-run interval in samples (default **0** = once per block). Smaller = more responsive, more CPU. The output is always block-rate (the Signal pin is refreshed every block and held between detections); Hop just controls how often the detector is actually re-run.
- **Min Hz** — the lowest note the node can detect. **This also sets the analysis window** (`window = ceil(3·sampleRate / minHz)`) and therefore the latency, so lowering it costs proportionally more delay. It's floored at `3·sampleRate / kMaxWindow` so the derived window still fits the `kMaxWindow = 65536`-sample ring buffer. Together with Max Hz it defines the band that maps to the `0..1` output.
- **Max Hz** — the top of the output band. **Can go well above 20 kHz**: the graph's internal sample rate can far exceed the audio output rate (`NodeGraph::projectSampleRate`, user-selectable up to 192 kHz), so the usable band runs higher than the audio-rate Nyquist would suggest — it's clamped to `0.45·sampleRate`.
- **Mapping** — `Log` (default) / `Linear`, a popup pick. **Logarithmic** spaces the output musically (an octave is the same output distance everywhere — the geometric mean of the band sits at `0.5`); **Linear** spaces by raw Hz (the arithmetic mean sits at `0.5`).
- **Detected Hz** — read-only display of the most recent detection (updated by the audio thread).

### Tests

`testPitchDetect` (`self_test.cpp`) covers YIN + autocorrelation accuracy within 1% across 110–1760 Hz, the `440 Hz → MIDI 69 (A4)` note mapping, the `minHz ≥ maxHz` guard, the log/linear endpoint and mid-point mapping math, and the window-floor behaviour (a short window can't resolve a low note; a large one can).

---

## Convolution Filter

Audio effect that convolves input audio with a stored impulse response (IR). Add via right-click → **Effects → Convolution Filter**; double-click to open the editor.

### Three ways to build an IR

**1. Presets** — pick from the dropdown, tweak sliders, click **Apply Preset** to generate the IR (replaces the editor's current content; hand edits are lost).

| Preset       | Knobs                                   | Ranges                              |
|--------------|-----------------------------------------|-------------------------------------|
| **Lowpass**  | Cutoff, Steepness                       | Cutoff 20–20000 Hz; Steepness 1–200 |
| **Highpass** | Cutoff, Steepness                       | same                                |
| **Bandpass** | Cutoff, Bandwidth                       | same Cutoff range                   |
| **Echo / Delay** | Delay, Feedback, Echoes             | Delay 1–2000 ms; Feedback 0–0.99; Echoes 1–20 |

**2. Drawing by hand** — two modes:

- **Control points** (default) — small number of draggable points; **Catmull-Rom** smoothing between them.
- **Freehand** — sample-by-sample mouse painting with no smoothing. For sharp transients and surgical edits.

A frequency-response preview updates live as you draw.

**3. Loading from a file** — **Load File…** picks a `.wav`, `.aiff`, or `.flac`. The audio becomes the IR.

### Saving to / loading from the library

Two buttons next to the **IR length** slider tie the editor into the project's [asset library](#asset-library-project-stores):

- **Save to Library** — prompts for a name (defaulting to the node's name) and publishes the current IR as a **Convolution IR** asset. It then shows up under **Edit → Asset Library → Convolution IRs**, where it can be renamed, starred, duplicated, or archived like any other asset, and it travels with the project's library on **Export/Import**.
- **Load from Library** — pops up a list of every saved IR; picking one loads it into the editor as an **independent copy** (further edits here never touch the stored asset — same as loading from a `.wav`). If nothing has been saved yet, a note tells you so.

This lets you build up a personal collection of room/cabinet/EQ impulse responses once and reuse them across any Convolution Filter in the project.

### IR length

Maximum **4096 samples**. Longer files are truncated; the **IR Length** slider lets you trim further to reduce CPU (convolution cost is roughly proportional to IR length).

### Latency and delay compensation

The filter picks its algorithm from the IR length, and this affects processing latency:

- **Short IRs (< 1024 samples)** run **direct time-domain** convolution, which writes each output sample in place with **zero added latency**.
- **Long IRs (≥ 1024 samples)** switch to a **partitioned overlap-add FFT** for efficiency. This path buffers one 512-sample partition of input before it can emit the matching output, so it adds a fixed **512 samples** of processing latency (~11.6 ms at 44.1 kHz).

That 512-sample delay is **reported to the graph's plugin-delay compensation** (via `setLatencySamples`), so a long-IR convolution stays time-aligned with any parallel dry path or other branch it's mixed back against — the graph delays the other branches to match. Switching the IR between the short and long regimes (e.g. dragging the **IR Length** slider across 1024) updates the reported latency automatically.

The IR's own **group delay** (its internal build-up before the main peak) is *not* reported or compensated — that delay is part of the intended filtering sound, and time-advancing it would corrupt the effect. Only the artificial block-buffering delay is ever reported.

> **Merging convolutions:** SEANCE can combine two chained convolution nodes into one node whose IR is the pre-convolution of both (`IR_combined = IR₁ ∗ IR₂`) to save CPU. This can also *reduce* total latency when it collapses two long-IR (FFT-path) stages into one — you pay the 512-sample buffering once instead of twice. Note the corner case: merging two short IRs whose combined length crosses 1024 pushes the result onto the FFT path, which *adds* 512 samples that neither original stage had.

### Zoom and grid

- Mouse wheel — horizontal scroll
- Ctrl+wheel — zoom in toward cursor, up to **128×**
- When zoomed far enough that each sample is ≥ 5 pixels wide, the view switches to **sample-stems**: each sample is a vertical line with a dot at its tip, with a faint grid marking sample boundaries. Surgical sample-level editing.

### Room IR capture

**Tools → Capture Room IR…** opens the capture dialog. Plays a sine sweep through speakers, records the room with a microphone, deconvolves to produce an IR, and loads it into a new Convolution Filter node ready to use.

---

## 3D Spatializer (binaural / holophonic)

Places a sound source anywhere in 3D space *around the listener's head* using **HRTF** (Head-Related Transfer Function) processing. This is the "holophonic" / binaural-audio effect — the technique behind binaural recordings where sounds seem to come from above, behind, or beside you. **It only works on headphones**: the left/right ear cues that create the illusion partly cancel when played over speakers.

Add it via right-click → **Effects → 3D Spatializer (binaural / holophonic)** — it's at the bottom of the Effects submenu. The node has one **Audio In** and one stereo **Audio Out**; insert it anywhere in an audio chain (typically just before the Mixer/Output).

### Position parameters

Three knobs on the node define where the source sits relative to the listener:

| Param | Range | Meaning |
|-------|-------|---------|
| **Azimuth** | −180°…+180° | Horizontal angle. `0` = dead ahead, `+90` = hard right, `−90` = hard left, `±180` = directly behind. |
| **Elevation** | −90°…+90° | Vertical angle. `0` = ear level, `+90` = straight up, `−90` = straight down. |
| **Distance** | 0…1 | `0` = close and loud, `1` = far and quiet. Implemented as a level attenuation (gain `1 / (1 + 3·distance)`); it does not add reverb or air-absorption colouring. |

All three are ordinary modulatable params: automate them in a piano-roll automation lane, or drive them with a Param/Signal cable (on-demand modulation pin), to **move the source around the head in real time** — e.g. an LFO on Azimuth orbits the sound, an envelope on Distance makes it swoop in. Param changes are smoothed internally (one-pole, ~0.05/block) so fast modulation doesn't click, and the per-direction HRTF impulse response is crossfaded as the angle changes.

### How the spatialisation works

Per block the effect: (1) sums the input to mono, (2) looks up the binaural impulse-response pair (one per ear) for the current azimuth/elevation, (3) crossfades toward that IR to avoid clicks, (4) convolves the mono signal with each ear's 64-tap IR, and (5) applies the distance gain. The convolution tail is only 64 samples (≈1.5 ms at 44.1 kHz), so the node is effectively memoryless — it is not in the transport-panic reset list because there is no audible tail to cut.

The HRTF itself comes from a **synthetic spherical-head model** (`HRTFTable`, in `hrtf_data.cpp`): Woodworth interaural-time-delay + frequency-dependent head-shadow low-pass + a pinna comb filter, precomputed at 13 azimuths × 14 elevations (182 direction pairs, 64 samples/ear). At runtime it bilinearly interpolates the four nearest grid points; negative azimuths reuse the mirrored positive-azimuth entry with the ears swapped.

### Save / load and undo

The node serialises generically — the `__spatializer3d__` script token plus the three params round-trip through save/load like any other effect. Creating the node is one `commitSnapshot` undo step; param-knob drags commit on release like every other param.

### Known limitation — measured HRTF datasets are not wired up

Loading **measured HRTF datasets** (MIT KEMAR / CIPIC / SOFA) is a README **Roadmap** item, not a shipped feature — and the half-written code for it is **not reachable**. Two loader stubs exist (`Spatializer3DProcessor::loadHrtfDirectory` for `hrtf_az*_el*_L.wav` files, and `HRTFTable::loadFromDirectory` for MIT-KEMAR-style names) but **nothing calls either function** — there is no UI, no menu item, no project-file hook, and no `.sofa` parser at all. The spatializer always uses the synthetic model above. Tracked in `known-issues.md`; until a loader is wired in, treat the 3D Spatializer as synthetic-HRTF-only.

---

## MIDI Modulator

Adapter node that uses Signal sources to modify MIDI events on the way through. Add via right-click → **Effects → MIDI Modulator**. Double-click to open the rule editor.

### Pins

- **MIDI In** (left) — input event stream
- **MIDI Out** (right) — modified event stream
- **N × Signal In** (left, dynamically allocated) — one per rule

### Rules

Each rule corresponds to one Signal input pin. A rule has:

- **Target** — which MIDI attribute this signal modulates:
  - **Velocity** — scales the velocity of every note-on. Sample-accurate at the moment of the event.
  - **Pitch Bend** — sets the channel pitch-bend value continuously. Replaces any incoming pitch bend.
  - **Mod Wheel** — sends CC#1 at the signal's current value.
  - **Channel Pressure** — sends channel-aftertouch (`channelPressureChange`) events. Emitted as **channel pressure**, i.e. one value applied to every note held on the channel — deliberately *not* polyphonic key pressure. A Signal cable carries a single scalar per sample, which maps one-to-one onto channel pressure; per-note pressure is inherently impossible to carry on a mono signal (which note would the value belong to when several are held?), so it isn't attempted here. True per-note pressure lives inside the synth voice (`MpeVoiceState.pressure`, keyed per voice by MPE channel), not on a graph cable. *(Labeled "Channel Pressure" in the UI; older builds called this "Aftertouch". The serialized `target` is an enum index, so existing projects load unchanged.)*
  - **CC#** — sends an arbitrary CC number, specified in the box next to the target combo.
- **Amount** — strength; 1.0 = full, 0.5 = half, 0 = disabled, negative = inverted. Doubles as a polarity switch.
- **X** — deletes the rule and removes its Signal input pin.

`+ Add Input` adds a new rule and a corresponding Signal input pin. The node's pin layout updates immediately.

### Signal mapping

Inputs are the standard `0..1` control signal (see [Control signal range](#control-signal-range-01)). The **one-directional** targets — Mod Wheel, Channel Pressure, CC# — map straight through (`0 → 0`, `1 → 127`, scaled by *Amount*). The **two-directional** targets — Pitch Bend and Velocity — treat `0.5` as the neutral centre via `toBipolar(sig)`: `0.5` = no bend / unchanged velocity, `1` = `+Amount`, `0` = `−Amount`. So an LFO resting at `0.5`, or a not-yet-fired Signal Shape envelope, leaves pitch and velocity untouched until something moves the signal off centre.

### Update cadence

- Continuous targets (Pitch Bend, Mod Wheel, Channel Pressure, CC) emit an event each audio block when the signal changes.
- **Velocity** is sampled at the exact sample-index of each incoming note-on, then multiplied into that note's velocity.

### Rule composition

Multiple rules targeting the same MIDI attribute **add together** — useful for layering modulation sources (slow LFO + per-note envelope both feeding mod wheel, say).

### Interaction with MIDI Learn

CCs emitted by the MIDI Modulator are **not** subject to the MIDI-Learn filter (which only strips learned CCs from incoming cable traffic). Modulator-generated CCs always reach the downstream synth.

---

## Trigger Node

Adapter node that sits on a MIDI cable and fires additional events (MIDI and/or Signal) when notes pass through. The original note always passes through unchanged — Trigger is a *generator*, not a filter; to suppress notes use a different node.

Add via right-click → **Effects → Trigger**. Pins: **MIDI In**, **Audio In** (optional — only needed for AudioThreshold rules), **MIDI Out**, **Signal Out**. Double-click to open the rule editor. Rules are stored as a serialized blob in `node.script` and decoded into a `TriggerDoc` (`trigger_node.h`).

### Rule structure

Every rule — MIDI or Signal — has a **match** section, a **firing condition**, and a **target action**.

**Match** (which incoming events activate this rule):

- **`minPitch` / `maxPitch`** — pitch range, 0..127. Default 0..127 = all pitches.
- **`minVel` / `maxVel`** — velocity range, 1..127. Default 1..127 = all velocities.
- **`probability`** — 0..1, stochastic gate. The rule rolls a per-fire die against this value (random engine seeded at construction); 1.0 = always fire.

**Firing condition** (`TriggerEvent`):

- **NoteOn** (default) — fires when a matching note-on arrives
- **NoteOff** — fires on the matching note-off
- **AudioThreshold** — fires when audio on the **Audio In** pin crosses `thresholdDb` (default −20 dBFS), with a minimum re-trigger gap of `retriggerMs` (default 100 ms). The processor scans channel 0 of the node's working buffer (`trigger_node.cpp:431`), which the graph processor populates from whatever's wired to Audio In. Classic side-chain use: wire a kick-drum bus into Audio In and have the rule fire a synth note on each transient.

**Target** (`TriggerTarget`):

- **Midi** — emit a new note on MIDI Out
- **Signal** — schedule a signal shape on Signal Out

### MIDI target action

- **`pitchOffset`** — semitones relative to the incoming note. Default +12.
- **`velocityDelta`** — *added* to the incoming velocity (not a multiplier). Default 0.
- **`delayBeats`** — beats to wait before firing. Default 0. Delays scale with transport BPM.
- **`lengthBeats`** — duration of the generated note. Default 0.25 (a sixteenth).
- **`outChannel`** — MIDI channel 1..16. Default 1.

### Signal target action

Shape is one of (`TriggerShape`):

- **Step** — instant jump to `peakValue`, hold for `holdMs`, drop to `restValue`.
- **Envelope** — classic ADSR: `attackMs` → `peakValue`, `decayMs` → `sustainLevel × peakValue`, hold while the source note is held, then `releaseMs` to `restValue`.
- **Ramp** — linear slew from the current output to `peakValue` over `rampDurationMs`, then return.
- **FromVelocity** — output = `velocityScale × (incomingVelocity / 127) + velocityOffset`, held for the rule's duration. Used for "play harder = modulate further."
- **Curve** — user-defined breakpoint list (`std::vector<CurvePoint>` of `{timeMs, value}` pairs). The curve plays from first to last point when the rule fires, then holds the last value. Linear interpolation between points. An empty list falls through to Envelope behaviour. **The Curve shape exists in the source but is not currently documented in the HTML help.**

Editor labels (Min / Max, etc.) map onto these underlying fields — typically Min ↔ `restValue` and Max ↔ `peakValue`.

### Overlap semantics

When multiple signal shapes from the same Trigger overlap on the Signal output, **the most recent shape wins** (replace semantics — `ActiveShape` in `trigger_node.h`). A snapshot of the rule's params is taken at trigger time, so later edits to the rule don't retroactively change a running shape.

### Tail length

The `TriggerProcessor::getTailLengthSeconds()` is computed from the rule list rather than a magic constant — it reflects the longest possible time any rule can keep producing output (delayed MIDI events or running signal shapes) after the last input note. This is what JUCE uses to keep audio rendering after MIDI stops.

### Presets

Five buttons replace the current rule list with a common starting configuration (factory methods on `TriggerDoc`):

- **+ Octave** (`presetOctaveDouble`) — one MIDI rule, +12 semitones, length 0.25 beats
- **Chord** (`presetChordMajor`) — **two** MIDI rules at +4 and +7 semitones (major third and perfect fifth). Combined with the pass-through original note this forms a major triad. Change +4 → +3 for a minor triad.
- **Flam** (`presetFlam`) — **two** MIDI ghost copies at 1/32 and 2/32 beats after the hit, with `velocityDelta` of −30 and −60 respectively, length 0.125 beats
- **Pluck** (`presetPluckEnvelope`) — one Signal envelope rule with `attackMs=2`, `decayMs=200`, `sustainLevel=0`, `releaseMs=0`, `peakValue=1`, `restValue=0`
- **Velocity follower** (`presetVelocityFollower`) — one Signal `FromVelocity` rule, `velocityScale=1`, `velocityOffset=0`, `holdMs=1000`

### Note traffic

Stacking rules multiplies note traffic: a chord rule sends 3+ notes per incoming note; chaining triggers compounds further. Watch downstream synth voice counts.

---

## Analyzer / visualizer nodes (Spectrum Analyzer, Oscilloscope, Spectrogram)

Three passthrough **Effect** nodes that tap the audio flowing through them and draw it in a non-modal, resizable floating window (double-click the node to open). Audio passes through unchanged, so they sit inline anywhere in a chain — drop one mid-signal to see what a specific effect or instrument is producing, distinct from the global **View → Spectrum Analyzer** which always measures the master output.

All three share one backing class, `AnalyzerProcessor`, which copies each input block into a per-node ring buffer (`AnalyzerCapture`, 4096 samples, `analyzer_nodes.h`). The processor fills it on the audio thread; the editor component polls a snapshot at UI rate. The capture is looked up by node id through `AnalyzerCaptureRegistry` and held by `shared_ptr`, so it survives briefly after you disconnect the cable (you keep seeing the last buffer) and is freed once both the processor and editor let go.

- **Spectrum Analyzer** (`__spectrum__`) — live FFT bar display. Bar count derives from the window width (~one bar per 4 px), so a wider window shows finer spectral detail; height scales the bars.
- **Spectrogram** (`__spectrogram__`) — scrolling time-frequency waterfall; new FFT frames enter at the right and fade out at the left. Log-frequency vertical axis, magma colour ramp. The FFT size scales with window height (next power of two ≥ 2·H, capped at 4096), so a taller window resolves finer frequency detail.

### Oscilloscope (`__oscilloscope__`)

Time-domain waveform display; L (blue) and R (orange) overlaid. One pixel of canvas width = one captured sample, so dragging wider shows more samples per frame (capped at 4096 samples / ~93 ms @ 44.1 kHz once the ring fills — a "(buffer cap reached)" note appears at the cap). A top-right overlay shows the current sample count and equivalent time span.

A control strip across the top picks the **acquisition mode**:

- **Triggered** (default) — the trace is aligned to a **level crossing** of the chosen slope, so a periodic waveform appears stationary (classic oscilloscope sync). The scope snapshots a window up to twice as wide as it displays, then searches the early portion for the first sample where the L channel crosses the trigger **level** with the chosen **slope** (Rising: `prev < level && cur >= level`; Falling: `prev > level && cur <= level`) and draws forward from there. If no crossing is found it falls back to the most-recent N samples (so a flat/non-crossing signal still shows something). A faint dashed green line marks the trigger level.
  - **Slope** button — toggles **Rising** / **Falling** (which direction the signal must cross the level to start the trace).
  - **Level** slider — trigger threshold in **−1..1** (0 = zero crossing).
- **Roll** — free-running strip chart: the newest N samples are drawn each frame with the newest at the right edge, no edge alignment. The Slope and Level controls grey out (their tooltip explains why and points back to Triggered mode).

The mode, slope, and level persist on the node (`Node::scopeTriggered` / `scopeTrigLevel` / `scopeTrigRising`, serialized in `project_file.cpp` only when non-default — old projects without these keys open in Triggered mode at zero level / rising). Editing any control writes the node fields and calls `graph.commitSnapshot("Oscilloscope settings")`, so changes are undoable and mark the project dirty; the level slider commits once on drag-end rather than per tick.

---

## Script program reference (algorithmic MIDI, languages)

This section documents the **program model** — statements, persistent state, MIDI-emit side effects, sections, languages — used by the unified [Script](#script-signal--midi) node when it generates MIDI (and, with the same machinery, continuous output). It is the algorithmic-MIDI reference: a Script node with one or more MIDI outputs runs a small program live and emits MIDI — a generative sequencer, euclidean-rhythm engine, arpeggiator, LFO-to-CC, chord exploder, etc. By default the program is written in the same `WaveExprParser` math language used elsewhere, extended with **statements, persistent state, and MIDI-emit side effects**, and runs **once per audio sample**. A **Language** dropdown switches it to **Lua** or a **WebAssembly** module instead (see [Scripting language](#scripting-language-built-in--lua--webassembly)).

> This material formerly described a separate **MIDI Script** node. That node merged into the unified Script node — set the *MIDI outputs* count to ≥1 in the [Script](#script-signal--midi) editor to get the emit functions described here. Legacy MIDI Script nodes in old projects still load and edit through their original editor.

The built-in language is the **no-toolchain** path for algorithmic MIDI; Lua and WebAssembly add a real programming language (the same node can host all three) for anyone who outgrows the expression vocabulary. The rest of this section documents the **built-in** language unless noted; Lua and Wasm share the same emit functions, output routing, signal inputs and shape table.

### The program

Statements are separated by `;` or newlines. A statement is either an **assignment** (`name = expr`) or a bare expression evaluated for its side effects (the emit functions). **Any variable you assign persists across samples and blocks**, so you can keep a running counter, oscillator phase, or RNG seed between samples without any special declaration:

```
ph = beat - floor(beat)             # fractional beat position
(ph < dt*bpm/60) ? note(36, 110, 0.1) : 0   # kick on each downbeat
(noise(0) > 0.6) ? note(42, 60, 0.05) : 0   # random closed hat
```

Persistent state is reset only when the program text changes (an edit) or the node is rebuilt — at which point in-flight notes are also flushed so an edit can't leave a stuck note.

### Sections: `init:`, `start:`, `loop:`

Because the body runs every sample forever, doing something exactly once (seed a counter, play a single downbeat note) needs a dedicated hook. The program can be split into up to three sections by **header lines** — a line whose only content (case-insensitive, whitespace ignored) is `init:`, `start:`, or `loop:`. Lines before the first header belong to `loop:` by default, so **a program with no headers behaves exactly as before** (all per-sample).

| Section | When it runs | Sink | Typical use |
|---------|--------------|------|-------------|
| `init:` | **Once** when the program loads or its text changes (inside `reloadIfScriptChanged()`, after state is cleared). | **Null** — emit calls are no-ops. | Seed persistent variables (counters, RNG seed, lookup tables). |
| `start:` | **Once** per transport play rising edge (stopped → rolling), at sample offset 0. Also re-armed when the script changes mid-play, so editing while playing re-fires it. | **Live** — can emit. | A one-shot at the downbeat: a single note/chord, a reset CC. |
| `loop:` | **Every sample** (the default body). | **Live**. | The ongoing generative pattern. |

All three sections share the same persistent `stateVars`, so `init:` can set a value `loop:` reads and `start:` resets. Example — a 4-step sequencer that restarts from step 0 on every play:

```
init:
step = 0

start:
step = 0

loop:
hit = (beat - floor(beat)) < dt*bpm/60
hit ? (step = (step + 1) % 4) : 0
hit ? note(48 + step*3, 100, 0.2) : 0
```

Implementation: the `init:` / `start:` / `loop:` sectioning is part of the **built-in language runtime** (`BuiltinExprRuntime` in `script_runtime_builtin.cpp`), not the processor. The runtime is created with `ScriptRole::Unified`: `BuiltinExprRuntime::splitSections()` partitions the source into `initProgram` / `startProgram` / `bodyProgram`; `reset()` runs `initProgram` with a null sink (seeding `stateVars`); `onStart()` runs `startProgram`; and `runUnified()` runs `bodyProgram` once per sample, both emitting MIDI through the sink **and** harvesting the assigned `o1`..`oP` outputs from `stateVars` (with `o1` falling back to the program's last value). The processor (`SignalShapeProcessor::processBlock()`) tracks `wasPlaying` to detect the play edge and calls `runtime->onStart()` at offset 0 before the per-sample `runUnified()` loop, then ages out any pending note-offs at end-of-block (so a `start:`-only program still releases its notes correctly). The sections are a feature of the built-in language only — Lua uses its own `start()` / `loop()` functions and top-level init code (see [Scripting language](#scripting-language-built-in--lua--webassembly) below).

### Emit functions

These push MIDI at the current sample; each returns 1.0 (so they compose inside `?:` / arithmetic). `pitch` and `vel` are 0..127 (rounded, clamped). Anywhere a `pitch` number is accepted you can instead write a **quoted note name** — `note("C4", 100, 0.5)` is the same as `note(60, 100, 0.5)`. See [Note names and frequency](#note-names-and-frequency) below.

| Call | Effect |
|------|--------|
| `note(pitch, vel, durSec)` | Note-on now, auto note-off after `durSec` seconds. Scheduled note-offs survive across blocks. `vel <= 0` emits nothing. |
| `noteon(pitch, vel)` | Note-on only (you release it yourself). `vel <= 0` becomes a note-off. |
| `noteoff(pitch)` | Note-off now. |
| `cc(number, value)` | Control change. `value` is **0..1**, scaled to 0..127. `number` is the CC index 0..127. |
| `bend(value)` | Pitch bend. `value` is **-1..1** (0 = centre), mapped to the 14-bit wheel 0..16383. |

`durSec` is converted to whole samples (`max(1, round(durSec × sampleRate))`); pending note-offs are capped at 512 in flight per node.

### Multiple MIDI outputs

The editor's *MIDI outputs* count (0..16) sets how many independent MIDI output pins the node has ("MIDI Out 1" … "MIDI Out N"). **Each output is its own MIDI cable, not a MIDI channel** — route the *following* emits to output *k* (0-based) by assigning the reserved variable `out`:

```
out = 0;  note(36, 110, 0.1)        # kick -> MIDI Out 1
out = 1;  note(38, 90, 0.1)         # snare -> MIDI Out 2
```

`out` is just a persistent variable like any other (default 0), clamped to the declared output count. Under the hood the node tags each emitted event with channel = `out + 1` and the graph splices a per-output `MidiChannelFilterProcessor` between the node and each destination, which keeps only its channel and rewrites it back to channel 1 — so every cable downstream sees a clean single-stream MIDI feed. With a single output (the default) no filter is inserted.

### Multiple MIDI inputs

The editor's *MIDI inputs* count (0..16) sets how many independent MIDI input pins the node has: **0** = none (the `note` / `vel` / `gate` / `freq` variables stay idle), **1** = a single "MIDI In" pin (the classic case), **>1** = "MIDI In 1" … "MIDI In N", each its own cable. When more than one input pin is present, every incoming MIDI event reports **which input it arrived on** so one program can react to several sources independently:

- `pollmidi()` and `midievent(i)` return a 1-based input index as their **last** value — `kind, offset, a, b, idx` (idx last, so the older `local kind, off, a, b = …` idiom still works).
- The structured `pullblock()` event tables carry the same index as their `idx` field (`{kind, offset, a, b, idx}`).
- The `note` / `vel` / `gate` / `freq` convenience variables still track the **most-recent note-on across *all* inputs** — use the per-event `idx` when you need to tell inputs apart.

Under the hood this is the mirror image of the multi-output mechanism: JUCE merges every incoming MIDI cable into a node's single MIDI bus, so the graph splices a per-input `MidiChannelStampProcessor` onto each cable that rewrites its events' channel to (input index + 1); the node recovers the pin index from the channel nibble. With 0 or 1 input pins no stamper is inserted and the channel carries no routing meaning. (WASM modules declare their input-pin count by exporting `ss_num_midi_inputs()`, the input-side counterpart of `ss_num_midi_outputs()`; each delivered event's `ss_midi_event_t.input_index` holds the 0-based pin.)

### Signal inputs and the shape table

- **Signal inputs (s1..sN)** — the *Signal inputs* count (0..16) appends Signal/Param input pins read per-sample as `s1`, `s2`, … `sN` (0 when not wired). Pin ids are preserved across temporary shrinks. The "MIDI In" pin(s) (the editor's *MIDI inputs* count) drive the `note` / `vel` / `gate` / `freq` variables from the most-recent incoming note across all inputs — see [Multiple MIDI inputs](#multiple-midi-inputs).
- **Shape table** — an optional embedded [Layered Waveform](#layered-waveform-editor) stack (same *+ Layer* editor as everywhere else, with a summation preview). Sample it at any phase `pos` (0..1, wraps) with `shape(pos)`. Until you add a layer, `shape()` returns 0. Useful as a hand-drawn velocity contour, melodic table, probability curve, etc.

### Variable vocabulary

Readable inside the program (in addition to your own persistent variables):

| Variable | Meaning |
|----------|---------|
| `t` | Seconds since transport start (at the current sample) |
| `beat` | Transport beat position |
| `bar` | `beat / 4` |
| `bpm` | Tempo (beats per minute) |
| `playing` | 1 while transport is playing, else 0 |
| `sr` | Sample rate (Hz) |
| `dt` | Seconds per sample (`1 / sr`) |
| `note` | MIDI note number of the most recent MIDI-input note-on (-1 if none) |
| `vel` | Velocity 0..1 of that note |
| `gate` | 1 while any MIDI-input note is held, else 0 |
| `freq` | Frequency in Hz of the most recent note (0 if none), using the **project tuning system** (see [Note names and frequency](#note-names-and-frequency)) |
| `s1`..`sN` | Signal input pin values at this sample |
| `out` | (read/write) destination MIDI output index for subsequent emits |

Plus the full `WaveExprParser` math vocabulary: `sin cos tan asin acos sinh cosh atan(y[,x]) asinh acosh atanh abs sign sqrt inversesqrt exp exp2 log log2 pow tanh saw square triangle noise floor ceil round roundEven trunc fract mod(a,b) min(a,b) max(a,b) clamp(v,lo,hi) mix(a,b,t) step(edge,x) smoothstep(e0,e1,x) fma(a,b,c) radians degrees if(c,a,b)`, the comparisons `< > <= >= == !=`, boolean `&& || !`, and the ternary `c ? a : b`. (The hyperbolic/inverse-hyperbolic, `exp2 log2`, `roundEven fma`, and `mix step smoothstep fract sign mod radians degrees inversesqrt` group mirror the GLSL shape dialect — the complete scalar slice of GLSL's builtins.) Unknown identifiers read as 0. `shape(pos)` samples the embedded shape table.

### Note names and frequency

Every MIDI-scripting surface — the built-in expression language, Lua, and the offline Python API — accepts **note names** wherever a MIDI pitch number is expected, and exposes conversion helpers. Names follow scientific-pitch convention: a letter `A`–`G` (case-insensitive), any run of accidentals (`#`/`+` = sharp, `b` = flat — so `C##4` = D4, `Cb4` = B3, `B#4` = C5), and an octave number where **C4 = MIDI 60, A4 = 69**. A name with no octave digit defaults to octave 4.

| Helper | Built-in | Lua | Python | Result |
|--------|:--------:|:---:|:------:|--------|
| Quoted note name as a pitch | ✓ (`note("C4",…)`) | ✓ (`note("C4",…)`) | ✓ (`add_note(n,c,"C4",…)`) | the name's MIDI number |
| `notenum(name)` / `notenum(name, octave)` | ✓¹ | ✓ | ✓ | MIDI number (`notenum("C",4)` → 60); −1 on a bad name |
| `notename(num)` | —² | ✓ | ✓ | note-name string (`notename(60)` → `"C4"`) |
| `notefreq(note)` / `notefreq(name, octave)` | ✓ | ✓ | ✓ | frequency in Hz |

¹ In the built-in numeric language a quoted string already evaluates to its MIDI number, so `notenum(x)` there is just a readable pass-through (it doesn't take a separate octave argument — write `notenum("C4")` or the bare literal `"C4"`).
² The built-in language has no string type, so `notename` (which returns a string) exists only in Lua and Python.

**WebAssembly.** A WASM module reaches the same project tuning through the host import `ss_note_to_freq(midinote) -> float` (the one note helper that needs the host, since only the host knows the tuning). Name ↔ number conversion is pure, so `soundshop_wasm.h` ships it inline — `ss_notenum("C4")`, `ss_notename(60, buf)`, and `ss_notefreq("C4")` (which combines the two) — with no host round-trip. See [`cpp/scripts/wasm_examples/README.md`](cpp/scripts/wasm_examples/README.md).

**`notefreq` uses the project tuning system.** Frequency depends on the project-global **tuning** (Equal Temperament / Pythagorean / Just Intonation / Quarter-Comma Meantone) and **concert pitch** (A4 = 440 Hz by default) set in the tuning dialog — *not* on any track's musical scale (a scale/key is a compositional constraint on the piano roll and the degree system; it never changes pitch frequencies). This is the same mapping the `freq` variable and the built-in Terrain Synth use, so `notefreq("A4")` returns exactly the concert pitch. Hosted VST3/AU instruments are tuned too, by a cable-level pitch-bend adapter — see [Microtuning hosted plugins](#microtuning-hosted-plugins).

### What it outputs / passthrough

In MIDI mode the Script node is a **generator, not a filter**: its MIDI input is consumed only to set the `note` / `vel` / `gate` / `freq` variables and is **not** passed through to the MIDI outputs. The audio buffer's channels 2+ carry the incoming Signal pins (the standard control-channel convention) on the way in and the continuous outputs `o1`..`oP` on the way out.

**Stop flushes held notes.** When the transport stops (rolling → stopped), the node emits an all-notes-off (CC 123) on all 16 channels and drops any scheduled note-offs still in flight. This guarantees a note the script left open — e.g. a `start: noteon(64)` with no matching `noteoff` — releases on the downstream synth instead of ringing forever; pressing Stop is the reliable way to silence a script-driven drone. (This mirrors the MIDI timeline's stop behaviour.)

### Scripting language (Built-in / Lua / WebAssembly)

The editor's **Language** dropdown (top of the dialog) chooses which runtime executes the program. All three share the same emit functions, `out` routing, Signal inputs and shape table; they differ in language power and execution rate. The choice, the execution rate, and the `.wasm` path are stored in the doc and round-trip through save/load and undo.

| Language | Source | Execution rate | Notes |
|----------|--------|----------------|-------|
| **Built-in** (default) | The program text, in the `WaveExprParser` expression language with `init:` / `start:` / `loop:` sections. | **Per sample**, always. | Real-time-safe, allocation-free, no setup. |
| **Lua** | The program text, as a Lua 5.4 program. | **Per sample** *or* **per block** (a second *Run* dropdown). | A full language; sandboxed (no `io`/`os`/`package`/`debug`, no `require`/`load`/`dofile`). |
| **WebAssembly** | A `.wasm` binary chosen via **Choose .wasm file…** (the editor shows a file picker in place of the text box). | **Per block**, always. | C / Rust / Zig / AssemblyScript — or GLSL via SPIR-V (see below) — compiled to Wasm; must implement the SEANCE script ABI. Requires a wasm3-enabled build. |

The **Run** dropdown sets the execution rate and is only editable for Lua. Built-in is forced to per-sample and Wasm to per-block (both greyed out, with tooltips explaining why). Per-sample runs the program once for every audio sample (sample-accurate, but heavy Lua can stutter the audio — the editor shows an inline warning for Lua + per-sample). Per-block runs it once per audio block (cheap, scales to many instances); for sample-accurate emits the program stamps each event with a sample offset itself.

**Running GLSL in real time, via WASM (power-user toolchain).** GLSL is a *bake-only* language in SEANCE — it never runs on the audio thread, because a GPU is a high-latency batch device unsuited to a real-time callback (see [SCRIPTING-LANGUAGES.md](SCRIPTING-LANGUAGES.md#why-glsl-cant-do-real-time-audio--in-any-mode) for the full rationale). But if you have a GLSL compute algorithm you genuinely want to run *live* on a Script node, you can compile it to WebAssembly offline and load the resulting `.wasm` like any other module — CPU execution sidesteps every GPU concern, and a wasm-compiled GLSL kernel is just an ordinary real-time-safe per-block program. The route is:

1. **GLSL → SPIR-V**: `glslangValidator -V --target-env opengl yourkernel.comp -o kernel.spv` (or `glslc` from the Vulkan SDK). Author it as a normal compute shader whose body fills an output buffer.
2. **SPIR-V → C**: `spirv-cross --output kernel.c kernel.spv` (SPIRV-Cross's C/C++ backend), which emits portable C that computes the same arithmetic on the CPU.
3. **Wrap to the SEANCE ABI**: write a thin `ss_process()` that, each block, calls the generated kernel per output sample (or over the whole block) and writes the SEANCE output pins. The `soundshop_wasm.h` header (in `cpp/scripts/wasm_examples/`) provides the ABI scaffolding.
4. **C → WASM**: compile with `clang --target=wasm32 …` (or Emscripten / `zig cc -target wasm32-freestanding`), then pick the `.wasm` in the Script node.

This is deliberately a manual, offline path rather than a built-in button: it's an advanced workflow, and the resulting module is indistinguishable from any other WASM node once loaded. The reason there's no "GLSL" option in the *real-time* Language dropdown is that a CPU-executed GLSL kernel is architecturally identical to a WASM module — so WASM already *is* the real-time path for GLSL-style algorithms (see the rationale doc's [CPU-GLSL discussion](SCRIPTING-LANGUAGES.md#why-glsl-cant-do-real-time-audio--in-any-mode)). For waveshaping math (as opposed to a whole ported kernel), the **Built-in** language now covers the complete *scalar* slice of GLSL's Trigonometry/Exponential/Common builtins (`mix step smoothstep fract sign mod atan(y,x) asinh acosh atanh exp2 log2 roundEven fma radians degrees inversesqrt`, …), so many GLSL idioms port directly with no toolchain at all. The remaining gap vs GLSL is the *type system* (vectors/matrices/swizzles), not the math vocabulary — see the rationale doc's [sizing note](SCRIPTING-LANGUAGES.md#how-big-is-make-our-expression-language-exactly-glsl).

**Lua program model.** Top-level code runs once at load (globals persist). Define **one body**: `function loop()` (per-sample or per-block) or `function stream()` (the streaming pull-model, per-block only — see [Streaming](#streaming-pull-model-coroutine-scripts) below). `function start()` (optional) runs once when playback starts. Emit with `note(p,v,dur[,offset])`, `noteon`, `noteoff`, `cc`, `bend` — in per-block mode the final `offset` argument (0..n-1) places the event at a precise sample. Set `out = k` to pick the output pin. The note functions accept a name string for the pitch (`note("C4", 100, 0.25)`), and `notenum` / `notename` / `notefreq` convert between names, numbers and Hz (see [Note names and frequency](#note-names-and-frequency)). Per-block variables include `n` (block length), `tStart`/`tEnd`, `beatStart`/`beatEnd`, plus `sig(k, i)` to read a signal pin at sample `i`. The math aliases (`sin`, `clamp`, `saw`, `noise`, …) and `shape(pos)` are available; `math.*` works too. The in-editor **Lua reference** button has the full vocabulary and examples (including the sample-accurate per-block loop `for i=0,n-1 do … note(p,v,d,i) end`).

**Event-driven MIDI input (per-block).** Alongside the block-constant `note`/`vel`/`gate`/`freq` snapshot (the *most-recent* held-note state), a per-block program can iterate the **actual MIDI-input events** that arrived in this block, each with its own sample offset, and react to them one by one. `midiin()` returns the **count** of input events; `midievent(i)` (1-based) returns `kind, offset, a, b`:

| `kind` | `offset` | `a` | `b` |
|---|---|---|---|
| `"on"` | sample within block | note number | velocity `0..1` |
| `"off"` | sample within block | note number | `0` |
| `"cc"` | sample within block | controller number | value `0..1` |
| `"bend"` | sample within block | `0` | `−1..1` (0 = centre) |

```lua
function loop()
  for k = 1, midiin() do
    local kind, off, a, b = midievent(k)
    if kind == "on" then
      -- start a voice for note `a` at velocity `b`, sample-accurate at `off`
    elseif kind == "off" then
      -- release note `a`
    end
  end
  -- ...then fill the block with out(i, value) as usual
end
```

`midiin()`/`midievent()` work in any per-block program (whether it uses `loop()` or `stream()`). The list is available in every role that has a MIDI-input pin (a Signal/Unified node can react to notes too), and is in arrival order (non-decreasing `offset`). Outside per-block mode `midiin()` returns `0`, so a program that references it still runs unchanged at per-sample rate. (Per-sample programs keep using `note`/`vel`/`gate`, which already update every block.) For the streaming body, the cursor-aligned `pollmidi()` (below) is usually more convenient than counting `midiin()`.

#### Streaming (pull-model) coroutine scripts

`function loop()` is a *push* model: the host calls it once per block and the script addresses samples by absolute index (`out(i, v)`). That's efficient but means the program can't be written as a single continuous loop — it has to be re-entered every block and rebuild any cross-block state from globals by hand.

`function stream()` is the alternative **pull model**. The program owns its own control flow: it loops forever, **pulls** input and **pushes** output, and is *suspended and resumed* at the block boundary by a Lua coroutine. "The next sample doesn't exist yet" is handled by the program *suspending* until the host hands it the next block — not by re-evaluating the whole program. Crucially, **the coroutine's local variables persist across the suspend/resume**, so a filter's running state, a delay line's history, a phase accumulator, etc. are just ordinary `local`s in the loop — no manual block bookkeeping.

The stream coroutine is created **once, off the audio thread** (when the script loads), so `runBlock()` only ever resumes it — no per-block allocation. The same pull/push API serves both **audio-rate** work (grab one sample per loop iteration) and **block-rate** work (grab a whole block per iteration); the only difference is how much you take each time. Because a streaming program is inherently sample-accurate (it iterates samples itself), the **Run dropdown is ignored** for `stream()` programs — streaming always runs through the block path regardless of whether per-sample or per-block is selected.

| Function | Blocking? | Returns / does |
|---|---|---|
| `pull()` | **blocks** (suspends at block end) | next input sample(s) — one return value per signal-in pin (`local a,b = pull()`); a node with no inputs returns a single `0` (call it just to advance the cursor) |
| `poll()` | non-blocking | next input sample(s) if one is still available this block, else `nil` |
| `wait()` | **suspends** | hand the block back and resume in the next one (the explicit suspend for `poll()` loops); a no-op outside streaming |
| `out(v)` | — | write the **current** sample (the one just `pull()`ed) to output pin `o1`, clamped to `0..1`. `out(n, v)` writes pin `n` (**1-based**: `o1, o2, …`) — a streaming node drives every continuous output pin independently |
| `pullblock(pin)` | **blocks** | the whole block's input samples for that **1-based** pin as a flat array `{…}` (single-input convenience form) |
| `pullblock()` | **blocks** | *no arg* → two values `params, events`: `params[k]` is input pin `k`'s sample array (one list per pin — all the same length, since param inputs are synchronized), and `events` is a list of this block's MIDI-input event tables (see below) |
| `pollblock([pin])` | non-blocking | the **remaining** input this block (same two shapes as `pullblock`), or `nil` |
| `outblock([n,] t)` | — | write array `t` (`t[1..]`) to output pin `n` (**1-based**; `o1` default), clamped to `0..1` |
| `pollmidi()` | non-blocking | `kind, offset, a, b, idx` for the next MIDI-input event the cursor has reached (`offset ≤` current sample), or `nil` — drains the block's events in order as the stream advances, giving event-driven MIDI consumption *inside* the loop. `idx` is the **1-based** MIDI-input pin the event arrived on, returned **last** so `local kind,off,a,b = pollmidi()` keeps working (same `kind`/`a`/`b` meanings as `midievent()`) |

**Unified event format.** Both `pollmidi()` and the `events` list from `pullblock()` describe a MIDI-input event with the same fields — `kind` (`"on"`/`"off"`/`"cc"`/`"bend"`), `offset` (sample within the block), `a`/`b` (note/controller and velocity/value), and `idx` (1-based source pin). In `pullblock()`'s list each event is a table `{kind=…, offset=…, a=…, b=…, idx=…}`; `pollmidi()` returns the same fields as multiple values. Param inputs, being dense and synchronized, are delivered as the `params` list-of-lists rather than as sparse events — but if you ever represent a param change as an event it uses the same `{idx, offset, value}` shape, so the two stay consistent.

`pull()`/`poll()`/`out()` share one read==write cursor, so the canonical transducer `local x = pull(); out(f(x))` is automatically sample-aligned. A one-pole low-pass is then just:

```lua
function stream()
  local y = 0
  while true do
    local x = pull()
    y = y + 0.05*(x - y)   -- `y` survives the block boundary for free
    out(y)
  end
end
```

A `poll()`-based loop instead checks for input and yields explicitly when there's none left: `while true do local x = poll(); if x then out(f(x)) else wait() end end`. The block-rate style takes the whole block at once: `local t = pullblock(1); for i=1,#t do t[i]=f(t[i]) end; outblock(t)`.

**Multi-I/O.** A streaming node can have any number of signal/param inputs and outputs. Inputs arrive together (`pull()` returns one value per input pin; `pullblock()` returns `params` as one list per pin), and outputs are driven independently via `out(n, v)` / `outblock(n, t)` for pin `n` (1-based). The host hands the runtime one buffer per output pin and routes each back to its own cable — there's no fan-out, so `o1` and `o2` carry whatever the script wrote to each. A node with several MIDI inputs shares one event stream tagged by `idx` (see the unified event format above).

If `stream()` ever **returns** (ends its loop), the stream is finished and the output simply holds at `0` until the script is reloaded. A runtime error inside the coroutine is reported in the editor like any other script error. **Note:** the coroutine is created at *load* (not on each transport-start), so a generator that should restart on play should read the `playing` global and reset its own state. (Today every node still has a single MIDI **input** pin, so `idx` is always `1`; the wire format already carries the index so multi-MIDI-input is a routing change, not an API change.)

**Wasm module.** A WebAssembly module is itself a *program-owns-the-loop* runtime — `ss_process()` fills the whole block and the module's linear memory persists between calls, so it is morally a streaming program and the host treats it as one (`isStreaming() == true`): the **Run dropdown is ignored** (audio-rate vs block-rate is the module's own internal business), and it gets the same **multi-I/O** treatment as a Lua `stream()`. The host writes each signal input pin into flat audio-in channel `k`, reads each flat audio-out channel `p` back into output pin `o(p+1)` **independently** (no fan-out — a multi-output module drives every pin), and — when the node has a MIDI input — forwards the block's MIDI-input events into the shared `midiIn` region in raw-MIDI form, where the module reads them via `ss_midi_in_events()` / `ss_midi_in_count()` (`soundshop_wasm.h`); each event's `input_index` byte carries the 0-based source pin (the same value Lua's `pollmidi()` returns as 1-based `idx`). For MIDI **output** the module emits via `ss_midi_out` / `ss_midi_out_n` with per-event sample offsets. Note helpers are available too: the host import `ss_note_to_freq(midinote)` returns the project-tuned frequency, and `soundshop_wasm.h` adds the pure `ss_notenum` / `ss_notename` / `ss_notefreq` (see [Note names and frequency](#note-names-and-frequency)). **Factory waveforms** are reachable too, closing the last cross-language gap: the host imports `ss_waveform(int id, float phase)` (the raw `[-1,1]` sample, same wrap+interpolate as every other language — see [Factory waveform library](#factory-waveform-library)) and `ss_waveform_id(const char* name)` (resolve a name to its stable integer id once, then reuse the integer in the hot loop, exactly like Lua's `waveforms[name]`). The bank is warmed off the audio thread at module-link time so the first call is allocation-free. Because the module compiles `-nostdlib` (no libm), `soundshop_wasm.h` also ships header-only GLSL-parity shaping helpers built on `__builtin_floorf` — `ss_fract`, `ss_sign`, `ss_mod` (GLSL floored modulo), `ss_clamp`, `ss_mix`, `ss_step`, `ss_smoothstep`, `ss_radians`, `ss_degrees`, `ss_saw`, `ss_square`, `ss_triangle`, `ss_unipolar`, `ss_bipolar` — under `#ifndef SS_NO_SHAPING_HELPERS`; the transcendentals (`sinf`/`cosf`/`expf`/`tanhf`) still need `<math.h>` + a linked libm. This is the same shared-memory ABI the standalone [WASM Script](../cpp/scripts/wasm_examples/README.md) node uses. If the build has no wasm3, choosing Wasm loads nothing and the node falls back to silence (the dropdown still shows the option greyed-out).

### Script error reporting

A program that fails to compile is surfaced two ways, so the problem is visible whether or not the editor is open, and the node's audio output stays silent until it's fixed (a broken script never makes noise):

- **Editor error strip.** A one-line red strip appears at the bottom of the script editor showing `Script error: <message>` whenever the current program fails to compile. It's fed by a message-thread *linter* — every edit (debounced ~300 ms so a heavy Lua top-level isn't recompiled on every keystroke) compiles the program with a throwaway runtime and shows or hides the strip. The strip is hidden while the script is clean. This catches **Lua** syntax errors (`luaL_loadstring`), **WebAssembly** load/link failures, and structural errors in the **Built-in** expression language (see below).
- **Node error badge.** In the node graph, a node whose live script failed to compile is drawn with a **red border** and a red **"!"** disc at its top-right corner. Hovering the node shows a tooltip ("Script error — this node's program failed to compile. Open the editor to see the error message."). The badge reflects the *running* processor's state (the script as the audio engine last loaded it), read live from the audio thread via an atomic flag, so it stays accurate even with every editor closed.

Both real-time script editors — the unified **Script** (Signal Shape) editor and the legacy **MIDI Script** editor — and their processors carry this machinery, so a broken program lights up the same way whichever node type hosts it. Every failed load is also written to `seance.log` (`Signal Shape "<name>": <message>` / `MIDI Script "<name>": <message>`), so there's a persistent record even if no UI is showing the error at the time.

**Built-in language checking.** The Built-in expression evaluator is deliberately *error-tolerant* at run time — an unknown identifier reads as `0`, a missing close paren is silently ignored — so it can never raise a syntax error mid-render. To still surface obvious mistakes, `load()` runs a cheap, side-effect-free **structural** pass (`WaveExprParser::validate`) over the program before it's used. It reports, with a 1-based line number:
- **Unbalanced parentheses** — an extra `)` with no matching `(`, or one or more `(` never closed.
- **An unterminated string literal** — a `"…` / `'…` (waveform/method name) with no closing quote.
- **An out-of-grammar character** — anything the language can't consume at all, e.g. `%`, `@`, `#`, `\`, `~`, `[`. (Parens and otherwise-illegal characters inside `"…"`/`'…'` literals are ignored.)

It deliberately does **not** flag unknown identifiers — those legitimately evaluate to 0 by design, so flagging them would break valid programs. The same out-of-grammar character that the validator reports would otherwise leave a per-sample program stuck on a non-advancing token; the evaluator's program loop now also has a hang-proof guard that steps over any such character, so a malformed Built-in program degrades gracefully instead of spinning the audio thread.

### Editing, undo, save/load

- Edits are written to `node.script` live (per keystroke) so the running graph hears them immediately, but a single undo step is pushed when the editor **closes** (`commitSnapshot("Edit Signal Shape")`), which also marks the project dirty. Per-keystroke edits intentionally don't each become an undo step. Changing the Language / Run dropdowns, the I/O counts, or picking a `.wasm` file commits the same way.
- **Project file format**: the unified Script node serialises its whole state (program, layers, I/O counts, language/rate) under the `__signalshape__:v1` prefix — see [Project file format](#project-file-format) in the Script section. The legacy `__midiscript__:v1` prefix (keys `program`, `lang`, `rate`, `wasm`, `sigCount`, `outCount`, `layer`) is still decoded for old MIDI Script nodes. `NodeType::MidiScript` remains a valid enum value (kept for serialization stability of old projects), but new nodes are always `NodeType::SignalShape`.

---

## Script (signal + MIDI)

Right-click the graph → *Signal Shape → Script (signal + MIDI)* creates a single unified **Script** node. One scriptable node now covers **both** signal generation (LFOs, envelopes, custom continuous control) **and** algorithmic MIDI generation — you decide which by setting its I/O counts in the editor. It is internally `NodeType::SignalShape` (the same node family as XY Pad and Control Bank), and the dedicated editor opens on create / double-click / right-click → *Edit Script…*.

This node subsumes the former separate **Signal Shape** and **MIDI Script** nodes (themselves descendants of the even older *LFO (sine)* / *LFO (custom expression)* / *Envelope (custom expression)* trio). A Script node with **0 MIDI outputs** behaves exactly like the old Signal Shape; one with a **MIDI-emitting program and no continuous output of interest** behaves like the old MIDI Script; one with **both** is a hybrid (e.g. an arpeggiator that also outputs an envelope). Legacy MIDI Script nodes in old projects still load and edit through their original editor; you just can't create new ones (the menu item is gone).

### I/O counts (what makes it signal, MIDI, or both)

Four controls in the editor set the node's pin layout. Each rewrites the node's pins immediately (preserving pin ids by name so existing cables survive a count change):

- **Signal inputs (s1..sN)** — 0..16 continuous input pins, read per sample as `s1`, `s2`, … `sN` (0 when not wired).
- **Signal outputs (o1..oP)** — 1..16 continuous output pins. The program assigns `o1`..`oP`; `o1` defaults to the program's last bare value (so a program that is just `curve` still drives `o1 = curve`). Always at least one.
- **MIDI outputs** — 0..16. **0 = pure signal source** (the node never touches MIDI). **≥1** enables the emit functions `note()` / `noteon()` / `noteoff()` / `cc()` / `bend()`, with the reserved variable `out` selecting which MIDI output pin (0-based). Each MIDI output is an independent cable (see *Multiple MIDI outputs* below).
- **MIDI input** — a toggle adding/removing the "MIDI In" pin that drives the `note` / `vel` / `gate` / `freq` variables. On by default; turn off for a node that generates signal or MIDI from scratch with no note input.
- **Pin type** — a dropdown flipping **all** continuous pins (inputs and outputs) between **Signal** (blue, audio-rate control) and **Param** (orange, parameter automation). Both carry the same 0..1 data and interconvert freely on the wire, so this is purely which colour/category the pins present as.

### Default on create

A brand-new Script node is **neutral**: it has **zero layers**, repeat mode *Forever*, `expr = curve`, and the default I/O — **one MIDI input, one Signal output (`o1`), zero MIDI outputs** — so it starts life as a classic LFO/envelope. A layer-less shape renders to a flat **0.5** — the neutral "no modulation" level on the 0..1 control wire (see [Control signal range](#control-signal-range-01)). Two consequences:

- **It outputs a constant 0.5 until you add a layer**, so creating the node does not immediately start modulating whatever it's wired to. (Previously the default was a free-running sine, which began sweeping downstream params the moment the node existed — surprising when you hadn't configured anything yet.)
- **The editor opens showing an empty layer stack** with a *+ Layer* button and a hint line ("No layers yet… until then it outputs a steady 0.5"). Click *+ Layer* to add the first sine layer, then pick *Sine* / *Saw* / *Square* / *Triangle* / *Noise* / *Drawn* / *Formula* on that layer's row, or draw / type a formula. Add more layers to sum several shapes into one contour (e.g. a slow sine plus a fast ripple).

### What it outputs

The continuous outputs `o1`..`oP` each carry a 0..1 value clamped to **[0, 1]** — the standard control-signal range (see [Control signal range](#control-signal-range-01); `0.5` is the neutral "no change" level). Their pin type (Signal blue / Param orange) is set by the **Pin type** dropdown; Signal and Param interconvert on the wire, so a single output can drive both audio-rate Signal consumers and orange param-arming inputs.

Held-value behaviour: each `oN` holds its last value when the shape isn't running (the envelope "stuck at the end"). A program that doesn't assign a given `oN` leaves `o1` at the program's return value and the rest at 0 / their last value.

If the node has **≥1 MIDI output**, those pins emit the events produced by the program's `note()` / `cc()` / `bend()` / … calls — see *MIDI emission* below.

### MIDI emission

With **MIDI outputs ≥ 1**, the program can emit MIDI as a side effect, exactly as the [Script program reference](#script-program-reference-algorithmic-midi-languages) documents in full. In brief: `note(pitch, vel, durSec)` (auto note-off), `noteon(pitch, vel)` / `noteoff(pitch)`, `cc(number, value)` (value 0..1) and `bend(value)` (value −1..1) push events at the current sample; assigned variables persist across samples; the `init:` / `start:` / `loop:` sections let you run code once vs every sample. The reserved `out` variable selects which MIDI output pin subsequent emits go to (0-based). **Each MIDI output is an independent cable** — under the hood events are tagged channel = `out + 1` and the graph splices a per-output `MidiChannelFilterProcessor` so every downstream cable sees a clean single-stream feed. Pressing **Stop** flushes any held/scheduled notes (all-notes-off on every channel), so a program that opens a note without releasing it can't ring forever. A node with **0 MIDI outputs** never touches the MIDI buffer at all.

Signal and MIDI emission happen in the **same per-sample pass**: a single program can assign `o1 = curve * s1` (an envelope) *and* call `note(...)` (an arpeggio) in one body, which is the point of unifying the two former node types.

### Anatomy of a shape

Three things define one Signal Shape:

1. **Shape waveform** — a `LayeredWaveform`: a stack of `WaveLayer`s (each sine / saw / square / triangle / noise / drawn points / freehand / formula) that are summed and peak-normalised into one contour, then mapped from the layer renderer's bipolar −1..1 onto the 0..1 control range (trough → 0, peak → 1) so `curve` reads as a 0..1 value. This is the **same shared layer-stack widget** (`LayerStackComponent`) the Wavetable editor uses — same *+ Layer* button, same per-row preset / harmonic-ratio / phase / amplitude controls, same drawn / freehand / formula UX. The only difference is that the Signal Shape editor shows a **"Sum of all layers" summation preview** under the stack (the Wavetable editor has its own multi-frame-type preview instead). The summed shape is sampled per audio sample from an internal phase that advances at the node's `Rate` param. A stack with **zero layers renders to a flat 0.5** (the neutral default).
2. **Composition program** `expr` — a math expression (or, for multiple outputs / MIDI, a multi-statement program) evaluated per sample, producing the 0..1 output. Defaults to `curve` (pass the shape through unchanged). You can modulate the shape arbitrarily: `curve * gate`, `curve * vel`, `clamp(curve + s1 * 0.5, 0, 1)`, etc. With **more than one continuous output**, assign each one explicitly — `o1 = curve * vel; o2 = s1` — where `o1` defaults to the program's last bare value if you don't assign it. The same program can also emit MIDI (see [MIDI emission](#midi-emission)). **Polarity gotcha:** `curve` (and the `s1..sN` inputs) are already 0..1, but `sin`/`cos`/`tan` (and `saw`/`square`/`triangle`/`noise`) return bipolar −1..1, so a bare `sin(...)` clips its negative half on the 0..1 output. Use the unipolar forms `usin`/`ucos`/`utan` (or `usaw`/`usquare`/`utriangle`), or wrap a whole bipolar sub-expression in `unipolar(...)`; `bipolar(x)` does the reverse (0..1 → −1..1).
3. **Trigger expression** `triggerExpr` — when this expression evaluates `> 0`, the shape advances. Rising edges (`false → true`) reset phase to 0 and start a new run. Empty trigger = always running (the LFO case).

### Repeat modes

When a run hits the end of one shape cycle:

- **Forever** — wrap and keep cycling. The classic LFO behaviour.
- **Once** — stop and hold the final value. The classic envelope behaviour: combine with `triggerExpr = "gate"` to fire on each note-on.
- **N times** — repeat N cycles then hold. The N-spinner becomes editable only in this mode.

### Variable vocabulary

Available inside `expr` and `triggerExpr`:

| Variable     | Meaning |
|--------------|---------|
| `curve`      | The drawn shape's height at the current phase, as 0..1 (trough → 0, peak → 1) |
| `x`, `phase` | Current phase 0..1 through the cycle |
| `t`          | Seconds since the last trigger fired (always advancing while running) |
| `beat`       | Transport beat position |
| `bpm`        | Current tempo (beats per minute) |
| `gate`       | 1 while any MIDI note is held, 0 otherwise |
| `freq`       | Frequency in Hz of the most-recently-pressed held note (0 if none) |
| `note`       | MIDI note number of the most recent note-on (-1 if none) |
| `vel`        | Velocity 0..1 of the most recent note-on |
| `rep`        | Number of complete cycles done since the last trigger |
| `rate`       | The node's Rate param |
| `s1`..`sN`   | Values of the Signal input pins at this sample (resolve to 0 when not wired) |

Plus the full `WaveExprParser` math vocabulary: `sin cos tan asin acos sinh cosh atan(y[,x]) asinh acosh atanh abs sign sqrt inversesqrt exp exp2 log log2 pow tanh saw square triangle noise floor ceil round roundEven trunc fract mod(a,b) min(a,b) max(a,b) clamp(v,lo,hi) mix(a,b,t) step(edge,x) smoothstep(e0,e1,x) fma(a,b,c) radians degrees if(c,a,b)`, comparison ops `<  >  <=  >=  ==  !=`, boolean `&& || !`, and the C ternary `c ? a : b`. (The hyperbolic/inverse-hyperbolic, `exp2 log2`, `roundEven fma`, and `mix step smoothstep fract sign mod radians degrees inversesqrt` group mirror the GLSL shape dialect for cross-language consistency — the full scalar slice of GLSL's builtins.) Unknown identifiers evaluate to 0. The **range helpers** `unipolar(x)` (−1..1 → 0..1) and `bipolar(x)` (0..1 → −1..1), plus the unipolar aliases `usin ucos utan usaw usquare utriangle unoise` (each the 0..1 form of its bipolar namesake), exist so bipolar math can be brought onto the 0..1 output cleanly — see the polarity gotcha above.

#### `shape(pos)` — input-driven shape lookup

`curve` always reads the drawn waveform at the node's *own* running phase, so it can only ever produce the LFO/envelope contour over time. When you instead want to read the drawing at a position you compute yourself — e.g. drive the lookup from an input signal, treating the drawing as a transfer function / waveshaper — use the `shape(pos)` function:

| Call | Effect |
|------|--------|
| `shape(x)` | Identical to `curve` (reads the drawing at the current phase). |
| `shape(s1)` | Reads the drawing at a position set by input `s1` — input-driven lookup / waveshaping. |
| `shape(x + s1*0.1)` | The current phase, warped by input `s1`. |
| `shape(t * 2)` | Plays the drawing through at twice the phase rate. |

`pos` is a **0..1 phase that wraps** (modulo 1, same linear-interpolated sampler as `curve`). Inputs `s1..sN` are already 0..1, so `shape(s1)` spans the full drawing directly. `shape()` is bound only inside the SignalShape node's per-sample evaluation; in other contexts that reuse `WaveExprParser` it evaluates to 0. This resolves the apparent impedance mismatch between continuous inputs (no absolute sample index) and the finite drawing: the drawing is never addressed by absolute sample number — only by a normalized phase, which either the node's clock (`curve` / `x`) or any expression you write (`shape(...)`) can supply.

### Signal inputs

The editor's *Signal inputs (s1..sN)* field accepts 0..16. Increasing N appends `s1`, `s2`, … pins to the node's input side; decreasing N drops the trailing pins (any cables wired to dropped pins go orphan and are skipped by the graph rebuild). Pin ids are preserved when N grows back, so wiring stays stable across temporary shrinks. The same applies to the continuous outputs `o1`..`oP` and the MIDI outputs (see [I/O counts](#io-counts-what-makes-it-signal-midi-or-both) above).

The MIDI input pin (`gate` / `freq` / `note` / `vel`) is present unless you turn off the *MIDI input* toggle.

### Manual trigger

The **Manual Trigger** button fires one rising-edge as if the trigger expression had just gone low→high. Useful for auditioning envelope-style shapes (repeat: Once) without actually wiring up a MIDI source. The button is disabled if the audio graph is in a state where the live processor can't be located (rare; only happens mid-graph-rebuild).

### Speed: Cycle length and Rate (two views of one value)

The editor exposes the node's speed two equivalent ways, side by side on one row, because "how fast does it oscillate" reads naturally as either a *frequency* (Rate) or a *duration* (Cycle length), and different users reach for different ones:

- **Cycle length** — how long one full cycle of the shape takes.
- **Rate** — how many cycles happen per unit time.

They are **the same underlying setting** — the `Rate` node param — shown reciprocally: `Rate = 1 / Cycle length`. The `=` between the two fields is a reminder that editing one immediately rewrites the other; whichever field you type into is the one that "wins", and the other recomputes. (The field you're actively typing in is never reformatted out from under your cursor; its partner updates live.)

The **Sync to beat** toggle next to them is the node's `Beat Sync` param, and it chooses the units both fields are expressed in:

| Sync to beat | Cycle length unit | Rate unit |
|---|---|---|
| Off (free-run) | seconds | Hz (cycles/sec) |
| On (tempo-locked) | beats | cycles per beat |

So a 2-second LFO in free-run reads *Cycle length = 2 sec / Rate = 0.5 Hz*; flip Sync on and the same value is now read against the song tempo as beats / cycles-per-beat. Editing here writes the `Rate` (and `Beat Sync`) node params directly — the same params shown on the node face — so the node body sliders and the editor fields always agree. Rate is clamped to the param's range (0.01–50), i.e. cycle length spans roughly 0.02–100 in the current units.

> **Note** — "Sync to beat" (above) and "Free-run (ignore song position)" (below) are independent settings. "Sync to beat" picks the *units* of the Rate (Hz vs cycles-per-beat). "Free-run" picks the *clock* the phase runs off (the song position vs a free-running oscillator). You can have a Hz-rate shape that's still locked to song position, or a beat-rate shape that free-runs.

### Phase source: locked to song position (default) vs free-run

A non-triggered Signal Shape (LFO — empty Trigger field) derives its phase one of two ways, chosen by the **Free-run (ignore song position)** checkbox in the editor:

| Free-run | Phase source | Behaviour |
|---|---|---|
| **Off (default)** | The transport / song position | **Deterministic**: the same song position always produces the same phase, so what you hear matches the rendered/exported audio and replaying a section sounds identical every time. The shape also **freezes while playback is stopped** (and follows the playhead if you scrub). |
| **On** | A free-running internal clock | The shape oscillates continuously off its own clock (analog-LFO style), **drifting independently of the song** and never stopping — livelier, but *not reproducible*: a bounce can land differently than what you heard, and two playthroughs differ. |

The default is **off (locked to song position)** because for a DAW, reproducibility — *export == playback* — is normally the property you want. Free-run is offered for sound-design cases where a never-resetting, grid-independent wobble is desirable.

In locked mode, **Beat Sync** chooses whether the phase tracks musical beats (`phase = frac(beats × Rate)`) or elapsed seconds (`phase = frac(seconds × Rate)`); either way it's a pure function of the song position. Finite repeat modes (*Once* / *N times*) still apply: after the allowed number of cycles the shape holds at the end of the last cycle, deterministically, regardless of where playback started.

**Free-run has no effect when a Trigger expression is set** — a triggered shape already restarts deterministically on each trigger — so the checkbox is greyed out in that case.

This setting is per-node and saved with the project (`freeRun` key, see *Project file format* below). Projects saved before this option existed load with free-run **off**, so older free-running LFOs become locked-to-song-position (deterministic) on load.

### Params on the node face

The four params shown on the node body in the graph view are also editable inline without opening the editor:

- **Rate** — cycles per second. When *Beat Sync* is on, this is interpreted as cycles per beat instead. Also editable as *Rate* / *Cycle length* in the editor (see above).
- **Beat Sync** — 0/1 toggle. Off = free-running Hz, On = tempo-locked. Also the editor's *Sync to beat* toggle.
- **Phase** — phase offset 0..1 added before sampling the shape every block. Useful for ganging two LFOs in quadrature.
- **Output** — read-only mirror of the last computed sample (use as a meter / for param-arming feedback).

Everything else — the shape itself, the composition expression, the trigger expression, the repeat mode, the signal-input count — lives in the editor.

### Scripting language (Built-in / Lua / WebAssembly)

Like [MIDI Script](#scripting-language-built-in--lua--webassembly), the editor has a **Language** dropdown that picks the runtime computing the output, plus a **Run** dropdown for the execution rate (editable only for Lua). The way the script plugs into the LFO/envelope machinery depends on the rate:

- **Built-in** (default) — per sample. The composition `expr` ("Output") turns the drawn `curve` into the output, exactly as documented above. The full phase / repeat / trigger machinery is in play.
- **Lua, per sample** — the program's `loop()` runs once per sample and **returns** the output value (0..1). It can read `curve` (the drawn shape at the current phase) and every variable in the table above, so it's a drop-in for the built-in composition expression with a real language. The same range helpers (`unipolar`/`bipolar`, `usin`/`ucos`/…) are in the Lua prelude. The phase / repeat / trigger machinery still drives `curve` and the timing variables.
- **Lua, per block** *or* **WebAssembly** — **raw-buffer mode**: the script fills the whole audio block itself (Lua via `out(i, value)`, `i = 0..n-1`; Wasm via the block ABI). The phase / repeat / trigger / curve machinery is **bypassed** — the editor greys out the Trigger, Repeat, Speed and Free-run controls and shows a note saying so. The drawn shape is still readable via `shape(pos)`, signal inputs via `sig(k, i)`, and incoming MIDI as events via `midiin()` / `midievent(i)` (see [Event-driven MIDI input](#scripting-language-built-in--lua--webassembly) above) so the block program can react to notes/CC/bend at sample accuracy. This is the mode for fully custom signal generation (sample-accurate sweeps, sequenced control, audio-rate DSP).

The trigger expression, when active, is always the built-in expression language regardless of the main language. For Lua + per-sample the inline "Output" box becomes a multi-line Lua program editor; for Wasm it's replaced by the **Choose .wasm file…** picker. The in-editor **Lua reference** button documents the full per-sample and per-block models with examples.

### Project file format

`node.script` carries the Script-node state in a multi-line key-value format prefixed with `__signalshape__:v1`. Keys include `expr` / `trigger` (Base64), `lang` (0=Built-in, 1=Lua, 2=Wasm), `rate` (0=per-sample, 1=per-block), `wasm` (Base64 `.wasm` path), `repeat`, `repeatN`, `freeRun` (0/1 — the phase-source toggle), and the I/O counts: `sigCount` (signal inputs), `outCount` (continuous outputs `o1`..`oP`, ≥1), `midiOut` (MIDI outputs, 0..16), `midiIn` (0/1 — whether the MIDI In pin exists), `paramKind` (0=Signal / 1=Param for all continuous pins), plus `layer`. For Lua the program source is stored in `expr` (the same key the built-in composition expression uses), so switching languages keeps your text. Expression strings are Base64-encoded so they can contain newlines / `=` / `|` without breaking the framing; the **whole layer stack** is encoded inline under the `layer=` key via `LayeredWaveform::encodeBody()` (`tableSize|layer1|layer2|…`). Multi-line scripts are serialised by `project_file.cpp` via the `scriptLines=N` form so the full payload round-trips through save/load. A missing `freeRun` / `lang` / `rate` / `outCount` / `midiOut` / `midiIn` / `paramKind` key (older projects) decodes to its default (`false` / Built-in / per-sample / `1` / `0` / `true` / `false`), so a pre-unification Signal Shape loads as a 1-continuous-output, 0-MIDI-output Script — identical behaviour.

Back-compat: the older single-layer Signal Shape scripts used the *same* `LayeredWaveform::encodeBody()` payload under the same `layer=` key, so they decode straight into a one-element layer stack with no migration step. Legacy plain-expression scripts (`sin(x)`, `(1 - cos(x)) * 0.5`, etc.) from before the redesign load as a single Formula layer with the old expression in `formulaExpr` — the user sees their old curve preserved and can edit it as a layer just like any other shape.

---

## Control Bank

Right-click the graph → *Signal Shape → Control Bank* creates a Control Bank node: a bank of *N* manual macro faders, each emitting one control-signal output. It shares `NodeType::SignalShape` with Signal Shape and XY Pad (so it's the same node family on disk and in the processor), distinguished by the `__controlbank__` script tag and handled by a dedicated branch in `SignalShapeProcessor::processBlock`. The editor opens immediately on create, and via double-click or right-click → *Edit Control Bank…*.

### What it outputs

Each slider is a `Param` (range 0..1, default 0.5) paired 1:1 with a **Signal output pin** of the same name. The slider's value *is* the output value — written straight onto the pin's control channel every block, with no shape/trigger/phase machinery (exactly like XY Pad's X/Y/Z). This puts Control Bank squarely on the unipolar [Control signal range (0..1)](#control-signal-range-01) convention: the default of **0.5 is the neutral "no modulation" level** for bipolar-additive consumers (push a fader up to add, down to subtract), and 0/1 are the extremes for one-directional consumers. A Signal output connects to both Signal/Mod inputs and orange param-arming inputs (Param and Signal are interchangeable at the cable level), so one fader can drive any parameter on any node. A fresh node starts with **4 sliders** ("Slider 1".."Slider 4").

### Editor

- **Faders** — drag to set the value (0..1); double-click a fader to reset it to 0.5. The value readout sits under (vertical) or beside (horizontal) each fader. The track's filled/unfilled colours are swapped relative to JUCE's default, so the colour grows from the high end of the throw.
- **Resize = precision.** The window is resizable, and the sliders stretch to fill it. A longer JUCE linear slider maps the same 0..1 range across more pixels, so a taller/wider fader is proportionally finer to drag. This is the intended way to get fine control: make the window bigger.
- **+ Slider** — append a fader (and its output pin). Capped at 32.
- **- Slider** — remove the *last* fader, its output pin, and any cables wired to it. Disabled at the 1-slider minimum. (Equivalent to the last fader's **X** button — a convenience that doesn't require reaching the specific fader.)
- **X** (per fader) — remove that fader, its output pin, and any cables wired to it. Disabled at the 1-slider minimum.
- **Rename** — double-click a fader's name label to rename it; the new name flows onto its output pin (so the cable endpoint label updates too).
- **Horizontal sliders** toggle — off (default) lays the faders out as vertical faders in a row, so window *height* sets their length; on stacks horizontal sliders, so window *width* sets their length.
- **Non-modal window.** The Control Bank editor (like the XY Pad and Signal Shape editors — the whole `SignalShape` input-node family) opens **non-modal**: it does not block the main window, the transport, or other editors. You can leave several open at once and ride multiple banks/pads live while the song plays. The editor is node-id-safe (it re-looks-up its node every access), so it stays valid even if the node is edited or deleted underneath it. (Genuinely blocking dialogs — confirmations, settings — stay modal.)

### Undo / dirty

Structural edits (add / remove / rename a slider, flip orientation) each push one `commitSnapshot` step. A fader drag commits a single *"Set control value"* step on release (continuous gesture → one undo step at the endpoint); wheel / typed-value edits commit immediately. A value move needs no graph rebuild — the processor reads the live `Param` value each block — but adding/removing/renaming a slider changes the pin set, so those call `onNodeEdited` → `requestRebuild`.

### Project file format

Nothing bespoke: the per-slider `Param`s and Signal output `Pin`s round-trip through the standard node param / pin serialisation, and the orientation lives in `node.script` (`__controlbank__` = vertical, `__controlbank__:h` = horizontal). On load the editor rebuilds its slider widgets from `node.params`, pairing each with the like-named output pin by position.

---

## Shared AHDSR envelope

The AHDSR envelope is the amplitude envelope for the synths whose voices are
driven by the shared `AHDSREnvelopeRuntime`: **Terrain Synth** (and its
wavetable / frequency-domain / wavelet-space variants), **Additive**,
**Phase Distortion**, **Spectral Grain**, and **Particle Cloud**. On Particle
Cloud the shared envelope is a **note-level VCA** over the whole grain cloud —
it sits *alongside*, not instead of, each grain's own attack/release window, so
you get cloud-level shaping (attack swell, sustain, release tail) on top of the
per-grain envelope. On those nodes it's edited two
ways, both opening the *same* editor on the *same* `node.ahdsrEnvelope` (the
single source of truth — there are no separate Attack/Decay/Sustain/Release
params on the node):

- **Right-click the node → *Envelope (AHDSR)…*** — works on every tonal synth,
  including ones with no dedicated editor dialog (plain Terrain, Additive, PD).
- **An *Envelope…* button inside the instrument's own editor dialog** — the
  Wavetable / Layered editor and the Frequency-Domain (spectral) editor each
  carry an *Envelope…* button in their top toolbar that pops the envelope
  editor in a separate window, so you don't have to close the instrument
  editor and hunt for the node's right-click menu. It's a separate dialog by
  design — folding A/H/D/S/R, six sliders, three tension knobs, three curve
  editors and the preset library into the already-dense wavetable editor would
  bloat it. (The Frequency-Domain editor only shows the button when opened on
  a node; in its sub-editor role inside a wavetable cell there is no node
  envelope to edit, so the button is hidden.)

The **FM** synth is a special case: it has **four** envelopes (one per
operator), not one node-level envelope, so the single-envelope editor above
would be the wrong shape for it. Instead it gets its own item — **right-click
the FM node → *Operator Envelopes (AHDSR)…*** — which opens a tabbed dialog
(**Op 1 – Op 4**) with one full AHDSR editor per tab. Each operator envelope is
the same model as everywhere else (Attack / Hold / Decay / Sustain / Release,
per-segment curves, tension, velocity sensitivity), so an FM operator can now
have a hold plateau, curved ramps, etc. — things the old per-operator linear
A/D/S/R couldn't do. The old `Op{i} A/D/S/R` param sliders are gone (replaced by
the envelopes); the `Op{i} Ratio` and `Op{i} Level` sliders stay on the node.
Projects saved before this change migrate automatically on load: the four
linear A/D/S/R settings become four AHDSR envelopes that reproduce the old
sound. **Velocity** note: the FM master output scales each voice by note
velocity once, so the per-operator envelopes default to **Velocity Sensitivity
= 0** (raising it on an operator adds the classic FM velocity→brightness
behaviour on top, which is intentional).

The remaining synths that carry their **own** amplitude envelope inside the
engine — **Drum** (per-sound) — plus the sample/region players **SoundFont**,
**SFZ**, **Sfizz**, and **MultiSampler** do **not** expose the shared editor,
because editing it would be inert. (Particle Cloud used to be in this list, but
it now layers the shared envelope on as a note-level VCA — see above.) A shared
master-VCA stage that would let the remaining synths honor a node-level AHDSR
too is tracked as future work in `known-issues.md`. Raw plugin-hosting
Instruments keep their envelope inside the plugin.

### Editor controls

- **Five stages**:
  - **Attack** — fade-in time from silence to the peak level.
  - **Hold** — a flat plateau at the peak level before the decay starts.
    Useful for organ stabs and pad attacks. Set the Hold *time* to 0 for a
    classic ADSR shape (the hold stage is skipped entirely).
  - **Decay** — time for the volume to fall from peak to the sustain level.
  - **Sustain** — the level the note holds at while the key stays pressed
    (0..1, where 0 means the note dies after decay and 1 means it sits at
    full volume forever).
  - **Release** — fade-out time after the key is released.
- All times are in milliseconds; sustain is a 0..1 level. Six vertical
  sliders span the bottom of the editor (the sixth is **Velocity
  Sensitivity**: 0 = organ-like uniform volume, 1 = piano-like — how hard
  you press the key scales the envelope's peak amplitude).
- Each slider carries a **two-line label**: the canonical term in bold
  (Attack / Hold / Decay / Sustain / Release / Velocity) with a small dim
  plain-language descriptor under it — *(fade in)*, *(hold at peak)*, *(fall
  to sustain)*, *(held level)*, *(fade out)*, *(touch response)*. SEANCE is
  built for non-musicians, so the descriptor names what the control does
  without making the user hover; the full sentence still lives in the slider
  tooltip. (The sixth slider's term reads **Velocity** rather than the older
  "Vel Sens" abbreviation.)
- Time sliders are skewed so the bottom half of the throw maps to the
  0-100ms range musicians actually want fine control over; a linear
  0-10000 slider would shove all useful values into a few pixels.

### Per-segment tension (curve-bend) knobs

Between the sliders and the per-segment *…Curve…* buttons sit three rotary
**tension** knobs — **A Curve**, **D Curve**, **R Curve** — one for the
Attack, Decay, and Release ramps. Each is the single-linear-control way to
reshape a ramp without opening the full curve editor, matching how
hardware/virtual-analog synths expose one "curve"/"slope" knob per stage:

- **Centre (0)** = a straight line. Double-click the knob to snap back to 0.
- **Turn right (toward +1)** = a *slow start* that accelerates — the classic
  "exponential" attack / decay / release where the level lingers near the
  start of the segment then rushes to the end (ease-in).
- **Turn left (toward −1)** = a *fast start* that eases out — the level jumps
  early then settles into the end value (logarithmic).

The math is a normalized exponential time-warp,
`warp(t) = (e^{k·t} − 1)/(e^{k} − 1)` with `k = tension·6`, applied to the
*input* (time axis) of the segment's curve before it's baked. This is the
"optimal" single-knob shaper in the sense that one parameter sweeps the whole
concave ↔ linear ↔ convex range continuously, with a true linear midpoint and
the segment's start/end levels pinned (the warp fixes `warp(0)=0`,
`warp(1)=1`). Because it warps the *timing* of whatever curve the segment
holds rather than replacing it, **tension composes with a custom curve**:
freehand-draw or type an equation for the shape, then bend its timing with the
knob; tension 0 leaves any authored curve exactly as drawn.

Tension is stored per segment in `node.ahdsrEnvelope` (`attackTension`,
`decayTension`, `releaseTension`, each `−1..1`) and round-trips through the
`ahdsrv1:` encoding via optional `at`/`dt`/`rt` fields — projects written
before tension existed load with all three at 0 (linear), so their sound is
unchanged.

### Per-segment shape curves

Attack, Decay, and Release each have their own *…Curve…* button that
opens the same three-mode editor used elsewhere in SEANCE:

- **Equation** — type a formula in terms of `x` (the normalized stage
  position `0..1`). The language dropdown offers **Built-in**, **Lua**,
  **Python**, and **GLSL**, exactly as in the [layer Formula](#formula-authoring-language-built-in--lua--python--glsl)
  and [spectral-curve](#frequency-domain-spectral-synth) editors. In all
  four, `x` is the normalized position; `f` is available as an alias of
  `x`. Built-in evaluates live; Lua/Python/GLSL are baked to samples when you
  edit (a multi-line Lua/Python body must end with `return`; the GLSL body
  becomes the inside of `float shapeValue(...)`). Examples: `x`,
  `x^2`, `x^0.5`, `1-exp(-3*x)`, `0.5 - 0.5*cos(3.14159*x)`.
- **Drawn Points** — Catmull-Rom interpolation through user-placed control
  points.
- **Freehand** — per-sample painting.

The curve dialog also has quick-set buttons for the most common shapes:
**Linear**, **Fast→slow** (`x^2`), **Slow→fast** (`x^0.5`), **S-curve**
(`0.5 - 0.5*cos(π*x)`).

The **Attack**, **Decay**, and **Release** curves shape the rising/falling
ramp of their stage. **Hold** and **Sustain** are flat by definition (Hold
sits at peak, Sustain at the sustain level) and have no curve editor.

### Live preview

A waveform at the top of the editor shows the full envelope shape at the
current parameters, with vertical dividers and stage labels (A / H / D / S
/ R) along the top so you can see at a glance what your sound is going to
do over time. The preview includes a synthetic 18%-width "sustain hold"
segment so the sustain level is visible even when the actual hold time is
zero. The preview bakes each segment with its tension applied (the same
`AHDSREnvelope::bakeSegment` the audio engine uses), so what you see is what
you hear — turning a tension knob bends the drawn ramp in real time.

### Preset library

The preset library is project-independent and shared across every synth —
saving a preset in one Wavetable node makes it immediately available in
every other tonal synth in the project (and across all projects on this
machine).

- The library lives on disk at `<userdata>/SoundShop/EnvelopePresets.xml`,
  alongside `Preferences.xml`. It persists across projects and across
  SEANCE versions, including any deletions you've made.
- Factory starting points cover the obvious cases: *Default*, *Pluck*,
  *Pluck Long*, *Pad*, *Bass*, *Organ*, *Strings*, *Brass*, *Stab*. They
  are marked with a gold ★ in the dropdown and the manage dialog so
  they're visually distinct from anything you've authored or modified.
- Picking a preset from the dropdown snaps the sliders, curves, and
  velocity sensitivity to that shape. The dropdown groups factory
  presets above a divider and user presets below it, but selecting a
  preset doesn't lock the editor — you can keep tweaking afterwards, and
  the preset isn't modified until you explicitly save over it.

#### Save as preset…

Captures whatever you currently have configured into a new entry in the
library. Using the name of an existing preset overwrites it. If the
overwritten entry was a factory preset, the built-in marker is dropped
(the entry is no longer treated as factory — see the Restore semantics
below for how to get the original back).

#### Manage presets…

Opens a list with four action buttons:

- **Duplicate** — copies the selected preset under a new name (default
  `"<name> copy"`, auto-disambiguated to `"<name> copy 2"`, `"<name> copy
  3"`, etc. if the default is already taken). The text is editable before
  you commit. The original is left untouched. Useful when you want to
  tweak a factory preset without losing the original — duplicate it
  first, then edit the duplicate. The duplicate is always a user preset
  (no factory marker), even if the source was a factory preset, so the
  factory entry's "origin" is still tied to the original.
- **Rename** — renames the selected preset. Renaming a factory preset
  drops its built-in marker but preserves the hidden origin tag, so
  Restore Built-ins won't double up by re-adding the canonical entry
  underneath. Collisions append `" (2)"`, `" (3)"`, etc.
- **Delete** — removes the selected preset. Factory presets are no
  longer protected from deletion; deleting one simply removes it and
  clears its hidden origin tag, so Restore Built-ins is allowed to bring
  the original back.
- **Restore Built-ins** — re-adds factory presets that have been deleted
  outright. Restore is **strictly add-only**: it never overwrites,
  modifies, or replaces an entry that's still in the library, even if
  you've edited the values or renamed it. The dialog previews what
  Restore will actually do before you confirm: it counts how many
  presets would be added vs. how many can't be added because of a name
  collision with an unrelated entry, and tells you the difference. If
  nothing would change, Restore tells you that too and reminds you to
  delete the edited entry first if you want the original back.

#### Restore semantics in detail

Every preset carries a hidden `builtInOriginId` field that's set to the
canonical factory name (`"Pluck"`, `"Pad"`, ...) for entries that started
out as factory presets. The id is preserved across renames and edits and
is cleared only by deletion. Restore uses it as the source of truth for
"is this factory preset still represented in the library?":

1. For each canonical, if any existing preset carries `builtInOriginId ==
   canonical.name`, the canonical is considered still represented (even
   if the user renamed it `"MyPluck"` or saved different values over it)
   — Restore skips.
2. Otherwise, if no preset's name collides with the canonical name,
   Restore adds a fresh canonical copy.
3. Otherwise (the user has a same-named entry that isn't tied to this
   canonical's origin — e.g. they deleted `"Pluck"` and then created
   their own preset called `"Pluck"`), Restore skips with no change.
   Their entry is never clobbered.

To get the original `"Pluck"` back after editing it in place, you have
to delete the edited entry first; the explicit deletion is the signal
that clears the origin marker and unblocks restoration.

### Pressure input pin

Every tonal synth also has a **Pressure** control input pin auto-added to
its node. Wire any Signal source (an LFO, an envelope, an XY-pad axis, a
[MIDI Breakout](#midi-breakout-node) Pressure output) into it and the
per-voice volume swells with the signal value — the natural expressive
layer once the envelope's release stage isn't doing the work. When the pin
is left unwired, the synth falls back to the keyboard's own channel
pressure (aftertouch), so out of the box a pressure-sensitive keyboard just
works.

**Hover tooltip.** Resting the mouse over the pin shows a reminder of what
it does (0 = normal level, higher = louder; amount scaled by the node's
Aftertouch sensitivity) plus a wiring note: when the pin is wired it
**overwrites** (replaces) the keyboard's own channel pressure rather than
adding to it, so feeding a synth its *own* pressure back via a MIDI
Breakout is redundant — it overwrites the value with the same number (read
once per block instead of sample-accurately) and wastes the pin. It is
**not** a double-application (the override means the swell is applied once
either way); the earlier wording warning of a "double swell" was wrong for
this pin specifically and has been corrected. Tooltips are stored on
`Pin::tooltip` and surfaced by `NodeGraphComponent::getTooltip` (the
component is a `juce::TooltipClient`); the text is re-set on every graph
build, never serialized, so it always reflects the current code.

**Naming / migration.** This pin was historically labelled *Aftertouch*.
It is now **Pressure** (clearer for non-musicians: it scales loudness like
key pressure, and "Aftertouch" is MIDI jargon). Projects saved with the old
name are migrated **in place** by the graph builder — the pin keeps its id,
so any cable already attached to it survives the rename; only the label and
tooltip change. Match sites accept either name for safety.

Note the difference between the two pressure routes. The **control pin**
carries a single mono value, so it maps onto **channel pressure** (one
swell for every held note together) — exactly what a mono cable can
express. **Polyphonic key pressure** (per-note aftertouch, where one held
key can be pushed harder than its neighbour) *cannot* travel a mono cable,
because a lone value can't say which of several held notes it belongs to.
It's therefore consumed directly inside the synth voice allocator, matched
to each voice by note number, and **added** to channel pressure before the
sensitivity multiply (`effectivePressure()` in `signal_modulation.h`,
distributed by `distributeMpeMessages()`). Both end up scaling the same
per-voice volume swell.

The envelope's existing peak-level math is unchanged; pressure is a
separate multiplier on top. The aftertouch sensitivity (how strongly the
combined pressure scales the voice volume) defaults to 0.5 and is saved
per node.

**Pin ordering.** The Pressure pin is appended at graph-build time, so on
a wavetable synth it could end up *between* two Position inputs: a 1D
wavetable builds as `[MIDI, Mod: Position, Pressure]`, then adding a
second axis push-backs `Mod: Position Y` *after* the existing Pressure pin,
leaving it wedged in the middle. Both the graph builder (`graph_processor.cpp`)
and the wavetable editor's `syncPositionModPins` (`layered_wave_editor.cpp`)
run a `std::stable_partition` that keeps the single Pressure pin
**after** every other input pin while preserving all other pins' relative
order, so the input row always reads `[MIDI, Position X, Position Y, …,
Pressure]`. It runs on every build, so projects saved with the old wedged
layout are normalized on load. Links reference pins by id, never by index,
so the reorder never breaks a cable.

### MIDI Breakout node

**MIDI Breakout** (right-click → *Signal Shape* → *MIDI Breakout (MIDI →
signals)*) taps a live MIDI stream and re-emits its expression controllers
as block-rate control signals, so you can route any of them anywhere a
control cable is accepted — a filter cutoff, a wavetable position, a
*different* synth's Pressure input, an effect knob. It has one **MIDI In**
pin and four Signal outputs, in the order the processor writes them:

| Output | Range | Source |
|--------|-------|--------|
| **Velocity**   | 0..1 | last note-on velocity, held until the next note |
| **Pressure**   | 0..1 | channel aftertouch, or the latest poly key-pressure |
| **Mod Wheel**  | 0..1 | MIDI CC 1 |
| **Pitch Bend** | 0..1 | 14-bit wheel normalized, **0.5 = centre** (down = 0, up = 1) |

Pitch Bend's 0.5-centre convention lines up with a param's **Modulate**
mode (where 0.5 = no change); use **Absolute/Set** mode to map it
edge-to-edge across `[min,max]`. Values are held across blocks, so an
unchanging controller keeps emitting its last value rather than snapping to
zero between events. Implemented by `MidiBreakoutProcessor`
(`midi_breakout_node.h`); the node is `NodeType::MidiBreakout`, tagged
`__midibreakout__`, and writes each output to control channels 2.. (sized
by `widenForControl`).

**Redundancy caveat (in every output's hover tooltip).** Every tonal synth
already reads pressure / pitch-bend / mod-wheel from its *own* MIDI input,
so fanning the same MIDI into both a synth **and** a Breakout and then
wiring a Breakout output back into that same synth is redundant — but the
*kind* of redundancy differs by output, and only one of them actually
doubles:

- **Pressure** → a synth's **Pressure input pin overwrites** (replaces) the
  keyboard's own pressure with the wired signal, so looping it back is
  harmless but pointless: it overwrites the value with the same number (and
  downgrades it to a once-per-block read). **Not** a double-application.
- **Mod Wheel / Pitch Bend** → a synth has no input pin for these; it bends
  pitch and vibratos straight from MIDI. If you wire one of these into a
  *modulation* pin that drives the same thing, it stacks on top of the
  synth's own handling and **is** applied twice.

Either way the node is for sending a controller somewhere it would not
otherwise reach — a filter cutoff, a wavetable position, a *different*
synth, an effect knob — not for re-driving the synth that already gets it.
(There is intentionally no MIDI *output* on this node; it is a pure
MIDI-to-control tap.)

## Voice container (per-voice polyphony)

The **Voice container** is a node that holds an **inner subgraph — a per-note
patch — and instantiates it once per sounding MIDI note**, summing every active
copy. It is SEANCE's answer to a Bitwig-Grid-style "poly" wrapper: instead of
polyphony being sealed inside a single monolithic instrument, you build a voice
out of ordinary graph nodes (oscillators, filters, envelopes, Script nodes, …)
and the container makes that little patch polyphonic. Design rationale and the
full milestone plan live in `poly-voice-architecture.md` at the repo root.

### What it looks like on the canvas

- **Add Node → Instruments → Voice subgraph (per-note polyphony)** opens a
  submenu of **factory presets** (see below). The submenu is labelled "subgraph"
  (with a disabled "cloned per note, like Bitwig's Poly Grid" hint line) so it's
  findable by anyone hunting for modular/per-note polyphony. It only appears at
  the **top level** — you cannot nest a Voice container inside another one in M1.
- On the main canvas the container is a **single node** with one **MIDI input**
  (left) and one **stereo audio output** (right). Because node colour is inferred
  from pins (`getVisualCategory`), a MIDI-in / audio-out node reads as an
  **instrument** automatically — no special-case colour. Wire a MIDI source
  (Timeline, Computer Keyboard, MIDI Input, …) into it and its audio out to a
  Mixer / Output exactly like any built-in synth.
- Every preset ships with a complete inner patch so the container makes sound
  immediately — there is no "empty" Voice container.

### Factory presets

The **Voice subgraph (per-note polyphony)** submenu offers ready-made voices so
you don't have to wire an inner patch by hand. Each entry builds the full container shell (VoiceIn
puck + inner instrument + VoiceOut puck) **and** pre-tunes the container's
polyphony / glide / unison settings, then drops it on the canvas as one undo
step ("Add Voice container (*name*)"). Pick one and play — drill in afterwards to
customise.

| Preset | Inner instrument | Voices | Unison | Glide | Character |
|---|---|---|---|---|---|
| **Basic (FM Synth)** | FM Synth (`__fmsynth__`) | 8 | 1 | — | The original neutral starting point; a 4-op FM synth driven by VoiceIn's **MIDI** fork. Best base for "I'll build my own patch." |
| **Warm Pad** | Signal Osc (triangle) | 8 | 3 @ 8¢ | — | Mellow triangle with a slow 400 ms swell and 900 ms release; 3-voice unison for width. |
| **Pluck** | Signal Osc (saw) | 8 | 1 | — | Fast saw attack, no sustain, short release — a percussive pluck. |
| **Supersaw Lead** | Signal Osc (saw) | **1 (mono)** | **7 @ 25¢, full spread** | **50 ms** | Monophonic gliding lead with a fat 7-voice supersaw stack spread hard across the stereo field. |
| **Noise Perc** | Signal Noise (white) | 8 | 1 | — | Gated white-noise burst with a short percussive envelope — snare/hat-style hits. |

The construction itself lives in the free function **`buildVoicePreset(graph,
pos, presetId)`** (`node_graph.cpp`), not in the GUI, so the *same* code path is
exercised headlessly by the self-test (`testVoicePresets`). The GUI menu wrapper
(`NodeGraphComponent::createVoicePreset`) only adds the post-build side effects
the data model can't own: the undo snapshot and the audio-graph rebuild. "Basic"
is preset id 0; the named presets are 1–4. Unknown ids fall back to Basic.

### Drilling in — the scoped inner editor

- **Double-click the container** to drill into its inner graph. The editor
  enters a **scoped view**: it draws and hit-tests **only** the nodes that belong
  to that container (`viewScope` in `node_graph_component.cpp`; `nodeVisible(n)`
  ⇔ `n.voiceContainerId == viewScope`, `linkVisible(l)` ⇔ both endpoints
  visible). Top-level nodes are hidden while you're inside; inner nodes are hidden
  while you're at the top.
- A **breadcrumb chip** (violet, top-left, "← Root / *name*") shows you're inside
  a container. **Click the chip** or press **Esc** to pop back out to the top
  level. The same scoped component is what would back a future detached pop-out
  window.
- Selection, hover, drag, rubber-band, "fit all", and link hit-testing all
  respect the scope — you can only touch what you can see. **Global** passes
  (save/load, audio-graph rebuild, latency/PDC walk) deliberately iterate the
  **whole** graph regardless of scope, so the hidden nodes still process audio
  and still get serialized.
- **Nodes you create while scoped are auto-stamped into the container.** Any node
  added from the right-click menu while `viewScope != -1` gets
  `voiceContainerId = viewScope`, so it joins the patch you're editing rather than
  landing at the top level. *(Known M1 gap: nodes created through an **async file
  chooser** — hosted plugins, WASM modules, SoundFonts — currently land at the
  top level even when you're scoped, because the chooser callback runs after the
  scope-stamp pass. Drag them in or recreate them at the right level for now.)*

### The boundary pucks: VoiceIn and VoiceOut

Inside the container the patch is bounded by two special nodes, mirroring JUCE's
`AudioGraphIOProcessor` I/O nodes and Max/PD inlets/outlets:

- **VoiceIn** (left puck) — the origin of the per-note context. It has these
  outputs:
  - **MIDI** — this voice's note forwarded as a real MIDI stream (note-on at the
    allocation offset, note-off on release). This is **fork (a)**: it lets any
    existing MIDI-driven synth (FM, Waveform, SoundFont, a hosted plugin, …) work
    inside a voice **unmodified** — the default patch uses exactly this.
  - **Pitch** (Signal, Hz) — the note's frequency, written every sample.
    **Includes per-note pitch bend** (MPE / channel pitch wheel folded in
    multiplicatively, so a glide and a bend compose cleanly).
  - **Gate** (Signal, 0/1) — `1.0` while the note is held, `0.0` after note-off.
  - **Velocity** (Signal, 0..1) — the note-on velocity, latched for the note.
  - **Pressure** (Signal, 0..1) — per-note pressure, `0` at rest. Driven by MPE
    channel pressure (or polyphonic key pressure / aftertouch on the note's
    channel). Wire it into a filter cutoff, an amp VCA, a wavetable position, … to
    make pressing harder open/brighten/swell that one note.
  - **Timbre** (Signal, 0..1) — per-note timbre, `0.5` at rest (centre). Driven by
    MPE **CC74** ("slide" / the Y axis on an MPE controller). The canonical
    second expression axis: wire it wherever a per-note tone-colour control fits.

  The Pitch/Gate/Velocity/Pressure/Timbre outputs are **fork (b)**: the
  modular-synthesis path, consumed by nodes that read control Signals directly
  (see Signal Oscillator below). They arrive on control channels 2/3/4/5/6 of the
  buffer, the standard Signal-pin-as-extra-channel mechanism.

  **MPE / per-note expression.** `PolyVoiceProcessor` parses pitch wheel, channel
  pressure, polyphonic key pressure, and CC74 out of the incoming MIDI and routes
  each to the matching voice **by MIDI channel** (the channel the note was
  allocated on is remembered per slot). A message on a **member channel** (2–16)
  reaches only the voice(s) on that channel — true per-note expression from an MPE
  controller, where every held note lands on its own channel. A message on the
  **master channel 1** (also the non-MPE case, where a piano-roll/keyboard puts
  everything on channel 1) **broadcasts to all voices** — so an ordinary mod/bend
  still works as a global gesture. Pitch-bend range is the MPE default **±48
  semitones** on member channels and the conventional **±2 semitones** on channel
  1. The three dimensions are smoothed with a short (~5 ms) one-pole so continuous
  controllers don't zipper, and **reset to neutral on every fresh note-on** so one
  note's expression never bleeds into the next note that reuses the voice. None of
  this needs configuration — patch the Pressure/Timbre outputs and play.
- **VoiceOut** (right puck) — the audio sink for the patch. Whatever you wire into
  it is this voice's contribution; the container sums VoiceOut across all active
  voices into its single output. It is mapped to the inner graph's output node the
  same way the top-level **Output** node is.

### The engine — `PolyVoiceProcessor`

The container is realized as **one** `juce::AudioProcessor` (`PolyVoiceProcessor`,
`poly_voice_processor.cpp`) inserted as a single node in the main JUCE graph, so
main-graph mixing, routing, and plugin-delay compensation treat it like any other
instrument. Internally:

- **N inner-graph clones.** It builds **N independent `GraphProcessor` clones**,
  each with `setBuildScope(containerId)` so `rebuildGraph` includes **only** that
  container's inner nodes/links (the pre-existing nodeMap membership guard
  auto-scopes the links — zero wiring duplication). All clones read the **same**
  `Node` objects for parameters; they differ only in DSP state (oscillator phase,
  filter memory, envelope stage) and per-voice context. Each clone runs via its
  inner `getGraph()->processBlock(...)` (not `GraphProcessor::processBlock`, which
  would re-inject the metronome and click).
- **Voice allocation & lifecycle.** The container parses its incoming
  `MidiBuffer`: a **note-on** allocates a free voice (or **steals** one per the
  selected steal mode when all N are busy — see below), sets that voice's VoiceIn
  pitch/velocity, raises its gate, and forwards the note as MIDI into the clone. A
  **note-off** drops that voice's gate; the voice keeps running so its envelope
  release tail finishes.
- **Voice stealing.** When every voice is busy and a new note arrives, one active
  voice must be sacrificed. Right-click the container → **Voice stealing** to pick
  the policy (radio-ticked to the current choice). A free voice always wins over
  stealing; the mode only decides *which active voice* loses:
  - **Steal oldest** (default) — reuse the longest-sounding voice. The musical
    default: the note you played first is usually the one you miss least.
  - **Steal quietest** — reuse the voice whose last block was quietest (lowest
    RMS). Least audible interruption, ideal for pads and long release tails where
    cutting a still-loud voice would be obvious.
  - **Cycle voices (round-robin)** — hand out voices in a fixed 0,1,2,… cycle,
    ignoring age and level. Predictable; handy for drum-style patches.

  The choice is stored in `node.voiceStealMode` (0/1/2) and re-read by
  `PolyVoiceProcessor` **every block**, so switching modes is audible immediately
  with no graph rebuild. It snapshots for undo and saves with the project. The
  policy itself lives in the JUCE-free `VoiceAllocator` (`voice_allocator.h`),
  unit-tested in isolation.
- **Glide (portamento).** Right-click the container → **Glide** to pick a slide
  time (Off / 20 / 60 / 150 / 400 ms, radio-ticked). When a sounding voice is
  **stolen** for a new note, its Pitch signal **slides** from the old note to the
  new one over the glide time instead of jumping — a ramp linear in log-frequency
  (constant semitones/sec), so an octave takes the same time as a tone. The **Gate
  still steps instantly**, so the envelope retriggers on time even while the pitch
  glides. A **fresh** voice (one that wasn't already sounding) has no meaningful
  previous pitch and so starts **on pitch with no glide** — which means glide only
  happens when notes **overlap** (classic fingered/legato portamento): with
  polyphony 1, overlapping notes glide and gapped notes don't. The value lives in
  `node.voiceGlideMs` (float, default 0), is re-read live each block by
  `PolyVoiceProcessor` (passed to `VoiceIn` only on a steal), snapshots for undo,
  and saves with the project. Implemented as a portamento ramp inside
  `VoiceInProcessor` that persists across blocks (a glide longer than one buffer
  keeps sliding).
- **Unison.** Right-click the container → **Unison** to stack several **detuned,
  stereo-spread** copies of the voice per note for a thicker, wider sound. The
  submenu sets the **count** (Off / 2 / 3 / 4 / 6 / 8, radio-ticked), a **Detune**
  amount (0 / 6 / 12 / 25 / 50 cents — disabled while Off), and a **Stereo spread**
  (Mono / Narrow 33% / Wide 66% / Full 100% — also disabled while Off). A struck
  note allocates a whole **stack** of that many voice slots (`VoiceAllocator::
  noteOnGroup`, all sharing one `group` id so the note-off releases the stack
  together via `noteOffGroup`); each slot is detuned symmetrically across ±the
  detune amount and balance-panned across ±the spread, with a `1/√count`
  normalisation so the perceived level stays steady as you add voices. Detune
  rides into the **Pitch** signal alongside MPE bend (`VoiceInProcessor::
  setUnisonDetune`), so it composes with glide and pitch bend. **Unison consumes
  polyphony**: a 4-voice unison on an 8-voice container plays two notes at once,
  and a stack that needs more slots than remain steals a whole older stack. The
  count clamps to the slot count. All three fields (`voiceUnison`,
  `voiceUnisonDetune`, `voiceUnisonSpread`) are re-read live each block (no
  rebuild → no voice glitch), snapshot for undo, and save with the project.
- **Voice-free detection.** A voice is reclaimed once its gate is released **and**
  its output RMS has stayed below a floor (`kFloorRms = 1e-4`) for `kFreeMs = 250`
  ms. CPU therefore scales with **active** polyphony, not N — idle voices are
  skipped.

### Signal Oscillator (the fork-(b) instrument)

**Add Node → Signal Shape → Signal Oscillator (pitch/gate → tone)**
creates a `SignalOscillatorProcessor` (`signal_oscillator.h`, dispatched as
`NodeType::Instrument` with `script == "__signalosc__"`). It is the first node
that **takes its note from control Signals instead of a MIDI stream**, which makes
the VoiceIn Pitch/Gate/Velocity outputs real rather than decorative:

- **Inputs (all Signal):** **Pitch** (Hz, read every sample → oscillator
  frequency), **Gate** (0/1 — the envelope fires note-on on the rising edge and
  note-off on the falling edge), **Velocity** (0..1 — latched on the gate's rising
  edge, scales the envelope).
- **Params:** **Waveform** (0 = sine, 1 = saw, 2 = square, 3 = triangle, 4 =
  pulse) — a **discrete enum** edited by a **click-to-pick popup** (like the
  Signal Filter's Type), not a drag-scrubbed slider, so you can't park it between
  shapes; **Volume**; and **Pulse Width** (0.05..0.95, default 0.5). Pulse Width
  is the duty cycle of the Pulse waveform (0.5 = a square, away from 0.5 = a
  thinner, more nasal pulse). It only affects the Pulse shape but is always
  present so it can be **modulated via #88** — right-click its row → *Add
  Modulation Input* and wire a Signal LFO into it for classic **PWM** movement.
- **Envelope:** it uses the **shared AHDSR** (`node.ahdsrEnvelope`), so
  right-click → **Envelope (AHDSR)…** opens the normal envelope editor on it with
  no special-casing (it isn't in the `ownEnvelope` exclusion list, so it counts as
  a tonal synth). Default envelope: A 5 ms / D 100 ms / S 0.7 / R 300 ms.
- **Monophonic by design.** It is deliberately a one-voice oscillator — polyphony
  comes from the **container** cloning the patch, not from the oscillator. Dropped
  outside a Voice container it still works as a standalone Signal-controlled tone
  generator (wire any Signal source into Pitch/Gate/Velocity).

To build a fully modular voice, drill into a Voice container, delete the default
FM synth, drop in a Signal Oscillator, and wire **VoiceIn Pitch → Pitch**,
**VoiceIn Gate → Gate**, **VoiceIn Velocity → Velocity**, then **Signal Oscillator
audio → VoiceOut**.

### Modular kit — signal utilities (M3)

Beyond the Signal Oscillator, a growing **kit of small modules** lets you build
modulation, shaping, and sources inside a voice (or anywhere signals flow) out of
native nodes instead of a script. The control-signal utilities (Math, LFO, Sample
& Hold, Logic) are tiny, stateless-or-near-stateless `NodeType::SignalShape` nodes
(signal-family orange), tagged by a `script` string and dispatched in
`GraphProcessor::createNodeProcessor`; they have **no editor** (params are edited
on the node face, so double-clicking is a no-op). Two members are different node
kinds because they handle audio rather than control signals: the **Signal Filter**
is a blue `NodeType::Effect`, and the **Signal Noise** generator is a brown
`NodeType::Instrument` (it carries the shared AHDSR envelope, like the Signal
Oscillator). Both are documented below alongside the utilities.

#### Signal Math (`__signalmath__`)

**Add Node → Signal Shape → Signal Math (A op B)** creates a
`SignalMathProcessor` (`signal_math.h`). It combines two control Signals
sample-by-sample with a selectable operation:

- **Inputs (Signal):** **A** (channel 2) and **B** (channel 3). An **unwired
  input reads as 0** — the honest modular convention. So Subtract with **A**
  unwired negates **B** (`Out = -B`, a one-input inverter); Multiply with either
  input unwired is silent.
- **Output (Signal):** **Out** (channel 2) = `A op B`, computed every sample.
- **Param — Operation** (discrete enum, popup picker on the node face, not a drag
  slider): **0 Add** (`A+B`), **1 Subtract** (`A-B`), **2 Multiply** (`A*B` — a
  per-sample VCA / ring-mod / scaler), **3 Divide** (`A/B`, with **B = 0 → 0** so
  no NaN/inf escapes), **4 Min**, **5 Max**. Any non-finite result is clamped to 0.
- A constant operand is **not** baked in; wire a dedicated constant/control source
  to one input. This keeps each operation's semantics clean (no per-op "what does
  the default constant mean" ambiguity).

The node's audio bus (channels 0/1) is always cleared on output so it never leaks
the raw input Signals downstream as audio. Save/load is the generic node path
(type + `script` + params + pins all round-trip); covered by `testSignalMath` in
`--self-test` (each operation, divide-by-zero safety, unwired-input behavior, the
silent audio bus, and the Operation-param round-trip).

#### Signal LFO (`__signallfo__`)

**Add Node → Signal Shape → Signal LFO (modulation source)** creates a
`SignalLFOProcessor` (`signal_lfo.h`) — a control-rate oscillator for
modulation:

- **Input (Signal):** **Sync** (channel 2), optional. A **rising edge** (≥ 0.5)
  resets the phase to 0. Wire a Voice container's **VoiceIn Gate → Sync** and the
  LFO **retriggers at the start of every note**; unwired (reads as 0) it
  free-runs. Because each voice clone owns its own `SignalLFOProcessor`, per-voice
  LFOs have independent phase.
- **Output (Signal):** **Out** (channel 2) — the waveform.
- **Params:** **Rate** (Hz, 0.1–20, continuous slider); **Shape** (popup enum: 0
  sine, 1 triangle, 2 saw, 3 square); **Polarity** (popup enum: 0 **bipolar**
  −1…+1, 1 **unipolar** 0…1). Unipolar is the convenient form for driving a 0..1
  parameter; bipolar suits pitch/pan around a center.

Phase persists across blocks (it's stateful). The audio bus stays silent.
Covered by `testSignalLFO` in `--self-test` (waveform values, sine bounds,
bipolar vs unipolar range, mid-block sync reset, and the param save/load
round-trip).

#### Sample & Hold (`__signalsh__`)

**Add Node → Signal Shape → Sample & Hold (stepped/random)** creates a
`SampleHoldProcessor` (`signal_sample_hold.h`). On each trigger it latches a
value and holds it until the next trigger — the classic source of stepped and
random-per-note modulation:

- **Inputs (Signal):** **In** (channel 2 — the value sampled in Input mode) and
  **Trigger** (channel 3 — a **rising edge** ≥ 0.5 takes a sample). Wire a Voice
  container's **VoiceIn Gate → Trigger** for one fresh value per note.
- **Output (Signal):** **Out** (channel 2) — the most recently held value.
- **Param — Source** (popup enum): **0 Input** (hold the In signal), **1 Random
  ±1** (hold a fresh random value in −1…+1, ignoring In), **2 Random 0…1** (random
  in 0…1, ignoring In). The built-in random source means "random per note" works
  with **nothing wired to In** — just feed the gate to Trigger.

Each voice clone owns its own instance **and its own RNG state**, so per-voice
random values vary between voices. Covered by `testSampleHold` in `--self-test`
(input sampling + hold across two triggers, random modes ignoring In and staying
in range, hold steadiness, the silent audio bus, and the Source param round-trip).

#### Signal Logic (`__signallogic__`)

**Add Node → Signal Shape → Signal Logic (compare / gate)** creates a
`SignalLogicProcessor` (`signal_logic.h`). It turns two control Signals into a
clean **0/1 gate** every sample — for thresholding a signal or combining gates. A
boolean input is read as "true when ≥ 0.5":

- **Inputs (Signal):** **A** (channel 2) and **B** (channel 3 — threshold / second
  operand; B unwired = 0).
- **Output (Signal):** **Out** (channel 2) — a hard 1.0 or 0.0.
- **Param — Operation** (popup enum): **0 A > B** (gate while A is above the
  threshold B), **1 A < B**, **2 A AND B**, **3 A OR B**, **4 A XOR B** (exactly
  one true), **5 NOT A** (invert A; ignores B).

Because the output is a hard gate it composes cleanly with anything that wants
one — a Sample & Hold's Trigger, a Signal Oscillator's Gate, another Logic input.
Stateless; the audio bus stays silent. Covered by `testSignalLogic` in
`--self-test` (every operation true/false, the silent audio bus, and the
Operation param round-trip).

#### Signal Filter (`__signalfilter__`)

**Add Node → Effects → Signal Filter (resonant LP/HP/BP)** creates a
`SignalFilterProcessor` (`signal_filter.h`). Unlike the four utilities above it is
a true **audio Effect** (blue node, **Audio In → Audio Out**), not a control-Signal
node — it's the modular kit's actual filter. It's a hand-rolled **TPT
state-variable filter** (Andrew Simper / Cytomic topology), the same numerically
stable form used for clean cutoff sweeps:

- **Param — Type** (popup enum): **0 Low-pass** (keep lows), **1 High-pass** (keep
  highs), **2 Band-pass** (keep a band centred on the cutoff).
- **Param — Cutoff** (20–20 000 Hz, shown as `… Hz`): the corner frequency. Clamped
  internally to 0.45 × sample-rate so it never reaches Nyquist (where `tan()` blows
  up).
- **Param — Resonance** (0–1): emphasis right at the cutoff. Maps to filter **Q
  0.5–10**; at 1.0 it's a sharp resonant peak that can ring/whistle. The filter
  stays finite even at max resonance + an impulse (state is NaN-flushed each block).

**Modulating the cutoff is done the proper SEANCE way — via the on-demand
modulation-pin mechanism (#88), not a hardcoded signal pin.** Right-click the
**Cutoff** (or **Resonance**) param row → **Add Modulation Input (Mod)** grows a
`Mod: Cutoff` control pin; wire an LFO / envelope / Sample & Hold / any Signal to it
and it swings the cutoff around the knob's setting (block-rate). This mirrors Signal
EQ and deliberately avoids a bespoke "Cutoff" input pin (CLAUDE.md #88 rule). Filter
state (`ic1eq`/`ic2eq`) is per audio channel and persists across blocks, so a
per-voice clone inside a Voice container gets its own independent filter — each note
can have its cutoff swept independently. Covered by `testSignalFilter` in
`--self-test` (LP passes lows / rejects highs, HP rejects DC / passes highs, BP
rejects DC and peaks at cutoff, high-resonance stability, and the Type/Cutoff/
Resonance save-load round-trip).

#### Signal Noise (`__signalnoise__`)

**Add Node → Signal Shape → Signal Noise (gate → noise burst)** creates a
`SignalNoiseProcessor` (`signal_noise.h`). It's the **noise counterpart to the
Signal Oscillator** — a gated, enveloped monophonic source — so it's a brown
`NodeType::Instrument`, not a control utility. Noise has no pitch, so it reads two
control Signals (instead of the oscillator's three) and turns them into an
enveloped burst:

- **Inputs (Signal):** **Gate** (channel 2 — note on while ≥ 0.5, release on the
  falling edge, drives the AHDSR) and **Velocity** (channel 3 — latched on the gate
  rising edge, scales the envelope).
- **Output (Audio):** stereo noise on channels 0/1, with **decorrelated left/right**
  channels for natural stereo width.
- **Param — Type** (popup enum): **0 White** (flat spectrum, brightest), **1 Pink**
  (−3 dB/octave, warmer — Paul Kellet's economical 7-pole filter), **2 Brown**
  (−6 dB/octave, darkest — a leaky integral of white).
- **Param — Volume** (0–1).
- **Amplitude envelope:** the shared node AHDSR (`node.ahdsrEnvelope`), edited via
  right-click → **Envelope (AHDSR)…** exactly like any tonal synth. The default is a
  short percussive hit (1 ms attack, 120 ms decay, 0 sustain, 80 ms release) rather
  than the oscillator's sustained shape — so a bare Signal Noise already sounds like
  a snare/hat without touching the envelope.

Because it carries a gate + envelope it's a **drop-in voice primitive**: wire
VoiceIn's Gate/Velocity into it inside a Voice container and you have a noise voice
(snares, hats, wind, breath, percussion). Run its output through a **Signal Filter**
(modulating the cutoff with an envelope or LFO) for tuned-noise and resonant-sweep
timbres. Each per-voice clone keeps its own RNG and filter state, so stacked notes
don't share a noise stream. Covered by `testSignalNoise` in `--self-test` (gate-on
produces output in all three colours, white is near-uncorrelated while brown is
strongly low-pass / correlated, output stays in [−1, 1], gate-off releases to
silence, and the Type/Volume save-load round-trip).

### Save / load, dirty tracking, undo

Inner nodes and links serialize through the **same** generic path as any node —
they just carry a `voiceContainerId` membership field that ties them to their
container, plus the container's polyphony count. Container creation and all
inner-graph edits go through the normal `commitSnapshot()` snapshot-undo path
(they are topology changes), so they participate in dirty tracking and Ctrl+Z
exactly like adding or rewiring any other node.

### Known M1 limitations

These are documented design boundaries for the first milestone, not bugs:

- **Fixed N** chosen at creation (default 8); no live re-voice slider yet.
- **No nested containers** — a Voice container can't live inside another.
- **Async-file-chooser nodes land at top level when scoped** (see the scoped-editor
  note above).

> **M2 progress:** voice stealing now offers oldest / quietest / round-robin (was
> oldest-only in M1) — see **Voice stealing** above; **glide (portamento)** slides
> the pitch when a voice is stolen — see **Glide** above. Gates are now
> **sample-accurate**: a note-on/off in the Signal fork lands on its exact
> within-block sample offset (VoiceIn writes Pitch/Gate/Velocity as a
> piecewise-constant ramp rather than a flat per-block constant), matching the
> MIDI fork which was already sample-accurate.

The allocation/lifecycle policy and the end-to-end audio path are both covered by
`--self-test`: `testVoiceAllocator` checks free-slot allocation, all three steal
modes (oldest / quietest / round-robin, including round-robin cursor reset and the
"free slot wins over a steal" rule), note-matched release, and RMS free-detection on
the pure `VoiceAllocator`; `testVoiceInSignals` drives a `VoiceInProcessor` directly
and asserts the Pitch/Gate/Velocity edges land on their exact within-block sample
offset (including multiple segments per block, carry across blocks, reset, and the
glide ramp — start, monotonic slide, snap-to-target, hold, and a glide spanning
multiple blocks); `testVoiceMpe` covers the per-note expression path in three
parts — the `VoiceInProcessor` expression signals (neutral rest values, smoothing
toward target, +12-semitone bend doubling the Pitch signal, and the reset-to-
neutral on a fresh note-on), the end-to-end routing through `PolyVoiceProcessor`
(a member-channel bend raises only the matching voice ≈2×, a bend on a different
channel leaves it alone, and a master-channel-1 bend broadcasts to it), and the
load migration that grows an old 4-output VoiceIn its Pressure/Timbre pins;
`testVoiceUnison` covers the unison stack — the `VoiceAllocator` group API
(allocating a stack that shares one group id, releasing the whole stack on
note-off, clamping to the polyphony, and channel-matched release so two same-note
stacks on different MPE channels stay independent), the end-to-end audio (a
full-spread unison decorrelates the L/R channels and the note frees the whole
stack to silence on release), and the `voiceUnison`/`Detune`/`Spread` save/load
round-trip; and `testVoiceContainerAudio` builds a real container
(VoiceIn → Signal Oscillator → VoiceOut), drives it with a synthetic MIDI buffer
through `PolyVoiceProcessor`, and asserts a held note makes a tone, three notes sum
louder than one, and the voices decay back to silence after release. The modular
kit has its own coverage: `testSignalMath` runs every Signal Math operation,
checks divide-by-zero safety, unwired-input (=0) behavior, the silent audio bus,
and the Operation-param save/load round-trip; `testSignalLFO` checks LFO waveform
values, sine bounds, bipolar/unipolar range, mid-block sync reset, and its param
round-trip; `testSampleHold` checks input sampling and hold across triggers, the
random modes' range and In-independence, and its Source param round-trip;
`testSignalLogic` checks every comparison/boolean operation and its param
round-trip; and `testSignalFilter` runs the resonant filter in all three modes
(low-pass passes a 50 Hz tone and rejects 8 kHz, high-pass rejects DC and passes
8 kHz, band-pass rejects DC and peaks at the cutoff), confirms a high-resonance
impulse stays finite, and round-trips the Type/Cutoff/Resonance params; and
`testSignalNoise` confirms a held gate produces output in all three colours, that
adjacent-sample correlation rises white < pink < brown (brown being heavily
low-pass), that output stays in [−1, 1], that the gate releases to silence, and the
Type/Volume round-trip.

## Asset library (project stores)

The **asset library** is a per-project collection of reusable building blocks
that can be **published once and referenced from many places at once**. Unlike
the app-global *preset* systems (e.g. the AHDSR preset manager), the asset
library lives **inside the project file**.

> **⚠ MODEL IN TRANSITION (2026-06-14) — partially migrated, INCOMPLETE.**
> The library is moving from a *bidirectional live-reference* model (any
> referencing node could edit the shared asset, propagating to all references)
> to a **fork-by-default + opt-in read-only link** model:
> - **Loading** a library asset into a node **forks** by default — the node gets
>   an **independent copy**, with no link.
> - **Syncing** is an explicit opt-in (the **Sync with library curve
>   (read-only)** menu item) that makes the node a **live, read-only mirror** of
>   the asset. A synced curve can't be edited in the node; to diverge you **click
>   the panel's read-only badge** (*linked – click to edit a copy*), which breaks
>   the link and forks an independent copy, or you edit the shared item **in the
>   library** (the only sanctioned action-at-distance — propagates to all active
>   links). Unlinking is intentionally **not** a library-menu item: the popup only
>   does library operations (publish / load / sync), and unlinking is a node-state
>   action surfaced on the panel badge.
> - Consumers **never write back** to the asset on edit.
>
> **Migrated so far (sanity-check slice):** the three **FrequencyGraph**
> consumers — **Curve EQ**, **Spectral FFT** mag/phase, **Spectrum Tap** per-bin
> response. These now fork on load, lock the curve panel when synced, and surface
> the fork-a-copy action on the panel's read-only badge.
>
> **Still on the OLD bidirectional model (not yet migrated):** **Waveforms**,
> **AHDSR Curves**, **Morph Algorithms**. The descriptions in *Identity, ids, and
> the live-reference model* below still describe the old behavior for those kinds.
>
> **Library-side editor (added 2026-06-14):** the **Frequency Graphs** tab has an
> **Edit…** button that opens the stored curve in a full `SpectralCurvePanel`.
> Edits write back into the asset and re-run all three FrequencyGraph resolvers,
> so they propagate live to every *linked* consumer (the sanctioned
> action-at-distance). This closes the loop: a linked curve is read-only in its
> own node, and the library editor is where you change the shared curve. Forked
> (independent) copies are unaffected. One undo snapshot is committed per edit
> session (on close). See *FrequencyGraph asset editor* below.
>
> **Still pending:** rolling the new fork/link model out to the other kinds
> (Waveforms, AHDSR, Morph) is awaiting a design decision from the user.

Open it from **Edit → Asset Library…**. The dialog (`AssetLibraryComponent`)
has one tab per asset kind:

- **Waveforms** — a single-cycle/wavetable frame (any frame type: layered,
  spectral, wavelet, granular, inharmonic, sample). Published from the
  Layered-Waveform editor.
- **Instruments** — (reserved) independent instruments.
- **ADHSR Curves** — a full AHDSR amplitude envelope shape. Published from the
  shared AHDSR editor.
- **Morph Algorithms** — a frame-scope **warp chain** (a `std::vector<WarpOp>`:
  the ordered shape-bending stages applied to a wavetable frame). Published from
  the wavetable editor's warp panel. **Every project is seeded with the curated
  built-in chains** (Warm Saturation, West Coast Fold, Lo-Fi Crush, …) so this tab
  and the [Summation Morph picker](#built-in--saved-morphs-the-library-row) are
  never empty — they show ★ starred and are **code-owned** (re-seeded on new
  project / load, not written to the project file, in a reserved id range below
  the user id base). See [Built-in & saved morphs](#built-in--saved-morphs-the-library-row).
- <a name="frequency-graph-library-curves"></a>**Frequency Graphs** — a 1-D
  frequency-domain curve (a `SpectralCurve`: an EQ/response shape over a `[0,1]`
  frequency axis). A distinct kind because a frequency curve is fundamentally
  different from a time-domain wave shape or an amplitude envelope. Shared by the
  **Spectral FFT** waveform type's magnitude/phase curves (both a standalone
  Frequency Domain node and a Spectral frame nested inside a wavetable — each
  curve has its own **Library…** button, `resolveSpectralReferences`), the
  **Curve EQ** node's response curve (`resolveCurveEqReferences`, see
  [Curve EQ](#curve-eq)), and the **Spectrum Tap** per-bin custom response (published from the
  Spectrum Tap response editor's **Library…** button — see
  [SpectrumTap](#spectrumtap)). The publish / load-copy / link / unlink popup is
  one shared helper (`showFrequencyGraphLibraryMenu` in `curve_editor.cpp`) used
  by every consumer (these three consumers are on the **new fork-by-default +
  read-only-link** model — see the [transition note](#asset-library-project-stores)). The payload is **source-preserving**: the
  equation text *and* its authoring language for formula curves, the control points
  for Drawn/Points, and the per-sample buffer for Drawn/Freehand all round-trip, so
  a stored curve can always be re-edited in the form it was authored.
- <a name="convolution-ir-library"></a>**Convolution IRs** — a convolution
  **impulse response** (a time-domain sample list). The payload is exactly the
  encoding stored on a Convolution Filter node's script
  (`ConvolutionProcessor::encodeIR`, the `"__convolution__:<len>,<sample>,…"`
  form). Published from the **Convolution Filter** editor's **Save to Library**
  button, and loaded back via its **Load from Library** button (a popup listing
  every stored IR). Unlike the Frequency Graphs, IRs load as an **independent
  copy** — the loaded IR is dropped into the node and can be edited freely without
  touching the stored asset (there is no live link, matching how the editor loads
  an IR from a `.wav` file). See [Convolution Filter](#convolution-filter).

Each tab lists its assets with **Rename**, **Duplicate**, **Star** / **Unstar**,
and **Archive** (soft-delete) / **Restore**, plus two filter toggles: **Show
archived** (include soft-deleted entries) and **Starred only** (show just your
favourites). The **Frequency Graphs** tab adds an **Edit…** button (see
[below](#frequencygraph-asset-editor)). Starred assets show a ★ in the list; the
**starred** flag is the user-side analogue of the built-in factory "curated"
flag, and pickers offer a **Starred only** filter over both so you can surface
favourites out of a large library.

There is deliberately **no hard-delete button** here. Assets can be referenced by
id from node scripts (waveforms, morph algorithms, frequency graphs), so an in-UI
purge would silently dangle those references. Removal from the management dialog
is **archive-only** (hides from pickers, stays resolvable). A true hard purge is
reserved for an explicit, warned CLI action; the model still exposes
`AssetLibrary::erase()` for that path, it just isn't wired to a button.

<a name="frequencygraph-asset-editor"></a>
### FrequencyGraph asset editor

The **Frequency Graphs** tab has an **Edit…** button (enabled when a
non-archived asset is selected) that opens the stored curve in a full
`SpectralCurvePanel` — the same editor used inside the consumer nodes (equation /
drawn-points / freehand authoring, language selector, etc.). The panel's y-range
is auto-detected from the stored curve: a signed curve (any negative sample)
opens on the phase range `[-π, π]`; everything else opens on `[0, 2]`, which
contains both magnitude (`0..1`) and gain (`0..2`) shapes.

This is the **only** place a *linked* FrequencyGraph curve can be changed (a
linked curve is read-only in its own node). Every edit:
1. writes the curve back into the asset (`AssetLibrary::update`), and
2. re-runs all three FrequencyGraph resolvers (`resolveCurveEqReferences`,
   `resolveSpectralReferences`, `resolveSpectrumTapReferences`),

so the change **propagates live to every node that links the asset** — the
sanctioned action-at-distance of the new model. Forked (independent) copies are
unaffected. The live writes keep the audio graph current during editing; a
**single undo snapshot** ("Edit frequency graph") is committed when the editor
closes, iff anything changed (not one step per drag tick). Implemented as
`FrequencyGraphAssetEditor` in `asset_library_component.cpp`; the whole
`AssetLibraryComponent` now takes the full `NodeGraph&` (not just its
`AssetLibrary&`) so the editor can reach the resolvers.

### Identity, ids, and the live-reference model

- Every asset has a stable integer **id**. User-created ids start at
  `1000000` (`AssetLibrary::kUserIdBase`), disjoint from any built-in id space,
  so the two never collide.
- A node **references an asset by id**, not by copying it. While referenced, the
  node keeps a local resolved copy that the audio thread reads directly (so no
  string decoding happens per audio block).
  - **Old model (Waveforms, AHDSR Curves, Morph Algorithms — still current):**
    any edit — from any referencing node, or from this dialog — is written back
    to the asset and **propagated** to every other reference
    (`resolveAhdsrReferences` for curves, `resolveWaveformReferences` for
    waveforms, `resolveWarpReferences` for morph algorithms).
  - **New model (FrequencyGraph consumers — migrated):** a link is **read-only**;
    consumers never write back. The resolvers (`resolveSpectralReferences`,
    `resolveSpectrumTapReferences`, `resolveCurveEqReferences`) only mirror the
    asset **into** linked nodes (library → consumer), so editing the asset in the
    library propagates, but editing a node forks rather than writing back. See the
    [transition note](#asset-library-project-stores).
- **No detach-in-place.** To make one copy diverge from the shared asset, use
  **Duplicate** (mints a new id) and point the node at the duplicate (e.g. via the
  waveform editor's **Use Library…** picker). There is deliberately no per-instance
  "detach" button — divergence is always Duplicate + repoint.
- **Soft-delete (Archive)** hides an asset from the pickers but keeps it
  resolvable, so existing references stay valid. This is the only removal action
  in the management dialog — there is no in-UI hard delete (it would dangle
  id-based script references). A hard erase (`AssetLibrary::erase`) is reserved
  for an explicit, warned CLI action; when it eventually removes an asset, any
  node still referencing it falls back to **independent** (keeps its
  last-resolved shape) on the next resolve.
- **Content-hash dedup.** Each asset carries a content hash
  (`AssetLibrary::computeHash`, shared FNV-1a from `hash_util.h`) over its
  kind + sub-type + payload, used to detect identical content. The hash is
  conservative — no normalization — so only byte-identical payloads dedup.

### Referencing from a node

- **AHDSR curves** — the shared AHDSR editor (see
  [Shared AHDSR envelope](#shared-ahdsr-envelope)) shows a **Library:** row:
  a picker to reference a stored curve (or **(Independent)**) and **Add to
  Library** to publish the current shape. The reference id lives on the node as
  `ahdsrAssetId`.
- **Waveforms** — the Layered-Waveform editor shows a reference row beneath the
  per-waveform gain: a read-only status (**Library: → name**, or **Independent
  waveform**), an **Unlink** button, a **Use Library…** button, and a **Save to
  Library** button.
  **Save to Library** publishes the current waveform as a new asset and links this
  slot to it (disabled once linked — diverge via Duplicate instead). **Use
  Library…** opens the unified [waveform-library browser](#picking-a-waveform-the-unified-browser)
  to repoint this slot — at the frame scope it opens in **frame mode** (leads with
  your saved frames, factory single-cycles demoted under a divider; see the browser
  section). **Unlink** detaches the slot's live link (sets
  `assetId = -1`) while keeping the current frame as an independent editable copy,
  so edits stop propagating to/from the shared waveform; it's **disabled** (greyed,
  with an explaining tooltip) while the slot is already independent. The reference
  lives on the wavetable library entry
  (`WaveformLibraryEntry.assetId`) — a node's wavetable can hold many waveforms,
  so each slot references independently. Adopting an asset keeps the slot's own
  **gain** (a placement-level property, not part of the shared shape).
- **Waveforms (per layer)** — each **layer** inside a Layered-Waveform frame can
  *also* live-link to a Waveform asset, via **Use Library…** / **Save to Library…**
  / **Unlink from Library** in the layer's [wave-source picker](#wave-source-per-layer). **Use
  Library…** opens the same browser; ticking **Sync to library** adopts the asset
  as a live reference (else it loads a copy), and **Save to Library…** publishes
  the layer as a single-layer Waveform asset and links it. **Unlink from Library**
  detaches the layer's link (`assetId = -1`) keeping the current cycle as an
  independent copy (greyed in the menu while the layer is unlinked). The reference lives on
  `WaveLayer::assetId`; the shared unit is the layer's **shape** (its **amp** is a
  per-slot property preserved across resolves, like a frame slot's gain).
  `resolvePerLayerWaveformReferences` pulls each referenced asset into its layer on
  load and after settled edits; `writeBackPerLayerWaveforms` pushes layer edits
  back. A layer pointed at a **multi-layer** asset is flattened to that frame's
  summed cycle (so multi-layer assets are best used by *copy* at the layer scope).
- **Morph algorithms (warp chains)** — the wavetable editor's frame-scope
  [Summation Morph](#built-in--saved-morphs-the-library-row) panel (`WarpChainEditor`)
  shows a **Library row** (a status label + **Unlink** / **Use Library…** /
  **Save to Library** buttons). Its picker lists two sections (every row **loads** a
  chain — there is intentionally no no-op "Independent" row; detaching is done with
  the dedicated **Unlink** button instead): **Built-in** (curated code-defined
  Type-2 chains — `builtinMorphChains()` — that **copy in** as a template and detach
  to Independent, *not* live references), and **Saved** (user-published Morph
  Algorithm assets — live-linked only when **Sync to library** is ticked in the
  picker, else copied in). **Save to Library** publishes the current
  chain as a new asset (disabled until the chain has at least one stage).
  **Unlink** detaches the frame's live link (`morphAssetId = -1`) while keeping the
  current chain as an independent editable copy; it's **disabled** (greyed, with an
  explaining tooltip) while the frame's morph is already independent. While a
  *Saved* asset is referenced, editing the chain here writes back to the asset and
  re-resolves, so every frame using the same algorithm re-shapes together. Adopting
  a referenced chain reconciles the node's modulation params to the new stage count
  (`syncWarpParamsForNode`), so each stage's amount stays modulatable. The
  reference id lives **per-frame** on `IWavetableFrame::morphAssetId` (built-in ids are
  never stored — picking one detaches to Independent); each frame links independently,
  so two frames can reference different morph assets or one can be linked while
  another is independent. Only the frame-scope warp
  opts into the library; the baked per-layer / spectral / wavelet warp chains stay
  local (no picker). The reference is the only "downward" coupling — selecting or
  editing a chain never touches the other warp sites.

### Picking a waveform (the unified browser)

Selecting a waveform — whether adding a new wavetable frame (**+ Waveform → From
waveform library…**) or repointing an existing slot (**Use Library…**) — opens one
picker, `WaveformLibraryBrowser`, that lists **both**:

- the **built-in factory single-cycle library** (thousands of shapes, grouped into
  categories, curated entries marked ★), and
- this project's **saved Waveform assets** (under a **★ My waveforms**
  pseudo-category; archived assets are hidden).

The library is far too large for a flat dropdown, so the browser offers a category
list, a **search** box, a **Starred only** filter (built-in *curated* OR user
*starred*), a **Show my waveforms** toggle, and a live cycle preview. Choosing a
**built-in** drops in an editable independent copy (button reads **Insert**);
choosing a **saved waveform** can either copy it in or create a **live reference**,
governed by the **Sync to library** checkbox (enabled only when a saved waveform is
selected — built-ins are immutable templates that always copy). With Sync on, later
edits propagate everywhere it's used. The same browser + Sync checkbox drives all
three waveform entry points (the **+ Waveform** add flow, the frame-scope **Use
Library…**, and the per-layer **Use Library…**); per-layer picks set
`WaveLayer::assetId` instead of the frame slot's `assetId`. To later break a live
reference without changing the current content, use the **Unlink** button (frame
row) or **Unlink from Library** menu item (per-layer picker) — see the per-scope
bullets above.

**Frame scope vs. layer scope — the same browser, re-ordered.** A *frame* is a
whole stack of layers, whereas the factory `AKWF_*` entries are single cycles
(layer-level primitives), so the frame-scope **Use Library…** (`setFrameScope(true)`,
only on the identity-row button — *not* the per-layer picker or the **+ Waveform**
add flow) **leads with your saved frames and demotes the factory catalog**: the
category list starts with a **★ My saved frames** category (the default view), then
a non-selectable **"Start over with a single cycle"** divider, then **All single
cycles** + the factory categories (listed factory-only, since your frames are the
category above). Picking a saved frame still replaces the whole frame (all layers,
copy or live reference per **Sync**); picking a factory single cycle replaces the
frame with a fresh single-layer copy ("start over"). The dialog title reads
**"Replace frame from Library"**. The per-layer and **+ Waveform** flows keep the
flat factory-first layout — there a single cycle genuinely *is* the unit being
chosen. The **★ My saved frames** category and the **"Start over with a single
cycle"** divider stay present even before you've saved any frames (the category
shows **(0)** and an empty list explains how to save one) — they're structural,
so the two ways to start a frame are always spelled out rather than appearing
only once you happen to have saved something.

### Import / export between projects

The bottom of the **Asset Library** dialog has **Import…** and **Export…**
buttons that move assets between projects.

- **Export…** writes the project's **entire** asset library to a standalone
  `.seancelib` library file (a minimal project file carrying only `[AssetStore]`
  blocks — archived and starred entries included). `ProjectFile::exportAssets`.
- **Import…** merges the assets from another file into this project. The source
  can be **either** a dedicated `.seancelib` export **or any saved `.seance`
  session** — a project file already contains a full `[AssetStore]`, so there is a
  single import path for both (`importAssets` in `asset_import.cpp`, fed by
  `readProject` into a throwaway graph; the live project's `currentPath` is never
  touched).

The merge is **content-aware**, in three flat passes (no bottom-up ordering —
each asset's stored hash already encodes any descendants):

1. **Closure expansion** — importing a parent pulls in its dependency closure
   (children, grandchildren). Every asset is a leaf today, so the closure is just
   the selected set; the hook (`assetChildIds`) is in place for future composite
   instruments.
2. **Decide each item's final id** — for every closure item the importer
   **re-derives** the content hash from the payload (it never trusts the source
   file's stored hash, so a corrupt/hand-edited hash can't cause a *false merge*
   that destroys data). On a match against an existing destination asset the
   **existing item wins** (its id *and* name are kept) and the import deduplicates;
   otherwise a **fresh destination id** is allocated.
3. **Rewrite child-id references** in inserted payloads using the complete remap
   table (no-op for leaves; hook `assetRewriteChildIds`).

Other merge rules:

- **Name clashes** between a genuinely-new import and an existing same-kind asset
  get the **lowest free numeric suffix** ("Bass" → "Bass 2"). A same-name *and*
  same-content asset just dedups (keeping the destination's name).
- Re-importing the same file is **idempotent** — everything dedups, nothing is
  added.
- The post-import dialog reports how many assets were added, deduplicated, and
  renamed. The import is one undo step (`Import assets`) only when something was
  actually added.
- **Bias toward not merging:** false merge destroys data, false non-merge is
  harmless (a duplicate import). Cross-version hash-algo changes can only cause
  harmless false *non*-merges.

### On disk and undo

- Assets are written to the project file in `[AssetStore]` blocks (one per
  asset: id, kind tag, name, sub-type, content hash, archived flag, starred flag,
  and a base64 payload — the archived/starred lines are omitted when false, so
  older files load unchanged). The block is **included in undo snapshots**, so
  store edits participate in undo/redo like any other project state.
- Node references serialize alongside the node: `ahdsrAssetId` for curves; an
  optional `:assets:` block inside the wavetable script (`__wavetable5__`) maps
  each library entry id to its asset id; an optional `:warpAsset:<id>` block
  (written after `:assets:`, before the always-last `:warp:` block) carries the
  frame-scope warp chain's morph-algorithm id. All are re-resolved on load, so a
  reopened project sees the current asset content. Each block is omitted when
  unused, so unreferenced wavetable payloads round-trip byte-identically.

Data model: `asset_library.h/.cpp` (`AssetKind`, `AssetEntry`, `AssetLibrary`),
owned by `NodeGraph::assets`. Import/merge + export: `asset_import.h/.cpp`
(`importAssets`) and `ProjectFile::exportAssets`. Management UI:
`asset_library_component.h/.cpp`.
Waveform picker: `WaveformLibraryBrowser` in `layered_wave_editor.cpp`. Morph
picker + write-back: `WarpChainEditor::LibraryContext` (`warp_editor.cpp`),
`resolveWarpReferences` / `syncWarpParamsForNode` (`layered_wave_editor.cpp`).

## Terrain-synth self-test (`--self-test`)

SEANCE has a headless, in-process test harness for the 1D / 2D / 3D terrain
synths (audio-file, image, and video sources), run from the command line:

```
SEANCE.exe --self-test <output-dir>
```

It creates no window. It generates synthetic test media, renders audio
through real `TerrainSynthProcessor` instances, checks the results, writes
everything it does to `<output-dir>`, sets the process exit code (**0** = all
passed, **1** = any failure, so it can gate CI), and quits. With no
`<output-dir>` it defaults to a `selftest_out` folder next to the executable.
Source: `cpp/src/self_test.{h,cpp}`, wired into `main.cpp::initialise` next to
the `--plugin-sandbox` child-process branch. Because SEANCE is a GUI-subsystem
binary with no reliable console, the harness writes its PASS/FAIL log to
`<output-dir>/selftest_report.txt` (it also best-effort echoes to stdout when
launched from a terminal it can attach to).

Three layers of checks run in order:

1. **Terrain data (exact).** Fills a `Terrain` from a known synthetic pattern
   — a −1..+1 ramp WAV (1D), a left-to-right brightness gradient PNG (2D), and
   a brightness-ramps-with-frame video grid (3D) — and asserts `Terrain::at()`
   and the N-linear `Terrain::sample()` read back the expected values. Image
   and video data are `uint8_t`-quantized, so those use a 1/255 tolerance; the
   1D float WAV and the exact endpoint samples are checked tight. Also
   round-trips a `makeVideoTerrainScript` / `parseVideoTerrainScript` pair
   (path with spaces, time/pixel crop, grid size, base64 gray bytes).

2. **Synth render (Sig-driven position → audio).** Instantiates a standalone
   `TerrainSynthProcessor`, holds a note, and drives the **`Sig X/Y/Z`**
   coordinate pins with ramp signals on the buffer's control channels — the
   exact "move the read position with a signal cable" path the node graph uses.
   For the 2D and 3D cases it uses **AM-sine** synth mode, whose output
   amplitude is `volume·(0.5 + 0.5·terrain.sample(coord))` with the coordinate
   *not* phase-shifted, so the rendered envelope directly traces the terrain
   readout. It asserts the measured envelope (a) actually *moves* as the
   position sweeps (range test — proves the Sig channels are live) and (b)
   Pearson-correlates >0.9 with the predicted readout (typically ~0.998). Each
   render is also exported as a `.wav` so the result is audible.

3. **ffmpeg round-trip (optional).** Skipped with a note when ffmpeg isn't on
   `PATH`. Otherwise generates a `testsrc` clip, probes it, and decodes it back
   through `VideoDecoder::decodeGrid`, asserting a non-uniform grid.

The render layer is what guards the signal-driven coordinate path:
`TerrainSynthProcessor::processBlock` clears its render buffer near the top
(`buf.clear()`), which also wipes the incoming control-signal channels (buffer
channels 2+). The per-sample `Sig X/Y/Z` and Aftertouch reads happen *after*
that clear, so they snapshot channels 2+ into a `controlInBuf` member
**before** the clear and read from the snapshot — without that, every
Sig-driven coordinate reads 0 and the position never moves. The range +
correlation assertions in layer 2 fail loudly if that regresses.

Artifacts written to `<output-dir>`: `selftest_report.txt`, the synthetic
inputs (`test_audio_1d.wav`, `test_image_2d.png`, `test_video.mp4`), and the
rendered outputs (`render_1d_direct.wav`, `render_2d_amsine_sweepY.wav`,
`render_3d_amsine_sweepFrame.wav`).

## Ephemeral session (`--ephemeral`)

<a name="ephemeral-session---ephemeral"></a>A flag for **throwaway / test launches** of the
full GUI:

```
SEANCE.exe --ephemeral
```

It runs the app exactly as normal **except** that every crash-recovery /
session-state file is redirected away from the user's real app-data folder
(`%APPDATA%/SoundShop`) into an isolated throwaway directory
(`<temp>/SEANCE-ephemeral`), which is **wiped at the start of each ephemeral
launch**. The redirected set is the whole autosave family — `autosave.ssp`, its
`autosave.meta.xml` sidecar, the `undo-tree.dat` persistence, the per-plugin
`autosave-plugin-*.dat` blobs, and the `session.lock` crash sentinel (everything
under `getAutosaveDir()`).

**How crash detection works (the `session.lock` sentinel).** SEANCE does **not**
treat the mere presence of `autosave.ssp` as a crash — a normal idle session
also writes that file periodically (the slow autosave does a full save whenever
`autosave.ssp` is missing or the refresh interval elapses), so basing crash
detection on it produced **false "didn't shut down cleanly" prompts on every
launch** whenever any abnormal exit (force-kill, a crash during shutdown, power
loss, or even the autosave worker re-writing the file in the last milliseconds
before exit) left the file behind. Instead, the constructor drops a
`session.lock` file (`setupSessionLock()`) and remembers whether one was
**already** there from a previous run (`startupWasUncleanShutdown`). A clean quit
deletes the lock (`markCleanShutdown()`, called from the single clean-quit
chokepoint `MainWindow::tryQuit` after `tryQuit()` returns true). So at startup:

- **Lock present** → the previous run never reached a clean shutdown → if an
  `autosave.ssp` exists, `tryRecoverAutosave()` shows the *"didn't shut down
  cleanly — recover?"* prompt. **This is the only thing that fires the prompt.**
- **Lock absent** → the previous run exited cleanly → any leftover `autosave.ssp`
  is stale and is **swept silently** (no prompt), and the persisted undo tree is
  restored as usual.

**Why `--ephemeral` exists.** Automated / test launches (e.g. opening the app to
verify a feature, then killing the process) leave a `session.lock` behind — which
would make the **next normal launch correctly but unhelpfully report a crash** for
what was really just a killed test run. With `--ephemeral`, the lock (and any
autosave) only ever lands inside the throwaway temp dir, which is wiped on entry,
so:

- A genuine crash in a normal (non-ephemeral) launch still leaves the real
  `session.lock` → the recovery prompt fires and **means something went wrong**.
- A killed `--ephemeral` launch leaves nothing in the real dir → no spurious
  prompt, and (because the temp dir is wiped on entry, erasing any lock) no
  recovery prompt within ephemeral runs either.

The window title is suffixed with **`[ephemeral session]`** so the mode is
obvious at a glance and the absence of a recovery prompt is explained. The flag
does **not** isolate the preferences file (`soundshop_prefs.xml`, kept next to
the exe) — only the crash-recovery state. Parsed in `main.cpp::initialise`;
implemented by `SoundShop::setEphemeralSession()` (`main_window.cpp`), which the
`getAutosaveDir()` family consults. (`--self-test` and `--plugin-sandbox` never
create a window or run the autosave machinery, so they were already safe;
`--ephemeral` covers the GUI-launch case.)

## Opening a project from the command line

<a name="opening-a-project-from-the-command-line"></a>Passing a `.ssp` project
path as a bare argument opens that project on launch instead of auto-loading the
most-recent one:

```
SEANCE.exe "C:\path\to\song.ssp"
```

This is what the OS uses for "Open with…" / double-clicking a `.ssp` file (once
the file type is associated with the exe), and it composes with `--ephemeral`
for opening a known project into a throwaway test session
(`SEANCE.exe --ephemeral "…\song.ssp"`). The first non-flag token ending in
`.ssp` wins; any leading flags (`--ephemeral`, etc.) are ignored when picking the
file. The named project takes priority over the normal *auto-load last project*
behaviour. Editor panels saved in the project's `[Editors]` section are restored
just as they are for an auto-loaded project. Parsed in `main.cpp::initialise`
(stored via `SoundShop::setStartupProjectFile`); loaded in the
`MainContentComponent` constructor before the auto-load fallback.

---

## Asynchronous plugin loading

<a name="asynchronous-plugin-loading"></a>Opening a project that hosts VST3/AU
plugins no longer blocks the UI while those plugins instantiate. Plugin
instantiation (`createPluginInstance` + restoring the saved `setStateInformation`
state) is slow — often a second or more per plugin — and **must** run on the
message thread (third-party plugins are not safe to instantiate off-thread, and
SEANCE deliberately loads them **one at a time**, not thread-per-core, for the
same reason). Previously the whole project froze until every plugin finished;
now the nodes appear immediately and each plugin loads in the background.

**How it works.** `ProjectFile::load` is called with a `nullptr` plugin host on
the interactive open path (`openProjectFile`) and at startup, so it parses every
node — keeping each plugin node's saved state in `pendingPluginState` — without
instantiating anything while holding `NodeGraph::mutationLock`. The graph is
painted right away. Then `MainContentComponent::beginAsyncPluginLoad()` marks
every not-yet-loaded plugin node **Pending**, pushes its id onto a FIFO queue,
and `processNextPluginLoad()` walks the queue one node per `callAsync` tick: the
node flips to **Loading**, the heavy `loadPlugin` + `setStateInformation` runs
**off** the graph lock, then the result is published **under** the lock
(`node.plugin` set, `pendingPluginState` cleared) and the audio graph is rebuilt.
The loader never holds a `Node*` across the heavy call — it re-looks-up by id —
so the queue is safe even if you add/remove nodes mid-load. Opening a second
project while the first is still loading rebuilds the queue without starting a
duplicate loader chain. (The **autosave crash-recovery** path still loads plugins
synchronously, because it immediately applies per-plugin override blobs that need
the live processors.)

**Per-node loading badge.** While a project's plugins resolve, each plugin node
shows a small badge at its **top-left** corner (the script-error badge owns the
top-right):

- **Pending** — a dim grey hollow ring: queued, waiting its turn.
- **Loading** — an animated aqua arc spinner (driven by the 30 Hz UI timer):
  instantiating now.
- **Failed** — an amber "x" disc: the plugin couldn't be instantiated (missing,
  blocklisted, or incompatible). The node is kept so you can replace the plugin.
- **Ready / none** — no badge.

Each state has a hover tooltip explaining it. The spinner only animates (and the
graph only repaints every tick) while loading is in progress.

**Save is gated during load.** *Save Project* and *Save Project As…* are greyed
in the File menu — their labels read *"(loading plugins…)"* — until the queue
drains, because the audio graph isn't fully live yet. The keyboard shortcut is
backstopped by a brief "Still loading" dialog. As soon as the last plugin
resolves, `projectLoading` clears and the menu refreshes (`menuItemsChanged`),
re-enabling Save.

**Mid-load save safety.** The slow autosave can still fire while plugins are
loading. To avoid silently dropping a not-yet-applied plugin's saved state,
`writeProject` falls back to writing `node.pendingPluginState` when a plugin node
has no live processor to query and no cached state — so an autosave mid-load
preserves the plugin state read from the file.

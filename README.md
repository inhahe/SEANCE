# SEANCE

**SEANCE** stands for **Sound Exploration And Node-based Composition Environment.**

A node-based digital audio workstation (DAW) designed to be intuitive for people without a music background.

> ⚠ **Status — early, untested.** A lot of features have been added since the project was last seriously tested end-to-end. I can't currently say for sure that the build is in a working state, or which features are working and which are broken. This commit exists primarily as an off-site backup so I don't lose work if something happens to my computer. Treat anything you see here as in-development, not as a release. This notice will go away once the project has been re-validated.

Most DAWs are designed by musicians for musicians and are full of cryptic knobs and abbreviations. SEANCE takes a different approach: every building block — instruments, effects, timelines, mixers, MIDI inputs, the speakers themselves — is a **node** in a graph, and you wire them together with **cables** that visibly carry audio, MIDI, parameter, and signal data. Signal flow between nodes is never hidden in menus or in invisible buses — if a signal goes from one node to another, it does so as a cable on screen.

Intended to be cross-platform: Windows, macOS, and Linux. The codebase uses JUCE 8 + CMake throughout and contains no platform-specific assumptions in the source, but in practice it has only been built and tested on Windows so far. macOS and Linux builds aren't expected to need anything beyond installing the platform compiler and running the build commands in the [Building](#building) section, but they haven't been verified yet.

## Vision

- **No assumed music knowledge.** Every control has a plain-language label or tooltip; music-theory terminology is explained where it appears.
- **Signal flow between nodes is always visible.** Cables on the graph are the *only* way one node modulates another. There are no global "modulators by name," no hidden routing flags, no "send to bus 5" magic — if a signal is going from one node to another, you can see the cable. (A node's *internal* state — a piano roll's automation curves on its own track parameters, a plugin's own ADSR envelope on its own filter, etc. — isn't covered by this rule because it isn't flowing anywhere; it's just state on one node mutating itself over time.)
- **Try things without losing your place.** Linear undo punishes exploration: the moment you back up a few steps and try a different idea, the path you backed away from is gone forever. SEANCE keeps every path you've ever taken in a branching undo tree, navigable indefinitely. Combined with cross-session undo persistence — your history survives quit-and-reopen and even continues past your last saved version — the cost of "let me try a wild idea" drops to near zero.
- **Simple by default, powerful when needed.** New users see a small graph that already works; advanced users can add as much complexity as they want.

## Core concepts

### Signals (the four pin kinds)

SEANCE has four cable kinds, color-coded so you can tell at a glance what's flowing where:

- **Audio (blue)** — stereo audio.
- **MIDI (green)** — MIDI events (notes, CC, pitch bend, aftertouch).
- **Param (orange)** — *block-rate* control signal, used for modulating knobs and sliders. One value per audio block (~ms granularity). Cheap.
- **Signal (amber)** — *audio-rate* control signal, used for sample-accurate modulation (audio-rate FM, audio-rate filter modulation, etc.).

Audio→Audio and MIDI→MIDI must connect like-to-like. **Param and Signal are both control kinds and SEANCE treats them as interchangeable** — a Param output can drive a Signal input and vice versa, with automatic conversion. The cable shows the conversion visually: its head segment is the source colour and its tail segment is the destination colour, so you can see where the rate change happens.

Most users only need Param. Use Signal when you specifically need sample-rate precision (e.g., audio-rate FM through a synth's frequency parameter).

### Wavetable arrangement

A wavetable is a sequence of single-cycle waveforms that the synth morphs between as you sweep a Position knob. SEANCE arranges wavetables on **three levels of structure**:

1. **Layers within a waveform.** Each waveform in a wavetable is built by summing layers (sine, saw, square, triangle, noise, or hand-drawn shapes), each at its own harmonic ratio, phase, and amplitude. Drawn layers have two sub-modes: **Points** (place control points; waveform is Catmull-Rom interpolated through them) and **Freehand** (click-and-drag to paint arbitrary per-sample waveform data). Stacking layers at integer harmonic ratios gives organ-like additive sounds; non-integer ratios give bell-like inharmonic textures. Waveforms can also be authored in the **frequency domain** (magnitude and phase curves IFFTed to a cycle), in the **wavelet domain** (paint DWT coefficients and IDWT to a cycle), or **captured from real audio** (project playback, microphone, or an audio file — see below), and a single wavetable can mix all four kinds — so you can blend a layered waveform, a spectral waveform, a wavelet waveform, and a captured-audio waveform on the same Position axis. The graph menu's **Wavetable** entry opens a new wavetable node directly in its single-window editor — the wavetable arrangement view on the left, the per-waveform editor on the right, both always visible. The **+ Waveform** button is a popup menu — pick "Layered (time domain)", "Frequency Domain (FFT)", "Wavelet Space (DWT)", or one of the three "From..." capture entries to add a waveform of that kind. Clicking a dot in the arrangement view (or any row in the Cells / Library list, or any waveform tab above the body) selects that cell; the right-pane editor (layer rows / frequency-domain magnitude+phase / wavelet coefficient grid) updates instantly to show the selected waveform's controls — no extra window, no mode toggle. The per-waveform editor swaps its body as you switch between waveforms of different kinds, so a single wavetable can mix all kinds and still has just one editor window.

   **Capture from real audio (granular waveforms).** Captured waveforms are a fifth kind — instead of storing a single boundary-cleaned cycle they hold a multi-second window of source PCM plus a grain length, and the synth plays them back via an overlap-add granular stream (a configurable number of Hann-windowed grains — default 4 — with jittered source-start positions) at the held note's pitch. That means the captured sound's *time-domain evolution* is preserved at playback: hold a note and you hear a continuous textural drone derived from the source material, not a repeating single cycle. Granular waveforms coexist with cycle waveforms (Layered / Frequency Domain / Wavelet) in the same wavetable, with morphing handled per-layer — the cycle portion of the morph plays through the terrain as usual, the granular portion plays through the OLA stream, and the result is summed sample-by-sample, so sweeping Position smoothly cross-blends granular textures with cycle waveforms. MIDI pitch tracking uses an embedded reference frequency stored with each granular waveform — set at capture time via the dialog's **Note + Octave** picker (default A4 = 440 Hz) and re-editable later from the per-waveform editor's Hz slider; per-waveform automatic pitch detection is a planned follow-up.

   The **+ Waveform** menu's three capture entries open dialogs that pull from different sources:
   - **From project song...** — pre-renders the whole project to audio when the dialog opens, then shows it as a song-length timeline with a **full Play / Pause / Stop transport**. Press **Play** to hear the song at full fidelity (a thin playhead sweeps the timeline, so it's easy to find the part you want); press **Pause** (or grab a region handle) to switch to a continuously-looping audition of the selected waveform. Apart from the transport, the song dialog uses the **same region / multi-waveform selection model as the Mic and File dialogs**: drag the two orange handles to bracket a region, set **Waveforms to slice out** to split that region into N equally-spaced waveforms (shown as shaded orange **section bands**), and use the **Preview waveform** selector to choose which of them you audition. The same **Freeze** mode picker (Crossfade loop / Async granular / Pitch-sync grains / Spectral freeze), **Grains per waveform** / **FFT size per waveform** texture controls, **Gain**, **Grain length**, **Crossfade**, **Window length** (ms — the per-waveform freeze window) + **Fit width to selection** button, and **Note + Octave** embedded-pitch picker all behave exactly as described for the Mic/File dialogs below — what you hear while Paused is what a held note will play. Press **Capture waveforms** to slice the region into N granular waveforms — they're added to the Library list only, leaving the arrangement untouched. To place one, select a cell in the arrangement view and press **Assign to selected cell** in the Library list sidebar. The dialog stays open after each capture so you can pick another region from the same song. (Capture is greyed out while Play is running, since the playhead is moving — pause first.)
   - **From microphone / audio input...** — shows the last ~10 seconds of audio from your input device, sweeping continuously, and **plays it through your speakers** so you can hear what you're about to capture. A small status line tells you whether any input signal has been detected. Press **Pause** to mute the monitor and hold the display still so you can drag the region handles on a steady waveform; press **Go live** to resume hearing and showing your input. (Use headphones while monitoring to avoid feedback.)
   - **From audio file...** — press **Load file...** and pick a .wav/.mp3/.aiff/.flac/.ogg. The file is decoded (long files are truncated to 60 s at 96 kHz to keep the display readable) and shown end-to-end.

   For the Mic and File dialogs you drag the orange handles to pick a region, choose how many waveforms to extract (1–32, default 8), label the captured source's pitch via the **Note + Octave** dropdowns (default A4 = 440 Hz, used as each waveform's embedded pitch reference for MIDI playback), pick a **Freeze** mode (Crossfade loop / Async granular / Pitch-sync grains / Spectral freeze — the same four sustain algorithms as the song dialog, baked into the captured waveforms and previewed live so you can A/B them before capturing) with its mode-specific **Grains per waveform** / **FFT size per waveform** control (same as the song dialog), set the captured **Gain** (0–4×, 1.00 = the recorded level — level a quiet mic take up or a hot file down; the same per-waveform gain the wave editor's Gain knob adjusts, and the Preview reflects it live), set the **Crossfade** seam (1 ms – half the freeze window, default 50 ms — the same loop-seam control the wave editor and song dialog expose, audible in Crossfade loop mode and reflected live in the Preview), and press **Capture waveforms**. Like the wave editor, the dialog separates **grain** from **window** with two sliders: a **Grain length** slider (5–500 ms, used only by the two grain-cloud modes — it greys out for Crossfade and Spectral) sets each overlapping grain, and a **Window length** slider sets each captured waveform's **freeze window** — the region the mode loops (Crossfade), roams (the cloud modes), or analyses (Spectral). The window is now given in **milliseconds**, the same unit as Grain length and Crossfade, so the window:grain ratio reads off directly. It **auto-tracks a multiple of the grain** until you drag it (4× for the grain-cloud modes so they have room to roam, 1× for Crossfade/Spectral), exactly like the editor. The selected region is split into that many positions, shown as shaded orange **section bands** over the waveform; you can't drag the selection narrower than one window length (so a band always fits), and the view-zoom is likewise capped so you can never zoom in tighter than one whole captured waveform. **With a single waveform**, the one band spans the whole selection and the captured frame loops the **entire recorded sound** — record yourself saying a word, fit the selection around it, and the synth plays that word back on each held note. A **Fit width to selection** button snaps the bands to an exact zero-gap tiling; both the window slider and Fit are disabled at a single waveform, where the one window simply tracks the selection. Resizing the selection re-spaces the bands without rescaling them, opening gaps or forcing (contained) overlap. The captured waveforms are added to the **Library list only** — capturing never rebuilds your arrangement or grows the grid's cell count; to place one, select a cell and use the Library's **Assign to selected cell** button. Each appears in the Library named after its source and freeze method plus a stable id number (e.g. *Mic – Crossfade loop 7*) rather than a generic "Waveform N". The mic dialog plays your input live through the speakers and sweeps the display; press **Pause** to mute the monitor and hold the display still while you drag handles, then **Go live** to resume. In both the Mic and File dialogs a **Preview** button (and dragging the handles itself) loops the selected slice so you can *hear* the exact region before capturing it — on the mic you have to Pause first so the loop has a steady buffer to play. When you're slicing out more than one waveform, a **Preview waveform** selector picks which of them you audition, and changing it mid-preview switches instantly. The project-song dialog adds a full Play/Pause/Stop transport on top of this same region/multi-waveform model (see its bullet above). All three are how you turn a riff, a synth lead, your own voice, or any recorded sample into wavetable material the synth can morph through.
2. **Waveforms within an N-dimensional arrangement.** Multiple waveforms stack into a wavetable that morphs as you change Position. The arrangement is N-dimensional — add an axis to get a second Position knob, add another for a third, etc. Most uses stay at 1D or 2D, but the option goes up to 8 dimensions. A Position knob only appears once its axis is actually *traversable* — a grid axis still 1 waveform wide (or a lone scatter waveform under normalized blending) has nothing to morph between, so it stays knob-free until you give it a second waveform.
3. **Two layout modes for the N-D arrangement:**
   - **Grid mode** — waveforms placed in specific cells of a rectilinear N-dimensional grid. Predictable, evenly-spaced morphing across each axis, and grids can be **sparse** — you can place specific waveforms at specific positions without filling every cell. By default an empty cell doesn't drain volume: the synth renormalizes the morph over the filled cells so the sound stays full-volume across the gaps. A per-wavetable **Empty cells fade volume** toggle switches to the alternative where morphing toward an empty cell ducks toward silence (so sparse grids read as rhythmic dropouts) — the grid counterpart of Scatter's *Distance fades volume*. A grid starts one-dimensional and renders as a **segmented line** (cells are tick-divided segments with a centred dot and `[i]` index label); **+ Dim** turns it into a 2D checkerboard, a 3D cube, and so on. Per-axis cell-count steppers in the Wavetable View window go from 1 to 64 waveforms per axis; resizing an axis preserves every existing waveform's (i, j, k, …) coordinate. Grid mode is the cheaper-to-render of the two modes (the synth reads 2^N corners per sample regardless of axis size).
   - **Scatter mode** — waveforms placed at arbitrary positions in N-dimensional space and blended via a Wendland radial basis function. Useful when your morph axes don't fit a grid (three waveforms forming a triangle, clusters at non-uniform spacings, etc.). The **RBF radius** slider (in the Wavetable View window) controls how far each waveform's influence reaches — smaller for sharper transitions, larger for smoother blends. A **Distance fades volume** toggle switches the blend from normalized (every Position is a full-volume weighted average) to absolute, so each waveform becomes a "loudness island" — loudest at its dot, fading to silence at the edge of its radius, and silent in the gaps between dots. The synth exposes a **Position** knob for each axis the dots actually spread out along (two dots apart horizontally give an X knob, vertically give a Y knob, both give both) — exactly like a grid axis needing two or more cells — so a lone dot, or dots that all share a coordinate, expose nothing to automate regardless of the blend mode. Scatter starts as a **line view** (one axis); **+ Dim** grows it to a square, cube, and beyond, while **− Dim** can shrink it all the way to a **drop-target placeholder** ("drag a waveform here") with no axes at all.

The wavetable editor is a single resizable window split side-by-side: on the **left**, the **wavetable arrangement view** — a spatial visualization showing the waveforms as coloured dots (in Grid mode at cell centers labelled with their (i,j,k) coord, in Scatter mode at arbitrary positions labelled "Waveform N") plus a sidebar with the Library list, Cells list, axis controls, etc. On the **right**, the **per-waveform editor** for whichever library entry is currently focused — both halves are always visible. The synth's Position knob / Position pin / a Param cable drives a smooth blend of the nearby waveforms at audio time. To add a new waveform, press **+ Waveform** and pick a kind from the popup. To edit a waveform, click any cell that holds it (the dot in the arrangement view or its row in the Cells list), its row in the Library list, or its tab in the row above the editor — the right pane updates instantly, no extra window to open or close. The right pane adapts to the waveform's type: layered waveforms show the layer rows + "+ Layer" button; frequency-domain waveforms show the magnitude / phase curve editors; wavelet waveforms show the wavelet painter; and **granular** (captured) waveforms show the captured source as a waveform display with a **draggable, resizable amber selection band** (the *freeze window* — the slice of the capture that every freeze mode actually sustains; drag its middle to move it and its edges to resize it, up to the whole capture, picking which part of the capture to freeze and how wide a slice to use — the band is resizable in every mode and always sets the freeze window, but what that window *means* is mode-specific: in Async / Pitch-synced grains the grains roam over it, so a window wider than the grain is what makes those two modes diverge; in Crossfade loop the band *is* the loop, so resizing sets the loop length; in Spectral freeze it sets the FFT analysis region. The grain-length slider only applies to the two grain-cloud modes and greys out for Crossfade and Spectral), sliders for grain length / crossfade / embedded pitch, a freeze-mode picker, a **Grains** slider (2–16) and **FFT size** combo that set the cloud density / spectral resolution for the modes that use them (each greys out when its mode isn't active), a **Play** button that auditions the waveform through the audio engine using its current freeze-mode / grain-length / crossfade / window settings (toggles to **Stop** while playing, and live-updates if you change Freeze mode mid-audition), and a **Re-capture from song...** button that swaps the source PCM in place (keeping the entry's name, colour, and current grain params, so re-capture doesn't pile up new library entries). In Scatter mode you can drag dots to reposition them and shift-click to delete.

The wavetable view also has:

- A **per-waveform Gain knob** on the editor's identity row. Every waveform is peak-normalised (so all frames play at the same level), which means the shape controls alone can't make one frame louder or quieter than another. The Gain knob (0–2, 1.00 = unchanged) scales the cycle *after* normalisation, so it actually changes the samples — it shows in the preview and gives morphs an honest volume contour. Gain is a per-waveform identity (like name and colour), shared by every cell that references the waveform.
- A **Library list** — every waveform that exists in this wavetable's library, whether or not it's currently placed in a cell. Each row shows a **colour swatch**, the waveform's name, and a "(used N×)" badge when placed. **Clicking a row makes that library entry the right-pane editor's target** — the row highlights, and every cell that references it gains an amber outer ring in the arrangement view so you can see exactly where the waveform you're editing is placed. The swatch on the left of each row opens a palette picker (Auto + 8 named colours); the chosen colour is also what every dot or cell referencing that waveform paints with, so colour is a per-waveform identity that the user can dial in (instead of being auto-derived from spectral content alone). An **X** removes the entry from the library (cells that referenced it become empty). The **+ Waveform** button adds a new waveform (Layered / Frequency-Domain / Wavelet / captured). The **Assign to selected cell** button places the highlighted library entry into the cell currently selected in the arrangement view (replacing whatever was there). This separation between library data and cell placements means deleting a cell no longer destroys the waveform — its library entry survives and is one click away from being placed somewhere else.
- A scrolling **Cells list** — every cell in the arrangement. In Grid mode this includes **empty cells** (shown dimmed with `— empty —` next to their coord) so you can select an empty slot and use **Assign to selected cell** to drop a library entry into it. In Scatter mode only the existing dots are listed (scatter points don't exist until placed). Picking a row selects that cell; if the cell holds a library entry the right-pane editor follows the selection too. The X next to each non-empty entry clears that cell (the library entry it referenced survives in the Library list).
- A **Convert** button — **Convert → Scatter** turns a Grid wavetable into a Scatter wavetable (every non-empty cell becomes a scatter dot at the cell center). **↩ Back to Grid** does the reverse, **but only while no dot has been moved off its original cell center** — drag any dot away and the button greys out, with a tooltip explaining why. This lets you start in Grid, convert when you need finer placement, and still revert as long as you haven't actually used the freedom yet.
- **Grid mode controls** — per-axis cell-count steppers (X, Y, Z, …, 1..64), plus **+ Axis** / **− Axis** to add or drop a dimension (up to 8 axes), and an **Empty cells fade volume** toggle (off by default = empty cells renormalized away, on = empty cells duck the output toward silence — see Grid mode above).
- **Scatter mode controls** — the **Blend width** slider (sharpness of the position blend), a **Distance fades volume** toggle (normalized inverse-distance blend vs. absolute distance-faded gain — see Scatter mode above), plus the same **+ Axis** / **− Axis** buttons (0..8 geometric dims: 0 = drop-target placeholder view, 1 = line view, 2 = square, 3 = cube, …; adding an axis defaults every existing dot's new coordinate to the midpoint, removing one truncates each dot's position vector).
- A **View** dropdown for stereoscopic 3D rendering — **Flat 2D** (default), **3D Anaglyph** (one composite image with red/cyan eye channels; requires red/cyan paper glasses), **3D Cross-eyed stereoscope** (side-by-side pair viewed cross-eyed, so the left half lands on the right eye and vice versa — no hardware needed), or **3D Parallel stereoscope** (side-by-side pair viewed wall-eyed, how a Holmes stereoscope or a VR headset presents the pair).
- A sidebar of **rotation sliders, one per N-D plane** (1 for 2D, 3 for 3D, 6 for 4D, 10 for 5D, …). Mouse-drag in the view orbits the (axisX, axisZ) and (axisY, axisZ) planes; the sliders give direct access to every plane, including ones the current projection isn't showing, so in a 4D-or-higher wavetable you can rotate axes out of the projection plane and back in to see how the arrangement looks from any angle. A **Reset rotation** button at the bottom zeroes every plane back to the axis-aligned default. The wireframe in the view is a projection of the full N-dim hypercube (every corner of [0,1]^N, connected by every 1-bit-differing edge — capped at 6 dims for legibility), so the box always encloses every dot regardless of rotation, and it automatically fit-scales to fill the available view area. Un-rotated, dims past the projected two collapse on top of each other (mathematically correct — the projection has degenerate edges), so a 4D wavetable just looks like a 2D square until you rotate a higher plane to pull the extra dims apart.

The per-waveform editor (the right half of the window — always visible) shows the editing UI for the currently-selected library entry: layer rows for Layered, magnitude+phase curves for Frequency Domain, a DWT coefficient grid for Wavelet Space, and a Compare panel at the top for A/B'ing Direct vs Additive-bank render modes on the same cycle. Above the editor body sits a per-waveform identity row — a **colour swatch** (same palette picker as the Library list) and a **name field** — so renaming the waveform or changing its colour is one click away while you're already editing it. The same name and colour are what every Library row and arrangement-view placement of this waveform displays. A **waveform tabs** row above the body lists one tab per library entry (not per cell — a single entry placed in three cells is still one tab); click a tab to make that entry the editor's target. The other two ways to focus the editor are clicking a row in the **Library list** or clicking a non-empty cell (dot in the arrangement view / row in the Cells list), which both select the cell AND sync the editor target to whatever library entry that cell references. **Editor target vs cell selection are two separate states**: editing the right pane edits the data (library entry), and that edit shows up in every cell that references the entry; the Cells-list selection just controls where **Assign to selected cell** sends the entry next. An amber outer ring in the arrangement view marks every cell currently referencing the editor's target so the link between "what I'm editing" and "where it ends up" stays visible. If the library is empty the right pane shows a placeholder telling you to add an entry with **+ Waveform**.

### Waveterrain (Terrain Synth)

Terrain Synth is the underlying engine that powers most of SEANCE's built-in synths. It treats your sound source as an **N-dimensional terrain** of sample values, plus a **traversal** that walks through that terrain over time, reading values to produce audio.

A terrain can come from:

- A 1D layered waveform (the standard wavetable case — terrain is the cycle, traversal sweeps through it once per played pitch).
- A 1D audio file (sample player — terrain is the recording, traversal advances at note-pitch-relative speed).
- A 2D image (pixel brightness becomes amplitude).
- A 3D video (**Add Node → Terrain → From Video…**): an Import Video dialog lets you scrub/play the clip, crop it in space (a draggable rectangle) and in time (in/out handles on the timeline), and scale the cropped region down to a chosen pixel size (the width/height editors are aspect-linked) and a chosen number of frames. Each frame's grayscale brightness becomes a 2D amplitude surface and the frames stack along the time axis. Decoding shells out to **ffmpeg** (cross-platform; the feature greys out with an explanatory tooltip when ffmpeg isn't on `PATH`), and the decoded grid is baked into the project so reloading needs neither ffmpeg nor the source file. See [REFERENCE.md](REFERENCE.md#video-import-3d-terrain).
- An N-D wavetable (each axis becomes a Position knob on the synth node).
- A math expression like `sin(x*y)` evaluated over a grid.

The terrain visualizer has **+ Dim / - Dim** buttons to add or remove dimension axes at runtime (up to 8D). Each dimension adds a signal input pin and Center/Radius parameter knobs on the synth node. When the terrain has more than 2 dimensions, a projection combo lets you choose which two axes to view in the heatmap.

Traversal modes:

- **Linear** — sweep a single axis at constant speed. The standard wavetable / sample-player behavior.
- **Orbit** — circle around a center point in 2D-or-higher. Produces evolving pad sounds where the traversal periodically revisits each region.
- **Lissajous** — figure-8 / lemniscate / other patterns formed by independent X and Y oscillators at different rates. Complex periodic timbres.
- **Path** — a user-drawn polyline through the terrain. Click points to define vertices, or freehand-draw a curve. Loop or bounce playback at the ends.

The same terrain with different traversals produces wildly different sounds. The same traversal with different terrains does too. Per-axis parameters of each can be modulated by signals, automation, or live MIDI controllers — turning a static N-D terrain into a fully alive, controllable instrument.

Render modes (Synth Mode parameter on the Terrain Synth / Wavetable nodes):

- **Direct** — the terrain value at the traversal point *is* the audio sample. Pitch comes from how fast the traversal sweeps. The classic wavetable / sample-player behaviour: cheap, alias-prone on bright cycles at high notes, and the right choice for 1D wavetables and sample playback.
- **AM-sine** — a pitched sine oscillator is amplitude-modulated by the terrain value. Pitch comes from the sine, not from the traversal speed, so 2D/N-D terrains and slow orbit/Lissajous paths stay in tune. Intended for terrain sonification (image terrains, math terrains, fractal noise, slow orbits in N-D wavetables) where Direct mode would just sound like a low rumble.
- **Additive bank** — at the start of every audio block the current single cycle of the terrain is FFT'd and the magnitudes/phases drive an independent per-partial sine oscillator bank (up to 64 partials, Nyquist-culled). Higher CPU cost than Direct, but every partial is its own clean sine — no aliasing, very smooth pitch sweeps, and a natural fit for high-quality wavetable playback. The Compare panel in the Layered Wave editor switches between Direct and Additive bank on the selected wavetable cycle for instant A/B.

### Layers (time-gated cables)

Most DAWs treat "which effects are active" as a fixed property of a track. SEANCE lets you make that time-varying: a specific cable can be **on** only during certain beat ranges and **off** otherwise.

A layer is a colored bar drawn in the routing strip above the piano roll. You drag a region to set the start and end beats. Outside the region, the cable is muted with a smooth crossfade at the edges (configurable globally; default 50 ms) so you never hear a click when a layer turns on or off.

This is how you'd put reverb on the chorus only without automating a dry/wet knob, or activate a parallel filter chain on the bridge only, or have one synth speak only during the second verse.

### Groups (effect groups)

Sometimes you want to activate **multiple cables at once** — e.g., a reverb send AND a delay send AND a filter modulation, all gated together as one "chorus effects on" layer. That's what an Effect Group is: a named bundle of cables that activate together as one unit.

To create a group: right-click any cable → Effect Group → New Group, give it a name, then right-click other cables → Effect Group → Add to <name>. Now you can create a single layer that references the group, and all cables in the group activate or mute as one.

Each group gets its own color, drawn on member wires as visual tags (circles for individual wire identity, diamonds for group membership) so you can see at a glance which routings belong to which groups.

### Triggers

A Trigger node sits on a MIDI cable and fires *extra* events whenever a note passes through. The original note still flows through unchanged — the trigger only adds. It's the unified solution for everything you'd normally use a separate "chord generator" plugin or "envelope follower" plugin for, in one rule-list editor.

Two rule kinds, freely mixed:

- **MIDI rules** generate additional MIDI notes per incoming note. Each rule specifies a pitch offset (in semitones), an optional time delay, a velocity scaling, and a duration. Stack several rules to build chords, octave doubles, drum flams, transposed harmonies, or echoing arpeggios from a single played note.
- **Signal rules** generate a control signal on the Trigger node's Signal output, fired by each note. Each rule picks a shape (Step, smooth Envelope, Ramp, or From-Velocity), a duration, and a min/max output range. Wire the Signal output into a synth parameter to add per-note envelopes that the synth's own ADSR couldn't easily produce — pluck transients, velocity-followed filter sweeps, gated rhythm modulation, etc.

Five presets cover common patterns: **+Octave** (doubles every note one octave higher), **Chord** (turns each note into a major triad), **Flam** (re-triggers each note ~30ms later for the drum-machine flam effect), **Pluck** (fires a short envelope on every note for filter sweeps), and **Velocity follower** (outputs a constant signal proportional to the note's velocity for "play harder, modulate further" effects).

Triggers are generators, not filters — they always pass the original note through. To suppress or replace incoming notes, use a different effect node.

### Convolution

Convolution is the math under the hood of basically every "real space" reverb and "real cabinet" guitar amp simulation. You take your audio and combine it with a stored sound called an **impulse response** (IR) — and the result is your audio re-shaped by the IR's spectral and temporal character. Record what a clap sounds like in a cathedral, save it as the IR, and any audio you push through the convolution filter sounds like it was played in that cathedral. The same trick works for guitar speaker cabinets, EQ matching, and surgical custom filters.

SEANCE's convolution node has an editor with three ways to build an IR:

- **Presets** — pick lowpass / highpass / bandpass / echo from a dropdown, tweak Cutoff / Steepness / Bandwidth / Delay / Feedback / Echo count sliders, click Apply Preset. The IR is generated from the parameters and you can audition immediately.
- **Drawing** — sculpt the IR by hand. Two modes:
  - **Control points** — drag a few smooth control points to shape a curve. Catmull-Rom interpolation between points gives flowing shapes from just a few clicks.
  - **Freehand** — draw individual sample values directly with the mouse. Good for sharp transients and surgical per-sample edits.
- **Load a .wav** — pick any audio file as the IR. Use real cabinet IRs, real room reverbs, anything you have on disk.

The editor includes a **frequency response preview** that updates as you draw, so you can see what filter shape your time-domain IR is producing in the frequency domain. Mouse-wheel zoom (up to 128×) reveals individual samples as stems with sample-boundary grids when zoomed in enough.

#### Room IR capture

For capturing real-world impulse responses, the **Room IR Capture** tool (Tools menu) plays a sine sweep through your speakers, records the room's response with your microphone, and automatically loads the captured IR into a new Convolution node. Place a mic where the listener would sit, point the speakers at the room, click Capture, and you have a convolution-ready IR of that exact space.

## Features

### Built-in instruments

- **Wavetable / Layered Waveform synth.** Build single-cycle waveforms by stacking layers. Each layer is one of seven shapes (sine / saw / square / triangle / noise / hand-drawn / **formula**) at a chosen harmonic ratio, phase, and amplitude. Hand-drawn layers support two editing sub-modes: **Points** (Catmull-Rom control points) and **Freehand** (click-and-drag per-sample painting). Formula layers take a single math expression in `x` (radians over one cycle) - same vocabulary the frequency-domain editor uses, so anything from `tanh(3*sin(x))` to `sin(x) + 0.3*saw(3*x)` is one field away. The formula can be written in the **built-in** expression language, **Lua**, or **Python** (a per-layer language dropdown) - the script languages bake to samples offline so they stay off the audio thread. Stack multiple cycles into a wavetable that morphs as you play. See the [Wavetable arrangement](#wavetable-arrangement) concept above for the full structure.
- **Frequency-domain (spectral) synth.** Author the sound directly as a magnitude curve and a phase curve over FFT bins; the synth IFFTs to a single-cycle waveform. Each curve can be defined as a **formula** - in the **built-in** expression language (`f` = the bin index), **Lua**, or **Python** (`x`/`f` = normalized position over the curve) - or **drawn graphically** with the same Points / Freehand sub-modes as the waveform editor. The phase canvas sits above the magnitude canvas, both update a live time-domain preview as you edit. Old projects authored with the original three-line spectral dialog open in the new editor and round-trip losslessly.
- **Wavelet Space synth.** Author the sound directly in the wavelet domain by painting on a **scales × time grid** of DWT coefficients. The top row is the approximation band (overall shape), and each row below is a finer-resolution detail band — paint a cell at the toolbar Amp value (Shift-click for negative, right-click to erase) and the synth IDWTs the grid into a single-cycle waveform, updating a live preview as you edit. Pick the wavelet family (db1/Haar, db2, db4, sym4) to change the reconstruction shape from the same coefficient grid: db1 gives blocky/stepped tones, db4 is a balanced default, sym4 is smooth and symmetric. Useful for designing sounds whose character is more naturally expressed as "energy at this scale, at this time within the cycle" than as harmonic spectra.
- **MultiSampler.** Zone-based sampling instrument with N sample slots, each with configurable key range (low/high note), velocity range (low/high velocity), base note, fine-tune, gain, and pan. Multi-point linear envelopes for volume, pan, pitch, and filter with sustain/loop markers. Built-in state-variable filter (lowpass / highpass / bandpass) with envelope modulation. Replaces the old single-file Sampler — a one-zone MultiSampler covering 0-127 is exactly the old workflow, while multi-zone configs give you velocity-layered and key-split instruments. Legacy `__audio__:` projects auto-upgrade on load.
- **FM Synth.** 4-operator frequency modulation synthesis with 8 selectable algorithms, per-operator frequency ratios, levels, and ADSR envelopes, and operator-4 self-feedback. Produces electric pianos, bells, metallic basses, evolving pads, and everything in between.
- **Phase Distortion Synth.** Casio CZ-style synthesis: warps a sine wave's phase with a modulator function to produce subtractive-like sounds (sawtooth, square, pulse, resonant) without a traditional filter. "Depth" parameter + DCW envelope shape the harmonic content over time for filter-sweep-like timbral motion.
- **Particle Cloud Synth.** Granular texture instrument that generates many overlapping short waveform bursts ("particles") with per-grain frequency randomization, stereo scatter, and mini-envelopes. Produces ambient pads, evolving textures, and glitchy effects. Controls: density, frequency spread, grain size, per-grain attack/release, waveform shape (sine/saw/square/noise).
- **Drum synth.** Eight analog-style voice algorithms — **Kick** (pitched sine sweep), **Snare** (filtered noise), **Hi-Hat** (FM noise), **Clap**, **Tom**, **Cowbell**, **Rimshot**, **Cymbal** (with crash/ride/bell variants). Each voice gets its own MIDI note assignment via MIDI Learn and a four-knob synthesis row (pitch, decay, tone, level).
- **SoundFont (SF2 / SFZ).** Load multi-sample patches with full preset / bank navigation. Hundreds of free SF2 packs available online for orchestral, piano, drums, ethnic instruments, etc. SF2 support is complete; SFZ uses a built-in basic loader, with full SFZ-spec compliance via the sfizz library on the roadmap.
- **Terrain Synth.** The N-dimensional sample-array engine that powers the wavetable and sampler instruments — see the [Waveterrain](#waveterrain-terrain-synth) concept above. Can also be used directly with image (2D), audio file (1D), or math expression sources. Math-expression terrains are creatable at any dimensionality from 1D through 8D via the *N-D Terrain (custom expression)* menu item, with axis variables `x, y, z, w, v, u, s, t` corresponding to dimensions 1-8. The terrain visualizer's `+ Dim` / `- Dim` buttons also extend or contract dimensionality on an existing node, adding/removing Sig input pins and per-axis Center/Radius parameter knobs.

#### Shared AHDSR envelope

The synths built on the shared envelope engine — **Terrain Synth** (and its Wavetable / Frequency-domain / Wavelet-space variants), **Additive**, **Phase Distortion**, and **Spectral Grain** — all use the same amplitude envelope, edited the same way. Right-click one of those nodes → *Envelope (AHDSR)…* opens a shared editor with five stages (Attack, Hold, Decay, Sustain, Release), per-segment shape curves, velocity sensitivity, a live preview, and a project-independent preset library with editable factory presets. (Synths with their own built-in envelopes — FM, Particle Cloud, Drum, and the SoundFont/SFZ/MultiSampler players — keep their internal envelope and don't show this editor.) Tonal synths also get an **Aftertouch** signal input pin so MIDI channel-pressure (or any Signal source) can drive expressive per-voice volume swells on top of the envelope. See [REFERENCE.md](REFERENCE.md#shared-ahdsr-envelope) for the full feature breakdown.

### Supported plugin and instrument formats

| Format | Type | Platforms | Notes |
|---|---|---|---|
| **VST3** (64-bit) | Plugin (instrument or effect) | Windows / macOS / Linux | Standard third-party plugin format. Full state persistence, parameter automation, MIDI Learn for any plugin parameter. |
| **VST3** (32-bit) | Plugin | Windows | Planned, depends on out-of-process plugin sandboxing (roadmap). 32-bit DLLs can't load into a 64-bit host directly, so this requires a child-process plugin host. |
| **AU** (Audio Units) | Plugin | macOS only | Apple's plugin format. Same hosting infrastructure as VST3. |
| **LV2** | Plugin | Windows / macOS / Linux | Open-spec plugin format. JUCE 8 supports LV2 hosting on all platforms, not just Linux. |
| **LADSPA** | Plugin | Linux only | Older Linux audio plugin format. |
| **SF2** (SoundFont 2) | Instrument file | All platforms | Multi-sample patches with preset/bank navigation. Built into SEANCE, no external library needed. |
| **SFZ** | Instrument file | All platforms | Basic spec coverage built in; full compliance via the sfizz library is on the roadmap. |
| **WASM modules** | Custom audio DSP node | All platforms | Write your own audio effects or instruments in C, Rust, Zig, or AssemblyScript, compile to WebAssembly, load as audio nodes with sample-accurate parameter input. Hosted via wasm3. |
| **VST2** | — | — | Not supported. Steinberg deprecated VST2 and the SDK is no longer freely distributable. |

Loaded plugins are scanned and indexed at startup (cached so subsequent launches are fast); plugin scan directories are configurable via Settings → Plugin Settings, and plugins that crash during scan are automatically blocklisted so the next scan skips them.

### Built-in effects

All built-in effects can be combined freely with cables — pre-effect, post-effect, parallel, sidechained, time-gated, signal-modulated. Each one is a node in the graph with a left-side audio in pin and a right-side audio out pin (some also have signal modulation inputs for audio-rate or block-rate parameter control).

**Modulation effects**

- **Tremolo** — amplitude modulation by a built-in LFO. Rate and depth controls. Classic surf-guitar wobble or slow-pad shimmer.
- **Vibrato** — pitch modulation by an LFO. Subtle pitch wobble for expressive lead lines, or extreme settings for dive-bomb effects.
- **Flanger** — feedback delay with a modulated delay time. Creates the metallic comb-filter "jet plane" sweep effect.
- **Phaser** — cascade of all-pass filters with modulated cutoffs. Sweeping notch-filter sound, smoother and more vintage than a flanger.

**Time-based effects**

- **Echo / Delay** — repeating echoes with feedback. Configurable delay time, feedback amount, and number of repeats. Slap-back to ambient washes depending on settings.
- **Convolution filter** — apply any impulse response (preset, hand-drawn, or loaded .wav) to any audio. Used for filters, real-room reverbs, guitar cabinet sims, EQ matching. See the [Convolution](#convolution) concept above.
- **Pitch Shifter** — change pitch independently of speed via Rubber Band. Drop your vocals down an octave without slowing them down, or pitch up a sample without speeding it up.

**Reverb and spatial**

- **Reverb** — algorithmic Freeverb: 8 parallel lowpass-feedback comb filters + 4 serial allpass filters per channel. Controls: mix, room size, damping, stereo width, pre-delay. High-frequency decay simulates real-room absorption.
- **Parametric EQ** — 4-band biquad EQ. Each band has selectable type (peak / low shelf / high shelf / highpass / lowpass), frequency, gain (dB), and Q. Uses the standard RBJ Audio EQ Cookbook coefficients.

**Dynamics**

- **Compressor** — reduces dynamic range above a threshold. Threshold, ratio, attack, release, makeup gain. **Sidechain input**: wire any audio source (e.g., kick drum) into the Signal "Sidechain" pin and the compressor's envelope follower triggers from that signal instead of the main input.
- **Limiter** — hard ceiling on output level. Prevents clipping no matter what's coming in. Use as a final stage before the Master Out.
- **Gate** — silences the signal when it falls below a threshold. Used to clean up noisy tracks, tighten drums, or as a creative rhythmic gate.

**Creative / utility**

- **Ring Modulator** — multiplies the input audio with an internal oscillator (sine, square, or triangle) to produce metallic, bell-like, inharmonic tones. Classic Dalek-voice / sci-fi sound design tool.
- **Mid/Side Encode + Decode** — utility pair for mastering. Encode splits stereo into Mid (center) + Side (width); Decode recombines them. Wire EQ or compression between them to process mid and side independently.

**MIDI processors**

- **MIDI Modulator** — uses signal inputs to modulate MIDI attributes (velocity, pitch bend, mod wheel, aftertouch, any CC) on a passing MIDI stream at sample-accurate rate. Add as many signal inputs as you want; each one maps to a different MIDI target.
- **Trigger** — fires additional MIDI events and/or signal envelopes per incoming note. Use for chord generation, octave doubling, drum flams, transposed harmonies, per-note filter envelopes, velocity-followed modulation. See the [Triggers](#triggers) concept above.
- **Arpeggiator** — takes held notes and outputs them as a sequence (up, down, up-down, random, played-order, etc.) at a chosen rate.
- **Algorithmic MIDI** is a mode of the unified **Script** node (below in *Spatial / utility*): give a Script node one or more MIDI outputs and write a program that emits MIDI live with `note(pitch, vel, durSec)`, `noteon`/`noteoff`, `cc(number, value)` and `bend(value)`. Variables you assign **persist across samples** (running counters, phases, RNG seeds), statements are separated by `;` or new lines, and the program reads the transport, the MIDI-input note state, and any signal inputs. For example, a four-on-the-floor kick is `ph = beat - floor(beat); (ph < dt*bpm/60) ? note(36, 110, 0.1) : 0`. See [Script](REFERENCE.md#script-signal--midi) in REFERENCE.md.

**Spatial / utility**

- **3D Spatializer** — HRTF binaural audio placement with X/Y/Z position parameters. Wire signal sources into the position pins to move the sound source around the listener's head in 3D in real time.
- **Script (signal + MIDI)** — one programmable node that covers LFOs, envelopes, **and** algorithmic MIDI generation, with **user-configurable I/O counts** so you decide what it does. Build a shape from a **stack of layers** (each a sine / saw / square / triangle / noise / drawn points / freehand / formula) summed into one contour, using the exact same layer editor as the wavetable instrument — *+ Layer* button, preset menu, and all — with a live "sum of all layers" preview. A fresh node starts with **zero layers** (flat 0 output) so it never sweeps anything until you add a layer. The shape advances at a chosen rate (free or beat-synced), and a per-sample program turns the curve into the output (defaults to "pass through"). A separate trigger expression decides when the cycle starts: empty = always running (LFO), or anything that goes high on a note — e.g. `gate` — gives envelope behaviour. Choose **Forever / Once / N times** for what happens at the end of a cycle. In the editor you set four I/O counts: **signal inputs** (`s1..sN`), **signal outputs** (`o1..oP`, assigned in the program; `o1` defaults to the curve), **MIDI outputs** (0 = pure signal source; ≥1 lets the program emit MIDI with `note(pitch,vel,durSec)`, `noteon`/`noteoff`, `cc(num,val)`, `bend(val)` — `out = k` selects which MIDI output pin), and a **MIDI input** toggle (drives `note`/`vel`/`gate`/`freq`). A **Pin type** dropdown flips all continuous pins between Signal (blue) and Param (orange). So 0 MIDI outputs = a classic LFO/envelope, a MIDI-emitting program with no continuous output = a classic algorithmic MIDI generator, and both at once = a hybrid (e.g. an arpeggiator that also outputs an envelope). A **Manual Trigger** button auditions one-shot shapes without wiring anything. A **Language** dropdown lets the program run in the built-in expression language, **Lua** (per-sample, transforming the drawn `curve`, or per-block, filling the buffer directly), or a **WebAssembly** `.wasm` module. Double-click the node to edit. See [Script](REFERENCE.md#script-signal--midi) in REFERENCE.md for the full vocabulary.
- **XY Pad** — two-axis click-and-drag controller, mapped to any combination of node parameters. Great for live performance.
- **Control Bank** — a bank of *N* manual macro faders, each emitting its own control-signal output pin (0..1) you can wire into any parameter. Add or remove sliders in the editor and rename each one (the name flows onto its output pin). The editor window is resizable, and stretching it makes the sliders longer — a longer fader spreads the same 0..1 range across more pixels, so dragging it becomes proportionally more precise. A toggle switches between vertical faders (laid out in a row) and horizontal sliders (stacked). Double-click the node to edit. See [Control Bank](REFERENCE.md#control-bank) in REFERENCE.md.
- **Spectrum Tap** — analyzes audio and outputs amplitude signals for configurable frequency bands. Each bin exposes its own Signal Out pin on the node, so you can wire individual bands to different downstream parameters (spectrum-following effects, vocoder-like routing, visualizers, etc.). Audio passes through the node unchanged. By default each bin uses a 2nd-order biquad bandpass at the bin's centre frequency, but right-clicking a bin opens a menu with **Edit response curve...** — this lets you author a custom frequency-response shape inside the bin's range using the same three-mode curve editor used elsewhere in the app (Equation / Drawn points / Freehand), giving you arbitrary bandpass / notch / comb / tilted / multi-peak shapes per bin instead of a fixed bandpass. Bins with a custom response trace the chosen shape live inside the bin in the editor. The editor also has an **FFT size** dropdown (128 / 256 / 512 / 1024 / 2048 / 4096 / 8192) that controls how many frequency bins the custom-response FFT uses — smaller sizes give lower latency and CPU cost (latency ≈ size / sample rate) at the cost of coarser per-bin frequency resolution; the setting only affects bins with a custom response.
- **Spectrum Analyzer** — passthrough effect node that shows a live FFT bar display of the audio flowing through it. Double-click the node to open the display in a non-modal, resizable floating window — drag it wider to add more frequency bars (roughly one bar per 4 pixels of canvas width, so a wider window shows finer spectral detail), drag it taller or shorter to scale the bar height. Unlike the global View → Spectrum Analyzer (which always measures the master output), this node measures whatever you wire into it, so you can drop it mid-chain to see what a specific effect or instrument is producing.
- **Oscilloscope** — passthrough effect node that shows the time-domain waveform of the audio flowing through it. L and R channels are overlaid in two colours. Double-click the node to open the scope in a non-modal, resizable floating window — drag it wider to see more samples per frame (one pixel = one sample, capped at 4096 samples / ~93 ms @ 44.1 kHz once the capture buffer fills), drag it taller or shorter to scale the waveform height. A small overlay in the top-right of the canvas shows the current sample count and the equivalent time span.
- **Spectrogram** — passthrough effect node that shows a scrolling time-frequency waterfall display: new FFT frames scroll in from the right edge, older frames slide left and eventually off-screen. Log-frequency vertical axis, magma-style colour ramp on intensity. Double-click the node to open it in a non-modal, resizable floating window — drag it wider to keep more history visible on screen, drag it taller to increase the FFT size (the analyzer aims for roughly two FFT bins per pixel of height, rounded to the next power of two, so taller windows resolve finer frequency detail at the cost of a slightly larger FFT per frame). Useful for seeing how spectral content evolves over time at any point in your signal chain.
- **Mixture** — sums multiple audio inputs into one output. Basic mixer node for combining several signal paths.
- **Peak Metering** — every node in the graph shows live green/yellow/red peak meter bars at its bottom edge during playback, captured from the post-pan audio each block. No separate meter node needed — the meters are always-on visual feedback built into the graph view.

### Capture and analysis tools

- **Room IR Capture** (Tools menu) — sample the impulse response of any real space by playing a sine sweep through your speakers and recording the response with your microphone. The captured IR is automatically loaded into a new Convolution node, ready to use as a "this sounds like that room" reverb. Configurable sweep length and recording length; the longer the sweep, the cleaner the result in noisy environments.
- **Audio bounce** (Capture button in transport) — render any node's output to a fresh Audio Track inside the project, either instantly from the cached output of the last playback or via offline re-render of the project from scratch.
- **Multi-track recording** — record multiple hardware audio inputs simultaneously, each routed into a separate Audio Track, with input arming per track.

### Routing and control

- **Cable-based routing.** Drag from any output pin to any compatible input pin. Audio (blue), MIDI (green), Param (orange, block-rate), and Signal (amber, audio-rate) cables are color-coded; pins light up bright yellow when you hover a valid drop target. See [Signals](#signals-the-four-pin-kinds) above.
- **Implicit Signal ↔ Param conversion.** Either control-rate kind plugs into either control-rate input. The cable shows its source colour at the head and destination colour at the tail so the conversion is visible.
- **Time-gated effect groups.** Make any cable active only during specific beat ranges, with smooth crossfaded edges. Group multiple cables so they activate together as one "layer." A routing strip above the piano roll shows which routings are live at which beats. See [Layers](#layers-time-gated-cables) and [Groups](#groups-effect-groups) above.

### MIDI

- **Computer keyboard as MIDI controller.** A-L for white keys, W-P for black keys, Z/X for octave shift. Velocity zones via Shift (loud) / Alt (soft) / plain (medium). Toggleable from the toolbar so the same keys can be used for shortcuts when you're not playing.
- **MIDI input as nodes.** Every input device — computer keyboard, hardware MIDI controllers, network MIDI clients, virtual ports — is its own MIDI Input node in the graph. Wire it to wherever you want the events to go. No hidden routing.
- **Hardware device wizard.** A first-launch dialog lists every detected MIDI input device with checkboxes; pick the ones you want to add. Re-openable from Options → Add MIDI Input Device.
- **Hotplug detection.** New MIDI devices plugged in mid-session prompt to be added to the graph automatically.
- **Pitch bend, mod wheel vibrato, sustain pedal, aftertouch** all handled in the built-in synths. Aftertouch comes in two flavors and both are supported: *channel pressure* (one value for the whole keyboard) and *polyphonic key pressure* (per-note — push one held key harder than another and only that note swells). Per-note pressure is routed to the right voice by note number inside the synth, since a mono signal cable can't say which note a single pressure value belongs to. Channel pressure is *also* exposed as a per-synth Aftertouch signal input pin so you can route it anywhere else in the graph — filter cutoff, FX wet mix, vibrato depth, whatever. See the *Shared AHDSR envelope* section above for the per-synth volume-swell mapping.
- **MPE (per-note expression).** Right-click a Timeline node → *Enable MPE* and each note plays out on its own member channel (2–16) carrying independent pitch bend, pressure, and timbre — bend one note of a chord while the others hold. MPE controllers (Seaboard, LinnStrument, Osmose) can be *recorded* into a track, capturing the per-note gestures as expression curves you can then edit in the piano roll — pick a Pitch Bend / Slide / Pressure lane and drag the breakpoints, smooth/thin the dense recorded streams, or bake a note's pressure onto any parameter's automation. Built-in synths read MPE natively; for a **hosted VST3/AU plugin**, right-click → *Enable MPE mode* (set it to match the plugin's own MPE setting) and SEANCE injects the MPE Configuration Message handshake plus spreads each note onto its own member channel.
- **MIDI Learn** for any node parameter — right-click → MIDI Learn, then move a knob on your controller. Learned CCs are filtered out of the cable routing so they only drive their mapped parameter.
- **Signal modulation for any parameter.** Right-click any param row on any node → "Add Modulation Input" → a new Signal pin appears. Wire an LFO, XY Pad, Envelope, or any signal source into it and the parameter wobbles in real time. The signal cable and automation lane coexist — automation sets the base value, signal modulation adds on top.
- **Song Length and Repeat.** Set a project-wide end beat and a repeat policy (None / Loop Forever / N Times) via the transport bar's "Song" button. Leave the length at 0 and the engine auto-derives it from the last clip across all timelines — pressing Play on a fresh project stops at the end of the music instead of running forever. At the end, playback enters a short "tail grace" period equal to the longest per-node tail (reverb, delay feedback, instrument release) so ringing sounds finish naturally instead of being cut off mid-decay. Tracker imports with whole-song loops wire this automatically.
- **TPDF dithering on export.** When exporting to WAV or FLAC at 16-bit (or any lower bit depth), triangular-PDF dither noise is applied so quantization artifacts become inaudible hiss instead of correlated distortion.
- **Custom hotkeys.** Bind any keyboard shortcut OR any MIDI controller button to any host action via the Hotkey Settings dialog. Includes per-node shortcuts (toggle mute on this specific track, open this specific editor, etc.).
- **Tracker file import (MOD / S3M / IT / XM) — unlock any tracker song into a full DAW workflow.** The whole point of this feature is that tracker files (.mod, .s3m, .it, .xm) are a decades-deep library of freely available music — thousands of songs from the demoscene and the broader tracker community — but they're trapped in a format that only tracker players can open. SEANCE breaks them out: import a tracker file and it becomes a standard SEANCE project that you can edit with all the tools of a modern DAW, then export as WAV, MP3, FLAC, or Opus. Every note is editable in the piano roll, every sample is a standalone WAV you can swap or process, and the full signal chain is visible as nodes and cables on the graph. You're not "playing back a tracker file" — you're working in a DAW that happens to have started from tracker data.

  What the importer does under the hood:

  - **Sample extraction.** Every sample in the module is rendered to a standalone WAV file (via libopenmpt's interactive playback interface) and saved to a `<song>_samples/` folder next to the source file. Each sample becomes a zone-based MultiSampler instrument node.
  - **Instrument mode (IT / XM).** When the module uses instruments (a layer above raw samples with key splits, envelopes, and per-instrument settings), SEANCE parses the raw file bytes to extract note-sample keymaps, volume / pan / pitch / filter envelopes, NNA (new-note action) defaults, fadeout rates, default pan, and initial filter settings. Each instrument becomes a single MultiSampler node with multiple zones covering its keymap — so a multisampled piano stays as one instrument, not N separate nodes.
  - **Per-format effect dispatch.** MOD, S3M, IT, and XM each have different effect-letter conventions. The importer detects the format and dispatches through format-specific handlers so the same underlying effect (e.g., "volume slide") is decoded correctly regardless of which tracker wrote the file.
  - **Comprehensive effect translation.** Arpeggio, portamento (up/down/tone), vibrato (with per-channel waveform: sine/ramp/square/random), tremolo, volume slides (coarse and fine), set volume, set panning (as Sampler pan-param automation), sample offset, retrigger, note cut, note delay, pattern delay, position jump, pattern break, pattern loops (unrolled inline as duplicated note data), set speed/tempo, global volume (baked into note velocities), panbrello (as pan automation curves), finetune, glissando control, vibrato/tremolo waveform selection, and combo effects (tone+vol slide, vibrato+vol slide).
  - **NNA (new-note action).** Per-channel NNA state (Cut / Continue / Off / Fade) is tracked through the pattern walk. Channels set to Continue or Off leave previous notes ringing through the Sampler's release envelope instead of hard-cutting them.
  - **Sound control (S9x).** When the module uses reverb-enable/disable commands (S98/S99), the importer inserts a real algorithmic Reverb node into the graph as a parallel wet send, gated by effect regions at the exact beats where the commands toggle — so the reverb fades in and out at the right moments during playback.
  - **MIDI macros (Zxx).** IT files can define arbitrary MIDI macro templates in their header. The importer parses the raw file bytes to read the macro table, then expands each Zxx command by substituting its parameter into the template and emitting the result as MIDI CC events on the track. The common default macro (filter cutoff sweep) maps to CC74 automatically.
  - **Song-loop detection.** If the order list contains a position-jump (Bxx) back to an earlier order, the importer sets the project's Song Length and Repeat Mode to "Forever" so the transport loops the song automatically — matching the tracker's intended playback behavior without unrolling the entire order list into a flat timeline.
  - **Per-channel stereo panning.** Classic MOD files use LRRL channel panning (channels 0,3 hard-left, channels 1,2 hard-right); S3M uses alternating L/R. The importer creates separate track/sampler pairs for each pan position so the original stereo image is preserved. Instruments with their own default pan (IT) override the channel pan. Tracker-imported nodes use linear panning law (matching tracker behavior) rather than the equal-power law used by normal DAW instruments.
  - **Selectable interpolation.** Tracker-imported samplers default to linear interpolation (authentic tracker sound). The interpolation mode can be changed per instrument: Linear (2-point, characteristic tracker grit), Cubic (4-point Catmull-Rom), or Sinc (8-point Lanczos-4, highest quality). New non-tracker instruments default to Sinc.
  - **Amiga reconstruction-filter emulation.** MOD and S3M imports route the sampler's post-mix output through a 4-pole Butterworth lowpass at ~5.5 kHz, emulating the analog reconstruction filter the Amiga PAULA chip applied to all module-era audio. Without it, naive sample-pitch playback produces broadband aliasing the reference (Winamp / OpenMPT) output doesn't have — audible as a brighter, harsher "texture". The filter is per-instrument and serialized with the project; IT and XM imports leave it off because those formats targeted PC sound cards with no equivalent analog stage.
  - **One MIDI track per (instrument, pan-group).** Notes from tracker channels that play the same instrument at the same pan position share a track. Channels with different panning get separate tracks so the stereo field is preserved. Per-channel monophony (a new note on a channel cuts that channel's previous note) is preserved even when multiple channels share a track.

  After import, hit Play — the song plays back through the extracted samples and the graph you can see. From there you can edit notes in the piano roll, swap samples, add effects (EQ, compression, delay, your own plugins), restructure the arrangement, change the tempo, re-export as a modern audio file, or use the imported material as a starting point for something new. That's the workflow: tracker file in, DAW project out.

### Editing

- **Piano roll** with click-to-place, drag-to-move, edge-to-resize, box-select, copy / cut / paste, alt-for-no-snap. Velocity lane (drag bar heights), automation lanes (Catmull-Rom curves on any parameter). Horizontal and vertical zoom sliders at the bottom-right (or Ctrl+scroll / Ctrl+Shift+scroll on the grid) let you thicken or thin the note lanes and stretch or shrink the timeline independently. A thin grip strip at the top of each piano roll panel can be dragged up or down to resize that track's height — handy for seeing more octaves at once, and when several piano rolls are stacked, resizing one shifts the tracks above it up or down so each track keeps the size you set. A draggable orange **END** handle at the end of the notes lets you shorten the song (dragging left trims notes past the new end) or lengthen it (dragging right adds empty beats).
- **Music theory helpers** — Root / Key / Mode / Scale dropdowns highlight in-key pitches on the piano roll, with **Snap to Scale** mode and **Detect Key** auto-analysis.
- **Note transformations** — transpose by octaves or semitones, nudge in time, double or halve duration, reverse, fine-tune detune in cents.
- **Quantize** — snap notes toward the nearest grid position with a user-adjustable strength slider (1-100%). At 100% notes land exactly on the grid; lower values move notes only partway, keeping a natural feel. Available as a toolbar button and via the right-click context menu.
- **MIDI Note Degree System.** Each note stores its scale degree (1st-7th), octave, and chromatic offset, so you can change key/scale and have the melody automatically re-pitch to fit (Major to Minor, Ionian to Dorian, etc.).
- **Microtuning (alternate temperaments).** Pick a project-wide tuning system — Equal Temperament, Pythagorean, Just Intonation, or Quarter-Comma Meantone — and a concert pitch (A4 = 440 Hz by default, with Baroque/Verdi/orchestral presets or a custom Hz). Built-in synths tune natively, and **hosted VST3/AU plugins are tuned too**: a cable-level adapter bends each note into tune automatically (per-note via MPE when the plugin is in MPE mode, or a uniform concert-pitch shift otherwise).
- **Audio timeline** with clip move/resize, fade in/out, slip offset, gain, snapping.
- **Multi-track recording** with simultaneous input routing for hardware-controller workflows.
- **Take lanes and comping** for capturing multiple performances and assembling the best parts.

### Reliability and crash safety

This is one of the things that sets SEANCE apart from typical home DAWs. Three independent layers protect your work:

- **Branching undo tree (not linear).** Every edit becomes an undo step. *Most* DAWs have linear undo: Ctrl+Z to back up, then any new edit destroys the path you'd backed away from. SEANCE keeps every path. Undo five steps, take a different direction, then walk back to the original branch and continue from where you'd left off — both branches stay alive forever in the tree. Each redo branch shows a chain of descriptions ("+1 octave → Move notes → x2 duration") so you can navigate the full history of "things you tried" without guessing which branch is which.
- **Cross-session undo persistence.** The full undo tree is saved to disk on every change (event-driven, coalesced per UI frame). Quit the app, reopen the next day, and Ctrl+Z keeps working back through everything you did last session — even past your last saved version.
- **Two-channel autosave.**
  - **Fast channel:** the undo tree itself, persisted on every gesture endpoint. Effectively zero data loss on a crash for graph edits — notes, cables, parameters, structural changes are all captured at gesture granularity.
  - **Slow channel:** plugin internal state, captured incrementally via per-plugin files. The slow channel runs on a background thread, so disk writes never affect UI responsiveness, and uses per-plugin dirty tracking so only the plugins you actually tweaked get re-serialized.
  - Both channels write to user app-data, never to your project file, until you explicitly save.
- **Adaptive autosave interval** based on whether you're on a desktop (5-second default) or laptop (20-second default to save battery). The autosave system detects the platform automatically and shows a one-time notice when it picks the laptop default.
- **Crash recovery.** On startup, if the previous session didn't shut down cleanly, you get a recovery prompt. Accept and the project comes back exactly as it was at the moment of the crash, with full undo history.

### Built-in documentation

- **Help menu** opens HTML documentation pages in your browser, covering the main concepts (graph basics, pin kinds, MIDI input architecture) and each major editor (piano roll, wavetables, terrain synth, trigger node, MIDI modulator, convolution filter).
- **In-editor help buttons** — every editor dialog has a `?` button that opens the relevant doc page.
- **Aggressive tooltips.** Every UI control whose meaning isn't obvious from its label has a tooltip. Music-theory terms (BPM, octave, semitone, modes, CC numbers) get plain-language explanations targeted at non-musicians.
- **Disabled controls explain themselves** — when a control is greyed out (signal-locked, wrong mode, etc.), its tooltip explains why and how to enable it.

### Audio I/O

- **VST3 plugin hosting** on all platforms; **AU** on macOS; **LV2 / LADSPA** on Linux.
- **Export to WAV, FLAC, OGG, Opus, M4A, WMA.** Choose format, channels (mono/stereo), sample rate, and format-specific options (bit depth, quality, or bitrate) upfront, then pick a filename.
- **Pitch shifting / time stretching** via Rubber Band.
- **ASIO support** on Windows for low-latency hardware audio.
- **Configurable project sample rate** with internal resampling.
- **Audio bounce** to render an audio clip from any node in the graph.

### Scripting

- **Python scripting** for project manipulation — add nodes, place notes, automate parameters, generate musical structures programmatically. A built-in script console with run / load / save / recent-files history.
- **WASM scripting** for real-time audio DSP. Write custom audio effects or instruments in C, Rust, Zig, or AssemblyScript, compile to WebAssembly, and load them as audio nodes with sample-accurate parameter input.

## Roadmap

A non-exhaustive list of planned and in-progress features. Things below are *planned*, not promised — order and scope may shift.

### Wavelet-based audio (large planned area)

Wavelets give you joint time + frequency resolution that traditional FFT-based effects can't match — sharp transients keep their attack while sustained tones get analyzed at high frequency resolution. This unlocks a whole class of effects that aren't widely available in commercial DAWs:

- **DWT / IDWT and CWT utilities** — the core forward/inverse wavelet transforms (Daubechies, Symlet, Biorthogonal families) plus continuous wavelet transform for non-dyadic scale operations.
- **Wavetable mipmap pyramid** — anti-aliased wavetable pitch-up via wavelet-based oversampling.
- **Wavelet-basis wavetable storage** — store wavetables in the wavelet domain for cheaper interpolation.
- **Wavetable complexity knob** — sparsify wavelet coefficients to smoothly simplify a complex waveform.
- **Self-similar / fractal wavetable generator** — build wavetables from fractal recursion.
- **Wavelet-domain morphing** in scatter wavetables.
- **Transient/sustain split node** — separate the attack from the body of any sound, route them independently.
- **Octave-band wavelet multiband compressor** — compression with naturally-shaped octave bands instead of artificial crossovers.
- **Wavelet shrinkage denoiser** — surgical noise reduction that preserves transients.
- **Wavelet bitcrush** — bit-reduction artifacts that hit only at the frequencies you choose.
- **Asymmetric / non-causal wavelet filters** — filters that anticipate transients ahead of where they happen.
- **Self-similar / 1/f wavelet reverb** — fractal reverb tails.
- **Scale-shift wavelet pitch shifter** — pitch shifting via wavelet scale modification rather than time stretching.
- **Free dyadic octave shifter / sub-octave** — clean octave doubling and sub-bass generation.
- **Independent transient + tonal pitch shifting** — keep drums punchy while pitching melodic content.
- **Adaptive resolution wavelet pitch tracker** — pitch detection that handles vibrato and bends gracefully.
- **Formant-preserving pitch shift via wavelet packets** — vocal pitch shifting without the chipmunk effect.
- **Wavelet-band vocoder** — vocoder using wavelet bands instead of fixed FFT bins.

### Synthesis

- **FM Synthesis instrument** — DX7-style multi-operator FM synth with selectable algorithms, per-operator frequency ratios, modulation indices, and envelopes. Produces electric pianos, bells, metallic basses, evolving pads. The most-requested synthesis type after subtractive and wavetable.
- **Phase Distortion Synthesis instrument** — Casio CZ-style PD: warps a carrier sine's phase with a modulator shape to produce subtractive-like sounds (brass, filtered pads) through a fundamentally different mechanism. Niche but differentiating — most DAWs don't offer it.
- **Spectral Modeling Synthesis (SMS)** — decomposes any sound into harmonic peaks (deterministic sinusoids) and noise (stochastic residual), then lets you manipulate each independently. Make a voice breathier without changing pitch, isolate transients, cross-synthesize tonal and noisy components from different sources. Builds on the existing FFT infrastructure.
- **Particle / granular cloud synth** — a dedicated texture instrument that generates many overlapping short waveform bursts ("particles"), each with its own mini-envelope, frequency, and harmonic parameter. Produces ambient pads, evolving textures, and glitchy effects. Based on the iMS-20 particle generator concept.
- **Spectral grain mode** — IFFT-to-windowed-grain spectral grain synthesis as a fourth Wavetable render mode alongside Direct, AM-sine, and Additive bank. Reuses the same cycle FFT that Additive bank already computes, but instead of summing partials it IFFTs slightly windowed chunks for granular textures.
- **Inharmonic presets and ratio expression** for the Additive bank mode — Bell, Drum, Stretched Piano starting points plus a custom `ratio(f)` expression for designing your own inharmonic partial series.
- **Spectral visualizer pane** — live spectrum + resulting grain waveform.

### Mixing and mastering

- **Real parametric EQ** — multi-band parametric EQ replacing the current stub: low shelf, high shelf, 2-4 peak bands, HP/LP filters, each with freq/gain/Q/type controls. Essential for mixing.
- **Metering** — per-node peak meters, master LUFS loudness metering (broadcast standard), and a spectrum analyzer. You can't mix properly without visual level feedback.
- **Sidechain compression** — add a sidechain audio input to the existing Compressor node so a kick drum can duck the bass (or any other source can trigger compression on another signal).
- **Mid/Side processing** — a utility node pair (M/S Encode + M/S Decode) that splits stereo into Mid (center) and Side (width) for independent processing. Wire EQ, compression, or saturation between them. Standard mastering technique.
- **Dithering on export** — triangular PDF dither with optional noise shaping for bit-depth reduction (24→16 bit). Turns quantization distortion into inaudible hiss. Applied at the export stage only.
- **Ring Modulator effect** — multiplies two audio signals sample-by-sample, producing metallic, bell-like, inharmonic tones. Two audio inputs (carrier + modulator). Classic Dalek-voice / sci-fi sound design tool.

### MIDI and recording

- **Per-node recording** ("Record Here" mark) — arm individual nodes for MIDI capture instead of arming whole tracks, so anything in the graph can be recorded into.
- **Trigger node v2** — multiple MIDI/Signal output pins, per-rule output assignment, signal-rule delay support, beats/ms unit picker.
- **Trigger node enhancements** — note-off triggers (actions fire on note release, not just note-on), free-drawn Curve shape, and a threshold-trigger input (audio level → trigger bridge so loud audio events can fire triggers without MIDI).

### Plugin / sample library support

- **Out-of-process plugin sandboxing** — host VST3/AU plugins in a child process so a crashy plugin can't take down the whole DAW. Includes per-plugin latency settings, owner-window-based UI, and graceful crash recovery.
- **sfizz integration** — full SFZ specification compliance for the SFZ instrument format (currently we have a limited internal SFZ loader).
- **Measured HRTF datasets (SOFA / WAV)** — load published HRTF measurements for the 3D Spatializer instead of using the built-in synthetic head model.
- **Plugin UI modulation indicators** — show on the host's plugin window which parameters are currently being modulated by automation, signal cables, or MIDI Learn.
- **Plugin parameter sanitization** — cap visible param count and fix invalid min/max ranges for badly-behaved plugins that report thousands of parameters or nonsensical ranges. Prevents UI lockups.

### Editor and workflow

- **Make the layered editor non-modal** — let the wavetable editor stay open while you work elsewhere in the graph.
- **Persist project-wide settings** in `.ssp` (tuning system, concert pitch, crossfade duration, effect group definitions) — currently some of these don't survive save/load.

### Reliability

- **Power-state-aware autosave** — detect AC vs battery on laptops and adjust autosave interval automatically. Live response to plug/unplug events. Per-platform code paths for Windows / macOS / Linux.

### Scripting

- **Python scripting bindings** — expand the existing Python scripting console with a complete API for project manipulation (add nodes, place notes, automate parameters, generate musical structures programmatically).

## Building

### Prerequisites

- Windows 10/11, macOS, or Linux
- Visual Studio 2022 (Windows), Xcode (macOS), or GCC/Clang (Linux)
- CMake 3.22+
- [JUCE 8.0.12](https://juce.com/get-juce/download)
- Python 3.10+ (for scripting support)

### Setup

1. Install JUCE and note the path.
2. **Apply the required JUCE patches.** SEANCE depends on small fixes to JUCE
   that live in [`patches/`](patches/) and must be re-applied to every fresh
   JUCE install/upgrade (e.g. a WASAPI capture fix without which some USB mics
   record as garbage). From the JUCE root:

   ```
   cd /d D:\JUCE-8.0.12
   patch -p1 < "path\to\SoundShop2\patches\juce-wasapi-ieee-float-tag.diff"
   ```

   See [`patches/README.md`](patches/README.md) for the full list and details.
3. Run the dependency setup script:

   ```
   cd cpp
   setup_dependencies.bat    (Windows)
   ```

4. Build:

   ```
   cd cpp
   cmake -B build -G "Visual Studio 17 2022" -A x64 -DJUCE_DIR=D:/JUCE-8.0.12
   cmake --build build --config Release
   ```

   On Linux:

   ```
   cd cpp
   cmake -B build -DJUCE_DIR=/path/to/JUCE
   cmake --build build
   ```

### Optional dependencies (downloaded by setup script)

- **Rubber Band** — pitch shifting / time stretching
- **libopenmpt** — MOD / S3M / IT / XM import
- **libopus + libogg** — Opus audio export
- **wasm3** — WASM script nodes

## Documentation

The application's Help menu opens HTML documentation directly in your browser. The same files live in [`docs/`](docs/) and can be browsed online.

For the full feature reference — exact button labels, dialog semantics, edge cases, file locations, on-disk formats — see [`REFERENCE.md`](REFERENCE.md). The README gives the high-level tour; REFERENCE.md is where the granular behaviour lives.

For a deeper architectural overview — the data model, save/load lifecycle, undo strategy, and the rules for adding new features — see [`CLAUDE.md`](CLAUDE.md).

For the WASM scripting layer specifically, see [`cpp/scripts/wasm_examples/README.md`](cpp/scripts/wasm_examples/README.md).

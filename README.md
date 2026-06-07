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

   **Capture from real audio (granular waveforms).** Captured waveforms are a fifth kind — instead of storing a single boundary-cleaned cycle they hold a multi-second window of source PCM plus a grain length, and the synth plays them back via a 4-voice overlap-add granular stream (Hann-windowed grains with jittered source-start positions) at the held note's pitch. That means the captured sound's *time-domain evolution* is preserved at playback: hold a note and you hear a continuous textural drone derived from the source material, not a repeating single cycle. Granular waveforms coexist with cycle waveforms (Layered / Frequency Domain / Wavelet) in the same wavetable, with morphing handled per-layer — the cycle portion of the morph plays through the terrain as usual, the granular portion plays through the OLA stream, and the result is summed sample-by-sample, so sweeping Position smoothly cross-blends granular textures with cycle waveforms. MIDI pitch tracking uses an embedded reference frequency stored with each granular waveform — set at capture time via the dialog's **Note + Octave** picker (default A4 = 440 Hz) and re-editable later from the per-waveform editor's Hz slider; per-waveform automatic pitch detection is a planned follow-up.

   The **+ Waveform** menu's three capture entries open dialogs that pull from different sources:
   - **From project song...** — pre-renders the whole project to audio when the dialog opens, then shows it as a song-length timeline. Press **Play** to hear the song at full fidelity (so it's easy to identify the spot you want); press **Pause** or drag the orange marker on the timeline to switch to a continuously-looping audition grain centered on the marker. The orange band highlights the audio range that would be captured. Tune the **Width** slider (10–500 ms, default 100 ms) to choose the grain length — shorter for tighter pluck-like tones, longer for pad/drone-style waveforms. The **Crossfade** slider (1–500 ms, default 50 ms; capped internally at half the Width) sets how much of each iteration is blended across the seam in Crossfade-loop mode — short for a tight bare loop where the period is most audible, long for a smooth rolling crossfade between two playheads that hides the periodicity at the cost of some smearing. The **Freeze** dropdown picks the algorithm used to sustain the marker spot while a captured note is held — **Crossfade loop** (faithful tape loop of the chosen window with a Hann fade across the seam whose length you set with the Crossfade slider; source pitch is baked in and the synth resamples to your MIDI pitch, sampler-style), **Async granular** (many short grains at randomised positions for a frozen blur / GRM-Freeze texture), **Pitch-sync grains** (grain hop locked to a single detected pitch period for a sustained tone at that pitch; needs a pitched source), or **Spectral freeze** (FFT magnitudes preserved, phases regenerated per waveform for ethereal pad sustain). The picker drives both the live audition and the algorithm baked into the saved waveform, so you can A/B between modes before committing. The **Note** and **Octave** dropdowns label the captured spot with a pitch (default A4 = 440 Hz) so MIDI playback at the matching key plays it at 1:1 and other keys pitch-shift via the synth's usual wavetable stride math — set this to whatever pitch you can hear in the marker's loop before saving. Press **Save waveform at marker** to commit a granular waveform — it is added to the Library list only, leaving the arrangement untouched. To place the new waveform in the arrangement, select a cell in the arrangement view and press **Assign to selected cell** in the Library list sidebar. The dialog stays open after each save so you can scrub on and grab more spots from the same song. (Save is greyed out while Play is running, since the marker is moving — pause or drag first.)
   - **From microphone / audio input...** — shows the last ~10 seconds of audio from your input device. Make a sound; the buffer fills continuously and a small status line tells you whether any input signal has been detected yet.
   - **From audio file...** — press **Load file...** and pick a .wav/.mp3/.aiff/.flac/.ogg. The file is decoded (long files are truncated to 60 s at 96 kHz to keep the display readable) and shown end-to-end.

   For the Mic and File dialogs you drag the orange handles to pick a region, choose how many waveforms to extract (1–32, default 8), label the captured source's pitch via the **Note + Octave** dropdowns (default A4 = 440 Hz, used as each waveform's embedded pitch reference for MIDI playback), and press **Capture waveforms**. The selected region is split into that many equally-spaced positions; each one becomes a granular waveform holding a ~1-second source window around its position (clamped to the region) with a default 100 ms grain length. The waveforms are appended along the Position dimension so sweeping Position morphs through the captured timbre evolution. The mic dialog auto-refreshes the waveform display; toggle **Freeze view** to pause it while you drag handles. The project-song dialog uses the transport / marker model described above and creates a single granular waveform per save instead. All three are how you turn a riff, a synth lead, your own voice, or any recorded sample into wavetable material the synth can morph through.
2. **Waveforms within an N-dimensional arrangement.** Multiple waveforms stack into a wavetable that morphs as you change Position. The arrangement is N-dimensional — add an axis to get a second Position knob, add another for a third, etc. Most uses stay at 1D or 2D, but the option goes up to 8 dimensions.
3. **Two layout modes for the N-D arrangement:**
   - **Grid mode** — waveforms placed in specific cells of a rectilinear N-dimensional grid. Predictable, evenly-spaced morphing across each axis, and grids can be **sparse**: empty cells are silence, so you can place specific waveforms at specific positions without filling every cell. Per-axis cell-count steppers in the Wavetable View window go from 1 to 64 waveforms per axis; resizing an axis preserves every existing waveform's (i, j, k, …) coordinate. Grid mode is the cheaper-to-render of the two modes (the synth reads 2^N corners per sample regardless of axis size).
   - **Scatter mode** — waveforms placed at arbitrary positions in N-dimensional space and blended via a Wendland radial basis function. Useful when your morph axes don't fit a grid (three waveforms forming a triangle, clusters at non-uniform spacings, etc.). The **RBF radius** slider (in the Wavetable View window) controls how far each waveform's influence reaches — smaller for sharper transitions, larger for smoother blends.

The wavetable editor is a single resizable window split side-by-side: on the **left**, the **wavetable arrangement view** — a spatial visualization showing the waveforms as coloured dots (in Grid mode at cell centers labelled with their (i,j,k) coord, in Scatter mode at arbitrary positions labelled "Waveform N") plus a sidebar with the Library list, Cells list, axis controls, etc. On the **right**, the **per-waveform editor** for whichever library entry is currently focused — both halves are always visible. The synth's Position knob / Position pin / a Param cable drives a smooth blend of the nearby waveforms at audio time. To add a new waveform, press **+ Waveform** and pick a kind from the popup. To edit a waveform, click any cell that holds it (the dot in the arrangement view or its row in the Cells list), its row in the Library list, or its tab in the row above the editor — the right pane updates instantly, no extra window to open or close. The right pane adapts to the waveform's type: layered waveforms show the layer rows + "+ Layer" button; frequency-domain waveforms show the magnitude / phase curve editors; wavelet waveforms show the wavelet painter; and **granular** (captured) waveforms show the captured source as a waveform display, sliders for grain length / crossfade / embedded pitch, a freeze-mode picker, a **Play** button that auditions the waveform through the audio engine using its current freeze-mode / grain-length / crossfade settings (toggles to **Stop** while playing, and live-updates if you change Freeze mode mid-audition), and a **Re-capture from song...** button that swaps the source PCM in place (keeping the entry's name, colour, and current grain params, so re-capture doesn't pile up new library entries). In Scatter mode you can drag dots to reposition them and shift-click to delete.

The wavetable view also has:

- A **Library list** — every waveform that exists in this wavetable's library, whether or not it's currently placed in a cell. Each row shows a **colour swatch**, the waveform's name, and a "(used N×)" badge when placed. **Clicking a row makes that library entry the right-pane editor's target** — the row highlights, and every cell that references it gains an amber outer ring in the arrangement view so you can see exactly where the waveform you're editing is placed. The swatch on the left of each row opens a palette picker (Auto + 8 named colours); the chosen colour is also what every dot or cell referencing that waveform paints with, so colour is a per-waveform identity that the user can dial in (instead of being auto-derived from spectral content alone). An **X** removes the entry from the library (cells that referenced it become empty). The **+ Waveform** button adds a new waveform (Layered / Frequency-Domain / Wavelet / captured). The **Assign to selected cell** button places the highlighted library entry into the cell currently selected in the arrangement view (replacing whatever was there). This separation between library data and cell placements means deleting a cell no longer destroys the waveform — its library entry survives and is one click away from being placed somewhere else.
- A scrolling **Cells list** — every cell in the arrangement. In Grid mode this includes **empty cells** (shown dimmed with `— empty —` next to their coord) so you can select an empty slot and use **Assign to selected cell** to drop a library entry into it. In Scatter mode only the existing dots are listed (scatter points don't exist until placed). Picking a row selects that cell; if the cell holds a library entry the right-pane editor follows the selection too. The X next to each non-empty entry clears that cell (the library entry it referenced survives in the Library list).
- A **Convert** button — **Convert → Scatter** turns a Grid wavetable into a Scatter wavetable (every non-empty cell becomes a scatter dot at the cell center). **↩ Back to Grid** does the reverse, **but only while no dot has been moved off its original cell center** — drag any dot away and the button greys out, with a tooltip explaining why. This lets you start in Grid, convert when you need finer placement, and still revert as long as you haven't actually used the freedom yet.
- **Grid mode controls** — per-axis cell-count steppers (X, Y, Z, …, 1..64), plus **+ Axis** / **− Axis** to add or drop a dimension. Up to 8 axes.
- **Scatter mode controls** — the **RBF radius** slider, plus the same **+ Axis** / **− Axis** buttons (2..8 dims; adding an axis defaults every existing dot's new coordinate to the midpoint, removing one truncates each dot's position vector).
- A **View** dropdown for stereoscopic 3D rendering — **Flat 2D** (default), **3D Anaglyph** (one composite image with red/cyan eye channels; requires red/cyan paper glasses), **3D Cross-eyed stereoscope** (side-by-side pair viewed cross-eyed, so the left half lands on the right eye and vice versa — no hardware needed), or **3D Parallel stereoscope** (side-by-side pair viewed wall-eyed, how a Holmes stereoscope or a VR headset presents the pair).
- A sidebar of **rotation sliders, one per N-D plane** (1 for 2D, 3 for 3D, 6 for 4D, 10 for 5D, …). Mouse-drag in the view orbits the (axisX, axisZ) and (axisY, axisZ) planes; the sliders give direct access to every plane, including ones the current projection isn't showing, so in a 4D-or-higher wavetable you can rotate axes out of the projection plane and back in to see how the arrangement looks from any angle. A **Reset rotation** button at the bottom zeroes every plane back to the axis-aligned default. The wireframe in the view is a projection of the full N-dim hypercube (every corner of [0,1]^N, connected by every 1-bit-differing edge — capped at 6 dims for legibility), so the box always encloses every dot regardless of rotation, and it automatically fit-scales to fill the available view area. Un-rotated, dims past the projected two collapse on top of each other (mathematically correct — the projection has degenerate edges), so a 4D wavetable just looks like a 2D square until you rotate a higher plane to pull the extra dims apart.

The per-waveform editor (the right half of the window — always visible) shows the editing UI for the currently-selected library entry: layer rows for Layered, magnitude+phase curves for Frequency Domain, a DWT coefficient grid for Wavelet Space, and a Compare panel at the top for A/B'ing Direct vs Additive-bank render modes on the same cycle. Above the editor body sits a per-waveform identity row — a **colour swatch** (same palette picker as the Library list) and a **name field** — so renaming the waveform or changing its colour is one click away while you're already editing it. The same name and colour are what every Library row and arrangement-view placement of this waveform displays. A **waveform tabs** row above the body lists one tab per library entry (not per cell — a single entry placed in three cells is still one tab); click a tab to make that entry the editor's target. The other two ways to focus the editor are clicking a row in the **Library list** or clicking a non-empty cell (dot in the arrangement view / row in the Cells list), which both select the cell AND sync the editor target to whatever library entry that cell references. **Editor target vs cell selection are two separate states**: editing the right pane edits the data (library entry), and that edit shows up in every cell that references the entry; the Cells-list selection just controls where **Assign to selected cell** sends the entry next. An amber outer ring in the arrangement view marks every cell currently referencing the editor's target so the link between "what I'm editing" and "where it ends up" stays visible. If the library is empty the right pane shows a placeholder telling you to add an entry with **+ Waveform**.

### Waveterrain (Terrain Synth)

Terrain Synth is the underlying engine that powers most of SEANCE's built-in synths. It treats your sound source as an **N-dimensional terrain** of sample values, plus a **traversal** that walks through that terrain over time, reading values to produce audio.

A terrain can come from:

- A 1D layered waveform (the standard wavetable case — terrain is the cycle, traversal sweeps through it once per played pitch).
- A 1D audio file (sample player — terrain is the recording, traversal advances at note-pitch-relative speed).
- A 2D image (pixel brightness becomes amplitude).
- An N-D wavetable (each axis becomes a Position knob on the synth node).
- A math expression like `sin(x*y)` evaluated over a grid.
- Fractal value noise — sums several octaves of smoothstep-interpolated random fields into a 1/f-like bumpy terrain. Different from a noise oscillator: the orbit reveals smoothly-varying noisy texture as it moves, with the same path retracing the same waveform each cycle.

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

- **Wavetable / Layered Waveform synth.** Build single-cycle waveforms by stacking layers. Each layer is one of seven shapes (sine / saw / square / triangle / noise / hand-drawn / **formula**) at a chosen harmonic ratio, phase, and amplitude. Hand-drawn layers support two editing sub-modes: **Points** (Catmull-Rom control points) and **Freehand** (click-and-drag per-sample painting). Formula layers take a single math expression in `x` (radians over one cycle) - same vocabulary the frequency-domain editor uses, so anything from `tanh(3*sin(x))` to `sin(x) + 0.3*saw(3*x)` is one field away. Stack multiple cycles into a wavetable that morphs as you play. See the [Wavetable arrangement](#wavetable-arrangement) concept above for the full structure.
- **Frequency-domain (spectral) synth.** Author the sound directly as a magnitude curve and a phase curve over FFT bins; the synth IFFTs to a single-cycle waveform. Each curve can be defined as a **formula** in `f` (the bin index) or **drawn graphically** with the same Points / Freehand sub-modes as the waveform editor. The phase canvas sits above the magnitude canvas, both update a live time-domain preview as you edit. Old projects authored with the original three-line spectral dialog open in the new editor and round-trip losslessly.
- **Wavelet Space synth.** Author the sound directly in the wavelet domain by painting on a **scales × time grid** of DWT coefficients. The top row is the approximation band (overall shape), and each row below is a finer-resolution detail band — paint a cell at the toolbar Amp value (Shift-click for negative, right-click to erase) and the synth IDWTs the grid into a single-cycle waveform, updating a live preview as you edit. Pick the wavelet family (db1/Haar, db2, db4, sym4) to change the reconstruction shape from the same coefficient grid: db1 gives blocky/stepped tones, db4 is a balanced default, sym4 is smooth and symmetric. Useful for designing sounds whose character is more naturally expressed as "energy at this scale, at this time within the cycle" than as harmonic spectra.
- **MultiSampler.** Zone-based sampling instrument with N sample slots, each with configurable key range (low/high note), velocity range (low/high velocity), base note, fine-tune, gain, and pan. Multi-point linear envelopes for volume, pan, pitch, and filter with sustain/loop markers. Built-in state-variable filter (lowpass / highpass / bandpass) with envelope modulation. Replaces the old single-file Sampler — a one-zone MultiSampler covering 0-127 is exactly the old workflow, while multi-zone configs give you velocity-layered and key-split instruments. Legacy `__audio__:` projects auto-upgrade on load.
- **FM Synth.** 4-operator frequency modulation synthesis with 8 selectable algorithms, per-operator frequency ratios, levels, and ADSR envelopes, and operator-4 self-feedback. Produces electric pianos, bells, metallic basses, evolving pads, and everything in between.
- **Phase Distortion Synth.** Casio CZ-style synthesis: warps a sine wave's phase with a modulator function to produce subtractive-like sounds (sawtooth, square, pulse, resonant) without a traditional filter. "Depth" parameter + DCW envelope shape the harmonic content over time for filter-sweep-like timbral motion.
- **Particle Cloud Synth.** Granular texture instrument that generates many overlapping short waveform bursts ("particles") with per-grain frequency randomization, stereo scatter, and mini-envelopes. Produces ambient pads, evolving textures, and glitchy effects. Controls: density, frequency spread, grain size, per-grain attack/release, waveform shape (sine/saw/square/noise).
- **Drum synth.** Eight analog-style voice algorithms — **Kick** (pitched sine sweep), **Snare** (filtered noise), **Hi-Hat** (FM noise), **Clap**, **Tom**, **Cowbell**, **Rimshot**, **Cymbal** (with crash/ride/bell variants). Each voice gets its own MIDI note assignment via MIDI Learn and a four-knob synthesis row (pitch, decay, tone, level).
- **SoundFont (SF2 / SFZ).** Load multi-sample patches with full preset / bank navigation. Hundreds of free SF2 packs available online for orchestral, piano, drums, ethnic instruments, etc. SF2 support is complete; SFZ uses a built-in basic loader, with full SFZ-spec compliance via the sfizz library on the roadmap.
- **Terrain Synth.** The N-dimensional sample-array engine that powers the wavetable and sampler instruments — see the [Waveterrain](#waveterrain-terrain-synth) concept above. Can also be used directly with image (2D), audio file (1D), or math expression sources. Math-expression terrains are creatable at any dimensionality from 1D through 8D via the *N-D Terrain (custom expression)* menu item, with axis variables `x, y, z, w, v, u, s, t` corresponding to dimensions 1-8. The terrain visualizer's `+ Dim` / `- Dim` buttons also extend or contract dimensionality on an existing node, adding/removing Sig input pins and per-axis Center/Radius parameter knobs.

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

**Spatial / utility**

- **3D Spatializer** — HRTF binaural audio placement with X/Y/Z position parameters. Wire signal sources into the position pins to move the sound source around the listener's head in 3D in real time.
- **Signal Shape (LFO / Envelope)** — generates a control signal from a chosen waveform shape, at a chosen rate (free or beat-synced) with a chosen output range. Wire to any synth parameter for modulation. Single LFO node can drive many parameters at once.
- **XY Pad** — two-axis click-and-drag controller, mapped to any combination of node parameters. Great for live performance.
- **Spectrum Tap** — analyzes audio and outputs amplitude signals for configurable frequency bands. Wire those bands to other parameters for spectrum-following effects, vocoder-like routing, or visualizers.
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
- **Pitch bend, mod wheel vibrato, sustain pedal, channel aftertouch** all handled in the built-in synths.
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

- **Piano roll** with click-to-place, drag-to-move, edge-to-resize, box-select, copy / cut / paste, alt-for-no-snap. Velocity lane (drag bar heights), automation lanes (Catmull-Rom curves on any parameter). Horizontal and vertical zoom sliders at the bottom-right (or Ctrl+scroll / Ctrl+Shift+scroll on the grid) let you thicken or thin the note lanes and stretch or shrink the timeline independently. A thin grip strip at the top of each piano roll panel can be dragged up or down to resize that track's height — handy for seeing more octaves at once, and when several piano rolls are stacked, resizing one shifts the tracks above it up or down so each track keeps the size you set.
- **Music theory helpers** — Root / Key / Mode / Scale dropdowns highlight in-key pitches on the piano roll, with **Snap to Scale** mode and **Detect Key** auto-analysis.
- **Note transformations** — transpose by octaves or semitones, nudge in time, double or halve duration, reverse, fine-tune detune in cents.
- **Quantize** — snap notes toward the nearest grid position with a user-adjustable strength slider (1-100%). At 100% notes land exactly on the grid; lower values move notes only partway, keeping a natural feel. Available as a toolbar button and via the right-click context menu.
- **MIDI Note Degree System.** Each note stores its scale degree (1st-7th), octave, and chromatic offset, so you can change key/scale and have the melody automatically re-pitch to fit (Major to Minor, Ionian to Dorian, etc.).
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
- **Proper MPE controller support** — full MIDI Polyphonic Expression handling for pitch slide, pressure, and timbre per note from MPE controllers like the Seaboard, Linnstrument, Osmose.
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
2. Run the dependency setup script:

   ```
   cd cpp
   setup_dependencies.bat    (Windows)
   ```

3. Build:

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

For a deeper architectural overview — the data model, save/load lifecycle, undo strategy, and the rules for adding new features — see [`CLAUDE.md`](CLAUDE.md).

For the WASM scripting layer specifically, see [`cpp/scripts/wasm_examples/README.md`](cpp/scripts/wasm_examples/README.md).

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

1. **Layers within a frame.** Each waveform frame is built by summing layers (sine, saw, square, triangle, noise, or hand-drawn shapes), each at its own harmonic ratio, phase, and amplitude. Drawn layers have two sub-modes: **Points** (place control points; waveform is Catmull-Rom interpolated through them) and **Freehand** (click-and-drag to paint arbitrary per-sample waveform data). Stacking layers at integer harmonic ratios gives organ-like additive sounds; non-integer ratios give bell-like inharmonic textures.
2. **Frames within an N-dimensional arrangement.** Multiple frames stack into a wavetable that morphs as you change Position. The arrangement is N-dimensional — add an axis to get a second Position knob, add another for a third, etc. Most uses stay at 1D or 2D, but the option goes up to 8 dimensions.
3. **Two layout modes for the N-D arrangement:**
   - **Grid mode** — frames laid out on a regular N-dimensional grid. Predictable, evenly-spaced morphing across each axis.
   - **Scatter mode** — frames placed at arbitrary positions in N-dimensional space and blended via a Wendland radial basis function. Useful when your morph axes don't fit a grid (three frames forming a triangle, clusters at non-uniform spacings, etc.).

For 3D-and-higher scatter spaces, the editor includes a red/cyan anaglyph viewport for visualizing frame positions in stereoscopic depth.

### Waveterrain (Terrain Synth)

Terrain Synth is the underlying engine that powers most of SEANCE's built-in synths. It treats your sound source as an **N-dimensional terrain** of sample values, plus a **traversal** that walks through that terrain over time, reading values to produce audio.

A terrain can come from:

- A 1D layered waveform (the standard wavetable case — terrain is the cycle, traversal sweeps through it once per played pitch).
- A 1D audio file (sample player — terrain is the recording, traversal advances at note-pitch-relative speed).
- A 2D image (pixel brightness becomes amplitude).
- An N-D wavetable (each axis becomes a Position knob on the synth node).
- A math expression like `sin(x*y)` evaluated over a grid.

The terrain visualizer has **+ Dim / - Dim** buttons to add or remove dimension axes at runtime (up to 8D). Each dimension adds a signal input pin and Center/Radius parameter knobs on the synth node. When the terrain has more than 2 dimensions, a projection combo lets you choose which two axes to view in the heatmap.

Traversal modes:

- **Linear** — sweep a single axis at constant speed. The standard wavetable / sample-player behavior.
- **Orbit** — circle around a center point in 2D-or-higher. Produces evolving pad sounds where the traversal periodically revisits each region.
- **Lissajous** — figure-8 / lemniscate / other patterns formed by independent X and Y oscillators at different rates. Complex periodic timbres.
- **Path** — a user-drawn polyline through the terrain. Click points to define vertices, or freehand-draw a curve. Loop or bounce playback at the ends.

The same terrain with different traversals produces wildly different sounds. The same traversal with different terrains does too. Per-axis parameters of each can be modulated by signals, automation, or live MIDI controllers — turning a static N-D terrain into a fully alive, controllable instrument.

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

- **Wavetable / Layered Waveform synth.** Build single-cycle waveforms by stacking layers (sine / saw / square / triangle / noise / hand-drawn) at chosen harmonic ratios, phases, and amplitudes. Hand-drawn layers support two editing modes: **Points** (Catmull-Rom control points) and **Freehand** (click-and-drag per-sample painting). Stack multiple cycles into a wavetable that morphs as you play. See the [Wavetable arrangement](#wavetable-arrangement) concept above for the full structure.
- **MultiSampler.** Zone-based sampling instrument with N sample slots, each with configurable key range (low/high note), velocity range (low/high velocity), base note, fine-tune, gain, and pan. Multi-point linear envelopes for volume, pan, pitch, and filter with sustain/loop markers. Built-in state-variable filter (lowpass / highpass / bandpass) with envelope modulation. Replaces the old single-file Sampler — a one-zone MultiSampler covering 0-127 is exactly the old workflow, while multi-zone configs give you velocity-layered and key-split instruments. Legacy `__audio__:` projects auto-upgrade on load.
- **FM Synth.** 4-operator frequency modulation synthesis with 8 selectable algorithms, per-operator frequency ratios, levels, and ADSR envelopes, and operator-4 self-feedback. Produces electric pianos, bells, metallic basses, evolving pads, and everything in between.
- **Phase Distortion Synth.** Casio CZ-style synthesis: warps a sine wave's phase with a modulator function to produce subtractive-like sounds (sawtooth, square, pulse, resonant) without a traditional filter. "Depth" parameter + DCW envelope shape the harmonic content over time for filter-sweep-like timbral motion.
- **Particle Cloud Synth.** Granular texture instrument that generates many overlapping short waveform bursts ("particles") with per-grain frequency randomization, stereo scatter, and mini-envelopes. Produces ambient pads, evolving textures, and glitchy effects. Controls: density, frequency spread, grain size, per-grain attack/release, waveform shape (sine/saw/square/noise).
- **Drum synth.** Eight analog-style voice algorithms — **Kick** (pitched sine sweep), **Snare** (filtered noise), **Hi-Hat** (FM noise), **Clap**, **Tom**, **Cowbell**, **Rimshot**, **Cymbal** (with crash/ride/bell variants). Each voice gets its own MIDI note assignment via MIDI Learn and a four-knob synthesis row (pitch, decay, tone, level).
- **SoundFont (SF2 / SFZ).** Load multi-sample patches with full preset / bank navigation. Hundreds of free SF2 packs available online for orchestral, piano, drums, ethnic instruments, etc. SF2 support is complete; SFZ uses a built-in basic loader, with full SFZ-spec compliance via the sfizz library on the roadmap.
- **Terrain Synth.** The N-dimensional sample-array engine that powers the wavetable and sampler instruments — see the [Waveterrain](#waveterrain-terrain-synth) concept above. Can also be used directly with image, audio file, or math expression sources.

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

- **Piano roll** with click-to-place, drag-to-move, edge-to-resize, box-select, copy / cut / paste, alt-for-no-snap. Velocity lane (drag bar heights), automation lanes (Catmull-Rom curves on any parameter).
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
- **Drawing in wavelet space** — paint directly on a scales × time grid to design sounds.

### Synthesis

- **FM Synthesis instrument** — DX7-style multi-operator FM synth with selectable algorithms, per-operator frequency ratios, modulation indices, and envelopes. Produces electric pianos, bells, metallic basses, evolving pads. The most-requested synthesis type after subtractive and wavetable.
- **Phase Distortion Synthesis instrument** — Casio CZ-style PD: warps a carrier sine's phase with a modulator shape to produce subtractive-like sounds (brass, filtered pads) through a fundamentally different mechanism. Niche but differentiating — most DAWs don't offer it.
- **Spectral Modeling Synthesis (SMS)** — decomposes any sound into harmonic peaks (deterministic sinusoids) and noise (stochastic residual), then lets you manipulate each independently. Make a voice breathier without changing pitch, isolate transients, cross-synthesize tonal and noisy components from different sources. Builds on the existing FFT infrastructure.
- **Particle / granular cloud synth** — a dedicated texture instrument that generates many overlapping short waveform bursts ("particles"), each with its own mini-envelope, frequency, and harmonic parameter. Produces ambient pads, evolving textures, and glitchy effects. Based on the iMS-20 particle generator concept.
- **Spectral grain mode** — IFFT-to-windowed-grain spectral grain synthesis as a third Wavetable mode (Mode A is wavetable, B will be additive).
- **Additive bank mode** — per-partial sine oscillator bank for high-quality additive synthesis.
- **Inharmonic presets and ratio expression** — Bell, Drum, Stretched Piano starting points plus a custom `ratio(f)` expression for designing your own inharmonic series.
- **Spectral visualizer pane** — live spectrum + resulting grain waveform.
- **Compare buttons (A/B preview)** — A/B audition of the same spectrum through different render modes.

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

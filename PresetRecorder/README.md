# Plugin Preset Recorder

A companion tool to **SEANCE** (SoundShop2). It loads every plugin in SoundShop2's
plugin folders, records a short FLAC of **each preset** (or of the default sound
if the plugin has no presets), and lets you browse and listen to the results -
so you can hear what your plugins and their presets sound like without opening
them one by one.

- **Instruments** play a short built-in phrase: a held note, a quick arpeggio and a
  held chord (drum machines get a two-bar groove instead), or a MIDI file of your
  choice.
- **Effects** process the same four seconds of a public-domain big-band recording,
  so every effect is heard on identical material.
- Files are named **`<company> - <plugin> - <preset>.flac`** and sorted into an
  `Instruments` and an `Effects` folder.
- Plugins on **SoundShop2's skip list** are skipped automatically; a checkbox tree
  lets you unskip them all, all from one company, or any you pick.
- Every plugin is loaded in a **separate process**. A plugin that crashes, hangs or
  waits on an activation dialog costs that plugin (or that one preset), never the
  whole run.

It's a separate program from SEANCE - it never changes SoundShop2's files - that
lives in the SoundShop2 folder and reads SoundShop2's plugin settings.

## Building

Needs what SEANCE needs: Visual Studio 2022, CMake 3.22+, JUCE 8.0.12 at
`D:/JUCE-8.0.12` (or pass `-DJUCE_DIR=...`).

```
cd PresetRecorder
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\PresetRecorder_artefacts\Release\PresetRecorder.exe
```

Close PresetRecorder before rebuilding - a running copy (or one of its worker
processes) locks the exe and the link fails with LNK1104.

It has to stay inside the SoundShop2 folder: it compiles two of SEANCE's source
files (`cpp/src/plugin_settings.cpp`, `cpp/src/dialog_helpers.cpp`) so it reads
SoundShop2's plugin list exactly the way SoundShop2 does.

## Using it

### Record tab

**What to record, and where**

| Control | What it does |
|---|---|
| SoundShop2 plugin list | SoundShop2's `soundshop_plugins.cfg`: its plugin folders and its skip list. Found automatically - SoundShop2 writes it in the folder it was started from, usually the SoundShop2 folder (or next to `SEANCE.exe`); the newest one wins. **Browse...** picks another, **Find automatically** goes back, **Folders...** lists every folder that will be searched. |
| Save recordings in | Default: `Music\Plugin Preset Recordings`. |
| Effect input song | The song effects process. Bundled: *Small Note Boogaloo* by the U.S. Air Force Band's Airmen of Note (public domain). **Browse...** to use your own (WAV, AIFF, FLAC, Ogg, MP3), **Use bundled song** to go back, **Preview excerpt** to hear exactly what effects get. |
| Excerpt starts at / length | Which part of the song, in seconds. Default 207.55 s for 4 s: a dense full-band passage. |
| Instruments play | **Built-in phrase**, or **MIDI file** (up to 60 s; program changes in it are ignored). |

**How to record**

| Setting | Default | Meaning |
|---|---|---|
| Sample rate / Bit depth | 44100 Hz / 16-bit | Of the FLAC files (and of the plugins while recording). |
| Tempo | 130 BPM | Reported to plugins, so tempo-synced delays/LFOs/arpeggiators follow it; the phrase is played at it. 130 matches the bundled song. |
| Max tail | 3 s | After the input ends, recording continues until the sound dies away (below -70 dB) - but no longer than this. Anything still ringing is faded out. |
| Settle time | 250 ms | Pause after switching preset, for plugins that load a preset's samples in the background. Raise it if a recording starts with the previous preset's sound. |
| Presets per plugin | 0 (all) | Record at most this many presets of each plugin - handy for a quick first pass. |
| At a time | 2-4 | How many plugins are recorded in parallel (one process each). |
| Load timeout | 90 s | A plugin that takes longer to load is stopped and marked failed. Plugins waiting on an activation/registration dialog end up here. |
| Preset timeout | 60 s | A single preset taking longer is abandoned; the plugin carries on with its next preset. |
| Keep existing recordings | on | Presets that already have a file are skipped, so an interrupted run just continues when started again. |
| Presets built into the plugin | on | The plugin's own preset list (VST3 program list, LV2 presets). |
| .vstpreset files | on | `.vstpreset` files from the standard VST3 preset folders, matched to the plugin by its ID. |

**Buttons**

- **Scan plugins** - find the plugins in SoundShop2's folders. Only new or changed
  files are scanned (each in its own process); the rest come from a cache.
  **Rescan all** forgets the cache.
- **Record all** - scans first if needed, then records every plugin in the table
  except skip-listed ones. **Record selected** records only the plugins selected in
  the table (Ctrl/Shift-click). **Stop** stops; what's recorded is kept.

The table lists every plugin file SoundShop2's folders contain - one row per
plugin inside it - with its company, type, format, path on disk and live status.
Right-click a row to record just that plugin, jump to its recordings, show it in
Explorer or copy its path; double-click to jump to its recordings. The log at the
bottom says what happened to each plugin and preset (it's also saved to
`%APPDATA%\PresetRecorder\PresetRecorder.log`).

### Browse & Listen tab

Plugins on the left, the selected plugin's presets in the middle, details on the
right.

- **Click a preset to hear it.** The Up/Down arrow keys move through the presets and
  play each one; **Space** pauses/resumes; click or drag in the waveform to jump.
- The details show the plugin's **path on disk** (**Show plugin in Explorer**,
  **Copy plugin path**), the preset's origin, level and length, and any note
  (turned down to avoid clipping, tail cut, silent, why it failed...).
- **Dry input** plays the unprocessed excerpt every effect was fed, for comparison.
- **Search** filters by company, plugin or preset name; **Show** limits the list to
  instruments or effects. **Audio settings...** picks the output device.

### Skip List tab

SoundShop2 keeps a skip list (the `[Blocked]` section of `soundshop_plugins.cfg`):
plugins it won't load, added automatically when scanning one crashes or fails
(and by hand from its Plugin Settings). This tool skips them too, and lists them
here as a checkbox tree grouped by company:

- **Tick = record it anyway.** Tick the top row to unskip everything, a company row
  to unskip all of that company's entries, or single rows. A dash means "some".
- Or select rows (Ctrl/Shift-click) and use **Unskip selected** / **Skip selected**;
  **Space** toggles the selection. **Unskip all** / **Skip all** do the lot.
- Selecting an entry shows its path (**Show in Explorer**, **Copy path**), its
  company and where that name came from, whether it's 32-bit, and why it's probably
  on the list.

This only changes this tool's settings - SoundShop2's file is never touched, so
SoundShop2 keeps skipping those plugins. Changes apply from the next scan or
recording run.

## What you get

```
Plugin Preset Recordings\
  Instruments\u-he - Zebra2 - Program 0.flac
  Instruments\u-he - Zebra2 - Program 1.flac
  Effects\iZotope - Vinyl - Default.flac
  Effect input (dry).flac
  manifest.json
```

- One file per preset, `<company> - <plugin> - <preset>.flac`. Characters Windows
  doesn't allow in file names are replaced; two presets with the same name get
  " (2)"; the same plugin installed twice or in two formats gets " [VST3]"/" [LV2]".
- A plugin with no presets is recorded once, as "Default".
- Levels are as the plugin produced them, except that a take that would clip is
  turned down just enough (the manifest notes by how much). Plugin latency is
  removed, so every recording starts on the first note / first beat.
- `manifest.json` lists every plugin and preset with its source, status, length,
  peak level and the plugin's path; the Browse tab reads it.

## Good to know

- **Preset names come from the plugin.** Some plugins publish only numbered slots -
  u-he's VST3s list their 128 presets as "Program 0" to "Program 127" - and that's
  what the files are called.
- **Identical presets are flagged.** Plugins often fill unused preset slots with
  the same starting sound. Those are still recorded, but a preset that sounds
  exactly like an earlier one of the same plugin is noted "sounds identical to
  ..." in the Browse tab.
- **Presets in a plugin's own format** (u-he `.h2p`, Serum `.fxp`, Kontakt `.nki`,
  ...) can only be loaded from inside that plugin's own browser, so they can't be
  recorded; the plugin's own preset list (or its default sound) is recorded
  instead.
- **Copy-protected plugins** (iLok/PACE and similar) without a licence open an
  activation window from their worker process and are stopped when the load
  timeout runs out. Clicking "Try" (trial) in that window lets the recording go
  ahead.
- **32-bit plugins** can't be loaded by a 64-bit program - that's why SoundShop2
  put the 32-bit u-he installs on its skip list. Unskipping them just produces a
  scan failure that says so.
- **VST2 plugins** (`.dll`) aren't loaded, as in SoundShop2.
- An instrument that needs sample content that isn't installed records as
  **silent**; the Browse tab shows these greyed out.
- Nothing is lost by stopping: with *Keep existing recordings* on, the next run
  resumes where the last one stopped.

## Files it keeps

- `%APPDATA%\PresetRecorder\PresetRecorder.settings` - your settings, including the
  list of unskipped plugins
- `%APPDATA%\PresetRecorder\plugin_cache.xml` - scan results (delete it, or use
  Rescan all, to start fresh)
- `%APPDATA%\PresetRecorder\PresetRecorder.log` - the log
- `%TEMP%\PresetRecorder\` - per-run scratch files, cleaned up after a day

## The bundled song

`resources/Small Note Boogaloo - Airmen of Note.mp3` - composed and arranged by
Master Sgt. Ben Patterson, performed and recorded by the United States Air Force
Band's Airmen of Note. As a work of the U.S. federal government it is in the
public domain; the Air Force Band publishes it on its Public Domain Music page.
Downloaded from Wikimedia Commons; provenance, checksum and why this excerpt are
in `resources/Small Note Boogaloo - Airmen of Note.txt`. If the file goes missing,
the Record tab offers to download it again.

How it works inside: see `design.md`.

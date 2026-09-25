# PresetRecorder - design

A companion tool to SEANCE (SoundShop2): it records a short FLAC preview of every
preset of every plugin SoundShop2 can see, and lets you browse and audition the
recordings. It is a separate program with its own CMake project; it shares two
source files with SEANCE (see *Code shared with SEANCE*). User-facing
documentation is in `README.md`.

## Process model

```
PresetRecorder.exe (GUI)                        PresetRecorder.exe --worker job-N.json
  Controller ── JobRunner ──CreateProcess──►      Worker.cpp: load ONE plugin, scan or record it
     ▲              │                                  │
     │              └──polls job-N.log ◄───────────────┘ appends JSON-lines events
     └── Catalog, Manifest, Settings
```

The GUI never loads a plugin. Every scan and every recording runs in a worker
process (the same exe with `--worker <job file>`), one plugin per worker, a few
workers at a time (`At a time` setting). Plugins crash, hang, pop up
activation dialogs and leak; isolating them means any of that costs one plugin,
never the run. The pieces:

- **WorkerProcess** (`WorkerProcess.h/.cpp`) - on Windows, `CreateProcessW` with no
  inherited handles, `BELOW_NORMAL_PRIORITY_CLASS` (auditioning in the GUI stays
  glitch-free while plugins render flat out), and a job object with
  `KILL_ON_JOB_CLOSE | DIE_ON_UNHANDLED_EXCEPTION` (no orphaned workers if the GUI
  dies; no Windows Error Reporting dialog parking a crashed worker). Falls back to
  `juce::ChildProcess` elsewhere.
- **Event log** (`EventLogWriter/Reader` in `Util.h`) - the worker appends one JSON
  object per line through a raw `FILE_APPEND_DATA` handle shared for writing; the
  GUI re-reads new complete lines every 100 ms. Because the log is on disk,
  everything a worker said survives it crashing, and the crash handler
  (`Worker.cpp`) can append a last `crash` event through the same handle without
  touching the heap.
- **JobRunner** (`JobRunner.h/.cpp`) - queue + N running workers + watchdog:
  - no event for `Load timeout` (before `loaded`) or `Preset timeout` (after) →
    the worker is killed ("timed out");
  - a render worker that dies or is killed after loading its plugin is replaced:
    the plugin is re-queued at the *front* with `skipKeys` = every preset already
    finished, plus - if a `begin` had no `done` - the preset in progress, which is
    reported lost (`crashed`/`timeout`). A death between presets loses nothing.
    The plugin is given up on after `maxFruitlessAttempts` (3) deaths in a row
    that finished no preset - so a big bank with the odd crashing preset still
    gets through - or `maxAttempts` (100) deaths in total as a backstop;
  - the elapsed-time arithmetic is signed and wrap-safe (`millisecondsSince`) -
    an unsigned `now - lastActivity` with `now` read before pumping events once
    killed every worker instantly;
  - a worker is killed once (`killRequested`); `TerminateProcess` is asynchronous,
    so the runner waits at most 500 ms and then just re-checks on later ticks;
  - failures reported as `transient` (killed for no progress, or the worker
    couldn't be started) are never cached.
- **Controller** (`Controller.h/.cpp`) - everything on the message thread: owns
  Settings, SeanceConfig, Catalog, Manifest and JobRunner; turns Scan / Record /
  Stop into jobs; merges worker events into the manifest; broadcasts changes.
  Slow preparation (decoding the song excerpt, indexing `.vstpreset` files) runs on
  a detached thread and comes back via `callAsync`, guarded by a `WeakReference`
  and a run-generation counter so a Stop in the meantime wins.

## Inputs from SoundShop2

`SeanceConfig` reads SEANCE's own files, never writes them:

| File | Used for |
|---|---|
| `soundshop_plugins.cfg` `[ScanDirs]` | the plugin folders to search |
| `soundshop_plugins.cfg` `[Blocked]` | SEANCE's skip list |
| `soundshop_plugins_cache.dat` (JUCE XML part) | names/companies for skip-list entries |

SEANCE opens these relative to its working directory, so their location depends
on how SEANCE was started. `SeanceConfig::findCandidates()` looks in every
ancestor of the tool's exe (the tool lives inside the SoundShop2 folder, where
`seance.bat` starts SEANCE), in `cpp/build/SEANCE_artefacts/{Release,Debug}` of each
(double-click launches), and in the current directory; the newest file wins. The
user can pick a file explicitly (setting `seanceConfig`).

The search path per format comes from SEANCE's own functions in
`plugin_settings.cpp` (compiled in, see *Code shared with SEANCE*), the ones
SEANCE's *Scan Now* uses, so the two can't drift: `pluginSearchPath` =
`format->getDefaultLocationsToSearch()` followed by `userPluginFolders` - SEANCE's
folders minus those defaults, and for LV2 only the folders that contain an LV2
bundle (a subfolder with a `manifest.ttl`). lilv, JUCE's LV2 library, treats
*every* entry of a folder it is given as a bundle and prints three `failed to
open file .../manifest.ttl` errors to stderr for each one that isn't; handed
SEANCE's VST3 and VST2 folders, that was ~100 lines of console noise at every
enumeration (startup, each scan, each skip-list change). lilv can only find
plugins in subfolders with a `manifest.ttl`, so the filter changes nothing but
the noise. The formats are SEANCE's: VST3 + LV2 (+ AU on macOS). VST2 is off in
both (needs the discontinued VST2 SDK).

lilv has a second kind of noise: reading a folder it has already read logs a
`Reloading plugin <uri>` warning for every plugin in it - and a new LV2 format
has already read the default folders as it was created. So no LV2 world is ever
given a folder twice: `enumerate()` uses a brand-new `AudioPluginFormatManager`
each time and searches only `getUserFolders()` with its LV2 format (the search
returns every plugin the world has read, defaults included); workers get only
those folders too (`extraFolders` in the job).

## Catalog and scanning

`Catalog` (GUI side) holds one `PluginFile` per fileOrIdentifier - the unit
SEANCE's skip list works in - with state `pending / scanning / scanned /
scanFailed / skipped` and the `PluginDescription`s found inside. `enumerate()`
lists files (VST3: directory walk; LV2: lilv TTL parsing - neither runs plugin
code) plus every skip-list entry, and returns the files that need a scan.

Scan results are cached in `%APPDATA%\PresetRecorder\plugin_cache.xml`, keyed by
`identifierKey()` (normalised, lower-cased path on Windows) and the file's
modification time (for a bundle: the newest of the bundle folder, its binary and
its moduleinfo.json, since installers often replace the inner binary only).
A clean scan that finds nothing loadable (a 32-bit plugin) is cached too; scans that
crash, hang or can't run are not - a crash on unload or an unanswered activation
dialog isn't a verdict on the plugin, so those files are simply scanned again next
time. "Rescan all" clears the cache. The scan worker takes its own reference to the
plugin binary (`LoadLibraryW`) before JUCE's `findAllTypesForFile`, because JUCE
unloads the module (plugin exit function, DLL detach) before returning its results.

`getRecordablePlugins()` returns scanned, non-skipped plugin types sorted by
company/name and assigns each a unique **base name** `"<Company> - <Plugin>"`;
clashes (the same plugin as VST3 and LV2, or two installs) get `" [FORMAT]"`, then
`" (2)"`. Comparison is case-insensitive because Windows file names are. A `" - "`
inside a company or plugin name becomes `"-"`, so the first two `" - "` of any
file name are the separators and two plugins' files can never share a name
(`A - Verb` + preset `Plate - X` vs `A - Verb - Plate` + preset `X`).

## Skip list

A skip-list entry is skipped unless its key is in the tool's own `unskipped`
setting. SEANCE's file is never changed. The Skip List tab shows the entries as a
tristate checkbox tree (All → company → entry; tick = record anyway) with details
from `Catalog::describeSkipEntry`, which names an entry **without loading it**, in
this order: the tool's own scan of it, SEANCE's scan cache, the file itself
(`PluginFileInfo`: VST3 `moduleinfo.json` vendor/classes, the Windows version
resource via `GetFileVersionInfo`, a vendor-named parent folder), and finally
another install of the same plugin by name (the 32-bit `Podolski.vst3` is named
after the 64-bit `Podolski(x64).vst3`). `PluginFileInfo` also reads the PE header,
because the usual reason for being on the list is a 32-bit binary a 64-bit host
can't load.

## Recording one plugin (worker)

1. Build the input: instruments get a phrase (`PreviewInput`: held C3,
   C4-E4-G4-C5 arpeggio, C-major chord; drum-like plugins - category/name contains
   "drum", "percussion", "808"... - a two-bar GM groove on channel 10; or the
   user's MIDI file, program changes stripped, capped at 60 s). Effects get the
   song excerpt, prepared once per run by the GUI as a 32-bit float WAV at the
   render rate. An "effect" with no audio input that takes MIDI is played the
   phrase instead.
2. `createPluginInstance` from the worker thread (JUCE creates it on the message
   thread and waits). Everything that touches a plugin's controller -
   bus setup, `prepareToPlay`, preset changes, `reset`, deletion - is marshalled
   with `MessageManager::callSync`; only `processBlock` runs on the worker
   thread, like a host's audio thread; an exception thrown by plugin code on the
   message thread is carried back and rethrown on the worker thread.
   `setNonRealtime(true)` (offline bounce). Buses: stereo main out (and stereo
   main in for effects) if the plugin accepts it, otherwise its own layout. The
   instance and its playhead are never deleted (see step 6).
3. Presets, in order (`enumeratePresets`):
   - the plugin's program list (VST3 unit program list / program-change
     parameter; LV2 presets) when it has more than one program - keys
     `program:<i>`;
   - `.vstpreset` files for this plugin's class ID (next section) - keys
     `vstpreset:<path>`;
   - if neither, one `default` recording of the plugin as loaded (named after its
     single program if it has a meaningful name).
   Names come from the plugin; some (u-he) only publish "Program 0"... names.
4. Per preset (skipping `skipKeys`, and - with *Keep existing* - presets the job's
   `recorded` map says are recorded and whose file still exists): apply on the
   message thread (`setCurrentProgram` / `VST3Client::setPreset`) + `reset()`,
   wait `settleMs` wall-clock (async preset loaders), process `preRollSeconds` of
   silence starting with sustain-off, reset-all-controllers, centred pitch bend
   and all-notes/sound-off (a VST3 program change reaches the processor with the
   next block), then render, logging a `tick` every 2 s so a slow-but-progressing
   take isn't mistaken for a hang. An exception anywhere in a preset becomes a
   `fail` for that preset and the loop continues. The render: the playhead reports
   4/4 at `tempoBpm` and "playing"; after the input, keep going until 0.3 s below
   `silenceThresholdDb` or `maxTailSeconds`. Then: drop the plugin's latency from
   the front, trim trailing near-silence (never inside the input span), fade out
   20 ms if the tail was cut, turn the whole take down if it peaked over 0 dBFS
   (FLAC is fixed-point) and say so, replace NaN/inf with silence and say so, mark
   "silent" below -90 dBFS. A take whose 16-bit fingerprint matches an earlier take
   of the same worker attempt is noted "sounds identical to <preset>" (plugins
   often fill unused program slots with one init patch - u-he's Zebrify does).
   The reported `peakDb` is the plugin's own output peak, before any turn-down.
5. Write FLAC via `<name>.flac.partial` → rename (the GUI deletes stale
   `.partial` files at run start/stop), then emit `done`.
6. After `end`, `TerminateProcess` - no JUCE shutdown and no DLL unload code
   (`std::_Exit` would be `ExitProcess`, which still runs every DLL's
   `DLL_PROCESS_DETACH`). The plugin is never deleted, so a plugin that crashes or
   hangs on teardown can't turn finished work into a failure.

### `.vstpreset` files

`Vst3PresetIndex` (built by the GUI once per run, handed to workers as JSON) maps
the 32-hex-char class ID in each `.vstpreset` header to files, from the VST3 SDK's
standard folders (`Documents\VST3 Presets`, `%PROGRAMDATA%\VST3 Presets`,
`Common Files\VST3 Presets`; macOS/Linux equivalents) plus each VST3 bundle's
`Contents/Resources`. A worker learns its plugin's class ID from the header of the
plugin's own state saved via `VST3Client::getPreset()`, so the ID format matches by
construction; `setPreset` checks the ID again when loading. Display names are the
path below the plugin-named folder: `.../Plugin/Bass/Deep.vstpreset` → `Bass - Deep`.
Plugin-proprietary preset formats (.h2p, .fxp, .nki, ...) can't be loaded from
outside the plugin and are out of scope.

## Output

```
<output folder>/
  Instruments/<Company> - <Plugin> - <Preset>.flac
  Effects/<Company> - <Plugin> - <Preset>.flac
  Effect input (dry).flac        the exact excerpt every effect was fed
  manifest.json
```

Name parts go through `sanitiseFileNamePart` (Windows-illegal characters,
trailing dots/spaces, reserved device names; company ≤ 40 and plugin ≤ 60
characters, and the preset part shrinks so the whole name stays within ~150,
under Windows' 260-character path limit with a typical folder). File names are
**stable per preset key**: the GUI sends each render job a `recorded` map (key →
file, from the manifest, existing files only); `assignFileNames` gives those
presets their existing names and every other preset a name no recording of the
plugin uses (`" (2)"`...). So a changed preset list - a new `.vstpreset`, a
toggled source, a plugin update - can neither skip a preset because another one's
file has its name nor overwrite another preset's recording.

`manifest.json` - written only by the GUI (`Manifest`), read by the Browse tab:

```
{ tool, version, saved, dryInput,
  plugins: [ { id, name, company, format, version, category, path, kind,
               baseName, status, error, lastRun,
               presets: [ { key, name, source, file, status, note,
                            seconds, peakDb, recordedAt } ] } ] }
```

Plugin `status`: `ok | partial | failed | recording | not recorded`. Preset
`status`: `ok | silent | failed | crashed | timeout | pending | not recorded`.
`id` is `PluginDescription::createIdentifierString()`. A `presets` event from a
worker rebuilds the plugin's preset list in enumeration order, carrying earlier
results over by key; presets beyond the per-plugin limit stay listed as `not
recorded`.

## Worker protocol

Job file (JSON, written by `JobRunner::launch`):

| field | scan | render |
|---|---|---|
| `type` | `"scan"` | `"render"` |
| `eventLog`, `skipKeys` | added per attempt | added per attempt |
| `format`, `fileOrId`, `resultFile` (XML of descriptions) | ✓ | |
| `extraFolders` (SEANCE's folders beyond the defaults; lets LV2 resolve URIs from them) | ✓ | ✓ |
| `plugin` (PluginDescription XML), `pluginId`, `baseName`, `outputRoot`, `subfolder`, `instrument`, `settings` (RenderSettings), `effectInput`, `midiFile`, `presetIndex`, `recorded` (key → file) | | ✓ |

Events: `hello`, `loading`, `loaded {inputs, outputs, latency, classId, input}`,
`presets {list:[{key,name,source,file}], limit}`, `begin {key,index,count}`,
`done {key,file,status,seconds,peakDb,note}`, `skip {key,file,reason}`,
`fail {key,reason}`, `tick` (heartbeat during a take), `error {message}`,
`scanned {count}`, `crash {what,code}`, `end {code}`. Exit codes: `WorkerExitCode` in `Worker.h`.

## GUI

`MainComponent` (tabs) over one `Controller` and one `Player`:

- **Record** (`RecordTab`) - sources (SoundShop2 cfg, output folder, effect song +
  excerpt, instrument phrase / MIDI file), render settings, Scan / Rescan all /
  Record all / Record selected / Stop, progress (per worker), a sortable
  multi-select table of every plugin file/type with live status, and the log.
  Settings controls are disabled during a run and say so in their tooltips.
- **Browse & Listen** (`BrowseTab`) - manifest-driven plugin list → preset list;
  selecting a preset plays it (arrow keys audition), Space toggles, details show
  the plugin's path on disk with Show in Explorer / Copy path, and "Dry input"
  plays the unprocessed excerpt. Refreshes from a manifest signature on a timer
  rather than on every controller message.
- **Skip List** (`SkipListTab`) - the tristate tree described above.
- **Player** - `AudioDeviceManager` + `AudioTransportSource` (resamples to the
  device) + `AudioThumbnail`; device state persisted.

Dialogs follow SEANCE's taskbar rule (no second taskbar button): message boxes
via `SoundShop::showAlertAsync`, the audio-device dialog via
`SoundShop::launchAudioDeviceSettings`, file choosers parented to the window.
SEANCE's dialog lint (`cpp/cmake/check_dialog_patterns.cmake`) runs over this
tool's sources on every build.

## Settings and scratch files

- `%APPDATA%\PresetRecorder\PresetRecorder.settings` - JUCE PropertiesFile (XML):
  `seanceConfig`, `outputDir`, `inputSong`, `songStart`, `songLength`,
  `useMidiFile`, `midiFile`, `render` (RenderSettings JSON), `parallelWorkers`,
  `loadTimeout`, `presetTimeout`, `unskipped` (newline-separated keys), `volume`,
  `audioDevice`, `tab`, `windowState`.
- `%APPDATA%\PresetRecorder\plugin_cache.xml` - scan cache.
- `%APPDATA%\PresetRecorder\PresetRecorder.log` - the log (rotated at 4 MB).
- `%TEMP%\PresetRecorder\run-*` - per-session scratch: job files, event logs, scan
  results, and per recording run (suffixed with the run's generation number, so
  a stopped run's preparation thread can't overwrite a newer run's files) the
  float excerpt, the dry excerpt and the preset index. Folders over a day old are
  removed at startup.
- A named `InterProcessLock` per output folder is held for the length of a
  recording run, so two copies of the tool can't record into (or clean
  `.partial` files out of) the same folder.

## Code shared with SEANCE

Compiled from `../cpp/src`, never copied (see `CMakeLists.txt`):

- `plugin_settings.cpp/.h` (+ `plugin_host.h` for its include) - SEANCE's parser
  for `soundshop_plugins.cfg`, its default scan folders, and `pluginSearchPath` /
  `userPluginFolders` (where each format looks), so the format, the defaults and
  the search can't drift.
- `dialog_helpers.cpp/.h` - AppLookAndFeel and taskbar-correct dialogs.

SEANCE headers are included as `"cpp/src/..."` from the SoundShop2 root rather than
by putting `cpp/src` on the include path, so SEANCE's ~150 header names can't
shadow this tool's. Both shared headers carry a note that PresetRecorder compiles
them; keep them free of SEANCE-internal dependencies.

## Testing the failure paths

No installed plugin is guaranteed to crash or hang, so the worker has a fault
injection hook: start the GUI with the environment variable
`PRESETRECORDER_FAULT_TEST` set to `;`-separated `crash:<preset key>` /
`hang:<preset key>` entries (e.g. `crash:program:1;hang:program:2`). Workers
inherit it; on reaching that preset a worker dereferences null (exercising the
crash handler → `crash` event → JobRunner restart) or sleeps forever (exercising
the preset timeout). With a small *Preset timeout* and *Presets per plugin* = 3,
every plugin should end `partial` with presets `ok`, `crashed`, `timeout`.

Other checks worth repeating after changes: Stop mid-run (no worker processes
left, no `.partial` files, rows "stopped"); Record again with *Keep existing
recordings* (only missing presets recorded); an unskipped 32-bit skip-list entry
(scan fails with the 32-bit message); an iLok plugin without a licence (load
timeout, then the run continues).

## Known limitations

- Presets stored only in a plugin's own format (u-he .h2p, Serum .fxp, Kontakt
  .nki, ...) aren't reachable from a host; such plugins get their program list
  (u-he: 128 slots named "Program 0"...) or a single default recording.
- iLok/PACE-protected plugins without a licence show an activation dialog in their
  worker and time out (Load timeout), unless you choose "Try" in the dialog.
- VST2 isn't supported (as in SEANCE).
- Sample-based instruments whose content isn't installed record as `silent`.

# JUCE patches required by SEANCE

SEANCE builds against a JUCE install at `D:/JUCE-8.0.12` (see the top-level
build instructions). A small number of upstream JUCE bugs are fixed by patching
that install directly. **These patches live outside the JUCE tree, so they are
lost whenever JUCE is reinstalled or upgraded — they must be re-applied.**

Whenever you reinstall / upgrade JUCE (or set up a fresh dev machine), re-apply
every patch in this folder before building.

## Applying

From the JUCE root (the folder that contains `modules/`):

```
cd /d D:\JUCE-8.0.12
patch -p1 < "D:\visual studio projects\SoundShop2\patches\juce-wasapi-ieee-float-tag.diff"
```

GNU `patch` tolerates small line-number drift between JUCE point releases via
the surrounding context lines. If a hunk fails, open the `.diff`, find the
function it targets, and apply the change by hand — each patch header explains
exactly what it does and why.

## Patch inventory

| File | Target | What it fixes |
|------|--------|---------------|
| `juce-wasapi-ieee-float-tag.diff` | `modules/juce_audio_devices/native/juce_WASAPI_windows.cpp` (JUCE 8.0.12) | WASAPI capture misclassifies devices that report the **plain** `WAVE_FORMAT_IEEE_FLOAT` (0x0003) tag (e.g. Logitech C615 webcam mic) as integer PCM, decoding 32-bit float through the Int32 converter → square-wave-garbage mic. Teaches `isFloat` detection to accept the plain float tag. |

## Verifying a patch took

After re-applying and rebuilding, capture from a USB mic that previously
garbled (e.g. the C615). Clean audio = patch is in effect. If you need to prove
which branch fired, temporarily re-add an `fprintf(stderr, ...)` of
`format->Format.wFormatTag` and `isFloat` just before `updateFormat(isFloat)` —
SEANCE redirects stderr to `seance.log` next to the exe.

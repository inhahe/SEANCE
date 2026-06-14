#pragma once
#include <juce_core/juce_core.h>

namespace SoundShop {

// ============================================================================
// Terrain-synth self-test (--self-test <dir>)
// ----------------------------------------------------------------------------
// A headless, in-process test harness for the 1D / 2D / 3D terrain synths
// (audio file, image, video sources). SEANCE is a GUI-subsystem app with no
// reliable console, so the harness writes everything it does to disk:
//
//   <dir>/selftest_report.txt   - human-readable PASS/FAIL log
//   <dir>/*.wav                 - rendered audio you can listen to
//   <dir>/*.png, *.wav (inputs) - the synthetic test media it generated
//
// It runs three layers of checks:
//   1. EXACT terrain-data tests - fill a Terrain from a known synthetic
//      pattern (ramp audio / gradient image / gradient video grid) and assert
//      Terrain::sample() reads back the expected interpolated values, plus a
//      makeVideoTerrainScript/parseVideoTerrainScript round-trip.
//   2. RENDER tests - instantiate a real TerrainSynthProcessor standalone,
//      drive the "Sig X/Y/Z" coordinate pins with ramp signals (the same
//      "move the position with a signal" path the node graph uses), render a
//      held note, and assert the output tracks the terrain readout. WAVs are
//      exported so the result is also audible.
//   3. ffmpeg round-trip (skipped when ffmpeg is absent) - generate a test
//      video and decode it back through VideoDecoder::decodeGrid.
//
// Returns 0 if every check passed, 1 otherwise (so it can gate CI / scripts).
// ============================================================================
int runSelfTest(const juce::File& outDir);

} // namespace SoundShop

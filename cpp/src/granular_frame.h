#pragma once
#include "wavetable_frame.h"
#include <vector>
#include <string>
#include <memory>

namespace SoundShop {

// Maximum grain length (ms) offered by the granular frame editor's grain-length
// slider, and the reference "max freeze window" size. A captured source is
// always at least this long (the song capture grabs ~1 s, the editor caps the
// slider at min(this, captured duration)), so the freeze window - whose width
// is the grain length - can range up to its max entirely inside the stored
// buffer, leaving room to slide the window as a sub-selection of the capture.
inline constexpr int kGranularMaxGrainMs = 500;

// Configurable overlapping-grain-count bounds for the two grain-cloud freeze
// modes (AsyncGranular, PitchSyncGrains). The granular voice clamps grainCount
// to this range and normalises gain by 2/grainCount so the level is constant.
// The frame-editor and capture-dialog "Grains" controls use it as their range.
// 4 is the historical fixed count (kept the default for backward compat).
inline constexpr int kGranularMinGrains = 2;
inline constexpr int kGranularMaxGrains = 16;

// Power-of-two FFT sizes offered by the Spectral-freeze "FFT size" pickers (in
// the frame editor and the capture dialogs). The UI prepends an "Auto" entry
// that maps to fftSize == 0 (the voice auto-picks the largest size that fits
// the window, <= 2048). Shared so the two UIs stay in lockstep. Capped at 8192
// to match the voice's hard cap (granular_freeze.cpp SpectralFreeze case).
inline constexpr int kSpectralFftSizes[]   = {256, 512, 1024, 2048, 4096, 8192};
inline constexpr int kNumSpectralFftSizes  = 6;

// Picks which "sustain the marker spot" algorithm the granular layer runs
// while a note is held on this frame. None is strictly better than the
// others; they each have a unique sonic character:
//   CrossfadeLoop    - faithful tape loop of an L-sample window with a
//                      short Hann crossfade at the seam. Source pitch is
//                      baked in; the synth resamples the whole loop to
//                      track MIDI note (sampler-with-sustain-loop). Most
//                      "what does this spot literally sound like" of the
//                      four.
//   AsyncGranular    - many short grains at randomised start positions in
//                      a small range near the marker, decoupled from each
//                      other. Frozen blur / GRM-Freeze texture.
//   PitchSyncGrains  - grain hop locked to one detected pitch period, so
//                      the loop is a single cycle and the output is a
//                      true sustained tone at that pitch. Requires the
//                      embeddedPitchHz to be the real detected pitch.
//   SpectralFreeze   - FFT a window at the marker, keep magnitudes,
//                      regenerate phases per frame, IFFT. Ethereal pad
//                      sustain - the most decoupled from the source's
//                      time-domain identity.
// Stored as an int on disk for forward-compat with future variants. Wire
// values are stable: 0=CrossfadeLoop, 1=AsyncGranular, 2=PitchSyncGrains,
// 3=SpectralFreeze.
enum class GranularFreezeMode : int {
    CrossfadeLoop   = 0,
    AsyncGranular   = 1,
    PitchSyncGrains = 2,
    SpectralFreeze  = 3,
};

// Freeze-window WIDTH sentinels (windowLen / GranularFrame::windowLen). A
// non-negative windowLen is an explicit absolute width in samples; the two
// negative values below are "auto" markers resolved by resolveAutoWindowLen():
//
//   kWindowLegacyAuto (-1): the original "auto = exactly one grainLength wide"
//       window. EVERY frame saved before per-method auto existed carries -1, and
//       it MUST keep resolving to grainLen so those frames stay byte-for-byte
//       identical on reload. Do not repurpose this value.
//
//   kWindowAutoPerMethod (-2): "auto = autoWindowMultiplier(mode) x grainLength",
//       recomputed whenever the grain length or the freeze method changes. New
//       captures / freshly-created frames default to this so the grain-cloud
//       modes get several grain-periods of roam room out of the box (a 1x window
//       collapses AsyncGranular / PitchSyncGrains into a one-grain loop). It
//       only becomes an explicit (>=0) width once the user drags the band or
//       moves a capture window slider.
inline constexpr int kWindowLegacyAuto    = -1;
inline constexpr int kWindowAutoPerMethod = -2;

// Per-method auto window-width multiplier (x grainLength) used by the
// kWindowAutoPerMethod sentinel. The grain-cloud modes (AsyncGranular,
// PitchSyncGrains) need multiple grain-periods of roam room or they degenerate
// into a single repeating grain, so they default to 4x. CrossfadeLoop's window
// IS the loop body and SpectralFreeze's window IS one analysis frame, so both
// default to 1x (which matches the historical one-grain-wide behaviour). If you
// retune these the change is forward-only: it affects newly captured frames
// (which carry the -2 sentinel); frames already saved keep whatever width they
// resolved to, so old projects never shift.
inline int autoWindowMultiplier(GranularFreezeMode mode) {
    switch (mode) {
        case GranularFreezeMode::AsyncGranular:
        case GranularFreezeMode::PitchSyncGrains:
            return 4;
        case GranularFreezeMode::CrossfadeLoop:
        case GranularFreezeMode::SpectralFreeze:
        default:
            return 1;
    }
}

// Resolve a (possibly sentinel) windowLen into a freeze-window WIDTH in samples.
// Shared by the audio voice (granular_freeze) and the frame editor / capture
// preview so audition, held notes, and the on-screen band all use an identical
// width. The result is intentionally NOT floored or capped here - the voice
// applies its own clamps (cloud modes floor the width at grainLen; every mode
// caps at srcLen).
//   windowLen >= 0                    -> explicit width (returned as-is).
//   windowLen == kWindowAutoPerMethod -> autoWindowMultiplier(mode) * grainLen.
//   anything else (incl. kWindowLegacyAuto) -> grainLen (byte-for-byte legacy).
inline int resolveAutoWindowLen(int windowLen, GranularFreezeMode mode, int grainLen) {
    if (windowLen >= 0) return windowLen;
    if (windowLen == kWindowAutoPerMethod)
        return autoWindowMultiplier(mode) * grainLen;
    return grainLen;
}

// GranularFrame holds a multi-second window of mono PCM that the synth
// plays back via overlap-add granular synthesis (4-voice Hann-windowed
// OLA grains with jittered source-start positions) at the note's pitch.
// Unlike SampleFrame - which collapses its source to a single cycle and
// loops it at note frequency - a GranularFrame preserves the time-domain
// evolution of the source: while a note is held, the OLA stream picks
// jittered start positions inside the source so what you hear is a
// continuous textural drone derived from the source material, not a
// repeating cycle.
//
// Pitch model: each output sample reads from
//   src[srcStart[v] + envPos[v] * pitchRatio]
// where pitchRatio = noteHz / embeddedPitchHz. The grain envelope itself
// runs at the device sample rate, but the *source* read rate is scaled
// so the resulting timbre tracks MIDI pitch the same way a cycle frame
// does. embeddedPitchHz defaults to A4 (440 Hz) when the source's true
// pitch is unknown - so MIDI note 69 plays at 1:1, other notes get the
// usual wavetable-style pitch shift. YIN pitch detection is a planned
// follow-up that will set embeddedPitchHz to the real value at capture
// time.
//
// Morphing with cycle frames: the synth's terrain renderer treats this
// frame's contribution as a *parallel granular layer* on top of the
// (cycle-only) terrain. Grid / Scatter morph weights are computed for
// granular cells the same way they are for cycle cells; whatever
// fraction of the morph is "on" a granular frame plays through the OLA
// stream, the cycle-frame fraction plays through the normal terrain
// path. Sum = the final voice sample. This lets a wavetable hold a mix
// of cycle frames (Layered / Spectral / Wavelet / SampleFrame) and
// granular frames and morph smoothly between them.
//
// render(tableSize, out) fallback: extracts a tableSize-sample slice
// centered in the source, runs SampleFrame-style boundary cleaning
// (FFT round-trip kill-DC) and returns it. This cycle is what the
// wavetable editor draws as a thumbnail and what the synth's terrain
// would use if granular playback ever needs a fallback (e.g. when the
// source is shorter than one grain or the engine has granular disabled).
//
// Wire format used in __wavetable2__ frames is:
//   <srcLenSamples>;<sampleRate>;<grainLenSamples>;<embeddedPitchHz>;<freezeMode>;<xfadeSamples>;<s0,s1,...,s{N-1}>
// where srcLenSamples == N (number of source PCM samples). The sample
// rate is stored so that on load we know what the grain length means in
// time terms even if the project's device rate changed between saves.
// The container length-prefixes the body so the delimiters inside the
// sample list are safe.
//
// Backwards compatibility: optional header fields (freezeMode,
// xfadeSamples) sit between the embeddedPitchHz float and the
// comma-separated sample list. decodeBody recognises them as a
// contiguous run of pure-digit tokens separated by ';' that come BEFORE
// the first ',' (sample list). Each successive int extends the recognised
// header. Older saved frames that don't have them (no digit token before
// the comma) decode with freezeMode=CrossfadeLoop and
// xfadeSamples=480 (~10 ms, matching the original hardcoded value, so
// the recovered sound matches what the user saved). New writes always
// include both fields.
struct GranularFrame : public IWavetableFrame {
    // Source PCM, mono, at `sourceSampleRate`. Length = total samples.
    // Typically 0.1-2 seconds; the capture dialog produces 1 second by
    // default. Values in roughly [-1, 1]; no peak normalisation is
    // applied to the source (the granular voice's 4-overlap Hann sum
    // already normalises to unity gain).
    std::vector<float> source;

    // Sample rate of `source`. Stored so that on project reload at a
    // different device sample rate we still know the source's natural
    // pitch and can resample the granular reads correctly. 0 means
    // "unknown / not applicable", in which case the synth assumes the
    // source is at its current device rate.
    double sourceSampleRate = 0.0;

    // Length (in samples) of each grain envelope in the OLA stream.
    // The synth's 4-voice OLA uses voices at envelope-phase offsets of
    // grainLength/4; their Hann windows sum to constant amplitude 2.0.
    // Typical: 0.01-0.5 seconds. Independent of how long `source` is -
    // a 100 ms grain inside a 1 s source means the voices roam over the
    // source picking jittered start positions.
    int grainLength = 4800;  // ~100 ms @ 48 kHz

    // Start sample of the freeze WINDOW within `source`. The freeze window is
    // the sub-selection of the captured buffer that every freeze mode actually
    // operates on: CrossfadeLoop loops [windowStart, windowStart+windowLen),
    // the granular modes roam inside it, SpectralFreeze analyses inside it. The
    // editor draws it as a draggable/resizable band over the source thumbnail.
    //
    // -1 is the sentinel "auto-centre": the voice positions the window with the
    // historical centred-with-lookahead formula
    // max(0, (srcLen - (grainLength + grainLength/2)) / 2). All frames captured
    // or saved before this field existed decode as -1, so they sound exactly as
    // before. Fresh captures also leave it -1 (centred, matching the audition);
    // it only becomes an explicit offset once the user drags the band.
    int windowStart = -1;

    // Width (in samples) of the freeze window. Decoupled from grainLength: the
    // window is the region the freeze roams over, the grain is the size of each
    // overlapping Hann grain inside it. They are different scales - only
    // CrossfadeLoop wants them equal. A window WIDER than the grain is what
    // makes AsyncGranular and PitchSyncGrains diverge: the cloud needs multiple
    // grain-periods of roam room for pitch-period snapping to matter. The
    // window must satisfy grainLength <= windowLen <= srcLen.
    //
    // Two negative sentinels (see kWindowLegacyAuto / kWindowAutoPerMethod and
    // resolveAutoWindowLen above):
    //   kWindowLegacyAuto (-1): "auto = exactly grainLength" - the original
    //       one-grain-wide window. Every pre-field frame decodes as -1 and keeps
    //       sounding byte-for-byte the same.
    //   kWindowAutoPerMethod (-2): "auto = autoWindowMultiplier(mode) x
    //       grainLength" - the cloud modes get roam room, the loop/FFT modes
    //       stay 1x. This is the default for freshly created / captured frames,
    //       so a new Async/PitchSync capture no longer collapses into a loop.
    // The editor clamps windowLen up to at least grainLength whenever the grain
    // grows, and seeds a fresh explicit (>=0) width when the user resizes the
    // band. On disk the auto state rides a dedicated flag field so -2 survives a
    // round-trip without breaking the +1-biased width encoding (see encodeBody).
    int windowLen = kWindowAutoPerMethod;

    // Source pitch in Hz. The synth scales source reads by
    // noteHz / embeddedPitchHz so MIDI pitch tracks. Default A4 = 440 Hz
    // (set at construction); when YIN detection lands the capture
    // dialog will replace this with the detected fundamental.
    float embeddedPitchHz = 440.0f;

    // Freeze-mode for held notes. Default CrossfadeLoop is what the
    // dialog auditions by default and what most "captured a clip,
    // re-pitched it across the keyboard" use cases want. See enum docs
    // above for the full rundown.
    GranularFreezeMode freezeMode = GranularFreezeMode::CrossfadeLoop;

    // Crossfade-loop seam length, in samples at sourceSampleRate.
    // Only meaningful for freezeMode = CrossfadeLoop; ignored by the
    // other algorithms. The synth and audition engine clamp the value
    // at grainLength/2 at use time so a too-large value doesn't break
    // playback - it just saturates at the half-window cap.
    // Default ~50 ms at 48 kHz = a smooth seam blend for typical
    // musical content. Frames captured before this field existed
    // decode with the previous hard-coded ~10 ms (480-sample) default
    // so their audible character is preserved.
    int crossfadeSamples = 2400;  // ~50 ms @ 48 kHz

    // Number of overlapping Hann grains in the OLA cloud, for the two
    // grain-cloud freeze modes (AsyncGranular, PitchSyncGrains). More grains
    // = denser, smoother cloud (and a softer per-grain identity); fewer = a
    // sparser, more granular stutter. Ignored by CrossfadeLoop and
    // SpectralFreeze. The voice normalises gain by 2/grainCount (Hann COLA at
    // grainLength/grainCount hop), so the level stays constant as you change
    // it. Range [2, 16]; 4 is the historical hard-coded value (kAsync/kPS), so
    // frames saved before this field existed decode as 4 and sound identical.
    int grainCount = 4;

    // Requested FFT size (samples, a power of two) for SpectralFreeze. Larger
    // = finer frequency resolution (more bins = N/2+1) but a coarser time
    // window; must fit inside the freeze window. Ignored by the other three
    // modes. 0 == "auto": the voice picks the largest power of two that fits
    // the window, capped at 2048 (the historical behaviour), so frames saved
    // before this field existed decode as 0 and sound identical. A non-zero
    // value is clamped to the largest power of two <= min(value, window,
    // 8192) and floored at 256.
    int fftSize = 0;

    // Which capture source originally produced this frame's PCM:
    //   -1 = unknown / not applicable (frame built from scratch, duplicated,
    //        or loaded from a project saved before this field existed)
    //    0 = project song playback   (CaptureSource::Playback)
    //    1 = microphone / audio input (CaptureSource::Mic)
    //    2 = audio file               (CaptureSource::File)
    // Used by the granular frame editor so the "Re-capture from …" button
    // names the right source and re-opens the matching capture panel. A
    // mic-captured frame must not offer "Re-capture from song" (and vice
    // versa) - that re-captures from the wrong place entirely.
    int captureSourceKind = -1;

    GranularFrame() = default;
    GranularFrame(std::vector<float> src,
                  double srcRate,
                  int grainLen,
                  float pitchHz = 440.0f,
                  GranularFreezeMode mode = GranularFreezeMode::CrossfadeLoop,
                  int xfadeSamples = 2400)
        : source(std::move(src)),
          sourceSampleRate(srcRate),
          grainLength(grainLen),
          embeddedPitchHz(pitchHz),
          freezeMode(mode),
          crossfadeSamples(xfadeSamples) {}

    const char* typeId() const override { return "granular"; }
    void renderRaw(int tableSize, std::vector<float>& out) const override;
    std::string encodeBody() const override;
    bool decodeBody(const std::string& body) override;
    std::unique_ptr<IWavetableFrame> clone() const override;

    // Default empty frame with a 1-second sine source at A4. Returned
    // by the type-id registry on decode failure so an unknown body
    // doesn't leave a null slot the synth would misinterpret.
    static GranularFrame defaultEmpty();
};

} // namespace SoundShop

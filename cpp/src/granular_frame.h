#pragma once
#include "wavetable_frame.h"
#include <vector>
#include <string>
#include <memory>

namespace SoundShop {

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

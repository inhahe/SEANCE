#pragma once
#include "node_graph.h"
#include "transport.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>
#include <string>
#include <cmath>
#include <functional>
#include <array>

namespace SoundShop {

// ==============================================================================
// N-dimensional terrain of audio data
// ==============================================================================

class Terrain {
public:
    // Dimensions and sizes (e.g., {512, 512} for a 2D 512x512 terrain)
    void init(const std::vector<int>& dimensions);
    int numDimensions() const { return (int)dims.size(); }
    const std::vector<int>& getDimensions() const { return dims; }
    int totalSize() const { return (int)data.size(); }

    // Access: coordinate is a vector of floats in [0,1] per dimension
    // Interpolates between neighboring samples (N-linear interpolation)
    float sample(const std::vector<float>& coord) const;

    // Raw access by flat index
    float& at(int flatIndex) { return data[flatIndex]; }
    float at(int flatIndex) const { return data[flatIndex]; }

    // Fill methods
    void fillFromExpression(const std::string& expr);   // vars: x, y, z, w...
    void fillFromImage(const std::string& path);         // 2D, pixel brightness
    void fillFromAudioFile(const std::string& path);     // 1D, raw samples

    // Fractal / self-similar fill (#51): build a 1D waveform from recursive
    // wavelet coefficient patterns. The base pattern is a short seed
    // waveform; at each DWT level, the detail coefficients are scaled
    // copies of the seed. The 1/f-ish spectrum produces rich, organic
    // waveforms. `iterations` = number of self-similar recursion levels.
    void fillFractal(int size, int iterations = 5, float decay = 0.7f);

    // Wavelet-basis storage (#49): convert terrain data to/from DWT
    // coefficient representation. When stored as coefficients,
    // interpolation between frames happens in the wavelet domain
    // (smoother than time-domain averaging), and reconstruction is
    // a single IDWT per block. Call toWaveletBasis() after filling
    // the terrain to convert; fromWaveletBasis() to reconstruct.
    void toWaveletBasis(int levels = 4);
    void fromWaveletBasis(int levels = 4);
    bool isWaveletBasis = false;

    // Wavetable mipmap pyramid (#48): for anti-aliased pitch-up.
    // Each level is a half-resolution version of the previous, created
    // by DWT -> drop finest detail -> IDWT. Higher-pitched playback uses
    // smaller mipmaps to avoid aliasing. Call buildMipmaps() after
    // filling the terrain with a 1D wavetable. The playback code picks
    // the level based on the current pitch ratio.
    std::vector<std::vector<float>> mipmaps; // [0] = original, [1] = half, etc.
    void buildMipmaps(int maxLevels = 6);
    // Sample from the appropriate mipmap level for a given pitch ratio.
    // pitchRatio = playback_freq / base_freq. Higher ratio -> smaller mipmap.
    float sampleMipmap(float phase01, float pitchRatio) const;

    // Frequency-domain fill (1D only). Evaluates magExpr(f) and phaseExpr(f)
    // over FFT bins, inverse-FFTs to a real waveform, and fills this terrain
    // (which should be 1D of size == fftSize). Normalized to peak 1.0.
    // phaseMode: 0=expression 1=random 2=zero 3=linear
    void fillFromSpectralExpression(const std::string& magExpr,
                                    const std::string& phaseExpr,
                                    int fftSize,
                                    int phaseMode);

    // Frequency-domain fill from a SpectralDoc: each curve (mag, phase) is
    // evaluated as either an expression or a drawn curve, then combined
    // into a complex spectrum and inverse-FFTed. The doc carries its own
    // fftSize. Normalized to peak 1.0.
    void fillFromSpectralDoc(const struct SpectralDoc& doc);
    void fillConstant(float value);
    void fillNoise(unsigned int seed = 42);

    // Fractal value noise (N-D). Generates `octaves` coarse random grids of
    // doubling resolution, smoothstep-interpolates each into the terrain, and
    // sums with amplitude * persistence^octave. The result is a smooth,
    // 1/f-like bumpy field - the orbit reads it as varying noisy texture
    // instead of independent samples (which would just degenerate into a
    // noise oscillator). Persistence in (0,1): lower = smoother / fewer
    // high-frequency details; higher = harsher / more detail. Result is
    // peak-normalized to 1.0.
    void fillValueNoise(int octaves = 4, float persistence = 0.55f, unsigned int seed = 42);

    void smooth(int passes = 2);            // Gaussian blur to reduce quantization noise

    // Get raw data for display
    const std::vector<float>& getData() const { return data; }
    std::vector<float>& getData() { return data; }

    int coordToFlatIndex(const std::vector<int>& indices) const;

private:
    std::vector<int> dims;      // size of each dimension
    std::vector<float> data;    // flat array, row-major order
};

// ==============================================================================
// Traversal: maps time -> N-dimensional coordinate
// ==============================================================================

enum class TraversalMode {
    Linear,       // sweep along one axis
    Orbit,        // circle/ellipse on 2D plane (generalizes to N-D)
    Lissajous,    // sine-driven per axis with different frequencies
    Path,         // user-defined sequence of control points
    Physics,      // gravity wells, bouncing point
    Custom        // expression or code-defined
};

struct TraversalParams {
    TraversalMode mode = TraversalMode::Orbit;

    // Orbit params
    float centerX = 0.5f, centerY = 0.5f;  // center position [0,1]
    float radiusX = 0.3f, radiusY = 0.3f;  // ellipse radii
    float speed = 1.0f;                      // cycles per beat
    float radiusModSpeed = 0.0f;             // radius change over time
    float radiusModAmount = 0.0f;

    // Lissajous params (per dimension, up to 8)
    struct AxisParams {
        float frequency = 1.0f;   // relative frequency
        float phase = 0.0f;       // initial phase offset [0,1]
        float center = 0.5f;      // center position [0,1]
        float amplitude = 0.4f;   // swing [0,0.5]
    };
    std::array<AxisParams, 8> axes;

    // Linear params
    int linearAxis = 0;            // which dimension to sweep
    float linearSpeed = 1.0f;      // sweeps per beat

    // Physics params
    float friction = 0.01f;
    float gravity = 0.5f;
    struct Attractor { float x, y, strength; };
    std::vector<Attractor> attractors;

    // Path params
    struct PathPoint { float time; std::vector<float> coord; }; // time in beats
    std::vector<PathPoint> pathPoints;

    // Custom expression: returns coord per axis
    // Variables: t (time in beats), bpm, sr
    std::string customExpr;
};

class Traversal {
public:
    // Evaluate position at a given time (in beats)
    std::vector<float> evaluate(const TraversalParams& params, int numDims,
                                 double beatTime, double bpm, double sampleRate) const;

private:
    // Physics state (mutable for simulation stepping)
    mutable std::vector<float> physPos;
    mutable std::vector<float> physVel;
    mutable double lastPhysBeat = -1;
};

// ==============================================================================
// Terrain Synth mode
// ==============================================================================

// Three render modes for terrain synth:
//   Direct (SamplePerPoint) - read the terrain value as the audio sample.
//     Classic wavetable / sample playback. Natural for 1D wavetables and
//     1D audio files where the terrain "is" a waveform.
//   AmSine (WaveformPerPoint) - run a sine oscillator at the played pitch,
//     and use the terrain value at each sample to amplitude-modulate it.
//     Generic terrain sonification: the only mode that produces meaningful
//     sound for non-1D terrains (2D images, math expressions, fractal
//     noise) where Direct mode would just be noise.
//   AdditiveBank - FFT the 1D wavetable cycle to extract harmonic
//     magnitudes/phases, then synthesize per-note as a sum of independent
//     sine partials running at fundamental x ratio. Applies only to terrains
//     that are 1D wavetable cycles. Unlike Direct mode (where you hear the
//     baked cycle through one oscillator) each partial is a live sine, so
//     inharmonic stretches are aliased-free and per-partial modulation
//     becomes possible.
enum class TerrainSynthMode {
    SamplePerPoint,    // Direct: terrain value IS the audio sample
    WaveformPerPoint,  // AM-sine: terrain value AM-modulates a pitched sine
    AdditiveBank       // Additive: FFT the cycle, run a partial bank
};

// How many partials the additive-bank mode synthesises. Higher = brighter
// and more accurate to the wavetable, more CPU. 32 is a reasonable default
// (covers up to ~3.5kHz of harmonics for a low A note at 110Hz).
inline constexpr int kAdditiveBankMaxPartials = 64;

// Classification of what kind of source feeds a TerrainSynthProcessor.
// Different sources make different synthesis modes meaningful:
//   Wavetable - a single 1D cycle (or an N-D wavetable where the Position
//     knob picks a 1D slice every block). Direct (play the cycle) and
//     AdditiveBank (FFT the cycle, sum its partials) both apply. AM-sine
//     is degenerate here: a sine carrier at the played pitch, AM-modulated
//     by the cycle moving at the SAME played pitch, just distorts the
//     cycle into something less musical than the direct cycle itself.
//   Sample - a 1D audio recording (the Sampler / MultiSampler case). Only
//     Direct (sample playback at the transposed rate) makes musical sense.
//     The recording IS the sound, not a single cycle, so the FFT-cycle
//     assumption Additive bank makes doesn't hold; AM-sine reshapes the
//     recording into something rarely useful.
//   Surface - an N-D sonification source (2D image, math expr with y/z/w,
//     2D+ fractal noise). Direct mode reads raw pixel/voxel values into
//     the audio stream, producing noise. AM-sine is the only musical
//     mode here - the terrain becomes a slow amplitude envelope on a
//     pitched sine carrier.
enum class SynthSourceClass {
    Wavetable,
    Sample,
    Surface
};

struct SynthModeAvailability {
    bool direct       = false;
    bool amSine       = false;
    bool additiveBank = false;
    bool allows(TerrainSynthMode m) const {
        switch (m) {
            case TerrainSynthMode::SamplePerPoint:   return direct;
            case TerrainSynthMode::WaveformPerPoint: return amSine;
            case TerrainSynthMode::AdditiveBank:     return additiveBank;
        }
        return false;
    }
    // Snap a candidate mode to the nearest applicable one. Preference order
    // is the mode itself if allowed, then Direct, then AM-sine, then
    // AdditiveBank, so a project with a stale invalid mode always falls
    // back to a sensible default.
    TerrainSynthMode clamp(TerrainSynthMode m) const {
        if (allows(m)) return m;
        if (direct)       return TerrainSynthMode::SamplePerPoint;
        if (amSine)       return TerrainSynthMode::WaveformPerPoint;
        if (additiveBank) return TerrainSynthMode::AdditiveBank;
        return m;
    }
};

// Classify a TerrainSynthProcessor source by its script prefix. Mirrors
// the source-detection chain in TerrainSynthProcessor::reloadIfScriptChanged,
// kept as a free function so the node-graph UI can filter the Synth Mode
// picker without holding a pointer to the audio processor.
SynthSourceClass     classifySynthSource(const std::string& script);
SynthModeAvailability synthModeAvailabilityFor(SynthSourceClass cls);

// ==============================================================================
// Terrain Synth Processor
// ==============================================================================

class TerrainSynthProcessor : public juce::AudioProcessor {
public:
    TerrainSynthProcessor(Node& node, Transport& transport);

    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double sr, int bs) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;
    // Tail = ADSR release (param idx 3).  No magic constant - reads the
    // live param value so changing release shrinks/extends the tail.
    double getTailLengthSeconds() const override { return (double) getParam(3, 0.3f); }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    // Access for UI
    Terrain& getTerrain() { return terrain; }
    TraversalParams& getTraversalParams() { return traversalParams; }
    TerrainSynthMode getMode() const { return mode; }
    void setMode(TerrainSynthMode m) { mode = m; }

    // Get the current traversal position (for visualization)
    std::vector<float> getCurrentPosition() const { return lastPosition; }

    // Check if node.script changed since last processBlock and re-parse
    // the terrain data if so. This lets the layered editor commit changes
    // to node.script without triggering a full graph rebuild (#23).
    void reloadIfScriptChanged();

private:
    Node& node;
    Transport& transport;
    double sampleRate = 44100;

    // Cached copy of node.script from the last parse. When processBlock
    // detects that node.script differs, it triggers a re-parse.
    std::string cachedScript;

    Terrain terrain;
    Traversal traversal;
    TraversalParams traversalParams;
    TerrainSynthMode mode = TerrainSynthMode::SamplePerPoint;

    // Wavetable-synth mode: when true, terrain is a 2D buffer of stacked
    // frames {tableSize, nFrames}. coord[1] is the frame position and is
    // driven by the "Position" node param (index 21) rather than traversal.
    bool isWavetable = false;
    // Audio-sample playback: when true, the terrain holds a 1D audio
    // recording (loaded from __audio__:path). Used to classify the source
    // for SynthSourceClass::Sample so the Synth Mode picker only offers
    // Direct (sample playback) rather than AM-sine / AdditiveBank, which
    // don't make musical sense on a recording.
    bool isAudioSample = false;
    int  wtFrameCount = 0;
    int  wtNumDims = 0; // number of Position dimensions (1D, 2D, ...)

    // Scatter wavetable: instead of a rectilinear terrain, frames are stored
    // explicitly with their N-D positions. Each block we compute a Wendland
    // RBF blend at the current Position into a 1D terrain so the per-sample
    // path stays unchanged.
    bool wtScatter = false;
    int  wtScatterDims = 0;
    float wtScatterRadius = 0.45f;
    std::vector<std::vector<float>> wtScatterFrameSamples; // [frame][sample]
    std::vector<std::vector<float>> wtScatterFramePositions; // [frame][dim]

    // ---- Granular layer (parallel to the cycle layer) ----
    //
    // GranularFrame cells in the wavetable are NOT baked into the cycle
    // terrain (the cells store zeros there) - instead the synth runs a
    // per-voice 4-voice OLA granular stream for each granular frame and
    // mixes it in by the same morph weight the cycle layer would use.
    //
    // For grid mode: position is the frame's grid coord normalized to
    // [0,1] per dimension (so (gx / max(1, dims[d]-1))). The per-block
    // weight is the N-linear interp weight of the current Position into
    // this grid cell.
    //
    // For scatter mode: position is the frame's authored scatter position
    // (already in [0,1]). The per-block weight is the Wendland RBF weight
    // divided by the total RBF weight (same normalization the cycle
    // layer uses).
    struct GranularLayerEntry {
        // Source PCM, owned. Voice readers index into this directly via
        // the frame index; mid-session script reload of a __wavetable2__
        // synth would reset the table, but reloadIfScriptChanged only
        // handles __layered__ reloads, so for wavetable2 sources this
        // vector is stable for the processor's lifetime.
        std::vector<float> source;
        double             sourceSampleRate = 0.0;
        int                grainLength      = 4800;
        float              embeddedPitchHz  = 440.0f;
        // Freeze-mode for held notes. Mirrors GranularFrame::freezeMode
        // (0=CrossfadeLoop, 1=AsyncGranular, ...). The synth voice
        // branches on this so "what you audition = what you get": the
        // capture/freeze audition plays CrossfadeLoop, so the synth
        // must too. Stored as int to avoid pulling granular_frame.h's
        // enum into this header.
        int                freezeMode       = 0;   // 0 = CrossfadeLoop
        // Seam crossfade length in samples, used by CrossfadeLoop.
        // Clamped per-sample to [0, grainLength/2] by the reader.
        int                crossfadeSamples = 2400;
        // Position in the wavetable's N-D Position space, normalized
        // [0,1] per dim. For grid frames this is the grid-cell coord
        // mapped to [0,1]; for scatter frames it's the authored coord.
        std::vector<float> position;
    };
    std::vector<GranularLayerEntry> wtGranularFrames;

    // Per-block granular weights, sized to wtGranularFrames.size(). Filled
    // at the top of each processBlock from the current Position, then read
    // per-sample. Kept as a member so we don't realloc every block.
    std::vector<float> granWeights;

    // Recompute granWeights from the current Position params. Called once
    // per block.
    void updateGranularWeights();

    // Compute per-granular-frame morph weights for an arbitrary Position
    // vector into `out` (resized to wtGranularFrames.size()). updateGranular-
    // Weights() is just this called with the live Position params; audition
    // voices call it with their own per-voice override Position so a frame's
    // Play button in the wavetable editor auditions THAT frame regardless of
    // where the live Position knob sits.
    void computeGranularWeights(const std::vector<float>& pos,
                                std::vector<float>& out);

    // Allocate (stealing the quietest if all busy) and start a voice for a
    // note-on. Returns the voice index, or -1 if none could be allocated.
    // Shared by the MIDI note-on path and the UI audition drain so both go
    // through identical envelope / velocity / pitch-bend seeding. Clears any
    // stale audition override on the reused voice. velocity is 0..127.
    int  startVoice(int noteNumber, int channel, int velocity);
    // Release every active voice matching noteNumber on `channel` (honouring
    // the sustain pedal). Shared by the MIDI note-off path and the audition
    // drain's note-off.
    void releaseNote(int noteNumber, int channel);

    // Envelope curve evaluation cache
    struct EnvCurve {
        std::vector<float> table; // 256 samples, maps t(0..1) to amplitude(0..1)
        bool valid = false;
        void buildFromExpression(const std::string& expr);
        void buildFromPoints(const std::vector<std::pair<float, float>>& points);
        float evaluate(float t) const; // t in 0..1
    };
    EnvCurve attackCurve, decayCurve, releaseCurve;
    void rebuildEnvCurves();

    // Voices
    struct Voice {
        bool active = false;
        int noteNumber = -1;
        int midiChannel = 1;       // 1..16, used for per-channel bend / mod wheel
        float baseFrequency = 440.0f; // frequency before bend, set at note-on
        float frequency = 440.0f;  // effective frequency (base * bend), used by render
        float phase = 0.0f;
        double startBeat = 0;
        // Sustain pedal: true if this voice received a note-off while CC64
        // was down, so release is deferred until the pedal comes back up.
        bool sustainHeld = false;

        // AHDSR (Attack, Hold, Decay, Sustain, Release). The Hold stage
        // sits between Attack and Decay and stays at peak for
        // node.ahdsrEnvelope.holdMs - useful for percussive / brassy
        // patches where the note should "ring out" at full volume
        // before fading. holdMs = 0 (the default) skips Hold and
        // transitions Attack -> Decay directly.
        enum Stage { Off, Attack, Hold, Decay, Sustain, Release };
        Stage envStage = Off;
        float envLevel = 0.0f;
        double envTime = 0.0;
        // Velocity-scaled peak amplitude. Captured at note-on from
        // (1 - velSens) + velSens * (vel/127). The envelope shape goes
        // from 0 to envPeak across Attack instead of 0->1, then Decay
        // lands at envPeak * sustain. Velocity sensitivity comes from
        // node.ahdsrEnvelope.velocitySensitivity.
        float envPeak = 1.0f;

        // Per-note (polyphonic) aftertouch value 0..1. Set by
        // poly-pressure MIDI events; layered on top of the per-channel
        // aftertouch in the render loop. MPE-style controllers route
        // independent pressure per voice via this field.
        float polyAftertouch = 0.0f;

        // Additive-bank mode per-partial phases. Allocated lazily on the
        // first note-on that enters AdditiveBank mode so the Voice array
        // doesn't pay the ~256 byte / voice cost when this mode is unused.
        std::vector<float> partialPhases;

        // Per-granular-frame playback state. One entry per
        // GranularLayerEntry in TerrainSynthProcessor::wtGranularFrames.
        // Allocated lazily on note-on once the granular table is known.
        // The synth plays the CrossfadeLoop freeze mode (matching what the
        // capture/freeze dialog auditions), so the only state needed is a
        // fractional playhead. When per-mode synth implementations land
        // (AsyncGranular, ...) add their state here alongside loopPhase.
        struct GranStream {
            bool initialized = false;
            // Fractional playhead for the CrossfadeLoop freeze mode.
            // Advances by the pitch `ratio` each output sample and wraps
            // at grainLength, so a held note plays the marker window as a
            // faithful tape loop resampled to track MIDI pitch.
            float loopPhase = 0.0f;
        };
        std::vector<GranStream> granStreams;

        // Wavetable-editor audition override. When a Play button in the
        // wavetable editor triggers this voice, hasAuditionPos is set and
        // auditionPos holds the edited frame's normalized Position. The
        // granular layer then uses auditionWeights (computed per block from
        // auditionPos) instead of the synth-wide granWeights, so the voice
        // plays the frame being edited rather than whatever the live Position
        // knob selects. Plain MIDI / timeline notes leave this false and use
        // the shared granWeights. Reset on every voice (re)allocation.
        bool               hasAuditionPos = false;
        std::vector<float> auditionPos;
        std::vector<float> auditionWeights;

        float advanceEnv(float sr, float a, float h, float d, float s, float r,
                         const EnvCurve* aCurve, const EnvCurve* dCurve, const EnvCurve* rCurve);
    };
    static constexpr int MAX_VOICES = 16;
    Voice voices[MAX_VOICES];

    // Per-channel pitch bend factor (1.0 = no bend, 2^(semis/12) otherwise).
    // Default bend range is +/-2 semitones - configurable per-synth later.
    float pitchBendFactor[16] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
    static constexpr float kPitchBendRangeSemis = 2.0f;

    // Per-channel mod wheel (CC#1) value, normalized 0..1. Drives a default
    // fixed-rate vibrato on the voice frequency when > 0.
    float modWheel[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    float vibratoPhase = 0.0f;
    static constexpr float kVibratoRateHz = 6.0f;
    static constexpr float kVibratoMaxSemis = 0.4f; // +/-0.4 semis at full mod

    // Per-channel sustain pedal (CC#64) state. While true, note-offs are
    // captured as sustainHeld instead of immediately releasing.
    bool sustainPedal[16] = {false,false,false,false,false,false,false,false,
                              false,false,false,false,false,false,false,false};

    // Per-channel aftertouch (channel pressure, CC-like value 0..1). Set
    // by isChannelPressure() MIDI events; consumed in the render loop
    // as a volume swell multiplier scaled by node.aftertouchSensitivity.
    // The "Aftertouch" Signal input pin on the synth, when wired,
    // overrides this with the wired signal's current sample value
    // instead (lets users drive aftertouch from any signal source -
    // LFO, XY pad, automation, another voice's amplitude, etc.)
    float channelAftertouch[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    // Set per-block from the "Aftertouch" Signal input pin when it's
    // wired. Negative means "unwired - fall back to MIDI value".
    float aftertouchOverride = -1.0f;

    // Internal LFOs for modulation
    struct LFO {
        float frequency = 1.0f;  // Hz
        float phase = 0.0f;
        float advance(float sr) {
            float v = std::sin(phase * 2.0f * 3.14159265f);
            phase += frequency / sr;
            if (phase > 1.0f) phase -= 1.0f;
            return v;
        }
    };
    LFO lfo1, lfo2;

    // Graintable parameters
    float grainSize = 0.0f;       // in seconds, 0 = off (raw sample), >0 = crossfaded grains
    bool grainFreeze = false;      // freeze at current position
    float freezePosition = 0.0f;   // captured position when freeze was activated

    std::vector<float> lastPosition; // for UI visualization

    // Additive-bank cache: harmonic magnitudes and phases extracted from the
    // current 1D wavetable cycle (or the current scatter-blended cycle).
    // Computed once per block (or on cycle change) so the per-sample loop
    // is just N sine-table reads + an accumulate.
    struct PartialBank {
        std::vector<float> magnitude; // size = kAdditiveBankMaxPartials
        std::vector<float> phase;     // size = kAdditiveBankMaxPartials
        bool valid = false;
        // Fingerprint of the cycle this was computed from, so we don't
        // recompute on every block when the cycle is static.
        size_t cycleHash = 0;
    };
    PartialBank partialBank;
    // Refresh partialBank from the current 1D terrain cycle (or
    // wtScatterFrameSamples blended at the current Position when in
    // scatter mode).
    void refreshPartialBank();

    float getParam(int idx, float def) const;
};

} // namespace SoundShop

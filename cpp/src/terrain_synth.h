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

    // Frequency-domain fill (1D only). Evaluates magExpr(f) and phaseExpr(f)
    // over FFT bins, inverse-FFTs to a real waveform, and fills this terrain
    // (which should be 1D of size == fftSize). Normalized to peak 1.0.
    // phaseMode: 0=expression 1=random 2=zero 3=linear
    void fillFromSpectralExpression(const std::string& magExpr,
                                    const std::string& phaseExpr,
                                    int fftSize,
                                    int phaseMode);
    void fillConstant(float value);
    void fillNoise(unsigned int seed = 42);
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

enum class TerrainSynthMode {
    SamplePerPoint,    // terrain value IS the audio sample
    WaveformPerPoint   // terrain value selects from a wavetable bank
};

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
    double getTailLengthSeconds() const override { return 2.0; }
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

private:
    Node& node;
    Transport& transport;
    double sampleRate = 44100;

    Terrain terrain;
    Traversal traversal;
    TraversalParams traversalParams;
    TerrainSynthMode mode = TerrainSynthMode::SamplePerPoint;

    // Wavetable-synth mode: when true, terrain is a 2D buffer of stacked
    // frames {tableSize, nFrames}. coord[1] is the frame position and is
    // driven by the "Position" node param (index 21) rather than traversal.
    bool isWavetable = false;
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
        float velocity = 1.0f;
        float phase = 0.0f;
        double startBeat = 0;
        // Sustain pedal: true if this voice received a note-off while CC64
        // was down, so release is deferred until the pedal comes back up.
        bool sustainHeld = false;

        // ADSR
        enum Stage { Off, Attack, Decay, Sustain, Release };
        Stage envStage = Off;
        float envLevel = 0.0f;
        double envTime = 0.0;

        float advanceEnv(float sr, float a, float d, float s, float r,
                         const EnvCurve* aCurve, const EnvCurve* dCurve, const EnvCurve* rCurve);
    };
    static constexpr int MAX_VOICES = 16;
    Voice voices[MAX_VOICES];

    // Per-channel pitch bend factor (1.0 = no bend, 2^(semis/12) otherwise).
    // Default bend range is ±2 semitones — configurable per-synth later.
    float pitchBendFactor[16] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
    static constexpr float kPitchBendRangeSemis = 2.0f;

    // Per-channel mod wheel (CC#1) value, normalized 0..1. Drives a default
    // fixed-rate vibrato on the voice frequency when > 0.
    float modWheel[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    float vibratoPhase = 0.0f;
    static constexpr float kVibratoRateHz = 6.0f;
    static constexpr float kVibratoMaxSemis = 0.4f; // ±0.4 semis at full mod

    // Per-channel sustain pedal (CC#64) state. While true, note-offs are
    // captured as sustainHeld instead of immediately releasing.
    bool sustainPedal[16] = {false,false,false,false,false,false,false,false,
                              false,false,false,false,false,false,false,false};

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

    float getParam(int idx, float def) const;
};

} // namespace SoundShop

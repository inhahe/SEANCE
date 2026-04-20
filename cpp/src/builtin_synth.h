#pragma once
#include "node_graph.h"
#include "transport.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>
#include <array>
#include <string>
#include <cmath>

namespace SoundShop {

// Single-cycle waveform with mipmap levels for anti-aliased playback
class Wavetable {
public:
    static constexpr int NUM_MIPMAPS = 10;

    // Table size (samples per cycle). Default 2048, user-configurable.
    int getTableSize() const { return tableSize; }
    void setTableSize(int size);

    // Generate from a math expression like "sin(x) + 0.3*sin(3*x)"
    void generateFromExpression(const std::string& expr);

    // Frequency-domain mode A: evaluate mag(f) and phase(f) over FFT bins,
    // inverse-FFT to a single-period waveform. fftSize must be a power of two.
    // phaseMode: 0=expression, 1=random, 2=zero, 3=linear
    // Result is normalized to peak 1.0.
    void generateFromSpectralExpression(const std::string& magExpr,
                                        const std::string& phaseExpr,
                                        int fftSize,
                                        int phaseMode);

    // Generate from control points (cubic interpolation, enforced periodic)
    void generateFromPoints(const std::vector<std::pair<float, float>>& points);

    // Generate standard waveforms
    void generateSine();
    void generateSaw();
    void generateSquare();
    void generateTriangle();

    // Set the base table directly (e.g., from editor)
    void setTable(const std::vector<float>& samples);

    // Read a sample at a given phase (0-1) with mipmap level selection
    float sample(float phase, float frequencyHz, float sampleRate) const;

    // Get the base waveform for display
    const std::vector<float>& getBaseTable() const { return tables[0]; }

    // Rebuild mipmaps (call after modifying base table)
    void buildMipmaps();

    // Store the expression so we can regenerate after size change
    std::string lastExpression;

private:
    int tableSize = 2048;
    std::vector<float> tables[NUM_MIPMAPS];
    void initTables();
};

// Simple expression parser for waveform generation
// Supports: sin, cos, tan, abs, sqrt, pow, x, pi, numbers, +, -, *, /, (, )
// Also: saw(x), square(x), triangle(x), noise()
class WaveExprParser {
public:
    // Evaluate expression using `x` as free variable over [0, 2*pi).
    // Result size = tableSize. Clamped to [-1, 1].
    static std::vector<float> evaluate(const std::string& expr, int tableSize);

    // Evaluate expression using `f` as free variable over integers [0, numBins).
    // No clamping (used for spectrum magnitude/phase/ratio).
    static std::vector<float> evaluateOverBins(const std::string& expr, int numBins);

    // Evaluate a single value with both x and f bound (general helper).
    static float evaluateAt(const std::string& expr, float x, float f);
};

// Voice for polyphonic playback
struct SynthVoice {
    bool active = false;
    int noteNumber = -1;
    int midiChannel = 1;
    float frequency = 440.0f;
    float phase = 0.0f;
    float velocity = 1.0f;

    // ADSR state
    enum EnvStage { Off, Attack, Decay, Sustain, Release };
    EnvStage envStage = Off;
    float envLevel = 0.0f;
    double envTime = 0.0; // seconds in current stage

    float advance(float sampleRate, float attack, float decay, float sustain, float release);
};

// Built-in synth AudioProcessor
class BuiltinSynthProcessor : public juce::AudioProcessor {
public:
    BuiltinSynthProcessor(Node& node, Transport& transport);

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

    Wavetable& getWavetable() { return wavetable; }
    float getCurrentPhase() const { return lastPhase; }

private:
    float lastPhase = 0.0f; // for visualization
    Node& node;
    Transport& transport;
    double sampleRate = 44100;

    Wavetable wavetable;
    static constexpr int MAX_VOICES = 16;
    SynthVoice voices[MAX_VOICES];

    // Parameters read from node.params
    float getParam(int idx, float def) const;
};

} // namespace SoundShop

#pragma once
#include "node_graph.h"
#include "transport.h"
#include "signal_modulation.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>
#include <array>
#include <string>
#include <cmath>
#include <unordered_map>
#include <functional>

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

// Expression parser for waveform / LFO / envelope generation.
//
// Grammar precedence (low to high):
//   ternary (? :) -> || -> && -> < > <= >= == != -> + - -> * / -> ^ -> ! -unary
//   -> atom (number, identifier, function, parenthesized expression)
//
// Built-in vars:        x, f, pi, e
// Custom vars:          any identifier name, value supplied via `extraVars`
//                       map passed to evaluateWithVars()
// Built-in functions:   sin, cos, tan, abs, sqrt, exp, log, pow(a,b), tanh,
//                       saw, square, triangle, noise, floor, ceil,
//                       min(a,b), max(a,b), clamp(v,lo,hi), if(c,a,b),
//                       shape(pos)  (samples a caller-supplied table; see the
//                                    evaluateWithVars overload below — returns
//                                    0 when no sampler is supplied)
// Zero-arg keyword:     random  (uniform [-pi, pi])
//
// Comparison / boolean ops return 1.0f for true, 0.0f for false. They are
// NOT short-circuit (both sides evaluate) — same for the ternary's true and
// false branches and if()'s second/third arg. Unknown identifiers evaluate
// to 0 (silent fallback) rather than parse errors.
//
// ----------------------------------------------------------------------------
// Program mode (statements, persistent state, side effects)
// ----------------------------------------------------------------------------
//
// runProgram() runs a *multi-statement* program rather than a single
// expression. This powers the algorithmic MIDI node: the same grammar and
// built-in functions as the expression evaluator, plus:
//
//   * Statements separated by ';' or newlines. Each statement is either:
//       - an assignment  `name = expr`  (writes to the persistent stateVars
//         store, so the value survives across calls / audio blocks), or
//       - a bare expression evaluated for its side effects (the emit
//         functions below).
//   * MIDI emit functions that push events into a caller-supplied sink:
//       note(p,v,d) noteon(p,v) noteoff(p) cc(n,v) bend(v)
//     All take floats; the sink quantizes (7-bit CC, 14-bit bend) at emission.
//   * The reserved variable `out` selects which MIDI output the subsequent
//     emit calls route to (0-based; default 0). Set it with `out = 1`.
//
// The emit functions are no-ops (returning 0) when no sink is supplied, so a
// program can be parse-checked or run purely for its assignments. Likewise an
// assignment with no stateVars store is silently discarded.
struct IExprEmitSink {
    virtual ~IExprEmitSink() = default;
    // pitch/vel are 0..127 (rounded); durSec is the note length in seconds.
    virtual void emitNote   (int out, float pitch, float vel, float durSec) = 0;
    virtual void emitNoteOn (int out, float pitch, float vel) = 0;
    virtual void emitNoteOff(int out, float pitch) = 0;
    // value01 is 0..1 (scaled to 0..127). num is the CC number 0..127.
    virtual void emitCC     (int out, float num, float value01) = 0;
    // value is -1..1 (scaled to the 14-bit 0..16383 pitch-bend range).
    virtual void emitBend   (int out, float value) = 0;
    // PerBlock path: set the sample offset (within the current block) stamped on
    // subsequently-emitted events. No-op for per-sample sinks (offset is always
    // the host loop's current sample). Default no-op so existing sinks compile.
    virtual void setSampleOffset(int /*sampleOffset*/) {}
};

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

    // Evaluate with a caller-supplied variable bindings map. Use this for
    // expressions that reference dynamic variables (signal-input samples,
    // MIDI state like gate/freq/vel, the composite shape value `curve`,
    // etc.). The map name "x" / "f" override the default x=0 / f=0.
    // No clamping — caller decides.
    static float evaluateWithVars(const std::string& expr,
                                  const std::unordered_map<std::string, float>& vars);

    // Same as above, but also binds the `shape(pos)` function to `shapeFn`.
    // `pos` is whatever the expression passes; the sampler decides the domain
    // (SignalShape treats it as a 0..1 phase that wraps). When `shapeFn` is
    // empty (default-constructed), `shape(...)` evaluates to 0. Use this for
    // input-driven lookups into a drawn waveform (waveshaping), independent of
    // the phase-locked `curve` variable.
    static float evaluateWithVars(const std::string& expr,
                                  const std::unordered_map<std::string, float>& vars,
                                  const std::function<float(float)>& shapeFn,
                                  const std::function<float(int)>& noteToFreqFn = {});

    // Run a multi-statement program once (see the "Program mode" comment
    // above). `vars` are read-only per-call bindings (transport, inputs);
    // `stateVars` is the persistent read/write store assignments land in
    // (and where the reserved `out` selector lives). `sink` receives MIDI
    // emit calls (may be null). `shapeFn` backs shape(pos) (may be empty).
    // Returns the value of the last statement (0 for an empty program).
    static float runProgram(const std::string& program,
                            const std::unordered_map<std::string, float>& vars,
                            std::unordered_map<std::string, float>& stateVars,
                            IExprEmitSink* sink,
                            const std::function<float(float)>& shapeFn,
                            const std::function<float(int)>& noteToFreqFn = {});

    // Lightweight *structural* validator for the built-in expression/program
    // language. The evaluator itself is intentionally error-tolerant (unknown
    // identifiers read as 0, a missing close paren is silently ignored, etc.)
    // so it can never report a syntax error during real-time evaluation. This
    // static does a cheap, side-effect-free structural pass over the source so
    // the editor / node badge can surface obvious mistakes:
    //   * unbalanced parentheses  (extra ')' or unclosed '(')
    //   * an unterminated string literal ("softclip ...)
    //   * a character that is not part of the grammar at all (e.g. '%', '@',
    //     '#', '\', '~', '[' ... ), which would otherwise stall a per-sample
    //     program on a non-advancing token.
    // It deliberately does NOT flag unknown identifiers, preserving the
    // tolerant "unknown -> 0" design. Returns true (and leaves `error` empty)
    // when the source is structurally sound; false with a human-readable
    // message (including a 1-based line number) otherwise. Quote-aware: parens
    // and otherwise-illegal characters inside "..."/'...' literals are ignored.
    static bool validate(const std::string& program, std::string& error);
};

// Voice for polyphonic playback
struct SynthVoice {
    bool active = false;
    int note = -1;
    int mpeChannel = 1;
    float frequency = 440.0f;
    float phase = 0.0f;
    float velocity = 1.0f;

    // Per-voice expression (pitch bend, channel + poly key pressure, timbre).
    // distributeMpeMessages() in signal_modulation.h fills this from the MIDI
    // stream; the render loop reads mpe.pitchBend and effectivePressure(mpe).
    MpeVoiceState mpe;

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
    // Tail = the ADSR release time (param idx 3), the only source of
    // post-note-off audio in this synth.  No magic constant - reads the
    // live param value so changing release shrinks/extends the tail.
    double getTailLengthSeconds() const override { return (double) getParam(3, 0.3f); }
    // Transport panic (Stop): silence every voice at once so release tails are
    // cut immediately instead of fading out over the ADSR release time.
    void reset() override {
        for (auto& v : voices) { v.active = false; v.envStage = SynthVoice::Off; v.envLevel = 0.0f; }
    }
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

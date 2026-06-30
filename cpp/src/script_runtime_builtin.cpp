#include "script_runtime.h"
#include <juce_core/juce_core.h>

namespace SoundShop {

// -----------------------------------------------------------------------------
// BuiltinExprRuntime - the SEANCE mini-expression language (WaveExprParser).
// -----------------------------------------------------------------------------
//
// PerSample only. For the MIDI role the source is a multi-statement program that
// may be split into `init:` / `start:` / `loop:` sections (see splitSections);
// for the Signal role the source is a single composition expression evaluated
// each sample.
class BuiltinExprRuntime : public IScriptRuntime {
public:
    explicit BuiltinExprRuntime(ScriptRole r) : role(r) {}

    // Both Midi and Unified roles run a multi-statement program that may be
    // split into init:/start:/loop: sections; the Signal role is a single
    // composition expression.
    bool isProgramRole() const { return role == ScriptRole::Midi || role == ScriptRole::Unified; }

    bool load(const std::string& src, std::string& error) override {
        source = src;
        if (isProgramRole())
            splitSections();
        // Signal role: `source` IS the composition expression.
        //
        // The evaluator itself is intentionally tolerant (it can never throw a
        // syntax error mid-render), so a cheap structural pass here is what
        // surfaces obvious mistakes — unbalanced parens, an unterminated string
        // literal, an out-of-grammar character — to the editor strip / node
        // badge. It does NOT flag unknown identifiers (those legitimately read
        // as 0). On failure we still keep `source` loaded so evaluation degrades
        // gracefully exactly as before; we just report the problem.
        return WaveExprParser::validate(source, error);
    }

    void reset() override {
        stateVars.clear();
        if (isProgramRole())
            runInit();
    }

    void onStart(const ScriptVars& vars, IExprEmitSink* sink) override {
        if (!isProgramRole() || startProgram.empty()) return;
        WaveExprParser::runProgram(startProgram, vars, stateVars, sink, shape, noteToFreq);
    }

    float evalSignal(const ScriptVars& vars) override {
        return WaveExprParser::evaluateWithVars(source, vars, shape, noteToFreq);
    }

    void runMidi(const ScriptVars& vars, IExprEmitSink* sink) override {
        if (bodyProgram.empty()) return;
        WaveExprParser::runProgram(bodyProgram, vars, stateVars, sink, shape, noteToFreq);
    }

    void runUnified(const ScriptVars& vars, IExprEmitSink* sink,
                    float* outs, int outCount) override {
        // Run the body program: it emits MIDI through `sink` and assigns any
        // continuous outputs o1..oP into the persistent stateVars store. The
        // return value is the last bare expression (so a program that is just
        // "curve" yields curve as o1 without an explicit assignment).
        float ret = 0.0f;
        if (!bodyProgram.empty())
            ret = WaveExprParser::runProgram(bodyProgram, vars, stateVars, sink, shape, noteToFreq);
        for (int i = 0; i < outCount; ++i) {
            auto it = stateVars.find("o" + std::to_string(i + 1));
            if (it != stateVars.end())      outs[i] = it->second;   // explicit oN = ...
            else if (i == 0)                outs[i] = ret;          // o1 defaults to return value
            else                            outs[i] = 0.0f;
        }
    }

    bool supportsPerSample() const override { return true; }
    bool supportsPerBlock()  const override { return false; }

private:
    ScriptRole role;
    std::string source;

    // MIDI-role sections.
    std::string initProgram, startProgram, bodyProgram;
    ScriptVars  stateVars;

    // Split `source` into init / start / loop by header lines. A header is a
    // line whose only non-whitespace content (case-insensitive) is `init:`,
    // `start:` or `loop:`. Lines before the first header are loop (body) by
    // default, so a program with no headers behaves as a plain per-sample body.
    void splitSections() {
        initProgram.clear();
        startProgram.clear();
        bodyProgram.clear();

        auto trimmed = [](const std::string& s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) return std::string();
            size_t b = s.find_last_not_of(" \t\r\n");
            return s.substr(a, b - a + 1);
        };

        std::string* target = &bodyProgram;
        auto lines = juce::StringArray::fromLines(juce::String(source));
        for (int i = 0; i < lines.size(); ++i) {
            std::string raw = lines[i].toStdString();
            std::string low = juce::String(trimmed(raw)).toLowerCase().toStdString();
            if (low == "init:")  { target = &initProgram;  continue; }
            if (low == "start:") { target = &startProgram; continue; }
            if (low == "loop:")  { target = &bodyProgram;  continue; }
            *target += raw;
            *target += '\n';
        }
    }

    // Run the init section once with a null sink (emit calls are no-ops; init is
    // for seeding persistent state). Standard transport vars are not available
    // here (init runs at reload time), so init sees a minimal var set.
    void runInit() {
        if (!isProgramRole() || initProgram.empty()) return;
        ScriptVars vars; // empty: unknown identifiers read as 0
        WaveExprParser::runProgram(initProgram, vars, stateVars, nullptr, shape, noteToFreq);
    }
};

std::unique_ptr<IScriptRuntime> makeBuiltinRuntime(ScriptRole role, ScriptRate /*rate*/) {
    return std::make_unique<BuiltinExprRuntime>(role);
}

} // namespace SoundShop

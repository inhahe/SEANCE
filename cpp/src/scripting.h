#pragma once
#include "node_graph.h"
#include <string>
#include <functional>

namespace SoundShop {

class ScriptEngine {
public:
    ScriptEngine();
    ~ScriptEngine();

    // Process-wide singleton. The embedded CPython interpreter is a global
    // resource (one Py_Initialize / Py_Finalize per process), so all callers -
    // the script console, signal evaluation, and the static-shape baker - must
    // share one ScriptEngine. MainContentComponent's `scriptEngine` member is a
    // reference to this instance.
    static ScriptEngine& instance();

    // Initialize Python interpreter
    bool init();
    void shutdown();

    // Run a script with access to the project
    // Returns output text (print statements, errors)
    // activeNodeIdx: if >= 0, ss.this_node() returns this index
    std::string run(const std::string& code, NodeGraph& graph, int activeNodeIdx = -1);

    // Evaluate all bound signals at the given sample position
    // Returns list of (node_idx, param_idx, value) to apply
    struct SignalValue {
        int nodeIdx;
        int paramIdx;
        float value;
    };
    std::vector<SignalValue> evaluateSignals(int sample, int sampleRate, int blockSize);

    // Bake a user Python expression/body into N float samples, for static-shape
    // authoring (wavetable Formula layers, spectral curves). The shape variable
    // is always `x`:
    //   - domainRadians == true  (periodic, e.g. wavetable): x sweeps [0, 2*pi)
    //     over one cycle and outputs are clamped to [-1, 1].
    //   - domainRadians == false (non-periodic, e.g. spectral / ADSR curves):
    //     x sweeps [0, 1] and `f` is provided as an alias of x (so curves
    //     written in terms of either name work); no clamping.
    // The source may be a bare expression or a multi-line body that `return`s a
    // value.
    //
    // MUST be called on the UI/message thread (the interpreter is single,
    // GIL-held continuously). On any error returns false, fills `out` with N
    // zeros, and sets `error`. See shape_expr.cpp for the language dispatch.
    bool bakeShapeExpr(const std::string& src, bool domainRadians, int N,
                       std::vector<float>& out, std::string& error);

    bool isInitialized() const { return initialized; }

private:
    bool initialized = false;

    // Register the 'project' module that scripts can use
    void registerProjectModule(NodeGraph& graph);
};

} // namespace SoundShop

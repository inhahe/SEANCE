#pragma once
#include "node_graph.h"
#include <string>
#include <functional>

namespace SoundShop {

struct Transport;

class ScriptEngine {
public:
    ScriptEngine();
    ~ScriptEngine();

    // Register the app's live Transport so soundshop.render() bounces through
    // the real tempo / time-signature maps.
    //
    // Why this isn't just read off NodeGraph: the tempo map and time-signature
    // map live on MainContentComponent's Transport, not in the graph (the graph
    // carries only a scalar `bpm`). A render that ignored them would silently
    // disagree with File -> Export Audio on any project with a tempo ramp.
    //
    // MainContentComponent calls this once from its constructor. When nothing is
    // registered - the self-test, or any headless use - render() falls back to a
    // default Transport at graph.bpm, which is correct for a constant tempo.
    static void setHostTransport(Transport* t);
    static Transport* hostTransport();

    // Process-wide singleton. The embedded CPython interpreter is a global
    // resource (one Py_Initialize / Py_Finalize per process), so all callers -
    // the script console, signal evaluation, and the static-shape baker - must
    // share one ScriptEngine. MainContentComponent's `scriptEngine` member is a
    // reference to this instance.
    static ScriptEngine& instance();

    // Whether an embedded Python interpreter is usable in this run. True only
    // when SEANCE was built with Python (HAS_PYTHON) AND the Python DLL can be
    // loaded at runtime (it is delay-loaded, so a missing DLL just disables
    // scripting instead of blocking launch). Cheap and cached; safe to call
    // from the UI thread to grey out Python-only controls. This is the gate
    // every Python entry point checks before touching the C API, so calling a
    // Python function is never attempted when the DLL is absent.
    static bool pythonAvailable();

    // Initialize Python interpreter. Returns false (without touching the C API)
    // when pythonAvailable() is false, so callers degrade gracefully.
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

    // Bake an N-dimensional terrain grid from a user Python program (the Python
    // generation backend for Terrain Synth's "generate from program" feature).
    // `dims` is the grid shape (row-major, any rank); `out` is filled with
    // product(dims) cells in the terrain's BIPOLAR [-1,1] range, ready to bake
    // straight into the node (same final form as Terrain::fillFromScript).
    //
    //   - wholeGrid == false (per-cell): `src` is an expression or body that
    //     returns one value in [0,1]. It sees c0..c7 (the cell's normalized
    //     [0,1] coordinate per axis; axes past the rank read 0), x,y,z,w (c0..c3
    //     scaled to [0,2*pi]) and nd. Mirrors the Builtin/Lua per-cell vocabulary.
    //   - wholeGrid == true: `src` defines a function generate() that fills the
    //     grid via set(i,v) (clamped to [0,1]), get(i) and coord(i,axis), with
    //     globals dims, nd, total. Mirrors the Lua whole-grid API.
    //
    // The loop runs inside Python (one PyRun_String) rather than per-cell across
    // the C boundary, so it's fast. MUST be called on the UI/message thread (the
    // GIL-held interpreter is single). On error returns false, clears `out`, and
    // fills `error`. Because Python can't run on the audio thread (where the
    // processor is built), the caller bakes the result into the node tag.
    bool bakeTerrain(const std::string& src, bool wholeGrid,
                     const std::vector<int>& dims,
                     std::vector<float>& out, std::string& error);

    bool isInitialized() const { return initialized; }

private:
    bool initialized = false;

    // Register the 'project' module that scripts can use
    void registerProjectModule(NodeGraph& graph);
};

} // namespace SoundShop

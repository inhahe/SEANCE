#pragma once
#include "node_graph.h"
#include <string>
#include <functional>

namespace SoundShop {

class ScriptEngine {
public:
    ScriptEngine();
    ~ScriptEngine();

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

    bool isInitialized() const { return initialized; }

private:
    bool initialized = false;

    // Register the 'project' module that scripts can use
    void registerProjectModule(NodeGraph& graph);
};

} // namespace SoundShop

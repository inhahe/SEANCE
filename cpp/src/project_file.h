#pragma once
#include "node_graph.h"
#include "graph_processor.h"
#include <string>

namespace SoundShop {

class ProjectFile {
public:
    // Save the entire graph to a file
    // graphProcessor is optional — if provided, plugin states are saved
    static bool save(const std::string& path, NodeGraph& graph, GraphProcessor* gp = nullptr);

    // Load a graph from a file (replaces current graph contents)
    // pluginHost is optional — if provided, plugins are reloaded
    static bool load(const std::string& path, NodeGraph& graph, PluginHost* pluginHost = nullptr);

    // Current project path (empty = untitled)
    static std::string currentPath;
};

} // namespace SoundShop

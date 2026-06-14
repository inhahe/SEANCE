#pragma once
#include "node_graph.h"
#include "graph_processor.h"
#include <iosfwd>
#include <string>

namespace SoundShop {

class ProjectFile {
public:
    // Save the entire graph to a file
    // graphProcessor is optional - if provided, plugin states are saved
    static bool save(const std::string& path, NodeGraph& graph, GraphProcessor* gp = nullptr);

    // Load a graph from a file (replaces current graph contents)
    // pluginHost is optional - if provided, plugins are reloaded
    static bool load(const std::string& path, NodeGraph& graph, PluginHost* pluginHost = nullptr);

    // Stream-based variants. These do all the actual work; the path-based
    // entry points are thin wrappers that open a file. Used by the snapshot
    // undo system (#84) to (de)serialize graph state to/from in-memory text
    // without touching the filesystem.
    // includeView controls whether the node-graph component's pan/zoom
    // are written. True for file saves (so reopening restores the
    // view), false for undo snapshots (so undoing graph edits doesn't
    // also fling the user's viewport around). Defaults to true so all
    // existing call sites get the save-style behaviour.
    //
    // includeBlobs controls whether the content-addressed [Blob] store
    // (baked terrain grids etc.) is written. True for file saves (the bytes
    // must be on disk to reload), false for undo snapshots (the hash travels
    // in node.script; the bytes stay in the live in-memory store across
    // undo/redo, so duplicating them into every snapshot would bloat undo).
    static bool writeProject(std::ostream& out, NodeGraph& graph,
                             GraphProcessor* gp, bool includeView = true,
                             bool includeBlobs = true);
    static bool readProject(std::istream& in, NodeGraph& graph, PluginHost* pluginHost);

    // Convenience: serialize the graph to a string with NO plugin state.
    // This is the "fast" serializer used by commitSnapshot(). Excluding
    // plugin state keeps it cheap regardless of how many plugins are loaded -
    // plugin internal state is captured separately by the slow autosave path.
    static std::string serializeForUndo(NodeGraph& graph);

    // Parse a previously-serialized graph from a string. Used by the undo
    // system to revert to a snapshotted state, and by undo-tree persistence
    // restore.
    static bool loadFromString(const std::string& text, NodeGraph& graph,
                               PluginHost* pluginHost = nullptr);

    // Export the project's asset library (or a selected subset, expanded to its
    // dependency closure) to a standalone library-export file. The output is a
    // minimal project file carrying only [AssetStore] sections, so importing it
    // reuses the same readProject + merge path as importing a full session.
    // selectedIds empty = export every asset. Does NOT touch currentPath.
    static bool exportAssets(const std::string& path, const AssetLibrary& lib,
                             const std::vector<int>& selectedIds = {});

    // Current project path (empty = untitled)
    static std::string currentPath;
};

} // namespace SoundShop

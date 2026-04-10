#pragma once
#include "node_graph.h"
#include "transport.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <string>
#include <unordered_set>

namespace SoundShop {

class AudioCacheManager {
public:
    // Set the cache directory (typically next to the project file)
    void setCacheDir(const std::string& dir) { cacheDir = dir; }
    const std::string& getCacheDir() const { return cacheDir; }

    // Compute a hash for a node's inputs (params, clips, upstream hashes, plugin state)
    // Returns 0 if the node has live inputs and can't be cached
    uint64_t computeNodeHash(const Node& node, const NodeGraph& graph);

    // Check if a node is deterministic (no live inputs)
    bool isNodeDeterministic(const Node& node, const NodeGraph& graph,
                              std::unordered_set<int>& visited);

    // Update determinism flags for all nodes
    void updateDeterminism(NodeGraph& graph);

    // Check if cache is valid for a node; if hash changed, invalidate
    bool isCacheValid(Node& node, const NodeGraph& graph);

    // Save node's cached audio to disk
    bool saveToDisk(Node& node, double sampleRate);

    // Load node's cached audio from disk
    bool loadFromDisk(Node& node);

    // Clean up cache files that don't match any current node hash
    void cleanupStaleFiles(const NodeGraph& graph);

private:
    std::string cacheDir;

    // Simple hash combine
    static uint64_t hashCombine(uint64_t seed, uint64_t value) {
        return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    }
    static uint64_t hashFloat(float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        return bits;
    }
    static uint64_t hashString(const std::string& s) {
        uint64_t h = 0;
        for (char c : s) h = hashCombine(h, (uint64_t)c);
        return h;
    }

    // Find upstream node IDs (nodes whose output pins connect to this node's input pins)
    std::vector<int> getUpstreamNodeIds(const Node& node, const NodeGraph& graph);
};

} // namespace SoundShop

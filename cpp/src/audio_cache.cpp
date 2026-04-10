#include "audio_cache.h"
#include <fstream>
#include <cstdio>
#include <algorithm>
#include <filesystem>

namespace SoundShop {

// ==============================================================================
// Upstream detection
// ==============================================================================

std::vector<int> AudioCacheManager::getUpstreamNodeIds(const Node& node, const NodeGraph& graph) {
    std::vector<int> upstream;
    for (auto& link : graph.links) {
        for (auto& pin : node.pinsIn) {
            if (pin.id == link.endPin) {
                // Find which node owns the start pin
                for (auto& other : graph.nodes) {
                    for (auto& op : other.pinsOut) {
                        if (op.id == link.startPin)
                            upstream.push_back(other.id);
                    }
                }
            }
        }
    }
    return upstream;
}

// ==============================================================================
// Determinism check
// ==============================================================================

bool AudioCacheManager::isNodeDeterministic(const Node& node, const NodeGraph& graph,
                                             std::unordered_set<int>& visited) {
    if (visited.count(node.id)) return true; // break cycles
    visited.insert(node.id);

    // Nodes with live MIDI CC mappings are non-deterministic
    for (auto& m : graph.ccMappings)
        if (m.nodeId == node.id) return false;

    // Check upstream nodes recursively
    auto upIds = getUpstreamNodeIds(node, graph);
    for (int uid : upIds) {
        for (auto& other : graph.nodes) {
            if (other.id == uid) {
                if (!isNodeDeterministic(other, graph, visited))
                    return false;
            }
        }
    }

    return true;
}

void AudioCacheManager::updateDeterminism(NodeGraph& graph) {
    for (auto& node : graph.nodes) {
        std::unordered_set<int> visited;
        node.cache.deterministic = isNodeDeterministic(node, graph, visited);
    }
}

// ==============================================================================
// Hashing
// ==============================================================================

uint64_t AudioCacheManager::computeNodeHash(const Node& node, const NodeGraph& graph) {
    if (!node.cache.deterministic) return 0;

    uint64_t h = 0;

    // Hash node type and name
    h = hashCombine(h, (uint64_t)node.type);
    h = hashCombine(h, hashString(node.name));

    // Hash all parameter values and automation
    for (auto& p : node.params) {
        h = hashCombine(h, hashFloat(p.value));
        h = hashCombine(h, hashFloat(p.minVal));
        h = hashCombine(h, hashFloat(p.maxVal));
        for (auto& ap : p.automation.points) {
            h = hashCombine(h, hashFloat(ap.beat));
            h = hashCombine(h, hashFloat(ap.value));
        }
    }

    // Hash clip data (notes, CC events, audio file paths)
    for (auto& clip : node.clips) {
        h = hashCombine(h, hashFloat(clip.startBeat));
        h = hashCombine(h, hashFloat(clip.lengthBeats));
        h = hashCombine(h, hashString(clip.audioFilePath));
        h = hashCombine(h, hashFloat(clip.gainDb));
        h = hashCombine(h, hashFloat(clip.slipOffset));
        for (auto& n : clip.notes) {
            h = hashCombine(h, hashFloat(n.offset));
            h = hashCombine(h, (uint64_t)n.pitch);
            h = hashCombine(h, hashFloat(n.duration));
            h = hashCombine(h, hashFloat(n.detune));
            // Hash expression data
            for (auto& pt : n.expression.pitchBend)
                h = hashCombine(h, hashCombine(hashFloat(pt.time), hashFloat(pt.value)));
            for (auto& pt : n.expression.slide)
                h = hashCombine(h, hashCombine(hashFloat(pt.time), hashFloat(pt.value)));
            for (auto& pt : n.expression.pressure)
                h = hashCombine(h, hashCombine(hashFloat(pt.time), hashFloat(pt.value)));
        }
        for (auto& cc : clip.ccEvents) {
            h = hashCombine(h, hashFloat(cc.offset));
            h = hashCombine(h, (uint64_t)cc.controller);
            h = hashCombine(h, (uint64_t)cc.value);
        }
    }

    // Hash script
    h = hashCombine(h, hashString(node.script));

    // Hash plugin index and state
    h = hashCombine(h, (uint64_t)(node.pluginIndex + 1));
    if (!node.pendingPluginState.empty())
        h = hashCombine(h, hashString(node.pendingPluginState));

    // Hash performance mode settings
    h = hashCombine(h, (uint64_t)node.performanceMode);
    h = hashCombine(h, (uint64_t)node.mpeEnabled);

    // Hash upstream nodes' hashes (recursive dependency)
    auto upIds = getUpstreamNodeIds(node, graph);
    std::sort(upIds.begin(), upIds.end());
    for (int uid : upIds) {
        for (auto& other : graph.nodes) {
            if (other.id == uid) {
                uint64_t upHash = other.cache.inputHash;
                if (upHash == 0) upHash = computeNodeHash(other, graph);
                h = hashCombine(h, upHash);
            }
        }
    }

    return h;
}

bool AudioCacheManager::isCacheValid(Node& node, const NodeGraph& graph) {
    if (!node.cache.deterministic) return false;
    if (!node.cache.autoCache && !node.cache.enabled) return false;

    uint64_t newHash = computeNodeHash(node, graph);
    if (newHash == 0) return false; // non-deterministic

    if (node.cache.valid && node.cache.inputHash == newHash)
        return true;

    // Hash changed — invalidate
    node.cache.inputHash = newHash;
    node.cache.valid = false;
    return false;
}

// ==============================================================================
// Disk I/O
// ==============================================================================

bool AudioCacheManager::saveToDisk(Node& node, double sampleRate) {
    if (cacheDir.empty() || !node.cache.valid || node.cache.numSamples == 0) return false;

    // Create cache directory
    auto dir = juce::File(cacheDir);
    if (!dir.exists()) dir.createDirectory();

    // Filename from hash
    auto filename = juce::String::toHexString((int64_t)node.cache.inputHash) + ".cache";
    auto file = dir.getChildFile(filename);
    node.cache.diskPath = file.getFullPathName().toStdString();

    // Write raw float data (simple format: header + L + R)
    std::ofstream f(node.cache.diskPath, std::ios::binary);
    if (!f) return false;

    int64_t ns = node.cache.numSamples;
    double sr = node.cache.sampleRate;
    f.write((char*)&ns, sizeof(ns));
    f.write((char*)&sr, sizeof(sr));
    f.write((char*)node.cache.left.data(), ns * sizeof(float));
    f.write((char*)node.cache.right.data(), ns * sizeof(float));

    // Free memory since it's on disk now
    node.cache.left.clear();
    node.cache.left.shrink_to_fit();
    node.cache.right.clear();
    node.cache.right.shrink_to_fit();
    node.cache.useDisk = true;

    fprintf(stderr, "Cache saved to disk: %s (%lld samples)\n",
            filename.toRawUTF8(), (long long)ns);
    return true;
}

bool AudioCacheManager::loadFromDisk(Node& node) {
    if (node.cache.diskPath.empty()) return false;

    std::ifstream f(node.cache.diskPath, std::ios::binary);
    if (!f) return false;

    int64_t ns;
    double sr;
    f.read((char*)&ns, sizeof(ns));
    f.read((char*)&sr, sizeof(sr));

    if (ns <= 0 || ns > 500000000) return false; // sanity check (~3 hours at 48kHz)

    node.cache.left.resize(ns);
    node.cache.right.resize(ns);
    f.read((char*)node.cache.left.data(), ns * sizeof(float));
    f.read((char*)node.cache.right.data(), ns * sizeof(float));

    node.cache.numSamples = ns;
    node.cache.sampleRate = sr;
    node.cache.valid = true;

    fprintf(stderr, "Cache loaded from disk: %s (%lld samples)\n",
            node.cache.diskPath.c_str(), (long long)ns);
    return true;
}

void AudioCacheManager::cleanupStaleFiles(const NodeGraph& graph) {
    if (cacheDir.empty()) return;
    auto dir = juce::File(cacheDir);
    if (!dir.exists()) return;

    // Collect all valid hashes
    std::unordered_set<std::string> validFiles;
    for (auto& node : graph.nodes) {
        if (node.cache.inputHash != 0) {
            auto filename = juce::String::toHexString((int64_t)node.cache.inputHash) + ".cache";
            validFiles.insert(filename.toStdString());
        }
    }

    // Delete files not in the valid set
    for (auto& entry : juce::RangedDirectoryIterator(dir, false, "*.cache")) {
        auto name = entry.getFile().getFileName().toStdString();
        if (!validFiles.count(name)) {
            entry.getFile().deleteFile();
            fprintf(stderr, "Cleaned up stale cache: %s\n", name.c_str());
        }
    }
}

} // namespace SoundShop

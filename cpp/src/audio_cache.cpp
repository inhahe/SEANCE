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

    // Track nesting shifts every clip on this node by the cascading parent
    // offset, so two otherwise-identical nodes at different nesting offsets
    // render differently and must not share a cache entry.
    h = hashCombine(h, hashFloat(node.absoluteBeatOffset));

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

    // Hash script.
    //
    // THREAD SAFETY: this runs on the AUDIO thread (computeNodeHash ->
    // isCacheValid -> GraphProcessor::rebuildGraph, which executes inside the
    // audio callback). Meanwhile UI-thread editors rewrite node.script in place
    // when the user edits a waveform - e.g. dragging the freeze band in the
    // layered-wave / granular editor fires onLayerChanged -> commitToNode ->
    // setNodeScriptSynced on every drag tick. A std::string assignment is not
    // atomic, so without synchronisation this hash read can observe the new
    // size paired with a stale/freed data pointer and copy from garbage - for a
    // multi-megabyte granular wavetable script that reliably crashes mid-copy
    // (SEANCE.exe.17316.dmp: access violation in hashString during a band
    // drag). setNodeScriptSynced holds node.auditionMutex around the write, so
    // we take the same per-node mutex around the read to pair with it. This is
    // the cache-hash sibling of the reloadIfScriptChanged fix in known-issues.md
    // (that reader was synced; this one had been missed).
    {
        std::lock_guard<std::mutex> scriptLock(*node.auditionMutex);
        h = hashCombine(h, hashString(node.script));
    }

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

    // Hash changed - invalidate
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

    // Filename keyed by node id. Node ids are unique within a project and the
    // cache dir is per-project (<projectDir>/soundshop_cache), so this is
    // collision-free and stable across edits - unlike the old hash-based name,
    // which collided whenever two frozen nodes shared inputHash==0 (a manual
    // freeze never populates inputHash) and thus clobbered each other's file.
    auto filename = "node_" + juce::String(node.id) + ".cache";
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

    // Keep the cache file for every node that still has a live cache - either in
    // memory (freshly rendered) or lazily on disk (restored from a reloaded
    // project, memory not yet paged in). Keyed by node id to match saveToDisk.
    std::unordered_set<std::string> validFiles;
    for (auto& node : graph.nodes) {
        if (node.cache.useDisk || (node.cache.valid && node.cache.numSamples > 0)) {
            auto filename = "node_" + juce::String(node.id) + ".cache";
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

#pragma once
#include "node_graph.h"
#include <string>

namespace SoundShop {

// Import a MOD/S3M/IT/XM file into the node graph.
// Creates one MIDI track per channel, one Sampler instrument per sample,
// and converts all tracker effects to automation/expression/notes.
class ModImporter {
public:
    struct ImportResult {
        bool success = false;
        std::string error;
        int numChannels = 0;
        int numPatterns = 0;
        int numSamples = 0;
        int numNotes = 0;
    };

    // Import a tracker module file into the graph
    static ImportResult import(const std::string& path, NodeGraph& graph, float posX = 50, float posY = 50);

    // Supported extensions
    static bool isSupported(const std::string& path);
};

} // namespace SoundShop

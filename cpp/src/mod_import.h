#pragma once
#include "node_graph.h"
#include <string>
#include <vector>

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
        int numSamples = 0;       // total samples in the module
        int numSamplesExtracted = 0; // how many were rendered to WAV
        int numTracks = 0;        // MIDI tracks created (one per channel × sample)
        int numNotes = 0;
        std::string sampleDir;    // directory that received the WAVs (may be empty)
        int groupId = -1;         // id of the container Group node (for view framing)
        std::vector<int> nodeIds; // all nodes created by the import (group + children)
    };

    // Import a tracker module file into the graph
    static ImportResult import(const std::string& path, NodeGraph& graph, float posX = 50, float posY = 50);

    // Supported extensions
    static bool isSupported(const std::string& path);
};

} // namespace SoundShop

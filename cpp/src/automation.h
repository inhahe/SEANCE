#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>
#include <mutex>
#include <unordered_map>

namespace SoundShop {

// A parameter automation value to apply
struct AutomationValue {
    int nodeId;      // our node ID
    int paramIdx;    // parameter index on the plugin
    float value;     // normalized value 0-1
};

// MIDI CC to parameter mapping
struct CCMapping {
    int midiChannel;    // 1-16
    int ccNumber;       // 0-127
    int nodeId;         // target node
    int paramIdx;       // target parameter
    float minValue;     // mapped range min (0-1)
    float maxValue;     // mapped range max (0-1)
};

class AutomationManager {
public:
    // Apply automation values to plugins in the graph
    void applyValues(const std::vector<AutomationValue>& values,
                     juce::AudioProcessorGraph& graph,
                     const std::unordered_map<int, juce::AudioProcessorGraph::NodeID>& nodeMap);

    // Process incoming MIDI CC and apply mapped parameters
    void processMidiCC(const juce::MidiBuffer& midi,
                       juce::AudioProcessorGraph& graph,
                       const std::unordered_map<int, juce::AudioProcessorGraph::NodeID>& nodeMap);

    // CC mappings — thread-safe access
    void addCCMapping(const CCMapping& mapping);
    void removeCCMapping(int midiChannel, int ccNumber);
    void clearCCMappings();
    std::vector<CCMapping> getCCMappings() const;

    // Store latest signal values for UI display
    void setLatestValues(const std::vector<AutomationValue>& values);
    std::vector<AutomationValue> getLatestValues() const;

private:
    mutable std::mutex ccMutex;
    std::vector<CCMapping> ccMappings;

    mutable std::mutex valueMutex;
    std::vector<AutomationValue> latestValues;
};

} // namespace SoundShop

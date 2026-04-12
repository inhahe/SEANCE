#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <functional>

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

    // Per-plugin dirty marking hook (#86). Called by applyValues whenever
    // a parameter is pushed into a plugin, so the app can mark that
    // plugin's cached state stale. The slow autosave path then knows to
    // re-query getStateInformation on the next save. Set once at startup
    // by the application; if unset, dirty marking is skipped.
    //
    // Only fires from applyValues (message-thread automation push), NOT
    // from processMidiCC (audio thread — graph lookups would be unsafe
    // there). The autosave path's periodic "force-dirty all" pass catches
    // changes that processMidiCC made between automation passes.
    std::function<void(int nodeId)> onPluginParamChanged;

private:
    mutable std::mutex ccMutex;
    std::vector<CCMapping> ccMappings;

    mutable std::mutex valueMutex;
    std::vector<AutomationValue> latestValues;
};

} // namespace SoundShop

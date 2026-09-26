#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <functional>

namespace SoundShop {

class PluginCopies;

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
    // Apply automation values to plugins in the graph - or, for a node that
    // isn't in it, a plugin inside a Voice container: its master and every
    // voice's copy (`voices`, the live graph's pool; may be null).
    void applyValues(const std::vector<AutomationValue>& values,
                     juce::AudioProcessorGraph& graph,
                     const std::unordered_map<int, juce::AudioProcessorGraph::NodeID>& nodeMap,
                     PluginCopies* voices = nullptr);

    // Process incoming MIDI CC and apply mapped parameters, reaching a plugin
    // inside a Voice container as applyValues does. Audio thread, holding the
    // graph lock. `changed`, if given, gets (node id, parameter) for each
    // parameter a CC gave a different value - it's the caller's to clear.
    void processMidiCC(const juce::MidiBuffer& midi,
                       juce::AudioProcessorGraph& graph,
                       const std::unordered_map<int, juce::AudioProcessorGraph::NodeID>& nodeMap,
                       PluginCopies* voices = nullptr,
                       std::vector<std::pair<int, int>>* changed = nullptr);

    // CC mappings - thread-safe access
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
    // from processMidiCC (audio thread): that reports what it changed
    // instead, which the audio engine passes to the UI timer
    // (AudioEngine::drainLearnedCcChanges).
    std::function<void(int nodeId)> onPluginParamChanged;

private:
    mutable std::mutex ccMutex;
    std::vector<CCMapping> ccMappings;

    mutable std::mutex valueMutex;
    std::vector<AutomationValue> latestValues;
};

} // namespace SoundShop

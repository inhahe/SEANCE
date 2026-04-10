#include "automation.h"
#include <cstdio>

namespace SoundShop {

void AutomationManager::applyValues(const std::vector<AutomationValue>& values,
                                     juce::AudioProcessorGraph& graph,
                                     const std::unordered_map<int, juce::AudioProcessorGraph::NodeID>& nodeMap) {
    for (auto& av : values) {
        auto it = nodeMap.find(av.nodeId);
        if (it == nodeMap.end()) continue;

        auto graphNode = graph.getNodeForId(it->second);
        if (!graphNode || !graphNode->getProcessor()) continue;

        auto* proc = graphNode->getProcessor();
        auto& params = proc->getParameters();
        if (av.paramIdx >= 0 && av.paramIdx < (int)params.size()) {
            params[av.paramIdx]->setValue(av.value);
        }
    }
}

void AutomationManager::processMidiCC(const juce::MidiBuffer& midi,
                                        juce::AudioProcessorGraph& graph,
                                        const std::unordered_map<int, juce::AudioProcessorGraph::NodeID>& nodeMap) {
    std::lock_guard<std::mutex> lock(ccMutex);

    for (auto metadata : midi) {
        auto msg = metadata.getMessage();
        if (!msg.isController()) continue;

        int ch = msg.getChannel();
        int cc = msg.getControllerNumber();
        int val = msg.getControllerValue();

        for (auto& mapping : ccMappings) {
            if (mapping.midiChannel == ch && mapping.ccNumber == cc) {
                // Map 0-127 to minValue-maxValue
                float normalized = mapping.minValue +
                    (mapping.maxValue - mapping.minValue) * (val / 127.0f);

                auto it = nodeMap.find(mapping.nodeId);
                if (it == nodeMap.end()) continue;

                auto graphNode = graph.getNodeForId(it->second);
                if (!graphNode || !graphNode->getProcessor()) continue;

                auto* proc = graphNode->getProcessor();
                auto& params = proc->getParameters();
                if (mapping.paramIdx >= 0 && mapping.paramIdx < (int)params.size()) {
                    params[mapping.paramIdx]->setValue(normalized);
                }
            }
        }
    }
}

void AutomationManager::addCCMapping(const CCMapping& mapping) {
    std::lock_guard<std::mutex> lock(ccMutex);
    ccMappings.push_back(mapping);
}

void AutomationManager::removeCCMapping(int midiChannel, int ccNumber) {
    std::lock_guard<std::mutex> lock(ccMutex);
    ccMappings.erase(
        std::remove_if(ccMappings.begin(), ccMappings.end(),
            [&](auto& m) { return m.midiChannel == midiChannel && m.ccNumber == ccNumber; }),
        ccMappings.end());
}

void AutomationManager::clearCCMappings() {
    std::lock_guard<std::mutex> lock(ccMutex);
    ccMappings.clear();
}

std::vector<CCMapping> AutomationManager::getCCMappings() const {
    std::lock_guard<std::mutex> lock(ccMutex);
    return ccMappings;
}

void AutomationManager::setLatestValues(const std::vector<AutomationValue>& values) {
    std::lock_guard<std::mutex> lock(valueMutex);
    latestValues = values;
}

std::vector<AutomationValue> AutomationManager::getLatestValues() const {
    std::lock_guard<std::mutex> lock(valueMutex);
    return latestValues;
}

} // namespace SoundShop

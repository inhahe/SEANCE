#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>

namespace SoundShop {

struct PluginInfo {
    std::string name;
    std::string manufacturer;
    std::string format;       // "VST3", "AU", etc.
    std::string fileOrId;     // file path or unique ID
    std::string category;
    std::string version;
    std::string description;
    std::string uniqueId;
    bool isInstrument = false;
    bool hasAudioInput = false;
    bool hasAudioOutput = false;
    bool hasMidiInput = false;
    bool hasMidiOutput = false;
    int numAudioInputChannels = 0;
    int numAudioOutputChannels = 0;
    int numMidiInputPorts = 0;
    int numMidiOutputPorts = 0;
};

class PluginHost {
public:
    PluginHost();
    ~PluginHost();

    // Scan for plugins using given directories and blocklist
    void scanForPlugins(const std::vector<std::string>& dirs = {},
                        const std::set<std::string>& blocked = {});

    // Load a single plugin file directly
    bool loadPluginFile(const std::string& path);

    // Save/load scan results cache
    void saveScanCache(const std::string& path);
    void loadScanCache(const std::string& path);

    // Plugins that failed to load during scan (fileOrId)
    std::set<std::string> failedPlugins;
    bool isScanning() const { return scanning; }
    const std::vector<PluginInfo>& getAvailablePlugins() const { return availablePlugins; }

    // Load a plugin instance
    struct LoadedPlugin {
        std::unique_ptr<juce::AudioPluginInstance> instance;
        PluginInfo info;
        bool editorOpen = false;
        bool prepared = false;
        int graphNodeId = -1; // JUCE graph node ID when in graph
    };

    // Load a plugin by index into availablePlugins
    std::unique_ptr<LoadedPlugin> loadPlugin(int pluginIndex, double sampleRate, int blockSize);

    // Detailed plugin info (requires temporarily loading the plugin)
    struct PluginDetail {
        PluginInfo info;
        struct BusInfo {
            std::string name;
            int channels;
            bool isInput;
        };
        std::vector<BusInfo> buses;
        struct ParamInfo {
            std::string name;
            std::string label;   // unit label
            float defaultValue;
            int numSteps;        // 0 = continuous
            bool isAutomatable;
            bool isDiscrete;
        };
        std::vector<ParamInfo> params;
        std::vector<std::string> presets;
        int latencySamples = 0;
        int tailSeconds = 0;
        bool acceptsMidi = false;
        bool producesMidi = false;
    };

    PluginDetail getPluginDetail(int pluginIndex);

    // Show/hide the plugin's native editor UI
    // Returns a JUCE component that must be added to a window
    void openPluginEditor(LoadedPlugin& plugin);
    void closePluginEditor(LoadedPlugin& plugin);

private:
    juce::AudioPluginFormatManager formatManager;
    std::unique_ptr<juce::KnownPluginList> knownPlugins;
    std::vector<PluginInfo> availablePlugins;
    bool scanning = false;

    void addPluginPaths();
};

} // namespace SoundShop

#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <functional>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <set>

namespace SoundShop {

struct PluginInfo {
    std::string name;
    std::string manufacturer;
    std::string format;       // "VST3", "AU", etc.
    std::string fileOrId;     // file path or unique ID
    std::string category;
    std::string version;
    std::string descriptiveName;
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

    // The JUCE description everything above is derived from - and the only
    // thing that can instantiate the plugin. It is the plugin's identity: a
    // project saves it for each plugin node (Node::pluginDescription), and
    // PluginHost::resolvePlugin matches it against the plugin list on open.
    juce::PluginDescription description;
};

class PluginHost {
public:
    PluginHost();
    ~PluginHost();

    // Scan Now: find the plugins in `dirs` (the [ScanDirs] list) plus each
    // format's own default locations - see pluginSearchPath.
    //
    // `blocked` (the [Blocked] list) is handed to JUCE's scanner as its
    // blacklist, so a blocked plugin isn't loaded at all - a plugin gets
    // blocked because loading it went wrong. Blocked plugins also stay out of
    // getAvailablePlugins() (see setBlockedPlugins).
    //
    // `deadMansPedal`: a file the scanner writes each plugin's identifier to
    // before loading it and clears after. If SEANCE dies while loading one,
    // the next scan finds the culprit there, skips it, and reports it in
    // failedPlugins for blocking. juce::File() = no crash protection.
    void scanForPlugins(const std::vector<std::string>& dirs,
                        const std::set<std::string>& blocked,
                        const juce::File& deadMansPedal = {});

    // The same scan over exactly the folders `pathFor` gives each format -
    // scanForPlugins is this with pluginSearchPath. The self-tests use it to
    // scan their own test plugins without touching the user's plugin folders.
    using SearchPathFn = std::function<juce::FileSearchPath(juce::AudioPluginFormat&)>;
    void scanFolders(const SearchPathFn& pathFor,
                     const std::set<std::string>& blocked,
                     const juce::File& deadMansPedal = {});

    // The [Blocked] list, by identifier (file path or LV2 URI). Blocked
    // plugins leave getAvailablePlugins() at once and are refused by
    // resolvePlugin, loadPlugin and getPluginDetail; unblocking one that was
    // described before brings it straight back. Call at startup and whenever
    // the list changes.
    void setBlockedPlugins(const std::set<std::string>& blocked);
    bool isBlocked(const juce::PluginDescription& desc) const;

    // Make the plugins in the user's plugin folders loadable without a scan.
    // Call once at startup, before any plugin is instantiated, with the
    // [ScanDirs] list (Scan Now does the same for the folders it scans).
    // Without it an LV2 plugin living in one of those folders - rather than a
    // standard LV2 folder - loads in the session it was scanned in, and fails
    // with "Unable to locate plugin with the requested URI" after a restart.
    // Message thread only.
    void registerPluginFolders(const std::vector<std::string>& dirs);

    // Describe the plugin(s) in one file and add them to the list.
    bool loadPluginFile(const std::string& path);

    // Save/load scan results cache
    void saveScanCache(const std::string& path);
    void loadScanCache(const std::string& path);

    // Plugins that failed to load during the last scan (fileOrId), including
    // ones the dead man's pedal caught. Scan Now blocks them.
    std::set<std::string> failedPlugins;
    bool isScanning() const { return scanning; }

    // Every plugin the scan described, minus the blocked ones - what the
    // plugin menus offer. The row order means nothing beyond the moment: to
    // refer to a plugin anywhere that outlives it, keep its description.
    const std::vector<PluginInfo>& getAvailablePlugins() const { return availablePlugins; }

    // A described plugin by identifier, blocked or not (so the plugin
    // settings can name blocked plugins). nullptr if the scan never described
    // it - a plugin blocked before it was scanned has no name to show.
    const PluginInfo* findDescribed(const std::string& fileOrId) const;

    // Load a plugin instance
    struct LoadedPlugin {
        std::unique_ptr<juce::AudioPluginInstance> instance;
        PluginInfo info;
        bool editorOpen = false;
        bool prepared = false;
        int graphNodeId = -1; // JUCE graph node ID when in graph
        int nodeId = -1;      // the node it belongs to, once attached to one

        // `instance` normally moves into the live audio graph soon after the
        // plugin loads, and leaves it through GraphProcessor's retirement.
        // A plugin inside a Voice container keeps it for good, though - it's
        // the master its voices' copies follow (see plugin_copies.h) - and
        // goes with its node: deleted, undone, its project replaced. onRelease
        // is told first, while the instance is still alive, so the main window
        // can close the plugin's window and keep its state for an undo. Set by
        // the main window; message thread.
        static inline std::function<void(LoadedPlugin&)> onRelease;
        ~LoadedPlugin() {
            if (instance && onRelease)
                onRelease(*this);
        }
    };

    // Which plugin a project's saved description should load now:
    //   1. exactly that plugin (same format, file or LV2 URI, and ids), if listed;
    //   2. the same plugin listed elsewhere - same format, format id (VST3
    //      class id) and name: moved, reinstalled or updated;
    //   3. the same format, name and maker, listed (an update that changed
    //      the id);
    //   4. the saved description itself, if the plugin isn't listed but is
    //      still installed where the project says (another computer's
    //      project, no scan yet) - unless it's blocked.
    // When nothing matches, `problem` says why in a sentence that names the
    // plugin, for the node's Failed badge.
    struct Resolution {
        bool found = false;
        juce::PluginDescription description;
        std::string problem;
    };
    Resolution resolvePlugin(const juce::PluginDescription& saved) const;

    // Projects saved before SEANCE recorded plugin identities only give the
    // plugin's row in that day's plugin list (the pre-fix "pluginIndex").
    // Nodes are created with their plugin's name, so a plugin of that name
    // wins (a numbered repeat, "Serum 2", counts); the row in today's list is
    // the fallback - still right if the list hasn't changed since.
    Resolution resolveLegacyIndex(int index, const std::string& nodeName) const;

    // Instantiate exactly `desc` (see resolvePlugin for which one to ask
    // for). nullptr on failure, with the reason in *error. Blocked plugins
    // are refused.
    std::unique_ptr<LoadedPlugin> loadPlugin(const juce::PluginDescription& desc,
                                             double sampleRate, int blockSize,
                                             std::string* error = nullptr);

    // Just the instance, not yet prepared or configured - for a copy of a
    // plugin that's already loaded (PluginCopies). Blocked plugins are refused.
    // Message thread.
    std::unique_ptr<juce::AudioPluginInstance> instantiate(const juce::PluginDescription& desc,
                                                           double sampleRate, int blockSize,
                                                           std::string* error = nullptr);

    // resolvePlugin / resolveLegacyIndex + loadPlugin, for a node being
    // opened: `plugin` on success; `resolved` is the plugin the node should
    // now name (set whenever resolution found one, even if it then failed to
    // start); `error` says why `plugin` is null.
    struct NodeLoad {
        std::unique_ptr<LoadedPlugin> plugin;
        juce::PluginDescription resolved;
        std::string error;
    };
    NodeLoad loadNodePlugin(const juce::PluginDescription& saved, int legacyIndex,
                            const std::string& nodeName,
                            double sampleRate, int blockSize);

    // "'Name' (Maker, VST3)" - how the messages above name a plugin.
    static std::string describeForUser(const juce::PluginDescription& desc);

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

    PluginDetail getPluginDetail(const juce::PluginDescription& desc);

    // Show/hide the plugin's native editor UI
    // Returns a JUCE component that must be added to a window
    void openPluginEditor(LoadedPlugin& plugin);
    void closePluginEditor(LoadedPlugin& plugin);

private:
    juce::AudioPluginFormatManager formatManager;
    // Every plugin described by the last scan (or the cache, or
    // loadPluginFile), each carrying its own JUCE description. There is no
    // second list: availablePlugins is derived from this one, so a row can
    // never load a different plugin than the one it shows.
    std::vector<PluginInfo> plugins;
    std::vector<PluginInfo> availablePlugins;   // plugins minus blocked
    std::set<std::string> blocked;
    bool scanning = false;

    void updateAvailable();
    juce::AudioPluginFormat* findFormat(const juce::String& name) const;
    static PluginInfo makeInfo(const juce::PluginDescription& desc);
};

} // namespace SoundShop

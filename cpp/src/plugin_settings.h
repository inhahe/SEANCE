#pragma once
#include "plugin_host.h"
#include <string>
#include <vector>
#include <set>

namespace SoundShop {

// soundshop_plugins.cfg: [ScanDirs] (plugin folders) and [Blocked] (the skip
// list), plus SEANCE's built-in default folders.
//
// Also compiled into PresetRecorder (../../PresetRecorder), which reads this
// file to record every plugin SEANCE can see. Keep this file and
// plugin_settings.cpp free of SEANCE-internal dependencies (JUCE and the
// standard library only), and remember that tool if the file format changes.
class PluginSettings {
public:
    PluginSettings();

    std::vector<std::string> scanDirs;
    std::set<std::string> blockedPlugins;

    void addDefaultDirs();
    void save(const std::string& path) const;
    void load(const std::string& path);

    bool isBlocked(const std::string& fileOrId) const {
        return blockedPlugins.count(fileOrId) > 0;
    }
};

} // namespace SoundShop

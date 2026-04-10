#pragma once
#include "plugin_host.h"
#include <string>
#include <vector>
#include <set>

namespace SoundShop {

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

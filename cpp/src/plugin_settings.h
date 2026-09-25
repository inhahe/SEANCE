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

// The folders in `folders` (the [ScanDirs] list) that `format` should search
// on top of its own default install locations: absolute paths that aren't one
// of those defaults - and, for LV2, only the folders that actually contain an
// LV2 bundle (a subfolder holding a manifest.ttl).
//
// Why LV2 is filtered: JUCE's LV2 library (lilv) treats EVERY entry of a folder
// it is given as a bundle, and for each one that isn't it writes three
// "failed to open file .../manifest.ttl" errors to stderr - about a hundred
// lines into seance.log for the usual VST3/VST2 folders. It can only find LV2
// plugins in subfolders with a manifest.ttl anyway, so the filter changes which
// folders it looks at, never which plugins it finds.
//
// Why the defaults are left out: they are searched anyway, and lilv writes a
// "Reloading plugin" warning for every plugin in a folder it loads twice.
//
// PluginHost::registerPluginFolders loads exactly these at startup.
juce::FileSearchPath userPluginFolders(juce::AudioPluginFormat& format,
                                       const std::vector<std::string>& folders);

// Everywhere to look for plugins of `format`: its default install locations
// followed by userPluginFolders(). Used by PluginHost's scan and by
// PresetRecorder, so both search exactly the same places.
juce::FileSearchPath pluginSearchPath(juce::AudioPluginFormat& format,
                                      const std::vector<std::string>& folders);

} // namespace SoundShop

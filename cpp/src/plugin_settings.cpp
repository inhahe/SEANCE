#define NOMINMAX
#include "plugin_settings.h"
#include <fstream>
#include <algorithm>
#include <cstdlib>

namespace SoundShop {

PluginSettings::PluginSettings() {
    addDefaultDirs();
}

void PluginSettings::addDefaultDirs() {
#ifdef _WIN32
    scanDirs.push_back("C:\\Program Files\\Common Files\\VST3");
    scanDirs.push_back("C:\\Program Files\\Steinberg\\VSTPlugins");
    scanDirs.push_back("C:\\Program Files\\Vstplugins");
    scanDirs.push_back("C:\\Program Files (x86)\\Common Files\\VST3");
    scanDirs.push_back("C:\\Program Files (x86)\\Steinberg\\VSTPlugins");
    scanDirs.push_back("C:\\Program Files (x86)\\VstPlugins");
    scanDirs.push_back("C:\\VstPlugins");
    char* appdata = std::getenv("LOCALAPPDATA");
    if (appdata)
        scanDirs.push_back(std::string(appdata) + "\\Programs\\Common\\VST3");
#elif __APPLE__
    scanDirs.push_back("/Library/Audio/Plug-Ins/VST");
    scanDirs.push_back("/Library/Audio/Plug-Ins/VST3");
    scanDirs.push_back("/Library/Audio/Plug-Ins/Components");
    scanDirs.push_back("~/Library/Audio/Plug-Ins/VST");
    scanDirs.push_back("~/Library/Audio/Plug-Ins/VST3");
    scanDirs.push_back("~/Library/Audio/Plug-Ins/Components");
    scanDirs.push_back("/Library/Audio/Plug-Ins/LV2");
    scanDirs.push_back("~/Library/Audio/Plug-Ins/LV2");
#else
    // Linux/BSD
    scanDirs.push_back("/usr/lib/vst");
    scanDirs.push_back("/usr/local/lib/vst");
    scanDirs.push_back("/usr/lib/vst3");
    scanDirs.push_back("/usr/local/lib/vst3");
    scanDirs.push_back("/usr/lib/ladspa");
    scanDirs.push_back("/usr/local/lib/ladspa");
    scanDirs.push_back("/usr/lib/lv2");
    scanDirs.push_back("/usr/local/lib/lv2");
    char* home = std::getenv("HOME");
    if (home) {
        scanDirs.push_back(std::string(home) + "/.vst");
        scanDirs.push_back(std::string(home) + "/.vst3");
        scanDirs.push_back(std::string(home) + "/.ladspa");
        scanDirs.push_back(std::string(home) + "/.lv2");
    }
#endif
}

void PluginSettings::save(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return;
    f << "[ScanDirs]\n";
    for (auto& d : scanDirs) f << d << "\n";
    f << "[Blocked]\n";
    for (auto& b : blockedPlugins) f << b << "\n";
    f << "[End]\n";
}

void PluginSettings::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return;
    scanDirs.clear();
    blockedPlugins.clear();
    std::string line, section;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (line[0] == '[') { section = line; continue; }
        if (section == "[ScanDirs]") scanDirs.push_back(line);
        else if (section == "[Blocked]") blockedPlugins.insert(line);
    }
    if (scanDirs.empty()) addDefaultDirs();
}

// True if `dir` holds at least one LV2 bundle: a subfolder with a manifest.ttl.
static bool containsLv2Bundle(const juce::File& dir) {
    for (const auto& entry : juce::RangedDirectoryIterator(dir, false, "*", juce::File::findDirectories))
        if (entry.getFile().getChildFile("manifest.ttl").existsAsFile())
            return true;
    return false;
}

// One of a format's default locations as a real folder. The formats give their
// defaults in the form their own library wants - JUCE's LV2 ones are
// "%APPDATA%\LV2" and "~/.lv2", which lilv expands itself - and juce::File
// doesn't expand %VAR%, so do that here. (File does resolve a leading ~.)
static juce::File defaultLocation(const juce::String& raw) {
    juce::String path;
    for (int pos = 0;;) {
        const int start = raw.indexOfChar(pos, '%');
        const int end = start < 0 ? -1 : raw.indexOfChar(start + 1, '%');
        if (end < 0) { path << raw.substring(pos); break; }
        path << raw.substring(pos, start)
             << juce::SystemStats::getEnvironmentVariable(raw.substring(start + 1, end), {});
        pos = end + 1;
    }
    return juce::File::isAbsolutePath(path) ? juce::File(path) : juce::File();
}

juce::FileSearchPath userPluginFolders(juce::AudioPluginFormat& format,
                                       const std::vector<std::string>& folders) {
    const auto defaults = format.getDefaultLocationsToSearch();
    const bool lv2 = format.getName() == "LV2";

    juce::FileSearchPath result;
    for (auto& f : folders) {
        const juce::String folder(f);
        if (!juce::File::isAbsolutePath(folder)) continue;  // a relative entry has no meaning here
        const juce::File dir(folder);

        bool isDefault = false;
        for (int i = 0; i < defaults.getNumPaths() && !isDefault; ++i)
            isDefault = defaultLocation(defaults.getRawString(i)) == dir;
        if (isDefault) continue;                            // see the header comment

        if (lv2 && !containsLv2Bundle(dir)) continue;       // see the header comment

        result.addIfNotAlreadyThere(dir);
    }
    return result;
}

juce::FileSearchPath pluginSearchPath(juce::AudioPluginFormat& format,
                                      const std::vector<std::string>& folders) {
    auto path = format.getDefaultLocationsToSearch();
    const auto extra = userPluginFolders(format, folders);
    for (int i = 0; i < extra.getNumPaths(); ++i)
        path.add(extra[i]);
    return path;
}

} // namespace SoundShop

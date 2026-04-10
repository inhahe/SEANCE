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

} // namespace SoundShop

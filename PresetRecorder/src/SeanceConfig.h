#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

namespace PresetRecorder {

// What SoundShop2 (SEANCE) knows about plugins, read from its own files:
//
//   soundshop_plugins.cfg        [ScanDirs] - the plugin folders SEANCE scans
//                                [Blocked]  - SEANCE's skip list: plugins it will
//                                             not load (auto-filled with plugins
//                                             whose scan failed, plus any the user
//                                             blocked by hand)
//   soundshop_plugins_cache.dat  SEANCE's last scan results, including a JUCE
//                                KnownPluginList XML - used here only to put
//                                names and companies on skip-list entries
//
// SEANCE reads these relative to its working directory, so where they are
// depends on how SEANCE was launched; see findCandidates(). The .cfg is parsed
// by SEANCE's own PluginSettings class (compiled into this tool), so its format
// and SEANCE's built-in default folders can never drift apart.
struct SeanceConfig
{
    juce::File configFile;          // the .cfg in use (may not exist)
    bool configFound = false;
    bool autoDetected = false;

    juce::StringArray scanDirs;     // [ScanDirs], or SEANCE's defaults if there's no .cfg
    juce::StringArray skipList;     // [Blocked] (SEANCE keeps it sorted)

    juce::File cacheFile;           // soundshop_plugins_cache.dat next to the .cfg
    juce::Array<juce::PluginDescription> cachedTypes;

    // Loads `explicitConfig` if given, else the best candidate from findCandidates().
    static SeanceConfig load(const juce::File& explicitConfig);

    // Every place SEANCE may have written soundshop_plugins.cfg that exists,
    // newest first: the SoundShop2 folder above this exe (SEANCE is normally
    // started from there by seance.bat), SEANCE's own exe folder (double-click
    // launches), and the current directory.
    static juce::Array<juce::File> findCandidates();

    juce::String summary() const;
};

} // namespace PresetRecorder

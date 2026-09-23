#pragma once
#include <juce_core/juce_core.h>

namespace PresetRecorder {

// What can be learned about a plugin file WITHOUT loading it. Used for the
// skip list: those plugins are on it because loading them crashed, hung or
// failed, so the tool must be able to name and group them by company without
// ever running their code.
//
// Sources, in the order they are tried for the company name:
//   1. a VST3 bundle's Contents/Resources/moduleinfo.json (vendor, class names,
//      instrument/effect sub-categories) - the VST3 3.7.5+ "module info";
//   2. the Windows version resource of the binary (CompanyName, ProductName),
//      read with GetFileVersionInfo, which maps the file as data;
//   3. the folder name, when the plugin sits in a vendor folder (VST3\iZotope\x.vst3).
// The binary's PE header also tells us whether it is 32-bit - the usual reason
// a plugin lands on SoundShop2's skip list, since a 64-bit host can't load it.
struct PluginFileInfo
{
    bool isFile = false;            // false: an identifier such as an LV2 URI
    bool exists = false;
    juce::File binary;              // the DLL/.so/.vst3 that would actually be loaded
    juce::String architecture;      // "64-bit (x64)", "32-bit (x86)", "ARM64", or empty
    bool loadableArchitecture = true;

    juce::String company;
    juce::String companySource;     // where `company` came from, for the details panel
    juce::StringArray pluginNames;  // plugin classes inside, when known
    juce::String version;
    int instrumentState = -1;       // -1 unknown, 0 effect, 1 instrument

    static PluginFileInfo inspect(const juce::String& fileOrIdentifier);

    // "SSL 4K B" from "C:\...\SSL 4K B.vst3", "Podolski" from "Podolski(x64).vst3".
    static juce::String displayStem(const juce::String& fileOrIdentifier);
};

} // namespace PresetRecorder

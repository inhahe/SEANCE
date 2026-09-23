#pragma once
#include <juce_core/juce_core.h>
#include <map>

namespace PresetRecorder {

// Index of the .vstpreset files in the standard VST3 preset folders, keyed by
// the plugin class ID stored in each file's header.
//
// A .vstpreset starts with "VST3", a 4-byte version, then the 32-character
// class ID of the plugin it belongs to. Matching on that ID (rather than on the
// Vendor\Plugin folder names, which vary) means a preset is only ever applied
// to the plugin that wrote it; JUCE's VST3Client::setPreset checks the same ID
// again when loading.
//
// The GUI process builds the index once per recording run and hands it to the
// workers as a file; each worker asks its loaded plugin for its own class ID
// (from the header of the plugin's current state saved as a .vstpreset) and
// looks it up.
struct Vst3PresetIndex
{
    std::map<juce::String, juce::StringArray> byClassId;  // upper-case hex ID -> files
    int numFiles = 0;

    // The VST3 SDK's standard preset locations for this OS.
    static juce::Array<juce::File> standardRoots();

    // Scans `roots` recursively (plus any extra folders, e.g. VST3 bundles'
    // Contents/Resources) and reads every header.
    static Vst3PresetIndex build(const juce::Array<juce::File>& roots);

    // Class ID from a .vstpreset file / from .vstpreset data; empty if not one.
    static juce::String readClassId(const juce::File& file);
    static juce::String readClassId(const void* data, size_t size);

    juce::var toVar() const;
    static Vst3PresetIndex fromVar(const juce::var&);
};

// Display name for a .vstpreset: its path below the plugin's own preset folder
// ("Bass - Deep Sub" for .../VST3 Presets/Vendor/Plugin/Bass/Deep Sub.vstpreset),
// or just the file name if it isn't under a folder named after the plugin.
juce::String presetNameFromFile(const juce::File& file, const juce::String& pluginName);

} // namespace PresetRecorder

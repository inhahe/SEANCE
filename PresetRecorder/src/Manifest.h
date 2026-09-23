#pragma once
#include <juce_core/juce_core.h>
#include <vector>

namespace PresetRecorder {

// One recorded (or attempted) preset.
struct PresetResult
{
    juce::String key;        // "program:12", "vstpreset:<path>", "default" - stable identity
    juce::String name;       // display name
    juce::String source;     // human-readable origin: "factory program #12", a .vstpreset path, ...
    juce::String file;       // recording, relative to the output folder ("Instruments/u-he - Zebra2 - Init.flac")
    juce::String status;     // "ok", "silent", "failed", "crashed", "timeout", "not recorded"
    juce::String note;       // extra detail (why it failed, gain applied, ...)
    double seconds = 0.0;
    double peakDb = -200.0;
    juce::String recordedAt;

    bool hasAudio() const { return status == "ok" || status == "silent"; }
};

// One plugin's results.
struct PluginResult
{
    juce::String id;         // PluginDescription::createIdentifierString()
    juce::String name, company, format, version, category;
    juce::String path;       // fileOrIdentifier - where the plugin is on the drive
    juce::String kind;       // "instrument" or "effect"
    juce::String baseName;
    juce::String status;     // "ok", "partial", "failed", "recording"
    juce::String error;
    juce::String lastRun;
    std::vector<PresetResult> presets;

    bool isInstrument() const { return kind == "instrument"; }
    PresetResult* findPreset(const juce::String& key);
    int countWithAudio() const;
};

// manifest.json in the output folder: what was recorded, from what, and how it
// went. The GUI process is its only writer (workers report through their event
// logs), and the Browse tab reads it, so recordings stay browsable across runs.
class Manifest
{
public:
    void setRoot(const juce::File& outputFolder);
    const juce::File& getRoot() const { return root; }
    juce::File getFile() const { return root.getChildFile("manifest.json"); }

    bool load();   // replaces the contents; false if there's no manifest yet
    bool save();
    void markDirty() { dirty = true; }
    bool isDirty() const { return dirty; }

    std::vector<PluginResult>& getPlugins() { return plugins; }
    const std::vector<PluginResult>& getPlugins() const { return plugins; }
    PluginResult* find(const juce::String& id);
    PluginResult& getOrCreate(const juce::String& id);

    // The dry effect input excerpt written alongside the recordings.
    juce::String dryInputFile;

    static constexpr const char* instrumentsFolder = "Instruments";
    static constexpr const char* effectsFolder = "Effects";

private:
    juce::File root;
    std::vector<PluginResult> plugins;
    bool dirty = false;
};

} // namespace PresetRecorder

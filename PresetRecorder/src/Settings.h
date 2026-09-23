#pragma once
#include <juce_data_structures/juce_data_structures.h>

namespace PresetRecorder {

// Everything a worker needs to know about *how* to render. Sent to each worker
// in its job file; also what the Record tab edits.
struct RenderSettings
{
    double sampleRate = 44100.0;
    int bitDepth = 16;                 // FLAC: 16 or 24
    int blockSize = 512;
    double tempoBpm = 130.0;           // reported to tempo-synced plugins; the bundled song is ~130
    double maxTailSeconds = 3.0;       // how long a reverb/release may ring after the input ends
    double silenceThresholdDb = -70.0; // the tail ends once the output stays below this
    double settleMs = 250.0;           // wall-clock pause after switching preset (async preset loaders)
    double preRollSeconds = 0.3;       // silent audio processed after switching preset, discarded
    int maxPresetsPerPlugin = 0;       // 0 = all
    bool skipExisting = true;          // keep recordings that already exist
    bool includePrograms = true;       // presets the plugin exposes itself (VST3 program list, LV2 presets)
    bool includePresetFiles = true;    // .vstpreset files in the standard VST3 preset folders

    juce::var toVar() const;
    static RenderSettings fromVar(const juce::var&);
};

// Persistent settings, stored in %APPDATA%\PresetRecorder\PresetRecorder.settings.
class AppSettings
{
public:
    AppSettings();

    juce::PropertiesFile& props() { return *properties; }
    void saveNow() { properties->saveIfNeeded(); }

    // Where SoundShop2's soundshop_plugins.cfg is. Empty = find it automatically.
    juce::String getSeanceConfigOverride() const;
    void setSeanceConfigOverride(const juce::String&);

    juce::File getOutputDir() const;
    void setOutputDir(const juce::File&);

    // Effect input. Empty path = the bundled public-domain song.
    juce::String getInputSongOverride() const;
    void setInputSongOverride(const juce::String&);
    double getSongStart() const;
    void setSongStart(double);
    double getSongLength() const;
    void setSongLength(double);

    // Instrument input: the built-in phrase, or a MIDI file.
    bool getUseMidiFile() const;
    void setUseMidiFile(bool);
    juce::String getMidiFile() const;
    void setMidiFile(const juce::String&);

    RenderSettings getRenderSettings() const;
    void setRenderSettings(const RenderSettings&);

    int getParallelWorkers() const;
    void setParallelWorkers(int);
    double getLoadTimeoutSeconds() const;
    void setLoadTimeoutSeconds(double);
    double getPresetTimeoutSeconds() const;
    void setPresetTimeoutSeconds(double);

    // Entries of SoundShop2's skip list that the user chose to record anyway,
    // keyed by identifierKey(). Tool-local: SoundShop2's own file is never touched.
    juce::StringArray getUnskipped() const;
    void setUnskipped(const juce::StringArray&);

    double getPlaybackVolume() const;
    void setPlaybackVolume(double);

    // The bundled song lives next to the exe (copied there by the build), or -
    // when running from a build tree whose copy is missing - in the source
    // tree's resources/ folder, or finally in app data (where the in-app
    // download puts it).
    static juce::File findBundledSong();
    static juce::File downloadedSongLocation();
    static const char* bundledSongFileName();
    static const char* bundledSongUrl();
    static constexpr double defaultSongStart = 207.55;
    static constexpr double defaultSongLength = 4.0;

private:
    std::unique_ptr<juce::PropertiesFile> properties;
};

} // namespace PresetRecorder

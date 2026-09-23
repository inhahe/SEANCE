#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "SeanceConfig.h"
#include "PluginFileInfo.h"
#include <map>
#include <vector>

namespace PresetRecorder {

// One plugin file (a .vst3 file or bundle) or identifier (an LV2 URI) - the
// unit SoundShop2's skip list works in. A single file can hold several plugins
// (u-he's Zebra2 .vst3 holds Zebra2, Zebralette, Zebrify and ZRev).
struct PluginFile
{
    enum class State
    {
        pending,     // found, waiting to be scanned
        scanning,
        scanned,     // `types` holds what it contains
        scanFailed,  // `error` says why
        skipped      // on SoundShop2's skip list and not unskipped here
    };

    juce::String format;             // "VST3", "LV2", ...
    juce::String fileOrId;
    juce::String key;                // identifierKey(fileOrId)
    bool onSkipList = false;
    bool inScanFolders = false;
    State state = State::pending;
    juce::String error;
    juce::Array<juce::PluginDescription> types;

    PluginFileInfo fileInfo;         // lazily filled for skip-list display
    bool fileInfoLoaded = false;
};

// One plugin (a type inside a file), ready to be recorded.
struct PluginEntry
{
    juce::PluginDescription desc;
    juce::String id;        // desc.createIdentifierString() - stable across runs
    juce::String baseName;  // "<Company> - <Plugin>", unique; every recording is "<baseName> - <Preset>.flac"
    bool instrument = false;
    juce::String fileKey;
};

// What the skip-list tree shows for one entry.
struct SkipEntryDetails
{
    juce::String company;
    juce::String companySource;
    juce::StringArray names;
    juce::String displayName;
};

// The GUI process's model of the plugin world: the files in SoundShop2's scan
// folders, their scan results (cached in %APPDATA%\PresetRecorder\plugin_cache.xml
// so unchanged files are never re-scanned), and SoundShop2's skip list.
//
// Nothing in here ever loads a plugin. Scanning happens in worker processes
// (see JobRunner / Worker); the catalog only lists files and records results.
class Catalog
{
public:
    Catalog();

    juce::AudioPluginFormatManager& getFormats() { return formatManager; }

    void setSeanceConfig(const SeanceConfig& c) { seance = c; }
    const SeanceConfig& getSeanceConfig() const { return seance; }

    // Skip-list entries (identifierKey form) the user chose to record anyway.
    void setUnskipped(const juce::StringArray& keys) { unskipped = keys; }
    const juce::StringArray& getUnskipped() const { return unskipped; }
    bool isSkipped(const PluginFile& f) const { return f.onSkipList && ! unskipped.contains(f.key); }

    // SoundShop2's scan folders plus the format's own default locations -
    // exactly what SEANCE's PluginHost::scanForPlugins searches.
    juce::FileSearchPath getSearchPath(juce::AudioPluginFormat& format) const;

    struct ScanRequest { juce::String format, fileOrId; };

    // Rebuilds the file list from the scan folders and the skip list and returns
    // the files that need scanning (not skipped, and not already cached for the
    // file's current modification time unless `forceRescan`).
    std::vector<ScanRequest> enumerate(bool forceRescan);

    PluginFile* findFile(const juce::String& fileOrId);
    std::vector<PluginFile>& getFiles() { return files; }
    const std::vector<PluginFile>& getFiles() const { return files; }

    void setScanning(const juce::String& fileOrId);
    void applyScanResult(const juce::String& fileOrId, const juce::Array<juce::PluginDescription>& types);
    // A scan that crashed, hung or couldn't run. Not cached: the file is
    // scanned again next time (a clean scan that finds nothing loadable - a
    // 32-bit plugin - goes through applyScanResult and is cached).
    void applyScanFailure(const juce::String& fileOrId, const juce::String& reason);

    // Every plugin that can be recorded, sorted by company then name, with
    // collision-free base names assigned.
    std::vector<PluginEntry> getRecordablePlugins() const;

    std::vector<PluginFile*> getSkipListEntries();
    SkipEntryDetails describeSkipEntry(PluginFile& f);

    void loadCache();
    void saveCache() const;
    void clearCache();

private:
    struct CacheEntry
    {
        juce::String format, fileOrId;
        juce::int64 modTime = 0;
        juce::Array<juce::PluginDescription> types;
        juce::String failure;
    };

    PluginFile& addOrGet(const juce::String& format, const juce::String& fileOrId);
    juce::String guessFormat(const juce::String& fileOrId);
    static juce::int64 modTimeOf(const juce::String& fileOrId);
    bool isFresh(const CacheEntry&, const juce::String& fileOrId) const;

    juce::AudioPluginFormatManager formatManager;
    SeanceConfig seance;
    juce::StringArray unskipped;
    std::vector<PluginFile> files;
    std::map<juce::String, CacheEntry> cache;
};

} // namespace PresetRecorder

#pragma once
#include <juce_events/juce_events.h>
#include "Settings.h"
#include "SeanceConfig.h"
#include "Catalog.h"
#include "Manifest.h"
#include "JobRunner.h"
#include <map>

namespace PresetRecorder {

// The GUI process's brain: owns the settings, SoundShop2's configuration, the
// plugin catalog, the output manifest and the job runner, and turns the UI's
// requests (scan, record, stop) into worker jobs. Everything here runs on the
// message thread; it broadcasts a change message whenever something the UI
// shows has changed.
class Controller : public juce::ChangeBroadcaster,
                   private JobRunner::Listener,
                   private juce::Timer
{
public:
    Controller();
    ~Controller() override;

    AppSettings& getSettings() { return settings; }
    Catalog& getCatalog() { return catalog; }
    Manifest& getManifest() { return manifest; }

    //==========================================================================
    // SoundShop2 configuration and the skip list

    void reloadSeanceConfig();
    void setSeanceConfigFile(const juce::String& pathOrEmptyForAuto);

    // Unskip / re-skip entries of SoundShop2's skip list (identifierKey form).
    // Only this tool's own settings change; SoundShop2's file is never written.
    void setUnskipped(const juce::StringArray& keys);

    //==========================================================================
    // Output

    void setOutputDir(const juce::File& dir);   // switches the manifest too
    juce::File getOutputDir() const { return settings.getOutputDir(); }

    juce::File getEffectInputSong() const;       // the override, or the bundled song
    bool isUsingBundledSong() const;
    void downloadBundledSong(std::function<void(bool ok, juce::String error)> done);

    //==========================================================================
    // Actions

    enum class Phase { idle, scanning, preparing, recording };
    Phase getPhase() const { return phase; }
    bool isBusy() const { return phase != Phase::idle; }
    bool hasScanned() const { return scannedThisSession; }

    void scan(bool forceRescan);
    void recordAll();
    void recordPlugins(const juce::StringArray& pluginIds);
    void stop();

    //==========================================================================
    // Progress and status, for the UI

    struct Progress
    {
        int pluginsDone = 0, pluginsTotal = 0;
        int presetsRecorded = 0;
        double fraction = 0.0;  // 0..1, or -1 for "indeterminate"
        juce::String headline;
        juce::StringArray workers;
    };
    Progress getProgress() const;

    // "queued", "recording 3/20", "done: 20 presets", "failed: ..." for a plugin id.
    juce::String getLiveStatus(const juce::String& pluginId) const;

    std::function<void(const juce::String&)> onLogLine;
    void log(const juce::String& line);
    const juce::StringArray& getLogLines() const { return logLines; }

private:
    void jobStarted(const JobRunner::Job&) override;
    void jobEvent(const JobRunner::Job&, const juce::var& event) override;
    void jobPresetLost(const JobRunner::Job&, const juce::String& key, const juce::String& status,
                       const juce::String& reason, bool willRetry) override;
    void jobFinished(const JobRunner::Job&, bool ok, const juce::String& error, bool timedOut) override;
    void allJobsFinished() override;
    void timerCallback() override;

    void startScan(bool forceRescan, std::function<void()> then);
    void finishScan();
    void startRecording(const juce::StringArray& pluginIds);
    void queueRenderJobs(const std::vector<PluginEntry>& plugins, const juce::File& effectInput,
                         const juce::String& effectInputError, const juce::File& presetIndex);
    void mergePresetList(PluginResult& result, const juce::var& event, const juce::StringArray& keepKeys);
    void removePartialFiles();
    void applyTimeoutsAndParallelism();

    AppSettings settings;
    Catalog catalog;
    Manifest manifest;
    JobRunner runner { *this };

    Phase phase = Phase::idle;
    bool scannedThisSession = false;
    bool stopRequested = false;
    std::function<void()> afterScan;

    std::map<juce::String, juce::String> liveStatus;   // plugin id -> status text
    std::map<juce::String, juce::String> scanResultFiles; // fileOrId -> result xml
    int runPluginsTotal = 0, runPluginsDone = 0, runPresetsRecorded = 0;
    int scanTotal = 0, scanDone = 0;
    int runGeneration = 0;   // bumped by every start/stop, so a stale preparation step can tell it's stale
    juce::Time runStarted;

    juce::StringArray logLines;
    juce::File logFile;

    JUCE_DECLARE_WEAK_REFERENCEABLE(Controller)
};

} // namespace PresetRecorder

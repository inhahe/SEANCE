#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Controller.h"
#include "Player.h"

namespace PresetRecorder {

// Where things come from and go to, how to render, and the run itself:
// scan/record buttons, progress, a table of every plugin file SoundShop2's
// folders contain (with live status), and the log.
class RecordTab : public juce::Component,
                  private juce::ChangeListener,
                  private juce::Timer,
                  private juce::TableListBoxModel
{
public:
    RecordTab(Controller&, Player&);
    ~RecordTab() override;

    void resized() override;
    void paint(juce::Graphics&) override;

    void appendLog(const juce::String& line);

    std::function<void(const juce::String& pluginId)> onShowRecordings;

private:
    struct Row
    {
        juce::String company, name, type, format, path, pluginId, fileKey, fixedStatus;
        bool recordable = false;
    };

    enum Column { colCompany = 1, colPlugin, colType, colFormat, colStatus, colPath };

    // TableListBoxModel
    int getNumRows() override;
    void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics&, int row, int column, int w, int h, bool selected) override;
    void sortOrderChanged(int newSortColumnId, bool isForwards) override;
    void cellClicked(int row, int column, const juce::MouseEvent&) override;
    void cellDoubleClicked(int row, int column, const juce::MouseEvent&) override;
    void selectedRowsChanged(int lastRowSelected) override;
    juce::String getCellTooltip(int row, int column) override;

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    void rebuildRows();
    void sortRows();
    juce::String statusFor(const Row&) const;
    juce::StringArray selectedPluginIds() const;
    void updateEnablement();
    void updateSourceInfo();
    void loadSettingsIntoControls();
    void storeRenderSettings();
    void chooseSeanceConfig();
    void chooseOutputDir();
    void chooseSong();
    void chooseMidiFile();
    void previewExcerpt();
    void showScanFolders();

    Controller& controller;
    Player& player;

    // ---- Sources ----
    juce::Label sourcesHeader, settingsHeader;
    juce::Label cfgLabel, cfgInfo, outLabel, songLabel, songInfo, excerptLabel, instLabel;
    juce::TextEditor cfgPath, outPath, songPath, midiPath;
    juce::TextButton cfgBrowse { "Browse..." }, cfgAuto { "Find automatically" }, cfgFolders { "Folders..." },
                     outBrowse { "Browse..." }, outOpen { "Open" },
                     songBrowse { "Browse..." }, songBundled { "Use bundled song" }, songPreview { "Preview excerpt" },
                     songDownload { "Download it" }, midiBrowse { "Browse..." };
    juce::Slider songStart, songLength;
    juce::Label songStartLabel, songLengthLabel;
    juce::ToggleButton phraseRadio { "Built-in phrase" }, midiRadio { "MIDI file:" };

    // ---- Render settings ----
    juce::Label rateLabel, bitsLabel, tempoLabel, tailLabel, settleLabel, limitLabel, workersLabel,
                loadTimeoutLabel, presetTimeoutLabel;
    juce::ComboBox rateBox, bitsBox;
    juce::Slider tempo, tail, settle, limit, workers, loadTimeout, presetTimeout;
    juce::ToggleButton keepExisting { "Keep existing recordings" },
                       usePrograms { "Presets built into the plugin" },
                       usePresetFiles { ".vstpreset files" };

    // ---- Run ----
    juce::TextButton scanButton { "Scan plugins" }, rescanButton { "Rescan all" },
                     recordAllButton { "Record all" }, recordSelectedButton { "Record selected" },
                     stopButton { "Stop" };
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };
    juce::Label headline, workerInfo;

    juce::TableListBox table { "Plugins", this };
    std::vector<Row> rows;
    int sortColumn = colCompany;
    bool sortForwards = true;
    bool rowsDirty = true;

    juce::TextEditor logView;
    int logLinesShown = 0;

    std::unique_ptr<juce::FileChooser> chooser;
    bool loadingControls = false;

    // The song's length, re-read only when the file changes (decoding an MP3's
    // length isn't free).
    juce::String cachedSongPath;
    juce::int64 cachedSongStamp = 0;
    double cachedSongLength = 0.0;
};

} // namespace PresetRecorder

#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Controller.h"
#include "Player.h"

namespace PresetRecorder {

// Browse and audition the recordings: plugins on the left, the selected
// plugin's presets in the middle (click one to hear it; the arrow keys step
// through them and play each), details on the right - including where the
// plugin is on the drive. For effects, "Dry input" plays the unprocessed song
// excerpt for comparison.
class BrowseTab : public juce::Component,
                  private juce::ChangeListener,
                  private juce::Timer
{
public:
    BrowseTab(Controller&, Player&);
    ~BrowseTab() override;

    void resized() override;
    void paint(juce::Graphics&) override;
    bool keyPressed(const juce::KeyPress&) override;

    void refresh();                              // re-reads the manifest's contents
    void showPlugin(const juce::String& pluginId);

    std::function<void()> onShowAudioSettings;

private:
    struct PluginList final : juce::ListBoxModel
    {
        BrowseTab& owner;
        explicit PluginList(BrowseTab& o) : owner(o) {}
        int getNumRows() override;
        void paintListBoxItem(int row, juce::Graphics&, int w, int h, bool selected) override;
        void selectedRowsChanged(int lastRowSelected) override;
        juce::String getTooltipForRow(int row) override;
    };

    struct PresetList final : juce::ListBoxModel
    {
        BrowseTab& owner;
        explicit PresetList(BrowseTab& o) : owner(o) {}
        int getNumRows() override;
        void paintListBoxItem(int row, juce::Graphics&, int w, int h, bool selected) override;
        void listBoxItemClicked(int row, const juce::MouseEvent&) override;
        void selectedRowsChanged(int lastRowSelected) override;
        void returnKeyPressed(int row) override;
        juce::String getTooltipForRow(int row) override;
    };

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    const PluginResult* selectedPlugin() const;
    const PresetResult* selectedPreset() const;
    void pluginSelectionChanged();
    void updatePresetHeader();
    void presetSelectionChanged(bool play);
    void playPreset(int row);
    void updateDetails();
    void updateTransportButtons();
    void rebuildVisibleList();

    Controller& controller;
    Player& player;

    PluginList pluginModel { *this };
    PresetList presetModel { *this };

    juce::Label searchLabel, showLabel, folderLabel;
    juce::TextEditor searchBox;
    juce::ComboBox typeFilter;
    juce::TextButton openFolderButton { "Open folder" }, audioSettingsButton { "Audio settings..." };
    juce::ListBox pluginListBox { "Plugins", &pluginModel }, presetListBox { "Presets", &presetModel };
    juce::Label pluginHeader, presetHeader;

    juce::TextEditor details;
    juce::TextButton revealPluginButton { "Show plugin in Explorer" }, copyPathButton { "Copy plugin path" },
                     revealRecordingButton { "Show recording" };

    juce::TextButton playButton { "Play" }, stopButton { "Stop" }, dryButton { "Dry input" };
    juce::ToggleButton loopToggle { "Loop" };
    juce::Slider volume;
    juce::Label volumeLabel, statusLabel;
    WaveformView waveform { player };

    std::vector<int> visible;            // indices into manifest plugins, after filtering
    juce::String selectedPluginId;
    juce::String playingPluginId, playingPresetKey;
    bool dryPlaying = false;
    juce::String lastManifestSignature;
};

} // namespace PresetRecorder

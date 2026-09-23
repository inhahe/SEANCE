#include "BrowseTab.h"
#include "Theme.h"

namespace PresetRecorder {

//==============================================================================
static juce::String matchText(const juce::TextEditor& box)
{
    return box.getText().trim();
}

static bool textContains(const juce::String& haystack, const juce::String& needle)
{
    return needle.isEmpty() || haystack.containsIgnoreCase(needle);
}

// Rows of the preset list for a plugin, honouring the search box: every preset
// if the plugin itself matches, otherwise only presets whose names match.
static std::vector<int> visiblePresetIndices(const PluginResult& p, const juce::String& search)
{
    std::vector<int> rows;
    const bool pluginMatches = textContains(p.company + " " + p.name, search);
    for (int i = 0; i < (int) p.presets.size(); ++i)
        if (pluginMatches || textContains(p.presets[(size_t) i].name, search))
            rows.push_back(i);
    return rows;
}

//==============================================================================
BrowseTab::BrowseTab(Controller& c, Player& p) : controller(c), player(p)
{
    Theme::styleFormLabel(searchLabel, "Search");
    Theme::styleFormLabel(showLabel, "Show");
    addAndMakeVisible(searchLabel);
    addAndMakeVisible(showLabel);

    searchBox.setTextToShowWhenEmpty("company, plugin or preset name", Theme::dimText);
    searchBox.setTooltip("Type part of a company, plugin or preset name to narrow the lists.");
    searchBox.onTextChange = [this] { refresh(); };
    searchBox.onEscapeKey = [this] { searchBox.clear(); refresh(); };
    addAndMakeVisible(searchBox);

    typeFilter.addItem("Instruments and effects", 1);
    typeFilter.addItem("Instruments only", 2);
    typeFilter.addItem("Effects only", 3);
    typeFilter.setSelectedId(1, juce::dontSendNotification);
    typeFilter.onChange = [this] { refresh(); };
    typeFilter.setTooltip("Instruments were recorded playing a short phrase; effects were recorded processing "
                          "the same few seconds of a public-domain song.");
    addAndMakeVisible(typeFilter);

    folderLabel.setColour(juce::Label::textColourId, Theme::dimText);
    folderLabel.setFont(Theme::font(12.0f));
    folderLabel.setMinimumHorizontalScale(0.5f);
    addAndMakeVisible(folderLabel);

    openFolderButton.setTooltip("Open the recordings folder (Instruments and Effects sub-folders) in Explorer.");
    openFolderButton.onClick = [this]
    {
        const auto dir = controller.getOutputDir();
        dir.createDirectory();
        dir.startAsProcess();
    };
    addAndMakeVisible(openFolderButton);

    audioSettingsButton.setTooltip("Choose the audio output device used for listening.");
    audioSettingsButton.onClick = [this] { if (onShowAudioSettings) onShowAudioSettings(); };
    addAndMakeVisible(audioSettingsButton);

    for (auto* h : { &pluginHeader, &presetHeader })
    {
        h->setFont(Theme::font(13.0f, true));
        h->setColour(juce::Label::textColourId, Theme::dimText);
        addAndMakeVisible(*h);
    }

    for (auto* lb : { &pluginListBox, &presetListBox })
    {
        lb->setRowHeight(24);
        lb->setColour(juce::ListBox::backgroundColourId, Theme::panel);
        lb->setMultipleSelectionEnabled(false);
        addAndMakeVisible(*lb);
    }

    details.setMultiLine(true, true);
    details.setReadOnly(true);
    details.setCaretVisible(false);
    details.setFont(Theme::font(13.0f));
    details.setColour(juce::TextEditor::backgroundColourId, Theme::panel);
    details.setColour(juce::TextEditor::outlineColourId, Theme::panel);
    addAndMakeVisible(details);

    revealPluginButton.onClick = [this]
    {
        if (auto* pr = selectedPlugin())
            if (juce::File::isAbsolutePath(pr->path))
                juce::File(pr->path).revealToUser();
    };
    copyPathButton.onClick = [this]
    {
        if (auto* pr = selectedPlugin())
            juce::SystemClipboard::copyTextToClipboard(pr->path);
    };
    revealRecordingButton.onClick = [this]
    {
        if (auto* preset = selectedPreset())
            controller.getManifest().getRoot().getChildFile(preset->file).revealToUser();
    };
    for (auto* b : { &revealPluginButton, &copyPathButton, &revealRecordingButton })
        addAndMakeVisible(*b);

    playButton.onClick = [this]
    {
        if (player.getCurrentFile() == juce::File())
            playPreset(presetListBox.getSelectedRow());
        else
            player.togglePlayPause();
    };
    stopButton.onClick = [this] { player.stop(); };
    stopButton.setTooltip("Stop and go back to the start of the recording.");

    dryButton.setTooltip("Play the unprocessed song excerpt every effect was fed, to compare with the effect's recording.");
    dryButton.onClick = [this]
    {
        const auto& dry = controller.getManifest().dryInputFile;
        juce::String error;
        if (dry.isNotEmpty() && player.play(controller.getManifest().getRoot().getChildFile(dry), error))
        {
            dryPlaying = true;
            playingPluginId = playingPresetKey = {};
            presetListBox.repaint();
        }
        else
        {
            statusLabel.setText(dry.isEmpty() ? "No dry input recorded yet - record an effect first." : error,
                                juce::dontSendNotification);
        }
    };

    loopToggle.setTooltip("Repeat the recording until you stop it.");
    loopToggle.onClick = [this] { player.setLooping(loopToggle.getToggleState()); };

    volume.setSliderStyle(juce::Slider::LinearHorizontal);
    volume.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    volume.setRange(0.0, 1.0);
    volume.setValue(controller.getSettings().getPlaybackVolume(), juce::dontSendNotification);
    volume.setTooltip("Listening volume (doesn't change the recordings).");
    volume.onValueChange = [this]
    {
        player.setGain((float) volume.getValue());
        controller.getSettings().setPlaybackVolume(volume.getValue());
    };
    player.setGain((float) volume.getValue());
    Theme::styleFormLabel(volumeLabel, "Volume");

    statusLabel.setColour(juce::Label::textColourId, Theme::dimText);
    statusLabel.setFont(Theme::font(12.0f));

    for (auto* comp : std::initializer_list<juce::Component*> { &playButton, &stopButton, &dryButton, &loopToggle,
                                                                 &volume, &volumeLabel, &statusLabel, &waveform })
        addAndMakeVisible(comp);

    controller.addChangeListener(this);
    player.addChangeListener(this);
    setWantsKeyboardFocus(false);
    refresh();
    startTimer(700);
}

BrowseTab::~BrowseTab()
{
    controller.removeChangeListener(this);
    player.removeChangeListener(this);
}

void BrowseTab::paint(juce::Graphics& g)
{
    g.fillAll(Theme::background);
}

void BrowseTab::resized()
{
    auto area = getLocalBounds().reduced(12);

    auto top = area.removeFromTop(28);
    searchLabel.setBounds(top.removeFromLeft(52));
    top.removeFromLeft(4);
    searchBox.setBounds(top.removeFromLeft(240));
    top.removeFromLeft(12);
    showLabel.setBounds(top.removeFromLeft(44));
    top.removeFromLeft(4);
    typeFilter.setBounds(top.removeFromLeft(190));
    top.removeFromLeft(12);
    audioSettingsButton.setBounds(top.removeFromRight(130));
    top.removeFromRight(8);
    openFolderButton.setBounds(top.removeFromRight(100));
    top.removeFromRight(8);
    folderLabel.setBounds(top);

    area.removeFromTop(10);

    // Transport along the bottom: buttons on the left, the waveform beside them.
    auto transport = area.removeFromBottom(96);
    auto buttons = transport.removeFromLeft(250).withTrimmedTop(16);
    auto row1 = buttons.removeFromTop(30);
    playButton.setBounds(row1.removeFromLeft(70).reduced(0, 1));
    row1.removeFromLeft(6);
    stopButton.setBounds(row1.removeFromLeft(70).reduced(0, 1));
    row1.removeFromLeft(6);
    dryButton.setBounds(row1.removeFromLeft(90).reduced(0, 1));
    buttons.removeFromTop(4);
    auto row2 = buttons.removeFromTop(28);
    loopToggle.setBounds(row2.removeFromLeft(70));
    volumeLabel.setBounds(row2.removeFromLeft(56));
    volume.setBounds(row2.reduced(4, 2));
    transport.removeFromLeft(10);
    statusLabel.setBounds(transport.removeFromBottom(18));
    waveform.setBounds(transport);

    area.removeFromBottom(10);

    auto pluginCol = area.removeFromLeft(juce::jmax(240, area.getWidth() * 30 / 100));
    area.removeFromLeft(10);
    auto presetCol = area.removeFromLeft(juce::jmax(240, area.getWidth() * 45 / 100));
    area.removeFromLeft(10);

    pluginHeader.setBounds(pluginCol.removeFromTop(22));
    pluginListBox.setBounds(pluginCol);
    presetHeader.setBounds(presetCol.removeFromTop(22));
    presetListBox.setBounds(presetCol);

    auto detailButtons = area.removeFromBottom(64);
    auto b1 = detailButtons.removeFromTop(30);
    revealPluginButton.setBounds(b1.removeFromLeft(juce::jmin(180, b1.getWidth() / 2)).reduced(0, 1));
    b1.removeFromLeft(6);
    copyPathButton.setBounds(b1.reduced(0, 1));
    detailButtons.removeFromTop(4);
    revealRecordingButton.setBounds(detailButtons.removeFromTop(30).removeFromLeft(180).reduced(0, 1));
    area.removeFromBottom(8);
    details.setBounds(area);
}

bool BrowseTab::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        if (player.getCurrentFile() == juce::File())
            playPreset(presetListBox.getSelectedRow());
        else
            player.togglePlayPause();
        return true;
    }
    return false;
}

//==============================================================================
void BrowseTab::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &player)
    {
        updateTransportButtons();
        presetListBox.repaint();
    }
    // Controller changes are picked up by the timer, so a busy recording run
    // doesn't rebuild the lists dozens of times a second.
}

void BrowseTab::timerCallback()
{
    // Refresh when the manifest's shape or statuses changed.
    juce::String sig;
    for (auto& p : controller.getManifest().getPlugins())
    {
        sig << p.id << p.status << (int) p.presets.size() << ":";
        for (auto& q : p.presets)
            sig << q.status.substring(0, 2);
    }
    sig << controller.getOutputDir().getFullPathName();

    if (sig != lastManifestSignature)
    {
        lastManifestSignature = sig;
        refresh();
    }
}

void BrowseTab::refresh()
{
    folderLabel.setText("Recordings in: " + controller.getOutputDir().getFullPathName(), juce::dontSendNotification);

    const auto search = matchText(searchBox);
    const int type = typeFilter.getSelectedId();
    auto& plugins = controller.getManifest().getPlugins();

    visible.clear();
    for (int i = 0; i < (int) plugins.size(); ++i)
    {
        const auto& p = plugins[(size_t) i];
        if ((type == 2 && ! p.isInstrument()) || (type == 3 && p.isInstrument()))
            continue;
        if (! visiblePresetIndices(p, search).empty() || textContains(p.company + " " + p.name, search))
            visible.push_back(i);
    }

    std::sort(visible.begin(), visible.end(), [&plugins](int a, int b)
    {
        const auto& pa = plugins[(size_t) a];
        const auto& pb = plugins[(size_t) b];
        const auto c = pa.company.compareIgnoreCase(pb.company);
        return c != 0 ? c < 0 : pa.name.compareIgnoreCase(pb.name) < 0;
    });

    pluginListBox.updateContent();

    int rowToSelect = -1;
    for (int r = 0; r < (int) visible.size(); ++r)
        if (plugins[(size_t) visible[(size_t) r]].id == selectedPluginId)
            rowToSelect = r;

    if (rowToSelect < 0 && ! visible.empty() && selectedPluginId.isEmpty())
        rowToSelect = 0;

    // Don't scroll: this runs whenever a recording run updates the manifest,
    // and yanking the list back to the selection while you browse is rude.
    if (rowToSelect >= 0)
        pluginListBox.selectRow(rowToSelect, true, true);
    else
        pluginListBox.deselectAllRows();

    pluginHeader.setText("Plugins (" + juce::String((int) visible.size()) + " shown of "
                         + juce::String((int) plugins.size()) + ")", juce::dontSendNotification);

    // The selected plugin's preset list may have changed under it (a run just
    // recorded it), even when the selection itself didn't.
    presetListBox.updateContent();
    presetListBox.repaint();
    updatePresetHeader();
    updateDetails();
    updateTransportButtons();
    pluginListBox.repaint();

    if (plugins.empty())
        waveform.emptyText = "No recordings in this folder yet - record some in the Record tab.";
    else
        waveform.emptyText = "Click a preset to hear it.";
}

void BrowseTab::showPlugin(const juce::String& pluginId)
{
    searchBox.clear();
    typeFilter.setSelectedId(1, juce::dontSendNotification);
    selectedPluginId = pluginId;
    refresh();
}

const PluginResult* BrowseTab::selectedPlugin() const
{
    for (auto& p : controller.getManifest().getPlugins())
        if (p.id == selectedPluginId)
            return &p;
    return nullptr;
}

const PresetResult* BrowseTab::selectedPreset() const
{
    auto* p = selectedPlugin();
    if (p == nullptr)
        return nullptr;

    const auto rows = visiblePresetIndices(*p, matchText(searchBox));
    const int row = presetListBox.getSelectedRow();
    return juce::isPositiveAndBelow(row, (int) rows.size()) ? &p->presets[(size_t) rows[(size_t) row]] : nullptr;
}

void BrowseTab::pluginSelectionChanged()
{
    const int row = pluginListBox.getSelectedRow();
    auto& plugins = controller.getManifest().getPlugins();
    const auto newId = juce::isPositiveAndBelow(row, (int) visible.size())
                     ? plugins[(size_t) visible[(size_t) row]].id : juce::String();

    if (newId != selectedPluginId)
    {
        selectedPluginId = newId;
        presetListBox.updateContent();
        presetListBox.deselectAllRows();
        presetListBox.scrollToEnsureRowIsOnscreen(0);
    }

    updatePresetHeader();
    presetListBox.repaint();
    updateDetails();
}

void BrowseTab::updatePresetHeader()
{
    auto* p = selectedPlugin();
    presetHeader.setText(p != nullptr ? "Presets of " + p->name + " (" + juce::String(p->countWithAudio())
                                            + " recorded of " + juce::String((int) p->presets.size()) + ")"
                                      : juce::String("Presets"),
                         juce::dontSendNotification);
}

void BrowseTab::presetSelectionChanged(bool play)
{
    updateDetails();
    if (play)
        playPreset(presetListBox.getSelectedRow());
}

void BrowseTab::playPreset(int row)
{
    auto* p = selectedPlugin();
    if (p == nullptr)
        return;

    const auto rows = visiblePresetIndices(*p, matchText(searchBox));
    if (! juce::isPositiveAndBelow(row, (int) rows.size()))
        return;

    const auto& preset = p->presets[(size_t) rows[(size_t) row]];
    if (! preset.hasAudio() || preset.file.isEmpty())
    {
        statusLabel.setText("\"" + preset.name + "\" has no recording (" + preset.status + ")",
                            juce::dontSendNotification);
        return;
    }

    juce::String error;
    if (player.play(controller.getManifest().getRoot().getChildFile(preset.file), error))
    {
        playingPluginId = p->id;
        playingPresetKey = preset.key;
        dryPlaying = false;
        statusLabel.setText("Playing " + p->company + " - " + p->name + " - " + preset.name, juce::dontSendNotification);
    }
    else
    {
        statusLabel.setText("Can't play: " + error, juce::dontSendNotification);
    }

    presetListBox.repaint();
}

void BrowseTab::updateTransportButtons()
{
    playButton.setButtonText(player.isPlaying() ? "Pause" : "Play");
    playButton.setTooltip(player.isPlaying() ? "Pause playback (Space)."
                                             : "Play the selected preset's recording (Space).");

    const bool hasDry = controller.getManifest().dryInputFile.isNotEmpty();
    dryButton.setEnabled(hasDry);
    dryButton.setTooltip(hasDry ? "Play the unprocessed song excerpt every effect was fed, to compare with an effect's recording."
                                : "Play the unprocessed effect input. (Disabled: no effect has been recorded into this folder yet.)");

    if (player.getDeviceError().isNotEmpty())
        statusLabel.setText("Audio device problem: " + player.getDeviceError() + " - see Audio settings.",
                            juce::dontSendNotification);
}

void BrowseTab::updateDetails()
{
    auto* p = selectedPlugin();
    const bool hasPlugin = p != nullptr;
    auto* preset = selectedPreset();

    revealPluginButton.setEnabled(hasPlugin && juce::File::isAbsolutePath(p->path) && juce::File(p->path).exists());
    copyPathButton.setEnabled(hasPlugin);
    revealRecordingButton.setEnabled(preset != nullptr && preset->hasAudio());

    revealPluginButton.setTooltip(revealPluginButton.isEnabled()
        ? "Show the plugin's file in Explorer."
        : (hasPlugin ? "Show the plugin's file in Explorer. (Disabled: the file isn't there any more.)"
                     : "Show the plugin's file in Explorer. (Disabled: select a plugin first.)"));
    copyPathButton.setTooltip(hasPlugin ? "Copy the plugin's full path to the clipboard."
                                        : "Copy the plugin's path. (Disabled: select a plugin first.)");
    revealRecordingButton.setTooltip(revealRecordingButton.isEnabled()
        ? "Show this preset's FLAC file in Explorer."
        : "Show the preset's FLAC file. (Disabled: select a preset that has a recording.)");

    if (! hasPlugin)
    {
        details.setText(controller.getManifest().getPlugins().empty()
                            ? "Nothing has been recorded into\n" + controller.getOutputDir().getFullPathName()
                              + "\nyet. Use the Record tab to record your plugins' presets."
                            : "Select a plugin on the left.");
        return;
    }

    int ok = 0, silent = 0, bad = 0, notRecorded = 0;
    for (auto& q : p->presets)
    {
        ok += q.status == "ok" ? 1 : 0;
        silent += q.status == "silent" ? 1 : 0;
        bad += (q.status == "failed" || q.status == "crashed" || q.status == "timeout") ? 1 : 0;
        notRecorded += (q.status == "not recorded" || q.status == "pending") ? 1 : 0;
    }

    juce::String t;
    t << p->name << "\n";
    t << "by " << (p->company.isNotEmpty() ? p->company : juce::String("unknown company"))
      << "  -  " << p->format << "  -  " << (p->isInstrument() ? "instrument" : "effect");
    if (p->version.isNotEmpty())
        t << "  -  version " << p->version;
    t << "\n";
    if (p->category.isNotEmpty())
        t << "Category: " << p->category << "\n";
    t << "\nPlugin file on disk:\n" << p->path << "\n\n";
    t << "Presets: " << ok << " recorded";
    if (silent > 0)      t << ", " << silent << " silent";
    if (bad > 0)         t << ", " << bad << " failed";
    if (notRecorded > 0) t << ", " << notRecorded << " not recorded";
    t << "\n";
    if (p->lastRun.isNotEmpty())
        t << "Last run: " << p->lastRun << "\n";
    if (p->error.isNotEmpty())
        t << "Problem: " << p->error << "\n";

    if (preset != nullptr)
    {
        t << "\n--- Preset ---\n" << preset->name << "\n";
        t << "Status: " << preset->status << "\n";
        t << "From: " << preset->source << "\n";
        if (preset->hasAudio())
        {
            t << "Recording: " << controller.getManifest().getRoot().getChildFile(preset->file).getFullPathName() << "\n";
            t << juce::String(preset->seconds, 1) << " s, peak " << juce::String(preset->peakDb, 1) << " dBFS";
            if (preset->recordedAt.isNotEmpty())
                t << ", recorded " << preset->recordedAt;
            t << "\n";
        }
        if (preset->note.isNotEmpty())
            t << "Note: " << preset->note << "\n";
    }

    details.setText(t);
}

//==============================================================================
int BrowseTab::PluginList::getNumRows()
{
    return (int) owner.visible.size();
}

void BrowseTab::PluginList::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (! juce::isPositiveAndBelow(row, (int) owner.visible.size()))
        return;

    const auto& p = owner.controller.getManifest().getPlugins()[(size_t) owner.visible[(size_t) row]];
    g.fillAll(selected ? Theme::rowSelected : (row % 2 != 0 ? Theme::rowAlt : Theme::panel));

    const int n = p.countWithAudio();
    auto area = juce::Rectangle<int>(8, 0, w - 14, h);

    g.setFont(Theme::font(12.0f));
    g.setColour(p.status == "failed" ? Theme::bad : (p.status == "recording" ? Theme::accent : Theme::dimText));
    const auto count = p.status == "failed" ? juce::String("failed")
                     : (p.status == "recording" ? juce::String("recording") : juce::String(n));
    g.drawText(count, area.removeFromRight(70), juce::Justification::centredRight);

    g.setColour(n > 0 ? Theme::text : Theme::dimText);
    g.setFont(Theme::font(13.5f));
    g.drawFittedText(p.company + " - " + p.name + (p.isInstrument() ? "" : "  (fx)"), area,
                     juce::Justification::centredLeft, 1);
}

void BrowseTab::PluginList::selectedRowsChanged(int)
{
    owner.pluginSelectionChanged();
}

juce::String BrowseTab::PluginList::getTooltipForRow(int row)
{
    if (! juce::isPositiveAndBelow(row, (int) owner.visible.size()))
        return {};
    const auto& p = owner.controller.getManifest().getPlugins()[(size_t) owner.visible[(size_t) row]];
    return p.path;
}

int BrowseTab::PresetList::getNumRows()
{
    auto* p = owner.selectedPlugin();
    return p != nullptr ? (int) visiblePresetIndices(*p, matchText(owner.searchBox)).size() : 0;
}

void BrowseTab::PresetList::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected)
{
    auto* p = owner.selectedPlugin();
    if (p == nullptr)
        return;

    const auto rows = visiblePresetIndices(*p, matchText(owner.searchBox));
    if (! juce::isPositiveAndBelow(row, (int) rows.size()))
        return;

    const auto& preset = p->presets[(size_t) rows[(size_t) row]];
    g.fillAll(selected ? Theme::rowSelected : (row % 2 != 0 ? Theme::rowAlt : Theme::panel));

    auto area = juce::Rectangle<int>(6, 0, w - 12, h);
    const bool playing = owner.playingPluginId == p->id && owner.playingPresetKey == preset.key
                         && owner.player.isPlaying();

    g.setColour(Theme::accent);
    g.setFont(Theme::font(12.0f));
    g.drawText(playing ? juce::String::fromUTF8("\xe2\x96\xb6") : juce::String(), area.removeFromLeft(16),
               juce::Justification::centredLeft);

    if (preset.status != "ok")
    {
        g.setColour(Theme::forStatus(preset.status));
        g.drawText(preset.status, area.removeFromRight(90), juce::Justification::centredRight);
    }

    g.setColour(preset.hasAudio() ? Theme::text : Theme::dimText);
    g.setFont(Theme::font(13.5f));
    g.drawFittedText(preset.name, area, juce::Justification::centredLeft, 1);
}

void BrowseTab::PresetList::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    // Clicking the preset that is already playing starts it again.
    auto* preset = owner.selectedPreset();
    if (preset != nullptr && owner.playingPresetKey == preset->key && owner.player.getPosition() > 0.3)
        owner.playPreset(row);
}

void BrowseTab::PresetList::selectedRowsChanged(int)
{
    owner.presetSelectionChanged(true);
}

void BrowseTab::PresetList::returnKeyPressed(int row)
{
    owner.playPreset(row);
}

juce::String BrowseTab::PresetList::getTooltipForRow(int row)
{
    auto* p = owner.selectedPlugin();
    if (p == nullptr)
        return {};

    const auto rows = visiblePresetIndices(*p, matchText(owner.searchBox));
    if (! juce::isPositiveAndBelow(row, (int) rows.size()))
        return {};

    const auto& preset = p->presets[(size_t) rows[(size_t) row]];
    return preset.note.isNotEmpty() ? preset.status + ": " + preset.note : preset.source;
}

} // namespace PresetRecorder

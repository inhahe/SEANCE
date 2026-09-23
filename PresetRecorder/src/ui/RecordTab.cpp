#include "RecordTab.h"
#include "Theme.h"
#include "../PreviewInput.h"
#include "../Util.h"
#include "cpp/src/dialog_helpers.h"
#include <thread>

namespace PresetRecorder {

static const juce::String kBusyNote = " (Disabled while a scan or recording is running - press Stop first.)";

static void setupNumber(juce::Slider& s, double min, double max, double step, const juce::String& suffix,
                        const juce::String& tooltip)
{
    s.setSliderStyle(juce::Slider::IncDecButtons);
    s.setIncDecButtonsMode(juce::Slider::incDecButtonsNotDraggable);
    s.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 70, 24);
    s.setRange(min, max, step);
    s.setTextValueSuffix(suffix);
    s.setTooltip(tooltip);
}

static void setupPathBox(juce::TextEditor& e)
{
    e.setReadOnly(true);
    e.setCaretVisible(false);
    e.setFont(Theme::font(13.0f));
    e.setColour(juce::TextEditor::backgroundColourId, Theme::panel);
}

//==============================================================================
RecordTab::RecordTab(Controller& c, Player& p) : controller(c), player(p)
{
    for (auto* h : { &sourcesHeader, &settingsHeader })
    {
        h->setFont(Theme::font(15.0f, true));
        addAndMakeVisible(*h);
    }
    sourcesHeader.setText("What to record, and where", juce::dontSendNotification);
    settingsHeader.setText("How to record", juce::dontSendNotification);

    // ---- SoundShop2 settings file -------------------------------------------------
    Theme::styleFormLabel(cfgLabel, "SoundShop2 plugin list");
    setupPathBox(cfgPath);
    cfgPath.setTooltip("SoundShop2's soundshop_plugins.cfg: its plugin folders ([ScanDirs]) and its skip list "
                       "([Blocked]). This tool records every plugin in those folders, except skip-listed ones.");
    cfgInfo.setFont(Theme::font(12.0f));
    cfgInfo.setColour(juce::Label::textColourId, Theme::dimText);
    cfgBrowse.setTooltip("Pick a different soundshop_plugins.cfg (SoundShop2 writes it in the folder it was started from).");
    cfgAuto.setTooltip("Look for soundshop_plugins.cfg automatically: in the SoundShop2 folder this tool lives in, "
                       "next to SEANCE.exe, and in the current folder. The newest one wins.");
    cfgFolders.setTooltip("List the plugin folders that will be searched.");
    cfgBrowse.onClick = [this] { chooseSeanceConfig(); };
    cfgAuto.onClick = [this] { controller.setSeanceConfigFile({}); };
    cfgFolders.onClick = [this] { showScanFolders(); };

    // ---- Output -----------------------------------------------------------------------
    Theme::styleFormLabel(outLabel, "Save recordings in");
    setupPathBox(outPath);
    outPath.setTooltip("Recordings go into an \"Instruments\" and an \"Effects\" folder in here, named "
                       "\"<company> - <plugin> - <preset>.flac\", with a manifest.json describing them.");
    outBrowse.setTooltip("Choose the folder the recordings are saved in.");
    outOpen.setTooltip("Open the recordings folder in Explorer.");
    outBrowse.onClick = [this] { chooseOutputDir(); };
    outOpen.onClick = [this]
    {
        const auto dir = controller.getOutputDir();
        dir.createDirectory();
        dir.startAsProcess();
    };

    // ---- Effect input -----------------------------------------------------------------
    Theme::styleFormLabel(songLabel, "Effect input song");
    setupPathBox(songPath);
    songPath.setTooltip("Effects need sound going into them: every effect preset processes the same few seconds "
                        "of this song. The bundled song is a public-domain big-band recording by the U.S. Air "
                        "Force Band (drums, bass, piano, horns - something for every kind of effect).");
    songInfo.setFont(Theme::font(12.0f));
    songInfo.setColour(juce::Label::textColourId, Theme::dimText);
    songBrowse.setTooltip("Use your own song or loop (WAV, AIFF, FLAC, Ogg or MP3).");
    songBundled.setTooltip("Go back to the bundled public-domain song.");
    songPreview.setTooltip("Hear exactly the excerpt the effects will be fed.");
    songDownload.setTooltip("The bundled song file is missing; download it again from Wikimedia Commons.");
    songBrowse.onClick = [this] { chooseSong(); };
    songBundled.onClick = [this]
    {
        controller.getSettings().setInputSongOverride({});
        controller.getSettings().setSongStart(AppSettings::defaultSongStart);
        controller.getSettings().setSongLength(AppSettings::defaultSongLength);
        loadSettingsIntoControls();
        updateSourceInfo();
    };
    songPreview.onClick = [this] { previewExcerpt(); };
    songDownload.onClick = [this]
    {
        songDownload.setEnabled(false);
        songInfo.setText("Downloading...", juce::dontSendNotification);
        controller.downloadBundledSong([this](bool, juce::String) { songDownload.setEnabled(true); updateSourceInfo(); });
    };

    Theme::styleFormLabel(excerptLabel, "Excerpt");
    Theme::styleFormLabel(songStartLabel, "starts at");
    Theme::styleFormLabel(songLengthLabel, "length");
    setupNumber(songStart, 0.0, 3600.0, 0.01, " s", "Where in the song the excerpt starts, in seconds. "
                "The default (207.55 s) is a dense full-band passage of the bundled song.");
    setupNumber(songLength, 0.5, 60.0, 0.5, " s", "How many seconds of the song each effect preset processes. "
                "A reverb or delay tail (up to the max tail) is recorded after it.");
    songStart.onValueChange = [this] { if (! loadingControls) controller.getSettings().setSongStart(songStart.getValue()); };
    songLength.onValueChange = [this] { if (! loadingControls) controller.getSettings().setSongLength(songLength.getValue()); };

    // ---- Instrument input ---------------------------------------------------------------
    Theme::styleFormLabel(instLabel, "Instruments play");
    phraseRadio.setRadioGroupId(1001);
    midiRadio.setRadioGroupId(1001);
    phraseRadio.setTooltip("A short built-in phrase: a held note, a quick arpeggio and a held chord (drum machines "
                           "get a two-bar General MIDI groove instead). About 3.5 s plus the release.");
    midiRadio.setTooltip("Play your own Standard MIDI File to every instrument instead (up to 60 s; program "
                         "changes in it are ignored).");
    phraseRadio.onClick = [this] { if (! loadingControls) controller.getSettings().setUseMidiFile(midiRadio.getToggleState()); };
    midiRadio.onClick = [this]
    {
        if (loadingControls)
            return;
        if (controller.getSettings().getMidiFile().isEmpty())
            chooseMidiFile();
        controller.getSettings().setUseMidiFile(midiRadio.getToggleState());
    };
    setupPathBox(midiPath);
    midiPath.setTextToShowWhenEmpty("(no MIDI file chosen)", Theme::dimText);
    midiBrowse.setTooltip("Choose a MIDI file for the instruments to play.");
    midiBrowse.onClick = [this] { chooseMidiFile(); };

    // ---- Render settings ----------------------------------------------------------------
    Theme::styleFormLabel(rateLabel, "Sample rate");
    Theme::styleFormLabel(bitsLabel, "Bit depth");
    Theme::styleFormLabel(tempoLabel, "Tempo");
    Theme::styleFormLabel(tailLabel, "Max tail");
    Theme::styleFormLabel(settleLabel, "Settle time");
    Theme::styleFormLabel(limitLabel, "Presets per plugin");
    Theme::styleFormLabel(workersLabel, "At a time");
    Theme::styleFormLabel(loadTimeoutLabel, "Load timeout");
    Theme::styleFormLabel(presetTimeoutLabel, "Preset timeout");

    for (int r : { 44100, 48000, 88200, 96000 })
        rateBox.addItem(juce::String(r) + " Hz", r);
    rateBox.setTooltip("Sample rate of the recordings (and of the plugins while recording).");
    bitsBox.addItem("16-bit", 16);
    bitsBox.addItem("24-bit", 24);
    bitsBox.setTooltip("FLAC bit depth. 16-bit is plenty for previews and makes files about a third smaller.");
    rateBox.onChange = [this] { storeRenderSettings(); };
    bitsBox.onChange = [this] { storeRenderSettings(); };

    setupNumber(tempo, 40.0, 240.0, 1.0, " BPM", "The tempo reported to plugins (beats per minute): tempo-synced "
                "delays, LFOs and arpeggiators follow it, and the built-in phrase is played at it. 130 matches "
                "the bundled song.");
    setupNumber(tail, 0.0, 20.0, 0.5, " s", "After the phrase or the song excerpt ends, keep recording until the "
                "sound dies away (below -70 dB), but for at most this many seconds - long enough for most "
                "reverbs and releases. Anything still ringing is faded out.");
    setupNumber(settle, 0.0, 5000.0, 50.0, " ms", "How long to wait after switching to each preset before recording, "
                "for plugins that load a preset's samples or settings in the background. Raise it if the first "
                "notes of recordings sound like the previous preset.");
    setupNumber(limit, 0.0, 100000.0, 1.0, "", "Record at most this many presets of each plugin (0 = all of them). "
                "Handy for a quick first pass over everything.");
    setupNumber(workers, 1.0, 16.0, 1.0, "", "How many plugins are recorded at the same time, each in its own "
                "process. More is faster on a many-core CPU but uses more memory.");
    setupNumber(loadTimeout, 10.0, 900.0, 5.0, " s", "A plugin that takes longer than this to load is stopped and "
                "marked as failed. Plugins waiting on an activation or registration dialog end up here too.");
    setupNumber(presetTimeout, 10.0, 900.0, 5.0, " s", "A single preset that takes longer than this to switch to and "
                "record is abandoned; recording continues with the plugin's next preset.");

    tempo.onValueChange = [this] { storeRenderSettings(); };
    tail.onValueChange = [this] { storeRenderSettings(); };
    settle.onValueChange = [this] { storeRenderSettings(); };
    limit.onValueChange = [this] { storeRenderSettings(); };
    workers.onValueChange = [this] { if (! loadingControls) controller.getSettings().setParallelWorkers((int) workers.getValue()); };
    loadTimeout.onValueChange = [this] { if (! loadingControls) controller.getSettings().setLoadTimeoutSeconds(loadTimeout.getValue()); };
    presetTimeout.onValueChange = [this] { if (! loadingControls) controller.getSettings().setPresetTimeoutSeconds(presetTimeout.getValue()); };

    keepExisting.setTooltip("Don't record presets that already have a recording in the output folder, so an "
                            "interrupted run can simply be started again. Untick to record everything afresh.");
    usePrograms.setTooltip("Record the presets the plugin lists itself (its factory presets / program list; for LV2, "
                           "its presets).");
    usePresetFiles.setTooltip("Also record the plugin's .vstpreset files from the standard VST3 preset folders "
                              "(Documents\\VST3 Presets, ProgramData\\VST3 Presets, ...), matched to the plugin by "
                              "its ID. Presets in a plugin's own format (e.g. .h2p, .fxp, .nki) can't be loaded "
                              "from outside the plugin.");
    for (auto* t : { &keepExisting, &usePrograms, &usePresetFiles })
        t->onClick = [this] { storeRenderSettings(); };

    // ---- Run ---------------------------------------------------------------------------
    scanButton.onClick = [this] { controller.scan(false); };
    rescanButton.onClick = [this] { controller.scan(true); };
    recordAllButton.onClick = [this] { controller.recordAll(); };
    recordSelectedButton.onClick = [this] { controller.recordPlugins(selectedPluginIds()); };
    stopButton.onClick = [this] { controller.stop(); };

    progressBar.setPercentageDisplay(true);
    headline.setFont(Theme::font(13.0f, true));
    workerInfo.setFont(Theme::font(12.0f));
    workerInfo.setColour(juce::Label::textColourId, Theme::dimText);
    workerInfo.setJustificationType(juce::Justification::topLeft);

    auto& header = table.getHeader();
    header.addColumn("Company", colCompany, 140, 60, 400);
    header.addColumn("Plugin", colPlugin, 200, 60, 500);
    header.addColumn("Type", colType, 80, 50, 120);
    header.addColumn("Format", colFormat, 60, 40, 100);
    header.addColumn("Status", colStatus, 250, 80, 800);
    header.addColumn("Path on disk", colPath, 420, 80, 3000);
    header.setSortColumnId(colCompany, true);
    table.setMultipleSelectionEnabled(true);
    table.setRowHeight(22);
    table.setColour(juce::ListBox::backgroundColourId, Theme::panel);

    logView.setMultiLine(true, false);
    logView.setReadOnly(true);
    logView.setCaretVisible(false);
    logView.setScrollbarsShown(true);
    logView.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.5f, juce::Font::plain)));
    logView.setColour(juce::TextEditor::backgroundColourId, Theme::panel.darker(0.2f));
    logView.setColour(juce::TextEditor::textColourId, Theme::text.withAlpha(0.85f));

    for (auto* comp : std::initializer_list<juce::Component*> {
             &cfgLabel, &cfgPath, &cfgBrowse, &cfgAuto, &cfgFolders, &cfgInfo,
             &outLabel, &outPath, &outBrowse, &outOpen,
             &songLabel, &songPath, &songBrowse, &songBundled, &songPreview, &songDownload, &songInfo,
             &excerptLabel, &songStartLabel, &songStart, &songLengthLabel, &songLength,
             &instLabel, &phraseRadio, &midiRadio, &midiPath, &midiBrowse,
             &rateLabel, &rateBox, &bitsLabel, &bitsBox, &tempoLabel, &tempo, &tailLabel, &tail,
             &settleLabel, &settle, &limitLabel, &limit, &workersLabel, &workers,
             &loadTimeoutLabel, &loadTimeout, &presetTimeoutLabel, &presetTimeout,
             &keepExisting, &usePrograms, &usePresetFiles,
             &scanButton, &rescanButton, &recordAllButton, &recordSelectedButton, &stopButton,
             &progressBar, &headline, &workerInfo, &table, &logView })
        addAndMakeVisible(comp);

    for (auto& line : controller.getLogLines())
        appendLog(line);

    loadSettingsIntoControls();
    updateSourceInfo();
    controller.addChangeListener(this);
    rebuildRows();
    updateEnablement();
    startTimerHz(4);
}

RecordTab::~RecordTab()
{
    controller.removeChangeListener(this);
}

void RecordTab::paint(juce::Graphics& g)
{
    g.fillAll(Theme::background);
}

//==============================================================================
void RecordTab::resized()
{
    auto area = getLocalBounds().reduced(12, 8);
    constexpr int rowH = 28, labelW = 150, gap = 6;

    auto formRow = [&](juce::Label& label) -> juce::Rectangle<int>
    {
        auto r = area.removeFromTop(rowH);
        area.removeFromTop(4);
        label.setBounds(r.removeFromLeft(labelW));
        r.removeFromLeft(gap);
        return r;
    };

    auto place = [](juce::Rectangle<int>& r, juce::Component& comp, int width, bool fromRight = false)
    {
        auto b = fromRight ? r.removeFromRight(width) : r.removeFromLeft(width);
        comp.setBounds(b.reduced(0, 1));
        if (fromRight) r.removeFromRight(6); else r.removeFromLeft(6);
    };

    sourcesHeader.setBounds(area.removeFromTop(24));

    auto r = formRow(cfgLabel);
    place(r, cfgFolders, 90, true);
    place(r, cfgAuto, 140, true);
    place(r, cfgBrowse, 90, true);
    cfgPath.setBounds(r.reduced(0, 1));
    r = area.removeFromTop(18);
    area.removeFromTop(4);
    cfgInfo.setBounds(r.withTrimmedLeft(labelW + gap));

    r = formRow(outLabel);
    place(r, outOpen, 70, true);
    place(r, outBrowse, 90, true);
    outPath.setBounds(r.reduced(0, 1));

    r = formRow(songLabel);
    place(r, songPreview, 120, true);
    place(r, songBundled, 130, true);
    place(r, songBrowse, 90, true);
    songPath.setBounds(r.reduced(0, 1));

    r = formRow(excerptLabel);
    place(r, songStartLabel, 60);
    place(r, songStart, 150);
    place(r, songLengthLabel, 50);
    place(r, songLength, 130);
    if (songDownload.isVisible())
        place(r, songDownload, 100);
    songInfo.setBounds(r);

    r = formRow(instLabel);
    place(r, phraseRadio, 130);
    place(r, midiRadio, 90);
    place(r, midiBrowse, 90, true);
    midiPath.setBounds(r.reduced(0, 1));

    area.removeFromTop(6);
    settingsHeader.setBounds(area.removeFromTop(24));

    auto pair = [&](juce::Rectangle<int>& row, juce::Label& label, juce::Component& comp, int labelWidth, int compWidth)
    {
        label.setBounds(row.removeFromLeft(labelWidth));
        row.removeFromLeft(4);
        comp.setBounds(row.removeFromLeft(compWidth).reduced(0, 1));
        row.removeFromLeft(14);
    };

    r = area.removeFromTop(rowH);
    area.removeFromTop(4);
    pair(r, rateLabel, rateBox, labelW, 110);
    pair(r, bitsLabel, bitsBox, 70, 90);
    pair(r, tempoLabel, tempo, 50, 150);
    pair(r, tailLabel, tail, 64, 130);
    pair(r, settleLabel, settle, 76, 150);

    r = area.removeFromTop(rowH);
    area.removeFromTop(4);
    pair(r, limitLabel, limit, labelW, 150);
    pair(r, workersLabel, workers, 70, 110);
    pair(r, loadTimeoutLabel, loadTimeout, 90, 130);
    pair(r, presetTimeoutLabel, presetTimeout, 100, 130);

    r = area.removeFromTop(rowH);
    area.removeFromTop(8);
    r.removeFromLeft(labelW + gap);
    place(r, keepExisting, 200);
    place(r, usePrograms, 230);
    place(r, usePresetFiles, 160);

    // Run controls.
    r = area.removeFromTop(32);
    area.removeFromTop(4);
    place(r, scanButton, 110);
    place(r, rescanButton, 100);
    place(r, recordAllButton, 110);
    place(r, recordSelectedButton, 130);
    place(r, stopButton, 80);
    progressBar.setBounds(r.reduced(0, 3));

    headline.setBounds(area.removeFromTop(20));
    workerInfo.setBounds(area.removeFromTop(34));
    area.removeFromTop(4);

    auto logArea = area.removeFromBottom(juce::jmax(110, area.getHeight() * 32 / 100));
    area.removeFromBottom(8);
    table.setBounds(area);
    logView.setBounds(logArea);
}

//==============================================================================
void RecordTab::loadSettingsIntoControls()
{
    const juce::ScopedValueSetter<bool> svs(loadingControls, true);
    auto& s = controller.getSettings();
    const auto render = s.getRenderSettings();

    songStart.setValue(s.getSongStart(), juce::dontSendNotification);
    songLength.setValue(s.getSongLength(), juce::dontSendNotification);
    (s.getUseMidiFile() ? midiRadio : phraseRadio).setToggleState(true, juce::dontSendNotification);
    midiPath.setText(s.getMidiFile(), false);

    rateBox.setSelectedId((int) render.sampleRate, juce::dontSendNotification);
    if (rateBox.getSelectedId() == 0)
        rateBox.setSelectedId(44100, juce::dontSendNotification);
    bitsBox.setSelectedId(render.bitDepth, juce::dontSendNotification);
    tempo.setValue(render.tempoBpm, juce::dontSendNotification);
    tail.setValue(render.maxTailSeconds, juce::dontSendNotification);
    settle.setValue(render.settleMs, juce::dontSendNotification);
    limit.setValue(render.maxPresetsPerPlugin, juce::dontSendNotification);
    keepExisting.setToggleState(render.skipExisting, juce::dontSendNotification);
    usePrograms.setToggleState(render.includePrograms, juce::dontSendNotification);
    usePresetFiles.setToggleState(render.includePresetFiles, juce::dontSendNotification);

    workers.setValue(s.getParallelWorkers(), juce::dontSendNotification);
    loadTimeout.setValue(s.getLoadTimeoutSeconds(), juce::dontSendNotification);
    presetTimeout.setValue(s.getPresetTimeoutSeconds(), juce::dontSendNotification);
}

void RecordTab::storeRenderSettings()
{
    if (loadingControls)
        return;

    auto render = controller.getSettings().getRenderSettings();
    render.sampleRate = rateBox.getSelectedId() > 0 ? (double) rateBox.getSelectedId() : 44100.0;
    render.bitDepth = bitsBox.getSelectedId() > 0 ? bitsBox.getSelectedId() : 16;
    render.tempoBpm = tempo.getValue();
    render.maxTailSeconds = tail.getValue();
    render.settleMs = settle.getValue();
    render.maxPresetsPerPlugin = (int) limit.getValue();
    render.skipExisting = keepExisting.getToggleState();
    render.includePrograms = usePrograms.getToggleState();
    render.includePresetFiles = usePresetFiles.getToggleState();
    controller.getSettings().setRenderSettings(render);
}

void RecordTab::updateSourceInfo()
{
    auto& cfg = controller.getCatalog().getSeanceConfig();

    cfgPath.setText(cfg.configFile != juce::File() ? cfg.configFile.getFullPathName()
                                                   : juce::String("(no soundshop_plugins.cfg found)"), false);
    cfgInfo.setText(cfg.summary() + (cfg.configFound ? (cfg.autoDetected ? "  -  found automatically" : "  -  chosen by you")
                                                     : juce::String()),
                    juce::dontSendNotification);
    cfgInfo.setColour(juce::Label::textColourId, cfg.configFound ? Theme::dimText : Theme::warn);

    outPath.setText(controller.getOutputDir().getFullPathName(), false);

    const auto song = controller.getEffectInputSong();
    const bool bundled = controller.isUsingBundledSong();
    songPath.setText((bundled ? "Bundled: " : "") + song.getFullPathName(), false);
    songBundled.setEnabled(! bundled && ! controller.isBusy());

    const bool exists = song.existsAsFile();
    if (exists && (cachedSongPath != song.getFullPathName()
                   || cachedSongStamp != song.getLastModificationTime().toMilliseconds()))
    {
        cachedSongPath = song.getFullPathName();
        cachedSongStamp = song.getLastModificationTime().toMilliseconds();
        cachedSongLength = getAudioFileLength(song);
    }
    const double cachedLength = cachedSongLength;

    const bool showDownload = bundled && ! exists;
    if (songDownload.isVisible() != showDownload)
    {
        songDownload.setVisible(showDownload);
        resized();
    }

    if (! exists)
    {
        songInfo.setText(bundled ? "The bundled song file is missing - download it, or choose another song."
                                 : "This file doesn't exist.",
                         juce::dontSendNotification);
        songInfo.setColour(juce::Label::textColourId, Theme::bad);
    }
    else if (cachedLength <= 0.0)
    {
        songInfo.setText("Can't decode this file.", juce::dontSendNotification);
        songInfo.setColour(juce::Label::textColourId, Theme::bad);
    }
    else
    {
        songInfo.setText("song length " + formatDuration(cachedLength)
                         + (bundled ? juce::String("  -  public domain (U.S. Air Force Band)") : juce::String()),
                         juce::dontSendNotification);
        songInfo.setColour(juce::Label::textColourId, Theme::dimText);

        const juce::ScopedValueSetter<bool> svs(loadingControls, true);
        songStart.setRange(0.0, juce::jmax(0.1, cachedLength - 0.5), 0.01);
    }

    songPreview.setEnabled(exists && cachedLength > 0.0);
    midiPath.setText(controller.getSettings().getMidiFile(), false);
}

//==============================================================================
void RecordTab::chooseSeanceConfig()
{
    auto start = controller.getCatalog().getSeanceConfig().configFile;
    chooser = std::make_unique<juce::FileChooser>("Choose SoundShop2's soundshop_plugins.cfg", start, "*.cfg", true, false, this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this](const juce::FileChooser& fc)
                         {
                             if (fc.getResult() != juce::File())
                                 controller.setSeanceConfigFile(fc.getResult().getFullPathName());
                         });
}

void RecordTab::chooseOutputDir()
{
    chooser = std::make_unique<juce::FileChooser>("Choose where to save the recordings", controller.getOutputDir(),
                                                  juce::String(), true, false, this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                         [this](const juce::FileChooser& fc)
                         {
                             if (fc.getResult() != juce::File())
                             {
                                 controller.setOutputDir(fc.getResult());
                                 updateSourceInfo();
                             }
                         });
}

void RecordTab::chooseSong()
{
    chooser = std::make_unique<juce::FileChooser>("Choose a song for the effects to process",
                                                  controller.getEffectInputSong(),
                                                  "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3", true, false, this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this](const juce::FileChooser& fc)
                         {
                             const auto f = fc.getResult();
                             if (f == juce::File())
                                 return;

                             auto& s = controller.getSettings();
                             s.setInputSongOverride(f.getFullPathName());

                             // Start a new song near a third of the way in, where most songs are in full swing.
                             const auto length = getAudioFileLength(f);
                             s.setSongStart(length > 20.0 ? std::floor(length / 3.0) : 0.0);
                             loadSettingsIntoControls();
                             updateSourceInfo();
                         });
}

void RecordTab::chooseMidiFile()
{
    const auto current = controller.getSettings().getMidiFile();
    chooser = std::make_unique<juce::FileChooser>("Choose a MIDI file for the instruments to play",
                                                  current.isNotEmpty() ? juce::File(current) : juce::File(),
                                                  "*.mid;*.midi", true, false, this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this](const juce::FileChooser& fc)
                         {
                             const auto f = fc.getResult();
                             if (f != juce::File())
                             {
                                 controller.getSettings().setMidiFile(f.getFullPathName());
                                 controller.getSettings().setUseMidiFile(true);
                             }
                             else if (controller.getSettings().getMidiFile().isEmpty())
                             {
                                 controller.getSettings().setUseMidiFile(false);
                             }
                             loadSettingsIntoControls();
                             updateSourceInfo();
                         });
}

void RecordTab::previewExcerpt()
{
    const auto song = controller.getEffectInputSong();
    const auto start = songStart.getValue(), length = songLength.getValue();
    const auto target = getSessionScratchDir().getChildFile("excerpt_preview.wav");
    songPreview.setEnabled(false);
    headline.setText("Decoding the excerpt...", juce::dontSendNotification);

    juce::Component::SafePointer<RecordTab> safe(this);
    std::thread([safe, song, start, length, target]
    {
        juce::AudioBuffer<float> excerpt;
        juce::String error;
        bool ok = prepareSongExcerpt(song, start, length, 44100.0, excerpt, error)
               && writeAudioFile(target, excerpt, 2, 44100.0, 16, error);

        juce::MessageManager::callAsync([safe, ok, error, target]
        {
            if (safe == nullptr)
                return;
            safe->songPreview.setEnabled(true);
            juce::String playError;
            if (! ok || ! safe->player.play(target, playError))
                safe->headline.setText("Can't preview the excerpt: " + (ok ? playError : error), juce::dontSendNotification);
            else
                safe->headline.setText("Playing the effect input excerpt (the Browse tab's Stop button stops it).",
                                       juce::dontSendNotification);
        });
    }).detach();
}

void RecordTab::showScanFolders()
{
    auto& catalog = controller.getCatalog();
    juce::String text;
    text << "Plugin folders from SoundShop2's settings:\n";
    for (auto& d : catalog.getSeanceConfig().scanDirs)
        text << "   " << d << (juce::File::isAbsolutePath(d) && juce::File(d).isDirectory() ? "" : "   (doesn't exist)") << "\n";

    text << "\nSearched too (each format's standard folders, as SoundShop2 does):\n";
    for (auto* format : catalog.getFormats().getFormats())
    {
        const auto defaults = format->getDefaultLocationsToSearch();
        for (int i = 0; i < defaults.getNumPaths(); ++i)
            text << "   " << format->getName() << ": " << defaults.getRawString(i) << "\n";
    }

    text << "\nVST2 plugins (.dll) are not included: SoundShop2 doesn't load VST2 either.";

    SoundShop::showAlertAsync(juce::MessageBoxOptions()
                                  .withIconType(juce::MessageBoxIconType::InfoIcon)
                                  .withTitle("Plugin folders")
                                  .withMessage(text)
                                  .withButton("OK"),
                              this);
}

//==============================================================================
void RecordTab::appendLog(const juce::String& line)
{
    if (++logLinesShown > 3000)
    {
        // Keep the view light: restart it from the controller's recent lines.
        logLinesShown = 0;
        logView.clear();
        const auto& all = controller.getLogLines();
        for (int i = juce::jmax(0, all.size() - 500); i < all.size(); ++i)
        {
            logView.moveCaretToEnd();
            logView.insertTextAtCaret(all[i] + "\n");
            ++logLinesShown;
        }
        return;
    }

    logView.moveCaretToEnd();
    logView.insertTextAtCaret(line + "\n");
}

void RecordTab::changeListenerCallback(juce::ChangeBroadcaster*)
{
    rowsDirty = true;
}

void RecordTab::timerCallback()
{
    const auto progress = controller.getProgress();
    progressValue = controller.isBusy() ? progress.fraction : 0.0;
    headline.setText(progress.headline, juce::dontSendNotification);
    workerInfo.setText(progress.workers.joinIntoString("\n"), juce::dontSendNotification);

    if (rowsDirty)
    {
        rowsDirty = false;
        rebuildRows();
        updateSourceInfo();
    }

    updateEnablement();
    table.repaint();
}

void RecordTab::updateEnablement()
{
    const bool busy = controller.isBusy();

    for (auto* comp : std::initializer_list<juce::Component*> {
             &cfgBrowse, &cfgAuto, &outBrowse, &songBrowse, &songPreview, &songStart, &songLength,
             &phraseRadio, &midiRadio, &midiBrowse, &rateBox, &bitsBox, &tempo, &tail, &settle, &limit,
             &workers, &loadTimeout, &presetTimeout, &keepExisting, &usePrograms, &usePresetFiles })
    {
        if (comp == &songPreview)
            continue; // follows the song's validity (updateSourceInfo)
        comp->setEnabled(! busy);
    }

    if (busy)
        songBundled.setEnabled(false);

    scanButton.setEnabled(! busy);
    rescanButton.setEnabled(! busy);
    recordAllButton.setEnabled(! busy);
    stopButton.setEnabled(busy);

    const auto selected = selectedPluginIds();
    recordSelectedButton.setEnabled(! busy && ! selected.isEmpty());

    scanButton.setTooltip("Find the plugins in SoundShop2's plugin folders. New or changed files are scanned, each in "
                          "a separate process so a crashing plugin can't take this tool down; unchanged files come "
                          "from the cache." + (busy ? kBusyNote : juce::String()));
    rescanButton.setTooltip("Forget the scan cache and scan every plugin file again." + (busy ? kBusyNote : juce::String()));
    recordAllButton.setTooltip("Record every preset of every plugin in the list (scanning first if needed). Skip-listed "
                               "plugins are left out unless you unskip them in the Skip List tab."
                               + (busy ? kBusyNote : juce::String()));
    recordSelectedButton.setTooltip("Record only the plugins selected in the table below (Ctrl/Shift-click to select several)."
                                    + (busy ? kBusyNote
                                            : (selected.isEmpty() ? " (Disabled: select one or more recordable plugins in the table.)"
                                                                  : juce::String())));
    stopButton.setTooltip(busy ? "Stop scanning/recording. Presets recorded so far are kept; start again later and the "
                                 "run carries on where it stopped."
                               : "Stop the current run. (Disabled: nothing is running.)");

    // Grey settings explain themselves.
    auto busyTip = [busy](juce::SettableTooltipClient& c, const juce::String& base)
    {
        c.setTooltip(base + (busy ? kBusyNote : juce::String()));
    };
    busyTip(cfgBrowse, "Pick a different soundshop_plugins.cfg (SoundShop2 writes it in the folder it was started from).");
    busyTip(outBrowse, "Choose the folder the recordings are saved in.");
    busyTip(songBrowse, "Use your own song or loop (WAV, AIFF, FLAC, Ogg or MP3).");
    busyTip(keepExisting, "Don't record presets that already have a recording in the output folder, so an interrupted "
                          "run can simply be started again. Untick to record everything afresh.");
}

//==============================================================================
void RecordTab::rebuildRows()
{
    // Remember the selection by plugin id / file.
    juce::StringArray selectedIds;
    for (int i = 0; i < table.getNumSelectedRows(); ++i)
    {
        const int row = table.getSelectedRow(i);
        if (juce::isPositiveAndBelow(row, (int) rows.size()))
            selectedIds.add(rows[(size_t) row].pluginId.isNotEmpty() ? rows[(size_t) row].pluginId
                                                                      : rows[(size_t) row].fileKey);
    }

    auto& catalog = controller.getCatalog();
    rows.clear();

    for (auto& p : catalog.getRecordablePlugins())
    {
        Row r;
        r.company = p.desc.manufacturerName;
        r.name = p.desc.name;
        r.type = p.instrument ? "Instrument" : "Effect";
        r.format = p.desc.pluginFormatName;
        r.path = p.desc.fileOrIdentifier;
        r.pluginId = p.id;
        r.fileKey = p.fileKey;
        r.recordable = true;
        rows.push_back(r);
    }

    for (auto& f : catalog.getFiles())
    {
        if (f.state == PluginFile::State::scanned && ! catalog.isSkipped(f))
            continue; // listed above, one row per plugin inside

        Row r;
        r.format = f.format;
        r.path = f.fileOrId;
        r.fileKey = f.key;
        r.name = PluginFileInfo::displayStem(f.fileOrId);

        switch (f.state)
        {
            case PluginFile::State::skipped:
            {
                const auto d = catalog.describeSkipEntry(f);
                r.company = d.company;
                r.name = d.displayName;
                r.fixedStatus = "skipped (on SoundShop2's skip list)";
                break;
            }
            case PluginFile::State::scanFailed: r.fixedStatus = "scan failed: " + f.error; break;
            case PluginFile::State::scanning:   r.fixedStatus = "scanning..."; break;
            case PluginFile::State::pending:    r.fixedStatus = "not scanned yet"; break;
            case PluginFile::State::scanned:    r.fixedStatus = "skipped"; break;
        }

        rows.push_back(r);
    }

    sortRows();
    table.updateContent();

    table.deselectAllRows();
    for (int i = 0; i < (int) rows.size(); ++i)
    {
        const auto& r = rows[(size_t) i];
        if (selectedIds.contains(r.pluginId.isNotEmpty() ? r.pluginId : r.fileKey))
            table.selectRow(i, true, false);
    }

    table.repaint();
}

void RecordTab::sortRows()
{
    const int column = sortColumn;
    const bool forwards = sortForwards;

    auto keyOf = [column](const Row& r) -> juce::String
    {
        switch (column)
        {
            case colPlugin: return r.name;
            case colType:   return r.type;
            case colFormat: return r.format;
            case colStatus: return r.fixedStatus;
            case colPath:   return r.path;
            default:        return r.company;
        }
    };

    std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b)
    {
        auto c = keyOf(a).compareNatural(keyOf(b));
        if (c == 0) c = a.company.compareNatural(b.company);
        if (c == 0) c = a.name.compareNatural(b.name);
        return forwards ? c < 0 : c > 0;
    });
}

juce::String RecordTab::statusFor(const Row& r) const
{
    return r.recordable ? controller.getLiveStatus(r.pluginId) : r.fixedStatus;
}

juce::StringArray RecordTab::selectedPluginIds() const
{
    juce::StringArray ids;
    for (int i = 0; i < table.getNumSelectedRows(); ++i)
    {
        const int row = table.getSelectedRow(i);
        if (juce::isPositiveAndBelow(row, (int) rows.size()) && rows[(size_t) row].recordable)
            ids.add(rows[(size_t) row].pluginId);
    }
    return ids;
}

int RecordTab::getNumRows()
{
    return (int) rows.size();
}

void RecordTab::paintRowBackground(juce::Graphics& g, int row, int, int, bool selected)
{
    g.fillAll(selected ? Theme::rowSelected : (row % 2 != 0 ? Theme::rowAlt : Theme::panel));
}

void RecordTab::paintCell(juce::Graphics& g, int row, int column, int w, int h, bool)
{
    if (! juce::isPositiveAndBelow(row, (int) rows.size()))
        return;

    const auto& r = rows[(size_t) row];
    juce::String text;
    auto colour = r.recordable ? Theme::text : Theme::dimText;

    switch (column)
    {
        case colCompany: text = r.company; break;
        case colPlugin:  text = r.name; break;
        case colType:    text = r.type; break;
        case colFormat:  text = r.format; break;
        case colStatus:  text = statusFor(r); colour = Theme::forStatus(text); break;
        case colPath:    text = r.path; break;
        default: break;
    }

    g.setColour(colour);
    g.setFont(Theme::font(13.0f));
    g.drawText(text, 6, 0, w - 10, h, juce::Justification::centredLeft, true);
}

void RecordTab::sortOrderChanged(int newSortColumnId, bool isForwards)
{
    sortColumn = newSortColumnId;
    sortForwards = isForwards;
    rebuildRows();
}

void RecordTab::cellClicked(int row, int, const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu() || ! juce::isPositiveAndBelow(row, (int) rows.size()))
        return;

    if (! table.isRowSelected(row))
        table.selectRow(row);

    const auto r = rows[(size_t) row];
    const bool busy = controller.isBusy();
    const bool hasRecordings = r.recordable && controller.getManifest().find(r.pluginId) != nullptr;
    const bool isFile = juce::File::isAbsolutePath(r.path) && juce::File(r.path).exists();

    juce::PopupMenu menu;
    menu.addSectionHeader(r.company.isNotEmpty() ? r.company + " - " + r.name : r.name);
    menu.addItem(1, "Record this plugin", r.recordable && ! busy);
    menu.addItem(2, "Show its recordings", hasRecordings);
    menu.addSeparator();
    menu.addItem(3, "Show in Explorer", isFile);
    menu.addItem(4, "Copy path");

    juce::Component::SafePointer<RecordTab> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(), [safe, r](int result)
    {
        if (safe == nullptr)
            return;
        switch (result)
        {
            case 1: safe->controller.recordPlugins({ r.pluginId }); break;
            case 2: if (safe->onShowRecordings) safe->onShowRecordings(r.pluginId); break;
            case 3: juce::File(r.path).revealToUser(); break;
            case 4: juce::SystemClipboard::copyTextToClipboard(r.path); break;
            default: break;
        }
    });
}

void RecordTab::cellDoubleClicked(int row, int, const juce::MouseEvent&)
{
    if (juce::isPositiveAndBelow(row, (int) rows.size()) && rows[(size_t) row].recordable
        && controller.getManifest().find(rows[(size_t) row].pluginId) != nullptr && onShowRecordings)
        onShowRecordings(rows[(size_t) row].pluginId);
}

void RecordTab::selectedRowsChanged(int)
{
    updateEnablement();
}

juce::String RecordTab::getCellTooltip(int row, int column)
{
    if (! juce::isPositiveAndBelow(row, (int) rows.size()))
        return {};

    const auto& r = rows[(size_t) row];
    if (column == colStatus)
        return statusFor(r);
    if (r.recordable)
        return r.path + "\nDouble-click to hear its recordings; right-click for more.";
    return r.path;
}

} // namespace PresetRecorder

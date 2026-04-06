#pragma once
#include "node_graph.h"
#include "node_graph_component.h"
#include "piano_roll_component.h"
#include "audio_engine.h"
#include "transport.h"
#include "plugin_host.h"
#include "plugin_settings.h"
#include "project_file.h"
#include "scripting.h"
#include "plugin_window.h"
#include "audio_export.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace SoundShop {

class MainContentComponent : public juce::Component,
                              public juce::MenuBarModel,
                              public juce::Timer {
public:
    MainContentComponent();
    ~MainContentComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override;
    bool keyPressed(const juce::KeyPress& key) override;

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int idx, const juce::String& name) override;
    void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

    // Transport
    void onPlay();
    void onStop();
    void onRecord();

    // File operations
    void newProject();
    void openProject();
    void saveProject();
    void saveProjectAs();
    void exportAudio();
    void importModFile();
    void doExportRender(const juce::File& file, const ExportOptions& opts, float maxBeat);
    void exportAudioWithBeat(float maxBeat);
    bool tryQuit(); // returns true if ok to quit

    NodeGraph graph;
    AudioEngine audioEngine;
    Transport transport;
    PluginSettings pluginSettings;

private:
    std::unique_ptr<juce::MenuBarComponent> menuBar;
    std::unique_ptr<NodeGraphComponent> graphComponent;
    std::unique_ptr<juce::StretchableLayoutResizerBar> splitter;

    // Transport bar components
    juce::TextButton playBtn{"Play"}, stopBtn{"Stop"}, recordBtn{"Play & Record"};
    juce::TextButton fitAllBtn{"Fit All"};
    juce::TextButton metroBtn{"Metro"};
    juce::TextButton loopBtn{"Loop"};
    juce::TextButton monitorBtn{"Mon"};
    juce::ComboBox timeSigCombo;
    juce::Label timeSigLabel;
    juce::Label positionLabel;
    juce::TextButton tapTempoBtn{"Tap"};
    std::vector<double> tapTimes;
    double lastTapTime = 0;
    juce::TextButton addMidiBtn{"+ MIDI Track"}, addAudioBtn{"+ Audio Track"};
    juce::Slider bpmSlider;
    juce::Label bpmLabel;

    // Editor panel (bottom)
    struct EditorPanel {
        int nodeId;
        std::unique_ptr<PianoRollComponent> component;
    };
    std::vector<std::unique_ptr<EditorPanel>> editorPanels;
    int editorPanelHeight = 250;
    void openEditor(Node& node);
    void closeEditor(int nodeId);
    void updateLayout();

    bool projectDirty = false;
    ScriptEngine scriptEngine;
    PluginWindowManager pluginWindows;
    void showScriptConsole();
    void showScriptConsoleForNode(int nodeId);
    void runScriptFile(const juce::String& path);
    void browseAndRunScript();
    void showPluginUI(int nodeId);
    void showPluginInfo(int nodeId);
    void showPluginPresets(int nodeId);
    void showMidiMap(int nodeId);
    void freezeNode(int nodeId);
    void syncCCMappingsToGraph();
    void syncCCMappingsFromGraph();
    int startupFrames = 5; // bring to front after this many timer ticks
    int saveFlashFrames = 0; // countdown for "Saved!" title flash
    void showPluginSettingsDialog();
    void showAudioDeviceSettings();

    // Recent projects
    std::vector<juce::String> recentProjects;
    bool autoLoadLastProject = true;
    void addToRecentProjects(const juce::String& path);
    void loadRecentProjects();
    void saveRecentProjects();
    void openProjectFile(const juce::String& path);
    void loadPreferences();
    void savePreferences();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainContentComponent)
};

class MainWindow : public juce::DocumentWindow {
public:
    MainWindow(const juce::String& name);
    ~MainWindow() override;
    void closeButtonPressed() override;
    void tryQuit();
    void saveWindowState();
    void restoreWindowState();

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};

} // namespace SoundShop

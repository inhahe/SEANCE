#pragma once
#include "node_graph.h"
#include "node_graph_component.h"
#include "piano_roll_component.h"
#include "audio_engine.h"
#include "transport.h"
#include "plugin_host.h"
#include "plugin_settings.h"
#include "routing_strip.h"
#include "hotkey_manager.h"
#include "project_file.h"
#include "scripting.h"
#include "plugin_window.h"
#include "audio_export.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <set>

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
    bool keyStateChanged(bool isKeyDown) override;

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
    void showMidiDeviceWizard();
    // Open a documentation file (relative path within docs/ folder, e.g.
    // "layers.html") in the OS's default browser. Resolves the path
    // relative to the exe's folder, which is where CMake POST_BUILD
    // copies the docs/ tree.
    void openHelpDoc(const juce::String& docRelativePath);
    void openProject();
    // onSaved fires after a successful save (sync if a current path exists,
    // async after the file chooser if not). Cancelled file chooser → never
    // fires. Used by tryQuit() to defer the actual app exit until the save
    // round-trip completes.
    void saveProject(std::function<void()> onSaved = {});
    void saveProjectAs(std::function<void()> onSaved = {});
    void exportAudio();
    void importModFile();
    void doExportRender(const juce::File& file, const ExportOptions& opts, float maxBeat);
    void exportAudioWithBeat(float maxBeat);
    bool tryQuit(); // returns true if ok to quit

    NodeGraph graph;
    AudioEngine audioEngine;
    Transport transport;
    PluginSettings pluginSettings;
    HotkeyManager hotkeyManager;

    void setupHotkeyCallbacks();
    void openHotkeySettings();

private:
    std::unique_ptr<juce::MenuBarComponent> menuBar;
    std::unique_ptr<NodeGraphComponent> graphComponent;
    std::unique_ptr<RoutingStrip> routingStrip;
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
    juce::TextButton captureBtn{"Capture"};
    juce::TextButton keyboardMidiBtn{"Keys"};

    // Computer keyboard → MIDI mapping
    int keyToMidiNote(int keyCode) const;
    bool handleKeyboardMidi(const juce::KeyPress& key, bool isDown);

    // Bounce: offline-render the entire project and create an Audio Timeline node.
    void bounceToAudioTrack();
    // Create audio track from the Output node's cache (instant — no re-render).
    void createAudioTrackFromOutputCache(Node& outputNode);

    // Real-time capture helpers (still available for arm-and-capture workflow)
    void saveCaptureToDisk(const juce::File& file);
    void createAudioTrackFromCapture();
    juce::Slider bpmSlider;
    juce::Label bpmLabel;

    // A single TooltipWindow owned by the main component is required for
    // juce::setTooltip() calls anywhere in the hierarchy to actually
    // display anything. Without this, every setTooltip call in the
    // codebase is silently dead code.
    juce::TooltipWindow tooltipWindow { this, 600 }; // 600ms show delay

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

    // Hotplug detection for MIDI input devices. The timer polls
    // MidiInput::getAvailableDevices() periodically; on seeing a new
    // identifier we offer to add it to the graph. Only fires after the
    // first scan completes so we don't nag about devices present at startup.
    std::set<std::string> previousMidiDeviceIds;
    int midiDeviceCheckCounter = 0;
    bool midiDeviceScanInitialized = false;
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

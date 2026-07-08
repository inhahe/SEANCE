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
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace SoundShop {

// Ephemeral-session mode (--ephemeral). When enabled, all crash-recovery /
// session-state files (autosave.ssp, autosave.meta.xml, undo-tree.dat, the
// per-plugin autosave-plugin-*.dat blobs) are redirected from the user's real
// app-data folder to an isolated throwaway directory that is wiped on every
// launch, and the "didn't shut down cleanly - recover?" prompt therefore never
// fires for these runs. Purpose: automated / test launches (e.g. opening the
// app to verify a feature, then killing the process) must not leave a stale
// autosave that makes the user's NEXT normal launch falsely report a crash. A
// genuine crash in a normal (non-ephemeral) launch still leaves the real
// autosave, so the recovery prompt continues to mean "something went wrong".
// Must be called before MainContentComponent is constructed. isEphemeralSession
// reflects the current state (false by default).
void setEphemeralSession(bool on);
bool isEphemeralSession();

// Optional startup project file. When a non-empty path is set (from a bare
// `.ssp` argument on the command line), MainContentComponent loads it at
// construction instead of the most-recently-opened project. Combine with
// --ephemeral to open a specific file in a throwaway session. Must be called
// before MainContentComponent is constructed.
void setStartupProjectFile(const juce::String& path);
juce::String startupProjectFile();

class MainContentComponent : public juce::Component,
                              public juce::MenuBarModel,
                              public juce::Timer {
public:
    MainContentComponent();
    ~MainContentComponent() override;

    // Delete the session-lock sentinel to record that this run exited cleanly,
    // so the next launch won't offer to recover a now-stale autosave. Called
    // from MainWindow::tryQuit at the single clean-quit chokepoint (public so
    // the owning MainWindow can invoke it).
    void markCleanShutdown();

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

    // Automation recording (see resolveArmMode / recordAutomationPoint in
    // node_graph.h). The actual per-tick point-writing lives in timerCallback();
    // these manage the pass lifecycle and gesture arming.
    void beginAutomationPass();  // play-start: arm every Write-resolved param
    void endAutomationPass();    // stop: simplify recorded lanes + one snapshot
    void handleParamGesture(int nodeId, int paramIdx, bool begin); // Touch/Latch arm
    void processPluginParamEvents(); // drain hosted-plugin knob events into rec state
    void syncAutoButton();       // refresh the transport Auto button label + colour
    bool recPrevPlaying = false; // edge-detect play<->stop transitions in timer

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
    // async after the file chooser if not). Cancelled file chooser -> never
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

    // Upgrade any legacy-format nodes in the graph in place. Shared by
    // startup autoload and openProjectFile so both paths pick up fixes
    // for old projects (e.g. stub "Reverb" nodes getting a script +
    // param set, MidiTimeline nodes missing MIDI In pins, etc.).
    void upgradeLegacyNodes();

private:
    std::unique_ptr<juce::MenuBarComponent> menuBar;
    std::unique_ptr<NodeGraphComponent> graphComponent;
    std::unique_ptr<RoutingStrip> routingStrip;
    std::unique_ptr<juce::StretchableLayoutResizerBar> splitter;

    // Transport bar components
    juce::TextButton playBtn{"Play"}, stopBtn{"Stop"}, recordBtn{"Play & Record"};
    juce::TextButton fitAllBtn{"Fit All"};
    juce::TextButton metroBtn{"Metronome"};
    juce::TextButton loopBtn{"Loop"};
    juce::TextButton autoBtn{"Auto: Off"}; // global automation-record mode (Off/Touch/Latch)
    juce::TextButton songBtn{"Song"};
    juce::TextButton monitorBtn{"Monitor"};
    juce::ComboBox timeSigCombo;
    juce::Label timeSigLabel;
    juce::Label positionLabel;
    juce::TextButton tapTempoBtn{"Tap"};
    std::vector<double> tapTimes;
    double lastTapTime = 0;
    juce::TextButton addMidiBtn{"+ MIDI Track"}, addAudioBtn{"+ Audio Track"};
    juce::TextButton captureBtn{"Capture"};
    juce::TextButton keyboardMidiBtn{"Keys"};

    // Computer keyboard -> MIDI mapping
    int keyToMidiNote(int keyCode) const;
    bool handleKeyboardMidi(const juce::KeyPress& key, bool isDown);

    // Bounce: offline-render the entire project and create an Audio Timeline node.
    void bounceToAudioTrack();
    // Create audio track from the Output node's cache (instant - no re-render).
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
        // Per-panel height in pixels. Each panel gets its own height so
        // the user can resize MIDI tracks independently by dragging the
        // strip at the top of each piano roll. editorPanelHeight (the
        // total stack height) is recomputed as the sum of these.
        int heightPx = 200;
    };
    std::vector<std::unique_ptr<EditorPanel>> editorPanels;
    int editorPanelHeight = 250;
    void openEditor(Node& node);
    void closeEditor(int nodeId);
    void updateLayout();
    // Resize callback wired into each PianoRollComponent's resize handle.
    // `deltaPx` is the pixel delta from the drag (positive = handle moved
    // down, i.e. shrink). The matching panel's heightPx is adjusted by
    // -deltaPx (drag UP = grow); panels above it keep their heights and
    // just shift position. Total editorPanelHeight tracks the sum.
    void resizeEditorPanel(int nodeId, int deltaPx);
    // Recompute editorPanelHeight as the sum of every panel's heightPx,
    // clamped to leave at least some graph area visible. Called whenever
    // a panel is added, removed, or resized.
    void recalcEditorPanelHeight();

    bool projectDirty = false;
    // The embedded CPython interpreter is process-global; share the one
    // ScriptEngine instance with the static-shape baker (shape_expr.cpp).
    ScriptEngine& scriptEngine = ScriptEngine::instance();
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
    // Batch freeze: render the graph ONCE and capture each listed node's own
    // output into its cache via a per-node tap. Freezing N nodes this way costs
    // one render pass, not N. freezeNode() is a thin wrapper for the 1-node
    // case. Nodes that don't exist or are the Output sink are skipped.
    void freezeNodes(const std::vector<int>& nodeIds);
    // After loading a project from `projectPath`, point the cache manager at
    // that project's soundshop_cache folder and re-attach each persisted freeze
    // to its on-disk PCM file (lazy: the audio thread loads it on first use).
    // Freezes whose cache file is missing are silently dropped so the node
    // renders live instead of playing silence.
    void rehydrateNodeCaches(const juce::String& projectPath);
    void syncCCMappingsToGraph();
    void syncCCMappingsFromGraph();

    // Heavy one-time startup init (audio device open + plugin instantiation /
    // state restore). Scheduled via callAsync off the window's first paint so
    // the window is on screen before the message thread blocks. The flag guards
    // it to fire exactly once.
    void runDeferredStartupInit();
    bool deferredInitScheduled = false;

    // ---- Async serial plugin loading (project open + startup) ----
    // Plugin instantiation (createPluginInstance + setStateInformation) is slow
    // and must run on the message thread (third-party plugins are not safe to
    // instantiate off-thread). To keep nodes visible and the UI responsive, we
    // don't load plugins on the synchronous load path any more. Instead, after a
    // project is parsed, every plugin node is marked Pending and pushed onto
    // pluginLoadQueue, then processNextPluginLoad() walks the queue one node per
    // callAsync tick: the node goes Loading, the heavy work runs, then it's
    // published Ready/Failed and the audio graph is rebuilt. projectLoading stays
    // true (greying out Save/Save As) until the queue drains.
    bool projectLoading = false;
    std::vector<int> pluginLoadQueue;     // node IDs still to load (FIFO)
    void beginAsyncPluginLoad();          // mark plugin nodes Pending, kick off the queue
    void processNextPluginLoad();         // load one queued plugin, then schedule the next

    int saveFlashFrames = 0; // countdown for "Saved!" title flash

    // Hotplug detection for MIDI input devices. The timer polls
    // MidiInput::getAvailableDevices() periodically; on seeing a new
    // identifier we offer to add it to the graph. Only fires after the
    // first scan completes so we don't nag about devices present at startup.
    std::set<std::string> previousMidiDeviceIds;
    int midiDeviceCheckCounter = 0;
    bool midiDeviceScanInitialized = false;
    void showPluginSettingsDialog();
    void showSongSettingsDialog();
    void showAssetLibraryDialog();
    void showAudioDeviceSettings();

    // Recent projects
    std::vector<juce::String> recentProjects;
    bool autoLoadLastProject = true;

    // View → "Auto-Fit Graph": keep the node graph continuously framed so the
    // whole graph is always visible. Persisted in soundshop_prefs.xml; the
    // NodeGraphComponent owns the live behaviour (a manual zoom/pan releases it
    // and calls back to untick this). Off by default - it's an opt-in mode.
    bool autoFitGraph = false;
    void addToRecentProjects(const juce::String& path);
    void loadRecentProjects();
    void saveRecentProjects();
    void openProjectFile(const juce::String& path);
    void loadPreferences();
    void savePreferences();

    // Autosave / crash recovery. A single rolling autosave file lives in the
    // user's app-data folder and gets rewritten every autosaveIntervalSeconds
    // while the project is dirty. On a clean save or "Don't Save" quit the
    // file is deleted. On startup, if the file still exists we offer to
    // recover it - meaning the app quit uncleanly (crash, power loss, kill).
    bool autosaveEnabled = true;
    int autosaveIntervalSeconds = 60;
    // juce::Time::getMillisecondCounterHiRes() snapshot of the last attempt.
    // 0 means "never attempted this session" - first tick defers by the full
    // interval so we don't autosave within the first second of opening.
    double lastAutosaveAttemptMs = 0.0;
    bool autosaveRecoveryOffered = false; // gate so we only prompt once
    bool autosaveLaptopNoticeShown = false; // first-launch laptop notice gate
    // Crash detection: true if a session-lock sentinel was already present at
    // startup, meaning the previous run never reached a clean shutdown. This is
    // what decides whether to offer autosave recovery - NOT the mere presence of
    // autosave.ssp (a normal idle session writes that file too). Captured in the
    // constructor before we (re)create our own lock.
    bool startupWasUncleanShutdown = false;
    void performAutosave();
    void discardAutosave();
    void tryRecoverAutosave(); // called after window is visible on startup
    // Capture whether a session lock was left over from a previous run (-> set
    // startupWasUncleanShutdown), then (re)create our own lock for this run.
    // Called once from the constructor.
    void setupSessionLock();

    // Slow autosave background worker (#86). Owns a single-slot mailbox
    // that the UI thread fills with the next save's content; the worker
    // pulls from the mailbox, writes to disk, and goes back to sleep.
    // Single worker + pending flag = no thread stacking by construction.
    //
    // The job model is a list of file writes. Each entry is one file
    // (full path + content). The worker walks the list and writes each
    // atomically via tmp+rename. Two kinds of writes happen:
    //   - The main autosave.ssp file (graph metadata + plugin state cache)
    //   - One per dirty plugin: autosave-plugin-{nodeId}.dat (raw base64)
    //
    // The slow tick decides which files to write based on what's dirty.
    // Plugin-only changes write only the affected plugin files. The main
    // autosave.ssp gets rewritten only when graph topology has changed
    // (signaled by graph.dirty / projectDirty), or on the first save.
    //
    // No compaction needed: each plugin file is overwritten in place
    // every time it gets saved, so there's never any stale data to
    // compact.
    struct AutosaveFileWrite {
        juce::File destFile;
        std::string content;
    };
    struct AutosaveJob {
        std::vector<AutosaveFileWrite> writes;
        std::string metaPath;       // original project path for sidecar
        std::string metaTimestamp;
        bool writeMeta = false;     // sidecar only refreshed on Full saves
    };
    void startAutosaveWorker();
    void stopAutosaveWorker();
    void enqueueAutosaveJob(AutosaveJob job); // hand off to worker
    void autosaveWorkerMain();                // worker thread entry point

    void quiesceAutosaveWorker();             // block until worker is idle + no pending job

    std::thread autosaveWorkerThread;
    std::mutex  autosaveWorkerMutex;
    std::condition_variable autosaveWorkerCv;     // signals the worker that a job is waiting
    std::condition_variable autosaveWorkerIdleCv; // signals callers that the worker went idle
    AutosaveJob autosaveWorkerPending;
    bool autosaveWorkerHasJob = false;
    bool autosaveWorkerBusy = false;          // true while the worker is mid-write (lock released)
    std::atomic<bool> autosaveWorkerStop {false};

    // Counts slow ticks since the last Full save of autosave.ssp. When
    // this hits kAutosaveTicksBetweenFullSaves, the next tick is forced
    // to be a Full save so the main file's graph metadata refreshes
    // periodically (catches topology changes between full saves).
    int autosaveTicksSinceFullSave = 0;
    static constexpr int kAutosaveTicksBetweenFullSaves = 12; // ~1 min @ 5s

    // Undo-tree persistence (#84). Set by the UndoTree::onTreeChanged
    // callback after every push/undo/redo. The next timerCallback tick
    // writes the tree to disk via writeUndoTreePersist() and clears the
    // flag, coalescing any number of mutations within one UI frame into
    // a single disk write.
    bool undoTreeDirty = false;
    void writeUndoTreePersist();    // serialize graph.undoTree to disk
    void discardUndoTreePersist();  // delete the on-disk undo tree
    void tryRestoreUndoTree();      // called once at startup, after the
                                    // graph state and audio engine are ready

    // Shared undo history sidecar (#90). When a project has an associated
    // sidecar file (graph.historyFilePath is set), writeUndoTreePersist
    // also updates it alongside the machine-local undo-tree.dat. Save-As
    // offers to create one on demand. Opening a project with an unseen
    // sidecar triggers the 3-option prompt (adopt / copy / ignore).
    //
    // Known-histories preferences: a per-user record of which decision
    // the user made for each project path, so repeat opens don't re-
    // prompt. Keyed by absolute project path.
    enum class HistoryDecision { NeverSeen, Adopted, Copied, Ignored };
    struct HistoryRecord {
        HistoryDecision decision = HistoryDecision::NeverSeen;
        std::string copyPath;   // populated when decision == Copied
    };
    std::unordered_map<std::string, HistoryRecord> knownHistories;
    void loadKnownHistories();
    void saveKnownHistories();
    HistoryRecord getHistoryDecision(const juce::String& projectPath) const;
    void recordHistoryDecision(const juce::String& projectPath,
                               HistoryDecision d,
                               const std::string& copyPath = {});

    // Runs after openProjectFile to check for a sidecar and prompt if
    // unseen. Takes the absolute path of the project file that was just
    // loaded so it can look up the sidecar's location and the user's
    // past decision.
    void handleSharedHistoryOnOpen(const juce::String& projectAbsPath);

    // After a successful Save As, optionally write a sidecar alongside
    // the .ssp file (if the user confirmed "include undo history"). The
    // function takes the same path the save was written to so it can
    // determine the sidecar location.
    void offerSharedHistoryOnSaveAs(const juce::String& savedProjectPath);

    // Write the current undo tree to an explicit sidecar path (used by
    // the save-as opt-in path and by the "use a copy" branch of the
    // open prompt). Updates graph.historyFilePath to the relative path
    // (relative to the project's .ssp location) so future writes go to
    // the same sidecar.
    void writeSharedHistorySidecar(const juce::File& sidecarFile);

    // Returns the absolute path of the sidecar associated with the
    // currently-loaded project, or an empty File if none. If the .ssp
    // file's historyFile field is set, uses that (resolved relative to
    // the .ssp). Otherwise returns empty.
    juce::File currentProjectSidecarFile() const;

    // After loading the autosave file (which sets each plugin's state via
    // [Node] pluginState= entries), apply any per-plugin override files
    // (autosave-plugin-{nodeId}.dat) on top. Per-plugin files are written
    // by the slow autosave's incremental path between full saves and
    // contain newer plugin state than what's in autosave.ssp.
    //
    // Walks all loaded plugin nodes, reads their override file if present,
    // and calls setStateInformation on the live plugin instance.
    void applyPerPluginOverrides();

    // Walk the autosave directory and delete any autosave-plugin-*.dat
    // files whose nodeId no longer corresponds to a node in the current
    // graph. Called after recovery and after Full saves so the directory
    // doesn't accumulate orphan files from removed plugins.
    void cleanupOrphanPluginFiles();

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

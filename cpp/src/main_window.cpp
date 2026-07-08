#include "main_window.h"
#include "dialog_helpers.h"
#include "asset_library_component.h"
#include "terrain_synth.h"
#include "builtin_synth.h"
#include "layered_wave_editor.h"
#include "spectral_editor.h"
#include "signal_eq_editor.h"
#include "wavelet_painter.h"
#include "trigger_node.h"
#include "midi_mod_node.h"
#include "midi_device_wizard.h"
#include "xy_pad.h"
#include "signal_shape_node.h"
#include "midi_script_node.h"
#include "spectrum_tap.h"
#include "analyzer_nodes.h"
#include "convolution_processor.h"
#include "convolution_editor.h"
#include "sampler_editor.h"
#include "multi_sampler.h"
#include "multi_sampler_editor.h"
#include "spectrum_visualizer.h"
#include "room_ir_capture.h"
#include "drum_synth.h"
#include "audio_export.h"
#include "mod_import.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <fstream>
#include <sstream>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace SoundShop {

// Shared "active/lit" accent for transport toggles. The Loop button uses it
// while looping is enabled, and the Play button uses it while audio is
// actually playing, so both light up the same recognisable blue (requested:
// Play and Loop share the active colour). Distinct from the green/red used by
// the metronome, monitor and keyboard-MIDI toggles. The resting (inactive)
// look is the LookAndFeel default, restored via removeColour rather than a
// hardcoded grey, so an un-lit transport button matches every other button.
static const juce::Colour kTransportLitColour(64, 132, 223);

// Returns true if this machine has a battery (laptop). JUCE 8.0.12 doesn't
// wrap battery detection cross-platform, so we touch the OS APIs directly:
// Win32 GetSystemPowerStatus on Windows, fall through to "no battery"
// (desktop) on macOS / Linux until #87 adds proper per-platform paths.
//
// On Windows, BatteryFlag bit 128 means "no system battery" (desktop).
// 255 means "unknown" - we treat that as desktop too, since assuming the
// aggressive interval is the friendlier default for unknown machines.
static bool machineHasBattery() {
#ifdef _WIN32
    SYSTEM_POWER_STATUS sps{};
    if (GetSystemPowerStatus(&sps))
        return sps.BatteryFlag != 128 && sps.BatteryFlag != 255;
    return false;
#else
    return false; // #87: implement IOKit (macOS) and UPower / sysfs (Linux)
#endif
}

// Returns true if the machine is currently on AC power. On desktops
// (no battery) this always returns true. On laptops, it reads the OS
// power state: true = plugged in, false = running on battery.
static bool isOnACPower() {
#ifdef _WIN32
    SYSTEM_POWER_STATUS sps{};
    if (GetSystemPowerStatus(&sps))
        return sps.ACLineStatus == 1; // 1 = AC online
    return true; // unknown -> assume desktop
#else
    return true; // #87: implement for macOS / Linux
#endif
}

// Pick the default autosave interval based on whether we're on a laptop.
// Desktop systems get the aggressive 5-second interval; laptops get a more
// conservative 20s default to save battery.
static int defaultAutosaveIntervalForThisMachine() {
    return machineHasBattery() ? 20 : 5;
}

// Returns true if a MIDI device name looks like a virtual or control-surface
// port that shouldn't be auto-created as a musical MidiInput node. These
// are drivers that install system-wide virtual ports for DAW control
// protocols, not for playing notes. Checked case-insensitively.
static bool isVirtualOrControlPort(const juce::String& name) {
    auto lower = name.toLowerCase();
    // SSL 360° control surface driver (UF8, UF1, UC1)
    if (lower.contains("ssl v-midi") || lower.contains("ssl vmidi")) return true;
    // Mackie/HUI control surface protocols
    if (lower.contains("mackie control") || lower.contains("hui")) return true;
    // Common virtual MIDI loopback drivers (user creates these intentionally
    // and can add them via the wizard - don't auto-create)
    if (lower.contains("loopmidi") || lower.contains("loop midi")) return true;
    // Windows built-in "Microsoft GS Wavetable Synth" (output only but
    // sometimes appears in input lists on some drivers)
    if (lower.contains("microsoft gs")) return true;
    // Avid/Pro Tools control surfaces
    if (lower.contains("eucon") || lower.contains("avid control")) return true;
    return false;
}

// ==============================================================================
// MainContentComponent
// ==============================================================================

MainContentComponent::MainContentComponent() {
    // Menu bar
    menuBar = std::make_unique<juce::MenuBarComponent>(this);
    addAndMakeVisible(menuBar.get());

    // Node graph
    graphComponent = std::make_unique<NodeGraphComponent>(graph);
    addAndMakeVisible(graphComponent.get());
    graphComponent->onNodeEdited = [this]() {
        audioEngine.getGraphProcessor().requestRebuild();
    };
    graphComponent->onNodeDeleted = [this](int nodeId) {
        closeEditor(nodeId);
    };
    // Auto-fit auto-released itself (the user manually zoomed/panned): mirror
    // the new state into our preference, persist it, and refresh the menu tick.
    graphComponent->onAutoFitViewChanged = [this](bool on) {
        autoFitGraph = on;
        savePreferences();
        menuItemsChanged();
    };
    graphComponent->getAudioFormat = [this]() {
        return std::make_pair(audioEngine.getSampleRate(),
                              audioEngine.getBlockSize());
    };
    graphComponent->getNodeLatencies = [this]() {
        return audioEngine.getGraphProcessor().snapshotNodeLatencies();
    };

    // Hotkey system: register callbacks and load saved bindings
    setupHotkeyCallbacks();

    // Routing strip: shared timeline for link gates, above piano roll panels
    routingStrip = std::make_unique<RoutingStrip>(graph, transport);
    addAndMakeVisible(routingStrip.get());

    // Transport buttons
    addAndMakeVisible(playBtn);
    addAndMakeVisible(stopBtn);
    addAndMakeVisible(recordBtn);
    addAndMakeVisible(addMidiBtn);
    addAndMakeVisible(addAudioBtn);
    addAndMakeVisible(fitAllBtn);
    addAndMakeVisible(metroBtn);
    addAndMakeVisible(captureBtn);

    // Play/Stop buttons skip tooltips - labels are self-explanatory and
    // universally understood. Keeping tooltips would just clutter the
    // hover layer over the most-used controls.
    // Stop is only meaningful while the song is actually playing; it starts
    // disabled (with an explanatory tooltip) and the timer enables it during
    // playback. See the timer callback for the live enable/tooltip update.
    stopBtn.setEnabled(false);
    stopBtn.setTooltip("Nothing is playing right now - press Play to start. "
                       "Stop then halts playback and rewinds.");
    recordBtn.setTooltip("Start playback while arming any tracks ready to record audio or MIDI input");
    fitAllBtn.setTooltip("Zoom and pan the node graph so every node fits in the visible area");
    metroBtn.setTooltip("Toggle the metronome click during playback and recording");
    captureBtn.setTooltip("Bounce the current Output node's audio to a new Audio Track. "
                          "Uses the cached output from the last playback if available, "
                          "otherwise re-renders the project offline.");
    playBtn.onClick = [this]() { onPlay(); };
    stopBtn.onClick = [this]() { onStop(); };
    recordBtn.onClick = [this]() { onRecord(); };
    // Set by findPlacement when the visible area was too crowded - tells the
    // caller to fitAll() after the new node has been added so the refit
    // includes it.
    auto needsFitAfterPlacement = std::make_shared<bool>(false);
    auto findPlacement = [this, needsFitAfterPlacement]() -> Vec2 {
        *needsFitAfterPlacement = false;
        // Place new tracks in the user's currently visible canvas rect.
        // If no empty slot fits there, fit-all to expand the view and then
        // place at the bottom of the (now wider) visible area.
        const float nodeW = 200, nodeH = 80, padX = 30, padY = 20;
        const float marginX = 40, marginY = 40;

        // Compute visible canvas rect from the graph component's screen size.
        auto screenW = (float)graphComponent->getWidth();
        auto screenH = (float)graphComponent->getHeight();
        auto tl = graphComponent->screenToCanvas({0.0f, 0.0f});
        auto br = graphComponent->screenToCanvas({screenW, screenH});

        // Collect existing timeline node positions for collision testing.
        std::vector<Vec2> taken;
        for (auto& n : graph.nodes)
            if (n.type == NodeType::MidiTimeline || n.type == NodeType::AudioTimeline)
                taken.push_back(n.pos);

        auto isOccupied = [&](float x, float y) {
            for (auto& t : taken)
                if (std::abs(t.x - x) < nodeW && std::abs(t.y - y) < nodeH)
                    return true;
            return false;
        };

        // Walk slots inside the visible rect, column by column.
        float startX = tl.x + marginX;
        float startY = tl.y + marginY;
        float endX   = br.x - marginX - nodeW;
        float endY   = br.y - marginY - nodeH;
        for (float x = startX; x <= endX; x += nodeW + padX) {
            for (float y = startY; y <= endY; y += nodeH + padY) {
                if (!isOccupied(x, y)) return {x, y};
            }
        }

        // No empty slot in the visible area. Place the new node just below
        // the bottom-most existing node, then signal the caller to fit-all
        // *after* the node is added so the refit includes it.
        float bottomMost = startY;
        for (auto& t : taken)
            if (t.y + nodeH > bottomMost) bottomMost = t.y + nodeH;
        *needsFitAfterPlacement = true;
        return {startX, bottomMost + padY};
    };

    addMidiBtn.onClick = [this, findPlacement, needsFitAfterPlacement]() {
        auto pos = findPlacement();
        auto& n = graph.addNode("MIDI Track", NodeType::MidiTimeline,
            {Pin{0, "MIDI In", PinKind::Midi, true}},
            {Pin{0, "MIDI", PinKind::Midi, false}}, pos);
        n.clips.push_back({"Clip 1", 0, 4, juce::Colours::cornflowerblue.getARGB()});
        if (*needsFitAfterPlacement) graphComponent->fitAll();
        graphComponent->repaint();
    };
    addAudioBtn.onClick = [this, findPlacement, needsFitAfterPlacement]() {
        auto pos = findPlacement();
        auto& n = graph.addNode("Audio Track", NodeType::AudioTimeline,
            {Pin{0, "Audio In", PinKind::Audio, true}},  // input pin for recording/monitoring
            {Pin{0, "Audio", PinKind::Audio, false}}, pos);
        n.clips.push_back({"Clip 1", 0, 4, juce::Colours::forestgreen.getARGB()});
        // Recording params
        n.params.push_back({"Input Channel", -1.0f, -1.0f, 31.0f}); // -1 = none, 0-31 = channel
        n.params.push_back({"Volume", 1.0f, 0.0f, 1.0f});
        n.params.push_back({"Pan", 0.0f, -1.0f, 1.0f});
        if (*needsFitAfterPlacement) graphComponent->fitAll();
        graphComponent->repaint();
    };
    fitAllBtn.onClick = [this]() { graphComponent->fitAll(); };

    addAndMakeVisible(keyboardMidiBtn);
    keyboardMidiBtn.setClickingTogglesState(true);
    keyboardMidiBtn.setTooltip("Use computer keyboard as MIDI piano (A-L = white keys, W-P = black keys, Z/X = octave)");
    keyboardMidiBtn.onClick = [this]() {
        audioEngine.keyboardMidiEnabled = keyboardMidiBtn.getToggleState();
        keyboardMidiBtn.setColour(juce::TextButton::buttonColourId,
            audioEngine.keyboardMidiEnabled.load()
                ? juce::Colour(50, 130, 80) : juce::Colour(55, 55, 60));
    };

    captureBtn.onClick = [this]() {
        // If currently playing, stop first - bouncing while the live graph
        // is running races on shared graph data and crashes.
        if (audioEngine.isPlaying()) {
            audioEngine.stop();
            transport.playing = false;
        }

        // If the Output node has a valid cache (populated automatically after
        // the last Play->Stop cycle), save it instantly. Otherwise fall back to
        // an offline bounce.
        Node* outNode = nullptr;
        for (auto& n : graph.nodes)
            if (n.type == NodeType::Output) { outNode = &n; break; }

        if (outNode && outNode->cache.valid && outNode->cache.numSamples > 0)
            createAudioTrackFromOutputCache(*outNode);
        else
            bounceToAudioTrack();
    };
    // Position display
    positionLabel.setText("0:00.0   Bar 1:1.0", juce::dontSendNotification);
    positionLabel.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 14.0f, 0));
    positionLabel.setColour(juce::Label::textColourId, juce::Colours::limegreen);
    positionLabel.setTooltip("Playback position / total song length, shown two ways:\n"
                             "  - left: elapsed time / total time as minutes:seconds "
                             "(e.g. 0:00.0/15:30.0)\n"
                             "  - right: musical position / total bars as Bar:Beat "
                             "(both 1-based, so the song starts at Bar 1, Beat 1.0)\n"
                             "The total is shown once the song has clips (or an explicit "
                             "length set via the Song button).");
    addAndMakeVisible(positionLabel);

    // Time signature
    timeSigLabel.setText("Time:", juce::dontSendNotification);
    addAndMakeVisible(timeSigLabel);
    addAndMakeVisible(timeSigCombo);
    timeSigCombo.setTooltip("Project time signature - affects bar length, the metronome accent pattern, and the snap grid");
    timeSigCombo.addItem("4/4", 1);
    timeSigCombo.addItem("3/4", 2);
    timeSigCombo.addItem("6/8", 3);
    timeSigCombo.addItem("2/4", 4);
    timeSigCombo.addItem("5/4", 5);
    timeSigCombo.addItem("7/8", 6);
    timeSigCombo.addItem("12/8", 7);
    timeSigCombo.addItem("2/2", 8);
    timeSigCombo.setSelectedItemIndex(0);
    timeSigCombo.onChange = [this]() {
        int nums[] = {4, 3, 6, 2, 5, 7, 12, 2};
        int dens[] = {4, 4, 8, 4, 4, 8, 8, 2};
        int idx = timeSigCombo.getSelectedItemIndex();
        if (idx >= 0 && idx < 8) {
            graph.timeSignatureNum = nums[idx];
            graph.timeSignatureDen = dens[idx];
        }
    };

    addAndMakeVisible(loopBtn);
    loopBtn.setTooltip("Toggle loop playback. When enabled, playback wraps around between the loop start and end "
                       "(initially set to the full project length - drag the loop region in the routing strip to adjust).");
    loopBtn.onClick = [this]() {
        if (!graph.loopEnabled) {
            // Enable loop: default to full project length
            float maxBeat = 0;
            for (auto& n : graph.nodes)
                for (auto& c : n.clips)
                    maxBeat = std::max(maxBeat, c.startBeat + c.lengthBeats);
            if (maxBeat <= 0) maxBeat = 4;
            graph.loopStartBeat = 0;
            graph.loopEndBeat = maxBeat;
            graph.loopEnabled = true;
        } else {
            graph.loopEnabled = false;
        }
        if (graph.loopEnabled)
            loopBtn.setColour(juce::TextButton::buttonColourId, kTransportLitColour);
        else
            loopBtn.removeColour(juce::TextButton::buttonColourId);
    };

    addAndMakeVisible(autoBtn);
    autoBtn.setTooltip(
        "Automation recording (the global switch). Click to cycle Off / Touch / Latch:\n"
        "  \x95 Off - knobs are manual; existing automation still plays back.\n"
        "  \x95 Touch - while playing, a knob you drag records ONLY while you hold it.\n"
        "  \x95 Latch - while playing, once you grab a knob it keeps recording your\n"
        "     value until you press Stop.\n"
        "Recording only happens while the transport is playing. Per-node and\n"
        "per-param overrides (right-click a node or a param) can force Off, a\n"
        "different mode, or Write (overwrite the whole pass) for specific targets.\n"
        "To lock the exact sound instead, use Freeze. See Help for details.");
    autoBtn.onClick = [this]() {
        // Cycle Off -> Touch -> Latch -> Off. Write is deliberately NOT reachable
        // here (node/param scope only) so no single switch can flatten every lane.
        switch (graph.autoArmGlobal) {
            case AutoArmMode::Off:   graph.autoArmGlobal = AutoArmMode::Touch; break;
            case AutoArmMode::Touch: graph.autoArmGlobal = AutoArmMode::Latch; break;
            default:                 graph.autoArmGlobal = AutoArmMode::Off;   break;
        }
        // If we armed mid-playback, catch up: arm any now-Write-resolved params
        // (Touch/Latch still wait for a gesture). Harmless when stopped.
        if (transport.playing) beginAutomationPass();
        syncAutoButton();
    };
    syncAutoButton();

    addAndMakeVisible(songBtn);
    songBtn.setTooltip("Project-wide Song Length and Repeat settings. "
                       "Song Length is where playback auto-stops (in beats); 0 means no explicit end. "
                       "Repeat mode chooses between None (stop at end), Forever (loop until Stop), "
                       "or N times (loop a fixed number of times then stop). Tracker imports with "
                       "whole-song loops use this to preserve the original's looping behavior.");
    songBtn.onClick = [this]() { showSongSettingsDialog(); };

    addAndMakeVisible(monitorBtn);
    monitorBtn.setTooltip("Toggle input monitoring - when on, audio coming in from any input device is "
                          "routed straight through to the Output node so you can hear yourself in real time");
    monitorBtn.onClick = [this]() {
        bool on = !audioEngine.inputMonitoring.load();
        audioEngine.inputMonitoring.store(on);
        monitorBtn.setColour(juce::TextButton::buttonColourId,
            on ? juce::Colour(120, 60, 60) : juce::Colour(55, 55, 60));
    };

    metroBtn.onClick = [this]() {
        graph.metronomeEnabled = !graph.metronomeEnabled;
        metroBtn.setColour(juce::TextButton::buttonColourId,
            graph.metronomeEnabled ? juce::Colour(80, 120, 60) : juce::Colour(55, 55, 60));
    };

    // BPM
    bpmLabel.setText("BPM:", juce::dontSendNotification);
    addAndMakeVisible(bpmLabel);
    bpmSlider.setRange(20, 999, 1);
    bpmSlider.setValue(120);
    bpmSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 50, 20);
    bpmSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    bpmSlider.setTooltip("Project tempo in beats per minute. Higher = faster. "
                         "Typical pop/rock is 90-130; dance music 120-140; ballads 60-80.");
    addAndMakeVisible(bpmSlider);
    bpmSlider.onValueChange = [this]() { graph.bpm = (float)bpmSlider.getValue(); };

    // Tap tempo
    addAndMakeVisible(tapTempoBtn);
    tapTempoBtn.setTooltip("Click repeatedly in time with a beat to set the BPM. "
                           "Averages the last few clicks; resets if you wait more than 2 seconds between clicks.");
    tapTempoBtn.onClick = [this]() {
        double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
        // Reset if more than 2 seconds since last tap
        if (now - lastTapTime > 2.0)
            tapTimes.clear();
        tapTimes.push_back(now);
        lastTapTime = now;
        // Need at least 2 taps to calculate
        if (tapTimes.size() >= 2) {
            // Average the intervals
            double totalInterval = 0;
            int count = 0;
            // Use last 8 taps max
            int start = std::max(0, (int)tapTimes.size() - 8);
            for (int i = start + 1; i < (int)tapTimes.size(); ++i) {
                totalInterval += tapTimes[i] - tapTimes[i - 1];
                count++;
            }
            double avgInterval = totalInterval / count;
            double bpm = 60.0 / avgInterval;
            bpm = juce::jlimit(20.0, 999.0, bpm);
            graph.bpm = (float)bpm;
            bpmSlider.setValue(bpm, juce::dontSendNotification);
        }
    };

    // Wire up graph callback
    graphComponent->onOpenEditor = [this](Node& node) { openEditor(node); };
    graphComponent->onShowPluginUI = [this](int nodeId) { showPluginUI(nodeId); };
    graphComponent->onShowPluginInfo = [this](int nodeId) { showPluginInfo(nodeId); };
    graphComponent->onShowPluginPresets = [this](int nodeId) { showPluginPresets(nodeId); };
    graphComponent->onShowMidiMap = [this](int nodeId) { showMidiMap(nodeId); };
    graphComponent->onFreezeNode = [this](int nodeId) { freezeNode(nodeId); };
    graphComponent->onFreezeArmedNodes = [this]() {
        std::vector<int> armed;
        for (auto& n : graph.nodes) if (n.armedForFreeze) armed.push_back(n.id);
        if (!armed.empty()) freezeNodes(armed);
    };
    graphComponent->onParamGesture = [this](int nodeId, int paramIdx, bool begin) {
        handleParamGesture(nodeId, paramIdx, begin);
    };
    graphComponent->onRunScript = [this](int nodeId) { showScriptConsoleForNode(nodeId); };
    graphComponent->onOpenHelpDoc = [this](juce::String rel) { openHelpDoc(rel); };
    graphComponent->onSignalShapeManualTrigger = [this](int nodeId) {
        if (auto* proc = dynamic_cast<SignalShapeProcessor*>(
                audioEngine.getGraphProcessor().getProcessorForNode(nodeId)))
            proc->fireManualTrigger();
    };
    graphComponent->getNodeScriptError = [this](int nodeId) -> bool {
        auto* p = audioEngine.getGraphProcessor().getProcessorForNode(nodeId);
        if (auto* ms = dynamic_cast<MidiScriptProcessor*>(p))
            return ms->hasScriptError();
        if (auto* ss = dynamic_cast<SignalShapeProcessor*>(p))
            return ss->hasScriptError();
        return false;
    };

    // Load prefs, plugin cache, recent projects (audio engine deferred to timer)
    pluginSettings.load("soundshop_plugins.cfg");
    loadRecentProjects();
    loadPreferences();
    if (graphComponent) graphComponent->setAutoFitView(autoFitGraph);
    loadKnownHistories();

    // Load last project or set up default graph
    bool loaded = false;
    // A bare `.ssp` path on the command line overrides the auto-load-last
    // behaviour: load exactly that file (used for `SEANCE.exe foo.ssp` and for
    // opening a known project into an --ephemeral test session).
    if (juce::String startup = startupProjectFile(); startup.isNotEmpty()) {
        auto file = juce::File(startup);
        if (file.existsAsFile()) {
            {
                std::lock_guard<std::recursive_mutex> graphLk(graph.mutationLock);
                ProjectFile::load(startup.toStdString(), graph, nullptr);
                rehydrateNodeCaches(startup);
            }
            if (graphComponent) graphComponent->notifyProjectLoaded();
            loaded = true;
        }
    }
    if (!loaded && autoLoadLastProject && !recentProjects.empty()) {
        auto file = juce::File(recentProjects[0]);
        if (file.existsAsFile()) {
            // Lock for the batch mutation (see node_graph.h mutationLock
            // comment). Constructor-time load typically runs before the
            // audio device callback fires, but locking unconditionally
            // makes the invariant "all batch graph mutations hold this
            // lock" hold even if the device starts unusually early.
            {
                std::lock_guard<std::recursive_mutex> graphLk(graph.mutationLock);
                ProjectFile::load(recentProjects[0].toStdString(), graph, nullptr);
                rehydrateNodeCaches(recentProjects[0]);
            }
            // Re-apply the saved pan/zoom from the loaded graph (or fit-all
            // if none was persisted) on the next paint.
            if (graphComponent) graphComponent->notifyProjectLoaded();
            loaded = true;
        }
    }
    if (!loaded) {
        graph.setupDefaultGraph();
        // Auto-create MidiInput nodes for all connected hardware devices
        // so a fresh install sees the user's controller immediately.
        auto devices = juce::MidiInput::getAvailableDevices();
        // Resolve the default track's MIDI input pin ID up front - pin IDs are
        // stable across addNode() reallocations, so we never hold a Node*/Pin*
        // across the addNode() calls below (dangling-reference anti-pattern).
        int defaultTrackMidiPinId = -1;
        for (auto& n : graph.nodes)
            if (n.type == NodeType::MidiTimeline) {
                for (auto& pin : n.pinsIn)
                    if (pin.kind == PinKind::Midi) { defaultTrackMidiPinId = pin.id; break; }
                break;
            }
        float yPos = 200;
        for (auto& dev : devices) {
            if (isVirtualOrControlPort(dev.name)) continue;
            bool exists = false;
            for (auto& n : graph.nodes)
                if (n.type == NodeType::MidiInput && n.midiInputSourceId == dev.identifier.toStdString())
                    { exists = true; break; }
            if (exists) continue;
            auto& n = graph.addNode(dev.name.toStdString(), NodeType::MidiInput,
                {}, {Pin{0, "MIDI Out", PinKind::Midi, false}}, {80, yPos});
            n.midiInputSourceId = dev.identifier.toStdString();
            int outPinId = n.pinsOut.empty() ? -1 : n.pinsOut[0].id;
            if (defaultTrackMidiPinId >= 0 && outPinId >= 0)
                graph.addLink(outPinId, defaultTrackMidiPinId);
            yPos += 50;
        }
    } else {
        upgradeLegacyNodes();
    }

    audioEngine.setGraph(&graph, &transport);

    // Per-plugin dirty tracking (#86): when AutomationManager pushes a
    // value into a plugin parameter, mark that node's plugin state cache
    // stale so the next slow autosave re-queries getStateInformation.
    // Only fires from the message-thread automation path; processMidiCC
    // (audio thread) does NOT call this - the periodic force-dirty pass
    // catches changes that route through MIDI CC mappings.
    audioEngine.getGraphProcessor().getAutomation().onPluginParamChanged =
        [this](int nodeId) {
            if (auto* n = graph.findNode(nodeId))
                n->pluginStateDirty = true;
        };

    // Crash detection (must run before tryRecoverAutosave is scheduled and
    // before the autosave worker can touch the dir): note whether a session
    // lock survived from a previous run, then drop a fresh lock for this run.
    setupSessionLock();

    // Background worker for slow autosave (#86). Runs the disk write
    // off the UI thread so larger projects don't hiccup during the save.
    startAutosaveWorker();

    // Wire the snapshot-based undo system (#84). Three pieces:
    //  1. onLoadSnapshot - when undo/redo lands on a step that has no
    //     LambdaCommand (a snapshot-only step from commitSnapshot, or any
    //     step in a session restored from disk where the closures are gone),
    //     parse the snapshot back into the live graph and rebuild routing.
    //  2. onTreeChanged - fires after every push/undo/redo. Used to lazily
    //     fill in the snapshot text for steps pushed via exec()/pushDone()
    //     (LambdaCommand path) so they're persistable cross-session.
    //  3. setRootSnapshot below - capture the initial state so the very
    //     first edit has a state to revert to.
    graph.undoTree.onLoadSnapshot = [this](const std::string& snap) {
        // Hold the graph mutation lock for the snapshot reparse: it clears
        // graph.nodes/links and rebuilds them from the snapshot text, which
        // is the same kind of batch mutation as MOD import. Same race risk
        // (see mutationLock comment in node_graph.h), same fix.
        {
            std::lock_guard<std::recursive_mutex> graphLk(graph.mutationLock);
            ProjectFile::loadFromString(snap, graph, nullptr);
        }
        // Drop editor panels whose underlying node no longer exists in the
        // restored state. Surviving panels keep their state and just
        // re-render the new node data.
        editorPanels.erase(
            std::remove_if(editorPanels.begin(), editorPanels.end(),
                [this](const std::unique_ptr<EditorPanel>& p) {
                    return graph.findNode(p->nodeId) == nullptr;
                }),
            editorPanels.end());
        // Refresh any open wavetable editor windows so they re-decode their
        // doc from the restored node->script. Without this, an undo/redo
        // (including one capturing a mic/file capture into the library) would
        // leave the open editor showing its stale pre-undo doc.
        LayeredWaveEditorComponent::reloadOpenEditorsAfterSnapshot(graph);
        audioEngine.getGraphProcessor().requestRebuild();
        if (graphComponent) graphComponent->repaint();
        for (auto& panel : editorPanels)
            if (panel->component) panel->component->repaint();
    };
    graph.undoTree.onTreeChanged = [this]() {
        // Lazy-fill: any step pushed without a snapshot gets one captured
        // from the post-state right now. Cheap (graph-only serializer,
        // typically <10 ms) and only fires when the current snapshot slot
        // is actually empty, so undo/redo navigation skips this entirely.
        if (graph.undoTree.currentSnapshotIsEmpty())
            graph.undoTree.setCurrentSnapshot(ProjectFile::serializeForUndo(graph));
        // Mark for persistence; the next timer tick writes the full tree
        // to disk. Coalesces any number of mutations within one UI frame
        // into a single disk write.
        undoTreeDirty = true;
    };
    // Initial state for the root undo node - without this, undoing the
    // first user edit has nothing to revert to.
    graph.undoTree.setRootSnapshot(ProjectFile::serializeForUndo(graph));

    // Timer for UI updates
    startTimerHz(30);

    setSize(1440, 900);

    // If a stale autosave file exists from a previous crash, offer to
    // recover it. Deferred via callAsync so the dialog opens on top of the
    // already-visible main window rather than blocking the constructor.
    juce::Component::SafePointer<MainContentComponent> safe(this);
    juce::MessageManager::callAsync([safe]() {
        if (safe) safe->tryRecoverAutosave();
    });

    // Shared-history prompt for the auto-loaded startup project (#90).
    // openProjectFile already fires this for user-opened projects, but
    // the autoload path bypasses it - so do it here. Deferred so the
    // dialog appears over the visible main window.
    if (loaded && !ProjectFile::currentPath.empty()) {
        juce::String startupPath = juce::String(ProjectFile::currentPath);
        juce::Component::SafePointer<MainContentComponent> safe3(this);
        juce::MessageManager::callAsync([safe3, startupPath]() {
            if (safe3) safe3->handleSharedHistoryOnOpen(startupPath);
        });
    }

    // Laptop autosave notice (#86): on first launch on a laptop, tell the
    // user we picked a slower default to save battery and where to change
    // it. Only shows once - saved in prefs as autosaveLaptopNoticeShown.
    if (!autosaveLaptopNoticeShown && machineHasBattery()) {
        juce::Component::SafePointer<MainContentComponent> safe2(this);
        int interval = autosaveIntervalSeconds;
        juce::MessageManager::callAsync([safe2, interval]() {
            if (!safe2) return;
            juce::AlertWindow::showAsync(
                juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::InfoIcon)
                    .withTitle("Autosave on Laptop")
                    .withMessage(
                        "Detected a laptop. Autosave is set to " + juce::String(interval) +
                        " seconds to save battery - frequent disk writes can wake the SSD "
                        "and shorten unplugged runtime.\n\n"
                        "Crash recovery still loses at most a few plugin tweaks. Graph "
                        "edits (notes, cables, parameters) are protected at gesture "
                        "granularity by a separate, much faster channel.\n\n"
                        "You can change the interval (or turn autosave off entirely) "
                        "later from the Options menu.")
                    .withButton("OK"),
                [safe2](int) {
                    if (!safe2) return;
                    safe2->autosaveLaptopNoticeShown = true;
                    safe2->savePreferences();
                });
        });
    }
}

MainContentComponent::~MainContentComponent() {
    stopAutosaveWorker();
    audioEngine.shutdown();
}

void MainContentComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(30, 30, 35));

    // First-paint trigger for the heavy startup init (audio device + plugin
    // instantiation/state-restore). We defer it off the *first* paint rather
    // than doing it in the constructor so the window is actually on screen
    // before the message thread blocks - otherwise the user stares at nothing
    // during the couple-second warm-up. callAsync posts the work to the next
    // message-loop iteration, by which point this first frame has been flushed
    // to the screen. Guarded so it only fires once; a SafePointer keeps it safe
    // if the component is torn down before the async lands.
    if (!deferredInitScheduled) {
        deferredInitScheduled = true;
        juce::Component::SafePointer<MainContentComponent> safeThis(this);
        juce::MessageManager::callAsync([safeThis]() {
            if (auto* self = safeThis.getComponent())
                self->runDeferredStartupInit();
        });
    }
}

void MainContentComponent::runDeferredStartupInit() {
    // Runs exactly once, on the message-loop iteration after the window's first
    // paint. The audio device init + per-plugin instantiation/state-restore
    // below run synchronously and block interaction for a couple of seconds, so
    // we bracket the work with the OS busy cursor to signal "not ready yet".
    // No messages are pumped during the block, so the cursor stays busy the
    // whole time; hideWaitCursor() restores it once we're interactive.
    juce::MouseCursor::showWaitCursor();

    audioEngine.init();
    audioEngine.getPluginHost().loadScanCache("soundshop_plugins_cache.dat");

    // Instantiate plugins for any nodes loaded before the audio engine was ready.
    // Done via the serial async loader so the window stays responsive instead of
    // blocking the message thread for seconds while plugins initialise. Each node
    // shows a loading badge; the audio graph is rebuilt as each plugin appears.
    beginAsyncPluginLoad();

    // Restore CC mappings from loaded project
    syncCCMappingsFromGraph();

    // Restore project sample rate
    if (graph.projectSampleRate > 0)
        audioEngine.setProjectSampleRate(graph.projectSampleRate);

    // Restore editor panels from loaded project (deferred until UI is ready)
    if (!graph.openEditors.empty()) {
        auto editorIds = graph.openEditors;
        graph.openEditors.clear();
        for (int id : editorIds)
            if (auto* node = graph.findNode(id))
                openEditor(*node);
    }
    // Note: the initial fit-all happens inside NodeGraphComponent's first
    // resized()/paint() call, *before* this runs, so the user never sees the
    // un-fit default zoom. Don't re-fit here - that would clobber any manual
    // pan/zoom the user has already done while the audio engine was warming up.

    // Force the OS to clear any stale "Not Responding" state
#ifdef _WIN32
    if (auto* tlc = getTopLevelComponent())
        if (auto* peer = tlc->getPeer())
            if (auto hwnd = (HWND)peer->getNativeHandle()) {
                wchar_t title[256];
                GetWindowTextW(hwnd, title, 256);
                SetWindowTextW(hwnd, title);
            }
#endif

    // Heavy init done - the app is interactive now, so drop the busy cursor.
    juce::MouseCursor::hideWaitCursor();
}

void MainContentComponent::beginAsyncPluginLoad() {
    // Build the FIFO of plugin nodes that still need instantiation and mark
    // them Pending. Then start the serial loader. If nothing needs loading we
    // leave projectLoading false so Save stays enabled.
    // If a load chain is already draining (e.g. the user opened another project
    // mid-load), we rebuild the queue but must NOT start a second callAsync chain
    // - the in-flight one will pick up the refreshed queue on its next tick.
    // While projectLoading is true there is exactly one pending processNextPluginLoad.
    bool wasLoading = projectLoading;
    pluginLoadQueue.clear();
    {
        std::lock_guard<std::recursive_mutex> graphLk(graph.mutationLock);
        for (auto& n : graph.nodes) {
            if (n.pluginIndex >= 0 && !n.plugin) {
                n.pluginLoadState = PluginLoadState::Pending;
                pluginLoadQueue.push_back(n.id);
            }
        }
    }
    if (pluginLoadQueue.empty()) {
        projectLoading = false;
        menuItemsChanged();
        return;
    }
    projectLoading = true;
    menuItemsChanged();   // reflect greyed Save immediately
    if (graphComponent) graphComponent->repaint();
    if (!wasLoading)
        juce::MessageManager::callAsync([this] { processNextPluginLoad(); });
}

void MainContentComponent::processNextPluginLoad() {
    // Drain the queue one node per call. Each tick: pick the next still-valid
    // node, instantiate its plugin OFF the graph lock (heavy, message-thread
    // only), then publish the result UNDER the lock and rebuild the audio graph.
    // We never hold a Node* across the heavy call (graph.nodes may reallocate if
    // the user adds nodes meanwhile) - we re-look-up by id before publishing.
    while (!pluginLoadQueue.empty()) {
        int nodeId = pluginLoadQueue.front();
        pluginLoadQueue.erase(pluginLoadQueue.begin());

        // Snapshot what we need under the lock, mark the node Loading.
        int pluginIndex = -1;
        std::string pendingState;
        {
            std::lock_guard<std::recursive_mutex> graphLk(graph.mutationLock);
            Node* n = graph.findNode(nodeId);
            if (!n || n->pluginIndex < 0 || n->plugin) continue; // gone / already loaded
            pluginIndex = n->pluginIndex;
            pendingState = n->pendingPluginState;
            n->pluginLoadState = PluginLoadState::Loading;
        }
        if (graphComponent) graphComponent->repaint();

        // Heavy: instantiate + restore state. No graph lock held here.
        auto loaded = audioEngine.getPluginHost().loadPlugin(
            pluginIndex, audioEngine.getSampleRate(), audioEngine.getBlockSize());
        if (loaded && loaded->instance && !pendingState.empty()) {
            juce::MemoryBlock stateData;
            stateData.fromBase64Encoding(pendingState);
            if (stateData.getSize() > 0)
                loaded->instance->setStateInformation(
                    stateData.getData(), (int)stateData.getSize());
        }

        // Publish under the lock so the audio thread sees a consistent node.
        {
            std::lock_guard<std::recursive_mutex> graphLk(graph.mutationLock);
            Node* n = graph.findNode(nodeId);
            if (n) {
                if (loaded) {
                    n->plugin = std::move(loaded);
                    n->pendingPluginState.clear();
                    n->pluginLoadState = PluginLoadState::Ready;
                } else {
                    n->pluginLoadState = PluginLoadState::Failed;
                }
            }
        }
        // (On failure loadPlugin already logged to stderr; we continue anyway.)
        audioEngine.getGraphProcessor().requestRebuild();
        if (graphComponent) graphComponent->repaint();

        // Yield back to the message loop so the UI updates between plugins,
        // then continue with the next one.
        juce::MessageManager::callAsync([this] { processNextPluginLoad(); });
        return;
    }

    // Queue drained - loading complete. Re-enable Save and refresh the menu.
    projectLoading = false;
    menuItemsChanged();
    if (graphComponent) graphComponent->repaint();
}

bool MainContentComponent::keyPressed(const juce::KeyPress& key) {
    // Computer keyboard MIDI: intercept note keys before anything else
    if (handleKeyboardMidi(key, true)) return true;

    // Check custom hotkey bindings: fixed actions first, then dynamic node actions
    auto action = hotkeyManager.findActionForKey(key);
    if (action != HotkeyAction::COUNT) {
        hotkeyManager.executeAction(action);
        return true;
    }
    if (auto* nb = hotkeyManager.findNodeBindingForKey(key)) {
        if (hotkeyManager.onNodeAction)
            hotkeyManager.onNodeAction(nb->nodeName, nb->actionType);
        return true;
    }

    if (key.getModifiers().isCtrlDown()) {
        switch (key.getKeyCode()) {
            case 'S': saveProject(); return true;
            case 'O': openProject(); return true;
            case 'N': newProject(); return true;
            case 'Z':
                if (key.getModifiers().isShiftDown()) {
                    int branches = graph.undoTree.redoBranchCount();
                    if (branches > 1) {
                        // Show branch picker
                        juce::PopupMenu menu;
                        for (int i = 0; i < branches; ++i)
                            menu.addItem(i + 1, juce::String(i + 1) + ": " +
                                graph.undoTree.redoBranchChainDescription(i));
                        menu.showMenuAsync(juce::PopupMenu::Options(),
                            [this](int result) {
                                if (result > 0) {
                                    graph.undoTree.doRedo(result - 1);
                                    graphComponent->repaint();
                                }
                            });
                    } else {
                        graph.undoTree.doRedo();
                        graphComponent->repaint();
                    }
                } else {
                    graph.undoTree.doUndo();
                    graphComponent->repaint();
                }
                return true;
            case 'Y': {
                int branches = graph.undoTree.redoBranchCount();
                if (branches > 1) {
                    juce::PopupMenu menu;
                    for (int i = 0; i < branches; ++i)
                        menu.addItem(i + 1, juce::String(i + 1) + ": " +
                            graph.undoTree.redoBranchChainDescription(i));
                    menu.showMenuAsync(juce::PopupMenu::Options(),
                        [this](int result) {
                            if (result > 0) {
                                graph.undoTree.doRedo(result - 1);
                                graphComponent->repaint();
                            }
                        });
                } else {
                    graph.undoTree.doRedo();
                    graphComponent->repaint();
                }
                return true;
            }
        }
    }
    return false;
}

void MainContentComponent::resized() {
    auto area = getLocalBounds();

    // Menu bar
    menuBar->setBounds(area.removeFromTop(24));

    // Transport bar
    auto transport = area.removeFromTop(32);
    int x = 4;
    auto placeBtn = [&](juce::Component& c, int w) {
        c.setBounds(transport.getX() + x, transport.getY() + 2, w, 28);
        x += w + 4;
    };
    placeBtn(playBtn, 50);
    placeBtn(stopBtn, 50);
    placeBtn(recordBtn, 100);
    x += 8;
    placeBtn(addMidiBtn, 90);
    placeBtn(addAudioBtn, 90);
    x += 8;
    bpmLabel.setBounds(transport.getX() + x, transport.getY() + 2, 35, 28);
    x += 38;
    bpmSlider.setBounds(transport.getX() + x, transport.getY() + 2, 120, 28);
    x += 124;
    placeBtn(tapTempoBtn, 38);
    placeBtn(fitAllBtn, 50);
    placeBtn(metroBtn, 80);
    placeBtn(loopBtn, 42);
    placeBtn(autoBtn, 82);
    placeBtn(songBtn, 46);
    placeBtn(monitorBtn, 64);
    placeBtn(captureBtn, 60);
    placeBtn(keyboardMidiBtn, 42);
    x += 2;
    // Time signature combo (no label - "4/4" is self-explanatory)
    timeSigLabel.setBounds(0, 0, 0, 0); // hidden
    timeSigCombo.setBounds(transport.getX() + x, transport.getY() + 4, 75, 24);
    x += 78;
    // Wide enough for the current/total form, e.g. "0:00.0/15:30.0   Bar 1:1.0/20".
    positionLabel.setBounds(transport.getX() + x, transport.getY() + 2, 270, 28);

    // Split: graph on top, editors on bottom, routing strip between them
    if (!editorPanels.empty()) {
        auto editorArea = area.removeFromBottom(editorPanelHeight);

        // Routing strip: collapsible, sized to its content
        int routingH = routingStrip->getDesiredHeight();
        if (routingH > 0) {
            routingStrip->setBounds(editorArea.removeFromTop(routingH));
            routingStrip->setVisible(true);
        } else {
            routingStrip->setVisible(false);
        }

        // Lay out panels top-to-bottom using each panel's own heightPx.
        // The last panel absorbs whatever rounding/remainder is left so we
        // don't draw a sliver of unpainted area at the bottom. If the
        // summed heights overflow the available area (heightPx wasn't
        // recalc'd, edge case), each panel still gets its requested
        // height and trailing panels get clipped by setBounds; that's
        // visibly wrong but recoverable on next resize.
        int n = (int)editorPanels.size();
        for (int i = 0; i < n; ++i) {
            auto& panel = editorPanels[(size_t)i];
            int h = (i == n - 1) ? editorArea.getHeight()
                                 : juce::jmin(editorArea.getHeight(), panel->heightPx);
            panel->component->setBounds(editorArea.removeFromTop(h));
        }
    } else {
        routingStrip->setVisible(false);
    }
    graphComponent->setBounds(area);
}

void MainContentComponent::timerCallback() {
    // While a project's plugins are loading asynchronously, repaint the graph
    // each tick so the per-node "loading" spinner animates smoothly.
    if (projectLoading && graphComponent)
        graphComponent->repaint();

    // Power-state-aware autosave interval (#87): every ~5 seconds
    // (150 ticks at 30 Hz), check AC vs battery and adjust the
    // autosave interval. Desktops and AC-powered laptops use the
    // faster 5s interval; battery-powered laptops use 20s.
    if (machineHasBattery()) {
        static int powerCheckCounter = 0;
        if (++powerCheckCounter >= 150) {
            powerCheckCounter = 0;
            bool ac = isOnACPower();
            int desired = ac ? 5 : 20;
            if (autosaveIntervalSeconds != desired) {
                autosaveIntervalSeconds = desired;
                fprintf(stderr, "Power state changed - autosave interval -> %ds\n",
                        desired);
            }
        }
    }

    // Hotplug detection: poll the MIDI device list ~once a second (30Hz
    // timer × 30) and show a confirmation dialog when a new device
    // appears that isn't already a node in the graph.
    if (++midiDeviceCheckCounter >= 30) {
        midiDeviceCheckCounter = 0;
        auto devices = juce::MidiInput::getAvailableDevices();
        for (auto& dev : devices) {
            auto idStd = dev.identifier.toStdString();
            if (previousMidiDeviceIds.count(idStd) > 0) continue; // already seen
            previousMidiDeviceIds.insert(idStd);
            // Skip if already represented as a node in the graph
            bool inGraph = false;
            for (auto& n : graph.nodes)
                if (n.type == NodeType::MidiInput && n.midiInputSourceId == idStd)
                    { inGraph = true; break; }
            if (inGraph) continue;
            // Skip the initial scan - we don't want to nag about devices
            // that were already there before the app started. Only offer
            // on NEW device connections after this flag is set.
            if (!midiDeviceScanInitialized) continue;

            // Offer to add this new device
            juce::String name = dev.name;
            juce::String id = dev.identifier;
            juce::AlertWindow::showAsync(
                juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::QuestionIcon)
                    .withTitle("MIDI device connected")
                    .withMessage("A new MIDI input device was detected:\n\n  " + name
                                 + "\n\nAdd it to the graph?")
                    .withButton("Add")
                    .withButton("Ignore"),
                [this, name, id](int result) {
                    if (result != 1) return;
                    auto& n = graph.addNode(name.toStdString(), NodeType::MidiInput,
                        {}, {Pin{0, "MIDI Out", PinKind::Midi, false}}, {80, 400});
                    n.midiInputSourceId = id.toStdString();
                    audioEngine.syncMidiDeviceEnablement();
                    audioEngine.getGraphProcessor().requestRebuild();
                    graphComponent->repaint();
                });
        }
        midiDeviceScanInitialized = true;
    }

    transport.bpm = graph.bpm;
    transport.tuningSystem = graph.tuningSystem;
    transport.concertPitch = graph.concertPitch;
    transport.loopEnabled = graph.loopEnabled;
    transport.loopStartBeat = graph.loopStartBeat;
    transport.loopEndBeat = graph.loopEndBeat;
    graph.resolveAnchors();
    if (transport.tempoMap.getPoints().size() == 1)
        transport.tempoMap.setGlobalBpm(graph.bpm);
    if (transport.timeSigMap.sigs.size() == 1)
        transport.timeSigMap.setGlobal(graph.timeSignatureNum, graph.timeSignatureDen);
    // Sync UI with audio engine's playing state. The audio thread may
    // stop playback internally (e.g., when Song Length + Song Repeat
    // policy fires), so the button text has to reflect that - otherwise
    // it stays stuck on "Stop" after the song auto-stops.
    bool engineIsPlaying = audioEngine.isPlaying();
    if (transport.playing && !engineIsPlaying) {
        // Transport just stopped itself - update button label.
        playBtn.setButtonText("Play");
    }
    transport.playing = engineIsPlaying;

    // Light the Play button while audio is actually playing, sharing the Loop
    // button's accent colour. Driven off the engine's real playing state (not
    // just onPlay/onStop) so it also tracks programmatic stops - e.g. the song
    // auto-stopping at its end. Guarded so we don't churn the colour every tick.
    if (engineIsPlaying) {
        if (playBtn.findColour(juce::TextButton::buttonColourId) != kTransportLitColour)
            playBtn.setColour(juce::TextButton::buttonColourId, kTransportLitColour);
    } else if (playBtn.isColourSpecified(juce::TextButton::buttonColourId)) {
        playBtn.removeColour(juce::TextButton::buttonColourId);
    }

    // Stop is only actionable while the song is actually playing. Grey it out
    // (with an explanatory tooltip) when stopped/paused so it's clear there's
    // nothing to stop - and re-enable it the moment playback starts. Guarded
    // on the current enabled state so we don't churn the tooltip every tick.
    // (See "grayed-out controls must explain themselves" in the project rules.)
    if (stopBtn.isEnabled() != transport.playing) {
        stopBtn.setEnabled(transport.playing);
        stopBtn.setTooltip(transport.playing
            ? "Stop playback and rewind to the start of the loop (or to 0 if loop is off)"
            : "Nothing is playing right now - press Play to start. "
              "Stop then halts playback and rewinds.");
    }

    // Keep the Loop button's lit/unlit state in sync with the actual loop
    // setting. The onClick handler sets the colour when the user toggles it,
    // but looping can also be enabled programmatically - by a project load,
    // an undo/redo, or a tracker import whose module loops back to a section.
    // Syncing here (guarded so we don't repaint every tick) makes the button
    // reflect all of those uniformly. Colours mirror the onClick handler.
    if (graph.loopEnabled) {
        if (loopBtn.findColour(juce::TextButton::buttonColourId) != kTransportLitColour)
            loopBtn.setColour(juce::TextButton::buttonColourId, kTransportLitColour);
    } else if (loopBtn.isColourSpecified(juce::TextButton::buttonColourId)) {
        loopBtn.removeColour(juce::TextButton::buttonColourId);
    }

    // Evaluate Python signals on UI thread and apply to plugin parameters
    if (scriptEngine.isInitialized()) {
        int sample = (int)transport.positionSamples;
        auto values = scriptEngine.evaluateSignals(sample, (int)transport.sampleRate, 480);
        if (!values.empty()) {
            // Convert script engine values to automation values
            std::vector<AutomationValue> autoValues;
            for (auto& sv : values) {
                // Map node index to node ID
                if (sv.nodeIdx >= 0 && sv.nodeIdx < (int)graph.nodes.size())
                    autoValues.push_back({graph.nodes[sv.nodeIdx].id, sv.paramIdx, sv.value});
            }
            audioEngine.getGraphProcessor().applyAutomation(autoValues);
        }
    }

    // Automation-recording pass edges. Placed after transport.playing has been
    // reconciled with the engine (above) so it also catches programmatic stops
    // (song-end auto-stop), not just the Stop button. Play-start arms every
    // Write-resolved param; stop simplifies + commits one undo snapshot.
    if (transport.playing && !recPrevPlaying) beginAutomationPass();
    else if (!transport.playing && recPrevPlaying) endAutomationPass();
    recPrevPlaying = transport.playing;

    // Record (capture) and read (playback) automation during playback. Both run
    // in this one loop so point-writing lives in a single place; the UI controls
    // only flip per-param recWriting via handleParamGesture().
    if (transport.playing) {
        float beat = (float)transport.positionBeats();
        std::vector<AutomationValue> autoValues;
        for (auto& node : graph.nodes) {
            for (int pi = 0; pi < (int)node.params.size(); ++pi) {
                auto& p = node.params[pi];
                // ---- Record axis: capture the live value into the lane ----
                if (p.recWriting) {
                    // Absolute-cable ("Set") params are wholly driven by the
                    // cable and can't be hand-authored, so never record them.
                    bool absLocked = p.modulated && graph.paramHasAbsoluteInput(node.id, pi);
                    if (!absLocked) {
                        float v = p.modulated ? p.baseValue : p.value;
                        recordAutomationPoint(p, beat, v);
                    }
                    continue; // never read-drive a param we're actively recording
                }
                // ---- Read axis: drive the param from its lane ----
                // node-level ignore mutes all lanes; per-param bypass mutes one.
                if (node.ignoreAutomation || p.bypassAutomation) continue;
                auto& lane = p.automation;
                if (!lane.points.empty()) {
                    float val = lane.evaluate(beat);
                    if (val >= -0.5f) { // valid (not sentinel)
                        p.value = val;
                        // Also push to plugin
                        float normalized = (val - p.minVal) /
                            std::max(0.001f, p.maxVal - p.minVal);
                        autoValues.push_back({node.id, pi, juce::jlimit(0.0f, 1.0f, normalized)});
                    }
                }
            }
        }
        if (!autoValues.empty())
            audioEngine.getGraphProcessor().applyAutomation(autoValues);
    }

    // Update position display - show BOTH representations so it's clear what
    // each value means: elapsed wall-clock time (min:sec, starts at 0:00.0,
    // natural for non-musicians) AND musical position (Bar:Beat, 1-based, for
    // anyone working to the grid). Each is shown as current / total so the song
    // length is always visible, e.g. "0:00.0/15:30.0   Bar 1:1.0/20".
    {
        auto fmtMinSec = [](double secs) {
            if (secs < 0.0) secs = 0.0;
            int mins = (int)(secs / 60.0);
            double rem = secs - mins * 60.0;
            return juce::String(mins) + ":" + juce::String(rem, 1).paddedLeft('0', 4);
        };

        double secs = (transport.sampleRate > 0.0)
                          ? (double)transport.positionSamples / transport.sampleRate
                          : 0.0;
        auto bb = transport.timeSigMap.beatToBarBeat(transport.positionBeats());

        // Total song length (beats). 0 = no clips / no explicit length, in which
        // case there's no meaningful total to show.
        double totalBeats = graph.effectiveSongLengthBeats();

        juce::String timeStr = fmtMinSec(secs);
        juce::String barStr  = "Bar " + juce::String(bb.first) + ":"
                             + juce::String(bb.second, 1);

        if (totalBeats > 0.0) {
            double totalSecs = (transport.sampleRate > 0.0)
                                   ? transport.beatsToSamples(totalBeats) / transport.sampleRate
                                   : 0.0;
            // Total bar count: the bar that the song's end falls in. When the
            // length lands exactly on a downbeat (beat 1.0), the song fills the
            // PREVIOUS bar, so a 20-bar song reads "/20" not "/21".
            auto tot = transport.timeSigMap.beatToBarBeat(totalBeats);
            int totalBars = tot.first;
            if (tot.second <= 1.0 + 1e-6 && totalBars > 1) totalBars -= 1;

            timeStr += "/" + fmtMinSec(totalSecs);
            barStr  += "/" + juce::String(totalBars);
        }

        positionLabel.setText(timeStr + "   " + barStr, juce::dontSendNotification);
    }

    graphComponent->repaint();
    for (auto& panel : editorPanels)
        panel->component->repaint();

    // (Heavy startup init is no longer driven from here - it runs once from
    // runDeferredStartupInit(), scheduled off the window's first paint. See
    // MainContentComponent::paint().)

    // Undo-tree persistence tick. Coalesces any number of pushes/undos
    // since the previous tick into one disk write. The serializer is
    // graph-only (no plugin state) so it stays cheap even for large
    // sessions, but we still keep the work to once per UI frame to be
    // friendly to slower disks.
    if (undoTreeDirty) {
        undoTreeDirty = false;
        writeUndoTreePersist();
    }

    // Autosave tick. Uses hi-res ms counter (monotonic, unaffected by wall
    // clock changes). First tick seeds lastAutosaveAttemptMs so the first
    // save fires a full interval after startup, not immediately.
    if (autosaveEnabled) {
        double nowMs = juce::Time::getMillisecondCounterHiRes();
        if (lastAutosaveAttemptMs == 0.0) {
            lastAutosaveAttemptMs = nowMs;
        } else if (nowMs - lastAutosaveAttemptMs >= autosaveIntervalSeconds * 1000.0) {
            lastAutosaveAttemptMs = nowMs;
            performAutosave();
        }
    }

    // Update window title with save flash and dirty indicator
    if (auto* win = dynamic_cast<juce::DocumentWindow*>(getTopLevelComponent())) {
        juce::String title = "SoundShop";
        if (!ProjectFile::currentPath.empty())
            title = juce::File(ProjectFile::currentPath).getFileNameWithoutExtension();
        if (saveFlashFrames > 0) {
            title += " - Saved";
            saveFlashFrames--;
        } else if (projectDirty || graph.dirty) {
            title += " *";
        }
        // Make ephemeral (test) sessions obvious so they're never mistaken for
        // a real working session - and so the absence of a recovery prompt is
        // clearly explained.
        if (isEphemeralSession())
            title += "  [ephemeral session]";
        win->setName(title);
    }
}

// Menu bar
juce::StringArray MainContentComponent::getMenuBarNames() {
    return {"File", "Edit", "Scripts", "View", "Settings", "Plugins", "Help"};
}

juce::PopupMenu MainContentComponent::getMenuForIndex(int idx, const juce::String& name) {
    juce::PopupMenu menu;
    if (name == "File") {
        menu.addItem(1, "New Project");
        menu.addItem(2, "Open Project...", true, false);
        // Save is disabled while a project's plugins are still loading: saving
        // mid-load would risk writing an incomplete graph. The greyed label
        // tells the user why and that it's temporary.
        if (projectLoading) {
            menu.addItem(3, "Save Project  (loading plugins...)", false, false);
            menu.addItem(4, "Save Project As...  (loading plugins...)", false, false);
        } else {
            menu.addItem(3, "Save Project", true, false);
            menu.addItem(4, "Save Project As...");
        }
        menu.addItem(5, "Export Audio...");
        menu.addItem(6, "Import MOD/S3M/IT/XM...");
        menu.addSeparator();
        juce::PopupMenu recentMenu;
        if (recentProjects.empty()) {
            recentMenu.addItem(-1, "(no recent projects)", false);
        } else {
            for (int i = 0; i < (int)recentProjects.size(); ++i) {
                auto file = juce::File(recentProjects[i]);
                recentMenu.addItem(60 + i, file.getFileName() + "  -  " + file.getParentDirectory().getFileName());
            }
            recentMenu.addSeparator();
            recentMenu.addItem(59, "Clear Recents");
        }
        menu.addSubMenu("Recent Projects", recentMenu);
        menu.addSeparator();
        menu.addItem(10, "Quit");
    } else if (name == "Edit") {
        menu.addItem(20, "Undo", graph.undoTree.canUndo());
        menu.addItem(21, "Redo", graph.undoTree.canRedo());
        menu.addSeparator();
        bool hasLoop = graph.loopEnabled && graph.loopEndBeat > graph.loopStartBeat;
        menu.addItem(22, "Write Automation to Selection",
                     hasLoop); // only enabled when loop region is set
        menu.addItem(23, "Arm All Params for Write");
        menu.addItem(24, "Disarm All Params");
        menu.addSeparator();
        menu.addItem(26, "Asset Library...");
    } else if (name == "Scripts") {
        menu.addItem(90, "Script Console...");
        menu.addItem(91, "Run Script File...");
        menu.addSeparator();
        // Recent scripts (reuse the same list as the console)
        auto recentsFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                               .getSiblingFile("soundshop_recent_scripts.txt");
        if (recentsFile.existsAsFile()) {
            juce::StringArray lines;
            lines.addLines(recentsFile.loadFileAsString());
            int ri = 0;
            for (auto& line : lines) {
                if (line.isNotEmpty() && ri < 10) {
                    auto f = juce::File(line);
                    menu.addItem(900 + ri, f.getFileName() + "  -  " + f.getParentDirectory().getFileName());
                    ri++;
                }
            }
            if (ri > 0) menu.addSeparator();
        }
        menu.addItem(92, "Clear Recent Scripts");
    } else if (name == "View") {
        menu.addItem(30, "Fit All");
        menu.addItem(401, "Auto-Fit Graph (always show whole graph)", true, autoFitGraph);
        menu.addItem(400, "Spectrum Analyzer");
    } else if (name == "Settings") {
        menu.addItem(39, "Audio Device...");
        juce::PopupMenu srMenu;
        double curSr = audioEngine.getProjectSampleRate();
        srMenu.addItem(33, "Device Rate (" + juce::String((int)audioEngine.getDeviceSampleRate()) + " Hz)",
                        true, graph.projectSampleRate == 0);
        srMenu.addItem(34, "44100 Hz", true, (int)curSr == 44100 && graph.projectSampleRate > 0);
        srMenu.addItem(35, "48000 Hz", true, (int)curSr == 48000 && graph.projectSampleRate > 0);
        srMenu.addItem(36, "88200 Hz", true, (int)curSr == 88200);
        srMenu.addItem(37, "96000 Hz", true, (int)curSr == 96000);
        srMenu.addItem(38, "192000 Hz", true, (int)curSr == 192000);
        menu.addSubMenu("Project Sample Rate", srMenu);
        menu.addSeparator();
        menu.addItem(32, "Reload Last Project on Startup", true, autoLoadLastProject);
        menu.addSeparator();
        {
            juce::PopupMenu tuningMenu;
            for (int i = 0; i < (int)TuningSystem::COUNT; ++i)
                tuningMenu.addItem(70 + i, tuningSystemName((TuningSystem)i),
                                   true, graph.tuningSystem == (TuningSystem)i);
            // Unequal temperaments need per-note pitch bend, which for hosted
            // VST3/AU plugins means MPE. Warn that the temperament only reaches
            // a hosted plugin if that plugin is switched to MPE mode (right-click
            // the plugin node -> Enable MPE), and that even then a plugin which
            // ignores the bend-range message and keeps its own fixed range may
            // not be tuned exactly. Built-in synths are always tuned correctly.
            if (graph.tuningSystem != TuningSystem::Equal12) {
                tuningMenu.addSeparator();
                tuningMenu.addItem(-1, "Note: this temperament reaches a hosted plugin", false);
                tuningMenu.addItem(-1, "only when that plugin is in MPE mode", false);
                tuningMenu.addItem(-1, "(right-click the plugin node -> Enable MPE).", false);
                tuningMenu.addItem(-1, "Plugins that keep their own fixed bend range", false);
                tuningMenu.addItem(-1, "may still not be tuned exactly. Built-in", false);
                tuningMenu.addItem(-1, "synths are always tuned correctly.", false);
            }
            menu.addSubMenu("Tuning System", tuningMenu);

            juce::PopupMenu pitchMenu;
            int presetCount = 0;
            auto* presets = getConcertPitchPresets(presetCount);
            for (int i = 0; i < presetCount; ++i) {
                bool isCurrent = std::abs(graph.concertPitch - presets[i].hz) < 0.5f;
                pitchMenu.addItem(80 + i, presets[i].name, true, isCurrent);
            }
            pitchMenu.addSeparator();
            pitchMenu.addItem(99, "Custom...");
            menu.addSubMenu("Concert Pitch (A4 = " + juce::String(graph.concertPitch, 1) + " Hz)", pitchMenu);
        }
        menu.addSeparator();
        menu.addItem(110, "Crossfade Duration ("
                    + juce::String((int)std::round(graph.globalCrossfadeSec * 1000.0f))
                    + " ms)...");
        menu.addItem(111, "Add MIDI Input Device...");
        menu.addSeparator();
        menu.addItem(50, "Assign Hotkeys...");
        menu.addItem(51, "Capture Room IR...");
    } else if (name == "Plugins") {
        menu.addItem(40, "Plugin Settings...");
        menu.addSeparator();
        auto& plugins = audioEngine.getPluginHost().getAvailablePlugins();
        menu.addItem(-1, juce::String((int)plugins.size()) + " plugins loaded", false);
    } else if (name == "Help") {
        menu.addItem(300, "User Guide (Home)");
        menu.addSeparator();
        menu.addItem(301, "Getting Started");
        menu.addItem(302, "Graph Basics");
        menu.addItem(303, "Pin Kinds (Cable Colors)");
        menu.addSeparator();
        menu.addItem(304, "MIDI Input and Routing");
        menu.addItem(305, "Piano Roll");
        menu.addItem(306, "Wavetables and Layered Waveforms");
        menu.addItem(307, "Terrain Synth");
        menu.addItem(308, "Layers and Effect Groups");
        menu.addItem(309, "Trigger Node");
        menu.addItem(310, "MIDI Modulator");
        menu.addItem(311, "Convolution Filter");
        menu.addItem(313, "Script (Signal + MIDI)");
        menu.addItem(314, "Algorithmic MIDI");
        menu.addItem(315, "Voices (Polyphony)");
        menu.addItem(316, "Recording Automation");
        menu.addSeparator();
        menu.addItem(312, "Keyboard Shortcuts");
        menu.addSeparator();
        menu.addItem(320, "About SoundShop");
    }
    return menu;
}

void MainContentComponent::menuItemSelected(int menuItemID, int) {
    // Recent scripts
    if (menuItemID >= 900 && menuItemID < 910) {
        auto recentsFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                               .getSiblingFile("soundshop_recent_scripts.txt");
        if (recentsFile.existsAsFile()) {
            juce::StringArray lines;
            lines.addLines(recentsFile.loadFileAsString());
            int idx = menuItemID - 900;
            int ri = 0;
            for (auto& line : lines) {
                if (line.isNotEmpty()) {
                    if (ri == idx) { runScriptFile(line); return; }
                    ri++;
                }
            }
        }
        return;
    }

    if (menuItemID >= 60 && menuItemID < 60 + (int)recentProjects.size()) {
        auto path = recentProjects[menuItemID - 60];
        if (juce::File(path).existsAsFile())
            openProjectFile(path);
        else {
            recentProjects.erase(recentProjects.begin() + (menuItemID - 60));
            saveRecentProjects();
        }
        return;
    }
    switch (menuItemID) {
        case 1: newProject(); break;
        case 2: openProject(); break;
        case 3: saveProject(); break;
        case 4: saveProjectAs(); break;
        case 5: exportAudio(); break;
        case 6: importModFile(); break;
        case 10: juce::JUCEApplication::getInstance()->systemRequestedQuit(); break;
        case 20: graph.undoTree.doUndo(); graphComponent->repaint(); break;
        case 22: {
            // Write Automation to Selection - uses loop region as the range
            if (graph.loopEnabled && graph.loopEndBeat > graph.loopStartBeat) {
                graph.writeAutomationToSelection(
                    (float)graph.loopStartBeat, (float)graph.loopEndBeat);
                graphComponent->repaint();
                juce::Logger::writeToLog("Wrote automation to beats "
                    + juce::String(graph.loopStartBeat, 1) + "-"
                    + juce::String(graph.loopEndBeat, 1));
            }
            break;
        }
        case 23: graph.armAllParams(true); graphComponent->repaint(); break;
        case 24: graph.armAllParams(false); graphComponent->repaint(); break;
        case 26: showAssetLibraryDialog(); break;
        case 70: case 71: case 72: case 73:
            graph.tuningSystem = (TuningSystem)(menuItemID - 70);
            break;
        case 80: case 81: case 82: case 83: case 84: case 85: case 86: case 87: {
            int presetCount = 0;
            auto* presets = getConcertPitchPresets(presetCount);
            int idx = menuItemID - 80;
            if (idx >= 0 && idx < presetCount)
                graph.concertPitch = presets[idx].hz;
            break;
        }
        case 99: {
            // Custom concert pitch
            auto* aw = new juce::AlertWindow("Concert Pitch",
                "Enter the frequency of A4 in Hz:",
                juce::MessageBoxIconType::NoIcon);
            aw->addTextEditor("hz", juce::String(graph.concertPitch, 1), "Hz:");
            aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
            aw->addButton("Cancel", 0);
            aw->enterModalState(true, juce::ModalCallbackFunction::create(
                [this, aw](int res) {
                    if (res == 1) {
                        float hz = aw->getTextEditorContents("hz").getFloatValue();
                        if (hz > 200 && hz < 600)
                            graph.concertPitch = hz;
                    }
                    delete aw;
                }), true);
            break;
        }
        case 110: {
            // Global crossfade duration (ms)
            auto* aw = new juce::AlertWindow("Crossfade Duration",
                "How long should crossfades be?\n"
                "(Used at effect region edges and any other start/stop transition.)",
                juce::MessageBoxIconType::NoIcon);
            aw->addTextEditor("ms",
                juce::String((int)std::round(graph.globalCrossfadeSec * 1000.0f)),
                "ms:");
            aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
            aw->addButton("Cancel", 0);
            aw->enterModalState(true, juce::ModalCallbackFunction::create(
                [this, aw](int res) {
                    if (res == 1) {
                        float ms = aw->getTextEditorContents("ms").getFloatValue();
                        // 0..2000 ms is plenty; 0 means "instant" (you'll hear clicks).
                        ms = juce::jlimit(0.0f, 2000.0f, ms);
                        graph.globalCrossfadeSec = ms / 1000.0f;
                        audioEngine.getGraphProcessor().requestRebuild();
                    }
                    delete aw;
                }), true);
            break;
        }
        case 111: showMidiDeviceWizard(); break;
        case 400: {
            // Spectrum Analyzer (#10): open a floating non-modal window.
            // Parented to the main content component so the OS doesn't give
            // it a separate taskbar entry.
            auto* viz = new SpectrumVisualizerComponent(audioEngine);
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned(viz);
            opts.dialogTitle = "Spectrum Analyzer";
            opts.dialogBackgroundColour = juce::Colour(18, 20, 28);
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar = false;
            opts.resizable = true;
            opts.componentToCentreAround = this;
            SoundShop::launchNonModalToolDialog(opts);
            break;
        }
        case 300: openHelpDoc("index.html"); break;
        case 301: openHelpDoc("getting-started.html"); break;
        case 302: openHelpDoc("graph-basics.html"); break;
        case 303: openHelpDoc("pin-kinds.html"); break;
        case 304: openHelpDoc("midi-input.html"); break;
        case 305: openHelpDoc("piano-roll.html"); break;
        case 306: openHelpDoc("wavetables.html"); break;
        case 307: openHelpDoc("terrain-synth.html"); break;
        case 308: openHelpDoc("layers-and-groups.html"); break;
        case 309: openHelpDoc("trigger-node.html"); break;
        case 310: openHelpDoc("midi-modulator.html"); break;
        case 311: openHelpDoc("convolution.html"); break;
        case 312: openHelpDoc("keyboard-shortcuts.html"); break;
        case 313: openHelpDoc("signal-shape.html"); break;
        case 314: openHelpDoc("midi-script.html"); break;
        case 315: openHelpDoc("voices.html"); break;
        case 316: openHelpDoc("automation-recording.html"); break;
        case 320:
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::InfoIcon,
                "About SoundShop",
                "SoundShop2\n\n"
                "A node-based DAW designed to be intuitive for people "
                "without a musical background.\n\n"
                "See Help > User Guide for documentation.");
            break;
        case 50: openHotkeySettings(); break;
        case 51: {
            auto* comp = new RoomIRCaptureComponent(graph, *audioEngine.getDeviceManager(),
                [this]() {
                    audioEngine.getGraphProcessor().requestRebuild();
                    graphComponent->repaint();
                });
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned(comp);
            opts.dialogTitle = "Capture Room Impulse Response";
            opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar = false;
            opts.resizable = true;
            opts.componentToCentreAround = this;
            SoundShop::launchToolDialog(opts);
            break;
        }
        case 21: {
            int branches = graph.undoTree.redoBranchCount();
            if (branches > 1) {
                juce::PopupMenu menu;
                for (int i = 0; i < branches; ++i)
                    menu.addItem(i + 1, juce::String(i + 1) + ": " +
                        graph.undoTree.redoBranchChainDescription(i));
                menu.showMenuAsync(juce::PopupMenu::Options(),
                    [this](int result) {
                        if (result > 0) {
                            graph.undoTree.doRedo(result - 1);
                            graphComponent->repaint();
                        }
                    });
            } else {
                graph.undoTree.doRedo();
                graphComponent->repaint();
            }
            break;
        }
        case 30: graphComponent->fitAll(); break;
        case 401:
            autoFitGraph = !autoFitGraph;
            if (graphComponent) graphComponent->setAutoFitView(autoFitGraph);
            savePreferences();
            break;
        case 31: showScriptConsole(); break;
        case 32: autoLoadLastProject = !autoLoadLastProject; savePreferences(); break;
        case 33: graph.projectSampleRate = 0; audioEngine.setProjectSampleRate(0); break;
        case 34: graph.projectSampleRate = 44100; audioEngine.setProjectSampleRate(44100); break;
        case 35: graph.projectSampleRate = 48000; audioEngine.setProjectSampleRate(48000); break;
        case 36: graph.projectSampleRate = 88200; audioEngine.setProjectSampleRate(88200); break;
        case 37: graph.projectSampleRate = 96000; audioEngine.setProjectSampleRate(96000); break;
        case 38: graph.projectSampleRate = 192000; audioEngine.setProjectSampleRate(192000); break;
        case 39: showAudioDeviceSettings(); break;
        case 40: showPluginSettingsDialog(); break;
        case 90: showScriptConsole(); break;
        case 91: browseAndRunScript(); break;
        case 92: {
            auto rf = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                          .getSiblingFile("soundshop_recent_scripts.txt");
            rf.deleteFile();
            break;
        }
        case 59: recentProjects.clear(); saveRecentProjects(); break;
        default: break;
    }
}

// ==============================================================================
// Waveform Visualizer (for built-in synth)
// ==============================================================================

class WaveformVisualizerComponent : public juce::Component, public juce::Timer {
public:
    WaveformVisualizerComponent(BuiltinSynthProcessor& proc, Node& node)
        : proc(proc), node(node) {
        startTimerHz(30);
        setSize(450, 250);
    }

    void timerCallback() override { repaint(); }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(25, 25, 30));

        auto& table = proc.getWavetable().getBaseTable();
        if (table.empty()) return;

        auto area = getLocalBounds().reduced(10).toFloat();
        float w = area.getWidth();
        float h = area.getHeight();
        float cx = area.getX();
        float cy = area.getCentreY();

        // Draw center line
        g.setColour(juce::Colours::grey.withAlpha(0.3f));
        g.drawHorizontalLine((int)cy, cx, cx + w);

        // Draw waveform
        g.setColour(juce::Colours::cornflowerblue);
        juce::Path path;
        int n = (int)table.size();
        for (int i = 0; i < n; ++i) {
            float x = cx + (float)i / n * w;
            float y = cy - table[i] * h * 0.45f;
            if (i == 0) path.startNewSubPath(x, y);
            else path.lineTo(x, y);
        }
        g.strokePath(path, juce::PathStrokeType(1.5f));

        // Draw current position
        float phase = proc.getCurrentPhase();
        if (phase >= 0 && phase <= 1) {
            float px = cx + phase * w;
            int idx = juce::jlimit(0, n - 1, (int)(phase * n));
            float py = cy - table[idx] * h * 0.45f;

            g.setColour(juce::Colours::white);
            g.drawVerticalLine((int)px, area.getY(), area.getBottom());
            g.setColour(juce::Colours::yellow);
            g.fillEllipse(px - 4, py - 4, 8, 8);
        }

        // Label
        g.setColour(juce::Colours::grey);
        g.setFont(11.0f);
        g.drawText(juce::String(n) + " samples | " + node.name,
                    0, getHeight() - 16, getWidth(), 16, juce::Justification::centred);
    }

private:
    BuiltinSynthProcessor& proc;
    Node& node;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformVisualizerComponent)
};

// ==============================================================================
// Terrain Visualizer
// ==============================================================================

class TerrainVisualizerComponent : public juce::Component, public juce::Timer {
public:
    TerrainVisualizerComponent(TerrainSynthProcessor& proc, Node& node)
        : proc(proc), node(node) {
        startTimerHz(30);

        addAndMakeVisible(clearPathBtn);
        addAndMakeVisible(loopModeCombo);
        addAndMakeVisible(drawModeCombo);

        clearPathBtn.setButtonText("Clear Path");
        clearPathBtn.setTooltip("Discard all the points in the current synthesis traversal path");
        clearPathBtn.onClick = [this]() {
            this->proc.getTraversalParams().pathPoints.clear();
            repaint();
        };

        drawModeCombo.addItem("Off", 1);
        drawModeCombo.addItem("Click Points", 2);
        drawModeCombo.addItem("Freehand", 3);
        drawModeCombo.setSelectedItemIndex(0);
        drawModeCombo.setTooltip("How to add points to the traversal path. Off = visual only; "
                                 "Click Points = each click drops a point; Freehand = drag the mouse to draw a continuous path.");

        loopModeCombo.addItem("Loop", 1);
        loopModeCombo.addItem("Bounce", 2);
        loopModeCombo.setSelectedItemIndex(0);
        loopModeCombo.setTooltip("Loop replays the path from start when it reaches the end. "
                                 "Bounce reverses direction at each end, ping-ponging back and forth.");

        addAndMakeVisible(smoothToggle);
        smoothToggle.setButtonText("Smooth");
        smoothToggle.setTooltip("When on, the traversal interpolates smoothly between points "
                                "(curved path). When off, it jumps in straight lines from point to point.");
        smoothToggle.setToggleable(true);
        smoothToggle.setClickingTogglesState(true);
        smoothToggle.setToggleState(true, juce::dontSendNotification);

        addAndMakeVisible(addDimBtn);
        addAndMakeVisible(removeDimBtn);
        addDimBtn.setButtonText("+ Dim");
        removeDimBtn.setButtonText("- Dim");
        addDimBtn.setTooltip("Add a terrain dimension. Adds a new Sig input pin and Position "
                             "knobs (Center/Radius) on the synth node. The terrain extends into "
                             "the new axis - reference it in your expression with the axis variable "
                             "(x, y, z, w, v, u, s, t for dims 1-8). Axes you don't reference are "
                             "constant along that axis but still exist as inputs you can modulate.");
        removeDimBtn.setTooltip("Remove the last terrain dimension");
        addDimBtn.onClick = [this]() { changeDimCount(1); };
        removeDimBtn.onClick = [this]() { changeDimCount(-1); };
        updateDimLabel();

        addAndMakeVisible(projCombo);
        projCombo.setTooltip("Choose which two axes to display in the heatmap when the terrain "
                             "has more than 2 dimensions.");
        projCombo.onChange = [this]() { repaint(); };
        rebuildProjCombo();

        setSize(450, 480);
    }

    void timerCallback() override { repaint(); }

    void resized() override {
        auto bottom = getLocalBounds().removeFromBottom(30).reduced(4, 2);
        drawModeCombo.setBounds(bottom.removeFromLeft(90));
        bottom.removeFromLeft(4);
        smoothToggle.setBounds(bottom.removeFromLeft(60));
        bottom.removeFromLeft(4);
        loopModeCombo.setBounds(bottom.removeFromLeft(70));
        bottom.removeFromLeft(4);
        clearPathBtn.setBounds(bottom.removeFromLeft(65));
        bottom.removeFromLeft(8);
        addDimBtn.setBounds(bottom.removeFromLeft(70));
        bottom.removeFromLeft(2);
        removeDimBtn.setBounds(bottom.removeFromLeft(48));
        if (projCombo.isVisible()) {
            bottom.removeFromLeft(4);
            projCombo.setBounds(bottom.removeFromLeft(60));
        }
    }

    void mouseDown(const juce::MouseEvent& e) override {
        int mode = drawModeCombo.getSelectedItemIndex(); // 0=Off, 1=Click, 2=Freehand
        if (mode == 0) return;
        auto dims = proc.getTerrain().getDimensions();
        if (dims.size() < 2) return;

        auto [nx, ny] = screenToNorm(e.position);
        auto& path = proc.getTraversalParams().pathPoints;

        if (e.mods.isRightButtonDown()) {
            // Right-click: remove nearest point
            float bestDist = 20.0f;
            int bestIdx = -1;
            for (int i = 0; i < (int)path.size(); ++i) {
                auto [sx, sy] = normToScreen(path[i].coord);
                float dist = e.position.getDistanceFrom({sx, sy});
                if (dist < bestDist) { bestDist = dist; bestIdx = i; }
            }
            if (bestIdx >= 0) path.erase(path.begin() + bestIdx);
            for (int i = 0; i < (int)path.size(); ++i)
                path[i].time = (float)i;
        } else if (mode == 2) {
            // Freehand: clear and start fresh
            path.clear();
            TraversalParams::PathPoint pt;
            pt.time = 0;
            pt.coord = {nx, ny};
            path.push_back(pt);
            drawing = true;
        } else {
            // Click Points mode
            // Check if clicking near the first point to close the loop
            if (path.size() >= 3) {
                auto [sx, sy] = normToScreen(path[0].coord);
                if (e.position.getDistanceFrom({sx, sy}) < 12.0f) {
                    // Close the loop: set loop mode and switch traversal
                    loopModeCombo.setSelectedItemIndex(0); // Loop
                    proc.getTraversalParams().mode = TraversalMode::Path;
                    repaint();
                    return;
                }
            }
            // Add point
            TraversalParams::PathPoint pt;
            pt.time = path.empty() ? 0.0f : path.back().time + 1.0f;
            pt.coord = {nx, ny};
            path.push_back(pt);
        }

        proc.getTraversalParams().mode = TraversalMode::Path;
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        if (!drawing) return;
        auto dims = proc.getTerrain().getDimensions();
        if (dims.size() < 2) return;

        auto [nx, ny] = screenToNorm(e.position);
        auto& path = proc.getTraversalParams().pathPoints;

        // Only add point if moved enough from the last one
        if (!path.empty()) {
            auto& last = path.back().coord;
            float dx = nx - (last.size() > 0 ? last[0] : 0);
            float dy = ny - (last.size() > 1 ? last[1] : 0);
            if (dx * dx + dy * dy < 0.0002f) return;
        }

        // Check if near the start point to auto-close loop
        if (path.size() >= 10) {
            float dx = nx - (path[0].coord.size() > 0 ? path[0].coord[0] : 0);
            float dy = ny - (path[0].coord.size() > 1 ? path[0].coord[1] : 0);
            if (dx * dx + dy * dy < 0.001f) {
                // Close the loop
                loopModeCombo.setSelectedItemIndex(0);
                drawing = false;
                repaint();
                return;
            }
        }

        TraversalParams::PathPoint pt;
        pt.time = path.empty() ? 0.0f : path.back().time + 1.0f;
        pt.coord = {nx, ny};
        path.push_back(pt);
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override {
        if (drawing) {
            drawing = false;
            // Simplify freehand path: reduce points if too many
            auto& path = proc.getTraversalParams().pathPoints;
            if (path.size() > 200) {
                // Keep every Nth point
                int step = (int)path.size() / 100;
                std::vector<TraversalParams::PathPoint> simplified;
                for (int i = 0; i < (int)path.size(); i += step)
                    simplified.push_back(path[i]);
                if (simplified.back().time != path.back().time)
                    simplified.push_back(path.back());
                path = simplified;
                for (int i = 0; i < (int)path.size(); ++i)
                    path[i].time = (float)i;
            }
            repaint();
        }
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(25, 25, 30));

        auto& terrain = proc.getTerrain();
        auto& data = terrain.getData();
        auto dims = terrain.getDimensions();
        if (data.empty()) return;

        // 1D terrain: waveform view
        if (dims.size() == 1) {
            auto area = getLocalBounds().reduced(10).removeFromTop(getHeight() - 35).toFloat();
            float w = area.getWidth(), h = area.getHeight();
            float cx = area.getX(), cy = area.getCentreY();

            g.setColour(juce::Colours::grey.withAlpha(0.3f));
            g.drawHorizontalLine((int)cy, cx, cx + w);

            g.setColour(juce::Colour(120, 60, 100));
            juce::Path path;
            int n = (int)data.size();
            for (int i = 0; i < n; ++i) {
                float x = cx + (float)i / n * w;
                float y = cy - data[i] * h * 0.45f;
                if (i == 0) path.startNewSubPath(x, y); else path.lineTo(x, y);
            }
            g.strokePath(path, juce::PathStrokeType(1.5f));

            auto pos = proc.getCurrentPosition();
            if (!pos.empty()) {
                float px = cx + pos[0] * w;
                int idx = juce::jlimit(0, n - 1, (int)(pos[0] * n));
                float py = cy - data[idx] * h * 0.45f;
                g.setColour(juce::Colours::white);
                g.drawVerticalLine((int)px, area.getY(), area.getBottom());
                g.setColour(juce::Colours::yellow);
                g.fillEllipse(px - 4, py - 4, 8, 8);
            }
            return;
        }

        if (dims.size() < 2) return;

        // 2D terrain: heatmap
        int rows = dims[0], cols = dims[1];
        float mapSize = std::min((float)getWidth(), (float)getHeight() - 50.0f) - 10.0f;
        float ox = (getWidth() - mapSize) / 2.0f;
        float oy = 5.0f;
        mapOx = ox; mapOy = oy; mapSize_ = mapSize;

        int stepR = std::max(1, rows / 200);
        int stepC = std::max(1, cols / 200);
        float cellW = mapSize / cols, cellH = mapSize / rows;
        for (int r = 0; r < rows; r += stepR) {
            for (int c = 0; c < cols; c += stepC) {
                float val = data[r * cols + c];
                float bright = (val + 1.0f) * 0.5f;
                g.setColour(juce::Colour::fromHSV(0.6f - bright * 0.6f, 0.7f, bright, 1.0f));
                g.fillRect(ox + c * cellW, oy + r * cellH,
                           cellW * stepC + 1, cellH * stepR + 1);
            }
        }

        // Draw path if in Path mode
        auto& pathPts = proc.getTraversalParams().pathPoints;
        if (!pathPts.empty()) {
            bool smooth = smoothToggle.getToggleState() && pathPts.size() >= 3;
            bool isLoop = loopModeCombo.getSelectedItemIndex() == 0;

            g.setColour(juce::Colours::yellow.withAlpha(0.8f));
            juce::Path pathLine;

            if (smooth) {
                // Catmull-Rom spline through the points
                int n = (int)pathPts.size();
                int segments = n + (isLoop ? 1 : 0);
                for (int seg = 0; seg < segments; ++seg) {
                    for (int step = 0; step < 10; ++step) {
                        float t = step / 10.0f;
                        int i0 = (seg - 1 + n) % n;
                        int i1 = seg % n;
                        int i2 = (seg + 1) % n;
                        int i3 = (seg + 2) % n;
                        if (!isLoop) {
                            i0 = juce::jlimit(0, n - 1, seg - 1);
                            i1 = juce::jlimit(0, n - 1, seg);
                            i2 = juce::jlimit(0, n - 1, seg + 1);
                            i3 = juce::jlimit(0, n - 1, seg + 2);
                        }
                        auto getC = [&](int idx, int dim) {
                            return idx < n && dim < (int)pathPts[idx].coord.size()
                                ? pathPts[idx].coord[dim] : 0.5f;
                        };
                        float t2 = t * t, t3 = t2 * t;
                        auto catmull = [&](int dim) {
                            float p0 = getC(i0, dim), p1 = getC(i1, dim);
                            float p2 = getC(i2, dim), p3 = getC(i3, dim);
                            return 0.5f * ((2*p1) + (-p0+p2)*t + (2*p0-5*p1+4*p2-p3)*t2 + (-p0+3*p1-3*p2+p3)*t3);
                        };
                        float nx = catmull(0), ny = catmull(1);
                        auto [sx, sy] = normToScreen(std::vector<float>{nx, ny});
                        if (seg == 0 && step == 0) pathLine.startNewSubPath(sx, sy);
                        else pathLine.lineTo(sx, sy);
                    }
                }
            } else {
                // Straight lines
                for (int i = 0; i < (int)pathPts.size(); ++i) {
                    auto [sx, sy] = normToScreen(pathPts[i].coord);
                    if (i == 0) pathLine.startNewSubPath(sx, sy);
                    else pathLine.lineTo(sx, sy);
                }
                if (isLoop && pathPts.size() > 1) {
                    auto [sx, sy] = normToScreen(pathPts[0].coord);
                    pathLine.lineTo(sx, sy);
                }
            }
            g.strokePath(pathLine, juce::PathStrokeType(2.0f));

            // Draw control points (skip for freehand with many points)
            if (pathPts.size() <= 50) {
                for (int i = 0; i < (int)pathPts.size(); ++i) {
                    auto [sx, sy] = normToScreen(pathPts[i].coord);
                    g.setColour(i == 0 ? juce::Colours::limegreen : juce::Colours::yellow);
                    g.fillEllipse(sx - 4, sy - 4, 8, 8);
                }
                // Show close-loop hint on first point when in Click mode with 3+ points
                if (pathPts.size() >= 3 && drawModeCombo.getSelectedItemIndex() == 1) {
                    auto [sx, sy] = normToScreen(pathPts[0].coord);
                    g.setColour(juce::Colours::limegreen.withAlpha(0.3f));
                    g.drawEllipse(sx - 12, sy - 12, 24, 24, 1.5f);
                }
            }
        }

        // Draw current position
        auto pos = proc.getCurrentPosition();
        if (pos.size() >= 2) {
            float px = ox + pos[0] * mapSize;
            float py = oy + pos[1] * mapSize;
            g.setColour(juce::Colours::white);
            g.fillEllipse(px - 4, py - 4, 8, 8);
            g.setColour(juce::Colours::white.withAlpha(0.4f));
            g.drawEllipse(px - 8, py - 8, 16, 16, 1.5f);
        }

        // Info label
        g.setColour(juce::Colours::grey);
        g.setFont(11.0f);
        const char* modeStr;
        switch (proc.getMode()) {
            case TerrainSynthMode::SamplePerPoint:   modeStr = "Direct";        break;
            case TerrainSynthMode::WaveformPerPoint: modeStr = "AM-sine";       break;
            case TerrainSynthMode::AdditiveBank:     modeStr = "Additive bank"; break;
            default:                                 modeStr = "?";             break;
        }
        juce::String travStr;
        switch (proc.getTraversalParams().mode) {
            case TraversalMode::Orbit: travStr = "Orbit"; break;
            case TraversalMode::Linear: travStr = "Linear"; break;
            case TraversalMode::Lissajous: travStr = "Lissajous"; break;
            case TraversalMode::Path: travStr = "Path (" + juce::String((int)pathPts.size()) + " pts)"; break;
            case TraversalMode::Physics: travStr = "Physics"; break;
            default: travStr = "Custom"; break;
        }
        g.drawText(juce::String(cols) + "x" + juce::String(rows) + "  " + modeStr + "  " + travStr,
                    0, getHeight() - 48, getWidth(), 16, juce::Justification::centred);
    }

    void changeDimCount(int delta) {
        static const char* axisNames[] = {"X", "Y", "Z", "W", "V", "U", "S", "T"};
        int curDims = 0;
        for (auto& pin : node.pinsIn)
            if (pin.kind == PinKind::Signal) ++curDims;
        int newDims = juce::jlimit(1, 8, curDims + delta);
        if (newDims == curDims) return;

        if (newDims > curDims) {
            // Add signal pin + Center/Radius params
            for (int d = curDims; d < newDims; ++d) {
                std::string axis = (d < 8) ? axisNames[d] : std::to_string(d);
                node.pinsIn.push_back(Pin{0, "Sig " + axis, PinKind::Signal, true, 1});
                // Assign unique pin ID
                int maxId = 0;
                for (auto& p : node.pinsIn)  maxId = std::max(maxId, p.id);
                for (auto& p : node.pinsOut) maxId = std::max(maxId, p.id);
                node.pinsIn.back().id = maxId + 1;
                node.params.push_back({"Center " + axis, 0.5f, 0.0f, 1.0f});
                node.params.push_back({"Radius " + axis, 0.3f, 0.0f, 0.5f});
            }
        } else {
            // Remove from the end
            for (int d = curDims; d > newDims; --d) {
                std::string axis = (d - 1 < 8) ? axisNames[d - 1] : std::to_string(d - 1);
                // Remove signal pin
                for (int i = (int)node.pinsIn.size() - 1; i >= 0; --i) {
                    if (node.pinsIn[i].kind == PinKind::Signal &&
                        node.pinsIn[i].name == "Sig " + axis) {
                        node.pinsIn.erase(node.pinsIn.begin() + i);
                        break;
                    }
                }
                // Remove Center/Radius params
                auto removeParam = [&](const std::string& name) {
                    for (int i = (int)node.params.size() - 1; i >= 0; --i)
                        if (node.params[i].name == name)
                            { node.params.erase(node.params.begin() + i); break; }
                };
                removeParam("Center " + axis);
                removeParam("Radius " + axis);
            }
        }
        // Rebuild the terrain dimensions
        std::vector<int> dims(newDims, 64);
        proc.getTerrain().init(dims);
        // Re-evaluate expression
        if (!node.script.empty())
            proc.getTerrain().fillFromExpression(node.script);
        updateDimLabel();
        rebuildProjCombo();
        repaint();
    }

    void updateDimLabel() {
        int dims = 0;
        for (auto& pin : node.pinsIn)
            if (pin.kind == PinKind::Signal) ++dims;
        addDimBtn.setButtonText(dims > 2
            ? ("+ Dim (" + juce::String(dims) + "D)")
            : juce::String("+ Dim"));
    }

    void rebuildProjCombo() {
        static const char* axisNames[] = {"X", "Y", "Z", "W", "V", "U", "S", "T"};
        projCombo.clear(juce::dontSendNotification);
        int dims = 0;
        for (auto& pin : node.pinsIn)
            if (pin.kind == PinKind::Signal) ++dims;
        if (dims <= 2) {
            projCombo.setVisible(false);
            return;
        }
        projCombo.setVisible(true);
        int id = 1;
        for (int a = 0; a < dims; ++a)
            for (int b = a + 1; b < dims; ++b) {
                juce::String label = juce::String(axisNames[a]) + "-" + axisNames[b];
                projCombo.addItem(label, id++);
            }
        projCombo.setSelectedItemIndex(0, juce::dontSendNotification);
    }

    // Get the two projected axis indices from the projection combo selection.
    std::pair<int, int> projAxes() const {
        int dims = 0;
        for (auto& pin : node.pinsIn)
            if (pin.kind == PinKind::Signal) ++dims;
        if (dims <= 2) return {0, 1};
        int sel = projCombo.getSelectedItemIndex();
        int idx = 0;
        for (int a = 0; a < dims; ++a)
            for (int b = a + 1; b < dims; ++b) {
                if (idx == sel) return {a, b};
                ++idx;
            }
        return {0, 1};
    }

private:
    TerrainSynthProcessor& proc;
    Node& node;
    juce::TextButton clearPathBtn, smoothToggle;
    juce::TextButton addDimBtn, removeDimBtn;
    juce::ComboBox loopModeCombo, drawModeCombo, projCombo;
    float mapOx = 0, mapOy = 0, mapSize_ = 100;
    bool drawing = false;

    std::pair<float, float> screenToNorm(juce::Point<float> pos) {
        float nx = juce::jlimit(0.0f, 1.0f, (pos.x - mapOx) / mapSize_);
        float ny = juce::jlimit(0.0f, 1.0f, (pos.y - mapOy) / mapSize_);
        return {nx, ny};
    }

    std::pair<float, float> normToScreen(const std::vector<float>& coord) {
        float sx = mapOx + (coord.size() > 0 ? coord[0] : 0.5f) * mapSize_;
        float sy = mapOy + (coord.size() > 1 ? coord[1] : 0.5f) * mapSize_;
        return {sx, sy};
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TerrainVisualizerComponent)
};

void MainContentComponent::showPluginUI(int nodeId) {
    auto* node = graph.findNode(nodeId);

    // Drum Synth: open the drum editor with pad mapping
    if (node && node->script == "__drumsynth__") {
        auto* editor = new DrumSynthEditorComponent(graph, node->id, &audioEngine);
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(editor);
        opts.dialogTitle = "Drum Synth: " + juce::String(node->name);
        opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = false;
        opts.resizable = true;
        opts.componentToCentreAround = this;
        SoundShop::launchToolDialog(opts);
        return;
    }

    // MultiSampler: open the zone-based sampler editor.
    if (node && node->type == NodeType::Instrument
        && node->script.rfind(MultiSamplerDoc::kPrefix, 0) == 0) {
        auto* editor = new MultiSamplerEditorComponent(graph, node->id, audioEngine);
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(editor);
        opts.dialogTitle = "Sampler: " + juce::String(node->name);
        opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = false;
        opts.resizable = true;
        opts.componentToCentreAround = this;
        SoundShop::launchToolDialog(opts);
        return;
    }

    // Legacy single-sample Sampler editor - still supported for old
    // projects that haven't been upgraded yet. Normal load path converts
    // "__audio__:" into "__multisampler__:" so this only fires if
    // upgradeLegacyNodes() didn't run for some reason.
    if (node && node->type == NodeType::Instrument
        && node->script.rfind("__audio__:", 0) == 0) {
        auto* editor = new SamplerEditorComponent(graph, node->id, audioEngine);
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(editor);
        opts.dialogTitle = "Sampler (legacy): " + juce::String(node->name);
        opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = false;
        opts.resizable = true;
        opts.componentToCentreAround = this;
        SoundShop::launchToolDialog(opts);
        return;
    }

    // Convolution Filter: open the IR editor
    if (node && node->script.rfind("__convolution__:", 0) == 0) {
        auto* editor = new ConvolutionEditorComponent(graph, node->id, [this]() {
            audioEngine.getGraphProcessor().requestRebuild();
        });
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(editor);
        opts.dialogTitle = "Convolution: " + juce::String(node->name);
        opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = false;
        opts.resizable = true;
        opts.componentToCentreAround = this;
        SoundShop::launchToolDialog(opts);
        return;
    }

    // Spectrum Tap: open the frequency bin editor. Match by prefix since the
    // script may carry per-bin custom response curves after the tag.
    if (node && node->script.rfind("__spectrumtap__", 0) == 0) {
        auto* comp = new SpectrumTapComponent(graph, node->id, [this]() {
            audioEngine.getGraphProcessor().requestRebuild();
            graphComponent->repaint();
        });
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(comp);
        opts.dialogTitle = "Spectrum Tap: " + juce::String(node->name);
        opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = false;
        opts.resizable = true;
        opts.componentToCentreAround = this;
        SoundShop::launchToolDialog(opts);
        return;
    }

    // Audio analyzer nodes (Spectrum Analyzer, Oscilloscope, Spectrogram):
    // open the matching live visualizer in a non-modal floating window so
    // the user can keep the song playing while watching the display.
    if (node && (node->script == "__spectrumanalyzer__" ||
                 node->script == "__oscilloscope__" ||
                 node->script == "__spectrogram__")) {
        juce::Component* viz = nullptr;
        juce::String title;
        if (node->script == "__spectrumanalyzer__") {
            viz = new SpectrumAnalyzerComponent(graph, node->id);
            title = "Spectrum Analyzer: " + juce::String(node->name);
        } else if (node->script == "__oscilloscope__") {
            viz = new OscilloscopeComponent(graph, node->id);
            title = "Oscilloscope: " + juce::String(node->name);
        } else {
            viz = new SpectrogramComponent(graph, node->id);
            title = "Spectrogram: " + juce::String(node->name);
        }
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(viz);
        opts.dialogTitle = title;
        opts.dialogBackgroundColour = juce::Colour(18, 20, 28);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = false;
        opts.resizable = true;
        opts.componentToCentreAround = this;
        SoundShop::launchNonModalToolDialog(opts);
        return;
    }

    // XY Pad: reopen the pad window on double-click. Non-modal - a live input
    // surface must stay usable alongside the main window, the transport, and
    // other input-node editors while the song plays (matches the graph-view
    // open path). XYPadComponent is node-id-safe, so it survives node deletion.
    if (node && node->script == "__xypad__") {
        auto* pad = new XYPadComponent(graph, node->id);
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(pad);
        opts.dialogTitle = "XY Pad";
        opts.dialogBackgroundColour = juce::Colour(25, 25, 32);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = false;
        opts.resizable = true;
        opts.componentToCentreAround = this;
        if (auto* dlg = SoundShop::launchNonModalToolDialog(opts))
            dlg->setResizeLimits(320, 400, 6000, 6000);
        return;
    }

    // MIDI Modulator node: open its rule editor. Covers both the new
    // __midimod__ script marker and the legacy __velscale__ marker so old
    // projects get the new editor when the node is opened.
    if (node && node->type == NodeType::Effect
        && (node->script.rfind("__midimod__:", 0) == 0
            || node->script == "__velscale__")) {
        auto* editor = new MidiModEditorComponent(graph, node->id, [this]() {
            audioEngine.getGraphProcessor().requestRebuild();
            graphComponent->repaint();
        });
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(editor);
        opts.dialogTitle = "MIDI Modulator: " + juce::String(node->name);
        opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = false;
        opts.resizable = true;
        opts.componentToCentreAround = this;
        SoundShop::launchToolDialog(opts);
        return;
    }

    // Trigger node: open its rule editor.
    if (node && node->type == NodeType::Effect
        && node->script.rfind("__trigger__:", 0) == 0) {
        auto* editor = new TriggerEditorComponent(graph, node->id, [this]() {
            audioEngine.getGraphProcessor().requestRebuild();
        });
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(editor);
        opts.dialogTitle = "Trigger: " + juce::String(node->name);
        opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = false;
        opts.resizable = true;
        opts.componentToCentreAround = this;
        SoundShop::launchToolDialog(opts);
        return;
    }

    // Wavelet-space painter for nodes authored with a DWT coefficient grid.
    if (node && (node->type == NodeType::Instrument || node->type == NodeType::TerrainSynth)
        && !node->plugin && node->pluginIndex < 0
        && node->script.rfind("__waveletpaint__:", 0) == 0) {
        auto* editor = new WaveletPainterComponent(graph, node->id, [this]() {
            audioEngine.getGraphProcessor().requestRebuild();
        });
        // Use LaunchOptions + componentToCentreAround so the OS treats this as
        // a child of the main window. With useNativeTitleBar(true) and no
        // parent, Windows gives the dialog its own taskbar entry, which is
        // wrong for an editor sub-window.
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(editor);
        opts.dialogTitle = "Wavelet Space: " + juce::String(node->name);
        opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = false;
        opts.resizable = true;
        opts.componentToCentreAround = this;
        SoundShop::launchToolDialog(opts);
        return;
    }

    // Curve EQ editor: an Effect node whose script is a `__curveeq__:` curve.
    // Draws an arbitrary magnitude-response curve applied via block-local STFT.
    if (node && node->type == NodeType::Effect
        && node->script.rfind("__curveeq__:", 0) == 0) {
        auto* editor = new CurveEQEditorComponent(graph, node->id, [this]() {
            audioEngine.getGraphProcessor().requestRebuild();
        });
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(editor);
        opts.dialogTitle = "Curve EQ: " + juce::String(node->name);
        opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = false;
        opts.resizable = true;
        opts.componentToCentreAround = this;
        SoundShop::launchToolDialog(opts);
        return;
    }

    // Signal EQ editor: an Effect node whose script is "__signaleq__". Curve
    // points (each a peaking bell with a signal-modulatable Freq + Gain) are
    // dragged on a log-frequency / dB canvas.
    if (node && node->type == NodeType::Effect && node->script == "__signaleq__") {
        auto* editor = new SignalEQEditorComponent(graph, node->id, [this]() {
            audioEngine.getGraphProcessor().requestRebuild();
        });
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(editor);
        opts.dialogTitle = "Signal EQ: " + juce::String(node->name);
        opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = false;
        opts.resizable = true;
        opts.componentToCentreAround = this;
        SoundShop::launchToolDialog(opts);
        return;
    }

    // Spectral (frequency-domain) editor for nodes authored with a
    // mag/phase spectrum. Both the new `__spectral2__:` format and the
    // legacy `__spectral__:` format open the same editor; the editor
    // converts legacy data to the new format on save.
    if (node && (node->type == NodeType::Instrument || node->type == NodeType::TerrainSynth)
        && !node->plugin && node->pluginIndex < 0
        && (node->script.rfind("__spectral__:", 0) == 0
            || node->script.rfind("__spectral2__:", 0) == 0)) {
        auto* editor = new SpectralEditorComponent(graph, node->id, [this]() {
            audioEngine.getGraphProcessor().requestRebuild();
        });
        // Use LaunchOptions + componentToCentreAround so the OS treats this as
        // a child of the main window (no separate taskbar entry).
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(editor);
        opts.dialogTitle = "Frequency Domain: " + juce::String(node->name);
        opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = false;
        opts.resizable = true;
        opts.componentToCentreAround = this;
        SoundShop::launchToolDialog(opts);
        return;
    }

    // Standalone single-frame "frame synth" instrument nodes (#- the six
    // capture/synthesis frame types as their own node type). The script is a
    // __framesynth__ wrapper around a one-frame WavetableDoc; it opens the SAME
    // LayeredWaveEditorComponent, which detects the prefix and runs in FOCUSED
    // mode (no grid / library / Position morph - just the one frame's editor
    // plus Gain / Preview / Envelope / Morph). This must be checked before the
    // wavetable gate below: the wrapped body is itself a __wavetable5__ encode,
    // so without this earlier gate a frame synth would open as a full wavetable.
    if (node && (node->type == NodeType::Instrument || node->type == NodeType::TerrainSynth)
        && !node->plugin && node->pluginIndex < 0
        && node->script.rfind("__framesynth__:", 0) == 0) {
        auto* editor = new LayeredWaveEditorComponent(graph, node->id, [this]() {
            audioEngine.getGraphProcessor().requestRebuild();
        });
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(editor);
        opts.dialogTitle = "Instrument: " + juce::String(node->name);
        opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = false;
        opts.resizable = true;
        opts.componentToCentreAround = this;
        // Non-modal for the same drag-and-drop reason as the wavetable editor
        // (the focused editor still hosts library-capable sub-editors).
        SoundShop::launchNonModalToolDialog(opts);
        return;
    }

    // Layered waveform editor takes priority for nodes whose script is a
    // layered spec (single frame) or a wavetable spec (multi-frame). All
    // five formats open the same editor:
    //   __layered__:    single-frame legacy time-domain layered spec
    //   __wavetable__:  legacy multi-frame wavetable (v1)
    //   __wavetable2__: v2 multi-frame container (inline frame data per cell)
    //   __wavetable3__: v3 library + cell-by-reference format (pre-colorIdx)
    //   __wavetable4__: v4 library + cell-by-reference format with per-entry
    //                   colorIdx (no gain).
    //   __wavetable5__: current library + cell-by-reference format with
    //                   per-entry colorIdx AND per-frame gain. This is what
    //                   newly-created Wavetable nodes encode their script as
    //                   (defaultEmpty().encode()), so leaving it out of the
    //                   gate meant double-click on a freshly-added Wavetable
    //                   node fell through to the generic visualizer with no
    //                   way to reopen the wavetable editor.
    if (node && (node->type == NodeType::Instrument || node->type == NodeType::TerrainSynth)
        && !node->plugin && node->pluginIndex < 0
        && (node->script.rfind("__layered__:", 0) == 0
            || node->script.rfind("__wavetable__:", 0) == 0
            || node->script.rfind("__wavetable2__:", 0) == 0
            || node->script.rfind("__wavetable3__:", 0) == 0
            || node->script.rfind("__wavetable4__:", 0) == 0
            || node->script.rfind("__wavetable5__:", 0) == 0)) {
        auto* editor = new LayeredWaveEditorComponent(graph, node->id, [this]() {
            audioEngine.getGraphProcessor().requestRebuild();
        });
        // Non-modal (#17): use launchAsync which creates a non-blocking
        // window. The user can keep working in the graph while the
        // waveform editor is open. Closing the window destroys the
        // editor. Multiple editors for different nodes can coexist.
        // componentToCentreAround = this parents the dialog to the main
        // window so the OS doesn't give it a separate taskbar entry.
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(editor);
        opts.dialogTitle = "Wavetable: " + juce::String(node->name);
        opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = false;
        opts.resizable = true;
        opts.componentToCentreAround = this;
        // Single-window editor: arrangement view and per-waveform edit view
        // are both panels of the same editor (toggled via "Edit waveform" /
        // close in the per-waveform view).
        //
        // Non-modal launch (#17) is required, not just nice-to-have: the
        // arrangement view's library list uses JUCE DragAndDropContainer
        // to drop library entries onto grid cells / scatter positions. A
        // modal parent dialog blocks the DragImageComponent (which lives
        // on the desktop, outside the modal hierarchy) from receiving
        // mouseUp via the source-component listener forwarding chain, so
        // itemDropped never fires and the drag silently leaves a stranded
        // drag-image bitmap on the cell. launchNonModalToolDialog keeps
        // the delete-on-close behavior of launchToolDialog without
        // entering modal state.
        SoundShop::launchNonModalToolDialog(opts);
        return;
    }

    // Unified synth visualizer (both Instrument and TerrainSynth use TerrainSynthProcessor)
    if (node && (node->type == NodeType::Instrument || node->type == NodeType::TerrainSynth)
        && !node->plugin && node->pluginIndex < 0) {
        auto* proc = dynamic_cast<TerrainSynthProcessor*>(
            audioEngine.getGraphProcessor().getProcessorForNode(nodeId));
        if (proc) {
            auto* viz = new TerrainVisualizerComponent(*proc, *node);
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned(viz);
            opts.dialogTitle = "Terrain: " + juce::String(node->name);
            opts.dialogBackgroundColour = juce::Colour(25, 25, 30);
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar = false;
            opts.resizable = true;
            opts.componentToCentreAround = this;
            SoundShop::launchToolDialog(opts);
        }
        return;
    }

    // Regular plugin UI
    auto& gp = audioEngine.getGraphProcessor();
    auto& nodeMap = gp.getNodeMap();
    auto* juceGraph = gp.getGraph();
    if (!juceGraph) return;

    auto it = nodeMap.find(nodeId);
    if (it == nodeMap.end()) return;

    auto graphNode = juceGraph->getNodeForId(it->second);
    if (!graphNode || !graphNode->getProcessor()) return;

    auto name = node ? node->name : "Plugin";
    pluginWindows.showWindowFor(*graphNode->getProcessor(), name);
}

void MainContentComponent::showPluginInfo(int nodeId) {
    auto* node = graph.findNode(nodeId);
    if (!node || node->pluginIndex < 0) return;

    auto detail = audioEngine.getPluginHost().getPluginDetail(node->pluginIndex);
    auto& info = detail.info;

    juce::String text;
    text += "Name: " + juce::String(info.name) + "\n";
    text += "Manufacturer: " + juce::String(info.manufacturer) + "\n";
    text += "Format: " + juce::String(info.format) + "\n";
    text += "Version: " + juce::String(info.version) + "\n";
    text += "Category: " + juce::String(info.category) + "\n";
    text += "Instrument: " + juce::String(info.isInstrument ? "Yes" : "No") + "\n";
    text += "MIDI In: " + juce::String(detail.acceptsMidi ? "Yes" : "No") + "\n";
    text += "MIDI Out: " + juce::String(detail.producesMidi ? "Yes" : "No") + "\n";
    text += "Latency: " + juce::String(detail.latencySamples) + " samples\n";
    text += "Tail: " + juce::String(detail.tailSeconds) + " s\n";
    text += "\n";

    if (!detail.buses.empty()) {
        text += "--- Buses ---\n";
        for (auto& bus : detail.buses)
            text += juce::String(bus.isInput ? "  In:  " : "  Out: ")
                    + bus.name + " (" + juce::String(bus.channels) + " ch)\n";
        text += "\n";
    }

    if (!detail.params.empty()) {
        text += "--- Parameters (" + juce::String((int)detail.params.size()) + ") ---\n";
        for (int i = 0; i < (int)detail.params.size(); ++i) {
            auto& p = detail.params[i];
            text += "  [" + juce::String(i) + "] " + p.name;
            if (!p.label.empty()) text += " (" + juce::String(p.label) + ")";
            text += "  default=" + juce::String(p.defaultValue, 3);
            if (p.isAutomatable) text += "  [auto]";
            text += "\n";
        }
        text += "\n";
    }

    if (!detail.presets.empty()) {
        text += "--- Presets (" + juce::String((int)detail.presets.size()) + ") ---\n";
        for (int i = 0; i < std::min((int)detail.presets.size(), 20); ++i)
            text += "  " + juce::String(detail.presets[i]) + "\n";
        if (detail.presets.size() > 20)
            text += "  ... and " + juce::String((int)detail.presets.size() - 20) + " more\n";
    }

    text += "\nFile: " + juce::String(info.fileOrId) + "\n";

    // Show in a resizable dialog with a text editor
    auto* comp = new juce::Component();
    auto* editor = new juce::TextEditor();
    editor->setMultiLine(true);
    editor->setReadOnly(true);
    editor->setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 12.0f, 0));
    editor->setText(text);
    editor->setColour(juce::TextEditor::backgroundColourId, juce::Colour(30, 30, 35));
    comp->addAndMakeVisible(editor);
    comp->setSize(500, 450);
    editor->setBounds(0, 0, 500, 450);

    // Make editor resize with dialog
    struct ResizeHelper : public juce::ComponentListener {
        juce::TextEditor* ed;
        ResizeHelper(juce::TextEditor* e) : ed(e) {}
        void componentMovedOrResized(juce::Component& c, bool, bool resized) override {
            if (resized) ed->setBounds(c.getLocalBounds());
        }
    };
    auto* helper = new ResizeHelper(editor);
    comp->addComponentListener(helper);

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(comp);
    opts.dialogTitle = "Plugin Info: " + juce::String(info.name);
    opts.dialogBackgroundColour = juce::Colour(40, 40, 45);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = false;
    opts.resizable = true;
    opts.componentToCentreAround = this;
    SoundShop::launchToolDialog(opts);
}

void MainContentComponent::showPluginPresets(int nodeId) {
    auto* proc = audioEngine.getGraphProcessor().getProcessorForNode(nodeId);
    if (!proc) return;

    int numPresets = proc->getNumPrograms();
    int currentPreset = proc->getCurrentProgram();

    juce::PopupMenu menu;
    for (int i = 0; i < numPresets; ++i) {
        auto name = proc->getProgramName(i);
        if (name.isEmpty()) name = "Preset " + juce::String(i);
        menu.addItem(i + 1, name, true, i == currentPreset);
    }

    if (numPresets == 0)
        menu.addItem(-1, "(no presets)", false);

    menu.showMenuAsync(juce::PopupMenu::Options(), [proc, nodeId, this](int result) {
        if (result > 0) {
            proc->setCurrentProgram(result - 1);
            graph.dirty = true;
        }
    });
}

// ==============================================================================
// MIDI Map Dialog
// ==============================================================================

class MidiMapComponent : public juce::Component, public juce::Timer {
public:
    MidiMapComponent(int nodeId, juce::AudioProcessor* proc,
                     AutomationManager& automation,
                     AudioEngine::MidiLearnState& midiLearn)
        : nodeId(nodeId), proc(proc), automation(automation), midiLearn(midiLearn) {

        if (!proc) { setSize(300, 100); return; }

        auto& params = proc->getParameters();
        constexpr int kMaxDisplayParams = 256;
        for (int i = 0; i < std::min((int)params.size(), kMaxDisplayParams); ++i) {
            auto name = params[i]->getName(128);
            if (name.isEmpty()) name = "Param " + juce::String(i);
            paramNames.push_back(name);
        }

        addAndMakeVisible(paramList);
        listModel.parent = this;
        paramList.setModel(&listModel);
        paramList.setRowHeight(28);

        addAndMakeVisible(statusLabel);
        statusLabel.setColour(juce::Label::textColourId, juce::Colours::yellow);
        statusLabel.setFont(juce::Font(12.0f));

        startTimerHz(15);
        setSize(450, std::min(500, 50 + (int)paramNames.size() * 28));
    }

    ~MidiMapComponent() override {
        midiLearn.active.store(false);
        paramList.setModel(nullptr);
    }

    void resized() override {
        auto area = getLocalBounds().reduced(6);
        statusLabel.setBounds(area.removeFromBottom(22));
        paramList.setBounds(area);
    }

    void timerCallback() override {
        if (learningParamIdx >= 0 && midiLearn.active.load()) {
            int cc = midiLearn.lastCC.load();
            int ch = midiLearn.lastChannel.load();
            if (cc >= 0 && ch >= 0) {
                // CC captured - create mapping
                midiLearn.active.store(false);

                // Remove any existing mapping for this CC
                automation.removeCCMapping(ch, cc);
                // Remove any existing mapping for this param on this node
                auto mappings = automation.getCCMappings();
                for (auto& m : mappings)
                    if (m.nodeId == nodeId && m.paramIdx == learningParamIdx)
                        automation.removeCCMapping(m.midiChannel, m.ccNumber);

                CCMapping mapping;
                mapping.midiChannel = ch;
                mapping.ccNumber = cc;
                mapping.nodeId = nodeId;
                mapping.paramIdx = learningParamIdx;
                mapping.minValue = 0.0f;
                mapping.maxValue = 1.0f;
                automation.addCCMapping(mapping);

                statusLabel.setText("Mapped CC " + juce::String(cc) + " (ch " +
                                    juce::String(ch) + ") -> " +
                                    paramNames[learningParamIdx],
                                    juce::dontSendNotification);
                learningParamIdx = -1;
                paramList.repaint();
            }
        }
    }

    // ListBox delegate (inline - small enough)
    struct ListModel : public juce::ListBoxModel {
        MidiMapComponent* parent = nullptr;
        int getNumRows() override { return parent ? (int)parent->paramNames.size() : 0; }
        void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override {
            if (!parent || row < 0 || row >= (int)parent->paramNames.size()) return;
            if (selected) g.fillAll(juce::Colour(50, 70, 100));

            // Parameter name
            g.setColour(juce::Colours::white);
            g.setFont(12.0f);
            g.drawText(juce::String("[") + juce::String(row) + "] " + parent->paramNames[row],
                        4, 0, w / 2 - 4, h, juce::Justification::centredLeft);

            // Current CC assignment
            auto mappings = parent->automation.getCCMappings();
            juce::String ccText = "-";
            for (auto& m : mappings)
                if (m.nodeId == parent->nodeId && m.paramIdx == row)
                    ccText = "CC " + juce::String(m.ccNumber) + " (ch " + juce::String(m.midiChannel) + ")";

            if (parent->learningParamIdx == row)
                ccText = "Waiting...";

            g.setColour(ccText == "-" ? juce::Colours::grey
                        : parent->learningParamIdx == row ? juce::Colours::yellow
                        : juce::Colours::limegreen);
            g.drawText(ccText, w / 2, 0, w / 2 - 80, h, juce::Justification::centredLeft);
        }
        void listBoxItemClicked(int row, const juce::MouseEvent& e) override {
            if (!parent || !e.mods.isRightButtonDown()) return;
            auto mappings = parent->automation.getCCMappings();
            bool hasMapped = false;
            for (auto& m : mappings)
                if (m.nodeId == parent->nodeId && m.paramIdx == row) hasMapped = true;

            juce::PopupMenu menu;
            menu.addItem(1, "Learn");
            if (hasMapped) menu.addItem(2, "Remove Mapping");

            int nodeId = parent->nodeId;
            int paramIdx = row;
            auto* p = parent;
            menu.showMenuAsync(juce::PopupMenu::Options(), [p, nodeId, paramIdx](int result) {
                if (result == 1) {
                    p->learningParamIdx = paramIdx;
                    p->midiLearn.lastCC.store(-1);
                    p->midiLearn.lastChannel.store(-1);
                    p->midiLearn.active.store(true);
                    p->statusLabel.setText("Move a knob/slider on your MIDI controller...",
                                            juce::dontSendNotification);
                    p->paramList.repaint();
                } else if (result == 2) {
                    auto mappings = p->automation.getCCMappings();
                    for (auto& m : mappings)
                        if (m.nodeId == nodeId && m.paramIdx == paramIdx)
                            p->automation.removeCCMapping(m.midiChannel, m.ccNumber);
                    p->paramList.repaint();
                }
            });
        }
    };

private:
    int nodeId;
    juce::AudioProcessor* proc;
    AutomationManager& automation;
    AudioEngine::MidiLearnState& midiLearn;

    std::vector<juce::String> paramNames;
    int learningParamIdx = -1;

    ListModel listModel;
    struct ParamListBox : public juce::ListBox {
        void setModel(juce::ListBoxModel* m) { juce::ListBox::setModel(m); }
    };
    juce::ListBox paramList{"Parameters"};
    juce::Label statusLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiMapComponent)
};

void MainContentComponent::showMidiMap(int nodeId) {
    auto* proc = audioEngine.getGraphProcessor().getProcessorForNode(nodeId);
    if (!proc) return;

    auto* comp = new MidiMapComponent(nodeId, proc,
        audioEngine.getGraphProcessor().getAutomation(),
        audioEngine.midiLearn);

    auto* node = graph.findNode(nodeId);
    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(comp);
    opts.dialogTitle = "MIDI Map: " + juce::String(node ? node->name : "Plugin");
    opts.dialogBackgroundColour = juce::Colour(40, 40, 45);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = false;
    opts.resizable = true;
    opts.componentToCentreAround = this;
    SoundShop::launchToolDialog(opts);
}

void MainContentComponent::onPlay() {
    // If the playhead is parked at or past the end of the song (e.g. a previous
    // play-once playthrough finished and left the playhead at the end), restart
    // from the top. Otherwise Play would resume past the last note and produce
    // silence until a loop wrapped the playhead back to the start.
    double endBeat = graph.effectiveSongLengthBeats();
    if (endBeat > 0.0 && transport.positionBeats() >= endBeat - 1e-6)
        audioEngine.rewindToStart();

    transport.playing = true;
    audioEngine.play();
    playBtn.setButtonText("Pause");
}

void MainContentComponent::onStop() {
    // Stop recording if active
    if (audioEngine.getRecordingManager().isRecording()) {
        int nodeId = audioEngine.getRecordingManager().getRecordingNodeId();
        auto* node = graph.findNode(nodeId);
        if (node) audioEngine.getRecordingManager().stopRecording(*node, transport);
        recordBtn.setButtonText("Play & Record");
    }
    // Stop multi-track recording
    if (audioEngine.getMultitrackRecorder().isRecording()) {
        audioEngine.getMultitrackRecorder().stopRecording(
            graph, transport, audioEngine.getSampleRate());
        recordBtn.setButtonText("Play & Record");
        audioEngine.getGraphProcessor().requestRebuild();
    }
    // Stop MIDI recording
    if (audioEngine.isMidiRecording()) {
        audioEngine.stopMidiRecording();
        recordBtn.setButtonText("Play & Record");
    }
    // Stop automation recording
    audioEngine.disarmAutomationRecording();

    transport.playing = false;
    audioEngine.stop();
    playBtn.setButtonText("Play");
}

void MainContentComponent::beginAutomationPass() {
    // A new playback pass begins. Arm every param that RESOLVES to Write so it
    // overwrites its whole pass at its current value whether or not the user
    // touches it (the "flatten to static" mode - node/param scope only, so this
    // can never wipe every lane in the project). Touch/Latch params arm later,
    // on their gesture begin, via handleParamGesture(). Reset per-param sweep
    // state for a clean pass either way.
    for (auto& node : graph.nodes) {
        for (int pi = 0; pi < (int)node.params.size(); ++pi) {
            auto& p = node.params[pi];
            p.recLastBeat = -1.0f;
            p.recDidWrite = false;
            AutoArmMode m = resolveArmMode(graph.autoArmGlobal, node, p);
            bool absLocked = p.modulated && graph.paramHasAbsoluteInput(node.id, pi);
            p.recWriting = (m == AutoArmMode::Write) && !absLocked;
        }
    }
}

void MainContentComponent::endAutomationPass() {
    // The pass ended (Stop, pause, or programmatic song-end). Thin every lane we
    // recorded into (collinear reduction collapses a static Write to two
    // endpoints while keeping the shape of a genuine sweep), then commit ONE
    // undo snapshot for the whole pass - consistent with SEANCE's gesture-
    // endpoint undo policy. Clear all transient recording state.
    bool anyRecorded = false;
    for (auto& node : graph.nodes) {
        for (auto& p : node.params) {
            if (p.recDidWrite) {
                float eps = (p.maxVal - p.minVal) * 0.005f;
                if (eps <= 0.0f) eps = 1e-4f;
                simplifyAutomationLane(p.automation, eps);
                anyRecorded = true;
            }
            p.recWriting = false;
            p.recLastBeat = -1.0f;
            p.recDidWrite = false;
        }
    }
    if (anyRecorded) {
        graph.dirty = true;
        projectDirty = true;
        graph.commitSnapshot("Record automation");
    }
}

void MainContentComponent::handleParamGesture(int nodeId, int paramIdx, bool begin) {
    // A user grabbed (begin) or released (end) a param control. Only Touch/Latch
    // recording is gesture-driven (Write arms at play-start); and only while the
    // transport is actually rolling. The per-tick point-writing happens in
    // timerCallback() - here we just flip recWriting.
    if (!transport.playing) return;
    auto* node = graph.findNode(nodeId);
    if (!node || paramIdx < 0 || paramIdx >= (int)node->params.size()) return;
    auto& p = node->params[paramIdx];
    // Absolute-cable params are cable-driven; never hand-record them.
    if (p.modulated && graph.paramHasAbsoluteInput(nodeId, paramIdx)) return;
    AutoArmMode m = resolveArmMode(graph.autoArmGlobal, *node, p);
    if (begin) {
        if (m == AutoArmMode::Touch || m == AutoArmMode::Latch) {
            p.recWriting = true;
            p.recLastBeat = -1.0f; // fresh sweep segment (punch-in)
        }
    } else {
        // Release. Touch stops writing and ends the segment (so the untouched
        // gap before the next punch-in keeps its existing automation). Latch
        // keeps writing the held value until the pass ends. Write (if this
        // param resolved to it) is unaffected - it stops only at pass end.
        if (m == AutoArmMode::Touch) {
            p.recWriting = false;
            p.recLastBeat = -1.0f;
        }
    }
}

void MainContentComponent::syncAutoButton() {
    // Armed (Touch/Latch) uses a red accent to read as "recording-enabled",
    // distinct from the blue transport accent on Play/Loop.
    static const juce::Colour kAutoArmedColour(200, 60, 60);
    const char* label = "Auto: Off";
    bool armed = false;
    switch (graph.autoArmGlobal) {
        case AutoArmMode::Touch: label = "Auto: Touch"; armed = true; break;
        case AutoArmMode::Latch: label = "Auto: Latch"; armed = true; break;
        default: break; // Off (Inherit/Write never appear at global scope)
    }
    autoBtn.setButtonText(label);
    if (armed)
        autoBtn.setColour(juce::TextButton::buttonColourId, kAutoArmedColour);
    else
        autoBtn.removeColour(juce::TextButton::buttonColourId);
}

void MainContentComponent::onRecord() {
    // Find which node to record into. Priority:
    //  1. Any node explicitly armed via "Record Here" (#77)
    //  2. The active editor node (the one whose piano roll is open)
    //  3. Fallback: first MIDI timeline, then first audio timeline
    Node* recordNode = nullptr;

    // 1. Prefer explicitly armed nodes (MIDI timelines).
    for (auto& n : graph.nodes) {
        if (n.recordArmed && n.type == NodeType::MidiTimeline) {
            recordNode = &n;
            break;
        }
    }

    // 2. Active editor node.
    if (!recordNode && graph.activeEditorNodeId >= 0)
        recordNode = graph.findNode(graph.activeEditorNodeId);

    // 3. Fallback: first MIDI timeline, then audio timeline.
    if (!recordNode) {
        for (auto& n : graph.nodes) {
            if (n.type == NodeType::MidiTimeline) { recordNode = &n; break; }
        }
    }
    if (!recordNode) {
        for (auto& n : graph.nodes) {
            if (n.type == NodeType::AudioTimeline) { recordNode = &n; break; }
        }
    }

    if (!recordNode) {
        // Create a MIDI track
        auto& n = graph.addNode("MIDI Track", NodeType::MidiTimeline,
            {Pin{0, "MIDI In", PinKind::Midi, true}},
            {Pin{0, "MIDI", PinKind::Midi, false}}, {50, 50});
        n.clips.push_back({"Clip 1", 0, 8, juce::Colours::cornflowerblue.getARGB()});
        recordNode = &n;
        graphComponent->repaint();
    }

    // Check if any Audio Tracks are armed for multi-track recording
    bool anyArmed = false;
    for (auto& n : graph.nodes)
        if (n.type == NodeType::AudioTimeline && n.recordArmed) { anyArmed = true; break; }

    if (anyArmed) {
        // Multi-track recording: all armed Audio Tracks record simultaneously
        auto outputDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                             .getParentDirectory().getChildFile("recordings").getFullPathName().toStdString();
        // Sync the inputChannel param to the node field for the recorder
        for (auto& n : graph.nodes) {
            if (n.type != NodeType::AudioTimeline) continue;
            for (auto& p : n.params)
                if (p.name == "Input Channel") { n.recordInputChannel = (int)p.value; break; }
        }
        audioEngine.getMultitrackRecorder().startRecording(
            graph, transport, audioEngine.getSampleRate(), outputDir);
    }

    if (recordNode->type == NodeType::MidiTimeline) {
        // MIDI note recording
        audioEngine.startMidiRecording(recordNode->id);
        audioEngine.armAutomationRecording();
    } else if (recordNode->type == NodeType::AudioTimeline && !anyArmed) {
        // Single-track audio recording (legacy, when no tracks are armed)
        auto outputDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                             .getParentDirectory().getChildFile("recordings").getFullPathName().toStdString();
        audioEngine.getRecordingManager().startRecording(
            *recordNode, 2, audioEngine.getSampleRate(), outputDir);
    }

    // Start playback
    onPlay();
    recordBtn.setButtonText("Recording...");
}

void MainContentComponent::newProject() {
    editorPanels.clear();
    editorPanelHeight = 250;
    // Clearing + rebuilding the graph is a structural mutation the audio
    // callback can race against (it iterates graph.nodes/links under a
    // try-lock). Pair the lock here, matching the project-load path. See the
    // mutationLock comment in node_graph.h.
    {
        std::lock_guard<std::recursive_mutex> graphLk(graph.mutationLock);
        graph.nodes.clear();
        graph.links.clear();
        graph.openEditors.clear();
        graph.setupDefaultGraph();
    }

    // Auto-create MidiInput nodes for all currently connected hardware
    // MIDI devices - so the user's controller is immediately wired and
    // ready on a fresh project without needing a wizard. The Computer
    // Keyboard node is already created by setupDefaultGraph(); this
    // adds hardware devices alongside it.
    {
        // Same structural-mutation lock as the clear/rebuild above: addNode()
        // below can reallocate graph.nodes while the audio callback iterates.
        std::lock_guard<std::recursive_mutex> graphLk(graph.mutationLock);

        auto devices = juce::MidiInput::getAvailableDevices();
        // Resolve the default MIDI track's MIDI input PIN ID up front. Pin IDs
        // are stable across vector reallocation, so we never hold a Node* /
        // Pin* across the addNode() calls below (which can reallocate
        // graph.nodes and dangle such pointers - the dangling-reference
        // anti-pattern CLAUDE.md forbids).
        int defaultTrackMidiPinId = -1;
        for (auto& n : graph.nodes)
            if (n.type == NodeType::MidiTimeline) {
                for (auto& pin : n.pinsIn)
                    if (pin.kind == PinKind::Midi) { defaultTrackMidiPinId = pin.id; break; }
                break;
            }

        float yPos = 200; // stagger below the Computer Keyboard node
        for (auto& dev : devices) {
            // Skip virtual / control-surface ports (SSL V-MIDI, Mackie, etc.)
            if (isVirtualOrControlPort(dev.name)) continue;

            // Skip if this identifier is already in the graph.
            bool exists = false;
            for (auto& n : graph.nodes)
                if (n.type == NodeType::MidiInput && n.midiInputSourceId == dev.identifier.toStdString())
                    { exists = true; break; }
            if (exists) continue;

            auto& n = graph.addNode(dev.name.toStdString(), NodeType::MidiInput,
                {}, {Pin{0, "MIDI Out", PinKind::Midi, false}}, {80, yPos});
            n.midiInputSourceId = dev.identifier.toStdString();
            int outPinId = n.pinsOut.empty() ? -1 : n.pinsOut[0].id;

            // Wire to the default track so the device plays immediately. Both
            // endpoints are referenced by stable pin ID, not by pointer.
            if (defaultTrackMidiPinId >= 0 && outPinId >= 0)
                graph.addLink(outPinId, defaultTrackMidiPinId);
            yPos += 50;
        }
    }

    ProjectFile::currentPath.clear();
    projectDirty = false;
    graph.dirty = false;
    discardAutosave();
    discardUndoTreePersist();
    lastAutosaveAttemptMs = juce::Time::getMillisecondCounterHiRes();
    resized();
    graphComponent->fitAll();
    graphComponent->repaint();
}

void MainContentComponent::openHelpDoc(const juce::String& docRelativePath) {
    auto exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                      .getParentDirectory();
    auto docFile = exeDir.getChildFile("docs").getChildFile(docRelativePath);
    if (!docFile.existsAsFile()) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            "Help file not found",
            "Couldn't find the docs file:\n\n  " + docFile.getFullPathName()
            + "\n\nThe docs folder should sit alongside SoundShop.exe. "
            + "If you built from source, re-run the build to copy the docs, "
            + "or browse the project's docs/ folder directly.");
        return;
    }
    // startAsProcess opens the file in its default OS handler - for .html
    // files that's the user's browser.
    docFile.startAsProcess();
}

void MainContentComponent::showMidiDeviceWizard() {
    auto* wizard = new MidiDeviceWizardComponent(graph, audioEngine, [this]() {
        audioEngine.getGraphProcessor().requestRebuild();
        graphComponent->repaint();
    });
    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(wizard);
    opts.dialogTitle = "Add MIDI Input Devices";
    opts.dialogBackgroundColour = juce::Colour(28, 28, 36);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = false;
    opts.resizable = false;
    opts.componentToCentreAround = this;
    SoundShop::launchToolDialog(opts);
}

void MainContentComponent::openProject() {
    auto chooser = std::make_shared<juce::FileChooser>("Open Project", juce::File(), "*.ssp");
    chooser->launchAsync(juce::FileBrowserComponent::openMode, [this, chooser](const juce::FileChooser& fc) {
        auto file = fc.getResult();
        if (file.existsAsFile())
            openProjectFile(file.getFullPathName());
    });
}

void MainContentComponent::upgradeLegacyNodes() {
    // Upgrade legacy-format nodes in place. Runs on every project load
    // (startup autoload, File -> Open, crash recovery) so users always
    // get the latest behavior even from old .ssp files.

    // MidiInput node type predates the old "keyboard-is-a-flag" model.
    // If no "Computer Keyboard" MidiInput node exists, add one so
    // typing still reaches synths via the new routing model.
    bool hasKbdInput = false;
    for (auto& n : graph.nodes)
        if (n.type == NodeType::MidiInput && n.midiInputSourceId == "keyboard") {
            hasKbdInput = true; break;
        }
    if (!hasKbdInput) {
        auto& keyIn = graph.addNode("Computer Keyboard", NodeType::MidiInput,
            {}, {Pin{0, "MIDI Out", PinKind::Midi, false}}, {80, 80});
        keyIn.midiInputSourceId = "keyboard";
    }

    for (auto& n : graph.nodes) {
        // Add a MIDI In pin to any MidiTimeline that's missing one.
        if (n.type == NodeType::MidiTimeline) {
            bool hasMidiIn = false;
            for (auto& p : n.pinsIn)
                if (p.kind == PinKind::Midi) { hasMidiIn = true; break; }
            if (!hasMidiIn)
                n.pinsIn.insert(n.pinsIn.begin(),
                    {graph.allocId(), "MIDI In", PinKind::Midi, true});
        }

        // Legacy "Reverb" stub: Effect node with name "Reverb" and no
        // script. Upgrade to the new algorithmic reverb by attaching the
        // "__reverb__" script and the default param set so the real DSP
        // takes over. Preserves the node's ID and position so existing
        // cables still connect to the right node.
        // Legacy "EQ" stub -> real parametric EQ.
        if (n.type == NodeType::Effect && n.name == "EQ" && n.script.empty()) {
            n.script = "__eq__";
            if (n.params.empty()) {
                n.params.push_back({"B1 Type", 3.0f, 0.0f, 4.0f});
                n.params.push_back({"B1 Freq", 80.0f, 20.0f, 20000.0f});
                n.params.push_back({"B1 Gain", 0.0f, -24.0f, 24.0f});
                n.params.push_back({"B1 Q", 0.707f, 0.1f, 10.0f});
                n.params.push_back({"B2 Type", 0.0f, 0.0f, 4.0f});
                n.params.push_back({"B2 Freq", 400.0f, 20.0f, 20000.0f});
                n.params.push_back({"B2 Gain", 0.0f, -24.0f, 24.0f});
                n.params.push_back({"B2 Q", 0.707f, 0.1f, 10.0f});
                n.params.push_back({"B3 Type", 0.0f, 0.0f, 4.0f});
                n.params.push_back({"B3 Freq", 2500.0f, 20.0f, 20000.0f});
                n.params.push_back({"B3 Gain", 0.0f, -24.0f, 24.0f});
                n.params.push_back({"B3 Q", 0.707f, 0.1f, 10.0f});
                n.params.push_back({"B4 Type", 4.0f, 0.0f, 4.0f});
                n.params.push_back({"B4 Freq", 8000.0f, 20.0f, 20000.0f});
                n.params.push_back({"B4 Gain", 0.0f, -24.0f, 24.0f});
                n.params.push_back({"B4 Q", 0.707f, 0.1f, 10.0f});
            }
        }

        if (n.type == NodeType::Effect && n.name == "Reverb" && n.script.empty()) {
            n.script = "__reverb__";
            if (n.params.empty()) {
                n.params.push_back({"Mix",       0.3f,  0.0f, 1.0f});
                n.params.push_back({"Size",      0.6f,  0.0f, 1.0f});
                n.params.push_back({"Damping",   0.5f,  0.0f, 1.0f});
                n.params.push_back({"Width",     1.0f,  0.0f, 1.0f});
                n.params.push_back({"Pre-Delay", 0.0f,  0.0f, 200.0f});
            }
        }

        // Legacy single-sample Sampler (TerrainSynth + "__audio__:" path).
        // The old Sampler was a subset of MultiSampler's capabilities -
        // upgrade it in place to a one-zone MultiSampler pointing at the
        // same WAV file. Preserves the node's ID, position, name, and
        // existing cables, and picks up the old node's ADSR / Base Note /
        // Fine Tune settings if present in the param list.
        if (n.type == NodeType::Instrument
            && n.script.rfind("__audio__:", 0) == 0) {
            auto path = n.script.substr(10);
            MultiSamplerDoc doc;
            MultiSamplerZone z;
            z.samplePath = path;
            z.loNote = 0; z.hiNote = 127;
            z.loVel = 1; z.hiVel = 127;
            z.baseNote = 69; // matches old Sampler default (A4)
            // Salvage structured fields from the old flat param list.
            float salvagedVolume = 0.5f;
            float salvagedPan    = 0.0f;
            bool  foundVolume = false, foundPan = false;
            for (auto& p : n.params) {
                if      (p.name == "Base Note") z.baseNote      = (int)p.value;
                else if (p.name == "Fine Tune") z.fineTuneCents = p.value;
                else if (p.name == "Volume")    { salvagedVolume = p.value; foundVolume = true; }
                else if (p.name == "Pan")       { salvagedPan    = p.value; foundPan    = true; }
                else if (p.name == "Attack")    doc.attack       = p.value;
                else if (p.name == "Decay")     doc.decay        = p.value;
                else if (p.name == "Sustain")   doc.sustain      = p.value;
                else if (p.name == "Release")   doc.release      = p.value;
            }
            doc.zones.push_back(z);
            n.script = doc.encode();
            // Strip the old flat param list (it mixed TerrainSynth-
            // specific knobs with the sampler ones we salvaged).
            n.params.clear();
            // Keep Volume and Pan as real params - MultiSampler reads
            // them at block time so automation lanes and Signal cables
            // work against them.
            n.params.push_back({"Volume", foundVolume ? salvagedVolume : 0.5f,  0.0f, 1.0f});
            n.params.push_back({"Pan",    foundPan    ? salvagedPan    : 0.0f, -1.0f, 1.0f});
            // Also remove the legacy Signal pins the old Sampler had
            // (Sig X, Sig Y) - MultiSampler uses script-embedded
            // envelopes instead.
            n.pinsIn.erase(std::remove_if(n.pinsIn.begin(), n.pinsIn.end(),
                [](const Pin& p) {
                    return p.kind == PinKind::Signal &&
                           (p.name == "Sig X" || p.name == "Sig Y");
                }), n.pinsIn.end());
        }
    }
}

void MainContentComponent::openProjectFile(const juce::String& path) {
    editorPanels.clear();
    // Hold the graph mutation lock for the load + legacy-node fixup.
    // ProjectFile::load clears graph.nodes/links and rebuilds them from the
    // file - same batch-mutation race surface as MOD import. See the
    // mutationLock comment in node_graph.h.
    // Collect any factory-waveform references that fail to resolve against this
    // build's WaveformBank (e.g. the project was saved by a newer SEANCE whose
    // waveforms.bin added cycles this build doesn't have). resolveFactoryRef()
    // silences such layers; without this warning the song would be untrue to
    // the original with no indication. We warn after the load completes.
    FactoryRefResolutionScope factoryRefScope;
    {
        std::lock_guard<std::recursive_mutex> graphLk(graph.mutationLock);
        // Pass nullptr for the plugin host so ProjectFile::load does NOT
        // instantiate plugins synchronously on this (locked, message-thread)
        // critical path. Plugin nodes are parsed with their pendingPluginState
        // intact; beginAsyncPluginLoad() (below) instantiates them serially off
        // the lock so the nodes appear immediately and the UI stays responsive.
        ProjectFile::load(path.toStdString(), graph, nullptr);
        rehydrateNodeCaches(path);
        upgradeLegacyNodes();

        // Embed baked formula cycles for old projects (#crash-python314). A
        // Lua/Python/GLSL Formula layer in a Terrain Synth (__layered__) or
        // Signal Shape script used to be re-baked by re-running the interpreter
        // whenever the audio thread rebuilt the processor - which crashes deep
        // in python3xx.dll because the CPython interpreter is message-thread
        // only. encodeLayer now embeds the baked cycle ("bake=" field) so the
        // audio thread never needs an interpreter, but projects saved before
        // that change have no embedded cycle. Re-bake here (we're on the message
        // thread, holding mutationLock so the audio thread is parked) and
        // re-encode so the embed is present before the graph goes live. New
        // edits already save with the embed, so this only ever rewrites old
        // files. See rebakeFormula / migrateLayeredScriptEmbedBake.
        for (auto& node : graph.nodes) {
            std::string script = node.script;
            if (script.rfind("__layered__:", 0) == 0) {
                if (migrateLayeredScriptEmbedBake(script))
                    setNodeScriptSynced(node, script);
            } else if (SignalShapeDoc::isSignalShapeScript(script)) {
                SignalShapeDoc d;
                if (d.decode(script) && d.layers.decodedNeedsBakeEmbed)
                    setNodeScriptSynced(node, d.encode());
            }
        }
    }

    auto editorIds = graph.openEditors;
    graph.openEditors.clear();
    for (int id : editorIds)
        if (auto* node = graph.findNode(id))
            openEditor(*node);

    addToRecentProjects(path);
    // Loading a clean project on top of whatever was in memory invalidates
    // any autosave that was tracking the previous state. The undo history
    // also no longer applies - its snapshots described the old graph.
    projectDirty = false;
    graph.dirty = false;
    discardAutosave();
    discardUndoTreePersist();
    lastAutosaveAttemptMs = juce::Time::getMillisecondCounterHiRes();
    // Restore the saved pan/zoom from the loaded project (or fit-all if
    // none was persisted). Replaces the unconditional fitAll() that used
    // to clobber the user's last view on every load.
    graphComponent->notifyProjectLoaded();
    graphComponent->repaint();

    // Kick off serial async plugin instantiation. Nodes are already on screen;
    // each plugin loads one at a time off this critical path, with a per-node
    // loading badge. Save/Save As stay greyed until the queue drains.
    beginAsyncPluginLoad();

    // Shared-history handling (#90): check for a sidecar and, if it
    // hasn't been seen by this user before, show the 3-option prompt.
    handleSharedHistoryOnOpen(path);

    // Warn if the project referenced built-in factory waveforms this build's
    // WaveformBank doesn't have (typically a project saved by a newer SEANCE).
    // Those layers were silenced on load; surfacing the names lets the user
    // know the song won't sound exactly as authored.
    if (!factoryRefScope.unresolved.empty()) {
        juce::StringArray names;
        for (const auto& n : factoryRefScope.unresolved)
            names.add(juce::String(n));
        int extra = 0;
        const int kMaxListed = 12;
        if (names.size() > kMaxListed) {
            extra = names.size() - kMaxListed;
            names.removeRange(kMaxListed, extra);
        }
        juce::String msg =
            "This project references " + juce::String(factoryRefScope.unresolved.size()) +
            (factoryRefScope.unresolved.size() == 1
                 ? " built-in waveform that isn't in this version of SEANCE:\n\n"
                 : " built-in waveforms that aren't in this version of SEANCE:\n\n") +
            names.joinIntoString("\n");
        if (extra > 0)
            msg += "\n+ " + juce::String(extra) + " more";
        msg += "\n\nThis usually means the project was saved with a newer version of "
               "SEANCE that added these waveforms. The affected layers were silenced, "
               "so the song may not sound exactly as it was authored. Updating SEANCE "
               "should restore them.";
        juce::NativeMessageBox::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle("Missing built-in waveforms")
                .withMessage(msg)
                .withButton("OK")
                .withAssociatedComponent(this),
            nullptr);
    }
}

namespace {
// A sink AudioProcessor added to the offline render graph as an extra fan-out
// from a target node's output. It records every sample it receives so, after a
// single full-project render, each armed node's *own* output is captured in its
// tap - correctly isolating the node's signal instead of the full mix. This is
// what makes batch freeze one render pass for N nodes AND fixes the old
// single-node freeze bug (which stored the whole output mix).
class FreezeTapProcessor : public juce::AudioProcessor {
public:
    FreezeTapProcessor()
        : juce::AudioProcessor(BusesProperties().withInput(
              "Tap", juce::AudioChannelSet::stereo(), true)) {}
    const juce::String getName() const override { return "Freeze Tap"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void reserveSamples(int64_t n) {
        left.reserve((size_t)std::max<int64_t>(0, n));
        right.reserve((size_t)std::max<int64_t>(0, n));
    }
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        const int n = buf.getNumSamples();
        const int ch = buf.getNumChannels();
        const float* l = ch > 0 ? buf.getReadPointer(0) : nullptr;
        const float* r = ch > 1 ? buf.getReadPointer(1) : l;
        for (int s = 0; s < n; ++s) {
            left.push_back(l ? l[s] : 0.0f);
            right.push_back(r ? r[s] : 0.0f);
        }
    }
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    std::vector<float> left, right;
};
} // namespace

void MainContentComponent::freezeNode(int nodeId) {
    freezeNodes({ nodeId });
}

void MainContentComponent::freezeNodes(const std::vector<int>& nodeIds) {
    // Resolve targets: existing, non-Output nodes only (the Output sink is never
    // cached - it's the mix bus). De-dupe.
    std::vector<int> targets;
    for (int id : nodeIds) {
        auto* n = graph.findNode(id);
        if (!n || n->type == NodeType::Output) continue;
        if (std::find(targets.begin(), targets.end(), id) == targets.end())
            targets.push_back(id);
    }
    if (targets.empty()) return;

    // Project length in beats (+ a tail so release/reverb aren't chopped).
    float maxBeat = 0;
    for (auto& n : graph.nodes)
        for (auto& c : n.clips)
            maxBeat = std::max(maxBeat, c.startBeat + c.lengthBeats);
    if (maxBeat <= 0) maxBeat = 4;
    maxBeat += 4;

    double sr = audioEngine.getSampleRate();
    if (sr <= 0) sr = 48000;
    const int blockSize = 512;

    double totalSeconds = transport.tempoMap.beatsToSeconds(maxBeat);
    int64_t totalSamples = (int64_t)(totalSeconds * sr);
    if (totalSamples <= 0) return;

    Transport offlineTransport;
    offlineTransport.bpm = graph.bpm;
    offlineTransport.tempoMap = transport.tempoMap;
    offlineTransport.sampleRate = sr;
    offlineTransport.playing = true;

    // Clear each target's existing cache so the offline render re-computes it
    // live (otherwise a stale freeze would feed its own cache back into the tap).
    for (int id : targets) {
        auto* n = graph.findNode(id);
        n->cache.valid = false;
        n->cache.enabled = false;
    }

    GraphProcessor offlineGP;
    offlineGP.prepare(graph, sr, blockSize);
    offlineGP.rebuildGraph(graph, offlineTransport);

    auto* jg = offlineGP.getGraph();
    if (!jg) return;
    const auto& nodeMap = offlineGP.getNodeMap();

    // Add one tap per target, wired as an extra fan-out from the target's output
    // (nodeMap = the OUTPUT side, i.e. the pan node when one exists). Keep raw
    // pointers to the taps so we can read them back after the render.
    std::vector<std::pair<int, FreezeTapProcessor*>> taps; // (nodeId, tap)
    for (int id : targets) {
        auto it = nodeMap.find(id);
        if (it == nodeMap.end()) continue;
        auto srcJuceId = it->second;
        auto* srcNode = jg->getNodeForId(srcJuceId);
        if (!srcNode || !srcNode->getProcessor()) continue;
        int outCh = srcNode->getProcessor()->getTotalNumOutputChannels();

        auto tapProc = std::make_unique<FreezeTapProcessor>();
        tapProc->reserveSamples(totalSamples);
        auto* tapRaw = tapProc.get();
        auto tapNode = jg->addNode(std::move(tapProc));
        if (!tapNode) continue;

        for (int ch = 0; ch < std::min(2, outCh); ++ch)
            jg->addConnection({ { srcJuceId, ch }, { tapNode->nodeID, ch } });

        taps.push_back({ id, tapRaw });
    }
    if (taps.empty()) return;

    // Re-prepare so the newly-added taps are folded into the render sequence.
    // prepare() does NOT rebuild the graph, so the taps and their connections
    // survive.
    offlineGP.prepare(graph, sr, blockSize);

    // Single offline render of the whole project. Each tap accumulates its
    // node's output as the render proceeds.
    juce::AudioBuffer<float> buf(2, blockSize);
    for (int64_t pos = 0; pos < totalSamples; pos += blockSize) {
        int thisBlock = (int)std::min((int64_t)blockSize, totalSamples - pos);
        offlineTransport.positionSamples = pos;
        buf.setSize(2, thisBlock, false, false, true);
        buf.clear();
        juce::MidiBuffer midi;
        jg->processBlock(buf, midi);
    }

    // Move each tap's captured PCM into its node's cache and mark it frozen.
    auto& cm = offlineGP.getCacheManager();
    cm.updateDeterminism(graph);
    for (auto& [id, tap] : taps) {
        auto* node = graph.findNode(id);
        if (!node) continue;
        int64_t captured = (int64_t)std::min(tap->left.size(), tap->right.size());
        node->cache.left = std::move(tap->left);
        node->cache.right = std::move(tap->right);
        node->cache.sampleRate = sr;
        node->cache.startSample = 0;
        node->cache.numSamples = captured;
        node->cache.useDisk = false;
        node->cache.valid = true;
        node->cache.enabled = true;
        node->cache.inputHash = cm.computeNodeHash(*node, graph);
        node->armedForFreeze = false;

        fprintf(stderr, "Froze node '%s': %lld samples (%.1f sec)\n",
                node->name.c_str(), (long long)captured, totalSeconds);
    }

    graph.dirty = true;
    projectDirty = true;
    // Apply the freezes to the live graph immediately.
    audioEngine.getGraphProcessor().requestRebuild();
}

void MainContentComponent::rehydrateNodeCaches(const juce::String& projectPath) {
    // ProjectFile::load restored each node's cache *metadata* (enabled/valid/
    // useDisk/inputHash/sampleRate/numSamples) but no PCM - that lives in a
    // sibling soundshop_cache/node_<id>.cache file. Point the cache manager at
    // that folder and re-attach each persisted freeze to its file, lazily: the
    // audio thread pages the samples in on first playback (graph_processor's
    // cache branch). If a freeze's file is missing (project copied without its
    // cache folder), drop the freeze so the node renders live rather than
    // playing silence.
    auto projFile = juce::File(projectPath);
    auto cacheDir = projFile.getParentDirectory().getChildFile("soundshop_cache");
    auto& cm = audioEngine.getGraphProcessor().getCacheManager();
    cm.setCacheDir(cacheDir.getFullPathName().toStdString());

    for (auto& n : graph.nodes) {
        // Only nodes that came back marked as an on-disk cache need re-attaching.
        // (A cacheValid node with useDisk=false would have carried its PCM in
        // memory, but we never serialize the samples themselves, so in practice
        // every persisted freeze is useDisk=true. Guard on it anyway.)
        if (!(n.cache.valid && n.cache.useDisk)) continue;
        auto file = cacheDir.getChildFile("node_" + juce::String(n.id) + ".cache");
        if (file.existsAsFile()) {
            n.cache.diskPath = file.getFullPathName().toStdString();
            n.cache.left.clear();   // lazy - paged in on first use
            n.cache.right.clear();
        } else {
            // Cache file gone - forget the freeze entirely so the node plays
            // live. Leaving valid=true would route it through a silent cache.
            n.cache.valid = false;
            n.cache.enabled = false;
            n.cache.useDisk = false;
            n.cache.numSamples = 0;
            n.cache.diskPath.clear();
            fprintf(stderr, "Freeze cache missing for node %d ('%s') - node will "
                    "render live.\n", n.id, n.name.c_str());
        }
    }
}

void MainContentComponent::syncCCMappingsToGraph() {
    auto mappings = audioEngine.getGraphProcessor().getAutomation().getCCMappings();
    graph.ccMappings.clear();
    for (auto& m : mappings)
        graph.ccMappings.push_back({m.midiChannel, m.ccNumber, m.nodeId, m.paramIdx});
}

void MainContentComponent::syncCCMappingsFromGraph() {
    auto& am = audioEngine.getGraphProcessor().getAutomation();
    am.clearCCMappings();
    for (auto& m : graph.ccMappings)
        am.addCCMapping({m.midiCh, m.ccNum, m.nodeId, m.paramIdx, 0.0f, 1.0f});
}

void MainContentComponent::saveProject(std::function<void()> onSaved) {
    // Backstop for the keyboard shortcut (the menu item is greyed): refuse to
    // save while plugins are still loading, since the graph isn't fully live yet.
    if (projectLoading) {
        juce::NativeMessageBox::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::InfoIcon)
                .withTitle("Still loading")
                .withMessage("This project's plugins are still loading. Saving will be "
                             "available as soon as they finish.")
                .withButton("OK")
                .withAssociatedComponent(this),
            nullptr);
        return;
    }
    if (ProjectFile::currentPath.empty()) {
        // No filename yet - defer to Save As, which will run the file chooser
        // and call us back through onSaved on success.
        saveProjectAs(std::move(onSaved));
        return;
    }

    syncCCMappingsToGraph();

    // Set cache dir and save node caches to disk
    auto& cm = audioEngine.getGraphProcessor().getCacheManager();
    auto projFile = juce::File(ProjectFile::currentPath);
    cm.setCacheDir(projFile.getParentDirectory()
        .getChildFile("soundshop_cache").getFullPathName().toStdString());
    for (auto& n : graph.nodes) {
        // The Output node's cache is transient live-capture state (populated on
        // Play->Stop, read from memory by the song-capture dialog and never
        // played back from the graph's cache branch). Persisting/flushing it
        // would both bloat saves and break the in-memory song cache, so skip it.
        if (n.type == NodeType::Output) continue;
        if (!(n.cache.valid && n.cache.numSamples > 0)) continue;
        // A freeze restored from a reloaded project (or one whose memory was
        // freed by a prior save) is on-disk-only. Page it back in so saveToDisk
        // can (re)write it under this project's cache dir - crucial for Save As,
        // where the destination folder differs from where the PCM lives now.
        if (n.cache.left.empty() && n.cache.useDisk)
            cm.loadFromDisk(n);
        if (!n.cache.left.empty())
            cm.saveToDisk(n, audioEngine.getSampleRate());
    }
    cm.cleanupStaleFiles(graph);

    ProjectFile::save(ProjectFile::currentPath, graph, &audioEngine.getGraphProcessor());
    addToRecentProjects(ProjectFile::currentPath);
    projectDirty = false;
    graph.dirty = false;
    saveFlashFrames = 60; // ~2 seconds at 30Hz
    // Explicit user save supersedes any crash-recovery autosave on disk.
    discardAutosave();
    lastAutosaveAttemptMs = juce::Time::getMillisecondCounterHiRes();
    if (onSaved) onSaved();
}

void MainContentComponent::saveProjectAs(std::function<void()> onSaved) {
    if (projectLoading) {
        juce::NativeMessageBox::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::InfoIcon)
                .withTitle("Still loading")
                .withMessage("This project's plugins are still loading. Saving will be "
                             "available as soon as they finish.")
                .withButton("OK")
                .withAssociatedComponent(this),
            nullptr);
        return;
    }
    syncCCMappingsToGraph();
    auto chooser = std::make_shared<juce::FileChooser>("Save Project", juce::File(), "*.ssp");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode
                             | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, chooser, onSaved = std::move(onSaved)](const juce::FileChooser& fc) {
            auto file = fc.getResult();
            if (file == juce::File()) return; // user cancelled - don't fire onSaved
            ProjectFile::save(file.getFullPathName().toStdString(),
                              graph, &audioEngine.getGraphProcessor());
            addToRecentProjects(file.getFullPathName());
            projectDirty = false;
            graph.dirty = false;
            saveFlashFrames = 60;
            discardAutosave();
            lastAutosaveAttemptMs = juce::Time::getMillisecondCounterHiRes();
            if (onSaved) onSaved();
            // Shared-history opt-in (#90): only offer the "bundle undo
            // history" prompt if the project isn't already bound to a
            // sidecar (which would be the case when Save-As is used to
            // copy a project that already shipped with shared history).
            if (graph.historyFilePath.empty())
                offerSharedHistoryOnSaveAs(file.getFullPathName());
        });
}

void MainContentComponent::importModFile() {
    auto chooser = std::make_shared<juce::FileChooser>(
        "Import Tracker Module", juce::File(), "*.mod;*.s3m;*.it;*.xm");
    chooser->launchAsync(juce::FileBrowserComponent::openMode,
        [this, chooser](const juce::FileChooser& fc) {
            auto file = fc.getResult();
            if (!file.existsAsFile()) return;

            // Stop transport before import so the newly wired graph
            // doesn't start playing immediately on rebuild.
            if (transport.playing) onStop();

            // Hold the graph mutation lock for the entire import. Without
            // this, the audio callback (which iterates graph.nodes and
            // calls GraphProcessor::rebuildGraph whenever it observes a
            // node-count change) can race with mod_import's repeated
            // graph.addNode() calls and read torn Node::id values from a
            // mid-reallocation vector - the root cause of the earlier
            // tracker-import crash (SEANCE.exe.63000.dmp, observed
            // nodeMap entries with id=0 and id=1132382734). The audio
            // thread uses try_lock and outputs silence while we hold
            // this, which is the right tradeoff for a few hundred ms of
            // import work.
            ModImporter::ImportResult result;
            {
                std::lock_guard<std::recursive_mutex> graphLk(graph.mutationLock);
                result = ModImporter::import(file.getFullPathName().toStdString(), graph);
            }

            juce::String msg;
            if (result.success) {
                msg = "Imported successfully!\n\n"
                    + juce::String(result.numChannels) + " channels\n"
                    + juce::String(result.numPatterns) + " patterns\n"
                    + juce::String(result.numSamplesExtracted) + " / "
                    + juce::String(result.numSamples) + " samples extracted\n"
                    + juce::String(result.numTracks) + " MIDI tracks created\n"
                    + juce::String(result.numNotes) + " notes";
                if (!result.sampleDir.empty())
                    msg += "\n\nSamples saved to:\n" + juce::String(result.sampleDir);
                projectDirty = true;
                graph.dirty = true;
                // Push an undo snapshot so the imported group/tracks/samplers
                // enter the undo system. Without this the new nodes live only
                // in the live graph and a later unrelated Ctrl+Z restores a
                // pre-import snapshot, silently deleting the whole import -
                // which then gets saved, so the mod "doesn't save/reload".
                graph.commitSnapshot("Import tracker module");
                audioEngine.getGraphProcessor().requestRebuild();
                // Frame the view on just the imported nodes, not the whole
                // graph: a pre-existing Master Out / synth parked in another
                // corner would otherwise inflate fitAll's bounding box and
                // zoom the import down to a tiny cluster with empty margins.
                if (!result.nodeIds.empty())
                    graphComponent->fitNodes(result.nodeIds);
                else
                    graphComponent->fitAll();
            } else {
                msg = "Import failed: " + juce::String(result.error);
            }

            // NativeMessageBox (not AlertWindow) so the popup is parented
            // to this window's HWND via the OS MessageBox() API - that's
            // what actually prevents a second taskbar entry. JUCE's
            // AlertWindow is a top-level desktop component without an
            // owner HWND, so even withAssociatedComponent it still spawns
            // its own taskbar icon.
            juce::NativeMessageBox::showAsync(
                juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::InfoIcon)
                    .withTitle("Tracker Import")
                    .withMessage(msg)
                    .withButton("OK")
                    .withAssociatedComponent(this),
                nullptr);
            graphComponent->repaint();
        });
}

void MainContentComponent::exportAudio() {
    float maxBeat = 0;
    for (auto& n : graph.nodes)
        for (auto& c : n.clips)
            maxBeat = std::max(maxBeat, c.startBeat + c.lengthBeats);
    if (maxBeat <= 0) maxBeat = 4;
    maxBeat += 4;

    // If looping, ask how many times to loop
    if (graph.loopEnabled && graph.loopEndBeat > graph.loopStartBeat) {
        auto* aw = new juce::AlertWindow("Export Looping Song",
            "Loop region: beat " + juce::String(graph.loopStartBeat, 1) +
            " to " + juce::String(graph.loopEndBeat, 1),
            juce::MessageBoxIconType::NoIcon);
        aw->addComboBox("loops", {"1x (no repeat)", "2x", "3x", "4x", "5x", "10x"});
        aw->getComboBoxComponent("loops")->setSelectedItemIndex(1);
        aw->addButton("Continue", 1); aw->addButton("Cancel", 0);
        float loopLen = (float)(graph.loopEndBeat - graph.loopStartBeat);
        float loopStart = (float)graph.loopStartBeat;
        aw->enterModalState(true, juce::ModalCallbackFunction::create(
            [this, aw, maxBeat, loopLen, loopStart](int result) {
                if (result == 1) {
                    int counts[] = {1, 2, 3, 4, 5, 10};
                    int idx = aw->getComboBoxComponent("loops")->getSelectedItemIndex();
                    int loops = counts[juce::jlimit(0, 5, idx)];
                    float exportBeat = loopStart + loopLen * loops + 4; // +4 tail
                    delete aw;
                    // Continue with normal export flow using adjusted maxBeat
                    exportAudioWithBeat(exportBeat);
                } else {
                    delete aw;
                }
            }), true);
        return;
    }

    exportAudioWithBeat(maxBeat);
}

void MainContentComponent::exportAudioWithBeat(float maxBeat) {
    // Show export options dialog FIRST, then file chooser
    auto* aw = new juce::AlertWindow("Export Options", "",
        juce::MessageBoxIconType::NoIcon);

    // Format
    aw->addComboBox("format", {"WAV (lossless)", "FLAC (lossless)",
                                "OGG Vorbis (lossy)", "Opus (lossy)",
                                "M4A/AAC (lossy)", "WMA (lossy)"});
    aw->getComboBoxComponent("format")->setSelectedItemIndex(0);

    // Channels
    aw->addComboBox("channels", {"Mono", "Stereo"});
    aw->getComboBoxComponent("channels")->setSelectedItemIndex(1);

    // Sample Rate
    aw->addComboBox("samplerate", {"44100 Hz", "48000 Hz", "88200 Hz", "96000 Hz", "192000 Hz"});
    {
        int srDefault = 1;
        double projSr = audioEngine.getProjectSampleRate();
        if (projSr <= 44100) srDefault = 0;
        else if (projSr <= 48000) srDefault = 1;
        else if (projSr <= 88200) srDefault = 2;
        else if (projSr <= 96000) srDefault = 3;
        else srDefault = 4;
        aw->getComboBoxComponent("samplerate")->setSelectedItemIndex(srDefault);
    }

    // Bit depth (for lossless formats)
    aw->addComboBox("bits", {"16-bit", "24-bit", "32-bit float"});
    aw->getComboBoxComponent("bits")->setSelectedItemIndex(0);

    // Quality (for OGG Vorbis)
    aw->addComboBox("quality", {"Low (q3)", "Medium (q5)", "High (q7)", "Very High (q8)", "Maximum (q10)"});
    aw->getComboBoxComponent("quality")->setSelectedItemIndex(2);

    // Bitrate (for Opus/AAC/WMA)
    aw->addComboBox("bitrate", {"64 kbps", "96 kbps", "128 kbps", "160 kbps",
                                 "192 kbps", "256 kbps", "320 kbps"});
    aw->getComboBoxComponent("bitrate")->setSelectedItemIndex(4);

    // Set initial enabled state: WAV selected -> only bit depth enabled
    aw->getComboBoxComponent("quality")->setEnabled(false);
    aw->getComboBoxComponent("bitrate")->setEnabled(false);

    // Update enabled state when format changes
    auto* fmtBox = aw->getComboBoxComponent("format");
    fmtBox->onChange = [aw]() {
        int idx = aw->getComboBoxComponent("format")->getSelectedItemIndex();
        bool isLossless = (idx <= 1);       // WAV, FLAC
        bool isVorbis   = (idx == 2);       // OGG Vorbis
        bool isBitrate  = (idx >= 3);       // Opus, M4A, WMA
        aw->getComboBoxComponent("bits")->setEnabled(isLossless);
        aw->getComboBoxComponent("quality")->setEnabled(isVorbis);
        aw->getComboBoxComponent("bitrate")->setEnabled(isBitrate);
    };

    aw->addButton("Continue", 1, juce::KeyPress(juce::KeyPress::returnKey));
    aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    aw->enterModalState(true, juce::ModalCallbackFunction::create(
        [this, aw, maxBeat](int result) {
            if (result != 1) { delete aw; return; }

            ExportOptions opts;

            // Read format
            ExportFormat formats[] = {ExportFormat::WAV, ExportFormat::FLAC,
                ExportFormat::OggVorbis, ExportFormat::Opus,
                ExportFormat::M4A_AAC, ExportFormat::WMA};
            int fmtIdx = aw->getComboBoxComponent("format")->getSelectedItemIndex();
            opts.format = formats[juce::jlimit(0, 5, fmtIdx)];

            // Read channels
            opts.numChannels = aw->getComboBoxComponent("channels")->getSelectedItemIndex() == 0 ? 1 : 2;

            // Read sample rate
            int srOptions[] = {44100, 48000, 88200, 96000, 192000};
            int srIdx = aw->getComboBoxComponent("samplerate")->getSelectedItemIndex();
            opts.sampleRate = srOptions[juce::jlimit(0, 4, srIdx)];

            // Read format-specific options
            bool isLossy = (opts.format == ExportFormat::OggVorbis || opts.format == ExportFormat::Opus ||
                            opts.format == ExportFormat::M4A_AAC || opts.format == ExportFormat::WMA);
            if (isLossy) {
                if (opts.format == ExportFormat::OggVorbis) {
                    float qualities[] = {0.3f, 0.5f, 0.7f, 0.8f, 1.0f};
                    int idx = aw->getComboBoxComponent("quality")->getSelectedItemIndex();
                    opts.quality = qualities[juce::jlimit(0, 4, idx)];
                } else {
                    int bitrates[] = {64, 96, 128, 160, 192, 256, 320};
                    int idx = aw->getComboBoxComponent("bitrate")->getSelectedItemIndex();
                    opts.bitrate = bitrates[juce::jlimit(0, 6, idx)];
                }
            } else {
                int bitsOptions[] = {16, 24, 32};
                int idx = aw->getComboBoxComponent("bits")->getSelectedItemIndex();
                opts.bitsPerSample = bitsOptions[juce::jlimit(0, 2, idx)];
            }

            delete aw;

            // Now show file chooser with the correct extension filter
            auto ext = AudioExporter::getExtension(opts.format);
            auto filter = "*" + ext;
            auto chooser = std::make_shared<juce::FileChooser>(
                "Export Audio", juce::File(), filter);
            chooser->launchAsync(juce::FileBrowserComponent::saveMode
                                     | juce::FileBrowserComponent::warnAboutOverwriting,
                [this, chooser, opts, maxBeat](const juce::FileChooser& fc) {
                    auto file = fc.getResult();
                    if (file == juce::File()) return;
                    auto path = file.getFullPathName();
                    auto extension = AudioExporter::getExtension(opts.format);
                    if (!path.endsWithIgnoreCase(extension))
                        file = juce::File(path + extension);
                    doExportRender(file, opts, maxBeat);
                });
        }), true);
}

void MainContentComponent::doExportRender(const juce::File& file, const ExportOptions& opts, float maxBeat) {
    // Run the entire render + encode on a background thread with a progress
    // bar so the UI stays responsive and the user can cancel.
    struct ExportTask : juce::ThreadWithProgressWindow {
        ExportTask(NodeGraph& g, Transport& liveTransport,
                   const juce::File& f, const ExportOptions& o, float mb)
            : ThreadWithProgressWindow("Exporting audio...", true, true),
              graph(g), file(f), opts(o), maxBeat(mb)
        {
            offTransport.bpm = g.bpm;
            offTransport.tempoMap = liveTransport.tempoMap;
            offTransport.timeSigMap = liveTransport.timeSigMap;
            offTransport.sampleRate = o.sampleRate;
            offTransport.playing = true;
        }

        void run() override {
            double sr = opts.sampleRate;
            int blockSize = 512;
            double totalSeconds = offTransport.tempoMap.beatsToSeconds(maxBeat);
            int64_t totalSamples = (int64_t)(totalSeconds * sr);
            if (totalSamples <= 0) return;

            setStatusMessage("Building audio graph...");

            GraphProcessor offGP;
            offGP.prepare(graph, sr, blockSize);
            offGP.rebuildGraph(graph, offTransport);
            offGP.prepare(graph, sr, blockSize);

            // Always render in stereo - graph processor outputs stereo
            juce::AudioBuffer<float> renderBuf(2, (int)totalSamples);
            renderBuf.clear();

            setStatusMessage("Rendering audio...");

            for (int64_t pos = 0; pos < totalSamples; pos += blockSize) {
                if (threadShouldExit()) return;

                int thisBlock = (int)std::min((int64_t)blockSize, totalSamples - pos);
                offTransport.positionSamples = pos;
                float* outPtrs[2] = {
                    renderBuf.getWritePointer(0, (int)pos),
                    renderBuf.getWritePointer(1, (int)pos)
                };
                offGP.processBlock(graph, offTransport, outPtrs, 2, thisBlock);
                setProgress((double)pos / (double)totalSamples);
            }

            // Mix down to mono if requested
            if (opts.numChannels == 1) {
                setStatusMessage("Mixing to mono...");
                juce::AudioBuffer<float> monoBuf(1, (int)totalSamples);
                monoBuf.copyFrom(0, 0, renderBuf, 0, 0, (int)totalSamples);
                monoBuf.addFrom(0, 0, renderBuf, 1, 0, (int)totalSamples);
                monoBuf.applyGain(0.5f);
                renderBuf = std::move(monoBuf);
            }

            // Apply TPDF dithering for PCM formats
            if (opts.dither && (opts.format == ExportFormat::WAV
                                || opts.format == ExportFormat::FLAC)) {
                applyTPDFDither(renderBuf, opts.bitsPerSample);
            }

            setStatusMessage("Writing file...");
            exportSuccess = AudioExporter::exportToFile(file, renderBuf, opts);
        }

        NodeGraph& graph;
        Transport offTransport;
        juce::File file;
        ExportOptions opts;
        float maxBeat;
        bool exportSuccess = false;
    };

    ExportTask task(graph, transport, file, opts, maxBeat);

    if (task.runThread() && task.exportSuccess) {
        saveFlashFrames = 90;
        if (auto* win = dynamic_cast<juce::DocumentWindow*>(getTopLevelComponent()))
            win->setName("Exported!");
    }
}

void MainContentComponent::openEditor(Node& node) {
    // Only open piano roll editors for timeline nodes - instruments,
    // effects, etc. have their own editors (waveform editor, plugin UI).
    if (node.type != NodeType::MidiTimeline && node.type != NodeType::AudioTimeline)
        return;

    // Check if already open
    for (auto& panel : editorPanels)
        if (panel->nodeId == node.id) return;

    auto panel = std::make_unique<EditorPanel>();
    panel->nodeId = node.id;
    // 220px default: with the default vertical zoom (visibleRange 15) this
    // gives ~7.5px keyboard rows, tall enough for a per-row note-name label
    // (font floor 6.5px) so the user can read every row's note. A shorter
    // panel falls back to cramped labels; the user can still drag the panel
    // taller via the resize handle at its top edge.
    panel->heightPx = 220;
    panel->component = std::make_unique<PianoRollComponent>(graph, node, &transport);
    panel->component->onClose = [this](int nodeId) { closeEditor(nodeId); };
    // Resize handle at the top of the piano roll panel. The handle's
    // mouseDrag fires this with the per-frame screen-y delta; we adjust
    // THIS panel's heightPx by -deltaPx (drag UP = grow). Panels above
    // keep their own heightPx and just shift up/down as the total stack
    // grows/shrinks.
    int nodeIdCopy = node.id;
    panel->component->onResizeDrag = [this, nodeIdCopy](int deltaPx) {
        resizeEditorPanel(nodeIdCopy, deltaPx);
    };
    // A track's time offset / parent change shifts not just this track but any
    // children shown in OTHER stacked panels, and moves notes in the node
    // graph's mini-timelines. Repaint every editor panel and the graph.
    panel->component->onTimingChanged = [this]() {
        for (auto& p : editorPanels)
            p->component->repaint();
        if (graphComponent) graphComponent->repaint();
    };
    addAndMakeVisible(panel->component.get());
    editorPanels.push_back(std::move(panel));

    graph.activeEditorNodeId = node.id;
    recalcEditorPanelHeight();
    resized();
}

void MainContentComponent::closeEditor(int nodeId) {
    editorPanels.erase(
        std::remove_if(editorPanels.begin(), editorPanels.end(),
            [nodeId](auto& p) { return p->nodeId == nodeId; }),
        editorPanels.end());
    recalcEditorPanelHeight();
    resized();
}

void MainContentComponent::resizeEditorPanel(int nodeId, int deltaPx) {
    // Find the panel being resized and bump its heightPx. Drag UP gives
    // a negative deltaPx (cursor moves toward small Y), which should
    // GROW the panel - so subtract.
    //
    // The ceiling is computed dynamically against the available window
    // area minus the heights of the OTHER panels, so the total stack
    // never grows past where the node graph would disappear. Without
    // this dynamic ceiling, dragging up past the cap visually saturates
    // but the panel's heightPx keeps growing - the user then has to drag
    // back down the same number of pixels before anything happens.
    int othersTotal = 0;
    for (auto& p : editorPanels)
        if (p->nodeId != nodeId) othersTotal += p->heightPx;
    int maxStack = juce::jmax(120, getHeight() - 120);
    int maxThisPanel = juce::jmax(80, maxStack - othersTotal);

    for (auto& panel : editorPanels) {
        if (panel->nodeId != nodeId) continue;
        panel->heightPx = juce::jlimit(80, maxThisPanel, panel->heightPx - deltaPx);
        break;
    }
    recalcEditorPanelHeight();
    resized();
}

void MainContentComponent::recalcEditorPanelHeight() {
    if (editorPanels.empty()) {
        editorPanelHeight = 250; // keeps the empty-state default in sync
        return;
    }
    int total = 0;
    for (auto& panel : editorPanels)
        total += panel->heightPx;
    // Leave at least 120px for the graph area so the user can't drag the
    // stack so tall that the node graph becomes invisible / unreachable.
    int maxStack = juce::jmax(120, getHeight() - 120);
    editorPanelHeight = juce::jlimit(80, maxStack, total);
}

bool MainContentComponent::tryQuit() {
    if (!projectDirty && !graph.dirty) {
        // Clean exit with nothing to save - any leftover autosave is stale
        // (it would only exist if we crashed on a previous run and the user
        // already loaded a recent project past it). Sweep it away so the
        // next startup doesn't re-offer an irrelevant recovery.
        discardAutosave();
        return true;
    }

    int result = juce::AlertWindow::showYesNoCancelBox(
        juce::MessageBoxIconType::QuestionIcon,
        "Unsaved Changes",
        "You have unsaved changes. Save before quitting?",
        "Save", "Don't Save", "Cancel");
    if (result == 2) {                   // Don't Save
        // User explicitly threw their edits away - autosave AND undo
        // history go with them. (A clean save+quit instead would keep
        // the undo tree so the next session can continue undoing.)
        discardAutosave();
        discardUndoTreePersist();
        return true;
    }
    if (result != 1) return false;       // Cancel (or window closed)

    // Save first, then re-request quit on completion. If the project has no
    // current path the file chooser is async - we must NOT return true here
    // or the app will exit before the chooser even appears (which is the bug
    // the user hit: pressed Save, app quit, no file ever written, recent
    // projects never updated).
    saveProject([]() {
        // The save succeeded - ask the app to quit again. This goes through
        // tryQuit a second time, sees the dirty flags cleared, and returns
        // true immediately. Defer via callAsync so we're not still inside
        // the file-chooser callback when we tear down the window.
        juce::MessageManager::callAsync([]() {
            if (auto* app = juce::JUCEApplication::getInstance())
                app->systemRequestedQuit();
        });
    });
    return false; // wait for the async save to drive the next quit attempt
}

// ==============================================================================
// Recent Projects
// ==============================================================================

static juce::File getRecentProjectsFile() {
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile)
               .getSiblingFile("soundshop_recent_projects.txt");
}

void MainContentComponent::addToRecentProjects(const juce::String& path) {
    recentProjects.erase(
        std::remove(recentProjects.begin(), recentProjects.end(), path),
        recentProjects.end());
    recentProjects.insert(recentProjects.begin(), path);
    if (recentProjects.size() > 10)
        recentProjects.resize(10);
    saveRecentProjects();
}

void MainContentComponent::loadRecentProjects() {
    recentProjects.clear();
    auto file = getRecentProjectsFile();
    if (!file.existsAsFile()) return;
    juce::StringArray lines;
    lines.addLines(file.loadFileAsString());
    for (auto& line : lines)
        if (line.isNotEmpty())
            recentProjects.push_back(line);
}

void MainContentComponent::saveRecentProjects() {
    juce::String text;
    for (auto& path : recentProjects)
        text += path + "\n";
    getRecentProjectsFile().replaceWithText(text);
}

// ==============================================================================
// Preferences
// ==============================================================================

static juce::File getPreferencesFile() {
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile)
               .getSiblingFile("soundshop_prefs.xml");
}

void MainContentComponent::loadPreferences() {
    int autoDefault = defaultAutosaveIntervalForThisMachine();
    auto file = getPreferencesFile();
    if (!file.existsAsFile()) {
        autosaveIntervalSeconds = autoDefault;
        return;
    }
    auto xml = juce::parseXML(file);
    if (!xml || xml->getTagName() != "Preferences") {
        autosaveIntervalSeconds = autoDefault;
        return;
    }
    autoLoadLastProject = xml->getBoolAttribute("autoLoadLastProject", true);
    autoFitGraph = xml->getBoolAttribute("autoFitGraph", false);
    autosaveEnabled = xml->getBoolAttribute("autosaveEnabled", true);
    autosaveIntervalSeconds = xml->getIntAttribute("autosaveIntervalSeconds", autoDefault);
    if (autosaveIntervalSeconds < 1) autosaveIntervalSeconds = 1;
    autosaveLaptopNoticeShown = xml->getBoolAttribute("autosaveLaptopNoticeShown", false);
}

void MainContentComponent::savePreferences() {
    auto xml = std::make_unique<juce::XmlElement>("Preferences");
    xml->setAttribute("autoLoadLastProject", autoLoadLastProject);
    xml->setAttribute("autoFitGraph", autoFitGraph);
    xml->setAttribute("autosaveEnabled", autosaveEnabled);
    xml->setAttribute("autosaveIntervalSeconds", autosaveIntervalSeconds);
    xml->setAttribute("autosaveLaptopNoticeShown", autosaveLaptopNoticeShown);
    xml->writeTo(getPreferencesFile());
}

// ==============================================================================
// Autosave
// ==============================================================================

// Ephemeral-session state (see setEphemeralSession in main_window.h). File-
// local so the getAutosaveDir() family below can consult it; toggled through
// the namespace-scoped setter that main.cpp calls when --ephemeral is parsed.
static bool g_ephemeralSession = false;

bool isEphemeralSession() { return g_ephemeralSession; }
void setEphemeralSession(bool on) {
    g_ephemeralSession = on;
    if (on) {
        // Start every ephemeral launch from a clean slate so a previously
        // killed ephemeral run can't leave an autosave that makes the NEXT
        // ephemeral run prompt for recovery. (The user's real session dir is
        // never touched in this mode.)
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("SEANCE-ephemeral")
            .deleteRecursively();
    }
}

// Optional startup project file (set from a bare `.ssp` path on the command
// line). When non-empty it's loaded in place of the most-recent project, so a
// launch like `SEANCE.exe foo.ssp` (optionally with --ephemeral) opens that
// file directly. See setStartupProjectFile() / the constructor's load path.
static juce::String g_startupProjectFile;
juce::String startupProjectFile() { return g_startupProjectFile; }
void setStartupProjectFile(const juce::String& path) { g_startupProjectFile = path; }

static juce::File getAutosaveDir() {
    if (g_ephemeralSession)
        return juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("SEANCE-ephemeral");
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
               .getChildFile("SoundShop");
}
static juce::File getAutosaveFile() {
    return getAutosaveDir().getChildFile("autosave.ssp");
}
static juce::File getAutosaveMetaFile() {
    return getAutosaveDir().getChildFile("autosave.meta.xml");
}
static juce::File getUndoTreeFile() {
    return getAutosaveDir().getChildFile("undo-tree.dat");
}
// Session-lock sentinel. Created on startup, deleted on clean shutdown. Its
// presence at the next startup means the previous run never reached a clean
// shutdown (crash, force-kill, power loss, or a quit sequence that didn't
// finish). This is the crash signal - decoupled from autosave.ssp, which a
// normal idle session also writes and which a clean exit only *usually*
// manages to sweep before the process dies.
static juce::File getSessionLockFile() {
    return getAutosaveDir().getChildFile("session.lock");
}
static juce::File getPluginStateFile(int nodeId) {
    return getAutosaveDir().getChildFile("autosave-plugin-" + juce::String(nodeId) + ".dat");
}

void MainContentComponent::performAutosave() {
    if (!autosaveEnabled) return;

    // The slow channel only handles plugin internal state. Everything
    // else (notes, cables, params, structural changes) is captured at
    // gesture granularity by the fast undo-tree persistence (#84).
    //
    // Two kinds of dirty matter here:
    //   - Plugin state dirty (per-Node flag set by automation pushes):
    //     write that plugin's individual file (tiny, overwritten in place)
    //   - Graph topology dirty (graph.dirty / projectDirty set by general
    //     mutations): we need to refresh autosave.ssp so its [Node] entries
    //     reflect the current topology, plus capture any plugin states
    //     for plugins that may have just been added
    std::vector<int> dirtyPluginNodeIds;
    for (auto& n : graph.nodes) {
        if (n.pluginIndex >= 0 && n.pluginStateDirty)
            dirtyPluginNodeIds.push_back(n.id);
    }

    // Full save trigger: autosave.ssp doesn't exist yet, OR a periodic
    // refresh interval has elapsed (so the main file's graph metadata
    // catches up to topology changes that happened since the last full
    // save). Between full saves, only per-plugin files get written.
    //
    // We don't trigger Full saves on graph.dirty / projectDirty because
    // those flags are set by every kind of edit (notes, params, drags)
    // and would cause Full saves on every tick during normal editing -
    // defeating the whole point of incremental saves. The fast channel
    // (#84) is the source of truth for graph state in the recovery flow,
    // so autosave.ssp's slight staleness between full saves is fine.
    bool needFullSave = !getAutosaveFile().existsAsFile()
                     || autosaveTicksSinceFullSave >= kAutosaveTicksBetweenFullSaves;

    if (dirtyPluginNodeIds.empty() && !needFullSave) return;

    auto dir = getAutosaveDir();
    if (!dir.exists()) dir.createDirectory();

    syncCCMappingsToGraph();

    AutosaveJob job;
    auto& gp = audioEngine.getGraphProcessor();

    if (needFullSave) {
        // Full save: rewrite autosave.ssp with graph metadata ONLY
        // (no inline plugin states). All plugin states live in the
        // per-plugin files written below. This makes the periodic full
        // save cheap - same cost as a single fast-channel snapshot -
        // because we're not duplicating the plugin state data that's
        // already on disk in the per-plugin files.
        //
        // Recovery uses both: load autosave.ssp for graph topology,
        // then walk per-plugin files for plugin state (via
        // applyPerPluginOverrides). Plugins whose per-plugin file is
        // missing fall back to whatever default state the plugin loads
        // with - same behavior as opening a brand-new project file.
        auto graphText = ProjectFile::serializeForUndo(graph);
        if (graphText.empty()) return;

        job.writes.push_back({ getAutosaveFile(), std::move(graphText) });
        job.metaPath = ProjectFile::currentPath;
        job.metaTimestamp = juce::Time::getCurrentTime().toISO8601(true).toStdString();
        job.writeMeta = true;
        autosaveTicksSinceFullSave = 0;
    } else {
        autosaveTicksSinceFullSave++;
    }

    // Per-plugin files for whichever plugins are flagged dirty. The Full
    // save above (when one happened) writes graph metadata only - it does
    // NOT touch the plugin state cache or query getStateInformation. All
    // plugin state querying happens here, in this loop, exactly once per
    // dirty plugin. Clean plugins are skipped entirely (their on-disk
    // file from a previous tick is still correct).
    for (int nid : dirtyPluginNodeIds) {
        auto* n = graph.findNode(nid);
        if (!n || n->pluginIndex < 0) continue;
        if (n->pluginStateDirty) {
            // The Full path didn't already query this plugin (or there
            // was no Full path). Query now and refresh the cache.
            auto* proc = gp.getProcessorForNode(nid);
            if (!proc) continue;
            juce::MemoryBlock stateData;
            proc->getStateInformation(stateData);
            if (stateData.getSize() == 0) continue;
            n->cachedPluginStateBase64 = stateData.toBase64Encoding().toStdString();
            n->pluginStateDirty = false;
        }
        if (!n->cachedPluginStateBase64.empty())
            job.writes.push_back({ getPluginStateFile(nid), n->cachedPluginStateBase64 });
    }

    if (job.writes.empty()) return;
    enqueueAutosaveJob(std::move(job));

    // After a Full save we know autosave.ssp is fully up to date with
    // the current set of plugins, so any orphan per-plugin files (from
    // plugins that have been removed since) can be deleted now.
    if (needFullSave)
        cleanupOrphanPluginFiles();

    // Note: we do NOT clear projectDirty or graph.dirty here. Autosave is
    // invisible to normal dirty tracking - only an explicit user save
    // clears those.
}

void MainContentComponent::enqueueAutosaveJob(AutosaveJob job) {
    {
        std::lock_guard<std::mutex> lk(autosaveWorkerMutex);
        // Single-slot mailbox with file-keyed coalescing:
        //
        //   No pending job: just store.
        //
        //   Pending job exists: merge file lists. For each new write,
        //   if a write to the same file is already pending, replace its
        //   content (newer wins). Otherwise append to the pending list.
        //   Sidecar metadata (metaPath/timestamp/writeMeta) takes the
        //   newer non-empty value.
        //
        // The result: at most one pending write per destination file
        // exists in the mailbox at any time, no matter how many ticks
        // fire before the worker drains it. Each file's "latest value"
        // wins.
        if (!autosaveWorkerHasJob) {
            autosaveWorkerPending = std::move(job);
        } else {
            for (auto& w : job.writes) {
                bool replaced = false;
                for (auto& existing : autosaveWorkerPending.writes) {
                    if (existing.destFile == w.destFile) {
                        existing.content = std::move(w.content);
                        replaced = true;
                        break;
                    }
                }
                if (!replaced)
                    autosaveWorkerPending.writes.push_back(std::move(w));
            }
            if (job.writeMeta) {
                autosaveWorkerPending.writeMeta = true;
                autosaveWorkerPending.metaPath = std::move(job.metaPath);
                autosaveWorkerPending.metaTimestamp = std::move(job.metaTimestamp);
            }
        }
        autosaveWorkerHasJob = true;
    }
    autosaveWorkerCv.notify_one();
}

void MainContentComponent::startAutosaveWorker() {
    if (autosaveWorkerThread.joinable()) return;
    autosaveWorkerStop.store(false);
    autosaveWorkerThread = std::thread([this]() { autosaveWorkerMain(); });
}

void MainContentComponent::stopAutosaveWorker() {
    if (!autosaveWorkerThread.joinable()) return;
    autosaveWorkerStop.store(true);
    autosaveWorkerCv.notify_all();
    autosaveWorkerThread.join();
}

void MainContentComponent::autosaveWorkerMain() {
    for (;;) {
        AutosaveJob job;
        {
            std::unique_lock<std::mutex> lk(autosaveWorkerMutex);
            autosaveWorkerCv.wait(lk, [this]() {
                return autosaveWorkerHasJob || autosaveWorkerStop.load();
            });
            if (autosaveWorkerStop.load() && !autosaveWorkerHasJob) return;
            job = std::move(autosaveWorkerPending);
            autosaveWorkerHasJob = false;
            // Mark busy *before* releasing the lock so quiesceAutosaveWorker()
            // can't observe an idle gap between dequeue and the write below.
            autosaveWorkerBusy = true;
        }

        // Walk the file list and write each one atomically via tmp+rename.
        // Independent files = no ordering constraints; if any single file
        // fails (e.g. permissions), the others still go through.
        for (auto& w : job.writes) {
            auto tmp = w.destFile.getSiblingFile(w.destFile.getFileName() + ".tmp");
            if (tmp.existsAsFile()) tmp.deleteFile();
            {
                juce::FileOutputStream out(tmp);
                if (out.failedToOpen()) continue;
                out.write(w.content.data(), w.content.size());
            }
            if (w.destFile.existsAsFile()) w.destFile.deleteFile();
            tmp.moveFileTo(w.destFile);
        }

        // Sidecar metadata: only updated for Full saves. Per-plugin file
        // writes don't change the user's idea of "what's being edited."
        if (job.writeMeta) {
            auto meta = std::make_unique<juce::XmlElement>("Autosave");
            meta->setAttribute("originalPath", juce::String(job.metaPath));
            meta->setAttribute("timestamp", juce::String(job.metaTimestamp));
            meta->writeTo(getAutosaveMetaFile());
        }

        // Write finished. Drop the busy flag and wake any quiescing caller.
        {
            std::lock_guard<std::mutex> lk(autosaveWorkerMutex);
            autosaveWorkerBusy = false;
        }
        autosaveWorkerIdleCv.notify_all();
    }
}

// Block until the worker is fully idle: no pending job AND not mid-write. Used
// by discardAutosave() so a write that's queued or in flight can't recreate the
// autosave files we're about to delete. Called from the message thread; since
// performAutosave() (the only enqueuer) also runs on the message thread, no new
// job can appear while we're inside here, so returning idle stays idle until we
// return to the message loop.
void MainContentComponent::quiesceAutosaveWorker() {
    std::unique_lock<std::mutex> lk(autosaveWorkerMutex);
    autosaveWorkerHasJob = false;                  // cancel anything still queued
    autosaveWorkerIdleCv.wait(lk, [this]() {       // wait out any in-flight write
        return !autosaveWorkerBusy;
    });
}

void MainContentComponent::setupSessionLock() {
    auto dir = getAutosaveDir();
    if (!dir.exists()) dir.createDirectory();
    auto lock = getSessionLockFile();
    // If the lock is already here, the previous run never reached a clean
    // shutdown - that's our crash signal, independent of whether autosave.ssp
    // happens to exist. (For an --ephemeral run the whole dir was just wiped,
    // so the lock is absent and this run is correctly treated as clean.)
    startupWasUncleanShutdown = lock.existsAsFile();
    lock.replaceWithText(juce::Time::getCurrentTime().toISO8601(true));
}

void MainContentComponent::markCleanShutdown() {
    auto lock = getSessionLockFile();
    if (lock.existsAsFile()) lock.deleteFile();
}

void MainContentComponent::discardAutosave() {
    // Fully quiesce the worker first: cancel any queued job AND wait out any
    // write already in flight. The old version only cleared the pending flag,
    // which left a race - if the worker had already dequeued and was mid-write
    // (tmp.moveFileTo recreating autosave.ssp), it would resurrect the file
    // right after we deleted it. With the 5s autosave interval this fired often
    // enough on quit (especially while the modal "Save before quitting?" dialog
    // pumped timer ticks) that the next launch wrongly reported an unclean
    // shutdown. quiesceAutosaveWorker() guarantees we're the last writer.
    quiesceAutosaveWorker();
    auto f = getAutosaveFile();
    if (f.existsAsFile()) f.deleteFile();
    auto tmp = f.getSiblingFile(f.getFileName() + ".tmp");
    if (tmp.existsAsFile()) tmp.deleteFile();
    auto m = getAutosaveMetaFile();
    if (m.existsAsFile()) m.deleteFile();
    // Per-plugin files are part of the autosave too - sweep them all.
    auto dir = getAutosaveDir();
    if (dir.exists()) {
        for (auto& f2 : dir.findChildFiles(juce::File::findFiles, false, "autosave-plugin-*.dat"))
            f2.deleteFile();
        for (auto& f2 : dir.findChildFiles(juce::File::findFiles, false, "autosave-plugin-*.dat.tmp"))
            f2.deleteFile();
    }
    // Force the next slow autosave to do a Full save since we just
    // wiped the autosave.ssp file.
    autosaveTicksSinceFullSave = kAutosaveTicksBetweenFullSaves;
    // Note: this deliberately does NOT delete the undo tree. The undo
    // tree persists across clean save+quit so the next session can
    // continue undoing past the last save point. It's only thrown away
    // on explicit "Don't Save" / "Discard" / new project / open project
    // - those paths call discardUndoTreePersist() separately.
}

void MainContentComponent::applyPerPluginOverrides() {
    auto& gp = audioEngine.getGraphProcessor();
    for (auto& n : graph.nodes) {
        if (n.pluginIndex < 0) continue;
        auto file = getPluginStateFile(n.id);
        if (!file.existsAsFile()) continue;
        auto base64 = file.loadFileAsString().toStdString();
        if (base64.empty()) continue;

        // Update the cache so the next slow autosave doesn't redundantly
        // re-query - the cache is now in sync with what's actually on
        // the plugin instance.
        n.cachedPluginStateBase64 = base64;
        n.pluginStateDirty = false;

        // Push the override into the live plugin instance. The plugin
        // is already loaded by ProjectFile::load at this point, so we
        // call setStateInformation on its live instance to override
        // whatever ProjectFile::load applied from the file's [Node]
        // pluginState entry.
        auto* proc = gp.getProcessorForNode(n.id);
        if (!proc) continue;
        juce::MemoryBlock stateData;
        stateData.fromBase64Encoding(base64);
        if (stateData.getSize() > 0)
            proc->setStateInformation(stateData.getData(), (int)stateData.getSize());
    }
}

void MainContentComponent::cleanupOrphanPluginFiles() {
    auto dir = getAutosaveDir();
    if (!dir.exists()) return;
    auto files = dir.findChildFiles(juce::File::findFiles, false, "autosave-plugin-*.dat");
    std::set<int> currentPluginNodeIds;
    for (auto& n : graph.nodes)
        if (n.pluginIndex >= 0) currentPluginNodeIds.insert(n.id);
    for (auto& f : files) {
        // Extract nodeId from filename like "autosave-plugin-42.dat".
        auto stem = f.getFileNameWithoutExtension();
        auto prefix = juce::String("autosave-plugin-");
        if (!stem.startsWith(prefix)) continue;
        int nodeId = stem.substring(prefix.length()).getIntValue();
        if (currentPluginNodeIds.find(nodeId) == currentPluginNodeIds.end())
            f.deleteFile();
    }
}

void MainContentComponent::discardUndoTreePersist() {
    auto u = getUndoTreeFile();
    if (u.existsAsFile()) u.deleteFile();
    auto utmp = u.getSiblingFile(u.getFileName() + ".tmp");
    if (utmp.existsAsFile()) utmp.deleteFile();
}

void MainContentComponent::writeUndoTreePersist() {
    auto dir = getAutosaveDir();
    if (!dir.exists()) dir.createDirectory();
    auto file = getUndoTreeFile();

    // Serialize via stringstream first, then write atomically by going
    // through a .tmp file and renaming. Avoids leaving a half-written
    // tree on disk if the app crashes mid-write.
    std::ostringstream oss;
    graph.undoTree.serializeTo(oss);
    auto text = oss.str();

    // Helper: atomic tmp-then-rename write of `text` to `target`.
    auto atomicWrite = [&](const juce::File& target) {
        auto tmp = target.getSiblingFile(target.getFileName() + ".tmp");
        if (tmp.existsAsFile()) tmp.deleteFile();
        {
            juce::FileOutputStream out(tmp);
            if (out.failedToOpen()) return;
            out.write(text.data(), text.size());
        }
        if (target.existsAsFile()) target.deleteFile();
        tmp.moveFileTo(target);
    };

    // Primary destination: user app-data copy (crash recovery, machine-
    // local, always written).
    atomicWrite(file);

    // Secondary destination: project-bundled sidecar, if the current
    // project has one. This is what gets shipped with the .ssp when the
    // user shares the project, so it needs to stay in sync with the
    // in-memory undo tree just like the app-data copy does.
    auto sidecar = currentProjectSidecarFile();
    if (sidecar != juce::File())
        atomicWrite(sidecar);
}

juce::File MainContentComponent::currentProjectSidecarFile() const {
    if (graph.historyFilePath.empty()) return {};
    if (ProjectFile::currentPath.empty()) return {};
    auto projFile = juce::File(ProjectFile::currentPath);
    return projFile.getParentDirectory().getChildFile(juce::String(graph.historyFilePath));
}

void MainContentComponent::writeSharedHistorySidecar(const juce::File& sidecarFile) {
    // One-shot write of the current undo tree to an explicit file path.
    // Used by save-as opt-in and by the "use a copy" branch of the open
    // prompt. Updates graph.historyFilePath to the path relative to the
    // project's .ssp directory, so subsequent writeUndoTreePersist calls
    // keep the sidecar in sync automatically.
    auto dir = sidecarFile.getParentDirectory();
    if (!dir.exists()) dir.createDirectory();

    std::ostringstream oss;
    graph.undoTree.serializeTo(oss);
    auto text = oss.str();

    auto tmp = sidecarFile.getSiblingFile(sidecarFile.getFileName() + ".tmp");
    if (tmp.existsAsFile()) tmp.deleteFile();
    {
        juce::FileOutputStream out(tmp);
        if (out.failedToOpen()) return;
        out.write(text.data(), text.size());
    }
    if (sidecarFile.existsAsFile()) sidecarFile.deleteFile();
    tmp.moveFileTo(sidecarFile);

    // Update the graph's recorded sidecar path so writeUndoTreePersist
    // picks it up going forward.
    if (!ProjectFile::currentPath.empty()) {
        auto projDir = juce::File(ProjectFile::currentPath).getParentDirectory();
        auto rel = sidecarFile.getRelativePathFrom(projDir);
        graph.historyFilePath = rel.toStdString();
    } else {
        graph.historyFilePath = sidecarFile.getFullPathName().toStdString();
    }
}

// ============================================================================
// Known-histories preferences (per-user record of decisions per project path)
// ============================================================================

static juce::File getKnownHistoriesFile() {
    return getAutosaveDir().getChildFile("known-histories.txt");
}

void MainContentComponent::loadKnownHistories() {
    knownHistories.clear();
    auto file = getKnownHistoriesFile();
    if (!file.existsAsFile()) return;
    auto text = file.loadFileAsString();
    auto lines = juce::StringArray::fromLines(text);
    for (auto& line : lines) {
        if (line.isEmpty()) continue;
        // Format: <project_path>\t<decision>\t<aux>
        auto parts = juce::StringArray::fromTokens(line, "\t", "");
        if (parts.size() < 2) continue;
        HistoryRecord rec;
        auto decStr = parts[1];
        if      (decStr == "adopted") rec.decision = HistoryDecision::Adopted;
        else if (decStr == "copied")  rec.decision = HistoryDecision::Copied;
        else if (decStr == "ignored") rec.decision = HistoryDecision::Ignored;
        else continue;
        if (parts.size() >= 3) rec.copyPath = parts[2].toStdString();
        knownHistories[parts[0].toStdString()] = rec;
    }
}

void MainContentComponent::saveKnownHistories() {
    auto dir = getAutosaveDir();
    if (!dir.exists()) dir.createDirectory();
    juce::String text;
    for (auto& [path, rec] : knownHistories) {
        const char* decStr =
            rec.decision == HistoryDecision::Adopted ? "adopted" :
            rec.decision == HistoryDecision::Copied  ? "copied"  :
            rec.decision == HistoryDecision::Ignored ? "ignored" : "";
        if (*decStr == 0) continue;
        text += juce::String(path) + "\t" + decStr;
        if (!rec.copyPath.empty())
            text += juce::String("\t") + juce::String(rec.copyPath);
        text += "\n";
    }
    getKnownHistoriesFile().replaceWithText(text);
}

MainContentComponent::HistoryRecord
MainContentComponent::getHistoryDecision(const juce::String& projectPath) const {
    auto it = knownHistories.find(projectPath.toStdString());
    if (it == knownHistories.end()) return {};
    return it->second;
}

void MainContentComponent::recordHistoryDecision(const juce::String& projectPath,
                                                 HistoryDecision d,
                                                 const std::string& copyPath) {
    HistoryRecord rec;
    rec.decision = d;
    rec.copyPath = copyPath;
    knownHistories[projectPath.toStdString()] = rec;
    saveKnownHistories();
}

// Load an undo tree from a file on disk into the live graph.undoTree,
// and drive the live graph to the tree's current snapshot. Shared by
// the "Use it" and "Use a copy" branches of the open prompt. Returns
// true iff restoration succeeded.
static bool loadUndoTreeFromFile(const juce::File& file, NodeGraph& graph) {
    if (!file.existsAsFile()) return false;
    std::ifstream in(file.getFullPathName().toStdString());
    if (!in) return false;
    if (!graph.undoTree.restoreFrom(in)) return false;
    const auto& snap = graph.undoTree.currentSnapshot();
    if (!snap.empty() && graph.undoTree.onLoadSnapshot)
        graph.undoTree.onLoadSnapshot(snap);
    return true;
}

void MainContentComponent::handleSharedHistoryOnOpen(const juce::String& projectAbsPath) {
    // The project file we just loaded may have had a historyFile= field
    // pointing at a sidecar. If not, there's nothing to consider - bail.
    auto sidecar = currentProjectSidecarFile();
    if (sidecar == juce::File() || !sidecar.existsAsFile()) return;

    // Per-user memory of what the user already decided for this project.
    // Suppresses the re-prompt on every subsequent open.
    auto rec = getHistoryDecision(projectAbsPath);
    juce::Component::SafePointer<MainContentComponent> safe(this);
    juce::String capturedPath = projectAbsPath;

    auto adoptInPlace = [safe, sidecar]() {
        if (!safe) return;
        if (loadUndoTreeFromFile(sidecar, safe->graph)) {
            safe->undoTreeDirty = false;
            if (safe->graphComponent) safe->graphComponent->repaint();
        }
    };

    switch (rec.decision) {
        case HistoryDecision::Adopted:
            adoptInPlace();
            return;
        case HistoryDecision::Copied: {
            // User previously chose to use a private copy. Load from
            // that copy and leave the sidecar alone. If the copy is
            // missing (user deleted it), fall through to re-prompt.
            if (!rec.copyPath.empty()) {
                juce::File copyFile(juce::String(rec.copyPath));
                if (copyFile.existsAsFile()) {
                    if (loadUndoTreeFromFile(copyFile, graph)) {
                        undoTreeDirty = false;
                        // Detach the graph from the bundled sidecar: the
                        // user's edits go to their private copy, not the
                        // shared file.
                        graph.historyFilePath.clear();
                        if (graphComponent) graphComponent->repaint();
                        return;
                    }
                }
            }
            // Fall through to re-prompt.
            break;
        }
        case HistoryDecision::Ignored:
            // Detach from the sidecar so writeUndoTreePersist doesn't
            // mutate the shared file going forward this session either.
            graph.historyFilePath.clear();
            return;
        case HistoryDecision::NeverSeen:
        default:
            break;
    }

    // Show the 3-option modal. Button indexing:
    //   1 = Use it (adopt)     - shared tree becomes the live tree
    //   2 = Use a copy (copy)  - duplicate to a private file, edit that
    //   0 = Ignore             - leave the sidecar alone, no undo tree
    juce::String projName = juce::File(projectAbsPath).getFileName();
    juce::String message =
        "This project came with a shared undo history.\n\n"
        "What would you like to do with it?\n\n"
        "- Use it: your undo/redo will continue from where the sender "
        "left off, and your future edits will be written back to the "
        "shared history file bundled with the project.\n\n"
        "- Use a copy: keep a private copy of the history for yourself. "
        "The shared file won't be modified.\n\n"
        "- Ignore: start with a fresh undo history. The shared file "
        "stays untouched.";

    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle("Shared Undo History Found")
            .withMessage(message)
            .withButton("Use It")
            .withButton("Use a Copy")
            .withButton("Ignore"),
        [safe, sidecar, capturedPath](int result) {
            if (!safe) return;
            if (result == 1) {
                // Adopt: load the sidecar; future writes keep going to it.
                if (loadUndoTreeFromFile(sidecar, safe->graph)) {
                    safe->undoTreeDirty = false;
                    if (safe->graphComponent) safe->graphComponent->repaint();
                }
                safe->recordHistoryDecision(capturedPath, HistoryDecision::Adopted);
            } else if (result == 2) {
                // Copy: duplicate the sidecar into a private file under
                // the user's autosave dir, load from it, and detach the
                // graph from the bundled sidecar so writes go to the copy.
                auto dir = getAutosaveDir();
                if (!dir.exists()) dir.createDirectory();
                auto hashed = juce::String::toHexString(capturedPath.hashCode64());
                auto copyFile = dir.getChildFile("shared-history-copy-" + hashed + ".dat");
                sidecar.copyFileTo(copyFile);
                if (loadUndoTreeFromFile(copyFile, safe->graph)) {
                    safe->undoTreeDirty = false;
                    if (safe->graphComponent) safe->graphComponent->repaint();
                }
                // Detach: clear historyFilePath so the bundled sidecar
                // stops receiving writes. The copy is machine-local.
                safe->graph.historyFilePath.clear();
                safe->recordHistoryDecision(capturedPath, HistoryDecision::Copied,
                                            copyFile.getFullPathName().toStdString());
            } else {
                // Ignore: detach from the sidecar for this session.
                safe->graph.historyFilePath.clear();
                safe->recordHistoryDecision(capturedPath, HistoryDecision::Ignored);
            }
        });
}

void MainContentComponent::offerSharedHistoryOnSaveAs(const juce::String& savedProjectPath) {
    // Post-save opt-in: after the user saves a project under a new
    // name, ask whether the undo tree should be bundled alongside the
    // .ssp. If yes, write the sidecar and record the path in
    // graph.historyFilePath so future saves keep it in sync. The
    // historyFile= field gets persisted to the .ssp on the next save.
    juce::Component::SafePointer<MainContentComponent> safe(this);
    juce::String capturedPath = savedProjectPath;

    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle("Include Undo History?")
            .withMessage(
                "Would you like to save the undo history alongside "
                "this project so it travels with the file when you "
                "share it?\n\n"
                "This creates a sidecar file next to the .ssp. Anyone "
                "who opens the project will be offered the history "
                "(they can adopt it, keep a private copy, or ignore "
                "it).\n\n"
                "You can skip this - the undo history will still be "
                "saved privately on your machine either way.")
            .withButton("Yes, Include")
            .withButton("No Thanks"),
        [safe, capturedPath](int result) {
            if (!safe) return;
            if (result != 1) return;
            // Write the sidecar next to the .ssp using a conventional
            // name (same stem, .history extension).
            auto projFile = juce::File(capturedPath);
            auto sidecar = projFile.getSiblingFile(
                projFile.getFileNameWithoutExtension() + ".history");
            safe->writeSharedHistorySidecar(sidecar);
            // writeSharedHistorySidecar updated graph.historyFilePath;
            // re-save the project file once so its historyFile= field
            // reflects the new sidecar association.
            if (!ProjectFile::currentPath.empty()) {
                ProjectFile::save(ProjectFile::currentPath,
                                  safe->graph,
                                  &safe->audioEngine.getGraphProcessor());
            }
            // Remember: on subsequent opens of this project, don't
            // re-prompt - the user knows their own sidecar.
            safe->recordHistoryDecision(capturedPath, HistoryDecision::Adopted);
        });
}

void MainContentComponent::tryRestoreUndoTree() {
    auto file = getUndoTreeFile();
    if (!file.existsAsFile()) return;
    std::ifstream in(file.getFullPathName().toStdString());
    if (!in) return;
    if (!graph.undoTree.restoreFrom(in)) {
        // Corrupted or incompatible - drop it.
        in.close();
        file.deleteFile();
        return;
    }
    // The restored tree's currentNodeId already points at the right step;
    // load that snapshot into the live graph so the in-memory state matches
    // what undo/redo will revert to. (Without this, the graph the user sees
    // is whatever the autosave or last-loaded project says, which may not
    // line up with the undo position.)
    const auto& snap = graph.undoTree.currentSnapshot();
    if (!snap.empty() && graph.undoTree.onLoadSnapshot)
        graph.undoTree.onLoadSnapshot(snap);
    // Don't mark dirty for persistence - we just read this from disk.
    undoTreeDirty = false;
}

void MainContentComponent::tryRecoverAutosave() {
    if (autosaveRecoveryOffered) return;
    autosaveRecoveryOffered = true;

    auto autoFile = getAutosaveFile();

    // The previous run exited cleanly (its session lock was removed). Any
    // autosave still on disk is therefore stale - a clean exit normally sweeps
    // it, but a leftover (e.g. the autosave worker re-wrote autosave.ssp in the
    // last few milliseconds before the process exited, after discardAutosave
    // already ran) must NOT trigger a false "didn't shut down cleanly" prompt.
    // Sweep any straggler silently and just restore the persisted undo tree.
    if (!startupWasUncleanShutdown) {
        if (autoFile.existsAsFile()) discardAutosave();
        tryRestoreUndoTree();
        return;
    }

    if (!autoFile.existsAsFile()) {
        // Unclean shutdown, but nothing was autosaved (e.g. crashed before the
        // first autosave tick). Still restore the persisted undo tree if any.
        tryRestoreUndoTree();
        return;
    }

    juce::String originalPath;
    juce::String timestamp;
    if (auto meta = juce::parseXML(getAutosaveMetaFile())) {
        if (meta->getTagName() == "Autosave") {
            originalPath = meta->getStringAttribute("originalPath");
            timestamp = meta->getStringAttribute("timestamp");
        }
    }

    juce::String what = originalPath.isEmpty()
        ? juce::String("an unsaved project")
        : ("\"" + juce::File(originalPath).getFileName() + "\"");
    juce::String when = timestamp.isEmpty() ? juce::String("earlier")
                                             : ("at " + timestamp);

    juce::String message =
        "SoundShop didn't shut down cleanly last time.\n\n"
        "An autosaved version of " + what + " was saved " + when + ".\n\n"
        "Recover it? (Discard throws the autosave away and opens "
        "whatever would normally load at startup.)";

    juce::Component::SafePointer<MainContentComponent> safe(this);
    juce::String capturedOriginal = originalPath;
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle("Recover Autosaved Version?")
            .withMessage(message)
            .withButton("Recover")
            .withButton("Discard"),
        [safe, capturedOriginal](int result) {
            if (!safe) return;
            if (result != 1) {
                safe->discardAutosave();
                safe->discardUndoTreePersist();
                return;
            }
            // Load the autosave blob into the graph, then patch
            // ProjectFile::currentPath back to the original path so that
            // Ctrl+S saves to the real file, not the autosave. Leave the
            // project dirty so the user knows there are unsaved changes.
            safe->editorPanels.clear();
            // Lock for the batch mutation (see node_graph.h mutationLock
            // comment). Autosave recovery happens after the device is
            // already running, so the audio callback is actively iterating
            // graph.nodes and would otherwise race with the load.
            {
                std::lock_guard<std::recursive_mutex> graphLk(safe->graph.mutationLock);
                ProjectFile::load(getAutosaveFile().getFullPathName().toStdString(),
                                  safe->graph, &safe->audioEngine.getPluginHost());
                safe->upgradeLegacyNodes();
                // Per-plugin override files contain newer plugin state than
                // the (full but periodically-stale) autosave.ssp. Apply them
                // on top so the user sees the most recent plugin tweaks.
                safe->applyPerPluginOverrides();
                safe->cleanupOrphanPluginFiles();
            }
            ProjectFile::currentPath = capturedOriginal.toStdString();
            safe->projectDirty = true;
            safe->graph.dirty = true;

            auto editorIds = safe->graph.openEditors;
            safe->graph.openEditors.clear();
            for (int id : editorIds)
                if (auto* node = safe->graph.findNode(id))
                    safe->openEditor(*node);

            // Restore the saved pan/zoom from the autosave (or fit-all if
            // none was persisted) instead of unconditionally re-fitting.
            safe->graphComponent->notifyProjectLoaded();
            safe->graphComponent->repaint();

            // After loading the autosave, also restore the undo tree
            // from disk if one exists. The tree's currentNodeId picks up
            // wherever the user was in their history before the crash.
            safe->tryRestoreUndoTree();
        });
}

// ==============================================================================
// Script Console Dialog
// ==============================================================================

class ScriptConsoleComponent : public juce::Component {
public:
    ScriptConsoleComponent(ScriptEngine& engine, NodeGraph& graph, int activeNodeIdx = -1)
        : engine(engine), graph(graph), activeNodeIdx(activeNodeIdx) {

        addAndMakeVisible(codeEditor);
        addAndMakeVisible(outputEditor);
        addAndMakeVisible(runBtn);
        addAndMakeVisible(clearBtn);
        addAndMakeVisible(loadBtn);
        addAndMakeVisible(saveBtn);
        addAndMakeVisible(recentBtn);

        codeEditor.setMultiLine(true);
        codeEditor.setReturnKeyStartsNewLine(true);
        codeEditor.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 13.0f, 0));
        if (activeNodeIdx >= 0) {
            // Track-specific template
            juce::String nodeName = "?";
            if (activeNodeIdx < (int)graph.nodes.size())
                nodeName = graph.nodes[activeNodeIdx].name;
            codeEditor.setText(
                "import soundshop as ss\n"
                "import soundshop_tools as tools\n"
                "\n"
                "# Running on: " + nodeName + " (index " + juce::String(activeNodeIdx) + ")\n"
                "idx = ss.this_node()\n"
                "node = ss.get_node(idx)\n"
                "print(f'Track: {node[\"name\"]}, clips: {node[\"num_clips\"]}')\n"
                "\n"
                "# Add a C major scale\n"
                "# tools.add_scale(idx, 0, 'C', 'ionian', octave=4)\n"
                "\n"
                "# Add a chord progression\n"
                "# tools.add_chord_progression(idx, 0, 'C',\n"
                "#     [('I', 4), ('V', 4), ('vi', 4), ('IV', 4)])\n"
                "\n"
                "# Add an arpeggio\n"
                "# tools.add_arpeggio(idx, 0, 'C4 E4 G4 C5',\n"
                "#     note_duration=0.25, pattern='updown', repeats=2)\n"
            );
        } else {
            codeEditor.setText(
                "import soundshop as ss\n"
                "import soundshop_tools as tools\n"
                "from soundshop_music import Note, detect_keys\n"
                "\n"
                "# Show project overview\n"
                "tools.print_project()\n"
                "\n"
                "# --- Examples (uncomment to try) ---\n"
                "# tools.add_scale(0, 0, 'C', 'major', octave=4)\n"
                "# tools.add_chord_progression(0, 0, 'C',\n"
                "#     [('I', 4), ('V', 4), ('vi', 4), ('IV', 4)])\n"
            );
        }

        outputEditor.setMultiLine(true);
        outputEditor.setReadOnly(true);
        outputEditor.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 12.0f, 0));
        outputEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(30, 30, 35));

        runBtn.setButtonText("Run (Ctrl+Enter)");
        runBtn.onClick = [this]() { runScript(); };

        clearBtn.setButtonText("Clear Output");
        clearBtn.onClick = [this]() { outputEditor.clear(); };

        loadBtn.setButtonText("Load...");
        loadBtn.onClick = [this]() { loadScript(); };

        saveBtn.setButtonText("Save...");
        saveBtn.onClick = [this]() { saveScript(); };

        recentBtn.setButtonText("Recent");
        recentBtn.onClick = [this]() { showRecentMenu(); };

        loadRecentList();

        // If no Python interpreter is available, the console can't run anything.
        // Say so up front (rather than silently doing nothing on Run) and
        // explain how to enable it — see CLAUDE.md's grayed-control rule.
        if (!ScriptEngine::pythonAvailable()) {
            outputEditor.setText(
                "Python scripting is disabled: no Python interpreter was found.\n"
                "SEANCE delay-loads Python, so it runs fine without it — but the\n"
                "Script Console, Python signal evaluation, and the Python shape\n"
                "baker need a Python install. Install Python (matching this build's\n"
                "version) so its DLL is on the system PATH, then restart SEANCE.");
            runBtn.setEnabled(false);
            runBtn.setTooltip("Disabled — no Python interpreter was found. Install "
                              "Python and restart SEANCE to enable scripting.");
        }

        setSize(700, 550);
    }

    void resized() override {
        auto area = getLocalBounds().reduced(6);
        auto top = area.removeFromTop(28);
        runBtn.setBounds(top.removeFromLeft(120).reduced(0, 2));
        clearBtn.setBounds(top.removeFromLeft(90).reduced(2, 2));
        top.removeFromLeft(8);
        loadBtn.setBounds(top.removeFromLeft(60).reduced(0, 2));
        saveBtn.setBounds(top.removeFromLeft(60).reduced(2, 2));
        recentBtn.setBounds(top.removeFromLeft(60).reduced(2, 2));

        area.removeFromTop(4);
        auto codeArea = area.removeFromTop(area.getHeight() * 6 / 10);
        codeEditor.setBounds(codeArea);
        area.removeFromTop(4);
        outputEditor.setBounds(area);
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (key.getModifiers().isCtrlDown() && key.getKeyCode() == juce::KeyPress::returnKey) {
            runScript();
            return true;
        }
        if (key.getModifiers().isCtrlDown() && key.getKeyCode() == 'O') {
            loadScript();
            return true;
        }
        if (key.getModifiers().isCtrlDown() && key.getKeyCode() == 'S') {
            saveScript();
            return true;
        }
        return false;
    }

    void runScript() {
        auto code = codeEditor.getText().toStdString();
        auto result = engine.run(code, graph, activeNodeIdx);
        outputEditor.setText(result);
    }

    void loadScript() {
        fileChooser = std::make_shared<juce::FileChooser>(
            "Load Script", getLastDirectory(), "*.py");
        fileChooser->launchAsync(juce::FileBrowserComponent::openMode,
            [this](const juce::FileChooser& fc) {
                auto file = fc.getResult();
                if (file.existsAsFile()) {
                    codeEditor.setText(file.loadFileAsString());
                    addToRecent(file.getFullPathName());
                }
            });
    }

    void saveScript() {
        fileChooser = std::make_shared<juce::FileChooser>(
            "Save Script", getLastDirectory(), "*.py");
        fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                     | juce::FileBrowserComponent::warnAboutOverwriting,
            [this](const juce::FileChooser& fc) {
                auto file = fc.getResult();
                if (file != juce::File()) {
                    auto path = file.getFullPathName();
                    if (!path.endsWith(".py"))
                        file = juce::File(path + ".py");
                    file.replaceWithText(codeEditor.getText());
                    addToRecent(file.getFullPathName());
                }
            });
    }

    void showRecentMenu() {
        juce::PopupMenu menu;
        if (recentScripts.empty()) {
            menu.addItem(-1, "(no recent scripts)", false);
        } else {
            for (int i = 0; i < (int)recentScripts.size(); ++i) {
                auto file = juce::File(recentScripts[i]);
                menu.addItem(i + 1, file.getFileName() + "  -  " + file.getParentDirectory().getFileName());
            }
            menu.addSeparator();
            menu.addItem(999, "Clear Recents");
        }
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(recentBtn),
            [this](int result) {
                if (result == 999) {
                    recentScripts.clear();
                    saveRecentList();
                } else if (result > 0 && result <= (int)recentScripts.size()) {
                    auto file = juce::File(recentScripts[result - 1]);
                    if (file.existsAsFile()) {
                        codeEditor.setText(file.loadFileAsString());
                        addToRecent(file.getFullPathName());
                    } else {
                        // Remove stale entry
                        recentScripts.erase(recentScripts.begin() + (result - 1));
                        saveRecentList();
                    }
                }
            });
    }

private:
    ScriptEngine& engine;
    NodeGraph& graph;
    int activeNodeIdx = -1;
    juce::TextEditor codeEditor, outputEditor;
    juce::TextButton runBtn, clearBtn, loadBtn, saveBtn, recentBtn;
    std::shared_ptr<juce::FileChooser> fileChooser;
    std::vector<juce::String> recentScripts;

    static juce::File getRecentsFile() {
        return juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                   .getSiblingFile("soundshop_recent_scripts.txt");
    }

    static juce::File getLastDirectory() {
        return juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                   .getParentDirectory();
    }

    void addToRecent(const juce::String& path) {
        // Remove if already present, then prepend
        recentScripts.erase(
            std::remove(recentScripts.begin(), recentScripts.end(), path),
            recentScripts.end());
        recentScripts.insert(recentScripts.begin(), path);
        if (recentScripts.size() > 10)
            recentScripts.resize(10);
        saveRecentList();
    }

    void loadRecentList() {
        recentScripts.clear();
        auto file = getRecentsFile();
        if (!file.existsAsFile()) return;
        juce::StringArray lines;
        lines.addLines(file.loadFileAsString());
        for (auto& line : lines)
            if (line.isNotEmpty())
                recentScripts.push_back(line);
    }

    void saveRecentList() {
        juce::String text;
        for (auto& path : recentScripts)
            text += path + "\n";
        getRecentsFile().replaceWithText(text);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScriptConsoleComponent)
};

void MainContentComponent::runScriptFile(const juce::String& path) {
    auto file = juce::File(path);
    if (!file.existsAsFile()) return;

    if (!scriptEngine.isInitialized())
        scriptEngine.init();

    auto code = file.loadFileAsString().toStdString();
    auto result = scriptEngine.run(code, graph);

    // Add to recent scripts
    auto recentsFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                           .getSiblingFile("soundshop_recent_scripts.txt");
    juce::StringArray lines;
    if (recentsFile.existsAsFile())
        lines.addLines(recentsFile.loadFileAsString());
    // Remove if already present, prepend
    lines.removeString(path);
    lines.insert(0, path);
    while (lines.size() > 10) lines.remove(lines.size() - 1);
    recentsFile.replaceWithText(lines.joinIntoString("\n"));

    // Show output in a simple dialog
    auto* aw = new juce::AlertWindow("Script Output: " + file.getFileName(),
        "", juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor("output", result, "", true);
    aw->getTextEditor("output")->setFont(
        juce::Font(juce::Font::getDefaultMonospacedFontName(), 12.0f, 0));
    aw->getTextEditor("output")->setReadOnly(true);
    aw->addButton("OK", 1);
    aw->enterModalState(true, juce::ModalCallbackFunction::create(
        [aw](int) { delete aw; }), true);

    graphComponent->repaint();
    for (auto& panel : editorPanels)
        panel->component->repaint();
}

void MainContentComponent::browseAndRunScript() {
    auto chooser = std::make_shared<juce::FileChooser>(
        "Run Script", juce::File(), "*.py");
    chooser->launchAsync(juce::FileBrowserComponent::openMode,
        [this, chooser](const juce::FileChooser& fc) {
            auto file = fc.getResult();
            if (file.existsAsFile())
                runScriptFile(file.getFullPathName());
        });
}

void MainContentComponent::showScriptConsole() {
    showScriptConsoleForNode(-1);
}

void MainContentComponent::showScriptConsoleForNode(int nodeId) {
    if (!scriptEngine.isInitialized())
        scriptEngine.init();

    // Find the node index for this node ID
    int nodeIdx = -1;
    auto* node = graph.findNode(nodeId);
    if (node) {
        for (int i = 0; i < (int)graph.nodes.size(); ++i)
            if (graph.nodes[i].id == nodeId) { nodeIdx = i; break; }
    }

    auto* dlg = new ScriptConsoleComponent(scriptEngine, graph, nodeIdx);
    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(dlg);
    opts.dialogTitle = node ? "Script: " + juce::String(node->name) : "Script Console";
    opts.dialogBackgroundColour = juce::Colour(40, 40, 45);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = false;
    opts.resizable = true;
    opts.componentToCentreAround = this;
    SoundShop::launchToolDialog(opts);
}

// ==============================================================================
// Plugin Settings Dialog
// ==============================================================================

class PluginSettingsComponent : public juce::Component,
                                 public juce::ListBoxModel {
public:
    PluginSettingsComponent(PluginSettings& settings, PluginHost& host,
                             NodeGraph& graph)
        : settings(settings), host(host), graph(graph) {

        addAndMakeVisible(dirList);
        dirList.setModel(this);
        dirList.setRowHeight(20);

        addAndMakeVisible(addDirBtn);
        addAndMakeVisible(removeDirBtn);
        addAndMakeVisible(resetDirsBtn);
        addAndMakeVisible(scanBtn);
        addAndMakeVisible(dirInput);
        addAndMakeVisible(statusLabel);
        addAndMakeVisible(pluginList);

        addDirBtn.setButtonText("Add");
        removeDirBtn.setButtonText("Remove");
        resetDirsBtn.setButtonText("Reset Defaults");
        scanBtn.setButtonText("Scan Now");
        addDirBtn.setTooltip("Add the path in the text field below to the list of directories scanned for plugins");
        removeDirBtn.setTooltip("Remove the selected directory from the scan list");
        resetDirsBtn.setTooltip("Replace the scan list with the OS default plugin directories (Program Files/VST3, /Library/Audio/Plug-Ins, etc.)");
        scanBtn.setTooltip("Walk the listed directories now and load any new plugins. Plugins that crash during scan are automatically blocklisted.");
        dirInput.setTooltip("Type or paste a directory path here, then click Add");
        statusLabel.setText(juce::String((int)host.getAvailablePlugins().size()) + " plugins",
                            juce::dontSendNotification);

        addDirBtn.onClick = [this]() {
            auto text = dirInput.getText().toStdString();
            if (!text.empty()) {
                this->settings.scanDirs.push_back(text);
                dirInput.clear();
                dirList.updateContent();
            }
        };
        removeDirBtn.onClick = [this]() {
            int row = dirList.getSelectedRow();
            if (row >= 0 && row < (int)this->settings.scanDirs.size()) {
                this->settings.scanDirs.erase(this->settings.scanDirs.begin() + row);
                dirList.updateContent();
            }
        };
        resetDirsBtn.onClick = [this]() {
            this->settings.scanDirs.clear();
            this->settings.addDefaultDirs();
            dirList.updateContent();
        };
        scanBtn.onClick = [this]() {
            this->host.scanForPlugins(this->settings.scanDirs, this->settings.blockedPlugins);
            for (auto& f : this->host.failedPlugins)
                this->settings.blockedPlugins.insert(f);
            this->settings.save("soundshop_plugins.cfg");
            this->host.saveScanCache("soundshop_plugins_cache.dat");
            statusLabel.setText(juce::String((int)this->host.getAvailablePlugins().size()) + " plugins found",
                                juce::dontSendNotification);
            pluginList.updateContent();
        };

        pluginListModel.host = &host;
        pluginListModel.settings = &settings;
        pluginListModel.graph = &graph;
        pluginListModel.listBox = &pluginList;
        pluginList.setModel(&pluginListModel);
        pluginList.setRowHeight(22);
        pluginList.updateContent();
        addAndMakeVisible(addToGraphBtn);
        addToGraphBtn.setButtonText("Add to Graph");
        addToGraphBtn.setTooltip("Create a new node in the graph for the selected plugin and load it");
        addToGraphBtn.onClick = [this]() {
            int row = pluginList.getSelectedRow();
            auto& plugins = this->host.getAvailablePlugins();
            if (row >= 0 && row < (int)plugins.size()) {
                auto& pi = plugins[row];
                std::vector<Pin> ins, outs;
                if (pi.hasMidiInput) ins.push_back({0, "MIDI In", PinKind::Midi, true});
                if (pi.hasAudioInput) ins.push_back({0, "Audio In", PinKind::Audio, true, pi.numAudioInputChannels});
                if (pi.hasAudioOutput) outs.push_back({0, "Audio Out", PinKind::Audio, false, pi.numAudioOutputChannels});
                if (pi.hasMidiOutput) outs.push_back({0, "MIDI Out", PinKind::Midi, false});
                auto type = pi.isInstrument ? NodeType::Instrument : NodeType::Effect;
                auto& n = this->graph.addNode(pi.name, type, ins, outs, {100, 100});
                auto loaded = this->host.loadPlugin(row, 44100.0, 512);
                if (loaded) { n.plugin = std::move(loaded); n.pluginIndex = row; }
            }
        };

        setSize(650, 500);
    }

    void resized() override {
        auto area = getLocalBounds().reduced(8);

        auto top = area.removeFromTop(150);
        top.removeFromTop(2);
        auto dirBtns = top.removeFromBottom(28);
        dirList.setBounds(top);
        int bx = 0;
        removeDirBtn.setBounds(dirBtns.getX() + bx, dirBtns.getY() + 2, 65, 24); bx += 68;
        resetDirsBtn.setBounds(dirBtns.getX() + bx, dirBtns.getY() + 2, 100, 24); bx += 104;
        dirInput.setBounds(dirBtns.getX() + bx, dirBtns.getY() + 2, dirBtns.getWidth() - bx - 45, 24);
        addDirBtn.setBounds(dirBtns.getRight() - 40, dirBtns.getY() + 2, 38, 24);

        area.removeFromTop(8);
        auto scanRow = area.removeFromTop(28);
        scanBtn.setBounds(scanRow.removeFromLeft(80).reduced(0, 2));
        statusLabel.setBounds(scanRow.removeFromLeft(150).reduced(4, 2));
        addToGraphBtn.setBounds(scanRow.removeFromLeft(100).reduced(0, 2));

        area.removeFromTop(4);
        pluginList.setBounds(area);
    }

    // ListBoxModel for directories
    int getNumRows() override { return (int)settings.scanDirs.size(); }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override {
        if (selected) g.fillAll(juce::Colour(50, 70, 100));
        if (row >= 0 && row < (int)settings.scanDirs.size()) {
            g.setColour(juce::Colours::white);
            g.setFont(12.0f);
            g.drawText(settings.scanDirs[row], 4, 0, w - 8, h, juce::Justification::centredLeft);
        }
    }

private:
    PluginSettings& settings;
    PluginHost& host;
    NodeGraph& graph;

    juce::ListBox dirList{"Directories"};
    juce::TextButton addDirBtn, removeDirBtn, resetDirsBtn, scanBtn, addToGraphBtn;
    juce::TextEditor dirInput;
    juce::Label statusLabel;
    juce::ListBox pluginList{"Plugins"};

    // Plugin list model
    struct PluginListModel : public juce::ListBoxModel {
        PluginHost* host = nullptr;
        PluginSettings* settings = nullptr;
        NodeGraph* graph = nullptr;
        juce::ListBox* listBox = nullptr;

        int getNumRows() override {
            return host ? (int)host->getAvailablePlugins().size() : 0;
        }
        void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override {
            if (selected) g.fillAll(juce::Colour(50, 70, 100));
            if (!host) return;
            auto& plugins = host->getAvailablePlugins();
            if (row >= 0 && row < (int)plugins.size()) {
                auto& pi = plugins[row];
                bool blocked = settings && settings->isBlocked(pi.fileOrId);
                g.setColour(blocked ? juce::Colour(130, 80, 80) : juce::Colours::white);
                g.setFont(12.0f);
                auto label = pi.name + "  (" + pi.manufacturer + ")  [" + pi.format + "]";
                if (pi.hasAudioInput) label += "  audio in:" + std::to_string(pi.numAudioInputChannels);
                if (pi.hasAudioOutput) label += "  audio out:" + std::to_string(pi.numAudioOutputChannels);
                if (pi.numMidiInputPorts > 0) label += "  midi in:" + std::to_string(pi.numMidiInputPorts);
                if (pi.numMidiOutputPorts > 0) label += "  midi out:" + std::to_string(pi.numMidiOutputPorts);
                if (pi.isInstrument) label += "  [Instrument]";
                if (blocked) label += "  [BLOCKED]";
                g.drawText(label, 4, 0, w - 8, h, juce::Justification::centredLeft);
            }
        }
        void listBoxItemClicked(int row, const juce::MouseEvent& e) override {
            if (!e.mods.isRightButtonDown() || !host) return;
            auto& plugins = host->getAvailablePlugins();
            if (row < 0 || row >= (int)plugins.size()) return;
            auto& pi = plugins[row];
            bool blocked = settings && settings->isBlocked(pi.fileOrId);

            juce::PopupMenu menu;
            menu.addSectionHeader(pi.name);
            menu.addItem(1, "Add to Graph");
            menu.addItem(2, "Copy Path");
            menu.addSeparator();
            menu.addItem(3, blocked ? "Unblock" : "Block");

            menu.showMenuAsync(juce::PopupMenu::Options(), [this, row, pi, blocked](int result) {
                if (result == 1 && graph) {
                    std::vector<Pin> ins, outs;
                    if (pi.hasMidiInput) ins.push_back({0, "MIDI In", PinKind::Midi, true});
                    if (pi.hasAudioInput) ins.push_back({0, "Audio In", PinKind::Audio, true, pi.numAudioInputChannels});
                    if (pi.hasAudioOutput) outs.push_back({0, "Audio Out", PinKind::Audio, false, pi.numAudioOutputChannels});
                    if (pi.hasMidiOutput) outs.push_back({0, "MIDI Out", PinKind::Midi, false});
                    auto type = pi.isInstrument ? NodeType::Instrument : NodeType::Effect;
                    auto& n = graph->addNode(pi.name, type, ins, outs, {100, 100});
                    auto loaded = host->loadPlugin(row, 44100.0, 512);
                    if (loaded) { n.plugin = std::move(loaded); n.pluginIndex = row; }
                } else if (result == 2) {
                    juce::SystemClipboard::copyTextToClipboard(pi.fileOrId);
                } else if (result == 3 && settings) {
                    if (blocked) settings->blockedPlugins.erase(pi.fileOrId);
                    else settings->blockedPlugins.insert(pi.fileOrId);
                    settings->save("soundshop_plugins.cfg");
                    if (listBox) listBox->repaint();
                }
            });
        }
    } pluginListModel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginSettingsComponent)
};

void MainContentComponent::showAudioDeviceSettings() {
    auto* dm = audioEngine.getDeviceManager();
    if (!dm) return;

    // Shared with the microphone-capture dialog's "Audio device..." button so
    // both routes build the same AudioDeviceSelectorComponent.
    SoundShop::launchAudioDeviceSettings(*dm, this);
    return;

#if 0
    // --- Legacy custom panel below (kept for reference, not reached) ---
    auto* device = dm->getCurrentAudioDevice();

    juce::String info;
    info += "=== Current Device ===\n";
    if (device) {
        info += "Name: " + device->getName() + "\n";
        info += "Type: " + device->getTypeName() + "\n";
        info += "Sample Rate: " + juce::String(device->getCurrentSampleRate(), 0) + " Hz\n";
        info += "Buffer Size: " + juce::String(device->getCurrentBufferSizeSamples()) + " samples\n";
        info += "Input Channels: " + juce::String(device->getActiveInputChannels().countNumberOfSetBits()) + "\n";
        info += "Output Channels: " + juce::String(device->getActiveOutputChannels().countNumberOfSetBits()) + "\n";
        info += "Bit Depth: " + juce::String(device->getCurrentBitDepth()) + "-bit\n";
        info += "Latency (in): " + juce::String(device->getInputLatencyInSamples()) + " samples\n";
        info += "Latency (out): " + juce::String(device->getOutputLatencyInSamples()) + " samples\n";

        info += "\n=== Supported Sample Rates ===\n";
        auto rates = device->getAvailableSampleRates();
        for (auto r : rates)
            info += "  " + juce::String(r, 0) + " Hz" +
                (std::abs(r - device->getCurrentSampleRate()) < 1 ? "  [active]" : "") + "\n";

        info += "\n=== Supported Buffer Sizes ===\n";
        auto sizes = device->getAvailableBufferSizes();
        for (auto s : sizes)
            info += "  " + juce::String(s) + " samples" +
                (s == device->getCurrentBufferSizeSamples() ? "  [active]" : "") + "\n";

        info += "\nDevice bit depth: " + juce::String(device->getCurrentBitDepth()) + "-bit\n";
    } else {
        info += "No audio device available\n";
    }

    info += "\n=== Project Settings ===\n";
    info += "Project Sample Rate: ";
    if (graph.projectSampleRate > 0)
        info += juce::String(graph.projectSampleRate, 0) + " Hz\n";
    else
        info += "Same as device\n";
    info += "Internal Processing: 32-bit float (always)\n";

    // Show in a dialog with a text editor and device selector
    auto* comp = new juce::Component();
    auto* textEd = new juce::TextEditor();
    textEd->setMultiLine(true);
    textEd->setReadOnly(true);
    textEd->setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 12.0f, 0));
    textEd->setText(info);
    textEd->setColour(juce::TextEditor::backgroundColourId, juce::Colour(30, 30, 35));
    comp->addAndMakeVisible(textEd);

    // Add sample rate and buffer size combo boxes
    auto* srLabel = new juce::Label({}, "Device Sample Rate:");
    auto* srCombo = new juce::ComboBox();
    auto* bsLabel = new juce::Label({}, "Buffer Size:");
    auto* bsCombo = new juce::ComboBox();

    comp->addAndMakeVisible(srLabel);
    comp->addAndMakeVisible(srCombo);
    comp->addAndMakeVisible(bsLabel);
    comp->addAndMakeVisible(bsCombo);

    if (device) {
        auto rates = device->getAvailableSampleRates();
        for (int i = 0; i < (int)rates.size(); ++i) {
            srCombo->addItem(juce::String(rates[i], 0) + " Hz", i + 1);
            if (std::abs(rates[i] - device->getCurrentSampleRate()) < 1)
                srCombo->setSelectedItemIndex(i);
        }

        auto sizes = device->getAvailableBufferSizes();
        for (int i = 0; i < (int)sizes.size(); ++i) {
            bsCombo->addItem(juce::String(sizes[i]) + " samples", i + 1);
            if (sizes[i] == device->getCurrentBufferSizeSamples())
                bsCombo->setSelectedItemIndex(i);
        }
    }

    comp->setSize(500, 500);

    // Layout helper
    struct LayoutHelper : public juce::ComponentListener {
        juce::TextEditor* text;
        juce::Label* srL; juce::ComboBox* srC;
        juce::Label* bsL; juce::ComboBox* bsC;
        LayoutHelper(juce::TextEditor* t, juce::Label* sl, juce::ComboBox* sc,
                     juce::Label* bl, juce::ComboBox* bc)
            : text(t), srL(sl), srC(sc), bsL(bl), bsC(bc) {}
        void componentMovedOrResized(juce::Component& c, bool, bool resized) override {
            if (!resized) return;
            auto area = c.getLocalBounds().reduced(6);
            auto bottom = area.removeFromBottom(60);
            auto row1 = bottom.removeFromTop(28);
            srL->setBounds(row1.removeFromLeft(130));
            srC->setBounds(row1.removeFromLeft(150));
            auto row2 = bottom.removeFromTop(28);
            bsL->setBounds(row2.removeFromLeft(130));
            bsC->setBounds(row2.removeFromLeft(150));
            text->setBounds(area);
        }
    };
    auto* layout = new LayoutHelper(textEd, srLabel, srCombo, bsLabel, bsCombo);
    comp->addComponentListener(layout);
    // Trigger initial layout
    layout->componentMovedOrResized(*comp, false, true);

    // Apply changes when combo boxes change
    auto* dmPtr = dm;
    srCombo->onChange = [dmPtr, device, srCombo]() {
        if (!device) return;
        auto rates = device->getAvailableSampleRates();
        int idx = srCombo->getSelectedItemIndex();
        if (idx >= 0 && idx < (int)rates.size()) {
            auto setup = dmPtr->getAudioDeviceSetup();
            setup.sampleRate = rates[idx];
            dmPtr->setAudioDeviceSetup(setup, true);
        }
    };
    bsCombo->onChange = [dmPtr, device, bsCombo]() {
        if (!device) return;
        auto sizes = device->getAvailableBufferSizes();
        int idx = bsCombo->getSelectedItemIndex();
        if (idx >= 0 && idx < (int)sizes.size()) {
            auto setup = dmPtr->getAudioDeviceSetup();
            setup.bufferSize = sizes[idx];
            dmPtr->setAudioDeviceSetup(setup, true);
        }
    };

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(comp);
    opts.dialogTitle = "Audio Device Settings";
    opts.dialogBackgroundColour = juce::Colour(40, 40, 45);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = false;
    opts.resizable = true;
    opts.componentToCentreAround = this;
    SoundShop::launchToolDialog(opts);
#endif
}

void MainContentComponent::showAssetLibraryDialog() {
    auto* dlg = new AssetLibraryComponent(graph,
        [this](const std::string& desc) {
            projectDirty = true;
            graph.dirty = true;
            graph.commitSnapshot(desc);
            if (graphComponent) graphComponent->repaint();
        });

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(dlg);
    opts.dialogTitle = "Asset Library";
    opts.dialogBackgroundColour = juce::Colour(40, 40, 45);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = false;
    opts.resizable = true;
    opts.componentToCentreAround = this;
    SoundShop::launchToolDialog(opts);
}

void MainContentComponent::showPluginSettingsDialog() {
    auto* dlg = new PluginSettingsComponent(pluginSettings, audioEngine.getPluginHost(), graph);

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(dlg);
    opts.dialogTitle = "Plugin Settings";
    opts.dialogBackgroundColour = juce::Colour(40, 40, 45);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = false;
    opts.resizable = true;
    opts.componentToCentreAround = this;
    SoundShop::launchToolDialog(opts);
}

void MainContentComponent::showSongSettingsDialog() {
    // Small modal with three inputs: Song Length (beats), Repeat Mode,
    // and Repeat Count (used only when mode == N Times).
    // Build the explanatory header. If the user hasn't set an explicit
    // length, show the auto-derived value so they can see what playback
    // will use without having to guess.
    juce::String header =
        "Song Length is where playback auto-stops (in beats).\n"
        "0 = auto (derived from the last clip across all timelines).\n";
    if (graph.songLengthBeats <= 0) {
        double autoEnd = graph.effectiveSongLengthBeats();
        if (autoEnd > 0)
            header += "Current auto value: " + juce::String(autoEnd, 2) + " beats.\n";
        else
            header += "Current auto value: no clips yet, so playback won't auto-stop.\n";
    }
    header += "\nRepeat Mode:\n"
              "  None    - stop at Song Length.\n"
              "  Forever - loop back to beat 0 until Stop is pressed.\n"
              "  N Times - loop back and play N times total, then stop.";
    auto* aw = new juce::AlertWindow("Song Length + Repeat", header,
        juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor("length",
        juce::String(graph.songLengthBeats, 2),
        "Song Length (beats, 0 = auto):");
    aw->addComboBox("mode", {"None", "Forever (until Stop)", "N Times"}, "Repeat Mode:");
    aw->getComboBoxComponent("mode")->setSelectedItemIndex((int)graph.songRepeatMode,
                                                            juce::dontSendNotification);
    aw->addTextEditor("count",
        juce::String(graph.songRepeatCount),
        "Repeat Count (N):");
    aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    aw->addButton("Cancel", 0);
    aw->enterModalState(true, juce::ModalCallbackFunction::create(
        [this, aw](int res) {
            if (res == 1) {
                float len = aw->getTextEditorContents("length").getFloatValue();
                int modeIdx = aw->getComboBoxComponent("mode")->getSelectedItemIndex();
                int count = aw->getTextEditorContents("count").getIntValue();
                graph.songLengthBeats = std::max(0.0f, len);
                graph.songRepeatMode = (modeIdx == 1 ? NodeGraph::SongRepeat::Forever
                                        : modeIdx == 2 ? NodeGraph::SongRepeat::NTimes
                                        :                NodeGraph::SongRepeat::None);
                graph.songRepeatCount = std::max(1, count);
                projectDirty = true;
                graph.dirty = true;
                graph.commitSnapshot("Edit song length / repeat");
            }
            delete aw;
        }), true);
}

// ==============================================================================
// MainWindow
// ==============================================================================

MainWindow::MainWindow(const juce::String& name)
    : DocumentWindow(name, juce::Colours::darkgrey, DocumentWindow::allButtons) {
    setResizable(true, true);
    setResizeLimits(800, 500, 10000, 10000);
    centreWithSize(1440, 900);
    setContentOwned(new MainContentComponent(), true);
    setUsingNativeTitleBar(true);
    restoreWindowState();
    setVisible(true);
}

MainWindow::~MainWindow() {
    saveWindowState();
}

void MainWindow::saveWindowState() {
    auto file = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                    .getSiblingFile("soundshop_window.xml");
    auto xml = std::make_unique<juce::XmlElement>("WindowState");
    xml->setAttribute("x", getX());
    xml->setAttribute("y", getY());
    xml->setAttribute("width", getWidth());
    xml->setAttribute("height", getHeight());
    xml->setAttribute("maximised", isFullScreen() ? 1 : 0);
    xml->writeTo(file);
}

void MainWindow::restoreWindowState() {
    auto file = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                    .getSiblingFile("soundshop_window.xml");
    if (!file.existsAsFile()) return;

    auto xml = juce::parseXML(file);
    if (!xml || xml->getTagName() != "WindowState") return;

    int x = xml->getIntAttribute("x", getX());
    int y = xml->getIntAttribute("y", getY());
    int w = xml->getIntAttribute("width", getWidth());
    int h = xml->getIntAttribute("height", getHeight());
    bool maximised = xml->getIntAttribute("maximised", 0) != 0;

    // Sanity check: make sure the window is at least partially on-screen
    auto displays = juce::Desktop::getInstance().getDisplays();
    auto totalBounds = displays.getTotalBounds(true);
    if (x + w > 0 && x < totalBounds.getRight() && y + h > 0 && y < totalBounds.getBottom()) {
        setBounds(x, y, w, h);
    }

    if (maximised)
        setFullScreen(true);
}

void MainWindow::closeButtonPressed() {
    tryQuit();
}

void MainContentComponent::setupHotkeyCallbacks() {
    // Spacebar (the default Play binding) toggles transport, matching every
    // mainstream DAW: press once to start, again to stop. Route through the
    // full onPlay()/onStop() handlers (not the raw engine calls) so the
    // play-from-top-when-parked-at-end logic, recording teardown, the
    // immediate all-sound panic on stop, and the Play/Pause button label all
    // stay in sync regardless of whether the user clicks the button or hits
    // the key. See REFERENCE.md -> Transport.
    hotkeyManager.setCallback(HotkeyAction::Play, [this]() {
        if (transport.playing) onStop(); else onPlay();
    });
    hotkeyManager.setCallback(HotkeyAction::Stop, [this]() { onStop(); });
    hotkeyManager.setCallback(HotkeyAction::Undo, [this]() { graph.undoTree.doUndo(); graphComponent->repaint(); });
    hotkeyManager.setCallback(HotkeyAction::Redo, [this]() { graph.undoTree.doRedo(); graphComponent->repaint(); });
    hotkeyManager.setCallback(HotkeyAction::SaveProject, [this]() { saveProject(); });
    hotkeyManager.setCallback(HotkeyAction::OpenProject, [this]() { openProject(); });
    hotkeyManager.setCallback(HotkeyAction::NewProject, [this]() { newProject(); });
    hotkeyManager.setCallback(HotkeyAction::SaveProjectAs, [this]() { saveProjectAs(); });
    hotkeyManager.setCallback(HotkeyAction::ExportAudio, [this]() { exportAudio(); });
    hotkeyManager.setCallback(HotkeyAction::FitAll, [this]() { graphComponent->fitAll(); });
    hotkeyManager.setCallback(HotkeyAction::ToggleLoop, [this]() {
        graph.loopEnabled = !graph.loopEnabled;
    });
    hotkeyManager.setCallback(HotkeyAction::ToggleMetronome, [this]() {
        graph.metronomeEnabled = !graph.metronomeEnabled;
    });
    hotkeyManager.setCallback(HotkeyAction::Capture, [this]() { captureBtn.triggerClick(); });
    hotkeyManager.setCallback(HotkeyAction::ToggleKeyboardMidi, [this]() {
        keyboardMidiBtn.triggerClick();
    });
    hotkeyManager.setCallback(HotkeyAction::WriteAutoToSelection, [this]() {
        if (graph.loopEnabled && graph.loopEndBeat > graph.loopStartBeat)
            graph.writeAutomationToSelection((float)graph.loopStartBeat, (float)graph.loopEndBeat);
    });
    hotkeyManager.setCallback(HotkeyAction::ArmAllParams, [this]() {
        graph.armAllParams(true); graphComponent->repaint();
    });
    hotkeyManager.setCallback(HotkeyAction::DisarmAllParams, [this]() {
        graph.armAllParams(false); graphComponent->repaint();
    });
    // Piano roll actions - forward to the active editor's piano roll
    auto pianoRollAction = [this](const std::string& action) {
        for (auto& panel : editorPanels)
            if (panel->nodeId == graph.activeEditorNodeId && panel->component)
                panel->component->triggerAction(action);
    };
    hotkeyManager.setCallback(HotkeyAction::TransposeUpSemi,   [pianoRollAction]() { pianoRollAction("transpose_up_semi"); });
    hotkeyManager.setCallback(HotkeyAction::TransposeDownSemi, [pianoRollAction]() { pianoRollAction("transpose_down_semi"); });
    hotkeyManager.setCallback(HotkeyAction::TransposeUpOctave, [pianoRollAction]() { pianoRollAction("transpose_up_oct"); });
    hotkeyManager.setCallback(HotkeyAction::TransposeDownOctave,[pianoRollAction]() { pianoRollAction("transpose_down_oct"); });
    hotkeyManager.setCallback(HotkeyAction::NudgeLeft,          [pianoRollAction]() { pianoRollAction("nudge_left"); });
    hotkeyManager.setCallback(HotkeyAction::NudgeRight,         [pianoRollAction]() { pianoRollAction("nudge_right"); });
    hotkeyManager.setCallback(HotkeyAction::DoubleDuration,     [pianoRollAction]() { pianoRollAction("double_duration"); });
    hotkeyManager.setCallback(HotkeyAction::HalveDuration,      [pianoRollAction]() { pianoRollAction("halve_duration"); });
    hotkeyManager.setCallback(HotkeyAction::ReverseNotes,       [pianoRollAction]() { pianoRollAction("reverse"); });

    hotkeyManager.setCallback(HotkeyAction::AddMidiTrack, [this]() { addMidiBtn.triggerClick(); });
    hotkeyManager.setCallback(HotkeyAction::AddAudioTrack, [this]() { addAudioBtn.triggerClick(); });
    hotkeyManager.setCallback(HotkeyAction::AssignHotkeys, [this]() { openHotkeySettings(); });

    // Dynamic node action handler: mute/solo/open by node name
    hotkeyManager.onNodeAction = [this](const std::string& nodeName, NodeActionType type) {
        for (auto& n : graph.nodes) {
            if (n.name == nodeName) {
                if (type == NodeActionType::ToggleMute)
                    n.muted = !n.muted;
                else if (type == NodeActionType::ToggleSolo)
                    n.soloed = !n.soloed;
                else if (type == NodeActionType::OpenEditor)
                    showPluginUI(n.id);
                graph.dirty = true;
                graphComponent->repaint();
                return;
            }
        }
    };

    // Load saved hotkeys
    auto configFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                          .getSiblingFile("hotkeys.json");
    hotkeyManager.loadFromFile(configFile);
}

void MainContentComponent::openHotkeySettings() {
    auto* comp = new HotkeySettingsComponent(hotkeyManager, &graph);
    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(comp);
    opts.dialogTitle = "Assign Hotkeys";
    opts.dialogBackgroundColour = juce::Colour(22, 22, 28);
    opts.escapeKeyTriggersCloseButton = false; // escape is used for canceling capture
    opts.useNativeTitleBar = false;
    opts.resizable = true;
    opts.componentToCentreAround = this;
    SoundShop::launchToolDialog(opts);
}

bool MainContentComponent::keyStateChanged(bool isKeyDown) {
    if (!audioEngine.keyboardMidiEnabled.load()) return false;
    if (isKeyDown) return false; // keyPressed handles note-on

    // On key release, check which notes in keysDown are no longer held.
    // JUCE doesn't tell us WHICH key was released, so we poll each one.
    std::vector<int> toRelease;
    for (int note : audioEngine.keysDown) {
        // Reverse-map note to key code to check if it's still held.
        // This is a bit brute-force but keysDown is small (< 20 entries).
        bool stillDown = false;
        // Check all possible key codes that map to this note
        const char* allKeys = "ASDFGHJKLWETYUOP";
        for (int i = 0; allKeys[i]; ++i) {
            if (keyToMidiNote(allKeys[i]) == note) {
                if (juce::KeyPress::isKeyCurrentlyDown(allKeys[i]))
                    stillDown = true;
            }
        }
        if (!stillDown) toRelease.push_back(note);
    }
    for (int note : toRelease) {
        audioEngine.keysDown.erase(note);
        audioEngine.keyboardNoteOff(note);
    }
    return !toRelease.empty();
}

// ==============================================================================
// Computer Keyboard -> MIDI ("Musical Typing")
// ==============================================================================

// Maps a key code to a MIDI note offset from C (0-11), or -1 if not a note key.
// Layout:
//   W E   T Y U   O P       -> C# D#   F# G# A#   C# D#  (black keys)
//   A S D F G H J K L ; '   -> C  D  E  F  G  A  B  C  D  E  F  (white keys)
// Z/X = octave down/up
int MainContentComponent::keyToMidiNote(int keyCode) const {
    int oct = audioEngine.keyboardOctave;
    // White keys: A..L maps to C D E F G A B C D
    switch (keyCode) {
        case 'A': return oct * 12 + 0;   // C
        case 'S': return oct * 12 + 2;   // D
        case 'D': return oct * 12 + 4;   // E
        case 'F': return oct * 12 + 5;   // F
        case 'G': return oct * 12 + 7;   // G
        case 'H': return oct * 12 + 9;   // A
        case 'J': return oct * 12 + 11;  // B
        case 'K': return (oct + 1) * 12 + 0;  // C (next octave)
        case 'L': return (oct + 1) * 12 + 2;  // D (next octave)
        // Black keys: W E T Y U O P
        case 'W': return oct * 12 + 1;   // C#
        case 'E': return oct * 12 + 3;   // D#
        case 'T': return oct * 12 + 6;   // F#
        case 'Y': return oct * 12 + 8;   // G#
        case 'U': return oct * 12 + 10;  // A#
        case 'O': return (oct + 1) * 12 + 1;  // C# (next octave)
        case 'P': return (oct + 1) * 12 + 3;  // D# (next octave)
        default: return -1;
    }
}

bool MainContentComponent::handleKeyboardMidi(const juce::KeyPress& key, bool isDown) {
    if (!audioEngine.keyboardMidiEnabled.load()) return false;

    int keyCode = key.getTextCharacter();
    if (keyCode >= 'a' && keyCode <= 'z') keyCode -= 32; // uppercase

    // Octave up/down
    if (isDown) {
        if (keyCode == 'Z') {
            audioEngine.keyboardOctave = std::max(0, audioEngine.keyboardOctave - 1);
            return true;
        }
        if (keyCode == 'X') {
            audioEngine.keyboardOctave = std::min(8, audioEngine.keyboardOctave + 1);
            return true;
        }
    }

    int note = keyToMidiNote(keyCode);
    if (note < 0) return false;

    if (isDown) {
        if (audioEngine.keysDown.count(note)) return true; // key repeat, ignore
        audioEngine.keysDown.insert(note);
        // Modifier-based velocity zones so a QWERTY keyboard can produce
        // some dynamic range. Shift = louder, Alt = softer, plain = medium.
        int velocity = 90;
        if (key.getModifiers().isShiftDown()) velocity = 120;
        else if (key.getModifiers().isAltDown()) velocity = 50;
        audioEngine.keyboardNoteOn(note, velocity);
    } else {
        audioEngine.keysDown.erase(note);
        audioEngine.keyboardNoteOff(note);
    }
    return true;
}

void MainWindow::tryQuit() {
    auto* content = dynamic_cast<MainContentComponent*>(getContentComponent());
    if (content && content->tryQuit()) {
        // We're definitely quitting cleanly now: drop the session lock so the
        // next launch knows this shutdown was clean and won't offer to recover
        // a stale autosave. This is the single chokepoint every clean quit
        // funnels through (window close button, File -> Quit, OS quit request,
        // and the deferred re-quit after an async Save). A crash or force-kill
        // never reaches here, so the lock survives and recovery is offered.
        content->markCleanShutdown();
        juce::JUCEApplication::getInstance()->quit();
    }
}

// ==============================================================================
// Output Capture -> Audio Track
// ==============================================================================

void MainContentComponent::bounceToAudioTrack() {
    // Calculate project length from the last clip end + 4 beats of tail.
    float maxBeat = 0;
    for (auto& n : graph.nodes)
        for (auto& c : n.clips)
            maxBeat = std::max(maxBeat, c.startBeat + c.lengthBeats);
    if (maxBeat <= 0) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
            "Nothing to bounce", "Add some clips to the timeline first.");
        return;
    }
    maxBeat += 4; // tail for reverb / release tails

    double sr = audioEngine.getSampleRate();
    if (sr <= 0) sr = 48000;
    int blk = 512;

    double totalSeconds = transport.tempoMap.beatsToSeconds(maxBeat);
    int64_t totalSamples = (int64_t)(totalSeconds * sr);
    if (totalSamples <= 0) return;

    // Offline render into a buffer, using a separate transport + graph processor.
    Transport offlineTransport;
    offlineTransport.bpm = graph.bpm;
    offlineTransport.tempoMap = transport.tempoMap;
    offlineTransport.timeSigMap = transport.timeSigMap;
    offlineTransport.sampleRate = sr;
    offlineTransport.playing = true;

    GraphProcessor offlineGP;
    offlineGP.prepare(graph, sr, blk);
    offlineGP.rebuildGraph(graph, offlineTransport);
    offlineGP.prepare(graph, sr, blk);

    juce::AudioBuffer<float> result(2, (int)totalSamples);
    result.clear();

    for (int64_t pos = 0; pos < totalSamples; pos += blk) {
        int thisBlock = (int)std::min((int64_t)blk, totalSamples - pos);
        offlineTransport.positionSamples = pos;

        juce::AudioBuffer<float> buf(2, thisBlock);
        buf.clear();
        juce::MidiBuffer midi;
        if (auto* g = offlineGP.getGraph())
            g->processBlock(buf, midi);

        for (int ch = 0; ch < 2; ++ch)
            result.copyFrom(ch, (int)pos, buf, ch, 0, thisBlock);
    }

    // Save to disk
    juce::File captureDir = juce::File::getCurrentWorkingDirectory().getChildFile("captures");
    captureDir.createDirectory();
    auto fileName = juce::String("bounce_") + juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S") + ".wav";
    juce::File outFile = captureDir.getChildFile(fileName);

    ExportOptions opts;
    opts.format = ExportFormat::WAV;
    opts.sampleRate = (int)sr;
    opts.bitsPerSample = 24;
    AudioExporter::exportToFile(outFile, result, opts);

    // Create Audio Timeline node with a clip covering the full bounce
    auto tl = graphComponent->screenToCanvas({50.0f, 50.0f});
    auto& n = graph.addNode("Bounced Audio", NodeType::AudioTimeline,
        {}, {Pin{0, "Audio", PinKind::Audio, false}}, {tl.x, tl.y});
    Clip clip;
    clip.name = fileName.toStdString();
    clip.startBeat = 0;
    clip.lengthBeats = maxBeat;
    clip.color = juce::Colours::orange.getARGB();
    clip.audioFilePath = outFile.getFullPathName().toStdString();
    n.clips.push_back(clip);

    audioEngine.getGraphProcessor().requestRebuild();
    graphComponent->repaint();

    juce::Logger::writeToLog("Bounced to: " + outFile.getFullPathName()
        + " (" + juce::String(totalSeconds, 1) + "s)");
}

void MainContentComponent::createAudioTrackFromOutputCache(Node& outputNode) {
    auto& c = outputNode.cache;
    if (!c.valid || c.numSamples <= 0 || c.left.empty()) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
            "No cached audio", "Play the song first, then click Capture.");
        return;
    }

    // Build a JUCE buffer from the cache
    int n = (int)c.numSamples;
    juce::AudioBuffer<float> buf(2, n);
    buf.copyFrom(0, 0, c.left.data(), n);
    if ((int)c.right.size() >= n)
        buf.copyFrom(1, 0, c.right.data(), n);
    else
        buf.clear(1, 0, n);

    // Save to disk
    juce::File captureDir = juce::File::getCurrentWorkingDirectory().getChildFile("captures");
    captureDir.createDirectory();
    auto fileName = juce::String("capture_") + juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S") + ".wav";
    juce::File outFile = captureDir.getChildFile(fileName);

    ExportOptions opts;
    opts.format = ExportFormat::WAV;
    opts.sampleRate = (int)c.sampleRate;
    opts.bitsPerSample = 24;
    AudioExporter::exportToFile(outFile, buf, opts);

    // Compute beat length from sample count
    double durationSec = (double)n / c.sampleRate;
    double durationBeats = durationSec * (transport.bpm / 60.0);

    // Create Audio Timeline node
    auto tl = graphComponent->screenToCanvas({50.0f, 50.0f});
    auto& newNode = graph.addNode("Captured Audio", NodeType::AudioTimeline,
        {}, {Pin{0, "Audio", PinKind::Audio, false}}, {tl.x, tl.y});
    Clip clip;
    clip.name = fileName.toStdString();
    clip.startBeat = 0;
    clip.lengthBeats = (float)durationBeats;
    clip.color = juce::Colours::orange.getARGB();
    clip.audioFilePath = outFile.getFullPathName().toStdString();
    newNode.clips.push_back(clip);

    audioEngine.getGraphProcessor().requestRebuild();
    graphComponent->repaint();

    juce::Logger::writeToLog("Saved from cache: " + outFile.getFullPathName()
        + " (" + juce::String(durationSec, 1) + "s)");
}

void MainContentComponent::saveCaptureToDisk(const juce::File& file) {
    auto buf = audioEngine.getCaptureBuffer();
    if (buf.getNumSamples() == 0) return;

    ExportOptions opts;
    opts.format = ExportFormat::WAV;
    opts.sampleRate = (int)audioEngine.captureSampleRate;
    opts.bitsPerSample = 24;
    AudioExporter::exportToFile(file, buf, opts);
}

void MainContentComponent::createAudioTrackFromCapture() {
    auto buf = audioEngine.getCaptureBuffer();
    if (buf.getNumSamples() == 0) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
            "Capture Empty", "No audio was captured. Make sure to play something while Capture is on.");
        return;
    }

    // Save to a temp .wav in the project's directory (or a captures/ subfolder)
    juce::File projectDir = juce::File::getCurrentWorkingDirectory();
    juce::File captureDir = projectDir.getChildFile("captures");
    captureDir.createDirectory();

    // Unique filename with timestamp
    auto now = juce::Time::getCurrentTime();
    auto fileName = juce::String("capture_") + now.formatted("%Y%m%d_%H%M%S") + ".wav";
    juce::File captureFile = captureDir.getChildFile(fileName);

    saveCaptureToDisk(captureFile);

    // Compute the beat position where the capture started
    double startBeat = 0;
    auto startSample = audioEngine.getCaptureStartSample();
    if (startSample > 0)
        startBeat = transport.samplesToBeats(startSample);

    double durationSec = (double)buf.getNumSamples() / audioEngine.captureSampleRate;
    double durationBeats = durationSec * (transport.bpm / 60.0);

    // Create an Audio Timeline node with a clip pointing to the captured file.
    // Place it in the visible area, offset from existing nodes.
    auto tl = graphComponent->screenToCanvas({50.0f, 50.0f});
    auto& n = graph.addNode("Captured Audio", NodeType::AudioTimeline,
        {}, {Pin{0, "Audio", PinKind::Audio, false}},
        {tl.x, tl.y});
    Clip clip;
    clip.name = fileName.toStdString();
    clip.startBeat = (float)startBeat;
    clip.lengthBeats = (float)durationBeats;
    clip.color = juce::Colours::orange.getARGB();
    clip.audioFilePath = captureFile.getFullPathName().toStdString();
    n.clips.push_back(clip);

    audioEngine.clearCapture();
    audioEngine.getGraphProcessor().requestRebuild();
    graphComponent->repaint();

    juce::Logger::writeToLog("Capture saved: " + captureFile.getFullPathName()
        + " (" + juce::String(durationSec, 1) + "s, "
        + juce::String(buf.getNumSamples()) + " samples)");
}

} // namespace SoundShop

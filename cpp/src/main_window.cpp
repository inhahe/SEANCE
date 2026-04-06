#include "main_window.h"
#include "terrain_synth.h"
#include "builtin_synth.h"
#include "audio_export.h"
#include "mod_import.h"
#include <juce_gui_basics/juce_gui_basics.h>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace SoundShop {

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

    // Transport buttons
    addAndMakeVisible(playBtn);
    addAndMakeVisible(stopBtn);
    addAndMakeVisible(recordBtn);
    addAndMakeVisible(addMidiBtn);
    addAndMakeVisible(addAudioBtn);
    addAndMakeVisible(fitAllBtn);
    addAndMakeVisible(metroBtn);

    playBtn.onClick = [this]() { onPlay(); };
    stopBtn.onClick = [this]() { onStop(); };
    recordBtn.onClick = [this]() { onRecord(); };
    auto findPlacement = [this]() -> Vec2 {
        // Find the next open slot, wrapping into columns to stay visible
        float colX = 50, rowY = 30;
        float nodeH = 80, nodeW = 200, padX = 30, padY = 20;
        float maxVisibleY = 600; // approximate default visible height

        // Collect positions of existing timeline nodes
        std::vector<Vec2> taken;
        for (auto& n : graph.nodes)
            if (n.type == NodeType::MidiTimeline || n.type == NodeType::AudioTimeline)
                taken.push_back(n.pos);

        // Try slots column by column
        for (int col = 0; col < 20; ++col) {
            float x = colX + col * (nodeW + padX);
            for (float y = rowY; y + nodeH < maxVisibleY + rowY; y += nodeH + padY) {
                bool occupied = false;
                for (auto& t : taken)
                    if (std::abs(t.x - x) < nodeW && std::abs(t.y - y) < nodeH) {
                        occupied = true;
                        break;
                    }
                if (!occupied) return {x, y};
            }
        }
        return {colX, rowY}; // fallback
    };

    addMidiBtn.onClick = [this, findPlacement]() {
        auto pos = findPlacement();
        auto& n = graph.addNode("MIDI Track", NodeType::MidiTimeline,
            {}, {Pin{0, "MIDI", PinKind::Midi, false}}, pos);
        n.clips.push_back({"Clip 1", 0, 4, juce::Colours::cornflowerblue.getARGB()});
        graphComponent->repaint();
    };
    addAudioBtn.onClick = [this, findPlacement]() {
        auto pos = findPlacement();
        auto& n = graph.addNode("Audio Track", NodeType::AudioTimeline,
            {}, {Pin{0, "Audio", PinKind::Audio, false}}, pos);
        n.clips.push_back({"Clip 1", 0, 4, juce::Colours::forestgreen.getARGB()});
        graphComponent->repaint();
    };
    fitAllBtn.onClick = [this]() { graphComponent->fitAll(); };
    // Position display
    positionLabel.setText("1 : 1.0", juce::dontSendNotification);
    positionLabel.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 14.0f, 0));
    positionLabel.setColour(juce::Label::textColourId, juce::Colours::limegreen);
    addAndMakeVisible(positionLabel);

    // Time signature
    timeSigLabel.setText("Time:", juce::dontSendNotification);
    addAndMakeVisible(timeSigLabel);
    addAndMakeVisible(timeSigCombo);
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
        loopBtn.setColour(juce::TextButton::buttonColourId,
            graph.loopEnabled ? juce::Colour(60, 60, 120) : juce::Colour(55, 55, 60));
    };

    addAndMakeVisible(monitorBtn);
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
    addAndMakeVisible(bpmSlider);
    bpmSlider.onValueChange = [this]() { graph.bpm = (float)bpmSlider.getValue(); };

    // Tap tempo
    addAndMakeVisible(tapTempoBtn);
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
    graphComponent->onRunScript = [this](int nodeId) { showScriptConsoleForNode(nodeId); };

    // Load prefs, plugin cache, recent projects (audio engine deferred to timer)
    pluginSettings.load("soundshop_plugins.cfg");
    loadRecentProjects();
    loadPreferences();

    // Load last project or set up default graph
    bool loaded = false;
    if (autoLoadLastProject && !recentProjects.empty()) {
        auto file = juce::File(recentProjects[0]);
        if (file.existsAsFile()) {
            ProjectFile::load(recentProjects[0].toStdString(), graph, nullptr);
            loaded = true;
        }
    }
    if (!loaded)
        graph.setupDefaultGraph();

    audioEngine.setGraph(&graph, &transport);

    // Timer for UI updates
    startTimerHz(30);

    setSize(1440, 900);
}

MainContentComponent::~MainContentComponent() {
    audioEngine.shutdown();
}

void MainContentComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(30, 30, 35));
}

bool MainContentComponent::keyPressed(const juce::KeyPress& key) {
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
    placeBtn(tapTempoBtn, 32);
    placeBtn(fitAllBtn, 50);
    placeBtn(metroBtn, 50);
    placeBtn(loopBtn, 42);
    placeBtn(monitorBtn, 36);
    x += 4;
    timeSigLabel.setBounds(transport.getX() + x, transport.getY() + 2, 35, 28);
    x += 38;
    timeSigCombo.setBounds(transport.getX() + x, transport.getY() + 4, 55, 24);
    x += 62;
    positionLabel.setBounds(transport.getX() + x, transport.getY() + 2, 80, 28);

    // Split: graph on top, editors on bottom
    if (!editorPanels.empty()) {
        auto editorArea = area.removeFromBottom(editorPanelHeight);
        int perEditor = editorArea.getHeight() / (int)editorPanels.size();
        for (auto& panel : editorPanels) {
            panel->component->setBounds(editorArea.removeFromTop(perEditor));
        }
    }
    graphComponent->setBounds(area);
}

void MainContentComponent::timerCallback() {
    transport.bpm = graph.bpm;
    transport.loopEnabled = graph.loopEnabled;
    transport.loopStartBeat = graph.loopStartBeat;
    transport.loopEndBeat = graph.loopEndBeat;
    graph.resolveAnchors();
    if (transport.tempoMap.points.size() == 1)
        transport.tempoMap.setGlobalBpm(graph.bpm);
    if (transport.timeSigMap.sigs.size() == 1) {
        transport.timeSigMap.sigs[0].numerator = graph.timeSignatureNum;
        transport.timeSigMap.sigs[0].denominator = graph.timeSignatureDen;
    }
    transport.playing = audioEngine.isPlaying();

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

    // Apply recorded automation during playback
    if (transport.playing) {
        float beat = (float)transport.positionBeats();
        std::vector<AutomationValue> autoValues;
        for (auto& node : graph.nodes) {
            for (int pi = 0; pi < (int)node.params.size(); ++pi) {
                auto& lane = node.params[pi].automation;
                if (!lane.points.empty()) {
                    float val = lane.evaluate(beat);
                    if (val >= -0.5f) { // valid (not sentinel)
                        node.params[pi].value = val;
                        // Also push to plugin
                        float normalized = (val - node.params[pi].minVal) /
                            std::max(0.001f, node.params[pi].maxVal - node.params[pi].minVal);
                        autoValues.push_back({node.id, pi, juce::jlimit(0.0f, 1.0f, normalized)});
                    }
                }
            }
        }
        if (!autoValues.empty())
            audioEngine.getGraphProcessor().applyAutomation(autoValues);
    }

    // Update position display
    {
        auto [bar, beat] = transport.timeSigMap.beatToBarBeat(transport.positionBeats());
        positionLabel.setText(juce::String(bar) + " : " + juce::String(beat, 1),
                              juce::dontSendNotification);
    }

    graphComponent->repaint();
    for (auto& panel : editorPanels)
        panel->component->repaint();

    // Deferred startup: init audio engine after window is visible
    if (startupFrames > 0) {
        startupFrames--;
        if (startupFrames == 0) {
            audioEngine.init();
            audioEngine.getPluginHost().loadScanCache("soundshop_plugins_cache.dat");

            // Reload plugins for any nodes that were loaded before the audio engine was ready
            {
                bool anyLoaded = false;
                for (auto& n : graph.nodes) {
                    if (n.pluginIndex >= 0 && !n.plugin) {
                        auto loaded = audioEngine.getPluginHost().loadPlugin(
                            n.pluginIndex, audioEngine.getSampleRate(), audioEngine.getBlockSize());
                        if (loaded) {
                            // Restore saved plugin state
                            if (!n.pendingPluginState.empty() && loaded->instance) {
                                juce::MemoryBlock stateData;
                                stateData.fromBase64Encoding(n.pendingPluginState);
                                if (stateData.getSize() > 0)
                                    loaded->instance->setStateInformation(
                                        stateData.getData(), (int)stateData.getSize());
                                n.pendingPluginState.clear();
                            }
                            n.plugin = std::move(loaded);
                            anyLoaded = true;
                        }
                    }
                }
                if (anyLoaded)
                    audioEngine.getGraphProcessor().requestRebuild();
            }

            // Restore CC mappings from loaded project
            syncCCMappingsFromGraph();

            // Restore project sample rate
            if (graph.projectSampleRate > 0)
                audioEngine.setProjectSampleRate(graph.projectSampleRate);

            // Restore editor panels from loaded project (deferred until UI is ready)
            if (!graph.openEditors.empty()) {
                auto editorsToOpen = graph.openEditors;
                graph.openEditors.clear();
                for (auto* node : editorsToOpen)
                    openEditor(*node);
            }
            graphComponent->fitAll();

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
        }
    }

    // Update window title with save flash and dirty indicator
    if (auto* win = dynamic_cast<juce::DocumentWindow*>(getTopLevelComponent())) {
        juce::String title = "SoundShop";
        if (!ProjectFile::currentPath.empty())
            title = juce::File(ProjectFile::currentPath).getFileNameWithoutExtension();
        if (saveFlashFrames > 0) {
            title += " — Saved";
            saveFlashFrames--;
        } else if (projectDirty || graph.dirty) {
            title += " *";
        }
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
        menu.addItem(3, "Save Project", true, false);
        menu.addItem(4, "Save Project As...");
        menu.addItem(5, "Export Audio...");
        menu.addItem(6, "Import MOD/S3M/IT/XM...");
        menu.addSeparator();
        juce::PopupMenu recentMenu;
        if (recentProjects.empty()) {
            recentMenu.addItem(-1, "(no recent projects)", false);
        } else {
            for (int i = 0; i < (int)recentProjects.size(); ++i) {
                auto file = juce::File(recentProjects[i]);
                recentMenu.addItem(60 + i, file.getFileName() + "  —  " + file.getParentDirectory().getFileName());
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
                    menu.addItem(900 + ri, f.getFileName() + "  —  " + f.getParentDirectory().getFileName());
                    ri++;
                }
            }
            if (ri > 0) menu.addSeparator();
        }
        menu.addItem(92, "Clear Recent Scripts");
    } else if (name == "View") {
        menu.addItem(30, "Fit All");
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
    } else if (name == "Plugins") {
        menu.addItem(40, "Plugin Settings...");
        menu.addSeparator();
        auto& plugins = audioEngine.getPluginHost().getAvailablePlugins();
        menu.addItem(-1, juce::String((int)plugins.size()) + " plugins loaded", false);
    } else if (name == "Help") {
        menu.addItem(50, "About SoundShop");
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
        clearPathBtn.onClick = [this]() {
            this->proc.getTraversalParams().pathPoints.clear();
            repaint();
        };

        drawModeCombo.addItem("Off", 1);
        drawModeCombo.addItem("Click Points", 2);
        drawModeCombo.addItem("Freehand", 3);
        drawModeCombo.setSelectedItemIndex(0);

        loopModeCombo.addItem("Loop", 1);
        loopModeCombo.addItem("Bounce", 2);
        loopModeCombo.setSelectedItemIndex(0);

        addAndMakeVisible(smoothToggle);
        smoothToggle.setButtonText("Smooth");
        smoothToggle.setToggleable(true);
        smoothToggle.setClickingTogglesState(true);
        smoothToggle.setToggleState(true, juce::dontSendNotification);

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
        auto modeStr = proc.getMode() == TerrainSynthMode::SamplePerPoint
            ? "Sample/Point" : "Waveform/Point";
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

private:
    TerrainSynthProcessor& proc;
    Node& node;
    juce::TextButton clearPathBtn, smoothToggle;
    juce::ComboBox loopModeCombo, drawModeCombo;
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
            opts.useNativeTitleBar = true;
            opts.resizable = true;
            opts.launchAsync();
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
    opts.useNativeTitleBar = true;
    opts.resizable = true;
    opts.launchAsync();
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
        for (int i = 0; i < (int)params.size(); ++i) {
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
                // CC captured — create mapping
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

    // ListBox delegate (inline — small enough)
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
            juce::String ccText = "—";
            for (auto& m : mappings)
                if (m.nodeId == parent->nodeId && m.paramIdx == row)
                    ccText = "CC " + juce::String(m.ccNumber) + " (ch " + juce::String(m.midiChannel) + ")";

            if (parent->learningParamIdx == row)
                ccText = "Waiting...";

            g.setColour(ccText == "—" ? juce::Colours::grey
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
    opts.useNativeTitleBar = true;
    opts.resizable = true;
    opts.launchAsync();
}

void MainContentComponent::onPlay() {
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

void MainContentComponent::onRecord() {
    // Find the active editor node, or first MIDI/audio timeline
    Node* recordNode = nullptr;

    // Prefer the active editor node
    if (graph.activeEditorNodeId >= 0)
        recordNode = graph.findNode(graph.activeEditorNodeId);

    // Fallback: first MIDI timeline, then audio timeline
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
            {}, {Pin{0, "MIDI", PinKind::Midi, false}}, {50, 50});
        n.clips.push_back({"Clip 1", 0, 8, juce::Colours::cornflowerblue.getARGB()});
        recordNode = &n;
        graphComponent->repaint();
    }

    if (recordNode->type == NodeType::MidiTimeline) {
        // MIDI note recording
        audioEngine.startMidiRecording(recordNode->id);
        // Also arm automation
        audioEngine.armAutomationRecording();
    } else if (recordNode->type == NodeType::AudioTimeline) {
        // Audio recording
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
    graph.nodes.clear();
    graph.links.clear();
    graph.openEditors.clear();
    graph.setupDefaultGraph();
    ProjectFile::currentPath.clear();
    projectDirty = false;
    graph.dirty = false;
    graphComponent->repaint();
}

void MainContentComponent::openProject() {
    auto chooser = std::make_shared<juce::FileChooser>("Open Project", juce::File(), "*.ssp");
    chooser->launchAsync(juce::FileBrowserComponent::openMode, [this, chooser](const juce::FileChooser& fc) {
        auto file = fc.getResult();
        if (file.existsAsFile())
            openProjectFile(file.getFullPathName());
    });
}

void MainContentComponent::openProjectFile(const juce::String& path) {
    editorPanels.clear();
    ProjectFile::load(path.toStdString(), graph, &audioEngine.getPluginHost());

    auto editorsToOpen = graph.openEditors;
    graph.openEditors.clear();
    for (auto* node : editorsToOpen)
        openEditor(*node);

    addToRecentProjects(path);
    graphComponent->fitAll();
    graphComponent->repaint();
}

void MainContentComponent::freezeNode(int nodeId) {
    auto* node = graph.findNode(nodeId);
    if (!node) return;

    // Calculate project length in beats
    float maxBeat = 0;
    for (auto& n : graph.nodes)
        for (auto& c : n.clips)
            maxBeat = std::max(maxBeat, c.startBeat + c.lengthBeats);
    if (maxBeat <= 0) maxBeat = 4;
    maxBeat += 4; // tail

    double sr = audioEngine.getSampleRate();
    if (sr <= 0) sr = 48000;
    int blockSize = 512;

    double totalSeconds = transport.tempoMap.beatsToSeconds(maxBeat);
    int64_t totalSamples = (int64_t)(totalSeconds * sr);

    // Offline render of the entire graph, capturing this node's output
    Transport offlineTransport;
    offlineTransport.bpm = graph.bpm;
    offlineTransport.tempoMap = transport.tempoMap;
    offlineTransport.sampleRate = sr;
    offlineTransport.playing = true;

    // Temporarily disable this node's cache to get fresh render
    node->cache.valid = false;

    GraphProcessor offlineGP;
    offlineGP.prepare(graph, sr, blockSize);
    offlineGP.rebuildGraph(graph, offlineTransport);
    offlineGP.prepare(graph, sr, blockSize);

    // Allocate cache
    node->cache.left.resize(totalSamples, 0.0f);
    node->cache.right.resize(totalSamples, 0.0f);
    node->cache.sampleRate = sr;
    node->cache.startSample = 0;
    node->cache.numSamples = totalSamples;

    // Render full project, capturing from the graph
    // We render the whole graph and then read the node's contribution
    // For simplicity, render the full mix — the cache represents this node's output
    for (int64_t pos = 0; pos < totalSamples; pos += blockSize) {
        int thisBlock = (int)std::min((int64_t)blockSize, totalSamples - pos);
        offlineTransport.positionSamples = pos;

        // Process through the JUCE graph
        juce::AudioBuffer<float> buf(2, thisBlock);
        buf.clear();
        juce::MidiBuffer midi;

        auto* juceGraph = offlineGP.getGraph();
        if (juceGraph)
            juceGraph->processBlock(buf, midi);

        // Get this node's processor output
        auto* proc = offlineGP.getProcessorForNode(nodeId);
        if (proc) {
            // The processor already ran as part of the graph.
            // For the cache we store the full mix reaching this node.
            // This is a simplification — ideally we'd tap the node's output only.
        }

        // Store the mix (this captures everything up to and including this node)
        for (int s = 0; s < thisBlock; ++s) {
            node->cache.left[pos + s] = buf.getSample(0, s);
            node->cache.right[pos + s] = buf.getSample(1, s);
        }
    }

    node->cache.valid = true;
    node->cache.enabled = true;
    graph.dirty = true;

    fprintf(stderr, "Froze node '%s': %lld samples (%.1f sec)\n",
            node->name.c_str(), (long long)totalSamples, totalSeconds);
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

void MainContentComponent::saveProject() {
    if (ProjectFile::currentPath.empty())
        saveProjectAs();
    else {
        syncCCMappingsToGraph();

        // Set cache dir and save node caches to disk
        auto& cm = audioEngine.getGraphProcessor().getCacheManager();
        auto projFile = juce::File(ProjectFile::currentPath);
        cm.setCacheDir(projFile.getParentDirectory()
            .getChildFile("soundshop_cache").getFullPathName().toStdString());
        for (auto& n : graph.nodes)
            if (n.cache.valid && n.cache.numSamples > 0 && !n.cache.left.empty())
                cm.saveToDisk(n, audioEngine.getSampleRate());
        cm.cleanupStaleFiles(graph);

        ProjectFile::save(ProjectFile::currentPath, graph, &audioEngine.getGraphProcessor());
        addToRecentProjects(ProjectFile::currentPath);
        projectDirty = false;
        graph.dirty = false;
        saveFlashFrames = 60; // ~2 seconds at 30Hz
    }
}

void MainContentComponent::saveProjectAs() {
    syncCCMappingsToGraph();
    auto chooser = std::make_shared<juce::FileChooser>("Save Project", juce::File(), "*.ssp");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode, [this, chooser](const juce::FileChooser& fc) {
        auto file = fc.getResult();
        if (file != juce::File()) {
            ProjectFile::save(file.getFullPathName().toStdString(), graph, &audioEngine.getGraphProcessor());
            addToRecentProjects(file.getFullPathName());
            projectDirty = false;
            graph.dirty = false;
            saveFlashFrames = 60;
        }
    });
}

void MainContentComponent::importModFile() {
    auto chooser = std::make_shared<juce::FileChooser>(
        "Import Tracker Module", juce::File(), "*.mod;*.s3m;*.it;*.xm");
    chooser->launchAsync(juce::FileBrowserComponent::openMode,
        [this, chooser](const juce::FileChooser& fc) {
            auto file = fc.getResult();
            if (!file.existsAsFile()) return;

            auto result = ModImporter::import(file.getFullPathName().toStdString(), graph);

            juce::String msg;
            if (result.success) {
                msg = "Imported successfully!\n\n"
                    + juce::String(result.numChannels) + " channels\n"
                    + juce::String(result.numPatterns) + " patterns\n"
                    + juce::String(result.numSamples) + " samples\n"
                    + juce::String(result.numNotes) + " notes";
                graphComponent->fitAll();
            } else {
                msg = "Import failed: " + juce::String(result.error);
            }

            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::InfoIcon,
                "MOD Import", msg);
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
    auto chooser = std::make_shared<juce::FileChooser>(
        "Export Audio", juce::File(), AudioExporter::getFileFilter());
    chooser->launchAsync(juce::FileBrowserComponent::saveMode,
        [this, chooser, maxBeat](const juce::FileChooser& fc) {
            auto file = fc.getResult();
            if (file == juce::File()) return;

            auto path = file.getFullPathName();
            auto fmt = AudioExporter::formatFromExtension(path);
            auto ext = AudioExporter::getExtension(fmt);
            if (!path.endsWithIgnoreCase(ext))
                file = juce::File(path + ext);

            // For lossy formats, show options dialog
            bool isLossy = (fmt == ExportFormat::OggVorbis || fmt == ExportFormat::Opus ||
                            fmt == ExportFormat::M4A_AAC || fmt == ExportFormat::WMA);

            if (isLossy) {
                auto* aw = new juce::AlertWindow("Export Options",
                    "Format: " + ext.substring(1).toUpperCase(),
                    juce::MessageBoxIconType::NoIcon);

                aw->addComboBox("samplerate", {"44100 Hz", "48000 Hz", "88200 Hz", "96000 Hz", "192000 Hz"});
                // Default to project rate
                int srDefault = 1; // 48000
                double projSr = audioEngine.getProjectSampleRate();
                if (projSr <= 44100) srDefault = 0;
                else if (projSr <= 48000) srDefault = 1;
                else if (projSr <= 88200) srDefault = 2;
                else if (projSr <= 96000) srDefault = 3;
                else srDefault = 4;
                aw->getComboBoxComponent("samplerate")->setSelectedItemIndex(srDefault);

                if (fmt == ExportFormat::OggVorbis) {
                    aw->addComboBox("quality", {"Low (q3)", "Medium (q5)", "High (q7)", "Very High (q8)", "Maximum (q10)"});
                    aw->getComboBoxComponent("quality")->setSelectedItemIndex(2);
                    aw->addTextBlock("VBR (variable bitrate) — higher quality = larger file");
                } else {
                    aw->addComboBox("bitrate", {"64 kbps", "96 kbps", "128 kbps", "160 kbps",
                                                 "192 kbps", "256 kbps", "320 kbps"});
                    aw->getComboBoxComponent("bitrate")->setSelectedItemIndex(4);
                    if (fmt == ExportFormat::Opus)
                        aw->addTextBlock("Opus uses VBR by default — bitrate is a target average");
                    else
                        aw->addTextBlock("CBR (constant bitrate)");
                }

                aw->addButton("Export", 1, juce::KeyPress(juce::KeyPress::returnKey));
                aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

                auto exportFile = file;
                auto exportFmt = fmt;
                auto exportMaxBeat = maxBeat;
                aw->enterModalState(true, juce::ModalCallbackFunction::create(
                    [this, aw, exportFile, exportFmt, exportMaxBeat](int result) {
                        if (result == 1) {
                            ExportOptions opts;
                            opts.format = exportFmt;
                            int srOptions[] = {44100, 48000, 88200, 96000, 192000};
                            int srIdx = aw->getComboBoxComponent("samplerate")->getSelectedItemIndex();
                            opts.sampleRate = srOptions[juce::jlimit(0, 4, srIdx)];

                            if (exportFmt == ExportFormat::OggVorbis) {
                                float qualities[] = {0.3f, 0.5f, 0.7f, 0.8f, 1.0f};
                                int idx = aw->getComboBoxComponent("quality")->getSelectedItemIndex();
                                opts.quality = qualities[juce::jlimit(0, 4, idx)];
                            } else {
                                int bitrates[] = {64, 96, 128, 160, 192, 256, 320};
                                int idx = aw->getComboBoxComponent("bitrate")->getSelectedItemIndex();
                                opts.bitrate = bitrates[juce::jlimit(0, 6, idx)];
                            }

                            doExportRender(exportFile, opts, exportMaxBeat);
                        }
                        delete aw;
                    }), true);
            } else {
                // Lossless — show bit depth option
                auto* aw = new juce::AlertWindow("Export Options",
                    "Format: " + ext.substring(1).toUpperCase(),
                    juce::MessageBoxIconType::NoIcon);
                aw->addComboBox("samplerate", {"44100 Hz", "48000 Hz", "88200 Hz", "96000 Hz", "192000 Hz"});
                {
                    int srDef = 1;
                    double psr = audioEngine.getProjectSampleRate();
                    if (psr <= 44100) srDef = 0;
                    else if (psr <= 48000) srDef = 1;
                    else if (psr <= 88200) srDef = 2;
                    else if (psr <= 96000) srDef = 3;
                    else srDef = 4;
                    aw->getComboBoxComponent("samplerate")->setSelectedItemIndex(srDef);
                }
                aw->addComboBox("bits", {"16-bit (standard, smaller)", "24-bit (higher quality)", "32-bit float (maximum)"});
                aw->getComboBoxComponent("bits")->setSelectedItemIndex(0);
                aw->addButton("Export", 1, juce::KeyPress(juce::KeyPress::returnKey));
                aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

                auto exportFile = file;
                auto exportFmt = fmt;
                auto exportMaxBeat = maxBeat;
                aw->enterModalState(true, juce::ModalCallbackFunction::create(
                    [this, aw, exportFile, exportFmt, exportMaxBeat](int result) {
                        if (result == 1) {
                            ExportOptions opts;
                            opts.format = exportFmt;
                            int srOpts[] = {44100, 48000, 88200, 96000, 192000};
                            int srIdx = aw->getComboBoxComponent("samplerate")->getSelectedItemIndex();
                            opts.sampleRate = srOpts[juce::jlimit(0, 4, srIdx)];
                            int bitsOptions[] = {16, 24, 32};
                            int idx = aw->getComboBoxComponent("bits")->getSelectedItemIndex();
                            opts.bitsPerSample = bitsOptions[juce::jlimit(0, 2, idx)];
                            doExportRender(exportFile, opts, exportMaxBeat);
                        }
                        delete aw;
                    }), true);
            }
        });
}

void MainContentComponent::doExportRender(const juce::File& file, const ExportOptions& opts, float maxBeat) {
    double sr = opts.sampleRate;
    int blockSize = 512;
    int numChannels = 2;

    double totalSeconds = transport.tempoMap.beatsToSeconds(maxBeat);
    int64_t totalSamples = (int64_t)(totalSeconds * sr);

    Transport offlineTransport;
    offlineTransport.bpm = graph.bpm;
    offlineTransport.tempoMap = transport.tempoMap;
    offlineTransport.sampleRate = sr;
    offlineTransport.playing = true;

    GraphProcessor offlineGP;
    offlineGP.prepare(graph, sr, blockSize);
    offlineGP.rebuildGraph(graph, offlineTransport);
    offlineGP.prepare(graph, sr, blockSize);

    juce::AudioBuffer<float> renderBuf(numChannels, (int)totalSamples);
    renderBuf.clear();

    for (int64_t pos = 0; pos < totalSamples; pos += blockSize) {
        int thisBlock = (int)std::min((int64_t)blockSize, totalSamples - pos);
        offlineTransport.positionSamples = pos;
        float* outPtrs[2] = {
            renderBuf.getWritePointer(0, (int)pos),
            renderBuf.getWritePointer(1, (int)pos)
        };
        offlineGP.processBlock(graph, offlineTransport, outPtrs, numChannels, thisBlock);
    }

    if (AudioExporter::exportToFile(file, renderBuf, opts)) {
        saveFlashFrames = 90;
        if (auto* win = dynamic_cast<juce::DocumentWindow*>(getTopLevelComponent()))
            win->setName("Exported!");
    }
}

void MainContentComponent::openEditor(Node& node) {
    // Check if already open
    for (auto& panel : editorPanels)
        if (panel->nodeId == node.id) return;

    auto panel = std::make_unique<EditorPanel>();
    panel->nodeId = node.id;
    panel->component = std::make_unique<PianoRollComponent>(graph, node, &transport);
    panel->component->onClose = [this](int nodeId) { closeEditor(nodeId); };
    addAndMakeVisible(panel->component.get());
    editorPanels.push_back(std::move(panel));

    graph.activeEditorNodeId = node.id;
    editorPanelHeight = std::max(editorPanelHeight, (int)editorPanels.size() * 200);
    resized();
}

void MainContentComponent::closeEditor(int nodeId) {
    editorPanels.erase(
        std::remove_if(editorPanels.begin(), editorPanels.end(),
            [nodeId](auto& p) { return p->nodeId == nodeId; }),
        editorPanels.end());
    resized();
}

bool MainContentComponent::tryQuit() {
    if (projectDirty || graph.dirty) {
        int result = juce::AlertWindow::showYesNoCancelBox(
            juce::MessageBoxIconType::QuestionIcon,
            "Unsaved Changes",
            "You have unsaved changes. Save before quitting?",
            "Save", "Don't Save", "Cancel");
        if (result == 1) { saveProject(); return true; }
        if (result == 2) return true;
        return false; // cancel
    }
    return true;
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
    auto file = getPreferencesFile();
    if (!file.existsAsFile()) return;
    auto xml = juce::parseXML(file);
    if (!xml || xml->getTagName() != "Preferences") return;
    autoLoadLastProject = xml->getBoolAttribute("autoLoadLastProject", true);
}

void MainContentComponent::savePreferences() {
    auto xml = std::make_unique<juce::XmlElement>("Preferences");
    xml->setAttribute("autoLoadLastProject", autoLoadLastProject);
    xml->writeTo(getPreferencesFile());
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
        fileChooser->launchAsync(juce::FileBrowserComponent::saveMode,
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
                menu.addItem(i + 1, file.getFileName() + "  —  " + file.getParentDirectory().getFileName());
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
    opts.useNativeTitleBar = true;
    opts.resizable = true;
    opts.launchAsync();
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
    opts.useNativeTitleBar = true;
    opts.resizable = true;
    opts.launchAsync();
}

void MainContentComponent::showPluginSettingsDialog() {
    auto* dlg = new PluginSettingsComponent(pluginSettings, audioEngine.getPluginHost(), graph);

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(dlg);
    opts.dialogTitle = "Plugin Settings";
    opts.dialogBackgroundColour = juce::Colour(40, 40, 45);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = true;
    opts.resizable = true;
    opts.launchAsync();
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

void MainWindow::tryQuit() {
    auto* content = dynamic_cast<MainContentComponent*>(getContentComponent());
    if (content && content->tryQuit())
        juce::JUCEApplication::getInstance()->quit();
}

} // namespace SoundShop

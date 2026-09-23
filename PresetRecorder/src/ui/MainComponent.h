#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Controller.h"
#include "Player.h"

namespace PresetRecorder {

class RecordTab;
class BrowseTab;
class SkipListTab;

// The window's content: three tabs (Record, Browse & Listen, Skip List) over
// one Controller and one Player.
class MainComponent : public juce::Component
{
public:
    MainComponent();
    ~MainComponent() override;

    void resized() override;
    void paint(juce::Graphics&) override;

    Controller& getController() { return controller; }

    // Asks before quitting in the middle of a run; calls `quit` if it's OK to.
    void requestQuit(std::function<void()> quit);

    void saveState();

private:
    Controller controller;
    Player player;

    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    std::unique_ptr<RecordTab> recordTab;
    std::unique_ptr<BrowseTab> browseTab;
    std::unique_ptr<SkipListTab> skipTab;
    juce::TooltipWindow tooltips { this, 700 };
};

// Top-level window.
class MainWindow : public juce::DocumentWindow
{
public:
    explicit MainWindow(const juce::String& title);
    ~MainWindow() override;

    void closeButtonPressed() override;
    void tryQuit();

private:
    MainComponent* content = nullptr;
};

} // namespace PresetRecorder

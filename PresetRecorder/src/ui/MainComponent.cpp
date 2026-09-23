#include "MainComponent.h"
#include "RecordTab.h"
#include "BrowseTab.h"
#include "SkipListTab.h"
#include "Theme.h"
#include "cpp/src/dialog_helpers.h"

namespace PresetRecorder {

MainComponent::MainComponent()
{
    if (auto state = juce::parseXML(controller.getSettings().props().getValue("audioDevice")))
        player.restoreDeviceState(state.get());

    recordTab = std::make_unique<RecordTab>(controller, player);
    browseTab = std::make_unique<BrowseTab>(controller, player);
    skipTab = std::make_unique<SkipListTab>(controller);

    controller.onLogLine = [this](const juce::String& line) { recordTab->appendLog(line); };

    recordTab->onShowRecordings = [this](const juce::String& pluginId)
    {
        browseTab->showPlugin(pluginId);
        tabs.setCurrentTabIndex(1);
    };

    browseTab->onShowAudioSettings = [this]
    {
        SoundShop::launchAudioDeviceSettings(player.getDeviceManager(), this);
    };

    const auto tabColour = Theme::background;
    tabs.addTab("Record", tabColour, recordTab.get(), false);
    tabs.addTab("Browse & Listen", tabColour, browseTab.get(), false);
    tabs.addTab("Skip List", tabColour, skipTab.get(), false);
    tabs.setTabBarDepth(32);
    tabs.setOutline(0);
    tabs.setCurrentTabIndex(juce::jlimit(0, 2, controller.getSettings().props().getIntValue("tab", 0)));
    addAndMakeVisible(tabs);

    setSize(1200, 860);
}

MainComponent::~MainComponent()
{
    saveState();
    controller.onLogLine = nullptr;
    tabs.clearTabs();
    recordTab.reset();
    browseTab.reset();
    skipTab.reset();
}

void MainComponent::saveState()
{
    auto& props = controller.getSettings().props();
    props.setValue("tab", tabs.getCurrentTabIndex());
    if (auto state = player.getDeviceState())
        props.setValue("audioDevice", state->toString());
    controller.getSettings().saveNow();
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(Theme::background);
}

void MainComponent::resized()
{
    tabs.setBounds(getLocalBounds());
}

void MainComponent::requestQuit(std::function<void()> quit)
{
    if (! controller.isBusy())
    {
        quit();
        return;
    }

    SoundShop::showAlertAsync(juce::MessageBoxOptions()
                                  .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                  .withTitle("Recording in progress")
                                  .withMessage("Plugins are still being scanned or recorded. Stop and quit?\n\n"
                                               "Everything recorded so far is kept; with \"Keep existing "
                                               "recordings\" ticked, the next run carries on where this one stopped.")
                                  .withButton("Stop and quit")
                                  .withButton("Keep running"),
                              this,
                              [this, quit](int result)
                              {
                                  if (result == 1)
                                  {
                                      controller.stop();
                                      quit();
                                  }
                              });
}

//==============================================================================
MainWindow::MainWindow(const juce::String& title)
    : DocumentWindow(title, Theme::background, DocumentWindow::allButtons)
{
    setUsingNativeTitleBar(true);
    content = new MainComponent();
    setContentOwned(content, true);
    setResizable(true, true);
    setResizeLimits(980, 700, 10000, 10000);

    const auto saved = content->getController().getSettings().props().getValue("windowState");
    if (saved.isEmpty() || ! restoreWindowStateFromString(saved))
        centreWithSize(getWidth(), getHeight());

    setVisible(true);
}

MainWindow::~MainWindow()
{
    if (content != nullptr)
    {
        content->getController().getSettings().props().setValue("windowState", getWindowStateAsString());
        content->saveState();
    }
    clearContentComponent();
}

void MainWindow::closeButtonPressed()
{
    tryQuit();
}

void MainWindow::tryQuit()
{
    content->requestQuit([] { juce::JUCEApplication::getInstance()->systemRequestedQuit(); });
}

} // namespace PresetRecorder

#include "main_window.h"
#include <juce_gui_basics/juce_gui_basics.h>

class SoundShopApplication : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "SoundShop"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String&) override {
        mainWindow = std::make_unique<SoundShop::MainWindow>(getApplicationName());
    }

    void shutdown() override {
        mainWindow.reset();
    }

    void systemRequestedQuit() override {
        if (mainWindow)
            mainWindow->tryQuit();
        else
            quit();
    }

private:
    std::unique_ptr<SoundShop::MainWindow> mainWindow;
};

START_JUCE_APPLICATION(SoundShopApplication)

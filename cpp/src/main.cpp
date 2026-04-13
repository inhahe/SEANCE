#include "main_window.h"
#include "plugin_sandbox.h"
#include <juce_gui_basics/juce_gui_basics.h>

class SoundShopApplication : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "SEANCE"; }
    const juce::String getApplicationVersion() override { return "0.2.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLine) override {
        // Check for plugin sandbox child mode (#85).
        // If launched with --plugin-sandbox <pipe-name>, run as a
        // sandboxed plugin host child process instead of the main UI.
        auto args = juce::StringArray::fromTokens(commandLine, " ", "\"");
        int sandboxIdx = args.indexOf("--plugin-sandbox");
        if (sandboxIdx >= 0 && sandboxIdx + 1 < args.size()) {
            auto pipeName = args[sandboxIdx + 1].toStdString();
            int exitCode = SoundShop::runPluginSandboxChild(pipeName);
            setApplicationReturnValue(exitCode);
            quit();
            return;
        }

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

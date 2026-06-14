#include "main_window.h"
#include "plugin_sandbox.h"
#include "self_test.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdio>
#include <ctime>
#ifdef _WIN32
#include <Windows.h>
#endif

// Routes juce::Logger::writeToLog() through stderr, which is redirected
// to the log file at startup.  This keeps every diagnostic message -
// whether it comes from fprintf(stderr,...) or the JUCE logger - in one
// place.
class StderrLogger : public juce::Logger {
    void logMessage(const juce::String& message) override {
        fprintf(stderr, "%s\n", message.toRawUTF8());
        fflush(stderr);
    }
};

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

        // Headless terrain-synth self-test (#--self-test <dir>). Renders the
        // 1D/2D/3D terrain synths against known synthetic media and writes a
        // PASS/FAIL report plus audio to <dir>. No GUI is created.
        int selfTestIdx = args.indexOf("--self-test");
        if (selfTestIdx >= 0) {
            juce::File dir = (selfTestIdx + 1 < args.size())
                ? juce::File(args[selfTestIdx + 1])
                : juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                      .getParentDirectory().getChildFile("selftest_out");
            int rc = SoundShop::runSelfTest(dir);
            setApplicationReturnValue(rc);
            quit();
            return;
        }

        initLogging();

        mainWindow = std::make_unique<SoundShop::MainWindow>(getApplicationName());
    }

    void shutdown() override {
        mainWindow.reset();
        juce::Logger::setCurrentLogger(nullptr);
        logger.reset();
    }

    void systemRequestedQuit() override {
        if (mainWindow)
            mainWindow->tryQuit();
        else
            quit();
    }

private:
    std::unique_ptr<SoundShop::MainWindow> mainWindow;
    std::unique_ptr<StderrLogger> logger;

    void initLogging() {
#ifdef _WIN32
        FreeConsole();
#endif
        // Put seance.log next to the executable so it's easy to find while
        // debugging. Fall back to %APPDATA%/SEANCE/seance.log (Windows) or
        // ~/.config/SEANCE/seance.log (Linux/macOS) only if the exe dir
        // isn't writable (e.g. SEANCE installed under Program Files).
        auto exeDir = juce::File::getSpecialLocation(
            juce::File::currentExecutableFile).getParentDirectory();
        auto logFile = exeDir.getChildFile("seance.log");
        if (!exeDir.hasWriteAccess()) {
            auto logDir = juce::File::getSpecialLocation(
                juce::File::userApplicationDataDirectory).getChildFile("SEANCE");
            logDir.createDirectory();
            logFile = logDir.getChildFile("seance.log");
        }

        // Redirect stderr to the log file (truncates on each launch).
        // Every existing fprintf(stderr, ...) call in the codebase now
        // writes to this file instead of the console.
        freopen(logFile.getFullPathName().toRawUTF8(), "w", stderr);

        // Also route juce::Logger through stderr -> same file.
        logger = std::make_unique<StderrLogger>();
        juce::Logger::setCurrentLogger(logger.get());

        // Banner so the log is self-describing.
        std::time_t now = std::time(nullptr);
        char timeBuf[64];
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        fprintf(stderr, "SEANCE %s - started %s\n", getApplicationVersion().toRawUTF8(), timeBuf);
        fprintf(stderr, "Log: %s\n\n", logFile.getFullPathName().toRawUTF8());
        fflush(stderr);
    }
};

START_JUCE_APPLICATION(SoundShopApplication)

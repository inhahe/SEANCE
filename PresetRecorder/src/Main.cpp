#include <juce_gui_basics/juce_gui_basics.h>
#include "Worker.h"
#include "ui/MainComponent.h"
#include "cpp/src/dialog_helpers.h"

// PresetRecorder: records a FLAC preview of every preset of every plugin in
// SoundShop2's plugin folders. See README.md for use and design.md for how it
// works.
//
// The same exe runs in two modes:
//   PresetRecorder.exe                         the GUI
//   PresetRecorder.exe --worker <job.json>     a worker the GUI starts to scan
//                                              or record one plugin (Worker.h)
class PresetRecorderApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "PresetRecorder"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }

    // Must stay true: every worker is another instance of this exe. With
    // single-instance mode JUCE would hand a worker's command line to the
    // running GUI and exit the worker.
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLine) override
    {
        juce::File jobFile;
        if (PresetRecorder::isWorkerCommandLine(commandLine, jobFile))
        {
            isWorker = true;
            PresetRecorder::startWorker(jobFile); // exits the process itself when done
            return;
        }

        // Same look-and-feel as SEANCE; also strips the taskbar flag from
        // every AlertWindow (see dialog_helpers.h).
        SoundShop::installAppLookAndFeel();

        mainWindow = std::make_unique<PresetRecorder::MainWindow>(
            "Plugin Preset Recorder " + getApplicationVersion() + "  -  for SoundShop2");
    }

    void shutdown() override
    {
        mainWindow.reset();
        if (! isWorker)
            juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }

    void systemRequestedQuit() override
    {
        // The window asks first if a run is in progress (MainWindow::tryQuit),
        // then comes back here; by then it's decided.
        quit();
    }

    void anotherInstanceStarted(const juce::String&) override {}

private:
    std::unique_ptr<PresetRecorder::MainWindow> mainWindow;
    bool isWorker = false;
};

START_JUCE_APPLICATION(PresetRecorderApplication)

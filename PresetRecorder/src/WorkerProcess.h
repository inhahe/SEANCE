#pragma once
#include <juce_core/juce_core.h>
#include <optional>

namespace PresetRecorder {

// A child process running this same exe in worker mode.
//
// Why not juce::ChildProcess: on Windows it always creates an inheritable pipe
// and launches with bInheritHandles=TRUE, so every worker would inherit every
// other worker's pipe handles; it can't lower the child's priority; and it has
// no way to make children die with the parent. Here, on Windows, each worker:
//   - inherits no handles,
//   - runs at below-normal priority, so auditioning in the GUI stays glitch-free
//     while plugins render flat out in the background,
//   - is placed in a job object with KILL_ON_JOB_CLOSE, so if the GUI goes away
//     (even by crashing) no orphaned plugin process is left running, and
//     DIE_ON_UNHANDLED_EXCEPTION, so a crashing plugin can't park the worker
//     behind a Windows Error Reporting dialog.
// Elsewhere it falls back to juce::ChildProcess.
class WorkerProcess
{
public:
    WorkerProcess() = default;
    ~WorkerProcess();

    bool start(const juce::File& exe, const juce::StringArray& args, juce::String& error);

    bool isRunning() const;

    // The exit code once the process has ended; nullopt while it is running
    // (or if it never started).
    std::optional<juce::uint32> getExitCode() const;

    // Terminates the process (no-op if it isn't running).
    void kill();

private:
   #if JUCE_WINDOWS
    void* processHandle = nullptr;
   #else
    std::unique_ptr<juce::ChildProcess> child;
   #endif

    JUCE_DECLARE_NON_COPYABLE(WorkerProcess)
};

} // namespace PresetRecorder

#include "WorkerProcess.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace PresetRecorder {

#if JUCE_WINDOWS

// One job object for the life of the GUI process. Closing its last handle -
// which the OS does when the GUI exits for any reason - kills every worker.
static HANDLE getWorkerJob()
{
    static HANDLE job = []() -> HANDLE
    {
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        if (h == nullptr)
            return nullptr;

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info {};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                                              | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof(info));
        return h;
    }();

    return job;
}

static juce::String quoteArgument(const juce::String& arg)
{
    // CommandLineToArgvW rules: backslashes are literal unless they precede a
    // quote. Our arguments are file paths and flags, which never end in a
    // backslash followed by a quote once quoted, but escape embedded quotes.
    if (arg.isNotEmpty() && ! arg.containsAnyOf(" \t\""))
        return arg;

    return "\"" + arg.replace("\"", "\\\"") + "\"";
}

WorkerProcess::~WorkerProcess()
{
    kill();

    if (processHandle != nullptr)
        CloseHandle((HANDLE) processHandle);
}

bool WorkerProcess::start(const juce::File& exe, const juce::StringArray& args, juce::String& error)
{
    jassert(processHandle == nullptr);

    juce::String commandLine = quoteArgument(exe.getFullPathName());
    for (auto& a : args)
        commandLine << " " << quoteArgument(a);

    STARTUPINFOW si {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi {};

    // CreateProcessW may modify the command-line buffer, so hand it a copy.
    std::wstring buffer(commandLine.toWideCharPointer());

    const DWORD flags = CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS;

    if (! CreateProcessW(exe.getFullPathName().toWideCharPointer(), buffer.data(), nullptr, nullptr,
                         FALSE, flags, nullptr, nullptr, &si, &pi))
    {
        error = "could not start worker process (Windows error " + juce::String((int) GetLastError()) + ")";
        return false;
    }

    if (auto* job = getWorkerJob())
        AssignProcessToJobObject(job, pi.hProcess); // best effort; fails only under unusual nesting

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    processHandle = pi.hProcess;
    return true;
}

bool WorkerProcess::isRunning() const
{
    return processHandle != nullptr && WaitForSingleObject((HANDLE) processHandle, 0) == WAIT_TIMEOUT;
}

std::optional<juce::uint32> WorkerProcess::getExitCode() const
{
    if (processHandle == nullptr || isRunning())
        return std::nullopt;

    DWORD code = 0;
    if (! GetExitCodeProcess((HANDLE) processHandle, &code))
        return std::nullopt;

    return (juce::uint32) code;
}

void WorkerProcess::kill()
{
    if (isRunning())
    {
        // Asynchronous; a short wait covers the usual case without stalling the
        // UI thread on a process stuck in a driver. Callers re-check isRunning().
        TerminateProcess((HANDLE) processHandle, 0x40010004u /* DBG_TERMINATE_PROCESS: "killed" */);
        WaitForSingleObject((HANDLE) processHandle, 500);
    }
}

#else // ! JUCE_WINDOWS

WorkerProcess::~WorkerProcess()
{
    kill();
}

bool WorkerProcess::start(const juce::File& exe, const juce::StringArray& args, juce::String& error)
{
    juce::StringArray all;
    all.add(exe.getFullPathName());
    all.addArray(args);

    child = std::make_unique<juce::ChildProcess>();
    if (! child->start(all, 0))
    {
        child.reset();
        error = "could not start worker process";
        return false;
    }
    return true;
}

bool WorkerProcess::isRunning() const
{
    return child != nullptr && child->isRunning();
}

std::optional<juce::uint32> WorkerProcess::getExitCode() const
{
    if (child == nullptr || child->isRunning())
        return std::nullopt;
    return child->getExitCode();
}

void WorkerProcess::kill()
{
    if (child != nullptr && child->isRunning())
        child->kill();
}

#endif

} // namespace PresetRecorder

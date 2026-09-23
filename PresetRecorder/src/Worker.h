#pragma once
#include <juce_core/juce_core.h>

namespace PresetRecorder {

// Worker mode: "PresetRecorder.exe --worker <job.json>".
//
// The GUI never loads a plugin itself. Every scan and every recording happens
// in one of these short-lived processes, one plugin at a time, so a plugin that
// crashes, hangs or pops up an activation dialog only ever takes down (or
// stalls) its own worker. The worker reports everything it does through an
// append-only JSON-lines event log (see EventLogWriter); the GUI reads that log
// to follow progress and, when a worker dies, to find out which preset it was
// on so the retry can skip exactly that one.
//
// Job types:
//   scan    - list the plugins inside one file (AudioPluginFormat::findAllTypesForFile)
//             and write their descriptions to an XML file.
//   render  - load one plugin, enumerate its presets, and record each to FLAC.

// If the command line asks for worker mode, returns the job file.
bool isWorkerCommandLine(const juce::String& commandLine, juce::File& jobFile);

// Starts the job on a background thread (the message loop must keep running:
// plugins are created and fed their presets on the message thread). When the
// job ends the process exits with its result code - immediately, without
// unloading plugin DLLs, because some plugins crash in their unload code and
// there is nothing left to save by then.
void startWorker(const juce::File& jobFile);

// Worker exit codes.
enum WorkerExitCode
{
    workerOk = 0,
    workerBadJob = 2,
    workerLoadFailed = 3,
    workerNoOutput = 4,
    workerFailed = 5
};

} // namespace PresetRecorder

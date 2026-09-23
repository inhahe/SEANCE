#pragma once
#include <juce_events/juce_events.h>
#include "Util.h"
#include "WorkerProcess.h"
#include <deque>
#include <map>
#include <vector>

namespace PresetRecorder {

// Runs worker jobs (see Worker.h) a few at a time and turns what happens to
// them into callbacks on the message thread.
//
// Failure handling is the point of this class:
//   - A worker that stops reporting progress for too long (load timeout before
//     the plugin is loaded, preset timeout after) is killed - a plugin sitting
//     in an activation dialog or an endless loop can't stall the run.
//   - A render worker that dies or is killed part-way through a plugin is
//     restarted for the same plugin, skipping the preset it was on (and every
//     preset already finished), so one bad preset costs one preset, not the
//     rest of the plugin. After maxAttempts deaths the plugin is given up on.
class JobRunner : private juce::Timer
{
public:
    struct Job
    {
        enum class Type { scan, render };

        Type type = Type::scan;
        juce::String label;          // for the UI: a file name or "<Company> - <Plugin>"
        juce::var spec;              // the job file contents (Worker.cpp reads it)
        juce::String pluginId;       // render jobs
        juce::String fileOrId;       // scan jobs
        juce::StringArray skipKeys;  // presets not to attempt (finished or fatal in earlier attempts)
        int attempt = 0;
        int uid = 0;
    };

    struct Listener
    {
        virtual ~Listener() = default;
        virtual void jobStarted(const Job&) {}
        virtual void jobEvent(const Job&, const juce::var& event) = 0;
        // A preset was in progress when its worker died ("crashed") or was
        // killed for making no progress ("timeout").
        virtual void jobPresetLost(const Job&, const juce::String& key, const juce::String& status,
                                   const juce::String& reason) = 0;
        // The job is over for good (including all retries). `timedOut`: the
        // last attempt was killed for making no progress (as opposed to crashing
        // or failing).
        virtual void jobFinished(const Job&, bool ok, const juce::String& error, bool timedOut) = 0;
        virtual void allJobsFinished() = 0;
    };

    explicit JobRunner(Listener&);
    ~JobRunner() override;

    void setMaxParallel(int n)                          { maxParallel = juce::jlimit(1, 16, n); }
    void setTimeouts(double loadSeconds, double presetSeconds);

    void add(Job job);
    void cancelAll();

    bool isIdle() const { return queue.empty() && running.empty(); }
    int getNumRunning() const { return (int) running.size(); }
    int getNumQueued() const { return (int) queue.size(); }

    struct RunningInfo { juce::String label, status; int done = 0, total = 0; };
    std::vector<RunningInfo> getRunningInfo() const;

    static constexpr int maxAttempts = 6;

private:
    struct Running
    {
        Job job;
        WorkerProcess process;
        EventLogReader reader;
        juce::uint32 lastActivity = 0;
        juce::uint32 endSeenAt = 0;
        bool loaded = false;
        bool ended = false;
        bool timedOut = false;
        int endCode = -1;
        juce::String currentKey, currentName;
        juce::StringArray finishedKeys;
        juce::String lastError, crashWhat;
        juce::String status;
        int done = 0, total = 0;
        std::map<juce::String, juce::String> presetNames;
    };

    void timerCallback() override;
    void launch(Job job);
    void pump(Running& r);
    void finish(Running& r);

    Listener& listener;
    std::deque<Job> queue;
    std::vector<std::unique_ptr<Running>> running;
    int maxParallel = 2;
    double loadTimeout = 90.0, presetTimeout = 60.0;
    int nextUid = 1;
    bool wasBusy = false;
};

} // namespace PresetRecorder

#include "JobRunner.h"

namespace PresetRecorder {

JobRunner::JobRunner(Listener& l) : listener(l) {}

JobRunner::~JobRunner()
{
    stopTimer();
    queue.clear();
    running.clear(); // WorkerProcess destructors kill anything still running
}

void JobRunner::setTimeouts(double loadSeconds, double presetSeconds)
{
    loadTimeout = juce::jmax(5.0, loadSeconds);
    presetTimeout = juce::jmax(5.0, presetSeconds);
}

void JobRunner::add(Job job)
{
    if (job.uid == 0)
        job.uid = nextUid++;

    queue.push_back(std::move(job));
    wasBusy = true;

    if (! isTimerRunning())
        startTimer(100);
}

void JobRunner::cancelAll()
{
    queue.clear();

    for (auto& r : running)
        r->process.kill();

    running.clear();
}

std::vector<JobRunner::RunningInfo> JobRunner::getRunningInfo() const
{
    std::vector<RunningInfo> out;
    for (auto& r : running)
        out.push_back({ r->job.label, r->status, r->done, r->total });
    return out;
}

void JobRunner::launch(Job job)
{
    const auto scratch = getSessionScratchDir();
    const auto stem = "job-" + juce::String(job.uid) + "-" + juce::String(job.attempt);
    const auto jobFile = scratch.getChildFile(stem + ".json");
    const auto logFile = scratch.getChildFile(stem + ".log");
    logFile.deleteFile();

    auto spec = job.spec.clone();
    if (auto* obj = spec.getDynamicObject())
    {
        obj->setProperty("eventLog", logFile.getFullPathName());

        juce::Array<juce::var> skip;
        for (auto& k : job.skipKeys)
            skip.add(k);
        obj->setProperty("skipKeys", skip);
    }

    if (! writeJsonFile(jobFile, spec))
    {
        listener.jobFinished(job, false, "can't write the job file " + jobFile.getFullPathName(), false);
        return;
    }

    auto r = std::make_unique<Running>();
    r->job = std::move(job);
    r->reader = EventLogReader(logFile);
    r->lastActivity = juce::Time::getMillisecondCounter();
    r->status = r->job.type == Job::Type::scan ? "scanning" : "starting";

    juce::String error;
    if (! r->process.start(getExeFile(), { "--worker", jobFile.getFullPathName() }, error))
    {
        listener.jobFinished(r->job, false, error, false);
        return;
    }

    listener.jobStarted(r->job);
    running.push_back(std::move(r));
}

void JobRunner::pump(Running& r)
{
    for (auto& ev : r.reader.readNewEvents())
    {
        r.lastActivity = juce::Time::getMillisecondCounter();
        const auto type = ev["ev"].toString();
        const auto key = ev["key"].toString();

        if (type == "loading")
        {
            r.status = "loading the plugin";
        }
        else if (type == "loaded")
        {
            r.loaded = true;
            r.status = "loaded";
        }
        else if (type == "presets")
        {
            if (auto* list = ev["list"].getArray())
            {
                r.total = juce::jmin((int) ev["limit"], list->size());
                for (auto& p : *list)
                    r.presetNames[p["key"].toString()] = p["name"].toString();

                // A retry silently skips what earlier attempts finished or
                // lost; count those as done for the progress display.
                r.done = 0;
                for (int i = 0; i < r.total; ++i)
                    if (r.job.skipKeys.contains(list->getReference(i)["key"].toString()))
                        ++r.done;
            }
        }
        else if (type == "begin")
        {
            r.currentKey = key;
            r.currentName = r.presetNames.count(key) > 0 ? r.presetNames[key] : key;
            r.status = "preset " + juce::String((int) ev["index"] + 1) + "/" + juce::String((int) ev["count"])
                     + ": " + r.currentName;
        }
        else if (type == "done" || type == "skip" || type == "fail")
        {
            r.finishedKeys.addIfNotAlreadyThere(key);
            ++r.done;
            if (key == r.currentKey)
                r.currentKey = {};
        }
        else if (type == "error")
        {
            r.lastError = ev["message"].toString();
        }
        else if (type == "crash")
        {
            r.crashWhat = ev["what"].toString() + " (" + ev["code"].toString() + ")";
        }
        else if (type == "end")
        {
            r.ended = true;
            r.endCode = (int) ev["code"];
            r.endSeenAt = juce::Time::getMillisecondCounter();
        }

        listener.jobEvent(r.job, ev);
    }
}

void JobRunner::finish(Running& r)
{
    pump(r); // anything written just before the process ended

    if (r.ended)
    {
        listener.jobFinished(r.job, r.endCode == 0, r.lastError, false);
        return;
    }

    // The worker died or was killed without saying goodbye.
    juce::String reason;
    if (r.timedOut)
    {
        const auto limit = r.loaded ? presetTimeout : loadTimeout;
        reason = "no progress for " + juce::String(juce::roundToInt(limit))
               + " s, so it was stopped (the plugin may be waiting on a dialog, e.g. activation, or be stuck)";
    }
    else
    {
        const auto code = r.process.getExitCode();
        reason = "the plugin crashed: "
               + (r.crashWhat.isNotEmpty() ? r.crashWhat : describeExitCode(code.value_or(0xFFFFFFFFu)));
    }

    if (r.job.type == Job::Type::render && r.currentKey.isNotEmpty())
    {
        const int fruitless = r.finishedKeys.isEmpty() ? r.job.fruitlessAttempts + 1 : 0;
        const bool retry = fruitless < maxFruitlessAttempts && r.job.attempt + 1 < maxAttempts;

        listener.jobPresetLost(r.job, r.currentKey, r.timedOut ? "timeout" : "crashed", reason, retry);

        if (retry)
        {
            Job next = r.job;
            ++next.attempt;
            next.fruitlessAttempts = fruitless;
            next.skipKeys.addArray(r.finishedKeys);
            next.skipKeys.addIfNotAlreadyThere(r.currentKey);
            queue.push_front(std::move(next)); // carry on with this plugin first
            return;
        }

        listener.jobFinished(r.job, false,
                             fruitless >= maxFruitlessAttempts
                                 ? "gave up after " + juce::String(fruitless)
                                       + " crashes/timeouts in a row without finishing a preset; the last: " + reason
                                 : "gave up after " + juce::String(r.job.attempt + 1)
                                       + " crashes/timeouts; the last: " + reason,
                             r.timedOut);
        return;
    }

    if (r.job.type == Job::Type::render && r.loaded)
        listener.jobFinished(r.job, false, reason + " (between presets)", r.timedOut);
    else
        listener.jobFinished(r.job, false, (r.job.type == Job::Type::render ? "while loading: " : "") + reason, r.timedOut);
}

// Milliseconds since `then`. Signed and wrap-safe: the counter is a uint32, and
// `then` may be a hair *after* a `now` read earlier - which, unsigned, would
// look like 49 days.
static int millisecondsSince(juce::uint32 then)
{
    return (int) (juce::Time::getMillisecondCounter() - then);
}

void JobRunner::timerCallback()
{
    for (size_t i = 0; i < running.size();)
    {
        auto& r = *running[i];
        pump(r);

        if (r.process.isRunning())
        {
            if (r.ended)
            {
                // Workers exit straight after "end"; one that lingers is stuck in shutdown.
                if (millisecondsSince(r.endSeenAt) > 10000)
                    r.process.kill();
            }
            else
            {
                const double limit = (r.job.type == Job::Type::render && r.loaded) ? presetTimeout : loadTimeout;
                if ((double) millisecondsSince(r.lastActivity) > limit * 1000.0)
                {
                    r.timedOut = true;
                    r.process.kill();
                }
            }

            if (r.process.isRunning())
            {
                ++i;
                continue;
            }
        }

        auto finished = std::move(running[i]);
        running.erase(running.begin() + (std::ptrdiff_t) i);
        finish(*finished);
    }

    while ((int) running.size() < maxParallel && ! queue.empty())
    {
        auto job = std::move(queue.front());
        queue.pop_front();
        launch(std::move(job));
    }

    if (isIdle())
    {
        stopTimer();
        if (wasBusy)
        {
            wasBusy = false;
            listener.allJobsFinished();
        }
    }
}

} // namespace PresetRecorder

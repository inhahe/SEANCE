#include "Controller.h"
#include "PreviewInput.h"
#include "PresetSources.h"
#include "Util.h"
#include <thread>

namespace PresetRecorder {

static constexpr const char* kDryInputName = "Effect input (dry).flac";

Controller::Controller()
{
    logFile = getAppDataDir().getChildFile("PresetRecorder.log");
    if (logFile.getSize() > 4 * 1024 * 1024)
        logFile.moveFileTo(logFile.getSiblingFile("PresetRecorder.old.log"));

    cleanOldScratchDirs();
    catalog.loadCache();
    reloadSeanceConfig();

    manifest.setRoot(settings.getOutputDir());
    manifest.load();

    startTimer(5000);
}

Controller::~Controller()
{
    stopTimer();
    runner.cancelAll();
    if (manifest.isDirty())
        manifest.save();
    catalog.saveCache();
    settings.saveNow();
}

//==============================================================================
void Controller::log(const juce::String& line)
{
    const auto stamped = "[" + juce::Time::getCurrentTime().formatted("%H:%M:%S") + "] " + line;

    logLines.add(stamped);
    if (logLines.size() > 5000)
        logLines.removeRange(0, 1000);

    logFile.appendText(juce::Time::getCurrentTime().formatted("%Y-%m-%d ") + stamped + "\n", false, false, "\n");

    if (onLogLine)
        onLogLine(stamped);
}

static juce::String searchPathFor(Catalog& catalog, const juce::String& formatName)
{
    for (auto* f : catalog.getFormats().getFormats())
        if (f->getName() == formatName)
            return catalog.getSearchPath(*f).toString();
    return {};
}

//==============================================================================
void Controller::reloadSeanceConfig()
{
    const auto overridePath = settings.getSeanceConfigOverride();
    const auto cfg = SeanceConfig::load(overridePath.isNotEmpty() ? juce::File(overridePath) : juce::File());

    catalog.setSeanceConfig(cfg);
    catalog.setUnskipped(settings.getUnskipped());

    if (! isBusy())
        catalog.enumerate(false); // lists files (and the skip list) without loading anything

    if (cfg.configFound)
        log("SoundShop2 plugin settings: " + cfg.configFile.getFullPathName()
            + (cfg.autoDetected ? " (found automatically)" : "") + " - " + cfg.summary());
    else
        log("SoundShop2 plugin settings: " + cfg.summary());

    sendChangeMessage();
}

void Controller::setSeanceConfigFile(const juce::String& pathOrEmptyForAuto)
{
    settings.setSeanceConfigOverride(pathOrEmptyForAuto);
    reloadSeanceConfig();
}

void Controller::setUnskipped(const juce::StringArray& keys)
{
    settings.setUnskipped(keys);
    catalog.setUnskipped(keys);

    if (! isBusy())
        catalog.enumerate(false);

    sendChangeMessage();
}

void Controller::setOutputDir(const juce::File& dir)
{
    if (isBusy() || dir == juce::File())
        return;

    if (manifest.isDirty())
        manifest.save();

    settings.setOutputDir(dir);
    manifest.setRoot(dir);
    manifest.load();
    sendChangeMessage();
}

juce::File Controller::getEffectInputSong() const
{
    const auto overridePath = settings.getInputSongOverride();
    if (overridePath.isNotEmpty())
        return juce::File(overridePath);
    return AppSettings::findBundledSong();
}

bool Controller::isUsingBundledSong() const
{
    return settings.getInputSongOverride().isEmpty();
}

void Controller::downloadBundledSong(std::function<void(bool, juce::String)> done)
{
    const auto target = AppSettings::downloadedSongLocation();
    juce::WeakReference<Controller> weak(this);
    log("Downloading the effect input song from Wikimedia Commons...");

    std::thread([target, done, weak]
    {
        juce::String error;
        bool ok = false;
        int status = 0;

        const juce::URL url(AppSettings::bundledSongUrl());
        auto stream = url.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                                .withConnectionTimeoutMs(20000)
                                                .withStatusCode(&status)
                                                .withExtraHeaders("User-Agent: PresetRecorder/1.0 (SEANCE companion tool)"));

        if (stream == nullptr || status >= 400)
        {
            error = "couldn't download " + url.toString(false)
                  + (status >= 400 ? " (HTTP " + juce::String(status) + ")" : juce::String());
        }
        else
        {
            // Keep the .mp3 extension on the temporary name: the check below
            // opens it through AudioFormatManager, which picks decoders by extension.
            const auto temp = target.getSiblingFile(target.getFileNameWithoutExtension() + ".download"
                                                    + target.getFileExtension());
            temp.deleteFile();
            {
                juce::FileOutputStream out(temp);
                if (! out.openedOk())
                    error = "can't write " + temp.getFullPathName();
                else
                    out.writeFromInputStream(*stream, -1);
            }

            if (error.isEmpty() && temp.getSize() > 1000000 && getAudioFileLength(temp) > 60.0)
            {
                target.deleteFile();
                ok = temp.moveFileTo(target);
                if (! ok)
                    error = "can't move the download to " + target.getFullPathName();
            }
            else if (error.isEmpty())
            {
                error = "the download was incomplete or isn't a readable MP3";
                temp.deleteFile();
            }
        }

        juce::MessageManager::callAsync([weak, done, ok, error]
        {
            if (auto* c = weak.get())
            {
                c->log(ok ? "Downloaded the effect input song." : "Download failed: " + error);
                if (done)
                    done(ok, error);
                c->sendChangeMessage();
            }
        });
    }).detach();
}

//==============================================================================
void Controller::applyTimeoutsAndParallelism()
{
    runner.setMaxParallel(settings.getParallelWorkers());
    runner.setTimeouts(settings.getLoadTimeoutSeconds(), settings.getPresetTimeoutSeconds());
}

void Controller::scan(bool forceRescan)
{
    if (! isBusy())
        startScan(forceRescan, nullptr);
}

void Controller::startScan(bool forceRescan, std::function<void()> then)
{
    applyTimeoutsAndParallelism();
    phase = Phase::scanning;
    stopRequested = false;
    afterScan = std::move(then);

    // SoundShop2 may have changed its folders or skip list since we started.
    const auto overridePath = settings.getSeanceConfigOverride();
    catalog.setSeanceConfig(SeanceConfig::load(overridePath.isNotEmpty() ? juce::File(overridePath) : juce::File()));
    catalog.setUnskipped(settings.getUnskipped());

    if (forceRescan)
        catalog.clearCache();

    const auto todo = catalog.enumerate(forceRescan);
    scanTotal = (int) todo.size();
    scanDone = 0;
    scanResultFiles.clear();

    log(juce::String("Scanning SoundShop2's plugin folders: ") + juce::String((int) catalog.getFiles().size())
        + " plugin files found, " + juce::String(scanTotal) + " to scan"
        + (scanTotal < (int) catalog.getFiles().size() ? " (the rest are cached or skipped)" : ""));

    int n = 0;
    for (auto& req : todo)
    {
        const auto resultFile = getSessionScratchDir().getChildFile("scan-" + juce::String(++n) + ".xml");
        scanResultFiles[req.fileOrId] = resultFile.getFullPathName();

        JobRunner::Job job;
        job.type = JobRunner::Job::Type::scan;
        job.label = juce::File::isAbsolutePath(req.fileOrId) ? juce::File(req.fileOrId).getFileName() : req.fileOrId;
        job.fileOrId = req.fileOrId;
        job.spec = makeObject({ { "type", "scan" },
                                { "format", req.format },
                                { "fileOrId", req.fileOrId },
                                { "resultFile", resultFile.getFullPathName() },
                                { "searchPath", searchPathFor(catalog, req.format) } });

        catalog.setScanning(req.fileOrId);
        runner.add(std::move(job));
    }

    sendChangeMessage();

    if (todo.empty())
        finishScan();
}

void Controller::finishScan()
{
    catalog.saveCache();
    scannedThisSession = true;
    phase = Phase::idle;

    const auto plugins = catalog.getRecordablePlugins();
    int instruments = 0, failed = 0, skipped = 0;
    for (auto& p : plugins)
        instruments += p.instrument ? 1 : 0;
    for (auto& f : catalog.getFiles())
    {
        failed += f.state == PluginFile::State::scanFailed ? 1 : 0;
        skipped += f.state == PluginFile::State::skipped ? 1 : 0;
    }

    log("Scan complete: " + juce::String((int) plugins.size()) + " plugins ("
        + juce::String(instruments) + " instruments, " + juce::String((int) plugins.size() - instruments)
        + " effects); " + juce::String(failed) + " file(s) failed to scan; "
        + juce::String(skipped) + " skipped because of SoundShop2's skip list.");

    auto then = std::move(afterScan);
    afterScan = nullptr;
    sendChangeMessage();

    if (then && ! stopRequested)
        then();
}

//==============================================================================
void Controller::recordAll()
{
    recordPlugins({});
}

void Controller::recordPlugins(const juce::StringArray& pluginIds)
{
    if (isBusy())
        return;

    // Scan first if nothing has been scanned yet this session, or if some file
    // still needs it (e.g. a skip-list entry unskipped since the last scan).
    // Cached files are not re-scanned, so this is cheap when nothing changed.
    bool anythingPending = false;
    for (auto& f : catalog.getFiles())
        anythingPending = anythingPending || f.state == PluginFile::State::pending;

    if (! scannedThisSession || anythingPending)
        startScan(false, [this, pluginIds] { startRecording(pluginIds); });
    else
        startRecording(pluginIds);
}

void Controller::removePartialFiles()
{
    for (auto* sub : { Manifest::instrumentsFolder, Manifest::effectsFolder })
    {
        const auto dir = manifest.getRoot().getChildFile(sub);
        if (! dir.isDirectory())
            continue;

        for (const auto& entry : juce::RangedDirectoryIterator(dir, false, "*.partial", juce::File::findFiles))
            entry.getFile().deleteFile();
    }
}

void Controller::startRecording(const juce::StringArray& pluginIds)
{
    const auto all = catalog.getRecordablePlugins();
    std::vector<PluginEntry> chosen;
    for (auto& p : all)
        if (pluginIds.isEmpty() || pluginIds.contains(p.id))
            chosen.push_back(p);

    if (chosen.empty())
    {
        log(pluginIds.isEmpty() ? "Nothing to record: no loadable plugins were found in SoundShop2's plugin folders."
                                : "Nothing to record: the selected plugins are skipped or failed to scan.");
        phase = Phase::idle;
        sendChangeMessage();
        return;
    }

    const auto outDir = settings.getOutputDir();
    if (! outDir.createDirectory())
    {
        log("Can't create the output folder " + outDir.getFullPathName());
        phase = Phase::idle;
        sendChangeMessage();
        return;
    }

    if (manifest.getRoot() != outDir)
    {
        manifest.setRoot(outDir);
        manifest.load();
    }

    removePartialFiles();
    applyTimeoutsAndParallelism();

    phase = Phase::preparing;
    stopRequested = false;
    runStarted = juce::Time::getCurrentTime();
    runPluginsTotal = (int) chosen.size();
    runPluginsDone = runPresetsRecorded = 0;
    liveStatus.clear();
    for (auto& p : chosen)
        liveStatus[p.id] = "queued";
    sendChangeMessage();

    // Decoding the song and indexing preset files can take a few seconds, so
    // do it off the message thread.
    bool needEffectInput = false;
    juce::Array<juce::File> presetRoots = Vst3PresetIndex::standardRoots();
    for (auto& p : chosen)
    {
        needEffectInput = needEffectInput || ! p.instrument;

        // Some VST3 bundles carry factory .vstpresets inside them.
        const juce::File f(p.desc.fileOrIdentifier);
        if (p.desc.pluginFormatName == "VST3" && f.isDirectory())
            presetRoots.addIfNotAlreadyThere(f.getChildFile("Contents").getChildFile("Resources"));
    }

    const auto render = settings.getRenderSettings();
    const auto song = getEffectInputSong();
    const double songStart = settings.getSongStart(), songLength = settings.getSongLength();
    const auto scratch = getSessionScratchDir();
    const auto dryFile = outDir.getChildFile(kDryInputName);
    const int generation = ++runGeneration;
    juce::WeakReference<Controller> weak(this);

    log("Preparing: " + juce::String((int) chosen.size()) + " plugin(s) to record"
        + (needEffectInput ? ", decoding the effect input (" + song.getFileName() + " from "
                             + juce::String(songStart, 2) + " s, " + juce::String(songLength, 2) + " s long)"
                           : juce::String())
        + (render.includePresetFiles ? ", indexing .vstpreset files" : juce::String()) + "...");

    std::thread([=]
    {
        juce::String inputError;
        juce::File inputFile;

        if (needEffectInput)
        {
            juce::AudioBuffer<float> excerpt;
            if (prepareSongExcerpt(song, songStart, songLength, render.sampleRate, excerpt, inputError))
            {
                inputFile = scratch.getChildFile("effect_input.wav");
                if (! writeAudioFile(inputFile, excerpt, 2, render.sampleRate, 32, inputError))
                    inputFile = juce::File();
                else
                {
                    juce::String ignored;
                    writeAudioFile(dryFile, excerpt, 2, render.sampleRate, render.bitDepth, ignored);
                }
            }
        }

        juce::File indexFile;
        int numPresetFiles = 0;
        if (render.includePresetFiles)
        {
            const auto index = Vst3PresetIndex::build(presetRoots);
            numPresetFiles = index.numFiles;
            indexFile = scratch.getChildFile("vst3_presets.json");
            if (! writeJsonFile(indexFile, index.toVar()))
                indexFile = juce::File();
        }

        juce::MessageManager::callAsync([=]
        {
            auto* c = weak.get();
            if (c == nullptr || c->runGeneration != generation || c->stopRequested)
                return;

            if (render.includePresetFiles)
                c->log("Found " + juce::String(numPresetFiles) + " .vstpreset file(s) in the standard VST3 preset folders.");
            if (needEffectInput && inputError.isNotEmpty())
                c->log("Effect input problem: " + inputError + " - effects will be skipped this run.");

            c->queueRenderJobs(chosen, inputFile, inputError, indexFile);
        });
    }).detach();
}

void Controller::queueRenderJobs(const std::vector<PluginEntry>& plugins, const juce::File& effectInput,
                                 const juce::String& effectInputError, const juce::File& presetIndex)
{
    phase = Phase::recording;

    if (effectInput.existsAsFile())
        manifest.dryInputFile = kDryInputName;

    const auto render = settings.getRenderSettings();

    juce::String midiFile;
    if (settings.getUseMidiFile())
    {
        PreviewPhrase phrase;
        juce::String err;
        midiFile = settings.getMidiFile();
        if (! loadMidiFilePhrase(juce::File(midiFile), phrase, err))
        {
            log("MIDI file problem: " + err + " - instruments will play the built-in phrase instead.");
            midiFile = {};
        }
    }

    int queued = 0;
    for (auto& p : plugins)
    {
        auto& result = manifest.getOrCreate(p.id);
        result.name = p.desc.name;
        result.company = p.desc.manufacturerName;
        result.format = p.desc.pluginFormatName;
        result.version = p.desc.version;
        result.category = p.desc.category;
        result.path = p.desc.fileOrIdentifier;
        result.kind = p.instrument ? "instrument" : "effect";
        result.baseName = p.baseName;
        result.lastRun = nowString();
        result.error = {};

        if (! p.instrument && ! effectInput.existsAsFile())
        {
            result.status = result.countWithAudio() > 0 ? "partial" : "failed";
            result.error = "no effect input: " + effectInputError;
            liveStatus[p.id] = "skipped: no effect input";
            ++runPluginsDone;
            continue;
        }

        result.status = "recording";

        auto xml = p.desc.createXml();

        JobRunner::Job job;
        job.type = JobRunner::Job::Type::render;
        job.label = p.baseName;
        job.pluginId = p.id;
        job.spec = makeObject({ { "type", "render" },
                                { "plugin", xml != nullptr ? xml->toString() : juce::String() },
                                { "pluginId", p.id },
                                { "baseName", p.baseName },
                                { "outputRoot", manifest.getRoot().getFullPathName() },
                                { "subfolder", p.instrument ? Manifest::instrumentsFolder : Manifest::effectsFolder },
                                { "instrument", p.instrument },
                                { "settings", render.toVar() },
                                { "effectInput", effectInput.getFullPathName() },
                                { "midiFile", midiFile },
                                { "presetIndex", presetIndex.getFullPathName() },
                                { "searchPath", searchPathFor(catalog, p.desc.pluginFormatName) } });
        runner.add(std::move(job));
        ++queued;
    }

    manifest.save();
    log("Recording " + juce::String(queued) + " plugin(s) into " + manifest.getRoot().getFullPathName()
        + " (" + juce::String(settings.getParallelWorkers()) + " at a time).");
    sendChangeMessage();

    if (queued == 0)
        allJobsFinished();
}

void Controller::stop()
{
    if (! isBusy())
        return;

    stopRequested = true;
    ++runGeneration;
    afterScan = nullptr;
    runner.cancelAll();

    for (auto& [id, status] : liveStatus)
        if (! status.startsWith("done") && ! status.startsWith("failed") && ! status.startsWith("skipped"))
            status = "stopped";

    for (auto& r : manifest.getPlugins())
    {
        if (r.status == "recording")
            r.status = r.countWithAudio() > 0 ? "partial" : "not recorded";

        for (auto& p : r.presets)
            if (p.status == "pending")
                p.status = "not recorded";
    }

    if (phase == Phase::scanning)
    {
        for (auto& f : catalog.getFiles())
            if (f.state == PluginFile::State::scanning)
                f.state = PluginFile::State::pending;
        catalog.saveCache();
    }

    manifest.save();
    removePartialFiles();
    phase = Phase::idle;
    log("Stopped.");
    sendChangeMessage();
}

//==============================================================================
// `keepKeys`: presets this attempt won't touch because an earlier attempt of the
// same run finished them or lost them (crash/timeout) - their results stand.
// Everything else without a recording goes back to "pending".
void Controller::mergePresetList(PluginResult& result, const juce::var& event, const juce::StringArray& keepKeys)
{
    const int limit = (int) event["limit"];
    std::vector<PresetResult> merged;

    if (auto* list = event["list"].getArray())
    {
        for (int i = 0; i < list->size(); ++i)
        {
            const auto& item = list->getReference(i);
            const auto key = item["key"].toString();

            PresetResult p;
            if (auto* old = result.findPreset(key))
                p = *old;

            p.key = key;
            p.name = item["name"].toString();
            p.source = item["source"].toString();

            if (! p.hasAudio() && ! keepKeys.contains(key))
            {
                p.file = item["file"].toString();
                p.status = i < limit ? "pending" : "not recorded";
                p.note = i < limit ? juce::String()
                                   : "beyond the \"presets per plugin\" limit of " + juce::String(limit);
            }

            merged.push_back(p);
        }
    }

    result.presets = std::move(merged);
    manifest.markDirty();
}

void Controller::jobStarted(const JobRunner::Job& job)
{
    if (job.type == JobRunner::Job::Type::render)
        liveStatus[job.pluginId] = job.attempt > 0 ? "restarting after a crash" : "starting";
    sendChangeMessage();
}

void Controller::jobEvent(const JobRunner::Job& job, const juce::var& ev)
{
    if (job.type != JobRunner::Job::Type::render)
        return;

    auto* result = manifest.find(job.pluginId);
    if (result == nullptr)
        return;

    const auto type = ev["ev"].toString();
    const auto key = ev["key"].toString();

    auto presetFor = [&]() -> PresetResult&
    {
        if (auto* p = result->findPreset(key))
            return *p;
        PresetResult p;
        p.key = key;
        p.name = key;
        result->presets.push_back(p);
        return result->presets.back();
    };

    if (type == "loading")
    {
        liveStatus[job.pluginId] = "loading";
    }
    else if (type == "presets")
    {
        mergePresetList(*result, ev, job.skipKeys);
        liveStatus[job.pluginId] = "0/" + juce::String(juce::jmin((int) ev["limit"],
                                                                  ev["list"].getArray() != nullptr ? ev["list"].getArray()->size() : 0));
    }
    else if (type == "begin")
    {
        liveStatus[job.pluginId] = "recording " + juce::String((int) ev["index"] + 1) + "/" + juce::String((int) ev["count"]);
    }
    else if (type == "done")
    {
        auto& p = presetFor();
        p.file = ev["file"].toString();
        p.status = ev["status"].toString();
        p.seconds = (double) ev["seconds"];
        p.peakDb = (double) ev["peakDb"];
        p.note = ev["note"].toString();
        p.recordedAt = nowString();
        ++runPresetsRecorded;
        manifest.markDirty();
    }
    else if (type == "skip")
    {
        auto& p = presetFor();
        p.file = ev["file"].toString();
        if (! p.hasAudio())
        {
            p.status = "ok";
            p.note = "recorded in an earlier run";
        }
        manifest.markDirty();
    }
    else if (type == "fail")
    {
        auto& p = presetFor();
        p.status = "failed";
        p.note = ev["reason"].toString();
        log("  " + job.label + ": preset \"" + p.name + "\" failed - " + p.note);
        manifest.markDirty();
    }
    else if (type == "error")
    {
        result->error = ev["message"].toString();
    }

    sendChangeMessage();
}

void Controller::jobPresetLost(const JobRunner::Job& job, const juce::String& key,
                               const juce::String& status, const juce::String& reason)
{
    if (auto* result = manifest.find(job.pluginId))
    {
        auto* p = result->findPreset(key);
        if (p == nullptr)
        {
            PresetResult fresh;
            fresh.key = fresh.name = key;
            result->presets.push_back(fresh);
            p = &result->presets.back();
        }

        p->status = status;
        p->note = reason;
        p->file = {};
        manifest.markDirty();

        log("  " + job.label + ": preset \"" + p->name + "\" - " + reason
            + (job.attempt + 1 < JobRunner::maxAttempts ? "; restarting the plugin without it." : "."));
    }

    sendChangeMessage();
}

void Controller::jobFinished(const JobRunner::Job& job, bool ok, const juce::String& error, bool timedOut)
{
    if (job.type == JobRunner::Job::Type::scan)
    {
        ++scanDone;
        const juce::File resultFile(scanResultFiles[job.fileOrId]);

        if (ok)
        {
            juce::Array<juce::PluginDescription> types;
            if (auto xml = juce::parseXML(resultFile))
                for (auto* child : xml->getChildIterator())
                {
                    juce::PluginDescription d;
                    if (d.loadFromXml(*child))
                        types.add(d);
                }

            catalog.applyScanResult(job.fileOrId, types);

            juce::StringArray names;
            for (auto& t : types)
                names.add(t.name + (t.isInstrument ? " (instrument)" : " (effect)"));

            if (names.isEmpty())
            {
                const auto* f = catalog.findFile(job.fileOrId);
                log("  " + job.label + ": scan failed - " + (f != nullptr ? f->error : juce::String("no plugins inside")));
            }
            else
            {
                log("  " + job.label + ": " + names.joinIntoString(", "));
            }
        }
        else
        {
            catalog.applyScanFailure(job.fileOrId, error.isNotEmpty() ? error : juce::String("scan failed"), ! timedOut);
            log("  " + job.label + ": scan failed - " + (error.isNotEmpty() ? error : juce::String("unknown reason")));
        }

        sendChangeMessage();
        return;
    }

    ++runPluginsDone;
    auto* result = manifest.find(job.pluginId);
    if (result == nullptr)
        return;

    int good = 0, silent = 0, bad = 0;
    for (auto& p : result->presets)
    {
        if (p.status == "pending")
        {
            p.status = "not recorded";
            p.note = ok ? juce::String() : "the plugin stopped before reaching this preset";
        }

        good += p.status == "ok" ? 1 : 0;
        silent += p.status == "silent" ? 1 : 0;
        bad += (p.status == "failed" || p.status == "crashed" || p.status == "timeout") ? 1 : 0;
    }

    if (! ok && good + silent == 0)
        result->status = "failed";
    else if (! ok || bad > 0)
        result->status = "partial";
    else
        result->status = "ok";

    if (! ok)
        result->error = error.isNotEmpty() ? error : result->error;

    juce::String summary;
    if (result->status == "failed")
        summary = "failed: " + (result->error.isNotEmpty() ? result->error : juce::String("unknown error"));
    else
        summary = "done: " + juce::String(good) + " recorded"
                + (silent > 0 ? ", " + juce::String(silent) + " silent" : juce::String())
                + (bad > 0 ? ", " + juce::String(bad) + " failed" : juce::String());

    liveStatus[job.pluginId] = summary;
    log(job.label + " - " + summary);

    manifest.save();
    sendChangeMessage();
}

void Controller::allJobsFinished()
{
    if (phase == Phase::scanning)
    {
        finishScan();
        return;
    }

    if (phase != Phase::recording)
        return;

    phase = Phase::idle;
    manifest.save();
    removePartialFiles();

    const auto elapsed = (juce::Time::getCurrentTime() - runStarted).inSeconds();
    log("Finished: " + juce::String(runPluginsDone) + " plugin(s), " + juce::String(runPresetsRecorded)
        + " preset(s) recorded in " + formatDuration(elapsed) + ". Output: " + manifest.getRoot().getFullPathName());
    sendChangeMessage();
}

void Controller::timerCallback()
{
    if (manifest.isDirty())
        manifest.save();
}

//==============================================================================
Controller::Progress Controller::getProgress() const
{
    Progress p;
    p.pluginsDone = runPluginsDone;
    p.pluginsTotal = runPluginsTotal;
    p.presetsRecorded = runPresetsRecorded;

    for (auto& w : runner.getRunningInfo())
        p.workers.add(w.label + "  -  " + w.status);

    switch (phase)
    {
        case Phase::idle:
            p.headline = "Ready.";
            p.fraction = 0.0;
            break;

        case Phase::scanning:
            p.headline = "Scanning plugins: " + juce::String(scanDone) + " of " + juce::String(scanTotal) + " files";
            p.fraction = scanTotal > 0 ? (double) scanDone / scanTotal : -1.0;
            break;

        case Phase::preparing:
            p.headline = "Preparing (decoding the effect input, indexing preset files)...";
            p.fraction = -1.0;
            break;

        case Phase::recording:
        {
            double partial = 0.0;
            for (auto& w : runner.getRunningInfo())
                if (w.total > 0)
                    partial += juce::jlimit(0.0, 1.0, (double) w.done / w.total);

            p.headline = "Recording: " + juce::String(runPluginsDone) + " of " + juce::String(runPluginsTotal)
                       + " plugins done, " + juce::String(runPresetsRecorded) + " presets recorded";
            p.fraction = runPluginsTotal > 0 ? juce::jlimit(0.0, 1.0, (runPluginsDone + partial) / runPluginsTotal) : -1.0;
            break;
        }
    }

    return p;
}

juce::String Controller::getLiveStatus(const juce::String& pluginId) const
{
    auto it = liveStatus.find(pluginId);
    if (it != liveStatus.end())
        return it->second;

    for (auto& r : manifest.getPlugins())
    {
        if (r.id != pluginId)
            continue;

        const int n = r.countWithAudio();
        if (r.status == "failed")
            return "failed: " + r.error;
        if (n > 0)
            return juce::String(n) + " recorded" + (r.status == "partial" ? " (some failed)" : "");
        return r.status;
    }

    return "not recorded yet";
}

} // namespace PresetRecorder

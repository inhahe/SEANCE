#include "Worker.h"
#include "Util.h"
#include "Settings.h"
#include "PresetSources.h"
#include "PreviewInput.h"
#include "PluginFileInfo.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <set>
#include <thread>

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

//==============================================================================
// Crash reporting
//
// When a plugin crashes the worker, the GUI already knows which preset it was
// on (the last "begin" without a "done"). These handlers add the *kind* of
// crash to the log. They run in a broken process, so they format into a static
// buffer and append through the log's raw handle - no heap, no locks.

static EventLogWriter* gCrashLog = nullptr;

static void writeCrashEvent(const char* what, unsigned long code) noexcept
{
    if (gCrashLog == nullptr)
        return;

    static char buffer[160];
    const char prefix[] = "{\"ev\":\"crash\",\"what\":\"";
    int n = 0;

    for (const char* p = prefix; *p != 0 && n < 100; ++p)
        buffer[n++] = *p;
    for (const char* p = what; *p != 0 && n < 130; ++p)
        buffer[n++] = (*p == '"' || *p == '\\') ? '\'' : *p;

    const char mid[] = "\",\"code\":\"0x";
    for (const char* p = mid; *p != 0; ++p)
        buffer[n++] = *p;

    for (int shift = 28; shift >= 0; shift -= 4)
        buffer[n++] = "0123456789ABCDEF"[(code >> shift) & 0xF];

    buffer[n++] = '"';
    buffer[n++] = '}';
    buffer[n++] = '\n';
    gCrashLog->writeRawFromCrashHandler(buffer, n);
}

#if JUCE_WINDOWS
static LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* info)
{
    const auto code = (info != nullptr && info->ExceptionRecord != nullptr)
                          ? info->ExceptionRecord->ExceptionCode : 0xE0000001ul;
    writeCrashEvent("unhandled exception", code);
    TerminateProcess(GetCurrentProcess(), code);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

// Ends the worker without running any unload code. std::_Exit is ExitProcess
// in a desktop app, which still calls every loaded DLL's DLL_PROCESS_DETACH -
// plugin code - whereas TerminateProcess runs none. Everything worth keeping
// is on disk and in the event log by the time this is called.
[[noreturn]] static void terminateWorker(int code)
{
   #if JUCE_WINDOWS
    TerminateProcess(GetCurrentProcess(), (UINT) code);
   #endif
    std::_Exit(code);
}

static void onAbortSignal(int)
{
    writeCrashEvent("abort() called", 3);
    terminateWorker(3);
}

static void onTerminate()
{
    writeCrashEvent("std::terminate (an exception escaped)", 0xE06D7363ul);
    terminateWorker((int) 0xE06D7363ul);
}

static void installCrashHandlers()
{
   #if JUCE_WINDOWS
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
   #endif
    std::signal(SIGABRT, onAbortSignal);
    std::set_terminate(onTerminate);
}

//==============================================================================
// Fault injection for testing the GUI's crash/hang recovery without a
// misbehaving plugin: PRESETRECORDER_FAULT_TEST="crash:program:1;hang:program:2"
// makes a worker crash (access violation) or hang when it reaches a preset with
// that key. Workers inherit it from the GUI's environment. See design.md.
static void injectFaultIfRequested(const juce::String& presetKey)
{
    static const juce::StringArray faults = juce::StringArray::fromTokens(
        juce::SystemStats::getEnvironmentVariable("PRESETRECORDER_FAULT_TEST", {}), ";", "");

    if (faults.contains("crash:" + presetKey))
    {
        volatile int* nowhere = nullptr;
        *nowhere = 42;
    }

    if (faults.contains("hang:" + presetKey))
        for (;;)
            juce::Thread::sleep(1000);
}

//==============================================================================
// Helpers

// Runs `fn` on the message thread and waits. An exception thrown there (by
// plugin code) is carried back and rethrown on the calling thread, where the
// per-preset handler can deal with it - left on the message thread it would
// never complete the call and the worker would hang until the watchdog.
static void onMessageThread(std::function<void()> fn)
{
    std::exception_ptr error;
    juce::MessageManager::callSync([&fn, &error]
    {
        try { fn(); }
        catch (...) { error = std::current_exception(); }
    });

    if (error != nullptr)
        std::rethrow_exception(error);
}

static juce::AudioPluginFormat* findFormat(juce::AudioPluginFormatManager& fm, const juce::String& name)
{
    for (auto* f : fm.getFormats())
        if (f->getName() == name)
            return f;
    return nullptr;
}

// LV2 plugins are identified by URI and only resolve once their bundle has been
// loaded into the LV2 world. JUCE's LV2 format loads its default folders by
// itself; SoundShop2's extra folders have to be loaded by searching them. The
// job carries only those (Catalog::getUserFolders): searching a folder the
// world has already read logs a "Reloading plugin" warning per plugin in it.
static void loadExtraLv2Folders(juce::AudioPluginFormat& format, const juce::String& extraFolders)
{
    if (format.getName() == "LV2" && extraFolders.isNotEmpty())
        onMessageThread([&] { format.searchPathsForPlugins(juce::FileSearchPath(extraFolders), true, false); });
}

//==============================================================================
// A host transport: always 4/4 at the job's tempo. Tempo-synced delays, LFOs
// and arpeggiators need one, and without it many plugins assume 120 BPM or
// don't run at all.
class RenderPlayHead final : public juce::AudioPlayHead
{
public:
    RenderPlayHead(double bpmIn, double rateIn) : bpm(bpmIn), rate(rateIn) {}

    void set(juce::int64 samplePosition, bool isPlaying)
    {
        position.store(samplePosition);
        playing.store(isPlaying);
    }

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        const auto pos = position.load();
        const double seconds = (double) pos / rate;
        const double ppq = seconds * bpm / 60.0;

        info.setBpm(bpm);
        info.setTimeSignature(TimeSignature { 4, 4 });
        info.setTimeInSamples(pos);
        info.setTimeInSeconds(seconds);
        info.setPpqPosition(ppq);
        info.setPpqPositionOfLastBarStart(std::floor(ppq / 4.0) * 4.0);
        info.setBarCount((juce::int64) std::floor(ppq / 4.0));
        info.setIsPlaying(playing.load());
        return info;
    }

private:
    const double bpm, rate;
    std::atomic<juce::int64> position { 0 };
    std::atomic<bool> playing { false };
};

//==============================================================================
struct Preset
{
    enum class Kind { current, program, file };

    Kind kind = Kind::current;
    juce::String key, name, source;
    int program = -1;
    juce::File file;
    juce::String fileName; // "<baseName> - <name>.flac", unique within the plugin
};

// The plugin's own class ID, read back from its state saved as a .vstpreset.
static juce::String getVst3ClassId(juce::AudioPluginInstance& instance)
{
    struct Visitor final : juce::ExtensionsVisitor
    {
        juce::String id;
        void visitVST3Client(const VST3Client& client) override
        {
            auto data = client.getPreset();
            id = Vst3PresetIndex::readClassId(data.getData(), data.getSize());
        }
    };

    Visitor v;
    instance.getExtensions(v);
    return v.id;
}

static bool loadVst3PresetFile(juce::AudioPluginInstance& instance, const juce::File& file)
{
    juce::MemoryBlock data;
    if (! file.loadFileAsData(data))
        return false;

    struct Visitor final : juce::ExtensionsVisitor
    {
        const juce::MemoryBlock* data = nullptr;
        bool ok = false;
        void visitVST3Client(const VST3Client& client) override { ok = client.setPreset(*data); }
    };

    Visitor v;
    v.data = &data;
    instance.getExtensions(v);
    return v.ok;
}

static std::vector<Preset> enumeratePresets(juce::AudioPluginInstance& instance,
                                            const juce::PluginDescription& desc,
                                            const RenderSettings& settings,
                                            const juce::File& presetIndexFile,
                                            juce::String& classIdOut)
{
    std::vector<Preset> presets;
    juce::String singleProgramName;

    if (settings.includePrograms)
    {
        const int n = instance.getNumPrograms();

        if (n > 1)
        {
            for (int i = 0; i < n; ++i)
            {
                Preset p;
                p.kind = Preset::Kind::program;
                p.program = i;
                p.key = "program:" + juce::String(i);
                p.name = instance.getProgramName(i).trim();
                if (p.name.isEmpty())
                    p.name = "Program " + juce::String(i + 1);
                p.source = "the plugin's preset #" + juce::String(i + 1) + " of " + juce::String(n);
                presets.push_back(p);
            }
        }
        else if (n == 1)
        {
            singleProgramName = instance.getProgramName(0).trim();
        }
    }

    if (desc.pluginFormatName == "VST3")
    {
        classIdOut = getVst3ClassId(instance);

        if (settings.includePresetFiles && classIdOut.isNotEmpty())
        {
            auto index = Vst3PresetIndex::fromVar(parseJsonFile(presetIndexFile));
            auto it = index.byClassId.find(classIdOut);

            if (it != index.byClassId.end())
            {
                for (auto& path : it->second)
                {
                    Preset p;
                    p.kind = Preset::Kind::file;
                    p.file = juce::File(path);
                    p.key = "vstpreset:" + path;
                    p.name = presetNameFromFile(p.file, desc.name);
                    p.source = path;
                    presets.push_back(p);
                }
            }
        }
    }

    if (presets.empty())
    {
        Preset p;
        p.kind = Preset::Kind::current;
        p.key = "default";
        p.name = singleProgramName.isNotEmpty() && ! singleProgramName.equalsIgnoreCase("default")
                     ? singleProgramName : juce::String("Default");
        p.source = "the plugin's own settings right after loading (it offers no presets to choose from)";
        presets.push_back(p);
    }

    return presets;
}

// `recorded`: preset key -> file name of the recordings this plugin already has
// (from the manifest). A preset that has one keeps its file name; every other
// preset gets a name that no recording of this plugin uses, so a change in the
// preset list (a new .vstpreset, a toggled source, a plugin update) can never
// make one preset's recording be taken for - or overwritten by - another's.
static void assignFileNames(std::vector<Preset>& presets, const juce::String& baseName,
                            const std::map<juce::String, juce::String>& recorded)
{
    std::set<juce::String> taken;
    for (auto& [key, file] : recorded)
        taken.insert(file.toLowerCase());

    for (auto& p : presets)
        if (auto it = recorded.find(p.key); it != recorded.end())
            p.fileName = it->second;

    for (auto& p : presets)
    {
        if (p.fileName.isNotEmpty())
            continue;

        // Keep the whole name within ~150 characters: with a typical output
        // folder that stays under Windows' 260-character path limit (JUCE
        // doesn't use long-path syntax). Long preset names give way first.
        const int presetRoom = juce::jlimit(24, 100, 150 - baseName.length() - 3);
        const auto stem = baseName + " - " + sanitiseFileNamePart(p.name, presetRoom, "Preset");
        auto name = stem + ".flac";

        for (int n = 2; taken.count(name.toLowerCase()) > 0; ++n)
            name = stem + " (" + juce::String(n) + ").flac";

        taken.insert(name.toLowerCase());
        p.fileName = name;
    }
}

static void configureBuses(juce::AudioPluginInstance& p, bool instrument)
{
    // Ask for a stereo main output (and, for effects, a stereo main input);
    // leave everything else - sidechains, extra outputs - the way the plugin
    // set it up. Plugins that can't do stereo keep their own layout.
    auto layout = p.getBusesLayout();

    if (! layout.outputBuses.isEmpty())
        layout.outputBuses.getReference(0) = juce::AudioChannelSet::stereo();
    if (! instrument && ! layout.inputBuses.isEmpty())
        layout.inputBuses.getReference(0) = juce::AudioChannelSet::stereo();

    if (p.checkBusesLayoutSupported(layout))
        p.setBusesLayout(layout);
}

//==============================================================================
// Offline rendering of one preset.

struct RenderInput
{
    const juce::AudioBuffer<float>* audio = nullptr;     // effects: the song excerpt
    std::vector<std::pair<juce::int64, juce::MidiMessage>> midi; // instruments: sample-timed
    juce::int64 length = 0;                              // samples of input
};

struct RenderOutput
{
    juce::AudioBuffer<float> audio;
    int numChannels = 0;
    float peak = 0.0f;
    bool tailCut = false;
    bool invalidSamples = false;
    juce::String note;
};

// Fingerprint of a take at 16-bit resolution: plugins often fill unused
// program slots with the same init patch, and it's worth saying so rather than
// leaving the listener to discover 100 identical recordings.
static juce::uint64 fingerprint(const juce::AudioBuffer<float>& audio)
{
    juce::uint64 h = 1469598103934665603ull; // FNV-1a
    auto mix = [&h](juce::uint64 v) { h ^= v; h *= 1099511628211ull; };

    mix((juce::uint64) audio.getNumSamples());
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        const auto* d = audio.getReadPointer(ch);
        for (int i = 0; i < audio.getNumSamples(); ++i)
            mix((juce::uint32) (juce::int32) std::lrint(juce::jlimit(-1.0f, 1.0f, d[i]) * 32767.0f));
    }
    return h;
}

// Sent at the start of every preset's pre-roll: silence anything the previous
// preset left sounding and put the controllers back where a fresh plugin has
// them - a MIDI file can leave the sustain pedal down or the pitch bent, and
// All Notes Off alone doesn't lift the pedal.
static void resetMidiState(juce::MidiBuffer& midi)
{
    for (int ch = 1; ch <= 16; ++ch)
    {
        midi.addEvent(juce::MidiMessage::controllerEvent(ch, 64, 0), 0);   // sustain off
        midi.addEvent(juce::MidiMessage::controllerEvent(ch, 121, 0), 0);  // reset all controllers
        midi.addEvent(juce::MidiMessage::pitchWheel(ch, 8192), 0);         // centre pitch bend
        midi.addEvent(juce::MidiMessage::allNotesOff(ch), 0);
        midi.addEvent(juce::MidiMessage::allSoundOff(ch), 0);
    }
}

static bool renderPreset(juce::AudioPluginInstance& plugin, const RenderInput& input,
                         const RenderSettings& s, RenderPlayHead& playHead,
                         RenderOutput& result, juce::String& error, EventLogWriter& log)
{
    juce::ScopedNoDenormals noDenormals;

    const int bs = s.blockSize;
    const double rate = s.sampleRate;
    const int mainIn = plugin.getMainBusNumInputChannels();
    const int mainOut = plugin.getMainBusNumOutputChannels();

    if (mainOut <= 0)
    {
        error = "the plugin has no audio output";
        return false;
    }

    const int bufferChannels = juce::jmax(1, plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels());
    const int outChannels = juce::jmin(2, mainOut);

    juce::AudioBuffer<float> block(bufferChannels, bs);
    juce::MidiBuffer midi;

    // ---- Pre-roll: flush whatever the previous preset left ringing, and let
    // the plugin actually apply the new preset (a VST3 program change reaches
    // the audio side with the next processBlock; smoothing needs a moment).
    playHead.set(0, false);
    const auto preRoll = (juce::int64) (s.preRollSeconds * rate);
    for (juce::int64 pos = 0; pos < preRoll || pos == 0; pos += bs)
    {
        block.clear();
        midi.clear();
        if (pos == 0)
            resetMidiState(midi);
        plugin.processBlock(block, midi);
    }

    // Latency can depend on the preset, so read it now.
    const auto latency = (juce::int64) juce::jmax(0, plugin.getLatencySamples());
    const auto maxTail = (juce::int64) (s.maxTailSeconds * rate);
    const auto hardEnd = input.length + latency + maxTail;
    const float threshold = juce::Decibels::decibelsToGain((float) s.silenceThresholdDb);
    const auto quietNeeded = (juce::int64) (0.3 * rate);

    juce::AudioBuffer<float> recorded(outChannels, (int) (hardEnd + bs));
    recorded.clear();

    juce::int64 pos = 0;
    juce::int64 quietRun = 0;
    size_t nextMidi = 0;
    bool reachedSilence = false;
    auto lastHeartbeat = juce::Time::getMillisecondCounter();

    while (pos < hardEnd)
    {
        // A long take on a heavy plugin can outlast the preset timeout while
        // making perfectly good progress; say so every couple of seconds.
        if (juce::Time::getMillisecondCounter() - lastHeartbeat > 2000)
        {
            log.event("tick");
            lastHeartbeat = juce::Time::getMillisecondCounter();
        }

        block.clear();
        midi.clear();

        // Effect input: the excerpt on the main input bus (mono plugins get a mono mix).
        if (input.audio != nullptr && pos < input.length && mainIn > 0)
        {
            const int n = (int) juce::jmin<juce::int64>(bs, input.length - pos);
            const auto& src = *input.audio;

            if (mainIn == 1)
            {
                block.copyFrom(0, 0, src, 0, (int) pos, n);
                block.addFrom(0, 0, src, 1, (int) pos, n);
                block.applyGain(0, 0, n, 0.5f);
            }
            else
            {
                for (int ch = 0; ch < juce::jmin(mainIn, 2, block.getNumChannels()); ++ch)
                    block.copyFrom(ch, 0, src, ch, (int) pos, n);
            }
        }

        // Instrument input: the phrase's events that fall inside this block.
        while (nextMidi < input.midi.size() && input.midi[nextMidi].first < pos + bs)
        {
            const auto& [when, message] = input.midi[nextMidi++];
            midi.addEvent(message, (int) juce::jlimit<juce::int64>(0, bs - 1, when - pos));
        }

        playHead.set(pos, true);
        plugin.processBlock(block, midi);

        float blockPeak = 0.0f;
        for (int ch = 0; ch < outChannels; ++ch)
        {
            auto* data = block.getWritePointer(ch);
            for (int i = 0; i < bs; ++i)
            {
                if (! std::isfinite(data[i]))
                {
                    data[i] = 0.0f;
                    result.invalidSamples = true;
                }
                blockPeak = juce::jmax(blockPeak, std::abs(data[i]));
            }
            recorded.copyFrom(ch, (int) pos, block, ch, 0, bs);
        }

        pos += bs;

        // Once the input is over (and the plugin's latency has passed), stop as
        // soon as the output has been quiet for a moment.
        if (pos > input.length + latency)
        {
            quietRun = blockPeak < threshold ? quietRun + bs : 0;
            if (quietRun >= quietNeeded)
            {
                reachedSilence = true;
                break;
            }
        }
    }

    // Drop the plugin's latency from the start so every recording begins on
    // the first note / the first sample of the excerpt.
    const auto usable = juce::jmax<juce::int64>(0, pos - latency);

    // Trim trailing near-silence, keeping 100 ms, but never cut into the input span.
    const float trimThreshold = threshold * 0.25f;
    juce::int64 lastLoud = 0;
    for (int ch = 0; ch < outChannels; ++ch)
    {
        const auto* data = recorded.getReadPointer(ch, (int) latency);
        for (auto i = usable; --i >= 0;)
            if (std::abs(data[i]) > trimThreshold)
            {
                lastLoud = juce::jmax(lastLoud, i);
                break;
            }
    }

    const auto wanted = juce::jmax(input.length, lastLoud + (juce::int64) (0.1 * rate));
    const auto length = juce::jmax<juce::int64>(1, juce::jmin(usable, wanted));

    result.numChannels = outChannels;
    result.audio.setSize(outChannels, (int) length);
    for (int ch = 0; ch < outChannels; ++ch)
        result.audio.copyFrom(ch, 0, recorded, ch, (int) latency, (int) length);

    result.tailCut = ! reachedSilence && lastLoud + (juce::int64) (0.1 * rate) >= usable;
    if (result.tailCut)
    {
        const int fade = juce::jmin((int) length / 4, (int) (0.02 * rate));
        result.audio.applyGainRamp((int) length - fade, fade, 1.0f, 0.0f);
    }

    result.peak = result.audio.getMagnitude(0, (int) length);

    // FLAC is fixed-point: anything over full scale would clip. Turn the whole
    // recording down just enough instead, and say so.
    juce::StringArray notes;
    if (result.peak > 0.999f)
    {
        const float gain = 0.989f / result.peak;
        result.audio.applyGain(gain);
        notes.add("turned down " + juce::String(-juce::Decibels::gainToDecibels(gain), 1)
                  + " dB to avoid clipping (the plugin peaked at +"
                  + juce::String(juce::Decibels::gainToDecibels(result.peak), 1) + " dBFS)");
    }
    if (result.tailCut)
        notes.add("the sound was still ringing after the " + juce::String(s.maxTailSeconds, 1)
                  + " s tail limit, so the end is faded out");
    if (result.invalidSamples)
        notes.add("the plugin produced invalid (NaN/infinite) samples; they were replaced with silence");
    if (latency > 0)
        notes.add("plugin latency of " + juce::String(latency) + " samples removed");

    result.note = notes.joinIntoString("; ");
    return true;
}

//==============================================================================
static int runScanJob(const juce::var& job, EventLogWriter& log)
{
    const auto fileOrId = job["fileOrId"].toString();
    const auto resultFile = juce::File(job["resultFile"].toString());

    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager(fm);

    auto* format = findFormat(fm, job["format"].toString());
    if (format == nullptr)
    {
        log.event("error", { { "message", "unknown plugin format " + job["format"].toString() } });
        return workerBadJob;
    }

    loadExtraLv2Folders(*format, job["extraFolders"].toString());

    // Without module info, JUCE loads the plugin module, reads it and unloads it
    // again (the plugin's exit function and DLL unload code) before handing back
    // the results, so a plugin that misbehaves on unload would lose a perfectly
    // good scan. An extra reference of our own keeps the DLL from actually
    // unloading, and the worker then ends with TerminateProcess, which runs no
    // unload code at all. A bundle WITH Contents/Resources/moduleinfo.json is
    // described from that file without running any plugin code - pinning it
    // would load it for nothing (and copy-protected plugins then put up their
    // activation dialog in the middle of a scan).
   #if JUCE_WINDOWS
    juce::File binary;
    if (format->getName() == "VST3"
        && ! juce::File(fileOrId).getChildFile("Contents/Resources/moduleinfo.json").existsAsFile())
        binary = PluginFileInfo::inspect(fileOrId).binary;
   #endif

    juce::OwnedArray<juce::PluginDescription> found;
    onMessageThread([&]
    {
       #if JUCE_WINDOWS
        if (binary.existsAsFile())
            LoadLibraryW(binary.getFullPathName().toWideCharPointer()); // never freed, on purpose
       #endif
        format->findAllTypesForFile(found, fileOrId);
    });

    juce::XmlElement root("SCAN_RESULT");
    for (auto* d : found)
        if (auto xml = d->createXml())
            root.addChildElement(xml.release());

    if (! root.writeTo(resultFile))
    {
        log.event("error", { { "message", "can't write " + resultFile.getFullPathName() } });
        return workerFailed;
    }

    log.event("scanned", { { "count", found.size() } });
    return workerOk;
}

//==============================================================================
static int runRenderJob(const juce::var& job, EventLogWriter& log)
{
    const auto settings = RenderSettings::fromVar(job["settings"]);
    const bool instrument = (bool) job["instrument"];
    const auto baseName = job["baseName"].toString();
    const auto outputRoot = juce::File(job["outputRoot"].toString());
    const auto subfolder = job["subfolder"].toString();

    juce::StringArray skipKeys;
    if (auto* arr = job["skipKeys"].getArray())
        for (auto& k : *arr)
            skipKeys.add(k.toString());

    juce::PluginDescription desc;
    {
        auto xml = juce::parseXML(job["plugin"].toString());
        if (xml == nullptr || ! desc.loadFromXml(*xml))
        {
            log.event("error", { { "message", "the job's plugin description is unreadable" } });
            return workerBadJob;
        }
    }

    // ---- Inputs ---------------------------------------------------------------
    // Instruments play a phrase. Effects process the song excerpt - except an
    // "effect" with no audio input that takes MIDI (a generator filed under
    // Fx), which is played like an instrument; that's decided once it's loaded.
    PreviewPhrase phrase;
    {
        juce::String err;
        const auto midiFile = job["midiFile"].toString();

        if (midiFile.isNotEmpty())
        {
            if (! loadMidiFilePhrase(juce::File(midiFile), phrase, err))
            {
                log.event("error", { { "message", err } });
                return workerBadJob;
            }
        }
        else
        {
            const bool drums = looksLikeDrumInstrument(desc.category, desc.name);
            phrase = makeBuiltInPhrase(drums ? PhraseKind::drums : PhraseKind::melodic, settings.tempoBpm);
        }
    }

    juce::AudioBuffer<float> excerpt;
    if (! instrument)
    {
        double rate = 0.0;
        juce::String err;
        if (! readAudioFile(juce::File(job["effectInput"].toString()), excerpt, rate, err))
        {
            log.event("error", { { "message", "can't read the effect input: " + err } });
            return workerBadJob;
        }

        if (std::abs(rate - settings.sampleRate) > 0.5)
        {
            log.event("error", { { "message", "the effect input is at " + juce::String(rate) + " Hz but the recording is at "
                                              + juce::String(settings.sampleRate) + " Hz" } });
            return workerBadJob;
        }

        if (excerpt.getNumChannels() < 2)
        {
            excerpt.setSize(2, excerpt.getNumSamples(), true);
            excerpt.copyFrom(1, 0, excerpt, 0, 0, excerpt.getNumSamples());
        }
    }

    // ---- Load the plugin ----------------------------------------------------------
    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager(fm);

    if (auto* format = findFormat(fm, desc.pluginFormatName))
        loadExtraLv2Folders(*format, job["extraFolders"].toString());

    log.event("loading", { { "name", desc.name } });

    juce::String loadError;
    auto created = fm.createPluginInstance(desc, settings.sampleRate, settings.blockSize, loadError);

    if (created == nullptr)
    {
        log.event("error", { { "message", "the plugin could not be loaded"
                                          + (loadError.isNotEmpty() ? ": " + loadError : juce::String()) } });
        return workerLoadFailed;
    }

    // The plugin, and the transport it reads, are deliberately never deleted:
    // the process ends straight after the job (startWorker), and tearing a
    // plugin down - on the message thread, or worse during an exception unwind
    // on this one - is the last thing that could crash once the recordings are
    // safely on disk.
    auto* instance = created.release();
    auto* playHead = new RenderPlayHead(settings.tempoBpm, settings.sampleRate);

    onMessageThread([&]
    {
        configureBuses(*instance, instrument);
        instance->setPlayHead(playHead);
        instance->setNonRealtime(true);
        instance->prepareToPlay(settings.sampleRate, settings.blockSize);
    });

    if (instance->getMainBusNumOutputChannels() <= 0)
    {
        log.event("error", { { "message", "the plugin has no audio output, so there is nothing to record" } });
        return workerNoOutput;
    }

    RenderInput input;
    juce::String inputDescription;
    const bool playPhrase = instrument || (instance->getMainBusNumInputChannels() == 0 && instance->acceptsMidi());

    if (playPhrase)
    {
        for (int i = 0; i < phrase.events.getNumEvents(); ++i)
        {
            const auto& m = phrase.events.getEventPointer(i)->message;
            input.midi.emplace_back((juce::int64) std::llround(m.getTimeStamp() * settings.sampleRate), m);
        }

        input.length = juce::jmax<juce::int64>(1, (juce::int64) std::ceil(phrase.lengthSeconds * settings.sampleRate));
        inputDescription = phrase.description;
    }
    else
    {
        input.audio = &excerpt;
        input.length = excerpt.getNumSamples();
        inputDescription = instance->getMainBusNumInputChannels() > 0
                         ? juce::String("song excerpt")
                         : juce::String("nothing (the plugin takes neither audio nor MIDI; recorded as it sounds on its own)");
    }

    // ---- Presets -------------------------------------------------------------------
    // The recordings this plugin already has, per the manifest: key -> file name.
    std::map<juce::String, juce::String> recorded;
    if (auto* obj = job["recorded"].getDynamicObject())
        for (auto& prop : obj->getProperties())
            recorded[prop.name.toString()] = prop.value.toString().fromLastOccurrenceOf("/", false, false);

    std::vector<Preset> presets;
    juce::String classId;
    onMessageThread([&]
    {
        presets = enumeratePresets(*instance, desc, settings,
                                   juce::File(job["presetIndex"].toString()), classId);
    });
    assignFileNames(presets, baseName, recorded);

    const int limit = settings.maxPresetsPerPlugin > 0 ? settings.maxPresetsPerPlugin : (int) presets.size();

    {
        juce::Array<juce::var> list;
        for (auto& p : presets)
            list.add(makeObject({ { "key", p.key }, { "name", p.name }, { "source", p.source },
                                  { "file", subfolder + "/" + p.fileName } }));

        log.event("loaded", { { "inputs", instance->getMainBusNumInputChannels() },
                              { "outputs", instance->getMainBusNumOutputChannels() },
                              { "latency", instance->getLatencySamples() },
                              { "classId", classId },
                              { "input", inputDescription } });
        log.event("presets", { { "list", list }, { "limit", limit } });
    }

    // ---- Record ----------------------------------------------------------------------
    const auto folder = outputRoot.getChildFile(subfolder);
    folder.createDirectory();
    std::map<juce::uint64, juce::String> takesByFingerprint; // -> first preset that sounded like this

    for (int i = 0; i < (int) presets.size() && i < limit; ++i)
    {
        const auto& p = presets[(size_t) i];
        const auto relative = subfolder + "/" + p.fileName;
        const auto target = folder.getChildFile(p.fileName);

        if (skipKeys.contains(p.key))
            continue;

        // "Keep existing recordings" goes by the preset, not the file name: only
        // a preset the manifest says was recorded, and whose file is still there.
        if (settings.skipExisting && recorded.count(p.key) > 0 && target.existsAsFile())
        {
            log.event("skip", { { "key", p.key }, { "file", relative }, { "reason", "already recorded" } });
            continue;
        }

        log.event("begin", { { "key", p.key }, { "index", i }, { "count", juce::jmin(limit, (int) presets.size()) } });
        injectFaultIfRequested(p.key);

        // One misbehaving preset - an exception from the plugin, a failed write -
        // costs that preset only. (Crashes and hangs are the GUI's job: it
        // restarts the plugin without the preset.)
        try
        {
            // Apply the preset on the message thread (VST3 controllers expect it there).
            bool applied = true;
            onMessageThread([&]
            {
                switch (p.kind)
                {
                    case Preset::Kind::program: instance->setCurrentProgram(p.program); break;
                    case Preset::Kind::file:    applied = loadVst3PresetFile(*instance, p.file); break;
                    case Preset::Kind::current: break;
                }
                instance->reset();
            });

            if (! applied)
            {
                log.event("fail", { { "key", p.key }, { "reason", "the plugin refused this preset file" } });
                continue;
            }

            // Some plugins apply presets asynchronously (on their own threads or on
            // the message thread). Give them a moment; this thread just waits.
            if (settings.settleMs > 0)
                juce::Thread::sleep((int) settings.settleMs);

            RenderOutput out;
            juce::String error;
            if (! renderPreset(*instance, input, settings, *playHead, out, error, log))
            {
                log.event("fail", { { "key", p.key }, { "reason", error } });
                continue;
            }

            if (! writeAudioFile(target, out.audio, out.numChannels, settings.sampleRate, settings.bitDepth, error))
            {
                log.event("fail", { { "key", p.key }, { "reason", error } });
                continue;
            }

            const bool silent = out.peak < juce::Decibels::decibelsToGain(-90.0f);
            auto note = out.note;
            if (silent)
            {
                note = "no sound came out"
                     + juce::String(instrument ? " (the preset may need samples or content that isn't installed, or respond only to other notes)"
                                               : " (the plugin may be muted, in demo mode, or expect a sidechain)")
                     + (note.isNotEmpty() ? "; " + note : juce::String());
            }
            else
            {
                const auto fp = fingerprint(out.audio);
                if (auto it = takesByFingerprint.find(fp); it != takesByFingerprint.end())
                    note = "sounds identical to \"" + it->second + "\"" + (note.isNotEmpty() ? "; " + note : juce::String());
                else
                    takesByFingerprint[fp] = p.name;
            }

            log.event("done", { { "key", p.key },
                                { "file", relative },
                                { "status", silent ? "silent" : "ok" },
                                { "seconds", out.audio.getNumSamples() / settings.sampleRate },
                                { "peakDb", gainToDb(out.peak) },
                                { "note", note } });
        }
        catch (const std::exception& e)
        {
            log.event("fail", { { "key", p.key }, { "reason", juce::String("the plugin threw an exception: ") + e.what() } });
        }
        catch (...)
        {
            log.event("fail", { { "key", p.key }, { "reason", "the plugin threw an exception" } });
        }
    }

    return workerOk;
}

//==============================================================================
bool isWorkerCommandLine(const juce::String& commandLine, juce::File& jobFile)
{
    auto args = juce::StringArray::fromTokens(commandLine, true);
    const int i = args.indexOf("--worker");
    if (i < 0 || i + 1 >= args.size())
        return false;

    jobFile = juce::File(args[i + 1].unquoted());
    return true;
}

void startWorker(const juce::File& jobFile)
{
    std::thread([jobFile]
    {
        auto job = parseJsonFile(jobFile);
        auto log = std::make_unique<EventLogWriter>(juce::File(job["eventLog"].toString()));

        gCrashLog = log.get();
        installCrashHandlers();

        int code = workerBadJob;
        log->event("hello", { { "type", job["type"] } });

        try
        {
            if (! job.isObject())
                log->event("error", { { "message", "can't read the job file " + jobFile.getFullPathName() } });
            else if (job["type"].toString() == "scan")
                code = runScanJob(job, *log);
            else if (job["type"].toString() == "render")
                code = runRenderJob(job, *log);
            else
                log->event("error", { { "message", "unknown job type" } });
        }
        catch (const std::exception& e)
        {
            log->event("error", { { "message", juce::String("the plugin threw an exception: ") + e.what() } });
            code = workerFailed;
        }
        catch (...)
        {
            log->event("error", { { "message", "the plugin threw an unknown exception" } });
            code = workerFailed;
        }

        log->event("end", { { "code", code } });

        // Exit right here rather than shutting JUCE down: unloading some
        // plugins' DLLs crashes or hangs, and everything worth keeping is on
        // disk already.
        terminateWorker(code);
    }).detach();
}

} // namespace PresetRecorder

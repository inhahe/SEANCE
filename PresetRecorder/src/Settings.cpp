#include "Settings.h"
#include "Util.h"

namespace PresetRecorder {

//==============================================================================
juce::var RenderSettings::toVar() const
{
    return makeObject({
        { "sampleRate", sampleRate },
        { "bitDepth", bitDepth },
        { "blockSize", blockSize },
        { "tempoBpm", tempoBpm },
        { "maxTailSeconds", maxTailSeconds },
        { "silenceThresholdDb", silenceThresholdDb },
        { "settleMs", settleMs },
        { "preRollSeconds", preRollSeconds },
        { "maxPresetsPerPlugin", maxPresetsPerPlugin },
        { "skipExisting", skipExisting },
        { "includePrograms", includePrograms },
        { "includePresetFiles", includePresetFiles },
    });
}

RenderSettings RenderSettings::fromVar(const juce::var& v)
{
    RenderSettings s;
    auto get = [&v](const char* name, auto fallback)
    {
        auto p = v.getProperty(name, juce::var());
        return p.isVoid() ? fallback : static_cast<decltype(fallback)>(p);
    };

    s.sampleRate          = get("sampleRate", s.sampleRate);
    s.bitDepth            = get("bitDepth", s.bitDepth);
    s.blockSize           = get("blockSize", s.blockSize);
    s.tempoBpm            = get("tempoBpm", s.tempoBpm);
    s.maxTailSeconds      = get("maxTailSeconds", s.maxTailSeconds);
    s.silenceThresholdDb  = get("silenceThresholdDb", s.silenceThresholdDb);
    s.settleMs            = get("settleMs", s.settleMs);
    s.preRollSeconds      = get("preRollSeconds", s.preRollSeconds);
    s.maxPresetsPerPlugin = get("maxPresetsPerPlugin", s.maxPresetsPerPlugin);
    s.skipExisting        = get("skipExisting", s.skipExisting);
    s.includePrograms     = get("includePrograms", s.includePrograms);
    s.includePresetFiles  = get("includePresetFiles", s.includePresetFiles);

    // Guard against hand-edited nonsense.
    if (s.bitDepth != 16 && s.bitDepth != 24) s.bitDepth = 16;
    s.sampleRate = juce::jlimit(8000.0, 192000.0, s.sampleRate);
    s.blockSize = juce::jlimit(32, 4096, s.blockSize);
    s.tempoBpm = juce::jlimit(20.0, 400.0, s.tempoBpm);
    s.maxTailSeconds = juce::jlimit(0.0, 60.0, s.maxTailSeconds);
    s.maxPresetsPerPlugin = juce::jmax(0, s.maxPresetsPerPlugin);
    return s;
}

//==============================================================================
AppSettings::AppSettings()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "PresetRecorder";
    o.filenameSuffix = ".settings";
    o.folderName = "PresetRecorder";
    o.osxLibrarySubFolder = "Application Support";
    o.commonToAllUsers = false;
    o.millisecondsBeforeSaving = 1000;
    o.storageFormat = juce::PropertiesFile::storeAsXML;
    properties = std::make_unique<juce::PropertiesFile>(o);
}

juce::String AppSettings::getSeanceConfigOverride() const   { return properties->getValue("seanceConfig"); }
void AppSettings::setSeanceConfigOverride(const juce::String& s) { properties->setValue("seanceConfig", s); }

juce::File AppSettings::getOutputDir() const
{
    auto s = properties->getValue("outputDir");
    if (s.isNotEmpty() && juce::File::isAbsolutePath(s))
        return juce::File(s);

    return juce::File::getSpecialLocation(juce::File::userMusicDirectory)
               .getChildFile("Plugin Preset Recordings");
}
void AppSettings::setOutputDir(const juce::File& f) { properties->setValue("outputDir", f.getFullPathName()); }

juce::String AppSettings::getInputSongOverride() const       { return properties->getValue("inputSong"); }
void AppSettings::setInputSongOverride(const juce::String& s) { properties->setValue("inputSong", s); }
double AppSettings::getSongStart() const                     { return properties->getDoubleValue("songStart", defaultSongStart); }
void AppSettings::setSongStart(double v)                     { properties->setValue("songStart", v); }
double AppSettings::getSongLength() const                    { return properties->getDoubleValue("songLength", defaultSongLength); }
void AppSettings::setSongLength(double v)                    { properties->setValue("songLength", v); }

bool AppSettings::getUseMidiFile() const                     { return properties->getBoolValue("useMidiFile", false); }
void AppSettings::setUseMidiFile(bool b)                     { properties->setValue("useMidiFile", b); }
juce::String AppSettings::getMidiFile() const                { return properties->getValue("midiFile"); }
void AppSettings::setMidiFile(const juce::String& s)         { properties->setValue("midiFile", s); }

RenderSettings AppSettings::getRenderSettings() const
{
    juce::var v;
    if (juce::JSON::parse(properties->getValue("render"), v).wasOk() && v.isObject())
        return RenderSettings::fromVar(v);
    return {};
}
void AppSettings::setRenderSettings(const RenderSettings& r) { properties->setValue("render", juce::JSON::toString(r.toVar(), true)); }

int AppSettings::getParallelWorkers() const
{
    const int fallback = juce::jlimit(1, 4, juce::SystemStats::getNumCpus() / 4);
    return juce::jlimit(1, 16, properties->getIntValue("parallelWorkers", juce::jmax(2, fallback)));
}
void AppSettings::setParallelWorkers(int n)                  { properties->setValue("parallelWorkers", n); }
double AppSettings::getLoadTimeoutSeconds() const            { return properties->getDoubleValue("loadTimeout", 90.0); }
void AppSettings::setLoadTimeoutSeconds(double s)            { properties->setValue("loadTimeout", s); }
double AppSettings::getPresetTimeoutSeconds() const          { return properties->getDoubleValue("presetTimeout", 60.0); }
void AppSettings::setPresetTimeoutSeconds(double s)          { properties->setValue("presetTimeout", s); }

juce::StringArray AppSettings::getUnskipped() const
{
    juce::StringArray a;
    a.addLines(properties->getValue("unskipped"));
    a.removeEmptyStrings();
    return a;
}
void AppSettings::setUnskipped(const juce::StringArray& a)   { properties->setValue("unskipped", a.joinIntoString("\n")); }

double AppSettings::getPlaybackVolume() const                { return properties->getDoubleValue("volume", 0.8); }
void AppSettings::setPlaybackVolume(double v)                { properties->setValue("volume", v); }

//==============================================================================
const char* AppSettings::bundledSongFileName() { return "Small Note Boogaloo - Airmen of Note.mp3"; }

const char* AppSettings::bundledSongUrl()
{
    return "https://upload.wikimedia.org/wikipedia/commons/b/b2/"
           "Small_Note_Boogaloo_-_Airmen_of_Note_-_United_States_Air_Force_Band.mp3";
}

juce::File AppSettings::downloadedSongLocation()
{
    return getAppDataDir().getChildFile(bundledSongFileName());
}

juce::File AppSettings::findBundledSong()
{
    // 1. resources/ next to the exe (the build copies it there).
    auto f = getExeDir().getChildFile("resources").getChildFile(bundledSongFileName());
    if (f.existsAsFile())
        return f;

    // 2. The source tree: walk up from the exe looking for PresetRecorder/resources.
    for (auto dir = getExeDir(); dir.getParentDirectory() != dir; dir = dir.getParentDirectory())
    {
        auto candidate = dir.getChildFile("resources").getChildFile(bundledSongFileName());
        if (candidate.existsAsFile())
            return candidate;

        candidate = dir.getChildFile("PresetRecorder").getChildFile("resources").getChildFile(bundledSongFileName());
        if (candidate.existsAsFile())
            return candidate;
    }

    // 3. A previous in-app download.
    return downloadedSongLocation();
}

} // namespace PresetRecorder

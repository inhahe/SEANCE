#include "Manifest.h"
#include "Util.h"

namespace PresetRecorder {

PresetResult* PluginResult::findPreset(const juce::String& k)
{
    for (auto& p : presets)
        if (p.key == k)
            return &p;
    return nullptr;
}

int PluginResult::countWithAudio() const
{
    int n = 0;
    for (auto& p : presets)
        if (p.hasAudio())
            ++n;
    return n;
}

void Manifest::setRoot(const juce::File& outputFolder)
{
    if (root != outputFolder)
    {
        root = outputFolder;
        plugins.clear();
        dryInputFile = {};
        dirty = false;
    }
}

PluginResult* Manifest::find(const juce::String& id)
{
    for (auto& p : plugins)
        if (p.id == id)
            return &p;
    return nullptr;
}

PluginResult& Manifest::getOrCreate(const juce::String& id)
{
    if (auto* p = find(id))
        return *p;

    PluginResult r;
    r.id = id;
    plugins.push_back(r);
    dirty = true;
    return plugins.back();
}

bool Manifest::load()
{
    plugins.clear();
    dryInputFile = {};
    dirty = false;

    auto json = parseJsonFile(getFile());
    if (! json.isObject())
        return false;

    dryInputFile = json["dryInput"].toString();

    if (auto* arr = json["plugins"].getArray())
    {
        for (auto& pv : *arr)
        {
            PluginResult r;
            r.id       = pv["id"].toString();
            r.name     = pv["name"].toString();
            r.company  = pv["company"].toString();
            r.format   = pv["format"].toString();
            r.version  = pv["version"].toString();
            r.category = pv["category"].toString();
            r.path     = pv["path"].toString();
            r.kind     = pv["kind"].toString();
            r.baseName = pv["baseName"].toString();
            r.status   = pv["status"].toString();
            r.error    = pv["error"].toString();
            r.lastRun  = pv["lastRun"].toString();

            // An interrupted run leaves "recording" behind; it isn't any more.
            if (r.status == "recording")
                r.status = "partial";

            if (auto* presets = pv["presets"].getArray())
            {
                for (auto& qv : *presets)
                {
                    PresetResult p;
                    p.key        = qv["key"].toString();
                    p.name       = qv["name"].toString();
                    p.source     = qv["source"].toString();
                    p.file       = qv["file"].toString();
                    p.status     = qv["status"].toString();
                    p.note       = qv["note"].toString();
                    p.seconds    = (double) qv.getProperty("seconds", 0.0);
                    p.peakDb     = (double) qv.getProperty("peakDb", -200.0);
                    p.recordedAt = qv["recordedAt"].toString();
                    r.presets.push_back(p);
                }
            }

            if (r.id.isNotEmpty())
                plugins.push_back(r);
        }
    }

    return true;
}

bool Manifest::save()
{
    if (root == juce::File())
        return false;

    juce::Array<juce::var> pluginArray;

    for (auto& r : plugins)
    {
        juce::Array<juce::var> presetArray;
        for (auto& p : r.presets)
            presetArray.add(makeObject({
                { "key", p.key },
                { "name", p.name },
                { "source", p.source },
                { "file", p.file },
                { "status", p.status },
                { "note", p.note },
                { "seconds", p.seconds },
                { "peakDb", p.peakDb },
                { "recordedAt", p.recordedAt },
            }));

        pluginArray.add(makeObject({
            { "id", r.id },
            { "name", r.name },
            { "company", r.company },
            { "format", r.format },
            { "version", r.version },
            { "category", r.category },
            { "path", r.path },
            { "kind", r.kind },
            { "baseName", r.baseName },
            { "status", r.status },
            { "error", r.error },
            { "lastRun", r.lastRun },
            { "presets", presetArray },
        }));
    }

    auto json = makeObject({
        { "tool", "PresetRecorder" },
        { "version", JUCE_APPLICATION_VERSION_STRING },
        { "saved", nowString() },
        { "dryInput", dryInputFile },
        { "plugins", pluginArray },
    });

    root.createDirectory();
    const bool ok = writeJsonFile(getFile(), json);
    if (ok)
        dirty = false;
    return ok;
}

} // namespace PresetRecorder

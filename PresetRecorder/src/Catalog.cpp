#include "Catalog.h"
#include "Util.h"
#include "cpp/src/plugin_settings.h" // pluginSearchPath / userPluginFolders - SEANCE's own

namespace PresetRecorder {

static juce::File cacheFile() { return getAppDataDir().getChildFile("plugin_cache.xml"); }

Catalog::Catalog()
{
    juce::addDefaultFormatsToManager(formatManager);
}

std::vector<std::string> Catalog::seanceFolders() const
{
    std::vector<std::string> folders;
    for (auto& d : seance.scanDirs)
        folders.push_back(d.toStdString());
    return folders;
}

// Both are SEANCE's own functions (cpp/src/plugin_settings.h), so the tool
// searches exactly where SEANCE does - including leaving LV2 only the folders
// that hold an LV2 bundle, which keeps lilv's manifest.ttl errors out.
juce::FileSearchPath Catalog::getSearchPath(juce::AudioPluginFormat& format) const
{
    return SoundShop::pluginSearchPath(format, seanceFolders());
}

juce::FileSearchPath Catalog::getUserFolders(juce::AudioPluginFormat& format) const
{
    return SoundShop::userPluginFolders(format, seanceFolders());
}

PluginFile& Catalog::addOrGet(const juce::String& format, const juce::String& fileOrId)
{
    const auto key = identifierKey(fileOrId);

    for (auto& f : files)
        if (f.key == key)
            return f;

    PluginFile f;
    f.format = format;
    f.fileOrId = fileOrId;
    f.key = key;
    files.push_back(std::move(f));
    return files.back();
}

PluginFile* Catalog::findFile(const juce::String& fileOrId)
{
    const auto key = identifierKey(fileOrId);
    for (auto& f : files)
        if (f.key == key)
            return &f;
    return nullptr;
}

juce::String Catalog::guessFormat(const juce::String& fileOrId)
{
    for (auto* format : formatManager.getFormats())
        if (format->fileMightContainThisPluginType(fileOrId))
            return format->getName();

    return fileOrId.endsWithIgnoreCase(".dll") ? "VST" : "Unknown";
}

juce::int64 Catalog::modTimeOf(const juce::String& fileOrId)
{
    if (! juce::File::isAbsolutePath(fileOrId))
        return 0;

    const juce::File f(fileOrId);
    auto t = f.getLastModificationTime().toMilliseconds();

    // A bundle folder's own date often survives an update that replaced the
    // binary inside it, so look at the binary (and module info) too.
    if (f.isDirectory())
    {
        const auto stem = f.getFileNameWithoutExtension();
        for (auto rel : { juce::String("Contents/x86_64-win/") + stem + ".vst3",
                          juce::String("Contents/x86-win/") + stem + ".vst3",
                          juce::String("Contents/arm64x-win/") + stem + ".vst3",
                          juce::String("Contents/MacOS/") + stem,
                          juce::String("Contents/x86_64-linux/") + stem + ".so",
                          juce::String("Contents/Resources/moduleinfo.json") })
        {
            auto child = f.getChildFile(rel);
            if (child.existsAsFile())
                t = juce::jmax(t, child.getLastModificationTime().toMilliseconds());
        }
    }

    return t;
}

bool Catalog::isFresh(const CacheEntry& e, const juce::String& fileOrId) const
{
    if (! juce::File::isAbsolutePath(fileOrId))
        return true; // URIs (LV2) have no date; "Rescan all" refreshes them

    return juce::File(fileOrId).exists() && e.modTime == modTimeOf(fileOrId);
}

std::vector<Catalog::ScanRequest> Catalog::enumerate(bool forceRescan)
{
    files.clear();

    // New formats for every enumeration. lilv (JUCE's LV2 library) reads the
    // standard LV2 folders when an LV2 format is created, and only notices a
    // bundle installed after that by reading its folder again - which logs a
    // "Reloading plugin" warning for every plugin it had already read. A new
    // format reads each folder once: the standard ones as it's created,
    // SoundShop2's when searched (its search returns every plugin it has read).
    // The other formats keep no such state and get the whole search path.
    juce::AudioPluginFormatManager fresh;
    juce::addDefaultFormatsToManager(fresh);

    for (auto* format : fresh.getFormats())
    {
        const auto path = format->getName() == "LV2" ? getUserFolders(*format) : getSearchPath(*format);
        auto ids = format->searchPathsForPlugins(path, true, false);
        ids.sortNatural();

        for (auto& id : ids)
            addOrGet(format->getName(), id).inScanFolders = true;
    }

    // Skip-list entries are listed even when they aren't in the scan folders
    // any more (moved, uninstalled): the skip-list tab should show all of them.
    for (auto& s : seance.skipList)
        addOrGet(guessFormat(s), s).onSkipList = true;

    std::vector<ScanRequest> todo;

    for (auto& f : files)
    {
        f.types.clear();
        f.error = {};

        auto it = cache.find(f.key);
        const bool haveFresh = it != cache.end() && isFresh(it->second, f.fileOrId);

        if (isSkipped(f))
        {
            f.state = PluginFile::State::skipped;
            // Keep names from an earlier (unskipped) scan for the skip-list view.
            if (haveFresh)
                f.types = it->second.types;
            continue;
        }

        if (juce::File::isAbsolutePath(f.fileOrId) && ! juce::File(f.fileOrId).exists())
        {
            f.state = PluginFile::State::scanFailed;
            f.error = "file not found";
            continue;
        }

        if (! forceRescan && haveFresh)
        {
            if (it->second.failure.isNotEmpty())
            {
                f.state = PluginFile::State::scanFailed;
                f.error = it->second.failure;
            }
            else
            {
                f.state = PluginFile::State::scanned;
                f.types = it->second.types;
            }
            continue;
        }

        f.state = PluginFile::State::pending;
        todo.push_back({ f.format, f.fileOrId });
    }

    return todo;
}

void Catalog::setScanning(const juce::String& fileOrId)
{
    if (auto* f = findFile(fileOrId))
        f->state = PluginFile::State::scanning;
}

void Catalog::applyScanResult(const juce::String& fileOrId, const juce::Array<juce::PluginDescription>& types)
{
    auto* f = findFile(fileOrId);
    if (f == nullptr)
        return;

    f->types = types;
    f->error = {};
    f->state = types.isEmpty() ? PluginFile::State::scanFailed : PluginFile::State::scanned;

    if (types.isEmpty())
    {
        // Say why when we can tell: most often it's a 32-bit plugin, which a
        // 64-bit host can't load at all (JUCE just reports nothing inside).
        const auto info = PluginFileInfo::inspect(f->fileOrId);
        if (! info.loadableArchitecture)
            f->error = info.architecture + " plugin - a 64-bit program can't load it";
        else
            f->error = "no loadable plugin found in this file (damaged, missing a DLL it needs, or refused to load)";
    }

    CacheEntry e;
    e.format = f->format;
    e.fileOrId = f->fileOrId;
    e.modTime = modTimeOf(f->fileOrId);
    e.types = types;
    e.failure = f->error;
    cache[f->key] = e;
}

void Catalog::applyScanFailure(const juce::String& fileOrId, const juce::String& reason)
{
    auto* f = findFile(fileOrId);
    if (f == nullptr)
        return;

    f->types.clear();
    f->state = PluginFile::State::scanFailed;
    f->error = reason;
    cache.erase(f->key);
}

std::vector<PluginEntry> Catalog::getRecordablePlugins() const
{
    std::vector<PluginEntry> out;

    for (auto& f : files)
    {
        if (f.state != PluginFile::State::scanned || isSkipped(f))
            continue;

        for (auto& t : f.types)
        {
            PluginEntry e;
            e.desc = t;
            e.id = t.createIdentifierString();
            e.instrument = t.isInstrument;
            e.fileKey = f.key;
            out.push_back(e);
        }
    }

    std::sort(out.begin(), out.end(), [](const PluginEntry& a, const PluginEntry& b)
    {
        auto cmp = a.desc.manufacturerName.compareIgnoreCase(b.desc.manufacturerName);
        if (cmp == 0) cmp = a.desc.name.compareIgnoreCase(b.desc.name);
        if (cmp == 0) cmp = a.desc.pluginFormatName.compare(b.desc.pluginFormatName);
        if (cmp == 0) cmp = a.desc.fileOrIdentifier.compare(b.desc.fileOrIdentifier);
        if (cmp == 0) cmp = a.id.compare(b.id);
        return cmp < 0;
    });

    // Base names: "<Company> - <Plugin>". When two plugins share one (the same
    // plugin as VST3 and LV2, or two installs of it), the format is added, and
    // if that still clashes, a counter. Lower-cased comparison because Windows
    // file names are case-insensitive.
    std::map<juce::String, int> uses;
    auto plainName = [](const PluginEntry& e)
    {
        // Short enough to leave room for the preset name under Windows' 260-
        // character path limit (see assignFileNames in Worker.cpp). A " - "
        // inside the company or plugin name becomes "-": the first two " - " in
        // a file name must be the separators, or "A - Verb" + preset "Plate - X"
        // and "A - Verb - Plate" + preset "X" would both be "A - Verb - Plate - X".
        auto part = [](const juce::String& text, int maxChars, const char* fallback)
        {
            return sanitiseFileNamePart(text, maxChars, fallback).replace(" - ", "-");
        };
        return part(e.desc.manufacturerName, 40, "Unknown") + " - " + part(e.desc.name, 60, "Plugin");
    };

    for (auto& e : out)
        ++uses[plainName(e).toLowerCase()];

    std::map<juce::String, int> taken;
    for (auto& e : out)
    {
        auto name = plainName(e);
        if (uses[name.toLowerCase()] > 1)
            name << " [" << sanitiseFileNamePart(e.desc.pluginFormatName, 12, "?") << "]";

        auto base = name;
        for (int n = 2; taken.count(name.toLowerCase()) > 0; ++n)
            name = base + " (" + juce::String(n) + ")";

        taken[name.toLowerCase()] = 1;
        e.baseName = name;
    }

    return out;
}

std::vector<PluginFile*> Catalog::getSkipListEntries()
{
    std::vector<PluginFile*> out;
    for (auto& f : files)
        if (f.onSkipList)
            out.push_back(&f);
    return out;
}

SkipEntryDetails Catalog::describeSkipEntry(PluginFile& f)
{
    if (! f.fileInfoLoaded)
    {
        f.fileInfo = PluginFileInfo::inspect(f.fileOrId);
        f.fileInfoLoaded = true;
    }

    SkipEntryDetails d;

    auto takeTypes = [&d](const juce::Array<juce::PluginDescription>& types, const juce::String& source)
    {
        for (auto& t : types)
            d.names.addIfNotAlreadyThere(t.name);
        if (d.company.isEmpty() && ! types.isEmpty() && types.getFirst().manufacturerName.isNotEmpty())
        {
            d.company = types.getFirst().manufacturerName;
            d.companySource = source;
        }
    };

    // 1. Our own scan of it (it was unskipped at some point).
    takeTypes(f.types, "a scan of this file by PresetRecorder");

    // 2. SoundShop2's scan cache, if SoundShop2 once scanned it successfully.
    {
        juce::Array<juce::PluginDescription> fromSeance;
        for (auto& t : seance.cachedTypes)
            if (identifierKey(t.fileOrIdentifier) == f.key)
                fromSeance.add(t);
        takeTypes(fromSeance, "SoundShop2's plugin scan cache");
    }

    // 3. The file itself: module info, version resource, folder name.
    if (d.company.isEmpty() && f.fileInfo.company.isNotEmpty())
    {
        d.company = f.fileInfo.company;
        d.companySource = f.fileInfo.companySource;
    }
    for (auto& n : f.fileInfo.pluginNames)
        d.names.addIfNotAlreadyThere(n);

    // 4. Another install of the same plugin: the 32-bit "Podolski.vst3" SEANCE
    //    skipped is named like the 64-bit "Podolski(x64).vst3" it can load.
    const auto stem = PluginFileInfo::displayStem(f.fileOrId);

    if (d.company.isEmpty())
    {
        auto matchIn = [&](const juce::Array<juce::PluginDescription>& types, const juce::String& where) -> bool
        {
            for (auto& t : types)
            {
                if (identifierKey(t.fileOrIdentifier) == f.key || t.manufacturerName.isEmpty())
                    continue;

                if (t.name.equalsIgnoreCase(stem)
                     || PluginFileInfo::displayStem(t.fileOrIdentifier).equalsIgnoreCase(stem))
                {
                    d.company = t.manufacturerName;
                    d.companySource = "another install of the same plugin (" + t.fileOrIdentifier + ", from "
                                    + where + ")";
                    return true;
                }
            }
            return false;
        };

        bool matched = false;
        for (auto& other : files)
            if (! matched && &other != &f)
                matched = matchIn(other.types, "PresetRecorder's scan");

        if (! matched)
            matchIn(seance.cachedTypes, "SoundShop2's plugin scan cache");
    }

    if (d.company.isEmpty())
    {
        d.company = "Unknown company";
        d.companySource = "nothing - the plugin was never scanned successfully and its file carries no vendor information";
    }

    d.displayName = d.names.isEmpty() ? stem : d.names.joinIntoString(", ");
    return d;
}

//==============================================================================
void Catalog::loadCache()
{
    cache.clear();

    auto xml = juce::parseXML(cacheFile());
    if (xml == nullptr || ! xml->hasTagName("PRESETRECORDER_PLUGIN_CACHE"))
        return;

    for (auto* fileEl : xml->getChildWithTagNameIterator("FILE"))
    {
        CacheEntry e;
        e.format = fileEl->getStringAttribute("format");
        e.fileOrId = fileEl->getStringAttribute("id");
        e.modTime = fileEl->getStringAttribute("modTime").getLargeIntValue();
        e.failure = fileEl->getStringAttribute("failure");

        for (auto* p : fileEl->getChildIterator())
        {
            juce::PluginDescription d;
            if (d.loadFromXml(*p))
                e.types.add(d);
        }

        if (e.fileOrId.isNotEmpty())
            cache[identifierKey(e.fileOrId)] = e;
    }
}

void Catalog::saveCache() const
{
    juce::XmlElement root("PRESETRECORDER_PLUGIN_CACHE");
    root.setAttribute("version", 1);

    for (auto& [key, e] : cache)
    {
        auto* el = root.createNewChildElement("FILE");
        el->setAttribute("format", e.format);
        el->setAttribute("id", e.fileOrId);
        el->setAttribute("modTime", juce::String(e.modTime));
        if (e.failure.isNotEmpty())
            el->setAttribute("failure", e.failure);

        for (auto& t : e.types)
            if (auto child = t.createXml())
                el->addChildElement(child.release());
    }

    root.writeTo(cacheFile());
}

void Catalog::clearCache()
{
    cache.clear();
    cacheFile().deleteFile();
}

} // namespace PresetRecorder

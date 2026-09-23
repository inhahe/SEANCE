#include "SeanceConfig.h"
#include "Util.h"
#include "cpp/src/plugin_settings.h" // SEANCE's own parser for soundshop_plugins.cfg

namespace PresetRecorder {

static constexpr const char* kConfigName = "soundshop_plugins.cfg";
static constexpr const char* kCacheName  = "soundshop_plugins_cache.dat";

juce::Array<juce::File> SeanceConfig::findCandidates()
{
    juce::Array<juce::File> dirs;
    auto addDir = [&dirs](const juce::File& d)
    {
        if (d.isDirectory() && ! dirs.contains(d))
            dirs.add(d);
    };

    // The tool is built inside SoundShop2 (SoundShop2/PresetRecorder/build/...),
    // so one of the exe's ancestors is the SoundShop2 folder - where seance.bat
    // starts SEANCE, and so where SEANCE writes its .cfg. SEANCE started by
    // double-clicking its exe writes next to the exe instead.
    for (auto dir = getExeDir();; dir = dir.getParentDirectory())
    {
        addDir(dir);
        addDir(dir.getChildFile("cpp/build/SEANCE_artefacts/Release"));
        addDir(dir.getChildFile("cpp/build/SEANCE_artefacts/Debug"));

        if (dir.getParentDirectory() == dir)
            break;
    }

    addDir(juce::File::getCurrentWorkingDirectory());

    juce::Array<juce::File> found;
    for (auto& d : dirs)
    {
        auto f = d.getChildFile(kConfigName);
        if (f.existsAsFile())
            found.add(f);
    }

    std::stable_sort(found.begin(), found.end(), [](const juce::File& a, const juce::File& b)
    {
        return a.getLastModificationTime() > b.getLastModificationTime();
    });

    return found;
}

// PluginSettings::load takes a narrow std::string path, which on Windows means
// the ANSI code page. Give it an ASCII-only path to be safe.
static std::string loadablePath(const juce::File& cfg)
{
    auto path = cfg.getFullPathName();

    if (path.containsOnly(" !#$%&'()+,-.0123456789;=@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]^_`"
                          "abcdefghijklmnopqrstuvwxyz{}~\\/:"))
        return path.toStdString();

    auto copy = getSessionScratchDir().getChildFile("soundshop_plugins_copy.cfg");
    cfg.copyFileTo(copy);
    return copy.getFullPathName().toStdString();
}

SeanceConfig SeanceConfig::load(const juce::File& explicitConfig)
{
    SeanceConfig c;

    if (explicitConfig != juce::File())
    {
        c.configFile = explicitConfig;
    }
    else
    {
        auto candidates = findCandidates();
        if (! candidates.isEmpty())
        {
            c.configFile = candidates.getFirst();
            c.autoDetected = true;
        }
    }

    SoundShop::PluginSettings settings; // constructor fills in SEANCE's default folders

    if (c.configFile.existsAsFile())
    {
        settings.load(loadablePath(c.configFile));
        c.configFound = true;
    }

    for (auto& d : settings.scanDirs)
        c.scanDirs.addIfNotAlreadyThere(juce::String::fromUTF8(d.c_str()));

    for (auto& b : settings.blockedPlugins)
        c.skipList.addIfNotAlreadyThere(juce::String::fromUTF8(b.c_str()));

    // SEANCE's scan cache: a line-based header, then "JUCE_XML", a
    // KnownPluginList XML document, and "END_XML".
    if (c.configFile != juce::File())
    {
        c.cacheFile = c.configFile.getSiblingFile(kCacheName);

        if (c.cacheFile.existsAsFile())
        {
            auto text = c.cacheFile.loadFileAsString();
            auto start = text.indexOf("JUCE_XML");
            auto end = text.lastIndexOf("END_XML");

            if (start >= 0 && end > start)
            {
                if (auto xml = juce::parseXML(text.substring(start + 8, end).trim()))
                {
                    juce::KnownPluginList list;
                    list.recreateFromXml(*xml);
                    for (auto& t : list.getTypes())
                        c.cachedTypes.add(t);
                }
            }
        }
    }

    return c;
}

juce::String SeanceConfig::summary() const
{
    juce::String s;

    if (configFound)
        s << juce::String(scanDirs.size()) << " scan folder" << (scanDirs.size() == 1 ? "" : "s")
          << ", " << juce::String(skipList.size()) << " entr" << (skipList.size() == 1 ? "y" : "ies")
          << " on SoundShop2's skip list";
    else if (configFile != juce::File())
        s << "File not found - using SoundShop2's default scan folders and an empty skip list";
    else
        s << "No soundshop_plugins.cfg found - using SoundShop2's default scan folders and an empty skip list";

    return s;
}

} // namespace PresetRecorder

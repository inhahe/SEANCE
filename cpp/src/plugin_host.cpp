#include "plugin_host.h"
#include "plugin_settings.h"   // pluginSearchPath, userPluginFolders
#include <juce_audio_processors/juce_audio_processors.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace SoundShop {

PluginHost::PluginHost() {
    juce::addDefaultFormatsToManager(formatManager);
}

PluginHost::~PluginHost() = default;

void PluginHost::registerPluginFolders(const std::vector<std::string>& dirs) {
    // Only LV2 needs this. VST3 (and LADSPA) plugins are loaded straight from
    // the file path stored in their description, and AU from the system
    // registry, but an LV2 plugin is identified by URI and JUCE resolves that
    // URI only among the bundles its LV2 world has loaded - at construction,
    // just the standard LV2 folders. Searching the user's other folders loads
    // their bundles as well (lilv reads the manifests; no plugin code runs),
    // which is all this needs - not the list of URIs the search returns.
    // userPluginFolders leaves the standard folders out, since loading a
    // folder a second time logs a warning for every plugin in it.
    for (auto* format : formatManager.getFormats()) {
        if (format->getName() != "LV2") continue;
        const auto folders = userPluginFolders(*format, dirs);
        if (folders.getNumPaths() > 0)
            format->searchPathsForPlugins(folders, true, false);
    }
}

PluginInfo PluginHost::makeInfo(const juce::PluginDescription& desc) {
    PluginInfo info;
    info.name = desc.name.toStdString();
    info.manufacturer = desc.manufacturerName.toStdString();
    info.format = desc.pluginFormatName.toStdString();
    info.fileOrId = desc.fileOrIdentifier.toStdString();
    info.category = desc.category.toStdString();
    info.version = desc.version.toStdString();
    info.descriptiveName = desc.descriptiveName.toStdString();
    info.uniqueId = desc.fileOrIdentifier.toStdString(); // use file as unique ID
    info.isInstrument = desc.isInstrument;
    info.hasAudioInput = desc.numInputChannels > 0;
    info.hasAudioOutput = desc.numOutputChannels > 0;
    info.numAudioInputChannels = desc.numInputChannels;
    info.numAudioOutputChannels = desc.numOutputChannels;
    // A JUCE description doesn't say whether a plugin takes or sends MIDI.
    // Instruments always take it; a plugin in a shared container (a VST3 file
    // holding several) might - an approximation. Output is never assumed.
    info.hasMidiInput = desc.isInstrument || desc.hasSharedContainer;
    info.hasMidiOutput = false;
    info.numMidiInputPorts = info.hasMidiInput ? 1 : 0;
    info.numMidiOutputPorts = info.hasMidiOutput ? 1 : 0;
    info.description = desc;
    return info;
}

void PluginHost::updateAvailable() {
    availablePlugins.clear();
    for (auto& p : plugins)
        if (!blocked.count(p.fileOrId))
            availablePlugins.push_back(p);
}

void PluginHost::setBlockedPlugins(const std::set<std::string>& b) {
    blocked = b;
    updateAvailable();
}

bool PluginHost::isBlocked(const juce::PluginDescription& desc) const {
    return blocked.count(desc.fileOrIdentifier.toStdString()) > 0;
}

const PluginInfo* PluginHost::findDescribed(const std::string& fileOrId) const {
    for (auto& p : plugins)
        if (p.fileOrId == fileOrId)
            return &p;
    return nullptr;
}

juce::AudioPluginFormat* PluginHost::findFormat(const juce::String& name) const {
    for (auto* format : formatManager.getFormats())
        if (format->getName() == name)
            return format;
    return nullptr;
}

std::string PluginHost::describeForUser(const juce::PluginDescription& desc) {
    juce::String s;
    s << "'" << (desc.name.isNotEmpty() ? desc.name : desc.fileOrIdentifier) << "' (";
    if (desc.manufacturerName.isNotEmpty())
        s << desc.manufacturerName << ", ";
    s << (desc.pluginFormatName.isNotEmpty() ? desc.pluginFormatName : juce::String("plugin")) << ")";
    return s.toStdString();
}

void PluginHost::scanForPlugins(const std::vector<std::string>& dirs,
                                const std::set<std::string>& blockedList,
                                const juce::File& deadMansPedal) {
    // Search the user-configured dirs AND each format's own default install
    // locations. Merging both matters for formats like LV2 whose standard
    // paths (e.g. the Windows/Linux LV2 dirs) aren't in the user list - a
    // VST3-only user dir list would otherwise hide every LV2 plugin. (LV2
    // only gets the user dirs that hold an LV2 bundle; see userPluginFolders.)
    scanFolders([&dirs](juce::AudioPluginFormat& format) { return pluginSearchPath(format, dirs); },
                blockedList, deadMansPedal);
}

void PluginHost::scanFolders(const SearchPathFn& pathFor,
                             const std::set<std::string>& blockedList,
                             const juce::File& deadMansPedal) {
    scanning = true;
    // This scan's failures only: a plugin unblocked since an earlier scan
    // that now loads must not be blocked again by that scan's result.
    failedPlugins.clear();
    blocked = blockedList;

    // The skip list goes in as JUCE's blacklist, which the scanner checks
    // before loading anything (KnownPluginList::scanAndAddFile) - so a
    // blocked plugin is never loaded, rather than loaded and then hidden.
    juce::KnownPluginList list;
    for (auto& b : blocked)
        list.addToBlacklist(juce::String::fromUTF8(b.c_str()));

    // Scan every format the build registered. addDefaultFormatsToManager only
    // adds formats enabled via the JUCE_PLUGINHOST_* compile flags (VST3 + LV2
    // everywhere, AU on macOS, LADSPA on Linux), so iterating all of them is
    // exactly the supported set - no need to hardcode a name allow-list.
    for (auto* format : formatManager.getFormats()) {
        fprintf(stderr, "Scanning %s plugins...\n", format->getName().toRawUTF8());

        juce::PluginDirectoryScanner scanner(list, *format, pathFor(*format), true, deadMansPedal);
        juce::String pluginName;
        while (scanner.scanNextFile(true, pluginName))
            fprintf(stderr, "  Found: %s\n", pluginName.toRawUTF8());

        for (auto& f : scanner.getFailedFiles()) {
            fprintf(stderr, "  Skipped (incompatible/32-bit?): %s\n", f.toRawUTF8());
            failedPlugins.insert(f.toStdString());
        }
    }

    // Anything else on the blacklist now came from the dead man's pedal:
    // SEANCE died loading it during an earlier scan, and the scanner skipped
    // it this time. Report it for blocking and clear the pedal, whose record
    // has now been acted on - left there, it would blacklist the plugin on
    // every scan, and unblocking it could never work.
    for (auto& f : list.getBlacklistedFiles()) {
        if (blocked.count(f.toStdString())) continue;
        fprintf(stderr, "  Skipped (SEANCE crashed loading it during an earlier scan): %s\n",
                f.toRawUTF8());
        failedPlugins.insert(f.toStdString());
    }
    if (deadMansPedal != juce::File())
        deadMansPedal.deleteFile();

    // Name order for the menus. JUCE's list is the reverse of the order the
    // files were found in, which means nothing to anyone - and nothing
    // depends on the order any more: rows are never saved, descriptions are.
    plugins.clear();
    for (auto& desc : list.getTypes())
        plugins.push_back(makeInfo(desc));
    std::stable_sort(plugins.begin(), plugins.end(), [](const PluginInfo& a, const PluginInfo& b) {
        const int byName = a.description.name.compareNatural(b.description.name);
        return byName != 0 ? byName < 0 : a.format < b.format;
    });
    updateAvailable();

    fprintf(stderr, "Scan complete: %d plugins found\n", (int)availablePlugins.size());
    scanning = false;
}

PluginHost::Resolution PluginHost::resolvePlugin(const juce::PluginDescription& saved) const {
    Resolution r;
    auto use = [&r](const juce::PluginDescription& d) {
        r.found = true;
        r.description = d;
        return r;
    };

    // 1. Exactly the saved plugin: same format, file (or LV2 URI) and ids.
    //    (JUCE's isDuplicateOf ignores the format.)
    for (auto& p : availablePlugins)
        if (p.description.pluginFormatName == saved.pluginFormatName
            && p.description.isDuplicateOf(saved))
            return use(p.description);

    // 2. The same plugin somewhere else - moved, reinstalled or updated: the
    //    format's own id (a VST3's class id) and the name still match.
    if (saved.uniqueId != 0)
        for (auto& p : availablePlugins)
            if (p.description.pluginFormatName == saved.pluginFormatName
                && p.description.uniqueId == saved.uniqueId
                && p.description.name == saved.name)
                return use(p.description);

    // 3. Same format, name and maker: an update that changed the id.
    for (auto& p : availablePlugins)
        if (p.description.pluginFormatName == saved.pluginFormatName
            && p.description.name == saved.name
            && p.description.manufacturerName == saved.manufacturerName)
            return use(p.description);

    // Not in the list.
    const auto who = describeForUser(saved);
    if (isBlocked(saved)) {
        r.problem = who + " is blocked in Plugin Settings, so SEANCE won't load it (it failed "
                    "or crashed when it was scanned, or it was blocked there by hand). Unblock "
                    "it in Plugin Settings to use it.";
        return r;
    }
    auto* format = findFormat(saved.pluginFormatName);
    if (format == nullptr) {
        r.problem = who + " is a " + saved.pluginFormatName.toStdString()
                    + " plugin, which SEANCE can't host on this computer.";
        return r;
    }
    // 4. Not scanned here - a project from another computer, or no scan yet -
    //    but installed where the project says: load it from there.
    if (format->doesPluginStillExist(saved))
        return use(saved);

    r.problem = who + " isn't in the plugin list, and isn't installed where this project last "
                "found it (" + saved.fileOrIdentifier.toStdString() + "). If it's installed "
                "somewhere else, add that folder in Plugin Settings and click Scan Now.";
    return r;
}

// A plugin node is created with its plugin's name, and NodeGraph::addNode
// numbers repeats of a name: "Serum", "Serum 2", "Serum 3".
static bool isNodeNameFor(const std::string& nodeName, const std::string& pluginName) {
    if (nodeName == pluginName) return true;
    const size_t n = pluginName.size();
    if (pluginName.empty() || nodeName.size() <= n + 1
        || nodeName.compare(0, n, pluginName) != 0 || nodeName[n] != ' ')
        return false;
    return std::all_of(nodeName.begin() + (std::ptrdiff_t)(n + 1), nodeName.end(),
                       [](char c) { return c >= '0' && c <= '9'; });
}

PluginHost::Resolution PluginHost::resolveLegacyIndex(int index, const std::string& nodeName) const {
    Resolution r;
    const PluginInfo* atRow = (index >= 0 && index < (int)availablePlugins.size())
                                  ? &availablePlugins[(size_t)index] : nullptr;

    // The node's name is the best evidence of which plugin it was - better
    // than the row, which the old code didn't even load reliably. An exact
    // name beats a numbered one ("Serum 2" is the plugin "Serum 2" if there
    // is one); several plugins of that name (one plugin in two formats): the
    // one at the saved row, else the first.
    const PluginInfo* named = nullptr;
    for (int pass = 0; pass < 2 && named == nullptr; ++pass) {
        for (auto& p : availablePlugins) {
            const bool match = pass == 0 ? p.name == nodeName : isNodeNameFor(nodeName, p.name);
            if (!match) continue;
            if (named == nullptr || &p == atRow) named = &p;
        }
    }
    if (named) {
        r.found = true;
        r.description = named->description;
        return r;
    }

    // No plugin of that name - the node was renamed, or the plugin is gone.
    // The row: it indexed the plugin list as shown, which is this list's
    // order until the next scan.
    if (atRow) {
        r.found = true;
        r.description = atRow->description;
        return r;
    }
    r.problem = "This node's plugin can't be identified. The project was saved by an older "
                "SEANCE, which recorded only the plugin's position in the plugin list (number "
                + std::to_string(index + 1) + "), and no plugin called '" + nodeName
                + "' or at that position is in the list now. Add the plugin to the graph again.";
    return r;
}

std::unique_ptr<PluginHost::LoadedPlugin> PluginHost::loadPlugin(
        const juce::PluginDescription& desc, double sampleRate, int blockSize,
        std::string* error) {
    if (isBlocked(desc)) {
        if (error) *error = describeForUser(desc) + " is blocked in Plugin Settings.";
        return nullptr;
    }

    juce::String errorMsg;
    auto instance = formatManager.createPluginInstance(desc, sampleRate, blockSize, errorMsg);

    if (!instance) {
        fprintf(stderr, "Failed to load plugin '%s': %s\n",
                desc.name.toRawUTF8(), errorMsg.toRawUTF8());
        if (error)
            *error = describeForUser(desc) + " didn't load"
                     + (errorMsg.isNotEmpty() ? ": " + errorMsg.toStdString() : std::string("."));
        return nullptr;
    }

    // Enable all buses and prepare the plugin
    instance->enableAllBuses();
    instance->prepareToPlay(sampleRate, blockSize);
    instance->setNonRealtime(false);

    auto loaded = std::make_unique<LoadedPlugin>();
    loaded->instance = std::move(instance);
    loaded->info = makeInfo(desc);
    return loaded;
}

PluginHost::NodeLoad PluginHost::loadNodePlugin(const juce::PluginDescription& saved,
                                                int legacyIndex, const std::string& nodeName,
                                                double sampleRate, int blockSize) {
    NodeLoad out;
    const auto res = saved.fileOrIdentifier.isNotEmpty()
                         ? resolvePlugin(saved)
                         : resolveLegacyIndex(legacyIndex, nodeName);
    if (!res.found) {
        out.error = res.problem;
        return out;
    }
    out.resolved = res.description;
    out.plugin = loadPlugin(res.description, sampleRate, blockSize, &out.error);
    return out;
}

PluginHost::PluginDetail PluginHost::getPluginDetail(const juce::PluginDescription& desc) {
    PluginDetail detail;
    detail.info = makeInfo(desc);
    if (isBlocked(desc))
        return detail;

    // Temporarily load the plugin to query its full info
    juce::String errorMsg;
    auto instance = formatManager.createPluginInstance(desc, 44100.0, 512, errorMsg);
    if (!instance) return detail;

    instance->prepareToPlay(44100.0, 512);

    // Buses
    for (int dir = 0; dir < 2; ++dir) {
        bool isInput = (dir == 0);
        int numBuses = instance->getBusCount(isInput);
        for (int i = 0; i < numBuses; ++i) {
            auto* bus = instance->getBus(isInput, i);
            if (bus) {
                PluginDetail::BusInfo bi;
                bi.name = bus->getName().toStdString();
                bi.channels = bus->getNumberOfChannels();
                bi.isInput = isInput;
                detail.buses.push_back(bi);
            }
        }
    }

    // Parameters - cap at 256 to protect against badly-behaved plugins
    // that report thousands of params (one per input combination, etc.).
    // Also skip params with nonsensical ranges (max < min) since those
    // would crash or confuse the automation and MIDI Learn systems.
    {
        auto& params = instance->getParameters();
        constexpr int kMaxParams = 256;
        int count = 0;
        for (auto* p : params) {
            if (count >= kMaxParams) break;
            PluginDetail::ParamInfo pi;
            pi.name = p->getName(128).toStdString();
            pi.label = p->getLabel().toStdString();
            pi.defaultValue = p->getDefaultValue();
            pi.numSteps = p->getNumSteps();
            pi.isAutomatable = p->isAutomatable();
            pi.isDiscrete = p->isDiscrete();
            // Skip params with invalid default values (outside 0..1 JUCE range)
            // or obviously-broken names (empty or all whitespace).
            if (pi.name.empty() || pi.defaultValue < -0.01f || pi.defaultValue > 1.01f)
                continue;
            detail.params.push_back(pi);
            ++count;
        }
        if ((int)params.size() > kMaxParams)
            fprintf(stderr, "Plugin '%s' reports %d params; capped at %d\n",
                    detail.info.name.c_str(), (int)params.size(), kMaxParams);
    }

    // Presets
    int numPrograms = instance->getNumPrograms();
    for (int i = 0; i < numPrograms; ++i) {
        auto name = instance->getProgramName(i).toStdString();
        if (!name.empty())
            detail.presets.push_back(name);
    }

    detail.latencySamples = instance->getLatencySamples();
    detail.tailSeconds = (int)instance->getTailLengthSeconds();
    detail.acceptsMidi = instance->acceptsMidi();
    detail.producesMidi = instance->producesMidi();

    instance->releaseResources();
    return detail;
}

// The cache file: a count and nine text lines per plugin, then the plugins'
// JUCE descriptions as a KnownPluginList document between JUCE_XML and
// END_XML. Only the XML is read back - every text line is derived from it -
// but both halves are still written: PresetRecorder reads the XML half, and an
// older SEANCE reads the text half. The XML is read and written in document
// order here rather than through KnownPluginList, whose addType inserts at the
// front: the old code's cache reload reversed its list that way, while the
// menus kept the text half's order, so after a restart the plugin menus loaded
// the plugin in the mirror-image row.
void PluginHost::saveScanCache(const std::string& path) {
    std::ofstream f(path);
    if (!f) return;

    f << (int)plugins.size() << "\n";
    for (auto& pi : plugins) {
        f << pi.name << "\n";
        f << pi.manufacturer << "\n";
        f << pi.format << "\n";
        f << pi.fileOrId << "\n";
        f << pi.category << "\n";
        f << pi.version << "\n";
        f << pi.descriptiveName << "\n";
        f << pi.uniqueId << "\n";
        f << pi.isInstrument << " " << pi.hasAudioInput << " " << pi.hasAudioOutput << " "
          << pi.hasMidiInput << " " << pi.hasMidiOutput << " "
          << pi.numAudioInputChannels << " " << pi.numAudioOutputChannels << " "
          << pi.numMidiInputPorts << " " << pi.numMidiOutputPorts << "\n";
    }

    juce::XmlElement root("KNOWNPLUGINS");
    for (auto& pi : plugins)
        root.addChildElement(pi.description.createXml().release());
    f << "JUCE_XML\n" << root.toString().toStdString() << "\nEND_XML\n";

    fprintf(stderr, "Plugin cache saved: %d plugins\n", (int)plugins.size());
}

void PluginHost::loadScanCache(const std::string& path) {
    std::ifstream f(path);
    if (!f) return;

    // Skip the text half (see saveScanCache).
    std::string line;
    std::getline(f, line);
    int count = 0;
    try { count = std::stoi(line); } catch (...) { return; }
    for (int i = 0; i < count * 9 && std::getline(f, line); ++i) {}

    std::getline(f, line);
    if (line != "JUCE_XML") return;
    std::string xmlStr;
    while (std::getline(f, line) && line != "END_XML")
        xmlStr += line + "\n";

    auto xml = juce::parseXML(xmlStr);
    if (!xml || !xml->hasTagName("KNOWNPLUGINS")) return;

    plugins.clear();
    for (auto* e : xml->getChildIterator()) {
        juce::PluginDescription desc;
        if (desc.loadFromXml(*e))   // skips <BLACKLISTED> (older caches)
            plugins.push_back(makeInfo(desc));
    }
    updateAvailable();

    fprintf(stderr, "Plugin cache loaded: %d plugins (%d blocked)\n",
            (int)plugins.size(), (int)(plugins.size() - availablePlugins.size()));
}

bool PluginHost::loadPluginFile(const std::string& path) {
    juce::File file(path);
    if (!file.exists()) {
        fprintf(stderr, "Plugin file not found: %s\n", path.c_str());
        return false;
    }

    for (auto* format : formatManager.getFormats()) {
        juce::OwnedArray<juce::PluginDescription> results;
        format->findAllTypesForFile(results, path);
        for (auto* desc : results) {
            plugins.push_back(makeInfo(*desc));
            fprintf(stderr, "Loaded from file: %s (%s)\n",
                    desc->name.toRawUTF8(), desc->manufacturerName.toRawUTF8());
        }
        if (!results.isEmpty()) {
            updateAvailable();
            return true;
        }
    }
    fprintf(stderr, "No plugin found in file: %s\n", path.c_str());
    return false;
}

void PluginHost::openPluginEditor(LoadedPlugin& plugin) {
    if (!plugin.instance) return;
    auto* editor = plugin.instance->createEditorIfNeeded();
    if (editor) {
        // TODO: create a JUCE window to host this editor component
        // For now we just flag it
        plugin.editorOpen = true;
        fprintf(stderr, "Plugin editor opened for: %s\n", plugin.info.name.c_str());
    }
}

void PluginHost::closePluginEditor(LoadedPlugin& plugin) {
    if (plugin.instance) {
        auto* editor = plugin.instance->getActiveEditor();
        if (editor) {
            delete editor;
        }
    }
    plugin.editorOpen = false;
}

} // namespace SoundShop

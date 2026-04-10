#include "plugin_host.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace SoundShop {

PluginHost::PluginHost() {
    juce::addDefaultFormatsToManager(formatManager);
    knownPlugins = std::make_unique<juce::KnownPluginList>();
}

PluginHost::~PluginHost() = default;

void PluginHost::addPluginPaths() {
    // Common VST3 paths on Windows
    // JUCE's VST3 format already knows the standard paths,
    // but we can add custom ones here if needed
}

void PluginHost::scanForPlugins(const std::vector<std::string>& dirs,
                                const std::set<std::string>& blocked) {
    scanning = true;
    availablePlugins.clear();
    knownPlugins->clear();

    for (auto* format : formatManager.getFormats()) {
        auto name = format->getName();
        if (name != "VST3" && name != "AudioUnit")
            continue;

        fprintf(stderr, "Scanning %s plugins...\n", name.toRawUTF8());

        // Build search paths from user dirs, or use defaults
        juce::FileSearchPath searchPaths;
        if (dirs.empty()) {
            searchPaths = format->getDefaultLocationsToSearch();
        } else {
            for (auto& d : dirs)
                searchPaths.add(juce::File(d));
        }

        juce::File deadPluginsFile; // could persist this to skip known-bad files
        juce::PluginDirectoryScanner scanner(
            *knownPlugins, *format, searchPaths,
            true, deadPluginsFile
        );

        juce::String pluginName;
        while (scanner.scanNextFile(true, pluginName)) {
            fprintf(stderr, "  Found: %s\n", pluginName.toRawUTF8());
        }

        // Collect any plugins that failed during scan
        auto failures = scanner.getFailedFiles();
        for (auto& f : failures) {
            fprintf(stderr, "  Skipped (incompatible/32-bit?): %s\n", f.toRawUTF8());
            failedPlugins.insert(f.toStdString());
        }
    }

    // Convert KnownPluginList to our PluginInfo format, filtering blocked
    auto types = knownPlugins->getTypes();
    for (auto& desc : types) {
        std::string fid = desc.fileOrIdentifier.toStdString();
        if (blocked.count(fid)) {
            fprintf(stderr, "  Skipping blocked: %s\n", desc.name.toRawUTF8());
            continue;
        }
        PluginInfo info;
        info.name = desc.name.toStdString();
        info.manufacturer = desc.manufacturerName.toStdString();
        info.format = desc.pluginFormatName.toStdString();
        info.fileOrId = desc.fileOrIdentifier.toStdString();
        info.hasAudioInput = desc.numInputChannels > 0;
        info.hasAudioOutput = desc.numOutputChannels > 0;
        info.hasMidiInput = desc.isInstrument || desc.hasSharedContainer; // approximation
        info.hasMidiOutput = false; // VST3 doesn't clearly expose this in description
        info.numAudioInputChannels = desc.numInputChannels;
        info.numAudioOutputChannels = desc.numOutputChannels;
        info.category = desc.category.toStdString();
        info.version = desc.version.toStdString();
        info.description = desc.descriptiveName.toStdString();
        info.uniqueId = desc.fileOrIdentifier.toStdString(); // use file as unique ID
        info.isInstrument = desc.isInstrument;

        // Better MIDI detection: instruments always accept MIDI
        if (desc.isInstrument)
            info.hasMidiInput = true;

        info.numMidiInputPorts = info.hasMidiInput ? 1 : 0;
        info.numMidiOutputPorts = info.hasMidiOutput ? 1 : 0;

        availablePlugins.push_back(info);
    }

    fprintf(stderr, "Scan complete: %d plugins found\n", (int)availablePlugins.size());
    scanning = false;
}

std::unique_ptr<PluginHost::LoadedPlugin> PluginHost::loadPlugin(
        int pluginIndex, double sampleRate, int blockSize) {
    if (pluginIndex < 0 || pluginIndex >= (int)availablePlugins.size())
        return nullptr;

    auto types = knownPlugins->getTypes();
    if (pluginIndex >= (int)types.size())
        return nullptr;

    auto desc = types[pluginIndex];
    juce::String errorMsg;

    auto instance = formatManager.createPluginInstance(
        desc, sampleRate, blockSize, errorMsg);

    if (!instance) {
        fprintf(stderr, "Failed to load plugin '%s': %s\n",
                desc.name.toRawUTF8(), errorMsg.toRawUTF8());
        return nullptr;
    }

    // Enable all buses and prepare the plugin
    instance->enableAllBuses();
    instance->prepareToPlay(sampleRate, blockSize);
    instance->setNonRealtime(false);

    auto loaded = std::make_unique<LoadedPlugin>();
    loaded->instance = std::move(instance);
    loaded->info = availablePlugins[pluginIndex];
    return loaded;
}

PluginHost::PluginDetail PluginHost::getPluginDetail(int pluginIndex) {
    PluginDetail detail;
    if (pluginIndex < 0 || pluginIndex >= (int)availablePlugins.size())
        return detail;

    detail.info = availablePlugins[pluginIndex];

    // Temporarily load the plugin to query its full info
    auto types = knownPlugins->getTypes();
    if (pluginIndex >= (int)types.size()) return detail;

    auto desc = types[pluginIndex];
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

    // Parameters
    auto& params = instance->getParameters();
    for (auto* p : params) {
        PluginDetail::ParamInfo pi;
        pi.name = p->getName(128).toStdString();
        pi.label = p->getLabel().toStdString();
        pi.defaultValue = p->getDefaultValue();
        pi.numSteps = p->getNumSteps();
        pi.isAutomatable = p->isAutomatable();
        pi.isDiscrete = p->isDiscrete();
        detail.params.push_back(pi);
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

void PluginHost::saveScanCache(const std::string& path) {
    std::ofstream f(path);
    if (!f) return;

    f << (int)availablePlugins.size() << "\n";
    for (auto& pi : availablePlugins) {
        f << pi.name << "\n";
        f << pi.manufacturer << "\n";
        f << pi.format << "\n";
        f << pi.fileOrId << "\n";
        f << pi.category << "\n";
        f << pi.version << "\n";
        f << pi.description << "\n";
        f << pi.uniqueId << "\n";
        f << pi.isInstrument << " " << pi.hasAudioInput << " " << pi.hasAudioOutput << " "
          << pi.hasMidiInput << " " << pi.hasMidiOutput << " "
          << pi.numAudioInputChannels << " " << pi.numAudioOutputChannels << " "
          << pi.numMidiInputPorts << " " << pi.numMidiOutputPorts << "\n";
    }

    // Also save JUCE's KnownPluginList XML for plugin loading
    auto xml = knownPlugins->createXml();
    if (xml)
        f << "JUCE_XML\n" << xml->toString().toStdString() << "\nEND_XML\n";

    fprintf(stderr, "Plugin cache saved: %d plugins\n", (int)availablePlugins.size());
}

void PluginHost::loadScanCache(const std::string& path) {
    std::ifstream f(path);
    if (!f) return;

    availablePlugins.clear();

    int count = 0;
    std::string line;
    std::getline(f, line);
    try { count = std::stoi(line); } catch (...) { return; }

    for (int i = 0; i < count; ++i) {
        PluginInfo pi;
        std::getline(f, pi.name);
        std::getline(f, pi.manufacturer);
        std::getline(f, pi.format);
        std::getline(f, pi.fileOrId);
        std::getline(f, pi.category);
        std::getline(f, pi.version);
        std::getline(f, pi.description);
        std::getline(f, pi.uniqueId);
        std::getline(f, line);
        std::istringstream ss(line);
        int b[5];
        ss >> b[0] >> b[1] >> b[2] >> b[3] >> b[4]
           >> pi.numAudioInputChannels >> pi.numAudioOutputChannels
           >> pi.numMidiInputPorts >> pi.numMidiOutputPorts;
        pi.isInstrument = b[0];
        pi.hasAudioInput = b[1];
        pi.hasAudioOutput = b[2];
        pi.hasMidiInput = b[3];
        pi.hasMidiOutput = b[4];
        availablePlugins.push_back(pi);
    }

    // Load JUCE's KnownPluginList XML
    std::getline(f, line);
    if (line == "JUCE_XML") {
        std::string xmlStr;
        while (std::getline(f, line) && line != "END_XML")
            xmlStr += line + "\n";
        auto xml = juce::parseXML(xmlStr);
        if (xml)
            knownPlugins->recreateFromXml(*xml);
    }

    fprintf(stderr, "Plugin cache loaded: %d plugins\n", (int)availablePlugins.size());
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
            knownPlugins->addType(*desc);

            PluginInfo info;
            info.name = desc->name.toStdString();
            info.manufacturer = desc->manufacturerName.toStdString();
            info.format = desc->pluginFormatName.toStdString();
            info.fileOrId = desc->fileOrIdentifier.toStdString();
            info.hasAudioInput = desc->numInputChannels > 0;
            info.hasAudioOutput = desc->numOutputChannels > 0;
            info.hasMidiInput = desc->isInstrument;
            info.numAudioInputChannels = desc->numInputChannels;
            info.numAudioOutputChannels = desc->numOutputChannels;
            availablePlugins.push_back(info);

            fprintf(stderr, "Loaded from file: %s (%s)\n",
                    info.name.c_str(), info.manufacturer.c_str());
        }
        if (!results.isEmpty()) return true;
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

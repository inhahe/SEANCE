#include "PresetSources.h"
#include "Util.h"

namespace PresetRecorder {

juce::Array<juce::File> Vst3PresetIndex::standardRoots()
{
    juce::Array<juce::File> roots;

   #if JUCE_WINDOWS
    // User, all-users and "network" (Common Files) locations from the VST3 SDK docs.
    roots.add(juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("VST3 Presets"));
    roots.add(juce::File::getSpecialLocation(juce::File::commonApplicationDataDirectory).getChildFile("VST3 Presets"));
    roots.add(juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory)
                  .getChildFile("Common Files").getChildFile("VST3 Presets"));
   #elif JUCE_MAC
    roots.add(juce::File("~/Library/Audio/Presets"));
    roots.add(juce::File("/Library/Audio/Presets"));
    roots.add(juce::File("/Network/Library/Audio/Presets"));
   #else
    roots.add(juce::File("~/.vst3/presets"));
    roots.add(juce::File("/usr/share/vst3/presets"));
    roots.add(juce::File("/usr/local/share/vst3/presets"));
   #endif

    return roots;
}

juce::String Vst3PresetIndex::readClassId(const void* data, size_t size)
{
    if (size < 48)
        return {};

    auto* bytes = static_cast<const char*>(data);
    if (std::memcmp(bytes, "VST3", 4) != 0)
        return {};

    juce::String id(bytes + 8, 32);
    return id.containsOnly("0123456789abcdefABCDEF") && id.length() == 32 ? id.toUpperCase() : juce::String();
}

juce::String Vst3PresetIndex::readClassId(const juce::File& file)
{
    juce::FileInputStream in(file);
    if (! in.openedOk())
        return {};

    char header[48] {};
    return in.read(header, 48) == 48 ? readClassId(header, 48) : juce::String();
}

Vst3PresetIndex Vst3PresetIndex::build(const juce::Array<juce::File>& roots)
{
    Vst3PresetIndex index;
    constexpr int kMaxFiles = 200000; // runaway guard for a pathological folder

    for (auto& root : roots)
    {
        if (! root.isDirectory())
            continue;

        for (const auto& entry : juce::RangedDirectoryIterator(root, true, "*.vstpreset", juce::File::findFiles))
        {
            if (index.numFiles >= kMaxFiles)
                return index;

            auto id = readClassId(entry.getFile());
            if (id.isEmpty())
                continue;

            index.byClassId[id].addIfNotAlreadyThere(entry.getFile().getFullPathName());
            ++index.numFiles;
        }
    }

    for (auto& [id, list] : index.byClassId)
        list.sortNatural();

    return index;
}

juce::var Vst3PresetIndex::toVar() const
{
    auto* obj = new juce::DynamicObject();
    for (auto& [id, list] : byClassId)
    {
        juce::Array<juce::var> files;
        for (auto& f : list)
            files.add(f);
        obj->setProperty(id, files);
    }
    return makeObject({ { "numFiles", numFiles }, { "byClassId", juce::var(obj) } });
}

Vst3PresetIndex Vst3PresetIndex::fromVar(const juce::var& v)
{
    Vst3PresetIndex index;
    index.numFiles = (int) v.getProperty("numFiles", 0);

    if (auto* obj = v["byClassId"].getDynamicObject())
    {
        for (auto& prop : obj->getProperties())
        {
            juce::StringArray list;
            if (auto* arr = prop.value.getArray())
                for (auto& f : *arr)
                    list.add(f.toString());
            index.byClassId[prop.name.toString()] = list;
        }
    }

    return index;
}

juce::String presetNameFromFile(const juce::File& file, const juce::String& pluginName)
{
    // Find the nearest ancestor folder named like the plugin; the path below it
    // is the preset's category + name.
    juce::StringArray parts;
    parts.add(file.getFileNameWithoutExtension());

    for (auto dir = file.getParentDirectory(); dir.getParentDirectory() != dir; dir = dir.getParentDirectory())
    {
        if (dir.getFileName().equalsIgnoreCase(pluginName))
        {
            parts.removeEmptyStrings();
            return parts.joinIntoString(" - ");
        }

        parts.insert(0, dir.getFileName());

        if (parts.size() > 6)
            break;
    }

    return file.getFileNameWithoutExtension();
}

} // namespace PresetRecorder

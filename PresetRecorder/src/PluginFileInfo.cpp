#include "PluginFileInfo.h"
#include "Util.h"

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
static juce::String machineName(juce::uint16 machine, bool& loadable)
{
    const bool host64 = sizeof(void*) == 8;

    switch (machine)
    {
        case 0x014c: loadable = ! host64;  return "32-bit (x86)";
        case 0x8664: loadable = host64;    return "64-bit (x64)";
        case 0xAA64: loadable = false;     return "ARM64";
        case 0x01c4: loadable = false;     return "32-bit ARM";
        default:     loadable = true;      return {};
    }
}

// Reads the COFF machine field of a PE image (Windows DLL/.vst3).
static bool readPeMachine(const juce::File& f, juce::uint16& machine)
{
    juce::FileInputStream in(f);
    if (! in.openedOk())
        return false;

    char mz[2] {};
    if (in.read(mz, 2) != 2 || mz[0] != 'M' || mz[1] != 'Z')
        return false;

    if (! in.setPosition(0x3C))
        return false;

    const auto peOffset = (juce::int64) (juce::uint32) in.readInt();
    if (peOffset <= 0 || peOffset > 1 << 20 || ! in.setPosition(peOffset))
        return false;

    char sig[4] {};
    if (in.read(sig, 4) != 4 || sig[0] != 'P' || sig[1] != 'E' || sig[2] != 0 || sig[3] != 0)
        return false;

    machine = (juce::uint16) in.readShort();
    return true;
}

#if JUCE_WINDOWS
static juce::StringPairArray readVersionStrings(const juce::File& f)
{
    juce::StringPairArray result;
    const auto path = f.getFullPathName();

    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.toWideCharPointer(), &ignored);
    if (size == 0)
        return result;

    juce::HeapBlock<char> data(size);
    if (! GetFileVersionInfoW(path.toWideCharPointer(), 0, size, data.get()))
        return result;

    struct LangAndCodePage { WORD language; WORD codePage; };
    LangAndCodePage* translations = nullptr;
    UINT translationBytes = 0;

    juce::StringArray prefixes;
    if (VerQueryValueW(data.get(), L"\\VarFileInfo\\Translation", (LPVOID*) &translations, &translationBytes))
        for (UINT i = 0; i < translationBytes / sizeof(LangAndCodePage); ++i)
            prefixes.add(juce::String::formatted("\\StringFileInfo\\%04x%04x\\",
                                                 translations[i].language, translations[i].codePage));

    prefixes.addIfNotAlreadyThere("\\StringFileInfo\\040904b0\\");
    prefixes.addIfNotAlreadyThere("\\StringFileInfo\\040904e4\\");

    for (auto* key : { "CompanyName", "ProductName", "FileDescription", "FileVersion", "ProductVersion" })
    {
        for (auto& prefix : prefixes)
        {
            wchar_t* value = nullptr;
            UINT length = 0;
            auto query = prefix + key;

            if (VerQueryValueW(data.get(), query.toWideCharPointer(), (LPVOID*) &value, &length)
                  && value != nullptr && length > 0)
            {
                auto text = juce::String(value, (size_t) length).trim();
                if (text.isNotEmpty())
                {
                    result.set(key, text);
                    break;
                }
            }
        }
    }

    return result;
}
#endif

// A vendor-folder name is informative; the standard plugin roots are not.
static bool isGenericFolderName(const juce::String& name)
{
    static const juce::StringArray generic {
        "vst3", "vst", "vst2", "vstplugins", "vst plugins", "vst3 plugins", "common files",
        "program files", "program files (x86)", "steinberg", "plug-ins", "plugins",
        "components", "lv2", "clap", "audio", "library", "x64", "x86", "64-bit", "32-bit",
        "64bit", "32bit", "win64", "win32", "programs", "common"
    };
    return name.isEmpty() || generic.contains(name, true) || name.endsWithChar(':');
}

juce::String PluginFileInfo::displayStem(const juce::String& fileOrIdentifier)
{
    if (! juce::File::isAbsolutePath(fileOrIdentifier))
        return fileOrIdentifier.fromLastOccurrenceOf("/", false, false)
                               .fromLastOccurrenceOf("#", false, false);

    auto stem = juce::File(fileOrIdentifier).getFileNameWithoutExtension().trim();

    for (auto* suffix : { "(x64)", "(x86)", "(64 bit)", "(32 bit)", "(64-bit)", "(32-bit)",
                          " x64", " x86", "_x64", "_x86", "-x64", "-x86", "_64", "_32" })
        if (stem.endsWithIgnoreCase(suffix))
            stem = stem.dropLastCharacters((int) std::strlen(suffix)).trim();

    return stem;
}

PluginFileInfo PluginFileInfo::inspect(const juce::String& fileOrIdentifier)
{
    PluginFileInfo info;
    info.isFile = juce::File::isAbsolutePath(fileOrIdentifier);

    if (! info.isFile)
    {
        info.exists = true; // can't tell without loading the LV2 world; assume so
        return info;
    }

    const juce::File f(fileOrIdentifier);
    info.exists = f.exists();
    if (! info.exists)
        return info;

    // ---- Which binary would be loaded? --------------------------------------
    juce::File moduleInfo;

    if (f.isDirectory())
    {
        const auto stem = f.getFileNameWithoutExtension();
        const auto contents = f.getChildFile("Contents");
        moduleInfo = contents.getChildFile("Resources").getChildFile("moduleinfo.json");

        for (auto* arch : { "x86_64-win", "arm64x-win", "arm64ec-win", "arm64-win", "x86-win" })
        {
            auto candidate = contents.getChildFile(arch).getChildFile(stem + ".vst3");
            if (candidate.existsAsFile())
            {
                info.binary = candidate;
                break;
            }
        }

        if (info.binary == juce::File())
        {
            auto mac = contents.getChildFile("MacOS").getChildFile(stem);
            auto linux64 = contents.getChildFile("x86_64-linux").getChildFile(stem + ".so");
            if (mac.existsAsFile())          info.binary = mac;
            else if (linux64.existsAsFile()) info.binary = linux64;
        }
    }
    else
    {
        info.binary = f;
    }

    // ---- Architecture ---------------------------------------------------------
    juce::uint16 machine = 0;
    if (info.binary.existsAsFile() && readPeMachine(info.binary, machine))
        info.architecture = machineName(machine, info.loadableArchitecture);

    // ---- VST3 module info -----------------------------------------------------
    if (moduleInfo.existsAsFile())
    {
        auto json = parseLenientJson(moduleInfo.loadFileAsString());
        auto vendor = json["Factory Info"]["Vendor"].toString().trim();

        if (auto* classes = json["Classes"].getArray())
        {
            for (auto& cls : *classes)
            {
                if (cls["Category"].toString() != "Audio Module Class")
                    continue;

                info.pluginNames.addIfNotAlreadyThere(cls["Name"].toString());

                if (vendor.isEmpty())
                    vendor = cls["Vendor"].toString().trim();
                if (info.version.isEmpty())
                    info.version = cls["Version"].toString();

                if (auto* subs = cls["Sub Categories"].getArray())
                {
                    bool instrument = false;
                    for (auto& s : *subs)
                        instrument = instrument || s.toString().equalsIgnoreCase("Instrument");
                    info.instrumentState = juce::jmax(info.instrumentState, instrument ? 1 : 0);
                }
            }
        }

        if (vendor.isNotEmpty())
        {
            info.company = vendor;
            info.companySource = "the plugin's VST3 module info (moduleinfo.json)";
        }
    }

    // ---- Version resource -----------------------------------------------------
   #if JUCE_WINDOWS
    if (info.binary.existsAsFile())
    {
        auto strings = readVersionStrings(info.binary);
        auto company = strings["CompanyName"];

        // "iZotope, Inc." -> "iZotope": the legal suffix only gets in the way of grouping.
        for (auto* suffix : { ", Inc.", " Inc.", ", Inc", " GmbH", " Ltd.", " Ltd", " LLC", " B.V.", " AB", " S.A." })
            if (company.endsWithIgnoreCase(suffix))
                company = company.dropLastCharacters((int) std::strlen(suffix)).trim();

        if (info.company.isEmpty() && company.isNotEmpty())
        {
            info.company = company;
            info.companySource = "the file's version information";
        }

        if (info.pluginNames.isEmpty() && strings["ProductName"].isNotEmpty())
            info.pluginNames.add(strings["ProductName"]);

        if (info.version.isEmpty())
            info.version = strings["FileVersion"];
    }
   #endif

    // ---- Folder name ------------------------------------------------------------
    if (info.company.isEmpty())
    {
        auto parent = f.getParentDirectory().getFileName();
        if (! isGenericFolderName(parent))
        {
            info.company = parent;
            info.companySource = "the name of the folder it is installed in";
        }
    }

    return info;
}

} // namespace PresetRecorder

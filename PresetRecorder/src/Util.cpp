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
juce::File getAppDataDir()
{
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("PresetRecorder");
    dir.createDirectory();
    return dir;
}

static juce::File scratchRoot()
{
    return juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("PresetRecorder");
}

juce::File getSessionScratchDir()
{
    static const juce::File dir = []
    {
        auto name = "run-" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S") + "-"
                  + juce::String::toHexString(juce::Random::getSystemRandom().nextInt()).paddedLeft('0', 8);
        auto d = scratchRoot().getChildFile(name);
        d.createDirectory();
        return d;
    }();
    return dir;
}

void cleanOldScratchDirs()
{
    const auto mine = getSessionScratchDir();
    const auto cutoff = juce::Time::getCurrentTime() - juce::RelativeTime::days(1.0);

    for (const auto& entry : juce::RangedDirectoryIterator(scratchRoot(), false, "run-*",
                                                             juce::File::findDirectories))
    {
        const auto& dir = entry.getFile();
        if (dir != mine && dir.getLastModificationTime() < cutoff)
            dir.deleteRecursively();
    }
}

juce::File getExeFile()
{
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile);
}

juce::File getExeDir()
{
    return getExeFile().getParentDirectory();
}

//==============================================================================
juce::String sanitiseFileNamePart(const juce::String& text, int maxChars, const juce::String& fallback)
{
    juce::String cleaned;
    cleaned.preallocateBytes((size_t) text.getNumBytesAsUTF8() + 4);

    for (auto p = text.getCharPointer(); ! p.isEmpty();)
    {
        auto c = p.getAndAdvance();

        if (c < 32 || c == 127)
            c = ' ';

        switch (c)
        {
            case '"':  c = '\''; break;
            case '<':  c = '(';  break;
            case '>':  c = ')';  break;
            case ':': case '/': case '\\': case '|': c = '-'; break;
            case '?': case '*': continue;
            default: break;
        }

        cleaned << juce::String::charToString(c);
    }

    // Collapse whitespace, then trim what Windows refuses at the end of a name.
    auto collapsed = juce::StringArray::fromTokens(cleaned, " \t", "").joinIntoString(" ");
    auto trimEnd = [](juce::String s)
    {
        s = s.trim();
        while (s.endsWithChar('.') || s.endsWithChar(' '))
            s = s.dropLastCharacters(1);
        return s;
    };

    auto result = trimEnd(collapsed);

    if (result.length() > maxChars)
        result = trimEnd(result.substring(0, maxChars));

    static const juce::StringArray reserved { "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };

    if (reserved.contains(result, true))
        result << "_";

    return result.isEmpty() ? fallback : result;
}

juce::String identifierKey(const juce::String& fileOrIdentifier)
{
    if (juce::File::isAbsolutePath(fileOrIdentifier))
    {
        auto path = juce::File(fileOrIdentifier).getFullPathName();
       #if JUCE_WINDOWS
        return path.toLowerCase();
       #else
        return path;
       #endif
    }

    return fileOrIdentifier;
}

//==============================================================================
juce::String describeExitCode(juce::uint32 code)
{
    const char* what = nullptr;

    switch (code)
    {
        case 0xC0000005u: what = "access violation"; break;
        case 0xC00000FDu: what = "stack overflow"; break;
        case 0xC0000409u: what = "fail-fast / stack buffer overrun"; break;
        case 0xC0000374u: what = "heap corruption"; break;
        case 0xC000001Du: what = "illegal instruction"; break;
        case 0xC0000094u: what = "integer divide by zero"; break;
        case 0xC0000096u: what = "privileged instruction"; break;
        case 0xC0000017u: what = "out of memory"; break;
        case 0xC0000135u: what = "a DLL the plugin needs was not found"; break;
        case 0xC0000142u: what = "a DLL failed to initialise"; break;
        case 0xC000013Au: what = "terminated"; break;
        case 0xC0000602u: what = "fail-fast exception"; break;
        case 0x80000003u: what = "breakpoint / assertion"; break;
        case 0xE06D7363u: what = "unhandled C++ exception"; break;
        case 0x40010004u: what = "killed"; break;
        default: break;
    }

    if (what != nullptr)
        return juce::String(what) + " (0x" + juce::String::toHexString((juce::int64) code).toUpperCase() + ")";

    if (code >= 0x80000000u)
        return "crashed (0x" + juce::String::toHexString((juce::int64) code).toUpperCase() + ")";

    return "exit code " + juce::String((juce::int64) code);
}

juce::String nowString()
{
    return juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S");
}

juce::String formatDuration(double seconds)
{
    if (seconds < 60.0)
        return juce::String(juce::roundToInt(seconds)) + " s";

    auto total = (int) std::llround(seconds);
    auto h = total / 3600, m = (total / 60) % 60, s = total % 60;

    if (h > 0)
        return juce::String(h) + ":" + juce::String(m).paddedLeft('0', 2) + ":" + juce::String(s).paddedLeft('0', 2);

    return juce::String(m) + ":" + juce::String(s).paddedLeft('0', 2);
}

//==============================================================================
juce::var makeObject(Fields fields)
{
    auto* obj = new juce::DynamicObject();
    for (auto& f : fields)
        obj->setProperty(f.first, f.second);
    return juce::var(obj);
}

juce::var parseJsonFile(const juce::File& file)
{
    if (! file.existsAsFile())
        return {};

    juce::var result;
    if (juce::JSON::parse(file.loadFileAsString(), result).failed())
        return {};

    return result;
}

bool writeJsonFile(const juce::File& file, const juce::var& v)
{
    juce::TemporaryFile temp(file);

    if (! temp.getFile().replaceWithText(juce::JSON::toString(v, false)))
        return false;

    return temp.overwriteTargetFileWithTemporary();
}

juce::var parseLenientJson(const juce::String& text)
{
    const std::string in = text.toStdString();
    std::string out;
    out.reserve(in.size());

    bool inString = false, escaped = false;

    for (size_t i = 0; i < in.size(); ++i)
    {
        const char c = in[i];

        if (inString)
        {
            out += c;
            if (escaped)            escaped = false;
            else if (c == '\\')     escaped = true;
            else if (c == '"')      inString = false;
            continue;
        }

        if (c == '"')
        {
            inString = true;
            out += c;
            continue;
        }

        if (c == '/' && i + 1 < in.size() && in[i + 1] == '/')
        {
            while (i < in.size() && in[i] != '\n')
                ++i;
            out += '\n';
            continue;
        }

        if (c == '/' && i + 1 < in.size() && in[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < in.size() && ! (in[i] == '*' && in[i + 1] == '/'))
                ++i;
            ++i;
            continue;
        }

        if (c == ',')
        {
            auto j = i + 1;
            while (j < in.size() && std::isspace((unsigned char) in[j]))
                ++j;
            if (j < in.size() && (in[j] == '}' || in[j] == ']'))
                continue; // trailing comma
        }

        out += c;
    }

    juce::var result;
    if (juce::JSON::parse(juce::String::fromUTF8(out.data(), (int) out.size()), result).failed())
        return {};

    return result;
}

//==============================================================================
EventLogWriter::EventLogWriter(const juce::File& f) : file(f)
{
   #if JUCE_WINDOWS
    auto h = CreateFileW(f.getFullPathName().toWideCharPointer(), FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
        handle = h;
   #else
    auto stream = std::make_unique<juce::FileOutputStream>(f);
    if (stream->openedOk())
        out = std::move(stream);
   #endif
}

EventLogWriter::~EventLogWriter()
{
   #if JUCE_WINDOWS
    if (handle != nullptr)
        CloseHandle((HANDLE) handle);
   #endif
}

bool EventLogWriter::isOpen() const
{
   #if JUCE_WINDOWS
    return handle != nullptr;
   #else
    return out != nullptr;
   #endif
}

void EventLogWriter::append(const juce::String& line)
{
    const juce::ScopedLock sl(lock);

   #if JUCE_WINDOWS
    if (handle == nullptr)
        return;
    const auto utf8 = line.toStdString();
    DWORD written = 0;
    WriteFile((HANDLE) handle, utf8.data(), (DWORD) utf8.size(), &written, nullptr);
   #else
    if (out == nullptr)
        return;
    out->writeText(line, false, false, nullptr);
    out->flush();
   #endif
}

void EventLogWriter::writeRawFromCrashHandler(const char* bytes, int numBytes) const noexcept
{
   #if JUCE_WINDOWS
    if (handle != nullptr && numBytes > 0)
    {
        DWORD written = 0;
        WriteFile((HANDLE) handle, bytes, (DWORD) numBytes, &written, nullptr);
    }
   #else
    juce::ignoreUnused(bytes, numBytes);
   #endif
}

void EventLogWriter::write(const juce::var& object)
{
    append(juce::JSON::toString(object, true) + "\n");
}

void EventLogWriter::event(const juce::String& type, Fields fields)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("ev", type);
    for (auto& f : fields)
        obj->setProperty(f.first, f.second);
    write(juce::var(obj));
}

juce::Array<juce::var> EventLogReader::readNewEvents()
{
    juce::Array<juce::var> events;

    juce::FileInputStream in(file);
    if (! in.openedOk())
        return events;

    const auto total = in.getTotalLength();

    if (total < position) // the file was replaced: start over
    {
        position = 0;
        pending.reset();
    }

    if (total > position)
    {
        in.setPosition(position);
        juce::MemoryBlock chunk;
        const auto got = in.readIntoMemoryBlock(chunk, (juce::pointer_sized_int) (total - position));
        position += (juce::int64) got;
        pending.append(chunk.getData(), got);
    }

    const auto* data = static_cast<const char*>(pending.getData());
    const auto size = pending.getSize();
    size_t lineStart = 0;

    for (size_t i = 0; i < size; ++i)
    {
        if (data[i] != '\n')
            continue;

        auto line = juce::String::fromUTF8(data + lineStart, (int) (i - lineStart)).trim();
        lineStart = i + 1;

        if (line.isEmpty())
            continue;

        juce::var v;
        if (juce::JSON::parse(line, v).wasOk() && v.isObject())
            events.add(v);
        else
            events.add(makeObject({ { "ev", "raw" }, { "text", line } }));
    }

    if (lineStart > 0)
        pending.removeSection(0, lineStart);

    return events;
}

} // namespace PresetRecorder

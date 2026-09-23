#pragma once
#include <juce_core/juce_core.h>
#include <initializer_list>
#include <utility>

// Small helpers shared by the GUI process and the worker processes.
namespace PresetRecorder {

//==============================================================================
// Locations

// %APPDATA%\PresetRecorder - settings, the plugin scan cache and the log.
juce::File getAppDataDir();

// A scratch folder for this run under %TEMP%\PresetRecorder: worker job files,
// their event logs, the decoded effect-input excerpt. Created on first call.
juce::File getSessionScratchDir();

// Deletes scratch folders left behind by earlier runs (anything but ours that
// is more than a day old, so two copies of the tool never trip over each other).
void cleanOldScratchDirs();

juce::File getExeFile();
juce::File getExeDir();

//==============================================================================
// Names and identities

// Makes `text` safe to use as part of a file name on Windows, macOS and Linux:
// drops the characters Windows forbids (<>:"/\|?* and control characters),
// collapses runs of whitespace, trims trailing dots/spaces, caps the length and
// steers clear of reserved device names (CON, NUL, COM1...). Returns `fallback`
// if nothing usable is left.
juce::String sanitiseFileNamePart(const juce::String& text, int maxChars,
                                  const juce::String& fallback);

// Key for comparing plugin identifiers. SEANCE's skip list stores a plugin's
// fileOrIdentifier verbatim; for file paths that string can legitimately differ
// in case or separators from what a scan reports, so absolute paths are
// normalised (and lower-cased on Windows). Anything else - an LV2 URI - is
// compared exactly.
juce::String identifierKey(const juce::String& fileOrIdentifier);

//==============================================================================
// Misc

inline double gainToDb(double gain)
{
    return gain > 1.0e-10 ? 20.0 * std::log10(gain) : -200.0;
}

// "access violation (0xC0000005)" etc. for a process exit code; plain numbers
// for ordinary exit codes.
juce::String describeExitCode(juce::uint32 code);

// Local time as "2026-09-23 11:04:05".
juce::String nowString();

// "1:05:03", "4:07", "12 s"
juce::String formatDuration(double seconds);

//==============================================================================
// JSON

using Fields = std::initializer_list<std::pair<juce::Identifier, juce::var>>;

juce::var makeObject(Fields fields);
juce::var parseJsonFile(const juce::File& file);                // void on failure
bool writeJsonFile(const juce::File& file, const juce::var& v); // atomic replace

// Parses JSON5-flavoured text - trailing commas, // and /* */ comments - which
// is what VST3 bundles' moduleinfo.json files contain. JUCE's JSON parser is
// strict, so this strips those first.
juce::var parseLenientJson(const juce::String& text);

//==============================================================================
// Event logs: how a worker process reports to the GUI.
//
// One JSON object per line, appended and flushed as it happens. The GUI polls
// the file, so everything a worker managed to say survives the worker crashing
// - which is exactly the case we need it for: "the last BEGIN without a DONE"
// names the preset that took the plugin down.

class EventLogWriter
{
public:
    explicit EventLogWriter(const juce::File& file);
    ~EventLogWriter();

    bool isOpen() const;
    const juce::File& getFile() const { return file; }

    void write(const juce::var& object);
    void event(const juce::String& type, Fields fields = {});

    // Appends raw bytes without allocating or locking - for crash handlers,
    // which must not touch the heap. Windows only (a no-op elsewhere).
    void writeRawFromCrashHandler(const char* bytes, int numBytes) const noexcept;

private:
    void append(const juce::String& line);

    juce::File file;
    juce::CriticalSection lock;

    // On Windows the log is a raw FILE_APPEND_DATA handle shared for writing,
    // so a crash handler can append its last words through the same handle
    // (JUCE's FileOutputStream opens files without FILE_SHARE_WRITE, which
    // would lock a second writer out). Each WriteFile then lands atomically at
    // the end of the file.
   #if JUCE_WINDOWS
    void* handle = nullptr;
   #else
    std::unique_ptr<juce::FileOutputStream> out;
   #endif
};

class EventLogReader
{
public:
    EventLogReader() = default;
    explicit EventLogReader(const juce::File& fileToRead) : file(fileToRead) {}

    // Every complete line appended since the previous call, parsed. Lines that
    // don't parse are returned as {"ev":"raw","text":...} so nothing is lost.
    juce::Array<juce::var> readNewEvents();

private:
    juce::File file;
    juce::int64 position = 0;
    juce::MemoryBlock pending;
};

} // namespace PresetRecorder

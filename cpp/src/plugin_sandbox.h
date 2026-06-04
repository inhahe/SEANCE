#pragma once
#include "node_graph.h"
#include "plugin_host.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace SoundShop {

// ==============================================================================
// Out-of-process plugin sandboxing (#85)
//
// Each sandboxed plugin runs in its own child process. Audio and MIDI
// are transferred via shared memory (lock-free ring buffer); control
// messages (load/unload/state) go through a named pipe.
//
// Architecture:
//   Host process:
//     SandboxedPluginProxy (AudioProcessor) ← sits in the JUCE graph
//       → writes audio+MIDI to shared memory
//       → reads processed audio from shared memory
//       → monitors child process health
//
//   Child process (SEANCE.exe --plugin-sandbox <pipe-name>):
//     PluginSandboxChild
//       → loads the actual VST3/AU plugin
//       → reads audio+MIDI from shared memory
//       → processes through the plugin
//       → writes output to shared memory
//       → hosts the plugin's editor window
//
// If the child process crashes, the proxy detects it (pipe disconnect
// or process exit), logs the error, and outputs silence. The user can
// retry loading or remove the node.
//
// Latency: 1 block (configurable via double/triple buffering).
// ==============================================================================

// Shared memory layout for audio transfer. One instance per plugin.
struct SandboxAudioBuffer {
    static constexpr int kMaxBlockSize = 4096;
    static constexpr int kMaxChannels = 8;

    // Double buffer: host writes to writeSlot, child reads from readSlot.
    // After processing, child writes output to the same slot and flips.
    std::atomic<int> activeSlot{0}; // 0 or 1
    float audio[2][kMaxChannels][kMaxBlockSize]; // [slot][channel][sample]
    int numSamples = 0;
    int numChannels = 2;

    // MIDI: simple serialized buffer (max 1KB per block).
    uint8_t midiData[2][1024];
    int midiSize[2] = {0, 0};

    // Status flags.
    std::atomic<bool> childReady{false};
    std::atomic<bool> blockReady{false};   // host signals "new block available"
    std::atomic<bool> outputReady{false};  // child signals "output available"
};

// Control messages sent over the named pipe.
enum class SandboxCommand : int {
    LoadPlugin = 1,     // payload: plugin file path + format
    UnloadPlugin = 2,
    SetState = 3,       // payload: base64 state blob
    GetState = 4,
    PrepareToPlay = 5,  // payload: sampleRate, blockSize
    Shutdown = 99
};

// Proxy processor that sits in the host's JUCE graph and forwards
// audio to/from the child process.
class SandboxedPluginProxy : public juce::AudioProcessor {
public:
    SandboxedPluginProxy(Node& node, const std::string& pluginPath,
                          const std::string& pluginFormat);
    ~SandboxedPluginProxy() override;

    const juce::String getName() const override { return "Sandboxed Plugin"; }
    void prepareToPlay(double sr, int bs) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;

    // Tail: a sandboxed plugin runs in a separate process and we don't
    // round-trip the host plugin's getTailLengthSeconds() across the IPC
    // boundary.  Until that's wired, we use a named conservative upper
    // bound that covers common synth/reverb tails (TODO: forward the
    // real value from the child process).
    double getTailLengthSeconds() const override {
        static constexpr double kSandboxFallbackSeconds = 2.0;
        return kSandboxFallbackSeconds;
    }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    bool isChildAlive() const;
    bool isReady() const { return childAlive && sharedMem != nullptr; }

private:
    Node& node;
    std::string pluginPath;
    std::string pluginFormat;
    double sampleRate = 44100;
    int blockSize = 512;

    // Child process management.
    std::string pipeName;
    bool childAlive = false;

#ifdef _WIN32
    HANDLE childProcess = INVALID_HANDLE_VALUE;
    HANDLE sharedMemHandle = INVALID_HANDLE_VALUE;
    HANDLE pipeHandle = INVALID_HANDLE_VALUE;
#endif
    SandboxAudioBuffer* sharedMem = nullptr;

    bool launchChild();
    void killChild();
    bool sendCommand(SandboxCommand cmd, const std::string& payload = {});
};

// Child-process entry point. Called when SEANCE.exe is launched with
// --plugin-sandbox <pipe-name>. Runs the plugin host loop until the
// parent disconnects or sends Shutdown.
int runPluginSandboxChild(const std::string& pipeName);

} // namespace SoundShop

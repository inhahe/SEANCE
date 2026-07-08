#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include "plugin_sandbox.h"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <algorithm>

namespace SoundShop {

// ==============================================================================
// SandboxedPluginProxy - host-side proxy
// ==============================================================================

SandboxedPluginProxy::SandboxedPluginProxy(Node& n, const std::string& path,
                                            const std::string& fmt)
    : node(n), pluginPath(path), pluginFormat(fmt)
{
    // Generate a unique pipe name from the node ID.
    pipeName = "seance_sandbox_" + std::to_string(node.id) + "_" +
               std::to_string((uintptr_t)this);
}

SandboxedPluginProxy::~SandboxedPluginProxy() {
    killChild();
}

bool SandboxedPluginProxy::launchChild() {
#ifdef _WIN32
    // Create shared memory for audio transfer.
    std::string shmName = "Local\\seance_shm_" + pipeName;
    sharedMemHandle = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        sizeof(SandboxAudioBuffer), shmName.c_str());
    if (!sharedMemHandle) {
        fprintf(stderr, "Sandbox: failed to create shared memory\n");
        return false;
    }
    sharedMem = (SandboxAudioBuffer*)MapViewOfFile(
        sharedMemHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SandboxAudioBuffer));
    if (!sharedMem) {
        CloseHandle(sharedMemHandle);
        sharedMemHandle = INVALID_HANDLE_VALUE;
        fprintf(stderr, "Sandbox: failed to map shared memory\n");
        return false;
    }
    std::memset(sharedMem, 0, sizeof(SandboxAudioBuffer));

    // Create named pipe for control messages.
    std::string fullPipeName = "\\\\.\\pipe\\" + pipeName;
    pipeHandle = CreateNamedPipeA(
        fullPipeName.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, 4096, 4096, 0, nullptr);
    if (pipeHandle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Sandbox: failed to create named pipe\n");
        return false;
    }

    // Launch child process: SEANCE.exe --plugin-sandbox <pipe-name>
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string cmdLine = std::string("\"") + exePath + "\" --plugin-sandbox " + pipeName;

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, (LPSTR)cmdLine.c_str(), nullptr, nullptr,
                         FALSE, 0, nullptr, nullptr, &si, &pi)) {
        fprintf(stderr, "Sandbox: failed to launch child process\n");
        CloseHandle(pipeHandle);
        pipeHandle = INVALID_HANDLE_VALUE;
        return false;
    }
    childProcess = pi.hProcess;
    CloseHandle(pi.hThread);

    // Wait for the child to connect to the pipe.
    ConnectNamedPipe(pipeHandle, nullptr);
    childAlive = true;

    fprintf(stderr, "Sandbox: child process launched (PID %lu)\n",
            (unsigned long)pi.dwProcessId);
    return true;
#else
    fprintf(stderr, "Sandbox: not implemented on this platform yet\n");
    return false;
#endif
}

void SandboxedPluginProxy::killChild() {
#ifdef _WIN32
    if (childAlive) {
        sendCommand(SandboxCommand::Shutdown);
        // Give the child 500ms to exit gracefully.
        if (childProcess != INVALID_HANDLE_VALUE) {
            WaitForSingleObject(childProcess, 500);
            TerminateProcess(childProcess, 1);
            CloseHandle(childProcess);
            childProcess = INVALID_HANDLE_VALUE;
        }
    }
    if (sharedMem) {
        UnmapViewOfFile(sharedMem);
        sharedMem = nullptr;
    }
    if (sharedMemHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(sharedMemHandle);
        sharedMemHandle = INVALID_HANDLE_VALUE;
    }
    if (pipeHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(pipeHandle);
        pipeHandle = INVALID_HANDLE_VALUE;
    }
    childAlive = false;
#endif
}

bool SandboxedPluginProxy::sendCommand(SandboxCommand cmd, const std::string& payload) {
#ifdef _WIN32
    if (pipeHandle == INVALID_HANDLE_VALUE) return false;
    // Protocol: 4 bytes command ID + 4 bytes payload length + payload.
    int cmdInt = (int)cmd;
    int payloadLen = (int)payload.size();
    DWORD written;
    WriteFile(pipeHandle, &cmdInt, 4, &written, nullptr);
    WriteFile(pipeHandle, &payloadLen, 4, &written, nullptr);
    if (payloadLen > 0)
        WriteFile(pipeHandle, payload.data(), payloadLen, &written, nullptr);
    return true;
#else
    (void)cmd; (void)payload;
    return false;
#endif
}

bool SandboxedPluginProxy::isChildAlive() const {
#ifdef _WIN32
    if (childProcess == INVALID_HANDLE_VALUE) return false;
    DWORD exitCode;
    GetExitCodeProcess(childProcess, &exitCode);
    return exitCode == STILL_ACTIVE;
#else
    return false;
#endif
}

void SandboxedPluginProxy::prepareToPlay(double sr, int bs) {
    sampleRate = sr;
    blockSize = bs;
    if (!childAlive) {
        if (!launchChild()) return;
    }
    // Tell the child to prepare.
    std::string payload = std::to_string(sr) + "," + std::to_string(bs);
    sendCommand(SandboxCommand::PrepareToPlay, payload);
    // Tell the child to load the plugin.
    sendCommand(SandboxCommand::LoadPlugin, pluginPath + "|" + pluginFormat);
}

void SandboxedPluginProxy::releaseResources() {
    killChild();
}

void SandboxedPluginProxy::processBlock(juce::AudioBuffer<float>& buf,
                                         juce::MidiBuffer& midi) {
    if (!sharedMem || !childAlive) {
        // Child not running - output silence.
        buf.clear();
        return;
    }

    // Check if child is still alive.
    if (!isChildAlive()) {
        childAlive = false;
        buf.clear();
        fprintf(stderr, "Sandbox: child process crashed!\n");
        return;
    }

    int n = buf.getNumSamples();
    int ch = std::min(buf.getNumChannels(), SandboxAudioBuffer::kMaxChannels);
    n = std::min(n, SandboxAudioBuffer::kMaxBlockSize);
    int slot = sharedMem->activeSlot.load();

    // Write audio to shared memory.
    sharedMem->numSamples = n;
    sharedMem->numChannels = ch;
    for (int c = 0; c < ch; ++c)
        std::memcpy(sharedMem->audio[slot][c], buf.getReadPointer(c),
                     n * sizeof(float));

    // Serialize MIDI to shared memory.
    {
        int midiOff = 0;
        for (auto meta : midi) {
            auto msg = meta.getMessage();
            int msgSize = msg.getRawDataSize();
            if (midiOff + 4 + msgSize > 1024) break;
            // Format: [2-byte offset][2-byte size][data...]
            auto* dst = sharedMem->midiData[slot] + midiOff;
            *(uint16_t*)dst = (uint16_t)meta.samplePosition;
            *(uint16_t*)(dst + 2) = (uint16_t)msgSize;
            std::memcpy(dst + 4, msg.getRawData(), msgSize);
            midiOff += 4 + msgSize;
        }
        sharedMem->midiSize[slot] = midiOff;
    }

    // Signal "block ready" and wait for "output ready".
    sharedMem->blockReady.store(true);
    // Spin-wait with a timeout (~5ms = ~200 iterations at ~40µs/iter).
    for (int i = 0; i < 200 && !sharedMem->outputReady.load(); ++i) {
        // Yield to let the child process run.
#ifdef _WIN32
        SwitchToThread();
#endif
    }

    if (sharedMem->outputReady.load()) {
        // Read processed audio back.
        for (int c = 0; c < ch; ++c)
            std::memcpy(buf.getWritePointer(c), sharedMem->audio[slot][c],
                         n * sizeof(float));
        sharedMem->outputReady.store(false);
    } else {
        // Timeout - child is lagging. Output silence this block.
        buf.clear();
    }

    // Flip the active slot for the next block.
    sharedMem->activeSlot.store(1 - slot);
}

void SandboxedPluginProxy::getStateInformation(juce::MemoryBlock& dest) {
    // Ask the child for the plugin's state.
    sendCommand(SandboxCommand::GetState);
    // For now, state retrieval is async - a full implementation would
    // wait for the child's response on the pipe. Leave empty for MVP.
    (void)dest;
}

void SandboxedPluginProxy::setStateInformation(const void* data, int size) {
    if (size <= 0 || !data) return;
    juce::MemoryBlock block(data, size);
    sendCommand(SandboxCommand::SetState, block.toBase64Encoding().toStdString());
}

// ==============================================================================
// Child process entry point
// ==============================================================================

int runPluginSandboxChild(const std::string& pipeName) {
#ifdef _WIN32
    fprintf(stderr, "Sandbox child: starting with pipe %s\n", pipeName.c_str());

    // Open shared memory created by the parent.
    std::string shmName = "Local\\seance_shm_" + pipeName;
    HANDLE shm = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shmName.c_str());
    if (!shm) {
        fprintf(stderr, "Sandbox child: failed to open shared memory\n");
        return 1;
    }
    auto* buf = (SandboxAudioBuffer*)MapViewOfFile(
        shm, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SandboxAudioBuffer));
    if (!buf) {
        CloseHandle(shm);
        return 1;
    }

    // Connect to the parent's named pipe.
    std::string fullPipeName = "\\\\.\\pipe\\" + pipeName;
    HANDLE pipe = CreateFileA(fullPipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Sandbox child: failed to connect to pipe\n");
        UnmapViewOfFile(buf);
        CloseHandle(shm);
        return 1;
    }

    // Plugin instance (loaded on LoadPlugin command).
    juce::AudioPluginFormatManager formatManager;
    // JUCE 8 removed addDefaultFormats(); add formats individually.
    formatManager.addFormat(new juce::VST3PluginFormat());
#if JUCE_PLUGINHOST_LV2
    formatManager.addFormat(new juce::LV2PluginFormat());
#endif
#if JUCE_PLUGINHOST_AU
    formatManager.addFormat(new juce::AudioUnitPluginFormat());
#endif
#if JUCE_PLUGINHOST_LADSPA
    formatManager.addFormat(new juce::LADSPAPluginFormat());
#endif
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    double sampleRate = 44100;
    int blockSize = 512;

    buf->childReady.store(true);
    fprintf(stderr, "Sandbox child: ready, waiting for commands\n");

    bool running = true;
    while (running) {
        // Check for control messages on the pipe.
        DWORD available = 0;
        PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr);
        if (available >= 8) {
            int cmdInt, payloadLen;
            DWORD bytesRead;
            ReadFile(pipe, &cmdInt, 4, &bytesRead, nullptr);
            ReadFile(pipe, &payloadLen, 4, &bytesRead, nullptr);
            std::string payload;
            if (payloadLen > 0) {
                payload.resize(payloadLen);
                ReadFile(pipe, payload.data(), payloadLen, &bytesRead, nullptr);
            }

            auto cmd = (SandboxCommand)cmdInt;
            switch (cmd) {
                case SandboxCommand::PrepareToPlay: {
                    auto comma = payload.find(',');
                    if (comma != std::string::npos) {
                        sampleRate = std::stod(payload.substr(0, comma));
                        blockSize = std::stoi(payload.substr(comma + 1));
                    }
                    if (plugin) {
                        plugin->prepareToPlay(sampleRate, blockSize);
                        plugin->setPlayConfigDetails(2, 2, sampleRate, blockSize);
                    }
                    break;
                }
                case SandboxCommand::LoadPlugin: {
                    auto bar = payload.find('|');
                    std::string path = (bar != std::string::npos) ? payload.substr(0, bar) : payload;
                    // Try loading via the format manager.
                    juce::String errorMsg;
                    juce::PluginDescription desc;
                    // Scan the specific file to get a description.
                    for (auto* format : formatManager.getFormats()) {
                        juce::OwnedArray<juce::PluginDescription> found;
                        format->findAllTypesForFile(found, path);
                        if (!found.isEmpty()) {
                            desc = *found[0];
                            break;
                        }
                    }
                    if (desc.name.isNotEmpty()) {
                        plugin = formatManager.createPluginInstance(
                            desc, sampleRate, blockSize, errorMsg);
                        if (plugin) {
                            plugin->prepareToPlay(sampleRate, blockSize);
                            fprintf(stderr, "Sandbox child: loaded plugin '%s'\n",
                                    desc.name.toRawUTF8());
                        } else {
                            fprintf(stderr, "Sandbox child: failed to load plugin: %s\n",
                                    errorMsg.toRawUTF8());
                        }
                    }
                    break;
                }
                case SandboxCommand::SetState: {
                    if (plugin) {
                        juce::MemoryBlock stateData;
                        stateData.fromBase64Encoding(payload);
                        if (stateData.getSize() > 0)
                            plugin->setStateInformation(stateData.getData(),
                                                         (int)stateData.getSize());
                    }
                    break;
                }
                case SandboxCommand::Shutdown:
                    running = false;
                    break;
                default:
                    break;
            }
        }

        // Process audio if a block is ready.
        if (buf->blockReady.load() && plugin) {
            int slot = buf->activeSlot.load();
            int n = std::min(buf->numSamples, SandboxAudioBuffer::kMaxBlockSize);
            int ch = std::min(buf->numChannels, SandboxAudioBuffer::kMaxChannels);

            // Build JUCE buffers from shared memory.
            juce::AudioBuffer<float> audioBuf(ch, n);
            for (int c = 0; c < ch; ++c)
                std::memcpy(audioBuf.getWritePointer(c),
                             buf->audio[slot][c], n * sizeof(float));

            juce::MidiBuffer midiBuf;
            // Deserialize MIDI from shared memory.
            int midiOff = 0;
            while (midiOff + 4 <= buf->midiSize[slot]) {
                uint16_t samplePos = *(uint16_t*)(buf->midiData[slot] + midiOff);
                uint16_t msgSize = *(uint16_t*)(buf->midiData[slot] + midiOff + 2);
                if (midiOff + 4 + msgSize > buf->midiSize[slot]) break;
                midiBuf.addEvent(buf->midiData[slot] + midiOff + 4, msgSize, samplePos);
                midiOff += 4 + msgSize;
            }

            // Process through the plugin.
            plugin->processBlock(audioBuf, midiBuf);

            // Write output back to shared memory.
            for (int c = 0; c < ch; ++c)
                std::memcpy(buf->audio[slot][c],
                             audioBuf.getReadPointer(c), n * sizeof(float));

            buf->blockReady.store(false);
            buf->outputReady.store(true);
        } else {
            // No work - yield CPU.
            Sleep(0);
        }
    }

    UnmapViewOfFile(buf);
    CloseHandle(shm);
    CloseHandle(pipe);
    fprintf(stderr, "Sandbox child: shutting down\n");
    return 0;
#else
    (void)pipeName;
    fprintf(stderr, "Sandbox child: not implemented on this platform\n");
    return 1;
#endif
}

} // namespace SoundShop

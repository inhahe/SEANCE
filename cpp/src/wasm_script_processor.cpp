#include "wasm_script_processor.h"

// Only compile the real implementation when wasm3 is available.
// Otherwise provide a stub so the project still builds.
#if __has_include("wasm3.h")
#define HAS_WASM3 1
#include "wasm3.h"
#include "m3_env.h"
#else
#define HAS_WASM3 0
#endif

#include <cstring>
#include <cstdio>
#include <cmath>

namespace SoundShop {

// Shared-memory layout constants (must match soundshop_wasm.h)
static constexpr uint32_t HEADER_SIZE       = 256;
static constexpr uint32_t MAX_PARAMS        = 32;
static constexpr uint32_t PARAM_STRIDE      = 16;   // bytes per param
static constexpr uint32_t MAX_MIDI_EVENTS   = 256;
static constexpr uint32_t MIDI_EVENT_SIZE   = 8;
static constexpr uint32_t MAGIC             = 0x57415343; // "WASC"

// Header field offsets
static constexpr uint32_t H_MAGIC           = 0x00;
static constexpr uint32_t H_VERSION         = 0x04;
static constexpr uint32_t H_BLOCK_SIZE      = 0x08;
static constexpr uint32_t H_SAMPLE_RATE     = 0x0C;
static constexpr uint32_t H_BPM             = 0x10;
static constexpr uint32_t H_BEAT_POS        = 0x14;
static constexpr uint32_t H_TRANSPORT_FLAGS = 0x1C;
static constexpr uint32_t H_NUM_AUDIO_IN    = 0x20;
static constexpr uint32_t H_NUM_AUDIO_OUT   = 0x24;
static constexpr uint32_t H_NUM_PARAMS      = 0x28;
static constexpr uint32_t H_MIDI_IN_COUNT   = 0x2C;
static constexpr uint32_t H_MIDI_OUT_COUNT  = 0x30;
static constexpr uint32_t H_AUDIO_IN_OFF    = 0x34;
static constexpr uint32_t H_AUDIO_OUT_OFF   = 0x38;
static constexpr uint32_t H_PARAM_OFF       = 0x3C;
static constexpr uint32_t H_MIDI_IN_OFF     = 0x40;
static constexpr uint32_t H_MIDI_OUT_OFF    = 0x44;
static constexpr uint32_t H_LATENCY         = 0x48;
static constexpr uint32_t H_TAIL            = 0x4C;

// Helper: write typed values into wasm memory
template<typename T>
static void wmem(uint8_t* base, uint32_t off, T val) {
    std::memcpy(base + off, &val, sizeof(T));
}
template<typename T>
static T rmem(const uint8_t* base, uint32_t off) {
    T val;
    std::memcpy(&val, base + off, sizeof(T));
    return val;
}

// ==============================================================================
// Constructor / Destructor
// ==============================================================================

WasmScriptProcessor::WasmScriptProcessor(Node& n, Transport& t) : node(n), transport(t) {}

WasmScriptProcessor::~WasmScriptProcessor() {
#if HAS_WASM3
    if (wasmRuntime) m3_FreeRuntime(wasmRuntime);
    if (wasmEnv)     m3_FreeEnvironment(wasmEnv);
#endif
}

// ==============================================================================
// Load WASM binary
// ==============================================================================

bool WasmScriptProcessor::loadWasm(const std::vector<uint8_t>& wasmBytes) {
#if HAS_WASM3
    if (wasmBytes.empty()) return false;

    // Clean up previous
    if (wasmRuntime) { m3_FreeRuntime(wasmRuntime); wasmRuntime = nullptr; }
    if (wasmEnv)     { m3_FreeEnvironment(wasmEnv); wasmEnv = nullptr; }
    loaded = false;
    paramDecls.clear();

    wasmEnv = m3_NewEnvironment();
    if (!wasmEnv) { fprintf(stderr, "[WASM] Failed to create environment\n"); return false; }

    // 4 pages = 256 KB — plenty for audio buffers
    wasmRuntime = m3_NewRuntime(wasmEnv, 4 * 65536, this);
    if (!wasmRuntime) { fprintf(stderr, "[WASM] Failed to create runtime\n"); return false; }

    M3Result result = m3_ParseModule(wasmEnv, &wasmModule, wasmBytes.data(), (uint32_t)wasmBytes.size());
    if (result) { fprintf(stderr, "[WASM] Parse error: %s\n", result); return false; }

    result = m3_LoadModule(wasmRuntime, wasmModule);
    if (result) { fprintf(stderr, "[WASM] Load error: %s\n", result); return false; }

    // Link host imports
    m3_LinkRawFunction(wasmModule, "env", "ss_declare_param", "i(*fff)", [](IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem) -> const void* {
        auto* self = (WasmScriptProcessor*)m3_GetUserData(rt);
        uint32_t namePtr = (uint32_t)sp[0];
        float def = *(float*)&sp[1];
        float mn  = *(float*)&sp[2];
        float mx  = *(float*)&sp[3];
        const char* name = namePtr < self->wasmMemSize ? (const char*)(self->wasmMem + namePtr) : "?";
        int idx = (int)self->paramDecls.size();
        self->paramDecls.push_back({name, def, mn, mx});
        sp[0] = (uint64_t)idx;
        return m3Err_none;
    });

    m3_LinkRawFunction(wasmModule, "env", "ss_log", "v(*)", [](IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem) -> const void* {
        auto* self = (WasmScriptProcessor*)m3_GetUserData(rt);
        uint32_t msgPtr = (uint32_t)sp[0];
        if (msgPtr < self->wasmMemSize)
            fprintf(stderr, "[WASM script] %s\n", (const char*)(self->wasmMem + msgPtr));
        return m3Err_none;
    });

    m3_LinkRawFunction(wasmModule, "env", "ss_get_param", "f(i)", [](IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem) -> const void* {
        auto* self = (WasmScriptProcessor*)m3_GetUserData(rt);
        int idx = (int)sp[0];
        float val = 0;
        if (idx >= 0 && idx < (int)self->node.params.size())
            val = self->node.params[idx].value;
        *(float*)&sp[0] = val;
        return m3Err_none;
    });

    m3_LinkRawFunction(wasmModule, "env", "ss_midi_out", "v(iiii)", [](IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem) -> const void* {
        auto* self = (WasmScriptProcessor*)m3_GetUserData(rt);
        uint32_t count = rmem<uint32_t>(self->wasmMem, H_MIDI_OUT_COUNT);
        if (count >= MAX_MIDI_EVENTS) return m3Err_none;
        uint32_t off = self->midiOutOffset + count * MIDI_EVENT_SIZE;
        if (off + MIDI_EVENT_SIZE > self->wasmMemSize) return m3Err_none;
        wmem<uint32_t>(self->wasmMem, off + 0, (uint32_t)sp[0]);
        self->wasmMem[off + 4] = (uint8_t)sp[1];
        self->wasmMem[off + 5] = (uint8_t)sp[2];
        self->wasmMem[off + 6] = (uint8_t)sp[3];
        self->wasmMem[off + 7] = 0;
        wmem<uint32_t>(self->wasmMem, H_MIDI_OUT_COUNT, count + 1);
        return m3Err_none;
    });

    // Get WASM memory pointer
    wasmMem = m3_GetMemory(wasmRuntime, &wasmMemSize, 0);
    if (!wasmMem) { fprintf(stderr, "[WASM] No memory\n"); return false; }

    // Find exported functions
    m3_FindFunction(&fnInit, wasmRuntime, "ss_init");
    m3_FindFunction(&fnProcess, wasmRuntime, "ss_process");
    m3_FindFunction(&fnPrepare, wasmRuntime, "ss_prepare"); // optional

    if (!fnInit || !fnProcess) {
        fprintf(stderr, "[WASM] Missing required exports (ss_init, ss_process)\n");
        return false;
    }

    // Query I/O counts (optional exports)
    IM3Function fnNumIn = nullptr, fnNumOut = nullptr;
    m3_FindFunction(&fnNumIn, wasmRuntime, "ss_num_audio_inputs");
    m3_FindFunction(&fnNumOut, wasmRuntime, "ss_num_audio_outputs");
    if (fnNumIn)  { m3_CallV(fnNumIn);  m3_GetResultsV(fnNumIn, &numAudioInPairs); }
    if (fnNumOut) { m3_CallV(fnNumOut); m3_GetResultsV(fnNumOut, &numAudioOutPairs); }
    numAudioInPairs  = juce::jlimit(0, 8, numAudioInPairs);
    numAudioOutPairs = juce::jlimit(1, 8, numAudioOutPairs);

    // Initialize header before calling ss_init
    computeOffsets();
    writeHeader();

    // Call ss_init — script declares parameters here
    result = m3_CallV(fnInit);
    if (result) {
        fprintf(stderr, "[WASM] ss_init error: %s\n", result);
        return false;
    }

    loaded = true;
    fprintf(stderr, "[WASM] Loaded: %d params, %d audio in pairs, %d audio out pairs\n",
            (int)paramDecls.size(), numAudioInPairs, numAudioOutPairs);
    return true;
#else
    (void)wasmBytes;
    fprintf(stderr, "[WASM] wasm3 not available (not compiled with wasm3 support)\n");
    return false;
#endif
}

// ==============================================================================
// Populate node pins from declared parameters and I/O
// ==============================================================================

void WasmScriptProcessor::populateNodePins(Node& n) {
    // Clear existing pins (except preserve IDs for reconnection if possible)
    n.pinsIn.clear();
    n.pinsOut.clear();
    n.params.clear();

    int nextPinId = n.id * 100; // deterministic pin IDs based on node ID

    // Audio inputs
    for (int i = 0; i < numAudioInPairs; ++i) {
        auto name = numAudioInPairs == 1 ? "Audio In" : "Audio In " + std::to_string(i + 1);
        n.pinsIn.push_back({nextPinId++, name, PinKind::Audio, true, 2});
    }

    // MIDI input
    n.pinsIn.push_back({nextPinId++, "MIDI In", PinKind::Midi, true});

    // Param inputs
    for (int i = 0; i < (int)paramDecls.size(); ++i) {
        n.pinsIn.push_back({nextPinId++, paramDecls[i].name, PinKind::Param, true});
        Param p;
        p.name = paramDecls[i].name;
        p.value = paramDecls[i].defaultVal;
        p.minVal = paramDecls[i].minVal;
        p.maxVal = paramDecls[i].maxVal;
        n.params.push_back(p);
    }

    // Audio outputs
    for (int i = 0; i < numAudioOutPairs; ++i) {
        auto name = numAudioOutPairs == 1 ? "Audio Out" : "Audio Out " + std::to_string(i + 1);
        n.pinsOut.push_back({nextPinId++, name, PinKind::Audio, false, 2});
    }

    // MIDI output
    n.pinsOut.push_back({nextPinId++, "MIDI Out", PinKind::Midi, false});
}

// ==============================================================================
// Prepare / process
// ==============================================================================

void WasmScriptProcessor::prepareToPlay(double sr, int bs) {
    sampleRate = sr;
    blockSize = bs;
    if (!loaded) return;

    computeOffsets();

#if HAS_WASM3
    // Refresh memory pointer (may have changed after grow)
    wasmMem = m3_GetMemory(wasmRuntime, &wasmMemSize, 0);

    if (fnPrepare)
        m3_CallV(fnPrepare);
#endif
}

void WasmScriptProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) {
    if (!loaded || !wasmMem) {
        buf.clear();
        return;
    }

#if HAS_WASM3
    writeHeader();
    writeParams();
    copyAudioIn(buf);
    copyMidiIn(midi);

    // Clear MIDI out count
    wmem<uint32_t>(wasmMem, H_MIDI_OUT_COUNT, 0u);

    // Call the script's process function
    M3Result result = m3_CallV(fnProcess);
    if (result) {
        // Script error — mute output
        buf.clear();
        midi.clear();
        return;
    }

    copyAudioOut(buf);
    midi.clear();
    copyMidiOut(midi);
#else
    buf.clear();
#endif
}

double WasmScriptProcessor::getTailLengthSeconds() const {
    if (!wasmMem) return 0;
    uint32_t tail = rmem<uint32_t>(wasmMem, H_TAIL);
    return sampleRate > 0 ? tail / sampleRate : 0;
}

// ==============================================================================
// Memory layout and I/O helpers
// ==============================================================================

void WasmScriptProcessor::computeOffsets() {
    uint32_t off = HEADER_SIZE;

    paramOffset = off;
    off += MAX_PARAMS * PARAM_STRIDE;

    audioInOffset = off;
    off += numAudioInPairs * 2 * blockSize * sizeof(float);

    audioOutOffset = off;
    off += numAudioOutPairs * 2 * blockSize * sizeof(float);

    midiInOffset = off;
    off += MAX_MIDI_EVENTS * MIDI_EVENT_SIZE;

    midiOutOffset = off;
    off += MAX_MIDI_EVENTS * MIDI_EVENT_SIZE;

    // Ensure we have enough WASM memory
    // (wasm3 allocates in 64KB pages; 4 pages = 256KB should be enough)
}

void WasmScriptProcessor::writeHeader() {
    if (!wasmMem) return;
    wmem<uint32_t>(wasmMem, H_MAGIC, MAGIC);
    wmem<uint32_t>(wasmMem, H_VERSION, 1u);
    wmem<uint32_t>(wasmMem, H_BLOCK_SIZE, (uint32_t)blockSize);
    wmem<float>   (wasmMem, H_SAMPLE_RATE, (float)sampleRate);
    wmem<float>   (wasmMem, H_BPM, (float)transport.bpm);
    wmem<double>  (wasmMem, H_BEAT_POS, transport.positionBeats());
    uint32_t flags = 0;
    if (transport.playing) flags |= 0x01;
    if (transport.recording) flags |= 0x02;
    wmem<uint32_t>(wasmMem, H_TRANSPORT_FLAGS, flags);
    wmem<uint32_t>(wasmMem, H_NUM_AUDIO_IN, (uint32_t)numAudioInPairs);
    wmem<uint32_t>(wasmMem, H_NUM_AUDIO_OUT, (uint32_t)numAudioOutPairs);
    wmem<uint32_t>(wasmMem, H_NUM_PARAMS, (uint32_t)paramDecls.size());
    wmem<uint32_t>(wasmMem, H_AUDIO_IN_OFF, audioInOffset);
    wmem<uint32_t>(wasmMem, H_AUDIO_OUT_OFF, audioOutOffset);
    wmem<uint32_t>(wasmMem, H_PARAM_OFF, paramOffset);
    wmem<uint32_t>(wasmMem, H_MIDI_IN_OFF, midiInOffset);
    wmem<uint32_t>(wasmMem, H_MIDI_OUT_OFF, midiOutOffset);
}

void WasmScriptProcessor::writeParams() {
    if (!wasmMem) return;
    for (int i = 0; i < (int)paramDecls.size() && i < (int)node.params.size(); ++i) {
        uint32_t off = paramOffset + i * PARAM_STRIDE;
        wmem<float>(wasmMem, off + 0, node.params[i].value);
        wmem<float>(wasmMem, off + 4, paramDecls[i].minVal);
        wmem<float>(wasmMem, off + 8, paramDecls[i].maxVal);
        wmem<float>(wasmMem, off + 12, paramDecls[i].defaultVal);
    }
}

void WasmScriptProcessor::copyAudioIn(const juce::AudioBuffer<float>& buf) {
    if (!wasmMem) return;
    int samples = std::min(blockSize, buf.getNumSamples());
    int channels = buf.getNumChannels();
    for (int pair = 0; pair < numAudioInPairs; ++pair) {
        for (int ch = 0; ch < 2; ++ch) {
            int srcCh = pair * 2 + ch;
            uint32_t off = audioInOffset + (pair * 2 + ch) * blockSize * sizeof(float);
            if (off + blockSize * sizeof(float) > wasmMemSize) break;
            if (srcCh < channels)
                std::memcpy(wasmMem + off, buf.getReadPointer(srcCh), samples * sizeof(float));
            else
                std::memset(wasmMem + off, 0, blockSize * sizeof(float));
        }
    }
}

void WasmScriptProcessor::copyAudioOut(juce::AudioBuffer<float>& buf) {
    if (!wasmMem) return;
    int samples = std::min(blockSize, buf.getNumSamples());
    int channels = buf.getNumChannels();
    for (int pair = 0; pair < numAudioOutPairs; ++pair) {
        for (int ch = 0; ch < 2; ++ch) {
            int dstCh = pair * 2 + ch;
            uint32_t off = audioOutOffset + (pair * 2 + ch) * blockSize * sizeof(float);
            if (off + blockSize * sizeof(float) > wasmMemSize) break;
            if (dstCh < channels)
                std::memcpy(buf.getWritePointer(dstCh), wasmMem + off, samples * sizeof(float));
        }
    }
}

void WasmScriptProcessor::copyMidiIn(const juce::MidiBuffer& midi) {
    if (!wasmMem) return;
    uint32_t count = 0;
    for (auto metadata : midi) {
        if (count >= MAX_MIDI_EVENTS) break;
        auto msg = metadata.getMessage();
        if (msg.getRawDataSize() < 1) continue;
        uint32_t off = midiInOffset + count * MIDI_EVENT_SIZE;
        if (off + MIDI_EVENT_SIZE > wasmMemSize) break;
        wmem<uint32_t>(wasmMem, off + 0, (uint32_t)metadata.samplePosition);
        auto* raw = msg.getRawData();
        wasmMem[off + 4] = raw[0];
        wasmMem[off + 5] = msg.getRawDataSize() > 1 ? raw[1] : 0;
        wasmMem[off + 6] = msg.getRawDataSize() > 2 ? raw[2] : 0;
        wasmMem[off + 7] = 0;
        count++;
    }
    wmem<uint32_t>(wasmMem, H_MIDI_IN_COUNT, count);
}

void WasmScriptProcessor::copyMidiOut(juce::MidiBuffer& midi) {
    if (!wasmMem) return;
    uint32_t count = rmem<uint32_t>(wasmMem, H_MIDI_OUT_COUNT);
    count = std::min(count, MAX_MIDI_EVENTS);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t off = midiOutOffset + i * MIDI_EVENT_SIZE;
        if (off + MIDI_EVENT_SIZE > wasmMemSize) break;
        uint32_t samplePos = rmem<uint32_t>(wasmMem, off + 0);
        uint8_t status = wasmMem[off + 4];
        uint8_t d1 = wasmMem[off + 5];
        uint8_t d2 = wasmMem[off + 6];
        midi.addEvent(juce::MidiMessage(status, d1, d2), (int)samplePos);
    }
}

} // namespace SoundShop

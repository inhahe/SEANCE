#include "script_runtime.h"

// Only compile the real implementation when wasm3 is available; otherwise a
// graceful stub keeps the build working and the editor simply reports that the
// WASM language option is unavailable in this build.
#if __has_include("wasm3.h")
#define HAS_WASM3 1
#include "wasm3.h"
#include "m3_env.h"
#else
#define HAS_WASM3 0
#endif

#include "waveform_bank.h"      // shared cross-language waveform() factory bank
#include "warp.h"               // shared cross-language warpamp/warpphase shapers
#include "buffer_warp.h"        // Bucket C whole-buffer spectral/wavelet warps
#include <juce_core/juce_core.h>
#include <cstring>
#include <cmath>
#include <vector>

namespace SoundShop {

// -----------------------------------------------------------------------------
// WasmRuntime - a user-supplied WebAssembly binary as a MIDI / Signal generator.
// -----------------------------------------------------------------------------
//
// PerBlock only: the WASM ABI is inherently block-at-a-time (one ss_process call
// fills a whole buffer / emits a whole block of MIDI). The `source` passed to
// load() is the FILESYSTEM PATH to the .wasm file (chosen via the editor's file
// dialog), not program text.
//
// The shared-memory ABI mirrors the standalone "Script (WASM)" node
// (wasm_script_processor.cpp) so there is ONE documented WASM contract:
//
//   Exports the script provides:
//     ss_init()                  - called once after load (declare params)
//     ss_process()               - called once per block; fills audio-out /
//                                  emits MIDI for the block
//     ss_prepare()               - optional; called when sample rate / block
//                                  size changes
//     ss_num_audio_inputs()      - optional; pairs of audio-in (default 1)
//     ss_num_audio_outputs()     - optional; pairs of audio-out (default 1)
//     ss_num_midi_outputs()      - optional; independent MIDI out pins (default 1)
//
//   Imports the host provides (env.*):
//     ss_midi_out(pos, status, d1, d2)
//     ss_midi_out_n(outIdx, pos, status, d1, d2)
//     ss_get_param(idx) / ss_declare_param(name, def, min, max) / ss_log(msg)
//
// Because the module owns its own loop and linear memory persists across calls,
// WasmRuntime reports isStreaming() == true: the host runs it through the block
// path regardless of the Run dropdown (audio-rate vs block-rate is purely the
// module's internal business) and hands it an independent buffer per output pin,
// routing each back to its own cable (no fan-out).
//
// Role mapping for the two runtime uses:
//   * Signal role (LFO): the host writes each signal input pin into a flat
//     audio-in channel (pin k -> channel k) and reads each flat audio-out channel
//     p back into output pin o(p+1). MIDI-input events (if the node has a MIDI in)
//     are written into the shared midiIn region in raw-MIDI form so the module can
//     react to notes/CC while generating its signal.
//   * MIDI role: the host ignores audio and routes ss_midi_out* events into
//     ctx.sink, stamping each with its sample offset; MIDI input is likewise
//     forwarded into the midiIn region.
//
// NOTE: the shared-memory layout constants below duplicate the binary contract
// in wasm_script_processor.cpp (which in turn matches the WASM-side
// soundshop_wasm.h header). They are a stable ABI; if the layout ever changes,
// all three must change together.
#if HAS_WASM3
static constexpr uint32_t HEADER_SIZE       = 256;
static constexpr uint32_t MAX_PARAMS        = 32;
static constexpr uint32_t PARAM_STRIDE      = 16;
static constexpr uint32_t MAX_MIDI_EVENTS   = 256;
static constexpr uint32_t MIDI_EVENT_SIZE   = 8;
static constexpr uint32_t MAGIC             = 0x57415343; // "WASC"

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

template<typename T> static void wmem(uint8_t* base, uint32_t off, T v) { std::memcpy(base + off, &v, sizeof(T)); }
template<typename T> static T    rmem(const uint8_t* base, uint32_t off) { T v; std::memcpy(&v, base + off, sizeof(T)); return v; }
#endif // HAS_WASM3

class WasmRuntime : public IScriptRuntime {
public:
    explicit WasmRuntime(ScriptRole r) : role(r) {}
    ~WasmRuntime() override {
#if HAS_WASM3
        if (rt)  m3_FreeRuntime(rt);
        if (env) m3_FreeEnvironment(env);
#endif
    }

    bool load(const std::string& path, std::string& error) override {
#if HAS_WASM3
        // `path` is a filesystem path to the .wasm binary.
        juce::File f{ juce::String(path) };
        if (path.empty() || !f.existsAsFile()) {
            error = "No WASM file selected";
            loaded = false; lastErr = error; return false;
        }
        juce::MemoryBlock mb;
        if (!f.loadFileAsData(mb) || mb.getSize() == 0) {
            error = "Could not read " + path;
            loaded = false; lastErr = error; return false;
        }
        std::vector<uint8_t> bytes((const uint8_t*)mb.getData(),
                                   (const uint8_t*)mb.getData() + mb.getSize());
        bool ok = loadBytes(bytes, error);
        lastErr = ok ? std::string() : error;
        return ok;
#else
        (void)path;
        error = "WASM scripting is not available in this build (wasm3 not compiled in)";
        lastErr = error;
        return false;
#endif
    }

    void reset() override {
#if HAS_WASM3
        // Re-run ss_init to reseed script state.
        if (loaded && fnInit) m3_CallV(fnInit);
#endif
    }

    void runBlock(const ScriptBlockCtx& ctx) override {
#if HAS_WASM3
        if (!loaded || !mem || !fnProcess) {
            // A terrain-only module (ss_generate but no ss_process) has nothing
            // to run per block; emit silence rather than calling a null export.
            if (ctx.out) std::memset(ctx.out, 0, sizeof(float) * (size_t)ctx.numSamples);
            return;
        }
        prepareIfNeeded(ctx);
        writeHeader(ctx);
        writeSignalInputs(ctx);
        writeMidiInputs(ctx);

        wmem<uint32_t>(mem, H_MIDI_OUT_COUNT, 0u);

        M3Result res = m3_CallV(fnProcess);
        if (res) {
            if (ctx.out) std::memset(ctx.out, 0, sizeof(float) * (size_t)ctx.numSamples);
            return;
        }

        if (role == ScriptRole::Signal) readSignalOutput(ctx);
        else                            emitMidiOut(ctx);
#else
        if (ctx.out) std::memset(ctx.out, 0, sizeof(float) * (size_t)ctx.numSamples);
#endif
    }

    // --- Whole-grid terrain generation (offline bake; see runGenerate above) ---
    // Run the module's ss_generate() once; it fills `data` (length product(dims),
    // row-major, each cell 0..1) by calling the ss_grid_* host imports. Mirrors
    // LuaRuntime::runGenerate. Only reached for the Terrain "generate" feature -
    // never the audio thread. The grid lives HOST-side (genData); the imports
    // read/write it, so the module needs no shared-memory grid region.
    bool runGenerate(const std::vector<int>& dims, float* data,
                     std::string& error) override {
#if HAS_WASM3
        if (!loaded) { error = lastErr.empty() ? "WASM module not loaded" : lastErr; return false; }
        if (!fnGenerate) {
            error = "WASM module needs a ss_generate() export for whole-grid terrain";
            return false;
        }
        long long total = 1;
        for (int d : dims) total *= (d > 0 ? d : 1);
        genData = data; genDims = &dims; genTotal = total;
        M3Result res = m3_CallV(fnGenerate);
        genData = nullptr; genDims = nullptr; genTotal = 0;
        if (res) {
            error = std::string("WASM ss_generate() failed: ") + res;
            lastErr = error;
            return false;
        }
        return true;
#else
        (void)dims; (void)data;
        error = "WASM scripting is not available in this build (wasm3 not compiled in)";
        return false;
#endif
    }

    bool supportsPerSample() const override { return false; }
    bool supportsPerBlock()  const override { return true; }
    // A WASM module inherently owns its own loop (ss_process fills the whole block
    // and linear memory persists across calls), and it can write an independent
    // buffer per output channel - so it behaves like a streaming program: the host
    // routes each output pin separately (no fan-out) and the Run dropdown is moot.
    bool isStreaming() const override { return true; }
    std::string getError() const override { return lastErr; }

    // Public accessor for the protected note->Hz mapper so the static wasm3
    // import trampoline (a captureless lambda) can reach the project tuning.
    float callNoteFreq(int n) const { return noteFreq(n); }

private:
    ScriptRole role;
    std::string lastErr;
    bool loaded = false;

#if HAS_WASM3
    IM3Environment env = nullptr;
    IM3Runtime     rt  = nullptr;
    IM3Module      mod = nullptr;
    IM3Function    fnInit = nullptr, fnProcess = nullptr, fnPrepare = nullptr,
                   fnGenerate = nullptr;

    // Whole-grid generation state (set only for the duration of runGenerate; the
    // ss_grid_* import trampolines read these via m3_GetUserData). The grid is
    // HOST-owned - the module never sees it in linear memory, it pokes cells
    // through the imports - so no extra wasm memory region is needed.
    float*                  genData  = nullptr;
    const std::vector<int>* genDims  = nullptr;
    long long               genTotal = 0;

    // Integer coordinate of flat cell i along `axis` (row-major: last axis is the
    // fastest-varying). 0 outside range. Mirrors LuaRuntime's l_gen_coordAxis.
    long long gridCoordAxis(long long i, int axis) const {
        if (!genDims || axis < 0 || axis >= (int)genDims->size() || i < 0 || i >= genTotal)
            return 0;
        const auto& d = *genDims;
        long long tmp = i, c = 0;
        for (int a = (int)d.size() - 1; a >= 0; --a) {
            int sz = d[(size_t)a] > 0 ? d[(size_t)a] : 1;
            long long coordA = tmp % sz;
            tmp /= sz;
            if (a == axis) { c = coordA; break; }
        }
        return c;
    }
    // Normalised position [0,1] of flat cell i along `axis`. Mirrors l_gen_coord.
    float gridCoordNorm(long long i, int axis) const {
        if (!genDims || axis < 0 || axis >= (int)genDims->size() || i < 0 || i >= genTotal)
            return 0.0f;
        const auto& d = *genDims;
        long long c = gridCoordAxis(i, axis);
        int sz = d[(size_t)axis] > 0 ? d[(size_t)axis] : 1;
        return (sz > 1) ? (float)((double)c / (double)(sz - 1)) : 0.0f;
    }
    // Flat index of the cell `delta` steps from i along `axis`, edge-clamped.
    // Mirrors l_gen_neighbor.
    long long gridNeighbor(long long i, int axis, long long delta) const {
        if (!genDims || axis < 0 || axis >= (int)genDims->size() || i < 0 || i >= genTotal)
            return i;
        const auto& d = *genDims;
        int n = (int)d.size();
        std::vector<long long> co((size_t)n, 0);
        long long tmp = i;
        for (int a = n - 1; a >= 0; --a) {
            int sz = d[(size_t)a] > 0 ? d[(size_t)a] : 1;
            co[(size_t)a] = tmp % sz;
            tmp /= sz;
        }
        int sz = d[(size_t)axis] > 0 ? d[(size_t)axis] : 1;
        long long c = co[(size_t)axis] + delta;
        if (c < 0) c = 0; else if (c > sz - 1) c = sz - 1;
        co[(size_t)axis] = c;
        long long idx = 0;
        for (int a = 0; a < n; ++a) {
            int s = d[(size_t)a] > 0 ? d[(size_t)a] : 1;
            idx = idx * s + co[(size_t)a];
        }
        return idx;
    }

    uint8_t* mem = nullptr;
    uint32_t memSize = 0;

    int numAudioInPairs  = 1;
    int numAudioOutPairs = 1;
    int numMidiOutPins   = 1;

    double curSampleRate = 0.0;
    int    curBlockSize  = 0;

    uint32_t paramOffset = 0, audioInOffset = 0, audioOutOffset = 0,
             midiInOffset = 0, midiOutOffset = 0;

    bool loadBytes(const std::vector<uint8_t>& bytes, std::string& error) {
        if (rt)  { m3_FreeRuntime(rt);  rt  = nullptr; }
        if (env) { m3_FreeEnvironment(env); env = nullptr; }
        loaded = false; mem = nullptr;

        env = m3_NewEnvironment();
        if (!env) { error = "wasm3: failed to create environment"; return false; }
        rt = m3_NewRuntime(env, 4 * 65536, this);
        if (!rt) { error = "wasm3: failed to create runtime"; return false; }

        M3Result r = m3_ParseModule(env, &mod, bytes.data(), (uint32_t)bytes.size());
        if (r) { error = std::string("wasm3 parse error: ") + r; return false; }
        r = m3_LoadModule(rt, mod);
        if (r) { error = std::string("wasm3 load error: ") + r; return false; }

        linkImports();

        mem = m3_GetMemory(rt, &memSize, 0);
        if (!mem) { error = "wasm3: module has no memory"; return false; }

        m3_FindFunction(&fnInit, rt, "ss_init");
        m3_FindFunction(&fnProcess, rt, "ss_process");
        m3_FindFunction(&fnPrepare, rt, "ss_prepare");
        m3_FindFunction(&fnGenerate, rt, "ss_generate");
        // Two valid module shapes share this loader: an AUDIO module exports
        // ss_process() (filled per block), a TERRAIN module exports ss_generate()
        // (fills a whole grid once via runGenerate). Either satisfies the loader;
        // ss_init is always required (it seeds module state - may be empty).
        if (!fnInit || (!fnProcess && !fnGenerate)) {
            error = "WASM missing required exports: ss_init plus ss_process "
                    "(audio) or ss_generate (terrain)";
            return false;
        }

        IM3Function f = nullptr;
        m3_FindFunction(&f, rt, "ss_num_audio_inputs");
        if (f) { m3_CallV(f); m3_GetResultsV(f, &numAudioInPairs); }
        f = nullptr;
        m3_FindFunction(&f, rt, "ss_num_audio_outputs");
        if (f) { m3_CallV(f); m3_GetResultsV(f, &numAudioOutPairs); }
        f = nullptr;
        m3_FindFunction(&f, rt, "ss_num_midi_outputs");
        if (f) { m3_CallV(f); m3_GetResultsV(f, &numMidiOutPins); }
        numAudioInPairs  = juce::jlimit(0, 8, numAudioInPairs);
        numAudioOutPairs = juce::jlimit(1, 8, numAudioOutPairs);
        numMidiOutPins   = juce::jlimit(1, 16, numMidiOutPins);

        // Initial offsets (block size refined on first runBlock via prepareIfNeeded).
        curBlockSize = 512;
        computeOffsets();
        if (m3_CallV(fnInit)) { error = "WASM ss_init() failed"; return false; }

        loaded = true;
        return true;
    }

    void linkImports() {
        // Warm the factory-waveform bank off the audio thread so that both
        // ss_waveform() (sampleAtPhase) and ss_waveform_id() (indexForName builds
        // its lookup map on first call) are allocation-free when a script later
        // calls them from ss_process(). indexForName("") forces the map build.
        { auto& bank = WaveformBank::get(); bank.ensureLoaded(); bank.indexForName(""); }

        m3_LinkRawFunction(mod, "env", "ss_declare_param", "i(*fff)",
            [](IM3Runtime, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                sp[0] = 0; return m3Err_none; // params unused in runtime mode
            });
        m3_LinkRawFunction(mod, "env", "ss_log", "v(*)",
            [](IM3Runtime r, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                auto* self = (WasmRuntime*)m3_GetUserData(r);
                uint32_t p = (uint32_t)sp[0];
                if (self->mem && p < self->memSize)
                    juce::Logger::writeToLog("[WASM script] " + juce::String((const char*)(self->mem + p)));
                return m3Err_none;
            });
        m3_LinkRawFunction(mod, "env", "ss_get_param", "f(i)",
            [](IM3Runtime, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                *(float*)&sp[0] = 0.0f; return m3Err_none;
            });
        // ss_note_to_freq(midinote) -> Hz, using the PROJECT tuning system
        // (the same mapper backing notefreq() in the other languages). Out-of-
        // range notes return 0. notenum/notename stay WASM-side (pure helpers in
        // soundshop_wasm.h) since they need no host state.
        m3_LinkRawFunction(mod, "env", "ss_note_to_freq", "f(i)",
            [](IM3Runtime r, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                auto* self = (WasmRuntime*)m3_GetUserData(r);
                int n = (int)(int32_t)sp[0];
                float hz = (n < 0 || n > 127) ? 0.0f : self->callNoteFreq(n);
                *(float*)&sp[0] = hz;
                return m3Err_none;
            });
        m3_LinkRawFunction(mod, "env", "ss_midi_out", "v(iiii)",
            [](IM3Runtime r, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                auto* self = (WasmRuntime*)m3_GetUserData(r);
                self->pushMidi(0, (uint32_t)sp[0], (uint8_t)sp[1], (uint8_t)sp[2], (uint8_t)sp[3]);
                return m3Err_none;
            });
        m3_LinkRawFunction(mod, "env", "ss_midi_out_n", "v(iiiii)",
            [](IM3Runtime r, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                auto* self = (WasmRuntime*)m3_GetUserData(r);
                int outIdx = (int)(int32_t)sp[0];
                self->pushMidi(outIdx, (uint32_t)sp[1], (uint8_t)sp[2], (uint8_t)sp[3], (uint8_t)sp[4]);
                return m3Err_none;
            });
        // ss_waveform(id, phase) -> sample: the shared factory-waveform bank the
        // Built-in/Lua/Python/GLSL formula languages also expose. RT-safe (bank
        // warmed above; sampleAtPhase is a const table read).
        m3_LinkRawFunction(mod, "env", "ss_waveform", "f(if)",
            [](IM3Runtime, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                int id = (int)(int32_t)sp[0];
                float phase = *(float*)&sp[1];
                *(float*)&sp[0] = WaveformBank::get().sampleAtPhase(id, phase);
                return m3Err_none;
            });
        // ss_waveform_id(name) -> stable entry index (-1 if unknown).
        m3_LinkRawFunction(mod, "env", "ss_waveform_id", "i(*)",
            [](IM3Runtime r, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                auto* self = (WasmRuntime*)m3_GetUserData(r);
                uint32_t p = (uint32_t)sp[0];
                const char* name = (self->mem && p < self->memSize)
                                   ? (const char*)(self->mem + p) : "";
                int id = WaveformBank::get().indexForName(name);
                sp[0] = (uint64_t)(uint32_t)(int32_t)id;
                return m3Err_none;
            });
        // ss_warpamp(method, x, amount) / ss_warpphase(method, phase, amount) ->
        // the wavetable shape-bending warps as pure scalar functions, routed to
        // the SAME warpAmpValue/warpPhaseValue primitives the editor and synth
        // voice use. `method` is the integer WarpMethod id (resolve a name to an
        // id once with ss_warp_method). RT-safe: pure switch math, no allocation.
        m3_LinkRawFunction(mod, "env", "ss_warpamp", "f(iff)",
            [](IM3Runtime, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                WarpMethod m = (WarpMethod)(int)(int32_t)sp[0];
                float x   = *(float*)&sp[1];
                float amt = *(float*)&sp[2];
                *(float*)&sp[0] = warpAmpValue(m, x, amt);
                return m3Err_none;
            });
        m3_LinkRawFunction(mod, "env", "ss_warpphase", "f(iff)",
            [](IM3Runtime, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                WarpMethod m = (WarpMethod)(int)(int32_t)sp[0];
                float ph  = *(float*)&sp[1];
                float amt = *(float*)&sp[2];
                *(float*)&sp[0] = warpPhaseValue(m, ph, amt);
                return m3Err_none;
            });
        // ss_warp_method(name) -> integer WarpMethod id (0/None if unknown).
        m3_LinkRawFunction(mod, "env", "ss_warp_method", "i(*)",
            [](IM3Runtime r, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                auto* self = (WasmRuntime*)m3_GetUserData(r);
                uint32_t p = (uint32_t)sp[0];
                const char* name = (self->mem && p < self->memSize)
                                   ? (const char*)(self->mem + p) : "";
                sp[0] = (uint64_t)(uint32_t)(int32_t)warpMethodFromName(name);
                return m3Err_none;
            });
        // ss_spectralwarp(ptr, len, method, amount) / ss_waveletwarp(ptr, len,
        // method, amount, filterPtr, levels) -> Bucket C representation-bound
        // warps applied IN PLACE to a float buffer in WASM linear memory (ptr =
        // byte offset of the first float, len = sample count). Whole-buffer ops:
        // FFT-magnitude / DWT-coefficient shaping (see buffer_warp.h). Not RT-hot
        // (allocates a scratch vector) - meant for note-on/block-rate use.
        m3_LinkRawFunction(mod, "env", "ss_spectralwarp", "v(iiif)",
            [](IM3Runtime r, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                auto* self = (WasmRuntime*)m3_GetUserData(r);
                uint32_t ptr = (uint32_t)sp[0];
                int      len = (int)(int32_t)sp[1];
                WarpMethod m = (WarpMethod)(int)(int32_t)sp[2];
                float    amt = *(float*)&sp[3];
                if (!self->mem || len <= 0) return m3Err_none;
                if ((uint64_t)ptr + (uint64_t)len * 4u > self->memSize) return m3Err_none;
                float* base = (float*)(self->mem + ptr);
                std::vector<float> buf(base, base + len);
                spectralWarpBuffer(buf, m, amt);
                for (int i = 0; i < len; ++i) base[i] = buf[(size_t)i];
                return m3Err_none;
            });
        m3_LinkRawFunction(mod, "env", "ss_waveletwarp", "v(iiifii)",
            [](IM3Runtime r, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                auto* self = (WasmRuntime*)m3_GetUserData(r);
                uint32_t ptr       = (uint32_t)sp[0];
                int      len       = (int)(int32_t)sp[1];
                WarpMethod m       = (WarpMethod)(int)(int32_t)sp[2];
                float    amt       = *(float*)&sp[3];
                uint32_t filterPtr = (uint32_t)sp[4];
                int      levels    = (int)(int32_t)sp[5];
                if (!self->mem || len <= 0) return m3Err_none;
                if ((uint64_t)ptr + (uint64_t)len * 4u > self->memSize) return m3Err_none;
                const char* filter = (filterPtr && filterPtr < self->memSize)
                                     ? (const char*)(self->mem + filterPtr) : "db4";
                float* base = (float*)(self->mem + ptr);
                std::vector<float> buf(base, base + len);
                waveletWarpBuffer(buf, m, amt, filter, levels);
                for (int i = 0; i < len; ++i) base[i] = buf[(size_t)i];
                return m3Err_none;
            });

        // ---- Whole-grid terrain generation imports (ss_grid_*) --------------
        // Only meaningful inside ss_generate() (the offline Terrain bake path):
        // the grid is host-owned (genData), so a module reads/writes cells by
        // FLAT index through these calls rather than via linear memory. They
        // mirror the Lua whole-grid API (set/get/coord/coordAxis/neighbor) so the
        // same generator logic reads the same in either language. Outside
        // runGenerate genData is null and every call is a safe no-op / 0.
        m3_LinkRawFunction(mod, "env", "ss_grid_total", "i()",
            [](IM3Runtime r, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                auto* self = (WasmRuntime*)m3_GetUserData(r);
                sp[0] = (uint64_t)(uint32_t)(int32_t)self->genTotal;
                return m3Err_none;
            });
        m3_LinkRawFunction(mod, "env", "ss_grid_nd", "i()",
            [](IM3Runtime r, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                auto* self = (WasmRuntime*)m3_GetUserData(r);
                int nd = self->genDims ? (int)self->genDims->size() : 0;
                sp[0] = (uint64_t)(uint32_t)(int32_t)nd;
                return m3Err_none;
            });
        m3_LinkRawFunction(mod, "env", "ss_grid_dim", "i(i)",
            [](IM3Runtime r, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                auto* self = (WasmRuntime*)m3_GetUserData(r);
                int axis = (int)(int32_t)sp[0];
                int sz = 0;
                if (self->genDims && axis >= 0 && axis < (int)self->genDims->size())
                    sz = (*self->genDims)[(size_t)axis];
                sp[0] = (uint64_t)(uint32_t)(int32_t)sz;
                return m3Err_none;
            });
        // ss_grid_set(i, v): write flat cell i to v, clamped [0,1] (NaN->0). OOB ignored.
        m3_LinkRawFunction(mod, "env", "ss_grid_set", "v(if)",
            [](IM3Runtime r, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                auto* self = (WasmRuntime*)m3_GetUserData(r);
                long long i = (long long)(int32_t)sp[0];
                float v = *(float*)&sp[1];
                if (self->genData && i >= 0 && i < self->genTotal)
                    self->genData[(size_t)i] = juce::jlimit(0.0f, 1.0f, std::isfinite(v) ? v : 0.0f);
                return m3Err_none;
            });
        // ss_grid_get(i): read flat cell i back (0 outside range / before written).
        m3_LinkRawFunction(mod, "env", "ss_grid_get", "f(i)",
            [](IM3Runtime r, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                auto* self = (WasmRuntime*)m3_GetUserData(r);
                long long i = (long long)(int32_t)sp[0];
                float v = 0.0f;
                if (self->genData && i >= 0 && i < self->genTotal) v = self->genData[(size_t)i];
                *(float*)&sp[0] = v;
                return m3Err_none;
            });
        // ss_grid_coord(i, axis): normalised [0,1] position of flat cell i.
        m3_LinkRawFunction(mod, "env", "ss_grid_coord", "f(ii)",
            [](IM3Runtime r, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                auto* self = (WasmRuntime*)m3_GetUserData(r);
                *(float*)&sp[0] = self->gridCoordNorm((long long)(int32_t)sp[0], (int)(int32_t)sp[1]);
                return m3Err_none;
            });
        // ss_grid_coord_axis(i, axis): INTEGER coordinate of flat cell i.
        m3_LinkRawFunction(mod, "env", "ss_grid_coord_axis", "i(ii)",
            [](IM3Runtime r, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                auto* self = (WasmRuntime*)m3_GetUserData(r);
                long long c = self->gridCoordAxis((long long)(int32_t)sp[0], (int)(int32_t)sp[1]);
                sp[0] = (uint64_t)(uint32_t)(int32_t)c;
                return m3Err_none;
            });
        // ss_grid_neighbor(i, axis, delta): flat index delta steps along axis,
        // edge-clamped (compose for N-D stencils; read with ss_grid_get).
        m3_LinkRawFunction(mod, "env", "ss_grid_neighbor", "i(iii)",
            [](IM3Runtime r, IM3ImportContext, uint64_t* sp, void*) -> const void* {
                auto* self = (WasmRuntime*)m3_GetUserData(r);
                long long idx = self->gridNeighbor((long long)(int32_t)sp[0],
                                                   (int)(int32_t)sp[1],
                                                   (long long)(int32_t)sp[2]);
                sp[0] = (uint64_t)(uint32_t)(int32_t)idx;
                return m3Err_none;
            });
    }

    void pushMidi(int outIdx, uint32_t pos, uint8_t status, uint8_t d1, uint8_t d2) {
        if (!mem) return;
        uint32_t count = rmem<uint32_t>(mem, H_MIDI_OUT_COUNT);
        if (count >= MAX_MIDI_EVENTS) return;
        uint32_t off = midiOutOffset + count * MIDI_EVENT_SIZE;
        if (off + MIDI_EVENT_SIZE > memSize) return;
        if (outIdx < 0) outIdx = 0;
        if (outIdx > numMidiOutPins - 1) outIdx = numMidiOutPins - 1;
        wmem<uint32_t>(mem, off + 0, pos);
        mem[off + 4] = status; mem[off + 5] = d1; mem[off + 6] = d2;
        mem[off + 7] = (uint8_t)outIdx;
        wmem<uint32_t>(mem, H_MIDI_OUT_COUNT, count + 1);
    }

    void computeOffsets() {
        uint32_t off = HEADER_SIZE;
        paramOffset = off;     off += MAX_PARAMS * PARAM_STRIDE;
        audioInOffset = off;   off += (uint32_t)numAudioInPairs * 2 * (uint32_t)curBlockSize * sizeof(float);
        audioOutOffset = off;  off += (uint32_t)numAudioOutPairs * 2 * (uint32_t)curBlockSize * sizeof(float);
        midiInOffset = off;    off += MAX_MIDI_EVENTS * MIDI_EVENT_SIZE;
        midiOutOffset = off;   off += MAX_MIDI_EVENTS * MIDI_EVENT_SIZE;
    }

    void prepareIfNeeded(const ScriptBlockCtx& ctx) {
        if (ctx.sampleRate == curSampleRate && ctx.numSamples <= curBlockSize) return;
        curSampleRate = ctx.sampleRate;
        curBlockSize  = juce::jmax(curBlockSize, ctx.numSamples);
        computeOffsets();
        mem = m3_GetMemory(rt, &memSize, 0); // may have grown
        if (fnPrepare) m3_CallV(fnPrepare);
    }

    void writeHeader(const ScriptBlockCtx& c) {
        wmem<uint32_t>(mem, H_MAGIC, MAGIC);
        wmem<uint32_t>(mem, H_VERSION, 1u);
        wmem<uint32_t>(mem, H_BLOCK_SIZE, (uint32_t)c.numSamples);
        wmem<float>   (mem, H_SAMPLE_RATE, (float)c.sampleRate);
        wmem<float>   (mem, H_BPM, (float)c.bpm);
        wmem<double>  (mem, H_BEAT_POS, c.posBeats);
        wmem<uint32_t>(mem, H_TRANSPORT_FLAGS, c.playing ? 0x01u : 0u);
        wmem<uint32_t>(mem, H_NUM_AUDIO_IN, (uint32_t)numAudioInPairs);
        wmem<uint32_t>(mem, H_NUM_AUDIO_OUT, (uint32_t)numAudioOutPairs);
        wmem<uint32_t>(mem, H_NUM_PARAMS, 0u);
        wmem<uint32_t>(mem, H_AUDIO_IN_OFF, audioInOffset);
        wmem<uint32_t>(mem, H_AUDIO_OUT_OFF, audioOutOffset);
        wmem<uint32_t>(mem, H_PARAM_OFF, paramOffset);
        wmem<uint32_t>(mem, H_MIDI_IN_OFF, midiInOffset);
        wmem<uint32_t>(mem, H_MIDI_OUT_OFF, midiOutOffset);
        wmem<uint32_t>(mem, H_MIDI_IN_COUNT, 0u);
    }

    // Signal role: write each signal input pin into a flat audio-in channel.
    void writeSignalInputs(const ScriptBlockCtx& c) {
        if (role != ScriptRole::Signal || !c.sig) return;
        int numChans = numAudioInPairs * 2;
        for (int k = 0; k < c.sigCount && k < numChans; ++k) {
            const float* p = (k < (int)c.sig->size()) ? (*c.sig)[k] : nullptr;
            uint32_t off = audioInOffset + (uint32_t)k * (uint32_t)curBlockSize * sizeof(float);
            if (off + (uint32_t)c.numSamples * sizeof(float) > memSize) break;
            if (p) std::memcpy(mem + off, p, (size_t)c.numSamples * sizeof(float));
            else   std::memset(mem + off, 0, (size_t)c.numSamples * sizeof(float));
        }
    }

    // Signal role: read each flat audio-out channel back into its output pin.
    // Output pin p (0-based o1..oP) maps to flat audio-out channel p, mirroring
    // writeSignalInputs (pin k -> audio-in channel k). When the host hands us a
    // multi-output buffer array (ctx.outs) we fill each pin independently - the
    // WASM analogue of the Lua stream() out(pin,v); otherwise we fall back to the
    // single ctx.out (channel 0). Control signals are unipolar 0..1 on the wire
    // (see signal_modulation.h), so a signal-role module must write 0..1 into its
    // output channels; we clamp here to enforce that, matching out()'s clamp.
    void readSignalOutput(const ScriptBlockCtx& c) {
        int pins = (c.outs && c.outCount > 0) ? c.outCount : (c.out ? 1 : 0);
        for (int p = 0; p < pins; ++p) {
            float* dst = (c.outs && p < c.outCount && p < (int)c.outs->size())
                       ? (*c.outs)[(size_t)p] : (p == 0 ? c.out : nullptr);
            if (!dst) continue;
            uint32_t off = audioOutOffset + (uint32_t)p * (uint32_t)curBlockSize * sizeof(float);
            if (off + (uint32_t)c.numSamples * sizeof(float) <= memSize) {
                const float* src = reinterpret_cast<const float*>(mem + off);
                for (int i = 0; i < c.numSamples; ++i)
                    dst[i] = juce::jlimit(0.0f, 1.0f, src[i]);
            } else {
                std::memset(dst, 0, (size_t)c.numSamples * sizeof(float));
            }
        }
    }

    // Write the block's MIDI-input events into the shared midiIn region in the
    // standard raw-MIDI layout (pos, status, d1, d2, inIdx), so a WASM module can
    // consume MIDI input with the same soundshop_wasm.h helpers the standalone
    // Script (WASM) node uses. ScriptMidiEvent is typed (kind/a/b); we translate
    // back to raw status/data bytes. Byte 7 carries the 0-based MIDI-input pin so
    // the module sees the same per-input index the Lua pollmidi() idx exposes.
    void writeMidiInputs(const ScriptBlockCtx& c) {
        uint32_t count = 0;
        if (c.midiIn) {
            int n = juce::jmin(c.midiInCount, (int)MAX_MIDI_EVENTS);
            for (int i = 0; i < n; ++i) {
                const ScriptMidiEvent& e = (*c.midiIn)[(size_t)i];
                uint32_t off = midiInOffset + count * MIDI_EVENT_SIZE;
                if (off + MIDI_EVENT_SIZE > memSize) break;
                uint8_t status = 0, d1 = 0, d2 = 0;
                int note = (int)e.a;
                int vel  = juce::jlimit(0, 127, (int)std::lround(e.b * 127.0f));
                switch (e.kind) {
                    case 1: status = 0x90; d1 = (uint8_t)juce::jlimit(0,127,note); d2 = (uint8_t)juce::jmax(1, vel); break; // on
                    case 0: status = 0x80; d1 = (uint8_t)juce::jlimit(0,127,note); d2 = 0; break;                          // off
                    case 2: status = 0xB0; d1 = (uint8_t)juce::jlimit(0,127,note); d2 = (uint8_t)vel; break;               // cc
                    default: { // bend: b in -1..1 -> 14-bit
                        int b14 = juce::jlimit(0, 16383, (int)std::lround((e.b + 1.0f) * 8192.0f));
                        status = 0xE0; d1 = (uint8_t)(b14 & 0x7F); d2 = (uint8_t)((b14 >> 7) & 0x7F);
                    } break;
                }
                wmem<uint32_t>(mem, off + 0, (uint32_t)juce::jmax(0, e.offset));
                mem[off + 4] = status; mem[off + 5] = d1; mem[off + 6] = d2;
                mem[off + 7] = (uint8_t)juce::jlimit(0, 255, e.inIndex);
                count++;
            }
        }
        wmem<uint32_t>(mem, H_MIDI_IN_COUNT, count);
    }

    // MIDI role: drain the script's MIDI-out events into ctx.sink, stamping each
    // with its sample offset. Multi-output scripts re-stamp the channel nibble so
    // the host's per-output filter can split them.
    void emitMidiOut(const ScriptBlockCtx& c) {
        if (!c.sink) return;
        uint32_t count = rmem<uint32_t>(mem, H_MIDI_OUT_COUNT);
        count = juce::jmin(count, MAX_MIDI_EVENTS);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t off = midiOutOffset + i * MIDI_EVENT_SIZE;
            if (off + MIDI_EVENT_SIZE > memSize) break;
            uint32_t pos  = rmem<uint32_t>(mem, off + 0);
            uint8_t status = mem[off + 4];
            uint8_t d1 = mem[off + 5];
            uint8_t d2 = mem[off + 6];
            uint8_t outIdx = mem[off + 7];
            int sampleOff = (c.numSamples > 0) ? (int)juce::jlimit<uint32_t>(0, (uint32_t)c.numSamples - 1, pos) : 0;
            c.sink->setSampleOffset(sampleOff);

            // Translate raw status/data into the sink's typed emit calls.
            int type = status & 0xF0;
            if (type == 0x90 && d2 > 0)        c.sink->emitNoteOn(outIdx, (float)d1, (float)d2);
            else if (type == 0x80 || (type == 0x90 && d2 == 0))
                                               c.sink->emitNoteOff(outIdx, (float)d1);
            else if (type == 0xB0)             c.sink->emitCC(outIdx, (float)d1, d2 / 127.0f);
            else if (type == 0xE0) {
                int bend14 = (d1 & 0x7F) | ((d2 & 0x7F) << 7);
                c.sink->emitBend(outIdx, (bend14 - 8192) / 8192.0f);
            }
        }
    }
#endif // HAS_WASM3
};

std::unique_ptr<IScriptRuntime> makeWasmRuntime(ScriptRole role, ScriptRate /*rate*/) {
    return std::make_unique<WasmRuntime>(role);
}

bool wasmRuntimeAvailable() {
#if HAS_WASM3
    return true;
#else
    return false;
#endif
}

} // namespace SoundShop

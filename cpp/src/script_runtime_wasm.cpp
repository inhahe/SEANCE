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
// Role mapping for the two runtime uses:
//   * Signal role (LFO): the host writes each signal input pin into a flat
//     audio-in channel and reads audio-out channel 0 back into ctx.out.
//   * MIDI role: the host ignores audio and routes ss_midi_out* events into
//     ctx.sink, stamping each with its sample offset.
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
        juce::File f(juce::String(path));
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
        if (!loaded || !mem) {
            if (ctx.out) std::memset(ctx.out, 0, sizeof(float) * (size_t)ctx.numSamples);
            return;
        }
        prepareIfNeeded(ctx);
        writeHeader(ctx);
        writeSignalInputs(ctx);

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

    bool supportsPerSample() const override { return false; }
    bool supportsPerBlock()  const override { return true; }
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
    IM3Function    fnInit = nullptr, fnProcess = nullptr, fnPrepare = nullptr;

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
        if (!fnInit || !fnProcess) {
            error = "WASM missing required exports ss_init / ss_process";
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

    // Signal role: read audio-out channel 0 back into ctx.out. Control signals
    // are unipolar 0..1 on the wire (see signal_modulation.h), so a signal-role
    // module must write 0..1 into its output channel; we clamp here to enforce
    // that, matching the Lua runtime's out() clamp.
    void readSignalOutput(const ScriptBlockCtx& c) {
        if (!c.out) return;
        uint32_t off = audioOutOffset; // pair 0, channel 0
        if (off + (uint32_t)c.numSamples * sizeof(float) <= memSize) {
            const float* src = reinterpret_cast<const float*>(mem + off);
            for (int i = 0; i < c.numSamples; ++i)
                c.out[i] = juce::jlimit(0.0f, 1.0f, src[i]);
        } else {
            std::memset(c.out, 0, (size_t)c.numSamples * sizeof(float));
        }
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

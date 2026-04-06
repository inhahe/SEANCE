#pragma once
#include "node_graph.h"
#include "transport.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>
#include <string>

// Forward-declare wasm3 types so we don't pull the header into every TU
struct M3Environment;  typedef M3Environment* IM3Environment;
struct M3Runtime;      typedef M3Runtime*     IM3Runtime;
struct M3Module;       typedef M3Module*      IM3Module;
struct M3Function;     typedef M3Function*    IM3Function;

namespace SoundShop {

class WasmScriptProcessor : public juce::AudioProcessor {
public:
    WasmScriptProcessor(Node& node, Transport& transport);
    ~WasmScriptProcessor() override;

    // Load a .wasm binary. Returns true on success.
    // Must be called from the message thread before audio starts.
    bool loadWasm(const std::vector<uint8_t>& wasmBytes);
    bool isLoaded() const { return loaded; }

    // After loading, populates the node's pins/params based on what the script declared.
    void populateNodePins(Node& node);

    // --- juce::AudioProcessor ---
    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double sr, int bs) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;
    double getTailLengthSeconds() const override;
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
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    // Parameter declarations collected during ss_init()
    struct ParamDecl {
        std::string name;
        float defaultVal, minVal, maxVal;
    };
    const std::vector<ParamDecl>& getDeclaredParams() const { return paramDecls; }

private:
    Node& node;
    Transport& transport;
    double sampleRate = 44100;
    int blockSize = 512;
    bool loaded = false;

    // wasm3 state
    IM3Environment wasmEnv = nullptr;
    IM3Runtime wasmRuntime = nullptr;
    IM3Module wasmModule = nullptr;

    // Cached function pointers
    IM3Function fnInit = nullptr;
    IM3Function fnProcess = nullptr;
    IM3Function fnPrepare = nullptr;

    // Script's declared params
    std::vector<ParamDecl> paramDecls;

    // I/O counts (from script or defaults)
    int numAudioInPairs = 1;
    int numAudioOutPairs = 1;

    // Direct pointer into WASM linear memory
    uint8_t* wasmMem = nullptr;
    uint32_t wasmMemSize = 0;

    // Computed offsets into WASM memory
    uint32_t paramOffset = 0;
    uint32_t audioInOffset = 0;
    uint32_t audioOutOffset = 0;
    uint32_t midiInOffset = 0;
    uint32_t midiOutOffset = 0;

    void computeOffsets();
    void writeHeader();
    void copyAudioIn(const juce::AudioBuffer<float>& buf);
    void copyAudioOut(juce::AudioBuffer<float>& buf);
    void copyMidiIn(const juce::MidiBuffer& midi);
    void copyMidiOut(juce::MidiBuffer& midi);
    void writeParams();

    // Host import trampolines (called from WASM)
    // These use the wasm3 raw function signature convention
    friend struct WasmHostCallbacks;
};

} // namespace SoundShop

#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>
#include <string>
#include <map>
#include <memory>

// Forward declare TSF handle
struct tsf;

namespace SoundShop {

// ==============================================================================
// SFZ parser: reads .sfz text files into a multi-sample instrument definition.
// Supports the most common opcodes that cover ~90% of SFZ files.
// ==============================================================================

struct SFZRegion {
    std::string samplePath;
    int lokey = 0, hikey = 127;
    int lovel = 0, hivel = 127;
    int pitchKeycenter = 60;    // MIDI note the sample was recorded at
    float volume = 0.0f;        // dB offset
    float tune = 0.0f;          // cents fine-tune
    float ampegAttack = 0.001f;
    float ampegDecay = 0.0f;
    float ampegSustain = 100.0f; // percent
    float ampegRelease = 0.01f;
    int loopMode = 0;           // 0=no_loop, 1=loop_continuous, 2=loop_sustain
    int loopStart = 0, loopEnd = 0;
    int offset = 0;             // sample start offset

    // Loaded audio data (filled after parsing)
    std::vector<float> samples;
    double sampleRate = 44100;
    int numSamples = 0;
};

struct SFZInstrument {
    std::vector<SFZRegion> regions;
    std::string basePath; // directory containing the .sfz file

    bool loadFromFile(const std::string& path);

    // Find matching regions for a note-on event
    std::vector<const SFZRegion*> findRegions(int midiNote, int velocity) const;
};

// ==============================================================================
// Unified SoundFont processor: handles both .sf2 (via TSF) and .sfz files.
// Receives MIDI, outputs audio.
// ==============================================================================

class SoundFontProcessor : public juce::AudioProcessor {
public:
    SoundFontProcessor(Node& node);
    ~SoundFontProcessor() override;

    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double sr, int bs) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override;
    double getTailLengthSeconds() const override { return 2.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
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

    // Get preset names (SF2 only — SF2 files contain multiple presets)
    std::vector<std::string> getPresetNames() const;
    int getCurrentPreset() const { return currentPreset; }
    void setPreset(int idx);

    bool isSF2() const { return sf2 != nullptr; }
    bool isSFZ() const { return !sfz.regions.empty(); }

private:
    Node& node;
    double sampleRate = 44100;

    // SF2 via tinysoundfont
    tsf* sf2 = nullptr;
    int currentPreset = 0;

    // SFZ
    SFZInstrument sfz;

    // SFZ voice management
    struct SFZVoice {
        bool active = false;
        int midiNote = -1;
        const SFZRegion* region = nullptr;
        double phase = 0;       // sample playback position
        float envLevel = 0;
        enum Stage { Attack, Decay, Sustain, Release, Off };
        Stage envStage = Off;
        double envTime = 0;
        float velocity = 1.0f;
    };
    static constexpr int MAX_SFZ_VOICES = 32;
    SFZVoice sfzVoices[MAX_SFZ_VOICES];

    void loadFile();
};

} // namespace SoundShop

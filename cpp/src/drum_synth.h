#pragma once
#include "node_graph.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <vector>
#include <map>
#include <random>

namespace SoundShop {

class AudioEngine; // forward decl — only used as a pointer in the editor

// ==============================================================================
// Analog drum synthesis — each sound is a simple oscillator + envelope
// ==============================================================================

// The type determines the synthesis algorithm
enum class DrumType {
    Kick,       // pitch-sweeping sine + click
    Snare,      // sine + noise
    HiHat,      // metallic squares + noise (short or long decay)
    Clap,       // multiple noise bursts
    Tom,        // pitch-sweeping sine (like kick but configurable)
    Cowbell,    // dual inharmonic squares
    Rimshot,    // short noise + tone
    Cymbal,     // crash/ride/bell — tone slider controls character
                // (0=crash: noisy wash, 0.5=ride: metallic ring, 1=bell: bright ping)
};

struct DrumSound {
    std::string name;
    DrumType type = DrumType::Kick;
    int midiNote = 36;     // which MIDI note triggers this sound

    // Tunable params
    float pitch = 1.0f;    // multiplier on base frequency
    float decay = 1.0f;    // multiplier on base decay time
    float tone  = 0.5f;    // 0 = pure noise, 1 = pure tone, 0.5 = mixed
    float level = 0.8f;    // volume
    float size  = 0.5f;    // cymbal only: 0=small(8"), 0.5=medium(16"), 1=large(24")
};

// A single active drum voice
struct DrumVoice {
    bool active = false;
    int soundIdx = -1;
    double time = 0;        // seconds since trigger
    float velocity = 1.0f;
    std::mt19937 rng{42};
};

class DrumSynthProcessor : public juce::AudioProcessor {
public:
    DrumSynthProcessor(Node& node);
    const juce::String getName() const override { return node.name; }
    void prepareToPlay(double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}
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

    // Access for the editor
    std::vector<DrumSound>& getSounds() { return sounds; }
    void syncFromParams();
    void syncToParams();

private:
    Node& node;
    double sampleRate = 44100;
    std::vector<DrumSound> sounds;
    static constexpr int MAX_DRUM_VOICES = 16;
    DrumVoice voices[MAX_DRUM_VOICES];

    int findSoundForNote(int midiNote);
    float renderDrumSample(int soundIdx, DrumVoice& voice);
};

// ==============================================================================
// Editor UI: shows all drum sounds with Learn buttons and tuning params
// ==============================================================================

class DrumSynthEditorComponent : public juce::Component, private juce::Timer {
public:
    DrumSynthEditorComponent(NodeGraph& graph, int nodeId, AudioEngine* engine);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override { repaint(); }

private:
    NodeGraph& graph;
    int nodeId;
    AudioEngine* audioEngine;
    int learnIdx = -1;

    juce::TextButton addSoundBtn{"+ Add Sound"};
    juce::TextButton closeBtn{"Close"};
    juce::Viewport scrollView;
    juce::Component scrollContent;

    struct SoundRow {
        juce::Label nameLabel, noteLabel;
        juce::ComboBox typeCombo;
        juce::TextButton learnBtn{"Learn"};
        juce::TextButton deleteBtn{"X"};
        juce::Slider pitchSlider, decaySlider, toneSlider, levelSlider, sizeSlider;
        juce::Label sizeLbl;
        bool isCymbal = false;
    };
    std::vector<std::unique_ptr<SoundRow>> rows;

    void rebuildRows();
    void onMidiNote(int note);
    void addSound(DrumType type, const std::string& name, int note);
    void deleteSound(int index);
};

} // namespace SoundShop

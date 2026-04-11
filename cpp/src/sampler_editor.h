#pragma once
#include "node_graph.h"
#include "pitch_detect.h"
#include "audio_engine.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>

namespace SoundShop {

// Editor for the Sampler instrument. Shows:
// - Waveform display of the loaded sample
// - Dual pitch detection (autocorrelation + YIN) with A/B preview
// - Base note setting (manual or auto-detected)
// - Auto-tune toggle (snap to nearest note + cents correction)
// - Fine-tune slider (manual cents adjustment)
// - Pitch method choice (resample vs pitch-shift)

class SamplerEditorComponent : public juce::Component {
public:
    SamplerEditorComponent(NodeGraph& graph, int nodeId, AudioEngine& audioEngine);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    NodeGraph& graph;
    int nodeId;
    AudioEngine& audioEngine;

    // Loaded sample data (for display + analysis)
    std::vector<float> sampleData;
    double fileSampleRate = 44100;

    // Pitch detection results
    PitchResult autoCorResult, yinResult;
    bool analysisRun = false;

    // UI controls
    juce::Label titleLabel;
    juce::TextButton analyzeBtn{"Detect Pitch"};
    juce::TextButton loadBtn{"Load Sample..."};

    // Detection results display
    juce::Label acLabel, yinLabel;  // show detected freq + note

    // Preview buttons
    juce::TextButton playACBtn{"Play Autocorrelation"};
    juce::TextButton playYINBtn{"Play YIN"};
    juce::TextButton playSineBtn{"Play Sine Reference"};
    juce::TextButton useACBtn{"Use This"};
    juce::TextButton useYINBtn{"Use This"};

    // Base note
    juce::ComboBox baseNoteCombo;
    juce::Label baseNoteLbl;

    // Fine-tune
    juce::Slider fineTuneSlider;
    juce::Label fineTuneLbl;
    juce::ToggleButton autoTuneToggle{"Auto-tune to nearest note"};

    // Pitch method
    juce::ComboBox pitchMethodCombo;
    juce::Label pitchMethodLbl;

    // Apply / Close
    juce::TextButton applyBtn{"Apply"};
    juce::TextButton closeBtn{"Close"};

    void loadSample();
    void runAnalysis();
    void applyDetection(const PitchResult& result);
    void commitToNode();
    void playPreview(float frequencyHz);
    void playSinePreview(float frequencyHz);
};

} // namespace SoundShop

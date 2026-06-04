#pragma once
#include "node_graph.h"
#include "multi_sampler.h"
#include "audio_engine.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace SoundShop {

// Editor for MultiSampler nodes. Shows a zone list on the left, a
// per-zone parameter panel on the right, and global ADSR + filter +
// volume/pan controls across the top. Commits changes back to
// node.script via MultiSamplerDoc::encode() on every edit so the
// processor picks them up on its next block without needing a full
// graph rebuild.
class MultiSamplerEditorComponent : public juce::Component,
                                     public juce::ListBoxModel {
public:
    MultiSamplerEditorComponent(NodeGraph& graph, int nodeId, AudioEngine& audioEngine);

    void paint(juce::Graphics& g) override;
    void resized() override;

    // ListBoxModel
    int  getNumRows() override;
    void paintListBoxItem(int rowNumber, juce::Graphics& g,
                           int width, int height, bool rowIsSelected) override;
    void selectedRowsChanged(int lastRowSelected) override;

private:
    NodeGraph& graph;
    int nodeId;
    AudioEngine& audioEngine;
    MultiSamplerDoc doc;
    int selectedZone = 0;

    // ---- top row (global) ----
    juce::Label titleLabel;
    juce::TextButton closeBtn { "Close" };

    // Global volume / pan.
    juce::Slider volumeSlider;
    juce::Label  volumeLabel;
    juce::Slider panSlider;
    juce::Label  panLabel;

    // Global ADSR.
    juce::Slider attackSlider, decaySlider, sustainSlider, releaseSlider;
    juce::Label  attackLabel,  decayLabel,  sustainLabel,  releaseLabel;

    // Filter.
    juce::Slider filterCutoffSlider, filterResSlider;
    juce::Label  filterCutoffLabel,  filterResLabel;
    juce::ComboBox filterModeCombo;
    juce::Label    filterModeLabel;

    // ---- zone list ----
    juce::ListBox zoneList;
    juce::TextButton addZoneBtn    { "+ Zone" };
    juce::TextButton removeZoneBtn { "- Zone" };

    // ---- per-zone editor ----
    juce::Label  zonePathLabel;
    juce::TextButton loadSampleBtn { "Load..." };
    juce::Label  zonePathValue;

    juce::ComboBox baseNoteCombo;
    juce::Label    baseNoteLabel;
    juce::Slider   fineTuneSlider;
    juce::Label    fineTuneLabel;

    juce::ComboBox loNoteCombo, hiNoteCombo;
    juce::Label    loNoteLabel, hiNoteLabel;

    juce::Slider   loVelSlider, hiVelSlider;
    juce::Label    loVelLabel,  hiVelLabel;

    juce::Slider   zoneGainSlider, zonePanSlider;
    juce::Label    zoneGainLabel,  zonePanLabel;

    // Helpers.
    void reloadFromNode();
    void commit();                 // write doc back to node.script
    void refreshZonePanel();        // push zone fields into UI controls
    void populateNoteCombo(juce::ComboBox& c) const;

    void loadSampleForSelectedZone();
    void addZone();
    void removeSelectedZone();
};

} // namespace SoundShop

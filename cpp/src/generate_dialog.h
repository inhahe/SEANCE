#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "terrain_synth.h"
#include <functional>
#include <vector>

namespace SoundShop {

// ============================================================================
// GenerateDialogComponent - UI for Terrain Synth's "generate from program"
// feature (see terrain_synth.h GenerateTerrainParams / makeGenerateTerrainScript).
//
// The user picks a generation language (Builtin math / Lua / Python), a mode
// (per-cell or whole-grid), a grid shape of any rank (comma-separated dimension
// sizes), and writes a program that produces one value in [0,1] per cell. The
// dialog validates on a tiny probe grid, then generates the FULL-resolution grid
// on the message thread and bakes the computed data into the
// GenerateTerrainParams it hands back via applyFn. The host stores that (program
// + baked data) in the node's __generate__ script, so the terrain never re-runs
// the generator on load (essential for Python, which can't run on the audio
// thread where the processor is built).
//
// Two modes:
//   - CREATE (lockRank=false): the host has not made a node yet; the rank
//     (number of dimensions) is editable. On Generate the host creates a new
//     terrain node of the chosen rank.
//   - EDIT   (lockRank=true): re-opened on an existing __generate__ node. The
//     rank is fixed to the node's current axis count (changing it would have to
//     rebuild the node's Sig pins + Center/Radius params and drop cables); the
//     user can still change dimension SIZES, the language, and the program.
// ============================================================================
class GenerateDialogComponent : public juce::Component {
public:
    GenerateDialogComponent(const GenerateTerrainParams& seed,
                            bool lockRank,
                            std::function<void(const GenerateTerrainParams&)> applyFn);

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void rebuildLangChoices();
    void onLanguageChanged();
    void refreshModeAvailability();    // grey the mode the language can't do
    void updateProgramUiForLanguage(); // code editor vs .wasm file picker
    void browseForWasm();              // async file chooser for a .wasm module
    void refreshPassesVisibility();    // show Passes only for GLSL whole-grid
    int  selectedMode() const;         // 0 = per-cell, 1 = whole-grid
    int  selectedPasses() const;       // ping-pong passes (>=1), GLSL whole-grid only
    void insertTemplate();             // seed the editor with a default program
    void insertTemplateIfDefault();    // swap template only if editor holds a default
    bool parseDims(std::vector<int>& out, std::string& err) const;
    void doGenerate();               // validate + applyFn + close
    void closeSelf();

    bool lockRank;
    std::function<void(const GenerateTerrainParams&)> applyFn;

    // Languages offered in the combo, in display order. Stores the ScriptLang
    // int for each row so selection maps back to a language.
    std::vector<int> langValues;

    juce::Label      titleLabel;
    juce::Label      langLabel;
    juce::ComboBox   langCombo;
    juce::Label      modeLabel;
    juce::ComboBox   modeCombo;
    juce::Label      passesLabel;     // "Passes:" (GLSL whole-grid only)
    juce::TextEditor passesEditor;    // ping-pong iteration count
    juce::Label      dimsLabel;
    juce::TextEditor dimsEditor;
    juce::Label      dimsHint;
    juce::Label      scriptLabel;
    juce::TextEditor scriptEditor;   // multiline program source
    juce::TextButton templateBtn { "Insert example" };
    juce::Label      statusLabel;
    juce::TextButton generateBtn { "Generate" };
    juce::TextButton cancelBtn   { "Cancel" };

    // Async file chooser for the WASM "Browse .wasm..." button (kept alive while
    // the native dialog is open).
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GenerateDialogComponent)
};

} // namespace SoundShop

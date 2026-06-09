#pragma once
#include "node_graph.h"
#include "midi_script_node.h"
#include "layered_wave_editor.h"   // LayerStackComponent for the shape table
#include "script_lang_bar.h"       // ScriptLangBar + WasmFilePanel
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>

namespace SoundShop {

// -----------------------------------------------------------------------------
// MidiScriptEditorComponent
// -----------------------------------------------------------------------------
//
// Modeless editor for a MIDI Script node. Edits node.script in place via
// MidiScriptDoc::encode/decode and fires onChanged after every mutation
// (caller rebuilds the audio graph + flags the project dirty).
//
// Layout (top -> bottom):
//   - Program editor (large multi-line text box). The mini-language: statements
//     separated by ';' / newlines; assignments persist across samples; emit
//     functions note/cc/bend push MIDI.
//   - Signal input count (0..16) and MIDI output count (1..16) spinners; both
//     rewrite the node's pins.
//   - Optional shape editor (embedded LayerStackComponent) sampled by shape().
//   - "Language reference" help button + Close.
class MidiScriptEditorComponent : public juce::Component {
public:
    MidiScriptEditorComponent(NodeGraph& graph, int nodeId,
                              std::function<void()> onChanged);
    ~MidiScriptEditorComponent() override;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    NodeGraph& graph;
    int nodeId;
    std::function<void()> onChanged;

    MidiScriptDoc doc;

    std::unique_ptr<ScriptLangBar>  langBar;    // language + rate combos
    std::unique_ptr<WasmFilePanel>  wasmPanel;  // shown in place of the editor for Wasm

    juce::Label    programLabel { {}, "Program (runs once per sample):" };
    juce::TextEditor programEditor;

    juce::Label    sigCountLabel { {}, "Signal inputs (s1..sN):" };
    juce::TextEditor sigCountEditor;
    juce::Label    outCountLabel { {}, "MIDI outputs:" };
    juce::TextEditor outCountEditor;

    juce::Label    shapeLabel { {}, "Shape (sampled by shape(pos)):" };
    std::unique_ptr<LayerStackComponent> shapeStack;

    juce::TextButton helpBtn  { "Language reference\u2026" };
    juce::TextButton closeBtn { "Close" };

    void loadFromNode();
    void commitToNode();
    // Show the program editor (Builtin / Lua) or the .wasm file panel (Wasm),
    // and update the program-editor caption to match the current language/rate.
    void updateLanguageUI();
    // Rewrite node.pinsIn ("MIDI In" + N Signal) and node.pinsOut (M "MIDI Out k")
    // to match doc.signalInputCount / doc.outputCount, preserving pin ids where
    // possible so existing cables survive. Returns true if anything changed.
    bool syncPins();
};

} // namespace SoundShop

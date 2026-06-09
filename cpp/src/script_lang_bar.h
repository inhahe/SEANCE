#pragma once
#include "script_runtime.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <string>

namespace SoundShop {

// -----------------------------------------------------------------------------
// ScriptLangBar
// -----------------------------------------------------------------------------
//
// A reusable control strip shown at the top of the MIDI Script and Signal Shape
// editors. It lets the user choose:
//   * the scripting Language  (Builtin / Lua / Wasm)
//   * the execution Rate      (Per sample / Per block)
//
// and surfaces an inline warning when Lua + per-sample is selected (per-sample
// Lua calls the interpreter tens of thousands of times a second, which can
// stutter the audio thread).
//
// The bar enforces the capability matrix (script_runtime.h):
//   Builtin -> per-sample only  (rate combo forced + disabled)
//   Lua     -> per-sample OR per block  (rate combo enabled)
//   Wasm    -> per-block only    (rate combo forced + disabled, with a tooltip
//              explaining why per-sample isn't offered)
// Languages that aren't compiled into this build appear greyed-out in the combo
// so users can see they exist.
//
// The bar owns only the two combos + the warning line; it does NOT own the
// program-text editor or the .wasm path string. It reports changes through the
// Callbacks struct and the host editor updates its doc + commits.
class ScriptLangBar : public juce::Component {
public:
    struct Callbacks {
        std::function<void(ScriptLang)> onLanguage; // language combo changed
        std::function<void(ScriptRate)> onRate;     // rate combo changed
    };

    explicit ScriptLangBar(Callbacks cbs);

    // Push current doc state into the controls (call after loadFromNode). Does
    // NOT fire the callbacks. Clamps rate to a value the language supports.
    void setState(ScriptLang lang, ScriptRate rate);

    ScriptLang language() const { return lang; }
    ScriptRate rate() const { return rate_; }

    // Fixed height the host should reserve for this bar (combos row + the inline
    // warning line, which is always reserved so the layout doesn't jump).
    static constexpr int kHeight = 24 + 6 + 34;

    void resized() override;

private:
    void refresh();          // sync combo selections + enablement + warning
    Callbacks cb;

    ScriptLang lang = ScriptLang::Builtin;
    ScriptRate rate_ = ScriptRate::PerSample;

    juce::Label    langLabel { {}, "Language:" };
    juce::ComboBox langCombo;
    juce::Label    rateLabel { {}, "Run:" };
    juce::ComboBox rateCombo;
    juce::Label    warningLabel;   // Lua+per-sample caution (empty otherwise)
};

// -----------------------------------------------------------------------------
// WasmFilePanel
// -----------------------------------------------------------------------------
//
// Shown in place of the program-text editor when the language is Wasm. Displays
// the currently-chosen .wasm path (or a hint that none is chosen) plus a
// "Choose .wasm file..." button that opens the host OS's native file dialog
// (JUCE FileChooser). The native dialog is an OS-owned window, so it does not
// add a second SEANCE entry to the taskbar.
//
// The panel owns the displayed path string for rendering only; it reports the
// chosen path through onPathChanged and the host updates its doc + commits.
class WasmFilePanel : public juce::Component {
public:
    explicit WasmFilePanel(std::function<void(std::string)> onPathChanged);

    void setPath(const std::string& p);

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void choose();

    std::function<void(std::string)> onPath;
    std::string path;

    juce::Label      pathLabel;
    juce::TextButton chooseBtn { "Choose .wasm file\u2026" };
    juce::Label      hintLabel;
    std::shared_ptr<juce::FileChooser> chooser;
};

} // namespace SoundShop

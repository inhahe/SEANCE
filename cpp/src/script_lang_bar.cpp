#include "script_lang_bar.h"

namespace SoundShop {

// Combo item ids (1-based; JUCE reserves 0 for "nothing selected").
namespace {
constexpr int kLangBuiltin = 1;
constexpr int kLangLua     = 2;
constexpr int kLangWasm    = 3;
constexpr int kRatePerSample = 1;
constexpr int kRatePerBlock  = 2;

ScriptLang langFromId(int id) {
    switch (id) {
        case kLangLua:  return ScriptLang::Lua;
        case kLangWasm: return ScriptLang::Wasm;
        default:        return ScriptLang::Builtin;
    }
}
int idFromLang(ScriptLang l) {
    switch (l) {
        case ScriptLang::Lua:  return kLangLua;
        case ScriptLang::Wasm: return kLangWasm;
        default:               return kLangBuiltin;
    }
}
} // namespace

// =============================================================================
// ScriptLangBar
// =============================================================================
ScriptLangBar::ScriptLangBar(Callbacks cbs) : cb(std::move(cbs)) {
    addAndMakeVisible(langLabel);
    addAndMakeVisible(langCombo);
    addAndMakeVisible(rateLabel);
    addAndMakeVisible(rateCombo);
    addAndMakeVisible(warningLabel);

    langCombo.addItem("Built-in (expression)", kLangBuiltin);
    langCombo.addItem("Lua", kLangLua);
    langCombo.addItem("WebAssembly (.wasm)", kLangWasm);
    // Grey out backends that aren't compiled into this build, but keep them
    // listed so the user can see the feature exists.
    langCombo.setItemEnabled(kLangLua,  scriptLangAvailable(ScriptLang::Lua));
    langCombo.setItemEnabled(kLangWasm, scriptLangAvailable(ScriptLang::Wasm));
    langCombo.setTooltip("Which language runs this node's program.\n"
                         "Built-in: the SEANCE expression language (always per sample).\n"
                         "Lua: a full scripting language (per sample or per block).\n"
                         "WebAssembly: a compiled .wasm binary you supply (per block).");
    langCombo.onChange = [this]() {
        ScriptLang newLang = langFromId(langCombo.getSelectedId());
        if (newLang == lang) return;
        lang = newLang;
        // Snap the rate to one the new language supports.
        if (!scriptLangSupportsRate(lang, rate_))
            rate_ = (lang == ScriptLang::Wasm) ? ScriptRate::PerBlock
                                               : ScriptRate::PerSample;
        refresh();
        if (cb.onLanguage) cb.onLanguage(lang);
        if (cb.onRate)     cb.onRate(rate_);
    };

    rateCombo.addItem("Per sample", kRatePerSample);
    rateCombo.addItem("Per block",  kRatePerBlock);
    rateCombo.onChange = [this]() {
        ScriptRate newRate = (rateCombo.getSelectedId() == kRatePerBlock)
                                 ? ScriptRate::PerBlock : ScriptRate::PerSample;
        if (newRate == rate_) return;
        if (!scriptLangSupportsRate(lang, newRate)) return; // ignore forbidden
        rate_ = newRate;
        refresh();
        if (cb.onRate) cb.onRate(rate_);
    };

    warningLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd9a441));
    warningLabel.setJustificationType(juce::Justification::topLeft);
    warningLabel.setMinimumHorizontalScale(1.0f);
    warningLabel.setFont(juce::Font(12.0f));
}

void ScriptLangBar::setState(ScriptLang l, ScriptRate r) {
    lang = l;
    rate_ = scriptLangSupportsRate(l, r)
                ? r
                : ((l == ScriptLang::Wasm) ? ScriptRate::PerBlock
                                           : ScriptRate::PerSample);
    refresh();
}

void ScriptLangBar::refresh() {
    langCombo.setSelectedId(idFromLang(lang), juce::dontSendNotification);
    rateCombo.setSelectedId(rate_ == ScriptRate::PerBlock ? kRatePerBlock
                                                          : kRatePerSample,
                            juce::dontSendNotification);

    // Rate combo is only meaningful for Lua. Builtin is per-sample only and
    // Wasm is per-block only, so the choice is forced and the combo disabled
    // (with a tooltip explaining why, per the grayed-control rule).
    const bool rateEditable = (lang == ScriptLang::Lua);
    rateCombo.setEnabled(rateEditable);
    if (lang == ScriptLang::Builtin)
        rateCombo.setTooltip("The built-in expression language always runs once "
                             "per sample, so this can't be changed. Switch to Lua "
                             "for per-block execution.");
    else if (lang == ScriptLang::Wasm)
        rateCombo.setTooltip("WebAssembly always runs once per block: its module "
                             "interface processes a whole audio block at a time, "
                             "so per-sample isn't available. For sample-accurate "
                             "output, write each sample inside the block (see the "
                             "language reference).");
    else
        rateCombo.setTooltip("Per sample: your script runs once for every audio "
                             "sample (tens of thousands of times a second) - "
                             "sample-accurate but heavy code can stutter the audio.\n"
                             "Per block: your script runs once per audio block "
                             "(cheap, scales to many instances); fill each sample "
                             "yourself for sample-accurate output.");

    // Inline caution for the one combination that risks audio glitches.
    if (lang == ScriptLang::Lua && rate_ == ScriptRate::PerSample)
        warningLabel.setText(
            juce::String::fromUTF8(
                "\xE2\x9A\xA0 Per-sample Lua runs your script tens of thousands of "
                "times a second on the audio thread. Heavy code can stutter or glitch "
                "the sound - switch \"Run\" to Per block if you hear problems."),
            juce::dontSendNotification);
    else
        warningLabel.setText({}, juce::dontSendNotification);
}

void ScriptLangBar::resized() {
    auto r = getLocalBounds();
    auto row = r.removeFromTop(24);
    langLabel.setBounds(row.removeFromLeft(70));
    langCombo.setBounds(row.removeFromLeft(190));
    row.removeFromLeft(16);
    rateLabel.setBounds(row.removeFromLeft(34));
    rateCombo.setBounds(row.removeFromLeft(120));
    r.removeFromTop(6);
    warningLabel.setBounds(r);
}

// =============================================================================
// WasmFilePanel
// =============================================================================
WasmFilePanel::WasmFilePanel(std::function<void(std::string)> onPathChanged)
    : onPath(std::move(onPathChanged)) {
    addAndMakeVisible(pathLabel);
    addAndMakeVisible(chooseBtn);
    addAndMakeVisible(hintLabel);

    pathLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd0d0d4));
    pathLabel.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 13.0f,
                                 juce::Font::plain));
    pathLabel.setMinimumHorizontalScale(1.0f);

    hintLabel.setColour(juce::Label::textColourId, juce::Colour(0xff8a8a90));
    hintLabel.setFont(juce::Font(12.0f));
    hintLabel.setJustificationType(juce::Justification::topLeft);
    hintLabel.setText(
        "Pick a WebAssembly module (.wasm) that implements the SEANCE script ABI. "
        "The module runs once per audio block. See the language reference for the "
        "exports/imports it must provide.",
        juce::dontSendNotification);

    chooseBtn.setTooltip("Open a file dialog to pick the .wasm module this node runs.");
    chooseBtn.onClick = [this]() { choose(); };

    setPath({});
}

void WasmFilePanel::setPath(const std::string& p) {
    path = p;
    if (path.empty())
        pathLabel.setText("(no file chosen)", juce::dontSendNotification);
    else
        pathLabel.setText(path, juce::dontSendNotification);
    repaint();
}

void WasmFilePanel::choose() {
    auto start = path.empty() ? juce::File()
                              : juce::File(juce::String(path)).getParentDirectory();
    chooser = std::make_shared<juce::FileChooser>(
        "Choose a WebAssembly module", start, "*.wasm");
    auto self = this;
    chooser->launchAsync(juce::FileBrowserComponent::openMode,
        [self](const juce::FileChooser& fc) {
            auto file = fc.getResult();
            if (!file.existsAsFile()) return;
            std::string p = file.getFullPathName().toStdString();
            self->setPath(p);
            if (self->onPath) self->onPath(p);
        });
}

void WasmFilePanel::resized() {
    auto r = getLocalBounds().reduced(2);
    chooseBtn.setBounds(r.removeFromTop(28).removeFromLeft(200));
    r.removeFromTop(8);
    pathLabel.setBounds(r.removeFromTop(22));
    r.removeFromTop(8);
    hintLabel.setBounds(r.removeFromTop(60));
}

void WasmFilePanel::paint(juce::Graphics& g) {
    g.setColour(juce::Colour(0xff26262b));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
    g.setColour(juce::Colour(0xff3a3a40));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 4.0f, 1.0f);
}

} // namespace SoundShop

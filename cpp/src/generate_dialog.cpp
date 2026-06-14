#include "generate_dialog.h"
#include "script_runtime.h"
#include "scripting.h"
#include "glsl_compute.h"
#include "dialog_helpers.h"

namespace SoundShop {

// Display name for a language offered in the picker.
static juce::String langDisplayName(GenLang lang) {
    switch (lang) {
        case GenLang::Builtin: return "Builtin (math expression)";
        case GenLang::Lua:     return "Lua";
        case GenLang::Python:  return "Python";
        case GenLang::Glsl:    return "GLSL (compute shader, GPU)";
        case GenLang::Wasm:    return "WASM (.wasm module)";
        default:               return "Unknown";
    }
}

// Map a GenLang that IS backed by a ScriptLang runtime to that ScriptLang. Only
// Builtin/Lua/Wasm qualify; Python/Glsl are NOT ScriptLangs (they bake via their
// own paths and are handled before this is ever reached). This REPLACES the old
// blind (ScriptLang)(int)lang cross-cast, which was a latent trap: GenLang and
// ScriptLang only agree on Builtin/Lua - GenLang::Python==2 collides with
// ScriptLang::Wasm==2, and GenLang::Wasm==4 - so a cast would silently mis-route.
static ScriptLang genLangToScriptLang(GenLang lang) {
    switch (lang) {
        case GenLang::Builtin: return ScriptLang::Builtin;
        case GenLang::Lua:     return ScriptLang::Lua;
        case GenLang::Wasm:    return ScriptLang::Wasm;
        default:               jassertfalse; return ScriptLang::Builtin;
    }
}

// Whether a generation language is usable in this build.
static bool genLangAvailable(GenLang lang) {
    switch (lang) {
        case GenLang::Builtin: return scriptLangAvailable(ScriptLang::Builtin);
        case GenLang::Lua:     return scriptLangAvailable(ScriptLang::Lua);
        case GenLang::Python:  return ScriptEngine::pythonAvailable();
        case GenLang::Glsl:    return glslComputeAvailable();
        case GenLang::Wasm:    return scriptLangAvailable(ScriptLang::Wasm);
        default:               return false;
    }
}

// Whether a language can run PER-CELL (one call per cell). Every language can
// except WASM, whose ABI is block-only (it owns its own loop), so it generates
// whole-grid only.
static bool genLangPerCellCapable(GenLang lang) {
    switch (lang) {
        case GenLang::Builtin: return true;
        case GenLang::Lua:     return scriptLangSupportsRate(ScriptLang::Lua, ScriptRate::PerSample);
        case GenLang::Python:  return true;
        case GenLang::Glsl:    return true;
        case GenLang::Wasm:    return false;
        default:               return false;
    }
}

// Whether a language can do whole-grid (one call fills the array). Builtin is
// per-cell only; Lua (block-capable runtime), Python, GLSL and WASM all can.
static bool genLangWholeGridCapable(GenLang lang) {
    switch (lang) {
        case GenLang::Lua:     return scriptLangSupportsRate(ScriptLang::Lua, ScriptRate::PerBlock);
        case GenLang::Python:  return true;
        // GLSL whole-grid = "raw compute": the shader's main() owns the writes
        // to data[]. With Passes > 1 it ping-pongs over two buffers, so each pass
        // reads the previous pass's full output via prev[]/prevAt()/neighbor() —
        // enabling true cross-cell convolution, cellular automata, diffusion, etc.
        case GenLang::Glsl:    return true;
        // WASM is block-only: the whole module owns the array via ss_generate()
        // and the ss_grid_* imports (see soundshop_wasm.h).
        case GenLang::Wasm:    return scriptLangSupportsRate(ScriptLang::Wasm, ScriptRate::PerBlock);
        case GenLang::Builtin: return false;
        default:               return false;
    }
}

// True for languages whose "program" is a CHOSEN FILE (a pre-compiled binary),
// not typed source. WASM is the only one: the editor field holds a .wasm path and
// the template button becomes a file browser.
static bool genLangIsFileBased(GenLang lang) { return lang == GenLang::Wasm; }

// Default starter program for a language + mode. All produce a smooth 0..1 field
// (mapped to the terrain's bipolar [-1,1] as v*2-1) referencing the first two
// coordinates so 1D and 2D+ grids both look sensible.
//
// In EVERY language and mode, waveform(id_or_name, phase) samples one of the
// ~4000 bundled single-cycle factory waveforms: phase in [0,1) wraps and is
// linearly interpolated, and the result is the raw [-1,1] sample (map with
// *0.5+0.5 to fit the 0..1 output). Every waveform has a STABLE INTEGER id (the
// "#N" shown in the factory-waveform browser); that integer is the same in all
// languages. Builtin/Lua/Python also accept the NAME string directly (a
// convenience lookup); Lua/Python additionally expose waveforms["name"] -> id so
// you can resolve a name ONCE and reuse the fast integer. GLSL is integer-ONLY
// (no strings on the GPU): pass the id, e.g. waveform(42, phase). The whole bank
// is uploaded to the GPU once per session, so there is no source rewriting.
static std::string defaultSource(GenLang lang, int mode) {
    if (lang == GenLang::Lua && mode == 1) {
        // Whole-grid: generate() runs once and fills the whole array itself, so
        // it can do cross-cell work (blur, cellular automata, FFT, ...).
        return "-- Whole-grid mode: generate() runs ONCE and fills the array.\n"
               "-- dims = {size per axis}, nd = #dims, total = product(dims).\n"
               "-- N-D access: setAt(c0,c1,...,v) write a cell by integer coords;\n"
               "-- getAt(c0,c1,...) read one (edge-clamped). Or flat: set(i,v) /\n"
               "-- get(i), coord(i,axis) -> [0,1] pos, flatten(c0,...) -> flat idx.\n"
               "function generate()\n"
               "  for i = 0, total - 1 do\n"
               "    local x = coord(i, 0) * 2 * math.pi\n"
               "    local y = coord(i, 1) * 2 * math.pi\n"
               "    set(i, 0.5 + 0.5 * math.sin(x) * math.cos(y))\n"
               "  end\n"
               "end\n";
    }
    if (lang == GenLang::Lua) {
        return "-- Return one value in [0,1] per cell (a grayscale height).\n"
               "-- Coordinates: c0..c{nd-1} in [0,1]; nd = number of dimensions.\n"
               "-- x,y,z,w alias c0..c3 scaled to [0,2*pi].\n"
               "function loop()\n"
               "  local v = 0.5 + 0.5 * math.sin(x) * math.cos(y)\n"
               "  return v\n"
               "end\n";
    }
    if (lang == GenLang::Python && mode == 1) {
        // Whole-grid Python: define generate(); fill via set(i,v).
        return "# Whole-grid mode: generate() runs ONCE and fills the array.\n"
               "# dims = [size per axis], nd = len(dims), total = product(dims).\n"
               "# N-D access: setAt(c0,c1,...,v) write a cell by integer coords;\n"
               "# getAt(c0,c1,...) read one (edge-clamped). Or flat: set(i,v) /\n"
               "# get(i), coord(i,axis) -> [0,1] pos, flatten(c0,...) -> flat idx.\n"
               "def generate():\n"
               "    for i in range(total):\n"
               "        x = coord(i, 0) * 2 * pi\n"
               "        y = coord(i, 1) * 2 * pi\n"
               "        set(i, 0.5 + 0.5 * sin(x) * cos(y))\n";
    }
    if (lang == GenLang::Python) {
        return "# Return one value in [0,1] per cell (a grayscale height).\n"
               "# Coordinates: c0..c7 in [0,1]; nd = number of dimensions.\n"
               "# x,y,z,w alias c0..c3 scaled to [0,2*pi].\n"
               "return 0.5 + 0.5 * sin(x) * cos(y)\n";
    }
    if (lang == GenLang::Glsl && mode == 1) {
        // Whole-grid GLSL: the body of main(). The shader owns the write to
        // data[gid]. With Passes > 1 the shader runs repeatedly (ping-pong): on
        // uPass==0 you seed the field; on later passes you read the PREVIOUS
        // pass's output via prev[] / prevAt(idx) and neighbour helpers, so you
        // can blur, diffuse, run cellular automata, etc. Helpers available:
        //   coordOf(idx,axis) -> [0,1] position    coordAxis(idx,axis) -> int coord
        //   neighbor(idx,axis,delta) -> flat index, clamped to the edge
        //   prevAt(idx) -> prev[idx] (0 if out of range)
        //   flatten(c0,c1,...) -> flat index from per-axis coords (clamped)
        //   DIM0,DIM1,... -> this terrain's size along each axis (literals)
        //   waveform(id, phase) -> factory single-cycle sample, [-1,1] (id is the
        //                          "#N" from the factory-waveform browser)
        // This example seeds a sine field, then box-blurs it `Passes-1` times.
        return "// Whole-grid GLSL. Runs once per pass (set Passes above).\n"
               "// Pass 0 seeds the field; later passes blur the previous result.\n"
               "uint gid = gl_GlobalInvocationID.x;\n"
               "if (gid >= uint(uTotal)) return;\n"
               "int i = int(gid);\n"
               "if (uPass == 0) {\n"
               "    float x = coordOf(i, 0) * TAU;\n"
               "    float y = coordOf(i, 1) * TAU;\n"
               "    data[gid] = clamp(0.5 + 0.5 * sin(x) * cos(y), 0.0, 1.0);\n"
               "} else {\n"
               "    // 3x3 box blur of the previous pass (axis 0 = rows, 1 = cols).\n"
               "    float s = 0.0;\n"
               "    for (int dy = -1; dy <= 1; ++dy)\n"
               "        for (int dx = -1; dx <= 1; ++dx)\n"
               "            s += prevAt(neighbor(neighbor(i, 0, dy), 1, dx));\n"
               "    data[gid] = s / 9.0;\n"
               "}\n";
    }
    if (lang == GenLang::Glsl) {
        // Per-cell GLSL: the body of a function returning one float in [0,1].
        // Same coordinate vocabulary as Lua/Python plus GLSL's full math.
        return "// Return one value in [0,1] per cell (a grayscale height).\n"
               "// c[0..nd-1] in [0,1]; x,y,z,w = c0..c3 * TAU (2*pi).\n"
               "// coord[d] = integer cell index, dims[d] = axis size.\n"
               "return 0.5 + 0.5 * sin(x) * cos(y);\n";
    }
    if (lang == GenLang::Wasm) {
        // WASM has no inline source - the "program" is a pre-compiled .wasm file
        // chosen via the Browse button. The editor field holds its path, so the
        // default is empty (a placeholder prompt is shown instead).
        return "";
    }
    // Builtin: a single math expression. Same coordinate vocabulary.
    return "0.5 + 0.5 * sin(x) * cos(y)";
}

// Generate a full-resolution grid for the chosen language/mode, returning the
// FINAL bipolar [-1,1] cells in `data`. Routes Builtin/Lua through Terrain's
// script fill and Python through the embedded-CPython terrain baker. Returns
// false + fills `err` on any compile/run error.
static bool generateGrid(GenLang lang, int mode, const std::string& src,
                         const std::vector<int>& dims, int passes,
                         std::vector<float>& data, std::string& err) {
    if (lang == GenLang::Python)
        return ScriptEngine::instance().bakeTerrain(src, mode == 1, dims, data, err);

    Terrain t;
    if (lang == GenLang::Glsl) {
        // GLSL bakes on an offscreen GL compute context (NOT a ScriptLang -
        // never cross-cast it). mode==1 is whole-grid (raw main()), else per-cell.
        // `passes` drives multi-pass ping-pong (whole-grid only; ignored else).
        bool ok = t.fillFromGlsl(src, mode == 1, dims, err, passes);
        if (ok) data = t.getData();
        return ok;
    }
    // Builtin / Lua / WASM run through the IScriptRuntime path. Map the GenLang
    // to its ScriptLang EXPLICITLY (never blind-cast - see genLangToScriptLang).
    // For WASM, `src` is the chosen .wasm file PATH, not source text.
    ScriptLang sl = genLangToScriptLang(lang);
    bool ok = (mode == 1)
        ? t.fillFromScriptWholeGrid(sl, src, dims, err)
        : t.fillFromScript(sl, src, dims, err);
    if (ok) data = t.getData();
    return ok;
}

GenerateDialogComponent::GenerateDialogComponent(
        const GenerateTerrainParams& seed,
        bool lockRankArg,
        std::function<void(const GenerateTerrainParams&)> apply)
    : lockRank(lockRankArg), applyFn(std::move(apply)) {

    titleLabel.setText("Generate Terrain from a Program", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(titleLabel);

    langLabel.setText("Language:", juce::dontSendNotification);
    langLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible(langLabel);

    rebuildLangChoices();
    // Seed the language selection from the incoming params if it's offered.
    int seedRow = 0;
    for (size_t i = 0; i < langValues.size(); ++i)
        if (langValues[i] == seed.lang) seedRow = (int) i;
    if (!langValues.empty())
        langCombo.setSelectedItemIndex(seedRow, juce::dontSendNotification);
    langCombo.onChange = [this] { onLanguageChanged(); };
    langCombo.setTooltip("Only languages that can generate a terrain are listed. "
                         "Builtin is a single math expression (per-cell only); Lua "
                         "and Python let you write a function with full logic and "
                         "can also do whole-grid generation. Whichever language you "
                         "pick, the result is computed once now and baked into the "
                         "project, so it never re-runs when the project is reopened.");
    addAndMakeVisible(langCombo);

    modeLabel.setText("Mode:", juce::dontSendNotification);
    modeLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible(modeLabel);

    // Per-cell = the program runs once for every cell and returns that cell's
    // value. Whole-grid = the program runs ONCE and fills the entire array,
    // which lets it do cross-cell work (blur, cellular automata, FFT, global
    // normalisation) a per-cell program can't express. Whole-grid needs a
    // block-capable language (Lua); it's greyed for per-cell-only Builtin.
    modeCombo.addItem("Per-cell (one call per cell)", 1);
    modeCombo.addItem("Whole-grid (one call, full array)", 2);
    modeCombo.setSelectedId(seed.mode == 1 ? 2 : 1, juce::dontSendNotification);
    modeCombo.onChange = [this] { insertTemplateIfDefault(); refreshPassesVisibility(); };
    modeCombo.setTooltip("Per-cell: your program runs once per cell and returns "
                         "that cell's height. Whole-grid: it runs once and fills "
                         "the whole terrain itself - needed for effects that read "
                         "neighbouring cells (blur, cellular automata, FFT).");
    addAndMakeVisible(modeCombo);
    refreshModeAvailability();

    // Passes: GLSL whole-grid ping-pong iteration count. Each pass re-runs the
    // shader over the previous pass's output (prev[]), so neighbour reads see a
    // stable snapshot - this is how blur / cellular automata / diffusion work on
    // the GPU. Only shown for GLSL whole-grid; 1 = a single pass.
    passesLabel.setText("Passes:", juce::dontSendNotification);
    passesLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible(passesLabel);

    passesEditor.setInputRestrictions(5, "0123456789");
    passesEditor.setText(juce::String(juce::jmax(1, seed.passes)), juce::dontSendNotification);
    passesEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff1e1e22));
    passesEditor.setColour(juce::TextEditor::textColourId, juce::Colours::white);
    passesEditor.setTooltip("How many times to run a GLSL whole-grid program "
                            "(ping-pong iteration). Pass 0 typically seeds the "
                            "field; each later pass reads the previous pass's "
                            "result via prev[] / prevAt() / neighbor(), so you can "
                            "blur, diffuse, or run cellular automata. 1 = a single "
                            "pass. Use uPass / uNumPasses in the program.");
    addAndMakeVisible(passesEditor);

    dimsLabel.setText("Dimensions:", juce::dontSendNotification);
    dimsLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible(dimsLabel);

    {
        juce::String dimsText;
        for (size_t i = 0; i < seed.dims.size(); ++i) {
            if (i) dimsText << ", ";
            dimsText << seed.dims[i];
        }
        if (dimsText.isEmpty()) dimsText = "128, 128";
        dimsEditor.setText(dimsText, juce::dontSendNotification);
    }
    dimsEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff1e1e22));
    dimsEditor.setColour(juce::TextEditor::textColourId, juce::Colours::white);
    if (lockRank) {
        dimsEditor.setReadOnly(false);   // sizes still editable; rank checked on Generate
        dimsEditor.setTooltip("Comma-separated size of each axis. This terrain's "
                              "axis COUNT is fixed (changing it would rewire the "
                              "node) - edit only the sizes, keeping the same "
                              "number of values. Make a new generated terrain for "
                              "a different number of axes.");
    } else {
        dimsEditor.setTooltip("Comma-separated size of each axis, e.g. \"44100\" "
                              "for 1D audio, \"512, 512\" for a 2D image, "
                              "\"64, 128, 128\" for 3D video (frames, h, w). Any "
                              "number of axes (up to 8) is allowed.");
    }
    addAndMakeVisible(dimsEditor);

    dimsHint.setColour(juce::Label::textColourId, juce::Colours::grey);
    dimsHint.setFont(juce::Font(juce::FontOptions(12.0f)));
    dimsHint.setText(lockRank
        ? "Axis count is locked for this node - keep the same number of values."
        : "1 value = 1D audio, 2 = image, 3 = video; any rank up to 8.",
        juce::dontSendNotification);
    addAndMakeVisible(dimsHint);

    scriptLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    scriptLabel.setText("Program (returns 0..1 per cell):", juce::dontSendNotification);
    addAndMakeVisible(scriptLabel);

    scriptEditor.setMultiLine(true, false);
    scriptEditor.setReturnKeyStartsNewLine(true);
    scriptEditor.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 14.0f, 0)));
    scriptEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff1e1e22));
    scriptEditor.setColour(juce::TextEditor::textColourId, juce::Colours::white);
    {
        GenLang lang = langValues.empty() ? GenLang::Builtin
                                          : (GenLang) langValues[(size_t) juce::jmax(0, langCombo.getSelectedItemIndex())];
        scriptEditor.setText(seed.source.empty() ? defaultSource(lang, selectedMode()) : seed.source,
                             juce::dontSendNotification);
    }
    addAndMakeVisible(scriptEditor);

    templateBtn.setTooltip("Replace the program with a fresh example for the "
                           "selected language.");
    // For WASM the button browses for a .wasm file; for code languages it inserts
    // a fresh example (updateProgramUiForLanguage swaps the label/tooltip).
    templateBtn.onClick = [this] {
        if (!langValues.empty()) {
            int row = juce::jmax(0, langCombo.getSelectedItemIndex());
            if (genLangIsFileBased((GenLang) langValues[(size_t) row])) { browseForWasm(); return; }
        }
        insertTemplate();
    };
    addAndMakeVisible(templateBtn);

    statusLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
    statusLabel.setFont(juce::Font(juce::FontOptions(13.0f)));
    addAndMakeVisible(statusLabel);

    generateBtn.setTooltip("Validate the program, then build the terrain at the "
                           "chosen resolution. The program is re-run whenever the "
                           "project loads, so the data isn't stored in the file.");
    generateBtn.onClick = [this] { doGenerate(); };
    addAndMakeVisible(generateBtn);

    cancelBtn.onClick = [this] { closeSelf(); };
    addAndMakeVisible(cancelBtn);

    updateProgramUiForLanguage();   // sets label/button/placeholder for the seed language
    refreshPassesVisibility();
    setSize(560, 560);
}

void GenerateDialogComponent::rebuildLangChoices() {
    langCombo.clear(juce::dontSendNotification);
    langValues.clear();
    // Offer every generation language that is available in this build.
    const GenLang candidates[] = { GenLang::Builtin, GenLang::Lua, GenLang::Python,
                                   GenLang::Glsl, GenLang::Wasm };
    int itemId = 1;
    for (GenLang lang : candidates) {
        if (!genLangAvailable(lang)) continue;
        langCombo.addItem(langDisplayName(lang), itemId++);
        langValues.push_back((int) lang);
    }
    if (langValues.empty()) {
        // Builtin is always available, but guard anyway.
        langCombo.addItem(langDisplayName(GenLang::Builtin), itemId++);
        langValues.push_back((int) GenLang::Builtin);
    }
    langCombo.setSelectedItemIndex(0, juce::dontSendNotification);
}

int GenerateDialogComponent::selectedMode() const {
    return modeCombo.getSelectedId() == 2 ? 1 : 0;
}

void GenerateDialogComponent::refreshModeAvailability() {
    // Per-cell (id 1) and whole-grid (id 2) are each gated by the language's
    // capability. Builtin is per-cell only (whole-grid greyed); WASM is whole-grid
    // only (per-cell greyed); Lua/Python/GLSL do both. The selection snaps to an
    // available mode when the current one is disabled. Per CLAUDE.md, a disabled
    // control must explain why, so the tooltip names the language-specific reason.
    if (langValues.empty()) return;
    int row = juce::jmax(0, langCombo.getSelectedItemIndex());
    GenLang lang = (GenLang) langValues[(size_t) row];
    bool perCellOk = genLangPerCellCapable(lang);
    bool wholeOk   = genLangWholeGridCapable(lang);
    modeCombo.setItemEnabled(1, perCellOk);
    modeCombo.setItemEnabled(2, wholeOk);
    if (!perCellOk && modeCombo.getSelectedId() == 1)
        modeCombo.setSelectedId(2, juce::dontSendNotification);
    if (!wholeOk && modeCombo.getSelectedId() == 2)
        modeCombo.setSelectedId(1, juce::dontSendNotification);

    juce::String tip("Per-cell: your program runs once per cell and returns that "
                     "cell's height. Whole-grid: it runs once and fills the whole "
                     "terrain itself - needed for effects that read neighbouring "
                     "cells (blur, cellular automata, FFT).");
    if (!wholeOk)
        tip = juce::String("Whole-grid is unavailable for ") + langDisplayName(lang) +
              " - it can only run once per cell. Choose Lua to generate the whole "
              "grid in one pass (for blur, cellular automata, FFT, etc.).";
    else if (!perCellOk)
        tip = juce::String("Per-cell is unavailable for ") + langDisplayName(lang) +
              " - a WASM module owns its own loop, so it always generates the "
              "whole grid in one call (ss_generate). Use Builtin/Lua/Python/GLSL "
              "for per-cell generation.";
    modeCombo.setTooltip(tip);
}

void GenerateDialogComponent::onLanguageChanged() {
    refreshModeAvailability();
    updateProgramUiForLanguage();
    insertTemplateIfDefault();
    refreshPassesVisibility();
}

// Switch the program field between "typed source" and "chosen .wasm file" modes.
// For WASM the label/placeholder reflect a file path and the template button
// becomes a file browser; for every other language it's the normal code editor.
void GenerateDialogComponent::updateProgramUiForLanguage() {
    if (langValues.empty()) return;
    int row = juce::jmax(0, langCombo.getSelectedItemIndex());
    GenLang lang = (GenLang) langValues[(size_t) row];
    bool fileBased = genLangIsFileBased(lang);
    scriptLabel.setText(fileBased ? "WASM module (.wasm file):"
                                  : "Program (returns 0..1 per cell):",
                        juce::dontSendNotification);
    templateBtn.setButtonText(fileBased ? "Browse .wasm..." : "Insert example");
    templateBtn.setTooltip(fileBased
        ? juce::String("Choose a pre-compiled .wasm terrain module (exports "
                       "ss_generate; see Help). Its file path is stored in the "
                       "project; the generated grid is baked, so the module isn't "
                       "needed when the project is reopened.")
        : juce::String("Replace the program with a fresh example for the selected "
                       "language."));
    scriptEditor.setTextToShowWhenEmpty(
        fileBased ? "Click \"Browse .wasm...\" to choose a compiled module, or paste its path."
                  : juce::String(),
        juce::Colours::grey);
    scriptEditor.repaint();
}

void GenerateDialogComponent::browseForWasm() {
    chooser = std::make_unique<juce::FileChooser>(
        "Choose a .wasm terrain module", juce::File(), "*.wasm");
    auto flags = juce::FileBrowserComponent::openMode
               | juce::FileBrowserComponent::canSelectFiles;
    chooser->launchAsync(flags, [this](const juce::FileChooser& fc) {
        auto f = fc.getResult();
        if (f.existsAsFile())
            scriptEditor.setText(f.getFullPathName(), juce::dontSendNotification);
    });
}

int GenerateDialogComponent::selectedPasses() const {
    int p = passesEditor.getText().getIntValue();
    return juce::jmax(1, p);
}

void GenerateDialogComponent::refreshPassesVisibility() {
    // Passes only applies to GLSL whole-grid (ping-pong). For any other
    // language/mode it's irrelevant, so hide it rather than show a dead control.
    bool isGlsl = false;
    if (!langValues.empty()) {
        int row = juce::jmax(0, langCombo.getSelectedItemIndex());
        isGlsl = (GenLang) langValues[(size_t) row] == GenLang::Glsl;
    }
    bool show = isGlsl && selectedMode() == 1;
    passesLabel.setVisible(show);
    passesEditor.setVisible(show);
}

void GenerateDialogComponent::insertTemplateIfDefault() {
    // Offer to swap in the matching template only if the editor still holds an
    // untouched default for some language/mode (don't clobber real user code).
    const juce::String cur = scriptEditor.getText().trim();
    bool isDefault = cur.isEmpty();
    for (int v : langValues)
        for (int m = 0; m < 2; ++m)
            if (cur == juce::String(defaultSource((GenLang) v, m)).trim()) isDefault = true;
    if (isDefault) insertTemplate();
}

void GenerateDialogComponent::insertTemplate() {
    if (langValues.empty()) return;
    int row = juce::jmax(0, langCombo.getSelectedItemIndex());
    GenLang lang = (GenLang) langValues[(size_t) row];
    scriptEditor.setText(defaultSource(lang, selectedMode()), juce::dontSendNotification);
}

bool GenerateDialogComponent::parseDims(std::vector<int>& out, std::string& err) const {
    out.clear();
    juce::StringArray toks;
    toks.addTokens(dimsEditor.getText(), ",", "");
    for (const auto& t : toks) {
        auto s = t.trim();
        if (s.isEmpty()) continue;
        int v = s.getIntValue();
        if (v < 1) { err = "every dimension must be a whole number >= 1"; return false; }
        out.push_back(v);
    }
    if (out.empty()) { err = "enter at least one dimension size"; return false; }
    if ((int) out.size() > 8) { err = "at most 8 dimensions are supported"; return false; }
    long long total = 1;
    for (int d : out) {
        total *= d;
        if (total > (1LL << 30)) { err = "grid too large (> 1G cells)"; return false; }
    }
    return true;
}

void GenerateDialogComponent::doGenerate() {
    statusLabel.setColour(juce::Label::textColourId, juce::Colours::orange);

    std::vector<int> dims;
    std::string err;
    if (!parseDims(dims, err)) {
        statusLabel.setText(err, juce::dontSendNotification);
        return;
    }
    if (langValues.empty()) {
        statusLabel.setText("no script language available", juce::dontSendNotification);
        return;
    }
    int row = juce::jmax(0, langCombo.getSelectedItemIndex());
    GenLang lang = (GenLang) langValues[(size_t) row];
    int mode = selectedMode();
    int passes = (lang == GenLang::Glsl && mode == 1) ? selectedPasses() : 1;
    std::string src = scriptEditor.getText().toStdString();

    // WASM's "source" is a chosen .wasm path; give a clear prompt if it's blank
    // rather than the runtime's generic "No WASM file selected" load error.
    if (genLangIsFileBased(lang) && juce::String(src).trim().isEmpty()) {
        statusLabel.setText("Choose a .wasm module first (click \"Browse .wasm...\").",
                            juce::dontSendNotification);
        return;
    }

    // First validate on a tiny probe grid (same rank, sizes clamped) so compile
    // errors surface instantly without paying for the full grid. Probe with a
    // single pass - we only want to catch compile/link errors here, not run the
    // whole ping-pong iteration.
    std::vector<int> probe;
    probe.reserve(dims.size());
    for (int d : dims) probe.push_back(juce::jlimit(1, 3, d));
    std::vector<float> probeData;
    std::string genErr;
    if (!generateGrid(lang, mode, src, probe, 1, probeData, genErr)) {
        statusLabel.setText("Program error: " + genErr, juce::dontSendNotification);
        return;
    }

    // Generate the FULL-resolution grid here on the message thread and bake the
    // result, so the node never re-runs the generator on load (see
    // GenerateTerrainParams). This is the one-time generation cost; it blocks
    // briefly for large grids.
    std::vector<float> fullData;
    if (!generateGrid(lang, mode, src, dims, passes, fullData, genErr)) {
        statusLabel.setText("Program error: " + genErr, juce::dontSendNotification);
        return;
    }

    GenerateTerrainParams gp;
    gp.lang = (int) lang;
    gp.mode = mode;
    gp.passes = passes;
    gp.dims = dims;
    gp.source = src;
    gp.data = std::move(fullData);   // baked: the final bipolar grid
    if (applyFn) applyFn(gp);
    closeSelf();
}

void GenerateDialogComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff2b2b30));
}

void GenerateDialogComponent::resized() {
    auto r = getLocalBounds().reduced(16);
    titleLabel.setBounds(r.removeFromTop(28));
    r.removeFromTop(8);

    auto langRow = r.removeFromTop(26);
    langLabel.setBounds(langRow.removeFromLeft(90));
    langCombo.setBounds(langRow.removeFromLeft(240));
    r.removeFromTop(8);

    auto modeRow = r.removeFromTop(26);
    modeLabel.setBounds(modeRow.removeFromLeft(90));
    modeCombo.setBounds(modeRow.removeFromLeft(260));
    // Passes sits to the right of the mode combo (only visible for GLSL
    // whole-grid; setVisible(false) hides it without reclaiming the gap).
    modeRow.removeFromLeft(16);
    passesLabel.setBounds(modeRow.removeFromLeft(56));
    passesEditor.setBounds(modeRow.removeFromLeft(64));
    r.removeFromTop(8);

    auto dimRow = r.removeFromTop(26);
    dimsLabel.setBounds(dimRow.removeFromLeft(90));
    dimsEditor.setBounds(dimRow.removeFromLeft(240));
    r.removeFromTop(2);
    dimsHint.setBounds(r.removeFromTop(18));
    r.removeFromTop(8);

    auto scriptHdr = r.removeFromTop(22);
    scriptLabel.setBounds(scriptHdr.removeFromLeft(280));
    templateBtn.setBounds(scriptHdr.removeFromRight(120));
    r.removeFromTop(4);

    auto buttons = r.removeFromBottom(34);
    cancelBtn.setBounds(buttons.removeFromRight(110));
    buttons.removeFromRight(8);
    generateBtn.setBounds(buttons.removeFromRight(120));
    statusLabel.setBounds(buttons);
    r.removeFromBottom(8);

    scriptEditor.setBounds(r);
}

void GenerateDialogComponent::closeSelf() {
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
        juce::Component::SafePointer<juce::DialogWindow> safe(dw);
        juce::MessageManager::callAsync([safe] { if (safe) delete safe.getComponent(); });
    }
}

} // namespace SoundShop

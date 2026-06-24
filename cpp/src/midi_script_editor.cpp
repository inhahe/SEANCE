#include "midi_script_editor.h"

namespace SoundShop {

namespace {
const char* kMidiScriptHelp =
    "MIDI Script runs your program once per sample and emits MIDI live.\n"
    "\n"
    "Statements are separated by ';' or new lines. A statement is either an\n"
    "assignment (name = expr) or a bare expression run for its side effects.\n"
    "Any variable you assign PERSISTS across samples and blocks, so you can\n"
    "keep a running counter, phase or RNG seed.\n"
    "\n"
    "Emit functions (push MIDI at the current moment):\n"
    "  note(pitch, vel, durSec)  - a note that auto-releases after durSec\n"
    "  noteon(pitch, vel)        - note-on only (release it yourself)\n"
    "  noteoff(pitch)            - note-off\n"
    "  cc(number, value)         - control change; value 0..1 -> 0..127\n"
    "  bend(value)               - pitch bend; value -1..1 (0 = centre)\n"
    "pitch/vel are 0..127. Set `out = k` to route the following emits to MIDI\n"
    "output pin k (0-based; default 0).\n"
    "\n"
    "Note names: anywhere a pitch number goes you can write a quoted name like\n"
    "  \"C4\" (= 60), \"c#4\", \"Bb3\". notefreq(\"C4\") or notefreq(60) gives the\n"
    "  frequency in Hz using the project's tuning system.\n"
    "\n"
    "Per-sample variables you can read:\n"
    "  t        seconds since start      beat   transport beat position\n"
    "  bar      beat / 4                  bpm    tempo            playing 0/1\n"
    "  sr       sample rate              dt     seconds per sample\n"
    "  note/vel/gate   most-recent MIDI-input note, its velocity 0..1, held 0/1\n"
    "  s1..sN   Signal input pin values   shape(pos)  drawn shape at pos (0..1)\n"
    "\n"
    "Maths: sin cos tan abs sqrt exp log pow tanh saw square triangle noise\n"
    "  floor ceil min max clamp if(c,a,b)  c?a:b  < > <= >= == != && || !\n"
    "\n"
    "Sections (optional) - put a header line on its own to split the program:\n"
    "  init:   runs ONCE when the program loads or you edit it. Use it to seed\n"
    "          persistent state (counters, tables). Emit calls here are ignored.\n"
    "  start:  runs ONCE each time playback starts from stop, at the downbeat.\n"
    "          It CAN emit - perfect for a single note/CC at the very beginning.\n"
    "  loop:   runs every sample (the default). Lines before any header are loop.\n"
    "A program with no headers is all loop, exactly as before. All sections share\n"
    "the same persistent variables.\n"
    "\n"
    "Example - a four-on-the-floor kick plus a random hat:\n"
    "  ph = beat - floor(beat)\n"
    "  (ph < dt*bpm/60) ? note(36, 110, 0.1) : 0\n"
    "  (noise(0) > 0.6) ? note(42, 60, 0.05) : 0\n"
    "\n"
    "Example - one downbeat chord then nothing:\n"
    "  start:\n"
    "  note(60, 100, 1.0); note(64, 100, 1.0); note(67, 100, 1.0)";

const char* kMidiScriptLuaHelp =
    "MIDI Script (Lua) runs an embedded Lua 5.4 program that emits MIDI live.\n"
    "\n"
    "Program structure:\n"
    "  - Top-level code runs ONCE when the script loads. Use it to set up state\n"
    "    (globals persist across samples/blocks).\n"
    "  - function start()  optional - runs once when playback starts (downbeat).\n"
    "  - function loop()   required - runs every sample (Per sample) or every\n"
    "    block (Per block). This is where you emit MIDI.\n"
    "\n"
    "Emit functions (push MIDI at the current moment):\n"
    "  note(pitch, vel, durSec [, offset])  auto-releasing note\n"
    "  noteon(pitch, vel [, offset])        note-on only\n"
    "  noteoff(pitch [, offset])            note-off\n"
    "  cc(number, value [, offset])         control change (value 0..127)\n"
    "  bend(value [, offset])               pitch bend -1..1 (0 = centre)\n"
    "  The optional `offset` (Per block only) is the sample within the block to\n"
    "  stamp the event at (0..n-1) for sample-accurate timing.\n"
    "  Set the global `out = k` to route following emits to MIDI output pin k.\n"
    "\n"
    "Note names: the pitch argument accepts a quoted name like \"C4\" (= 60),\n"
    "  \"c#4\", \"Bb3\". Conversions: notenum(\"C4\") or notenum(\"C\", 4) -> 60;\n"
    "  notename(60) -> \"C4\"; notefreq(\"C4\") / notefreq(60) -> Hz via the\n"
    "  project tuning system.\n"
    "\n"
    "Variables (Per sample): t beat bar bpm playing sr dt note vel gate s1..sN.\n"
    "Variables (Per block):  n (block length) sr dt bpm playing rate\n"
    "  t/tStart/tEnd  beat/beatStart/beatEnd  note vel gate freq  s1..sN.\n"
    "  sig(k, i) reads signal pin k at sample i; shape(pos) samples the drawing.\n"
    "\n"
    "Maths: the math.* library plus aliases sin cos tan sqrt abs exp log floor\n"
    "  ceil min max pow(a,b) clamp(x,lo,hi) saw square triangle noise() pi.\n"
    "Sandboxed: no io/os/package/debug, no require/load/dofile.\n"
    "\n"
    "Example - four-on-the-floor kick (Per sample):\n"
    "  function loop()\n"
    "    local ph = beat - floor(beat)\n"
    "    if ph < dt*bpm/60 then note(36, 110, 0.1) end\n"
    "  end\n"
    "\n"
    "Example - sample-accurate hat grid (Per block): place one event exactly on\n"
    "every 1/4-beat boundary by scanning the block sample-by-sample:\n"
    "  function loop()\n"
    "    for i = 0, n-1 do\n"
    "      local b = beatStart + (beatEnd-beatStart) * i / n\n"
    "      if floor(b*4) > floor((b - (beatEnd-beatStart)/n)*4) then\n"
    "        note(42, 70, 0.05, i)   -- 4th arg = sample offset within the block\n"
    "      end\n"
    "    end\n"
    "  end";
} // namespace

MidiScriptEditorComponent::MidiScriptEditorComponent(NodeGraph& g, int nid,
                                                     std::function<void()> ch)
    : graph(g), nodeId(nid), onChanged(std::move(ch))
{
    loadFromNode();

    // Language + execution-rate control strip.
    {
        ScriptLangBar::Callbacks cbs;
        cbs.onLanguage = [this](ScriptLang l) {
            doc.language = l;
            updateLanguageUI();
            commitToNode();
        };
        cbs.onRate = [this](ScriptRate r) {
            doc.rate = r;
            updateLanguageUI();
            commitToNode();
        };
        langBar = std::make_unique<ScriptLangBar>(std::move(cbs));
        langBar->setState(doc.language, doc.rate);
        addAndMakeVisible(*langBar);
    }

    // .wasm file picker (only visible when the language is WebAssembly).
    wasmPanel = std::make_unique<WasmFilePanel>([this](std::string p) {
        doc.wasmPath = std::move(p);
        commitToNode();
    });
    wasmPanel->setPath(doc.wasmPath);
    addChildComponent(*wasmPanel);   // visibility toggled by updateLanguageUI()

    addAndMakeVisible(programLabel);
    addAndMakeVisible(programEditor);
    programEditor.setMultiLine(true, false);
    programEditor.setReturnKeyStartsNewLine(true);
    programEditor.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 14.0f,
                                     juce::Font::plain));
    programEditor.setText(doc.program, juce::dontSendNotification);
    programEditor.onTextChange = [this]() {
        doc.program = programEditor.getText().toStdString();
        commitToNode();
    };
    programEditor.setTooltip("Statements separated by ';' or new lines. Assignments persist\n"
                             "across samples. Emit with note/noteon/noteoff/cc/bend.\n"
                             "Click \"Language reference\" for the full list.");

    addAndMakeVisible(sigCountLabel);
    addAndMakeVisible(sigCountEditor);
    sigCountEditor.setInputRestrictions(2, "0123456789");
    sigCountEditor.setText(juce::String(doc.signalInputCount), juce::dontSendNotification);
    sigCountEditor.onTextChange = [this]() {
        int n = juce::jlimit(0, 16, sigCountEditor.getText().getIntValue());
        if (n != doc.signalInputCount) {
            doc.signalInputCount = n;
            syncPins();
            commitToNode();
        }
    };
    sigCountEditor.setTooltip("Number of incoming Signal pins exposed in the program "
                              "as s1, s2, ... sN. 0..16.");

    addAndMakeVisible(outCountLabel);
    addAndMakeVisible(outCountEditor);
    outCountEditor.setInputRestrictions(2, "0123456789");
    outCountEditor.setText(juce::String(doc.outputCount), juce::dontSendNotification);
    outCountEditor.onTextChange = [this]() {
        int n = juce::jlimit(1, 16, outCountEditor.getText().getIntValue());
        if (n != doc.outputCount) {
            doc.outputCount = n;
            syncPins();
            commitToNode();
        }
    };
    outCountEditor.setTooltip("Number of independent MIDI output pins (1..16). Route emits\n"
                              "to a pin by setting `out = k` (0-based) in the program.");

    addAndMakeVisible(midiInLabel);
    addAndMakeVisible(midiInEditor);
    midiInEditor.setInputRestrictions(2, "0123456789");
    midiInEditor.setText(juce::String(doc.midiInputCount), juce::dontSendNotification);
    midiInEditor.onTextChange = [this]() {
        int n = juce::jlimit(0, 16, midiInEditor.getText().getIntValue());
        if (n != doc.midiInputCount) {
            doc.midiInputCount = n;
            syncPins();
            commitToNode();
        }
    };
    midiInEditor.setTooltip("Number of MIDI input pins. 0 = none (note/vel/gate stay\n"
                            "idle); 1 = a single MIDI In. With >1 you get MIDI In 1..N,\n"
                            "and pollmidi()/midievent() tell you which input each event\n"
                            "arrived on (1-based). 0..16.");

    addAndMakeVisible(shapeLabel);
    {
        LayerStackComponent::Options lsOpts;
        lsOpts.showSummationPreview = true;
        lsOpts.summationPreviewHeight = 100;
        lsOpts.addLayerButtonText = "+ Layer";
        lsOpts.emptyHint = "Optional. Add a layer to give shape(pos) a curve to read "
                           "(until then shape() returns 0).";
        lsOpts.makeNewLayer = [](int count) {
            WaveLayer l;
            l.shape = WaveLayer::Sine;
            l.ratio = count + 1;
            l.phase = 0.0f;
            l.amp   = (count == 0) ? 1.0f : 0.5f;
            return l;
        };
        shapeStack = std::make_unique<LayerStackComponent>(
            std::move(lsOpts), [this]() { commitToNode(); });
        shapeStack->setTarget(&doc.shapeLayers);
        addAndMakeVisible(*shapeStack);
    }

    addAndMakeVisible(helpBtn);
    helpBtn.setTooltip(kMidiScriptHelp);
    helpBtn.onClick = [this]() {
        // Show the reference for whichever language is active.
        const char* msg = (doc.language == ScriptLang::Lua) ? kMidiScriptLuaHelp
                                                            : kMidiScriptHelp;
        const char* title = (doc.language == ScriptLang::Lua)
                                ? "MIDI Script - Lua reference"
                                : "MIDI Script - language reference";
        juce::NativeMessageBox::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::InfoIcon)
                .withTitle(title)
                .withMessage(msg)
                .withButton("OK")
                .withAssociatedComponent(this),
            nullptr);
    };

    addAndMakeVisible(closeBtn);
    closeBtn.onClick = [this]() {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->setVisible(false);
    };

    updateLanguageUI();
    setSize(700, 800);
}

MidiScriptEditorComponent::~MidiScriptEditorComponent() {
    // Per-keystroke commitToNode() writes node.script live (so the audio graph
    // hears edits immediately) but deliberately does NOT push an undo step -
    // that would fragment undo into one step per character and re-serialize the
    // whole graph on every keypress. The natural commit point for a modeless
    // dialog is when it closes, so we push a single snapshot here covering the
    // whole editing session. commitSnapshot de-dups against the previous step,
    // so closing without changes is a no-op; if anything changed it also sets
    // graph.dirty so the user is prompted to save on quit.
    graph.commitSnapshot("Edit MIDI Script");
}

void MidiScriptEditorComponent::loadFromNode() {
    auto* nd = graph.findNode(nodeId);
    if (!nd) { doc = MidiScriptDoc::defaultDoc(); return; }
    if (MidiScriptDoc::isMidiScriptScript(nd->script))
        doc.decode(nd->script);
    else
        doc = MidiScriptDoc::defaultDoc();
}

void MidiScriptEditorComponent::commitToNode() {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;
    nd->script = doc.encode();
    if (onChanged) onChanged();
}

void MidiScriptEditorComponent::updateLanguageUI() {
    const bool isWasm = (doc.language == ScriptLang::Wasm);
    // Wasm has no editable source text; show the file picker in the editor's
    // place. Builtin / Lua share the program text box.
    programEditor.setVisible(!isWasm);
    if (wasmPanel) wasmPanel->setVisible(isWasm);

    const char* caption =
        isWasm ? "WebAssembly module:"
               : (doc.language == ScriptLang::Lua
                      ? (doc.rate == ScriptRate::PerBlock
                             ? "Lua program (runs once per block):"
                             : "Lua program (runs once per sample):")
                      : "Program (runs once per sample):");
    programLabel.setText(caption, juce::dontSendNotification);

    // Point the program editor's tooltip + the help button at the active
    // language so the reference matches what's on screen.
    if (doc.language == ScriptLang::Lua) {
        helpBtn.setTooltip(kMidiScriptLuaHelp);
        programEditor.setTooltip("Lua program. Define function loop() (and optionally\n"
                                 "start()); top-level code runs once. Emit with\n"
                                 "note/noteon/noteoff/cc/bend. Click \"Language reference\".");
    } else {
        helpBtn.setTooltip(kMidiScriptHelp);
        programEditor.setTooltip("Statements separated by ';' or new lines. Assignments persist\n"
                                 "across samples. Emit with note/noteon/noteoff/cc/bend.\n"
                                 "Click \"Language reference\" for the full list.");
    }
}

bool MidiScriptEditorComponent::syncPins() {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return false;
    bool changed = false;

    // ---- Inputs: MIDI In pin(s) + N Signal pins (preserve ids by name) ----
    // 0 = none, 1 = a single "MIDI In", >1 = "MIDI In 1..N".
    {
        const int midiInCount = juce::jlimit(0, 16, doc.midiInputCount);
        std::vector<Pin> rebuilt;
        rebuilt.reserve(midiInCount + doc.signalInputCount);

        // Preserve existing MIDI-input pin ids by name so cables survive a
        // count change. Look up by the name we're about to assign.
        auto findMidiId = [&](const std::string& name) -> int {
            for (auto& p : nd->pinsIn)
                if (p.kind == PinKind::Midi && p.name == name) return p.id;
            return -1;
        };
        auto pushMidiIn = [&](const std::string& name) {
            int id = findMidiId(name);
            if (id < 0) id = graph.allocId();
            Pin mi; mi.id = id; mi.name = name;
            mi.kind = PinKind::Midi; mi.isInput = true;
            rebuilt.push_back(mi);
        };
        if (midiInCount == 1) {
            pushMidiIn("MIDI In");
        } else {
            for (int i = 0; i < midiInCount; ++i)
                pushMidiIn("MIDI In " + std::to_string(i + 1));
        }

        for (int i = 0; i < doc.signalInputCount; ++i) {
            std::string name = "s" + std::to_string(i + 1);
            int existingId = -1;
            for (auto& p : nd->pinsIn)
                if (p.kind == PinKind::Signal && p.name == name) { existingId = p.id; break; }
            Pin sp; sp.id = (existingId >= 0) ? existingId : graph.allocId();
            sp.name = name; sp.kind = PinKind::Signal; sp.isInput = true;
            rebuilt.push_back(sp);
        }

        if (rebuilt.size() != nd->pinsIn.size()) changed = true;
        else for (size_t i = 0; i < rebuilt.size(); ++i)
            if (rebuilt[i].id != nd->pinsIn[i].id || rebuilt[i].name != nd->pinsIn[i].name
                || rebuilt[i].kind != nd->pinsIn[i].kind) { changed = true; break; }
        if (changed) nd->pinsIn = std::move(rebuilt);
    }

    // ---- Outputs: M "MIDI Out k" pins (preserve ids by name) ----
    {
        std::vector<Pin> rebuilt;
        rebuilt.reserve(doc.outputCount);
        for (int i = 0; i < doc.outputCount; ++i) {
            std::string name = "MIDI Out " + std::to_string(i + 1);
            int existingId = -1;
            for (auto& p : nd->pinsOut)
                if (p.kind == PinKind::Midi && p.name == name) { existingId = p.id; break; }
            Pin op; op.id = (existingId >= 0) ? existingId : graph.allocId();
            op.name = name; op.kind = PinKind::Midi; op.isInput = false;
            rebuilt.push_back(op);
        }
        bool outChanged = false;
        if (rebuilt.size() != nd->pinsOut.size()) outChanged = true;
        else for (size_t i = 0; i < rebuilt.size(); ++i)
            if (rebuilt[i].id != nd->pinsOut[i].id || rebuilt[i].name != nd->pinsOut[i].name
                || rebuilt[i].kind != nd->pinsOut[i].kind) { outChanged = true; break; }
        if (outChanged) { nd->pinsOut = std::move(rebuilt); changed = true; }
    }

    return changed;
}

void MidiScriptEditorComponent::resized() {
    auto r = getLocalBounds().reduced(10);

    // Bottom button row first.
    auto btnRow = r.removeFromBottom(28);
    helpBtn.setBounds(btnRow.removeFromLeft(180));
    closeBtn.setBounds(btnRow.removeFromRight(80));
    r.removeFromBottom(8);

    // Counts row.
    {
        auto row = r.removeFromBottom(24);
        sigCountLabel.setBounds(row.removeFromLeft(150));
        sigCountEditor.setBounds(row.removeFromLeft(50));
        row.removeFromLeft(20);
        outCountLabel.setBounds(row.removeFromLeft(90));
        outCountEditor.setBounds(row.removeFromLeft(50));
        row.removeFromLeft(20);
        midiInLabel.setBounds(row.removeFromLeft(80));
        midiInEditor.setBounds(row.removeFromLeft(50));
        r.removeFromBottom(8);
    }

    // Shape stack occupies the lower portion (fixed-ish height); the program
    // editor takes the rest at the top.
    shapeLabel.setBounds(r.removeFromBottom(18).withTrimmedTop(2));
    if (shapeStack) {
        int shapeH = juce::jlimit(160, 380, r.getHeight() / 2);
        shapeStack->setBounds(r.removeFromBottom(shapeH));
        r.removeFromBottom(8);
    }

    // Language + rate control strip sits above the program editor.
    if (langBar) {
        langBar->setBounds(r.removeFromTop(ScriptLangBar::kHeight));
        r.removeFromTop(8);
    }

    programLabel.setBounds(r.removeFromTop(18));
    // The program editor and the .wasm file panel share the same rectangle;
    // updateLanguageUI() decides which one is visible.
    programEditor.setBounds(r);
    if (wasmPanel) wasmPanel->setBounds(r);
}

void MidiScriptEditorComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff1e1e22));
}

} // namespace SoundShop

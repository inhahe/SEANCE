#define _USE_MATH_DEFINES
#include "trigger_node.h"
#include "help_utils.h"
#include <cmath>
#include <sstream>
#include <algorithm>

namespace SoundShop {

// ============================================================================
// Encode / decode
// ============================================================================
//
// Format: __trigger__:v1|rule;rule;rule;...
//   where each rule is a comma-separated field list in a fixed order.
// Fields are identified positionally, not by name, so a version bump lets
// us add new trailing fields without breaking old saves.

static const char* shapeName(TriggerShape s) {
    switch (s) {
        case TriggerShape::Step:         return "step";
        case TriggerShape::Envelope:     return "env";
        case TriggerShape::Ramp:         return "ramp";
        case TriggerShape::FromVelocity: return "vel";
    }
    return "env";
}

static TriggerShape parseShape(const std::string& s) {
    if (s == "step") return TriggerShape::Step;
    if (s == "ramp") return TriggerShape::Ramp;
    if (s == "vel")  return TriggerShape::FromVelocity;
    return TriggerShape::Envelope;
}

// Percent-encode so commas/semicolons inside labels don't break the format.
static std::string pctEncode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == ',' || c == ';' || c == '|' || c == ':' || c == '%') {
            char buf[5];
            std::snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
            out += buf;
        } else {
            out += c;
        }
    }
    return out;
}
static std::string pctDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int v = 0;
            std::sscanf(s.c_str() + i + 1, "%2x", &v);
            out += (char)v;
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string TriggerDoc::encode() const {
    std::ostringstream o;
    o << "__trigger__:v1|";
    for (size_t i = 0; i < rules.size(); ++i) {
        if (i > 0) o << ";";
        const auto& r = rules[i];
        o << r.minPitch     << "," << r.maxPitch
          << "," << r.minVel << "," << r.maxVel
          << "," << r.probability
          << "," << (int)r.target
          << "," << r.pitchOffset
          << "," << r.velocityDelta
          << "," << r.delayBeats
          << "," << r.lengthBeats
          << "," << r.outChannel
          << "," << shapeName(r.shape)
          << "," << r.attackMs
          << "," << r.decayMs
          << "," << r.sustainLevel
          << "," << r.releaseMs
          << "," << r.peakValue
          << "," << r.restValue
          << "," << r.rampDurationMs
          << "," << r.holdMs
          << "," << r.velocityScale
          << "," << r.velocityOffset
          << "," << pctEncode(r.label);
    }
    return o.str();
}

static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= s.size()) {
        size_t n = s.find(sep, p);
        if (n == std::string::npos) n = s.size();
        out.push_back(s.substr(p, n - p));
        p = n + 1;
    }
    return out;
}

bool TriggerDoc::decode(const std::string& s) {
    rules.clear();
    if (s.rfind("__trigger__:", 0) != 0) return false;
    std::string body = s.substr(std::string("__trigger__:").size());
    auto barPos = body.find('|');
    if (barPos == std::string::npos) return true; // empty doc
    std::string rest = body.substr(barPos + 1);
    if (rest.empty()) return true;

    auto ruleStrs = split(rest, ';');
    for (auto& rs : ruleStrs) {
        if (rs.empty()) continue;
        auto f = split(rs, ',');
        if (f.size() < 22) continue;
        TriggerRule r;
        auto toI = [](const std::string& x, int def) {
            try { return std::stoi(x); } catch (...) { return def; }
        };
        auto toF = [](const std::string& x, float def) {
            try { return std::stof(x); } catch (...) { return def; }
        };
        int idx = 0;
        r.minPitch     = toI(f[idx++], 0);
        r.maxPitch     = toI(f[idx++], 127);
        r.minVel       = toI(f[idx++], 1);
        r.maxVel       = toI(f[idx++], 127);
        r.probability  = toF(f[idx++], 1.0f);
        r.target       = (TriggerTarget)toI(f[idx++], 0);
        r.pitchOffset  = toI(f[idx++], 12);
        r.velocityDelta = toI(f[idx++], 0);
        r.delayBeats   = toF(f[idx++], 0.0f);
        r.lengthBeats  = toF(f[idx++], 0.25f);
        r.outChannel   = toI(f[idx++], 1);
        r.shape        = parseShape(f[idx++]);
        r.attackMs     = toF(f[idx++], 10.0f);
        r.decayMs      = toF(f[idx++], 100.0f);
        r.sustainLevel = toF(f[idx++], 0.3f);
        r.releaseMs    = toF(f[idx++], 200.0f);
        r.peakValue    = toF(f[idx++], 1.0f);
        r.restValue    = toF(f[idx++], 0.0f);
        r.rampDurationMs = toF(f[idx++], 500.0f);
        r.holdMs       = toF(f[idx++], 100.0f);
        r.velocityScale = toF(f[idx++], 1.0f);
        r.velocityOffset = toF(f[idx++], 0.0f);
        if ((int)f.size() > idx) r.label = pctDecode(f[idx++]);
        rules.push_back(std::move(r));
    }
    return true;
}

// ============================================================================
// Preset factories
// ============================================================================

TriggerDoc TriggerDoc::defaultDoc() {
    // A harmless default: one MIDI rule that doubles each note one octave up.
    TriggerDoc d;
    TriggerRule r;
    r.label = "Octave up";
    r.target = TriggerTarget::Midi;
    r.pitchOffset = 12;
    r.lengthBeats = 0.25f;
    d.rules.push_back(r);
    return d;
}

TriggerDoc TriggerDoc::presetOctaveDouble() {
    TriggerDoc d;
    TriggerRule r;
    r.label = "Octave up";
    r.target = TriggerTarget::Midi;
    r.pitchOffset = 12;
    r.lengthBeats = 0.25f;
    d.rules.push_back(r);
    return d;
}

TriggerDoc TriggerDoc::presetChordMajor() {
    TriggerDoc d;
    for (int semi : {4, 7}) {
        TriggerRule r;
        r.label  = (semi == 4) ? "Major 3rd" : "Perfect 5th";
        r.target = TriggerTarget::Midi;
        r.pitchOffset = semi;
        r.lengthBeats = 0.25f;
        d.rules.push_back(r);
    }
    return d;
}

TriggerDoc TriggerDoc::presetFlam() {
    TriggerDoc d;
    // Two soft ghost copies at 1/32 and 2/32 after the hit.
    for (int i = 1; i <= 2; ++i) {
        TriggerRule r;
        r.label = std::string("Flam ") + std::to_string(i);
        r.target = TriggerTarget::Midi;
        r.pitchOffset = 0;
        r.velocityDelta = -30 * i;
        r.delayBeats = (float)i / 32.0f; // 1/32 and 2/32 notes
        r.lengthBeats = 0.125f;
        d.rules.push_back(r);
    }
    return d;
}

TriggerDoc TriggerDoc::presetPluckEnvelope() {
    TriggerDoc d;
    TriggerRule r;
    r.label = "Pluck env";
    r.target = TriggerTarget::Signal;
    r.shape = TriggerShape::Envelope;
    r.attackMs = 2.0f;
    r.decayMs = 200.0f;
    r.sustainLevel = 0.0f;
    r.releaseMs = 0.0f;
    r.peakValue = 1.0f;
    r.restValue = 0.0f;
    d.rules.push_back(r);
    return d;
}

TriggerDoc TriggerDoc::presetVelocityFollower() {
    TriggerDoc d;
    TriggerRule r;
    r.label = "Velocity follow";
    r.target = TriggerTarget::Signal;
    r.shape = TriggerShape::FromVelocity;
    r.velocityScale = 1.0f;
    r.velocityOffset = 0.0f;
    r.holdMs = 1000.0f;
    d.rules.push_back(r);
    return d;
}

// ============================================================================
// TriggerProcessor
// ============================================================================

TriggerProcessor::TriggerProcessor(Node& n, Transport& t)
    : node(n), transport(t)
{
    cachedScript = node.script;
    if (!doc.decode(cachedScript))
        doc = TriggerDoc::defaultDoc();
}

void TriggerProcessor::rereadDocIfChanged() {
    if (node.script != cachedScript) {
        cachedScript = node.script;
        if (!doc.decode(cachedScript))
            doc = TriggerDoc::defaultDoc();
    }
}

void TriggerProcessor::scheduleRule(const TriggerRule& r,
                                    const juce::MidiMessage& trig,
                                    int64_t trigSample)
{
    // Delay in samples (beats * bpm / 60 * sampleRate)
    double beatsPerSample = transport.bpm / (60.0 * sampleRate);
    int64_t delaySamples = (int64_t)((double)r.delayBeats / std::max(1e-9, beatsPerSample));

    if (r.target == TriggerTarget::Midi) {
        int outPitch = juce::jlimit(0, 127, trig.getNoteNumber() + r.pitchOffset);
        int outVel = juce::jlimit(1, 127, (int)trig.getVelocity() + r.velocityDelta);
        int ch = juce::jlimit(1, 16, r.outChannel);
        int64_t onSample = trigSample + delaySamples;
        int64_t offSample = onSample +
            (int64_t)((double)r.lengthBeats / std::max(1e-9, beatsPerSample));

        pendingMidi.push_back({
            onSample,
            juce::MidiMessage::noteOn(ch, outPitch, (juce::uint8)outVel)
        });
        pendingMidi.push_back({
            offSample,
            juce::MidiMessage::noteOff(ch, outPitch)
        });
    } else {
        // Signal: snapshot rule params into an ActiveShape
        ActiveShape s;
        s.startSample = trigSample + delaySamples;
        s.shape = r.shape;
        s.attackSamples  = r.attackMs  * 0.001f * (float)sampleRate;
        s.decaySamples   = r.decayMs   * 0.001f * (float)sampleRate;
        s.releaseSamples = r.releaseMs * 0.001f * (float)sampleRate;
        s.sustainLevel = r.sustainLevel;
        s.peakValue = r.peakValue;
        s.restValue = r.restValue;
        s.rampDurationSamples = r.rampDurationMs * 0.001f * (float)sampleRate;
        s.holdSamples = r.holdMs * 0.001f * (float)sampleRate;
        s.velocityNorm = (float)trig.getVelocity() / 127.0f;
        s.velocityScale = r.velocityScale;
        s.velocityOffset = r.velocityOffset;
        activeShapes.push_back(s);
    }
}

float TriggerProcessor::evalShape(const ActiveShape& s, int64_t nowSample, bool& expired) const {
    expired = false;
    float t = (float)(nowSample - s.startSample);
    if (t < 0) return s.restValue;

    switch (s.shape) {
        case TriggerShape::Step: {
            // Jump to peak, hold, jump back. hold -> expires.
            if (t < s.holdSamples) return s.peakValue;
            expired = true;
            return s.restValue;
        }
        case TriggerShape::Envelope: {
            float a = std::max(1.0f, s.attackSamples);
            float d = std::max(1.0f, s.decaySamples);
            float rel = std::max(0.0f, s.releaseSamples);
            float sus = s.peakValue * s.sustainLevel;
            // attack: 0 .. peak over attack
            if (t < a) {
                return s.restValue + (s.peakValue - s.restValue) * (t / a);
            }
            t -= a;
            // decay: peak .. sustain over decay
            if (t < d) {
                return s.peakValue + (sus - s.peakValue) * (t / d);
            }
            t -= d;
            // sustain hold (using holdMs as sustain duration)
            float holdS = std::max(0.0f, s.holdSamples);
            if (t < holdS) return sus;
            t -= holdS;
            // release
            if (t < rel) {
                return sus + (s.restValue - sus) * (t / rel);
            }
            expired = true;
            return s.restValue;
        }
        case TriggerShape::Ramp: {
            float dur = std::max(1.0f, s.rampDurationSamples);
            if (t < dur) {
                return s.restValue + (s.peakValue - s.restValue) * (t / dur);
            }
            // Hold at peak; expire after an additional holdSamples.
            if (t < dur + std::max(0.0f, s.holdSamples)) return s.peakValue;
            expired = true;
            return s.peakValue;
        }
        case TriggerShape::FromVelocity: {
            float v = s.velocityNorm * s.velocityScale + s.velocityOffset;
            if (t < std::max(1.0f, s.holdSamples)) return v;
            expired = true;
            return s.restValue;
        }
    }
    expired = true;
    return s.restValue;
}

void TriggerProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) {
    rereadDocIfChanged();

    int numSamples = buf.getNumSamples();
    int64_t blockStart = absSampleTime;
    int64_t blockEnd = blockStart + numSamples;

    // 1. Pass through input MIDI AND evaluate rules on note-ons.
    juce::MidiBuffer output;
    for (auto meta : midi) {
        auto msg = meta.getMessage();
        int sampleOffset = meta.samplePosition;
        int64_t trigSample = blockStart + sampleOffset;

        output.addEvent(msg, sampleOffset);

        if (msg.isNoteOn()) {
            int pitch = msg.getNoteNumber();
            int vel = msg.getVelocity();
            for (auto& r : doc.rules) {
                if (pitch < r.minPitch || pitch > r.maxPitch) continue;
                if (vel < r.minVel || vel > r.maxVel) continue;
                if (r.probability < 1.0f) {
                    float u = (float)rng() / (float)rng.max();
                    if (u > r.probability) continue;
                }
                scheduleRule(r, msg, trigSample);
            }
        }
    }

    // 2. Drain scheduled MIDI events whose fire time falls in this block.
    for (auto it = pendingMidi.begin(); it != pendingMidi.end();) {
        if (it->sampleTime < blockEnd) {
            // If the fire time is in the past (e.g. block jump), clamp to 0.
            int off = (int)std::max<int64_t>(0, it->sampleTime - blockStart);
            if (off >= numSamples) off = numSamples - 1;
            output.addEvent(it->msg, off);
            it = pendingMidi.erase(it);
        } else {
            ++it;
        }
    }
    midi.swapWith(output);

    // 3. Render signal output into audio channel 0 (the Signal out pin).
    // Multiple active shapes: last-added wins on overlap (replace policy).
    // If no shapes are active we emit the default rest value (0.0f via clear).
    int numCh = buf.getNumChannels();
    if (numCh > 0) {
        // Initialize to 0 (or the "rest" of the last still-active shape).
        auto* out = buf.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i) out[i] = 0.0f;

        // Iterate shapes in order; the last write for a given sample wins.
        for (auto it = activeShapes.begin(); it != activeShapes.end();) {
            bool anySampleInBlock = false;
            bool expiredAtBlockEnd = false;
            for (int i = 0; i < numSamples; ++i) {
                int64_t now = blockStart + i;
                if (now < it->startSample) continue;
                bool expired = false;
                float v = evalShape(*it, now, expired);
                out[i] = v;
                anySampleInBlock = true;
                if (expired) {
                    expiredAtBlockEnd = true;
                    break;
                }
            }
            (void)anySampleInBlock;
            if (expiredAtBlockEnd) {
                it = activeShapes.erase(it);
            } else {
                ++it;
            }
        }
    }

    absSampleTime = blockEnd;

    // 4. Safety net: cap queue size so a runaway can't eat memory.
    if (pendingMidi.size() > 4096)
        pendingMidi.erase(pendingMidi.begin(), pendingMidi.begin() + 2048);
    if (activeShapes.size() > 64)
        activeShapes.erase(activeShapes.begin(), activeShapes.begin() + 32);
}

// ============================================================================
// TriggerEditorComponent — minimal editor
// ============================================================================
//
// Each rule row shows: target label + preset params inline + delete button.
// Param editing is via small sliders on the row. Preset buttons at the top
// overwrite the rule list with a ready-to-use config.

class TriggerEditorComponent::RuleRow : public juce::Component {
public:
    RuleRow(TriggerEditorComponent& owner_, int index_)
        : owner(owner_), index(index_)
    {
        addAndMakeVisible(label);
        label.setFont(12.0f);
        label.setJustificationType(juce::Justification::centredLeft);

        addAndMakeVisible(targetLabel);
        targetLabel.setFont(11.0f);

        auto setupSlider = [this](juce::Slider& s, double lo, double hi, double step,
                                  const char* suffix) {
            addAndMakeVisible(s);
            s.setSliderStyle(juce::Slider::LinearHorizontal);
            s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 16);
            s.setRange(lo, hi, step);
            s.setTextValueSuffix(suffix);
            s.onValueChange = [this]() { applyToRule(); };
        };
        setupSlider(a, -24, 24, 1, " st");
        setupSlider(b, 0, 2, 0.01, " beats");
        setupSlider(c, 0, 4, 0.01, " beats");
        setupSlider(d, 0, 1, 0.01, "");

        aLbl.setText("Pitch/Peak", juce::dontSendNotification);
        bLbl.setText("Delay",      juce::dontSendNotification);
        cLbl.setText("Length",     juce::dontSendNotification);
        dLbl.setText("Prob",       juce::dontSendNotification);
        for (auto* l : {&aLbl, &bLbl, &cLbl, &dLbl}) {
            addAndMakeVisible(*l);
            l->setFont(10.0f);
            l->setJustificationType(juce::Justification::centredLeft);
        }

        addAndMakeVisible(deleteBtn);
        deleteBtn.setButtonText("X");
        deleteBtn.onClick = [this]() {
            owner.doc.rules.erase(owner.doc.rules.begin() + index);
            owner.rebuildRows();
            owner.onDocChanged();
        };
    }

    void syncFromRule() {
        auto& r = owner.doc.rules[index];
        juce::String lbl = r.label.empty()
            ? (r.target == TriggerTarget::Midi ? juce::String("MIDI rule") : juce::String("Signal rule"))
            : juce::String(r.label);
        label.setText(lbl, juce::dontSendNotification);

        targetLabel.setText(r.target == TriggerTarget::Midi ? "[MIDI]" : "[Signal]",
                            juce::dontSendNotification);

        if (r.target == TriggerTarget::Midi) {
            aLbl.setText("Pitch",  juce::dontSendNotification);
            a.setRange(-48, 48, 1);
            a.setValue((double)r.pitchOffset, juce::dontSendNotification);
            a.setTextValueSuffix(" st");
            cLbl.setText("Length", juce::dontSendNotification);
            c.setRange(0.001, 4, 0.001);
            c.setValue((double)r.lengthBeats, juce::dontSendNotification);
            c.setTextValueSuffix(" b");
        } else {
            aLbl.setText("Peak", juce::dontSendNotification);
            a.setRange(-1, 1, 0.01);
            a.setValue((double)r.peakValue, juce::dontSendNotification);
            a.setTextValueSuffix("");
            cLbl.setText("Dur", juce::dontSendNotification);
            c.setRange(1, 2000, 1);
            c.setValue((double)(r.shape == TriggerShape::Ramp ? r.rampDurationMs
                                                              : r.decayMs),
                       juce::dontSendNotification);
            c.setTextValueSuffix(" ms");
        }
        b.setValue((double)r.delayBeats, juce::dontSendNotification);
        d.setValue((double)r.probability, juce::dontSendNotification);
    }

    void applyToRule() {
        if (index < 0 || index >= (int)owner.doc.rules.size()) return;
        auto& r = owner.doc.rules[index];
        if (r.target == TriggerTarget::Midi) {
            r.pitchOffset = (int)a.getValue();
            r.lengthBeats = (float)c.getValue();
        } else {
            r.peakValue = (float)a.getValue();
            if (r.shape == TriggerShape::Ramp) r.rampDurationMs = (float)c.getValue();
            else                                r.decayMs        = (float)c.getValue();
        }
        r.delayBeats = (float)b.getValue();
        r.probability = (float)d.getValue();
        owner.onDocChanged();
    }

    void resized() override {
        auto r = getLocalBounds().reduced(4);
        auto top = r.removeFromTop(16);
        label.setBounds(top.removeFromLeft(140));
        targetLabel.setBounds(top.removeFromLeft(60));
        deleteBtn.setBounds(top.removeFromRight(22));

        r.removeFromTop(2);
        auto body = r;
        int w = body.getWidth() / 4;
        auto slot = [&](juce::Label& l, juce::Slider& s) {
            auto col = body.removeFromLeft(w).reduced(2, 0);
            l.setBounds(col.removeFromTop(12));
            s.setBounds(col);
        };
        slot(aLbl, a);
        slot(bLbl, b);
        slot(cLbl, c);
        slot(dLbl, d);
    }

    void paint(juce::Graphics& g) override {
        auto& r = owner.doc.rules[index];
        bool midi = (r.target == TriggerTarget::Midi);
        auto accent = midi ? juce::Colour(60, 120, 180) : juce::Colour(180, 120, 40);
        g.setColour(juce::Colour(36, 36, 46));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 3.0f);
        g.setColour(accent.withAlpha(0.7f));
        g.drawRoundedRectangle(getLocalBounds().toFloat(), 3.0f, 1.0f);
    }

    static int rowHeight() { return 56; }

private:
    TriggerEditorComponent& owner;
    int index;
    juce::Label label, targetLabel;
    juce::Label aLbl, bLbl, cLbl, dLbl;
    juce::Slider a, b, c, d;
    juce::TextButton deleteBtn;
};

TriggerEditorComponent::TriggerEditorComponent(NodeGraph& g, int nid, std::function<void()> apply)
    : graph(g), nodeId(nid), onApply(std::move(apply))
{
    auto* nd = graph.findNode(nodeId);
    if (nd && !doc.decode(nd->script))
        doc = TriggerDoc::defaultDoc();

    addAndMakeVisible(addMidiBtn);
    addMidiBtn.setTooltip("Add a new MIDI rule — sends a MIDI note (transposed by some number of semitones) "
                          "in response to incoming notes. Use to create harmonies, octave doubles, chords, etc.");
    addMidiBtn.onClick = [this]() {
        TriggerRule r;
        r.label = "MIDI rule";
        r.target = TriggerTarget::Midi;
        doc.rules.push_back(r);
        rebuildRows();
        onDocChanged();
    };

    addAndMakeVisible(addSignalBtn);
    addSignalBtn.setTooltip("Add a new signal rule — generates a control signal (envelope, ramp, step) "
                            "triggered by incoming MIDI notes. Wire its Signal output into a synth parameter to modulate.");
    addSignalBtn.onClick = [this]() {
        TriggerRule r;
        r.label = "Signal rule";
        r.target = TriggerTarget::Signal;
        r.shape  = TriggerShape::Envelope;
        doc.rules.push_back(r);
        rebuildRows();
        onDocChanged();
    };

    auto wirePreset = [this](juce::TextButton& btn, TriggerDoc (*fn)()) {
        addAndMakeVisible(btn);
        btn.onClick = [this, fn]() { loadPreset(fn()); };
    };
    wirePreset(presetOctaveBtn,    &TriggerDoc::presetOctaveDouble);
    wirePreset(presetChordBtn,     &TriggerDoc::presetChordMajor);
    wirePreset(presetFlamBtn,      &TriggerDoc::presetFlam);
    wirePreset(presetPluckBtn,     &TriggerDoc::presetPluckEnvelope);
    wirePreset(presetVelFollowBtn, &TriggerDoc::presetVelocityFollower);
    presetOctaveBtn.setTooltip("Load a preset that doubles every incoming note one octave higher");
    presetChordBtn.setTooltip("Load a preset that turns each incoming note into a major chord (root, third, fifth)");
    presetFlamBtn.setTooltip("Load a preset that adds a quick echo of each note ~30ms later — drum 'flam' effect");
    presetPluckBtn.setTooltip("Load a preset that fires a short envelope on every note — useful as a pluck/percussion modulator");
    presetVelFollowBtn.setTooltip("Load a preset that outputs a signal proportional to each note's velocity — drives parameters from how hard you play");

    addAndMakeVisible(helpBtn);
    helpBtn.setTooltip("Open the Trigger node docs");
    helpBtn.onClick = []() { openHelpDocFile("trigger-node.html"); };

    addAndMakeVisible(applyBtn);
    applyBtn.setTooltip("Save the current rule list to the Trigger node without closing this editor");
    applyBtn.onClick = [this]() { commitToNode(); if (onApply) onApply(); };

    addAndMakeVisible(closeBtn);
    closeBtn.onClick = [this]() {
        commitToNode();
        if (onApply) onApply();
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
            juce::Component::SafePointer<juce::DialogWindow> safe(dw);
            juce::MessageManager::callAsync([safe]() {
                if (safe) delete safe.getComponent();
            });
        }
    };

    addAndMakeVisible(rulesViewport);
    rulesViewport.setViewedComponent(&rulesContainer, false);
    rulesViewport.setScrollBarsShown(true, false);

    rebuildRows();
    setSize(720, 520);
}

void TriggerEditorComponent::loadPreset(const TriggerDoc& preset) {
    doc = preset;
    rebuildRows();
    onDocChanged();
}

void TriggerEditorComponent::rebuildRows() {
    rows.clear();
    rulesContainer.removeAllChildren();
    int y = 0;
    int rh = RuleRow::rowHeight();
    int vw = std::max(rulesViewport.getWidth(), 640);
    for (int i = 0; i < (int)doc.rules.size(); ++i) {
        auto row = std::make_unique<RuleRow>(*this, i);
        row->setBounds(0, y, vw, rh);
        row->syncFromRule();
        rulesContainer.addAndMakeVisible(row.get());
        rows.push_back(std::move(row));
        y += rh + 4;
    }
    rulesContainer.setSize(vw, std::max(y, 10));
}

void TriggerEditorComponent::commitToNode() {
    if (auto* nd = graph.findNode(nodeId))
        nd->script = doc.encode();
}

void TriggerEditorComponent::onDocChanged() {
    commitToNode();
    if (onApply) onApply();
}

void TriggerEditorComponent::resized() {
    auto a = getLocalBounds().reduced(8);

    auto top = a.removeFromTop(26);
    addMidiBtn.setBounds(top.removeFromLeft(90));
    top.removeFromLeft(4);
    addSignalBtn.setBounds(top.removeFromLeft(100));
    closeBtn.setBounds(top.removeFromRight(60));
    top.removeFromRight(4);
    applyBtn.setBounds(top.removeFromRight(60));
    top.removeFromRight(4);
    helpBtn.setBounds(top.removeFromRight(26));

    a.removeFromTop(4);
    auto presetRow = a.removeFromTop(24);
    presetOctaveBtn.setBounds(presetRow.removeFromLeft(110));
    presetRow.removeFromLeft(4);
    presetChordBtn.setBounds(presetRow.removeFromLeft(110));
    presetRow.removeFromLeft(4);
    presetFlamBtn.setBounds(presetRow.removeFromLeft(70));
    presetRow.removeFromLeft(4);
    presetPluckBtn.setBounds(presetRow.removeFromLeft(110));
    presetRow.removeFromLeft(4);
    presetVelFollowBtn.setBounds(presetRow.removeFromLeft(130));

    a.removeFromTop(6);
    rulesViewport.setBounds(a);

    // Re-layout rows to viewport width
    int vw = rulesViewport.getWidth();
    int rh = RuleRow::rowHeight();
    int y = 0;
    for (auto& row : rows) {
        row->setBounds(0, y, vw, rh);
        y += rh + 4;
    }
    rulesContainer.setSize(vw, std::max(y, 10));
}

void TriggerEditorComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(22, 22, 28));
    g.setColour(juce::Colours::grey);
    g.setFont(11.0f);
    g.drawText("Each rule fires on matching incoming notes. "
               "MIDI rules emit notes on the MIDI out pin; "
               "signal rules drive the Signal out pin.",
               getLocalBounds().reduced(10, 4),
               juce::Justification::topRight, true);
}

} // namespace SoundShop

#include "midi_mod_node.h"
#include <sstream>
#include <algorithm>
#include <cmath>

namespace SoundShop {

// ============================================================================
// Encode / decode
// ============================================================================
//
// Format: __midimod__:target,amount,ccNum;target,amount,ccNum;...
//   target: int matching ModTarget enum
//   amount: float
//   ccNum:  int, only meaningful when target == CC

static const char* kMidiModPrefix = "__midimod__:";
static const char* kLegacyVelScale = "__velscale__";

std::string MidiModDoc::encode() const {
    std::ostringstream o;
    o << kMidiModPrefix;
    for (size_t i = 0; i < rules.size(); ++i) {
        if (i > 0) o << ";";
        const auto& r = rules[i];
        o << (int)r.target << "," << r.amount << "," << r.ccNumber;
    }
    return o.str();
}

bool MidiModDoc::decode(const std::string& s) {
    rules.clear();
    if (s.rfind(kMidiModPrefix, 0) != 0) return false;
    std::string body = s.substr(std::strlen(kMidiModPrefix));
    if (body.empty()) return true;

    size_t p = 0;
    while (p <= body.size()) {
        size_t n = body.find(';', p);
        if (n == std::string::npos) n = body.size();
        std::string part = body.substr(p, n - p);
        if (!part.empty()) {
            // Split on ','
            int t = 0; float a = 1.0f; int cc = 74;
            size_t c1 = part.find(',');
            size_t c2 = (c1 != std::string::npos) ? part.find(',', c1 + 1) : std::string::npos;
            try { if (c1 != std::string::npos) t  = std::stoi(part.substr(0, c1)); } catch (...) {}
            try { if (c1 != std::string::npos && c2 != std::string::npos)
                    a  = std::stof(part.substr(c1 + 1, c2 - c1 - 1));
                  else if (c1 != std::string::npos)
                    a  = std::stof(part.substr(c1 + 1)); } catch (...) {}
            try { if (c2 != std::string::npos)
                    cc = std::stoi(part.substr(c2 + 1)); } catch (...) {}
            ModRule r;
            r.target = (ModTarget)juce::jlimit(0, 4, t);
            r.amount = a;
            r.ccNumber = juce::jlimit(0, 127, cc);
            rules.push_back(r);
        }
        p = n + 1;
    }
    return true;
}

MidiModDoc MidiModDoc::defaultDoc() {
    MidiModDoc d;
    ModRule r;
    r.target = ModTarget::Velocity;
    r.amount = 1.0f;
    d.rules.push_back(r);
    return d;
}

MidiModDoc MidiModDoc::fromLegacyVelScale() {
    // Old VelocityScale node: single velocity rule. The old node's
    // "Sensitivity" param controlled the amount; we can't read node
    // params from here cleanly, so just default to 1.0 and let the user
    // tweak. Old projects that had explicitly set Sensitivity will see
    // velocity=1 until they open the editor.
    return defaultDoc();
}

// ============================================================================
// Processor
// ============================================================================

void MidiModulatorProcessor::rereadDocIfChanged() {
    if (node.script != cachedScript) {
        cachedScript = node.script;
        if (cachedScript == kLegacyVelScale)
            doc = MidiModDoc::fromLegacyVelScale();
        else if (!doc.decode(cachedScript))
            doc = MidiModDoc::defaultDoc();
    }
}

void MidiModulatorProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) {
    rereadDocIfChanged();

    int numSamples = buf.getNumSamples();
    int numCh = buf.getNumChannels();

    // Each rule corresponds to signal input index i — which is audio
    // channel (2 + i) on this processor's input buffer. Signal inputs
    // start at channel 2 because 0/1 are reserved for stereo audio.
    auto readSigAt = [&](int ruleIdx, int sampleOffset) -> float {
        int ch = 2 + ruleIdx;
        if (ch >= numCh || sampleOffset < 0 || sampleOffset >= numSamples)
            return 0.0f;
        return buf.getSample(ch, sampleOffset);
    };

    juce::MidiBuffer output;

    // 1. Emit per-block continuous modulation events (pitch bend, mod
    //    wheel, aftertouch, CC). One message at the start of the block
    //    per rule, on the most-recently-active channel. Velocity rules
    //    are skipped here; they apply per note-on in step 2.
    int midpoint = std::max(0, numSamples / 2);
    for (size_t i = 0; i < doc.rules.size(); ++i) {
        const auto& r = doc.rules[i];
        if (r.target == ModTarget::Velocity) continue;
        if (std::abs(r.amount) < 1e-6f) continue;

        float sig = readSigAt((int)i, midpoint);
        int ch = juce::jlimit(1, 16, lastSeenChannel);

        switch (r.target) {
            case ModTarget::PitchBend: {
                // sig -1..+1 scales to -amount..+amount semitones around
                // center, then back to the 14-bit range assuming ±2 semi
                // is the typical synth bend range (value 16383 = +100%).
                float bendFrac = juce::jlimit(-1.0f, 1.0f, sig * r.amount);
                int bendVal = 8192 + (int)std::round(bendFrac * 8191.0f);
                bendVal = juce::jlimit(0, 16383, bendVal);
                output.addEvent(juce::MidiMessage::pitchWheel(ch, bendVal), 0);
                break;
            }
            case ModTarget::ModWheel: {
                // sig -1..+1 -> 0..1 -> 0..127 scaled by amount
                float n = juce::jlimit(0.0f, 1.0f, (sig + 1.0f) * 0.5f * r.amount);
                int v = (int)std::round(n * 127.0f);
                v = juce::jlimit(0, 127, v);
                output.addEvent(juce::MidiMessage::controllerEvent(ch, 1, v), 0);
                break;
            }
            case ModTarget::Aftertouch: {
                float n = juce::jlimit(0.0f, 1.0f, (sig + 1.0f) * 0.5f * r.amount);
                int v = (int)std::round(n * 127.0f);
                v = juce::jlimit(0, 127, v);
                output.addEvent(juce::MidiMessage::channelPressureChange(ch, v), 0);
                break;
            }
            case ModTarget::CC: {
                float n = juce::jlimit(0.0f, 1.0f, (sig + 1.0f) * 0.5f * r.amount);
                int v = (int)std::round(n * 127.0f);
                v = juce::jlimit(0, 127, v);
                output.addEvent(juce::MidiMessage::controllerEvent(ch, r.ccNumber, v), 0);
                break;
            }
            default: break;
        }
    }

    // 2. Pass through incoming MIDI, applying velocity rules to note-ons
    //    at the note-on's exact sample offset for sample-accurate timing.
    for (auto meta : midi) {
        auto msg = meta.getMessage();
        int off = meta.samplePosition;

        if (msg.isNoteOn()) {
            lastSeenChannel = msg.getChannel();
            float velScale = 1.0f;
            for (size_t i = 0; i < doc.rules.size(); ++i) {
                if (doc.rules[i].target != ModTarget::Velocity) continue;
                float sig = readSigAt((int)i, off);
                velScale *= 1.0f + doc.rules[i].amount * sig;
            }
            int newVel = juce::jlimit(1, 127,
                (int)std::round(msg.getVelocity() * velScale));
            auto m = juce::MidiMessage::noteOn(
                msg.getChannel(), msg.getNoteNumber(), (juce::uint8)newVel);
            output.addEvent(m, off);
        } else {
            output.addEvent(msg, off);
        }
    }

    midi.swapWith(output);

    // Clear audio output channels 0/1 — we don't generate audio. Leave
    // channels 2+ alone so signal inputs downstream of this node aren't
    // corrupted (they were just read-only inputs to us anyway).
    for (int c = 0; c < std::min(numCh, 2); ++c)
        for (int s = 0; s < numSamples; ++s)
            buf.setSample(c, s, 0.0f);
}

// ============================================================================
// Editor — one row per rule, with target combo + amount slider + delete
// ============================================================================

class MidiModEditorComponent::RuleRow : public juce::Component {
public:
    RuleRow(MidiModEditorComponent& owner_, int index_)
        : owner(owner_), index(index_)
    {
        addAndMakeVisible(label);
        label.setFont(12.0f);

        addAndMakeVisible(targetCombo);
        targetCombo.addItem("Velocity",    1);
        targetCombo.addItem("Pitch Bend",  2);
        targetCombo.addItem("Mod Wheel",   3);
        targetCombo.addItem("Aftertouch",  4);
        targetCombo.addItem("CC#",         5);
        targetCombo.setTooltip("Which MIDI message this signal input modulates: "
                               "Velocity (note loudness), Pitch Bend (note pitch ±2 semitones), "
                               "Mod Wheel (CC#1, often vibrato depth), Aftertouch (key pressure), "
                               "or any custom CC number.");
        targetCombo.onChange = [this]() { applyToRule(); };

        addAndMakeVisible(ccSlider);
        ccSlider.setSliderStyle(juce::Slider::IncDecButtons);
        ccSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 40, 18);
        ccSlider.setRange(0, 127, 1);
        ccSlider.setTooltip("CC number (0–127) when target is set to CC#. Common values: 1=mod wheel, 7=volume, 10=pan, 11=expression, 64=sustain pedal");
        ccSlider.onValueChange = [this]() { applyToRule(); };

        addAndMakeVisible(amountSlider);
        amountSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        amountSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 18);
        amountSlider.setRange(-2.0, 2.0, 0.01);
        amountSlider.setTooltip("How strongly this signal input affects the target. 1.0 = full effect, "
                                "0.5 = half effect, -1.0 = inverted, 0 = disabled.");
        amountSlider.onValueChange = [this]() { applyToRule(); };

        addAndMakeVisible(deleteBtn);
        deleteBtn.setButtonText("X");
        deleteBtn.onClick = [this]() {
            if ((int)owner.doc.rules.size() <= 1) return; // keep at least one
            owner.doc.rules.erase(owner.doc.rules.begin() + index);
            owner.rebuildRows();
            owner.onDocChanged();
        };
    }

    void syncFromRule() {
        auto& r = owner.doc.rules[index];
        label.setText("Input " + juce::String(index + 1), juce::dontSendNotification);
        targetCombo.setSelectedId((int)r.target + 1, juce::dontSendNotification);
        ccSlider.setValue((double)r.ccNumber, juce::dontSendNotification);
        ccSlider.setVisible(r.target == ModTarget::CC);
        amountSlider.setValue((double)r.amount, juce::dontSendNotification);
    }

    void applyToRule() {
        if (index < 0 || index >= (int)owner.doc.rules.size()) return;
        auto& r = owner.doc.rules[index];
        r.target = (ModTarget)juce::jlimit(0, 4, targetCombo.getSelectedId() - 1);
        r.amount = (float)amountSlider.getValue();
        r.ccNumber = (int)ccSlider.getValue();
        ccSlider.setVisible(r.target == ModTarget::CC);
        resized();
        owner.onDocChanged();
    }

    void resized() override {
        auto a = getLocalBounds().reduced(4);
        label.setBounds(a.removeFromLeft(70));
        a.removeFromLeft(4);
        targetCombo.setBounds(a.removeFromLeft(100));
        a.removeFromLeft(4);
        if (ccSlider.isVisible()) {
            ccSlider.setBounds(a.removeFromLeft(70));
            a.removeFromLeft(4);
        }
        deleteBtn.setBounds(a.removeFromRight(22));
        a.removeFromRight(4);
        amountSlider.setBounds(a);
    }

    void paint(juce::Graphics& g) override {
        g.setColour(juce::Colour(36, 36, 46));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 3.0f);
        g.setColour(juce::Colour(70, 70, 90));
        g.drawRoundedRectangle(getLocalBounds().toFloat(), 3.0f, 1.0f);
    }

    static int rowHeight() { return 32; }

private:
    MidiModEditorComponent& owner;
    int index;
    juce::Label label;
    juce::ComboBox targetCombo;
    juce::Slider ccSlider;
    juce::Slider amountSlider;
    juce::TextButton deleteBtn;
};

MidiModEditorComponent::MidiModEditorComponent(NodeGraph& g, int nid, std::function<void()> apply)
    : graph(g), nodeId(nid), onApply(std::move(apply))
{
    auto* nd = graph.findNode(nodeId);
    if (!nd) { doc = MidiModDoc::defaultDoc(); return; }
    if (nd->script == kLegacyVelScale)
        doc = MidiModDoc::fromLegacyVelScale();
    else if (!doc.decode(nd->script))
        doc = MidiModDoc::defaultDoc();

    addAndMakeVisible(addInputBtn);
    addInputBtn.setTooltip("Add a new signal input to this MIDI Modulator. Each input adds a Signal pin "
                           "on the node that can be wired up and routed to a MIDI target.");
    addInputBtn.onClick = [this]() {
        ModRule r;
        r.target = ModTarget::Velocity;
        r.amount = 1.0f;
        doc.rules.push_back(r);
        rebuildRows();
        onDocChanged();
    };

    addAndMakeVisible(applyBtn);
    applyBtn.setTooltip("Save the current rule list to the node without closing this editor");
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
    setSize(560, 400);
}

void MidiModEditorComponent::rebuildRows() {
    rows.clear();
    rulesContainer.removeAllChildren();
    int y = 0;
    int rh = RuleRow::rowHeight();
    int vw = std::max(rulesViewport.getWidth(), 500);
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

void MidiModEditorComponent::syncNodePins() {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;
    // Preserve pin IDs where possible so existing links survive a rebuild.
    // We always have 1 MIDI in; rebuild the Signal ins to match rule count.
    int wantedSigs = (int)doc.rules.size();

    // Collect existing signal pin IDs so we can reuse them.
    std::vector<int> existingSignalIds;
    for (auto& p : nd->pinsIn)
        if (p.kind == PinKind::Signal)
            existingSignalIds.push_back(p.id);

    // Rebuild pinsIn
    std::vector<Pin> newPins;
    // Find the existing MIDI in pin (keep its ID so the incoming link stays)
    int midiInId = -1;
    for (auto& p : nd->pinsIn)
        if (p.kind == PinKind::Midi) { midiInId = p.id; break; }
    if (midiInId < 0) midiInId = graph.getNextId();
    newPins.push_back({midiInId, "MIDI In", PinKind::Midi, true});

    for (int i = 0; i < wantedSigs; ++i) {
        int id = (i < (int)existingSignalIds.size())
                 ? existingSignalIds[i] : graph.getNextId();
        newPins.push_back({id,
            "Sig " + std::to_string(i + 1),
            PinKind::Signal, true, 1});
    }
    nd->pinsIn = std::move(newPins);

    // If the signal count went down, remove any now-dangling links.
    if (wantedSigs < (int)existingSignalIds.size()) {
        std::vector<int> droppedIds(existingSignalIds.begin() + wantedSigs,
                                    existingSignalIds.end());
        graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
            [&](const Link& l) {
                for (int did : droppedIds)
                    if (l.endPin == did) return true;
                return false;
            }), graph.links.end());
    }
}

void MidiModEditorComponent::commitToNode() {
    if (auto* nd = graph.findNode(nodeId))
        nd->script = doc.encode();
    syncNodePins();
}

void MidiModEditorComponent::onDocChanged() {
    commitToNode();
    if (onApply) onApply();
}

void MidiModEditorComponent::resized() {
    auto a = getLocalBounds().reduced(8);
    auto top = a.removeFromTop(26);
    addInputBtn.setBounds(top.removeFromLeft(90));
    closeBtn.setBounds(top.removeFromRight(60));
    top.removeFromRight(4);
    applyBtn.setBounds(top.removeFromRight(60));

    a.removeFromTop(6);
    rulesViewport.setBounds(a);

    int vw = rulesViewport.getWidth();
    int rh = RuleRow::rowHeight();
    int y = 0;
    for (auto& row : rows) {
        row->setBounds(0, y, vw, rh);
        y += rh + 4;
    }
    rulesContainer.setSize(vw, std::max(y, 10));
}

void MidiModEditorComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(22, 22, 28));
    g.setColour(juce::Colours::grey);
    g.setFont(11.0f);
    g.drawText(
        "Each signal input modulates a MIDI attribute. "
        "Velocity scales note-ons (sample-accurate); the others emit "
        "continuous bend / CC / aftertouch messages per block.",
        getLocalBounds().reduced(12, 4), juce::Justification::topRight, true);
}

} // namespace SoundShop

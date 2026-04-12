#define _USE_MATH_DEFINES
#include "drum_synth.h"
#include "audio_engine.h"
#include <cmath>
#include <algorithm>

namespace SoundShop {

// ==============================================================================
// DrumSynthProcessor
// ==============================================================================

DrumSynthProcessor::DrumSynthProcessor(Node& n) : node(n) {
    // Load sounds from params if they exist, otherwise create defaults
    syncFromParams();
    if (sounds.empty()) {
        // Default kit
        sounds.push_back({"Kick",           DrumType::Kick,    36, 1.0f, 1.0f, 0.9f, 0.9f});
        sounds.push_back({"Snare",          DrumType::Snare,   38, 1.0f, 1.0f, 0.5f, 0.8f});
        sounds.push_back({"Closed Hi-Hat",  DrumType::HiHat,   42, 1.0f, 0.5f, 0.1f, 0.6f});
        sounds.push_back({"Open Hi-Hat",    DrumType::HiHat,   46, 1.0f, 1.5f, 0.1f, 0.6f});
        sounds.push_back({"Clap",           DrumType::Clap,    39, 1.0f, 1.0f, 0.2f, 0.7f});
        sounds.push_back({"Tom Low",        DrumType::Tom,     41, 0.7f, 1.0f, 0.8f, 0.7f});
        sounds.push_back({"Tom Mid",        DrumType::Tom,     47, 1.0f, 1.0f, 0.8f, 0.7f});
        sounds.push_back({"Tom High",       DrumType::Tom,     50, 1.4f, 0.8f, 0.8f, 0.7f});
        sounds.push_back({"Cowbell",        DrumType::Cowbell,  56, 1.0f, 0.6f, 0.8f, 0.5f});
        sounds.push_back({"Rimshot",        DrumType::Rimshot,  37, 1.0f, 0.5f, 0.4f, 0.7f});
        sounds.push_back({"Crash",          DrumType::Cymbal,   49, 1.0f, 1.2f, 0.0f, 0.7f}); // tone=0 = crash
        sounds.push_back({"Ride",           DrumType::Cymbal,   51, 1.2f, 0.8f, 0.5f, 0.6f}); // tone=0.5 = ride
        sounds.push_back({"Ride Bell",      DrumType::Cymbal,   53, 1.5f, 0.5f, 1.0f, 0.6f}); // tone=1 = bell
        syncToParams();
    }
}

void DrumSynthProcessor::syncFromParams() {
    // Rebuild sounds list from params. Each sound has 6 consecutive params:
    // Name Type, Pitch, Decay, Tone, Level, Note
    sounds.clear();
    int i = 0;
    while (i < (int)node.params.size()) {
        if (node.params[i].name.find(" Type") == std::string::npos) { i++; continue; }
        if (i + 5 >= (int)node.params.size()) break;
        DrumSound s;
        s.name = node.params[i].name.substr(0, node.params[i].name.find(" Type"));
        s.type = (DrumType)(int)node.params[i].value;
        s.pitch = node.params[i+1].value;
        s.decay = node.params[i+2].value;
        s.tone  = node.params[i+3].value;
        s.level = node.params[i+4].value;
        s.midiNote = (int)node.params[i+5].value;
        // Check for optional Size param (only for Cymbal type)
        if (i + 6 < (int)node.params.size() &&
            node.params[i+6].name.find(" Size") != std::string::npos) {
            s.size = node.params[i+6].value;
            sounds.push_back(s);
            i += 7;
        } else {
            sounds.push_back(s);
            i += 6;
        }
    }
}

void DrumSynthProcessor::syncToParams() {
    // Preserve Volume and Pan if they exist
    float vol = 0.5f, pan = 0.0f;
    for (auto& p : node.params) {
        if (p.name == "Volume") vol = p.value;
        if (p.name == "Pan") pan = p.value;
    }
    node.params.clear();
    node.params.push_back({"Volume", vol, 0.0f, 1.0f});
    node.params.push_back({"Pan", pan, -1.0f, 1.0f});
    for (auto& s : sounds) {
        node.params.push_back({s.name + " Type", (float)s.type, 0.0f, 7.0f});
        node.params.push_back({s.name + " Pitch", s.pitch, 0.1f, 4.0f});
        node.params.push_back({s.name + " Decay", s.decay, 0.1f, 4.0f});
        node.params.push_back({s.name + " Tone", s.tone, 0.0f, 1.0f});
        node.params.push_back({s.name + " Level", s.level, 0.0f, 1.0f});
        node.params.push_back({s.name + " Note", (float)s.midiNote, 0.0f, 127.0f});
        if (s.type == DrumType::Cymbal)
            node.params.push_back({s.name + " Size", s.size, 0.0f, 1.0f});
    }
}

int DrumSynthProcessor::findSoundForNote(int midiNote) {
    for (int i = 0; i < (int)sounds.size(); ++i)
        if (sounds[i].midiNote == midiNote) return i;
    return -1;
}

float DrumSynthProcessor::renderDrumSample(int soundIdx, DrumVoice& voice) {
    auto& s = sounds[soundIdx];
    double t = voice.time;
    float out = 0;
    std::uniform_real_distribution<float> noiseDist(-1.0f, 1.0f);

    if (s.type == DrumType::Kick || s.type == DrumType::Tom) {
        // KICK / TOM — pitch-sweeping sine
        float baseFreq = (s.type == DrumType::Kick) ? 55.0f : 80.0f;
        baseFreq *= s.pitch;
        float decayTime = 0.4f * s.decay;
        float pitchEnv = baseFreq * (1.0f + 3.0f * std::exp(-t * 30.0f)); // sweep down
        float ampEnv = std::exp((float)(-t / std::max(0.01, (double)decayTime)));
        float toneOut = std::sin(2.0f * (float)M_PI * pitchEnv * (float)t) * ampEnv;
        // Click transient
        float click = std::exp((float)(-t * 200.0f)) * noiseDist(voice.rng) * 0.3f;
        out = toneOut * s.tone + click * (1.0f - s.tone);
    }
    else if (s.type == DrumType::Snare) {
        // SNARE — sine + noise
        float toneFreq = 180.0f * s.pitch;
        float decayTime = 0.15f * s.decay;
        float toneEnv = std::exp((float)(-t / std::max(0.01, (double)decayTime * 0.7)));
        float noiseEnv = std::exp((float)(-t / std::max(0.01, (double)decayTime)));
        float toneOut = std::sin(2.0f * (float)M_PI * toneFreq * (float)t) * toneEnv;
        float noiseOut = noiseDist(voice.rng) * noiseEnv;
        out = toneOut * s.tone + noiseOut * (1.0f - s.tone);
    }
    else if (s.type == DrumType::HiHat) {
        // HI-HAT — metallic noise (decay controls closed vs open)
        float decayTime = 0.05f + 0.25f * (s.decay - 0.5f); // short decay = closed, long = open
        decayTime *= s.decay;
        float env = std::exp((float)(-t / std::max(0.005, (double)decayTime)));
        // Mix of square waves at inharmonic frequencies for metallic character
        float metallic = 0;
        float freqs[] = {317.0f, 397.0f, 524.0f, 627.0f, 738.0f, 845.0f};
        for (float f : freqs) {
            f *= s.pitch;
            metallic += (std::sin(2.0f * (float)M_PI * f * (float)t) > 0) ? 0.15f : -0.15f;
        }
        float noise = noiseDist(voice.rng) * 0.3f;
        out = (metallic * s.tone + noise * (1.0f - s.tone)) * env;
    }
    else if (s.type == DrumType::Clap) {
        // CLAP — multiple short noise bursts
        float decayTime = 0.15f * s.decay;
        float env = std::exp((float)(-t / std::max(0.01, (double)decayTime)));
        // 4 noise bursts at ~0, 10, 20, 30 ms
        float burstEnv = 0;
        for (int b = 0; b < 4; ++b) {
            float bt = (float)t - b * 0.01f;
            if (bt >= 0 && bt < 0.005f)
                burstEnv += 1.0f;
        }
        burstEnv = std::min(1.0f, burstEnv);
        out = noiseDist(voice.rng) * burstEnv * env;
    }
    else if (s.type == DrumType::Cowbell) {
        // COWBELL — two square waves at inharmonic frequencies
        float f1 = 540.0f * s.pitch, f2 = 800.0f * s.pitch;
        float decayTime = 0.08f * s.decay;
        float env = std::exp((float)(-t / std::max(0.01, (double)decayTime)));
        float sq1 = (std::sin(2.0f * (float)M_PI * f1 * (float)t) > 0) ? 0.5f : -0.5f;
        float sq2 = (std::sin(2.0f * (float)M_PI * f2 * (float)t) > 0) ? 0.5f : -0.5f;
        out = (sq1 + sq2) * env * s.tone;
    }
    else if (s.type == DrumType::Rimshot) {
        // RIMSHOT — short noise + tone
        float freq = 400.0f * s.pitch;
        float decayTime = 0.03f * s.decay;
        float env = std::exp((float)(-t / std::max(0.005, (double)decayTime)));
        float toneOut = std::sin(2.0f * (float)M_PI * freq * (float)t);
        float noiseOut = noiseDist(voice.rng);
        out = (toneOut * s.tone + noiseOut * (1.0f - s.tone)) * env;
    }
    else if (s.type == DrumType::Cymbal) {
        // CYMBAL — crash / ride / bell controlled by tone slider
        // Size sets the baseline pitch and decay (0=small 8", 0.5=medium 16", 1=large 24")
        // Pitch and Decay sliders are offsets on top of the size-derived values.
        float character = s.tone;
        float sizeVal = s.size;

        // Size → base pitch multiplier: small (2.0x) → large (0.6x)
        float sizePitch = 2.0f - sizeVal * 1.4f;
        // Size → base decay multiplier: small (0.3x) → large (1.8x)
        float sizeDecay = 0.3f + sizeVal * 1.5f;
        // Size → spectral density: larger cymbals have more partials active
        float sizeDensity = 0.5f + sizeVal * 0.5f; // 0.5 to 1.0

        // Combined with user's pitch/decay sliders (multiplicative offset)
        float effectivePitch = sizePitch * s.pitch;
        float effectiveDecay = sizeDecay * s.decay;

        // Metallic component: inharmonic square/sine waves
        float metallic = 0;
        float freqs[] = {420.0f, 563.0f, 697.0f, 838.0f, 1043.0f, 1267.0f, 1563.0f, 1896.0f};
        for (int fi = 0; fi < 8; ++fi) {
            // Larger cymbals activate more partials
            if ((float)fi / 8.0f > sizeDensity) break;
            float f = freqs[fi];
            f *= effectivePitch;
            // Bell uses more sine (smoother), crash uses more square (harsher)
            if (character > 0.7f)
                metallic += std::sin(2.0f * (float)M_PI * f * (float)t) * 0.12f;
            else
                metallic += ((std::sin(2.0f * (float)M_PI * f * (float)t) > 0) ? 0.12f : -0.12f);
        }

        // Noise component
        float noise = noiseDist(voice.rng) * 0.4f;

        // Mix: crash = mostly noise, bell = mostly metallic
        float metallicAmt = 0.2f + character * 0.7f;  // 0.2 (crash) to 0.9 (bell)
        float noiseAmt = 1.0f - metallicAmt;

        // Decay: crash = long (1-3s), ride = medium (0.5-1.5s), bell = short-medium (0.3-0.8s)
        float baseDecay = 2.0f - character * 1.5f; // 2s (crash) to 0.5s (bell)
        baseDecay *= effectiveDecay;
        float env = std::exp((float)(-t / std::max(0.01, (double)baseDecay)));

        // Bell gets a sharper attack transient
        float attack = 1.0f;
        if (character > 0.7f)
            attack = 1.0f + 2.0f * std::exp((float)(-t * 100.0f));

        out = (metallic * metallicAmt + noise * noiseAmt) * env * attack;
    }

    return out * s.level * voice.velocity;
}

void DrumSynthProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) {
    buf.clear();
    syncFromParams(); // pick up any param changes from sliders

    float volume = 0.5f;
    float velSens = 1.0f;
    for (auto& p : node.params) {
        if (p.name == "Volume")    volume = p.value;
        else if (p.name == "Vel Sens") velSens = p.value;
    }

    // Process MIDI
    for (auto metadata : midi) {
        auto msg = metadata.getMessage();
        if (msg.isNoteOn()) {
            int si = findSoundForNote(msg.getNoteNumber());
            if (si >= 0) {
                // Find a free voice
                int vi = -1;
                for (int i = 0; i < MAX_DRUM_VOICES; ++i) {
                    if (!voices[i].active) { vi = i; break; }
                }
                if (vi < 0) vi = 0; // steal first voice
                voices[vi].active = true;
                voices[vi].soundIdx = si;
                voices[vi].time = 0;
                {
                    float raw = msg.getVelocity() / 127.0f;
                    voices[vi].velocity = 1.0f - velSens * (1.0f - raw);
                }
                voices[vi].rng.seed((unsigned)msg.getNoteNumber() * 1234 + (unsigned)(voices[vi].time * 10000));
            }
        }
    }

    // Render voices
    int nc = buf.getNumChannels();
    int ns = buf.getNumSamples();
    for (int s = 0; s < ns; ++s) {
        float total = 0;
        for (int vi = 0; vi < MAX_DRUM_VOICES; ++vi) {
            if (!voices[vi].active) continue;
            float sample = renderDrumSample(voices[vi].soundIdx, voices[vi]);
            total += sample;
            voices[vi].time += 1.0 / sampleRate;
            // Auto-deactivate after silence
            if (voices[vi].time > 5.0) voices[vi].active = false;
        }
        total *= volume;
        total = juce::jlimit(-1.0f, 1.0f, total);
        for (int c = 0; c < nc; ++c)
            buf.addSample(c, s, total);
    }
}

// ==============================================================================
// Editor UI
// ==============================================================================

DrumSynthEditorComponent::DrumSynthEditorComponent(NodeGraph& g, int nid, AudioEngine* ae)
    : graph(g), nodeId(nid), audioEngine(ae)
{
    addAndMakeVisible(addSoundBtn);
    addSoundBtn.setTooltip("Add a new drum voice (kick, snare, hi-hat, etc.) — pick the type from the popup. "
                           "Each voice gets its own MIDI note assignment and a row of params (pitch, decay, tone, level).");
    addSoundBtn.onClick = [this]() {
        juce::PopupMenu menu;
        menu.addItem(1, "Kick");
        menu.addItem(2, "Snare");
        menu.addItem(3, "Hi-Hat");
        menu.addItem(4, "Clap");
        menu.addItem(5, "Tom");
        menu.addItem(6, "Cowbell");
        menu.addItem(7, "Rimshot");
        menu.addItem(8, "Cymbal (crash/ride/bell)");
        menu.showMenuAsync({}, [this](int r) {
            if (r <= 0) return;
            const char* names[] = {"", "Kick", "Snare", "Hi-Hat", "Clap", "Tom", "Cowbell", "Rimshot", "Cymbal"};
            DrumType types[] = {DrumType::Kick, DrumType::Kick, DrumType::Snare, DrumType::HiHat,
                                DrumType::Clap, DrumType::Tom, DrumType::Cowbell, DrumType::Rimshot, DrumType::Cymbal};
            // Find next free MIDI note
            int note = 36;
            auto* nd = graph.findNode(nodeId);
            if (nd) {
                std::set<int> used;
                for (auto& p : nd->params)
                    if (p.name.find(" Note") != std::string::npos)
                        used.insert((int)p.value);
                while (used.count(note)) note++;
            }
            addSound(types[r], std::string(names[r]) + " " + std::to_string(note), note);
        });
    };

    addAndMakeVisible(scrollView);
    scrollView.setViewedComponent(&scrollContent, false);
    scrollView.setScrollBarsShown(true, false);

    addAndMakeVisible(closeBtn);
    closeBtn.onClick = [this]() {
        // Save params
        if (auto* nd = graph.findNode(nodeId)) {
            // Params are already synced via slider callbacks
            graph.dirty = true;
        }
        learnIdx = -1;
        if (audioEngine) audioEngine->hotkeyMidiCapture = nullptr;
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
            juce::Component::SafePointer<juce::DialogWindow> safe(dw);
            juce::MessageManager::callAsync([safe]() {
                if (safe) delete safe.getComponent();
            });
        }
    };

    rebuildRows();
    startTimerHz(15);
    setSize(800, 500);
}

void DrumSynthEditorComponent::addSound(DrumType type, const std::string& name, int note) {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;
    // Add 6 params for the new sound
    nd->params.push_back({name + " Type", (float)type, 0.0f, 7.0f});
    nd->params.push_back({name + " Pitch", 1.0f, 0.1f, 4.0f});
    nd->params.push_back({name + " Decay", 1.0f, 0.1f, 4.0f});
    nd->params.push_back({name + " Tone", 0.5f, 0.0f, 1.0f});
    nd->params.push_back({name + " Level", 0.8f, 0.0f, 1.0f});
    nd->params.push_back({name + " Note", (float)note, 0.0f, 127.0f});
    graph.dirty = true;
    rebuildRows();
    resized();
    repaint();
}

void DrumSynthEditorComponent::deleteSound(int index) {
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;
    // Find the Nth "Type" param and remove 6 consecutive params
    int count = 0;
    for (int i = 0; i < (int)nd->params.size(); ++i) {
        if (nd->params[i].name.find(" Type") != std::string::npos) {
            if (count == index) {
                int end = std::min(i + 6, (int)nd->params.size());
                nd->params.erase(nd->params.begin() + i, nd->params.begin() + end);
                break;
            }
            count++;
        }
    }
    graph.dirty = true;
    rebuildRows();
    resized();
    repaint();
}

void DrumSynthEditorComponent::rebuildRows() {
    rows.clear();
    scrollContent.removeAllChildren();
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;

    // Find all sounds by scanning for " Type" params
    int soundCount = 0;
    for (auto& p : nd->params)
        if (p.name.find(" Type") != std::string::npos) soundCount++;

    int y = 0, rowH = 34;
    int soundIdx = 0;
    for (int pi = 0; pi < (int)nd->params.size(); ++pi) {
        if (nd->params[pi].name.find(" Type") == std::string::npos) continue;
        if (pi + 5 >= (int)nd->params.size()) break;

        auto row = std::make_unique<SoundRow>();
        std::string name = nd->params[pi].name.substr(0, nd->params[pi].name.find(" Type"));

        row->nameLabel.setText(name, juce::dontSendNotification);
        row->nameLabel.setFont(juce::Font(11.0f, juce::Font::bold));
        scrollContent.addAndMakeVisible(row->nameLabel);

        // Type combo
        scrollContent.addAndMakeVisible(row->typeCombo);
        row->typeCombo.addItem("Kick", 1); row->typeCombo.addItem("Snare", 2);
        row->typeCombo.addItem("Hi-Hat", 3); row->typeCombo.addItem("Clap", 4);
        row->typeCombo.addItem("Tom", 5); row->typeCombo.addItem("Cowbell", 6);
        row->typeCombo.addItem("Rimshot", 7); row->typeCombo.addItem("Cymbal", 8);
        row->typeCombo.setSelectedId((int)nd->params[pi].value + 1, juce::dontSendNotification);
        row->typeCombo.setTooltip("Drum sound algorithm. Each type uses different synthesis (e.g. Kick is a "
                                  "pitched sine sweep, Snare is filtered noise, Hi-Hat is FM noise).");
        int typePI = pi;
        row->typeCombo.onChange = [this, typePI]() {
            if (auto* n = graph.findNode(nodeId))
                if (typePI < (int)n->params.size())
                    n->params[typePI].value = (float)(rows[0]->typeCombo.getSelectedId() - 1); // TODO: fix index
        };

        // Note display
        int note = (int)nd->params[pi+5].value;
        row->noteLabel.setText("N:" + juce::String(note), juce::dontSendNotification);
        row->noteLabel.setFont(10.0f);
        scrollContent.addAndMakeVisible(row->noteLabel);

        // Learn button
        scrollContent.addAndMakeVisible(row->learnBtn);
        row->learnBtn.setTooltip("MIDI Learn: click this button, then press a key on your MIDI controller to "
                                 "assign that note to this drum voice.");
        int si = soundIdx;
        row->learnBtn.onClick = [this, si]() {
            learnIdx = si;
            if (audioEngine) {
                audioEngine->hotkeyMidiCapture = [this](int type, int channel, int number) {
                    if (type == 0)
                        juce::MessageManager::callAsync([this, number]() { onMidiNote(number); });
                };
            }
        };

        // Delete button
        scrollContent.addAndMakeVisible(row->deleteBtn);
        row->deleteBtn.onClick = [this, si]() { deleteSound(si); };

        // Sliders
        auto setupSlider = [&](juce::Slider& s, float val, float lo, float hi) {
            scrollContent.addAndMakeVisible(s);
            s.setRange(lo, hi, 0.01);
            s.setValue(val, juce::dontSendNotification);
            s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 36, 16);
            s.setSliderStyle(juce::Slider::LinearHorizontal);
        };
        setupSlider(row->pitchSlider, nd->params[pi+1].value, 0.1f, 4.0f);
        setupSlider(row->decaySlider, nd->params[pi+2].value, 0.1f, 4.0f);
        setupSlider(row->toneSlider,  nd->params[pi+3].value, 0.0f, 1.0f);
        setupSlider(row->levelSlider, nd->params[pi+4].value, 0.0f, 1.0f);
        row->pitchSlider.setTooltip("Pitch multiplier — higher = brighter and 'tighter', lower = deeper and 'fatter'");
        row->decaySlider.setTooltip("How quickly the sound fades after being struck. Lower = short and percussive, higher = ringing tail");
        row->toneSlider.setTooltip("Timbre shaping — varies per drum type (filter cutoff, harmonic balance, noise mix, etc.)");
        row->levelSlider.setTooltip("Output volume of this drum voice");

        // Check if this is a cymbal (has Size param)
        DrumType dtype = (DrumType)(int)nd->params[pi].value;
        row->isCymbal = (dtype == DrumType::Cymbal);

        int base = pi;
        auto updateP = [this, base](int off, float v) {
            if (auto* n = graph.findNode(nodeId))
                if (base+off < (int)n->params.size()) n->params[base+off].value = v;
        };
        row->pitchSlider.onValueChange = [r=row.get(), updateP]() { updateP(1, (float)r->pitchSlider.getValue()); };
        row->decaySlider.onValueChange = [r=row.get(), updateP]() { updateP(2, (float)r->decaySlider.getValue()); };
        row->toneSlider.onValueChange  = [r=row.get(), updateP]() { updateP(3, (float)r->toneSlider.getValue()); };
        row->levelSlider.onValueChange = [r=row.get(), updateP]() { updateP(4, (float)r->levelSlider.getValue()); };

        // Size slider (cymbal only)
        if (row->isCymbal) {
            // Find the Size param index
            int sizePI = pi + 6; // Size is the 7th param for cymbals
            if (sizePI < (int)nd->params.size() &&
                nd->params[sizePI].name.find(" Size") != std::string::npos) {
                setupSlider(row->sizeSlider, nd->params[sizePI].value, 0.0f, 1.0f);
                row->sizeSlider.onValueChange = [r=row.get(), this, sizePI]() {
                    if (auto* n = graph.findNode(nodeId))
                        if (sizePI < (int)n->params.size())
                            n->params[sizePI].value = (float)r->sizeSlider.getValue();
                };
                row->sizeLbl.setText("Size:", juce::dontSendNotification);
                row->sizeLbl.setFont(9.0f);
                scrollContent.addAndMakeVisible(row->sizeLbl);
            }
        }

        // Layout the row
        int x = 0, w = std::max(scrollView.getWidth(), 780);
        row->nameLabel.setBounds(x, y, 80, rowH); x += 82;
        row->typeCombo.setBounds(x, y+2, 70, rowH-4); x += 72;
        row->noteLabel.setBounds(x, y, 40, rowH); x += 42;
        row->learnBtn.setBounds(x, y+2, 45, rowH-4); x += 47;
        row->deleteBtn.setBounds(x, y+2, 22, rowH-4); x += 24;
        row->pitchSlider.setBounds(x, y+2, 80, rowH-4); x += 82;
        row->decaySlider.setBounds(x, y+2, 80, rowH-4); x += 82;
        row->toneSlider.setBounds(x, y+2, 80, rowH-4); x += 82;
        row->levelSlider.setBounds(x, y+2, 80, rowH-4); x += 82;
        if (row->isCymbal) {
            row->sizeLbl.setBounds(x, y, 30, rowH); x += 32;
            row->sizeSlider.setBounds(x, y+2, 80, rowH-4); x += 82;
        }

        rows.push_back(std::move(row));
        y += rowH + 2;
        soundIdx++;
    }
    scrollContent.setSize(std::max(scrollView.getWidth(), 680), std::max(y, 10));
}

void DrumSynthEditorComponent::onMidiNote(int note) {
    if (learnIdx < 0) return;
    auto* nd = graph.findNode(nodeId);
    if (!nd) return;

    // Find the Note param for this sound
    int count = 0;
    for (auto& p : nd->params) {
        if (p.name.find(" Note") != std::string::npos) {
            if (count == learnIdx) {
                p.value = (float)note;
                break;
            }
            count++;
        }
    }

    // Update the display
    if (learnIdx < (int)rows.size())
        rows[learnIdx]->noteLabel.setText("Note: " + juce::String(note), juce::dontSendNotification);

    learnIdx = -1;
    if (audioEngine) audioEngine->hotkeyMidiCapture = nullptr;
    graph.dirty = true;
    repaint();
}

void DrumSynthEditorComponent::resized() {
    auto a = getLocalBounds().reduced(8);
    auto top = a.removeFromTop(24);
    addSoundBtn.setBounds(top.removeFromLeft(110).reduced(0, 2));
    closeBtn.setBounds(top.removeFromRight(60).reduced(0, 2));
    a.removeFromTop(4);
    scrollView.setBounds(a);
    rebuildRows(); // re-layout rows to match new viewport width
}

void DrumSynthEditorComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(22, 22, 28));

    // Column headers
    auto a = getLocalBounds().reduced(8);
    a.removeFromTop(28);
    auto header = a.removeFromTop(16);
    g.setColour(juce::Colours::grey);
    g.setFont(9.0f);
    header.removeFromLeft(100); // name
    header.removeFromLeft(60);  // note
    header.removeFromLeft(50);  // learn
    g.drawText("Pitch", header.removeFromLeft(82), juce::Justification::centred);
    g.drawText("Decay", header.removeFromLeft(82), juce::Justification::centred);
    g.drawText("Tone", header.removeFromLeft(82), juce::Justification::centred);
    g.drawText("Level", header.removeFromLeft(82), juce::Justification::centred);
    g.drawText("Size", header.removeFromLeft(112), juce::Justification::centred);

    // Learn mode indicator
    if (learnIdx >= 0) {
        g.setColour(juce::Colours::yellow);
        g.setFont(11.0f);
        g.drawText("Press a MIDI pad to assign it to the selected drum sound...",
                   getLocalBounds().reduced(8).removeFromBottom(20),
                   juce::Justification::centredLeft);
    }
}

} // namespace SoundShop

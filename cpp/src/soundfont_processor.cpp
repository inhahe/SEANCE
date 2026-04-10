#define TSF_IMPLEMENTATION
#include "../third_party/tinysoundfont/tsf.h"

#include "soundfont_processor.h"
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>

namespace SoundShop {

// ==============================================================================
// SFZ Parser
// ==============================================================================

bool SFZInstrument::loadFromFile(const std::string& path) {
    regions.clear();
    auto file = juce::File(path);
    if (!file.existsAsFile()) return false;
    basePath = file.getParentDirectory().getFullPathName().toStdString();

    auto content = file.loadFileAsString().toStdString();
    std::istringstream stream(content);
    std::string line;

    SFZRegion currentRegion;
    SFZRegion groupDefaults; // <group> defaults applied to subsequent <region>s
    bool inRegion = false;
    bool inGroup = false;

    auto trim = [](std::string s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        return s;
    };

    auto parseOpcode = [&](const std::string& key, const std::string& val, SFZRegion& r) {
        if (key == "sample") r.samplePath = val;
        else if (key == "lokey") r.lokey = std::atoi(val.c_str());
        else if (key == "hikey") r.hikey = std::atoi(val.c_str());
        else if (key == "key") { r.lokey = r.hikey = r.pitchKeycenter = std::atoi(val.c_str()); }
        else if (key == "lovel") r.lovel = std::atoi(val.c_str());
        else if (key == "hivel") r.hivel = std::atoi(val.c_str());
        else if (key == "pitch_keycenter") r.pitchKeycenter = std::atoi(val.c_str());
        else if (key == "volume") r.volume = std::strtof(val.c_str(), nullptr);
        else if (key == "tune") r.tune = std::strtof(val.c_str(), nullptr);
        else if (key == "ampeg_attack") r.ampegAttack = std::strtof(val.c_str(), nullptr);
        else if (key == "ampeg_decay") r.ampegDecay = std::strtof(val.c_str(), nullptr);
        else if (key == "ampeg_sustain") r.ampegSustain = std::strtof(val.c_str(), nullptr);
        else if (key == "ampeg_release") r.ampegRelease = std::strtof(val.c_str(), nullptr);
        else if (key == "offset") r.offset = std::atoi(val.c_str());
        else if (key == "loop_start") r.loopStart = std::atoi(val.c_str());
        else if (key == "loop_end") r.loopEnd = std::atoi(val.c_str());
        else if (key == "loop_mode") {
            if (val == "loop_continuous") r.loopMode = 1;
            else if (val == "loop_sustain") r.loopMode = 2;
            else r.loopMode = 0;
        }
        else {
            // Unknown opcode — skip gracefully but log for debugging
            juce::Logger::writeToLog("SFZ: skipping unknown opcode: " + juce::String(key) + "=" + juce::String(val));
        }
    };

    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '/') continue; // skip comments

        // Check for headers
        if (line.find("<group>") != std::string::npos) {
            if (inRegion) { regions.push_back(currentRegion); }
            inRegion = false;
            inGroup = true;
            groupDefaults = SFZRegion{};
            continue;
        }
        if (line.find("<region>") != std::string::npos) {
            if (inRegion) { regions.push_back(currentRegion); }
            currentRegion = groupDefaults; // start from group defaults
            inRegion = true;
            // Parse opcodes on the same line after <region>
            auto regionTag = line.find("<region>");
            line = line.substr(regionTag + 8);
        }

        // Parse opcodes (key=value pairs separated by spaces)
        std::istringstream opcodeStream(line);
        std::string token;
        while (opcodeStream >> token) {
            auto eq = token.find('=');
            if (eq == std::string::npos) continue;
            std::string key = token.substr(0, eq);
            std::string val = token.substr(eq + 1);
            if (inRegion)
                parseOpcode(key, val, currentRegion);
            else if (inGroup)
                parseOpcode(key, val, groupDefaults);
        }
    }
    if (inRegion) regions.push_back(currentRegion);

    // Load sample files for each region
    juce::AudioFormatManager mgr;
    mgr.registerBasicFormats();
    for (auto& r : regions) {
        if (r.samplePath.empty()) continue;
        // Resolve path relative to the .sfz file
        std::string fullPath = basePath + "/" + r.samplePath;
        // Replace backslashes (SFZ uses forward slashes but some files use back)
        std::replace(fullPath.begin(), fullPath.end(), '\\', '/');

        auto sampleFile = juce::File(fullPath);
        if (!sampleFile.existsAsFile()) continue;

        std::unique_ptr<juce::AudioFormatReader> reader(mgr.createReaderFor(sampleFile));
        if (!reader) continue;

        int len = (int)reader->lengthInSamples;
        juce::AudioBuffer<float> buf(1, len);
        reader->read(&buf, 0, len, 0, true, false);
        r.samples.resize(len);
        for (int i = 0; i < len; ++i) r.samples[i] = buf.getSample(0, i);
        r.sampleRate = reader->sampleRate;
        r.numSamples = len;
    }

    return !regions.empty();
}

std::vector<const SFZRegion*> SFZInstrument::findRegions(int midiNote, int velocity) const {
    std::vector<const SFZRegion*> matches;
    for (auto& r : regions) {
        if (midiNote >= r.lokey && midiNote <= r.hikey &&
            velocity >= r.lovel && velocity <= r.hivel &&
            !r.samples.empty()) {
            matches.push_back(&r);
        }
    }
    return matches;
}

// ==============================================================================
// SoundFontProcessor
// ==============================================================================

SoundFontProcessor::SoundFontProcessor(Node& n) : node(n) {
    loadFile();
}

SoundFontProcessor::~SoundFontProcessor() {
    if (sf2) tsf_close(sf2);
}

void SoundFontProcessor::loadFile() {
    auto& script = node.script;

    if (script.rfind("__sf2__:", 0) == 0) {
        std::string path = script.substr(7);
        if (sf2) { tsf_close(sf2); sf2 = nullptr; }
        sf2 = tsf_load_filename(path.c_str());
        if (sf2) {
            tsf_set_output(sf2, TSF_STEREO_INTERLEAVED, (int)sampleRate, 0.0f);
            // Read preset from param
            for (auto& p : node.params)
                if (p.name == "Preset") { currentPreset = (int)p.value; break; }
            tsf_set_max_voices(sf2, 64);
        }
    } else if (script.rfind("__sfz__:", 0) == 0) {
        std::string path = script.substr(7);
        sfz.loadFromFile(path);
    }
}

void SoundFontProcessor::prepareToPlay(double sr, int bs) {
    sampleRate = sr;
    if (sf2) tsf_set_output(sf2, TSF_STEREO_INTERLEAVED, (int)sr, 0.0f);
}

void SoundFontProcessor::releaseResources() {
    // Turn off all notes
    if (sf2) tsf_note_off_all(sf2);
    for (auto& v : sfzVoices) v.active = false;
}

std::vector<std::string> SoundFontProcessor::getPresetNames() const {
    std::vector<std::string> names;
    if (sf2) {
        int count = tsf_get_presetcount(sf2);
        for (int i = 0; i < count; ++i) {
            const char* name = tsf_get_presetname(sf2, i);
            names.push_back(name ? name : ("Preset " + std::to_string(i)));
        }
    }
    return names;
}

void SoundFontProcessor::setPreset(int idx) {
    currentPreset = idx;
    for (auto& p : node.params)
        if (p.name == "Preset") { p.value = (float)idx; return; }
    node.params.push_back({"Preset", (float)idx, 0, 127});
}

void SoundFontProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) {
    int numSamples = buf.getNumSamples();
    int numChannels = buf.getNumChannels();
    buf.clear();

    // Velocity sensitivity — read from node param (default 1 = linear).
    float velSensSF2 = 1.0f;
    for (auto& p : node.params)
        if (p.name == "Vel Sens") { velSensSF2 = p.value; break; }

    // === SF2 path: use tinysoundfont ===
    if (sf2) {
        // Process MIDI events. Use the channel-aware tsf API so that
        // pitch bend / CC messages routed per-channel work (MPE etc).
        for (auto metadata : midi) {
            auto msg = metadata.getMessage();
            int ch = juce::jlimit(1, 16, msg.getChannel()) - 1; // tsf uses 0-based
            if (msg.isNoteOn()) {
                float raw = msg.getVelocity() / 127.0f;
                float eff = 1.0f - velSensSF2 * (1.0f - raw);
                tsf_channel_note_on(sf2, ch, msg.getNoteNumber(), eff);
            }
            else if (msg.isNoteOff())
                tsf_channel_note_off(sf2, ch, msg.getNoteNumber());
            else if (msg.isAllNotesOff())
                tsf_note_off_all(sf2);
            else if (msg.isPitchWheel())
                tsf_channel_set_pitchwheel(sf2, ch, msg.getPitchWheelValue());
            else if (msg.isController())
                tsf_channel_midi_control(sf2, ch,
                    msg.getControllerNumber(), msg.getControllerValue());
        }

        // Render audio (TSF outputs interleaved stereo)
        std::vector<float> interleaved(numSamples * 2, 0.0f);
        tsf_render_float(sf2, interleaved.data(), numSamples, 0);

        // Deinterleave to JUCE buffer
        for (int s = 0; s < numSamples; ++s) {
            if (numChannels > 0) buf.addSample(0, s, interleaved[s * 2]);
            if (numChannels > 1) buf.addSample(1, s, interleaved[s * 2 + 1]);
        }
        return;
    }

    // === SFZ path: custom multi-sample playback ===
    if (sfz.regions.empty()) return;

    float volume = 0.5f;
    for (auto& p : node.params)
        if (p.name == "Volume") { volume = p.value; break; }

    // Process MIDI
    for (auto metadata : midi) {
        auto msg = metadata.getMessage();
        if (msg.isNoteOn()) {
            int note = msg.getNoteNumber();
            int vel = msg.getVelocity();
            auto matches = sfz.findRegions(note, vel);
            for (auto* region : matches) {
                // Find a free voice
                int vi = -1;
                float minLev = 999;
                for (int i = 0; i < MAX_SFZ_VOICES; ++i) {
                    if (!sfzVoices[i].active) { vi = i; break; }
                    if (sfzVoices[i].envLevel < minLev) { minLev = sfzVoices[i].envLevel; vi = i; }
                }
                if (vi >= 0) {
                    auto& v = sfzVoices[vi];
                    v.active = true;
                    v.midiNote = note;
                    v.region = region;
                    v.phase = region->offset;
                    v.envStage = SFZVoice::Attack;
                    v.envLevel = 0;
                    v.envTime = 0;
                    float raw = vel / 127.0f;
                    v.velocity = 1.0f - velSensSF2 * (1.0f - raw);
                }
            }
        } else if (msg.isNoteOff()) {
            int note = msg.getNoteNumber();
            for (auto& v : sfzVoices) {
                if (v.active && v.midiNote == note && v.envStage != SFZVoice::Release) {
                    v.envStage = SFZVoice::Release;
                    v.envTime = 0;
                }
            }
        }
    }

    // Render SFZ voices
    for (int s = 0; s < numSamples; ++s) {
        float totalL = 0, totalR = 0;
        for (auto& v : sfzVoices) {
            if (!v.active || !v.region) continue;
            auto* r = v.region;

            // ADSR envelope
            float dt = 1.0f / (float)sampleRate;
            v.envTime += dt;
            switch (v.envStage) {
                case SFZVoice::Attack:
                    v.envLevel += dt / std::max(0.001f, r->ampegAttack);
                    if (v.envLevel >= 1.0f) { v.envLevel = 1.0f; v.envStage = SFZVoice::Decay; v.envTime = 0; }
                    break;
                case SFZVoice::Decay: {
                    float sustain = r->ampegSustain / 100.0f;
                    v.envLevel -= dt / std::max(0.001f, r->ampegDecay) * (1.0f - sustain);
                    if (v.envLevel <= sustain) { v.envLevel = sustain; v.envStage = SFZVoice::Sustain; }
                    break;
                }
                case SFZVoice::Sustain:
                    v.envLevel = r->ampegSustain / 100.0f;
                    break;
                case SFZVoice::Release:
                    v.envLevel -= dt / std::max(0.001f, r->ampegRelease);
                    if (v.envLevel <= 0) { v.envLevel = 0; v.active = false; }
                    break;
                default: break;
            }
            if (!v.active) continue;

            // Sample playback with pitch transposition
            float noteFreq = 440.0f * std::pow(2.0f, (v.midiNote - 69) / 12.0f);
            float baseFreq = 440.0f * std::pow(2.0f, (r->pitchKeycenter - 69 + r->tune / 100.0f) / 12.0f);
            float pitchRatio = noteFreq / baseFreq;

            int idx = (int)v.phase;
            float frac = (float)(v.phase - idx);
            if (idx >= 0 && idx < r->numSamples - 1) {
                float sample = r->samples[idx] + frac * (r->samples[idx + 1] - r->samples[idx]);
                float gain = v.envLevel * v.velocity * std::pow(10.0f, r->volume / 20.0f);
                totalL += sample * gain;
                totalR += sample * gain;
            } else if (r->loopMode == 1 && r->loopEnd > r->loopStart) {
                // Loop
                int loopLen = r->loopEnd - r->loopStart;
                int loopIdx = r->loopStart + ((idx - r->loopStart) % loopLen);
                if (loopIdx >= 0 && loopIdx < r->numSamples) {
                    float sample = r->samples[loopIdx];
                    float gain = v.envLevel * v.velocity * std::pow(10.0f, r->volume / 20.0f);
                    totalL += sample * gain;
                    totalR += sample * gain;
                }
            } else {
                v.active = false;
                continue;
            }

            v.phase += pitchRatio * (r->sampleRate / sampleRate);
        }

        totalL *= volume;
        totalR *= volume;
        if (numChannels > 0) buf.addSample(0, s, juce::jlimit(-1.0f, 1.0f, totalL));
        if (numChannels > 1) buf.addSample(1, s, juce::jlimit(-1.0f, 1.0f, totalR));
    }
}

} // namespace SoundShop

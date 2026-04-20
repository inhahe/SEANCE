#include "mod_import.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <cmath>

// Use libopenmpt if available
#ifdef HAS_LIBOPENMPT
#include "libopenmpt/libopenmpt.h"
#else
#define HAS_LIBOPENMPT 0
#endif

namespace SoundShop {

bool ModImporter::isSupported(const std::string& path) {
    auto ext = path.substr(path.find_last_of('.') + 1);
    for (auto& c : ext) c = (char)std::tolower(c);
    return ext == "mod" || ext == "s3m" || ext == "it" || ext == "xm";
}

ModImporter::ImportResult ModImporter::import(const std::string& path, NodeGraph& graph,
                                                float posX, float posY) {
    ImportResult result;

#if HAS_LIBOPENMPT
    // Load the file
    std::ifstream file(path, std::ios::binary);
    if (!file) { result.error = "Cannot open file"; return result; }
    std::vector<char> data((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

    // Create libopenmpt module
    openmpt_module* mod = openmpt_module_create_from_memory2(
        data.data(), data.size(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!mod) { result.error = "Failed to parse module"; return result; }

    int numChannels = openmpt_module_get_num_channels(mod);
    int numPatterns = openmpt_module_get_num_patterns(mod);
    int numSamples = openmpt_module_get_num_samples(mod);
    int numOrders = openmpt_module_get_num_orders(mod);
    int initialTempo = openmpt_module_get_current_tempo(mod);
    int initialSpeed = openmpt_module_get_current_speed(mod);

    result.numChannels = numChannels;
    result.numPatterns = numPatterns;
    result.numSamples = numSamples;

    // Set project BPM from module tempo
    // MOD tempo: BPM = 2 * tempo / 5 (when speed = 6)
    float bpm = std::max(60.0f, (float)initialTempo);
    graph.bpm = bpm;

    // Ticks per row (speed), rows per beat conversion
    float ticksPerRow = (float)std::max(1, initialSpeed);
    float rowsPerBeat = 4.0f; // standard: 4 rows = 1 beat at normal speed
    float beatsPerRow = 1.0f / rowsPerBeat;

    // Create a group for the module
    auto& group = graph.createGroup(
        path.substr(path.find_last_of("/\\") + 1), {posX, posY});
    int groupIdx = (int)graph.nodes.size() - 1;

    // Create a MIDI track for each channel
    std::vector<int> channelNodeIndices;
    for (int ch = 0; ch < numChannels; ++ch) {
        auto chName = "Ch " + std::to_string(ch + 1);
        const char* cname = openmpt_module_get_channel_name(mod, ch);
        if (cname && cname[0]) chName = cname;

        float ny = posY + 60 + ch * 40;
        auto& n = graph.addNode(chName, NodeType::MidiTimeline,
            {Pin{0, "MIDI In", PinKind::Midi, true}},
            {Pin{0, "MIDI", PinKind::Midi, false}}, {posX + 30, ny});
        n.clips.push_back({"Pattern", 0, 4, 0xFF6688CC});
        graph.addToGroup(group.id, n.id);
        channelNodeIndices.push_back((int)graph.nodes.size() - 1);
    }

    // Walk through the order list and extract notes/effects
    float currentBeat = 0;
    float currentSpeed = ticksPerRow;
    float currentTempo = bpm;

    for (int order = 0; order < numOrders; ++order) {
        int pattern = openmpt_module_get_order_pattern(mod, order);
        if (pattern < 0 || pattern >= numPatterns) continue;
        int numRows = openmpt_module_get_pattern_num_rows(mod, pattern);

        for (int row = 0; row < numRows; ++row) {
            float rowBeat = currentBeat + row * beatsPerRow;

            for (int ch = 0; ch < numChannels; ++ch) {
                // Command indices: 0=note, 1=instrument, 2=volumecolumn, 3=effect, 4=effectparam
                uint8_t note = openmpt_module_get_pattern_row_channel_command(mod, pattern, row, ch, 0);
                uint8_t inst = openmpt_module_get_pattern_row_channel_command(mod, pattern, row, ch, 1);
                uint8_t volcmd = openmpt_module_get_pattern_row_channel_command(mod, pattern, row, ch, 2);
                uint8_t effect = openmpt_module_get_pattern_row_channel_command(mod, pattern, row, ch, 3);
                uint8_t param = openmpt_module_get_pattern_row_channel_command(mod, pattern, row, ch, 4);

                int nodeIdx = channelNodeIndices[ch];
                auto& node = graph.nodes[nodeIdx];
                if (node.clips.empty()) continue;
                auto& clip = node.clips[0];

                // Note: 1-120 = C-1 to B-9, 255 = note off, 254 = note cut
                if (note >= 1 && note <= 120) {
                    int midiPitch = note - 1; // MOD note 1 = C-0 = MIDI 0
                    MidiNote nn;
                    nn.offset = rowBeat - clip.startBeat;
                    nn.pitch = std::min(127, midiPitch);
                    nn.duration = beatsPerRow; // default 1 row; extended by next note-off
                    nn.velocity = 100;

                    // Volume column as velocity
                    if (volcmd > 0 && volcmd <= 64)
                        nn.velocity = std::min(127, (int)(volcmd * 2));

                    clip.notes.push_back(nn);
                    result.numNotes++;

                    // Extend clip if needed
                    float noteEnd = nn.offset + nn.duration;
                    if (noteEnd > clip.lengthBeats)
                        clip.lengthBeats = std::ceil(noteEnd / 4.0f) * 4.0f;
                }
                else if (note == 255 || note == 254) {
                    // Note off/cut: find the last note on this channel and trim its duration
                    if (!clip.notes.empty()) {
                        auto& lastNote = clip.notes.back();
                        float offBeat = rowBeat - clip.startBeat;
                        if (offBeat > lastNote.offset)
                            lastNote.duration = offBeat - lastNote.offset;
                    }
                }

                // Convert effects to automation/expression/notes
                if (effect > 0) {
                    switch (effect) {
                        case 0x00: // Arpeggio (if param != 0)
                        {
                            if (param != 0 && !clip.notes.empty()) {
                                auto& base = clip.notes.back();
                                int semi1 = param >> 4;
                                int semi2 = param & 0x0F;
                                // Expand to 3 rapid notes per row (base, +semi1, +semi2)
                                float third = beatsPerRow / 3.0f;
                                if (semi1 > 0) {
                                    MidiNote n1;
                                    n1.offset = base.offset + third;
                                    n1.pitch = std::min(127, base.pitch + semi1);
                                    n1.duration = third;
                                    n1.velocity = base.velocity;
                                    clip.notes.push_back(n1);
                                }
                                if (semi2 > 0) {
                                    MidiNote n2;
                                    n2.offset = base.offset + third * 2;
                                    n2.pitch = std::min(127, base.pitch + semi2);
                                    n2.duration = third;
                                    n2.velocity = base.velocity;
                                    clip.notes.push_back(n2);
                                }
                                base.duration = third; // shorten original
                            }
                            break;
                        }
                        case 0x01: // Portamento up
                        case 0x02: // Portamento down
                        {
                            if (!clip.notes.empty()) {
                                auto& n = clip.notes.back();
                                float cents = (effect == 0x01 ? 1.0f : -1.0f) * param * 4.0f;
                                n.detune += cents;
                            }
                            break;
                        }
                        case 0x03: // Tone portamento (slide to note)
                        {
                            // Create a pitch bend expression from previous note to current
                            if (clip.notes.size() >= 2) {
                                auto& cur = clip.notes.back();
                                auto& prev = clip.notes[clip.notes.size() - 2];
                                float startPB = 0.5f; // center
                                int semidiff = cur.pitch - prev.pitch;
                                // Encode as pitch bend on the previous note's expression
                                float endPB = 0.5f + (float)semidiff / 96.0f; // 48 semi range
                                prev.expression.pitchBend.push_back({0, startPB});
                                prev.expression.pitchBend.push_back({prev.duration, endPB});
                                // Remove the target note (it's reached via bend)
                                clip.notes.pop_back();
                            }
                            break;
                        }
                        case 0x04: // Vibrato
                        {
                            if (!clip.notes.empty()) {
                                auto& n = clip.notes.back();
                                float speed = (float)(param >> 4) / 16.0f;
                                float depth = (float)(param & 0x0F) / 15.0f * 0.1f;
                                // Add sinusoidal pitch bend expression
                                int steps = 8;
                                for (int s = 0; s <= steps; ++s) {
                                    float t = (float)s / steps * n.duration;
                                    float val = 0.5f + depth * std::sin(t * speed * 6.2832f);
                                    n.expression.pitchBend.push_back({t, val});
                                }
                            }
                            break;
                        }
                        case 0x07: // Tremolo
                        {
                            if (!clip.notes.empty()) {
                                auto& n = clip.notes.back();
                                float speed = (float)(param >> 4) / 16.0f;
                                float depth = (float)(param & 0x0F) / 15.0f;
                                // Velocity modulation approximation
                                int baseVel = n.velocity;
                                n.velocity = std::max(1, (int)(baseVel * (1.0f - depth * 0.5f)));
                            }
                            break;
                        }
                        case 0x08: // Set panning
                        {
                            graph.nodes[nodeIdx].pan = (param - 128.0f) / 128.0f;
                            break;
                        }
                        case 0x09: // Sample offset
                        {
                            // Map to slip offset on the clip (approximate)
                            if (!clip.notes.empty()) {
                                // param * 256 samples into the sample
                                // We store this as detune metadata for now
                                // TODO: proper sample offset when sampler supports it
                            }
                            break;
                        }
                        case 0x0A: // Volume slide
                        {
                            if (!clip.notes.empty()) {
                                auto& n = clip.notes.back();
                                int hi = param >> 4, lo = param & 0x0F;
                                if (hi > 0) n.velocity = std::min(127, n.velocity + hi * 4);
                                else if (lo > 0) n.velocity = std::max(1, n.velocity - lo * 4);
                            }
                            break;
                        }
                        case 0x0C: // Set volume
                        {
                            if (!clip.notes.empty())
                                clip.notes.back().velocity = std::min(127, (int)(param * 2));
                            break;
                        }
                        case 0x0E: // Extended effects
                        {
                            int extCmd = param >> 4;
                            int extParam = param & 0x0F;
                            switch (extCmd) {
                                case 0x1: // Fine portamento up
                                    if (!clip.notes.empty())
                                        clip.notes.back().detune += extParam * 1.0f;
                                    break;
                                case 0x2: // Fine portamento down
                                    if (!clip.notes.empty())
                                        clip.notes.back().detune -= extParam * 1.0f;
                                    break;
                                case 0x9: // Retrigger note
                                    if (!clip.notes.empty() && extParam > 0) {
                                        auto base = clip.notes.back();
                                        float retrigInterval = beatsPerRow / extParam;
                                        base.duration = retrigInterval;
                                        clip.notes.back() = base;
                                        for (int ri = 1; ri < extParam; ++ri) {
                                            MidiNote rn = base;
                                            rn.offset = base.offset + ri * retrigInterval;
                                            clip.notes.push_back(rn);
                                        }
                                    }
                                    break;
                                case 0xC: // Note cut after X ticks
                                    if (!clip.notes.empty()) {
                                        float cutAt = beatsPerRow * extParam / currentSpeed;
                                        clip.notes.back().duration = std::min(clip.notes.back().duration, cutAt);
                                    }
                                    break;
                                case 0xD: // Note delay X ticks
                                    if (!clip.notes.empty()) {
                                        float delay = beatsPerRow * extParam / currentSpeed;
                                        clip.notes.back().offset += delay;
                                    }
                                    break;
                                default: break;
                            }
                            break;
                        }
                        case 0x0F: // Set speed/tempo
                        {
                            if (param < 32)
                                currentSpeed = (float)std::max(1, (int)param);
                            else {
                                currentTempo = (float)param;
                                graph.bpm = currentTempo;
                            }
                            beatsPerRow = 1.0f / rowsPerBeat;
                            break;
                        }
                        default:
                            break;
                    }
                }
            }
        }

        currentBeat += numRows * beatsPerRow;
    }

    // Extend all clips to cover their content
    for (int ch = 0; ch < numChannels; ++ch) {
        auto& node = graph.nodes[channelNodeIndices[ch]];
        if (!node.clips.empty())
            node.clips[0].lengthBeats = std::max(node.clips[0].lengthBeats,
                std::ceil(currentBeat / 4.0f) * 4.0f);
    }

    // Connect all channels to Master Out
    for (auto& n : graph.nodes) {
        if (n.type == NodeType::Output) {
            for (int ch = 0; ch < numChannels; ++ch) {
                auto& chNode = graph.nodes[channelNodeIndices[ch]];
                if (!chNode.pinsOut.empty() && !n.pinsIn.empty())
                    graph.addLink(chNode.pinsOut[0].id, n.pinsIn[0].id);
            }
            break;
        }
    }

    openmpt_module_destroy(mod);

    result.success = true;
    fprintf(stderr, "MOD imported: %d channels, %d patterns, %d notes\n",
            numChannels, numPatterns, result.numNotes);

#else
    result.error = "libopenmpt not available (compile with HAS_LIBOPENMPT)";
    fprintf(stderr, "MOD import: %s\n", result.error.c_str());
#endif

    return result;
}

} // namespace SoundShop

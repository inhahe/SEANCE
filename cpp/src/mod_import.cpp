#include "mod_import.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "multi_sampler.h"
#include "tracker_file_parser.h"

#ifdef HAS_LIBOPENMPT
#include "libopenmpt/libopenmpt.h"
#include "libopenmpt/libopenmpt_ext.h"
#include <juce_audio_formats/juce_audio_formats.h>
#else
#define HAS_LIBOPENMPT 0
#endif

namespace SoundShop {

bool ModImporter::isSupported(const std::string& path) {
    auto ext = path.substr(path.find_last_of('.') + 1);
    for (auto& c : ext) c = (char)std::tolower(c);
    return ext == "mod" || ext == "s3m" || ext == "it" || ext == "xm";
}

#if HAS_LIBOPENMPT

// =============================================================================
// SAMPLE EXTRACTION
// =============================================================================
//
// Each tracker sample is rendered to a standalone WAV by triggering it via
// libopenmpt's interactive interface. We trigger at libopenmpt note 60 (its
// middle C, == MIDI C4) and set the resulting Sampler node's "Base Note" to
// MIDI 60 too — pattern note values are on the same scale, so when those raw
// pattern notes feed the Sampler at base 60, the resulting pitches match
// exactly what the tracker would have played.
// =============================================================================

static std::string makeSampleDir(const std::string& modulePath) {
    auto modFile = juce::File(modulePath);
    auto stem = modFile.getFileNameWithoutExtension();
    auto dir = modFile.getParentDirectory().getChildFile(stem + "_samples");
    if (dir.createDirectory().failed()) return {};
    return dir.getFullPathName().toStdString();
}

static std::string sanitizeFilename(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char)c < 32)
            continue;
        out.push_back(c);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
    return out;
}

static std::string extractSampleToWav(openmpt_module_ext* modExt,
                                      openmpt_module_ext_interface_interactive& interactive,
                                      openmpt_module* mod,
                                      int sampleId,
                                      const std::string& sampleName,
                                      const std::string& destDir) {
    constexpr int kRenderRate = 44100;
    constexpr int kMaxSamples = kRenderRate * 4;
    constexpr int kSilenceMs = 250;
    constexpr float kSilenceThresh = 1e-4f;
    constexpr int kChunk = 1024;

    int instId = sampleId - 1;
    int playChannel = interactive.play_note(modExt, instId, 60, 1.0, 0.0);
    if (playChannel < 0) return {};

    std::vector<float> left, right;
    left.reserve(kMaxSamples);
    right.reserve(kMaxSamples);

    int silenceRun = 0;
    int silenceLimit = (kSilenceMs * kRenderRate) / 1000;
    std::vector<float> chunkL(kChunk), chunkR(kChunk);

    while ((int)left.size() < kMaxSamples) {
        size_t got = openmpt_module_read_float_stereo(
            mod, kRenderRate, kChunk, chunkL.data(), chunkR.data());
        if (got == 0) break;

        for (size_t i = 0; i < got; ++i) {
            left.push_back(chunkL[i]);
            right.push_back(chunkR[i]);
        }

        float maxAbs = 0.0f;
        for (size_t i = 0; i < got; ++i) {
            float a = std::abs(chunkL[i]);
            float b = std::abs(chunkR[i]);
            if (a > maxAbs) maxAbs = a;
            if (b > maxAbs) maxAbs = b;
        }
        if (maxAbs < kSilenceThresh) {
            silenceRun += (int)got;
            if (silenceRun >= silenceLimit) break;
        } else {
            silenceRun = 0;
        }
    }

    interactive.stop_note(modExt, playChannel);
    if (left.empty()) return {};

    int tail = (int)left.size();
    while (tail > 0 && std::abs(left[tail - 1]) < kSilenceThresh
                    && std::abs(right[tail - 1]) < kSilenceThresh)
        --tail;
    if (tail < 32) return {};

    char idxBuf[16];
    std::snprintf(idxBuf, sizeof(idxBuf), "%02d_", sampleId);
    auto safeName = sanitizeFilename(sampleName);
    if (safeName.empty()) safeName = "sample";
    auto outFile = juce::File(destDir).getChildFile(
        juce::String(std::string(idxBuf) + safeName + ".wav"));
    if (outFile.existsAsFile()) outFile.deleteFile();

    auto fileStream = std::make_unique<juce::FileOutputStream>(outFile);
    if (fileStream->failedToOpen()) return {};
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(fileStream.get(), kRenderRate, 2, 24, {}, 0));
    if (!writer) return {};
    fileStream.release();

    juce::AudioBuffer<float> buf(2, tail);
    std::memcpy(buf.getWritePointer(0), left.data(),  sizeof(float) * tail);
    std::memcpy(buf.getWritePointer(1), right.data(), sizeof(float) * tail);
    writer->writeFromAudioSampleBuffer(buf, 0, tail);
    writer.reset();
    return outFile.getFullPathName().toStdString();
}

// Create a Sampler node — same param schema as the right-click "Sampler"
// menu item in node_graph_component.cpp.
// Create a MultiSampler instrument node with one zone pointing at the
// given WAV file. Base note = MIDI 60 (C4) because that's the libopenmpt
// note we used at sample-extraction time — pattern notes from the
// tracker land on the same MIDI scale so the zone's natural playback
// matches the tracker's pitches.
static int createSamplerNode(NodeGraph& graph, const std::string& name,
                              const std::string& wavPath, Vec2 pos,
                              float pan = 0.0f,
                              bool amigaFilter = false) {
    auto& n = graph.addNode(name, NodeType::Instrument,
        {Pin{0, "MIDI", PinKind::Midi, true}},
        {Pin{0, "Audio", PinKind::Audio, false}}, pos);
    MultiSamplerDoc doc;
    doc.interpMode = InterpMode::Linear; // authentic tracker interpolation
    // MOD/S3M imports route through an Amiga PAULA reconstruction filter
    // to match the reference (OpenMPT/Winamp) output — without it sample
    // playback aliasing produces excess high-frequency content that's
    // audible as a harsher "texture" vs the reference.
    doc.amigaFilter = amigaFilter;
    MultiSamplerZone z;
    z.samplePath = wavPath;
    z.loNote = 0; z.hiNote = 127;
    z.loVel = 1; z.hiVel = 127;
    z.baseNote = 60;
    doc.zones.push_back(z);
    n.script = doc.encode();
    n.panLaw = PanLaw::Linear; // tracker uses linear pan law
    n.params.push_back({"Volume", 0.5f,  0.0f, 1.0f});
    n.params.push_back({"Pan",    pan, -1.0f, 1.0f});
    return n.id;
}

#endif // HAS_LIBOPENMPT

ModImporter::ImportResult ModImporter::import(const std::string& path, NodeGraph& graph,
                                                float posX, float posY) {
    ImportResult result;

#if HAS_LIBOPENMPT
    std::ifstream file(path, std::ios::binary);
    if (!file) { result.error = "Cannot open file"; return result; }
    std::vector<char> data((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

    openmpt_module_ext* modExt = openmpt_module_ext_create_from_memory(
        data.data(), data.size(),
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!modExt) { result.error = "Failed to parse module"; return result; }
    openmpt_module* mod = openmpt_module_ext_get_module(modExt);

    openmpt_module_ext_interface_interactive interactive{};
    int hasInteractive = openmpt_module_ext_get_interface(
        modExt, LIBOPENMPT_EXT_C_INTERFACE_INTERACTIVE,
        &interactive, sizeof(interactive));

    int numChannels  = openmpt_module_get_num_channels(mod);
    int numPatterns  = openmpt_module_get_num_patterns(mod);
    int numSamples   = openmpt_module_get_num_samples(mod);
    int numOrders    = openmpt_module_get_num_orders(mod);
    int initialTempo = openmpt_module_get_current_tempo(mod);
    int initialSpeed = openmpt_module_get_current_speed(mod);

    // Detect format. libopenmpt's "type" metadata is a short identifier
    // ("mod", "s3m", "it", "xm", "med", "stm", ...).
    enum class Fmt { Mod, S3m, It, Xm, Other };
    Fmt fmt = Fmt::Other;
    if (auto* tp = openmpt_module_get_metadata(mod, "type")) {
        std::string ts = tp;
        if      (ts == "mod") fmt = Fmt::Mod;
        else if (ts == "s3m") fmt = Fmt::S3m;
        else if (ts == "it")  fmt = Fmt::It;
        else if (ts == "xm")  fmt = Fmt::Xm;
    }
    // Default unknown formats to MOD-style dispatch (most common ancestor).
    if (fmt == Fmt::Other) fmt = Fmt::Mod;

    result.numChannels = numChannels;
    result.numPatterns = numPatterns;
    result.numSamples  = numSamples;

    // Tracker effective BPM = 6 * tempo / speed. At the standard defaults
    // (T=125, S=6) this gives 125 BPM. At S=3 it's 250, at S=12 it's 62.5.
    float initialSpeedF = (float)std::max(1, initialSpeed);
    float bpm = std::max(60.0f, 6.0f * (float)initialTempo / initialSpeedF);
    graph.bpm = bpm;

    float ticksPerRow = initialSpeedF;
    float beatsPerRow = 1.0f / 4.0f; // baseline: 4 rows per beat at initial speed

    // Current playhead beat for the pattern walk. Declared here (before
    // the effect-helper lambdas) so the lambdas can capture it by
    // reference and read "what beat is this row at" without needing
    // beat passed through every dispatch signature.
    float currentBeat = 0;
    float currentSpeed = ticksPerRow;

    // ------------------------------------------------------------------
    // Pass 1: extract samples to WAV via render-and-capture.
    // ------------------------------------------------------------------
    std::vector<std::string> samplePaths(numSamples + 1);
    std::vector<std::string> sampleNames(numSamples + 1);
    std::string sampleDir;

    if (hasInteractive && numSamples > 0) {
        sampleDir = makeSampleDir(path);
        if (!sampleDir.empty()) {
            // Silence the song so each extracted WAV captures only the
            // interactive note we trigger via play_note. play_note allocates
            // a channel outside the module's pattern-channel range, so muting
            // every pattern channel leaves the interactive note audible.
            //
            // CRITICAL: without muting, the song's pattern playback bleeds
            // into every sample WAV. When MultiSampler later plays back
            // those contaminated WAVs at varying pitches, the embedded
            // song fragments get pitch-shifted along with the actual sample
            // content — producing phantom notes at wrong pitches in the
            // rendered output. The previous "set_position_seconds(1e9)
            // + repeat_count(-1)" approach was meant to skip past song
            // playback but in practice just looped the song endlessly, so
            // every read_float_stereo call captured live pattern audio.
            for (int ch = 0; ch < numChannels; ++ch)
                interactive.set_channel_mute_status(modExt, ch, 1);
            // Repeat -1 keeps the module producing audio frames so the
            // interactive note continues rendering even if the (silent)
            // song internally reaches its end.
            openmpt_module_set_repeat_count(mod, -1);
            openmpt_module_set_position_seconds(mod, 0);
            // Use default (0) interpolation so the extracted WAVs
            // include the tracker's high-quality sinc resampling.
            // The old setting of 1 (no interpolation) caused harsh
            // aliasing when upsampling Amiga-rate samples to 44100 Hz.
            openmpt_module_set_render_param(
                mod, OPENMPT_MODULE_RENDER_INTERPOLATIONFILTER_LENGTH, 0);

            for (int s = 1; s <= numSamples; ++s) {
                const char* sn = openmpt_module_get_sample_name(mod, s);
                std::string name = (sn && sn[0]) ? sn : "";
                if (name.empty()) name = "sample_" + std::to_string(s);
                sampleNames[s] = name;
                samplePaths[s] = extractSampleToWav(modExt, interactive, mod,
                                                     s, name, sampleDir);
            }

            // Restore channel mute state — the pattern walk below reads
            // pattern data (not audio) so this isn't strictly needed for
            // correctness, but leaves the module in a clean state.
            for (int ch = 0; ch < numChannels; ++ch)
                interactive.set_channel_mute_status(modExt, ch, 0);
        }
    }

    openmpt_module_set_repeat_count(mod, 0);
    openmpt_module_set_position_order_row(mod, 0, 0);

    // ------------------------------------------------------------------
    // Pass 1b: parse the file bytes directly to pick up instrument-mode
    // metadata (note-sample keymap, envelopes, NNA, fadeout, filter
    // defaults). libopenmpt exposes instrument names but nothing else
    // — see tracker_file_parser.h/cpp for the raw format parsing.
    // ------------------------------------------------------------------
    auto parsedFile = parseTrackerFile(path);
    int numInstruments = (int)parsedFile.instruments.size();
    if (numInstruments > 0) --numInstruments; // slot 0 is unused
    const bool instrumentMode = numInstruments > 0
        && (parsedFile.format == ParsedTrackerFile::Format::IT
            || parsedFile.format == ParsedTrackerFile::Format::XM);

    // "Slot" = either sample id (sample mode) or instrument id
    // (instrument mode). In instrument mode the pattern walk uses
    // instrument ids to route notes, and each instrument becomes ONE
    // MultiSampler node with all its key-split zones pre-populated.
    // In sample mode (MOD/S3M and non-instrument XM/IT), slot == sample
    // id and we use the original one-MultiSampler-per-sample path.
    const int numSlots = instrumentMode ? numInstruments : numSamples;

    // ------------------------------------------------------------------
    // Build node structure: Group container, Sampler-per-(slot,pan) (lazy),
    // MIDI-track-per-(slot,pan) (lazy). Notes from any tracker channel
    // that play the same slot AND have the same effective pan land in a
    // shared track. Channels with different panning get separate
    // track/sampler pairs so the stereo image is preserved (e.g. MOD's
    // classic LRRL channel layout). Per-channel monophony is preserved
    // by trimming the channel's previous note when a new note arrives.
    // ------------------------------------------------------------------
    auto modName = juce::File(path).getFileName().toStdString();
    auto& group = graph.createGroup(modName, {posX, posY});
    int groupId = group.id;

    const int arraySize = std::max(numSlots, numSamples) + 1;

    // Primary arrays: store the first-created sampler/track per slot
    // for backward compat with layout code. Full pan-aware routing
    // uses the maps below.
    std::vector<int> samplerNodeId(arraySize, 0);
    std::vector<int> trackNodeId(arraySize, 0);

    // Pan-aware maps: (slotId, panKey) → nodeId. panKey = round(pan*100).
    std::map<std::pair<int,int>, int> samplerByPan;
    std::map<std::pair<int,int>, int> trackByPan;

    auto panKeyFor = [](float pan) -> int {
        return (int)std::round(pan * 100.0f);
    };

    // Compute default panning per channel based on tracker format.
    std::vector<float> channelDefaultPan(numChannels, 0.0f);
    if (fmt == Fmt::Mod || fmt == Fmt::Other) {
        // Classic Amiga LRRL pattern.
        for (int ch = 0; ch < numChannels; ++ch) {
            int m = ch % 4;
            channelDefaultPan[ch] = (m == 0 || m == 3) ? -1.0f : 1.0f;
        }
    } else if (fmt == Fmt::S3m) {
        // S3M typically alternates channels L/R.
        for (int ch = 0; ch < numChannels; ++ch)
            channelDefaultPan[ch] = (ch % 2 == 0) ? -0.75f : 0.75f;
    }
    // IT and XM: channels default to center (0.0); instrument default
    // pan (if present) overrides, handled in getOrCreateSampler.

    // Helper: effective pan for a (slotId, channelPan) combo. If the
    // instrument has its own default pan, that wins over the channel pan.
    auto effectivePanFor = [&](int slotId, float channelPan) -> float {
        if (instrumentMode && slotId > 0 && slotId <= numInstruments)
            if (parsedFile.instruments[slotId].hasDefaultPan)
                return parsedFile.instruments[slotId].defaultPan;
        return channelPan;
    };

    auto wireToMasterOut = [&](int nodeId) {
        auto* src = graph.findNode(nodeId);
        if (!src || src->pinsOut.empty()) return;
        for (auto& mn : graph.nodes) {
            if (mn.type != NodeType::Output) continue;
            if (!mn.pinsIn.empty())
                graph.addLink(src->pinsOut[0].id, mn.pinsIn[0].id);
            break;
        }
    };

    // Map a TrackerEnvelope (raw tick-based values) into a
    // SamplerEnvelope (time-based in seconds, values in the envelope's
    // conventional range for our Sampler: volume 0..1, pan -1..+1).
    // IT envelope ticks run at ~50 Hz by default, so each tick = 20 ms.
    auto envelopeFromTracker = [](const TrackerEnvelope& src,
                                   bool isPan, bool isPitch) -> SamplerEnvelope {
        SamplerEnvelope dst;
        if (!src.enabled || src.points.empty()) return dst;
        constexpr float kSecsPerTick = 1.0f / 50.0f;
        for (auto& p : src.points) {
            SamplerEnvelope::Point q;
            q.time = p.tick * kSecsPerTick;
            if (isPan) {
                // IT: 0..64 (32 = center).
                // XM: 0..64 (32 = center).
                q.value = (p.value - 32.0f) / 32.0f;
            } else if (isPitch) {
                // IT pitch envelope: ±32 ≈ ±32 semitones (1 unit = 1 semi).
                q.value = p.value;
            } else {
                // Volume envelope: 0..64 → 0..1.
                q.value = p.value / 64.0f;
            }
            dst.points.push_back(q);
        }
        if (src.hasSustain) {
            dst.hasSustain = true;
            dst.sustainStart = src.sustainStart;
            dst.sustainEnd   = src.sustainEnd;
        }
        if (src.hasLoop) {
            dst.hasLoop = true;
            dst.loopStart = src.loopStart;
            dst.loopEnd   = src.loopEnd;
        }
        return dst;
    };

    // Build a MultiSamplerDoc for an instrument by walking its noteMap
    // and grouping consecutive notes that reference the same sample
    // into one zone. Envelopes are copied over from the parsed
    // instrument, converted from tick units to seconds.
    auto buildInstrumentDoc = [&](const TrackerInstrument& inst) -> MultiSamplerDoc {
        MultiSamplerDoc doc;
        // Walk the noteMap. Group consecutive notes by sample id.
        int start = 0;
        while (start < (int)inst.noteMap.size()) {
            int sample = inst.noteMap[start].sample;
            if (sample <= 0 || sample > numSamples
                || samplePaths[sample].empty()) {
                ++start;
                continue;
            }
            int end = start;
            while (end + 1 < (int)inst.noteMap.size()
                   && inst.noteMap[end + 1].sample == sample) {
                ++end;
            }
            MultiSamplerZone z;
            z.samplePath = samplePaths[sample];
            z.loNote = start;
            z.hiNote = end;
            z.loVel  = 1;
            z.hiVel  = 127;
            z.baseNote = 60;           // matches extractSampleToWav render note
            doc.zones.push_back(z);
            start = end + 1;
        }
        // If the whole map was empty (unused instrument slot), fall
        // back to a full-range zone using sample 1 so the node still
        // makes sound if the pattern references the instrument.
        if (doc.zones.empty() && numSamples >= 1 && !samplePaths[1].empty()) {
            MultiSamplerZone z;
            z.samplePath = samplePaths[1];
            z.loNote = 0; z.hiNote = 127;
            z.loVel = 1;  z.hiVel = 127;
            z.baseNote = 60;
            doc.zones.push_back(z);
        }
        doc.volumeEnv = envelopeFromTracker(inst.volumeEnv, false, false);
        doc.panEnv    = envelopeFromTracker(inst.panEnv,    true,  false);
        doc.pitchEnv  = envelopeFromTracker(inst.pitchEnv,  false, true);
        // Filter defaults from the instrument header. IT cutoff/res are
        // 0..127; convert cutoff into Hz using the common IT formula
        // (cutoff = 2^(8 + x/24) Hz).
        if (inst.useFilterCutoff) {
            doc.filterCutoff = std::pow(2.0f, 8.0f + inst.filterCutoff / 24.0f);
            doc.filterMode = 0; // LP
        }
        if (inst.useFilterResonance)
            doc.filterResonance = inst.filterResonance / 127.0f;
        // Fadeout → add to release so the note dies naturally after
        // note-off. IT fadeout units: 0..1024, representing amount the
        // note's volume drops per tick (50 Hz). Time to fully fade:
        // 1024 / fadeOut ticks = (1024 / fadeOut) / 50 seconds.
        if (inst.fadeOut > 0) {
            float fadeSec = (1024.0f / (float)inst.fadeOut) / 50.0f;
            doc.release = std::max(doc.release, fadeSec);
        }
        return doc;
    };

    // Get-or-create a MultiSampler node for a (slot, channelPan) pair.
    // In instrument mode, the doc is populated from parsed instrument
    // metadata. In sample mode, we build a one-zone doc pointing at the
    // extracted sample WAV. Separate nodes are created for different
    // effective pan positions (e.g. MOD L/R channels).
    auto getOrCreateSampler = [&](int slotId, float channelPan) -> int {
        if (slotId <= 0 || slotId > numSlots) return -1;
        float epan = effectivePanFor(slotId, channelPan);
        int pk = panKeyFor(epan);
        auto key = std::make_pair(slotId, pk);
        auto it = samplerByPan.find(key);
        if (it != samplerByPan.end()) return it->second;

        Vec2 pos{posX + 460, posY + 30 + (slotId - 1) * 120};

        // Label suffix when the same instrument gets split by pan.
        auto panSuffix = [&]() -> std::string {
            if (epan < -0.5f) return " (L)";
            if (epan >  0.5f) return " (R)";
            return "";
        };

        // Amiga PAULA reconstruction-filter emulation. MOD/S3M files
        // historically rendered through the Amiga's analog low-pass and
        // sound dull/warm compared to raw playback; OpenMPT/Winamp apply
        // this filter by default for those formats. IT and XM are
        // PC-tracker formats with no equivalent hardware filter, so we
        // leave the filter off there.
        bool useAmigaFilter = (fmt == Fmt::Mod) || (fmt == Fmt::S3m);

        if (instrumentMode) {
            const auto& inst = parsedFile.instruments[slotId];
            auto doc = buildInstrumentDoc(inst);
            doc.interpMode = InterpMode::Linear; // authentic tracker interp
            doc.amigaFilter = useAmigaFilter;
            if (doc.zones.empty()) return -1;
            std::string label = inst.name.empty()
                ? ("Instrument " + std::to_string(slotId))
                : inst.name;
            label += panSuffix();
            auto& n = graph.addNode(label, NodeType::Instrument,
                {Pin{0, "MIDI", PinKind::Midi, true}},
                {Pin{0, "Audio", PinKind::Audio, false}}, pos);
            n.script = doc.encode();
            n.panLaw = PanLaw::Linear; // tracker linear pan law
            float vol = inst.defaultVolume > 0 ? (inst.defaultVolume / 128.0f) : 0.5f;
            n.params.push_back({"Volume", vol, 0.0f, 1.0f});
            n.params.push_back({"Pan",    epan, -1.0f, 1.0f});
            samplerByPan[key] = n.id;
            if (samplerNodeId[slotId] == 0) samplerNodeId[slotId] = n.id;
            graph.addToGroup(groupId, n.id);
            wireToMasterOut(n.id);
            return n.id;
        }

        // Sample mode: one-zone instrument built from the extracted WAV.
        if (slotId > numSamples || samplePaths[slotId].empty()) return -1;
        std::string label = sampleNames[slotId].empty()
            ? ("Sample " + std::to_string(slotId))
            : sampleNames[slotId];
        label += panSuffix();
        int id = createSamplerNode(graph, label, samplePaths[slotId],
                                   pos, epan, useAmigaFilter);
        samplerByPan[key] = id;
        if (samplerNodeId[slotId] == 0) samplerNodeId[slotId] = id;
        graph.addToGroup(groupId, id);
        wireToMasterOut(id);
        return id;
    };

    auto getOrCreateTrack = [&](int slotId, float channelPan) -> int {
        if (slotId <= 0 || slotId > numSlots) {
            if (trackNodeId[0] != 0) return trackNodeId[0];
            auto& trk = graph.addNode("Pre-instrument", NodeType::MidiTimeline,
                {Pin{0, "MIDI In", PinKind::Midi, true}},
                {Pin{0, "MIDI", PinKind::Midi, false}}, Vec2{posX + 200, posY});
            trk.clips.push_back({"Pattern", 0, 4, 0xFF6688CC});
            graph.addToGroup(groupId, trk.id);
            trackNodeId[0] = trk.id;
            trackByPan[{0, 0}] = trk.id;
            wireToMasterOut(trk.id);
            return trk.id;
        }
        float epan = effectivePanFor(slotId, channelPan);
        int pk = panKeyFor(epan);
        auto key = std::make_pair(slotId, pk);
        auto it = trackByPan.find(key);
        if (it != trackByPan.end()) return it->second;

        int sampNodeId = getOrCreateSampler(slotId, channelPan);
        std::string baseName;
        if (instrumentMode) {
            baseName = parsedFile.instruments[slotId].name.empty()
                ? ("Instrument " + std::to_string(slotId))
                : parsedFile.instruments[slotId].name;
        } else if (slotId <= numSamples) {
            baseName = sampleNames[slotId].empty()
                ? ("Sample " + std::to_string(slotId))
                : sampleNames[slotId];
        } else {
            baseName = "Slot " + std::to_string(slotId);
        }
        // Pan suffix to distinguish L/R variants in the node graph.
        if (epan < -0.5f) baseName += " (L)";
        else if (epan > 0.5f) baseName += " (R)";

        Vec2 pos{posX + 200, posY + 30 + (slotId - 1) * 120};
        auto& trk = graph.addNode(baseName, NodeType::MidiTimeline,
            {Pin{0, "MIDI In", PinKind::Midi, true}},
            {Pin{0, "MIDI", PinKind::Midi, false}}, pos);
        trk.clips.push_back({"Pattern", 0, 4, 0xFF6688CC});
        graph.addToGroup(groupId, trk.id);
        int trkId = trk.id;
        if (sampNodeId >= 0) {
            auto* trkPtr = graph.findNode(trkId);
            auto* sampPtr = graph.findNode(sampNodeId);
            if (trkPtr && sampPtr && !trkPtr->pinsOut.empty()) {
                for (auto& pin : sampPtr->pinsIn) {
                    if (pin.kind == PinKind::Midi) {
                        graph.addLink(trkPtr->pinsOut[0].id, pin.id);
                        break;
                    }
                }
            }
        } else {
            wireToMasterOut(trkId);
        }
        trackByPan[key] = trkId;
        if (trackNodeId[slotId] == 0) trackNodeId[slotId] = trkId;
        return trkId;
    };

    // ------------------------------------------------------------------
    // Per-channel state carried during the pattern walk.
    // ------------------------------------------------------------------
    // "slotId" means the current routing key for a channel — sample id
    // in sample mode, instrument id in instrument mode.
    struct LastNoteRef { int sampleId = -1; int noteIdx = -1; int trkNodeId = 0; };
    // New-note action: decides what to do with the *previous* note on a
    // channel when a new note arrives. Cut is the default for all formats
    // (and the only behavior MOD/S3M support). IT instrument mode adds
    // Continue / Off / Fade, which can also be set per-row via S73-S76.
    //
    // For import, Cut = trim the previous note at the new note's start
    // beat (what we already did). The other three all boil down to "don't
    // trim; let the previous note's full duration and release play out",
    // which is what the Sampler's voice envelope will do automatically as
    // long as we leave the note alone. We don't currently distinguish
    // them in the baked MIDI data.
    enum class NNA : int { Cut = 0, Continue = 1, Off = 2, Fade = 3 };
    struct ChannelState {
        int currentSample = 0;
        int activeSamplerNodeId = 0;  // for pan/effect automation
        int  vibratoWave = 0;
        int  tremoloWave = 0;
        int  glissandoMode = 0;
        float finetuneCents = 0.0f;
        NNA   newNoteAction = NNA::Cut;
        LastNoteRef lastNote;
        // Sound-control tracking: S98/S99 reverb toggle.
        // When reverbOn is true, reverb send is active from
        // reverbOnBeat through the current beat. On S98 or song end,
        // the region is closed and pushed into reverbRegions[slot].
        bool  reverbOn = false;
        float reverbOnBeat = 0.0f;
    };
    std::vector<ChannelState> chState(numChannels);

    // Project-wide global volume as a 0..1 multiplier. Vxx / Gxx set it
    // absolutely; Wxx / Hxx slide it linearly. Multiplied into every note
    // velocity at creation time so velocity automation reflects the song's
    // running dynamics without needing a live global-volume processor.
    float globalVolume = 1.0f;

    // Accumulated reverb-send regions per slot. After the pattern walk,
    // any slot that has entries gets a Sampler→Reverb link with these
    // regions attached as EffectRegions on the slot's track node.
    struct BeatRegion { float startBeat; float endBeat; };
    std::vector<std::vector<BeatRegion>> reverbRegions(arraySize);

    // Close a channel's open reverb region (if any) at `endBeat` and
    // push it into reverbRegions[currentSlot].
    auto closeReverbRegion = [&](int ch, float endBeat) {
        if (!chState[ch].reverbOn) return;
        chState[ch].reverbOn = false;
        int slot = chState[ch].currentSample;
        if (slot > 0 && slot < (int)reverbRegions.size())
            reverbRegions[slot].push_back({chState[ch].reverbOnBeat, endBeat});
    };

    auto resolveLast = [&](LastNoteRef ref) -> MidiNote* {
        if (ref.trkNodeId == 0 || ref.noteIdx < 0) return nullptr;
        auto* tn = graph.findNode(ref.trkNodeId);
        if (!tn || tn->clips.empty()) return nullptr;
        auto& clip = tn->clips[0];
        if (ref.noteIdx >= (int)clip.notes.size()) return nullptr;
        return &clip.notes[ref.noteIdx];
    };

    auto trimNote = [&](LastNoteRef ref, float endBeat) {
        auto* nn = resolveLast(ref);
        if (!nn) return;
        float trkStart = 0.0f;
        if (auto* tn = graph.findNode(ref.trkNodeId))
            if (!tn->clips.empty()) trkStart = tn->clips[0].startBeat;
        float relEnd = endBeat - trkStart;
        if (relEnd > nn->offset && relEnd < nn->offset + nn->duration)
            nn->duration = relEnd - nn->offset;
    };

    // ------------------------------------------------------------------
    // Effect helpers (called from per-format dispatch). All operate on
    // the captured `ch`/`param`/state.
    // ------------------------------------------------------------------
    auto applyVolumeSlide = [&](int ch, uint8_t param) {
        if (auto* nn = resolveLast(chState[ch].lastNote)) {
            int hi = param >> 4, lo = param & 0x0F;
            if (hi > 0) nn->velocity = std::min(127, nn->velocity + hi * 4);
            else if (lo > 0) nn->velocity = std::max(1, nn->velocity - lo * 4);
        }
    };
    auto applyFineVolUp = [&](int ch, int amt) {
        if (auto* nn = resolveLast(chState[ch].lastNote))
            nn->velocity = std::min(127, nn->velocity + amt * 4);
    };
    auto applyFineVolDown = [&](int ch, int amt) {
        if (auto* nn = resolveLast(chState[ch].lastNote))
            nn->velocity = std::max(1, nn->velocity - amt * 4);
    };
    auto applyPortaUp = [&](int ch, uint8_t param) {
        if (auto* nn = resolveLast(chState[ch].lastNote))
            nn->detune += param * 4.0f;
    };
    auto applyPortaDown = [&](int ch, uint8_t param) {
        if (auto* nn = resolveLast(chState[ch].lastNote))
            nn->detune -= param * 4.0f;
    };
    auto applyFinePortaUp = [&](int ch, int amt) {
        if (auto* nn = resolveLast(chState[ch].lastNote)) nn->detune += amt;
    };
    auto applyFinePortaDown = [&](int ch, int amt) {
        if (auto* nn = resolveLast(chState[ch].lastNote)) nn->detune -= amt;
    };
    auto applyExtraFinePortaUp = [&](int ch, int amt) {
        // XM Xxx: 1/4 the step of regular fine portamento
        if (auto* nn = resolveLast(chState[ch].lastNote)) nn->detune += amt * 0.25f;
    };
    auto applyExtraFinePortaDown = [&](int ch, int amt) {
        if (auto* nn = resolveLast(chState[ch].lastNote)) nn->detune -= amt * 0.25f;
    };
    auto applyToneSlide = [&](int ch) {
        int trkId = chState[ch].lastNote.trkNodeId;
        if (trkId == 0) return;
        auto* tn = graph.findNode(trkId);
        if (!tn || tn->clips.empty()) return;
        auto& clip = tn->clips[0];
        if (clip.notes.size() < 2) return;
        auto& cur = clip.notes.back();
        auto& prev = clip.notes[clip.notes.size() - 2];
        int semidiff = cur.pitch - prev.pitch;
        prev.expression.pitchBend.push_back({0,                0.5f});
        prev.expression.pitchBend.push_back({prev.duration,
                                              0.5f + (float)semidiff / 96.0f});
        clip.notes.pop_back();
        chState[ch].lastNote.noteIdx = (int)clip.notes.size() - 1;
    };
    auto applyVibrato = [&](int ch, uint8_t param) {
        auto* nn = resolveLast(chState[ch].lastNote);
        if (!nn) return;
        float speed = (float)(param >> 4) / 16.0f;
        float depth = (float)(param & 0x0F) / 15.0f * 0.1f;
        int steps = 8;
        for (int s = 0; s <= steps; ++s) {
            float t = (float)s / steps * nn->duration;
            // Choose shape from per-channel state.
            float lfo = 0.0f;
            float ph = t * speed * 6.2832f;
            switch (chState[ch].vibratoWave) {
                case 1: lfo = std::fmod(ph / 6.2832f, 1.0f) * 2.0f - 1.0f; break; // ramp
                case 2: lfo = std::sin(ph) >= 0 ? 1.0f : -1.0f; break;            // square
                case 3: lfo = ((float)std::rand() / RAND_MAX) * 2.0f - 1.0f; break;// random
                default: lfo = std::sin(ph); break;                                // sine
            }
            nn->expression.pitchBend.push_back({t, 0.5f + depth * lfo});
        }
    };
    auto applyTremolo = [&](int ch, uint8_t param) {
        // Approximation: scale velocity by half the depth.
        if (auto* nn = resolveLast(chState[ch].lastNote)) {
            float depth = (float)(param & 0x0F) / 15.0f;
            int baseVel = nn->velocity;
            nn->velocity = std::max(1, (int)(baseVel * (1.0f - depth * 0.5f)));
        }
    };
    auto applySetVolume = [&](int ch, uint8_t param) {
        // MOD/XM Cxx: param is 0-64. S3M is the same.
        if (auto* nn = resolveLast(chState[ch].lastNote))
            nn->velocity = std::min(127, (int)(param * 2));
    };
    // Find the Sampler node's "Pan" parameter for a given sample id,
    // creating the Sampler on demand if it doesn't exist yet. Returns
    // nullptr if there's no sampler for that id (sample extraction
    // failed). Used by both set-pan and panbrello to post points into
    // the Pan param's automation lane — so multiple pan commands over
    // the song cumulatively form a pan curve rather than having the
    // last one win.
    auto getSamplerPanLane = [&](int ch) -> AutomationLane* {
        int sampId = chState[ch].activeSamplerNodeId;
        if (sampId == 0) return nullptr;
        auto* sn = graph.findNode(sampId);
        if (!sn) return nullptr;
        for (auto& p : sn->params)
            if (p.name == "Pan") return &p.automation;
        return nullptr;
    };

    auto applySetPanning = [&](int ch, int panVal) {
        // panVal is in tracker units (0-255 for 8xx / S8x); map to [-1,1].
        // Emit as an automation point on the sampler's Pan param at the
        // current beat so successive pan changes accumulate into a lane.
        auto* lane = getSamplerPanLane(ch);
        if (!lane) return;
        float pan = juce::jlimit(-1.0f, 1.0f, (panVal - 128.0f) / 128.0f);
        lane->points.push_back({currentBeat, pan});
    };

    // Panbrello (S3M/IT Yxx): sinusoidal (or other waveform) pan wobble.
    // We don't have a runtime panbrello processor — instead we sample the
    // waveform at 8 points across the current row and post them as
    // automation points on the sampler's Pan param. The channel's tremolo
    // waveform (chState[ch].tremoloWave) isn't used — panbrello has its
    // own waveform setting via S5x, which we don't track separately; we
    // treat panbrello as always sine for the baked output.
    auto applyPanbrello = [&](int ch, uint8_t param) {
        auto* lane = getSamplerPanLane(ch);
        if (!lane) return;
        float speed = (float)(param >> 4) / 16.0f;
        float depth = (float)(param & 0x0F) / 15.0f;
        constexpr int steps = 8;
        for (int s = 0; s <= steps; ++s) {
            float t = (float)s / steps;
            float localBeat = currentBeat + t * beatsPerRow;
            float lfo = std::sin(t * speed * 6.2832f);
            float pan = juce::jlimit(-1.0f, 1.0f, depth * lfo);
            lane->points.push_back({localBeat, pan});
        }
    };

    // Global volume handlers. S3M/IT Vxx is 0..64, IT extends to 0..128;
    // XM Gxx is 0..64. We normalize to a 0..1 multiplier.
    auto setGlobalVolAbs = [&](int param, int maxVal) {
        globalVolume = juce::jlimit(0.0f, 1.0f,
            (float)param / (float)std::max(1, maxVal));
    };
    // Wxx / Hxx: slide current global vol by nibble each tick. We
    // approximate "per-tick slide" with a single per-row step since our
    // pattern walk doesn't model individual ticks.
    auto applyGlobalVolSlide = [&](uint8_t param) {
        int hi = param >> 4, lo = param & 0x0F;
        if (hi > 0) globalVolume = juce::jlimit(0.0f, 1.0f,
            globalVolume + hi * 0.02f);
        else if (lo > 0) globalVolume = juce::jlimit(0.0f, 1.0f,
            globalVolume - lo * 0.02f);
    };
    auto applySampleOffset = [&](int ch, uint8_t param) {
        // 9xx (MOD/XM) / Oxx (S3M/IT): start `param * 256` samples in.
        // Approximation: nudge the note's offset slightly earlier.
        if (auto* nn = resolveLast(chState[ch].lastNote)) {
            float skipBeats = (param * 256.0f / 8363.0f) * (graph.bpm / 60.0f);
            nn->offset = std::max(0.0f, nn->offset - skipBeats);
        }
    };
    auto applyRetrig = [&](int ch, int times, float beatsPerRow_) {
        if (times <= 0) return;
        int trkId = chState[ch].lastNote.trkNodeId;
        if (trkId == 0) return;
        auto* tn = graph.findNode(trkId);
        if (!tn || tn->clips.empty()) return;
        auto& clip = tn->clips[0];
        if (clip.notes.empty()) return;
        MidiNote base = clip.notes.back();
        float retrigInterval = beatsPerRow_ / times;
        base.duration = retrigInterval;
        clip.notes.back() = base;
        for (int ri = 1; ri < times; ++ri) {
            MidiNote rn = base;
            rn.offset = base.offset + ri * retrigInterval;
            clip.notes.push_back(rn);
        }
        chState[ch].lastNote.noteIdx = (int)clip.notes.size() - 1;
    };
    auto applyArpeggio = [&](int ch, uint8_t param, float beatsPerRow_) {
        if (param == 0) return;
        auto ref = chState[ch].lastNote;
        auto* base = resolveLast(ref);
        if (!base) return;
        int trkId = ref.trkNodeId;
        auto* tn = graph.findNode(trkId);
        if (!tn || tn->clips.empty()) return;
        auto& clip = tn->clips[0];
        int semi1 = param >> 4;
        int semi2 = param & 0x0F;
        float third = beatsPerRow_ / 3.0f;
        MidiNote baseCopy = *base;
        if (semi1 > 0) {
            MidiNote n1 = baseCopy;
            n1.offset = baseCopy.offset + third;
            n1.pitch = std::min(127, baseCopy.pitch + semi1);
            n1.duration = third;
            clip.notes.push_back(n1);
        }
        if (semi2 > 0) {
            MidiNote n2 = baseCopy;
            n2.offset = baseCopy.offset + third * 2;
            n2.pitch = std::min(127, baseCopy.pitch + semi2);
            n2.duration = third;
            clip.notes.push_back(n2);
        }
        if (auto* baseAfter = resolveLast(ref))
            baseAfter->duration = third;
    };

    // Side-effect outputs from the per-row dispatch (consumed at end of row).
    int sharedNoteCutTicks = -1;          // 0xC: trim last note to N ticks
    int sharedNoteDelayTicks = -1;        // 0xD: shift last note by N ticks
    int patternDelayRows = 0;              // 0xE: rest after this row
    int posJumpTarget = -1;                // 0xB: switch to order
    int patternBreakRow = -1;              // 0xD: jump to next pattern at row
    int speedSet = -1;                     // tempo/speed events
    bool tempoSetThisRow = false;          // libopenmpt set_current_tempo via graph.bpm

    auto applyNoteCutTicks = [&](int ch, int ticks, float currentSpeed_, float beatsPerRow_) {
        if (auto* nn = resolveLast(chState[ch].lastNote)) {
            float cutAt = beatsPerRow_ * ticks / std::max(1.0f, currentSpeed_);
            nn->duration = std::min(nn->duration, cutAt);
        }
    };
    auto applyNoteDelayTicks = [&](int ch, int ticks, float currentSpeed_, float beatsPerRow_) {
        if (auto* nn = resolveLast(chState[ch].lastNote)) {
            float delay = beatsPerRow_ * ticks / std::max(1.0f, currentSpeed_);
            nn->offset += delay;
        }
    };

    // ------------------------------------------------------------------
    // Per-format effect dispatch helpers. All take the lowercased
    // effect-letter character (or '0'..'9' for MOD/XM numeric letters)
    // and the raw parameter byte. Each helper updates the graph and the
    // shared per-row out-vars. Per-channel state lives in `chState[ch]`.
    // ------------------------------------------------------------------

    // E-extended (MOD/XM "E" command). Subcommand in upper nibble of param.
    // Expand a MIDI macro template string, substituting placeholder
    // tokens with the current context, and emit the resulting bytes as
    // CC events on the track for the channel's current slot. The IT
    // default macro "F0F000z" encodes filter cutoff — we recognize that
    // common pattern and emit CC74 (brightness / filter cutoff) instead
    // of raw SysEx, since our MultiSampler reads CC74 via automation.
    //
    // Macro string format: pairs of hex characters form bytes. Special
    // single-character tokens: 'z'/'Z' = Zxx param (0..127), 'n' = note,
    // 'v' = velocity, 'u' = volume, 'p' = pan, others ignored.
    auto expandMidiMacro = [&](int ch, const std::string& macroStr, uint8_t zParam) {
        if (macroStr.empty()) return;
        int trkId = chState[ch].lastNote.trkNodeId;
        if (trkId == 0) return;
        auto* tn = graph.findNode(trkId);
        if (!tn || tn->clips.empty()) return;
        auto& clip = tn->clips[0];

        // Recognize the ultra-common default: "F0F000z" — IT's internal
        // filter cutoff. Emit as CC74 (MIDI standard brightness) on the
        // track, which the MultiSampler can read via CC routing.
        if (macroStr == "F0F000z" || macroStr == "F0F000Z") {
            MidiCCEvent cc;
            cc.offset     = currentBeat - clip.startBeat;
            cc.controller = 74; // brightness / filter cutoff
            cc.value      = std::min(127, (int)zParam);
            cc.channel    = 1;
            clip.ccEvents.push_back(cc);
            return;
        }

        // General expansion: walk the macro string two characters at a
        // time, parsing hex bytes and substituting tokens. Collect the
        // expanded bytes, then emit as individual CC events if the
        // result looks like CC messages, or as raw SysEx if it starts
        // with F0.
        std::vector<uint8_t> expanded;
        size_t i = 0;
        auto hexVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        while (i < macroStr.size()) {
            char c = macroStr[i];
            // Single-char placeholder tokens.
            if (c == 'z' || c == 'Z') { expanded.push_back(zParam & 0x7F); ++i; continue; }
            if (c == 'n') {
                auto ref = chState[ch].lastNote;
                auto* nn = resolveLast(ref);
                expanded.push_back(nn ? (uint8_t)(nn->pitch & 0x7F) : 0);
                ++i; continue;
            }
            if (c == 'v') {
                auto ref = chState[ch].lastNote;
                auto* nn = resolveLast(ref);
                expanded.push_back(nn ? (uint8_t)(nn->velocity & 0x7F) : 100);
                ++i; continue;
            }
            // Two-char hex byte.
            if (i + 1 < macroStr.size()) {
                int hi = hexVal(c);
                int lo = hexVal(macroStr[i + 1]);
                if (hi >= 0 && lo >= 0) {
                    expanded.push_back((uint8_t)((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            ++i; // skip unrecognized
        }

        // Emit: if the expanded data is a 3-byte MIDI CC (Bx cc vv),
        // emit as a MidiCCEvent. Otherwise emit the first two data
        // bytes as CC74 (heuristic — many macro expansions are
        // filter sweeps, and CC74 is the best-effort landing target
        // until we support arbitrary SysEx routing).
        if (expanded.size() >= 3 && (expanded[0] & 0xF0) == 0xB0) {
            MidiCCEvent cc;
            cc.offset     = currentBeat - clip.startBeat;
            cc.controller = expanded[1] & 0x7F;
            cc.value      = expanded[2] & 0x7F;
            cc.channel    = (expanded[0] & 0x0F) + 1;
            clip.ccEvents.push_back(cc);
        } else if (!expanded.empty()) {
            // Heuristic: treat the z-substituted byte as a CC74 value.
            MidiCCEvent cc;
            cc.offset     = currentBeat - clip.startBeat;
            cc.controller = 74;
            cc.value      = zParam & 0x7F;
            cc.channel    = 1;
            clip.ccEvents.push_back(cc);
        }
    };

    auto handleMODExtended = [&](int ch, uint8_t param, float currentSpeed_) {
        int sub = param >> 4;
        int x   = param & 0x0F;
        switch (sub) {
            case 0x0: break; // filter (Amiga LED) — ignore
            case 0x1: applyFinePortaUp(ch, x); break;
            case 0x2: applyFinePortaDown(ch, x); break;
            case 0x3: chState[ch].glissandoMode = x; break;
            case 0x4: chState[ch].vibratoWave = x & 0x3; break;
            case 0x5: { // set finetune (4-bit signed, 1/8 semitone steps)
                int signedFt = (x < 8) ? x : (x - 16);
                float cents = signedFt * 12.5f;
                chState[ch].finetuneCents = cents;
                if (auto* nn = resolveLast(chState[ch].lastNote))
                    nn->detune = cents;
                break;
            }
            case 0x6: // pattern loop — handled at row level via shared signal
                // Use sharedNoteCutTicks repurposed? No — we need a dedicated
                // signal. Pattern loops pass through a separate path; this
                // case is a no-op here and the row-level scan also looks
                // directly for E6/SBx.
                break;
            case 0x7: chState[ch].tremoloWave = x & 0x3; break;
            case 0x8: break; // sync — ignore
            case 0x9: applyRetrig(ch, x, beatsPerRow); break;
            case 0xA: applyFineVolUp(ch, x); break;
            case 0xB: applyFineVolDown(ch, x); break;
            case 0xC: applyNoteCutTicks(ch, x, currentSpeed_, beatsPerRow); break;
            case 0xD: applyNoteDelayTicks(ch, x, currentSpeed_, beatsPerRow); break;
            case 0xE: patternDelayRows = std::max(patternDelayRows, x); break;
            case 0xF: break; // invert loop — ignore
        }
    };

    // S3M/IT Sxx subcommand handler. Subcommand in upper nibble.
    auto handleSxxExtended = [&](int ch, uint8_t param, float currentSpeed_) {
        int sub = param >> 4;
        int x   = param & 0x0F;
        switch (sub) {
            case 0x0: break;                             // S0x filter
            case 0x1: chState[ch].glissandoMode = x; break; // S1x glissando
            case 0x2: { // S2x set finetune (S3M MIDI macro / set finetune)
                int signedFt = (x < 8) ? x : (x - 16);
                chState[ch].finetuneCents = signedFt * 12.5f;
                if (auto* nn = resolveLast(chState[ch].lastNote))
                    nn->detune = chState[ch].finetuneCents;
                break;
            }
            case 0x3: chState[ch].vibratoWave = x & 0x3; break; // S3x vibrato wave
            case 0x4: chState[ch].tremoloWave = x & 0x3; break; // S4x tremolo wave
            case 0x5: break; // S5x panbrello waveform
            case 0x6: patternDelayRows = std::max(patternDelayRows, x); break; // S6x fine pattern delay (in ticks; we round to row)
            case 0x7: {
                // S7x — past-note actions and new-note-action settings.
                // S70 = past note cut, S71 = past note off, S72 = past note fade:
                //   operate on the channel's currently-ringing note now.
                // S73-S76 = set the channel's NNA for future new-notes.
                // S77-S7F = duplicate check type/action — we don't model these.
                switch (x) {
                    case 0x0: // past note cut — trim now
                        trimNote(chState[ch].lastNote, currentBeat);
                        break;
                    case 0x1: // past note off — trim now (release envelope plays out)
                    case 0x2: // past note fade — trim now (approximation)
                        trimNote(chState[ch].lastNote, currentBeat);
                        break;
                    case 0x3: chState[ch].newNoteAction = NNA::Cut;      break;
                    case 0x4: chState[ch].newNoteAction = NNA::Continue; break;
                    case 0x5: chState[ch].newNoteAction = NNA::Off;      break;
                    case 0x6: chState[ch].newNoteAction = NNA::Fade;     break;
                    default: break; // S77-S7F ignored
                }
                break;
            }
            case 0x8: applySetPanning(ch, (x * 16 + 8)); break; // S8x set pan (4-bit -> 0..255)
            case 0x9: { // S9x sound control
                // S90/S91: surround off/on — TODO surround-widener effect
                // S98: reverb off for this channel
                // S99: reverb on for this channel
                if (x == 0x8) {
                    closeReverbRegion(ch, currentBeat);
                } else if (x == 0x9) {
                    if (!chState[ch].reverbOn) {
                        chState[ch].reverbOn = true;
                        chState[ch].reverbOnBeat = currentBeat;
                    }
                }
                break;
            }
            case 0xA: break; // SA0/SA1 stereo control
            case 0xB: // SB0 / SBn pattern loop — handled at row level
                break;
            case 0xC: applyNoteCutTicks(ch, x, currentSpeed_, beatsPerRow); break;
            case 0xD: applyNoteDelayTicks(ch, x, currentSpeed_, beatsPerRow); break;
            case 0xE: patternDelayRows = std::max(patternDelayRows, x); break; // SEx pattern delay (rows)
            case 0xF: break; // SFx MIDI macro
        }
    };

    // MOD effect dispatch. Letters '0'..'9','A'..'F'.
    auto handleMODEffect = [&](int ch, char letter, uint8_t param,
                                float currentSpeed_) {
        switch (letter) {
            case '0': applyArpeggio(ch, param, beatsPerRow); break;
            case '1': applyPortaUp(ch, param); break;
            case '2': applyPortaDown(ch, param); break;
            case '3': applyToneSlide(ch); break;
            case '4': applyVibrato(ch, param); break;
            case '5': applyToneSlide(ch); applyVolumeSlide(ch, param); break;
            case '6': applyVibrato(ch, param); applyVolumeSlide(ch, param); break;
            case '7': applyTremolo(ch, param); break;
            case '8': applySetPanning(ch, param); break;
            case '9': applySampleOffset(ch, param); break;
            case 'A': applyVolumeSlide(ch, param); break;
            case 'B': posJumpTarget = param; break;
            case 'C': applySetVolume(ch, param); break;
            case 'D': // pattern break — param is "decimal of hex" in classic MOD
                patternBreakRow = ((param >> 4) * 10) + (param & 0x0F);
                if (patternBreakRow < 0) patternBreakRow = 0;
                break;
            case 'E': handleMODExtended(ch, param, currentSpeed_); break;
            case 'F':
                if (param < 32)
                    speedSet = param;
                else {
                    graph.bpm = 6.0f * (float)param / initialSpeedF;
                    tempoSetThisRow = true;
                }
                break;
            default: break;
        }
    };

    // S3M effect dispatch. Letters 'A'..'Z'. IT inherits this set.
    auto handleS3MEffect = [&](int ch, char letter, uint8_t param,
                                float currentSpeed_) {
        switch (letter) {
            case 'A': // set speed (ticks per row)
                if (param > 0) speedSet = param;
                break;
            case 'B': posJumpTarget = param; break;
            case 'C': // pattern break (param is decimal-of-hex per spec)
                patternBreakRow = ((param >> 4) * 10) + (param & 0x0F);
                if (patternBreakRow < 0) patternBreakRow = 0;
                break;
            case 'D': applyVolumeSlide(ch, param); break;
            case 'E': applyPortaDown(ch, param); break;
            case 'F': applyPortaUp(ch, param); break;
            case 'G': applyToneSlide(ch); break;
            case 'H': applyVibrato(ch, param); break;
            case 'I': // tremor — gate the note in semi-regular pulses
                applyTremolo(ch, param);
                break;
            case 'J': applyArpeggio(ch, param, beatsPerRow); break;
            case 'K': applyVibrato(ch, param); applyVolumeSlide(ch, param); break;
            case 'L': applyToneSlide(ch); applyVolumeSlide(ch, param); break;
            case 'M': // set channel volume
                applySetVolume(ch, std::min<int>(64, param));
                break;
            case 'N': applyVolumeSlide(ch, param); break;
            case 'O': applySampleOffset(ch, param); break;
            case 'P': // pan slide — approximate as nothing for now
                break;
            case 'Q': applyRetrig(ch, param & 0x0F, beatsPerRow); break;
            case 'R': applyTremolo(ch, param); break;
            case 'S': handleSxxExtended(ch, param, currentSpeed_); break;
            case 'T': // set tempo (BPM)
                graph.bpm = 6.0f * (float)param / initialSpeedF;
                tempoSetThisRow = true;
                break;
            case 'U': applyVibrato(ch, param); break; // fine vibrato — same code, smaller depth in real engine
            case 'V': // set global volume. IT = 0..128, S3M = 0..64.
                setGlobalVolAbs(param, fmt == Fmt::It ? 128 : 64);
                break;
            case 'W': // global volume slide
                applyGlobalVolSlide(param);
                break;
            case 'X': applySetPanning(ch, param); break;
            case 'Y': // panbrello — sinusoidal pan wobble
                applyPanbrello(ch, param);
                break;
            case 'Z': { // MIDI macro — expand the Zxx macro template
                // Zxx invokes the fixed macro at index param (0x00-0x7F).
                // The macro string comes from the parsed file header.
                if (parsedFile.hasMidiMacros && param < 128) {
                    const auto& macro = parsedFile.midiMacros.zMacros[param];
                    if (!macro.empty())
                        expandMidiMacro(ch, macro, param);
                }
                break;
            }
            default: break;
        }
    };

    // XM effect dispatch. XM uses MOD's letters/numbers plus G/H/K/L/P/R/T/X.
    auto handleXMEffect = [&](int ch, char letter, uint8_t param,
                                float currentSpeed_) {
        switch (letter) {
            // MOD-style 0-F core
            case '0': applyArpeggio(ch, param, beatsPerRow); break;
            case '1': applyPortaUp(ch, param); break;
            case '2': applyPortaDown(ch, param); break;
            case '3': applyToneSlide(ch); break;
            case '4': applyVibrato(ch, param); break;
            case '5': applyToneSlide(ch); applyVolumeSlide(ch, param); break;
            case '6': applyVibrato(ch, param); applyVolumeSlide(ch, param); break;
            case '7': applyTremolo(ch, param); break;
            case '8': applySetPanning(ch, param); break;
            case '9': applySampleOffset(ch, param); break;
            case 'A': applyVolumeSlide(ch, param); break;
            case 'B': posJumpTarget = param; break;
            case 'C': applySetVolume(ch, param); break;
            case 'D':
                patternBreakRow = ((param >> 4) * 10) + (param & 0x0F);
                if (patternBreakRow < 0) patternBreakRow = 0;
                break;
            case 'E': handleMODExtended(ch, param, currentSpeed_); break;
            case 'F':
                if (param < 32) speedSet = param;
                else { graph.bpm = 6.0f * (float)param / initialSpeedF; tempoSetThisRow = true; }
                break;
            // XM extensions
            case 'G': // set global volume (0..64)
                setGlobalVolAbs(param, 64);
                break;
            case 'H': // global volume slide
                applyGlobalVolSlide(param);
                break;
            case 'K': // key off — trim last note now
                trimNote(chState[ch].lastNote, 0); // 0 means "now" handled by trim
                break;
            case 'L': // set envelope position — ignore
                break;
            case 'P': // panning slide — ignore
                break;
            case 'R': applyRetrig(ch, param & 0x0F, beatsPerRow); break;
            case 'T': // tremor
                applyTremolo(ch, param);
                break;
            case 'X': { // X1y / X2y extra fine portamento (y in low nibble)
                int sub = param >> 4;
                int y   = param & 0x0F;
                if      (sub == 1) applyExtraFinePortaUp(ch, y);
                else if (sub == 2) applyExtraFinePortaDown(ch, y);
                break;
            }
            default: break;
        }
    };

    // Per-row format dispatch.
    auto dispatchEffect = [&](int ch, char letter, uint8_t param,
                               float currentSpeed_) {
        switch (fmt) {
            case Fmt::Mod: handleMODEffect(ch, letter, param, currentSpeed_); break;
            case Fmt::Xm:  handleXMEffect (ch, letter, param, currentSpeed_); break;
            case Fmt::S3m:
            case Fmt::It:  handleS3MEffect(ch, letter, param, currentSpeed_); break;
            default: handleMODEffect(ch, letter, param, currentSpeed_); break;
        }
    };

    // ------------------------------------------------------------------
    // Pass 2: walk patterns/orders, decoding rows and placing notes.
    //
    // Pattern loops (E6 / SBx) are unrolled inline by re-walking rows
    // within the pattern with currentBeat continuing to advance, so the
    // looped sections appear as duplicated note data in the MIDI tracks
    // rather than requiring runtime looping.
    // ------------------------------------------------------------------
    // currentBeat / currentSpeed are already declared near the top of
    // the function so effect-helper lambdas can capture them by reference.
    currentBeat = 0;
    currentSpeed = ticksPerRow;

    int order = 0;
    int forcedNextRow = -1;     // set by pattern break for next pattern's start row

    // Track which orders we've already walked and the beat position each
    // one started at. When a Bxx position-jump lands on an order we've
    // already processed, that's a whole-song loop — we don't unroll it
    // inline. Instead we stop pattern walking, set the project's
    // songLengthBeats to the accumulated end beat, and set songRepeatMode
    // to Forever so the transport wraps back automatically at playback
    // time. (See task #91.)
    //
    // Loop points that target order 0 map exactly to the songRepeat
    // behavior (wrap back to beat 0). Loop points that target a later
    // order still wrap to beat 0, which means the intro replays on each
    // cycle — a small fidelity loss that affects very few songs. If we
    // ever want to fix that perfectly we can set the user-region loop
    // (loopEnabled / loopStartBeat / loopEndBeat) instead.
    std::vector<bool> orderVisited(numOrders, false);
    std::vector<float> orderStartBeat(numOrders, 0.0f);
    int wholeSongLoopTarget = -1;

    while (order < numOrders) {
        if (orderVisited[order]) {
            wholeSongLoopTarget = order;
            break;
        }
        orderVisited[order] = true;
        orderStartBeat[order] = currentBeat;

        int pattern = openmpt_module_get_order_pattern(mod, order);
        if (pattern < 0 || pattern >= numPatterns) { ++order; continue; }
        int numRows = openmpt_module_get_pattern_num_rows(mod, pattern);

        int startRow = (forcedNextRow >= 0) ? forcedNextRow : 0;
        forcedNextRow = -1;

        // Pattern loop state (single global per pattern, OpenMPT semantics).
        int loopStartRow = startRow;
        int loopsRemaining = 0;
        bool loopActive = false;

        bool jumped = false;
        int row = startRow;

        while (row < numRows && !jumped) {
            patternDelayRows = 0;
            speedSet = -1;
            tempoSetThisRow = false;
            posJumpTarget = -1;
            patternBreakRow = -1;
            int rowLoopBackTarget = -1;   // set if this row contains an E6N / SBN
            int rowLoopSetStart = -1;     // set if this row contains an E60 / SB0

            for (int ch = 0; ch < numChannels; ++ch) {
                uint8_t note    = openmpt_module_get_pattern_row_channel_command(mod, pattern, row, ch, OPENMPT_MODULE_COMMAND_NOTE);
                uint8_t inst    = openmpt_module_get_pattern_row_channel_command(mod, pattern, row, ch, OPENMPT_MODULE_COMMAND_INSTRUMENT);
                uint8_t volType = openmpt_module_get_pattern_row_channel_command(mod, pattern, row, ch, OPENMPT_MODULE_COMMAND_VOLUMEEFFECT);
                uint8_t volVal  = openmpt_module_get_pattern_row_channel_command(mod, pattern, row, ch, OPENMPT_MODULE_COMMAND_VOLUME);
                uint8_t param   = openmpt_module_get_pattern_row_channel_command(mod, pattern, row, ch, OPENMPT_MODULE_COMMAND_PARAMETER);

                int effectiveSlot = chState[ch].currentSample;
                if (inst > 0 && (int)inst <= numSlots) {
                    effectiveSlot = inst;
                    chState[ch].currentSample = inst;
                    // In instrument mode, picking up a new instrument
                    // also sets the channel's default NNA from the
                    // instrument header — unless an explicit S73-S76
                    // row command has overridden it in the meantime.
                    // (We don't track per-channel "was overridden" —
                    // the override persists implicitly by being set
                    // every time the row command fires.)
                    if (instrumentMode && inst <= (int)parsedFile.instruments.size() - 1) {
                        auto n = parsedFile.instruments[inst].newNoteAction;
                        chState[ch].newNoteAction = (NNA)(int)n;
                    }
                }

                // ---- note column ----
                if (note >= 1 && note <= 120) {
                    // Honor the channel's NNA: with NNA=Cut (the default
                    // and the only option in MOD/S3M), trim the previous
                    // note at the new note's start beat. With Continue /
                    // Off / Fade, leave the previous note ringing — the
                    // Sampler's voice envelope will handle release.
                    if (chState[ch].newNoteAction == NNA::Cut)
                        trimNote(chState[ch].lastNote, currentBeat);

                    float chPan = channelDefaultPan[ch];
                    int trackId = getOrCreateTrack(effectiveSlot, chPan);
                    if (auto* tn = graph.findNode(trackId)) {
                        if (!tn->clips.empty()) {
                            auto& clip = tn->clips[0];
                            int midiPitch = note - 1;
                            MidiNote nn;
                            nn.offset = currentBeat - clip.startBeat;
                            nn.pitch = std::min(127, midiPitch);
                            nn.duration = beatsPerRow;
                            int rawVel = 127;
                            // Volume column: VOLCMD_VOLUME == 1 (set volume).
                            // Other vol-col commands (slide etc.) are not yet
                            // mapped — treat them as no-ops for velocity.
                            if (volType == 1 && volVal <= 64)
                                rawVel = (int)(volVal * 2);
                            // Bake the running global volume into velocity
                            // so global-volume automation from Vxx/Wxx/
                            // Gxx/Hxx affects perceived loudness over time
                            // without needing a live global-volume node.
                            nn.velocity = std::max(1,
                                std::min(127, (int)std::round(rawVel * globalVolume)));
                            // Apply standing per-channel finetune.
                            nn.detune = chState[ch].finetuneCents;

                            clip.notes.push_back(nn);
                            chState[ch].lastNote = {effectiveSlot,
                                                     (int)clip.notes.size() - 1,
                                                     trackId};
                            // Track which sampler this channel is using
                            // so pan/effect automation routes to the right node.
                            float epan = effectivePanFor(effectiveSlot, chPan);
                            int pk = panKeyFor(epan);
                            auto sit = samplerByPan.find({effectiveSlot, pk});
                            chState[ch].activeSamplerNodeId =
                                (sit != samplerByPan.end()) ? sit->second : 0;
                            result.numNotes++;

                            float noteEnd = nn.offset + nn.duration;
                            if (noteEnd > clip.lengthBeats)
                                clip.lengthBeats = std::ceil(noteEnd / 4.0f) * 4.0f;
                        }
                    }
                }
                else if (note == 255 || note == 254) {
                    trimNote(chState[ch].lastNote, currentBeat);
                }

                // ---- volume column non-set-volume effects ----
                // (vol slide / pan / vibrato in vol col — basic coverage)
                switch (volType) {
                    case 3: // VOLCMD_VOLSLIDEUP
                        if (auto* nn = resolveLast(chState[ch].lastNote))
                            nn->velocity = std::min(127, nn->velocity + volVal * 4);
                        break;
                    case 4: // VOLCMD_VOLSLIDEDOWN
                        if (auto* nn = resolveLast(chState[ch].lastNote))
                            nn->velocity = std::max(1, nn->velocity - volVal * 4);
                        break;
                    case 5: // VOLCMD_FINEVOLUP
                        applyFineVolUp(ch, volVal); break;
                    case 6: // VOLCMD_FINEVOLDOWN
                        applyFineVolDown(ch, volVal); break;
                    case 11: // VOLCMD_TONEPORTAMENTO
                        applyToneSlide(ch); break;
                    case 12: // VOLCMD_PORTAUP
                        applyPortaUp(ch, volVal); break;
                    case 13: // VOLCMD_PORTADOWN
                        applyPortaDown(ch, volVal); break;
                    default: break;
                }

                // ---- effect column ----
                // Read the format-specific letter via libopenmpt's
                // formatter. The first character is the effect letter
                // (or empty if no effect on this row+channel).
                const char* fxStr = openmpt_module_format_pattern_row_channel_command(
                    mod, pattern, row, ch, OPENMPT_MODULE_COMMAND_EFFECT);
                char letter = (fxStr && fxStr[0] && fxStr[0] != ' ' && fxStr[0] != '.')
                                ? fxStr[0] : 0;

                // Pattern loop intercept — needs to mutate the row loop,
                // not just touch a note. We look directly at letter+param
                // before the per-format dispatch.
                bool isLoopEffect = false;
                int loopParam = -1;
                if (fmt == Fmt::Mod || fmt == Fmt::Xm) {
                    if (letter == 'E' && (param >> 4) == 0x6) {
                        isLoopEffect = true;
                        loopParam = param & 0x0F;
                    }
                } else if (fmt == Fmt::S3m || fmt == Fmt::It) {
                    if (letter == 'S' && (param >> 4) == 0xB) {
                        isLoopEffect = true;
                        loopParam = param & 0x0F;
                    }
                }
                if (isLoopEffect) {
                    if (loopParam == 0)
                        rowLoopSetStart = row;
                    else
                        rowLoopBackTarget = loopParam;
                    // Don't dispatch as a normal extended effect.
                } else if (letter) {
                    dispatchEffect(ch, letter, param, currentSpeed);
                }
            } // for ch

            // Apply this row's speed change before computing row duration.
            // Speed affects how fast rows advance: fewer ticks = faster rows.
            // Scale beatsPerRow so notes are placed at the correct wall-clock
            // positions relative to the fixed transport BPM.
            if (speedSet >= 0) {
                currentSpeed = (float)speedSet;
                beatsPerRow = currentSpeed / (4.0f * initialSpeedF);
            }

            // Advance the wall-clock by one row, plus any pattern delay.
            currentBeat += beatsPerRow * (1 + patternDelayRows);

            // Pattern-loop bookkeeping for this row.
            if (rowLoopSetStart >= 0) {
                loopStartRow = rowLoopSetStart;
                loopActive = false;
            }
            if (rowLoopBackTarget > 0) {
                if (!loopActive) {
                    loopActive = true;
                    loopsRemaining = rowLoopBackTarget;
                }
                if (loopsRemaining > 0) {
                    --loopsRemaining;
                    if (loopsRemaining > 0) {
                        // Re-walk from loopStartRow. currentBeat keeps
                        // advancing so the looped notes get placed at
                        // distinct beats — i.e. the loop is unrolled
                        // inline as duplicated note data.
                        row = loopStartRow;
                        continue;
                    } else {
                        loopActive = false;
                    }
                }
            }

            // Position jump (Bxx) — switch to a different order after this row.
            if (posJumpTarget >= 0 && posJumpTarget < numOrders) {
                order = posJumpTarget - 1;   // ++order at end of outer while
                jumped = true;
                continue;
            }
            // Pattern break (Dxx) — jump to the next order at a specified row.
            if (patternBreakRow >= 0) {
                forcedNextRow = patternBreakRow;
                jumped = true;
                continue;
            }

            ++row;
        }

        ++order;
    }

    // Close any reverb regions that were still open when the song ended.
    for (int ch = 0; ch < numChannels; ++ch)
        closeReverbRegion(ch, currentBeat);

    // ------------------------------------------------------------------
    // Sound-control wiring: if any slot accumulated reverb regions,
    // create a shared Reverb node and wire each affected Sampler to it
    // via a gated send link. The Reverb node is configured as a pure
    // wet send (Mix = 1.0) — the dry path is the existing Sampler →
    // Master Out link that's already in the graph. Audio flows through
    // the send link only during the beat ranges where the tracker's
    // S99 (reverb on) command was active.
    // ------------------------------------------------------------------
    {
        bool anyReverb = false;
        for (int s = 0; s < (int)reverbRegions.size(); ++s)
            if (!reverbRegions[s].empty()) { anyReverb = true; break; }

        if (anyReverb) {
            // Create the shared Reverb node.
            auto& rvNode = graph.addNode("Reverb Send", NodeType::Effect,
                {Pin{0, "Audio In", PinKind::Audio, true}},
                {Pin{0, "Audio Out", PinKind::Audio, false}},
                Vec2{posX + 700, posY + 10});
            rvNode.script = "__reverb__";
            rvNode.params.push_back({"Mix",       1.0f,  0.0f, 1.0f});  // full wet
            rvNode.params.push_back({"Size",      0.5f,  0.0f, 1.0f});
            rvNode.params.push_back({"Damping",   0.5f,  0.0f, 1.0f});
            rvNode.params.push_back({"Width",     1.0f,  0.0f, 1.0f});
            rvNode.params.push_back({"Pre-Delay", 10.0f, 0.0f, 200.0f});
            graph.addToGroup(groupId, rvNode.id);
            int rvNodeId = rvNode.id;

            // Wire Reverb → Master Out.
            wireToMasterOut(rvNodeId);

            // For each slot with reverb regions: wire ALL pan variants
            // of that Sampler → Reverb, and attach EffectRegions on
            // the corresponding track node.
            for (int s = 0; s < (int)reverbRegions.size(); ++s) {
                auto& regions = reverbRegions[s];
                if (regions.empty()) continue;
                // Wire every pan variant of this slot.
                for (auto& [key, sampId] : samplerByPan) {
                    if (key.first != s) continue;
                    auto* sampNode = graph.findNode(sampId);
                    auto* rvn      = graph.findNode(rvNodeId);
                    if (!sampNode || !rvn || sampNode->pinsOut.empty() || rvn->pinsIn.empty())
                        continue;
                    graph.addLink(sampNode->pinsOut[0].id, rvn->pinsIn[0].id);
                    int linkId = -1;
                    if (!graph.links.empty())
                        linkId = graph.links.back().id;
                    if (linkId < 0) continue;
                    // Find the matching track for this pan variant.
                    auto tit = trackByPan.find(key);
                    if (tit != trackByPan.end()) {
                        auto* trkNode = graph.findNode(tit->second);
                        if (trkNode) {
                            for (auto& br : regions) {
                                EffectRegion er;
                                er.linkId    = linkId;
                                er.startBeat = br.startBeat;
                                er.endBeat   = br.endBeat;
                                er.color     = 0xFF4488FF;
                                trkNode->effectRegions.push_back(er);
                            }
                        }
                    }
                }
            }
        }
    }

    // Extend each track's clip to cover the full song length.
    float songLengthBeatsOut = std::ceil(currentBeat / 4.0f) * 4.0f;
    for (auto& [key, nodeId] : trackByPan) {
        if (auto* tn = graph.findNode(nodeId))
            if (!tn->clips.empty())
                tn->clips[0].lengthBeats = std::max(tn->clips[0].lengthBeats,
                                                     songLengthBeatsOut);
    }

    // Sort every sampler param's automation lane by beat. Pan commands
    // and panbrello curves were pushed in order of encounter, but if the
    // order-list or pattern-break path jumped around, that's not
    // guaranteed to be sorted. AutomationLane::evaluate() assumes
    // ascending beats.
    for (auto& [key, nodeId] : samplerByPan) {
        if (auto* sn = graph.findNode(nodeId)) {
            for (auto& p : sn->params)
                if (!p.automation.points.empty())
                    std::sort(p.automation.points.begin(),
                              p.automation.points.end(),
                              [](const AutomationPoint& a, const AutomationPoint& b) {
                                  return a.beat < b.beat;
                              });
        }
    }

    // Whole-song loop detection: if the pattern walk ended because a Bxx
    // position jump landed on an already-visited order, set the project's
    // Song Length + Repeat so playback loops indefinitely back to beat 0.
    // (See task #91 and the orderVisited bookkeeping above.) This replaces
    // the old "safety counter unrolls the loop N times" hack.
    if (wholeSongLoopTarget >= 0) {
        graph.songLengthBeats  = currentBeat;
        graph.songRepeatMode   = NodeGraph::SongRepeat::Forever;
        graph.songRepeatCount  = 1;
        if (wholeSongLoopTarget != 0) {
            fprintf(stderr,
                "MOD song loop target order = %d (non-zero); loop approximates "
                "as wrap-to-beat-0 so the intro will replay each iteration. "
                "Use Song Length + Repeat to fine-tune if needed.\n",
                wholeSongLoopTarget);
        }
    }

    // ------------------------------------------------------------------
    // Layout pass: reposition all imported nodes so nothing overlaps.
    // Node dimensions match getNodeBounds() in node_graph_component.cpp:
    //   width = 180, height = 24 + max(numRows, 1) * 20 + 8
    // ------------------------------------------------------------------
    {
        constexpr float kNodeW = 180.0f;
        constexpr float kGap   = 20.0f;
        auto nodeH = [](const Node& n) -> float {
            int rows = std::max((int)n.pinsIn.size(), (int)n.pinsOut.size());
            rows += (int)n.params.size();
            return 24.0f + std::max(rows, 1) * 20.0f + 8.0f;
        };

        // Collect IDs of all nodes created by this import.
        // The group node's childNodeIds list catches everything including
        // the Reverb Send node.
        std::vector<bool> isOurs(graph.getNextId() + 1, false);
        isOurs[groupId] = true;
        if (auto* grpNode = graph.findNode(groupId))
            for (int cid : grpNode->childNodeIds)
                if (cid >= 0 && cid < (int)isOurs.size())
                    isOurs[cid] = true;

        // Find a clear Y below all pre-existing nodes (skip Master Out —
        // it stays far to the right and shouldn't push the import down).
        float startY = posY;
        for (auto& n : graph.nodes) {
            if (n.id < (int)isOurs.size() && isOurs[n.id]) continue;
            if (n.type == NodeType::Output) continue;
            float bottom = n.pos.y + nodeH(n);
            startY = std::max(startY, bottom + kGap * 2);
        }

        // Group container at top of the import area.
        float groupH = 0;
        if (auto* grp = graph.findNode(groupId)) {
            grp->pos = {posX, startY};
            groupH = nodeH(*grp);
        }

        float col1X = posX;                    // MIDI track column
        float col2X = posX + kNodeW + 60.0f;   // sampler / instrument column
        float curY  = startY + groupH + kGap;

        // Pre-instrument track (slot 0) if it exists.
        if (trackNodeId[0] != 0) {
            if (auto* trk = graph.findNode(trackNodeId[0])) {
                trk->pos = {col1X, curY};
                curY += nodeH(*trk) + kGap;
            }
        }

        // Collect all positioned node IDs so the catch-all loop skips them.
        std::set<int> positioned;
        if (trackNodeId[0] != 0) positioned.insert(trackNodeId[0]);

        // Track–sampler pairs, grouped by slot, one row per (slot,pan) pair.
        for (int s = 1; s <= numSlots; ++s) {
            for (auto& [key, trkId] : trackByPan) {
                if (key.first != s) continue;
                float rowH = 0;
                if (auto* trk = graph.findNode(trkId)) {
                    trk->pos = {col1X, curY};
                    rowH = std::max(rowH, nodeH(*trk));
                    positioned.insert(trkId);
                }
                auto sit = samplerByPan.find(key);
                if (sit != samplerByPan.end()) {
                    if (auto* smp = graph.findNode(sit->second)) {
                        smp->pos = {col2X, curY};
                        rowH = std::max(rowH, nodeH(*smp));
                        positioned.insert(sit->second);
                    }
                }
                curY += rowH + kGap;
            }
        }

        // Any remaining group members (e.g. Reverb Send) go below the pairs.
        if (auto* grpNode = graph.findNode(groupId)) {
            for (int cid : grpNode->childNodeIds) {
                if (cid == groupId) continue;
                if (positioned.count(cid)) continue;
                if (auto* n = graph.findNode(cid)) {
                    n->pos = {col2X, curY};
                    curY += nodeH(*n) + kGap;
                }
            }
        }
    }

    openmpt_module_ext_destroy(modExt);

    int numSamplers = (int)samplerByPan.size();
    int numTracks = (int)trackByPan.size();

    result.success = true;
    result.numSamplesExtracted = numSamplers;
    result.numTracks = numTracks;
    result.sampleDir = sampleDir;

    fprintf(stderr,
        "Tracker imported (%s): %d channels, %d patterns, %d notes, "
        "%d/%d samples extracted, %d MIDI tracks\n",
        (fmt == Fmt::Mod ? "MOD" :
         fmt == Fmt::S3m ? "S3M" :
         fmt == Fmt::It  ? "IT"  :
         fmt == Fmt::Xm  ? "XM"  : "?"),
        numChannels, numPatterns, result.numNotes,
        numSamplers, numSamples, numTracks);

#else
    (void)posX; (void)posY;
    result.error = "libopenmpt not available (compile with HAS_LIBOPENMPT)";
    fprintf(stderr, "Tracker import: %s\n", result.error.c_str());
#endif

    return result;
}

} // namespace SoundShop

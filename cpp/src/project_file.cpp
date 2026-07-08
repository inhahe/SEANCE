#include "project_file.h"
#include "asset_import.h"
#include "warp.h"           // seedBuiltinMorphLibrary, isBuiltinMorphAssetId
#include "spectrum_tap.h"   // resolveSpectrumTapReferences (FrequencyGraph refs)
#include "spectral_editor.h"  // resolveSpectralReferences (FrequencyGraph refs)
#include <juce_core/juce_core.h>
#include <functional>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <set>

namespace SoundShop {

std::string ProjectFile::currentPath;

// Simple text-based project format
// Sections: [Project], [Node], [Pin], [Clip], [Note], [Link], [Param]
//
// All actual I/O goes through the stream-based writeProject/readProject
// helpers - the path-based save()/load() are thin wrappers that open a file
// and delegate. The undo system (#84) reuses the same helpers against
// std::ostringstream / std::istringstream to (de)serialize snapshots in
// memory.

static void writeStr(std::ostream& f, const std::string& key, const std::string& val) {
    f << key << "=" << val << "\n";
}
static void writeInt(std::ostream& f, const std::string& key, int val) {
    f << key << "=" << val << "\n";
}
static void writeFloat(std::ostream& f, const std::string& key, float val) {
    f << key << "=" << val << "\n";
}

// Emit one [AssetStore] section. Shared by writeProject (full save) and
// exportAssets (library export) so the on-disk asset format has a single
// source of truth. The opaque payload is base64-encoded so arbitrary bytes
// (including newlines) survive the line-based format; the content hash is
// written verbatim and trusted on load (mirroring [Blob]).
static void writeAssetEntry(std::ostream& f, const AssetEntry& a) {
    f << "\n[AssetStore]\n";
    writeInt(f, "id", a.id);
    writeStr(f, "kind", assetKindTag(a.kind));
    writeStr(f, "name", a.name);
    if (!a.subType.empty()) writeStr(f, "subType", a.subType);
    writeStr(f, "hash", a.contentHash);
    if (a.archived) writeInt(f, "archived", 1);
    if (a.starred)  writeInt(f, "starred", 1);
    juce::String b64 = juce::Base64::toBase64(a.payload.data(),
                                              (int) a.payload.size());
    writeStr(f, "payload", b64.toStdString());
}

// Collect content-store blob hashes referenced by a node script. A reference is
// the trailing '#<hash>' field of a __generate__ script (and, in future, any
// other blob-backed node tag). Used at save time so writeProject emits only the
// blobs the current graph still references - orphans created by this session's
// edits (each Edit Source bakes a new hash) are pruned from the file. The live
// in-memory store keeps every blob so undo/redo can still resolve old hashes.
static void collectBlobHashes(const std::string& script, std::set<std::string>& out) {
    if (script.rfind("__generate__:", 0) != 0) return;
    size_t bar = script.rfind('|');
    if (bar == std::string::npos || bar + 2 > script.size()) return;
    if (script[bar + 1] != '#') return;
    std::string hash = script.substr(bar + 2);
    if (!hash.empty()) out.insert(hash);
}

bool ProjectFile::save(const std::string& path, NodeGraph& graph, GraphProcessor* gp) {
    std::ofstream f(path);
    if (!f) {
        fprintf(stderr, "Failed to save project: %s\n", path.c_str());
        return false;
    }
    bool ok = writeProject(f, graph, gp);
    if (ok) {
        currentPath = path;
        fprintf(stderr, "Project saved: %s\n", path.c_str());
    }
    return ok;
}

bool ProjectFile::exportAssets(const std::string& path, const AssetLibrary& lib,
                               const std::vector<int>& selectedIds) {
    std::ofstream f(path);
    if (!f) {
        fprintf(stderr, "Failed to export asset library: %s\n", path.c_str());
        return false;
    }
    // A library export is just a minimal project file carrying an [AssetStore]
    // closure - the importer reuses the exact same readProject + merge path it
    // uses for a full session, so there is one import code path, not two. The
    // marker key lets a reader tell an export apart from a full project if needed.
    f << "[Project]\n";
    writeStr(f, "assetLibraryExport", "1");

    // Expand the selection to its dependency closure (identity for leaves today;
    // see assetChildIds). Empty selection = export every entry.
    std::set<int> want;
    if (!selectedIds.empty()) {
        std::function<void(int)> expand = [&](int id) {
            if (want.count(id)) return;
            const AssetEntry* e = lib.find(id);
            if (!e) return;
            want.insert(id);
            for (int child : assetChildIds(*e)) expand(child);
        };
        for (int id : selectedIds) expand(id);
    }

    int count = 0;
    for (const auto& a : lib.all()) {
        if (!selectedIds.empty() && !want.count(a.id)) continue;
        // Built-in morph chains are code-owned and seeded into every project, so
        // exporting them is meaningless boilerplate (the destination already has
        // them). Skip, matching writeProject.
        if (a.kind == AssetKind::MorphAlgorithm && isBuiltinMorphAssetId(a.id))
            continue;
        writeAssetEntry(f, a);
        ++count;
    }
    fprintf(stderr, "Exported %d asset(s) to: %s\n", count, path.c_str());
    return true;
}

bool ProjectFile::writeProject(std::ostream& f, NodeGraph& graph,
                               GraphProcessor* gp, bool includeView,
                               bool includeBlobs) {

    f << "[Project]\n";
    writeFloat(f, "bpm", graph.bpm);
    writeInt(f, "timeSigNum", graph.timeSignatureNum);
    writeInt(f, "timeSigDen", graph.timeSignatureDen);
    if (graph.loopEnabled) {
        writeInt(f, "loopEnabled", 1);
        writeFloat(f, "loopStart", (float)graph.loopStartBeat);
        writeFloat(f, "loopEnd", (float)graph.loopEndBeat);
    }
    if (graph.projectSampleRate > 0)
        writeFloat(f, "projectSampleRate", (float)graph.projectSampleRate);
    // Project-wide settings (#66) - tuning, concert pitch, crossfade.
    if (graph.tuningSystem != TuningSystem::Equal12)
        writeInt(f, "tuningSystem", (int)graph.tuningSystem);
    if (std::abs(graph.concertPitch - 440.0f) > 0.01f)
        writeFloat(f, "concertPitch", graph.concertPitch);
    if (std::abs(graph.globalCrossfadeSec - 0.05f) > 0.001f)
        writeFloat(f, "globalCrossfadeSec", graph.globalCrossfadeSec);
    if (graph.metronomeEnabled)
        writeInt(f, "metronomeEnabled", 1);
    if (graph.songLengthBeats > 0)
        writeFloat(f, "songLengthBeats", (float)graph.songLengthBeats);
    if (graph.songRepeatMode != NodeGraph::SongRepeat::None)
        writeInt(f, "songRepeatMode", (int)graph.songRepeatMode);
    if (graph.songRepeatCount != 1)
        writeInt(f, "songRepeatCount", graph.songRepeatCount);
    if (!graph.historyFilePath.empty())
        writeStr(f, "historyFile", graph.historyFilePath);
    // Saved view state for the node-graph component. Skipped on undo
    // snapshots so undo never moves the user's viewport. Only written
    // when there's a non-default value to restore (viewZoom == 0 means
    // "no saved view, use fit-all").
    if (includeView && graph.viewZoom > 0.0f) {
        writeFloat(f, "viewZoom", graph.viewZoom);
        writeFloat(f, "viewPanX", graph.viewPanX);
        writeFloat(f, "viewPanY", graph.viewPanY);
    }
    // Effect group definitions (#66). Each group is a named bundle of
    // link IDs with an optional per-group crossfade override.
    for (auto& eg : graph.effectGroups) {
        f << "[EffectGroup]\n";
        writeInt(f, "id", eg.id);
        writeStr(f, "name", eg.name);
        writeInt(f, "color", (int)eg.color);
        if (eg.crossfadeSec > 0)
            writeFloat(f, "crossfadeSec", eg.crossfadeSec);
        std::string ids;
        for (int lid : eg.linkIds) {
            if (!ids.empty()) ids += ",";
            ids += std::to_string(lid);
        }
        if (!ids.empty()) writeStr(f, "linkIds", ids);
    }

    // Project-level asset library ("stores"). Each entry is one reusable asset
    // (waveform/generator, instrument, ADHSR curve, or morph algorithm) keyed by
    // a stable user id. Written into undo snapshots too (store edits are project
    // state and must be undoable) - the payloads are encoded bodies, generally
    // small. The opaque payload is base64-encoded so arbitrary content (including
    // newlines) survives the line-based format. The content hash is saved and
    // trusted on load (not recomputed), mirroring [Blob]. Archived (soft-deleted)
    // entries are persisted too so existing references stay resolvable.
    //
    // EXCEPTION: the curated built-in morph chains are code-owned (seeded into
    // every project by seedBuiltinMorphLibrary) and must NOT be written - that
    // keeps project files free of boilerplate and lets the built-ins improve
    // across app versions. They are re-seeded on load, so skipping them here is
    // safe. (Frames never live-reference a built-in - the picker copies a
    // built-in to an Independent chain - so this never orphans a reference.)
    for (const auto& a : graph.assets.all()) {
        if (a.kind == AssetKind::MorphAlgorithm && isBuiltinMorphAssetId(a.id))
            continue;
        writeAssetEntry(f, a);
    }

    writeInt(f, "nextId", 0);
    if (!graph.signalScript.empty()) {
        // Encode signal script with line count prefix so we know where it ends
        auto lines = juce::StringArray::fromLines(graph.signalScript);
        writeInt(f, "signalScriptLines", lines.size());
        for (auto& line : lines)
            f << line.toStdString() << "\n";
    }

    // Find max ID for nextId
    int maxId = 0;
    for (auto& n : graph.nodes) {
        maxId = std::max(maxId, n.id);
        for (auto& p : n.pinsIn) maxId = std::max(maxId, p.id);
        for (auto& p : n.pinsOut) maxId = std::max(maxId, p.id);
    }
    for (auto& l : graph.links) maxId = std::max(maxId, l.id);

    // Save nodes
    for (auto& node : graph.nodes) {
        f << "\n[Node]\n";
        writeInt(f, "id", node.id);
        writeStr(f, "name", node.name);
        writeInt(f, "type", (int)node.type);
        writeFloat(f, "posX", node.pos.x);
        writeFloat(f, "posY", node.pos.y);
        if (node.muted) writeInt(f, "muted", 1);
        if (node.soloed) writeInt(f, "soloed", 1);
        // Automation record override (record axis) + wholesale ignore (read
        // axis) for this node. Only emitted when non-default. armMode absent
        // => Inherit; ignoreAuto absent => false.
        if (node.armMode != AutoArmMode::Inherit) writeInt(f, "armMode", (int)node.armMode);
        if (node.ignoreAutomation) writeInt(f, "ignoreAuto", 1);
        // node.script can be multi-line for some node types - most notably
        // MultiSampler, whose encode() produces dozens of lines (one per
        // zone field). The old `writeStr(f, "script", val)` form wrote
        // `script=<val>\n` raw, so any `\n` inside val terminated the
        // line and the load-side `getline` recovered only the first line
        // (just the `__multisampler__:` prefix). On round-trip this
        // wiped the entire MultiSampler payload, producing silent MOD
        // playback after restart. The fix: detect multi-line scripts and
        // write them in a length-prefixed `scriptLines=N\n<line>...` form,
        // matching the existing pattern used for `signalScript` above.
        if (node.script.find('\n') != std::string::npos) {
            auto lines = juce::StringArray::fromLines(node.script);
            // Trim a possible empty trailing line so round-trips are stable
            // (StringArray::fromLines yields one extra empty element when
            // the source string ends with '\n').
            while (!lines.isEmpty() && lines.getReference(lines.size() - 1).isEmpty())
                lines.remove(lines.size() - 1);
            writeInt(f, "scriptLines", lines.size());
            for (auto& ln : lines)
                f << ln.toStdString() << "\n";
        } else {
            writeStr(f, "script", node.script);
        }
        if (!node.midiInputSourceId.empty())
            writeStr(f, "midiInputSourceId", node.midiInputSourceId);
        // Shared AHDSR envelope. One line — encode() returns a single line with
        // no newlines and length-prefixed curve fields, so it survives writeStr
        // round-trip regardless of curve content. Saved unconditionally because
        // it carries sensible defaults (linear ramps, 5ms / 0ms / 200ms / 0.7 /
        // 300ms / velSens=1) — we want those defaults to persist exactly across
        // save/load rather than relying on the constructor at load time.
        writeStr(f, "ahdsrEnvelope", node.ahdsrEnvelope.encode());
        // Live reference to a project asset-library AHDSR curve (-1 = none).
        if (node.ahdsrAssetId >= 0) writeInt(f, "ahdsrAssetId", node.ahdsrAssetId);
        // Additional per-component AHDSR envelopes (FM operators etc.). Saved
        // only when present so non-FM nodes stay clean. Count first, then one
        // encoded line per envelope keyed opEnvelope0..N.
        if (!node.opEnvelopes.empty()) {
            writeInt(f, "opEnvelopeCount", (int)node.opEnvelopes.size());
            for (size_t i = 0; i < node.opEnvelopes.size(); ++i)
                writeStr(f, "opEnvelope" + std::to_string(i),
                         node.opEnvelopes[i].encode());
        }
        if (node.aftertouchSensitivity != 0.5f)
            writeFloat(f, "aftertouchSensitivity", node.aftertouchSensitivity);
        writeInt(f, "pluginIndex", node.pluginIndex);
        if (node.panLaw != PanLaw::EqualPower) writeInt(f, "panLaw", (int)node.panLaw);
        if (node.pan != 0.0f) writeFloat(f, "pan", node.pan);
        if (node.spatialX != 0.0f) writeFloat(f, "spatialX", node.spatialX);
        if (node.spatialY != 0.0f) writeFloat(f, "spatialY", node.spatialY);
        if (node.spatialZ != 0.0f) writeFloat(f, "spatialZ", node.spatialZ);
        // Save plugin state as base64. Per-plugin dirty tracking (#86):
        // only call getStateInformation when the cache is stale, otherwise
        // reuse the cached base64 string. This avoids the expensive query
        // for plugins whose parameters haven't changed since the last
        // save - typical case is most plugins have nothing to re-query.
        // ProjectFile::save (the user-facing save path, gp != nullptr) and
        // the slow autosave path both share this cache.
        if (gp && node.pluginIndex >= 0) {
            if (node.pluginStateDirty || node.cachedPluginStateBase64.empty()) {
                auto* proc = gp->getProcessorForNode(node.id);
                if (proc) {
                    juce::MemoryBlock stateData;
                    proc->getStateInformation(stateData);
                    if (stateData.getSize() > 0) {
                        node.cachedPluginStateBase64 =
                            stateData.toBase64Encoding().toStdString();
                        node.pluginStateDirty = false;
                    }
                }
            }
            if (!node.cachedPluginStateBase64.empty())
                writeStr(f, "pluginState", node.cachedPluginStateBase64);
            else if (!node.pendingPluginState.empty())
                // Plugin hasn't finished loading yet (async load in progress), so
                // there's no live processor to query and no cached state. Fall
                // back to the state we read from the file but haven't applied
                // yet - otherwise an autosave mid-load would silently drop the
                // plugin's saved state. See MainContentComponent::beginAsyncPluginLoad.
                writeStr(f, "pluginState", node.pendingPluginState);
        }
        if (node.performanceMode) {
            writeInt(f, "performanceMode", 1);
            writeInt(f, "perfReleaseMode", node.performanceReleaseMode);
            writeInt(f, "perfVelocity", node.performanceVelocity ? 1 : 0);
        }
        // Oscilloscope display settings - written only when non-default to keep
        // files lean (only __oscilloscope__ nodes ever change these from default).
        if (!node.scopeTriggered) writeInt(f, "scopeTriggered", 0);
        if (node.scopeTrigLevel != 0.0f) writeFloat(f, "scopeTrigLevel", node.scopeTrigLevel);
        if (!node.scopeTrigRising) writeInt(f, "scopeTrigRising", 0);
        if (node.mpeEnabled) {
            writeInt(f, "mpeEnabled", 1);
            writeInt(f, "mpePitchBendRange", node.mpePitchBendRange);
        }
        writeInt(f, "parentGroupId", node.parentGroupId);
        writeFloat(f, "groupBeatOffset", node.groupBeatOffset);
        if (!node.anchorMarker.empty())
            writeStr(f, "anchorMarker", node.anchorMarker);
        writeInt(f, "groupExpanded", node.groupExpanded ? 1 : 0);
        // Voice container (per-voice polyphony) - see poly-voice-architecture.md
        if (node.voiceContainerId != -1)
            writeInt(f, "voiceContainerId", node.voiceContainerId);
        if (node.type == NodeType::VoiceContainer) {
            writeInt(f, "voicePolyphony", node.voicePolyphony);
            writeInt(f, "voiceStealMode", node.voiceStealMode);
            writeFloat(f, "voiceGlideMs", node.voiceGlideMs);
            writeInt(f, "voiceUnison", node.voiceUnison);
            writeFloat(f, "voiceUnisonDetune", node.voiceUnisonDetune);
            writeFloat(f, "voiceUnisonSpread", node.voiceUnisonSpread);
        }
        // Save child IDs as comma-separated
        if (!node.childNodeIds.empty()) {
            std::string ids;
            for (int i = 0; i < (int)node.childNodeIds.size(); ++i) {
                if (i > 0) ids += ",";
                ids += std::to_string(node.childNodeIds[i]);
            }
            writeStr(f, "childNodeIds", ids);
        }
        // MOD-import song-setting restore stash (only meaningful on the
        // import's root group node; harmless/absent on others).
        if (node.modImportSavedSong) {
            writeInt(f, "modImportSavedSong", 1);
            writeInt(f, "modImportPrevRepeatMode", node.modImportPrevRepeatMode);
            writeInt(f, "modImportPrevRepeatCount", node.modImportPrevRepeatCount);
            writeFloat(f, "modImportPrevSongLength", (float)node.modImportPrevSongLength);
            writeInt(f, "modImportPrevLoopEnabled", node.modImportPrevLoopEnabled ? 1 : 0);
            writeFloat(f, "modImportPrevLoopStart", (float)node.modImportPrevLoopStart);
            writeFloat(f, "modImportPrevLoopEnd", (float)node.modImportPrevLoopEnd);
        }
        // Audio cache / freeze state. The auto-cache toggle is a user preference
        // (default true) so persist it whenever it's off, regardless of whether
        // a cache exists. The freeze payload itself (enabled/valid/on-disk PCM)
        // is only persisted on real saves - undo snapshots pass includeBlobs
        // =false and must not carry freeze state (reparsing a snapshot must not
        // touch the disk cache). The PCM lives in a sibling soundshop_cache/
        // file named by node id; here we persist only the metadata needed to
        // re-attach it on load (see rehydrateNodeCaches). saveProject runs
        // saveToDisk before this, so a frozen node arrives here with
        // useDisk=true and its buffers already flushed.
        if (!node.cache.autoCache) writeInt(f, "cacheAuto", 0);
        // Require useDisk: only freezes actually backed by a soundshop_cache/
        // file are re-attachable on load. This also excludes the Output node's
        // memory-only stop-capture cache (never flushed to disk - see
        // saveProject), which is transient session state, not a saved freeze.
        if (includeBlobs && node.cache.hasCachedAudio() && node.cache.useDisk) {
            writeInt(f, "cacheEnabled", node.cache.enabled ? 1 : 0);
            writeInt(f, "cacheValid", 1);
            writeInt(f, "cacheUseDisk", node.cache.useDisk ? 1 : 0);
            writeStr(f, "cacheHash", std::to_string(node.cache.inputHash));
            writeFloat(f, "cacheSampleRate", (float)node.cache.sampleRate);
            writeStr(f, "cacheStartSample", std::to_string(node.cache.startSample));
            writeStr(f, "cacheNumSamples", std::to_string(node.cache.numSamples));
        }

        for (auto& pin : node.pinsIn) {
            f << "[PinIn]\n";
            writeInt(f, "id", pin.id);
            writeStr(f, "name", pin.name);
            writeInt(f, "kind", (int)pin.kind);
            writeInt(f, "channels", pin.channels);
        }
        for (auto& pin : node.pinsOut) {
            f << "[PinOut]\n";
            writeInt(f, "id", pin.id);
            writeStr(f, "name", pin.name);
            writeInt(f, "kind", (int)pin.kind);
            writeInt(f, "channels", pin.channels);
        }
        for (auto& param : node.params) {
            f << "[Param]\n";
            writeStr(f, "name", param.name);
            writeFloat(f, "value", param.modulated ? param.baseValue : param.value);
            writeFloat(f, "min", param.minVal);
            writeFloat(f, "max", param.maxVal);
            writeStr(f, "format", param.format);
            // Warp-slot key (unified warp/morph). Only emitted for warp params so
            // ordinary params stay unchanged; absent => -1 (not a warp param).
            if (param.warpSlot >= 0) writeInt(f, "warpSlot", param.warpSlot);
            // Warp scope: which chain the slot indexes. Only emitted for per-
            // layer warp params (>=0); absent => -1 (frame-scope / not a warp).
            // Also used as the layer index for layer-field (phase/amp) params.
            if (param.warpLayer >= 0) writeInt(f, "warpLayer", param.warpLayer);
            // Per-layer field key (0=phase, 1=amplitude). Only emitted for
            // layer-field modulation params; absent => -1 (not a layer field).
            if (param.layerField >= 0) writeInt(f, "layerField", param.layerField);
            // Owning wavetable frame (library id) for warp / layer-field params.
            // Only emitted when set (>=0); absent => -1 (legacy whole-node, the
            // pre-per-frame layout). Lets two frames carry the same warp op
            // without their modulation params colliding on (warpLayer,warpSlot).
            if (param.warpFrameId >= 0) writeInt(f, "warpFrameId", param.warpFrameId);
            // Automation record override (record axis) + lane bypass (read axis).
            // Only emitted when non-default so ordinary params round-trip
            // unchanged. armMode absent => Inherit; bypassAuto absent => false.
            if (param.armMode != AutoArmMode::Inherit) writeInt(f, "armMode", (int)param.armMode);
            if (param.bypassAutomation) writeInt(f, "bypassAuto", 1);
            for (auto& ap : param.automation.points)
                f << "auto=" << ap.beat << "," << ap.value << "\n";
        }
        // Signal modulation pin bindings (#88). The pins themselves are
        // already serialized in the [PinIn] block above; the modPin
        // entries just record which param each one drives.
        for (auto& mp : node.modPins) {
            f << "modPin=" << mp.paramIndex << "," << mp.pinId
              << "," << mp.depth
              << "," << (mp.mode == Node::ModPin::Mode::Absolute ? 1 : 0) << "\n";
        }
        // Recorded automation for hosted-plugin params (node-scope, since plugin
        // params have no [Param] block). One line per point: pluginAuto=<paramIdx>,
        // <beat>,<normValue>. Values are normalized 0..1. Emitted only for lanes
        // that actually hold points, so plain plugin nodes stay unchanged.
        for (auto& kv : node.pluginParamAutomation) {
            for (auto& ap : kv.second.points)
                f << "pluginAuto=" << kv.first << "," << ap.beat << "," << ap.value << "\n";
        }
        for (auto& clip : node.clips) {
            f << "[Clip]\n";
            writeStr(f, "name", clip.name);
            writeFloat(f, "start", clip.startBeat);
            writeFloat(f, "length", clip.lengthBeats);
            writeInt(f, "color", (int)clip.color);
            writeInt(f, "channels", clip.channels);
            writeInt(f, "waveformView", clip.waveformView);
            writeInt(f, "clipKeyRoot", clip.keyRoot);
            writeStr(f, "clipKeyType", clip.keyType);
            writeInt(f, "hasCustomKey", clip.hasCustomKey ? 1 : 0);
            for (auto& note : clip.notes) {
                f << "[Note]\n";
                writeFloat(f, "offset", note.offset);
                writeInt(f, "pitch", note.pitch);
                writeFloat(f, "duration", note.duration);
                writeInt(f, "velocity", note.velocity);
                writeInt(f, "degree", note.degree);
                writeInt(f, "octave", note.octave);
                writeInt(f, "chromatic", note.chromaticOffset);
                writeFloat(f, "detune", note.detune);
                if (note.exactOffset.den > 0) {
                    writeInt(f, "exactOffsetNum", note.exactOffset.num);
                    writeInt(f, "exactOffsetDen", note.exactOffset.den);
                }
                if (note.exactDuration.den > 0) {
                    writeInt(f, "exactDurNum", note.exactDuration.num);
                    writeInt(f, "exactDurDen", note.exactDuration.den);
                }
                // MPE expression curves
                for (auto& p : note.expression.pitchBend)
                    f << "exPB=" << p.time << "," << p.value << "\n";
                for (auto& p : note.expression.slide)
                    f << "exSL=" << p.time << "," << p.value << "\n";
                for (auto& p : note.expression.pressure)
                    f << "exPR=" << p.time << "," << p.value << "\n";
            }
            for (auto& cc : clip.ccEvents) {
                f << "[CC]\n";
                writeFloat(f, "offset", cc.offset);
                writeInt(f, "controller", cc.controller);
                writeInt(f, "value", cc.value);
                writeInt(f, "channel", cc.channel);
            }
            // Audio clip fields
            if (!clip.audioFilePath.empty())
                writeStr(f, "audioFile", clip.audioFilePath);
            writeFloat(f, "slipOffset", clip.slipOffset);
            writeFloat(f, "fadeIn", clip.fadeInBeats);
            writeFloat(f, "fadeOut", clip.fadeOutBeats);
            writeFloat(f, "gain", clip.gainDb);
        }

        // Take lanes
        for (auto& lane : node.takeLanes) {
            f << "[TakeLane]\n";
            writeStr(f, "name", lane.name);
            writeFloat(f, "timeOffset", lane.timeOffsetSamples);
            writeInt(f, "muted", lane.muted ? 1 : 0);
            for (auto& clip : lane.clips) {
                f << "[TakeClip]\n";
                writeStr(f, "name", clip.name);
                writeFloat(f, "start", clip.startBeat);
                writeFloat(f, "length", clip.lengthBeats);
                writeInt(f, "color", (int)clip.color);
                writeInt(f, "channels", clip.channels);
                if (!clip.audioFilePath.empty())
                    writeStr(f, "audioFile", clip.audioFilePath);
                writeFloat(f, "slipOffset", clip.slipOffset);
                writeFloat(f, "gain", clip.gainDb);
            }
        }

        // Comp segments
        for (auto& comp : node.compSegments) {
            f << "[Comp]\n";
            writeFloat(f, "start", comp.startBeat);
            writeFloat(f, "end", comp.endBeat);
            writeInt(f, "lane", comp.takeLaneIdx);
            writeFloat(f, "crossfade", comp.crossfadeBeats);
        }
    }

    // Save links
    for (auto& link : graph.links) {
        f << "\n[Link]\n";
        writeInt(f, "id", link.id);
        writeInt(f, "startPin", link.startPin);
        writeInt(f, "endPin", link.endPin);
        if (link.gainDb != 0.0f)
            writeFloat(f, "gain", link.gainDb);
    }

    // Save markers
    for (auto& m : graph.markers) {
        f << "\n[Marker]\n";
        writeInt(f, "id", m.id);
        writeStr(f, "name", m.name);
        writeFloat(f, "beat", m.beat);
        writeInt(f, "color", (int)m.color);
    }

    // Save CC mappings
    for (auto& m : graph.ccMappings) {
        f << "\n[CCMap]\n";
        writeInt(f, "midiCh", m.midiCh);
        writeInt(f, "ccNum", m.ccNum);
        writeInt(f, "nodeId", m.nodeId);
        writeInt(f, "paramIdx", m.paramIdx);
    }

    // Save waveform library
    for (auto& wf : graph.waveformLibrary) {
        f << "\n[Waveform]\n";
        writeStr(f, "name", wf.name);
        if (!wf.expression.empty())
            writeStr(f, "expression", wf.expression);
        for (auto& pt : wf.points)
            f << "point=" << pt.first << "," << pt.second << "\n";
    }

    // Save open editors
    if (!graph.openEditors.empty()) {
        f << "\n[Editors]\n";
        std::string ids;
        for (int i = 0; i < (int)graph.openEditors.size(); ++i) {
            if (i > 0) ids += ",";
            ids += std::to_string(graph.openEditors[i]);
        }
        writeStr(f, "openEditors", ids);
        writeInt(f, "activeEditor", graph.activeEditorNodeId);
    }

    // Content-addressed blob store (baked terrain grids; later decoded video /
    // wavetable PCM). Written on real saves but NOT into undo snapshots
    // (serializeForUndo passes includeBlobs=false): the snapshot carries the
    // hash inside node.script, and on undo/redo the bytes come from the live
    // in-memory store. Only emit blobs the current graph references, pruning
    // session orphans (each Edit Source bakes a new hash; the old one is dropped
    // from the file but kept in memory for undo).
    if (includeBlobs) {
        std::set<std::string> referenced;
        for (auto& node : graph.nodes)
            collectBlobHashes(node.script, referenced);
        const auto& entries = graph.contentStore.entries();
        for (const auto& h : referenced) {
            auto it = entries.find(h);
            if (it == entries.end()) continue;   // dangling ref - nothing to write
            f << "\n[Blob]\n";
            writeStr(f, "hash", h);
            juce::String b64 = juce::Base64::toBase64(it->second.compressed.data(),
                                                      (int) it->second.compressed.size());
            writeStr(f, "bytes", b64.toStdString());
        }
    }

    f << "\n[End]\n";
    return true;
}

bool ProjectFile::load(const std::string& path, NodeGraph& graph, PluginHost* pluginHost) {
    std::ifstream f(path);
    if (!f) {
        fprintf(stderr, "Failed to load project: %s\n", path.c_str());
        return false;
    }
    // Real file open replaces the whole project, so drop the previous project's
    // baked blobs before readProject repopulates from the file's [Blob] sections.
    // (Undo restore goes through loadFromString, which must NOT clear the store -
    // its snapshots carry no blobs, the bytes live in memory across undo/redo.)
    graph.contentStore.clear();
    bool ok = readProject(f, graph, pluginHost);
    if (ok) {
        currentPath = path;
        fprintf(stderr, "Project loaded: %s (%d nodes, %d links)\n",
                path.c_str(), (int)graph.nodes.size(), (int)graph.links.size());
    }
    return ok;
}

bool ProjectFile::readProject(std::istream& f, NodeGraph& graph, PluginHost* pluginHost) {
    // Clear existing
    graph.nodes.clear();
    graph.links.clear();
    graph.openEditors.clear();
    // The loop region is only serialized when enabled (writeProject omits the
    // keys otherwise), so reset it here: parsing a snapshot or file that has no
    // loop keys must yield "no loop", not inherit a stale loop from the graph's
    // prior state. Without this, undo/redo of a "disable loop" edit wouldn't
    // stick (the redo snapshot has no keys, so loopEnabled would stay true).
    graph.loopEnabled = false;
    graph.loopStartBeat = 0;
    graph.loopEndBeat = 0;

    std::vector<int> pendingEditorIds;
    int pendingActiveEditorId = -1;

    std::string line, section;
    Node* curNode = nullptr;
    Clip* curClip = nullptr;
    std::string curBlobHash;   // [Blob] section: hash read before its bytes line
    AssetEntry curAsset;       // [AssetStore] section: built up, committed on payload
    int maxId = 0;

    auto getValue = [](const std::string& line) -> std::string {
        auto pos = line.find('=');
        if (pos == std::string::npos) return "";
        return line.substr(pos + 1);
    };
    auto getKey = [](const std::string& line) -> std::string {
        auto pos = line.find('=');
        if (pos == std::string::npos) return line;
        return line.substr(0, pos);
    };

    while (std::getline(f, line)) {
        if (line.empty()) continue;

        // Section header
        if (line[0] == '[') {
            section = line;
            if (section == "[Node]") {
                graph.nodes.push_back({});
                curNode = &graph.nodes.back();
                curClip = nullptr;
            } else if (section == "[PinIn]") {
                if (curNode) curNode->pinsIn.push_back({});
            } else if (section == "[PinOut]") {
                if (curNode) curNode->pinsOut.push_back({});
            } else if (section == "[Param]") {
                if (curNode) curNode->params.push_back({});
            } else if (section == "[Clip]") {
                if (curNode) {
                    curNode->clips.push_back({});
                    curClip = &curNode->clips.back();
                }
            } else if (section == "[Note]") {
                if (curClip) curClip->notes.push_back({});
            } else if (section == "[CC]") {
                if (curClip) curClip->ccEvents.push_back({});
            } else if (section == "[TakeLane]") {
                if (curNode) curNode->takeLanes.push_back({});
            } else if (section == "[TakeClip]") {
                if (curNode && !curNode->takeLanes.empty())
                    curNode->takeLanes.back().clips.push_back({});
            } else if (section == "[Comp]") {
                if (curNode) curNode->compSegments.push_back({});
            } else if (section == "[Link]") {
                graph.links.push_back({});
            } else if (section == "[Marker]") {
                graph.markers.push_back({});
            } else if (section == "[CCMap]") {
                graph.ccMappings.push_back({});
            } else if (section == "[Waveform]") {
                graph.waveformLibrary.push_back({});
            } else if (section == "[EffectGroup]") {
                graph.effectGroups.push_back({});
            } else if (section == "[AssetStore]") {
                curAsset = AssetEntry{};   // staged, committed when payload arrives
            }
            continue;
        }

        auto key = getKey(line);
        auto val = getValue(line);

        // Signal modulation pin bindings (#88): "modPin=paramIdx,pinId,depth[,mode]"
        // (mode 0=Modulate, 1=Absolute; optional for back-compat -> Modulate).
        //
        // These are written right after the node's [Param] blocks with NO section
        // header of their own, so by the time the parser reaches them `section` is
        // still "[Param]" - NOT "[Node]". A historical bug handled `modPin` only
        // inside the "[Node]" branch, so the key never matched and EVERY saved
        // modulation-pin binding (a pinned morph/layer param, a wired LFO, ...) was
        // silently dropped on load: the [Param] survived but its pin + binding
        // vanished, so reopening a project lost all its pins. Handle it here, ahead
        // of the section dispatch, so it's recognised regardless of section - which
        // fixes both newly-saved and already-saved (old) projects.
        if (key == "modPin" && curNode) {
            Node::ModPin mp;
            auto c1 = val.find(',');
            auto c2 = (c1 == std::string::npos) ? std::string::npos
                                                : val.find(',', c1 + 1);
            if (c1 != std::string::npos && c2 != std::string::npos) {
                mp.paramIndex = std::stoi(val.substr(0, c1));
                mp.pinId      = std::stoi(val.substr(c1 + 1, c2 - c1 - 1));
                auto c3 = val.find(',', c2 + 1);
                if (c3 != std::string::npos) {
                    mp.depth = std::stof(val.substr(c2 + 1, c3 - c2 - 1));
                    mp.mode  = (std::stoi(val.substr(c3 + 1)) == 1)
                                   ? Node::ModPin::Mode::Absolute
                                   : Node::ModPin::Mode::Modulate;
                } else {
                    mp.depth = std::stof(val.substr(c2 + 1));
                }
                curNode->modPins.push_back(mp);
            }
            continue;
        }

        if (section == "[Project]") {
            if (key == "bpm") graph.bpm = std::stof(val);
            else if (key == "timeSigNum") graph.timeSignatureNum = std::stoi(val);
            else if (key == "timeSigDen") graph.timeSignatureDen = std::stoi(val);
            else if (key == "loopEnabled") graph.loopEnabled = (val == "1");
            else if (key == "loopStart") graph.loopStartBeat = std::stof(val);
            else if (key == "loopEnd") graph.loopEndBeat = std::stof(val);
            else if (key == "projectSampleRate") graph.projectSampleRate = std::stof(val);
            else if (key == "tuningSystem") graph.tuningSystem = (TuningSystem)std::stoi(val);
            else if (key == "concertPitch") graph.concertPitch = std::stof(val);
            else if (key == "globalCrossfadeSec") graph.globalCrossfadeSec = std::stof(val);
            else if (key == "metronomeEnabled") graph.metronomeEnabled = (val == "1");
            else if (key == "songLengthBeats") graph.songLengthBeats = std::stof(val);
            else if (key == "songRepeatMode") {
                int m = std::stoi(val);
                graph.songRepeatMode = (m == 1 ? NodeGraph::SongRepeat::Forever
                                       : m == 2 ? NodeGraph::SongRepeat::NTimes
                                       :          NodeGraph::SongRepeat::None);
            }
            else if (key == "songRepeatCount") graph.songRepeatCount = std::max(1, std::stoi(val));
            else if (key == "historyFile") graph.historyFilePath = val;
            else if (key == "viewZoom") graph.viewZoom = std::stof(val);
            else if (key == "viewPanX") graph.viewPanX = std::stof(val);
            else if (key == "viewPanY") graph.viewPanY = std::stof(val);
            else if (key == "signalScriptLines") {
                int numLines = std::stoi(val);
                graph.signalScript.clear();
                for (int sl = 0; sl < numLines && std::getline(f, line); ++sl) {
                    if (!graph.signalScript.empty()) graph.signalScript += "\n";
                    graph.signalScript += line;
                }
            }
        }
        else if (section == "[Node]" && curNode) {
            if (key == "id") { curNode->id = std::stoi(val); maxId = std::max(maxId, curNode->id); }
            else if (key == "name") curNode->name = val;
            else if (key == "type") curNode->type = (NodeType)std::stoi(val);
            else if (key == "posX") curNode->pos.x = std::stof(val);
            else if (key == "posY") curNode->pos.y = std::stof(val);
            else if (key == "muted") curNode->muted = (val == "1");
            else if (key == "soloed") curNode->soloed = (val == "1");
            else if (key == "armMode") curNode->armMode = (AutoArmMode)std::stoi(val);
            else if (key == "ignoreAuto") curNode->ignoreAutomation = (val == "1");
            else if (key == "pluginAuto") {
                // pluginAuto=<paramIdx>,<beat>,<normValue> - one recorded point on
                // a hosted-plugin param lane (see writeProject).
                auto c1 = val.find(',');
                auto c2 = (c1 == std::string::npos) ? std::string::npos
                                                    : val.find(',', c1 + 1);
                if (c1 != std::string::npos && c2 != std::string::npos) {
                    try {
                        int idx = std::stoi(val.substr(0, c1));
                        float beat = std::stof(val.substr(c1 + 1, c2 - c1 - 1));
                        float v = std::stof(val.substr(c2 + 1));
                        curNode->pluginParamAutomation[idx].points.push_back({ beat, v });
                    } catch (...) {}
                }
            }
            else if (key == "scriptLines") {
                // New multi-line-safe format (see writeProject above).
                int n = 0;
                try { n = std::stoi(val); } catch (...) { n = 0; }
                std::string buf;
                for (int li = 0; li < n && std::getline(f, line); ++li) {
                    if (!buf.empty()) buf += "\n";
                    buf += line;
                }
                curNode->script = std::move(buf);
            }
            else if (key == "script") {
                curNode->script = val;
                // Backward-compatibility recovery for the silent-MOD-playback
                // bug: pre-fix saves wrote multi-line scripts (notably
                // MultiSampler) as plain `script=<first-line>` followed by
                // the remaining lines as orphan `key=val` pairs at the
                // [Node] level. Detect the MultiSampler prefix specifically
                // and slurp continuation lines back into the script until
                // we hit a recognised [Node]-level key or a new section
                // header. Lines we consume here would otherwise be silently
                // discarded by the unknown-key fallthrough.
                if (val == "__multisampler__:" ||
                    val.rfind("__multisampler__:", 0) == 0) {
                    static const std::set<std::string> kNodeKeys = {
                        "id", "name", "type", "posX", "posY", "muted",
                        "soloed", "armMode", "ignoreAuto", "pluginAuto", "script", "scriptLines",
                        "midiInputSourceId", "envAttackCurve",
                        "envDecayCurve", "envReleaseCurve", "envAtkPt",
                        "envDecPt", "envRelPt", "ahdsrEnvelope",
                        "aftertouchSensitivity", "pluginIndex", "pluginState",
                        "panLaw", "pan", "spatialX", "spatialY", "spatialZ",
                        "performanceMode", "perfReleaseMode", "perfVelocity",
                        "mpeEnabled", "mpePitchBendRange", "parentGroupId",
                        "groupBeatOffset", "anchorMarker", "groupExpanded",
                        "childNodeIds", "modPin",
                        "voiceContainerId", "voicePolyphony", "voiceStealMode",
                        "voiceGlideMs", "voiceUnison", "voiceUnisonDetune",
                        "voiceUnisonSpread",
                        "modImportSavedSong", "modImportPrevRepeatMode",
                        "modImportPrevRepeatCount", "modImportPrevSongLength",
                        "modImportPrevLoopEnabled", "modImportPrevLoopStart",
                        "modImportPrevLoopEnd"
                    };
                    // Peek ahead. We need a one-line lookahead but can't
                    // un-read with std::getline, so do the scan inline:
                    // consume only lines that obviously belong to the
                    // multisampler doc, and break out the moment we see
                    // a section marker or a node-level key.
                    auto savePos = f.tellg();
                    std::string peek;
                    while (std::getline(f, peek)) {
                        if (peek.empty()) { savePos = f.tellg(); continue; }
                        if (peek[0] == '[') break;       // new [Section]
                        std::string pk = getKey(peek);
                        if (kNodeKeys.count(pk)) break;  // back to node keys
                        // Looks like a multisampler doc continuation line -
                        // append to script.
                        curNode->script += "\n";
                        curNode->script += peek;
                        savePos = f.tellg();
                    }
                    // Rewind to before the line that broke the loop so the
                    // outer while(getline) re-reads it normally.
                    if (!f.eof()) f.seekg(savePos);
                    else f.clear(); // EOF is fine, just clear flag
                }
            }
            else if (key == "midiInputSourceId") curNode->midiInputSourceId = val;
            // Legacy per-node envelope fields (envAttackCurve / envDecayCurve /
            // envReleaseCurve and the envAtkPt / envDecPt / envRelPt point
            // lists) were replaced by the shared ahdsrEnvelope. They're still
            // recognised here so old projects load cleanly, but their values
            // are intentionally discarded — the node now carries its amplitude
            // shape solely in ahdsrEnvelope.
            else if (key == "envAttackCurve" || key == "envDecayCurve" ||
                     key == "envReleaseCurve" || key == "envAtkPt" ||
                     key == "envDecPt" || key == "envRelPt") {
                // discard
            }
            else if (key == "ahdsrEnvelope") {
                // Failure to decode (corrupt / partial line) leaves the
                // freshly-constructed default envelope in place rather than
                // wiping it — better to play with default ADSR than to
                // hard-fail the load on one bad field.
                AHDSREnvelope tmp;
                if (AHDSREnvelope::decode(val, tmp))
                    curNode->ahdsrEnvelope = std::move(tmp);
            }
            else if (key == "ahdsrAssetId") {
                try { curNode->ahdsrAssetId = std::stoi(val); } catch (...) {}
            }
            // Additional per-component AHDSR envelopes (FM operators etc.).
            // opEnvelopeCount is written first and pre-sizes the vector; the
            // opEnvelopeN lines that follow fill each slot. We tolerate either
            // order (decode resizes on demand) so a hand-edited file can't lose
            // an envelope to ordering.
            else if (key == "opEnvelopeCount") {
                try {
                    int n = std::stoi(val);
                    if (n > 0 && n <= 64 && (int)curNode->opEnvelopes.size() < n)
                        curNode->opEnvelopes.resize(n);
                } catch (...) {}
            }
            else if (key.rfind("opEnvelope", 0) == 0 && key != "opEnvelopeCount") {
                try {
                    int idx = std::stoi(key.substr(std::string("opEnvelope").size()));
                    if (idx >= 0 && idx < 64) {
                        if ((int)curNode->opEnvelopes.size() <= idx)
                            curNode->opEnvelopes.resize(idx + 1);
                        AHDSREnvelope tmp;
                        if (AHDSREnvelope::decode(val, tmp))
                            curNode->opEnvelopes[idx] = std::move(tmp);
                    }
                } catch (...) {}
            }
            else if (key == "aftertouchSensitivity") {
                try { curNode->aftertouchSensitivity = std::stof(val); }
                catch (...) {}
            }
            else if (key == "pluginIndex") curNode->pluginIndex = std::stoi(val);
            else if (key == "pluginState") curNode->pendingPluginState = val;
            else if (key == "panLaw") curNode->panLaw = (PanLaw)std::stoi(val);
            else if (key == "pan") curNode->pan = std::stof(val);
            else if (key == "spatialX") curNode->spatialX = std::stof(val);
            else if (key == "spatialY") curNode->spatialY = std::stof(val);
            else if (key == "spatialZ") curNode->spatialZ = std::stof(val);
            else if (key == "performanceMode") curNode->performanceMode = (val == "1");
            else if (key == "perfReleaseMode") curNode->performanceReleaseMode = std::stoi(val);
            else if (key == "perfVelocity") curNode->performanceVelocity = (val == "1");
            else if (key == "scopeTriggered") curNode->scopeTriggered = (val == "1");
            else if (key == "scopeTrigLevel") curNode->scopeTrigLevel = std::stof(val);
            else if (key == "scopeTrigRising") curNode->scopeTrigRising = (val == "1");
            else if (key == "mpeEnabled") curNode->mpeEnabled = (val == "1");
            else if (key == "mpePitchBendRange") curNode->mpePitchBendRange = std::stoi(val);
            else if (key == "parentGroupId") curNode->parentGroupId = std::stoi(val);
            else if (key == "groupBeatOffset") curNode->groupBeatOffset = std::stof(val);
            else if (key == "anchorMarker") curNode->anchorMarker = val;
            else if (key == "groupExpanded") curNode->groupExpanded = (val == "1");
            else if (key == "voiceContainerId") curNode->voiceContainerId = std::stoi(val);
            else if (key == "voicePolyphony") curNode->voicePolyphony = std::stoi(val);
            else if (key == "voiceStealMode") curNode->voiceStealMode = std::stoi(val);
            else if (key == "voiceGlideMs") curNode->voiceGlideMs = std::stof(val);
            else if (key == "voiceUnison") curNode->voiceUnison = std::stoi(val);
            else if (key == "voiceUnisonDetune") curNode->voiceUnisonDetune = std::stof(val);
            else if (key == "voiceUnisonSpread") curNode->voiceUnisonSpread = std::stof(val);
            else if (key == "modImportSavedSong") curNode->modImportSavedSong = (val == "1");
            else if (key == "modImportPrevRepeatMode") curNode->modImportPrevRepeatMode = std::stoi(val);
            else if (key == "modImportPrevRepeatCount") curNode->modImportPrevRepeatCount = std::stoi(val);
            else if (key == "modImportPrevSongLength") curNode->modImportPrevSongLength = std::stof(val);
            else if (key == "modImportPrevLoopEnabled") curNode->modImportPrevLoopEnabled = (val == "1");
            else if (key == "modImportPrevLoopStart") curNode->modImportPrevLoopStart = std::stof(val);
            else if (key == "modImportPrevLoopEnd") curNode->modImportPrevLoopEnd = std::stof(val);
            else if (key == "childNodeIds") {
                // Parse comma-separated IDs
                std::istringstream ss(val);
                std::string token;
                while (std::getline(ss, token, ','))
                    if (!token.empty()) curNode->childNodeIds.push_back(std::stoi(token));
            }
            // Audio cache / freeze metadata (see writeProject). The on-disk PCM
            // is re-attached after load by rehydrateNodeCaches, which resolves
            // diskPath against the loaded file's soundshop_cache/ folder and
            // drops the freeze if the file is gone. deterministic is left to
            // AudioCacheManager::updateDeterminism (it depends on live CC maps).
            else if (key == "cacheAuto") curNode->cache.autoCache = (val == "1");
            else if (key == "cacheEnabled") curNode->cache.enabled = (val == "1");
            else if (key == "cacheValid") curNode->cache.valid = (val == "1");
            else if (key == "cacheUseDisk") curNode->cache.useDisk = (val == "1");
            else if (key == "cacheHash") { try { curNode->cache.inputHash = std::stoull(val); } catch (...) {} }
            else if (key == "cacheSampleRate") { try { curNode->cache.sampleRate = std::stod(val); } catch (...) {} }
            else if (key == "cacheStartSample") { try { curNode->cache.startSample = std::stoll(val); } catch (...) {} }
            else if (key == "cacheNumSamples") { try { curNode->cache.numSamples = std::stoll(val); } catch (...) {} }
            // NOTE: "modPin=" lines are parsed earlier (before the section
            // dispatch) because they are written after the [Param] blocks, so
            // the active section is "[Param]" — not "[Node]" — when they're
            // read. See the hoisted handler near the top of this loop.
        }
        else if ((section == "[PinIn]" || section == "[PinOut]") && curNode) {
            auto& pins = (section == "[PinIn]") ? curNode->pinsIn : curNode->pinsOut;
            if (pins.empty()) continue;
            auto& pin = pins.back();
            pin.isInput = (section == "[PinIn]");
            if (key == "id") { pin.id = std::stoi(val); maxId = std::max(maxId, pin.id); }
            else if (key == "name") pin.name = val;
            else if (key == "kind") pin.kind = (PinKind)std::stoi(val);
            else if (key == "channels") pin.channels = std::stoi(val);
        }
        else if (section == "[Param]" && curNode && !curNode->params.empty()) {
            auto& p = curNode->params.back();
            if (key == "name") p.name = val;
            else if (key == "value") p.value = std::stof(val);
            else if (key == "min") p.minVal = std::stof(val);
            else if (key == "max") p.maxVal = std::stof(val);
            else if (key == "format") p.format = val;
            else if (key == "warpSlot") p.warpSlot = std::stoi(val);
            else if (key == "warpLayer") p.warpLayer = std::stoi(val);
            else if (key == "layerField") p.layerField = std::stoi(val);
            else if (key == "warpFrameId") p.warpFrameId = std::stoi(val);
            else if (key == "armMode") p.armMode = (AutoArmMode)std::stoi(val);
            else if (key == "bypassAuto") p.bypassAutomation = (val == "1");
            else if (key == "auto") {
                auto comma = val.find(',');
                if (comma != std::string::npos)
                    p.automation.points.push_back({std::stof(val.substr(0, comma)),
                                                    std::stof(val.substr(comma + 1))});
            }
        }
        else if (section == "[Clip]" && curClip) {
            if (key == "name") curClip->name = val;
            else if (key == "start") curClip->startBeat = std::stof(val);
            else if (key == "length") curClip->lengthBeats = std::stof(val);
            else if (key == "color") curClip->color = (uint32_t)std::stoul(val);
            else if (key == "channels") curClip->channels = std::stoi(val);
            else if (key == "waveformView") curClip->waveformView = std::stoi(val);
            else if (key == "clipKeyRoot") curClip->keyRoot = std::stoi(val);
            else if (key == "clipKeyType") curClip->keyType = val;
            else if (key == "hasCustomKey") curClip->hasCustomKey = (val == "1");
            else if (key == "audioFile") curClip->audioFilePath = val;
            else if (key == "slipOffset") curClip->slipOffset = std::stof(val);
            else if (key == "fadeIn") curClip->fadeInBeats = std::stof(val);
            else if (key == "fadeOut") curClip->fadeOutBeats = std::stof(val);
            else if (key == "gain") curClip->gainDb = std::stof(val);
        }
        else if (section == "[CC]" && curClip && !curClip->ccEvents.empty()) {
            auto& cc = curClip->ccEvents.back();
            if (key == "offset") cc.offset = std::stof(val);
            else if (key == "controller") cc.controller = std::stoi(val);
            else if (key == "value") cc.value = std::stoi(val);
            else if (key == "channel") cc.channel = std::stoi(val);
        }
        else if (section == "[Note]" && curClip && !curClip->notes.empty()) {
            auto& n = curClip->notes.back();
            if (key == "offset") n.offset = std::stof(val);
            else if (key == "pitch") n.pitch = std::stoi(val);
            else if (key == "duration") n.duration = std::stof(val);
            else if (key == "velocity") n.velocity = std::stoi(val);
            else if (key == "degree") n.degree = std::stoi(val);
            else if (key == "octave") n.octave = std::stoi(val);
            else if (key == "chromatic") n.chromaticOffset = std::stoi(val);
            else if (key == "detune") n.detune = std::stof(val);
            else if (key == "exactOffsetNum") n.exactOffset.num = std::stoi(val);
            else if (key == "exactOffsetDen") n.exactOffset.den = std::stoi(val);
            else if (key == "exactDurNum") n.exactDuration.num = std::stoi(val);
            else if (key == "exactDurDen") n.exactDuration.den = std::stoi(val);
            else if (key == "exPB" || key == "exSL" || key == "exPR") {
                auto comma = val.find(',');
                if (comma != std::string::npos) {
                    ExpressionPoint pt{std::stof(val.substr(0, comma)), std::stof(val.substr(comma + 1))};
                    if (key == "exPB") n.expression.pitchBend.push_back(pt);
                    else if (key == "exSL") n.expression.slide.push_back(pt);
                    else n.expression.pressure.push_back(pt);
                }
            }
        }
        else if (section == "[TakeLane]" && curNode && !curNode->takeLanes.empty()) {
            auto& lane = curNode->takeLanes.back();
            if (key == "name") lane.name = val;
            else if (key == "timeOffset") lane.timeOffsetSamples = std::stof(val);
            else if (key == "muted") lane.muted = (val == "1");
        }
        else if (section == "[TakeClip]" && curNode && !curNode->takeLanes.empty()
                 && !curNode->takeLanes.back().clips.empty()) {
            auto& clip = curNode->takeLanes.back().clips.back();
            if (key == "name") clip.name = val;
            else if (key == "start") clip.startBeat = std::stof(val);
            else if (key == "length") clip.lengthBeats = std::stof(val);
            else if (key == "color") clip.color = (uint32_t)std::stoul(val);
            else if (key == "channels") clip.channels = std::stoi(val);
            else if (key == "audioFile") clip.audioFilePath = val;
            else if (key == "slipOffset") clip.slipOffset = std::stof(val);
            else if (key == "gain") clip.gainDb = std::stof(val);
        }
        else if (section == "[Comp]" && curNode && !curNode->compSegments.empty()) {
            auto& comp = curNode->compSegments.back();
            if (key == "start") comp.startBeat = std::stof(val);
            else if (key == "end") comp.endBeat = std::stof(val);
            else if (key == "lane") comp.takeLaneIdx = std::stoi(val);
            else if (key == "crossfade") comp.crossfadeBeats = std::stof(val);
        }
        else if (section == "[Link]" && !graph.links.empty()) {
            auto& l = graph.links.back();
            if (key == "id") { l.id = std::stoi(val); maxId = std::max(maxId, l.id); }
            else if (key == "startPin") l.startPin = std::stoi(val);
            else if (key == "endPin") l.endPin = std::stoi(val);
            else if (key == "gain") l.gainDb = std::stof(val);
        }
        else if (section == "[Marker]" && !graph.markers.empty()) {
            auto& m = graph.markers.back();
            if (key == "id") m.id = std::stoi(val);
            else if (key == "name") m.name = val;
            else if (key == "beat") m.beat = std::stof(val);
            else if (key == "color") m.color = (uint32_t)std::stoi(val);
        }
        else if (section == "[CCMap]" && !graph.ccMappings.empty()) {
            auto& m = graph.ccMappings.back();
            if (key == "midiCh") m.midiCh = std::stoi(val);
            else if (key == "ccNum") m.ccNum = std::stoi(val);
            else if (key == "nodeId") m.nodeId = std::stoi(val);
            else if (key == "paramIdx") m.paramIdx = std::stoi(val);
        }
        else if (section == "[EffectGroup]" && !graph.effectGroups.empty()) {
            auto& eg = graph.effectGroups.back();
            if (key == "id") { eg.id = std::stoi(val); graph.nextEffectGroupId = std::max(graph.nextEffectGroupId, eg.id + 1); }
            else if (key == "name") eg.name = val;
            else if (key == "color") eg.color = (uint32_t)std::stoul(val);
            else if (key == "crossfadeSec") eg.crossfadeSec = std::stof(val);
            else if (key == "linkIds") {
                std::istringstream ss(val);
                std::string token;
                while (std::getline(ss, token, ','))
                    if (!token.empty()) eg.linkIds.push_back(std::stoi(token));
            }
        }
        else if (section == "[Waveform]" && !graph.waveformLibrary.empty()) {
            auto& wf = graph.waveformLibrary.back();
            if (key == "name") wf.name = val;
            else if (key == "expression") wf.expression = val;
            else if (key == "point") {
                auto comma = val.find(',');
                if (comma != std::string::npos)
                    wf.points.push_back({std::stof(val.substr(0, comma)), std::stof(val.substr(comma + 1))});
            }
        }
        else if (section == "[AssetStore]") {
            // Project-level asset library entry (see writeProject [AssetStore]).
            // Fields arrive in write order with payload LAST, so we stage into
            // curAsset and commit when the payload line is seen. id/hash are
            // trusted from the file (not recomputed), mirroring [Blob].
            if (key == "id") curAsset.id = std::stoi(val);
            else if (key == "kind") assetKindFromTag(val, curAsset.kind);
            else if (key == "name") curAsset.name = val;
            else if (key == "subType") curAsset.subType = val;
            else if (key == "hash") curAsset.contentHash = val;
            else if (key == "archived") curAsset.archived = (val == "1");
            else if (key == "starred") curAsset.starred = (val == "1");
            else if (key == "payload") {
                juce::MemoryOutputStream mos;
                if (juce::Base64::convertFromBase64(mos, val)) {
                    const char* d = (const char*) mos.getData();
                    curAsset.payload.assign(d, d + mos.getDataSize());
                }
                graph.assets.insertRaw(curAsset);   // bumps nextId past this id
                curAsset = AssetEntry{};
            }
        }
        else if (section == "[Blob]") {
            // Content-addressed baked blob (see writeProject [Blob]). The hash is
            // trusted from the file - not recomputed - so loading never has to
            // decompress; integrity is checked lazily when a node resolves it.
            if (key == "hash") curBlobHash = val;
            else if (key == "bytes") {
                if (!curBlobHash.empty()) {
                    juce::MemoryOutputStream mos;
                    if (juce::Base64::convertFromBase64(mos, val)) {
                        const uint8_t* d = (const uint8_t*) mos.getData();
                        graph.contentStore.insertRaw(
                            curBlobHash,
                            std::vector<uint8_t>(d, d + mos.getDataSize()));
                    }
                    curBlobHash.clear();
                }
            }
        }
        else if (section == "[Editors]") {
            if (key == "openEditors") {
                std::istringstream ss(val);
                std::string token;
                while (std::getline(ss, token, ',')) {
                    if (!token.empty()) {
                        int nodeId = std::stoi(token);
                        pendingEditorIds.push_back(nodeId);
                    }
                }
            }
            else if (key == "activeEditor") pendingActiveEditorId = std::stoi(val);
        }
    }

    // Restore nextId so new IDs don't conflict
    graph.setNextId(maxId + 1);

    // Re-seed the code-owned built-in morph chains. They are deliberately NOT
    // serialized (writeProject skips them), so a loaded file - and every undo
    // snapshot restored through this same path - arrives without them; re-seeding
    // here makes them present and keeps any frame that references a built-in id
    // resolvable below. Idempotent, so a file that somehow already carries them
    // (or a back-to-back load) never duplicates. See seedBuiltinMorphLibrary.
    seedBuiltinMorphLibrary(graph.assets);

    // Mirror every live-referenced asset-library AHDSR curve into its
    // referencing nodes' local ahdsrEnvelope (the audio thread reads that
    // directly). Assets are parsed before nodes, so the library is complete here.
    graph.resolveAhdsrReferences();

    // FM synth: migrate any project that predates node.opEnvelopes. Old files
    // carry "Op{i} A/D/S/R" linear-ramp params and no opEnvelopes; this rebuilds
    // the 4 per-operator AHDSR envelopes from them (and strips the old params).
    // No-op for files already carrying the 4 envelopes and for non-FM nodes.
    for (auto& n : graph.nodes)
        ensureFmOpEnvelopes(n);

    // VoiceIn: migrate any project that predates the MPE expression outputs.
    // Old files carry only MIDI/Pitch/Gate/Velocity output pins; append the
    // Pressure and Timbre Signal outputs (channels 5/6) so MPE patches built
    // after the upgrade can wire them. Idempotent - skips if already present.
    for (auto& n : graph.nodes) {
        if (n.type != NodeType::VoiceIn) continue;
        auto hasPin = [&](const std::string& nm) {
            for (auto& p : n.pinsOut) if (p.name == nm) return true;
            return false;
        };
        if (!hasPin("Pressure")) {
            Pin p; p.id = graph.allocId(); p.name = "Pressure";
            p.kind = PinKind::Signal; p.isInput = false; p.channels = 1;
            n.pinsOut.push_back(p);
        }
        if (!hasPin("Timbre")) {
            Pin p; p.id = graph.allocId(); p.name = "Timbre";
            p.kind = PinKind::Signal; p.isInput = false; p.channels = 1;
            n.pinsOut.push_back(p);
        }
    }

    // Same for live-referenced Waveform assets: push each referenced asset's
    // frame into the wavetable library entries that point at it, re-encoding
    // the affected node scripts so the audio thread (which decodes node.script)
    // sees the asset content. Free function in layered_wave_editor.cpp.
    resolveWaveformReferences(graph);

    // Per-layer analogue: any individual WaveLayer that live-references a Waveform
    // asset (WaveLayer::assetId >= 0) has its cycle pulled from the asset, after
    // the frame-scope pass so a frame entry that itself came from an asset already
    // has its layers in place. Free function in layered_wave_editor.cpp.
    resolvePerLayerWaveformReferences(graph);

    // Same for live-referenced MorphAlgorithm (warp-chain) assets: replace each
    // referencing frame's cached warp chain with the asset's stored chain and
    // reconcile the node's warp modulation params. Free function in
    // layered_wave_editor.cpp.
    resolveWarpReferences(graph);

    // resolveWarpReferences only touches asset-referenced frames; independent
    // frames (the common case) need their warp params reconciled too so legacy
    // "Warp N" params migrate to the warpSlot key + named-morph labels and the
    // synth (reads warp amounts by warpSlot) finds them. Idempotent.
    reconcileAllWarpParams(graph);

    // Same for live-referenced FrequencyGraph assets: mirror each referenced
    // curve into the spectrum-tap bins that point at it, re-encoding the
    // affected node scripts. Free function in spectrum_tap.cpp.
    resolveSpectrumTapReferences(graph);

    // Same FrequencyGraph assets feed the spectral (FFT) mag/phase curves -
    // both standalone Frequency Domain nodes and SpectralFrames nested inside
    // wavetable nodes. Free function in spectral_editor.cpp.
    resolveSpectralReferences(graph);

    // ...and the Curve EQ nodes, the third FrequencyGraph consumer. Free
    // function in curve_editor.cpp.
    resolveCurveEqReferences(graph);

    // Restore open editors (store IDs only - never store Node*)
    graph.openEditors.clear();
    for (int id : pendingEditorIds) {
        if (graph.findNode(id))
            graph.openEditors.push_back(id);
    }
    graph.activeEditorNodeId = pendingActiveEditorId;

    // Reload plugins based on pluginIndex and restore state
    if (pluginHost) {
        for (auto& n : graph.nodes) {
            if (n.pluginIndex >= 0) {
                auto loaded = pluginHost->loadPlugin(n.pluginIndex, 48000.0, 480);
                if (loaded) {
                    // Restore saved plugin state
                    if (!n.pendingPluginState.empty() && loaded->instance) {
                        juce::MemoryBlock stateData;
                        stateData.fromBase64Encoding(n.pendingPluginState);
                        if (stateData.getSize() > 0)
                            loaded->instance->setStateInformation(
                                stateData.getData(), (int)stateData.getSize());
                    }
                    n.plugin = std::move(loaded);
                    fprintf(stderr, "  Reloaded plugin '%s' for node '%s'\n",
                            n.plugin->info.name.c_str(), n.name.c_str());
                } else {
                    fprintf(stderr, "  Failed to reload plugin index %d for node '%s'\n",
                            n.pluginIndex, n.name.c_str());
                }
            }
        }
    }

    // Reset node positions (force re-set in editor)
    for (auto& n : graph.nodes)
        n.posSet = false;

    graph.dirty = false;
    return true;
}

std::string ProjectFile::serializeForUndo(NodeGraph& graph) {
    // Pass nullptr for the GraphProcessor so plugin state is omitted -
    // that's the expensive part and undo doesn't need it (plugin internal
    // state is captured separately by the slow autosave path, task #86).
    std::ostringstream oss;
    // includeView=false: undo snapshots intentionally exclude the saved
    // pan/zoom so undoing a graph edit doesn't also jerk the user's
    // viewport around. The view state lives on NodeGraph but is treated
    // as out-of-band session state for undo purposes.
    writeProject(oss, graph, nullptr, /*includeView*/ false, /*includeBlobs*/ false);
    return oss.str();
}

bool ProjectFile::loadFromString(const std::string& text, NodeGraph& graph,
                                 PluginHost* pluginHost) {
    std::istringstream iss(text);
    return readProject(iss, graph, pluginHost);
}

} // namespace SoundShop

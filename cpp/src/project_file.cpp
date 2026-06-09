#include "project_file.h"
#include <juce_core/juce_core.h>
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

bool ProjectFile::writeProject(std::ostream& f, NodeGraph& graph,
                               GraphProcessor* gp, bool includeView) {

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
        }
        if (node.performanceMode) {
            writeInt(f, "performanceMode", 1);
            writeInt(f, "perfReleaseMode", node.performanceReleaseMode);
            writeInt(f, "perfVelocity", node.performanceVelocity ? 1 : 0);
        }
        if (node.mpeEnabled) {
            writeInt(f, "mpeEnabled", 1);
            writeInt(f, "mpePitchBendRange", node.mpePitchBendRange);
        }
        writeInt(f, "parentGroupId", node.parentGroupId);
        writeFloat(f, "groupBeatOffset", node.groupBeatOffset);
        if (!node.anchorMarker.empty())
            writeStr(f, "anchorMarker", node.anchorMarker);
        writeInt(f, "groupExpanded", node.groupExpanded ? 1 : 0);
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
            for (auto& ap : param.automation.points)
                f << "auto=" << ap.beat << "," << ap.value << "\n";
        }
        // Signal modulation pin bindings (#88). The pins themselves are
        // already serialized in the [PinIn] block above; the modPin
        // entries just record which param each one drives.
        for (auto& mp : node.modPins) {
            f << "modPin=" << mp.paramIndex << "," << mp.pinId
              << "," << mp.depth << "\n";
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

    f << "\n[End]\n";
    return true;
}

bool ProjectFile::load(const std::string& path, NodeGraph& graph, PluginHost* pluginHost) {
    std::ifstream f(path);
    if (!f) {
        fprintf(stderr, "Failed to load project: %s\n", path.c_str());
        return false;
    }
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
            }
            continue;
        }

        auto key = getKey(line);
        auto val = getValue(line);

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
                        "soloed", "script", "scriptLines",
                        "midiInputSourceId", "envAttackCurve",
                        "envDecayCurve", "envReleaseCurve", "envAtkPt",
                        "envDecPt", "envRelPt", "ahdsrEnvelope",
                        "aftertouchSensitivity", "pluginIndex", "pluginState",
                        "panLaw", "pan", "spatialX", "spatialY", "spatialZ",
                        "performanceMode", "perfReleaseMode", "perfVelocity",
                        "mpeEnabled", "mpePitchBendRange", "parentGroupId",
                        "groupBeatOffset", "anchorMarker", "groupExpanded",
                        "childNodeIds", "modPin",
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
            else if (key == "mpeEnabled") curNode->mpeEnabled = (val == "1");
            else if (key == "mpePitchBendRange") curNode->mpePitchBendRange = std::stoi(val);
            else if (key == "parentGroupId") curNode->parentGroupId = std::stoi(val);
            else if (key == "groupBeatOffset") curNode->groupBeatOffset = std::stof(val);
            else if (key == "anchorMarker") curNode->anchorMarker = val;
            else if (key == "groupExpanded") curNode->groupExpanded = (val == "1");
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
            // Signal modulation pin bindings (#88): "modPin=paramIdx,pinId,depth"
            else if (key == "modPin") {
                Node::ModPin mp;
                auto c1 = val.find(',');
                auto c2 = val.find(',', c1 + 1);
                if (c1 != std::string::npos && c2 != std::string::npos) {
                    mp.paramIndex = std::stoi(val.substr(0, c1));
                    mp.pinId      = std::stoi(val.substr(c1 + 1, c2 - c1 - 1));
                    mp.depth      = std::stof(val.substr(c2 + 1));
                    curNode->modPins.push_back(mp);
                }
            }
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
    writeProject(oss, graph, nullptr, /*includeView*/ false);
    return oss.str();
}

bool ProjectFile::loadFromString(const std::string& text, NodeGraph& graph,
                                 PluginHost* pluginHost) {
    std::istringstream iss(text);
    return readProject(iss, graph, pluginHost);
}

} // namespace SoundShop

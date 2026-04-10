#include "project_file.h"
#include <juce_core/juce_core.h>
#include <fstream>
#include <sstream>
#include <cstdio>

namespace SoundShop {

std::string ProjectFile::currentPath;

// Simple text-based project format
// Sections: [Project], [Node], [Pin], [Clip], [Note], [Link], [Param]

static void writeStr(std::ofstream& f, const std::string& key, const std::string& val) {
    f << key << "=" << val << "\n";
}
static void writeInt(std::ofstream& f, const std::string& key, int val) {
    f << key << "=" << val << "\n";
}
static void writeFloat(std::ofstream& f, const std::string& key, float val) {
    f << key << "=" << val << "\n";
}

bool ProjectFile::save(const std::string& path, NodeGraph& graph, GraphProcessor* gp) {
    std::ofstream f(path);
    if (!f) {
        fprintf(stderr, "Failed to save project: %s\n", path.c_str());
        return false;
    }

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
        writeStr(f, "script", node.script);
        if (!node.envAttackCurve.empty()) writeStr(f, "envAttackCurve", node.envAttackCurve);
        if (!node.envDecayCurve.empty()) writeStr(f, "envDecayCurve", node.envDecayCurve);
        if (!node.envReleaseCurve.empty()) writeStr(f, "envReleaseCurve", node.envReleaseCurve);
        for (auto& pt : node.envAttackPoints) f << "envAtkPt=" << pt.first << "," << pt.second << "\n";
        for (auto& pt : node.envDecayPoints) f << "envDecPt=" << pt.first << "," << pt.second << "\n";
        for (auto& pt : node.envReleasePoints) f << "envRelPt=" << pt.first << "," << pt.second << "\n";
        writeInt(f, "pluginIndex", node.pluginIndex);
        if (node.pan != 0.0f) writeFloat(f, "pan", node.pan);
        if (node.spatialX != 0.0f) writeFloat(f, "spatialX", node.spatialX);
        if (node.spatialY != 0.0f) writeFloat(f, "spatialY", node.spatialY);
        if (node.spatialZ != 0.0f) writeFloat(f, "spatialZ", node.spatialZ);
        // Save plugin state as base64
        if (gp && node.pluginIndex >= 0) {
            auto* proc = gp->getProcessorForNode(node.id);
            if (proc) {
                juce::MemoryBlock stateData;
                proc->getStateInformation(stateData);
                if (stateData.getSize() > 0)
                    writeStr(f, "pluginState", stateData.toBase64Encoding().toStdString());
            }
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
            writeFloat(f, "value", param.value);
            writeFloat(f, "min", param.minVal);
            writeFloat(f, "max", param.maxVal);
            writeStr(f, "format", param.format);
            for (auto& ap : param.automation.points)
                f << "auto=" << ap.beat << "," << ap.value << "\n";
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
            ids += std::to_string(graph.openEditors[i]->id);
        }
        writeStr(f, "openEditors", ids);
        writeInt(f, "activeEditor", graph.activeEditorNodeId);
    }

    f << "\n[End]\n";
    currentPath = path;
    fprintf(stderr, "Project saved: %s\n", path.c_str());
    return true;
}

bool ProjectFile::load(const std::string& path, NodeGraph& graph, PluginHost* pluginHost) {
    std::ifstream f(path);
    if (!f) {
        fprintf(stderr, "Failed to load project: %s\n", path.c_str());
        return false;
    }

    // Clear existing
    graph.nodes.clear();
    graph.links.clear();
    graph.openEditors.clear();

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
            else if (key == "script") curNode->script = val;
            else if (key == "envAttackCurve") curNode->envAttackCurve = val;
            else if (key == "envDecayCurve") curNode->envDecayCurve = val;
            else if (key == "envReleaseCurve") curNode->envReleaseCurve = val;
            else if (key == "envAtkPt") {
                auto c = val.find(',');
                if (c != std::string::npos)
                    curNode->envAttackPoints.push_back({std::stof(val.substr(0,c)), std::stof(val.substr(c+1))});
            }
            else if (key == "envDecPt") {
                auto c = val.find(',');
                if (c != std::string::npos)
                    curNode->envDecayPoints.push_back({std::stof(val.substr(0,c)), std::stof(val.substr(c+1))});
            }
            else if (key == "envRelPt") {
                auto c = val.find(',');
                if (c != std::string::npos)
                    curNode->envReleasePoints.push_back({std::stof(val.substr(0,c)), std::stof(val.substr(c+1))});
            }
            else if (key == "pluginIndex") curNode->pluginIndex = std::stoi(val);
            else if (key == "pluginState") curNode->pendingPluginState = val;
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
            else if (key == "childNodeIds") {
                // Parse comma-separated IDs
                std::istringstream ss(val);
                std::string token;
                while (std::getline(ss, token, ','))
                    if (!token.empty()) curNode->childNodeIds.push_back(std::stoi(token));
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

    // Restore open editors
    graph.openEditors.clear();
    for (int id : pendingEditorIds) {
        auto* node = graph.findNode(id);
        if (node) graph.openEditors.push_back(node);
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

    currentPath = path;
    graph.dirty = false;
    fprintf(stderr, "Project loaded: %s (%d nodes, %d links)\n",
            path.c_str(), (int)graph.nodes.size(), (int)graph.links.size());
    return true;
}

} // namespace SoundShop

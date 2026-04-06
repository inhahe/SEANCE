#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "scripting.h"
#include "music_theory.h"
#include <sstream>
#include <cstdio>

namespace SoundShop {

// Global pointer so Python callbacks can access the graph
static NodeGraph* g_currentGraph = nullptr;
static int g_activeNodeIndex = -1; // set when running script from a track context

// ==============================================================================
// Python module: soundshop
// Provides access to the project from scripts
// ==============================================================================

// Get the active node index (set when running from a track context)
// Returns -1 if not running from a track context
static PyObject* py_this_node(PyObject*, PyObject*) {
    return PyLong_FromLong(g_activeNodeIndex);
}

// Get number of nodes
static PyObject* py_get_node_count(PyObject*, PyObject*) {
    if (!g_currentGraph) return PyLong_FromLong(0);
    return PyLong_FromLong((long)g_currentGraph->nodes.size());
}

// Get node names as list
static PyObject* py_get_node_names(PyObject*, PyObject*) {
    if (!g_currentGraph) return PyList_New(0);
    auto* list = PyList_New(g_currentGraph->nodes.size());
    for (int i = 0; i < (int)g_currentGraph->nodes.size(); ++i)
        PyList_SetItem(list, i, PyUnicode_FromString(g_currentGraph->nodes[i].name.c_str()));
    return list;
}

// Get node by index or name — returns dict with node info
static PyObject* py_get_node(PyObject*, PyObject* args) {
    int idx = -1;

    // Try parsing as int first
    if (!PyArg_ParseTuple(args, "i", &idx)) {
        PyErr_Clear();
        // Try as string name
        const char* name;
        if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;
        if (!g_currentGraph) { PyErr_SetString(PyExc_RuntimeError, "No graph"); return nullptr; }
        std::string needle(name);
        for (auto& c : needle) c = (char)std::tolower(c);
        for (int i = 0; i < (int)g_currentGraph->nodes.size(); ++i) {
            std::string lower = g_currentGraph->nodes[i].name;
            for (auto& c : lower) c = (char)std::tolower(c);
            if (lower.find(needle) != std::string::npos) { idx = i; break; }
        }
        if (idx < 0) {
            PyErr_SetString(PyExc_KeyError, ("No node matching '" + std::string(name) + "'").c_str());
            return nullptr;
        }
    }
    if (!g_currentGraph || idx < 0 || idx >= (int)g_currentGraph->nodes.size()) {
        PyErr_SetString(PyExc_IndexError, "Node index out of range");
        return nullptr;
    }
    auto& node = g_currentGraph->nodes[idx];
    auto* dict = PyDict_New();
    PyDict_SetItemString(dict, "id", PyLong_FromLong(node.id));
    PyDict_SetItemString(dict, "name", PyUnicode_FromString(node.name.c_str()));
    PyDict_SetItemString(dict, "type", PyLong_FromLong((int)node.type));
    PyDict_SetItemString(dict, "num_clips", PyLong_FromLong((long)node.clips.size()));

    // Include clip info
    auto* clips = PyList_New(node.clips.size());
    for (int ci = 0; ci < (int)node.clips.size(); ++ci) {
        auto& clip = node.clips[ci];
        auto* cdict = PyDict_New();
        PyDict_SetItemString(cdict, "name", PyUnicode_FromString(clip.name.c_str()));
        PyDict_SetItemString(cdict, "start_beat", PyFloat_FromDouble(clip.startBeat));
        PyDict_SetItemString(cdict, "length_beats", PyFloat_FromDouble(clip.lengthBeats));
        PyDict_SetItemString(cdict, "num_notes", PyLong_FromLong((long)clip.notes.size()));

        // Notes
        auto* notes = PyList_New(clip.notes.size());
        for (int ni = 0; ni < (int)clip.notes.size(); ++ni) {
            auto& n = clip.notes[ni];
            auto* ndict = PyDict_New();
            PyDict_SetItemString(ndict, "offset", PyFloat_FromDouble(n.offset));
            PyDict_SetItemString(ndict, "pitch", PyLong_FromLong(n.pitch));
            PyDict_SetItemString(ndict, "duration", PyFloat_FromDouble(n.duration));
            PyDict_SetItemString(ndict, "degree", PyLong_FromLong(n.degree));
            PyDict_SetItemString(ndict, "velocity", PyLong_FromLong(n.velocity));
            PyDict_SetItemString(ndict, "detune", PyFloat_FromDouble(n.detune));
            PyDict_SetItemString(ndict, "name", PyUnicode_FromString(
                MusicTheory::noteName(n.pitch).c_str()));
            PyList_SetItem(notes, ni, ndict);
        }
        PyDict_SetItemString(cdict, "notes", notes);
        PyList_SetItem(clips, ci, cdict);
    }
    PyDict_SetItemString(dict, "clips", clips);
    return dict;
}

// Add a note to a clip
static PyObject* py_add_note(PyObject*, PyObject* args) {
    int nodeIdx, clipIdx, pitch;
    float offset, duration;
    int velocity = 100;
    if (!PyArg_ParseTuple(args, "iiiff|i", &nodeIdx, &clipIdx, &pitch, &offset, &duration, &velocity))
        return nullptr;
    if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
        PyErr_SetString(PyExc_IndexError, "Node index out of range");
        return nullptr;
    }
    auto& node = g_currentGraph->nodes[nodeIdx];
    if (clipIdx < 0 || clipIdx >= (int)node.clips.size()) {
        PyErr_SetString(PyExc_IndexError, "Clip index out of range");
        return nullptr;
    }
    MidiNote nn;
    nn.offset = offset;
    nn.pitch = pitch;
    nn.duration = duration;
    nn.velocity = juce::jlimit(1, 127, velocity);
    node.clips[clipIdx].notes.push_back(nn);
    node.clips[clipIdx].lengthBeats = std::max(node.clips[clipIdx].lengthBeats,
        std::ceil((offset + duration) / 4.0f) * 4.0f);
    Py_RETURN_NONE;
}

// Clear all notes in a clip
static PyObject* py_clear_notes(PyObject*, PyObject* args) {
    int nodeIdx, clipIdx;
    if (!PyArg_ParseTuple(args, "ii", &nodeIdx, &clipIdx)) return nullptr;
    if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
        PyErr_SetString(PyExc_IndexError, "Node index out of range");
        return nullptr;
    }
    auto& node = g_currentGraph->nodes[nodeIdx];
    if (clipIdx < 0 || clipIdx >= (int)node.clips.size()) {
        PyErr_SetString(PyExc_IndexError, "Clip index out of range");
        return nullptr;
    }
    node.clips[clipIdx].notes.clear();
    Py_RETURN_NONE;
}

// Set a note's properties
static PyObject* py_set_note(PyObject*, PyObject* args) {
    int nodeIdx, clipIdx, noteIdx, pitch;
    float offset, duration, detune;
    if (!PyArg_ParseTuple(args, "iiiifff", &nodeIdx, &clipIdx, &noteIdx, &pitch, &offset, &duration, &detune))
        return nullptr;
    if (!g_currentGraph) { PyErr_SetString(PyExc_RuntimeError, "No project"); return nullptr; }
    auto& node = g_currentGraph->nodes[nodeIdx];
    auto& note = node.clips[clipIdx].notes[noteIdx];
    note.pitch = pitch;
    note.offset = offset;
    note.duration = duration;
    note.detune = detune;
    Py_RETURN_NONE;
}

// Add MIDI CC event
static PyObject* py_add_cc(PyObject*, PyObject* args) {
    int nodeIdx, clipIdx, controller, value, channel;
    float offset;
    channel = 1;
    if (!PyArg_ParseTuple(args, "iiifi|i", &nodeIdx, &clipIdx, &controller, &offset, &value, &channel))
        return nullptr;
    if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
        PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
    }
    auto& node = g_currentGraph->nodes[nodeIdx];
    if (clipIdx < 0 || clipIdx >= (int)node.clips.size()) {
        PyErr_SetString(PyExc_IndexError, "Clip index out of range"); return nullptr;
    }
    MidiCCEvent cc;
    cc.offset = offset;
    cc.controller = controller;
    cc.value = value;
    cc.channel = channel;
    node.clips[clipIdx].ccEvents.push_back(cc);
    Py_RETURN_NONE;
}

// Set audio file on a clip
static PyObject* py_set_audio_file(PyObject*, PyObject* args) {
    int nodeIdx, clipIdx;
    const char* path;
    if (!PyArg_ParseTuple(args, "iis", &nodeIdx, &clipIdx, &path)) return nullptr;
    if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
        PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
    }
    auto& node = g_currentGraph->nodes[nodeIdx];
    if (clipIdx < 0 || clipIdx >= (int)node.clips.size()) {
        PyErr_SetString(PyExc_IndexError, "Clip index out of range"); return nullptr;
    }
    node.clips[clipIdx].audioFilePath = path;
    Py_RETURN_NONE;
}

// Add a CC-to-parameter mapping
static PyObject* py_map_cc(PyObject*, PyObject* args) {
    int midiCh, ccNum, nodeIdx, paramIdx;
    float minVal = 0.0f, maxVal = 1.0f;
    if (!PyArg_ParseTuple(args, "iiii|ff", &midiCh, &ccNum, &nodeIdx, &paramIdx, &minVal, &maxVal))
        return nullptr;
    // Store mapping — will be applied through automation manager
    // For now, print confirmation
    fprintf(stderr, "CC mapping: ch%d cc%d -> node %d param %d [%.2f-%.2f]\n",
            midiCh, ccNum, nodeIdx, paramIdx, minVal, maxVal);
    Py_RETURN_NONE;
}

// Get BPM
static PyObject* py_get_bpm(PyObject*, PyObject*) {
    if (!g_currentGraph) return PyFloat_FromDouble(120);
    return PyFloat_FromDouble(g_currentGraph->bpm);
}

// Find node by name — returns index, or list of indices if multiple match
static PyObject* py_find_node(PyObject*, PyObject* args) {
    const char* name;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;
    if (!g_currentGraph) return PyList_New(0);

    std::string needle(name);
    std::vector<int> matches;
    for (int i = 0; i < (int)g_currentGraph->nodes.size(); ++i) {
        auto& nodeName = g_currentGraph->nodes[i].name;
        // Case-insensitive substring match
        std::string lower = nodeName;
        std::string lowerNeedle = needle;
        for (auto& c : lower) c = (char)std::tolower(c);
        for (auto& c : lowerNeedle) c = (char)std::tolower(c);
        if (lower.find(lowerNeedle) != std::string::npos)
            matches.push_back(i);
    }

    if (matches.size() == 1)
        return PyLong_FromLong(matches[0]);

    auto* list = PyList_New(matches.size());
    for (int i = 0; i < (int)matches.size(); ++i)
        PyList_SetItem(list, i, PyLong_FromLong(matches[i]));
    return list;
}

// Create a MIDI track. Returns the new node's index.
static PyObject* py_add_midi_track(PyObject*, PyObject* args) {
    const char* name = "MIDI Track";
    float x = 50, y = 50;
    if (!PyArg_ParseTuple(args, "|sff", &name, &x, &y)) return nullptr;
    if (!g_currentGraph) Py_RETURN_NONE;

    auto& n = g_currentGraph->addNode(name, NodeType::MidiTimeline,
        {}, {Pin{0, "MIDI", PinKind::Midi, false}}, {x, y});
    n.clips.push_back({"Clip 1", 0, 4, 0xFF6688CC});
    g_currentGraph->dirty = true;
    return PyLong_FromLong((long)(g_currentGraph->nodes.size() - 1));
}

// Create an audio track. Returns the new node's index.
static PyObject* py_add_audio_track(PyObject*, PyObject* args) {
    const char* name = "Audio Track";
    float x = 50, y = 50;
    if (!PyArg_ParseTuple(args, "|sff", &name, &x, &y)) return nullptr;
    if (!g_currentGraph) Py_RETURN_NONE;

    auto& n = g_currentGraph->addNode(name, NodeType::AudioTimeline,
        {}, {Pin{0, "Audio", PinKind::Audio, false}}, {x, y});
    n.clips.push_back({"Clip 1", 0, 4, 0xFF66CC88});
    g_currentGraph->dirty = true;
    return PyLong_FromLong((long)(g_currentGraph->nodes.size() - 1));
}

// Add a clip to a node. Returns clip index.
static PyObject* py_add_clip(PyObject*, PyObject* args) {
    int nodeIdx;
    float startBeat = 0, lengthBeats = 4;
    const char* name = "Clip";
    if (!PyArg_ParseTuple(args, "i|ffs", &nodeIdx, &startBeat, &lengthBeats, &name)) return nullptr;
    if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
        PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
    }
    auto& node = g_currentGraph->nodes[nodeIdx];
    Clip c;
    c.name = name;
    c.startBeat = startBeat;
    c.lengthBeats = lengthBeats;
    c.color = 0xFF6688CC;
    node.clips.push_back(c);
    g_currentGraph->dirty = true;
    return PyLong_FromLong((long)(node.clips.size() - 1));
}

// Connect two nodes by pin. add_link(src_node_idx, dst_node_idx, [src_pin_idx=0, dst_pin_idx=0])
static PyObject* py_add_link(PyObject*, PyObject* args) {
    int srcNodeIdx, dstNodeIdx;
    int srcPinIdx = 0, dstPinIdx = 0;
    if (!PyArg_ParseTuple(args, "ii|ii", &srcNodeIdx, &dstNodeIdx, &srcPinIdx, &dstPinIdx)) return nullptr;
    if (!g_currentGraph) Py_RETURN_NONE;
    if (srcNodeIdx < 0 || srcNodeIdx >= (int)g_currentGraph->nodes.size() ||
        dstNodeIdx < 0 || dstNodeIdx >= (int)g_currentGraph->nodes.size()) {
        PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
    }
    auto& srcNode = g_currentGraph->nodes[srcNodeIdx];
    auto& dstNode = g_currentGraph->nodes[dstNodeIdx];
    if (srcPinIdx < 0 || srcPinIdx >= (int)srcNode.pinsOut.size()) {
        PyErr_SetString(PyExc_IndexError, "Source pin index out of range"); return nullptr;
    }
    if (dstPinIdx < 0 || dstPinIdx >= (int)dstNode.pinsIn.size()) {
        PyErr_SetString(PyExc_IndexError, "Destination pin index out of range"); return nullptr;
    }
    g_currentGraph->addLink(srcNode.pinsOut[srcPinIdx].id, dstNode.pinsIn[dstPinIdx].id);
    return PyLong_FromLong((long)(g_currentGraph->links.size() - 1));
}

// Create a group node. Returns node index.
static PyObject* py_add_group(PyObject*, PyObject* args) {
    const char* name = "Group";
    float x = 50, y = 50;
    if (!PyArg_ParseTuple(args, "|sff", &name, &x, &y)) return nullptr;
    if (!g_currentGraph) Py_RETURN_NONE;
    g_currentGraph->createGroup(name, {x, y});
    g_currentGraph->dirty = true;
    return PyLong_FromLong((long)(g_currentGraph->nodes.size() - 1));
}

// Add a node to a group: add_to_group(group_idx, child_idx)
static PyObject* py_add_to_group(PyObject*, PyObject* args) {
    int groupIdx, childIdx;
    if (!PyArg_ParseTuple(args, "ii", &groupIdx, &childIdx)) return nullptr;
    if (!g_currentGraph || groupIdx < 0 || groupIdx >= (int)g_currentGraph->nodes.size() ||
        childIdx < 0 || childIdx >= (int)g_currentGraph->nodes.size()) {
        PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
    }
    g_currentGraph->addToGroup(g_currentGraph->nodes[groupIdx].id,
                                g_currentGraph->nodes[childIdx].id);
    Py_RETURN_NONE;
}

// Set the beat offset for a child node in a group: set_beat_offset(node_idx, beats)
static PyObject* py_set_beat_offset(PyObject*, PyObject* args) {
    int nodeIdx;
    float offset;
    if (!PyArg_ParseTuple(args, "if", &nodeIdx, &offset)) return nullptr;
    if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
        PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
    }
    g_currentGraph->nodes[nodeIdx].groupBeatOffset = offset;
    g_currentGraph->dirty = true;
    Py_RETURN_NONE;
}

// Get/set the beat offset: get_beat_offset(node_idx)
static PyObject* py_get_beat_offset(PyObject*, PyObject* args) {
    int nodeIdx;
    if (!PyArg_ParseTuple(args, "i", &nodeIdx)) return nullptr;
    if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
        PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
    }
    return PyFloat_FromDouble(g_currentGraph->nodes[nodeIdx].groupBeatOffset);
}

// Add a marker: add_marker(name, beat)
static PyObject* py_add_marker(PyObject*, PyObject* args) {
    const char* name;
    float beat;
    if (!PyArg_ParseTuple(args, "sf", &name, &beat)) return nullptr;
    if (!g_currentGraph) Py_RETURN_NONE;
    // Remove existing marker with same name
    auto& markers = g_currentGraph->markers;
    markers.erase(std::remove_if(markers.begin(), markers.end(),
        [name](auto& m) { return m.name == name; }), markers.end());
    Marker m;
    m.id = g_currentGraph->getNextId();
    m.name = name;
    m.beat = beat;
    markers.push_back(m);
    g_currentGraph->dirty = true;
    Py_RETURN_NONE;
}

// Get marker beat: get_marker(name) -> float or None
static PyObject* py_get_marker(PyObject*, PyObject* args) {
    const char* name;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;
    if (!g_currentGraph) Py_RETURN_NONE;
    float beat = g_currentGraph->resolveMarkerBeat(name);
    if (beat < 0) Py_RETURN_NONE;
    return PyFloat_FromDouble(beat);
}

// List all markers: list_markers() -> [(name, beat), ...]
static PyObject* py_list_markers(PyObject*, PyObject*) {
    if (!g_currentGraph) return PyList_New(0);
    auto& markers = g_currentGraph->markers;
    auto* list = PyList_New(markers.size());
    for (int i = 0; i < (int)markers.size(); ++i) {
        auto* tup = PyTuple_New(2);
        PyTuple_SetItem(tup, 0, PyUnicode_FromString(markers[i].name.c_str()));
        PyTuple_SetItem(tup, 1, PyFloat_FromDouble(markers[i].beat));
        PyList_SetItem(list, i, tup);
    }
    return list;
}

// Anchor a node to a marker: anchor_to_marker(node_idx, marker_name)
static PyObject* py_anchor_to_marker(PyObject*, PyObject* args) {
    int nodeIdx;
    const char* markerName;
    if (!PyArg_ParseTuple(args, "is", &nodeIdx, &markerName)) return nullptr;
    if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
        PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
    }
    g_currentGraph->nodes[nodeIdx].anchorMarker = markerName;
    g_currentGraph->dirty = true;
    Py_RETURN_NONE;
}

// Remove anchor: clear_anchor(node_idx)
static PyObject* py_clear_anchor(PyObject*, PyObject* args) {
    int nodeIdx;
    if (!PyArg_ParseTuple(args, "i", &nodeIdx)) return nullptr;
    if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
        PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
    }
    g_currentGraph->nodes[nodeIdx].anchorMarker.clear();
    Py_RETURN_NONE;
}

// Set envelope curve expression: set_env_curve(node_idx, stage, expression)
// stage: "attack", "decay", "release"
static PyObject* py_set_env_curve(PyObject*, PyObject* args) {
    int nodeIdx;
    const char* stage;
    const char* expr;
    if (!PyArg_ParseTuple(args, "iss", &nodeIdx, &stage, &expr)) return nullptr;
    if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
        PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
    }
    auto& n = g_currentGraph->nodes[nodeIdx];
    std::string s(stage);
    if (s == "attack") n.envAttackCurve = expr;
    else if (s == "decay") n.envDecayCurve = expr;
    else if (s == "release") n.envReleaseCurve = expr;
    else { PyErr_SetString(PyExc_ValueError, "Stage must be 'attack', 'decay', or 'release'"); return nullptr; }
    g_currentGraph->dirty = true;
    Py_RETURN_NONE;
}

// Add automation point: add_automation(node_idx, param_idx, beat, value)
static PyObject* py_add_automation(PyObject*, PyObject* args) {
    int nodeIdx, paramIdx;
    float beat, value;
    if (!PyArg_ParseTuple(args, "iiff", &nodeIdx, &paramIdx, &beat, &value)) return nullptr;
    if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
        PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
    }
    auto& node = g_currentGraph->nodes[nodeIdx];
    if (paramIdx < 0 || paramIdx >= (int)node.params.size()) {
        PyErr_SetString(PyExc_IndexError, "Param index out of range"); return nullptr;
    }
    auto& lane = node.params[paramIdx].automation;
    lane.points.push_back({beat, value});
    std::sort(lane.points.begin(), lane.points.end(),
        [](auto& a, auto& b) { return a.beat < b.beat; });
    g_currentGraph->dirty = true;
    Py_RETURN_NONE;
}

// Clear automation: clear_automation(node_idx, param_idx)
static PyObject* py_clear_automation(PyObject*, PyObject* args) {
    int nodeIdx, paramIdx;
    if (!PyArg_ParseTuple(args, "ii", &nodeIdx, &paramIdx)) return nullptr;
    if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
        PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
    }
    auto& node = g_currentGraph->nodes[nodeIdx];
    if (paramIdx < 0 || paramIdx >= (int)node.params.size()) {
        PyErr_SetString(PyExc_IndexError, "Param index out of range"); return nullptr;
    }
    node.params[paramIdx].automation.points.clear();
    g_currentGraph->dirty = true;
    Py_RETURN_NONE;
}

// Insert time: insert_time(at_beat, duration, [node_idx=-1])
static PyObject* py_insert_time(PyObject*, PyObject* args) {
    float atBeat, duration;
    int nodeIdx = -1;
    if (!PyArg_ParseTuple(args, "ff|i", &atBeat, &duration, &nodeIdx)) return nullptr;
    if (!g_currentGraph) Py_RETURN_NONE;
    int nodeId = -1;
    if (nodeIdx >= 0 && nodeIdx < (int)g_currentGraph->nodes.size())
        nodeId = g_currentGraph->nodes[nodeIdx].id;
    g_currentGraph->insertTime(atBeat, duration, nodeId);
    Py_RETURN_NONE;
}

// Delete time: delete_time(from_beat, to_beat, [node_idx=-1])
static PyObject* py_delete_time(PyObject*, PyObject* args) {
    float fromBeat, toBeat;
    int nodeIdx = -1;
    if (!PyArg_ParseTuple(args, "ff|i", &fromBeat, &toBeat, &nodeIdx)) return nullptr;
    if (!g_currentGraph) Py_RETURN_NONE;
    int nodeId = -1;
    if (nodeIdx >= 0 && nodeIdx < (int)g_currentGraph->nodes.size())
        nodeId = g_currentGraph->nodes[nodeIdx].id;
    g_currentGraph->deleteTime(fromBeat, toBeat, nodeId);
    Py_RETURN_NONE;
}

// Set BPM
static PyObject* py_set_bpm(PyObject*, PyObject* args) {
    float bpm;
    if (!PyArg_ParseTuple(args, "f", &bpm)) return nullptr;
    if (g_currentGraph) g_currentGraph->bpm = bpm;
    Py_RETURN_NONE;
}

static PyMethodDef soundshopMethods[] = {
    {"this_node", py_this_node, METH_NOARGS, "Get active node index (-1 if not in track context)"},
    {"get_node_count", py_get_node_count, METH_NOARGS, "Get number of nodes"},
    {"get_node_names", py_get_node_names, METH_NOARGS, "Get list of node names"},
    {"get_node", py_get_node, METH_VARARGS, "Get node info by index or name"},
    {"find_node", py_find_node, METH_VARARGS, "Find node by name (case-insensitive substring). Returns index if one match, list if multiple"},
    {"add_midi_track", py_add_midi_track, METH_VARARGS, "Create MIDI track: ([name, x, y]). Returns node index"},
    {"add_audio_track", py_add_audio_track, METH_VARARGS, "Create audio track: ([name, x, y]). Returns node index"},
    {"add_clip", py_add_clip, METH_VARARGS, "Add clip: (node_idx, [start_beat, length_beats, name]). Returns clip index"},
    {"add_link", py_add_link, METH_VARARGS, "Connect nodes: (src_node_idx, dst_node_idx, [src_pin, dst_pin])"},
    {"add_group", py_add_group, METH_VARARGS, "Create group: ([name, x, y]). Returns node index"},
    {"add_to_group", py_add_to_group, METH_VARARGS, "Add node to group: (group_idx, child_idx)"},
    {"set_beat_offset", py_set_beat_offset, METH_VARARGS, "Set child beat offset: (node_idx, beats)"},
    {"get_beat_offset", py_get_beat_offset, METH_VARARGS, "Get child beat offset: (node_idx)"},
    {"add_marker", py_add_marker, METH_VARARGS, "Add/update marker: (name, beat)"},
    {"get_marker", py_get_marker, METH_VARARGS, "Get marker beat: (name) -> float or None"},
    {"list_markers", py_list_markers, METH_NOARGS, "List all markers: -> [(name, beat), ...]"},
    {"anchor_to_marker", py_anchor_to_marker, METH_VARARGS, "Anchor node to marker: (node_idx, marker_name)"},
    {"clear_anchor", py_clear_anchor, METH_VARARGS, "Remove marker anchor: (node_idx)"},
    {"set_env_curve", py_set_env_curve, METH_VARARGS, "Set envelope curve: (node_idx, 'attack'|'decay'|'release', expression)"},
    {"add_automation", py_add_automation, METH_VARARGS, "Add automation point: (node_idx, param_idx, beat, value)"},
    {"clear_automation", py_clear_automation, METH_VARARGS, "Clear automation: (node_idx, param_idx)"},
    {"insert_time", py_insert_time, METH_VARARGS, "Insert time: (at_beat, duration, [node_idx=-1 for all])"},
    {"delete_time", py_delete_time, METH_VARARGS, "Delete time: (from_beat, to_beat, [node_idx=-1 for all])"},
    {"add_note", py_add_note, METH_VARARGS, "Add note: (node_idx, clip_idx, pitch, offset, duration, [velocity=100])"},
    {"clear_notes", py_clear_notes, METH_VARARGS, "Clear notes: (node_idx, clip_idx)"},
    {"set_note", py_set_note, METH_VARARGS, "Set note: (node_idx, clip_idx, note_idx, pitch, offset, duration, detune)"},
    {"add_cc", py_add_cc, METH_VARARGS, "Add CC event: (node_idx, clip_idx, cc_num, offset, value, [channel])"},
    {"map_cc", py_map_cc, METH_VARARGS, "Map MIDI CC to param: (midi_ch, cc_num, node_idx, param_idx, [min, max])"},
    {"set_audio_file", py_set_audio_file, METH_VARARGS, "Set audio file: (node_idx, clip_idx, path)"},
    {"set_performance_mode", [](PyObject*, PyObject* args) -> PyObject* {
        int nodeIdx, enabled;
        if (!PyArg_ParseTuple(args, "ii", &nodeIdx, &enabled)) return nullptr;
        if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
            PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
        }
        g_currentGraph->nodes[nodeIdx].performanceMode = (enabled != 0);
        Py_RETURN_NONE;
    }, METH_VARARGS, "Enable/disable performance mode: (node_idx, 0/1)"},
    {"set_performance_release", [](PyObject*, PyObject* args) -> PyObject* {
        int nodeIdx, mode;
        if (!PyArg_ParseTuple(args, "ii", &nodeIdx, &mode)) return nullptr;
        if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
            PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
        }
        g_currentGraph->nodes[nodeIdx].performanceReleaseMode = mode;
        Py_RETURN_NONE;
    }, METH_VARARGS, "Set release mode: (node_idx, 0=on key up, 1=legato)"},
    {"set_performance_velocity", [](PyObject*, PyObject* args) -> PyObject* {
        int nodeIdx, enabled;
        if (!PyArg_ParseTuple(args, "ii", &nodeIdx, &enabled)) return nullptr;
        if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
            PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
        }
        g_currentGraph->nodes[nodeIdx].performanceVelocity = (enabled != 0);
        Py_RETURN_NONE;
    }, METH_VARARGS, "Set velocity sensitivity: (node_idx, 0=fixed, 1=from keyboard)"},
    {"get_bpm", py_get_bpm, METH_NOARGS, "Get BPM"},
    {"set_bpm", py_set_bpm, METH_VARARGS, "Set BPM"},
    {"set_tuning", [](PyObject*, PyObject* args) -> PyObject* {
        float hz;
        if (!PyArg_ParseTuple(args, "f", &hz)) return nullptr;
        MusicTheory::setReferencePitch(hz);
        Py_RETURN_NONE;
    }, METH_VARARGS, "Set reference pitch in Hz (440=standard, 432=Verdi)"},
    {"set_tuning_standard", [](PyObject*, PyObject*) -> PyObject* {
        MusicTheory::setStandardTuning(); Py_RETURN_NONE;
    }, METH_NOARGS, "Set A=440 Hz standard tuning"},
    {"set_tuning_verdi", [](PyObject*, PyObject*) -> PyObject* {
        MusicTheory::setVerdiTuning(); Py_RETURN_NONE;
    }, METH_NOARGS, "Set A=432 Hz Verdi tuning"},
    {"get_tuning", [](PyObject*, PyObject*) -> PyObject* {
        return PyFloat_FromDouble(MusicTheory::referencePitch);
    }, METH_NOARGS, "Get current reference pitch in Hz"},
    {nullptr, nullptr, 0, nullptr}
};

static PyModuleDef soundshopModule = {
    PyModuleDef_HEAD_INIT, "soundshop", "SoundShop project access", -1, soundshopMethods
};

static PyObject* PyInit_soundshop() {
    return PyModule_Create(&soundshopModule);
}

// ==============================================================================
// ScriptEngine implementation
// ==============================================================================

ScriptEngine::ScriptEngine() {}

ScriptEngine::~ScriptEngine() {
    shutdown();
}

bool ScriptEngine::init() {
    if (initialized) return true;

    // Register our module before initializing Python
    PyImport_AppendInittab("soundshop", &PyInit_soundshop);

    Py_Initialize();
    if (!Py_IsInitialized()) {
        fprintf(stderr, "Failed to initialize Python\n");
        return false;
    }

    initialized = true;

    // Add our scripts directory to Python path
    PyRun_SimpleString(
        "import sys, os\n"
        "exe_dir = os.path.dirname(os.path.abspath(sys.executable))\n"
        "scripts_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(exe_dir))), 'scripts')\n"
        "if os.path.isdir(scripts_dir): sys.path.insert(0, scripts_dir)\n"
        "# Also check relative to working directory\n"
        "for p in ['scripts', '../scripts', '../../scripts', 'cpp/scripts']:\n"
        "    if os.path.isdir(p): sys.path.insert(0, os.path.abspath(p))\n"
    );

    fprintf(stderr, "Python %s initialized\n", Py_GetVersion());
    return true;
}

void ScriptEngine::shutdown() {
    if (initialized) {
        Py_Finalize();
        initialized = false;
    }
}

std::string ScriptEngine::run(const std::string& code, NodeGraph& graph, int activeNodeIdx) {
    if (!initialized && !init()) return "Error: Python not initialized";

    g_currentGraph = &graph;
    g_activeNodeIndex = activeNodeIdx;

    // Redirect stdout/stderr to capture output
    std::string captureSetup = R"(
import sys, io
_soundshop_stdout = io.StringIO()
_soundshop_stderr = io.StringIO()
sys.stdout = _soundshop_stdout
sys.stderr = _soundshop_stderr
)";

    PyRun_SimpleString(captureSetup.c_str());

    // Run user code
    int result = PyRun_SimpleString(code.c_str());

    // Capture output
    std::string output;
    PyObject* mainModule = PyImport_AddModule("__main__");
    PyObject* mainDict = PyModule_GetDict(mainModule);

    PyObject* stdoutObj = PyDict_GetItemString(mainDict, "_soundshop_stdout");
    if (stdoutObj) {
        PyObject* val = PyObject_CallMethod(stdoutObj, "getvalue", nullptr);
        if (val) {
            const char* str = PyUnicode_AsUTF8(val);
            if (str) output += str;
            Py_DECREF(val);
        }
    }

    PyObject* stderrObj = PyDict_GetItemString(mainDict, "_soundshop_stderr");
    if (stderrObj) {
        PyObject* val = PyObject_CallMethod(stderrObj, "getvalue", nullptr);
        if (val) {
            const char* str = PyUnicode_AsUTF8(val);
            if (str && strlen(str) > 0) {
                output += "\n--- Errors ---\n";
                output += str;
            }
            Py_DECREF(val);
        }
    }

    // Restore stdout/stderr
    PyRun_SimpleString("sys.stdout = sys.__stdout__\nsys.stderr = sys.__stderr__\n");

    g_currentGraph = nullptr;

    if (result != 0 && output.empty())
        output = "Script execution failed";

    return output;
}

std::vector<ScriptEngine::SignalValue> ScriptEngine::evaluateSignals(
        int sample, int sampleRate, int blockSize) {
    std::vector<SignalValue> results;
    if (!initialized) return results;

    // Call into Python to evaluate bound signals
    // This runs the soundshop_signals.get_bindings() and evaluates each
    char code[512];
    snprintf(code, sizeof(code),
        "import soundshop_signals as _sig\n"
        "_sig_results = []\n"
        "_ctx = _sig.EvalContext(sample_rate=%d, block_size=%d)\n"
        "_cache = _sig.ControlCache()\n"
        "for _s, _ni, _pi in _sig.get_bindings():\n"
        "    _v = _s.at(%d, _ctx, _cache)\n"
        "    _sig_results.append((_ni, _pi, _v))\n",
        sampleRate, blockSize, sample);

    PyRun_SimpleString(code);

    // Extract results
    PyObject* mainModule = PyImport_AddModule("__main__");
    PyObject* mainDict = PyModule_GetDict(mainModule);
    PyObject* resultList = PyDict_GetItemString(mainDict, "_sig_results");

    if (resultList && PyList_Check(resultList)) {
        Py_ssize_t n = PyList_Size(resultList);
        for (Py_ssize_t i = 0; i < n; ++i) {
            PyObject* tuple = PyList_GetItem(resultList, i);
            if (tuple && PyTuple_Check(tuple) && PyTuple_Size(tuple) == 3) {
                int ni = (int)PyLong_AsLong(PyTuple_GetItem(tuple, 0));
                int pi = (int)PyLong_AsLong(PyTuple_GetItem(tuple, 1));
                float v = (float)PyFloat_AsDouble(PyTuple_GetItem(tuple, 2));
                results.push_back({ni, pi, v});
            }
        }
    }

    return results;
}

} // namespace SoundShop

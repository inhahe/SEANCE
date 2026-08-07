// =============================================================================
// Python embedding - OPTIONAL.
//
// Everything that touches the CPython C API lives under HAS_PYTHON. When SEANCE
// is built without Python (the find_package / hardcoded-path lookup in
// CMakeLists.txt failed), this file compiles to stubs and Python scripting is
// simply disabled - the build and the app still work.
//
// Even WITH HAS_PYTHON, the Python DLL is delay-loaded on Windows, so it may be
// absent at runtime. ScriptEngine::pythonAvailable() probes for it (and is the
// gate every entry point checks) so we never trigger the delay-load helper for
// a missing DLL. See CMakeLists.txt for the /DELAYLOAD wiring.
// =============================================================================
#ifdef HAS_PYTHON

#define PY_SSIZE_T_CLEAN
// Python's pyconfig.h auto-selects python3XY_d.lib and enables Py_REF_DEBUG
// refcount tracing (which references debug-only symbols) whenever _DEBUG is
// defined. Standard Python installs don't ship the debug lib or those
// symbols, so undefine _DEBUG around the Python.h include and restore it.
#ifdef _DEBUG
#  define SOUNDSHOP_RESTORE_DEBUG
#  undef _DEBUG
#endif
#include <Python.h>
#ifdef SOUNDSHOP_RESTORE_DEBUG
#  define _DEBUG
#  undef SOUNDSHOP_RESTORE_DEBUG
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#endif // HAS_PYTHON

#include "scripting.h"
#include "music_theory.h"
#include "waveform_bank.h"
#include "warp.h"
#include "buffer_warp.h"
#include <juce_core/juce_core.h>   // juce::Logger for full-traceback logging
#include <sstream>
#include <cstdio>
#include <cstring>
#include <cctype>

// Bare DLL filename used for the runtime load probe (passed by CMake). Falls
// back to the 3.14 name if the build didn't define it.
#ifndef PYTHON_DLL_NAME
#define PYTHON_DLL_NAME "python314.dll"
#endif

namespace SoundShop {

// -----------------------------------------------------------------------------
// pythonAvailable() - defined in ALL builds so callers (shape_expr.cpp, the UI
// combos) can gate Python-only features uniformly.
// -----------------------------------------------------------------------------
bool ScriptEngine::pythonAvailable() {
#ifdef HAS_PYTHON
  #ifdef _WIN32
    // Delay-loaded: probe whether the DLL can be found before any C-API call.
    // Cache the result (the answer can't change within a process run). We keep
    // the handle loaded so the subsequent delay-load resolves the same module.
    static int cached = -1;
    if (cached < 0) {
        HMODULE h = LoadLibraryA(PYTHON_DLL_NAME);
        cached = h ? 1 : 0;
    }
    return cached == 1;
  #else
    // Non-Windows: Python is linked normally (no delay-load), so if HAS_PYTHON
    // is defined the interpreter is present.
    return true;
  #endif
#else
    return false;
#endif
}

#ifdef HAS_PYTHON

// Global pointer so Python callbacks can access the graph
static NodeGraph* g_currentGraph = nullptr;
static int g_activeNodeIndex = -1; // set when running script from a track context

// Convert a Python pitch argument to a MIDI note number. Accepts an int / float
// (used as the raw MIDI number) OR a note-name string like "C4", "c#4", "Bb3"
// (a name with no octave defaults to octave 4). On a string that doesn't parse,
// sets a Python ValueError and returns false; on a wrong type, a TypeError.
static bool pyPitchToInt(PyObject* o, int& out) {
    if (PyLong_Check(o))  { out = (int)PyLong_AsLong(o);      return true; }
    if (PyFloat_Check(o)) { out = (int)PyFloat_AsDouble(o);   return true; }
    if (PyUnicode_Check(o)) {
        const char* s = PyUnicode_AsUTF8(o);
        int n = MusicTheory::parseNoteName(s ? s : "");
        if (n < 0) { PyErr_Format(PyExc_ValueError, "invalid note name '%s'", s ? s : ""); return false; }
        out = n; return true;
    }
    PyErr_SetString(PyExc_TypeError, "pitch must be an int or a note-name string like 'C4'");
    return false;
}

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

// Get node by index or name - returns dict with node info
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
    PyObject* pitchObj = nullptr;
    if (!PyArg_ParseTuple(args, "iiOff|i", &nodeIdx, &clipIdx, &pitchObj, &offset, &duration, &velocity))
        return nullptr;
    if (!pyPitchToInt(pitchObj, pitch)) return nullptr;
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
    PyObject* pitchObj = nullptr;
    if (!PyArg_ParseTuple(args, "iiiOfff", &nodeIdx, &clipIdx, &noteIdx, &pitchObj, &offset, &duration, &detune))
        return nullptr;
    if (!pyPitchToInt(pitchObj, pitch)) return nullptr;
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
    // Store mapping - will be applied through automation manager
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

// Find node by name - returns index, or list of indices if multiple match
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
        {Pin{0, "MIDI In", PinKind::Midi, true}},
        {Pin{0, "MIDI", PinKind::Midi, false}}, {x, y});
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

    // Same helper the toolbar and canvas menu use, so a scripted track is armable
    // for recording like a hand-made one (it used to get neither the Audio In pin
    // nor the recording params).
    g_currentGraph->addAudioTrack(name, {x, y});
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
    m.id = g_currentGraph->allocId();
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
    // The node's shared AHDSR envelope is the single source of truth for
    // every tonal synth's amplitude envelope; route scripted curve edits into
    // it (as Equation-mode expressions) so they actually take effect.
    auto setCurve = [&](SpectralCurve& c) {
        c.mode = SpectralCurve::Equation;
        c.expression = expr;
        c.freehandMode = false;
    };
    if (s == "attack") setCurve(n.ahdsrEnvelope.attackCurve);
    else if (s == "decay") setCurve(n.ahdsrEnvelope.decayCurve);
    else if (s == "release") setCurve(n.ahdsrEnvelope.releaseCurve);
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

// notenum("C4") -> 60   |   notenum("C", 4) -> 60   |   notenum(60) -> 60
// Returns -1 on a parse error / out-of-range result.
static PyObject* py_notenum(PyObject*, PyObject* args) {
    PyObject* o = nullptr; int octave = 0; bool haveOct = false;
    if (PyTuple_Size(args) >= 2) {
        const char* pc = nullptr;
        if (!PyArg_ParseTuple(args, "si", &pc, &octave)) return nullptr;
        return PyLong_FromLong(MusicTheory::noteNumber(pc ? pc : "", octave));
    }
    (void)haveOct;
    if (!PyArg_ParseTuple(args, "O", &o)) return nullptr;
    int n;
    if (!pyPitchToInt(o, n)) return nullptr;
    return PyLong_FromLong(n);
}

// notename(60) -> "C4". Accepts a note name too (round-trips via its number).
static PyObject* py_notename(PyObject*, PyObject* args) {
    PyObject* o = nullptr;
    if (!PyArg_ParseTuple(args, "O", &o)) return nullptr;
    int n;
    if (!pyPitchToInt(o, n)) return nullptr;
    if (n < 0 || n > 127) { PyErr_SetString(PyExc_ValueError, "note out of MIDI range 0..127"); return nullptr; }
    return PyUnicode_FromString(MusicTheory::noteName(n).c_str());
}

// notefreq("C4") / notefreq(60) / notefreq("C", 4) -> Hz using the PROJECT
// tuning system (Equal12 / Pythagorean / Just / Meantone) and concert pitch.
static PyObject* py_notefreq(PyObject*, PyObject* args) {
    int n = -1;
    if (PyTuple_Size(args) >= 2) {
        const char* pc = nullptr; int octave = 0;
        if (!PyArg_ParseTuple(args, "si", &pc, &octave)) return nullptr;
        n = MusicTheory::noteNumber(pc ? pc : "", octave);
    } else {
        PyObject* o = nullptr;
        if (!PyArg_ParseTuple(args, "O", &o)) return nullptr;
        if (!pyPitchToInt(o, n)) return nullptr;
    }
    if (n < 0 || n > 127) { PyErr_SetString(PyExc_ValueError, "note out of MIDI range 0..127"); return nullptr; }
    TuningSystem tuning = g_currentGraph ? g_currentGraph->tuningSystem : TuningSystem::Equal12;
    float concert = g_currentGraph ? g_currentGraph->concertPitch : 440.0f;
    return PyFloat_FromDouble((double)midiNoteToFrequency(n, tuning, concert));
}

static PyMethodDef soundshopMethods[] = {
    {"notenum", py_notenum, METH_VARARGS, "Note name to MIDI number: notenum('C4') or notenum('C', 4) -> 60; passthrough for ints"},
    {"notename", py_notename, METH_VARARGS, "MIDI number to note name: notename(60) -> 'C4'"},
    {"notefreq", py_notefreq, METH_VARARGS, "Note to frequency (Hz) using the project tuning: notefreq('C4') or notefreq(60) or notefreq('C', 4)"},
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
    {"add_note", py_add_note, METH_VARARGS, "Add note: (node_idx, clip_idx, pitch, offset, duration, [velocity=100]). pitch is a MIDI number or a note name like 'C4'"},
    {"clear_notes", py_clear_notes, METH_VARARGS, "Clear notes: (node_idx, clip_idx)"},
    {"set_note", py_set_note, METH_VARARGS, "Set note: (node_idx, clip_idx, note_idx, pitch, offset, duration, detune). pitch is a MIDI number or a note name like 'C4'"},
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
    // Extended bindings (#11): instrument-specific and project-level APIs.
    {"set_param", [](PyObject*, PyObject* args) -> PyObject* {
        int nodeIdx, paramIdx; float val;
        if (!PyArg_ParseTuple(args, "iif", &nodeIdx, &paramIdx, &val)) return nullptr;
        if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
            PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
        }
        auto& node = g_currentGraph->nodes[nodeIdx];
        if (paramIdx >= 0 && paramIdx < (int)node.params.size())
            node.params[paramIdx].value = val;
        Py_RETURN_NONE;
    }, METH_VARARGS, "Set param value: (node_idx, param_idx, value)"},
    {"get_param", [](PyObject*, PyObject* args) -> PyObject* {
        int nodeIdx, paramIdx;
        if (!PyArg_ParseTuple(args, "ii", &nodeIdx, &paramIdx)) return nullptr;
        if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
            PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
        }
        auto& node = g_currentGraph->nodes[nodeIdx];
        if (paramIdx >= 0 && paramIdx < (int)node.params.size())
            return PyFloat_FromDouble(node.params[paramIdx].value);
        Py_RETURN_NONE;
    }, METH_VARARGS, "Get param value: (node_idx, param_idx) -> float"},
    {"get_param_count", [](PyObject*, PyObject* args) -> PyObject* {
        int nodeIdx;
        if (!PyArg_ParseTuple(args, "i", &nodeIdx)) return nullptr;
        if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
            PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
        }
        return PyLong_FromLong((int)g_currentGraph->nodes[nodeIdx].params.size());
    }, METH_VARARGS, "Get param count: (node_idx) -> int"},
    {"get_param_name", [](PyObject*, PyObject* args) -> PyObject* {
        int nodeIdx, paramIdx;
        if (!PyArg_ParseTuple(args, "ii", &nodeIdx, &paramIdx)) return nullptr;
        if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
            PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
        }
        auto& node = g_currentGraph->nodes[nodeIdx];
        if (paramIdx >= 0 && paramIdx < (int)node.params.size())
            return PyUnicode_FromString(node.params[paramIdx].name.c_str());
        Py_RETURN_NONE;
    }, METH_VARARGS, "Get param name: (node_idx, param_idx) -> str"},
    {"set_script", [](PyObject*, PyObject* args) -> PyObject* {
        int nodeIdx; const char* script;
        if (!PyArg_ParseTuple(args, "is", &nodeIdx, &script)) return nullptr;
        if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
            PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
        }
        // Synchronised write: this can target a live node whose processor
        // polls script on the audio thread (see setNodeScriptSynced).
        setNodeScriptSynced(g_currentGraph->nodes[nodeIdx], script);
        Py_RETURN_NONE;
    }, METH_VARARGS, "Set node script: (node_idx, script_text)"},
    {"get_script", [](PyObject*, PyObject* args) -> PyObject* {
        int nodeIdx;
        if (!PyArg_ParseTuple(args, "i", &nodeIdx)) return nullptr;
        if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
            PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
        }
        return PyUnicode_FromString(g_currentGraph->nodes[nodeIdx].script.c_str());
    }, METH_VARARGS, "Get node script: (node_idx) -> str"},
    {"remove_node", [](PyObject*, PyObject* args) -> PyObject* {
        int nodeIdx;
        if (!PyArg_ParseTuple(args, "i", &nodeIdx)) return nullptr;
        if (!g_currentGraph || nodeIdx < 0 || nodeIdx >= (int)g_currentGraph->nodes.size()) {
            PyErr_SetString(PyExc_IndexError, "Node index out of range"); return nullptr;
        }
        int nodeId = g_currentGraph->nodes[nodeIdx].id;
        // Remove links connected to this node's pins.
        auto& nodes = g_currentGraph->nodes;
        auto& links = g_currentGraph->links;
        std::vector<int> pinIds;
        for (auto& p : nodes[nodeIdx].pinsIn) pinIds.push_back(p.id);
        for (auto& p : nodes[nodeIdx].pinsOut) pinIds.push_back(p.id);
        // Guard the structural edit against the audio callback iterating
        // graph.nodes/links (see node_graph.h mutationLock comment).
        std::lock_guard<std::recursive_mutex> graphLk(g_currentGraph->mutationLock);
        links.erase(std::remove_if(links.begin(), links.end(),
            [&pinIds](const Link& l) {
                for (int pid : pinIds) if (l.startPin == pid || l.endPin == pid) return true;
                return false;
            }), links.end());
        nodes.erase(nodes.begin() + nodeIdx);
        (void)nodeId;
        Py_RETURN_NONE;
    }, METH_VARARGS, "Remove node: (node_idx)"},
    {"set_song_length", [](PyObject*, PyObject* args) -> PyObject* {
        float beats;
        if (!PyArg_ParseTuple(args, "f", &beats)) return nullptr;
        if (g_currentGraph) g_currentGraph->songLengthBeats = beats;
        Py_RETURN_NONE;
    }, METH_VARARGS, "Set song length in beats"},
    {"set_song_repeat", [](PyObject*, PyObject* args) -> PyObject* {
        int mode, count = 1;
        if (!PyArg_ParseTuple(args, "i|i", &mode, &count)) return nullptr;
        if (g_currentGraph) {
            g_currentGraph->songRepeatMode = (NodeGraph::SongRepeat)mode;
            g_currentGraph->songRepeatCount = count;
        }
        Py_RETURN_NONE;
    }, METH_VARARGS, "Set repeat mode: (mode, [count]). mode: 0=None, 1=Forever, 2=NTimes"},
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

    // Gate the very first C-API call behind the DLL probe. With delay-loading,
    // calling PyImport_AppendInittab when the DLL is missing would trigger the
    // delay-load helper and crash; this returns false instead so callers
    // gracefully disable Python scripting.
    if (!pythonAvailable()) return false;

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
    if (!initialized && !init())
        return "Python is not available - install Python (matching the build's "
               "version) so its DLL can be found, then restart SEANCE. Python "
               "scripting is disabled until then.";

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

// =============================================================================
// Singleton + static-shape baking
// =============================================================================
ScriptEngine& ScriptEngine::instance() {
    static ScriptEngine engine;
    return engine;
}

// Count the newlines in a string (== number of complete lines when each line is
// '\n'-terminated). Used to locate where the user's source begins inside the
// generated bake program so error line numbers can be mapped back.
static int countNewlines(const std::string& s) {
    int n = 0;
    for (char c : s) if (c == '\n') ++n;
    return n;
}

// Build a user-facing error message from the current Python exception, mapping
// the generated-code line numbers back onto the user's OWN source lines.
//
// The bake wraps the user's program in generated scaffolding (imports, helper
// defs, a `def __shape(x):` / `def __cell(...)` header, a driver loop), so a raw
// Python line number points at machine-generated text the user never sees.
// `userLineOffset` is the number of generated lines that precede the user's
// first line; `userLineCount` is how many lines the user contributed. A
// traceback / SyntaxError line `g` is one of the user's iff
//   userLineOffset < g <= userLineOffset + userLineCount
// in which case the user-facing line is `g - userLineOffset` (1-based).
//
// The concise headline ("Type: message (line N)") is returned for the editor's
// error label; the FULL Python traceback (via traceback.format_exception) is
// always written to seance.log so the complete stack is recoverable even though
// the inline label only shows the headline.
static std::string formatPythonError(int userLineOffset, int userLineCount) {
    if (!PyErr_Occurred()) return {};
    PyObject *type = nullptr, *value = nullptr, *tb = nullptr;
    PyErr_Fetch(&type, &value, &tb);
    PyErr_NormalizeException(&type, &value, &tb);

    auto mapLine = [&](long g) -> long {
        if (g > userLineOffset && g <= (long)userLineOffset + userLineCount)
            return g - userLineOffset;
        return -1;
    };

    std::string typeName, msg;
    if (type) {
        if (PyObject* nm = PyObject_GetAttrString(type, "__name__")) {
            if (const char* c = PyUnicode_AsUTF8(nm)) typeName = c;
            Py_DECREF(nm);
        }
    }
    if (value) {
        if (PyObject* s = PyObject_Str(value)) {
            if (const char* c = PyUnicode_AsUTF8(s)) msg = c;
            Py_DECREF(s);
        }
    }

    long userLine = -1;

    // SyntaxError carries the offending line as an attribute (a compile failure
    // typically has no traceback frames to walk).
    if (value && PyObject_HasAttrString(value, "lineno")) {
        if (PyObject* lnO = PyObject_GetAttrString(value, "lineno")) {
            if (PyLong_Check(lnO)) userLine = mapLine(PyLong_AsLong(lnO));
            Py_DECREF(lnO);
        }
    }

    // Runtime error: walk the traceback for the DEEPEST frame within the user's
    // line range (their own offending call site). Attribute access (tb_lineno /
    // tb_next) keeps this ABI-stable across CPython versions.
    if (userLine < 0 && tb && tb != Py_None) {
        PyObject* t = tb; Py_INCREF(t);
        while (t && t != Py_None) {
            if (PyObject* lnO = PyObject_GetAttrString(t, "tb_lineno")) {
                if (PyLong_Check(lnO)) {
                    long m = mapLine(PyLong_AsLong(lnO));
                    if (m > 0) userLine = m;   // keep the deepest in-range frame
                }
                Py_DECREF(lnO);
            }
            PyObject* next = PyObject_GetAttrString(t, "tb_next"); // new ref; None at end
            Py_DECREF(t);
            t = next;
        }
        Py_XDECREF(t);
    }

    // Full traceback -> seance.log (line numbers are the generated ones, which is
    // fine for a developer reading the log; the inline headline is what's remapped).
    if (PyObject* tbmod = PyImport_ImportModule("traceback")) {
        if (PyObject* list = PyObject_CallMethod(tbmod, "format_exception", "OOO",
                                                 type  ? type  : Py_None,
                                                 value ? value : Py_None,
                                                 tb    ? tb    : Py_None)) {
            if (PySequence_Check(list)) {
                std::string full;
                Py_ssize_t n = PySequence_Size(list);
                for (Py_ssize_t i = 0; i < n; ++i) {
                    if (PyObject* it = PySequence_GetItem(list, i)) {
                        if (const char* c = PyUnicode_AsUTF8(it)) full += c;
                        Py_DECREF(it);
                    }
                }
                if (!full.empty())
                    juce::Logger::writeToLog("Python bake error:\n" + juce::String(full));
            }
            Py_DECREF(list);
        }
        Py_DECREF(tbmod);
        PyErr_Clear(); // format_exception shouldn't raise, but stay clean
    }

    Py_XDECREF(type); Py_XDECREF(value); Py_XDECREF(tb);
    PyErr_Clear();

    std::string out = typeName.empty() ? msg
                    : (msg.empty() ? typeName : typeName + ": " + msg);
    if (out.empty()) out = "Python error";
    if (userLine > 0) out += " (line " + std::to_string(userLine) + ")";
    return out;
}

// waveform(name, phase) - exposed to baked terrain/shape Python as a C builtin.
// Reads one of the ~4000 factory single-cycle waveforms at a normalised phase in
// [0,1) (wraps), linearly interpolated, returning the raw sample in [-1,1]. The
// first argument is the waveform NAME (case-insensitive) or a numeric entry
// index; an unknown name / out-of-range index reads as 0. Identical semantics to
// the Builtin/Lua/GLSL waveform() so every generator language reads the bank the
// same way. Registered into the run's globals by registerWaveformBuiltin().
static PyObject* py_waveform(PyObject* /*self*/, PyObject* args) {
    PyObject* nameObj = nullptr;
    double phase = 0.0;
    if (!PyArg_ParseTuple(args, "O|d", &nameObj, &phase)) return nullptr;
    auto& bank = WaveformBank::get();
    bank.ensureLoaded();
    int id = -1;
    if (PyUnicode_Check(nameObj)) {
        const char* nm = PyUnicode_AsUTF8(nameObj);
        id = bank.indexForName(nm ? nm : "");
    } else {
        id = (int)PyLong_AsLong(nameObj);
        if (PyErr_Occurred()) { PyErr_Clear(); id = (int)PyFloat_AsDouble(nameObj); }
        if (PyErr_Occurred()) { PyErr_Clear(); id = -1; }
    }
    return PyFloat_FromDouble((double)bank.sampleAtPhase(id, (float)phase));
}

static PyMethodDef kWaveformMethodDef = {
    "waveform", py_waveform, METH_VARARGS,
    "waveform(name, phase) -> factory single-cycle sample in [-1,1]"
};

// _ss_wfindex(name) -> stable entry index (int), or -1 for an unknown name. Backs
// the `waveforms` dict so a NAME resolves to the same integer id every language
// uses (and the id shown in the factory browser). The dict caches the result so
// each distinct name hits indexForName() only once.
//
// NOTE on the name: it must NOT use the `__name` (two leading underscores, no
// trailing) form, because the `waveforms` dict's __missing__ references it from
// INSIDE a class body, where Python private-name mangling would rewrite a
// `__wfindex` reference to `_WaveformDict__wfindex` and the global lookup would
// fail. A single leading underscore is immune to mangling.
static PyObject* py_waveform_index(PyObject* /*self*/, PyObject* args) {
    const char* nm = nullptr;
    if (!PyArg_ParseTuple(args, "s", &nm)) return nullptr;
    auto& bank = WaveformBank::get();
    bank.ensureLoaded();
    return PyLong_FromLong((long)bank.indexForName(nm ? nm : ""));
}

static PyMethodDef kWaveformIndexMethodDef = {
    "_ss_wfindex", py_waveform_index, METH_VARARGS,
    "_ss_wfindex(name) -> factory waveform entry index, or -1"
};

// Resolve a Python warp-method argument: a method NAME string ("softclip",
// "bend+", ...) via warpMethodFromName, or a numeric WarpMethod id. Shared by
// py_warpamp / py_warpphase.
static WarpMethod pyWarpMethodArg(PyObject* methodObj) {
    if (PyUnicode_Check(methodObj)) {
        const char* nm = PyUnicode_AsUTF8(methodObj);
        return warpMethodFromName(nm ? nm : "");
    }
    long v = PyLong_AsLong(methodObj);
    if (PyErr_Occurred()) { PyErr_Clear(); v = (long)PyFloat_AsDouble(methodObj); }
    if (PyErr_Occurred()) { PyErr_Clear(); v = 0; }
    return (WarpMethod)(int)v;
}

// warpamp(method, x, amount) / warpphase(method, phase, amount) - the wavetable
// shape-bending warps as pure scalar functions, routed to the SAME
// warpAmpValue/warpPhaseValue primitives the editor and synth voice use. `amount`
// is the 0..1 morph knob (0 = identity); unknown method = identity.
static PyObject* py_warpamp(PyObject* /*self*/, PyObject* args) {
    PyObject* methodObj = nullptr;
    double x = 0.0, amount = 0.0;
    if (!PyArg_ParseTuple(args, "Od|d", &methodObj, &x, &amount)) return nullptr;
    WarpMethod m = pyWarpMethodArg(methodObj);
    return PyFloat_FromDouble((double)warpAmpValue(m, (float)x, (float)amount));
}
static PyMethodDef kWarpAmpMethodDef = {
    "warpamp", py_warpamp, METH_VARARGS,
    "warpamp(method, x, amount) -> amplitude-domain warp of x in [-1,1]"
};

static PyObject* py_warpphase(PyObject* /*self*/, PyObject* args) {
    PyObject* methodObj = nullptr;
    double phase = 0.0, amount = 0.0;
    if (!PyArg_ParseTuple(args, "Od|d", &methodObj, &phase, &amount)) return nullptr;
    WarpMethod m = pyWarpMethodArg(methodObj);
    return PyFloat_FromDouble((double)warpPhaseValue(m, (float)phase, (float)amount));
}
static PyMethodDef kWarpPhaseMethodDef = {
    "warpphase", py_warpphase, METH_VARARGS,
    "warpphase(method, phase, amount) -> phase-domain read-position warp"
};

// --- Bucket C: representation-bound whole-buffer warps ----------------------
// spectralwarp(buf, method, amount) and waveletwarp(buf, method, amount,
// [filter], [levels]) take a sequence of samples, warp it in the FFT-magnitude /
// DWT-coefficient domain (see buffer_warp.h), and return a NEW list of the same
// length. Whole-buffer only, so they exist in the buffer-capable languages
// (Python / Lua / WASM), not the per-sample expression parser.
static bool pySeqToBuffer(PyObject* seq, std::vector<float>& out) {
    PyObject* fast = PySequence_Fast(seq, "expected a list/sequence of samples");
    if (!fast) return false;
    Py_ssize_t n = PySequence_Fast_GET_SIZE(fast);
    out.resize((size_t)n);
    for (Py_ssize_t i = 0; i < n; ++i)
        out[(size_t)i] = (float)PyFloat_AsDouble(PySequence_Fast_GET_ITEM(fast, i));
    Py_DECREF(fast);
    if (PyErr_Occurred()) return false;
    return true;
}
static PyObject* pyBufferToList(const std::vector<float>& buf) {
    PyObject* out = PyList_New((Py_ssize_t)buf.size());
    if (!out) return nullptr;
    for (size_t i = 0; i < buf.size(); ++i)
        PyList_SET_ITEM(out, (Py_ssize_t)i, PyFloat_FromDouble((double)buf[i]));
    return out;
}
static PyObject* py_spectralwarp(PyObject* /*self*/, PyObject* args) {
    PyObject* seq = nullptr; PyObject* methodObj = nullptr;
    double amount = 0.0;
    if (!PyArg_ParseTuple(args, "OO|d", &seq, &methodObj, &amount)) return nullptr;
    std::vector<float> buf;
    if (!pySeqToBuffer(seq, buf)) return nullptr;
    spectralWarpBuffer(buf, pyWarpMethodArg(methodObj), (float)amount);
    return pyBufferToList(buf);
}
static PyMethodDef kSpectralWarpMethodDef = {
    "spectralwarp", py_spectralwarp, METH_VARARGS,
    "spectralwarp(buf, method, amount) -> buffer warped in the FFT-magnitude domain"
};
static PyObject* py_waveletwarp(PyObject* /*self*/, PyObject* args) {
    PyObject* seq = nullptr; PyObject* methodObj = nullptr;
    double amount = 0.0;
    const char* filter = "db4";
    int levels = 5;
    if (!PyArg_ParseTuple(args, "OO|dsi", &seq, &methodObj, &amount, &filter, &levels))
        return nullptr;
    std::vector<float> buf;
    if (!pySeqToBuffer(seq, buf)) return nullptr;
    waveletWarpBuffer(buf, pyWarpMethodArg(methodObj), (float)amount,
                      filter ? filter : "db4", levels);
    return pyBufferToList(buf);
}
static PyMethodDef kWaveletWarpMethodDef = {
    "waveletwarp", py_waveletwarp, METH_VARARGS,
    "waveletwarp(buf, method, amount, filter='db4', levels=5) -> buffer warped in the DWT-coefficient domain"
};

// Bind waveform() and the _ss_wfindex helper into a run's globals dict. The
// `waveforms` dict itself is defined in pure Python by each baker's preamble
// (kWaveformsDictPreamble), which uses _ss_wfindex. Safe to call once per run
// right after the globals are created.
static void registerWaveformBuiltin(PyObject* globals) {
    PyObject* fn = PyCFunction_New(&kWaveformMethodDef, nullptr);
    if (fn) { PyDict_SetItemString(globals, "waveform", fn); Py_DECREF(fn); }
    PyObject* idx = PyCFunction_New(&kWaveformIndexMethodDef, nullptr);
    if (idx) { PyDict_SetItemString(globals, "_ss_wfindex", idx); Py_DECREF(idx); }
    PyObject* wa = PyCFunction_New(&kWarpAmpMethodDef, nullptr);
    if (wa) { PyDict_SetItemString(globals, "warpamp", wa); Py_DECREF(wa); }
    PyObject* wp = PyCFunction_New(&kWarpPhaseMethodDef, nullptr);
    if (wp) { PyDict_SetItemString(globals, "warpphase", wp); Py_DECREF(wp); }
    PyObject* sw = PyCFunction_New(&kSpectralWarpMethodDef, nullptr);
    if (sw) { PyDict_SetItemString(globals, "spectralwarp", sw); Py_DECREF(sw); }
    PyObject* ww = PyCFunction_New(&kWaveletWarpMethodDef, nullptr);
    if (ww) { PyDict_SetItemString(globals, "waveletwarp", ww); Py_DECREF(ww); }
}

// Pure-Python preamble defining the `waveforms` mapping: waveforms["name"] ->
// stable integer id, with the lookup cached so each name hashes through
// _ss_wfindex() (and thus indexForName) only once - every later access, even
// from a hot per-cell loop, is a plain dict hit. Resolve once and reuse the
// integer with waveform(id, phase) for the fastest path:
//   W = waveforms["AKWF sin"]      # one C lookup
//   ... waveform(W, phase) ...     # no per-call name hashing
// Appended verbatim into each terrain/shape program's source.
static const char* kWaveformsDictPreamble =
    "class __WaveformDict(dict):\n"
    "    def __missing__(self, k):\n"
    "        v = _ss_wfindex(k); self[k] = v; return v\n"
    "waveforms = __WaveformDict()\n";

// GLSL-parity scalar math, so the Python dialect exposes the same vocabulary as
// the Built-in, Lua, and GLSL shape languages. Must be emitted AFTER the base
// preamble (it relies on clamp/floor/sqrt being defined). atan(y) and atan(y,x)
// mirror GLSL's overload; mod uses GLSL's floored semantics, not Python's % on
// floats (which already floors, but we guard b==0). round = floor(x+0.5).
static const char* kGlslParityPyPreamble =
    "from math import atan as __atan1, atan2 as __atan2, sinh, cosh, "
    "radians, degrees, trunc\n"
    "def atan(y, x=None): return __atan2(y, x) if x is not None else __atan1(y)\n"
    "def sign(v): return 1.0 if v > 0 else (-1.0 if v < 0 else 0.0)\n"
    "def mod(a, b): return 0.0 if b == 0 else a - b * floor(a / b)\n"
    "def mix(a, b, t): return a + (b - a) * t\n"
    "def step(edge, x): return 0.0 if x < edge else 1.0\n"
    "def smoothstep(e0, e1, x):\n"
    "    t = clamp((x - e0) / (e1 - e0), 0.0, 1.0)\n"
    "    return t * t * (3 - 2 * t)\n"
    "def round(v): return floor(v + 0.5)\n"
    "def inversesqrt(v): return 1.0 / sqrt(v) if v > 0 else 0.0\n"
    // Remaining GLSL exponential + inverse-hyperbolic + common builtins. log2 is
    // safe (math.log2, Py3.3+); we wrap asinh/acosh/atanh with the same domain
    // guards as the Built-in parser; exp2/fma/roundEven are defined directly so
    // we don't depend on math.exp2 (3.11+) or math.fma (3.13+).
    "from math import log2 as __log2, asinh as __asinh, acosh as __acosh, "
    "atanh as __atanh\n"
    "def exp2(v): return 2.0 ** v\n"
    "def log2(v): return __log2(v) if v > 0 else 0.0\n"
    "def fma(a, b, c): return a * b + c\n"
    "def asinh(v): return __asinh(v)\n"
    "def acosh(v): return __acosh(v if v > 1.0 else 1.0)\n"
    "def atanh(v): return __atanh(clamp(v, -0.999999, 0.999999))\n"
    "def roundEven(v):\n"
    "    f = floor(v); d = v - f\n"
    "    if d < 0.5: return float(f)\n"
    "    if d > 0.5: return float(f + 1)\n"
    "    return float(f if f % 2 == 0 else f + 1)\n";

bool ScriptEngine::bakeShapeExpr(const std::string& src, bool domainRadians, int N,
                                 std::vector<float>& out, std::string& error) {
    const bool clampResult = domainRadians; // periodic shapes clamp to [-1,1]
    out.assign(N > 0 ? N : 0, 0.0f);
    error.clear();
    if (N <= 0) return true;

    if (!initialized && !init()) {
        error = "Python interpreter unavailable";
        return false;
    }

    // Wrap the user source into a function body. A bare expression (no newline,
    // no `return`) becomes `return (<expr>)`; a multi-line body is indented and
    // used verbatim (the user supplies the `return`).
    auto trim = [](const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace((unsigned char)s[a])) ++a;
        while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
        return s.substr(a, b - a);
    };
    std::string trimmed = trim(src);
    std::string body;
    // Non-periodic curves expose `f` as an alias of the normalized position `x`
    // so a curve written in terms of either name evaluates the same.
    if (!domainRadians) body = "    f = x\n";
    if (trimmed.find('\n') == std::string::npos &&
        trimmed.find("return") == std::string::npos) {
        body += "    return (" + trimmed + ")\n";
    } else {
        std::istringstream lines(trimmed);
        std::string ln;
        std::string user;
        while (std::getline(lines, ln)) user += "    " + ln + "\n";
        if (user.empty()) user = "    return 0.0\n";
        body += user;
    }

    std::ostringstream code;
    code << "from math import sin, cos, tan, sqrt, exp, log, floor, ceil, "
            "pi, e, tanh, atan, asin, acos\n"
            "import random as __ssr\n"
            "def pow(a, b): return a ** b\n"
            "def clamp(v, lo, hi): return lo if v < lo else (hi if v > hi else v)\n"
            "def fract(v): return v - floor(v)\n"
            "def saw(p):\n    p = p - floor(p)\n    return 2 * p - 1\n"
            "def square(p):\n    p = p - floor(p)\n    return 1.0 if p < 0.5 else -1.0\n"
            "def triangle(p):\n    p = p - floor(p)\n    return 4 * p - 1 if p < 0.5 else 3 - 4 * p\n"
            "def noise(v=0.0): return __ssr.random()\n"
         << kGlslParityPyPreamble
         << kWaveformsDictPreamble
         << "def __shape(x):\n";
    // The user's source begins here, inside `body`. Capture how many generated
    // lines precede it so a Python error line can be mapped back to the user's
    // own 1-based line (see formatPythonError). `body` may lead with an injected
    // `    f = x\n` line (non-radians) that is NOT user-authored.
    const int injected = domainRadians ? 0 : 1;          // injected body lines
    const int userLineOffset = countNewlines(code.str()) + injected;
    const int userLineCount  = countNewlines(body) - injected;
    code << body
         << "__N = " << N << "\n"
         << "if " << (clampResult ? "True" : "False") << ":\n"
            "    __result = [float(__shape(__i / __N * 2.0 * pi)) for __i in range(__N)]\n"
            "else:\n"
            "    __result = [float(__shape((__i / (__N - 1)) if __N > 1 else 0.0)) for __i in range(__N)]\n";

    PyObject* globals = PyDict_New();
    if (!globals) { error = "out of memory"; out.assign(N, 0.0f); return false; }
    PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
    registerWaveformBuiltin(globals);

    PyObject* res = PyRun_String(code.str().c_str(), Py_file_input, globals, globals);
    if (!res) {
        error = formatPythonError(userLineOffset, userLineCount);
        Py_DECREF(globals);
        out.assign(N, 0.0f);
        return false;
    }
    Py_DECREF(res);

    PyObject* list = PyDict_GetItemString(globals, "__result"); // borrowed
    bool ok = false;
    if (list && PyList_Check(list) && PyList_Size(list) == (Py_ssize_t)N) {
        ok = true;
        for (int i = 0; i < N; ++i) {
            PyObject* item = PyList_GetItem(list, i); // borrowed
            double v = item ? PyFloat_AsDouble(item) : 0.0;
            if (PyErr_Occurred()) { PyErr_Clear(); v = 0.0; }
            if (clampResult) v = v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v);
            out[i] = (float)v;
        }
    } else {
        error = "shape did not produce " + std::to_string(N) + " values";
    }
    Py_DECREF(globals);

    if (!ok) { out.assign(N, 0.0f); return false; }
    return true;
}

bool ScriptEngine::bakeTerrain(const std::string& src, bool wholeGrid,
                               const std::vector<int>& dims,
                               std::vector<float>& out, std::string& error) {
    out.clear();
    error.clear();
    if (dims.empty()) { error = "no dimensions given"; return false; }
    long long total = 1;
    for (int d : dims) {
        if (d < 1) { error = "every dimension must be >= 1"; return false; }
        total *= d;
        if (total > (1LL << 30)) { error = "terrain too large (> 1G cells)"; return false; }
    }
    if (!initialized && !init()) {
        error = "Python interpreter unavailable";
        return false;
    }

    auto trim = [](const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace((unsigned char)s[a])) ++a;
        while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
        return s.substr(a, b - a);
    };

    // Python literal for the dims list, e.g. "[7, 9, 3]".
    std::ostringstream dimsLit;
    dimsLit << "[";
    for (size_t i = 0; i < dims.size(); ++i) { if (i) dimsLit << ", "; dimsLit << dims[i]; }
    dimsLit << "]";

    // Shared preamble: the same math vocabulary the shape baker exposes.
    const char* kPreamble =
        "from math import sin, cos, tan, sqrt, exp, log, floor, ceil, "
        "pi, e, tanh, atan, asin, acos\n"
        "import random as __ssr\n"
        "def pow(a, b): return a ** b\n"
        "def clamp(v, lo, hi): return lo if v < lo else (hi if v > hi else v)\n"
        "def fract(v): return v - floor(v)\n"
        "def saw(p):\n    p = p - floor(p)\n    return 2 * p - 1\n"
        "def square(p):\n    p = p - floor(p)\n    return 1.0 if p < 0.5 else -1.0\n"
        "def triangle(p):\n    p = p - floor(p)\n    return 4 * p - 1 if p < 0.5 else 3 - 4 * p\n"
        "def noise(v=0.0): return __ssr.random()\n";

    std::ostringstream code;
    code << kPreamble
         << kGlslParityPyPreamble
         << kWaveformsDictPreamble
         << "__dims = " << dimsLit.str() << "\n"
         << "__nd = len(__dims)\n"
         << "__total = " << total << "\n";

    // Where the user's source lands inside the generated program, so a Python
    // error line can be mapped back to the user's 1-based line (see
    // formatPythonError). Set by whichever branch embeds the user source.
    int userLineOffset = 0, userLineCount = 0;

    if (wholeGrid) {
        // Whole-grid: expose dims/nd/total + the cell buffer, run the user's
        // generate(), then map the unipolar buffer to bipolar.
        //
        // Storage: when numpy is importable, the grid is a float64 ndarray
        // `grid` shaped exactly like the terrain (dims), so a program can write
        // `grid[r, c] = ...` or use vectorized numpy ops directly. Otherwise
        // `grid is None` and the cells live in a private flat list. EITHER way
        // the set/get/getAt/setAt helpers operate on the live grid, so a program
        // can mix numpy slicing with the helpers, and reassigning `grid` to a new
        // array (e.g. `grid = grid + 1`) is honoured at readback. Helpers keep the
        // [0,1] clamp; raw numpy writes are clamped once at readback via np.clip.
        code << "nd = __nd\n"
                "total = __total\n"
                "dims = list(__dims)\n"
                "try:\n"
                "    import numpy as __np\n"
                "except Exception:\n"
                "    __np = None\n"
                "if __np is not None:\n"
                "    grid = __np.zeros(tuple(__dims), dtype=__np.float64)\n"
                "    __store = None\n"
                "else:\n"
                "    grid = None\n"
                "    __store = [0.0] * __total\n"
                "def set(i, v):\n"
                "    v = float(v)\n"
                "    if v < 0.0: v = 0.0\n"
                "    elif v > 1.0: v = 1.0\n"
                "    if grid is None: __store[int(i)] = v\n"
                "    else: grid.flat[int(i)] = v\n"
                "def get(i):\n"
                "    return float(__store[int(i)] if grid is None else grid.flat[int(i)])\n"
                "def coord(i, axis):\n"
                "    __t = int(i)\n"
                "    for __a in range(__nd - 1, -1, -1):\n"
                "        __sz = __dims[__a]\n"
                "        __idx = __t % __sz\n"
                "        __t //= __sz\n"
                "        if __a == axis:\n"
                "            return (__idx / (__sz - 1)) if __sz > 1 else 0.0\n"
                "    return 0.0\n"
                // coordAxis(i, axis): INTEGER coordinate of cell i along axis -
                // the inverse companion to flatten(). Mirrors GLSL coordAxis().
                "def coordAxis(i, axis):\n"
                "    __t = int(i)\n"
                "    for __a in range(__nd - 1, -1, -1):\n"
                "        __sz = __dims[__a]\n"
                "        __idx = __t % __sz\n"
                "        __t //= __sz\n"
                "        if __a == axis:\n"
                "            return __idx\n"
                "    return 0\n"
                // flatten(*coords): row-major flat index from per-axis INTEGER
                // coordinates, each clamped to [0, dim-1]. Missing trailing args
                // count as 0; extras ignored. Inverse of coordAxis(); CPU twin of
                // the GLSL whole-grid flatten().
                "def flatten(*coords):\n"
                "    __idx = 0\n"
                "    for __a in range(__nd):\n"
                "        __sz = __dims[__a]\n"
                "        __c = int(coords[__a]) if __a < len(coords) else 0\n"
                "        if __c < 0: __c = 0\n"
                "        elif __c > __sz - 1: __c = __sz - 1\n"
                "        __idx = __idx * __sz + __c\n"
                "    return __idx\n"
                // neighbor(i, axis, delta): flat index delta steps from i along
                // axis, clamped to the edge. Read back with get(neighbor(...)).
                "def neighbor(i, axis, delta):\n"
                "    __t = int(i)\n"
                "    __co = [0] * __nd\n"
                "    for __a in range(__nd - 1, -1, -1):\n"
                "        __sz = __dims[__a]\n"
                "        __co[__a] = __t % __sz\n"
                "        __t //= __sz\n"
                "    __sz = __dims[axis]\n"
                "    __c = __co[axis] + int(delta)\n"
                "    if __c < 0: __c = 0\n"
                "    elif __c > __sz - 1: __c = __sz - 1\n"
                "    __co[axis] = __c\n"
                "    __idx = 0\n"
                "    for __a in range(__nd):\n"
                "        __idx = __idx * __dims[__a] + __co[__a]\n"
                "    return __idx\n"
                // getAt(*coords): DIRECT N-D read - the cell at per-axis INTEGER
                // coordinates, no manual flatten(). Each coord is EDGE-CLAMPED to
                // [0, dim-1] (reads past a border replicate the edge - the useful
                // default for stencils). Missing coords count as 0; extras ignored.
                "def getAt(*coords):\n"
                "    __idx = 0\n"
                "    for __a in range(__nd):\n"
                "        __sz = __dims[__a]\n"
                "        __c = int(coords[__a]) if __a < len(coords) else 0\n"
                "        if __c < 0: __c = 0\n"
                "        elif __c > __sz - 1: __c = __sz - 1\n"
                "        __idx = __idx * __sz + __c\n"
                "    return float(__store[__idx] if grid is None else grid.flat[__idx])\n"
                // setAt(c0, ..., v): DIRECT N-D write - store v (clamped [0,1]) at
                // the cell at the nd INTEGER coords; the value is the arg AFTER the
                // coords (args[__nd]). An OUT-OF-RANGE coord makes the write a
                // no-op (mirrors set), so an off-by-one never clobbers an edge cell.
                "def setAt(*args):\n"
                "    if len(args) < __nd + 1: return\n"
                "    __v = float(args[__nd])\n"
                "    __idx = 0\n"
                "    for __a in range(__nd):\n"
                "        __sz = __dims[__a]\n"
                "        __c = int(args[__a])\n"
                "        if __c < 0 or __c > __sz - 1: return\n"
                "        __idx = __idx * __sz + __c\n"
                "    if __v < 0.0: __v = 0.0\n"
                "    elif __v > 1.0: __v = 1.0\n"
                "    if grid is None: __store[__idx] = __v\n"
                "    else: grid.flat[__idx] = __v\n";
        // User source verbatim (defines generate()), then invoke it.
        std::string user = trim(src);
        if (user.empty()) user = "def generate():\n    pass";
        userLineOffset = countNewlines(code.str());     // generated lines before user
        userLineCount  = countNewlines(user) + 1;       // user occupies this many lines
        code << user << "\n"
             << "generate()\n"
                "if grid is None:\n"
                "    __result = [v * 2.0 - 1.0 for v in __store]\n"
                "else:\n"
                "    __result = __np.ascontiguousarray("
                "__np.clip(grid, 0.0, 1.0) * 2.0 - 1.0, dtype=__np.float64).reshape(-1)\n";
    } else {
        // Per-cell: wrap the user expression/body into __cell(...) and loop.
        std::string trimmed = trim(src);
        std::string body;
        if (trimmed.find('\n') == std::string::npos &&
            trimmed.find("return") == std::string::npos) {
            body = "    return (" + trimmed + ")\n";
        } else {
            std::istringstream lines(trimmed);
            std::string ln, user;
            while (std::getline(lines, ln)) user += "    " + ln + "\n";
            if (user.empty()) user = "    return 0.0\n";
            body = user;
        }
        code << "def __cell(c0, c1, c2, c3, c4, c5, c6, c7, x, y, z, w, nd):\n";
        userLineOffset = countNewlines(code.str());     // generated lines before body
        userLineCount  = countNewlines(body);           // body lines = user lines
        code << body
             << "__data = [0.0] * __total\n"
                "__TAU = 6.283185307179586\n"
                "for __i in range(__total):\n"
                "    __t = __i\n"
                "    __cc = [0.0] * 8\n"
                "    for __a in range(__nd - 1, -1, -1):\n"
                "        __sz = __dims[__a]\n"
                "        __idx = __t % __sz\n"
                "        __t //= __sz\n"
                "        __cc[__a] = (__idx / (__sz - 1)) if __sz > 1 else 0.0\n"
                "    c0, c1, c2, c3, c4, c5, c6, c7 = __cc\n"
                "    __v = float(__cell(c0, c1, c2, c3, c4, c5, c6, c7, "
                "c0 * __TAU, c1 * __TAU, c2 * __TAU, c3 * __TAU, __nd))\n"
                "    if __v < 0.0: __v = 0.0\n"
                "    elif __v > 1.0: __v = 1.0\n"
                "    __data[__i] = __v * 2.0 - 1.0\n"
                "__result = __data\n";
    }

    PyObject* globals = PyDict_New();
    if (!globals) { error = "out of memory"; return false; }
    PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
    registerWaveformBuiltin(globals);

    PyObject* res = PyRun_String(code.str().c_str(), Py_file_input, globals, globals);
    if (!res) {
        error = formatPythonError(userLineOffset, userLineCount);
        Py_DECREF(globals);
        return false;
    }
    Py_DECREF(res);

    PyObject* result = PyDict_GetItemString(globals, "__result"); // borrowed
    bool ok = false;
    if (result && PyList_Check(result) && PyList_Size(result) == (Py_ssize_t)total) {
        // Flat-list path (no numpy, or per-cell mode).
        ok = true;
        out.resize((size_t)total);
        for (long long i = 0; i < total; ++i) {
            PyObject* item = PyList_GetItem(result, (Py_ssize_t)i); // borrowed
            double v = item ? PyFloat_AsDouble(item) : 0.0;
            if (PyErr_Occurred()) { PyErr_Clear(); v = 0.0; }
            out[(size_t)i] = (float)v;
        }
    } else if (result && PyObject_CheckBuffer(result)) {
        // numpy path: __result is a C-contiguous float64 1-D ndarray. Read it
        // straight out of its buffer (no per-element Python calls).
        Py_buffer view;
        if (PyObject_GetBuffer(result, &view,
                               PyBUF_C_CONTIGUOUS | PyBUF_FORMAT) == 0) {
            if (view.itemsize == (Py_ssize_t)sizeof(double) &&
                view.format && std::strchr(view.format, 'd') &&
                view.len == (Py_ssize_t)(total * (long long)sizeof(double)) &&
                view.buf) {
                ok = true;
                out.resize((size_t)total);
                const double* d = (const double*)view.buf;
                for (long long i = 0; i < total; ++i) out[(size_t)i] = (float)d[i];
            }
            PyBuffer_Release(&view);
        } else {
            PyErr_Clear();
        }
    }
    if (!ok) {
        error = "program did not produce " + std::to_string(total) + " cells "
                "(whole-grid programs must define generate())";
    }
    Py_DECREF(globals);

    if (!ok) { out.clear(); return false; }
    return true;
}

#else // !HAS_PYTHON ----------------------------------------------------------
// Stub implementation for builds without Python. Every method is a no-op that
// reports unavailability so callers degrade gracefully. pythonAvailable() is
// defined above (it returns false in this branch).

static const char* kNoPython =
    "Python is not available in this build of SEANCE. Built-in expressions "
    "and Lua are still available.";

ScriptEngine::ScriptEngine() {}
ScriptEngine::~ScriptEngine() {}

bool ScriptEngine::init() { return false; }
void ScriptEngine::shutdown() {}

std::string ScriptEngine::run(const std::string&, NodeGraph&, int) {
    return kNoPython;
}

std::vector<ScriptEngine::SignalValue> ScriptEngine::evaluateSignals(int, int, int) {
    return {};
}

bool ScriptEngine::bakeShapeExpr(const std::string&, bool, int N,
                                 std::vector<float>& out, std::string& error) {
    out.assign(N > 0 ? N : 0, 0.0f);
    error = kNoPython;
    return false;
}

bool ScriptEngine::bakeTerrain(const std::string&, bool,
                               const std::vector<int>&,
                               std::vector<float>& out, std::string& error) {
    out.clear();
    error = kNoPython;
    return false;
}

ScriptEngine& ScriptEngine::instance() {
    static ScriptEngine engine;
    return engine;
}

#endif // HAS_PYTHON

} // namespace SoundShop

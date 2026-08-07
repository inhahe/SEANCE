cmake_minimum_required(VERSION 3.22)
project(SEANCE VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Check for optional dependencies and print status
message(STATUS "=== SEANCE Dependency Status ===")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libopenmpt_bin/lib/amd64/libopenmpt.lib")
    message(STATUS "  libopenmpt: FOUND")
else()
    message(STATUS "  libopenmpt: NOT FOUND (MOD import disabled)")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus/include/opus.h")
    message(STATUS "  libopus: FOUND")
else()
    message(STATUS "  libopus: NOT FOUND (Opus export disabled)")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wasm3/source/wasm3.h")
    message(STATUS "  wasm3: FOUND")
else()
    message(STATUS "  wasm3: NOT FOUND (WASM scripts disabled)")
endif()
message(STATUS "Run setup_dependencies.bat to download missing dependencies")
message(STATUS "====================================")

# ==============================================================================
# JUCE
# ==============================================================================
# Set JUCE_DIR to the path of your JUCE installation
if(NOT DEFINED JUCE_DIR)
    if(EXISTS "D:/JUCE-8.0.12")
        set(JUCE_DIR "D:/JUCE-8.0.12")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE")
        set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE")
    elseif(EXISTS "/usr/local/share/JUCE")
        set(JUCE_DIR "/usr/local/share/JUCE")
    else()
        message(FATAL_ERROR "JUCE not found. Set JUCE_DIR: cmake -DJUCE_DIR=/path/to/JUCE ..")
    endif()
endif()
message(STATUS "JUCE: ${JUCE_DIR}")
add_subdirectory(${JUCE_DIR} ${CMAKE_BINARY_DIR}/juce)

# ==============================================================================
# SEANCE
# ==============================================================================
juce_add_gui_app(SEANCE
    PRODUCT_NAME "SEANCE"
    COMPANY_NAME "SEANCE"
)

# Guard the dialog/taskbar rules (CLAUDE.md: a dialog must never earn its own
# Windows taskbar button). The failure mode this catches is silent - an
# AlertWindow subclass that overrides getDesktopWindowStyleFlags() compiles
# cleanly and then simply never shows its window - so it is worth a build error.
# Runs before SEANCE compiles, and on every build rather than only at configure
# time, since the thing it inspects is source text.
add_custom_target(seance_dialog_lint ALL
    COMMAND ${CMAKE_COMMAND}
            -DSEANCE_SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/src
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_dialog_patterns.cmake
    COMMENT "Checking dialog taskbar rules"
    VERBATIM
)
add_dependencies(SEANCE seance_dialog_lint)

target_sources(SEANCE PRIVATE
    src/main.cpp
    src/dialog_helpers.cpp
    src/dialog_helpers.h
    src/main_window.cpp
    src/main_window.h
    src/node_graph.cpp
    src/node_graph.h
    src/content_store.cpp
    src/content_store.h
    src/hash_util.cpp
    src/hash_util.h
    src/asset_library.cpp
    src/asset_library.h
    src/asset_import.cpp
    src/asset_import.h
    src/asset_library_component.cpp
    src/asset_library_component.h
    src/node_graph_component.cpp
    src/node_graph_component.h
    src/piano_roll_component.cpp
    src/piano_roll_component.h
    src/track_nesting_menu.h
    src/audio_engine.cpp
    src/audio_engine.h
    src/graph_processor.cpp
    src/graph_processor.h
    src/voice_nodes.h
    src/signal_oscillator.h
    src/voice_allocator.h
    src/poly_voice_processor.cpp
    src/poly_voice_processor.h
    src/plugin_host.cpp
    src/plugin_host.h
    src/plugin_settings.cpp
    src/plugin_settings.h
    src/project_file.cpp
    src/project_file.h
    src/music_theory.cpp
    src/music_theory.h
    src/transport.cpp
    src/transport.h
    src/undo.cpp
    src/undo.h
    src/piano_roll.h
    src/scripting.cpp
    src/scripting.h
    src/recording.cpp
    src/recording.h
    src/automation.cpp
    src/automation.h
    src/plugin_window.h
    src/melody_player.cpp
    src/melody_player.h
    src/wasm_script_processor.cpp
    src/wasm_script_processor.h
    src/script_runtime.h
    src/script_runtime.cpp
    src/script_runtime_builtin.cpp
    src/script_runtime_lua.cpp
    src/script_runtime_wasm.cpp
    src/builtin_synth.cpp
    src/builtin_synth.h
    src/terrain_synth.cpp
    src/terrain_synth.h
    src/glsl_compute.cpp
    src/glsl_compute.h
    src/glsl_waveform.cpp
    src/glsl_waveform.h
    src/granular_freeze.cpp
    src/granular_freeze.h
    src/multi_sampler.cpp
    src/multi_sampler.h
    src/multi_sampler_editor.cpp
    src/multi_sampler_editor.h
    src/tracker_file_parser.cpp
    src/tracker_file_parser.h
    src/signal_shape_node.cpp
    src/signal_shape_node.h
    src/midi_script_node.cpp
    src/midi_script_node.h
    src/midi_script_editor.cpp
    src/midi_script_editor.h
    src/script_lang_bar.cpp
    src/script_lang_bar.h
    src/shape_expr.cpp
    src/shape_expr.h
    src/warp.cpp
    src/warp.h
    src/buffer_warp.cpp
    src/buffer_warp.h
    src/warp_editor.cpp
    src/warp_editor.h
    src/lua_prelude.h
    src/signal_modulation.h
    src/wavelet.h
    src/spectrum_visualizer.h
    src/wavelet_painter.h
    src/wavelet_painter.cpp
    src/wavelet_paint.h
    src/sfizz_processor.h
    src/plugin_sandbox.h
    src/plugin_sandbox.cpp
    src/self_test.h
    src/self_test.cpp
    src/cache_processor.h
    src/audio_crossfader.h
    src/trigger_node.cpp
    src/trigger_node.h
    src/midi_mod_node.cpp
    src/midi_mod_node.h
    src/midi_input_node.h
    src/midi_device_wizard.cpp
    src/midi_device_wizard.h
    src/help_utils.h
    src/audio_cache.cpp
    src/audio_cache.h
    src/pan_processor.h
    src/gain_processor.h
    src/pitch_shift_processor.cpp
    src/pitch_shift_processor.h
    src/audio_export.cpp
    src/audio_export.h
    src/mod_import.cpp
    src/mod_import.h
    src/fft_util.cpp
    src/fft_util.h
    src/pitch_core.h
    src/wavetable_frame.h
    src/wavelet_frame.h
    src/wavelet_frame.cpp
    src/sample_frame.h
    src/sample_frame.cpp
    src/granular_frame.h
    src/granular_frame.cpp
    src/inharmonic_frame.h
    src/inharmonic_frame.cpp
    src/waveform_bank.h
    src/waveform_bank.cpp
    src/capture_from_playback.h
    src/capture_from_playback.cpp
    src/layered_wave_editor.cpp
    src/layered_wave_editor.h
    src/spectral_editor.cpp
    src/spectral_editor.h
    src/signal_eq_editor.cpp
    src/signal_eq_editor.h
    src/curve_editor.cpp
    src/curve_editor.h
    src/adsr_envelope.cpp
    src/adsr_envelope.h
    src/adsr_envelope_component.cpp
    src/adsr_envelope_component.h
    src/envelope_presets.cpp
    src/envelope_presets.h
    src/effect_regions.h
    src/time_gate_processor.cpp
    src/time_gate_processor.h
    src/layer_legend.cpp
    src/layer_legend.h
    src/xy_pad.cpp
    src/xy_pad.h
    src/control_bank.cpp
    src/control_bank.h
    src/spectrum_tap.cpp
    src/spectrum_tap.h
    src/analyzer_nodes.cpp
    src/analyzer_nodes.h
    src/convolution_processor.cpp
    src/convolution_processor.h
    src/convolution_editor.cpp
    src/convolution_editor.h
    src/pitch_detect.h
    src/sampler_editor.cpp
    src/sampler_editor.h
    src/soundfont_processor.cpp
    src/soundfont_processor.h
    src/hotkey_manager.cpp
    src/hotkey_manager.h
    src/room_ir_capture.cpp
    src/room_ir_capture.h
    src/multitrack_recorder.cpp
    src/multitrack_recorder.h
    src/builtin_effects.h
    src/drum_synth.cpp
    src/drum_synth.h
    src/spatializer_3d.cpp
    src/spatializer_3d.h
    src/hrtf_data.cpp
    src/hrtf_data.h
    src/tuning.h
    src/video_decoder.cpp
    src/video_decoder.h
    src/video_import_dialog.cpp
    src/video_import_dialog.h
    src/generate_dialog.cpp
    src/generate_dialog.h
)

target_link_libraries(SEANCE PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_opengl
)

target_compile_definitions(SEANCE PRIVATE
    JUCE_STANDALONE_APPLICATION=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1
    $<$<BOOL:${APPLE}>:JUCE_PLUGINHOST_AU=1>
    $<$<NOT:$<BOOL:${APPLE}>>:JUCE_PLUGINHOST_AU=0>
    $<$<BOOL:${LINUX}>:JUCE_PLUGINHOST_LADSPA=1>
    $<$<BOOL:${WIN32}>:JUCE_ASIO=1>
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:SEANCE,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:SEANCE,JUCE_VERSION>"
)

# libopenmpt (for MOD/S3M/IT/XM import)
# Try prebuilt Windows binaries first, then system package
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libopenmpt_bin/lib/amd64/libopenmpt.lib")
    target_include_directories(SEANCE PRIVATE third_party/libopenmpt_bin/inc)
    target_link_directories(SEANCE PRIVATE third_party/libopenmpt_bin/lib/amd64)
    target_link_libraries(SEANCE PRIVATE libopenmpt)
    target_compile_definitions(SEANCE PRIVATE HAS_LIBOPENMPT=1)
    # Copy every DLL from the libopenmpt bin directory to the output folder.
    # libopenmpt ships with several helper DLLs (zlib, mpg123, vorbis, ogg, ...)
    # and the exact set varies by version, so copying the whole directory is
    # simpler and more future-proof than listing each one.
    add_custom_command(TARGET SEANCE POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libopenmpt_bin/bin/amd64"
            "$<TARGET_FILE_DIR:SEANCE>"
    )
else()
    # Linux/macOS: try pkg-config or find_library
    find_library(OPENMPT_LIB openmpt)
    find_path(OPENMPT_INC libopenmpt/libopenmpt.h)
    if(OPENMPT_LIB AND OPENMPT_INC)
        target_include_directories(SEANCE PRIVATE ${OPENMPT_INC})
        target_link_libraries(SEANCE PRIVATE ${OPENMPT_LIB})
        target_compile_definitions(SEANCE PRIVATE HAS_LIBOPENMPT=1)
        message(STATUS "libopenmpt: system library found")
    endif()
endif()

# sfizz (for full SFZ compliance, task #37)
# If sfizz is cloned into third_party/sfizz, build and link it.
# Otherwise, the SfizzProcessor compiles as a stub that logs "sfizz not available".
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sfizz/CMakeLists.txt")
    message(STATUS "  sfizz: FOUND")
    add_subdirectory(third_party/sfizz EXCLUDE_FROM_ALL)
    target_link_libraries(SEANCE PRIVATE sfizz::sfizz)
    target_compile_definitions(SEANCE PRIVATE HAS_SFIZZ=1)
else()
    message(STATUS "  sfizz: NOT FOUND (full SFZ compliance disabled)")
    message(STATUS "         Clone: git clone https://github.com/sfztools/sfizz third_party/sfizz")
endif()

# libogg + libopus (for Opus export)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus/CMakeLists.txt")
    # Build libogg
    file(GLOB OGG_SOURCES third_party/ogg/src/*.c)
    add_library(ogg STATIC ${OGG_SOURCES})
    target_include_directories(ogg PUBLIC third_party/ogg/include)

    # Build libopus via its CMakeLists
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/opus EXCLUDE_FROM_ALL)
    target_link_libraries(SEANCE PRIVATE opus ogg)
    target_include_directories(SEANCE PRIVATE
        third_party/opus/include
        third_party/ogg/include
    )
endif()

# wasm3 (optional — builds without it, WASM scripts just won't load)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wasm3/source")
    file(GLOB WASM3_SOURCES third_party/wasm3/source/*.c)
    add_library(wasm3 STATIC ${WASM3_SOURCES})
    target_include_directories(wasm3 PUBLIC third_party/wasm3/source)
    target_link_libraries(SEANCE PRIVATE wasm3)
    target_include_directories(SEANCE PRIVATE third_party/wasm3/source)
endif()

# Lua 5.4 (optional — embedded scripting language for MIDI Script / Signal
# Shape nodes; builds without it, the "Lua" language option is just disabled).
# We compile the Lua core + standard libraries as a static C library, excluding
# the standalone interpreter (lua.c), the bytecode compiler/amalgamation
# (luac.c / onelua.c) and the internal test harness (ltests.c) — each of those
# either defines its own main() or pulls in everything via #include.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua/lua.h")
    file(GLOB LUA_SOURCES third_party/lua/*.c)
    list(FILTER LUA_SOURCES EXCLUDE REGEX "(lua|luac|onelua|ltests)\\.c$")
    add_library(lua54 STATIC ${LUA_SOURCES})
    target_include_directories(lua54 PUBLIC third_party/lua)
    target_link_libraries(SEANCE PRIVATE lua54)
    target_include_directories(SEANCE PRIVATE third_party/lua)
    target_compile_definitions(SEANCE PRIVATE HAS_LUA=1)
endif()

# ---------------------------------------------------------------------------
# Python embedding (OPTIONAL — Python is NOT a hard requirement to run SEANCE)
#
# The Script Console, Signal evaluation, and the Python shape-baker use an
# embedded CPython interpreter. We link Python's import library so the C API is
# available at build time, but on MSVC we DELAY-LOAD the DLL (see the /DELAYLOAD
# option in the MSVC block below) so SEANCE.exe still launches on a machine with
# no Python installed. Every entry into the interpreter is guarded by a
# LoadLibrary probe (ScriptEngine::pythonAvailable); if the DLL can't be loaded
# we disable Python scripting gracefully instead of crashing or failing to
# start. When Python IS present at build time we copy its runtime DLL next to
# the exe (post-build, below) so the produced build ships with working Python.
#
# HAS_PYTHON gates all the CPython API code in scripting.cpp: without it the
# file compiles to stubs and the build still succeeds with Python disabled.
# SEANCE_PYTHON_DLL_NAME is the bare DLL filename used for /DELAYLOAD and the
# runtime probe; SEANCE_PYTHON_DLL is its full path (for the copy step).
# ---------------------------------------------------------------------------
set(SEANCE_PYTHON_DLL "")
set(SEANCE_PYTHON_DLL_NAME "")

find_package(Python3 COMPONENTS Development)
if(Python3_FOUND)
    target_include_directories(SEANCE PRIVATE src ${Python3_INCLUDE_DIRS})
    target_link_libraries(SEANCE PRIVATE ${Python3_LIBRARIES})
    target_compile_definitions(SEANCE PRIVATE HAS_PYTHON=1)
    set(SEANCE_PYTHON_DLL_NAME "python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}.dll")
    # Locate the runtime DLL (Windows) for the copy step. With COMPONENTS
    # Development the Interpreter isn't requested, so Python3_EXECUTABLE may be
    # empty — search several candidate roots: the interpreter dir, the runtime
    # library dirs, and the install root (parent of include/ and of libs/).
    set(_py_dll_candidates "")
    if(Python3_EXECUTABLE)
        get_filename_component(_py_exe_dir "${Python3_EXECUTABLE}" DIRECTORY)
        list(APPEND _py_dll_candidates "${_py_exe_dir}")
    endif()
    if(Python3_RUNTIME_LIBRARY_DIRS)
        list(APPEND _py_dll_candidates ${Python3_RUNTIME_LIBRARY_DIRS})
    endif()
    foreach(_inc ${Python3_INCLUDE_DIRS})
        get_filename_component(_inc_parent "${_inc}" DIRECTORY)
        list(APPEND _py_dll_candidates "${_inc_parent}")
    endforeach()
    foreach(_libdir ${Python3_LIBRARY_DIRS})
        get_filename_component(_lib_parent "${_libdir}" DIRECTORY)
        list(APPEND _py_dll_candidates "${_libdir}" "${_lib_parent}")
    endforeach()
    foreach(_cand ${_py_dll_candidates})
        if(NOT SEANCE_PYTHON_DLL AND EXISTS "${_cand}/${SEANCE_PYTHON_DLL_NAME}")
            set(SEANCE_PYTHON_DLL "${_cand}/${SEANCE_PYTHON_DLL_NAME}")
        endif()
    endforeach()
    message(STATUS "Python: ${Python3_VERSION} at ${Python3_INCLUDE_DIRS} (DLL: ${SEANCE_PYTHON_DLL})")
else()
    # Fallback to hardcoded path (Windows dev machine).
    if(EXISTS "D:/python314/Include")
        target_include_directories(SEANCE PRIVATE src D:/python314/Include)
        target_link_directories(SEANCE PRIVATE D:/python314/libs)
        target_link_libraries(SEANCE PRIVATE python314)
        target_compile_definitions(SEANCE PRIVATE HAS_PYTHON=1)
        set(SEANCE_PYTHON_DLL_NAME "python314.dll")
        if(EXISTS "D:/python314/python314.dll")
            set(SEANCE_PYTHON_DLL "D:/python314/python314.dll")
        endif()
        message(STATUS "Python: using hardcoded D:/python314")
    else()
        message(WARNING "Python not found. Python scripting will be disabled (build still succeeds).")
    endif()
endif()

# Pass the DLL name to the runtime probe (ScriptEngine::pythonAvailable).
if(SEANCE_PYTHON_DLL_NAME)
    target_compile_definitions(SEANCE PRIVATE PYTHON_DLL_NAME="${SEANCE_PYTHON_DLL_NAME}")
endif()

# On Windows, Python's pyconfig.h auto-links python3XY_d.lib in Debug builds
# via #pragma comment(lib,...). Standard Python installs don't ship the debug
# lib, so tell the linker to ignore it and use the release lib in Debug too.
#
# NOTE on optionality: we link Python at LOAD time (not /DELAYLOAD). Delay-load
# is incompatible with CPython because the C API imports DATA symbols
# (`_Py_NoneStruct` behind Py_None/Py_RETURN_NONE, `PyFloat_Type`, the `PyExc_*`
# exception objects); MSVC's /DELAYLOAD only wraps function imports and errors
# with LNK1194 on data imports. So instead of making the DLL absence survivable
# at launch, we SHIP the DLL next to the exe (post-build copy below) so a build
# made with Python always has it. A build made WITHOUT Python (HAS_PYTHON
# undefined) links no Python at all and disables scripting via stubs. Truly
# launching without the DLL when the build linked it would require fully dynamic
# GetProcAddress loading of every symbol — tracked as a future option.
if(MSVC)
    target_link_options(SEANCE PRIVATE "/SUBSYSTEM:WINDOWS")
    if(SEANCE_PYTHON_DLL_NAME)
        string(REPLACE ".dll" "_d.lib" SEANCE_PYTHON_DEBUG_LIB "${SEANCE_PYTHON_DLL_NAME}")
        target_link_options(SEANCE PRIVATE
            "$<$<CONFIG:Debug>:/NODEFAULTLIB:${SEANCE_PYTHON_DEBUG_LIB}>")
    endif()

    # Emit debug symbols (PDB) for Release too. /Zi keeps full /O2 codegen but
    # produces a side PDB, and /DEBUG tells the linker to write it; /OPT:REF and
    # /OPT:ICF undo the linker's default of disabling those optimisations when
    # /DEBUG is present, so the Release binary stays the same size/speed. Without
    # this, a Release crash dump has no symbols and the faulting frame can't be
    # named - which is exactly what bit us diagnosing the granular-wavetable
    # autosave crash. The PDB ships next to SEANCE.exe in the artefacts folder.
    target_compile_options(SEANCE PRIVATE "$<$<CONFIG:Release>:/Zi>")
    target_link_options(SEANCE PRIVATE
        "$<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF>")
endif()

# Windows Media Foundation (for M4A/WMA export)
if(WIN32)
    target_link_libraries(SEANCE PRIVATE mfplat mfreadwrite mfuuid)
endif()

# Copy docs/ folder alongside the built exe so Help menu entries can open
# them via juce::File::startAsProcess. The docs are plain HTML shipped as
# files (not embedded) so users can also browse docs/ outside the app and
# we can update them without a rebuild.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../docs")
    add_custom_command(TARGET SEANCE POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/../docs"
            "$<TARGET_FILE_DIR:SEANCE>/docs"
    )
endif()

# Copy resources/ (the packed factory waveform library waveforms.bin, etc.)
# alongside the built exe so WaveformBank can load it at runtime. Like docs/,
# these ship as files rather than embedded data: a single ~4 MB read beats
# embedding 4000 cycles in the binary and bloating the build.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources")
    add_custom_command(TARGET SEANCE POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/resources"
            "$<TARGET_FILE_DIR:SEANCE>/resources"
    )
endif()

# Copy the Python runtime DLL next to the exe (when Python was found at build
# time) so the produced build has working Python scripting without relying on
# Python being on the system PATH. Because Python is delay-loaded, the DLL's
# absence only disables scripting — it never blocks launch — so this copy is a
# convenience, not a requirement. (The interpreter still needs its standard
# library, which it locates via the install/registry; we copy only the DLL.)
if(SEANCE_PYTHON_DLL)
    add_custom_command(TARGET SEANCE POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SEANCE_PYTHON_DLL}"
            "$<TARGET_FILE_DIR:SEANCE>"
    )
endif()

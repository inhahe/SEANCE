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
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rubberband/rubberband/RubberBandStretcher.h")
    message(STATUS "  Rubber Band: FOUND")
else()
    message(STATUS "  Rubber Band: NOT FOUND (pitch shift disabled)")
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

target_sources(SEANCE PRIVATE
    src/main.cpp
    src/dialog_helpers.cpp
    src/dialog_helpers.h
    src/main_window.cpp
    src/main_window.h
    src/node_graph.cpp
    src/node_graph.h
    src/node_graph_component.cpp
    src/node_graph_component.h
    src/piano_roll_component.cpp
    src/piano_roll_component.h
    src/audio_engine.cpp
    src/audio_engine.h
    src/graph_processor.cpp
    src/graph_processor.h
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
    src/wavetable_frame.h
    src/wavelet_frame.h
    src/wavelet_frame.cpp
    src/sample_frame.h
    src/sample_frame.cpp
    src/granular_frame.h
    src/granular_frame.cpp
    src/capture_from_playback.h
    src/capture_from_playback.cpp
    src/layered_wave_editor.cpp
    src/layered_wave_editor.h
    src/spectral_editor.cpp
    src/spectral_editor.h
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
    src/routing_strip.cpp
    src/routing_strip.h
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

# Rubber Band Library (pitch shifting / time stretching)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rubberband/src")
    file(GLOB_RECURSE RUBBERBAND_SOURCES
        third_party/rubberband/src/common/*.cpp
        third_party/rubberband/src/faster/*.cpp
        third_party/rubberband/src/finer/*.cpp
        third_party/rubberband/src/RubberBandStretcher.cpp
        third_party/rubberband/src/RubberBandLiveShifter.cpp
        third_party/rubberband/src/rubberband-c.cpp
    )
    # Exclude test files
    list(FILTER RUBBERBAND_SOURCES EXCLUDE REGEX "test")
    add_library(rubberband STATIC ${RUBBERBAND_SOURCES})
    target_include_directories(rubberband PUBLIC
        third_party/rubberband
        third_party/rubberband/src
        third_party/rubberband/src/ext/kissfft
        third_party/rubberband/src/common
    )
    # Also compile KissFFT and BQ resampler sources
    target_sources(rubberband PRIVATE
        third_party/rubberband/src/ext/kissfft/kiss_fft.c
        third_party/rubberband/src/ext/kissfft/kiss_fftr.c
    )
    target_compile_definitions(rubberband PRIVATE
        USE_BUILTIN_FFT=1
        HAVE_KISSFFT=1
        USE_BQRESAMPLER=1
        NOMINMAX
        _USE_MATH_DEFINES
        NO_THREADING=1
    )
    target_link_libraries(SEANCE PRIVATE rubberband)
    target_include_directories(SEANCE PRIVATE third_party/rubberband)
    target_compile_definitions(SEANCE PRIVATE HAS_RUBBERBAND=1)
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

# Python embedding
find_package(Python3 COMPONENTS Development)
if(Python3_FOUND)
    target_include_directories(SEANCE PRIVATE src ${Python3_INCLUDE_DIRS})
    target_link_libraries(SEANCE PRIVATE ${Python3_LIBRARIES})
    message(STATUS "Python: ${Python3_VERSION} at ${Python3_INCLUDE_DIRS}")
else()
    # Fallback to hardcoded path (Windows dev machine)
    if(EXISTS "D:/python314/Include")
        target_include_directories(SEANCE PRIVATE src D:/python314/Include)
        target_link_directories(SEANCE PRIVATE D:/python314/libs)
        target_link_libraries(SEANCE PRIVATE python314)
        message(STATUS "Python: using hardcoded D:/python314")
    else()
        message(WARNING "Python not found. Scripting will be disabled.")
    endif()
endif()

# On Windows, Python's pyconfig.h auto-links python314_d.lib in Debug builds
# via #pragma comment(lib,...). Standard Python installs don't ship the debug
# lib, so tell the linker to ignore it and use the release lib in Debug too.
# Python is dynamically loaded at runtime, so this is safe.
if(MSVC)
    target_link_options(SEANCE PRIVATE
        "$<$<CONFIG:Debug>:/NODEFAULTLIB:python314_d.lib>"
        "/SUBSYSTEM:WINDOWS")

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

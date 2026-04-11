cmake_minimum_required(VERSION 3.22)
project(SoundShop2 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Check for optional dependencies and print status
message(STATUS "=== SoundShop2 Dependency Status ===")
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
# SoundShop2
# ==============================================================================
juce_add_gui_app(SoundShop2
    PRODUCT_NAME "SoundShop"
    COMPANY_NAME "SoundShop"
)

target_sources(SoundShop2 PRIVATE
    src/main.cpp
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
    src/builtin_synth.cpp
    src/builtin_synth.h
    src/terrain_synth.cpp
    src/terrain_synth.h
    src/signal_shape_node.cpp
    src/signal_shape_node.h
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
    src/layered_wave_editor.cpp
    src/layered_wave_editor.h
    src/effect_regions.h
    src/time_gate_processor.cpp
    src/time_gate_processor.h
    src/routing_strip.cpp
    src/routing_strip.h
    src/xy_pad.cpp
    src/xy_pad.h
    src/spectrum_tap.cpp
    src/spectrum_tap.h
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
)

target_link_libraries(SoundShop2 PRIVATE
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

target_compile_definitions(SoundShop2 PRIVATE
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
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:SoundShop2,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:SoundShop2,JUCE_VERSION>"
)

# libopenmpt (for MOD/S3M/IT/XM import)
# Try prebuilt Windows binaries first, then system package
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libopenmpt_bin/lib/amd64/libopenmpt.lib")
    target_include_directories(SoundShop2 PRIVATE third_party/libopenmpt_bin/inc)
    target_link_directories(SoundShop2 PRIVATE third_party/libopenmpt_bin/lib/amd64)
    target_link_libraries(SoundShop2 PRIVATE libopenmpt)
    target_compile_definitions(SoundShop2 PRIVATE HAS_LIBOPENMPT=1)
    # Copy every DLL from the libopenmpt bin directory to the output folder.
    # libopenmpt ships with several helper DLLs (zlib, mpg123, vorbis, ogg, ...)
    # and the exact set varies by version, so copying the whole directory is
    # simpler and more future-proof than listing each one.
    add_custom_command(TARGET SoundShop2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libopenmpt_bin/bin/amd64"
            "$<TARGET_FILE_DIR:SoundShop2>"
    )
else()
    # Linux/macOS: try pkg-config or find_library
    find_library(OPENMPT_LIB openmpt)
    find_path(OPENMPT_INC libopenmpt/libopenmpt.h)
    if(OPENMPT_LIB AND OPENMPT_INC)
        target_include_directories(SoundShop2 PRIVATE ${OPENMPT_INC})
        target_link_libraries(SoundShop2 PRIVATE ${OPENMPT_LIB})
        target_compile_definitions(SoundShop2 PRIVATE HAS_LIBOPENMPT=1)
        message(STATUS "libopenmpt: system library found")
    endif()
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
    target_link_libraries(SoundShop2 PRIVATE opus ogg)
    target_include_directories(SoundShop2 PRIVATE
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
    target_link_libraries(SoundShop2 PRIVATE rubberband)
    target_include_directories(SoundShop2 PRIVATE third_party/rubberband)
    target_compile_definitions(SoundShop2 PRIVATE HAS_RUBBERBAND=1)
endif()

# wasm3 (optional — builds without it, WASM scripts just won't load)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wasm3/source")
    file(GLOB WASM3_SOURCES third_party/wasm3/source/*.c)
    add_library(wasm3 STATIC ${WASM3_SOURCES})
    target_include_directories(wasm3 PUBLIC third_party/wasm3/source)
    target_link_libraries(SoundShop2 PRIVATE wasm3)
    target_include_directories(SoundShop2 PRIVATE third_party/wasm3/source)
endif()

# Python embedding
find_package(Python3 COMPONENTS Development)
if(Python3_FOUND)
    target_include_directories(SoundShop2 PRIVATE src ${Python3_INCLUDE_DIRS})
    target_link_libraries(SoundShop2 PRIVATE ${Python3_LIBRARIES})
    message(STATUS "Python: ${Python3_VERSION} at ${Python3_INCLUDE_DIRS}")
else()
    # Fallback to hardcoded path (Windows dev machine)
    if(EXISTS "D:/python314/Include")
        target_include_directories(SoundShop2 PRIVATE src D:/python314/Include)
        target_link_directories(SoundShop2 PRIVATE D:/python314/libs)
        target_link_libraries(SoundShop2 PRIVATE python314)
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
    target_link_options(SoundShop2 PRIVATE
        "$<$<CONFIG:Debug>:/NODEFAULTLIB:python314_d.lib>")
endif()

# Windows Media Foundation (for M4A/WMA export)
if(WIN32)
    target_link_libraries(SoundShop2 PRIVATE mfplat mfreadwrite mfuuid)
endif()

# Copy docs/ folder alongside the built exe so Help menu entries can open
# them via juce::File::startAsProcess. The docs are plain HTML shipped as
# files (not embedded) so users can also browse docs/ outside the app and
# we can update them without a rebuild.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../docs")
    add_custom_command(TARGET SoundShop2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/../docs"
            "$<TARGET_FILE_DIR:SoundShop2>/docs"
    )
endif()

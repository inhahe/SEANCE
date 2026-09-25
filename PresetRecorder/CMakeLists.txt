cmake_minimum_required(VERSION 3.22)

# Plugin Preset Recorder - a companion tool to SEANCE (SoundShop2), built and
# shipped separately. It loads every plugin SEANCE's plugin settings point at,
# records a FLAC preview of each preset, and lets you browse/listen to them.
# See README.md (how to use it) and design.md (how it works).
#
# The version below is the single source of truth for the tool's version: it
# feeds JUCE_APPLICATION_VERSION_STRING (window title / About) and the exe's
# file-version resource. Bump it whenever you rebuild after a change.
project(PresetRecorder VERSION 1.0.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SEANCE's own sources live one level up, in ../cpp/src. Two small, self-
# contained files are compiled straight into this tool (see design.md,
# "Code shared with SEANCE"):
#   plugin_settings.cpp - parses soundshop_plugins.cfg, owns SEANCE's default
#                         scan folders and decides where each plugin format
#                         looks, so the tool reads the plugin list and skip
#                         list, and searches, exactly the way SEANCE does.
#   dialog_helpers.cpp  - taskbar-correct dialogs (no second taskbar button).
set(SEANCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(SEANCE_SRC "${SEANCE_ROOT}/cpp/src")
if(NOT EXISTS "${SEANCE_SRC}/plugin_settings.cpp")
    message(FATAL_ERROR "SEANCE sources not found at ${SEANCE_SRC}. "
                        "PresetRecorder must live inside the SoundShop2 directory.")
endif()

# ==============================================================================
# JUCE (same lookup order as SEANCE's cpp/CMakeLists.txt)
# ==============================================================================
if(NOT DEFINED JUCE_DIR)
    if(EXISTS "D:/JUCE-8.0.12")
        set(JUCE_DIR "D:/JUCE-8.0.12")
    elseif(EXISTS "${SEANCE_ROOT}/../JUCE")
        set(JUCE_DIR "${SEANCE_ROOT}/../JUCE")
    elseif(EXISTS "/usr/local/share/JUCE")
        set(JUCE_DIR "/usr/local/share/JUCE")
    else()
        message(FATAL_ERROR "JUCE not found. Set JUCE_DIR: cmake -DJUCE_DIR=/path/to/JUCE ..")
    endif()
endif()
message(STATUS "JUCE: ${JUCE_DIR}")
add_subdirectory(${JUCE_DIR} ${CMAKE_BINARY_DIR}/juce)

# ==============================================================================
# PresetRecorder
# ==============================================================================
juce_add_gui_app(PresetRecorder
    PRODUCT_NAME "PresetRecorder"
    COMPANY_NAME "SEANCE"
)

# Same JUCE 8.0.12 bug SEANCE works around (see the long comment in
# ../cpp/CMakeLists.txt): the generated .rc does not depend on the version, so
# a version bump would leave the exe's FILEVERSION stale. Delete the stale .rc
# at configure time so juceaide regenerates it.
get_target_property(_pr_juce_gen_dir PresetRecorder JUCE_GENERATED_SOURCES_DIRECTORY)
if(WIN32 AND _pr_juce_gen_dir)
    set(_pr_rc "${_pr_juce_gen_dir}/PresetRecorder_resources.rc")
    if(EXISTS "${_pr_rc}")
        file(READ "${_pr_rc}" _pr_rc_text)
        if(NOT _pr_rc_text MATCHES "\"FileVersion\",[ \t]*\"${PROJECT_VERSION}\\\\0\"")
            file(REMOVE "${_pr_rc}")
        endif()
    endif()
endif()

# SEANCE's dialog/taskbar lint (a dialog must never earn its own taskbar
# button) is a generic source scan, so run it over this tool's sources too.
add_custom_target(preset_recorder_dialog_lint ALL
    COMMAND ${CMAKE_COMMAND}
            -DSEANCE_SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/src
            -P ${SEANCE_ROOT}/cpp/cmake/check_dialog_patterns.cmake
    COMMENT "Checking dialog taskbar rules (PresetRecorder)"
    VERBATIM
)
add_dependencies(PresetRecorder preset_recorder_dialog_lint)

target_sources(PresetRecorder PRIVATE
    src/Main.cpp
    src/Util.cpp
    src/Util.h
    src/WorkerProcess.cpp
    src/WorkerProcess.h
    src/Settings.cpp
    src/Settings.h
    src/SeanceConfig.cpp
    src/SeanceConfig.h
    src/PluginFileInfo.cpp
    src/PluginFileInfo.h
    src/Catalog.cpp
    src/Catalog.h
    src/Manifest.cpp
    src/Manifest.h
    src/PresetSources.cpp
    src/PresetSources.h
    src/PreviewInput.cpp
    src/PreviewInput.h
    src/Worker.cpp
    src/Worker.h
    src/JobRunner.cpp
    src/JobRunner.h
    src/Controller.cpp
    src/Controller.h
    src/ui/Theme.h
    src/ui/MainComponent.cpp
    src/ui/MainComponent.h
    src/ui/RecordTab.cpp
    src/ui/RecordTab.h
    src/ui/BrowseTab.cpp
    src/ui/BrowseTab.h
    src/ui/SkipListTab.cpp
    src/ui/SkipListTab.h
    src/ui/Player.cpp
    src/ui/Player.h

    # Shared with SEANCE (compiled from ../cpp/src, never copied).
    ${SEANCE_SRC}/plugin_settings.cpp
    ${SEANCE_SRC}/plugin_settings.h
    ${SEANCE_SRC}/plugin_host.h
    ${SEANCE_SRC}/dialog_helpers.cpp
    ${SEANCE_SRC}/dialog_helpers.h
)

# SEANCE headers are included by their path under the SoundShop2 root
# ("cpp/src/plugin_settings.h") rather than by adding cpp/src to the include
# path, so none of SEANCE's ~150 header names can shadow one of ours.
target_include_directories(PresetRecorder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SEANCE_ROOT}
)

target_link_libraries(PresetRecorder PRIVATE
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
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# Plugin formats: exactly the set SEANCE hosts (VST3 + LV2 everywhere, AU on
# macOS), so the tool finds the same plugins SEANCE's scan does. VST2 is off in
# both because it needs Steinberg's discontinued VST2 SDK headers.
target_compile_definitions(PresetRecorder PRIVATE
    JUCE_STANDALONE_APPLICATION=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1
    $<$<BOOL:${APPLE}>:JUCE_PLUGINHOST_AU=1>
    $<$<NOT:$<BOOL:${APPLE}>>:JUCE_PLUGINHOST_AU=0>
    $<$<BOOL:${LINUX}>:JUCE_PLUGINHOST_LADSPA=1>
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    # dialog_helpers.cpp's blocking showAlert() needs NativeMessageBox::show.
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:PresetRecorder,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:PresetRecorder,JUCE_VERSION>"
    # plugin_settings.cpp (shared with SEANCE) uses getenv.
    $<$<BOOL:${MSVC}>:_CRT_SECURE_NO_WARNINGS>
)

if(MSVC)
    # Release PDBs, as in SEANCE: a crash dump from a worker process is only
    # useful if the frames can be named.
    target_compile_options(PresetRecorder PRIVATE "$<$<CONFIG:Release>:/Zi>" /utf-8)
    target_link_options(PresetRecorder PRIVATE
        "$<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF>")
endif()

if(WIN32)
    # version.lib: GetFileVersionInfo (company names of skip-listed plugins).
    target_link_libraries(PresetRecorder PRIVATE version)
endif()

# Ship resources/ (the bundled public-domain effect-input song and its
# provenance note) next to the exe, where the tool looks for it first.
add_custom_command(TARGET PresetRecorder POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        "$<TARGET_FILE_DIR:PresetRecorder>/resources"
)

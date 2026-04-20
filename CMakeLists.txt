# Build:
#   mkdir build && cd build
#   cmake .. -G "Visual Studio 17 2022"
#   cmake --build . --config Release
#
# Requires: CMake 3.20+, Python 3 (for GLAD generation), C++17 compiler

cmake_minimum_required(VERSION 3.20)
project(Julia4D LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# GLFW 3.4
# ---------------------------------------------------------------------------
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
)
FetchContent_MakeAvailable(glfw)

# ---------------------------------------------------------------------------
# GLAD 2  (OpenGL 4.3 Core loader)
# ---------------------------------------------------------------------------
FetchContent_Declare(
    glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG        v2.0.8
)
FetchContent_Populate(glad)
add_subdirectory("${glad_SOURCE_DIR}/cmake" "${glad_BINARY_DIR}")
glad_add_library(glad_gl_core_43 REPRODUCIBLE LOADER API gl:core=4.3)

# ---------------------------------------------------------------------------
# Dear ImGui v1.91.8
# ---------------------------------------------------------------------------
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.8
)
FetchContent_Populate(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
add_executable(julia4d main.cpp)
target_link_libraries(julia4d PRIVATE glfw glad_gl_core_43 imgui)

if(WIN32)
    target_link_libraries(julia4d PRIVATE opengl32)
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(julia4d PRIVATE OpenGL::GL)
endif()

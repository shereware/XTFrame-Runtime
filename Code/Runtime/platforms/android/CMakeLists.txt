# XTFrame View (T-240, Docs/design/23-host-integration.md §23.11.1): libmain.so for SDL2's Android
# shell. Built by Gradle's externalNativeBuild (app/build.gradle) with the NDK toolchain; adds the
# repository root with everything but the runtime switched off and the SW presentation path.
cmake_minimum_required(VERSION 3.24)
project(XTFrameView LANGUAGES C CXX)

# The root sets these in its own scope; libmain needs them here too.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(XT_BUILD_EDITOR OFF CACHE BOOL "" FORCE)
set(XT_BUILD_AGENT OFF CACHE BOOL "" FORCE)
set(XT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(XT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(XT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(XT_BUILD_FUZZ OFF CACHE BOOL "" FORCE)
# No desktop GL on Android (GLES xt_gl is T-242); xt_shell presents through xt_sw + SDL_Renderer.
set(XT_BUILD_GL OFF CACHE BOOL "" FORCE)
set(XT_BUILD_GPU OFF CACHE BOOL "" FORCE)
set(XT_SHELL_GL OFF CACHE BOOL "" FORCE)
set(XT_BUILD_SHELL ON CACHE BOOL "" FORCE)
set(XT_BUILD_TEXT ON CACHE BOOL "" FORCE)

get_filename_component(XT_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
add_subdirectory("${XT_REPO_ROOT}" xtframe)

# SDLActivity loads libmain.so (XTFrameActivity.getLibraries) and calls SDL_main.
add_library(main SHARED
    src/main.cpp
)
target_link_libraries(main PRIVATE xt_core xt_sw xt_text xt_bundle xt_flow xt_shell android log)
# Same flags as the desktop runtime targets (Docs/design/14-coding-standards.md §14.1).
xt_no_exceptions_rtti(main)
xt_set_warnings(main)

cmake_minimum_required(VERSION 3.24)
project(XTFrame VERSION 0.9.0 LANGUAGES CXX C)

# MSBuild compiles one file at a time per project unless /MP is given; apply it globally so the
# FetchContent dependencies (SDL2, FreeType, HarfBuzz) build in parallel too.
if(MSVC)
    add_compile_options(/MP)
    # Symbols in Release too, so the crash log's stack (Code/Editor/App/crash_log.cpp) names our
    # functions instead of "??" on a user's machine; Release stays optimized.
    add_compile_options($<$<CONFIG:Release>:/Zi>)
    add_link_options($<$<CONFIG:Release>:/DEBUG>)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(XT_BUILD_GL "Build OpenGL backend" ON)
# Docs/design/10-backends.md §10.5: the device-neutral GPU frame builder (no GL/D3D inside).
option(XT_BUILD_GPU "Build the device-neutral GPU frame builder (xt_gpu)" ON)
if(XT_BUILD_GL AND NOT XT_BUILD_GPU)
    message(FATAL_ERROR "XT_BUILD_GL requires XT_BUILD_GPU=ON (xt_gl is a GpuDevice over xt_gpu, T-147)")
endif()
option(XT_BUILD_SHELL "Build SDL2 shell" ON)
# T-240 (Docs/design/23-host-integration.md §23.11.1): xt_shell presents through xt_gl (ON) or
# through xt_sw + an SDL_Renderer streaming texture (OFF). Android has no desktop GL: forced OFF.
option(XT_SHELL_GL "xt_shell presents through xt_gl (OFF: xt_sw + SDL_Renderer)" ON)
if(ANDROID)
    set(XT_SHELL_GL OFF CACHE BOOL "" FORCE)
endif()
if(XT_BUILD_SHELL AND XT_SHELL_GL AND NOT XT_BUILD_GL)
    message(FATAL_ERROR "XT_SHELL_GL=ON requires XT_BUILD_GL=ON (or pass -DXT_SHELL_GL=OFF)")
endif()
option(XT_BUILD_TOOLS "Build CLI tools" ON)
option(XT_BUILD_TESTS "Build tests" ON)
option(XT_BUILD_TEXT "Build the xt_text FreeType/HarfBuzz TextShaper" ON)
option(XT_BUILD_FUZZ "Build libFuzzer targets (clang only)" OFF)
option(XT_BUILD_EXAMPLES "Build examples" ON)
# Docs/design/17-editor.md §17.1: the editor libraries and app are off by default; the Qt-facing
# targets (Code/Editor/Qt, Code/Editor/App) are added by T-60.
option(XT_BUILD_EDITOR "Build the editor libraries and app" OFF)
# Docs/design/36-history.md, Docs/tasks/T-307.md: the history core (Code/Editor/History, xt_history; needs
# XT_BUILD_EDITOR). On by default; the beta repository of 40 §40.10.1 turns it off.
option(XT_EDITOR_HISTORY "Build the history core (Code/Editor/History)" ON)
# T-423: the beta export (Tools/Scripts/export.sh --profile beta) leaves Code/Editor/History out; without the
# directory the history core is off whatever the cache says (same pattern as the guards below).
if(XT_EDITOR_HISTORY AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Code/Editor/History/CMakeLists.txt")
    message(STATUS "XT_EDITOR_HISTORY: OFF (the history core directory is absent)")
    set(XT_EDITOR_HISTORY OFF)
endif()
# Docs/design/20-agent-interface.md §20.1: the throwaway developer AI endpoint (Code/Editor/DevTools/agent).
# Off by default; the `vs2022-agent` preset turns it on on top of the editor preset.
option(XT_BUILD_AGENT "Build the throwaway developer agent endpoint (Code/Editor/DevTools/agent)" OFF)
# Docs/design/23-host-integration.md §23.ac (T-382, Aux Duty XF-033), 45 §45.4 (T-430): when not
# empty, every `xtframe-*` executable (the editor included) is written to this directory, the
# examples to its examples/ and the tests to its tests/, in every configuration. Top-level default:
# bin/<platform>_<arch>/<Config> (XT_PLATFORM_TAG); added by a parent project: empty (CMake default).
include(Tools/CMake/xt_runtime_output.cmake)
xt_runtime_output_default()
if(XT_BUILD_AGENT AND NOT XT_BUILD_EDITOR)
    message(FATAL_ERROR "XT_BUILD_AGENT requires XT_BUILD_EDITOR=ON (xt_agent binds xt_editor_core)")
endif()

include(Tools/CMake/xt_warnings.cmake)
include(Tools/CMake/xt_build_speed.cmake)
include(Tools/CMake/xt_nlohmann.cmake)
# T-430 (45 §45.3): Code/SDKs/versions.cmake and the shared fetched sources
# (Code/SDKs/_fetched/<name>/, XT_SDK_OFFLINE); used by the Tools/CMake/xt_<dependency>.cmake files.
include(Tools/CMake/xt_sdks.cmake)
# T-404: coverage instrumentation (the `ninja-coverage` preset, Tools/Scripts/coverage.sh).
option(XT_COVERAGE "Instrument every target for coverage (clang or gcc)" OFF)
if(XT_COVERAGE)
    include(Tools/CMake/xt_coverage.cmake)
endif()

# XT_BUILD_FUZZ: clang-only libFuzzer targets (Docs/design/15-testing.md §15.5,
# Docs/tasks/T-39.md). Every target gets ASan/UBSan plus libFuzzer instrumentation without the
# fuzzer runtime (-fsanitize=fuzzer-no-link); Tests/fuzz/CMakeLists.txt adds plain
# -fsanitize=fuzzer to the fuzz executables themselves to link in the driver/runtime.
if(XT_BUILD_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "XT_BUILD_FUZZ requires Clang (got CMAKE_CXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID})")
    endif()
    # -fsanitize=undefined's vptr check requires RTTI everywhere; xt_core is built with -fno-rtti
    # by design (Docs/design/14-coding-standards.md §14.1), so it is disabled here project-wide.
    add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize=vptr -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined -fno-sanitize=vptr)
    # RelWithDebInfo defines NDEBUG by default; the fuzz build keeps XT_ASSERT enabled
    # (Docs/tasks/T-39.md "規則": assertion failures are findings) by stripping it back out.
    string(REPLACE "-DNDEBUG" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
endif()
# XT_SANITIZE (Docs/tasks/T-402.md item 2, Docs/design/15-testing.md §15.8): a comma list of
# -fsanitize= checks for the whole tree, e.g. "address,undefined" (the `sanitizers` CI job, preset
# ninja-ci-asan). GCC/Clang only. Like XT_BUILD_FUZZ, the vptr check is left out because xt_core is
# built with -fno-rtti. Set before the FetchContent dependencies so they are instrumented too.
set(XT_SANITIZE "" CACHE STRING "Comma list of -fsanitize= checks for every target (empty: off)")
if(XT_SANITIZE)
    if(XT_MSVC_FRONTEND)
        message(FATAL_ERROR "XT_SANITIZE requires GCC or Clang (got an MSVC-style compiler)")
    endif()
    if(XT_BUILD_FUZZ)
        message(FATAL_ERROR "XT_SANITIZE and XT_BUILD_FUZZ are exclusive (the fuzz build sets its own sanitizers)")
    endif()
    add_compile_options(-fsanitize=${XT_SANITIZE} -fno-sanitize=vptr -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${XT_SANITIZE} -fno-sanitize=vptr)
endif()
if(XT_BUILD_GL OR XT_BUILD_SHELL)
    include(Tools/CMake/xt_sdl2.cmake)
endif()
if(XT_BUILD_TEXT)
    include(Tools/CMake/xt_text_deps.cmake)
endif()

# MSVC default CMAKE_CXX_FLAGS injects /EHsc. xt_core disables exceptions, so
# remove it here; non-core targets add /EHsc back explicitly.
# 見 Docs/design/14-coding-standards.md §14.1
if(CMAKE_CXX_FLAGS MATCHES "/EHsc")
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

# enable_testing() must precede any add_subdirectory() whose CMakeLists.txt calls add_test()
# (e.g. tools/, for xtframe-compile's CTest entries), otherwise CTest never sees them.
if(XT_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(Code/Runtime/core)
add_subdirectory(Code/Runtime/flow)
add_subdirectory(Code/Runtime/compile)
if(XT_BUILD_TEXT)
    add_subdirectory(Code/Runtime/text)
endif()
add_subdirectory(Code/Runtime/sw)
# xt_bundle's pack() links xt_compile/xt_flow/xt_sw (and xt_text), so it comes after them.
add_subdirectory(Code/Runtime/bundle)
# xt_import writes a bundle source tree and verifies it with the compiler, so it comes after both.
# T-400: the guards on editor-layer directories let the runtime export (Tools/Scripts/export_runtime.sh),
# which leaves them out, configure from this same file.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Code/Editor/Import/CMakeLists.txt")
    add_subdirectory(Code/Editor/Import)
endif()
if(XT_BUILD_GPU)
    add_subdirectory(Code/Runtime/gpu)
endif()
if(XT_BUILD_GL)
    add_subdirectory(Code/Runtime/gl)
endif()
if(XT_BUILD_SHELL)
    add_subdirectory(Code/Runtime/shell)
endif()
# Code/Backends/: reference platform x renderer glue (Code/Backends/README.md, 45 章 §45.7); every backend is
# optional and self-skipping, so the directory is always added when present.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Code/Backends/CMakeLists.txt")
    add_subdirectory(Code/Backends)
endif()
if(XT_BUILD_EDITOR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Code/Editor/CMakeLists.txt")
    add_subdirectory(Code/Editor)
endif()
if(XT_BUILD_AGENT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Code/Editor/DevTools/agent/CMakeLists.txt")
    add_subdirectory(Code/Editor/DevTools/agent)
endif()
if(XT_BUILD_TOOLS)
    add_subdirectory(Code/Tools)
endif()
if(XT_BUILD_TESTS)
    add_subdirectory(Tests)
endif()
if(XT_BUILD_EXAMPLES)
    add_subdirectory(Examples)
endif()

# T-382 / T-383 (23 §23.ac), T-430 (45 §45.4): XT_RUNTIME_OUTPUT_DIR for the executables, applied
# at the end of the top-level directory (Tools/CMake/xt_runtime_output.cmake), so a parent project that adds
# this tree with add_subdirectory() may set it before or after that call.
xt_runtime_output_dir_deferred()

# T-435: per-target definitions and include directories for the clang++ syntax-check stage of
# Tools/Scripts/local_ci.sh (<build>/xt_clang_check/; Tools/CMake/xt_clang_check.cmake).
include(Tools/CMake/xt_clang_check.cmake)
xt_clang_check_write()

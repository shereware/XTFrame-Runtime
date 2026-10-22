# Fetches Clipper2 (Boost Software License 1.0, pinned release tag) for the editor's destructive
# vector boolean / offset operations (Docs/design/31-vector-authoring.md §31.4.6; Docs/tasks/T-221.md).
# Only the engine / offset / rectclip sources of Clipper2Lib are compiled; the project's own
# CMakeLists (tests, examples, utils) is skipped by pointing SOURCE_SUBDIR at a directory without one.
# USINGZ is on (every vertex carries a 64-bit tag); it is a PUBLIC definition because it changes the
# layout of Clipper2's point types. Defines the STATIC target `xt_clipper2`, linked only by
# xt_editor_core (xt_core stays dependency-free) and, for reference results, by its tests.
# Version: Code/SDKs/versions.cmake (clipper2); source: Code/SDKs/_fetched/clipper2.
include(${CMAKE_CURRENT_LIST_DIR}/xt_sdks.cmake)
xt_sdk_fetch(clipper2 xt_clipper2_src SOURCE_SUBDIR xt-no-cmake)
set(_xt_clipper2_dir ${xt_clipper2_src_SOURCE_DIR}/CPP/Clipper2Lib)
add_library(xt_clipper2 STATIC
    ${_xt_clipper2_dir}/src/clipper.engine.cpp
    ${_xt_clipper2_dir}/src/clipper.offset.cpp
    ${_xt_clipper2_dir}/src/clipper.rectclip.cpp
)
target_include_directories(xt_clipper2 SYSTEM PUBLIC ${_xt_clipper2_dir}/include)
target_compile_definitions(xt_clipper2 PUBLIC USINGZ)
target_compile_features(xt_clipper2 PUBLIC cxx_std_17)
xt_enable_exceptions(xt_clipper2)

# Fetches Clipper2 (Boost Software License 1.0, pinned release tag) for the editor's destructive
# vector boolean / offset operations (docs/design/31-vector-authoring.md §31.4.6; docs/tasks/T-221.md).
# Only the engine / offset / rectclip sources of Clipper2Lib are compiled; the project's own
# CMakeLists (tests, examples, utils) is skipped by pointing SOURCE_SUBDIR at a directory without one.
# USINGZ is on (every vertex carries a 64-bit tag); it is a PUBLIC definition because it changes the
# layout of Clipper2's point types. Defines the STATIC target `xt_clipper2`, linked only by
# xt_editor_core (xt_core stays dependency-free) and, for reference results, by its tests.
include(FetchContent)
FetchContent_Declare(
    xt_clipper2_src
    URL https://github.com/AngusJohnson/Clipper2/archive/refs/tags/Clipper2_1.5.4.tar.gz
    URL_HASH SHA256=9d8a35a29d04cd1b7b45f542c0ba48015feece1210036ea9e4efaad3140af4b0
    SOURCE_SUBDIR xt-no-cmake
)
FetchContent_MakeAvailable(xt_clipper2_src)
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

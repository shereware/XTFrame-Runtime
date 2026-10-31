# Fetches miniz (MIT, pinned release 3.0.2, URL_HASH SHA256 in Code/SDKs/versions.cmake;
# Docs/design/14-coding-standards.md 14.1: third-party code is a vendored single header or a pinned
# FetchContent, 2026-10-31 spec review 7.2). The release archive holds the amalgamated miniz.c /
# miniz.h and no CMakeLists, so nothing of the project's own build runs (SOURCE_SUBDIR points at a
# directory that does not exist). Defines the STATIC target `xt_miniz` from miniz.c. Users: xt_bundle
# (the store-mode ZIP reader and writer behind `.xtsymbols` and `.xtb`, Docs/design/49-symbol-sets.md
# 49.4, 22 §22.5) and the editor's history core (zlib-compatible deflate / inflate of loose objects,
# Docs/design/40-product-blueprint-v2.md §40.3.1, Docs/tasks/T-307.md item 1). Nobody else links it
# (Tools/Scripts/check_layering.sh rule 6).
#
# Configuration, all through compile definitions (PUBLIC, so every includer sees the same
# declarations; the fetched sources are never edited): MINIZ_NO_TIME (no time functions: entries
# are written with a zero timestamp, which also keeps the archives byte-stable),
# MINIZ_NO_ZLIB_COMPATIBLE_NAMES (the `mz_` names only, so no zlib symbol clashes). The archive
# reading and writing APIs and stdio stay on: zip_archive.cpp streams through FILE* handles it
# opens itself (UTF-8 paths, the wide API on Windows). Third-party code: warnings are off for this
# target only (-Werror / /WX stay on for our own targets).
# Version: Code/SDKs/versions.cmake (miniz); source: Code/SDKs/_fetched/miniz.
include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/xt_sdks.cmake)
xt_sdk_fetch(miniz xt_miniz_src SOURCE_SUBDIR xt-no-cmake)
add_library(xt_miniz STATIC ${xt_miniz_src_SOURCE_DIR}/miniz.c)
target_include_directories(xt_miniz SYSTEM PUBLIC ${xt_miniz_src_SOURCE_DIR})
target_compile_definitions(xt_miniz PUBLIC MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
set_target_properties(xt_miniz PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON LINKER_LANGUAGE C)
if(XT_MSVC_FRONTEND)
    target_compile_options(xt_miniz PRIVATE /W0)
else()
    target_compile_options(xt_miniz PRIVATE -w)
endif()

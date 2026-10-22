# Fetches miniz (MIT, pinned release 3.0.2) for the history core's loose objects (zlib-compatible
# deflate / inflate; Docs/design/40-product-blueprint-v2.md §40.3.1, Docs/tasks/T-307.md item 1).
# The release archive holds the amalgamated miniz.c / miniz.h and no CMakeLists, so nothing of the
# project's own build runs (SOURCE_SUBDIR points at a directory that does not exist). Only miniz.c
# is compiled; the ZIP archive, stdio and time parts are switched off with the MINIZ_NO_* macros
# (PUBLIC, so every includer sees the same declarations), and MINIZ_NO_ZLIB_COMPATIBLE_NAMES keeps
# the `mz_` names only. Defines the STATIC target `xt_miniz`.
# Version: Code/SDKs/versions.cmake (miniz); source: Code/SDKs/_fetched/miniz.
include(${CMAKE_CURRENT_LIST_DIR}/xt_sdks.cmake)
xt_sdk_fetch(miniz xt_miniz_src SOURCE_SUBDIR xt-no-cmake)
add_library(xt_miniz STATIC ${xt_miniz_src_SOURCE_DIR}/miniz.c)
target_include_directories(xt_miniz SYSTEM PUBLIC ${xt_miniz_src_SOURCE_DIR})
target_compile_definitions(xt_miniz PUBLIC
    MINIZ_NO_ARCHIVE_APIS MINIZ_NO_ARCHIVE_WRITING_APIS MINIZ_NO_STDIO MINIZ_NO_TIME
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
set_target_properties(xt_miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(XT_MSVC_FRONTEND)
    target_compile_options(xt_miniz PRIVATE /W0)
else()
    target_compile_options(xt_miniz PRIVATE -w)
endif()

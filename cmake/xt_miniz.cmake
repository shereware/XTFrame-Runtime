# Fetches miniz (MIT, pinned release 3.0.2) for the history core's loose objects (zlib-compatible
# deflate / inflate; docs/design/40-product-blueprint-v2.md §40.3.1, docs/tasks/T-307.md item 1).
# The release archive holds the amalgamated miniz.c / miniz.h and no CMakeLists, so nothing of the
# project's own build runs (SOURCE_SUBDIR points at a directory that does not exist). Only miniz.c
# is compiled; the ZIP archive, stdio and time parts are switched off with the MINIZ_NO_* macros
# (PUBLIC, so every includer sees the same declarations), and MINIZ_NO_ZLIB_COMPATIBLE_NAMES keeps
# the `mz_` names only. Defines the STATIC target `xt_miniz`.
include(FetchContent)
FetchContent_Declare(
    xt_miniz_src
    URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
    URL_HASH SHA256=ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5
    SOURCE_SUBDIR xt-no-cmake
)
FetchContent_MakeAvailable(xt_miniz_src)
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

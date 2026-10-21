# Fetches cpp-httplib (MIT, single header, pinned release) for the editor's MCP endpoint
# (docs/design/17-editor.md §17.8b "vendored 單標頭"; docs/tasks/T-196.md). Only the header is used:
# the project's own CMakeLists is skipped (SOURCE_SUBDIR points at a directory without one), so no
# optional OpenSSL/zlib/brotli lookup happens. Defines the INTERFACE target `xt_httplib`.
include(FetchContent)
FetchContent_Declare(
    xt_cpp_httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.58.0.tar.gz
    URL_HASH SHA256=31932ce8b33f2905472a987dc1984b9d8a7d338083a066021349587cc5f1cfea
    SOURCE_SUBDIR xt-header-only
)
FetchContent_MakeAvailable(xt_cpp_httplib)
add_library(xt_httplib INTERFACE)
target_include_directories(xt_httplib SYSTEM INTERFACE ${xt_cpp_httplib_SOURCE_DIR})
if(WIN32)
    target_link_libraries(xt_httplib INTERFACE ws2_32)
    target_compile_definitions(xt_httplib INTERFACE WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(xt_httplib INTERFACE Threads::Threads)
endif()

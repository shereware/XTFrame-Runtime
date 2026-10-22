# Fetches cpp-httplib (MIT, single header, pinned release) for the editor's MCP endpoint
# (Docs/design/17-editor.md §17.8b "vendored 單標頭"; Docs/tasks/T-196.md). Only the header is used:
# the project's own CMakeLists is skipped (SOURCE_SUBDIR points at a directory without one), so no
# optional OpenSSL/zlib/brotli lookup happens. Defines the INTERFACE target `xt_httplib`.
# Version: Code/SDKs/versions.cmake (httplib); source: Code/SDKs/_fetched/httplib.
include(${CMAKE_CURRENT_LIST_DIR}/xt_sdks.cmake)
xt_sdk_fetch(httplib xt_cpp_httplib SOURCE_SUBDIR xt-header-only)
add_library(xt_httplib INTERFACE)
target_include_directories(xt_httplib SYSTEM INTERFACE ${xt_cpp_httplib_SOURCE_DIR})
if(WIN32)
    target_link_libraries(xt_httplib INTERFACE ws2_32)
    target_compile_definitions(xt_httplib INTERFACE WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(xt_httplib INTERFACE Threads::Threads)
endif()

# xt_capi: the C ABI shared library `xtframe` (xtframe.dll / libxtframe.so / libxtframe.dylib) for
# FFI hosts (Unity, ...). Docs/design/50-c-abi.md §50.2, Docs/tasks/T-504.md. A flat C interface
# (include/xt/capi/xtframe.h, pure C99) over xt_core / xt_bundle / xt_flow / xt_sw (and xt_text).
# Only the `xt_` functions are exported: hidden visibility for this target, and a linker export list
# that keeps the symbols of the static libraries linked in (built with default visibility) local.
# Docs/design/52-web-wasm.md §52.9 (T-525, T-578): under Emscripten the library is STATIC instead (a shared
# library means nothing in wasm): libxtframe.a, no visibility preset, no export list and no
# position-independent code, and XT_CAPI_STATIC (PUBLIC) makes the header's XT_CAPI macro empty.
set(xt_capi_sources
    src/capi_library.cpp
    src/capi_bundle.cpp
    src/capi_surface.cpp
    src/capi_ports.cpp
    src/capi_gpu.cpp
)
if(EMSCRIPTEN)
    add_library(xt_capi STATIC ${xt_capi_sources})
    set_target_properties(xt_capi PROPERTIES OUTPUT_NAME xtframe)
    target_compile_definitions(xt_capi PUBLIC XT_CAPI_STATIC)
else()
    add_library(xt_capi SHARED ${xt_capi_sources})
    set_target_properties(xt_capi PROPERTIES
        OUTPUT_NAME xtframe
        CXX_VISIBILITY_PRESET hidden
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
endif()
target_compile_definitions(xt_capi PRIVATE XT_CAPI_BUILD)
target_include_directories(xt_capi PUBLIC include)
# xt::bundle::detail::readFile / joinPath (Code/Runtime/bundle/src/bundle_json.h): the flow files and
# the font assets are read through xt_bundle's own reader, so the T-240 file reader hook applies.
target_include_directories(xt_capi PRIVATE ${PROJECT_SOURCE_DIR}/Code/Runtime/bundle/src)
target_link_libraries(xt_capi PRIVATE xt_core xt_bundle xt_flow xt_sw xt_nlohmann)
if(XT_BUILD_TEXT)
    target_link_libraries(xt_capi PRIVATE xt_text)
    target_compile_definitions(xt_capi PRIVATE XT_CAPI_HAS_TEXT)
endif()
# T-636 (50 §50.z): the GPU target over xt_wgpu when this build has it (the top level adds this directory
# after Code/Runtime/wgpu); without it src/capi_gpu.cpp only reports "built without wgpu". wgpu-native's
# shared library is copied next to the library after each build (the same POST_BUILD copy as xtframe-view,
# Tools/CMake/xt_runtime_output.cmake), so bin/<platform>/<Config>/ holds xtframe + wgpu_native together.
if(TARGET xt_wgpu)
    target_link_libraries(xt_capi PRIVATE xt_wgpu)
    target_compile_definitions(xt_capi PRIVATE XT_CAPI_HAS_WGPU)
    xt_copy_runtime_library(xt_capi xt::wgpu_native)
    # Linux / macOS: find libwgpu_native next to libxtframe wherever the pair is copied (untested there).
    if(APPLE)
        set_target_properties(xt_capi PROPERTIES BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
    elseif(NOT WIN32 AND NOT EMSCRIPTEN)
        set_target_properties(xt_capi PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
    endif()
endif()

# xt_capi_make_pic(<target>) (T-552, 2026-10-31 spec review 7.5): the static libraries a shared library links
# must be position-independent on Linux / macOS. Instead of CMAKE_POSITION_INDEPENDENT_CODE for the whole tree,
# POSITION_INDEPENDENT_CODE is set on every STATIC / OBJECT library <target> reaches through its link libraries
# (LINK_LIBRARIES and INTERFACE_LINK_LIBRARIES, `$<LINK_ONLY:...>` unwrapped, aliases resolved). Every one of
# them is defined before this directory (the top level adds Code/Runtime/capi after core / flow / bundle / sw /
# text). Windows needs nothing (a DLL links any object code).
function(xt_capi_make_pic target)
    set(_pending ${target})
    set(_seen)
    while(_pending)
        list(POP_FRONT _pending _t)
        if(NOT TARGET "${_t}")
            continue()
        endif()
        get_target_property(_aliased "${_t}" ALIASED_TARGET)
        if(_aliased)
            set(_t "${_aliased}")
        endif()
        if("${_t}" IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_t}")
        get_target_property(_type "${_t}" TYPE)
        if(_type STREQUAL "STATIC_LIBRARY" OR _type STREQUAL "OBJECT_LIBRARY")
            get_target_property(_imported "${_t}" IMPORTED)
            if(NOT _imported)
                set_target_properties("${_t}" PROPERTIES POSITION_INDEPENDENT_CODE ON)
            endif()
        endif()
        set(_deps)
        if(NOT _type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(_link "${_t}" LINK_LIBRARIES)
            if(_link)
                list(APPEND _deps ${_link})
            endif()
        endif()
        get_target_property(_ilink "${_t}" INTERFACE_LINK_LIBRARIES)
        if(_ilink)
            list(APPEND _deps ${_ilink})
        endif()
        foreach(_d IN LISTS _deps)
            string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" _d "${_d}")
            if(TARGET "${_d}")
                list(APPEND _pending "${_d}")
            endif()
        endforeach()
    endwhile()
endfunction()
if(NOT WIN32 AND NOT EMSCRIPTEN)
    xt_capi_make_pic(xt_capi)
endif()
# The module may use exceptions (xt_bundle / xt_text may throw); every exported function body runs
# inside a catch-all guard (src/capi_internal.h). No RTTI, like xt_core.
xt_enable_exceptions(xt_capi)
xt_no_rtti(xt_capi)
xt_set_warnings(xt_capi)

# Export list: only `xt_*` (a static library has none).
if(EMSCRIPTEN)
    # No export list: the program that links libxtframe.a decides what it exports.
elseif(APPLE)
    target_link_options(xt_capi PRIVATE "LINKER:-exported_symbol,_xt_*")
elseif(NOT WIN32)
    set(xt_capi_version_script "${CMAKE_CURRENT_BINARY_DIR}/xtframe.map")
    file(WRITE "${xt_capi_version_script}" "{\n  global:\n    xt_*;\n  local:\n    *;\n};\n")
    target_link_options(xt_capi PRIVATE "LINKER:--version-script=${xt_capi_version_script}")
    set_property(TARGET xt_capi APPEND PROPERTY LINK_DEPENDS "${xt_capi_version_script}")
endif()

# 45 §45.4: the library lands next to the xtframe-* executables, in bin/<platform>_<arch>/<Config>/
# (XT_RUNTIME_OUTPUT_DIR; Tools/CMake/xt_runtime_output.cmake places executables only). A DLL is a
# RUNTIME artifact on Windows, a .so / .dylib a LIBRARY one elsewhere; the import library stays in
# the build tree. Under Emscripten the static libxtframe.a is an ARCHIVE artifact, placed the same way
# (bin/wasm32/<Config>/libxtframe.a, 52 §52.9).
if(NOT "${XT_RUNTIME_OUTPUT_DIR}" STREQUAL "" AND EMSCRIPTEN)
    set(xt_capi_configs ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE} Debug Release RelWithDebInfo MinSizeRel)
    list(REMOVE_DUPLICATES xt_capi_configs)
    set_target_properties(xt_capi PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${XT_RUNTIME_OUTPUT_DIR}")
    foreach(config IN LISTS xt_capi_configs)
        string(TOUPPER "${config}" config_upper)
        set_target_properties(xt_capi PROPERTIES ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${XT_RUNTIME_OUTPUT_DIR}")
    endforeach()
elseif(NOT "${XT_RUNTIME_OUTPUT_DIR}" STREQUAL "")
    xt_set_runtime_dir(xt_capi "${XT_RUNTIME_OUTPUT_DIR}")
    set(xt_capi_configs ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE} Debug Release RelWithDebInfo MinSizeRel)
    list(REMOVE_DUPLICATES xt_capi_configs)
    set_target_properties(xt_capi PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${XT_RUNTIME_OUTPUT_DIR}")
    foreach(config IN LISTS xt_capi_configs)
        string(TOUPPER "${config}" config_upper)
        set_target_properties(xt_capi PROPERTIES LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${XT_RUNTIME_OUTPUT_DIR}")
    endforeach()
endif()

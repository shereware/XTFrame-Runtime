# Docs/design/10-backends.md §10.10.1 (T-437): wgpu-native for xt_wgpu.
#
# Included from the top-level CMakeLists.txt when XT_BUILD_WGPU is ON. Unpacks the PREBUILT entry
# `wgpu-native` of Code/SDKs/versions.cmake for this platform (xt_sdk_prebuilt, Tools/CMake/xt_sdks.cmake)
# into Code/SDKs/_fetched/wgpu-native/<XT_PLATFORM_TAG>/ and defines the imported shared library
# `xt::wgpu_native` (include/webgpu/{webgpu.h,wgpu.h}; lib/wgpu_native.dll + wgpu_native.dll.lib,
# lib/libwgpu_native.so, lib/libwgpu_native.dylib). When the platform has no archive, the download
# fails, or XT_SDK_OFFLINE=ON lacks the directory, it prints why and sets XT_BUILD_WGPU to OFF for
# the rest of the configure step: wgpu never stops the configure step.
#
# The shared library is copied next to the executables by Tools/CMake/xt_runtime_output.cmake
# (xt_runtime_shared_library / xt_copy_runtime_library), like the Qt DLLs, so they find it without
# PATH changes.

include_guard(GLOBAL)

function(_xt_wgpu_disable reason)
    message(STATUS "xt_wgpu: wgpu-native not available for ${XT_PLATFORM_TAG}, XT_BUILD_WGPU=OFF (${reason})")
    set(XT_BUILD_WGPU OFF PARENT_SCOPE)
endfunction()

# T-671 (50 §50.ab addendum 2, item 1): an experimental, locally built wgpu-native in place of the pinned
# one (Tools/Scripts/build_wgpu_native_trunk.sh lays the directory out: include/webgpu/{webgpu.h,wgpu.h},
# lib/, COMMIT.txt). Empty (the default) = the pinned PREBUILT entry; shipping builds never set it.
set(XT_WGPU_NATIVE_OVERRIDE_DIR "" CACHE PATH
    "Experimental: a wgpu-native directory (include/webgpu + lib) replacing the pinned prebuilt one; empty = pinned")
if(NOT XT_WGPU_NATIVE_OVERRIDE_DIR STREQUAL "")
    file(TO_CMAKE_PATH "${XT_WGPU_NATIVE_OVERRIDE_DIR}" _xt_wgpu_dir)
    set(_xt_wgpu_reason "")
    message(STATUS "xt_wgpu: XT_WGPU_NATIVE_OVERRIDE_DIR=${_xt_wgpu_dir} replaces the pinned wgpu-native")
else()
    xt_sdk_prebuilt(wgpu-native "${XT_PLATFORM_TAG}" _xt_wgpu_dir _xt_wgpu_reason)
endif()
if(_xt_wgpu_dir STREQUAL "")
    _xt_wgpu_disable("${_xt_wgpu_reason}")
    return()
endif()

if(WIN32)
    set(_xt_wgpu_lib "${_xt_wgpu_dir}/lib/wgpu_native.dll")
    set(_xt_wgpu_implib "${_xt_wgpu_dir}/lib/wgpu_native.dll.lib")
elseif(APPLE)
    set(_xt_wgpu_lib "${_xt_wgpu_dir}/lib/libwgpu_native.dylib")
    set(_xt_wgpu_implib "")
else()
    set(_xt_wgpu_lib "${_xt_wgpu_dir}/lib/libwgpu_native.so")
    set(_xt_wgpu_implib "")
endif()
if(NOT EXISTS "${_xt_wgpu_lib}" OR NOT EXISTS "${_xt_wgpu_dir}/include/webgpu/webgpu.h"
   OR NOT EXISTS "${_xt_wgpu_dir}/include/webgpu/wgpu.h"
   OR (NOT _xt_wgpu_implib STREQUAL "" AND NOT EXISTS "${_xt_wgpu_implib}"))
    _xt_wgpu_disable("${_xt_wgpu_dir} lacks the library or the headers")
    return()
endif()

if(NOT TARGET xt::wgpu_native)
    add_library(xt::wgpu_native SHARED IMPORTED GLOBAL)
    set_target_properties(xt::wgpu_native PROPERTIES
        IMPORTED_LOCATION "${_xt_wgpu_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_xt_wgpu_dir}/include"
        # dllimport declarations on Windows (webgpu.h's WGPU_EXPORT).
        INTERFACE_COMPILE_DEFINITIONS "WGPU_SHARED_LIBRARY")
    if(NOT _xt_wgpu_implib STREQUAL "")
        set_target_properties(xt::wgpu_native PROPERTIES IMPORTED_IMPLIB "${_xt_wgpu_implib}")
    endif()
    if(NOT WIN32 AND NOT APPLE)
        # The Linux release library has no SONAME: link it by name so the executables record
        # `libwgpu_native.so` (found through their build RPATH or next to them), not an absolute path.
        set_target_properties(xt::wgpu_native PROPERTIES IMPORTED_NO_SONAME TRUE)
    endif()
endif()
message(STATUS "xt_wgpu: wgpu-native from ${_xt_wgpu_dir}")

# T-671: WGPUDeviceExtras.memoryHints (wgpu-native PR #619; not in the pinned v29.0.1.1) -> the
# XT_WGPU_MEMORY_HINTS switch of WgpuInit::headless. Re-checked on every configure: the directory may change.
include(CheckStructHasMember)
unset(XT_WGPU_HAS_MEMORY_HINTS CACHE)
set(_xt_wgpu_saved_includes "${CMAKE_REQUIRED_INCLUDES}")
set(CMAKE_REQUIRED_INCLUDES "${_xt_wgpu_dir}/include")
check_struct_has_member("WGPUDeviceExtras" memoryHints "webgpu/webgpu.h;webgpu/wgpu.h" XT_WGPU_HAS_MEMORY_HINTS
                        LANGUAGE C)
set(CMAKE_REQUIRED_INCLUDES "${_xt_wgpu_saved_includes}")
if(XT_WGPU_HAS_MEMORY_HINTS)
    set_property(TARGET xt::wgpu_native APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS XT_WGPU_HAS_MEMORY_HINTS)
endif()
# The build's identity for the target-creation log line: COMMIT.txt of an override directory (a local build
# reports version 0.0.0.0 through wgpuGetVersion), else "pinned".
set(_xt_wgpu_source "pinned")
if(NOT XT_WGPU_NATIVE_OVERRIDE_DIR STREQUAL "")
    set(_xt_wgpu_source "override")
    if(EXISTS "${_xt_wgpu_dir}/COMMIT.txt")
        file(STRINGS "${_xt_wgpu_dir}/COMMIT.txt" _xt_wgpu_commit LIMIT_COUNT 1)
        set(_xt_wgpu_source "override ${_xt_wgpu_commit}")
    endif()
endif()
set_property(TARGET xt::wgpu_native APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
             "XT_WGPU_NATIVE_SOURCE=\"${_xt_wgpu_source}\"")

# bin/<tag>/<Config>/ (and its tests/ and examples/) get the library at configure time; executables
# that link xt_wgpu call xt_copy_runtime_library(<target> xt::wgpu_native) for the copy next to them
# (Tools/CMake/xt_runtime_output.cmake).
xt_runtime_shared_library(xt::wgpu_native)

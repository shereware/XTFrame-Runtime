# Docs/design/23-host-integration.md §23.ac (T-382, T-383; Aux Duty XF-033 / XF-045) and
# Docs/design/45-repository-layout.md §45.4 (T-430): XT_RUNTIME_OUTPUT_DIR for the executables of
# this tree.
#
# When this tree is the top-level project, XT_RUNTIME_OUTPUT_DIR defaults to
# `<source>/bin/<XT_PLATFORM_TAG>/$<CONFIG>` (xt_runtime_output_default(), called from the top-level
# CMakeLists.txt before any target): the `xtframe-*` tools and the editor land there, the examples in
# its `examples/` and the test executables in its `tests/`. Static libraries stay in the build tree.
# XT_BIN_VARIANT (empty by default) makes it `bin/<tag>/$<CONFIG>-<variant>`: the presets that build a
# different flavour of the same targets (swshell, agent, qt6, asan, coverage, fuzz) set it so they
# never overwrite the canonical executables, which every canonical preset shares on purpose.
# When a parent project adds this tree with add_subdirectory() the default stays empty (CMake's own
# output directories), and the parent may set XT_RUNTIME_OUTPUT_DIR itself.
#
# xt_runtime_output_dir_deferred() schedules xt_apply_runtime_output_dir() for the end of the
# top-level source directory (CMAKE_SOURCE_DIR; cmake_language(DEFER)), so the value is read after
# the whole configure step: a parent project that adds this tree with add_subdirectory() may set
# XT_RUNTIME_OUTPUT_DIR (a normal or a cache variable of its top-level directory) before or after
# that call. When this tree is the top-level project the call runs at the end of its own
# CMakeLists.txt. The value may hold generator expressions (`$<CONFIG>`; a single-configuration
# generator expands it to CMAKE_BUILD_TYPE); the status message does not print them (CMake cannot
# evaluate them at configure time).

# xt_platform_tag(<out>): `<platform>_<arch>` of the target system, e.g. win_x64, linux_x64,
# linux_arm64, mac_arm64, android_arm64 (CMAKE_SYSTEM_NAME x the target processor).
function(xt_platform_tag out)
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(platform win)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(platform mac)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
        set(platform android)
    else()
        string(TOLOWER "${CMAKE_SYSTEM_NAME}" platform)
    endif()
    # The processor of the target, not of the host: the Visual Studio platform, the one macOS
    # architecture asked for, or the Android ABI, before CMAKE_SYSTEM_PROCESSOR.
    set(arch "${CMAKE_SYSTEM_PROCESSOR}")
    if(CMAKE_VS_PLATFORM_NAME)
        set(arch "${CMAKE_VS_PLATFORM_NAME}")
    elseif(APPLE AND CMAKE_OSX_ARCHITECTURES)
        list(LENGTH CMAKE_OSX_ARCHITECTURES n)
        if(n EQUAL 1)
            set(arch "${CMAKE_OSX_ARCHITECTURES}")
        else()
            set(arch universal)
        endif()
    elseif(ANDROID AND CMAKE_ANDROID_ARCH_ABI)
        set(arch "${CMAKE_ANDROID_ARCH_ABI}")
    endif()
    string(TOLOWER "${arch}" arch)
    if(arch MATCHES "^(amd64|x86_64|x64)$")
        set(arch x64)
    elseif(arch MATCHES "^(aarch64|arm64|arm64-v8a)$")
        set(arch arm64)
    elseif(arch MATCHES "^(win32|i.86|x86)$")
        set(arch x86)
    endif()
    set(${out} "${platform}_${arch}" PARENT_SCOPE)
endfunction()

# xt_runtime_output_default(): defines the XT_RUNTIME_OUTPUT_DIR cache entry (and XT_PLATFORM_TAG).
# A build tree configured before T-430 holds an empty value with the old description; it is moved to
# the new default once (an empty value given on purpose, e.g. -DXT_RUNTIME_OUTPUT_DIR=, is kept).
function(xt_runtime_output_default)
    xt_platform_tag(tag)
    set(XT_PLATFORM_TAG "${tag}" CACHE INTERNAL "bin/ platform directory name (Tools/CMake/xt_runtime_output.cmake)")
    set(XT_BIN_VARIANT "" CACHE STRING "Suffix of the default bin/ directory (bin/<platform>_<arch>/<Config>-<variant>; empty: none)")
    set(help "Output directory of the executables (top-level default: bin/<platform>_<arch>/<Config>[-<XT_BIN_VARIANT>]; empty: CMake default)")
    if(PROJECT_IS_TOP_LEVEL)
        set(default "${CMAKE_SOURCE_DIR}/bin/${tag}/$<CONFIG>")
        if(NOT XT_BIN_VARIANT STREQUAL "")
            string(APPEND default "-${XT_BIN_VARIANT}")
        endif()
        get_property(old_help CACHE XT_RUNTIME_OUTPUT_DIR PROPERTY HELPSTRING)
        set(current "$CACHE{XT_RUNTIME_OUTPUT_DIR}")
        set(prefix "${CMAKE_SOURCE_DIR}/bin/${tag}/")
        string(LENGTH "${prefix}" prefix_len)
        string(LENGTH "${current}" current_len)
        set(current_prefix "")
        set(current_rest "")
        if(current_len GREATER prefix_len)
            string(SUBSTRING "${current}" 0 ${prefix_len} current_prefix)
            string(SUBSTRING "${current}" ${prefix_len} -1 current_rest)
        endif()
        if((old_help STREQUAL "Output directory of every xtframe-* tool (empty: CMake default)" AND current STREQUAL "")
           OR (current_prefix STREQUAL prefix AND current_rest MATCHES "^\\$<CONFIG>(-[^/]*)?$"))
            # Still a default value (of before T-430, or for another XT_BIN_VARIANT): follow the variant.
            set(XT_RUNTIME_OUTPUT_DIR "${default}" CACHE STRING "${help}" FORCE)
        else()
            set(XT_RUNTIME_OUTPUT_DIR "${default}" CACHE STRING "${help}")
        endif()
    else()
        set(XT_RUNTIME_OUTPUT_DIR "" CACHE STRING "${help}")
    endif()
endfunction()

# xt_collect_executables(<dir> <out>): every EXECUTABLE target of <dir> and its subdirectories.
function(xt_collect_executables dir out)
    set(found "")
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(type STREQUAL "EXECUTABLE")
            list(APPEND found ${target})
        endif()
    endforeach()
    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(subdir IN LISTS subdirs)
        xt_collect_executables("${subdir}" sub)
        list(APPEND found ${sub})
    endforeach()
    set(${out} ${found} PARENT_SCOPE)
endfunction()

# xt_set_runtime_dir(<target> <dir>): RUNTIME_OUTPUT_DIRECTORY and every per-configuration variant.
function(xt_set_runtime_dir target dir)
    set(configs ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE} Debug Release RelWithDebInfo MinSizeRel)
    list(REMOVE_DUPLICATES configs)
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${dir}")
    foreach(config IN LISTS configs)
        string(TOUPPER "${config}" config_upper)
        set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${dir}")
    endforeach()
endfunction()

# xt_apply_runtime_output_dir(): places the executables of the directory
# xt_runtime_output_dir_deferred() recorded: `xtframe-*` in XT_RUNTIME_OUTPUT_DIR, the ones defined
# under Examples/ in its examples/, the ones under Tests/ (and every other `*_tests`) in its tests/.
# Executables of the fetched third-party projects (outside this source tree) are left alone.
function(xt_apply_runtime_output_dir)
    if("${XT_RUNTIME_OUTPUT_DIR}" STREQUAL "")
        return()
    endif()
    get_property(root GLOBAL PROPERTY XT_RUNTIME_OUTPUT_ROOT)
    xt_collect_executables("${root}" executables)
    set(tools "")
    set(n_examples 0)
    set(n_tests 0)
    foreach(target IN LISTS executables)
        get_target_property(src ${target} SOURCE_DIR)
        file(RELATIVE_PATH rel "${root}" "${src}")
        if(rel MATCHES "^(\\.\\./|Code/SDKs/)")
            continue()
        endif()
        if(target MATCHES "^xtframe-")
            list(APPEND tools ${target})
            xt_set_runtime_dir(${target} "${XT_RUNTIME_OUTPUT_DIR}")
        elseif(rel MATCHES "^Examples(/|$)")
            math(EXPR n_examples "${n_examples} + 1")
            xt_set_runtime_dir(${target} "${XT_RUNTIME_OUTPUT_DIR}/examples")
        elseif(rel MATCHES "^Tests(/|$)" OR target MATCHES "_tests$")
            math(EXPR n_tests "${n_tests} + 1")
            xt_set_runtime_dir(${target} "${XT_RUNTIME_OUTPUT_DIR}/tests")
        endif()
    endforeach()
    if("${XT_RUNTIME_OUTPUT_DIR}" MATCHES "^([^$]*)/\\$<CONFIG>(-[^/$]*)?$")
        set(shown "${CMAKE_MATCH_1}/<Config>${CMAKE_MATCH_2}")
    elseif("${XT_RUNTIME_OUTPUT_DIR}" MATCHES "\\$<")
        set(shown "(generator expression)")
    else()
        set(shown "${XT_RUNTIME_OUTPUT_DIR}")
    endif()
    message(STATUS "XT_RUNTIME_OUTPUT_DIR: ${shown} (${tools}; examples/: ${n_examples}, tests/: ${n_tests})")
endfunction()

# xt_runtime_output_dir_deferred(): call once from this tree's top-level CMakeLists.txt.
function(xt_runtime_output_dir_deferred)
    set_property(GLOBAL PROPERTY XT_RUNTIME_OUTPUT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL xt_apply_runtime_output_dir)
endfunction()

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
    elseif(EMSCRIPTEN)
        # T-503: the Node.js/wasm build of the tools; one tag, there is no second wasm architecture.
        set(${out} wasm32 PARENT_SCOPE)
        return()
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
    _xt_copy_shared_libraries_now()
    _xt_check_no_d3dcompiler()
endfunction()

# _xt_check_no_d3dcompiler() (T-457): a configure warning when XT_RUNTIME_OUTPUT_DIR, its tests/ or its
# examples/ holds a D3Dcompiler_47.dll. One there (Qt's, deployed by an editor build before T-457)
# replaces the system copy for every executable of that directory and cannot compile ps_5_1, so each
# D3D12 pipeline of xt_wgpu is invalid. The editor's windeployqt step no longer deploys it and removes
# a stale copy next to its own target; this names the ones it cannot see.
function(_xt_check_no_d3dcompiler)
    if(NOT WIN32 OR "${XT_RUNTIME_OUTPUT_DIR}" STREQUAL "")
        return()
    endif()
    set(configs ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    if(NOT configs)
        set(configs Debug)
    endif()
    list(REMOVE_DUPLICATES configs)
    foreach(config IN LISTS configs)
        string(REPLACE "$<CONFIG>" "${config}" dir "${XT_RUNTIME_OUTPUT_DIR}")
        if(dir MATCHES "\\$<")
            return()
        endif()
        foreach(sub "" "/tests" "/examples")
            if(EXISTS "${dir}${sub}/D3Dcompiler_47.dll")
                message(WARNING "${dir}${sub}/D3Dcompiler_47.dll: a D3D compiler in the shared output "
                                "directory makes every D3D12 pipeline of xt_wgpu invalid there; delete it "
                                "(the editor's windeployqt step no longer deploys it, T-457)")
            endif()
        endforeach()
    endforeach()
endfunction()

# -- Shared libraries next to the executables (T-437, Docs/design/10-backends.md §10.10.1) --------
# A prebuilt third-party shared library (an IMPORTED SHARED target, e.g. xt::wgpu_native) has to sit
# next to the executables that load it, as the Qt DLLs do (windeployqt, Code/Editor/Qt). Two ways:
#   - xt_runtime_shared_library(<imported>): copied into XT_RUNTIME_OUTPUT_DIR, its tests/ and its
#     examples/ for every configuration at the end of the configure step (the deferred
#     xt_apply_runtime_output_dir()), so bin/<tag>/<Config>/ holds it before anything is built;
#   - xt_copy_runtime_library(<target> <imported>): copied next to <target> after each build of it
#     (also when XT_RUNTIME_OUTPUT_DIR is empty, as in a parent project's tree).

# xt_runtime_shared_library(<imported>): registers <imported> for the configure-time copy.
function(xt_runtime_shared_library imported)
    set_property(GLOBAL APPEND PROPERTY XT_RUNTIME_SHARED_LIBRARIES ${imported})
endfunction()

# xt_copy_runtime_library(<target> <imported>): POST_BUILD copy of <imported>'s file next to <target>.
function(xt_copy_runtime_library target imported)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${imported}>" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
endfunction()

# _xt_copy_shared_libraries_now(): the configure-time copy of every registered library into
# XT_RUNTIME_OUTPUT_DIR (with `$<CONFIG>` replaced by each configuration), its tests/ and examples/.
function(_xt_copy_shared_libraries_now)
    get_property(libs GLOBAL PROPERTY XT_RUNTIME_SHARED_LIBRARIES)
    if(NOT libs OR "${XT_RUNTIME_OUTPUT_DIR}" STREQUAL "")
        return()
    endif()
    set(configs ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    if(NOT configs)
        set(configs Debug)
    endif()
    list(REMOVE_DUPLICATES configs)
    foreach(lib IN LISTS libs)
        get_target_property(file ${lib} IMPORTED_LOCATION)
        if(NOT file OR NOT EXISTS "${file}")
            continue()
        endif()
        foreach(config IN LISTS configs)
            string(REPLACE "$<CONFIG>" "${config}" dir "${XT_RUNTIME_OUTPUT_DIR}")
            if(dir MATCHES "\\$<")
                return()  # another generator expression: left to xt_copy_runtime_library
            endif()
            foreach(sub "" "/tests" "/examples")
                file(COPY "${file}" DESTINATION "${dir}${sub}")
            endforeach()
        endforeach()
    endforeach()
endfunction()

# -- The preset symbol library next to the executables (T-198, T-474, T-498) ----------------------
# xtframe-pack, xtframe-design and xtframe-editor read Assets/Symbols (the whole library: library.json
# and groups/, Docs/design/49-symbol-sets.md 49.2) from `<executable dir>/symbols`.
# With XT_RUNTIME_OUTPUT_DIR set they share one directory, so one custom target, xt_copy_symbols,
# copies it there once per build and each of them depends on it: separate POST_BUILD copies into the
# same directory raced under a parallel --clean-first build (copy_directory failed with "No such file
# or directory"). Without XT_RUNTIME_OUTPUT_DIR (a parent project's tree) the custom target copies
# next to each registered executable, one after the other.

# xt_copy_standard_symbols(<target>): registers <target> and makes it depend on xt_copy_symbols.
function(xt_copy_standard_symbols target)
    set_property(GLOBAL APPEND PROPERTY XT_COPY_SYMBOLS_TARGETS ${target})
    add_dependencies(${target} xt_copy_symbols)
endfunction()

# _xt_make_copy_symbols_target(): the deferred definition of xt_copy_symbols (the executables'
# output directories are only final after xt_apply_runtime_output_dir()).
function(_xt_make_copy_symbols_target)
    get_property(targets GLOBAL PROPERTY XT_COPY_SYMBOLS_TARGETS)
    if(NOT targets)
        return()
    endif()
    get_property(root GLOBAL PROPERTY XT_RUNTIME_OUTPUT_ROOT)
    set(dirs "")
    if(NOT "${XT_RUNTIME_OUTPUT_DIR}" STREQUAL "")
        list(APPEND dirs "${XT_RUNTIME_OUTPUT_DIR}/symbols")
    else()
        foreach(target IN LISTS targets)
            list(APPEND dirs "$<TARGET_FILE_DIR:${target}>/symbols")
        endforeach()
    endif()
    set(commands "")
    foreach(dir IN LISTS dirs)
        list(APPEND commands
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${root}/Assets/Symbols" "${dir}")
    endforeach()
    add_custom_target(xt_copy_symbols ${commands}
        COMMENT "Copying the preset symbol library"
        VERBATIM)
endfunction()

# -- The standard fonts next to the executables (T-527b, 22 §22.10.3) ------------------------------
# Every executable that gets the symbol library also gets Assets/Fonts (Noto Sans, Noto Serif, Noto Sans TC and
# their OFL files; not README.md) in `<executable dir>/fonts`, where xtframe-pack's `--fonts-dir`
# default looks. Same shape as xt_copy_symbols: one shared custom target, xt_copy_fonts.

# xt_copy_standard_fonts(<target>): registers <target> and makes it depend on xt_copy_fonts.
function(xt_copy_standard_fonts target)
    set_property(GLOBAL APPEND PROPERTY XT_COPY_FONTS_TARGETS ${target})
    add_dependencies(${target} xt_copy_fonts)
endfunction()

# _xt_make_copy_fonts_target(): the deferred definition of xt_copy_fonts.
function(_xt_make_copy_fonts_target)
    get_property(targets GLOBAL PROPERTY XT_COPY_FONTS_TARGETS)
    if(NOT targets)
        return()
    endif()
    get_property(root GLOBAL PROPERTY XT_RUNTIME_OUTPUT_ROOT)
    file(GLOB files LIST_DIRECTORIES false "${root}/Assets/Fonts/*")
    list(FILTER files EXCLUDE REGEX "/README\\.md$")
    list(SORT files)
    set(dirs "")
    if(NOT "${XT_RUNTIME_OUTPUT_DIR}" STREQUAL "")
        list(APPEND dirs "${XT_RUNTIME_OUTPUT_DIR}/fonts")
    else()
        foreach(target IN LISTS targets)
            list(APPEND dirs "$<TARGET_FILE_DIR:${target}>/fonts")
        endforeach()
    endif()
    set(commands "")
    foreach(dir IN LISTS dirs)
        list(APPEND commands
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${files} "${dir}")
    endforeach()
    add_custom_target(xt_copy_fonts ${commands}
        COMMENT "Copying the standard fonts"
        VERBATIM)
endfunction()

# xt_runtime_output_dir_deferred(): call once from this tree's top-level CMakeLists.txt.
function(xt_runtime_output_dir_deferred)
    set_property(GLOBAL PROPERTY XT_RUNTIME_OUTPUT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL xt_apply_runtime_output_dir)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _xt_make_copy_symbols_target)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _xt_make_copy_fonts_target)
endfunction()

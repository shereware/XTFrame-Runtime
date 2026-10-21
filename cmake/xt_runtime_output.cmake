# docs/design/23-host-integration.md §23.ac (T-382, T-383; Aux Duty XF-033 / XF-045):
# XT_RUNTIME_OUTPUT_DIR for every `xtframe-*` executable target of this tree.
#
# xt_runtime_output_dir_deferred() schedules xt_apply_runtime_output_dir() for the end of the
# top-level source directory (CMAKE_SOURCE_DIR; cmake_language(DEFER)), so the value is read after
# the whole configure step: a parent project that adds this tree with add_subdirectory() may set
# XT_RUNTIME_OUTPUT_DIR (a normal or a cache variable of its top-level directory) before or after
# that call. When this tree is the top-level project the call runs at the end of its own
# CMakeLists.txt. The value may hold generator expressions (`$<CONFIG>`); the status message does
# not print them (CMake cannot evaluate them at configure time).

# xt_collect_tool_targets(<dir> <out>): every EXECUTABLE target named `xtframe-*` of <dir> and its
# subdirectories.
function(xt_collect_tool_targets dir out)
    set(found "")
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(type STREQUAL "EXECUTABLE" AND target MATCHES "^xtframe-")
            list(APPEND found ${target})
        endif()
    endforeach()
    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(subdir IN LISTS subdirs)
        xt_collect_tool_targets("${subdir}" sub)
        list(APPEND found ${sub})
    endforeach()
    set(${out} ${found} PARENT_SCOPE)
endfunction()

# xt_apply_runtime_output_dir(): sets RUNTIME_OUTPUT_DIRECTORY (and every per-configuration
# variant) of each tool target under the directory xt_runtime_output_dir_deferred() recorded.
function(xt_apply_runtime_output_dir)
    if("${XT_RUNTIME_OUTPUT_DIR}" STREQUAL "")
        return()
    endif()
    get_property(root GLOBAL PROPERTY XT_RUNTIME_OUTPUT_ROOT)
    xt_collect_tool_targets("${root}" xt_tool_targets)
    set(xt_output_configs ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE} Debug Release RelWithDebInfo MinSizeRel)
    list(REMOVE_DUPLICATES xt_output_configs)
    foreach(target IN LISTS xt_tool_targets)
        set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${XT_RUNTIME_OUTPUT_DIR}")
        foreach(config IN LISTS xt_output_configs)
            string(TOUPPER "${config}" config_upper)
            set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config_upper}
                                                       "${XT_RUNTIME_OUTPUT_DIR}")
        endforeach()
    endforeach()
    if("${XT_RUNTIME_OUTPUT_DIR}" MATCHES "\\$<")
        message(STATUS "XT_RUNTIME_OUTPUT_DIR: (generator expression) (${xt_tool_targets})")
    else()
        message(STATUS "XT_RUNTIME_OUTPUT_DIR: ${XT_RUNTIME_OUTPUT_DIR} (${xt_tool_targets})")
    endif()
endfunction()

# xt_runtime_output_dir_deferred(): call once from this tree's top-level CMakeLists.txt.
function(xt_runtime_output_dir_deferred)
    set_property(GLOBAL PROPERTY XT_RUNTIME_OUTPUT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL xt_apply_runtime_output_dir)
endfunction()

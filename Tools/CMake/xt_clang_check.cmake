# T-435 (Docs/design/15-testing.md §15.8): the clang++ syntax-check stage of Tools/Scripts/local_ci.sh takes
# each source file's preprocessor definitions and include directories from this build tree instead of
# hand-kept lists in the script (those drifted: a test target's new XT_*_DIR definition or the
# nlohmann ABI namespace of Tools/CMake/xt_nlohmann.cmake never reached the check). The Visual Studio
# generators write no compile_commands.json, so the same data is written here with file(GENERATE):
# one text file per first-party target, <build>/xt_clang_check/<target>.txt, holding
#   S <the target's source directory>
#   D <one compile definition>   (the transitive COMPILE_DEFINITIONS, usage requirements included)
#   I <one include directory>    (the transitive INCLUDE_DIRECTORIES)
#   F <one source file>          (as listed; a relative path is relative to S)
# A multi-configuration generator writes the Release values (the check runs with -DNDEBUG).
# Only the top-level project writes them; targets outside the source tree or under Code/SDKs/
# (FetchContent, vendored code) are left out.

# xt_clang_check_collect(<dir> <out>): every buildsystem target of <dir> and its subdirectories.
function(xt_clang_check_collect dir out)
    set(result "")
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    list(APPEND result ${targets})
    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(sub IN LISTS subdirs)
        xt_clang_check_collect("${sub}" sub_targets)
        list(APPEND result ${sub_targets})
    endforeach()
    set(${out} "${result}" PARENT_SCOPE)
endfunction()

function(xt_clang_check_write)
    if(NOT PROJECT_IS_TOP_LEVEL)
        return()
    endif()
    xt_clang_check_collect("${CMAKE_SOURCE_DIR}" targets)
    set(out_dir "${CMAKE_BINARY_DIR}/xt_clang_check")
    file(REMOVE_RECURSE "${out_dir}")
    if(CMAKE_CONFIGURATION_TYPES)
        set(condition "$<CONFIG:Release>")
    else()
        set(condition "1")
    endif()
    set(nl "\n")
    foreach(t IN LISTS targets)
        get_target_property(type ${t} TYPE)
        if(NOT type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
            continue()
        endif()
        get_target_property(src_dir ${t} SOURCE_DIR)
        file(RELATIVE_PATH rel "${CMAKE_SOURCE_DIR}" "${src_dir}")
        if(rel MATCHES "^\\.\\." OR rel MATCHES "^Code/SDKs(/|$)")
            continue()
        endif()
        set(defs "$<TARGET_PROPERTY:${t},COMPILE_DEFINITIONS>")
        set(incs "$<TARGET_PROPERTY:${t},INCLUDE_DIRECTORIES>")
        set(srcs "$<TARGET_PROPERTY:${t},SOURCES>")
        file(GENERATE OUTPUT "${out_dir}/${t}.txt"
             CONTENT "S ${src_dir}\n$<$<BOOL:${defs}>:D $<JOIN:${defs},${nl}D >${nl}>$<$<BOOL:${incs}>:I $<JOIN:${incs},${nl}I >${nl}>$<$<BOOL:${srcs}>:F $<JOIN:${srcs},${nl}F >${nl}>"
             CONDITION "${condition}" TARGET ${t})
    endforeach()
endfunction()

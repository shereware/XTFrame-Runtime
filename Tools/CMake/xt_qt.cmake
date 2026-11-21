# T-442 (Docs/design/17-editor.md §17.9, 45 §45.3): which Qt the editor is built against.
#
# Order:
#   1. XT_QT_DIR (cache variable, initialised from the XT_QT_DIR environment variable; the
#      vs2022-editor preset passes $env{XT_QT_DIR} through), when not empty;
#   2. Code/SDKs/Qt/<dir>[/Qt] holding the highest Qt 5.15.x (lib/cmake/Qt5 plus this platform's qmake);
#   3. the same for the highest Qt 5.12.x;
#   4. (T-852) in a git worktree without Code/SDKs/Qt: steps 2 and 3 under the main worktree (the
#      directory above `git rev-parse --path-format=absolute --git-common-dir`); no git: skipped;
#   5. nothing: find_package() keeps its own search (CMAKE_PREFIX_PATH / Qt5_DIR / Qt6_DIR from the
#      environment, qmake on PATH).
# The chosen prefix is put in front of CMAKE_PREFIX_PATH and reported as
#   xt_qt: using <path> (Qt <version>)
# Code/SDKs/Qt is git-ignored (Qt is never committed); see Code/SDKs/Qt/README.md.
# Tools/Scripts/dev_check.sh and local_ci.sh use the same order (Tools/Scripts/xt_qt_env.sh).

include_guard(GLOBAL)

set(XT_QT_DIR "$ENV{XT_QT_DIR}" CACHE PATH
    "Qt prefix for the editor (holds lib/cmake/Qt5 or Qt6); empty: the newest Qt 5.15, then 5.12, under Code/SDKs/Qt")

# _xt_qt_version(<prefix> <out>): the Qt version of an install prefix, empty when it has none.
function(_xt_qt_version prefix out)
    set(version "")
    foreach(file "lib/cmake/Qt5/Qt5ConfigVersion.cmake" "lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake"
                 "lib/cmake/Qt6/Qt6ConfigVersion.cmake")
        if(NOT version AND EXISTS "${prefix}/${file}")
            file(STRINGS "${prefix}/${file}" line REGEX "set\\(PACKAGE_VERSION \"?[0-9.]+\"?\\)" LIMIT_COUNT 1)
            if(line MATCHES "PACKAGE_VERSION \"?([0-9.]+)")
                set(version "${CMAKE_MATCH_1}")
            endif()
        endif()
    endforeach()
    set(${out} "${version}" PARENT_SCOPE)
endfunction()

# _xt_qt_sdk_pick(<base> <series> <out_dir> <out_version>): the highest Qt <series> (e.g. 5.15) under
# <base>/Code/SDKs/Qt; a directory counts when it (or its Qt/ subdirectory) has lib/cmake/Qt5 and a qmake
# for this platform (a Windows Qt seen from WSL is skipped).
function(_xt_qt_sdk_pick base series out_dir out_version)
    set(best_dir "")
    set(best_version "")
    if(WIN32)
        set(qmake "bin/qmake.exe")
    else()
        set(qmake "bin/qmake")
    endif()
    file(GLOB entries LIST_DIRECTORIES true "${base}/Code/SDKs/Qt/${series}*")
    foreach(entry IN LISTS entries)
        foreach(prefix "${entry}/Qt" "${entry}")
            if(IS_DIRECTORY "${prefix}/lib/cmake/Qt5" AND EXISTS "${prefix}/${qmake}")
                _xt_qt_version("${prefix}" version)
                if(version MATCHES "^${series}\\." AND (NOT best_version OR version VERSION_GREATER best_version))
                    set(best_dir "${prefix}")
                    set(best_version "${version}")
                endif()
                break()
            endif()
        endforeach()
    endforeach()
    set(${out_dir} "${best_dir}" PARENT_SCOPE)
    set(${out_version} "${best_version}" PARENT_SCOPE)
endfunction()

# _xt_qt_main_worktree(<out>): the main worktree of the git checkout holding CMAKE_SOURCE_DIR when
# that is a linked worktree (T-852); empty for the main worktree itself, without git or outside git.
function(_xt_qt_main_worktree out)
    set(main "")
    find_package(Git QUIET)
    if(GIT_EXECUTABLE)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --path-format=absolute --git-common-dir
                        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE common RESULT_VARIABLE rc
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(rc EQUAL 0 AND common)
            file(TO_CMAKE_PATH "${common}" common)
            get_filename_component(main "${common}" DIRECTORY)
            if(main STREQUAL CMAKE_SOURCE_DIR OR NOT IS_DIRECTORY "${main}/Code/SDKs/Qt")
                set(main "")
            endif()
        endif()
    endif()
    set(${out} "${main}" PARENT_SCOPE)
endfunction()

# xt_qt_select(): sets XT_QT_SELECTED_DIR / XT_QT_SELECTED_VERSION and prepends the prefix to
# CMAKE_PREFIX_PATH (a macro, so the change reaches the caller's scope).
macro(xt_qt_select)
    set(XT_QT_SELECTED_DIR "")
    set(XT_QT_SELECTED_VERSION "")
    set(_xt_qt_from "")
    if(XT_QT_DIR)
        file(TO_CMAKE_PATH "${XT_QT_DIR}" XT_QT_SELECTED_DIR)
        _xt_qt_version("${XT_QT_SELECTED_DIR}" XT_QT_SELECTED_VERSION)
    else()
        foreach(_xt_qt_series 5.15 5.12)
            if(NOT XT_QT_SELECTED_DIR)
                _xt_qt_sdk_pick("${CMAKE_SOURCE_DIR}" ${_xt_qt_series} XT_QT_SELECTED_DIR XT_QT_SELECTED_VERSION)
            endif()
        endforeach()
        if(NOT XT_QT_SELECTED_DIR)
            _xt_qt_main_worktree(_xt_qt_main)
            if(_xt_qt_main)
                foreach(_xt_qt_series 5.15 5.12)
                    if(NOT XT_QT_SELECTED_DIR)
                        _xt_qt_sdk_pick("${_xt_qt_main}" ${_xt_qt_series} XT_QT_SELECTED_DIR XT_QT_SELECTED_VERSION)
                    endif()
                endforeach()
                if(XT_QT_SELECTED_DIR)
                    set(_xt_qt_from ", from the main worktree ${_xt_qt_main}")
                endif()
            endif()
        endif()
    endif()
    if(XT_QT_SELECTED_DIR)
        # A tree configured against another Qt keeps Qt5_DIR / Qt5Core_DIR / ... in its cache, and
        # find_package() would keep using them; drop them when the prefix changes.
        # (A tree from before T-442 has no XT_QT_LAST_DIR: its Qt entries are dropped once.)
        if((DEFINED CACHE{XT_QT_LAST_DIR} AND NOT XT_QT_LAST_DIR STREQUAL XT_QT_SELECTED_DIR)
           OR NOT DEFINED CACHE{XT_QT_LAST_DIR})
            get_cmake_property(_xt_qt_cache CACHE_VARIABLES)
            foreach(_xt_qt_var IN LISTS _xt_qt_cache)
                if(_xt_qt_var MATCHES "^Qt[56][A-Za-z0-9]*_DIR$")
                    unset(${_xt_qt_var} CACHE)
                endif()
            endforeach()
        endif()
        set(XT_QT_LAST_DIR "${XT_QT_SELECTED_DIR}" CACHE INTERNAL "Qt prefix of the previous configure")
        list(PREPEND CMAKE_PREFIX_PATH "${XT_QT_SELECTED_DIR}")
        if(XT_QT_SELECTED_VERSION)
            message(STATUS "xt_qt: using ${XT_QT_SELECTED_DIR} (Qt ${XT_QT_SELECTED_VERSION}${_xt_qt_from})")
        else()
            message(STATUS "xt_qt: using ${XT_QT_SELECTED_DIR} (Qt version unknown: no lib/cmake/Qt5 or Qt6 there${_xt_qt_from})")
        endif()
    else()
        message(STATUS "xt_qt: XT_QT_DIR is empty and Code/SDKs/Qt (here or in the main worktree) has no Qt 5.15 / 5.12; "
                       "find_package uses CMAKE_PREFIX_PATH / Qt5_DIR / Qt6_DIR")
    endif()
endmacro()

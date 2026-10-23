# T-442 (Docs/design/17-editor.md §17.9, 45 §45.3): which Qt the editor is built against.
#
# Order:
#   1. XT_QT_DIR (cache variable, initialised from the XT_QT_DIR environment variable; the
#      vs2022-editor preset passes $env{XT_QT_DIR} through), when not empty;
#   2. Code/SDKs/Qt/<dir>[/Qt] holding the highest Qt 5.15.x (lib/cmake/Qt5 plus this platform's qmake);
#   3. the same for the highest Qt 5.12.x;
#   4. nothing: find_package() keeps its own search (CMAKE_PREFIX_PATH / Qt5_DIR / Qt6_DIR from the
#      environment, qmake on PATH).
# The chosen prefix is put in front of CMAKE_PREFIX_PATH and reported as
#   xt_qt: using <path> (Qt <version>)
# Code/SDKs/Qt is git-ignored (Qt is never committed); see Code/SDKs/Qt/README.md.
# Tools/Scripts/dev_check.sh and local_ci.sh use the same order (xt_qt_sdk_dir there).

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

# _xt_qt_sdk_pick(<series> <out_dir> <out_version>): the highest Qt <series> (e.g. 5.15) under
# Code/SDKs/Qt; a directory counts when it (or its Qt/ subdirectory) has lib/cmake/Qt5 and a qmake
# for this platform (a Windows Qt seen from WSL is skipped).
function(_xt_qt_sdk_pick series out_dir out_version)
    set(best_dir "")
    set(best_version "")
    if(WIN32)
        set(qmake "bin/qmake.exe")
    else()
        set(qmake "bin/qmake")
    endif()
    file(GLOB entries LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/Code/SDKs/Qt/${series}*")
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

# xt_qt_select(): sets XT_QT_SELECTED_DIR / XT_QT_SELECTED_VERSION and prepends the prefix to
# CMAKE_PREFIX_PATH (a macro, so the change reaches the caller's scope).
macro(xt_qt_select)
    set(XT_QT_SELECTED_DIR "")
    set(XT_QT_SELECTED_VERSION "")
    if(XT_QT_DIR)
        file(TO_CMAKE_PATH "${XT_QT_DIR}" XT_QT_SELECTED_DIR)
        _xt_qt_version("${XT_QT_SELECTED_DIR}" XT_QT_SELECTED_VERSION)
    else()
        foreach(_xt_qt_series 5.15 5.12)
            if(NOT XT_QT_SELECTED_DIR)
                _xt_qt_sdk_pick(${_xt_qt_series} XT_QT_SELECTED_DIR XT_QT_SELECTED_VERSION)
            endif()
        endforeach()
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
            message(STATUS "xt_qt: using ${XT_QT_SELECTED_DIR} (Qt ${XT_QT_SELECTED_VERSION})")
        else()
            message(STATUS "xt_qt: using ${XT_QT_SELECTED_DIR} (Qt version unknown: no lib/cmake/Qt5 or Qt6 there)")
        endif()
    else()
        message(STATUS "xt_qt: XT_QT_DIR is empty and Code/SDKs/Qt has no Qt 5.15 / 5.12; "
                       "find_package uses CMAKE_PREFIX_PATH / Qt5_DIR / Qt6_DIR")
    endif()
endmacro()

# Third-party version table and shared fetched sources (Docs/design/45-repository-layout.md §45.3,
# Docs/tasks/T-430.md items 8 and 10).
#
# Reads Code/SDKs/versions.cmake. Every FetchContent dependency is fetched once into
# Code/SDKs/_fetched/<name>/ (git-ignored) and shared by every build tree, instead of each build
# tree's _deps/<name>-src:
#   - a complete copy (its `.xt-sdk` stamp names the same source and ref as versions.cmake) is used
#     as is through FETCHCONTENT_SOURCE_DIR_<NAME>: nothing is downloaded;
#   - otherwise FetchContent downloads into that same directory (FetchContent_Declare SOURCE_DIR)
#     and the stamp is written afterwards;
#   - XT_SDK_OFFLINE=ON never downloads: a missing copy is a configure error naming the directory
#     and the source to put there. A non-empty directory without a matching stamp is used as is.
# A host that sets FETCHCONTENT_SOURCE_DIR_<NAME> itself (23 §23.ac) keeps its own directory.
#
# Tools/CMake/xt_{sdl2,text_deps,httplib,clipper2,miniz}.cmake call xt_sdk_fetch(); their target settings
# are unchanged.
#
# PREBUILT entries (T-437, Docs/design/10-backends.md §10.10.1) are per-platform binary archives:
# xt_sdk_prebuilt() unpacks the one of this platform into Code/SDKs/_fetched/<name>/<platform>/ and,
# unlike xt_sdk_fetch(), never stops the configure step -- a platform without an archive, a failed
# download or XT_SDK_OFFLINE=ON without the directory only report "not available" to the caller,
# which then builds without the component.

include_guard(GLOBAL)
include(FetchContent)

option(XT_SDK_OFFLINE "Never download third-party sources: Code/SDKs/_fetched/<name> must exist" OFF)

set(XT_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../Code/SDKs")
get_filename_component(XT_SDK_ROOT "${XT_SDK_ROOT}" ABSOLUTE)
set(XT_SDK_FETCHED_DIR "${XT_SDK_ROOT}/_fetched")

# xt_sdk(<name> <URL|GIT|VENDORED|ASSET|PREBUILT> <location> <hash|tag|version> [notice columns...]):
# one line of Code/SDKs/versions.cmake. Only the first four arguments matter to CMake, plus, for
# PREBUILT, the URL_<platform> / SHA256_<platform> pairs; the notice columns (TITLE, HOME, VERSION,
# ...) are read by Tools/Scripts/gen_third_party.py.
function(xt_sdk name kind location ref)
    if(NOT kind MATCHES "^(URL|GIT|VENDORED|ASSET|PREBUILT)$")
        message(FATAL_ERROR "Code/SDKs/versions.cmake: xt_sdk(${name}) has unknown kind '${kind}'")
    endif()
    set_property(GLOBAL PROPERTY XT_SDK_${name}_KIND "${kind}")
    set_property(GLOBAL PROPERTY XT_SDK_${name}_LOCATION "${location}")
    set_property(GLOBAL PROPERTY XT_SDK_${name}_REF "${ref}")
    if(kind STREQUAL "PREBUILT")
        set(args ${ARGN})
        list(LENGTH args n)
        set(i 0)
        while(i LESS n)
            list(GET args ${i} key)
            math(EXPR j "${i} + 1")
            if(key MATCHES "^(URL|SHA256)_[a-z0-9_]+$" AND j LESS n)
                list(GET args ${j} value)
                set_property(GLOBAL PROPERTY XT_SDK_${name}_${key} "${value}")
            endif()
            math(EXPR i "${i} + 2")
        endwhile()
    endif()
endfunction()

include("${XT_SDK_ROOT}/versions.cmake")

# xt_sdk_fetch(<name> <content> [FetchContent_Declare options...]): declares and makes available the
# FetchContent dependency <content> from the versions.cmake entry <name>, with its source in
# Code/SDKs/_fetched/<name>/. A macro, so <content>_SOURCE_DIR / _BINARY_DIR land in the caller's
# scope as FetchContent_MakeAvailable() sets them.
macro(xt_sdk_fetch _xt_sdk_name _xt_sdk_content)
    get_property(_xt_sdk_kind GLOBAL PROPERTY XT_SDK_${_xt_sdk_name}_KIND)
    get_property(_xt_sdk_loc GLOBAL PROPERTY XT_SDK_${_xt_sdk_name}_LOCATION)
    get_property(_xt_sdk_ref GLOBAL PROPERTY XT_SDK_${_xt_sdk_name}_REF)
    if(NOT _xt_sdk_kind MATCHES "^(URL|GIT)$")
        message(FATAL_ERROR "xt_sdk_fetch(${_xt_sdk_name}): no URL/GIT entry in Code/SDKs/versions.cmake")
    endif()
    set(_xt_sdk_dir "${XT_SDK_FETCHED_DIR}/${_xt_sdk_name}")
    set(_xt_sdk_stamp_text "${_xt_sdk_kind} ${_xt_sdk_loc} ${_xt_sdk_ref}\n")
    string(TOUPPER "${_xt_sdk_content}" _xt_sdk_upper)
    set(_xt_sdk_stamp_ok FALSE)
    if(EXISTS "${_xt_sdk_dir}/.xt-sdk")
        file(READ "${_xt_sdk_dir}/.xt-sdk" _xt_sdk_stamp)
        if(_xt_sdk_stamp STREQUAL _xt_sdk_stamp_text)
            set(_xt_sdk_stamp_ok TRUE)
        endif()
    endif()
    file(GLOB _xt_sdk_entries "${_xt_sdk_dir}/*")
    set(_xt_sdk_download FALSE)
    if(NOT "${FETCHCONTENT_SOURCE_DIR_${_xt_sdk_upper}}" STREQUAL "")
        message(STATUS "xt_sdk ${_xt_sdk_name}: FETCHCONTENT_SOURCE_DIR_${_xt_sdk_upper} given (${FETCHCONTENT_SOURCE_DIR_${_xt_sdk_upper}})")
    elseif(_xt_sdk_stamp_ok OR (XT_SDK_OFFLINE AND _xt_sdk_entries))
        set(FETCHCONTENT_SOURCE_DIR_${_xt_sdk_upper} "${_xt_sdk_dir}")
    elseif(XT_SDK_OFFLINE)
        message(FATAL_ERROR
            "XT_SDK_OFFLINE=ON and the source of '${_xt_sdk_name}' is missing.\n"
            "Put ${_xt_sdk_kind} ${_xt_sdk_loc} (${_xt_sdk_ref}) unpacked at:\n  ${_xt_sdk_dir}\n"
            "(versions: Code/SDKs/versions.cmake; or configure once with XT_SDK_OFFLINE=OFF to fetch it)")
    else()
        set(_xt_sdk_download TRUE)
        message(STATUS "xt_sdk ${_xt_sdk_name}: fetching ${_xt_sdk_loc} into ${_xt_sdk_dir}")
        # A previous download of this build tree (an older version, or a copy deleted since) leaves
        # stamps in the sub-build that would skip the download; start that sub-build afresh.
        if(FETCHCONTENT_BASE_DIR)
            set(_xt_sdk_base "${FETCHCONTENT_BASE_DIR}")
        else()
            set(_xt_sdk_base "${CMAKE_BINARY_DIR}/_deps")
        endif()
        string(TOLOWER "${_xt_sdk_content}" _xt_sdk_lower)
        file(REMOVE_RECURSE "${_xt_sdk_base}/${_xt_sdk_lower}-subbuild")
    endif()
    if(_xt_sdk_kind STREQUAL "URL")
        FetchContent_Declare(${_xt_sdk_content}
            URL "${_xt_sdk_loc}"
            URL_HASH SHA256=${_xt_sdk_ref}
            SOURCE_DIR "${_xt_sdk_dir}"
            ${ARGN})
    else()
        FetchContent_Declare(${_xt_sdk_content}
            GIT_REPOSITORY "${_xt_sdk_loc}"
            GIT_TAG "${_xt_sdk_ref}"
            GIT_SHALLOW TRUE
            SOURCE_DIR "${_xt_sdk_dir}"
            ${ARGN})
    endif()
    FetchContent_MakeAvailable(${_xt_sdk_content})
    if(_xt_sdk_download)
        file(WRITE "${_xt_sdk_dir}/.xt-sdk" "${_xt_sdk_stamp_text}")
    endif()
endmacro()

# xt_sdk_prebuilt(<name> <platform> <out_dir> <out_reason>): the PREBUILT entry <name> of
# Code/SDKs/versions.cmake for <platform> (XT_PLATFORM_TAG, e.g. win_x64), unpacked. Sets <out_dir> to
# Code/SDKs/_fetched/<name>/<platform> (or to FETCHCONTENT_SOURCE_DIR_<NAME>, '-' read as '_', when the
# host gives one) and <out_reason> to "", or <out_dir> to "" and <out_reason> to why it is not
# available. Never a configure error:
#   - a complete copy (its `.xt-sdk` stamp names the same URL and hash) is used as is;
#   - XT_SDK_OFFLINE=ON uses a non-empty directory as is and otherwise reports it missing;
#   - otherwise the archive is downloaded into the build tree, its SHA-256 checked, and unpacked.
function(xt_sdk_prebuilt name platform out_dir out_reason)
    set(${out_dir} "" PARENT_SCOPE)
    get_property(kind GLOBAL PROPERTY XT_SDK_${name}_KIND)
    if(NOT kind STREQUAL "PREBUILT")
        set(${out_reason} "no PREBUILT entry '${name}' in Code/SDKs/versions.cmake" PARENT_SCOPE)
        return()
    endif()
    string(TOUPPER "${name}" upper)
    string(REPLACE "-" "_" upper "${upper}")
    if(NOT "${FETCHCONTENT_SOURCE_DIR_${upper}}" STREQUAL "")
        if(IS_DIRECTORY "${FETCHCONTENT_SOURCE_DIR_${upper}}")
            message(STATUS "xt_sdk ${name}: FETCHCONTENT_SOURCE_DIR_${upper} given (${FETCHCONTENT_SOURCE_DIR_${upper}})")
            set(${out_dir} "${FETCHCONTENT_SOURCE_DIR_${upper}}" PARENT_SCOPE)
            set(${out_reason} "" PARENT_SCOPE)
        else()
            set(${out_reason} "FETCHCONTENT_SOURCE_DIR_${upper} (${FETCHCONTENT_SOURCE_DIR_${upper}}) is not a directory" PARENT_SCOPE)
        endif()
        return()
    endif()
    get_property(url GLOBAL PROPERTY XT_SDK_${name}_URL_${platform})
    get_property(sha GLOBAL PROPERTY XT_SDK_${name}_SHA256_${platform})
    if(url STREQUAL "" OR sha STREQUAL "")
        set(${out_reason} "no prebuilt archive for ${platform}" PARENT_SCOPE)
        return()
    endif()
    set(dir "${XT_SDK_FETCHED_DIR}/${name}/${platform}")
    set(stamp_text "PREBUILT ${url} ${sha}\n")
    if(EXISTS "${dir}/.xt-sdk")
        file(READ "${dir}/.xt-sdk" stamp)
        if(stamp STREQUAL stamp_text)
            set(${out_dir} "${dir}" PARENT_SCOPE)
            set(${out_reason} "" PARENT_SCOPE)
            return()
        endif()
    endif()
    file(GLOB entries "${dir}/*")
    if(XT_SDK_OFFLINE)
        if(entries)
            set(${out_dir} "${dir}" PARENT_SCOPE)
            set(${out_reason} "" PARENT_SCOPE)
        else()
            set(${out_reason} "XT_SDK_OFFLINE=ON and ${dir} is missing (put ${url} unpacked there)" PARENT_SCOPE)
        endif()
        return()
    endif()
    message(STATUS "xt_sdk ${name}: fetching ${url} into ${dir}")
    set(archive "${CMAKE_BINARY_DIR}/_deps/${name}-${platform}.zip")
    file(DOWNLOAD "${url}" "${archive}" STATUS status TLS_VERIFY ON)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        list(GET status 1 msg)
        file(REMOVE "${archive}")
        set(${out_reason} "download of ${url} failed (${msg})" PARENT_SCOPE)
        return()
    endif()
    file(SHA256 "${archive}" got)
    if(NOT got STREQUAL sha)
        file(REMOVE "${archive}")
        set(${out_reason} "${url} has SHA-256 ${got}, versions.cmake expects ${sha}" PARENT_SCOPE)
        return()
    endif()
    file(REMOVE_RECURSE "${dir}")
    file(MAKE_DIRECTORY "${dir}")
    # `cmake -E tar` in a child process, so a broken archive is a reason rather than a configure error;
    # --touch: some archives' time stamps cannot be restored on Windows.
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xf "${archive}" --touch
                    WORKING_DIRECTORY "${dir}" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
    file(REMOVE "${archive}")
    if(NOT rc EQUAL 0)
        file(REMOVE_RECURSE "${dir}")
        set(${out_reason} "unpacking ${url} failed (${err})" PARENT_SCOPE)
        return()
    endif()
    file(WRITE "${dir}/.xt-sdk" "${stamp_text}")
    set(${out_dir} "${dir}" PARENT_SCOPE)
    set(${out_reason} "" PARENT_SCOPE)
endfunction()

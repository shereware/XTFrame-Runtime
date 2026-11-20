# CTest helper (T-805): runs one command-line tool with the system temp directory (TMP, TEMP and
# TMPDIR) pointed at an empty directory of its own, checks the exit code exactly, and fails when
# anything is left in that directory afterwards -- a tool removes what it creates there, on success
# and on failure alike. Invoked as
#   cmake -DROOT=<scratch dir> -DEXPECT_CODE=<n> [-DEXPECT_OUTPUT=<regex>] [-DBLOCKER=<path>]
#         -P tool_temp_clean.cmake -- <command> <args>...
# ROOT is emptied first. BLOCKER, when given, is (re)created as a small regular file before the run,
# for a command that should fail because a directory it writes to is a file.
foreach(var ROOT EXPECT_CODE)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "tool_temp_clean.cmake: -D${var}=... is required")
    endif()
endforeach()

set(xt_cmd "")
set(xt_after_dashes FALSE)
foreach(i RANGE ${CMAKE_ARGC})
    if(i EQUAL CMAKE_ARGC)
        break()
    endif()
    if(xt_after_dashes)
        list(APPEND xt_cmd "${CMAKE_ARGV${i}}")
    elseif("${CMAKE_ARGV${i}}" STREQUAL "--")
        set(xt_after_dashes TRUE)
    endif()
endforeach()
if(NOT xt_cmd)
    message(FATAL_ERROR "tool_temp_clean.cmake: no command after --")
endif()

file(REMOVE_RECURSE "${ROOT}")
file(MAKE_DIRECTORY "${ROOT}")
if(DEFINED BLOCKER)
    file(REMOVE_RECURSE "${BLOCKER}")
    get_filename_component(xt_blocker_dir "${BLOCKER}" DIRECTORY)
    file(MAKE_DIRECTORY "${xt_blocker_dir}")
    file(WRITE "${BLOCKER}" "not a directory\n")
endif()
set(ENV{TMP} "${ROOT}")
set(ENV{TEMP} "${ROOT}")
set(ENV{TMPDIR} "${ROOT}")

execute_process(COMMAND ${xt_cmd} RESULT_VARIABLE xt_code OUTPUT_VARIABLE xt_out ERROR_VARIABLE xt_err)
set(xt_all "${xt_out}${xt_err}")
message("${xt_all}")
if(NOT "${xt_code}" STREQUAL "${EXPECT_CODE}")
    message(FATAL_ERROR "expected exit code ${EXPECT_CODE}, got ${xt_code}")
endif()
if(DEFINED EXPECT_OUTPUT AND NOT xt_all MATCHES "${EXPECT_OUTPUT}")
    message(FATAL_ERROR "output does not match '${EXPECT_OUTPUT}'")
endif()

file(GLOB_RECURSE xt_left LIST_DIRECTORIES true RELATIVE "${ROOT}" "${ROOT}/*")
if(xt_left)
    list(JOIN xt_left "\n  " xt_left_lines)
    message(FATAL_ERROR "the temp directory ${ROOT} is not empty after the run:\n  ${xt_left_lines}")
endif()
file(REMOVE_RECURSE "${ROOT}")
if(DEFINED BLOCKER)
    file(REMOVE "${BLOCKER}")
endif()

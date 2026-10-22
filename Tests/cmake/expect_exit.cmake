# CTest helper (T-274): runs one command and checks its exit code exactly, which plain WILL_FAIL
# cannot (it only checks for a non-zero code). Invoked as
#   cmake -DEXPECT_CODE=<n> [-DEXPECT_OUTPUT=<regex>] [-DREJECT_OUTPUT=<regex>] -P expect_exit.cmake -- <command> <args>...
# EXPECT_OUTPUT must match, REJECT_OUTPUT must not match, the combined stdout + stderr.
#
# T-267a: include()d from a CMakeLists.txt (not run with -P), it only defines
#   xt_add_expect_exit_test(<name> CODE <n> [OUTPUT <regex>] [REJECT <regex>]
#                           [WORKING_DIRECTORY <dir>] COMMAND <command> <args>...)
# which adds a CTest entry running <command> through this script. Every CLI test uses it instead of
# PASS_REGULAR_EXPRESSION, which makes CTest ignore the exit code (a crash after the expected line
# passed). <command> is a path or a $<TARGET_FILE:...> expression, not a bare target name.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    function(xt_add_expect_exit_test name)
        cmake_parse_arguments(PARSE_ARGV 1 arg "" "CODE;OUTPUT;REJECT;WORKING_DIRECTORY" "COMMAND")
        if(NOT DEFINED arg_CODE OR NOT arg_COMMAND)
            message(FATAL_ERROR "xt_add_expect_exit_test(${name}): CODE and COMMAND are required")
        endif()
        set(defs "-DEXPECT_CODE=${arg_CODE}")
        if(DEFINED arg_OUTPUT)
            list(APPEND defs "-DEXPECT_OUTPUT=${arg_OUTPUT}")
        endif()
        if(DEFINED arg_REJECT)
            list(APPEND defs "-DREJECT_OUTPUT=${arg_REJECT}")
        endif()
        set(wd)
        if(DEFINED arg_WORKING_DIRECTORY)
            set(wd WORKING_DIRECTORY "${arg_WORKING_DIRECTORY}")
        endif()
        add_test(NAME ${name}
                 COMMAND ${CMAKE_COMMAND} ${defs} -P "${PROJECT_SOURCE_DIR}/Tests/cmake/expect_exit.cmake" --
                         ${arg_COMMAND}
                 ${wd})
    endfunction()
    return()
endif()

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
    message(FATAL_ERROR "expect_exit.cmake: no command after --")
endif()
execute_process(COMMAND ${xt_cmd} RESULT_VARIABLE xt_code OUTPUT_VARIABLE xt_out ERROR_VARIABLE xt_err)
set(xt_all "${xt_out}${xt_err}")
message("${xt_all}")
if(NOT "${xt_code}" STREQUAL "${EXPECT_CODE}")
    message(FATAL_ERROR "expected exit code ${EXPECT_CODE}, got ${xt_code}")
endif()
if(DEFINED EXPECT_OUTPUT AND NOT xt_all MATCHES "${EXPECT_OUTPUT}")
    message(FATAL_ERROR "output does not match '${EXPECT_OUTPUT}'")
endif()
if(DEFINED REJECT_OUTPUT AND xt_all MATCHES "${REJECT_OUTPUT}")
    message(FATAL_ERROR "output matches '${REJECT_OUTPUT}'")
endif()

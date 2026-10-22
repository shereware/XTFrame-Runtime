# CTest helper (T-267d): a test executable removes its per-process temp root
# (<temp>/xt_tests_<pid>, Tests/common/test_temp.h) after a passing run, and keeps it when
# XT_TEST_KEEP_TEMP=1. Runs one passing case that uses the temp root twice, with the system temp
# directory pointed at an empty scratch directory, and looks at what is left there. Invoked as
#   cmake -DEXE=<test executable> -DCASE=<doctest test case name> -DROOT=<scratch dir> -P temp_root_check.cmake
foreach(var EXE CASE ROOT)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "temp_root_check.cmake: -D${var}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${ROOT}")
file(MAKE_DIRECTORY "${ROOT}")
set(ENV{TMP} "${ROOT}")
set(ENV{TEMP} "${ROOT}")
set(ENV{TMPDIR} "${ROOT}")

function(run_case keep)
    if(keep)
        set(ENV{XT_TEST_KEEP_TEMP} 1)
    else()
        unset(ENV{XT_TEST_KEEP_TEMP})
    endif()
    execute_process(COMMAND "${EXE}" "--test-case=${CASE}" RESULT_VARIABLE code OUTPUT_VARIABLE out
                    ERROR_VARIABLE err)
    message("${out}${err}")
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "expected exit code 0, got ${code}")
    endif()
    if(NOT out MATCHES "test cases: *1 \\| *1 passed")
        message(FATAL_ERROR "expected exactly one passing test case")
    endif()
    set(out "${out}" PARENT_SCOPE)
endfunction()

run_case(FALSE)
file(GLOB left "${ROOT}/xt_tests_*")
if(left)
    message(FATAL_ERROR "a passing run left its temp root behind: ${left}")
endif()

run_case(TRUE)
file(GLOB kept "${ROOT}/xt_tests_*")
if(NOT kept)
    message(FATAL_ERROR "XT_TEST_KEEP_TEMP=1 did not keep the temp root")
endif()
if(NOT out MATCHES "temp files kept: ")
    message(FATAL_ERROR "XT_TEST_KEEP_TEMP=1 did not print 'temp files kept: <path>'")
endif()

file(REMOVE_RECURSE "${ROOT}")

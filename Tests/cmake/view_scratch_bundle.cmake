# CTest helper (T-571, report F111 and 22 §22.14): runs xtframe-view on a UI Project and checks that
# the scratch Bundle it packed into is per process (`.../xtframe-view/<id>-<hash>-<pid>`) and is gone
# once the process has exited, and that the unused strings-table keys come as one summary line.
# Invoked as
#   cmake -DVIEW=<xtframe-view> -DPROJECT=<ui project dir> -DID=<project id> -DSUMMARY=<regex>
#         -P view_scratch_bundle.cmake
execute_process(COMMAND "${VIEW}" "${PROJECT}" --headless --frames 1
                RESULT_VARIABLE xt_code OUTPUT_VARIABLE xt_out ERROR_VARIABLE xt_err)
set(xt_all "${xt_out}${xt_err}")
string(REPLACE "\r" "" xt_all "${xt_all}")
message("${xt_all}")
if(NOT "${xt_code}" STREQUAL "0")
    message(FATAL_ERROR "expected exit code 0, got ${xt_code}")
endif()
if(NOT xt_all MATCHES "xtframe-view: packed [^\n]* -> ([^\n]*)")
    message(FATAL_ERROR "no 'xtframe-view: packed ... -> <bundle>' line")
endif()
set(xt_bundle "${CMAKE_MATCH_1}")
if(NOT xt_bundle MATCHES "/xtframe-view/${ID}-[0-9a-f]+-[0-9]+$")
    message(FATAL_ERROR "scratch Bundle '${xt_bundle}' is not <id>-<hash>-<pid>")
endif()
if(EXISTS "${xt_bundle}")
    message(FATAL_ERROR "scratch Bundle '${xt_bundle}' is still there after the process exited")
endif()
if(NOT xt_all MATCHES "${SUMMARY}")
    message(FATAL_ERROR "output does not match '${SUMMARY}'")
endif()
if(xt_all MATCHES "i18n: unused key '")
    message(FATAL_ERROR "per-key unused lines without --verbose")
endif()

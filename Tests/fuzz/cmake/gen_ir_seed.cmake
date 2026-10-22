# Helper invoked by Tests/fuzz/CMakeLists.txt's IR seed corpus custom commands. Runs xtframe-compile
# on one seed file and, if that fails (e.g. Tests/xt's deliberately-invalid *.xt/err_* fixtures),
# still produces an empty placeholder at OUT so the custom command's OUTPUT exists and the build
# does not fail on those expected compile errors. See Docs/tasks/T-39.md "種子語料".
execute_process(COMMAND "${XTFRAME_COMPILE}" "${SEED}" -o "${OUT}")
if(NOT EXISTS "${OUT}")
    file(WRITE "${OUT}" "")
endif()

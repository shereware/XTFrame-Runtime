# Helper invoked by Tests/fuzz/CMakeLists.txt's `.xtcl` seed corpus custom commands
# (Docs/tasks/T-267b.md): compiles SEED with XTFRAME_COMPILE, bakes 3 frames at 10 fps with
# XTFRAME_BAKE into OUT, and writes an empty placeholder at OUT when either step fails so the
# custom command's OUTPUT always exists.
set(ir "${OUT}.xtir")
execute_process(COMMAND "${XTFRAME_COMPILE}" "${SEED}" -o "${ir}")
if(EXISTS "${ir}")
    execute_process(COMMAND "${XTFRAME_BAKE}" "${ir}" -o "${OUT}" --fps 10 --range 0,0.3)
    file(REMOVE "${ir}")
endif()
if(NOT EXISTS "${OUT}")
    file(WRITE "${OUT}" "")
endif()

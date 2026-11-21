# CTest helper (T-827): what the CLI tools do with a UI they cannot run as given.
# Invoked as
#   cmake -DMODE=old_ir|bad_flow -DPACK=<xtframe-pack> -DVIEW=<xtframe-view> [-DPROBE=<xtframe-gpu-probe>]
#         -DPROJECT=<UI Project dir holding the UI 'flip'> -DOLD_IR=<an IR major 2 .xtir>
#         -DOUT=<scratch dir> -P t827_tools.cmake
# Both modes pack PROJECT into OUT/bundle first.
#   old_ir:   OUT/bundle/ui/flip.xtir is replaced by OLD_IR; xtframe-view (and xtframe-gpu-probe when
#             given) exit 1 and say why (IrView::openError(): "... recompile the source").
#   bad_flow: the UI's own flow is broken -- in the Bundle (manifest `flow`) and next to a `.xt` in a
#             copy of PROJECT. xtframe-view --headless --screenshot still draws the UI, prints one
#             `flow: flip: <problem>` line and exits 0. A broken flow given with --flow still exits 2.
cmake_minimum_required(VERSION 3.20)

function(t827_run expect_code expect_regex)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(all "${out}${err}")
    string(REPLACE "\r" "" all "${all}")
    message("$ ${ARGN}\n${all}")
    if(NOT "${code}" STREQUAL "${expect_code}")
        message(FATAL_ERROR "expected exit code ${expect_code}, got ${code}")
    endif()
    if(NOT all MATCHES "${expect_regex}")
        message(FATAL_ERROR "output does not match '${expect_regex}'")
    endif()
endfunction()

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(bundle "${OUT}/bundle")
execute_process(COMMAND "${PACK}" "${PROJECT}" -o "${bundle}" --no-thumbs --compress none --no-cache
                RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT "${code}" STREQUAL "0" OR NOT EXISTS "${bundle}/ui/flip.xtir")
    message(FATAL_ERROR "xtframe-pack failed (${code}):\n${out}${err}")
endif()

if(MODE STREQUAL "old_ir")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${OLD_IR}" "${bundle}/ui/flip.xtir")
    t827_run(1 "flip\\.xtir' \\(kInvalidIr: IR major 2 is not supported[^\n]*recompile the source\\)"
             "${VIEW}" "${bundle}" --ui flip --headless --frames 1 --no-flow)
    t827_run(1 "flip\\.xtir' \\(kInvalidIr: IR major 2 is not supported[^\n]*recompile the source\\)"
             "${VIEW}" "${bundle}/ui/flip.xtir" --headless --frames 1)
    if(PROBE)
        t827_run(1 "cannot load ui 'flip' \\(kInvalidIr: IR major 2 is not supported[^\n]*recompile the source\\)"
                 "${PROBE}" "${bundle}" flip 64 64 1)
    endif()
elseif(MODE STREQUAL "bad_flow")
    file(WRITE "${bundle}/ui/flip.xtflow" "{}\n")
    t827_run(0 "xtframe-view: flow: flip: malformed \\.xtflow document"
             "${VIEW}" "${bundle}" --ui flip --headless --frames 1 --screenshot "${OUT}/bundle.png")
    if(NOT EXISTS "${OUT}/bundle.png")
        message(FATAL_ERROR "no screenshot from the Bundle with a broken flow")
    endif()
    # A `.xt` whose sibling `.xtflow` is broken.
    file(COPY "${PROJECT}/" DESTINATION "${OUT}/project")
    file(WRITE "${OUT}/project/flip/flip.xtflow" "{}\n")
    t827_run(0 "xtframe-view: flow: flip: malformed \\.xtflow document"
             "${VIEW}" "${OUT}/project/flip/flip.xt" --headless --frames 1 --screenshot "${OUT}/xt.png")
    if(NOT EXISTS "${OUT}/xt.png")
        message(FATAL_ERROR "no screenshot from the .xt with a broken flow")
    endif()
    # A flow named with --flow is an argument: still exit 2.
    t827_run(2 "xtframe-view: flow: [^\n]*malformed \\.xtflow document"
             "${VIEW}" "${bundle}" --ui flip --headless --frames 1 --flow "${bundle}/ui/flip.xtflow")
else()
    message(FATAL_ERROR "t827_tools.cmake: unknown MODE '${MODE}'")
endif()

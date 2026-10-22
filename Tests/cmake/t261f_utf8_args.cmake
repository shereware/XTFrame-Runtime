# T-261f item 1/2 (Docs/tasks/T-261f.md): every tool reads its arguments as UTF-8, so a path the
# ANSI code page cannot spell reaches it intact. Run as
#   cmake -DCASE=<case> -DTOOL=<exe> -DWORK=<dir> -DSOURCE=<repo root> [-DCOMPILE=<exe>] [-DPACK=<exe>]
#         -P t261f_utf8_args.cmake
# For each of two names -- one the CP950 code page can spell, one it cannot (kana and Hangul) -- it
# creates `<WORK>/<case>/<name>/` (the non-ASCII files only ever exist here, at test time), runs the
# tool on a file named after it, and checks the exit code, the output and the file written.
# This file is UTF-8 (CMake reads scripts as UTF-8 and runs commands through the wide API).
foreach(var CASE TOOL WORK SOURCE)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "t261f_utf8_args.cmake: -D${var}=... is required")
    endif()
endforeach()

set(t261f_names "測試" "テスト글꼴")
set(t261f_root "${WORK}/${CASE}")
file(REMOVE_RECURSE "${t261f_root}")
file(MAKE_DIRECTORY "${t261f_root}")
set(t261f_empty "${t261f_root}/empty-stdin.txt")
file(WRITE "${t261f_empty}" "")

# t261f_run(<expected code> <output regex> <command>...): runs the command (stdin empty) and fails
# the test on another exit code or an output that does not match. A run is stopped after 180 s
# (a Debug build's abort() waits on a message box).
function(t261f_run code regex)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE got OUTPUT_VARIABLE out ERROR_VARIABLE err
                    INPUT_FILE "${t261f_empty}" TIMEOUT 180)
    set(all "${out}${err}")
    message("${all}")
    if(NOT "${got}" STREQUAL "${code}")
        message(FATAL_ERROR "[${name}] expected exit code ${code}, got ${got}: ${ARGN}")
    endif()
    if(NOT all MATCHES "${regex}")
        message(FATAL_ERROR "[${name}] output does not match '${regex}': ${ARGN}")
    endif()
endfunction()

function(t261f_expect_file path)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "[${name}] not written: ${path}")
    endif()
endfunction()

# A copy of Tests/xt/doc_basic.xt named `<dir>/xt/<name>.xt`, with the stylesheet, font, image and
# animation it links at the same relative places.
function(t261f_stage_xt dir name)
    file(COPY "${SOURCE}/Tests/xt/doc_theme.xts" "${SOURCE}/Tests/xt/doc_font.ttf" DESTINATION "${dir}/xt")
    file(COPY "${SOURCE}/Tests/lottie/min_solid_anim.json" DESTINATION "${dir}/lottie")
    file(COPY "${SOURCE}/Tests/lottie/img/pix.png" DESTINATION "${dir}/lottie/img")
    configure_file("${SOURCE}/Tests/xt/doc_basic.xt" "${dir}/xt/${name}.xt" COPYONLY)
endfunction()

foreach(name IN LISTS t261f_names)
    set(dir "${t261f_root}/${name}")
    file(MAKE_DIRECTORY "${dir}")
    if(CASE STREQUAL "compile")
        t261f_stage_xt("${dir}" "${name}")
        t261f_run(0 "wrote " "${TOOL}" "${dir}/xt/${name}.xt" -o "${dir}/out/${name}.xtir")
        t261f_expect_file("${dir}/out/${name}.xtir")
    elseif(CASE STREQUAL "bake")
        t261f_run(0 "wrote " "${COMPILE}" "${SOURCE}/Tests/lottie/min_solid_anim.json"
                  -o "${dir}/${name}.xtir")
        t261f_run(0 "wrote " "${TOOL}" "${dir}/${name}.xtir" -o "${dir}/out/${name}.xtcl" --fps 10)
        t261f_expect_file("${dir}/out/${name}.xtcl")
    elseif(CASE STREQUAL "view")
        t261f_stage_xt("${dir}" "${name}")
        t261f_run(0 "wrote screenshot " "${TOOL}" "${dir}/xt/${name}.xt" --headless --frames 1 --no-flow
                  --width 64 --height 64 --screenshot "${dir}/${name}.png")
        t261f_expect_file("${dir}/${name}.png")
        # A Bundle there too (its UI, flow and asset paths are joined onto the Bundle directory).
        t261f_run(0 "wrote 2 UI" "${PACK}" "${SOURCE}/Tests/bundle/fixtures/sample" -o "${dir}/${name}-bundle"
                  --no-thumbs)
        t261f_run(0 "headless run complete" "${TOOL}" "${dir}/${name}-bundle" --ui hud --headless --frames 1)
    elseif(CASE STREQUAL "gpu_probe")
        t261f_run(0 "wrote 2 UI" "${PACK}" "${SOURCE}/Tests/bundle/fixtures/sample" -o "${dir}/${name}"
                  --no-thumbs)
        t261f_run(0 "last frame dumped to " "${TOOL}" "${dir}/${name}" hud 64 64 1
                  --dump "${dir}/${name}.txt")
        t261f_expect_file("${dir}/${name}.txt")
    elseif(CASE STREQUAL "editor_bake_vectors")
        configure_file("${SOURCE}/Tests/bundle/fixtures/unbaked_modifier/main.xt" "${dir}/${name}.xt" COPYONLY)
        t261f_run(0 "xtframe-editor: baked 2 group" "${TOOL}" --bake-vectors "${dir}/${name}.xt"
                  -o "${dir}-copy")
        t261f_expect_file("${dir}-copy/${name}.xt")
    elseif(CASE STREQUAL "editor_service_schemas")
        t261f_run(0 "" "${TOOL}" --service --schemas "${dir}/${name}")
        t261f_expect_file("${dir}/${name}/index.json")
    elseif(CASE STREQUAL "mcp")
        # A service file naming a port nothing listens on: the adapter reads it (so it reports that
        # port, not "no running xtframe-editor found"), probes it, and ends at the empty stdin.
        file(WRITE "${dir}/${name}.json" "{\"port\": 9, \"token\": \"t261f\"}")
        t261f_run(0 "xtframe-editor at port 9 does not answer" "${TOOL}" --service-file "${dir}/${name}.json")
    else()
        message(FATAL_ERROR "t261f_utf8_args.cmake: unknown CASE '${CASE}'")
    endif()
endforeach()

# T-671: smoke test of xt_gpu_mem_probe (Tests/perf/CMakeLists.txt). Runs one round over two UIs of the
# capi `flip` fixture bundle and checks the CSV: the header, one row per sample point (start, bundle_open,
# target_create, 2 x scene, teardown, end) and no empty cell where the probe must have measured something.
#   cmake -DPROBE=<exe> -DBUNDLE=<packed bundle dir> -DOUT=<csv> -P gpu_mem_probe_smoke.cmake
execute_process(
    COMMAND "${PROBE}" --bundle "${BUNDLE}" --out "${OUT}" --sequence flip,lit --rounds 1 --frames-per-scene 3
            --width 256 --height 256 --msaa 4 --mode hide --trim-each-scene 1 --image-budget default
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}")
if(out MATCHES "SKIP: no WebGPU adapter")
    return()
endif()
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "xt_gpu_mem_probe exit ${rc}: ${err}")
endif()
# A bad argument must be refused with exit code 2.
execute_process(COMMAND "${PROBE}" --bundle "${BUNDLE}" --out "${OUT}.bad" --msaa 2 RESULT_VARIABLE bad
                OUTPUT_QUIET ERROR_QUIET)
if(NOT bad EQUAL 2)
    message(FATAL_ERROR "--msaa 2 gave exit ${bad}, expected 2")
endif()

file(STRINGS "${OUT}" lines)
list(LENGTH lines count)
if(NOT count EQUAL 8)
    message(FATAL_ERROR "expected 1 header + 7 rows, got ${count} lines")
endif()
list(GET lines 0 header)
if(NOT header MATCHES "^round,scene,phase,ui,private_bytes,working_set_bytes,dxgi_local_bytes,dxgi_nonlocal_bytes,image_textures,.*,first_frame_ms,steady_frame_ms,elapsed_ms$")
    message(FATAL_ERROR "unexpected header: ${header}")
endif()
set(expected_phases start bundle_open target_create scene scene teardown end)
# Columns that must not be empty, per phase (0-based): private, working set (all rows); on scene rows also
# the library's memory stats (8..16) and the frame times (17, 18); on Windows DXGI local (6) once the
# target exists.
foreach(i RANGE 1 7)
    list(GET lines ${i} row)
    string(REPLACE "," ";" cells "${row},")
    math(EXPR p "${i} - 1")
    list(GET expected_phases ${p} want)
    list(GET cells 2 phase)
    if(NOT phase STREQUAL want)
        message(FATAL_ERROR "row ${i}: phase '${phase}', expected '${want}'")
    endif()
    set(required 4 5)
    if(phase STREQUAL "scene")
        list(APPEND required 8 9 10 11 12 13 14 15 16 17 18)
        if(WIN32 OR CMAKE_HOST_WIN32)
            list(APPEND required 6)
        endif()
    endif()
    foreach(c IN LISTS required)
        list(GET cells ${c} v)
        if(v STREQUAL "")
            message(FATAL_ERROR "row ${i} (${phase}): column ${c} is empty: ${row}")
        endif()
    endforeach()
endforeach()
message("xt_gpu_mem_probe_smoke: OK (${count} lines)")

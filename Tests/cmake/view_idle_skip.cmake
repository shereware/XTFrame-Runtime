# T-749 (Docs/design/23-host-integration.md §23.ag item 6): xtframe-view --idle-skip draws the same frames as a
# run that never skips. The flip fixture (Tests/capi/fixtures/flip) idles, takes a click at frame 30 (its flow
# toggles `flipped`, starting a 1 s opacity transition) and idles again; the screenshots at frames inside and
# after the transition must be byte for byte those of the run without --idle-skip, and frames must be skipped.
#   cmake -DVIEW=<xtframe-view> -DPROJECT=<Flip fixture.xtui> -DOUT=<dir> -P view_idle_skip.cmake
file(MAKE_DIRECTORY "${OUT}")
foreach(frames 40 75 100 200)
    foreach(mode base idle)
        set(extra)
        if(mode STREQUAL "idle")
            set(extra --idle-skip --perf)
        endif()
        execute_process(
            COMMAND "${VIEW}" "${PROJECT}" --ui flip --headless --width 64 --height 64 --frames ${frames}
                    --pointer "30 32 32 move" --pointer "30 32 32 down" --pointer "30 32 32 up"
                    --screenshot "${OUT}/flip-${mode}-${frames}.png" ${extra}
            RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "xtframe-view (${mode}, ${frames} frames): exit ${rc}: ${out}${err}")
        endif()
        if(mode STREQUAL "idle")
            if(NOT out MATCHES "idle-skipped frames: ([0-9]+) of ${frames}")
                message(FATAL_ERROR "--idle-skip --perf printed no skipped-frame count: ${out}")
            endif()
            if(CMAKE_MATCH_1 LESS 20)
                message(FATAL_ERROR "--idle-skip skipped only ${CMAKE_MATCH_1} of ${frames} frames")
            endif()
        endif()
    endforeach()
    file(SHA256 "${OUT}/flip-base-${frames}.png" base_hash)
    file(SHA256 "${OUT}/flip-idle-${frames}.png" idle_hash)
    if(NOT base_hash STREQUAL idle_hash)
        message(FATAL_ERROR "frame ${frames}: the --idle-skip screenshot differs from the run without it")
    endif()
endforeach()
message("view idle skip: PASS")

# T-817: xtframe-view --advance <seconds> runs the frames before that time without painting; the first
# picture it paints must be byte for byte the one a run stepping frame by frame paints at the same time
# (software renderer). Frame k of a plain run shows time k/60, so `--frames 121`'s last picture is the
# one at 2 s. Each pair is also checked against a picture from another time, so a document that stopped
# changing cannot pass by accident. Also --seek (T-172), which had no test.
#   cmake -DVIEW=<xtframe-view> -DDOC=<Tests/xt/view_advance.xt> -DPROJECT=<Flip fixture.xtui> -DOUT=<dir>
#         -P view_advance.cmake
file(MAKE_DIRECTORY "${OUT}")

# shot(<name> <arguments>...): one --headless run writing ${OUT}/<name>.png; a failing run fails the test.
function(shot name)
    execute_process(COMMAND "${VIEW}" ${ARGN} --headless --screenshot "${OUT}/${name}.png"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "xtframe-view (${name}): exit ${rc}: ${out}${err}")
    endif()
endfunction()

function(expect_same a b)
    file(SHA256 "${OUT}/${a}.png" ha)
    file(SHA256 "${OUT}/${b}.png" hb)
    if(NOT ha STREQUAL hb)
        message(FATAL_ERROR "${a}.png and ${b}.png differ")
    endif()
endfunction()

function(expect_different a b)
    file(SHA256 "${OUT}/${a}.png" ha)
    file(SHA256 "${OUT}/${b}.png" hb)
    if(ha STREQUAL hb)
        message(FATAL_ERROR "${a}.png and ${b}.png are the same picture (the document did not change)")
    endif()
endfunction()

set(doc "${DOC}" --width 160 --height 64)

# 1. A timeline started by on:show, a looping CSS animation and particles: 2 s, and 0.5 s.
shot(doc-f1 ${doc} --frames 1)
shot(doc-f121 ${doc} --frames 121)
shot(doc-a2 ${doc} --advance 2 --frames 1)
expect_same(doc-f121 doc-a2)
expect_different(doc-f1 doc-f121)
shot(doc-f31 ${doc} --frames 31)
shot(doc-a05 ${doc} --advance 0.5 --frames 1)
expect_same(doc-f31 doc-a05)
expect_different(doc-f31 doc-f121)
# --frames after --advance go on from that time: --advance 1 --frames 61 ends at 2 s.
shot(doc-a1f61 ${doc} --advance 1 --frames 61)
expect_same(doc-f121 doc-a1f61)
# --advance 0 changes nothing.
shot(doc-a0 ${doc} --advance 0 --frames 1)
expect_same(doc-f1 doc-a0)

# 2. A timeline started by the flow's lifecycle.shown (Tests/capi/fixtures/flip, UI `intro`: 0.5 s of
#    opacity): 0.25 s.
set(intro "${PROJECT}" --ui intro --width 64 --height 64)
shot(intro-f1 ${intro} --frames 1)
shot(intro-f16 ${intro} --frames 16)
shot(intro-a025 ${intro} --advance 0.25 --frames 1)
expect_same(intro-f16 intro-a025)
expect_different(intro-f1 intro-f16)

# 3. Scripted input inside the advanced span is sent at its frame: the flip UI's click at frame 30
#    starts a 1 s opacity transition; 1 s.
set(flip "${PROJECT}" --ui flip --width 64 --height 64
    --pointer "30 32 32 move" --pointer "30 32 32 down" --pointer "30 32 32 up")
shot(flip-f1 ${flip} --frames 1)
shot(flip-f61 ${flip} --frames 61)
shot(flip-a1 ${flip} --advance 1 --frames 1)
expect_same(flip-f61 flip-a1)
expect_different(flip-f1 flip-f61)

# 4. --seek (T-172): --play slide --seek moves the root timeline at frame 0; --seek then --advance.
shot(seek-play ${doc} --play slide --frames 1)
shot(seek-05 ${doc} --play slide --seek 0.5 --frames 1)
expect_different(seek-play seek-05)
shot(seek-05-f31 ${doc} --play slide --seek 0.5 --frames 31)
shot(seek-05-a05 ${doc} --play slide --seek 0.5 --advance 0.5 --frames 1)
expect_same(seek-05-f31 seek-05-a05)
expect_different(seek-05 seek-05-f31)

message("view advance: PASS")

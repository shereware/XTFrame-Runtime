# T-713: smoke test of xtframe-bench's --json escaping, --system-fonts and --event (Tests/capi/CMakeLists.txt).
#   cmake -DBENCH=<xtframe-bench> -DFLIP=<packed flip fixture> -DHUD=<packed hud-overlay> -P bench_smoke.cmake
function(run_bench out_var)
    execute_process(COMMAND "${BENCH}" ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "xtframe-bench ${ARGN}: exit ${rc}: ${out}${err}")
    endif()
    set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

# 1. --json: the bundle path as given (native separators: backslashes on Windows) round-trips through JSON.
file(TO_NATIVE_PATH "${FLIP}" flip_native)
run_bench(json "${flip_native}" flip --frames 2 --warmup 0 --json)
string(JSON bundle ERROR_VARIABLE json_error GET "${json}" bundle)
if(json_error)
    message(FATAL_ERROR "--json is not valid JSON (${json_error}): ${json}")
endif()
if(NOT bundle STREQUAL flip_native)
    message(FATAL_ERROR "--json bundle '${bundle}', expected '${flip_native}'")
endif()
string(JSON has_fonts ERROR_VARIABLE no_key GET "${json}" system_fonts)
if(NOT no_key)
    message(FATAL_ERROR "the default --json object has a system_fonts member: ${json}")
endif()

# 2. --system-fonts: hud-overlay brings no font, so only with the system fonts is its text drawn.
run_bench(plain "${HUD}" hud --frames 2 --warmup 0 --json)
run_bench(fonts "${HUD}" hud --frames 2 --warmup 0 --json --system-fonts)
string(JSON plain_fnv GET "${plain}" pixels_fnv)
string(JSON fonts_fnv GET "${fonts}" pixels_fnv)
string(JSON fonts_flag GET "${fonts}" system_fonts)
if(plain_fnv STREQUAL fonts_fnv OR NOT fonts_flag EQUAL 1)
    message(FATAL_ERROR "--system-fonts changed nothing (fnv ${plain_fnv} / ${fonts_fnv}, flag ${fonts_flag})")
endif()

# 3. --event: a click (pointer down + up on the 64 x 64 card at frame 2) reaches the flow, which toggles
#    `flipped` (blue, fading out) -- the last frame differs from a run without it.
run_bench(still "${FLIP}" flip --frames 30 --warmup 0 --json)
run_bench(clicked "${FLIP}" flip --frames 30 --warmup 0 --json --event "2 pointer down 32 32"
          --event "2 pointer up 32 32")
string(JSON still_fnv GET "${still}" pixels_fnv)
string(JSON clicked_fnv GET "${clicked}" pixels_fnv)
string(JSON clicked_events GET "${clicked}" events)
if(still_fnv STREQUAL clicked_fnv OR NOT clicked_events EQUAL 2)
    message(FATAL_ERROR "--event pointer down/up changed nothing (fnv ${still_fnv} / ${clicked_fnv})")
endif()
# A port write and a system event are taken; an unknown port is reported and the run goes on.
execute_process(COMMAND "${BENCH}" "${FLIP}" ghost --frames 2 --warmup 0 --event "1 port show true"
                        --event "1 port nope 1" --event "0 system paused"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT err MATCHES "--event at frame 1 \\(nope\\): no such port")
    message(FATAL_ERROR "port / system events: exit ${rc}: ${out}${err}")
endif()
# #<id>.Click needs an element box the C ABI does not give: refused with exit 2.
execute_process(COMMAND "${BENCH}" "${FLIP}" flip --event "2 #card.Click"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT err MATCHES "cannot parse --event '2 #card.Click'")
    message(FATAL_ERROR "#card.Click: exit ${rc} (expected 2): ${err}")
endif()
message("xtframe-bench smoke: PASS")

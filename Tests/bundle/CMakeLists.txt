add_executable(xt_bundle_tests
    ${PROJECT_SOURCE_DIR}/Tests/unit/test_main.cpp
    test_project_file.cpp
    test_bundle.cpp
    test_pack.cpp
    test_bundle_load.cpp
    test_localization.cpp
    test_ui_project.cpp
    test_gen_ports.cpp
    test_flow_interface.cpp
    test_asset_pipeline.cpp
    test_parser_limits.cpp
    test_file_safety.cpp
    test_design_system.cpp
    test_design_doc.cpp
    test_design_export.cpp
    test_asset_meta.cpp
    test_project_runtime.cpp
    test_project_edit.cpp
    test_source_versions.cpp
    test_source_v2_readers.cpp
    test_roundtrip.cpp
    test_symbol_library.cpp
    test_symbol_namespaced.cpp
    test_project_style.cpp
    test_symbol_json.cpp
    test_zip_archive.cpp
    test_bundle_fonts.cpp
    test_default_language.cpp
    test_pack_stale.cpp
    test_xtb_cache.cpp
    test_publish_settings.cpp
)
target_include_directories(xt_bundle_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/Code/SDKs/doctest)
# doctest needs the real standard headers, otherwise it conflicts with newer MSVC STL versions.
target_compile_definitions(xt_bundle_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
target_compile_definitions(xt_bundle_tests PRIVATE XT_TEST_BUNDLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
# test_pack.cpp renders a thumbnail and reads it back (xt_sw), so it links the software backend.
target_link_libraries(xt_bundle_tests PRIVATE xt_core xt_bundle xt_sw)
if(TARGET xt_text)
    # T-273b: test_pack.cpp shapes a packed bundle's text with its own font (FtTextShaper).
    target_link_libraries(xt_bundle_tests PRIVATE xt_text)
    target_compile_definitions(xt_bundle_tests PRIVATE XT_TEST_HAS_TEXT)
endif()
# T-199: test_asset_pipeline.cpp exercises xt_bundle's internal atlas / DDS / font-subset modules.
target_include_directories(xt_bundle_tests PRIVATE ${PROJECT_SOURCE_DIR}/Code/Runtime/bundle/src)
# T-257: test_parser_limits.cpp exercises the internal YAML-lite reader, whose header includes json.hpp.
target_link_libraries(xt_bundle_tests PRIVATE xt_nlohmann)
xt_enable_exceptions(xt_bundle_tests)
xt_no_rtti(xt_bundle_tests)
xt_set_warnings(xt_bundle_tests)

add_test(NAME xt_bundle_tests COMMAND xt_bundle_tests)

# T-807: test_xtb_cache.cpp opens each `.xtb` in this helper's own process (its temp directory
# pointed at the test's), so one process's copies never shield the others from eviction.
add_executable(xt_xtb_cache_probe xtb_cache_probe.cpp)
target_link_libraries(xt_xtb_cache_probe PRIVATE xt_core xt_bundle)
xt_enable_exceptions(xt_xtb_cache_probe)
xt_no_rtti(xt_xtb_cache_probe)
xt_set_warnings(xt_xtb_cache_probe)
add_dependencies(xt_bundle_tests xt_xtb_cache_probe)
target_compile_definitions(xt_bundle_tests PRIVATE XT_TEST_XTB_PROBE="$<TARGET_FILE:xt_xtb_cache_probe>")

# T-623 (Docs/design/22-bundle.md §22.16): without --lang, xtframe-view starts a project with strings
# tables in its spec.defaultLanguage (a packed project, a lone .xt inside it) and says so; --lang
# still overrides it; a project without strings tables prints no language line.
if(TARGET xtframe-view)
    include("${PROJECT_SOURCE_DIR}/Tests/cmake/expect_exit.cmake")
    set(_t623 "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/default_language")
    xt_add_expect_exit_test(xtframe-view_t623_default CODE 0 OUTPUT "text=Nihao.*lang zh-TW [(]default[)]"
        COMMAND $<TARGET_FILE:xtframe-view> "${_t623}" --headless --frames 1 --print-text label)
    xt_add_expect_exit_test(xtframe-view_t623_override CODE 0 OUTPUT "text=Hello" REJECT "[(]default[)]"
        COMMAND $<TARGET_FILE:xtframe-view> "${_t623}" --headless --frames 1 --print-text label --lang en)
    xt_add_expect_exit_test(xtframe-view_t623_lone_xt CODE 0 OUTPUT "text=Nihao.*lang zh-TW [(]default[)]"
        COMMAND $<TARGET_FILE:xtframe-view> "${_t623}/greet/greet.xt" --headless --frames 1 --print-text label)
    xt_add_expect_exit_test(xtframe-view_t623_no_strings CODE 0 REJECT "[(]default[)]"
        COMMAND $<TARGET_FILE:xtframe-view> "${PROJECT_SOURCE_DIR}/Tests/capi/fixtures/flip" --ui flip
                --headless --frames 1)

    # T-627 (22 §22.14): --event clicks with a mouse button (`#B.Click button=right`, the raw
    # `pointer down|up <x> <y> button=middle`), the default stays left; --port x='' and
    # --port x '' (quotes passed through, as cmd.exe does; CMake cannot pass a real empty
    # argument) set the empty string, which differs from leaving the port out.
    set(_t627 "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/view_input/view_input.xt")
    xt_add_expect_exit_test(xtframe-view_t627_click_right CODE 0
        OUTPUT "port emit pressRight.*port emit rightClick" REJECT "leftClick"
        COMMAND $<TARGET_FILE:xtframe-view> "${_t627}" --headless --frames 2 --log-ports
                --event "1 #B.Click button=right")
    xt_add_expect_exit_test(xtframe-view_t627_click_default CODE 0 OUTPUT "port emit leftClick"
        REJECT "rightClick|pressRight"
        COMMAND $<TARGET_FILE:xtframe-view> "${_t627}" --headless --frames 2 --log-ports --event "1 #B.Click")
    xt_add_expect_exit_test(xtframe-view_t627_pointer_middle CODE 0 OUTPUT "port emit middleDown"
        REJECT "leftClick|rightClick"
        COMMAND $<TARGET_FILE:xtframe-view> "${_t627}" --headless --frames 2 --log-ports
                --event "1 pointer down 50 20 button=middle" --event "1 pointer up 50 20 button=middle")
    xt_add_expect_exit_test(xtframe-view_t627_pointer_right CODE 0 OUTPUT "port emit rightClick"
        REJECT "leftClick"
        COMMAND $<TARGET_FILE:xtframe-view> "${_t627}" --headless --frames 2 --log-ports
                --pointer "1 50 20 down button=right" --pointer "1 50 20 up button=right")
    xt_add_expect_exit_test(xtframe-view_t627_bad_button CODE 2 OUTPUT "is not button=left[|]right[|]middle"
        COMMAND $<TARGET_FILE:xtframe-view> "${_t627}" --headless --frames 1 --event "1 #B.Click button=side")
    xt_add_expect_exit_test(xtframe-view_t627_port_empty_eq CODE 0 OUTPUT "text=\n" REJECT "text=abc"
        COMMAND $<TARGET_FILE:xtframe-view> "${_t627}" --headless --frames 1 --print-text L --port "title=''")
    xt_add_expect_exit_test(xtframe-view_t627_port_empty_arg CODE 0 OUTPUT "text=\n" REJECT "text=abc"
        COMMAND $<TARGET_FILE:xtframe-view> "${_t627}" --headless --frames 1 --print-text L --port title "\"\"")
    xt_add_expect_exit_test(xtframe-view_t627_port_omitted CODE 0 OUTPUT "text=abc"
        COMMAND $<TARGET_FILE:xtframe-view> "${_t627}" --headless --frames 1 --print-text L)

    # T-871 (60 §60.7): --event "[<frame>] wheel <dx> <dy> [<x> <y>]" drives on:wheel (add() with a
    # range) and a scroll container (on:scroll's event.value); the same script logs the same port
    # values on every run (the two entries replay it and expect the exact values).
    set(_t871 "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/view_input/view_wheel.xt")
    set(_t871_out "port changed freq = 100[.]3[0-9]* [(]frame 1[)].*port changed freq = 100[.]1[0-9]* [(]frame 2[)].*port changed sv = 0[.]1333[0-9]* [(]frame 3[)]")
    foreach(_run 1 2)
        xt_add_expect_exit_test(xtframe-view_t871_wheel_${_run} CODE 0 OUTPUT "${_t871_out}"
            COMMAND $<TARGET_FILE:xtframe-view> "${_t871}" --headless --frames 4 --log-ports
                    --event "1 wheel 0 -3 50 20" --event "2 wheel 0 2 50 20" --event "3 wheel 0 1 50 100")
    endforeach()
    xt_add_expect_exit_test(xtframe-view_t871_wheel_bad CODE 2 OUTPUT "a wheel event is"
        COMMAND $<TARGET_FILE:xtframe-view> "${_t871}" --headless --frames 1 --event "1 wheel 0")

    # T-873 (60 §60.7): --event "channel ...", "axis ..." and "key down|up ..." drive on:input(...)
    # triggers (a relative host channel summed per frame, a player-prefixed button edge, the right
    # stick through the existing axis event, key.Shift from a sided key); replayed twice, same log.
    set(_t873 "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/view_input/view_channel.xt")
    set(_t873_out "port changed knob = 5 [(]frame 1[)].*port changed lever = 0[.]5 [(]frame 2[)].*port changed hits = 1 [(]frame 3[)].*port changed shift = 1 [(]frame 4[)].*port changed shift = 0 [(]frame 5[)]")
    foreach(_run 1 2)
        xt_add_expect_exit_test(xtframe-view_t873_channel_${_run} CODE 0 OUTPUT "${_t873_out}"
            COMMAND $<TARGET_FILE:xtframe-view> "${_t873}" --headless --frames 6 --log-ports
                    --event "1 channel host.knob 2 1 relative" --event "1 channel host.knob 3 1 relative"
                    --event "2 axis pad.rx 0.5" --event "3 channel player(1).host.trig 1 0.8 button"
                    --event "4 key down ShiftLeft" --event "5 key up ShiftLeft")
    endforeach()
    xt_add_expect_exit_test(xtframe-view_t873_channel_bad CODE 2 OUTPUT "a channel value is"
        COMMAND $<TARGET_FILE:xtframe-view> "${_t873}" --headless --frames 1 --event "1 channel pad.zz 1")

    # T-880 (60 §60.6): the acceptance fixture for a real controller shows the six axes; --gamepad
    # (now the default, no effect) and --no-gamepad are both accepted, the --event path unchanged.
    set(_t880 "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/view_input/view_gamepad.xt")
    xt_add_expect_exit_test(xtframe-view_t880_no_gamepad CODE 0 OUTPUT "text=0[.]5\n.*text=-0[.]25\n.*text=1\n"
        COMMAND $<TARGET_FILE:xtframe-view> "${_t880}" --headless --no-gamepad --frames 3
                --event "1 axis pad.rx 0.5" --event "1 axis pad.ry -0.25" --event "1 axis pad.rt 1"
                --print-text RxValue --print-text RyValue --print-text RtValue)
    xt_add_expect_exit_test(xtframe-view_t880_gamepad_flag CODE 0 OUTPUT "text=0[.]75\n"
        COMMAND $<TARGET_FILE:xtframe-view> "${_t880}" --headless --gamepad --frames 3
                --event "1 axis pad.lt 0.75" --print-text LtValue)
    # T-884 (60 §60.6): --gamepad-deadzone takes three numbers 0..0.9; --event axis values bypass it.
    xt_add_expect_exit_test(xtframe-view_t884_deadzone_range CODE 2
        OUTPUT "--gamepad-deadzone: expected three numbers 0-0[.]9 .*got '1'"
        COMMAND $<TARGET_FILE:xtframe-view> "${_t880}" --headless --gamepad-deadzone 0.24 1 0.12)
    xt_add_expect_exit_test(xtframe-view_t884_deadzone_missing CODE 2 OUTPUT "--gamepad-deadzone"
        COMMAND $<TARGET_FILE:xtframe-view> "${_t880}" --headless --gamepad-deadzone 0.24 0.2)
    xt_add_expect_exit_test(xtframe-view_t884_deadzone_event_bypass CODE 0 OUTPUT "text=0[.]25\n"
        COMMAND $<TARGET_FILE:xtframe-view> "${_t880}" --headless --frames 3 --gamepad-deadzone 0.5 0.5 0.5
                --event "1 axis pad.rx 0.25" --print-text RxValue)
endif()

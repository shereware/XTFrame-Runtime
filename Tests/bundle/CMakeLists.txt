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
    test_symbol_json.cpp
    test_zip_archive.cpp
    test_bundle_fonts.cpp
    test_default_language.cpp
    test_pack_stale.cpp
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
endif()

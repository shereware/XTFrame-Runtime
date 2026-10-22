add_executable(xt_text_tests
    # T-267d: the shared doctest main (watchdog, temp root clean-up, Debug-CRT dialogs to stderr).
    ${PROJECT_SOURCE_DIR}/Tests/unit/test_main.cpp
    test_ft_shaper.cpp
    test_glyph_atlas.cpp
    test_sw_glyphs.cpp
    test_text_bitmap_path.cpp
    test_i18n_regression.cpp
    test_system_fonts.cpp
)
target_include_directories(xt_text_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/Code/SDKs/doctest)
# test_ft_shaper.cpp exercises Code/Runtime/text/src/vo_table.h directly (Docs/tasks/T-29.md test list).
target_include_directories(xt_text_tests PRIVATE ${PROJECT_SOURCE_DIR}/Code/Runtime/text/src)
target_compile_definitions(xt_text_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
target_compile_definitions(xt_text_tests PRIVATE XT_TEST_FONTS_DIR="${PROJECT_SOURCE_DIR}/Tests/fonts")
# test_sw_glyphs.cpp renders DrawGlyphs through the SW backend (Docs/tasks/T-30.md).
target_link_libraries(xt_text_tests PRIVATE xt_text xt_core xt_sw)
xt_enable_exceptions(xt_text_tests)
xt_no_rtti(xt_text_tests)
xt_set_warnings(xt_text_tests)

add_test(NAME xt_text_tests COMMAND xt_text_tests)

# T-198: the standard symbol library (T-498: the preset group Assets/Symbols/groups/broadsheet-2026,
# Docs/design/30-ui-project.md 30.4, 49 §49.2) and the
# starter UI Project (Examples/ui-project-starter). The state goldens reuse xt_golden's `.xt`
# fixture runner (renderUiFixture / the `.events` grammar) from xt_golden_fixtures.
add_executable(xt_symbols_tests
    ${PROJECT_SOURCE_DIR}/Tests/unit/test_main.cpp
    test_symbols.cpp
    test_symbols_cli.cpp
    ${PROJECT_SOURCE_DIR}/Code/Tools/symbols/preview_render.cpp
)
# T-499: the headless render of `xtframe-symbols thumbnails` (Code/Tools/symbols/preview_render.cpp).
target_include_directories(xt_symbols_tests PRIVATE ${PROJECT_SOURCE_DIR}/Code/Tools/symbols)
if(TARGET xt_text)
    target_compile_definitions(xt_symbols_tests PRIVATE XT_SYMBOLS_HAS_TEXT)
    target_link_libraries(xt_symbols_tests PRIVATE xt_text)
endif()
target_include_directories(xt_symbols_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/Code/SDKs/doctest)
target_link_libraries(xt_symbols_tests PRIVATE xt_nlohmann)
# doctest needs the real standard headers, otherwise it conflicts with newer MSVC STL versions.
target_compile_definitions(xt_symbols_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
target_compile_definitions(xt_symbols_tests PRIVATE
    XT_SYMBOLS_DIR="${PROJECT_SOURCE_DIR}/Assets/Symbols/groups/broadsheet-2026"
    XT_SYMBOLS_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    XT_SYMBOLS_OUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/out"
    XT_SYMBOLS_LIBRARY_DIR="${PROJECT_SOURCE_DIR}/Assets/Symbols"
    XT_SKILL_TOKENS_JSON="${PROJECT_SOURCE_DIR}/Skills/skills/xtui/references/tokens.json"
    XT_STARTER_DIR="${PROJECT_SOURCE_DIR}/Examples/ui-project-starter"
    XT_MOBILE_HUD_DIR="${PROJECT_SOURCE_DIR}/Examples/demos/mobile-hud")
target_link_libraries(xt_symbols_tests PRIVATE xt_golden_fixtures xt_core xt_compile xt_sw xt_bundle)
xt_enable_exceptions(xt_symbols_tests)
xt_no_rtti(xt_symbols_tests)
xt_set_warnings(xt_symbols_tests)

add_test(NAME xt_symbols_tests COMMAND xt_symbols_tests)

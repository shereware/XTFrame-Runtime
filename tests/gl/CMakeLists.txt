add_executable(xt_gl_tests
    test_gl_main.cpp
    test_gl_renderer.cpp
    test_gl_text.cpp
    test_gl_3d.cpp
    test_gl_effects.cpp
    test_gl_particles.cpp
    test_gl_direct.cpp
)
target_include_directories(xt_gl_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/doctest)
target_compile_definitions(xt_gl_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
target_compile_definitions(xt_gl_tests PRIVATE
    XT_GOLDEN_EXPECTED_DIR="${PROJECT_SOURCE_DIR}/tests/golden/expected"
    XT_GOLDEN_UI_DIR="${PROJECT_SOURCE_DIR}/tests/golden/ui")
# test_gl_renderer.cpp uses xt_gl's private headers (gl_loader.h) directly to build its own
# offscreen render target with the same GL function pointers GlRenderer uses.
target_include_directories(xt_gl_tests PRIVATE ${PROJECT_SOURCE_DIR}/gl/src)
target_include_directories(xt_gl_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/golden)
# test_gl_3d.cpp uses core/src's internal camera.h (cssPerspectiveViewProj), like
# tests/unit/test_sw_3d.cpp does via tests/CMakeLists.txt's own core/src include.
target_include_directories(xt_gl_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
target_link_libraries(xt_gl_tests PRIVATE xt_gl xt_core xt_compile xt_sw xt_golden_fixtures
                                          SDL2::SDL2-static)
# test_gl_text.cpp (docs/tasks/T-31.md) needs xt_text's FtTextShaper/GlyphAtlas; excluded via
# #ifndef XT_GL_NO_TEXT when XT_BUILD_TEXT is off, so only link/define these when it's on.
if(XT_BUILD_TEXT)
    target_link_libraries(xt_gl_tests PRIVATE xt_text)
    target_compile_definitions(xt_gl_tests PRIVATE XT_TEST_FONTS_DIR="${PROJECT_SOURCE_DIR}/tests/fonts")
endif()
if(WIN32)
    target_link_libraries(xt_gl_tests PRIVATE SDL2::SDL2main)
endif()
if(XT_MSVC_FRONTEND)
    target_link_options(xt_gl_tests PRIVATE /WHOLEARCHIVE:xt_golden_fixtures)
elseif(APPLE)
    target_link_options(xt_gl_tests PRIVATE -Wl,-force_load,$<TARGET_FILE:xt_golden_fixtures>)
else()
    target_link_options(xt_gl_tests PRIVATE -Wl,--whole-archive $<TARGET_FILE:xt_golden_fixtures>
                                            -Wl,--no-whole-archive)
endif()
xt_enable_exceptions(xt_gl_tests)
xt_no_rtti(xt_gl_tests)
xt_set_warnings(xt_gl_tests)

# See docs/tasks/T-19.md "測試": no GL context available (e.g. a GPU-less CI runner) -> the test
# binary itself prints a skip message and exits 0, so add_test always reports this as a pass/skip,
# never a failure, without any special CTest properties. T-267a: unless XT_REQUIRE_GL=1 is in the
# environment (tools/dev_check.sh exports it on the gate machines): then "no context" exits 1.
add_test(NAME xt_gl_tests COMMAND xt_gl_tests)

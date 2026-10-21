# xt_gpu_tests (docs/tasks/T-146.md "測試"): builds GpuFrames against a recording FakeDevice from
# the golden scene fixtures, the UI `.xt` fixtures and hand-built CommandLists, and compares each
# frame's structural dump with tests/gpu/golden/*.txt (a missing one fails;
# XT_GPU_GOLDEN_UPDATE=1 writes them).
add_executable(xt_gpu_tests
    test_gpu_main.cpp
    test_gpu_frame.cpp
)
target_include_directories(xt_gpu_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/doctest)
target_compile_definitions(xt_gpu_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
target_compile_definitions(xt_gpu_tests PRIVATE
    XT_GPU_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
    # T-267a: a mismatching snapshot's <name>.actual.txt is written here, never into the sources.
    XT_GPU_ACTUAL_DIR="${CMAKE_CURRENT_BINARY_DIR}/actual"
    XT_GPU_UI_DIR="${PROJECT_SOURCE_DIR}/tests/golden/ui")
target_include_directories(xt_gpu_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/golden)
# The 3D case uses core/src's internal camera.h (cssPerspectiveViewProj), like tests/gl/test_gl_3d.cpp.
target_include_directories(xt_gpu_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
target_link_libraries(xt_gpu_tests PRIVATE xt_gpu xt_core xt_compile xt_sw xt_golden_fixtures)
if(XT_BUILD_TEXT)
    target_link_libraries(xt_gpu_tests PRIVATE xt_text)
    target_compile_definitions(xt_gpu_tests PRIVATE XT_TEST_FONTS_DIR="${PROJECT_SOURCE_DIR}/tests/fonts")
endif()
if(XT_MSVC_FRONTEND)
    target_link_options(xt_gpu_tests PRIVATE /WHOLEARCHIVE:xt_golden_fixtures)
elseif(APPLE)
    target_link_options(xt_gpu_tests PRIVATE -Wl,-force_load,$<TARGET_FILE:xt_golden_fixtures>)
else()
    target_link_options(xt_gpu_tests PRIVATE -Wl,--whole-archive $<TARGET_FILE:xt_golden_fixtures>
                                             -Wl,--no-whole-archive)
endif()
xt_enable_exceptions(xt_gpu_tests)
xt_no_rtti(xt_gpu_tests)
xt_set_warnings(xt_gpu_tests)

add_test(NAME xt_gpu_tests COMMAND xt_gpu_tests)

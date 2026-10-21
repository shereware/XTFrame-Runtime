# xt_golden_fixtures: the fixture-building code shared between xt_golden (this directory) and
# xt_gl_tests' SW-vs-GL comparison (tests/gl), extracted per docs/tasks/T-19.md "extract
# tests/golden/fixtures into a static library".
add_library(xt_golden_fixtures STATIC
    golden_common.cpp
    golden_lottie.cpp
    golden_partial.cpp
    golden_ui.cpp
    fixtures/scene_rect_solid.cpp
    fixtures/scene_evenodd_rings.cpp
    fixtures/scene_transform_chain.cpp
    fixtures/scene_opacity_group.cpp
    fixtures/scene_stroke_caps.cpp
    fixtures/scene_dash.cpp
    fixtures/scene_gradient_linear.cpp
    fixtures/scene_gradient_radial.cpp
    fixtures/scene_mask_alpha.cpp
    fixtures/scene_trim.cpp
    fixtures/scene_modifiers.cpp
    fixtures/scene_taper.cpp
)
target_include_directories(xt_golden_fixtures PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(xt_golden_fixtures PUBLIC
    XT_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/expected"
    XT_GOLDEN_LOTTIE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/lottie"
    XT_GOLDEN_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ui"
)
target_link_libraries(xt_golden_fixtures PUBLIC xt_core xt_compile xt_sw)
# T-30: the ui_text_* fixtures need a real TextShaper; without xt_text they are skipped entirely
# (see golden_ui.cpp).
if(XT_BUILD_TEXT)
    target_link_libraries(xt_golden_fixtures PUBLIC xt_text)
    target_compile_definitions(xt_golden_fixtures PUBLIC XT_GOLDEN_TEXT=1)
endif()
xt_enable_exceptions(xt_golden_fixtures)
xt_no_rtti(xt_golden_fixtures)
xt_set_warnings(xt_golden_fixtures)

add_executable(xt_golden
    golden_main.cpp
    # T-245: needs an AssetResolver (embedded image), which only xt_golden's renderFixture passes;
    # kept out of xt_golden_fixtures so the GL/GPU fixture loops never see it.
    fixtures/scene_image_mesh.cpp
)
target_link_libraries(xt_golden PRIVATE xt_golden_fixtures xt_core xt_compile xt_sw)
# xt_golden_fixtures' fixture .cpp files register themselves via static-init side effects only
# (XT_GOLDEN_FIXTURE); nothing else references their symbols, so a plain static-library link would
# let the linker drop those object files entirely. Force every object file from the archive in.
if(XT_MSVC_FRONTEND)
    target_link_options(xt_golden PRIVATE /WHOLEARCHIVE:xt_golden_fixtures)
elseif(APPLE)
    target_link_options(xt_golden PRIVATE -Wl,-force_load,$<TARGET_FILE:xt_golden_fixtures>)
else()
    target_link_options(xt_golden PRIVATE -Wl,--whole-archive $<TARGET_FILE:xt_golden_fixtures> -Wl,--no-whole-archive)
endif()
target_compile_definitions(xt_golden PRIVATE
    XT_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/expected"
    XT_GOLDEN_LOTTIE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/lottie"
    XT_GOLDEN_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ui"
)
xt_enable_exceptions(xt_golden)
xt_no_rtti(xt_golden)
xt_set_warnings(xt_golden)

add_test(NAME xt_golden COMMAND xt_golden)

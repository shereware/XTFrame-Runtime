# T-504 (Docs/design/50-c-abi.md §50.4): the C ABI shared library xtframe (xt_capi), tested through
# its C header only. The fixed bundles are packed at build time with xtframe-pack: Examples/demos/
# game-ui (cases 1-5, 7-9) and Examples/demos/hud-overlay (case 6: game-ui declares no `port:`).
if(NOT TARGET xtframe-pack)
    message(STATUS "xt_capi_tests: no xtframe-pack target (XT_BUILD_TOOLS=OFF), the C ABI tests are not built")
    return()
endif()

# xt_capi_pack_bundle(<out var> <name> <source dir>): packs <source dir> into
# ${CMAKE_CURRENT_BINARY_DIR}/<name>-bundle/ (its manifest.json is the custom command's output).
function(xt_capi_pack_bundle out name source)
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/${name}-bundle")
    file(GLOB_RECURSE inputs CONFIGURE_DEPENDS "${source}/*")
    add_custom_command(
        OUTPUT "${dir}/manifest.json"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${dir}"
        COMMAND $<TARGET_FILE:xtframe-pack> "${source}" -o "${dir}" --no-thumbs
        DEPENDS xtframe-pack ${inputs}
        COMMENT "Packing ${name} for xt_capi_tests"
        VERBATIM)
    set(${out} "${dir}" PARENT_SCOPE)
endfunction()
xt_capi_pack_bundle(XT_CAPI_GAME_UI game-ui "${PROJECT_SOURCE_DIR}/Examples/demos/game-ui")
xt_capi_pack_bundle(XT_CAPI_HUD_OVERLAY hud-overlay "${PROJECT_SOURCE_DIR}/Examples/demos/hud-overlay")
# T-552 (capi review K1 / K4): a one-box UI whose color follows `@media (prefers-reduced-motion)`.
xt_capi_pack_bundle(XT_CAPI_DEVICE device "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/device")
# T-561: a one-card UI whose flow toggles `flipped` on #card.Click, with an opacity transition; T-603:
# a second UI, `intro`, whose flow plays its intro from lifecycle.shown; T-651: a third, `lit`, whose
# click toggles `lit` on #card, starting three CSS animations (state snapshot version 3); T-658: a
# fourth, `ghost`, whose only card is hidden until its `show` port is true (an empty GPU surface).
xt_capi_pack_bundle(XT_CAPI_FLIP flip "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/flip")
# T-617: a one-box UI whose color follows `localize` bindings of `@greeting` (strings en / zh-TW) and
# whose flow writes system.languageChanged into the `heard` port.
xt_capi_pack_bundle(XT_CAPI_L10N l10n "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/l10n")
# T-635: four rows (a port-bound bar, custom states, a timeline, a scrolled box) for save / load state.
xt_capi_pack_bundle(XT_CAPI_STATE state "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/state")
# T-647: text at 12 / 24 / 48 px (light on dark, dark on transparent), a non-convex star and a 120 px
# word, with bundled Noto Sans (publish.fonts), for the GPU-target text tolerance cases; T-653: a second
# UI, `skew`, with text and thin borders under skewX / rotate.
xt_capi_pack_bundle(XT_CAPI_TEXT text "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/text")
# T-642: the npdo demo (hud / settings) for the partial-redraw equivalence cases (test_capi_partial.cpp).
xt_capi_pack_bundle(XT_CAPI_NPDO npdo "${PROJECT_SOURCE_DIR}/Examples/demos/npdo/ui-project")
# T-656: three 1024 x 1024 images on a 256 x 256 UI; test_capi_gpu.cpp copies the bundle and rewrites
# its images with new pixels every round (the GPU target's image idle budget).
xt_capi_pack_bundle(XT_CAPI_IMAGES images "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/images")
# T-678: six large overlapping panels with shadows, glows and shine, whose frame borrows far more than
# 16 MiB of transient targets (the transient pool budget case of test_capi_gpu.cpp).
xt_capi_pack_bundle(XT_CAPI_EFFECTS effects "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/effects")
# T-683: a card with a one-term and an outlined card with a three-term analytic drop shadow, and a
# non-convex star (a coverage-atlas fill at one sample): the prewarm and coverage-release cases.
xt_capi_pack_bundle(XT_CAPI_BOXSHADOW boxshadow "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/boxshadow")
add_custom_target(xt_capi_test_bundles
    DEPENDS "${XT_CAPI_GAME_UI}/manifest.json" "${XT_CAPI_HUD_OVERLAY}/manifest.json"
            "${XT_CAPI_DEVICE}/manifest.json" "${XT_CAPI_FLIP}/manifest.json"
            "${XT_CAPI_L10N}/manifest.json" "${XT_CAPI_STATE}/manifest.json"
            "${XT_CAPI_NPDO}/manifest.json" "${XT_CAPI_TEXT}/manifest.json"
            "${XT_CAPI_IMAGES}/manifest.json" "${XT_CAPI_EFFECTS}/manifest.json"
            "${XT_CAPI_BOXSHADOW}/manifest.json")

add_executable(xt_capi_tests
    test_main.cpp
    test_capi.cpp
    test_capi_l10n.cpp
    test_capi_state.cpp
    test_capi_trim.cpp
    test_capi_partial.cpp
    test_capi_threads.cpp
)
# T-689 (45 §45.8): the GPU target cases are exported with XTFrame-Backends and read from
# XT_BACKENDS_SOURCE_DIR (the top-level CMakeLists.txt) in a build with xt_wgpu; without it
# test_capi_gpu_stub.cpp checks the "built without wgpu" answers. This directory is an include directory
# so that test_capi_gpu.cpp's `#include "../common/test_temp.h"` resolves from the XTFrame-Backends tree.
if(TARGET xt_wgpu)
    target_sources(xt_capi_tests PRIVATE
        "${XT_BACKENDS_SOURCE_DIR}/Tests/capi/test_capi_gpu.cpp"
        "${XT_BACKENDS_SOURCE_DIR}/Tests/capi/test_capi_gpu_text.cpp")
else()
    target_sources(xt_capi_tests PRIVATE test_capi_gpu_stub.cpp)
endif()
target_include_directories(xt_capi_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_dependencies(xt_capi_tests xt_capi_test_bundles)
target_include_directories(xt_capi_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/Code/SDKs/doctest)
# doctest needs the real standard headers, otherwise it conflicts with newer MSVC STL versions.
target_compile_definitions(xt_capi_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
target_compile_definitions(xt_capi_tests PRIVATE
    XT_CAPI_TEST_BUNDLE="${XT_CAPI_GAME_UI}"
    XT_CAPI_TEST_PORTS_BUNDLE="${XT_CAPI_HUD_OVERLAY}"
    XT_CAPI_TEST_DEVICE_BUNDLE="${XT_CAPI_DEVICE}"
    XT_CAPI_TEST_FLIP_BUNDLE="${XT_CAPI_FLIP}"
    XT_CAPI_TEST_L10N_BUNDLE="${XT_CAPI_L10N}"
    XT_CAPI_TEST_STATE_BUNDLE="${XT_CAPI_STATE}"
    XT_CAPI_TEST_NPDO_BUNDLE="${XT_CAPI_NPDO}"
    XT_CAPI_TEST_TEXT_BUNDLE="${XT_CAPI_TEXT}"
    XT_CAPI_TEST_IMAGES_BUNDLE="${XT_CAPI_IMAGES}"
    XT_CAPI_TEST_EFFECTS_BUNDLE="${XT_CAPI_EFFECTS}"
    XT_CAPI_TEST_BOXSHADOW_BUNDLE="${XT_CAPI_BOXSHADOW}")
# Only the C header (and xt/version.h's macros, for case 1) -- the runtime is reached through the
# shared library alone. nlohmann reads the packed manifest for case 2.
target_include_directories(xt_capi_tests PRIVATE ${PROJECT_SOURCE_DIR}/Code/Runtime/core/include)
target_link_libraries(xt_capi_tests PRIVATE xt_capi xt_nlohmann)
# T-636: the GPU target cases need a library built with xt_wgpu (and wgpu-native next to the tests).
if(TARGET xt_wgpu)
    target_compile_definitions(xt_capi_tests PRIVATE XT_CAPI_TEST_HAS_WGPU)
    xt_copy_runtime_library(xt_capi_tests xt::wgpu_native)
endif()
xt_enable_exceptions(xt_capi_tests)
xt_no_rtti(xt_capi_tests)
xt_set_warnings(xt_capi_tests)
add_test(NAME xt_capi_tests COMMAND xt_capi_tests)

# Case 10: the header compiled as C99 by the C compiler, linked against the shared library and run.
add_executable(xt_capi_c_compile compile_as_c.c)
set_target_properties(xt_capi_c_compile PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(xt_capi_c_compile PRIVATE xt_capi)
if(XT_MSVC_FRONTEND)
    target_compile_options(xt_capi_c_compile PRIVATE /W4 /WX)
else()
    target_compile_options(xt_capi_c_compile PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME xt_capi_c_compile COMMAND xt_capi_c_compile)

# Windows looks for xtframe.dll next to the executable: copy it into the tests' directory.
if(WIN32)
    foreach(t xt_capi_tests xt_capi_c_compile)
        add_custom_command(TARGET ${t} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:xt_capi>" "$<TARGET_FILE_DIR:${t}>"
            VERBATIM)
    endforeach()
endif()

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
# T-561: a one-card UI whose flow toggles `flipped` on #card.Click, with an opacity transition.
xt_capi_pack_bundle(XT_CAPI_FLIP flip "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/flip")
add_custom_target(xt_capi_test_bundles
    DEPENDS "${XT_CAPI_GAME_UI}/manifest.json" "${XT_CAPI_HUD_OVERLAY}/manifest.json"
            "${XT_CAPI_DEVICE}/manifest.json" "${XT_CAPI_FLIP}/manifest.json")

add_executable(xt_capi_tests
    test_main.cpp
    test_capi.cpp
)
add_dependencies(xt_capi_tests xt_capi_test_bundles)
target_include_directories(xt_capi_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/Code/SDKs/doctest)
# doctest needs the real standard headers, otherwise it conflicts with newer MSVC STL versions.
target_compile_definitions(xt_capi_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
target_compile_definitions(xt_capi_tests PRIVATE
    XT_CAPI_TEST_BUNDLE="${XT_CAPI_GAME_UI}"
    XT_CAPI_TEST_PORTS_BUNDLE="${XT_CAPI_HUD_OVERLAY}"
    XT_CAPI_TEST_DEVICE_BUNDLE="${XT_CAPI_DEVICE}"
    XT_CAPI_TEST_FLIP_BUNDLE="${XT_CAPI_FLIP}")
# Only the C header (and xt/version.h's macros, for case 1) -- the runtime is reached through the
# shared library alone. nlohmann reads the packed manifest for case 2.
target_include_directories(xt_capi_tests PRIVATE ${PROJECT_SOURCE_DIR}/Code/Runtime/core/include)
target_link_libraries(xt_capi_tests PRIVATE xt_capi xt_nlohmann)
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

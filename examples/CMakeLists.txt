# Host-integration examples. See docs/tasks/T-40.md.

# T-396 (docs/design/23-host-integration.md §23.ae): the engine's own JSON code, built with its own
# nlohmann copy (no XTFrame ABI tag, JSON_NOEXCEPTION, no C++ exceptions) and nothing of XTFrame.
add_library(engine_integration_host_json STATIC engine_integration/host_json/host_json.cpp)
target_include_directories(engine_integration_host_json SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine_integration/host_json)
target_compile_definitions(engine_integration_host_json PRIVATE JSON_NOEXCEPTION)
xt_no_exceptions_rtti(engine_integration_host_json)

add_executable(engine_integration
    engine_integration/main.cpp
    engine_integration/json_coexist.cpp
)
target_include_directories(engine_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
# --bundle/--ui/--watch (docs/tasks/T-93.md): the published-bundle path plus its flow graph.
target_link_libraries(engine_integration PRIVATE xt_core xt_compile xt_sw xt_bundle xt_flow
    engine_integration_host_json)
if(XT_BUILD_TEXT)
    # Stand-in for a host font system: FtTextShaper over the system fonts (docs/tasks/T-97.md).
    target_link_libraries(engine_integration PRIVATE xt_text)
    target_compile_definitions(engine_integration PRIVATE XT_EXAMPLE_HAS_TEXT)
endif()
xt_enable_exceptions(engine_integration)
xt_set_warnings(engine_integration)

if(XT_BUILD_SHELL)
    add_executable(desktop_app
        desktop_app/main.cpp
    )
    target_include_directories(desktop_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
    target_link_libraries(desktop_app PRIVATE xt_core xt_compile xt_sw xt_shell)
    target_compile_definitions(desktop_app PRIVATE
        XT_DESKTOP_APP_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/desktop_app/ui")
    xt_enable_exceptions(desktop_app)
    xt_set_warnings(desktop_app)

    add_executable(lottie_player
        lottie_player/main.cpp
    )
    target_include_directories(lottie_player PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
    target_link_libraries(lottie_player PRIVATE xt_core xt_compile xt_sw xt_shell)
    xt_enable_exceptions(lottie_player)
    xt_set_warnings(lottie_player)
endif()

if(XT_BUILD_TESTS)
    # T-267a: exit code 0 and the "done" line (tests/cmake/expect_exit.cmake), not the line alone.
    include("${PROJECT_SOURCE_DIR}/tests/cmake/expect_exit.cmake")
    xt_add_expect_exit_test(example_engine_integration CODE 0 OUTPUT "done"
        COMMAND $<TARGET_FILE:engine_integration> --file "${PROJECT_SOURCE_DIR}/tests/xt/doc_basic.xt"
                --headless --frames 10 --simplify)
    # T-396: both nlohmann copies parse in one executable, each in its own namespace.
    xt_add_expect_exit_test(example_engine_integration_json_coexist CODE 0
        OUTPUT "xtframe json: parsed [(]xtframe_abi[)].*engine json:  parsed [(]untagged[)].*done"
        COMMAND $<TARGET_FILE:engine_integration> --json-coexist)

    if(XT_BUILD_SHELL)
        xt_add_expect_exit_test(example_desktop_app CODE 0 OUTPUT "done"
            COMMAND $<TARGET_FILE:desktop_app> --headless --frames 10)

        xt_add_expect_exit_test(example_lottie_player CODE 0 OUTPUT "done"
            COMMAND $<TARGET_FILE:lottie_player> "${PROJECT_SOURCE_DIR}/tests/lottie/min_solid_anim.json"
                    --headless --frames 10)
    endif()
endif()

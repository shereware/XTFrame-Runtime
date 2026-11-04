# T-596: the examples of Docs/intro/for-programmers.md, built and run so the intro cannot rot.
# Suite `intro` (ctest -R '^intro_'): the ten-minute path in order -- xtframe-compile, xtframe-view
# headless, the C++ host on the compiled .xtir, the in-process headless test, xtframe-pack (with
# --gen-ports) and the C ABI host on that bundle -- plus intro_snippets, which checks that every
# code block of the intro is still the code compiled here.
set(INTRO_HUD "${CMAKE_CURRENT_SOURCE_DIR}/hud/hud.xt")
set(INTRO_OUT "${CMAKE_CURRENT_BINARY_DIR}/out")
set(INTRO_DOC "${PROJECT_SOURCE_DIR}/Docs/intro/for-programmers.md")
include("${PROJECT_SOURCE_DIR}/Tests/cmake/expect_exit.cmake")

add_test(NAME intro_clean COMMAND ${CMAKE_COMMAND} -E rm -rf "${INTRO_OUT}")
add_test(NAME intro_mkdir COMMAND ${CMAKE_COMMAND} -E make_directory "${INTRO_OUT}")
set_tests_properties(intro_clean PROPERTIES FIXTURES_SETUP intro_clean)
set_tests_properties(intro_mkdir PROPERTIES FIXTURES_REQUIRED intro_clean FIXTURES_SETUP intro_out)

set(intro_marked "${CMAKE_CURRENT_SOURCE_DIR}/cpp_host.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/headless_test.cpp")

add_executable(intro_headless_test headless_test.cpp)
target_link_libraries(intro_headless_test PRIVATE xt_core xt_compile)
target_compile_definitions(intro_headless_test PRIVATE XT_INTRO_HUD="${INTRO_HUD}")
xt_enable_exceptions(intro_headless_test)
xt_set_warnings(intro_headless_test)
add_test(NAME intro_headless_test COMMAND intro_headless_test)

add_executable(intro_cpp_host cpp_host.cpp)
target_link_libraries(intro_cpp_host PRIVATE xt_core)
xt_enable_exceptions(intro_cpp_host)
xt_set_warnings(intro_cpp_host)

if(TARGET xtframe-compile)
    xt_add_expect_exit_test(intro_compile CODE 0
        COMMAND $<TARGET_FILE:xtframe-compile> "${INTRO_HUD}" -o "${INTRO_OUT}/hud.xtir")
    set_tests_properties(intro_compile PROPERTIES FIXTURES_REQUIRED intro_out FIXTURES_SETUP intro_xtir)
    xt_add_expect_exit_test(intro_cpp_host CODE 0 OUTPUT "HpText=42"
        COMMAND $<TARGET_FILE:intro_cpp_host> "${INTRO_OUT}/hud.xtir")
    set_tests_properties(intro_cpp_host PROPERTIES FIXTURES_REQUIRED intro_xtir)
endif()

if(TARGET xtframe-view)
    xt_add_expect_exit_test(intro_view CODE 0
        COMMAND $<TARGET_FILE:xtframe-view> "${INTRO_OUT}/hud.xtir" --headless --frames 2
                --screenshot "${INTRO_OUT}/hud.png")
    set_tests_properties(intro_view PROPERTIES FIXTURES_REQUIRED intro_xtir)
endif()

if(TARGET xtframe-pack)
    xt_add_expect_exit_test(intro_pack CODE 0 OUTPUT "wrote"
        COMMAND $<TARGET_FILE:xtframe-pack> "${INTRO_HUD}" -o "${INTRO_OUT}/hud-bundle" --no-thumbs
                --gen-ports cpp "${INTRO_OUT}/hud_ports.h")
    set_tests_properties(intro_pack PROPERTIES FIXTURES_REQUIRED intro_out FIXTURES_SETUP intro_bundle)
    add_test(NAME intro_gen_ports
             COMMAND ${CMAKE_COMMAND} -DFILE=${INTRO_OUT}/hud_ports.h
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/check_gen_ports.cmake")
    set_tests_properties(intro_gen_ports PROPERTIES FIXTURES_REQUIRED intro_bundle)

    if(TARGET xt_capi)
        add_executable(intro_capi_host capi_host.c)
        set_target_properties(intro_capi_host PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
        target_link_libraries(intro_capi_host PRIVATE xt_capi)
        if(WIN32)
            # Windows looks for xtframe.dll next to the executable.
            add_custom_command(TARGET intro_capi_host POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:xt_capi>"
                        "$<TARGET_FILE_DIR:intro_capi_host>"
                VERBATIM)
        endif()
        xt_add_expect_exit_test(intro_capi_host CODE 0 OUTPUT "hp=42"
            COMMAND $<TARGET_FILE:intro_capi_host> "${INTRO_OUT}/hud-bundle")
        set_tests_properties(intro_capi_host PROPERTIES FIXTURES_REQUIRED intro_bundle)
        list(APPEND intro_marked "${CMAKE_CURRENT_SOURCE_DIR}/capi_host.c")
    endif()
endif()

add_test(NAME intro_snippets
         COMMAND ${CMAKE_COMMAND} "-DDOC=${INTRO_DOC}"
                 "-DWHOLE=${CMAKE_CURRENT_SOURCE_DIR}/hud/hud.xt$<SEMICOLON>${CMAKE_CURRENT_SOURCE_DIR}/hud/hud.xts"
                 "-DMARKED=$<JOIN:${intro_marked},$<SEMICOLON>>"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/check_snippets.cmake")

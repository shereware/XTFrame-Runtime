add_executable(xt_flow_tests
    ${PROJECT_SOURCE_DIR}/Tests/unit/test_main.cpp
    test_flow_graph.cpp
    test_flow_nodes.cpp
    test_flow_runtime.cpp
    test_flow_rebind.cpp
    test_flow_ports.cpp
    test_host_ports.cpp
    test_flow_declared_ports.cpp
    test_flow_global.cpp
    test_flow_lifetime.cpp
    test_flow_coalesce.cpp
)
target_include_directories(xt_flow_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/Code/SDKs/doctest)
# doctest needs the real standard headers, otherwise it conflicts with newer MSVC STL versions.
target_compile_definitions(xt_flow_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
target_compile_definitions(xt_flow_tests PRIVATE XT_TEST_FLOW_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(xt_flow_tests PRIVATE
    XT_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/Examples/desktop_app/ui")
target_link_libraries(xt_flow_tests PRIVATE xt_core xt_compile xt_flow xt_bundle)
xt_enable_exceptions(xt_flow_tests)
xt_no_rtti(xt_flow_tests)
xt_set_warnings(xt_flow_tests)

add_test(NAME xt_flow_tests COMMAND xt_flow_tests)

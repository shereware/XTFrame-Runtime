add_library(xt_flow STATIC
    src/flow_graph.cpp
    src/flow_nodes.cpp
    src/flow_runtime.cpp
    src/flow_signals.cpp
    src/flow_bindings.cpp
    src/flow_ports.cpp
    src/host_ports.cpp
    src/bundle_context.cpp
)
target_link_libraries(xt_flow PUBLIC xt_core)
target_include_directories(xt_flow PUBLIC include)
target_link_libraries(xt_flow PRIVATE xt_nlohmann)
xt_enable_exceptions(xt_flow)
xt_set_warnings(xt_flow)

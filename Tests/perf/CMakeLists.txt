# xt_perf: allocation-counting and timing tests. See Docs/design/15-testing.md §15.4,
# Docs/design/01-architecture.md §1.7, Docs/tasks/T-38.md.
#
# A standalone executable, separate from xt_tests: alloc_counter.cpp replaces the process-wide
# global operator new/delete, which must not affect xt_tests' own doctest run.

option(XT_PERF_RELAXED "Triple xt_perf's timing thresholds (CI)" OFF)

add_executable(xt_perf
    # T-267d: the shared doctest main (watchdog, temp root clean-up, Debug-CRT dialogs to stderr).
    ${PROJECT_SOURCE_DIR}/Tests/unit/test_main.cpp
    alloc_counter.cpp
    test_zero_alloc.cpp
    test_perf_thresholds.cpp
    # T-267d: parseXt()'s sibling-count guard, by allocation count (moved from xt_tests).
    test_parse_cost.cpp
)
target_include_directories(xt_perf SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/Code/SDKs/doctest)
# doctest needs the real standard headers, otherwise it conflicts with newer MSVC STL versions.
target_compile_definitions(xt_perf PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
target_compile_definitions(xt_perf PRIVATE XT_PERF_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
if(XT_PERF_RELAXED)
    target_compile_definitions(xt_perf PRIVATE XT_PERF_RELAXED)
endif()
target_link_libraries(xt_perf PRIVATE xt_core xt_compile xt_sw)
xt_enable_exceptions(xt_perf)
xt_no_rtti(xt_perf)
xt_set_warnings(xt_perf)

add_test(NAME xt_perf COMMAND xt_perf)

# T-475 (Docs/design/21-perf-tracing.md §21.6, 27-interaction.md §27.4.2 item 6): the headless flow
# benchmark runner (Tests/perf/flow_big/README.md). Not a doctest executable and not a CTest entry:
# Tests/perf/flow_big/flow_bench.py --runner drives it. XT_PERF_ALLOC_COUNT makes
# flow_alloc_hook.cpp replace operator new, so FrameStats::flowAllocs is filled here and nowhere else.
add_executable(xt_perf_flow
    flow_perf_main.cpp
    flow_alloc_hook.cpp
)
target_compile_definitions(xt_perf_flow PRIVATE XT_PERF_ALLOC_COUNT)
target_link_libraries(xt_perf_flow PRIVATE xt_core xt_compile xt_flow xt_bundle)
xt_enable_exceptions(xt_perf_flow)
xt_no_rtti(xt_perf_flow)
xt_set_warnings(xt_perf_flow)

# T-403 (Docs/design/21-perf-tracing.md "flow budget"): the large / typical flow benchmark. Not a
# gate -- it only prints xtframe-view's --perf summary for CI to collect (no thresholds; it fails
# only when the fixture no longer loads). Tests/perf/flow_big/flow_bench.py computes the last-480-
# frame averages and p95 the budget is stated in.
if(TARGET xtframe-view)
    foreach(xt_flow_big large typical)
        add_test(NAME xt_perf_flow_big_${xt_flow_big}
                 COMMAND xtframe-view --project "${CMAKE_CURRENT_SOURCE_DIR}/flow_big/${xt_flow_big}"
                         --headless --perf --frames 600 --synth-pointer 16 --synth-ports 20)
        set_tests_properties(xt_perf_flow_big_${xt_flow_big} PROPERTIES LABELS "perf-report")
    endforeach()
endif()

# T-671 (Docs/design/50-c-abi.md §50.ab addendum 2, item 3): the GPU memory probe that
# Tools/Scripts/gpu_mem_matrix.py runs once per combination. C ABI only; built with the shared library
# and wgpu. The smoke test runs one round over the capi `flip` fixture bundle and checks the CSV.
if(TARGET xt_capi AND TARGET xt_wgpu)
    add_executable(xt_gpu_mem_probe gpu_mem_probe.cpp)
    target_link_libraries(xt_gpu_mem_probe PRIVATE xt_capi)
    if(WIN32)
        target_link_libraries(xt_gpu_mem_probe PRIVATE dxgi)
        add_custom_command(TARGET xt_gpu_mem_probe POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:xt_capi>" "$<TARGET_FILE_DIR:xt_gpu_mem_probe>"
            VERBATIM)
    endif()
    xt_copy_runtime_library(xt_gpu_mem_probe xt::wgpu_native)
    xt_enable_exceptions(xt_gpu_mem_probe)
    xt_no_rtti(xt_gpu_mem_probe)
    xt_set_warnings(xt_gpu_mem_probe)
    if(TARGET xtframe-pack)
        # Packed by Tests/capi/CMakeLists.txt (xt_capi_test_bundles, defined after this directory).
        add_dependencies(xt_gpu_mem_probe xt_capi_test_bundles)
        add_test(NAME xt_gpu_mem_probe_smoke
                 COMMAND "${CMAKE_COMMAND}" "-DPROBE=$<TARGET_FILE:xt_gpu_mem_probe>"
                         "-DBUNDLE=${PROJECT_BINARY_DIR}/Tests/capi/flip-bundle"
                         "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/gpu_mem_probe_smoke.csv"
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/gpu_mem_probe_smoke.cmake")
        set_tests_properties(xt_gpu_mem_probe_smoke PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP: no WebGPU adapter")
    endif()
endif()

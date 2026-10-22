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

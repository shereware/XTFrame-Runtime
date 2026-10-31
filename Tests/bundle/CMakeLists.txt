add_executable(xt_bundle_tests
    ${PROJECT_SOURCE_DIR}/Tests/unit/test_main.cpp
    test_project_file.cpp
    test_bundle.cpp
    test_pack.cpp
    test_bundle_load.cpp
    test_localization.cpp
    test_ui_project.cpp
    test_gen_ports.cpp
    test_asset_pipeline.cpp
    test_parser_limits.cpp
    test_file_safety.cpp
    test_design_system.cpp
    test_design_doc.cpp
    test_design_export.cpp
    test_asset_meta.cpp
    test_project_runtime.cpp
    test_project_edit.cpp
    test_source_versions.cpp
    test_roundtrip.cpp
    test_symbol_library.cpp
    test_symbol_json.cpp
    test_zip_archive.cpp
    test_bundle_fonts.cpp
)
target_include_directories(xt_bundle_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/Code/SDKs/doctest)
# doctest needs the real standard headers, otherwise it conflicts with newer MSVC STL versions.
target_compile_definitions(xt_bundle_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
target_compile_definitions(xt_bundle_tests PRIVATE XT_TEST_BUNDLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
# test_pack.cpp renders a thumbnail and reads it back (xt_sw), so it links the software backend.
target_link_libraries(xt_bundle_tests PRIVATE xt_core xt_bundle xt_sw)
if(TARGET xt_text)
    # T-273b: test_pack.cpp shapes a packed bundle's text with its own font (FtTextShaper).
    target_link_libraries(xt_bundle_tests PRIVATE xt_text)
    target_compile_definitions(xt_bundle_tests PRIVATE XT_TEST_HAS_TEXT)
endif()
# T-199: test_asset_pipeline.cpp exercises xt_bundle's internal atlas / DDS / font-subset modules.
target_include_directories(xt_bundle_tests PRIVATE ${PROJECT_SOURCE_DIR}/Code/Runtime/bundle/src)
# T-257: test_parser_limits.cpp exercises the internal YAML-lite reader, whose header includes json.hpp.
target_link_libraries(xt_bundle_tests PRIVATE xt_nlohmann)
xt_enable_exceptions(xt_bundle_tests)
xt_no_rtti(xt_bundle_tests)
xt_set_warnings(xt_bundle_tests)

add_test(NAME xt_bundle_tests COMMAND xt_bundle_tests)

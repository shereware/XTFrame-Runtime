# T-267a: Tests/common holds helpers every suite includes (test_temp.h: the per-process temp root).
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/common)

add_executable(xt_tests
    unit/test_main.cpp
    unit/test_version.cpp
    unit/test_charconv.cpp
    unit/test_locale_numbers.cpp
    unit/test_types.cpp
    unit/test_mat2x3.cpp
    unit/test_mat4.cpp
    unit/test_transform_compose.cpp
    unit/test_node_arena.cpp
    unit/test_ir_roundtrip.cpp
    unit/test_ir_malformed.cpp
    unit/test_ir_validation.cpp
    unit/test_geom_flatten.cpp
    unit/test_geom_stroke.cpp
    unit/test_geom_dash.cpp
    unit/test_path_trim.cpp
    unit/test_geom_bezier.cpp
    unit/test_value.cpp
    unit/test_property_access.cpp
    unit/test_easing.cpp
    unit/test_track_eval.cpp
    unit/test_scene_instance.cpp
    unit/test_animator.cpp
    unit/test_world_transform.cpp
    unit/test_layer_collect.cpp
    unit/test_camera.cpp
    unit/test_document_3d.cpp
    unit/test_painter.cpp
    unit/test_shape_modifiers.cpp
    unit/test_painter_3d.cpp
    unit/test_painter_element.cpp
    unit/test_lottie_parser.cpp
    unit/test_parse_svg.cpp
    unit/test_sw_raster.cpp
    unit/test_sw_renderer.cpp
    unit/test_context.cpp
    unit/test_damage.cpp
    unit/test_context_reload.cpp
    unit/test_tokens.cpp
    unit/test_selectors_ext.cpp
    unit/test_adaptive.cpp
    unit/test_responsive.cpp
    unit/test_app_region.cpp
    unit/test_localization.cpp
    unit/test_input_placeholder.cpp
    unit/test_sw_paint.cpp
    unit/test_sw_blend.cpp
    unit/test_sw_layer_mask.cpp
    unit/test_sw_stroke_image.cpp
    unit/test_sw_3d.cpp
    unit/test_sw_partial.cpp
    unit/test_sw_direct.cpp
    unit/test_lottie_shapes.cpp
    unit/test_base64.cpp
    unit/test_lottie_masks_assets.cpp
    unit/test_parse_xt.cpp
    unit/test_parse_xts.cpp
    unit/test_xts_values.cpp
    unit/test_xtfx.cpp
    unit/test_parser_depth.cpp
    unit/test_input_queue.cpp
    unit/test_compile_document.cpp
    unit/test_symbols.cpp
    unit/test_compile_bundle_hooks.cpp
    unit/test_compile_editor_only.cpp
    unit/test_file_asset_resolver.cpp
    unit/test_border_image.cpp
    unit/test_text_layout.cpp
    unit/test_selector_match.cpp
    unit/test_style_engine.cpp
    unit/test_layout_block.cpp
    unit/test_layout_absolute.cpp
    unit/test_layout_flex.cpp
    unit/test_layout_aspect.cpp
    unit/test_layout_overflow_order.cpp
    unit/test_layout_grid.cpp
    unit/test_hit_test.cpp
    unit/test_hit_test_3d.cpp
    unit/test_events.cpp
    unit/test_layout_text_node.cpp
    unit/test_rich_text.cpp
    unit/test_text_edit.cpp
    unit/test_composition.cpp
    unit/test_focus_keyboard.cpp
    unit/test_mini_css.cpp
    unit/test_css_parity.cpp
    unit/test_element_ref.cpp
    unit/test_query.cpp
    unit/test_dynamic_tree.cpp
    unit/test_style_animation_integration.cpp
    unit/test_top_level_elements.cpp
    unit/test_command_simplifier.cpp
    unit/test_command_list_validate.cpp
    unit/test_painter_effects.cpp
    unit/test_baked.cpp
    unit/test_event_integration.cpp
    unit/test_sw_effect_math.cpp
    unit/test_sw_effects.cpp
    unit/test_perf.cpp
    unit/test_public_headers.cpp
    unit/test_simd.cpp
    unit/test_field_fixes.cpp
    unit/test_timeline_group.cpp
    unit/test_external_media.cpp
    unit/test_timeline_clips.cpp
    unit/test_svg_style_rules.cpp
    unit/test_triggers.cpp
    unit/test_bindings.cpp
    unit/test_repeat.cpp
    unit/test_interaction.cpp
    unit/test_core_t200.cpp
    unit/test_symbol_gaps.cpp
    unit/test_particles.cpp
    unit/test_auto_orient.cpp
    unit/test_rig_model.cpp
    unit/test_rig_eval.cpp
    unit/test_reentrancy.cpp
    unit/test_reentrancy_removal.cpp
    unit/test_runtime_pools.cpp
    unit/test_num_guard.cpp
    unit/test_runtime_bounds.cpp
    unit/test_style_numbers.cpp
    unit/test_json_abi.cpp
    ir/ir_reserialize.cpp
    ir/test_ir_corpus.cpp
    ir/test_ir_fixtures.cpp
)
target_include_directories(xt_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/Code/SDKs/doctest)
# doctest needs the real standard headers, otherwise it conflicts with newer MSVC STL versions.
target_compile_definitions(xt_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
target_compile_definitions(xt_tests PRIVATE XT_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/Tests/lottie")
target_compile_definitions(xt_tests PRIVATE XT_TEST_XT_DIR="${PROJECT_SOURCE_DIR}/Tests/xt")
target_compile_definitions(xt_tests PRIVATE XT_TEST_LAYOUT_DIR="${PROJECT_SOURCE_DIR}/Tests/layout")
# test_compile_document.cpp's compileDocumentFromStrings test compares against the desktop_app
# example (Docs/tasks/T-59.md).
target_compile_definitions(xt_tests PRIVATE XT_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/Examples")
target_link_libraries(xt_tests PRIVATE xt_core xt_compile)
# T-396: test_json_abi.cpp checks the vendored nlohmann's ABI namespace.
target_link_libraries(xt_tests PRIVATE xt_nlohmann)
# test_sw_raster.cpp exercises the internal Rasterizer directly.
target_include_directories(xt_tests PRIVATE ${PROJECT_SOURCE_DIR}/Code/Runtime/sw/src)
# test_xts_values.cpp exercises xts_props.h's internal StyleProp/enum constants directly.
target_include_directories(xt_tests PRIVATE ${PROJECT_SOURCE_DIR}/Code/Runtime/compile/src/xts)
# test_selector_match.cpp/test_style_engine.cpp exercise core's internal selector_match.h directly.
target_include_directories(xt_tests PRIVATE ${PROJECT_SOURCE_DIR}/Code/Runtime/core/src)
# test_sw_effect_math.cpp times its Gaussian blur perf case with Tests/perf's perf_clock.h.
target_include_directories(xt_tests PRIVATE ${PROJECT_SOURCE_DIR}/Tests/perf)
target_link_libraries(xt_tests PRIVATE xt_core xt_compile xt_sw)
# T-240: SdlRwAssetResolver lives in xt_shell (SDL2).
if(XT_BUILD_SHELL)
    target_sources(xt_tests PRIVATE unit/test_sdl_rw_asset_resolver.cpp unit/test_hit_region.cpp
        unit/test_live_resize.cpp unit/test_multi_window.cpp)  # T-376, T-377, T-384b
    target_link_libraries(xt_tests PRIVATE xt_shell)
endif()
xt_enable_exceptions(xt_tests)
xt_no_rtti(xt_tests)
xt_set_warnings(xt_tests)
xt_precompile_like(xt_tests xt_core)  # T-361

add_test(NAME xt_tests COMMAND xt_tests)
# T-267d: a passing run removes its temp root, XT_TEST_KEEP_TEMP=1 keeps it (cmake/temp_root_check.cmake).
add_test(NAME xt_test_temp_cleanup
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:xt_tests>
                 "-DCASE=FileAssetResolver: single-root constructor keeps its behavior"
                 -DROOT=${CMAKE_CURRENT_BINARY_DIR}/temp_root_check
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/temp_root_check.cmake)

# T-309: `xtframe-design build / lint / audit` (Docs/design/35-design-system.md §35.7.3). build writes
# DESIGN.md, so it runs on a copy of the starter staged into the build tree; audit only reads and
# runs on Examples/demos/game-ui in place.
if(TARGET xtframe-design)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_exit.cmake")
    set(T309_DIR "${CMAKE_CURRENT_BINARY_DIR}/t309_design")
    add_test(NAME t309_design_clean COMMAND ${CMAKE_COMMAND} -E rm -rf "${T309_DIR}")
    set_tests_properties(t309_design_clean PROPERTIES FIXTURES_SETUP t309_clean)
    add_test(NAME t309_design_stage
             COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/Examples/ui-project-starter"
                     "${T309_DIR}/starter")
    set_tests_properties(t309_design_stage PROPERTIES FIXTURES_REQUIRED t309_clean FIXTURES_SETUP t309_staged)
    xt_add_expect_exit_test(xtframe-design_build CODE 0 OUTPUT "wrote .*DESIGN\\.md \\([0-9]+ lines"
        COMMAND $<TARGET_FILE:xtframe-design> build "${T309_DIR}/starter")
    set_tests_properties(xtframe-design_build PROPERTIES FIXTURES_REQUIRED t309_staged FIXTURES_SETUP t309_built)
    xt_add_expect_exit_test(xtframe-design_lint CODE 0 OUTPUT "lint: 0 error\\(s\\)" REJECT "design\\.docStale"
        COMMAND $<TARGET_FILE:xtframe-design> lint "${T309_DIR}/starter")
    set_tests_properties(xtframe-design_lint PROPERTIES FIXTURES_REQUIRED t309_built)
    xt_add_expect_exit_test(xtframe-design_audit CODE 0 OUTPUT "token coverage [0-9.]+ %"
        COMMAND $<TARGET_FILE:xtframe-design> audit "${PROJECT_SOURCE_DIR}/Examples/demos/game-ui" --scope uis)
endif()

# T-316: `xtframe-design export --claude-design` from a staged starter, then `import --claude-design`
# into a second staged copy (its variables and swatches exist already, so only text entries are added).
if(TARGET xtframe-design)
    set(T316_DIR "${CMAKE_CURRENT_BINARY_DIR}/t316_claude_design")
    add_test(NAME t316_claude_design_clean COMMAND ${CMAKE_COMMAND} -E rm -rf "${T316_DIR}")
    set_tests_properties(t316_claude_design_clean PROPERTIES FIXTURES_SETUP t316_clean)
    add_test(NAME t316_claude_design_stage
             COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/Examples/ui-project-starter"
                     "${T316_DIR}/starter")
    add_test(NAME t316_claude_design_stage_target
             COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/Examples/ui-project-starter"
                     "${T316_DIR}/target")
    set_tests_properties(t316_claude_design_stage t316_claude_design_stage_target
                         PROPERTIES FIXTURES_REQUIRED t316_clean FIXTURES_SETUP t316_staged)
    xt_add_expect_exit_test(xtframe-design_export_claude_design CODE 0
        OUTPUT "wrote Claude Design design system to .*\\(4 files, [0-9]+ README sections, 22 colour tokens, 2 themes, 20 swatches"
        COMMAND $<TARGET_FILE:xtframe-design> export "${T316_DIR}/starter" --claude-design "${T316_DIR}/cd")
    set_tests_properties(xtframe-design_export_claude_design
                         PROPERTIES FIXTURES_REQUIRED t316_staged FIXTURES_SETUP t316_exported)
    xt_add_expect_exit_test(xtframe-design_import_claude_design CODE 0
        OUTPUT "imported .*: [1-9][0-9]* added, 0 updated .*everything is a draft"
        COMMAND $<TARGET_FILE:xtframe-design> import "${T316_DIR}/target" --claude-design "${T316_DIR}/cd")
    set_tests_properties(xtframe-design_import_claude_design PROPERTIES FIXTURES_REQUIRED t316_exported)
endif()

add_subdirectory(golden)
add_subdirectory(perf)
add_subdirectory(flow)
add_subdirectory(bundle)
add_subdirectory(symbols)
# T-424: the template UI Projects (Assets/Templates/projects, editor layer; not in the runtime export).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/templates/CMakeLists.txt")
    add_subdirectory(templates)
endif()
# T-400: Tests/import (editor layer) is not part of the runtime export.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/import/CMakeLists.txt")
    add_subdirectory(import)
endif()

if(XT_BUILD_GPU)
    add_subdirectory(gpu)
endif()
if(XT_BUILD_GL)
    add_subdirectory(gl)
endif()
if(XT_BUILD_TEXT)
    add_subdirectory(text)
endif()
if(XT_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()
if(XT_BUILD_EDITOR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/editor/CMakeLists.txt")
    add_subdirectory(editor)
    add_subdirectory(editor_service)
    add_subdirectory(editor_qt)
    if(XT_EDITOR_HISTORY)
        add_subdirectory(history)
    endif()

    # Docs/tasks/T-66a.md: xtframe-editor's headless Demo mode, run against a copy of the desktop_app
    # example whose .xtflow is the timer fixture. The copy exists because FlowRuntime's graph has to
    # sit next to the document as "<name>.xtflow" (EditorSession::flowPath()), which would mean
    # overwriting the example's own graph. 120 frames at 1/60 s == 2 s of runtime time, so the
    # fixture's 0.17 s timer increments the counter 11 times.
    set(XT_DEMO_HEADLESS_DIR "${CMAKE_CURRENT_BINARY_DIR}/demo_headless")
    file(COPY "${PROJECT_SOURCE_DIR}/Examples/desktop_app/ui/app.xt"
              "${PROJECT_SOURCE_DIR}/Examples/desktop_app/ui/app.xts"
         DESTINATION "${XT_DEMO_HEADLESS_DIR}")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/editor_qt/fixtures/demo_timer.xtflow"
                   "${XT_DEMO_HEADLESS_DIR}/app.xtflow" COPYONLY)
    # T-267a: exit code 0 and the count line (Tests/cmake/expect_exit.cmake).
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_exit.cmake")
    xt_add_expect_exit_test(xtframe-editor_demo_headless CODE 0 OUTPUT "done count=1[0-2]"
        COMMAND $<TARGET_FILE:xtframe-editor> "${XT_DEMO_HEADLESS_DIR}/app.xt" --headless --frames 120 --demo)
    set_tests_properties(xtframe-editor_demo_headless PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # T-274 (Docs/RELEASE-MAP.md §6.1): `xtframe-editor --bake-vectors` (moved from xtframe-pack,
    # which is runtime layer). The two-step flow: bake a copy (sources untouched), then pack the copy
    # with --strict, which fails on any `vector.unbakedModifier` left. The in-place form is packed
    # the same way. The fixture is staged into the build tree first, so no run writes the sources.
    set(T274_DIR "${CMAKE_CURRENT_BINARY_DIR}/t274_bake")
    set(T274_FIXTURE "${CMAKE_CURRENT_SOURCE_DIR}/bundle/fixtures/unbaked_modifier")
    set(T274_EXPECT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_exit.cmake")
    add_test(NAME t274_editor_bake_clean COMMAND ${CMAKE_COMMAND} -E rm -rf "${T274_DIR}")
    set_tests_properties(t274_editor_bake_clean PROPERTIES FIXTURES_SETUP t274_clean)
    add_test(NAME t274_editor_bake_stage_src
             COMMAND ${CMAKE_COMMAND} -E copy_directory "${T274_FIXTURE}" "${T274_DIR}/src")
    add_test(NAME t274_editor_bake_stage_inplace
             COMMAND ${CMAKE_COMMAND} -E copy_directory "${T274_FIXTURE}" "${T274_DIR}/inplace")
    add_test(NAME t274_editor_bake_stage_root
             COMMAND ${CMAKE_COMMAND} -E copy_directory "${T274_FIXTURE}" "${T274_DIR}/root")
    set_tests_properties(t274_editor_bake_stage_src t274_editor_bake_stage_inplace t274_editor_bake_stage_root
                         PROPERTIES
                         FIXTURES_REQUIRED t274_clean FIXTURES_SETUP t274_staged)

    add_test(NAME t274_editor_bake_copy
             COMMAND ${CMAKE_COMMAND} -DEXPECT_CODE=0
                     "-DEXPECT_OUTPUT=xtframe-editor: baked 2 group\\(s\\), 3 shape\\(s\\), 0 frame\\(s\\)"
                     -P "${T274_EXPECT}" -- $<TARGET_FILE:xtframe-editor> --bake-vectors
                     "${T274_DIR}/src/main.xt" -o "${T274_DIR}/copy")
    set_tests_properties(t274_editor_bake_copy PROPERTIES
                         FIXTURES_REQUIRED t274_staged FIXTURES_SETUP t274_copy)
    add_test(NAME t274_editor_bake_source_untouched
             COMMAND ${CMAKE_COMMAND} -E compare_files "${T274_FIXTURE}/main.xt" "${T274_DIR}/src/main.xt")
    add_test(NAME t274_editor_bake_pack_copy
             COMMAND ${CMAKE_COMMAND} -DEXPECT_CODE=0 "-DEXPECT_OUTPUT=wrote 1 UI"
                     "-DREJECT_OUTPUT=unbakedModifier" -P "${T274_EXPECT}" -- $<TARGET_FILE:xtframe-pack>
                     "${T274_DIR}/copy/main.xt" -o "${T274_DIR}/bundle-copy" --no-thumbs --strict)
    set_tests_properties(t274_editor_bake_source_untouched t274_editor_bake_pack_copy PROPERTIES
                         FIXTURES_REQUIRED t274_copy)

    add_test(NAME t274_editor_bake_in_place
             COMMAND ${CMAKE_COMMAND} -DEXPECT_CODE=0
                     "-DEXPECT_OUTPUT=xtframe-editor: baked 2 group\\(s\\), 3 shape\\(s\\), 0 frame\\(s\\)"
                     -P "${T274_EXPECT}" -- $<TARGET_FILE:xtframe-editor> --bake-vectors
                     "${T274_DIR}/inplace/main.xt")
    set_tests_properties(t274_editor_bake_in_place PROPERTIES
                         FIXTURES_REQUIRED t274_staged FIXTURES_SETUP t274_inplace)
    add_test(NAME t274_editor_bake_pack_in_place
             COMMAND ${CMAKE_COMMAND} -DEXPECT_CODE=0 "-DEXPECT_OUTPUT=wrote 1 UI"
                     "-DREJECT_OUTPUT=unbakedModifier" -P "${T274_EXPECT}" -- $<TARGET_FILE:xtframe-pack>
                     "${T274_DIR}/inplace/main.xt" -o "${T274_DIR}/bundle-inplace" --no-thumbs --strict)
    set_tests_properties(t274_editor_bake_pack_in_place PROPERTIES FIXTURES_REQUIRED t274_inplace)

    # A `.xtui` input (the game-ui demo has no Offset / Merge: nothing to bake, still exit 0).
    add_test(NAME t274_editor_bake_project_copy
             COMMAND ${CMAKE_COMMAND} -DEXPECT_CODE=0 "-DEXPECT_OUTPUT=baked 0 group\\(s\\)"
                     -P "${T274_EXPECT}" -- $<TARGET_FILE:xtframe-editor> --bake-vectors
                     "${PROJECT_SOURCE_DIR}/Examples/demos/game-ui/XTFrame Game UI demo.xtui"
                     -o "${T274_DIR}/game-ui")
    set_tests_properties(t274_editor_bake_project_copy PROPERTIES FIXTURES_REQUIRED t274_clean)

    # Exit codes: 2 for argument errors (usage printed), 1 for an unreadable input.
    add_test(NAME t274_editor_bake_usage
             COMMAND ${CMAKE_COMMAND} -DEXPECT_CODE=2 "-DEXPECT_OUTPUT=usage: xtframe-editor --bake-vectors"
                     -P "${T274_EXPECT}" -- $<TARGET_FILE:xtframe-editor> --bake-vectors)
    add_test(NAME t274_editor_bake_unknown_option
             COMMAND ${CMAKE_COMMAND} -DEXPECT_CODE=2 "-DEXPECT_OUTPUT=unknown option '--bogus'"
                     -P "${T274_EXPECT}" -- $<TARGET_FILE:xtframe-editor> --bake-vectors
                     "${T274_FIXTURE}/main.xt" --bogus)
    add_test(NAME t274_editor_bake_out_is_root
             COMMAND ${CMAKE_COMMAND} -DEXPECT_CODE=2 "-DEXPECT_OUTPUT=-o is the project directory"
                     -P "${T274_EXPECT}" -- $<TARGET_FILE:xtframe-editor> --bake-vectors
                     "${T274_DIR}/root/main.xt" -o "${T274_DIR}/root")
    set_tests_properties(t274_editor_bake_out_is_root PROPERTIES FIXTURES_REQUIRED t274_staged)
    add_test(NAME t274_editor_bake_missing_input
             COMMAND ${CMAKE_COMMAND} -DEXPECT_CODE=1 "-DEXPECT_OUTPUT=cannot read"
                     -P "${T274_EXPECT}" -- $<TARGET_FILE:xtframe-editor> --bake-vectors
                     "${T274_DIR}/does-not-exist.xt")

    # T-265a (Docs/tasks/T-265a.md, "unbaked groups"): a group the bake leaves unbaked (a keyed
    # Offset over more frames than the bake allows) is a warning line of --bake-vectors, which says
    # what to do; packing that copy with --strict fails on the compiler's vector.unbakedModifier.
    set(T265A_FIXTURE "${CMAKE_CURRENT_SOURCE_DIR}/bundle/fixtures/unbaked_frames")
    add_test(NAME t265a_editor_bake_frames_stage
             COMMAND ${CMAKE_COMMAND} -E copy_directory "${T265A_FIXTURE}" "${T274_DIR}/t265a-src")
    set_tests_properties(t265a_editor_bake_frames_stage PROPERTIES
                         FIXTURES_REQUIRED t274_clean FIXTURES_SETUP t265a_staged)
    add_test(NAME t265a_editor_bake_frames
             COMMAND ${CMAKE_COMMAND} -DEXPECT_CODE=0
                     "-DEXPECT_OUTPUT=xtframe-editor: warning: vector.bakedFrames: timeline grow needs [0-9]+ frames \\(more than 3600\\); Icon is not baked. Lower the project fps or shorten the keyed range."
                     -P "${T274_EXPECT}" -- $<TARGET_FILE:xtframe-editor> --bake-vectors
                     "${T274_DIR}/t265a-src/main.xt" -o "${T274_DIR}/t265a-copy")
    set_tests_properties(t265a_editor_bake_frames PROPERTIES
                         FIXTURES_REQUIRED t265a_staged FIXTURES_SETUP t265a_copy)
    add_test(NAME t265a_editor_bake_frames_pack_strict
             COMMAND ${CMAKE_COMMAND} -DEXPECT_CODE=1 "-DEXPECT_OUTPUT=vector.unbakedModifier"
                     -P "${T274_EXPECT}" -- $<TARGET_FILE:xtframe-pack>
                     "${T274_DIR}/t265a-copy/main.xt" -o "${T274_DIR}/t265a-bundle" --no-thumbs --strict)
    set_tests_properties(t265a_editor_bake_frames_pack_strict PROPERTIES FIXTURES_REQUIRED t265a_copy)

    # T-261f item 1: xtframe-editor's windowless modes and xtframe-mcp read their arguments as
    # UTF-8, so a path the ANSI code page cannot spell (kana, Hangul) works too.
    set(T261F_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/t261f_utf8_args.cmake")
    set(T261F_WORK "${CMAKE_CURRENT_BINARY_DIR}/t261f-utf8-args")
    foreach(t261f_case editor_bake_vectors editor_service_schemas)
        add_test(NAME t261f_${t261f_case}
                 COMMAND ${CMAKE_COMMAND} -DCASE=${t261f_case} "-DTOOL=$<TARGET_FILE:xtframe-editor>"
                         "-DWORK=${T261F_WORK}" "-DSOURCE=${PROJECT_SOURCE_DIR}" -P "${T261F_SCRIPT}")
    endforeach()
    add_test(NAME t261f_editor_mcp
             COMMAND ${CMAKE_COMMAND} -DCASE=mcp "-DTOOL=$<TARGET_FILE:xtframe-mcp>" "-DWORK=${T261F_WORK}"
                     "-DSOURCE=${PROJECT_SOURCE_DIR}" -P "${T261F_SCRIPT}")
endif()

# T-401 (Docs/design/43-compatibility-and-versions.md 43.4): the project-level golden corpus
# (Tests/corpus). Tools/Scripts/corpus_check.sh is a thin wrapper of Tools/Scripts/corpus_check.py; CTest runs the
# Python script directly so it never depends on which `bash` the PATH holds. Its packs and renders
# go to the build tree; Tools/Scripts/dev_check.sh runs it as its own suite `corpus`.
# T-422: the expected screenshots are rendered with the Windows system fonts the corpus projects
# name (Bahnschrift, Segoe UI, Palatino Linotype, Georgia), so the test is registered on Windows
# only; cross-platform rendering is the golden suite's job (Tests/golden, with tolerances). A Debug
# run takes over 30 minutes on a hosted runner, so the test is limited to the optimised
# configurations (ctest -C Release / RelWithDebInfo).
# T-426: each project.json lists the font families it needs (`fonts`); a runner without one of them
# skips that project (--skip-missing-fonts) instead of failing on its screenshots. Tools/Scripts/dev_check.sh
# runs the strict mode (a missing font fails).
if(WIN32 AND TARGET xtframe-view AND TARGET xtframe-pack)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME corpus_check
                 COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/Tools/Scripts/corpus_check.py"
                         --view $<TARGET_FILE:xtframe-view> --pack $<TARGET_FILE:xtframe-pack>
                         --work "${CMAKE_CURRENT_BINARY_DIR}/corpus_work" --skip-missing-fonts
                 CONFIGURATIONS Release RelWithDebInfo)
        set_tests_properties(corpus_check PROPERTIES TIMEOUT 1800)
    endif()
endif()

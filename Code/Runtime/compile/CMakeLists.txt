add_library(xt_compile STATIC
    src/placeholder.cpp
    src/aliases.cpp
    src/diagnostic_report.cpp
    src/ir_writer.cpp
    src/parse_xt.cpp
    src/ports_section.cpp
    src/section_sources.cpp
    src/anchor_expand.cpp
    src/xt_canonical_writer.cpp
    src/migrate_v2.cpp
    src/migrate_sheet.cpp
    src/migrate_markup.cpp
    src/migrate_report.cpp
    src/computed_dump.cpp
    src/web_compat_lint.cpp
    src/compile_lottie.cpp
    src/compile_document.cpp
    src/style_emit.cpp
    src/calc_emit.cpp
    src/doc_emit.cpp
    src/symbol_expand.cpp
    src/bind_emit.cpp
    src/repeat_emit.cpp
    src/trigger_emit.cpp
    src/parse_svg.cpp
    src/svg_path.cpp
    src/svg_timeline.cpp
    src/lottie/lottie_parser.cpp
    src/lottie/lottie_json.cpp
    src/lottie/lottie_build.cpp
    src/lottie/lottie_parent_chain.cpp
    src/lottie/lottie_shapes_json.cpp
    src/lottie/lottie_shapes_build.cpp
    src/lottie/lottie_geometry.cpp
    src/lottie/lottie_masks.cpp
    src/lottie/lottie_assets.cpp
    src/lottie/base64.cpp
    src/xts/xts_props.cpp
    src/xts/xts_lexer.cpp
    src/xts/xts_parser.cpp
    src/xts/xts_sections.cpp
    src/xts/xts_values.cpp
    src/xts/xts_values_basic.cpp
    src/xts/xts_values_lists.cpp
    src/xts/xts_values_effect.cpp
    src/xts/xts_values_gradient.cpp
    src/xts/xts_values_transform.cpp
    src/xts/xts_values_shorthand.cpp
    src/xts/xts_values_grid.cpp
    src/xts/xts_values_calc.cpp
    src/xtfx/xtfx_parse.cpp
    src/xtfx/hlsl_subset.cpp
    src/xtfx/glsl_emit.cpp
)
target_link_libraries(xt_compile PUBLIC xt_core)
target_include_directories(xt_compile PUBLIC include)
# Code/Runtime/core/src private headers the compiler shares with the runtime: css_values.h (the CSS value grammar,
# T-413), css_color.h, transform_compose.h (T-120) and style_decode.h (the StyleProp numbers).
target_include_directories(xt_compile PRIVATE ${PROJECT_SOURCE_DIR}/Code/Runtime/core/src)
target_link_libraries(xt_compile PRIVATE xt_nlohmann)
xt_enable_exceptions(xt_compile)
xt_set_warnings(xt_compile)
xt_unity_build(xt_compile)  # T-361
# Both redeclare functions defined elsewhere with their default arguments (xts_parser.cpp,
# compile_document.cpp), which one translation unit cannot repeat: compiled on their own. T-412:
# xts_values.cpp is split into xts_values*.cpp, which all include xts_values_internal.h (the
# redeclarations), so every one of them stays out of the batches.
set_source_files_properties(src/bind_emit.cpp src/xts/xts_values.cpp src/xts/xts_values_basic.cpp
    src/xts/xts_values_lists.cpp src/xts/xts_values_effect.cpp src/xts/xts_values_gradient.cpp
    src/xts/xts_values_transform.cpp src/xts/xts_values_shorthand.cpp src/xts/xts_values_grid.cpp
    src/xts/xts_values_calc.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
# T-587: the v2 sources stay out of the batches too, so adding them does not regroup the batches
# above (whose files share anonymous-namespace names such as kPi).
set_source_files_properties(src/ports_section.cpp src/section_sources.cpp src/xt_canonical_writer.cpp
    src/xts/xts_sections.cpp src/anchor_expand.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
# T-589: the migrator and the computed-style dump, likewise.
set_source_files_properties(src/migrate_v2.cpp src/migrate_sheet.cpp src/migrate_markup.cpp src/migrate_report.cpp
    src/computed_dump.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
# T-594: the web-compat lint, likewise.
set_source_files_properties(src/web_compat_lint.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

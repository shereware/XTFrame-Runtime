add_library(xt_core STATIC
    src/version.cpp
    src/charconv.cpp
    src/assert.cpp
    src/perf.cpp
    src/perf_record.cpp  # T-750
    src/thread_clock.cpp  # T-750
    src/mat2x3.cpp
    src/mat4.cpp
    src/transform_compose.cpp
    src/project3d.cpp
    src/camera.cpp
    src/node_arena.cpp
    src/crc32.cpp
    src/sha256.cpp
    src/ir_view.cpp
    src/ir_view_texp.cpp  # T-776
    src/geom_flatten.cpp
    src/coverage_raster.cpp  # T-660a: moved from xt_sw
    src/coverage_bitmap.cpp  # T-660a
    src/image_mips.cpp  # T-665
    src/geom_stroke.cpp
    src/geom_dash.cpp
    src/path_trim.cpp
    src/shape_modifiers.cpp
    src/delaunay.cpp
    src/rig_eval.cpp
    src/noise.cpp
    src/geom_bezier.cpp
    src/property_access.cpp
    src/easing.cpp
    src/track_eval.cpp
    src/scene_instance.cpp
    src/animator.cpp
    src/animator_expr.cpp  # T-776
    src/animator_neutral.cpp  # T-869
    src/timeline_group.cpp
    src/timeline_expr.cpp  # T-776
    src/external_media.cpp
    src/trigger_runtime.cpp
    src/trigger_self_control.cpp  # T-774
    src/trigger_pointer.cpp  # T-871
    src/trigger_channels.cpp  # T-873
    src/repeat_controller.cpp
    src/repeat_template_content.cpp
    src/binding_runtime.cpp
    src/binding_runtime_eval.cpp
    src/binding_runtime_ports.cpp
    src/binding_runtime_api.cpp
    src/hot_reload.cpp
    src/document_state.cpp
    src/command_list.cpp
    src/command_list_append.cpp
    src/effect_utils.cpp
    src/box_shadow.cpp  # T-668
    src/shader_table.cpp
    src/command_simplifier.cpp
    src/baked_writer.cpp
    src/baked_player.cpp
    src/world_transform.cpp
    src/layer_collect.cpp
    src/rounded_rect.cpp
    src/clip_shape.cpp
    src/painter.cpp
    src/particles.cpp
    src/painter_3d.cpp
    src/painter_world_layers.cpp
    src/world_layer_output.cpp
    src/painter_element.cpp
    src/border_dash_cache.cpp  # T-737
    src/hit_test.cpp
    src/event_dispatcher.cpp
    src/focus.cpp
    src/focus_nav.cpp
    src/drag_drop.cpp
    src/gesture.cpp
    src/text_edit.cpp
    src/composition.cpp
    src/painter_text.cpp
    src/painter_caret.cpp
    src/document.cpp
    src/document_channels.cpp  # T-873
    src/element_ref.cpp
    src/mini_css.cpp
    src/style_template.cpp
    src/css_color.cpp
    src/css_values.cpp
    src/selector_query.cpp
    src/document_load.cpp
    src/context.cpp
    src/advance_plan.cpp  # T-879
    src/context_channels.cpp  # T-873
    src/input_channels.cpp  # T-873
    src/context_visual.cpp
    src/needs_frame.cpp  # T-749
    src/external_texture_use.cpp  # T-746
    src/command_hash.cpp
    src/damage.cpp
    src/file_asset_resolver.cpp
    src/file_open_utf8.cpp
    src/dds.cpp
    src/image_format.cpp  # T-821
    src/text_layout.cpp
    src/null_text_shaper.cpp
    src/style_decode.cpp
    src/selector_match.cpp
    src/style_compare.cpp  # T-753
    src/style_engine.cpp
    src/style_state_deps.cpp
    src/layout_engine.cpp
    src/layout_block.cpp
    src/layout_text.cpp
    src/text_runs.cpp
    src/text_case.cpp
    src/layout_absolute.cpp
    src/layout_flex.cpp
    src/layout_grid.cpp
    src/layout_partial.cpp
    src/layout_snapshot.cpp
)
target_include_directories(xt_core PUBLIC include PRIVATE src)
xt_set_warnings(xt_core)
xt_no_exceptions_rtti(xt_core)
xt_precompile(xt_core <vector> <string> <algorithm> <cstdint> <span>)  # T-361

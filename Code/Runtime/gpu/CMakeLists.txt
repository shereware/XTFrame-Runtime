# xt_gpu: the device-neutral GPU frame builder (Docs/design/10-backends.md §10.5, Docs/tasks/
# T-146.md). CommandList -> GpuFrame; no GL/D3D headers, no exceptions, no RTTI (same rules as
# xt_core). The glyph-atlas path needs xt_text; without it XT_GPU_NO_TEXT compiles it out and
# DrawGlyphs always uses the outline fallback (PUBLIC, so consumers see the same guarded
# declarations frame_builder.h has).
add_library(xt_gpu STATIC
    src/gpu_frame.cpp
    src/frame_emit.cpp
    src/gradient_lut.cpp
    src/gpu_effects.cpp
    src/frame_builder.cpp
)
target_include_directories(xt_gpu PUBLIC include)
# T-162: the effect result cache keys segments with core's per-command hash (damage_internal.h),
# the same one Docs/design/23-host-integration.md §23.2's DamageList is built from.
target_include_directories(xt_gpu PRIVATE ${PROJECT_SOURCE_DIR}/Code/Runtime/core/src)
target_link_libraries(xt_gpu PUBLIC xt_core)
if(XT_BUILD_TEXT)
    target_link_libraries(xt_gpu PUBLIC xt_text)
else()
    target_compile_definitions(xt_gpu PUBLIC XT_GPU_NO_TEXT)
endif()
xt_set_warnings(xt_gpu)
xt_no_exceptions_rtti(xt_gpu)

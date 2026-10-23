# xt_wgpu: the WebGPU GpuDevice (Docs/design/10-backends.md §10.10, Docs/tasks/T-437.md, T-438.md)
# over the prebuilt wgpu-native of Tools/CMake/xt_wgpu.cmake (xt::wgpu_native). Added only when
# XT_BUILD_WGPU is still ON after that file ran. Built like xt_gpu: no exceptions, no RTTI.

# T-438: the hand-written WGSL of src/shaders (one file per Shading / EffectProgram, each naming the
# GLSL of Code/Runtime/gl/src/gl_shaders.h it mirrors) embedded as strings at build time.
set(XT_WGSL_SOURCES
    src/shaders/prelude.wgsl
    src/shaders/solid.wgsl
    src/shaders/gradient.wgsl
    src/shaders/texture.wgsl
    src/shaders/texture_straight.wgsl
    src/shaders/glyph.wgsl
    src/shaders/composite.wgsl
    src/shaders/particle_texture.wgsl
    src/shaders/particle_solid.wgsl
    src/shaders/fx_gauss_scalar.wgsl
    src/shaders/fx_gauss_color.wgsl
    src/shaders/fx_dilate.wgsl
    src/shaders/fx_alpha_extract.wgsl
    src/shaders/fx_sdf.wgsl
    src/shaders/fx_color_matrix.wgsl
    src/shaders/fx_gradient_mask.wgsl
    src/shaders/fx_shine.wgsl
    src/shaders/fx_shadow_glow.wgsl
    src/shaders/fx_outline.wgsl
    src/shaders/fx_backdrop.wgsl
    src/shaders/fx_mask_apply.wgsl
    src/shaders/fx_downsample.wgsl
    src/shaders/fx_kawase.wgsl
    src/shaders/fx_level_mix.wgsl
    src/shaders/blit.wgsl
)
set(xt_wgsl_abs "")
foreach(f IN LISTS XT_WGSL_SOURCES)
    list(APPEND xt_wgsl_abs "${CMAKE_CURRENT_SOURCE_DIR}/${f}")
endforeach()
string(REPLACE ";" "|" xt_wgsl_arg "${xt_wgsl_abs}")
set(xt_wgsl_header "${CMAKE_CURRENT_BINARY_DIR}/gen/xt_wgpu_shaders.h")
add_custom_command(
    OUTPUT "${xt_wgsl_header}"
    COMMAND ${CMAKE_COMMAND} "-DOUT=${xt_wgsl_header}" "-DSOURCES=${xt_wgsl_arg}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_wgsl.cmake"
    DEPENDS ${xt_wgsl_abs} "${CMAKE_CURRENT_SOURCE_DIR}/embed_wgsl.cmake"
    COMMENT "Embedding xt_wgpu WGSL shaders"
    VERBATIM
)

add_library(xt_wgpu STATIC
    src/wgpu_init.cpp
    src/wgpu_device.cpp
    src/wgpu_draw.cpp
    src/wgpu_pipelines.cpp
    src/wgpu_custom.cpp
    src/wgpu_geometry.cpp
    src/wgpu_renderer.cpp
    "${xt_wgsl_header}"
)
target_include_directories(xt_wgpu PUBLIC include)
target_include_directories(xt_wgpu PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gen")
target_link_libraries(xt_wgpu PUBLIC xt_core xt_gpu xt::wgpu_native)
xt_set_warnings(xt_wgpu)
xt_no_exceptions_rtti(xt_wgpu)

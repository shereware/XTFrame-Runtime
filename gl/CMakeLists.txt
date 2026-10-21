add_library(xt_gl STATIC
    src/gl_loader.cpp
    src/gl_util.cpp
    src/gl_geometry.cpp
    src/gl_fbo.cpp
    src/gl_state.cpp
    src/gl_state_cache.cpp
    src/gl_effects.cpp
    src/gl_device.cpp
    src/gl_renderer.cpp
)
target_include_directories(xt_gl PUBLIC include)
# T-147: xt_gl is the first xt::gpu::GpuDevice; every CPU-side algorithm lives in xt_gpu
# (docs/design/10-backends.md §10.5).
target_link_libraries(xt_gl PUBLIC xt_core xt_gpu)
target_include_directories(xt_gl SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/stb)
# DrawGlyphs atlas support (docs/tasks/T-31.md): only available when xt_text is built; otherwise
# XT_GL_NO_TEXT compiles the public setGlyphAtlas() declaration out (xt_gpu's own XT_GPU_NO_TEXT
# does the same for FrameBuilder). PUBLIC so consumers (xt_shell, tests/gl) see the same macro
# gl_renderer.h's public declarations are guarded by.
if(XT_BUILD_TEXT)
    target_link_libraries(xt_gl PUBLIC xt_text)
else()
    target_compile_definitions(xt_gl PUBLIC XT_GL_NO_TEXT)
endif()
xt_enable_exceptions(xt_gl)
xt_set_warnings(xt_gl)

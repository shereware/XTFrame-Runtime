# Code/Backends/: reference platform x renderer glue (Code/Backends/README.md; Docs/design/45-repository-layout.md
# §45.7). Nothing here is required by the runtime. A backend is added only when every file of its
# platform pair, renderer pair and combination file exists, so this directory can hold the skeleton
# of planned Code/Backends without breaking the build.
#
# T-439: each present part is its own static library -- xt_platform_<p> and xt_renderer_<r> -- added
# only when its host API is available in this build (SDL2, Win32, xt_gl, xt_wgpu; otherwise skipped
# silently), and xt_backend_<p>_<r> is the combination file linking the two. An executable links
# exactly one xt_backend_* (each defines xt::backend::Backend); a tool that picks a renderer at
# run time (xtframe-view --renderer) links the part libraries instead.

set(XT_BACKEND_PLATFORMS sdl2 win32 glfw x11 wayland android)
set(XT_BACKEND_RENDERERS gl3 gles3 sw wgpu d3d11 d3d12 vulkan metal)

# _xt_backend_host_api(<kind> <name> <out_ok> <out_libs>): whether the host API of a part exists in
# this build, and what the part links for it.
function(_xt_backend_host_api kind name out_ok out_libs)
    set(ok FALSE)
    set(libs "")
    if(kind STREQUAL "platform" AND name STREQUAL "sdl2" AND TARGET SDL2::SDL2-static)
        set(ok TRUE)
        set(libs SDL2::SDL2-static)
    elseif(kind STREQUAL "platform" AND name STREQUAL "win32" AND WIN32)
        set(ok TRUE)
        set(libs user32 gdi32)
    elseif(kind STREQUAL "renderer" AND name STREQUAL "gl3" AND TARGET xt_gl)
        set(ok TRUE)
        set(libs xt_gl)
    elseif(kind STREQUAL "renderer" AND name STREQUAL "sw" AND TARGET xt_sw)
        set(ok TRUE)
        set(libs xt_sw)
    elseif(kind STREQUAL "renderer" AND name STREQUAL "wgpu" AND TARGET xt_wgpu)
        set(ok TRUE)
        set(libs xt_wgpu)
    endif()
    set(${out_ok} ${ok} PARENT_SCOPE)
    set(${out_libs} "${libs}" PARENT_SCOPE)
endfunction()

# xt_add_backend_part(<platform|renderer> <name>): STATIC library xt_<kind>_<name> when its .cpp
# exists and its host API is available; otherwise nothing.
function(xt_add_backend_part kind name)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/xt_${kind}_${name}.cpp")
    if(NOT EXISTS "${_src}")
        return()
    endif()
    _xt_backend_host_api(${kind} ${name} _ok _libs)
    if(NOT _ok)
        return()
    endif()
    set(_name xt_${kind}_${name})
    add_library(${_name} STATIC "${_src}")
    target_include_directories(${_name} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${_name} PUBLIC xt_core ${_libs})
    # Glyph atlases (GPU renderers) and setTextShaper() need xt_text; without it the declarations
    # are compiled out (XT_BACKEND_NO_TEXT, like XT_GL_NO_TEXT / XT_GPU_NO_TEXT).
    if(XT_BUILD_TEXT AND TARGET xt_text)
        target_link_libraries(${_name} PUBLIC xt_text)
    else()
        target_compile_definitions(${_name} PUBLIC XT_BACKEND_NO_TEXT)
    endif()
    xt_enable_exceptions(${_name})
    xt_set_warnings(${_name})
endfunction()

# xt_add_backend(<platform> <renderer>): STATIC library xt_backend_<platform>_<renderer> when its
# three parts are present (and built); otherwise nothing.
function(xt_add_backend platform renderer)
    set(_b "${CMAKE_CURRENT_SOURCE_DIR}/xt_backend_${platform}_${renderer}.cpp")
    if(NOT EXISTS "${_b}" OR NOT TARGET xt_platform_${platform} OR NOT TARGET xt_renderer_${renderer})
        return()
    endif()
    set(_name xt_backend_${platform}_${renderer})
    add_library(${_name} STATIC "${_b}")
    target_include_directories(${_name} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${_name} PUBLIC xt_core xt_platform_${platform} xt_renderer_${renderer})
    xt_enable_exceptions(${_name})
    xt_set_warnings(${_name})
    message(STATUS "backend: ${_name}")
endfunction()

foreach(_platform IN LISTS XT_BACKEND_PLATFORMS)
    xt_add_backend_part(platform ${_platform})
endforeach()
foreach(_renderer IN LISTS XT_BACKEND_RENDERERS)
    xt_add_backend_part(renderer ${_renderer})
endforeach()
foreach(_platform IN LISTS XT_BACKEND_PLATFORMS)
    foreach(_renderer IN LISTS XT_BACKEND_RENDERERS)
        xt_add_backend(${_platform} ${_renderer})
    endforeach()
endforeach()

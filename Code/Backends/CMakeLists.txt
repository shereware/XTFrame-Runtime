# Code/Backends/: reference platform × renderer glue (Code/Backends/README.md; Docs/design/45-repository-layout.md
# §45.7). Nothing here is required by the runtime. A backend is added only when every file of its
# platform pair, renderer pair and combination file exists, so this directory can hold the skeleton
# of planned Code/Backends without breaking the build.

set(XT_BACKEND_PLATFORMS sdl2 win32 glfw x11 wayland android)
set(XT_BACKEND_RENDERERS gl3 gles3 sw d3d11 d3d12 vulkan metal)

# xt_add_backend(<platform> <renderer>): STATIC library xt_backend_<platform>_<renderer> when its
# three parts are present; otherwise a one-line status message and nothing else.
function(xt_add_backend platform renderer)
    set(_p "${CMAKE_CURRENT_SOURCE_DIR}/xt_platform_${platform}.cpp")
    set(_r "${CMAKE_CURRENT_SOURCE_DIR}/xt_renderer_${renderer}.cpp")
    set(_b "${CMAKE_CURRENT_SOURCE_DIR}/xt_backend_${platform}_${renderer}.cpp")
    if(NOT (EXISTS "${_p}" AND EXISTS "${_r}" AND EXISTS "${_b}"))
        return()
    endif()
    set(_name xt_backend_${platform}_${renderer})
    add_library(${_name} STATIC "${_p}" "${_r}" "${_b}")
    target_include_directories(${_name} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${_name} PUBLIC xt_core)
    xt_set_warnings(${_name})
    message(STATUS "backend: ${_name}")
endfunction()

foreach(_platform IN LISTS XT_BACKEND_PLATFORMS)
    foreach(_renderer IN LISTS XT_BACKEND_RENDERERS)
        xt_add_backend(${_platform} ${_renderer})
    endforeach()
endforeach()

add_library(xt_shell STATIC
    src/app.cpp
    src/window.cpp
    src/sdl_input.cpp
    src/sdl_rw_asset_resolver.cpp
)
target_include_directories(xt_shell PUBLIC include)
# T-376 (23 §23.ab): the borderless window's platform side (Windows: the resizable style, the
# work-area maximize through a comctl32 window subclass, the DWM shadow). T-393 (08 §8.14): the
# OLE file drop target (ole32: OleInitialize / RegisterDragDrop; shell32: DragQueryFileW; uuid:
# IID_IDropTarget).
if(WIN32)
    target_sources(xt_shell PRIVATE src/platform_win.cpp)
    target_link_libraries(xt_shell PRIVATE dwmapi comctl32 ole32 shell32 uuid)
else()
    target_sources(xt_shell PRIVATE src/platform_other.cpp)
endif()
# T-598: no xt_compile here -- a shipped desktop host links xt_shell without the compiler; the
# `--watch` hot reload lives in xt_shell_watch below.
target_link_libraries(xt_shell PUBLIC xt_core xt_sw SDL2::SDL2-static)
if(WIN32)
    target_link_libraries(xt_shell PUBLIC SDL2::SDL2main)
endif()
if(XT_SHELL_GL)
    target_link_libraries(xt_shell PUBLIC xt_gl)
    # app.cpp/sdl_input.cpp use SDL's public headers (already SYSTEM via the SDL2::SDL2-static
    # target) and xt_gl's private gl_loader.h (shares the process-wide GL function-pointer globals
    # GlRenderer::init() loads -- see Tests/gl/CMakeLists.txt for the same pattern).
    target_include_directories(xt_shell PRIVATE ${PROJECT_SOURCE_DIR}/Code/Runtime/gl/src)
    # GlyphAtlas injection (Docs/tasks/T-31.md "app.cpp（建立 GlyphAtlas 並注入）"): xt_gl already
    # PUBLIC-links xt_text (and propagates XT_GL_NO_TEXT's absence/presence) when XT_BUILD_TEXT is
    # on -- see Code/Runtime/gl/CMakeLists.txt -- so app.cpp sees FtTextShaper/GlyphAtlas's headers
    # automatically and guards its own use of them on the same macro.
else()
    # T-240 (Docs/design/23-host-integration.md §23.11.1): the SW presentation path. xt_sw renders
    # the frame on the CPU and an SDL_Renderer streaming texture presents it; xt_gl is not linked.
    # XT_SHELL_SW is what app.h / xtframe-view test; XT_GL_NO_TEXT keeps its xt_gl meaning ("no
    # xt_text") so the text guards stay one macro.
    target_compile_definitions(xt_shell PUBLIC XT_SHELL_SW)
    if(XT_BUILD_TEXT)
        target_link_libraries(xt_shell PUBLIC xt_text)
    else()
        target_compile_definitions(xt_shell PUBLIC XT_GL_NO_TEXT)
    endif()
endif()
xt_enable_exceptions(xt_shell)
# T-240: SdlRwAssetResolver derives from xt_core's FileAssetResolver, whose typeinfo the Itanium ABI
# only emits next to its key function -- never under xt_core's -fno-rtti (see xt_no_rtti).
xt_no_rtti(xt_shell)
xt_set_warnings(xt_shell)

# T-595 / T-598: SourceWatcher (`--watch` hot reload) recompiles the UI with xt_compile, so it is a
# target of its own on top of xt_shell; only a host that wants hot reload (desktop_app, the tests)
# links it. Its header sits in xt_shell's include directory (xt/shell/source_watcher.h) but needs
# xt_compile's headers, which only this target provides.
add_library(xt_shell_watch STATIC
    src/source_watcher.cpp
)
target_link_libraries(xt_shell_watch PUBLIC xt_shell xt_compile)
xt_enable_exceptions(xt_shell_watch)
xt_no_rtti(xt_shell_watch)
xt_set_warnings(xt_shell_watch)

# T-598 item 1: xt_shell never links xt_compile. Checked once the whole tree is configured, so a
# target_link_libraries(xt_shell ...) added anywhere later is caught too.
function(_xt_assert_shell_without_compile)
    foreach(prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(libs xt_shell ${prop})
        if(libs AND "xt_compile" IN_LIST libs)
            message(FATAL_ERROR "T-598: xt_shell must not link xt_compile (found in ${prop}); "
                                "hot reload belongs to xt_shell_watch")
        endif()
    endforeach()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _xt_assert_shell_without_compile)

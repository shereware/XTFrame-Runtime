#define DOCTEST_CONFIG_IMPLEMENT
#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "doctest.h"
#include "gl_loader.h"
#include "test_temp.h"
#include "test_watchdog.h"

/// xt_gl_tests: a standalone doctest executable that needs a real GL context before any TEST_CASE
/// can run. SDL2 (hidden window) is used to create one (Docs/tasks/T-19.md "測試"). If SDL init or
/// context creation fails (no GPU/driver available, e.g. a Windows CI runner or a machine without
/// Mesa llvmpipe), print the sheet's required message and exit 0 without running any tests --
/// CI treats this as "skipped", not "failed". T-267a: with XT_REQUIRE_GL=1 in the environment (the
/// gate machines; Tools/Scripts/dev_check.sh exports it) a missing context is an error and the exit code is 1.
///
/// test_gl_renderer.cpp reads these globals (declared extern there) to get a working GL context
/// and a loader-compatible function pointer.

SDL_Window* g_xtGlWindow = nullptr;
SDL_GLContext g_xtGlContext = nullptr;
bool g_xtGlAvailable = false;

namespace {

bool initGl() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    g_xtGlWindow = SDL_CreateWindow("xt_gl_tests", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 128, 128,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (g_xtGlWindow == nullptr) {
        return false;
    }
    g_xtGlContext = SDL_GL_CreateContext(g_xtGlWindow);
    if (g_xtGlContext == nullptr) {
        return false;
    }
    if (SDL_GL_MakeCurrent(g_xtGlWindow, g_xtGlContext) != 0) {
        return false;
    }
    // A context can exist without exposing GL 3.3 entry points (e.g. the Windows CI runner's
    // software GL 1.1); treat that as "no usable context" and skip.
    if (!xt::gl::loadGl(
            [](const char* name) { return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name)); })) {
        std::printf("xt_gl_tests: GL 3.3 entry points missing (%s), skipping\n", xt::gl::lastMissing());
        return false;
    }
    return true;
}

/// T-267a: XT_REQUIRE_GL=1 turns "no usable GL context" from a skip into a failure.
bool requireGl() {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    if (_dupenv_s(&v, &len, "XT_REQUIRE_GL") != 0 || v == nullptr) {
        return false;
    }
    const bool on = std::strcmp(v, "1") == 0;
    std::free(v);
    return on;
#else
    const char* v = std::getenv("XT_REQUIRE_GL");
    return v != nullptr && std::strcmp(v, "1") == 0;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    g_xtGlAvailable = initGl();
    if (!g_xtGlAvailable) {
        const bool required = requireGl();
        if (required) {
            std::printf("xt_gl_tests: ERROR: no GL context available and XT_REQUIRE_GL=1 (%s)\n",
                        SDL_GetError());
        } else {
            std::printf("xt_gl_tests: no GL context available, skipping\n");
        }
        if (g_xtGlContext != nullptr) {
            SDL_GL_DeleteContext(g_xtGlContext);
        }
        if (g_xtGlWindow != nullptr) {
            SDL_DestroyWindow(g_xtGlWindow);
        }
        SDL_Quit();
        return required ? 1 : 0;
    }

    // T-267d: the per-case watchdog (Tests/common/test_watchdog.h) and the temp root clean-up.
    xt::test::startWatchdog();
    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    int result = ctx.run();
    if (ctx.shouldExit()) {
        result = 0;
    }

    SDL_GL_DeleteContext(g_xtGlContext);
    SDL_DestroyWindow(g_xtGlWindow);
    SDL_Quit();
    return xt::test::finishTempDir(result);
}

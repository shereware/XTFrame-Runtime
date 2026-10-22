#define SDL_MAIN_HANDLED  // doctest owns main(); SDL must not rename it.

#include <cstdint>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/shell/app.h"

/// T-377 (Docs/design/23-host-integration.md §23.ab, 13 §13.5): App's live-resize event watch. A
/// SIZE_CHANGED outside run() runs no frame; inside run(), one that arrives while a frame is in
/// pollEvents() (where the OS modal size loop runs; simulated from the event hook) runs one extra
/// frame right away with the new viewport; one pushed from inside onFrame does not re-enter.

using namespace xt;
using namespace xt::shell;

namespace {

// App has no window accessor; the test process has exactly one window open at a time.
SDL_Window* onlyWindow() {
    for (Uint32 id = 1; id < 256; ++id) {
        if (SDL_Window* window = SDL_GetWindowFromID(id)) {
            return window;
        }
    }
    return nullptr;
}

void pushSizeChanged(SDL_Window* window) {
    SDL_Event event{};
    event.type = SDL_WINDOWEVENT;
    event.window.event = SDL_WINDOWEVENT_SIZE_CHANGED;
    event.window.windowID = SDL_GetWindowID(window);
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    event.window.data1 = w;
    event.window.data2 = h;
    SDL_PushEvent(&event);
}

}  // namespace

TEST_CASE("T-377: the live-resize watch runs a frame only inside run(), from inside pollEvents") {
    WindowConfig config;
    config.width = 320;
    config.height = 240;
    config.hidden = true;
    config.vsync = false;
    config.msaa = 0;
    App app(config);
    if (!app.ok()) {
        MESSAGE("skipped: no window could be created (" << app.lastError() << ")");
        return;
    }
    SDL_Window* window = onlyWindow();
    REQUIRE(window != nullptr);

    // Outside run(): the watch does nothing (the event stays queued for the next poll).
    int frames = 0;
    pushSizeChanged(window);
    CHECK(frames == 0);

    const Uint32 kick = SDL_RegisterEvents(1);
    REQUIRE(kick != static_cast<Uint32>(-1));
    int framesBeforeResize = -1;
    int framesAfterResize = -1;
    int framesAfterPushInFrame = -1;
    int nestedWidth = 0;
    bool resizing = false;
    app.setEventHook([&](const SDL_Event& event) {
        if (event.type != kick) {
            return false;
        }
        // Inside pollEvents(), like the OS modal size loop: the resize runs a frame at once.
        framesBeforeResize = frames;
        resizing = true;
        SDL_SetWindowSize(window, 640, 360);
        pushSizeChanged(window);
        resizing = false;
        framesAfterResize = frames;
        return true;
    });
    app.run([&](float) {
        ++frames;
        if (resizing && nestedWidth == 0) {
            nestedWidth = app.drawableWidth();
        }
        if (frames == 1) {
            // Inside onFrame (not polling): no nested frame.
            pushSizeChanged(window);
            framesAfterPushInFrame = frames;
            SDL_Event event{};
            event.type = kick;
            SDL_PushEvent(&event);
        }
        if (frames >= 6) {
            app.quit();
        }
    });

    CHECK(framesAfterPushInFrame == 1);
    REQUIRE(framesBeforeResize >= 1);
    CHECK(framesAfterResize > framesBeforeResize);
    CHECK(nestedWidth > 320);
    CommandList list;
    app.context().render(list, nullptr);
    CHECK(list.viewport.w == doctest::Approx(640.0f));
    CHECK(list.viewport.h == doctest::Approx(360.0f));
}

TEST_CASE("T-377: WindowConfig::liveResize = false installs no watch") {
    WindowConfig config;
    config.width = 320;
    config.height = 240;
    config.hidden = true;
    config.vsync = false;
    config.msaa = 0;
    config.liveResize = false;
    App app(config);
    if (!app.ok()) {
        MESSAGE("skipped: no window could be created (" << app.lastError() << ")");
        return;
    }
    SDL_Window* window = onlyWindow();
    REQUIRE(window != nullptr);
    const Uint32 kick = SDL_RegisterEvents(1);
    REQUIRE(kick != static_cast<Uint32>(-1));
    int frames = 0;
    int framesBeforeResize = -1;
    int framesAfterResize = -1;
    app.setEventHook([&](const SDL_Event& event) {
        if (event.type != kick) {
            return false;
        }
        framesBeforeResize = frames;
        SDL_SetWindowSize(window, 640, 360);
        pushSizeChanged(window);
        framesAfterResize = frames;
        return true;
    });
    app.run([&](float) {
        if (++frames == 1) {
            SDL_Event event{};
            event.type = kick;
            SDL_PushEvent(&event);
        }
        if (frames >= 4) {
            app.quit();
        }
    });
    CHECK(framesBeforeResize == 1);
    CHECK(framesAfterResize == framesBeforeResize);
    // The poll loop still syncs the viewport on the next frame.
    CommandList list;
    app.context().render(list, nullptr);
    CHECK(list.viewport.w == doctest::Approx(640.0f));
}

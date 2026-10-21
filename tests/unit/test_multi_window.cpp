#define SDL_MAIN_HANDLED  // doctest owns main(); SDL must not rename it.

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/event.h"
#include "xt/shell/app.h"

/// T-384b (docs/design/23-host-integration.md §23.ad, 13 §13.5 T-384): App with a second window.
/// Each window has its own Context and damage; SDL events go to the window their windowID names;
/// closeWindow() is silent, a user CLOSE calls the window-closed handler, the main window's CLOSE
/// ends the frame loop.

using namespace xt;
using namespace xt::shell;

namespace {

// #A turns red on press and emits `down` to the Context listener.
constexpr const char* kDoc = R"X(<xt><head><style>
  #A { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; background-color: #00ff00; }
  #A:state(On) { background-color: #ff0000; }
</style></head><body id="Root"><div id="A" state-groups="G: Off|On" on:press="emit(down); state(On)"></div>
</body></xt>)X";

std::vector<uint8_t> compileDoc() {
    compile::CompileOptions options;
    options.emitMeta = false;
    std::vector<compile::SourceFile> files{{"main.xt", kDoc}};
    compile::CompileResult r = compile::compileDocumentFromStrings(files, "main.xt", options);
    REQUIRE(r.ok());
    return std::move(r.ir);
}

struct EmitLog : ContextListener {
    std::vector<std::string> names;
    void onHostEvent(const HostEvent& e) override {
        if (e.kind == HostEventKind::Emit) {
            names.emplace_back(e.name);
        }
    }
};

WindowConfig hiddenConfig() {
    WindowConfig config;
    config.width = 320;
    config.height = 240;
    config.hidden = true;
    config.vsync = false;
    config.msaa = 0;
    return config;
}

void pushMouseButton(Window& window, Uint32 type, int x, int y) {
    SDL_Event event{};
    event.type = type;
    event.button.windowID = SDL_GetWindowID(window.sdlWindow());
    event.button.button = SDL_BUTTON_LEFT;
    event.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
    event.button.clicks = 1;
    event.button.x = x;
    event.button.y = y;
    SDL_PushEvent(&event);
}

void pushClose(Window& window) {
    SDL_Event event{};
    event.type = SDL_WINDOWEVENT;
    event.window.event = SDL_WINDOWEVENT_CLOSE;
    event.window.windowID = SDL_GetWindowID(window.sdlWindow());
    SDL_PushEvent(&event);
}

bool noDamage(const DamageList& damage) { return !damage.full && damage.rects.empty(); }

}  // namespace

TEST_CASE("T-384b: two windows, independent Contexts, per-window event dispatch and close") {
    App app(hiddenConfig());
    if (!app.ok()) {
        MESSAGE("skipped: no window could be created (" << app.lastError() << ")");
        return;
    }
    CHECK(app.displayCount() >= 1);
    const Rect display = app.displayBounds(0);
    CHECK(display.w > 0.0f);
    CHECK(display.h > 0.0f);
    const Rect usable = app.displayUsableBounds(0);
    CHECK(usable.w > 0.0f);

    const WindowId second = app.createWindow(hiddenConfig());
    REQUIRE(second == 1);
    CHECK(app.windows() == std::vector<WindowId>{0, 1});
    REQUIRE(app.findWindow(1) != nullptr);
    CHECK(app.findWindow(7) == nullptr);
    CHECK(&app.window(0) == &app.mainWindow());
    CHECK(app.window(1).id() == 1);
    CHECK(app.window(1).sdlWindow() != app.window(0).sdlWindow());
    CHECK(&app.window(1).context() != &app.context());
    CHECK(app.window(1).displayIndex() >= 0);
    CHECK(app.window(1).bounds().w == doctest::Approx(320.0f));

    const std::vector<uint8_t> ir = compileDoc();
    EmitLog logs[2];
    for (WindowId id : {WindowId{0}, WindowId{1}}) {
        Window& window = app.window(id);
        Result<DocumentHandle> h = window.context().loadDocument(IrBlob{ir.data(), ir.size()}, false);
        REQUIRE(h.ok());
        window.setCurrentDocument(h.value());
        window.syncViewport();
        window.context().addListener(&logs[id]);
    }
    std::vector<WindowId> seen;
    auto perWindow = [&](float, WindowId id) { seen.push_back(id); };
    for (int i = 0; i < 3; ++i) {
        CHECK(app.runFrame(perWindow, 0.016f, /*swap=*/false));
    }
    CHECK(seen == std::vector<WindowId>{0, 1, 0, 1, 0, 1});
    // Settled: neither window has anything to redraw.
    CHECK(noDamage(app.window(0).damage()));
    CHECK(noDamage(app.window(1).damage()));

    // A press on window 1 reaches only window 1's document, and only window 1 gets damage.
    pushMouseButton(app.window(1), SDL_MOUSEBUTTONDOWN, 20, 20);
    pushMouseButton(app.window(1), SDL_MOUSEBUTTONUP, 20, 20);
    int mainFrames = 0;
    CHECK(app.runFrame([&](float) { ++mainFrames; }, 0.016f, /*swap=*/false));
    CHECK(mainFrames == 1);  // The single-argument onFrame runs for window 0 only.
    CHECK(logs[0].names.empty());
    REQUIRE(logs[1].names.size() == 1);
    CHECK(logs[1].names[0] == "down");
    CHECK(noDamage(app.window(0).damage()));
    CHECK_FALSE(noDamage(app.window(1).damage()));

    // closeWindow(): T-398 -- the same path as a user close (the closed handler), gone at once.
    std::vector<WindowId> closed;
    app.setWindowClosedHandler([&](WindowId id) { closed.push_back(id); });
    app.window(1).context().removeListener(&logs[1]);
    app.closeWindow(1);
    CHECK(closed == std::vector<WindowId>{1});
    closed.clear();
    CHECK(app.windows() == std::vector<WindowId>{0});
    CHECK(app.findWindow(1) == nullptr);

    // A user CLOSE on another window: the handler once, then the window is destroyed. Ids are
    // never reused, so the new window is 2.
    const WindowId third = app.createWindow(hiddenConfig());
    REQUIRE(third == 2);
    pushClose(app.window(2));
    CHECK(app.runFrame(perWindow, 0.016f, /*swap=*/false));
    CHECK(closed == std::vector<WindowId>{2});
    CHECK(app.windows() == std::vector<WindowId>{0});
    CHECK(app.runFrame(perWindow, 0.016f, /*swap=*/false));
    CHECK(closed.size() == 1);

    // The main window's CLOSE ends the loop.
    pushClose(app.mainWindow());
    CHECK_FALSE(app.runFrame(perWindow, 0.016f, /*swap=*/false));
    app.context().removeListener(&logs[0]);
}

TEST_CASE("T-384b: run() with a per-window onFrame, closeWindow() from inside a frame") {
    App app(hiddenConfig());
    if (!app.ok()) {
        MESSAGE("skipped: no window could be created (" << app.lastError() << ")");
        return;
    }
    REQUIRE(app.createWindow(hiddenConfig()) == 1);
    app.setTargetFps(120);  // Every window has vsync off: paced, and run() still ends.
    int frames[2] = {0, 0};
    bool handlerCalled = false;
    app.setWindowClosedHandler([&](WindowId) { handlerCalled = true; });
    app.run([&](float, WindowId id) {
        ++frames[id];
        if (id == 1 && frames[1] == 2) {
            app.closeWindow(1);  // Deferred: destroyed after this frame's window loop.
            CHECK(app.windows() == std::vector<WindowId>{0});
        }
        if (id == 0 && frames[0] >= 4) {
            app.quit();
        }
    });
    CHECK(frames[0] == 4);
    CHECK(frames[1] == 2);
    CHECK(handlerCalled);  // T-398: closeWindow() reports through the closed handler too.
    CHECK(app.windows() == std::vector<WindowId>{0});
}

TEST_CASE("T-386: SDL_WINDOWEVENT_FOCUS_LOST during a press sends the window's document PointerCancel") {
    App app(hiddenConfig());
    if (!app.ok()) {
        MESSAGE("skipped: no window could be created (" << app.lastError() << ")");
        return;
    }
    constexpr const char* kCancelDoc = R"X(<xt><head><style>
  #A { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }
</style></head><body id="Root"><div id="A" on:press="emit(down)" on:pointercancel="emit(cancel)"></div>
</body></xt>)X";
    compile::CompileOptions options;
    options.emitMeta = false;
    std::vector<compile::SourceFile> files{{"main.xt", kCancelDoc}};
    compile::CompileResult r = compile::compileDocumentFromStrings(files, "main.xt", options);
    REQUIRE(r.ok());
    const WindowId second = app.createWindow(hiddenConfig());
    EmitLog logs[2];
    for (WindowId id : {WindowId{0}, second}) {
        Window& window = app.window(id);
        Result<DocumentHandle> h = window.context().loadDocument(IrBlob{r.ir.data(), r.ir.size()}, false);
        REQUIRE(h.ok());
        window.setCurrentDocument(h.value());
        window.syncViewport();
        window.context().addListener(&logs[id == 0 ? 0 : 1]);
    }
    auto pushWindowEvent = [](Window& window, Uint8 kind) {
        SDL_Event event{};
        event.type = SDL_WINDOWEVENT;
        event.window.event = kind;
        event.window.windowID = SDL_GetWindowID(window.sdlWindow());
        SDL_PushEvent(&event);
    };
    CHECK(app.runFrame([](float) {}, 0.016f, /*swap=*/false));

    // Focus lost without a press: nothing.
    pushWindowEvent(app.window(second), SDL_WINDOWEVENT_FOCUS_LOST);
    CHECK(app.runFrame([](float) {}, 0.016f, /*swap=*/false));
    CHECK(logs[1].names.empty());

    // A press on the second window, then it loses the focus: only its document is cancelled.
    pushMouseButton(app.window(second), SDL_MOUSEBUTTONDOWN, 20, 20);
    CHECK(app.runFrame([](float) {}, 0.016f, /*swap=*/false));
    pushWindowEvent(app.window(second), SDL_WINDOWEVENT_FOCUS_LOST);
    CHECK(app.runFrame([](float) {}, 0.016f, /*swap=*/false));
    CHECK(logs[1].names == std::vector<std::string>{"down", "cancel"});
    CHECK(logs[0].names.empty());

    app.window(0).context().removeListener(&logs[0]);
    app.window(second).context().removeListener(&logs[1]);
}

// -- T-398 (23 §23.ad.1): close requests, focus, targetFps, displays, fullscreen -------------------

TEST_CASE("t398: the close-request handler vetoes OS closes, closeWindow() and Window::close()") {
    App app(hiddenConfig());
    if (!app.ok()) {
        MESSAGE("skipped: no window could be created (" << app.lastError() << ")");
        return;
    }
    REQUIRE(app.createWindow(hiddenConfig()) == 1);
    std::vector<WindowId> asked;
    std::vector<WindowId> closed;
    bool allow = false;
    app.setWindowCloseRequestHandler([&](WindowId id) {
        asked.push_back(id);
        return allow;
    });
    app.setWindowClosedHandler([&](WindowId id) { closed.push_back(id); });

    // A synthetic OS CLOSE, vetoed: the window stays, the closed handler is not called.
    pushClose(app.window(1));
    CHECK(app.runFrame([](float) {}, 0.016f, /*swap=*/false));
    CHECK(asked == std::vector<WindowId>{1});
    CHECK(closed.empty());
    CHECK(app.windows() == std::vector<WindowId>{0, 1});
    // closeWindow() and Window::close() ask too.
    app.closeWindow(1);
    app.window(1).close();
    CHECK(asked == std::vector<WindowId>{1, 1, 1});
    CHECK(app.findWindow(1) != nullptr);
    // The main window: vetoed, run goes on (OS CLOSE, closeWindow(0), Window::close()).
    pushClose(app.mainWindow());
    CHECK(app.runFrame([](float) {}, 0.016f, /*swap=*/false));
    app.closeWindow(0);
    app.mainWindow().close();
    CHECK(app.runFrame([](float) {}, 0.016f, /*swap=*/false));
    CHECK(asked == std::vector<WindowId>{1, 1, 1, 0, 0, 0});
    CHECK(closed.empty());

    // Allowed: request, then closed handler, then destroyed.
    allow = true;
    pushClose(app.window(1));
    CHECK(app.runFrame([](float) {}, 0.016f, /*swap=*/false));
    CHECK(closed == std::vector<WindowId>{1});
    CHECK(app.windows() == std::vector<WindowId>{0});
    app.closeWindow(0);
    CHECK_FALSE(app.runFrame([](float) {}, 0.016f, /*swap=*/false));
}

TEST_CASE("t398: focusedWindow, raise, setTargetFps result, displayName, display handler, fullscreen") {
    WindowConfig vsyncMain = hiddenConfig();
    vsyncMain.vsync = true;
    App app(vsyncMain);
    if (!app.ok()) {
        MESSAGE("skipped: no window could be created (" << app.lastError() << ")");
        return;
    }
    // setTargetFps is refused while the main window has vsync on.
    CHECK_FALSE(app.setTargetFps(60));
    CHECK_FALSE(app.targetFpsActive());
    CHECK(app.setTargetFps(0));

    const WindowId second = app.createWindow(hiddenConfig());
    REQUIRE(second == 1);
    auto pushWindowEvent = [](Window& window, Uint8 kind) {
        SDL_Event event{};
        event.type = SDL_WINDOWEVENT;
        event.window.event = kind;
        event.window.windowID = SDL_GetWindowID(window.sdlWindow());
        SDL_PushEvent(&event);
    };
    pushWindowEvent(app.window(1), SDL_WINDOWEVENT_FOCUS_GAINED);
    CHECK(app.runFrame([](float) {}, 0.016f, /*swap=*/false));
    CHECK(app.focusedWindow() == 1);
    CHECK(app.window(1).hasInputFocus());
    CHECK_FALSE(app.mainWindow().hasInputFocus());
    pushWindowEvent(app.window(1), SDL_WINDOWEVENT_FOCUS_LOST);
    pushWindowEvent(app.mainWindow(), SDL_WINDOWEVENT_FOCUS_GAINED);
    CHECK(app.runFrame([](float) {}, 0.016f, /*swap=*/false));
    CHECK(app.focusedWindow() == 0);
    app.window(1).raise();  // SDL_RaiseWindow on a hidden window: must simply not fail.

    CHECK_FALSE(app.displayName(0).empty());
    CHECK(app.displayName(999).empty());
    int displayEvents = 0;
    app.setDisplayChangedHandler([&] { ++displayEvents; });
    SDL_Event display{};
    display.type = SDL_DISPLAYEVENT;
    display.display.event = SDL_DISPLAYEVENT_CONNECTED;
    SDL_PushEvent(&display);
    CHECK(app.runFrame([](float) {}, 0.016f, /*swap=*/false));
    CHECK(displayEvents == 1);

    // Desktop fullscreen at creation and switched at run time.
    WindowConfig desktop = hiddenConfig();
    desktop.fullscreen = FullscreenMode::Desktop;
    const WindowId third = app.createWindow(desktop);
    REQUIRE(third == 2);
    CHECK((SDL_GetWindowFlags(app.window(2).sdlWindow()) & SDL_WINDOW_FULLSCREEN_DESKTOP) ==
          SDL_WINDOW_FULLSCREEN_DESKTOP);
    CHECK(app.window(2).fullscreen() == FullscreenMode::Desktop);
    CHECK(app.window(2).setFullscreen(FullscreenMode::Off));
    CHECK((SDL_GetWindowFlags(app.window(2).sdlWindow()) & SDL_WINDOW_FULLSCREEN) == 0);
    CHECK(app.window(1).setFullscreen(FullscreenMode::Desktop));
    CHECK(app.window(1).fullscreen() == FullscreenMode::Desktop);
    CHECK(app.window(1).setFullscreen(FullscreenMode::Off));

    // The bool form still compiles and means Exclusive.
    WindowConfig legacy;
    legacy.fullscreen = true;
    CHECK(static_cast<FullscreenMode>(legacy.fullscreen) == FullscreenMode::Exclusive);
    CHECK(static_cast<bool>(legacy.fullscreen));
    legacy.fullscreen = false;
    CHECK_FALSE(static_cast<bool>(legacy.fullscreen));
}

// -- T-387 (13 §13.9): App::modifierState / openUrl -------------------------------------------------

namespace xt::shell {
uint8_t sdlModifiers(uint16_t sdlMod);  // shell/src/sdl_input.cpp (no header, like translateSdlEvent).
}  // namespace xt::shell

TEST_CASE("T-387: SDL modifier bits map to InputEvent::modifiers bits") {
    static_assert(std::is_same_v<decltype(std::declval<const App&>().modifierState()), uint8_t>);
    static_assert(std::is_same_v<decltype(std::declval<App&>().openUrl(std::string_view{})), bool>);
    CHECK(sdlModifiers(KMOD_NONE) == 0);
    CHECK(sdlModifiers(KMOD_LSHIFT) == kShift);
    CHECK(sdlModifiers(KMOD_RSHIFT) == kShift);
    CHECK(sdlModifiers(KMOD_LCTRL) == kCtrl);
    CHECK(sdlModifiers(KMOD_RCTRL) == kCtrl);
    CHECK(sdlModifiers(KMOD_LALT) == kAlt);
    CHECK(sdlModifiers(KMOD_RGUI) == kSuper);
    CHECK(sdlModifiers(static_cast<uint16_t>(KMOD_LSHIFT | KMOD_RALT | KMOD_LGUI)) ==
          (kShift | kAlt | kSuper));
    // Lock keys are not modifiers.
    CHECK(sdlModifiers(static_cast<uint16_t>(KMOD_NUM | KMOD_CAPS)) == 0);
}

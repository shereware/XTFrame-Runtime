#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "doctest.h"
#include "golden_common.h"
#include "xt/command_list.h"
#include "xt/context.h"
#include "xt/sw/sw_renderer.h"
#include "xt_platform_sdl2.h"
#include "xt_renderer_sw.h"
#ifdef XT_TEST_GL
#include "xt_renderer_gl3.h"
#endif
#ifdef XT_TEST_WGPU
#include "xt_renderer_wgpu.h"
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "xt_platform_win32.h"
#endif

/// T-439 item 8: Code/Backends' platform and renderer parts. GPU cases compare with xt_sw at the
/// 15 §15.3 tolerance; without a WebGPU adapter the wgpu cases are skipped (MESSAGE), without a GL
/// context the gl case too unless XT_REQUIRE_GL=1 (set by Tools/Scripts/dev_check.sh).

using namespace xt;
using namespace xt::backend;

namespace {

bool envIs(const char* name, const char* value) {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    if (_dupenv_s(&v, &len, name) != 0 || v == nullptr) return false;
    const bool on = std::strcmp(v, value) == 0;
    std::free(v);
    return on;
#else
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, value) == 0;
#endif
}

CommandList makeList(int w, int h) {
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    return cl;
}

void addPolygonFill(CommandList& cl, std::initializer_list<Vec2> pts, Color color) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    bool first = true;
    for (Vec2 p : pts) {
        cl.verbs.push_back(first ? PathVerb::Move : PathVerb::Line);
        cl.points.push_back(p.x);
        cl.points.push_back(p.y);
        first = false;
    }
    cl.verbs.push_back(PathVerb::Close);
    ref.verbCount = static_cast<uint32_t>(cl.verbs.size()) - ref.verbOffset;
    ref.pointCount = static_cast<uint32_t>(cl.points.size()) - ref.pointOffset;
    FillCmd f{};
    f.path = ref;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(cl.fills.size() - 1)});
}

/// Two rectangles and an antialiased triangle on a `w` x `h` canvas.
CommandList scene(int w, int h, Color accent = Color{0.9f, 0.2f, 0.1f, 1}) {
    CommandList cl = makeList(w, h);
    addPolygonFill(cl, {Vec2{4, 4}, Vec2{40, 4}, Vec2{40, 30}, Vec2{4, 30}}, Color{0.1f, 0.4f, 0.9f, 1});
    addPolygonFill(cl, {Vec2{20, 20}, Vec2{60, 20}, Vec2{60, 50}, Vec2{20, 50}},
                   Color{0.2f, 0.8f, 0.3f, 0.6f});
    addPolygonFill(cl, {Vec2{50, 8}, Vec2{90, 60}, Vec2{30, 70}}, accent);
    return cl;
}

/// xt_sw into a premultiplied buffer cleared to `clear` (what every renderer here produces).
std::vector<uint8_t> renderSw(const CommandList& cl, int w, int h, Color clear) {
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    sw::SwRenderer renderer;
    sw::SwTarget target{buf.data(), w, h, w * 4};
    target.premultiplied = true;
    renderer.setTarget(target);
    renderer.clear(clear);
    renderer.render(cl, nullptr, nullptr, nullptr);
    return buf;
}

bool matches(const std::vector<uint8_t>& gpu, const std::vector<uint8_t>& sw, int w, int h,
             const char* what) {
    const golden::GoldenResult r = golden::compareRgba(gpu.data(), sw.data(), w, h, 16, 0.03f);
    std::printf("%s vs sw: %u px differ, max channel diff %u\n", what, r.diffPixels, r.maxChannelDiff);
    return r.pass;
}

/// PlatformSdl2::create, retried on SDL's offscreen video driver when there is no display.
bool createSdl(PlatformSdl2& platform, Sdl2Options options) {
    options.hidden = true;
    if (platform.create(options)) return true;
    if (options.graphics != Sdl2Graphics::None) return false;  // offscreen has no GL / surface
    SDL_setenv("SDL_VIDEODRIVER", "offscreen", 1);
    const bool ok = platform.create(options);
    SDL_setenv("SDL_VIDEODRIVER", "", 1);
    return ok;
}

SDL_Event sdlEvent(Uint32 type) {
    SDL_Event e;
    std::memset(&e, 0, sizeof(e));
    e.type = type;
    return e;
}

}  // namespace

TEST_CASE("sdl2: mouse, wheel, keys and text translate like the shell") {
    InputEvent out;
    SDL_Event e = sdlEvent(SDL_MOUSEMOTION);
    e.motion.x = 10;
    e.motion.y = 20;
    REQUIRE(PlatformSdl2::translateEvent(e, out));
    CHECK(out.kind == InputEvent::PointerMove);
    CHECK(out.pos.x == 10.0f);
    CHECK(out.pos.y == 20.0f);

    e = sdlEvent(SDL_MOUSEBUTTONDOWN);
    e.button.button = SDL_BUTTON_RIGHT;
    e.button.clicks = 2;
    REQUIRE(PlatformSdl2::translateEvent(e, out));
    CHECK(out.kind == InputEvent::PointerDown);
    CHECK(out.button == PointerButton::Right);
    CHECK(out.clicks == 2);

    e = sdlEvent(SDL_MOUSEWHEEL);
    e.wheel.preciseY = 1.0f;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    REQUIRE(PlatformSdl2::translateEvent(e, out));
    CHECK(out.wheelDelta.y == -1.0f);  // wheel up = scroll up = negative y (08 §8.1)
    e.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
    REQUIRE(PlatformSdl2::translateEvent(e, out));
    CHECK(out.wheelDelta.y == 1.0f);

    e = sdlEvent(SDL_KEYDOWN);
    e.key.keysym.scancode = SDL_SCANCODE_F3;
    e.key.keysym.mod = KMOD_LCTRL;
    e.key.repeat = 1;
    REQUIRE(PlatformSdl2::translateEvent(e, out));
    CHECK(out.key == KeyCode::F3);
    CHECK(out.modifiers == static_cast<uint8_t>(kCtrl));
    CHECK(out.repeat == 1);
    e.key.keysym.scancode = SDL_SCANCODE_KP_ENTER;
    REQUIRE(PlatformSdl2::translateEvent(e, out));
    CHECK(out.key == KeyCode::NumpadEnter);
    e.key.keysym.scancode = SDL_SCANCODE_A;
    REQUIRE(PlatformSdl2::translateEvent(e, out));
    CHECK(out.key == KeyCode::KeyA);

    // Whole codepoints, at most 7 bytes per event.
    e = sdlEvent(SDL_TEXTINPUT);
    const char* text = "h\xC3\xA9llo w\xC3\xB6rld \xE4\xB8\xAD";
    std::memcpy(e.text.text, text, std::strlen(text) + 1);
    InputEvent chunks[8];
    const size_t n = PlatformSdl2::translateTextInput(e, chunks, 8);
    REQUIRE(n >= 2);
    std::string joined;
    for (size_t i = 0; i < n; ++i) {
        CHECK(chunks[i].kind == InputEvent::TextInput);
        CHECK(chunks[i].utf8Len <= 7);
        joined.append(chunks[i].utf8, chunks[i].utf8Len);
    }
    CHECK(joined == text);
}

TEST_CASE("sdl2: a hidden window pumps into a Context and SDL_QUIT ends it") {
    PlatformSdl2 platform;
    Sdl2Options options;
    options.width = 64;
    options.height = 48;
    REQUIRE_MESSAGE(createSdl(platform, options), platform.lastError());
    int w = 0, h = 0;
    platform.logicalSize(w, h);
    CHECK(w == 64);
    CHECK(h == 48);
    CHECK(platform.devicePixelRatio() > 0.0f);
    Context ctx(ContextConfig{});
    CHECK(platform.pumpEvents(ctx));
    SDL_Event quit = sdlEvent(SDL_QUIT);
    SDL_PushEvent(&quit);
    CHECK_FALSE(platform.pumpEvents(ctx));
    const double t0 = platform.seconds();
    CHECK(t0 >= 0.0);
    platform.destroy();
}

TEST_CASE("sw renderer: xt_sw's pixels; an empty damage list keeps the frame") {
    const int w = 96, h = 80;
    const Color clear{1, 1, 1, 1};
    RendererSw renderer;
    renderer.resize(w, h);
    renderer.setClearColor(clear);
    const CommandList cl = scene(w, h);
    renderer.render(cl, nullptr, nullptr);
    CHECK(renderer.takeDirty());
    const std::vector<uint8_t> expected = renderSw(cl, w, h, clear);
    REQUIRE(renderer.stride() == w * 4);
    CHECK(std::memcmp(renderer.pixels(), expected.data(), expected.size()) == 0);

    DamageList none;
    none.full = false;
    renderer.render(scene(w, h, Color{0, 0, 0, 1}), nullptr, nullptr, nullptr, nullptr, &none);
    CHECK_FALSE(renderer.takeDirty());
    CHECK(std::memcmp(renderer.pixels(), expected.data(), expected.size()) == 0);
}

#ifdef XT_TEST_WGPU
TEST_CASE("wgpu renderer: offscreen frames match xt_sw (1x and 4x)") {
    const int w = 96, h = 80;
    const Color clear{1, 1, 1, 1};
    for (uint32_t samples : {1u, 4u}) {
        RendererWgpu renderer;
        if (!renderer.createOffscreen(w, h, samples)) {
            MESSAGE("SKIP: " << renderer.lastError());
            return;
        }
        renderer.setClearColor(clear);
        const CommandList cl = scene(w, h);
        renderer.beginFrame();
        renderer.render(cl, nullptr, nullptr);
        renderer.present();
        std::vector<uint8_t> px;
        int rw = 0, rh = 0;
        REQUIRE_MESSAGE(renderer.readPixels(px, rw, rh), renderer.lastError());
        REQUIRE(rw == w);
        REQUIRE(rh == h);
        CHECK(matches(px, renderSw(cl, w, h, clear), w, h, samples == 4 ? "wgpu 4x" : "wgpu 1x"));
        renderer.resize(64, 40);
        renderer.beginFrame();
        renderer.render(scene(64, 40), nullptr, nullptr);
        REQUIRE(renderer.readPixels(px, rw, rh));
        CHECK(rw == 64);
        CHECK(rh == 40);
    }
}

TEST_CASE("wgpu renderer: an SDL window's surface presents and resizes") {
    PlatformSdl2 platform;
    Sdl2Options options;
    options.width = 96;
    options.height = 80;
    options.graphics = Sdl2Graphics::Surface;
    if (!createSdl(platform, options)) {
        MESSAGE("SKIP: no SDL window for a surface: " << platform.lastError());
        return;
    }
    const Sdl2NativeWindow native = platform.nativeWindow();
    if (native.kind == Sdl2NativeWindow::Kind::None) {
        MESSAGE("SKIP: the SDL video driver gives no native window");
        return;
    }
    WgpuSurfaceSource source;
    source.kind = native.kind == Sdl2NativeWindow::Kind::Win32     ? WgpuSurfaceSource::Kind::Win32
                  : native.kind == Sdl2NativeWindow::Kind::X11     ? WgpuSurfaceSource::Kind::X11
                  : native.kind == Sdl2NativeWindow::Kind::Wayland ? WgpuSurfaceSource::Kind::Wayland
                                                                   : WgpuSurfaceSource::Kind::Metal;
    source.display = native.display;
    source.window = native.window;
    source.x11Window = native.x11Window;
    int w = 0, h = 0;
    platform.pixelSize(w, h);
    RendererWgpu renderer;
    if (!renderer.create(source, w, h, 4, true)) {
        MESSAGE("SKIP: " << renderer.lastError());
        return;
    }
    renderer.setClearColor(Color{1, 1, 1, 1});
    const CommandList cl = scene(w, h);
    renderer.beginFrame();
    renderer.render(cl, nullptr, nullptr);
    std::vector<uint8_t> px;
    int rw = 0, rh = 0;
    if (renderer.readPixels(px, rw, rh)) {  // only when the surface offers COPY_SRC
        CHECK(rw == w);
        CHECK(matches(px, renderSw(cl, w, h, Color{1, 1, 1, 1}), w, h, "wgpu surface"));
    }
    renderer.present();
    SDL_SetWindowSize(platform.window(), 120, 90);
    Context ctx(ContextConfig{});
    platform.pumpEvents(ctx);
    platform.pixelSize(w, h);
    renderer.resize(w, h);
    renderer.beginFrame();
    renderer.render(scene(w, h), nullptr, nullptr);
    renderer.present();
    CHECK(renderer.width() == w);
    CHECK(std::string(renderer.lastError()).empty());
}
#endif

#ifdef XT_TEST_GL
TEST_CASE("gl3 renderer: the FBO of a hidden GL window matches xt_sw") {
    PlatformSdl2 platform;
    Sdl2Options options;
    options.width = 96;
    options.height = 80;
    options.graphics = Sdl2Graphics::OpenGL;
    RendererGl3 renderer;
    const bool ok =
        createSdl(platform, options) && renderer.create(&PlatformSdl2::glGetProcAddress, 96, 80, 4);
    if (!ok) {
        const std::string why = *renderer.lastError() != '\0' ? renderer.lastError() : platform.lastError();
        if (envIs("XT_REQUIRE_GL", "1")) {
            FAIL("no GL 3.3 context: " << why);
        }
        MESSAGE("SKIP: no GL 3.3 context: " << why);
        return;
    }
    const Color clear{1, 1, 1, 1};
    renderer.setClearColor(clear);
    const CommandList cl = scene(96, 80);
    renderer.beginFrame();
    renderer.render(cl, nullptr, nullptr);
    std::vector<uint8_t> px;
    int rw = 0, rh = 0;
    REQUIRE(renderer.readPixels(px, rw, rh));
    REQUIRE(rw == 96);
    REQUIRE(rh == 80);
    CHECK(matches(px, renderSw(cl, 96, 80, clear), 96, 80, "gl3"));
    int pw = 0, ph = 0;
    platform.pixelSize(pw, ph);
    renderer.present(pw, ph);
    platform.glSwap();
    renderer.destroy();
}
#endif

#ifdef _WIN32
TEST_CASE("win32: messages translate with the DPI scale") {
    InputEvent out;
    REQUIRE(PlatformWin32::translateMessage(WM_LBUTTONDOWN, 0, MAKELPARAM(200, 100), 2.0f, out));
    CHECK(out.kind == InputEvent::PointerDown);
    CHECK(out.button == PointerButton::Left);
    CHECK(out.pos.x == 100.0f);
    CHECK(out.pos.y == 50.0f);
    CHECK(out.clicks == 1);
    REQUIRE(PlatformWin32::translateMessage(WM_RBUTTONDBLCLK, 0, MAKELPARAM(4, 6), 1.0f, out));
    CHECK(out.button == PointerButton::Right);
    CHECK(out.clicks == 2);
    REQUIRE(PlatformWin32::translateMessage(WM_MBUTTONUP, 0, 0, 1.0f, out));
    CHECK(out.kind == InputEvent::PointerUp);
    CHECK(out.button == PointerButton::Middle);
    REQUIRE(PlatformWin32::translateMessage(WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0, 1.0f, out));
    CHECK(out.kind == InputEvent::Wheel);
    CHECK(out.wheelDelta.y == -1.0f);
    REQUIRE(PlatformWin32::translateMessage(WM_KEYDOWN, 'A', 1 << 30, 1.0f, out));
    CHECK(out.key == KeyCode::KeyA);
    CHECK(out.repeat == 1);
    REQUIRE(PlatformWin32::translateMessage(WM_KEYUP, VK_RETURN, 1 << 24, 1.0f, out));
    CHECK(out.kind == InputEvent::KeyUp);
    CHECK(out.key == KeyCode::NumpadEnter);
    CHECK(PlatformWin32::mapVirtualKey(VK_OEM_PLUS, 0) == KeyCode::Equal);
    CHECK(PlatformWin32::mapVirtualKey(VK_CONTROL, 1 << 24) == KeyCode::ControlRight);
    CHECK(PlatformWin32::mapVirtualKey(VK_F12, 0) == KeyCode::F12);
    CHECK_FALSE(PlatformWin32::translateMessage(WM_TIMER, 0, 0, 1.0f, out));
}

TEST_CASE("win32: a hidden window pumps, takes text and closes on WM_CLOSE") {
    PlatformWin32 platform;
    Win32Options options;
    options.width = 64;
    options.height = 48;
    options.hidden = true;
    REQUIRE_MESSAGE(platform.create(options), platform.lastError());
    int pw = 0, ph = 0;
    platform.pixelSize(pw, ph);
    CHECK(pw > 0);
    CHECK(ph > 0);
    Context ctx(ContextConfig{});
    CHECK(platform.pumpEvents(ctx));
    // A surrogate pair arrives as two WM_CHARs.
    SendMessageW(static_cast<HWND>(platform.hwnd()), WM_CHAR, 0xD83D, 0);
    SendMessageW(static_cast<HWND>(platform.hwnd()), WM_CHAR, 0xDE00, 0);
    std::vector<uint8_t> px(static_cast<size_t>(pw) * static_cast<size_t>(ph) * 4, 128);
    CHECK(platform.blitRgba(px.data(), pw, ph, pw * 4));
    PostMessageW(static_cast<HWND>(platform.hwnd()), WM_CLOSE, 0, 0);
    CHECK_FALSE(platform.pumpEvents(ctx));
    platform.destroy();
}

#ifdef XT_TEST_WGPU
TEST_CASE("wgpu renderer: a Win32 HWND's surface presents") {
    PlatformWin32 platform;
    Win32Options options;
    options.width = 96;
    options.height = 80;
    options.hidden = true;
    REQUIRE(platform.create(options));
    WgpuSurfaceSource source;
    source.kind = WgpuSurfaceSource::Kind::Win32;
    source.display = platform.hinstance();
    source.window = platform.hwnd();
    int w = 0, h = 0;
    platform.pixelSize(w, h);
    RendererWgpu renderer;
    if (!renderer.create(source, w, h, 4, true)) {
        MESSAGE("SKIP: " << renderer.lastError());
        return;
    }
    renderer.beginFrame();
    renderer.render(scene(w, h), nullptr, nullptr);
    renderer.present();
    CHECK(std::string(renderer.lastError()).empty());
}
#endif
#endif

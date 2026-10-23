// XTFrame reference backend -- MIT License (see LICENSE). Copy and edit (Code/Backends/README.md).
// Host API: SDL 2.0.18 or newer (built against 2.30.9, Code/SDKs/versions.cmake).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xt/context.h"
#include "xt/input.h"

struct SDL_Window;
union SDL_Event;

/// SDL2 platform (45 §45.7): window, SDL events -> `xt::InputEvent`, clock, DPI, clipboard, IME
/// (SDL's TEXTEDITING / TEXTINPUT and the candidate-window rectangle), and the software blit
/// (the window framebuffer, SDL_GetWindowSurface: no SDL_Renderer, which GL builds compile out).
/// The event translation is copied from Code/Runtime/shell/src/sdl_input.cpp and app.cpp, not
/// linked: this file depends on nothing but SDL2 and the public `xt/...` headers.

namespace xt::backend {

/// What a renderer needs from the window.
enum class Sdl2Graphics {
    None,     ///< Software blit only (blitRgba).
    OpenGL,   ///< SDL_WINDOW_OPENGL + a 3.3 core context with 8-bit stencil.
    Surface,  ///< A native surface for WebGPU / Vulkan / D3D (SDL_WINDOW_METAL on Apple).
};

struct Sdl2Options {
    const char* title = "XTFrame";
    int width = 1280;  ///< logical pixels
    int height = 720;
    bool resizable = true;
    bool hidden = false;
    bool vsync = true;  ///< OpenGL swap interval
    Sdl2Graphics graphics = Sdl2Graphics::None;
};

/// The window's native handles (SDL_GetWindowWMInfo), for a WebGPU / Vulkan surface.
struct Sdl2NativeWindow {
    enum class Kind { None, Win32, X11, Wayland, Metal };
    Kind kind = Kind::None;
    void* display = nullptr;  ///< HINSTANCE, Display*, wl_display*
    void* window = nullptr;   ///< HWND, wl_surface*, CAMetalLayer*
    uint64_t x11Window = 0;   ///< X11 Window
};

class PlatformSdl2 {
public:
    PlatformSdl2() = default;
    ~PlatformSdl2();
    PlatformSdl2(const PlatformSdl2&) = delete;
    PlatformSdl2& operator=(const PlatformSdl2&) = delete;

    /// SDL_Init(VIDEO), the window and (OpenGL) its context. Honours SDL_VIDEODRIVER (e.g.
    /// `offscreen` for tests without a display). False and `lastError()` on failure.
    bool create(const Sdl2Options& options);
    void destroy();

    /// Translates every pending event into `context` (see Backend::pumpEvents). False once the
    /// window was closed (SDL_QUIT or its close button).
    bool pumpEvents(Context& context);
    /// True once after the window's pixel size changed (cleared by the call).
    bool takeResized();

    /// Client size in logical pixels / in pixels.
    void logicalSize(int& w, int& h) const;
    void pixelSize(int& w, int& h) const;
    float devicePixelRatio() const;
    double seconds() const;
    std::string clipboardText() const;
    void setClipboardText(std::string_view utf8);

    /// The handles of the window (Kind::None when SDL cannot give them, e.g. the offscreen driver).
    Sdl2NativeWindow nativeWindow() const;
    /// OpenGL: SDL_GL_GetProcAddress, usable as `xt::gl::GlLoadProc`.
    static void* glGetProcAddress(const char* name);
    void glMakeCurrent();
    void glSwap();
    /// Software presentation: copies `rgba` (premultiplied RGBA8, `w` x `h`, `stride` bytes per
    /// row; drawn opaque) into the window framebuffer, scaled to the window. False when SDL cannot.
    /// Only for a Graphics::None window (a window framebuffer excludes GL and surfaces).
    bool blitRgba(const uint8_t* rgba, int w, int h, int stride);

    SDL_Window* window() const { return m_window; }
    const char* lastError() const { return m_error.c_str(); }

    /// SDL event -> InputEvent (mouse, wheel, keys, LEAVE / FOCUS_LOST), copied from the shell's
    /// sdl_input.cpp. False for events that carry no InputEvent. Positions stay in SDL's logical
    /// window coordinates.
    static bool translateEvent(const SDL_Event& event, InputEvent& out);
    /// SDL_TEXTINPUT -> up to `maxOut` TextInput events of whole UTF-8 codepoints (7 bytes each).
    static size_t translateTextInput(const SDL_Event& event, InputEvent* out, size_t maxOut);
    /// SDL_Keymod -> InputEvent::modifiers.
    static uint8_t modifiers(uint16_t sdlMod);

private:
    void syncViewport(Context& context);
    void syncTextInput(Context& context);

    SDL_Window* m_window = nullptr;
    void* m_glContext = nullptr;  // SDL_GLContext
    void* m_metalView = nullptr;  // SDL_MetalView (Apple, Surface)
    bool m_sdlInit = false;
    bool m_resized = false;
    bool m_viewportSynced = false;
    bool m_composing = false;
    bool m_textInput = false;
    uint64_t m_startCounter = 0;
    std::string m_error;
};

}  // namespace xt::backend

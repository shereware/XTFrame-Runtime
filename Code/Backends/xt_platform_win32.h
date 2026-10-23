// XTFrame reference backend -- MIT License (see LICENSE). Copy and edit (Code/Backends/README.md).
// Host API: Win32 (Windows 10 1607 or newer for per-monitor DPI v2; older systems fall back).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "xt/context.h"
#include "xt/input.h"

/// Win32 platform (45 §45.7): a plain window and message loop, mouse / wheel / keyboard / WM_CHAR
/// -> `xt::InputEvent`, the performance-counter clock, per-monitor DPI, the clipboard and the
/// software blit (StretchDIBits). IME is a hook only: `setImeHook` sees every WM_IME_* message
/// first; without one they go to DefWindowProc (the system draws its own composition window).
/// No Win32 header leaks out of this one: handles are `void*` and messages plain integers.

namespace xt::backend {

struct Win32Options {
    const char* title = "XTFrame";  ///< UTF-8
    int width = 1280;               ///< logical pixels (scaled by the monitor's DPI)
    int height = 720;
    bool resizable = true;
    bool hidden = false;
};

class PlatformWin32 {
public:
    PlatformWin32() = default;
    ~PlatformWin32();
    PlatformWin32(const PlatformWin32&) = delete;
    PlatformWin32& operator=(const PlatformWin32&) = delete;

    bool create(const Win32Options& options);
    void destroy();

    /// PeekMessage loop; input goes to `context` (see Backend::pumpEvents). False once WM_CLOSE or
    /// WM_QUIT arrived.
    bool pumpEvents(Context& context);
    bool takeResized();

    void logicalSize(int& w, int& h) const;
    void pixelSize(int& w, int& h) const;
    float devicePixelRatio() const;
    double seconds() const;
    std::string clipboardText() const;
    void setClipboardText(std::string_view utf8);

    /// Copies `rgba` (premultiplied RGBA8) to the client area (converted to BGRA, StretchDIBits).
    bool blitRgba(const uint8_t* rgba, int w, int h, int stride);

    /// HWND / HINSTANCE.
    void* hwnd() const { return m_hwnd; }
    void* hinstance() const { return m_hinstance; }
    const char* lastError() const { return m_error.c_str(); }

    /// Called first for every WM_IME_* message (`message`, `wParam`, `lParam`); returning true
    /// consumes it. Empty (the default): DefWindowProc.
    void setImeHook(std::function<bool(uint32_t message, uintptr_t wParam, intptr_t lParam)> hook) {
        m_imeHook = std::move(hook);
    }

    /// One window message -> InputEvent (mouse buttons / move / wheel, keys); `dpr` turns client
    /// pixels into logical ones. False for any other message. WM_CHAR is handled by the loop
    /// (surrogate pairs need state).
    static bool translateMessage(uint32_t message, uintptr_t wParam, intptr_t lParam, float dpr,
                                 InputEvent& out);
    /// A Win32 virtual-key code (with the extended-key bit of lParam) -> KeyCode.
    static KeyCode mapVirtualKey(uintptr_t vk, intptr_t lParam);

    /// The window procedure's instance entry point (public for the C callback only).
    intptr_t handleMessage(void* hwnd, uint32_t message, uintptr_t wParam, intptr_t lParam);

private:
    void pushChar(uint32_t unit);
    void syncViewport(Context& context);

    void* m_hwnd = nullptr;
    void* m_hinstance = nullptr;
    Context* m_context = nullptr;  // only during pumpEvents
    bool m_open = false;
    bool m_resized = false;
    bool m_viewportSynced = false;
    bool m_tracking = false;  // TrackMouseEvent armed
    uint32_t m_highSurrogate = 0;
    int64_t m_startCounter = 0;
    int64_t m_frequency = 1;
    std::vector<uint8_t> m_bgra;
    std::function<bool(uint32_t, uintptr_t, intptr_t)> m_imeHook;
    std::string m_error;
};

}  // namespace xt::backend

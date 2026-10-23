// XTFrame reference backend -- MIT License (see LICENSE). Copy and edit (Code/Backends/README.md).
// Host API: Win32 (Windows 10 1607 or newer for per-monitor DPI v2; older systems fall back).
#include "xt_platform_win32.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include <cstring>

namespace xt::backend {

namespace {

constexpr wchar_t kClassName[] = L"XTFrameBackendWindow";

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(const wchar_t* wide) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr, nullptr);
    return out;
}

/// user32 entry points newer than the oldest supported Windows, looked up once.
template <typename Fn>
Fn user32Proc(const char* name) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return user32 != nullptr ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(user32, name)))
                             : nullptr;
}

UINT windowDpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const GetDpiForWindowFn fn = user32Proc<GetDpiForWindowFn>("GetDpiForWindow");
    const UINT dpi = (fn != nullptr && hwnd != nullptr) ? fn(hwnd) : 0;
    return dpi != 0 ? dpi : USER_DEFAULT_SCREEN_DPI;
}

UINT systemDpi() {
    using GetDpiForSystemFn = UINT(WINAPI*)();
    static const GetDpiForSystemFn fn = user32Proc<GetDpiForSystemFn>("GetDpiForSystem");
    return fn != nullptr ? fn() : USER_DEFAULT_SCREEN_DPI;
}

uint8_t currentModifiers() {
    uint8_t mods = 0;
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) mods |= static_cast<uint8_t>(kShift);
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) mods |= static_cast<uint8_t>(kCtrl);
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) mods |= static_cast<uint8_t>(kAlt);
    if (((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0) mods |= static_cast<uint8_t>(kSuper);
    return mods;
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<PlatformWin32*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr && message != WM_NCCREATE) {
        return static_cast<LRESULT>(self->handleMessage(hwnd, message, wParam, lParam));
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

PlatformWin32::~PlatformWin32() { destroy(); }

bool PlatformWin32::create(const Win32Options& options) {
    destroy();
    // Per-monitor DPI v2 when the system has it (a manifest may already have set awareness).
    using SetDpiCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (const SetDpiCtxFn setCtx = user32Proc<SetDpiCtxFn>("SetProcessDpiAwarenessContext")) {
        setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    if (GetClassInfoExW(instance, kClassName, &wc) == FALSE) {
        wc = WNDCLASSEXW{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC | CS_DBLCLKS;
        wc.lpfnWndProc = windowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
        wc.lpszClassName = kClassName;
        if (RegisterClassExW(&wc) == 0) {
            m_error = "RegisterClassExW failed";
            return false;
        }
    }
    const DWORD style =
        options.resizable ? WS_OVERLAPPEDWINDOW : (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX));
    const UINT dpi = systemDpi();
    RECT rect{0, 0, MulDiv(options.width, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI),
              MulDiv(options.height, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI)};
    AdjustWindowRectEx(&rect, style, FALSE, 0);
    const std::wstring title = widen(options.title != nullptr ? options.title : "");
    HWND hwnd =
        CreateWindowExW(0, kClassName, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                        rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, this);
    if (hwnd == nullptr) {
        m_error = "CreateWindowExW failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    m_hwnd = hwnd;
    m_hinstance = instance;
    if (!options.hidden) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    m_startCounter = counter.QuadPart;
    m_frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;
    m_open = true;
    m_viewportSynced = false;
    return true;
}

void PlatformWin32::destroy() {
    if (m_hwnd != nullptr) {
        SetWindowLongPtrW(static_cast<HWND>(m_hwnd), GWLP_USERDATA, 0);
        DestroyWindow(static_cast<HWND>(m_hwnd));
    }
    m_hwnd = nullptr;
    m_open = false;
    m_context = nullptr;
}

KeyCode PlatformWin32::mapVirtualKey(uintptr_t vk, intptr_t lParam) {
    const bool extended = (static_cast<uintptr_t>(lParam) & (1u << 24)) != 0;
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<KeyCode>(static_cast<int>(KeyCode::KeyA) + static_cast<int>(vk - 'A'));
    if (vk >= '1' && vk <= '9')
        return static_cast<KeyCode>(static_cast<int>(KeyCode::Key1) + static_cast<int>(vk - '1'));
    if (vk >= VK_F1 && vk <= VK_F12)
        return static_cast<KeyCode>(static_cast<int>(KeyCode::F1) + static_cast<int>(vk - VK_F1));
    if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9) {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::Numpad1) + static_cast<int>(vk - VK_NUMPAD1));
    }
    switch (vk) {
        case '0':
            return KeyCode::Key0;
        case VK_RETURN:
            return extended ? KeyCode::NumpadEnter : KeyCode::Enter;
        case VK_ESCAPE:
            return KeyCode::Escape;
        case VK_BACK:
            return KeyCode::Backspace;
        case VK_TAB:
            return KeyCode::Tab;
        case VK_SPACE:
            return KeyCode::Space;
        case VK_DELETE:
            return KeyCode::Delete;
        case VK_HOME:
            return KeyCode::Home;
        case VK_END:
            return KeyCode::End;
        case VK_RIGHT:
            return KeyCode::Right;
        case VK_LEFT:
            return KeyCode::Left;
        case VK_DOWN:
            return KeyCode::Down;
        case VK_UP:
            return KeyCode::Up;
        case VK_PRIOR:
            return KeyCode::PageUp;
        case VK_NEXT:
            return KeyCode::PageDown;
        case VK_INSERT:
            return KeyCode::Insert;
        case VK_SHIFT: {
            const UINT scan = (static_cast<UINT>(lParam) >> 16) & 0xFFu;
            return MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? KeyCode::ShiftRight
                                                                         : KeyCode::ShiftLeft;
        }
        case VK_LSHIFT:
            return KeyCode::ShiftLeft;
        case VK_RSHIFT:
            return KeyCode::ShiftRight;
        case VK_CONTROL:
            return extended ? KeyCode::ControlRight : KeyCode::ControlLeft;
        case VK_MENU:
            return extended ? KeyCode::AltRight : KeyCode::AltLeft;
        case VK_LWIN:
            return KeyCode::MetaLeft;
        case VK_RWIN:
            return KeyCode::MetaRight;
        case VK_CAPITAL:
            return KeyCode::CapsLock;
        case VK_OEM_MINUS:
            return KeyCode::Minus;
        case VK_OEM_PLUS:
            return KeyCode::Equal;
        case VK_OEM_4:
            return KeyCode::BracketLeft;
        case VK_OEM_6:
            return KeyCode::BracketRight;
        case VK_OEM_5:
        case VK_OEM_102:
            return KeyCode::Backslash;
        case VK_OEM_1:
            return KeyCode::Semicolon;
        case VK_OEM_7:
            return KeyCode::Quote;
        case VK_OEM_3:
            return KeyCode::Backquote;
        case VK_OEM_COMMA:
            return KeyCode::Comma;
        case VK_OEM_PERIOD:
            return KeyCode::Period;
        case VK_OEM_2:
            return KeyCode::Slash;
        case VK_NUMPAD0:
            return KeyCode::Numpad0;
        case VK_ADD:
            return KeyCode::NumpadAdd;
        case VK_SUBTRACT:
            return KeyCode::NumpadSubtract;
        case VK_MULTIPLY:
            return KeyCode::NumpadMultiply;
        case VK_DIVIDE:
            return KeyCode::NumpadDivide;
        case VK_DECIMAL:
            return KeyCode::NumpadDecimal;
        default:
            return KeyCode::Unknown;
    }
}

bool PlatformWin32::translateMessage(uint32_t message, uintptr_t wParam, intptr_t lParam, float dpr,
                                     InputEvent& out) {
    const float scale = dpr > 0.0f ? 1.0f / dpr : 1.0f;
    const Vec2 pos{static_cast<float>(GET_X_LPARAM(lParam)) * scale,
                   static_cast<float>(GET_Y_LPARAM(lParam)) * scale};
    switch (message) {
        case WM_MOUSEMOVE:
            out = InputEvent{};
            out.kind = InputEvent::PointerMove;
            out.pos = pos;
            out.modifiers = currentModifiers();
            return true;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDBLCLK:
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP: {
            out = InputEvent{};
            const bool up = message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP;
            const bool dbl =
                message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDBLCLK || message == WM_MBUTTONDBLCLK;
            out.kind = up ? InputEvent::PointerUp : InputEvent::PointerDown;
            out.pos = pos;
            if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP || message == WM_RBUTTONDBLCLK) {
                out.button = PointerButton::Right;
            } else if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP || message == WM_MBUTTONDBLCLK) {
                out.button = PointerButton::Middle;
            } else {
                out.button = PointerButton::Left;
            }
            out.clicks = dbl ? 2 : 1;
            out.modifiers = currentModifiers();
            return true;
        }
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            // lParam is in screen coordinates here; handleMessage replaces `pos` with the client one.
            out = InputEvent{};
            out.kind = InputEvent::Wheel;
            out.pos = pos;
            const float notches =
                static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA);
            // WM_MOUSEWHEEL > 0 = away from the user (up); positive y = scroll down (08 §8.1).
            out.wheelDelta = message == WM_MOUSEWHEEL ? Vec2{0.0f, -notches} : Vec2{notches, 0.0f};
            out.modifiers = currentModifiers();
            return true;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP:
            out = InputEvent{};
            out.kind =
                (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) ? InputEvent::KeyDown : InputEvent::KeyUp;
            out.key = mapVirtualKey(wParam, lParam);
            out.modifiers = currentModifiers();
            out.repeat =
                (out.kind == InputEvent::KeyDown && (static_cast<uintptr_t>(lParam) & (1u << 30)) != 0) ? 1
                                                                                                        : 0;
            return true;
        default:
            return false;
    }
}

void PlatformWin32::pushChar(uint32_t unit) {
    // UTF-16 code unit(s) -> one TextInput event of one codepoint.
    uint32_t cp = unit;
    if (unit >= 0xD800 && unit <= 0xDBFF) {
        m_highSurrogate = unit;
        return;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
        if (m_highSurrogate == 0) return;
        cp = 0x10000u + ((m_highSurrogate - 0xD800u) << 10) + (unit - 0xDC00u);
    }
    m_highSurrogate = 0;
    if (cp < 0x20 || cp == 0x7F || m_context == nullptr) return;  // control characters are keys, not text
    InputEvent ev{};
    ev.kind = InputEvent::TextInput;
    uint8_t n = 0;
    if (cp < 0x80) {
        ev.utf8[n++] = static_cast<char>(cp);
    } else if (cp < 0x800) {
        ev.utf8[n++] = static_cast<char>(0xC0 | (cp >> 6));
        ev.utf8[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        ev.utf8[n++] = static_cast<char>(0xE0 | (cp >> 12));
        ev.utf8[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        ev.utf8[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        ev.utf8[n++] = static_cast<char>(0xF0 | (cp >> 18));
        ev.utf8[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        ev.utf8[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        ev.utf8[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    }
    ev.utf8[n] = '\0';
    ev.utf8Len = n;
    m_context->pushInput(ev);
}

intptr_t PlatformWin32::handleMessage(void* window, uint32_t message, uintptr_t wParam, intptr_t lParam) {
    if (m_hwnd == nullptr) m_hwnd = window;  // messages sent from inside CreateWindowExW
    HWND hwnd = static_cast<HWND>(window);
    switch (message) {
        case WM_CLOSE:
            m_open = false;
            return 0;
        case WM_SIZE:
            m_resized = true;
            if (m_context != nullptr) syncViewport(*m_context);
            return 0;
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            m_resized = true;
            if (m_context != nullptr) syncViewport(*m_context);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;  // the renderer covers the client area
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            return 0;
        case WM_CHAR:
            pushChar(static_cast<uint32_t>(wParam));
            return 0;
        case WM_MOUSELEAVE:
            m_tracking = false;
            if (m_context != nullptr) {
                InputEvent ev{};
                ev.kind = InputEvent::PointerLeave;
                m_context->pushInput(ev);
            }
            return 0;
        case WM_KILLFOCUS:
            if (m_context != nullptr) {
                InputEvent ev{};
                ev.kind = InputEvent::PointerCancel;  // 08 §8.11
                m_context->pushInput(ev);
            }
            break;
        case WM_IME_STARTCOMPOSITION:
        case WM_IME_COMPOSITION:
        case WM_IME_ENDCOMPOSITION:
        case WM_IME_NOTIFY:
        case WM_IME_SETCONTEXT:
            if (m_imeHook && m_imeHook(message, wParam, lParam)) return 0;
            break;
        default:
            break;
    }
    InputEvent ev;
    if (m_context != nullptr && translateMessage(message, wParam, lParam, devicePixelRatio(), ev)) {
        if (message == WM_MOUSEMOVE && !m_tracking) {
            TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
            m_tracking = TrackMouseEvent(&tme) != FALSE;
        }
        if (ev.kind == InputEvent::PointerDown) SetCapture(hwnd);
        if (ev.kind == InputEvent::PointerUp) ReleaseCapture();
        if (ev.kind == InputEvent::Wheel) {
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &p);
            const float dpr = devicePixelRatio();
            ev.pos = Vec2{static_cast<float>(p.x) / dpr, static_cast<float>(p.y) / dpr};
        }
        m_context->pushInput(ev);
        // Alt+F4 and the system menu still need DefWindowProc.
        if (message != WM_SYSKEYDOWN && message != WM_SYSKEYUP) return 0;
    }
    return static_cast<intptr_t>(DefWindowProcW(hwnd, message, wParam, lParam));
}

void PlatformWin32::syncViewport(Context& context) {
    int w = 0, h = 0;
    logicalSize(w, h);
    context.setViewport(Rect{0, 0, static_cast<float>(w), static_cast<float>(h)}, devicePixelRatio());
    m_viewportSynced = true;
}

bool PlatformWin32::pumpEvents(Context& context) {
    if (m_hwnd == nullptr) return false;
    m_context = &context;
    if (!m_viewportSynced) syncViewport(context);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        if (msg.message == WM_QUIT) {
            m_open = false;
            break;
        }
        TranslateMessage(&msg);  // WM_KEYDOWN -> WM_CHAR
        DispatchMessageW(&msg);
    }
    m_context = nullptr;
    return m_open;
}

bool PlatformWin32::takeResized() {
    const bool r = m_resized;
    m_resized = false;
    return r;
}

void PlatformWin32::pixelSize(int& w, int& h) const {
    w = h = 0;
    RECT rc;
    if (m_hwnd != nullptr && GetClientRect(static_cast<HWND>(m_hwnd), &rc) != FALSE) {
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
    }
}

void PlatformWin32::logicalSize(int& w, int& h) const {
    pixelSize(w, h);
    const float dpr = devicePixelRatio();
    w = static_cast<int>(static_cast<float>(w) / dpr + 0.5f);
    h = static_cast<int>(static_cast<float>(h) / dpr + 0.5f);
}

float PlatformWin32::devicePixelRatio() const {
    return static_cast<float>(windowDpi(static_cast<HWND>(m_hwnd))) /
           static_cast<float>(USER_DEFAULT_SCREEN_DPI);
}

double PlatformWin32::seconds() const {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart - m_startCounter) / static_cast<double>(m_frequency);
}

std::string PlatformWin32::clipboardText() const {
    std::string out;
    if (OpenClipboard(static_cast<HWND>(m_hwnd)) == FALSE) return out;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* wide = static_cast<const wchar_t*>(GlobalLock(data))) {
            out = narrow(wide);
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return out;
}

void PlatformWin32::setClipboardText(std::string_view utf8) {
    const std::wstring wide = widen(utf8);
    if (OpenClipboard(static_cast<HWND>(m_hwnd)) == FALSE) return;
    EmptyClipboard();
    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* dst = GlobalLock(mem)) {
            std::memcpy(dst, wide.c_str(), bytes);
            GlobalUnlock(mem);
            if (SetClipboardData(CF_UNICODETEXT, mem) == nullptr) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

bool PlatformWin32::blitRgba(const uint8_t* rgba, int w, int h, int stride) {
    if (m_hwnd == nullptr || rgba == nullptr || w <= 0 || h <= 0) return false;
    m_bgra.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = rgba + static_cast<size_t>(y) * static_cast<size_t>(stride);
        uint8_t* dst = m_bgra.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u;
        for (int x = 0; x < w; ++x, src += 4, dst += 4) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = 255;
        }
    }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    int cw = 0, ch = 0;
    pixelSize(cw, ch);
    HWND hwnd = static_cast<HWND>(m_hwnd);
    HDC dc = GetDC(hwnd);
    const int lines =
        StretchDIBits(dc, 0, 0, cw, ch, 0, 0, w, h, m_bgra.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(hwnd, dc);
    return lines != 0;
}

}  // namespace xt::backend

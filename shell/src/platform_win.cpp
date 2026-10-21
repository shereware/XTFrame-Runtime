// T-376 (docs/design/23-host-integration.md §23.ab): the Windows side of a borderless App window:
// the resizable style (edge resizing, maximize, Snap) and the drop shadow, which a WS_POPUP-style
// (SDL_WINDOW_BORDERLESS) window lacks; extending the DWM frame one pixel into the client area
// gives the shadow back.
// T-393 (docs/design/08-events.md §8.14): the OLE IDropTarget each App window registers (files
// dragged in from Explorer), and OleInitialize / OleUninitialize around the App.

#include <SDL.h>
#include <SDL_syswm.h>  // Includes <windows.h> on Windows (dwmapi.h needs it first).
#include <commctrl.h>
#include <dwmapi.h>
#include <ole2.h>
#include <shellapi.h>

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/input.h"

namespace xt::shell {

// T-393: the callback a window's drop target reports to (window.cpp declares the same type).
using ExternalDragFn = std::function<bool(InputEvent::Kind, Vec2, std::span<const std::string_view>)>;

namespace {
bool g_oleInitialized = false;  // T-393: initPlatformFileDrop() succeeded.
}  // namespace

void prepareBorderlessWindow() {
    // SDL 2 gives a borderless window WS_POPUP | WS_MINIMIZEBOX only: without WS_THICKFRAME and
    // WS_MAXIMIZEBOX, Windows neither resizes it from the hit-test's HT* edges nor maximizes or
    // snaps it. With this hint SDL adds both and still sizes the non-client area to 0
    // (WM_NCCALCSIZE), so no frame shows.
    SDL_SetHint("SDL_BORDERLESS_RESIZABLE_STYLE", "1");
}

void applyBorderlessShadow(SDL_Window* window) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (window == nullptr || SDL_GetWindowWMInfo(window, &info) != SDL_TRUE ||
        info.subsystem != SDL_SYSWM_WINDOWS) {
        SDL_Log("xt_shell: borderless shadow: no HWND (%s)", SDL_GetError());
        return;
    }
    const MARGINS margins{1, 1, 1, 1};
    const HRESULT hr = DwmExtendFrameIntoClientArea(info.info.win.window, &margins);
    if (FAILED(hr)) {
        SDL_Log("xt_shell: DwmExtendFrameIntoClientArea failed (0x%08lx)", static_cast<unsigned long>(hr));
    }
}

namespace {

/// Runs after SDL's window procedure (DefSubclassProc): SDL answers WM_GETMINMAXINFO for a
/// borderless resizable window with the whole screen, which would maximize it over the taskbar;
/// the maximized rectangle becomes the monitor's work area instead. WM_GETMINMAXINFO is sent, not
/// posted, so SDL_SetWindowsMessageHook (posted messages only) never sees it.
LRESULT CALLBACK workAreaProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR /*id*/,
                              DWORD_PTR /*data*/) {
    const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
    if (msg == WM_GETMINMAXINFO) {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor) != 0) {
            // Relative to the monitor (Windows maps them onto the monitor the window maximizes on).
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left;
            info->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top;
            info->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left;
            info->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top;
        }
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, &workAreaProc, 1);
    }
    return result;
}

}  // namespace

void keepMaximizedInWorkArea(SDL_Window* window) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (window == nullptr || SDL_GetWindowWMInfo(window, &info) != SDL_TRUE ||
        info.subsystem != SDL_SYSWM_WINDOWS) {
        SDL_Log("xt_shell: borderless work area: no HWND (%s)", SDL_GetError());
        return;
    }
    if (SetWindowSubclass(info.info.win.window, &workAreaProc, 1, 0) == FALSE) {
        SDL_Log("xt_shell: SetWindowSubclass failed");
    }
}

// -- T-393 (docs/design/08-events.md §8.14): the OLE file drop target --------------------------

void initPlatformFileDrop() {
    // Once per App, before its first window (RegisterDragDrop needs OLE on this thread). SDL's own
    // drop events (WM_DROPFILES through DragAcceptFiles) are turned off so a drop is never
    // delivered twice; the OLE target takes the drag instead.
    const HRESULT hr = OleInitialize(nullptr);
    if (FAILED(hr)) {
        SDL_Log("xt_shell: OleInitialize failed (0x%08lx); no file drop", static_cast<unsigned long>(hr));
        g_oleInitialized = false;
    } else {
        g_oleInitialized = true;
    }
    SDL_EventState(SDL_DROPFILE, SDL_DISABLE);
    SDL_EventState(SDL_DROPTEXT, SDL_DISABLE);
    SDL_EventState(SDL_DROPBEGIN, SDL_DISABLE);
    SDL_EventState(SDL_DROPCOMPLETE, SDL_DISABLE);
}

void shutdownPlatformFileDrop() {
    if (g_oleInitialized) {
        OleUninitialize();
        g_oleInitialized = false;
    }
}

namespace {

/// UTF-16 -> UTF-8 (empty on failure).
std::string toUtf8(const wchar_t* text, int length) {
    if (length <= 0) {
        return std::string();
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), bytes, nullptr, nullptr);
    return out;
}

FORMATETC hdropFormat() {
    FORMATETC format{};
    format.cfFormat = CF_HDROP;
    format.dwAspect = DVASPECT_CONTENT;
    format.lindex = -1;
    format.tymed = TYMED_HGLOBAL;
    return format;
}

/// The IDropTarget of one window: CF_HDROP drags become ExternalDragEnter / Move / Leave /
/// ExternalDrop through the window's callback (window-logical coordinates); the effect is
/// DROPEFFECT_COPY over a `drop-target`, DROPEFFECT_NONE elsewhere and for anything but files.
/// Reference counted (COM); the window holds one reference from registerFileDrop() to
/// revokeFileDrop(). Every callback runs on the window's thread, inside SDL_PollEvent.
class FileDropTarget final : public IDropTarget {
public:
    FileDropTarget(HWND hwnd, SDL_Window* window, ExternalDragFn drag)
        : m_hwnd(hwnd), m_window(window), m_drag(std::move(drag)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDropTarget)) {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&m_refs)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG refs = InterlockedDecrement(&m_refs);
        if (refs == 0) {
            delete this;
        }
        return static_cast<ULONG>(refs);
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD /*keys*/, POINTL point,
                                        DWORD* effect) override {
        FORMATETC format = hdropFormat();
        m_files = data != nullptr && data->QueryGetData(&format) == S_OK;
        return answer(InputEvent::ExternalDragEnter, point, {}, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD /*keys*/, POINTL point, DWORD* effect) override {
        return answer(InputEvent::ExternalDragMove, point, {}, effect);
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (m_files) {
            m_drag(InputEvent::ExternalDragLeave, Vec2{}, {});
        }
        m_files = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD /*keys*/, POINTL point, DWORD* effect) override {
        std::vector<std::string> paths;
        if (m_files && data != nullptr) {
            FORMATETC format = hdropFormat();
            STGMEDIUM medium{};
            if (data->GetData(&format, &medium) == S_OK) {
                if (HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal)); drop != nullptr) {
                    const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
                    for (UINT i = 0; i < count; ++i) {
                        const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                        std::wstring name(static_cast<size_t>(length) + 1, L'\0');
                        DragQueryFileW(drop, i, name.data(), length + 1);
                        std::string utf8 = toUtf8(name.c_str(), static_cast<int>(length));
                        if (!utf8.empty()) {
                            paths.push_back(std::move(utf8));
                        }
                    }
                    GlobalUnlock(medium.hGlobal);
                }
                ReleaseStgMedium(&medium);
            }
        }
        if (paths.empty()) {
            // Not files after all: the drag ends here.
            DragLeave();
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }
        std::vector<std::string_view> views(paths.begin(), paths.end());
        const HRESULT hr = answer(InputEvent::ExternalDrop, point, views, effect);
        m_files = false;
        return hr;
    }

private:
    /// Pushes `kind` at `point` (screen pixels) and sets the drag effect from the answer.
    HRESULT answer(InputEvent::Kind kind, POINTL point, std::span<const std::string_view> paths,
                   DWORD* effect) {
        if (effect == nullptr) {
            return E_INVALIDARG;
        }
        if (!m_files) {
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }
        *effect = m_drag(kind, toWindow(point), paths) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    /// Screen pixels -> window-logical units (SDL's window size over the client area's pixels).
    Vec2 toWindow(POINTL point) const {
        POINT client{point.x, point.y};
        ScreenToClient(m_hwnd, &client);
        RECT rect{};
        GetClientRect(m_hwnd, &rect);
        int w = 0;
        int h = 0;
        SDL_GetWindowSize(m_window, &w, &h);
        const float sx = rect.right > 0 ? static_cast<float>(w) / static_cast<float>(rect.right) : 1.0f;
        const float sy = rect.bottom > 0 ? static_cast<float>(h) / static_cast<float>(rect.bottom) : 1.0f;
        return Vec2{static_cast<float>(client.x) * sx, static_cast<float>(client.y) * sy};
    }

    LONG m_refs = 1;
    HWND m_hwnd;
    SDL_Window* m_window;
    ExternalDragFn m_drag;
    bool m_files = false;  // The drag in progress carries CF_HDROP.
};

struct FileDropHandle {
    HWND hwnd = nullptr;
    FileDropTarget* target = nullptr;
};

}  // namespace

void* registerFileDrop(SDL_Window* window, ExternalDragFn drag) {
    if (!g_oleInitialized) {
        return nullptr;
    }
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (window == nullptr || SDL_GetWindowWMInfo(window, &info) != SDL_TRUE ||
        info.subsystem != SDL_SYSWM_WINDOWS) {
        SDL_Log("xt_shell: file drop: no HWND (%s)", SDL_GetError());
        return nullptr;
    }
    HWND hwnd = info.info.win.window;
    auto* target = new FileDropTarget(hwnd, window, std::move(drag));
    const HRESULT hr = RegisterDragDrop(hwnd, target);
    if (FAILED(hr)) {
        SDL_Log("xt_shell: RegisterDragDrop failed (0x%08lx)", static_cast<unsigned long>(hr));
        target->Release();
        return nullptr;
    }
    return new FileDropHandle{hwnd, target};
}

void revokeFileDrop(void* handle) {
    if (handle == nullptr) {
        return;
    }
    auto* drop = static_cast<FileDropHandle*>(handle);
    RevokeDragDrop(drop->hwnd);  // Releases OLE's references.
    drop->target->Release();     // Ours.
    delete drop;
}

}  // namespace xt::shell

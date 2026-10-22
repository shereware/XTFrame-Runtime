#include "xt/shell/app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "xt/sw/sw_png.h"

/// App: SDL2 window + GL 3.3 context + main loop. See Docs/tasks/T-20.md "app.h",
/// Docs/tasks/T-33b.md "shell". No `<chrono>` (SDL_GetPerformanceCounter/Frequency is used for
/// dt, per the task sheet).
///
/// Not covered by Docs/tasks/T-33b.md (conservative choices made here):
///   - SDL_TEXTEDITING's `start`/`length` count *characters*, not bytes, so they are converted
///     to UTF-8 byte offsets into the composition string before being handed to
///     InputEvent::compositionCursor/compositionSelLen (which 08.8 defines as byte offsets).
///   - SDL_SetTextInputRect is called once per frame, after Context::update() (so the rect
///     reflects this frame's layout) and only while text input is active and the rect is
///     non-empty; the rect is converted from logical pixels to physical ones with the DPR,
///     since SDL2 takes window pixels.
///   - A CompositionEnd is also pushed when text input stops (focus left the input) while a
///     composition was still in flight, so the core never keeps a stale composition.

namespace xt::shell {

// Declared in sdl_input.cpp (no header -- see that file's top comment).
bool translateSdlEvent(const SDL_Event& event, float dpr, InputEvent& out);
size_t translateSdlTextInput(const SDL_Event& event, InputEvent* out, size_t maxOut);
bool translateSdlGamepadEvent(const SDL_Event& event, InputEvent& out);  // T-194.
uint8_t sdlModifiers(uint16_t sdlMod);                                   // T-387.
// T-393 (08.14): SDL_DROP* -> one ExternalDrop (sdl_input.cpp).
bool translateSdlDrop(const SDL_Event& event, std::vector<std::string>& pending,
                      std::vector<std::string_view>& views, InputEvent& out);
// T-393: platform_win.cpp (OleInitialize / OleUninitialize, SDL's own drop events off) or
// platform_other.cpp (no-ops; SDL's drop events stay on).
void initPlatformFileDrop();
void shutdownPlatformFileDrop();

namespace {

/// Byte offset of the `index`-th UTF-8 character of `text` (clamped to its length). SDL2's
/// SDL_TextEditingEvent counts characters; 08.8 counts bytes.
uint32_t utf8ByteOffset(std::string_view text, uint32_t index) {
    size_t offset = 0;
    for (uint32_t i = 0; i < index && offset < text.size(); ++i) {
        ++offset;
        while (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xC0u) == 0x80u) {
            ++offset;
        }
    }
    return static_cast<uint32_t>(offset);
}

}  // namespace

bool parseFitMode(std::string_view text, FitMode& out) {
    if (text == "native") {
        out = FitMode::Native;
    } else if (text == "stretch") {
        out = FitMode::Stretch;
    } else if (text == "contain") {
        out = FitMode::Contain;
    } else if (text == "cover") {
        out = FitMode::Cover;
    } else if (text == "center") {
        out = FitMode::Center;
    } else {
        return false;
    }
    return true;
}

namespace {

/// A whole-string float parse (no exceptions: strtof + an end check).
bool parseWholeFloat(std::string_view text, float& out) {
    if (text.empty()) {
        return false;
    }
    std::string buffer(text);
    char* end = nullptr;
    const float v = std::strtof(buffer.c_str(), &end);
    if (end != buffer.c_str() + buffer.size()) {
        return false;
    }
    out = v;
    return true;
}

}  // namespace

bool parseDeviceAspect(std::string_view text, float& out) {
    const size_t colon = text.find(':');
    float w = 0;
    float h = 1;
    if (colon == std::string_view::npos) {
        if (!parseWholeFloat(text, w)) {
            return false;
        }
    } else if (!parseWholeFloat(text.substr(0, colon), w) || !parseWholeFloat(text.substr(colon + 1), h)) {
        return false;
    }
    if (!(w > 0.0f) || !(h > 0.0f)) {
        return false;
    }
    out = w / h;
    return true;
}

bool parseSafeArea(std::string_view text, Insets& out) {
    float v[4] = {};
    int count = 0;
    size_t start = 0;
    while (count < 4) {
        const size_t comma = text.find(',', start);
        const std::string_view part =
            text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        if (!parseWholeFloat(part, v[count]) || !(v[count] >= 0.0f) || v[count] > 0.5f) {
            return false;
        }
        ++count;
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    if (count == 1) {
        out = Insets{v[0], v[0], v[0], v[0]};
        return true;
    }
    if (count == 4 && text.find(',', start) == std::string_view::npos) {
        out = Insets{v[0], v[1], v[2], v[3]};
        return true;
    }
    return false;
}

bool parseDeviceKind(std::string_view text, DeviceKind& out) {
    if (text == "desktop") {
        out = DeviceKind::Desktop;
    } else if (text == "console") {
        out = DeviceKind::Console;
    } else if (text == "handheld") {
        out = DeviceKind::Handheld;
    } else {
        return false;
    }
    return true;
}

bool parseInputKind(std::string_view text, InputKind& out) {
    if (text == "mouse") {
        out = InputKind::Mouse;
    } else if (text == "gamepad") {
        out = InputKind::Gamepad;
    } else if (text == "touch") {
        out = InputKind::Touch;
    } else {
        return false;
    }
    return true;
}

Rect fitPlacement(FitMode mode, float designW, float designH, float windowW, float windowH) {
    if (mode == FitMode::Native || mode == FitMode::Stretch || designW <= 0.0f || designH <= 0.0f) {
        return Rect{0, 0, windowW, windowH};
    }
    float scale = 1.0f;
    if (mode == FitMode::Contain) {
        scale = std::min(windowW / designW, windowH / designH);
    } else if (mode == FitMode::Cover) {
        scale = std::max(windowW / designW, windowH / designH);
    }
    const float w = designW * scale;
    const float h = designH * scale;
    return Rect{(windowW - w) * 0.5f, (windowH - h) * 0.5f, w, h};
}

App::App(const WindowConfig& config) : m_assets(std::string(), sw::decodeImageBytes) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        m_error = SDL_GetError();
        return;
    }
    // T-398 (23 §23.ad.1): closing the last window must not post SDL_QUIT by itself -- the
    // close-request handler may veto that close (App ends run() on the main window's CLOSE).
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
    // T-393 (08.14): OLE for the windows' file drop targets, once, before the first window.
    initPlatformFileDrop();
    m_platformDrop = true;
    // T-384a (23 §23.ad): the window, its renderer and its Context live in window 0.
    m_windows.push_back(std::unique_ptr<Window>(new Window(*this, config, 0)));
    if (!m_windows[0]->m_created) {
        m_error = m_windows[0]->m_error;
        return;
    }
    // T-377 (23 §23.ab): live resize through an event watch (harmless where the OS has no modal
    // size loop: the watch only acts during run() and never inside a frame's update / render).
    m_mainThread = SDL_ThreadID();
    if (config.liveResize) {
        SDL_AddEventWatch(&App::liveResizeWatch, this);
        m_liveResize = true;
    }
    m_ok = true;
}

App::~App() {
    if (m_liveResize) {
        SDL_DelEventWatch(&App::liveResizeWatch, this);
    }
    m_windows.clear();
    if (m_platformDrop) {
        shutdownPlatformFileDrop();  // T-393: after every window revoked its drop target.
    }
    for (Pad& pad : m_pads) {
        if (pad.controller != nullptr) {
            SDL_GameControllerClose(pad.controller);
        }
    }
    SDL_Quit();
}

bool App::enableGamepad(uint8_t maxPlayers) {
    m_maxPlayers = maxPlayers == 0 || maxPlayers > kMaxPlayers ? kMaxPlayers : maxPlayers;
    if (!m_gamepad) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
            return false;
        }
        m_gamepad = true;
    }
    // Controllers already connected; SDL also queues a CONTROLLERDEVICEADDED for each of them,
    // which handleGamepadEvent() ignores once the instance is open.
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        SDL_Event added{};
        added.type = SDL_CONTROLLERDEVICEADDED;
        added.cdevice.which = i;
        handleGamepadEvent(added);
    }
    return true;
}

bool App::handleGamepadEvent(const SDL_Event& event) {
    if (!m_gamepad) {
        return false;
    }
    switch (event.type) {
        case SDL_CONTROLLERDEVICEADDED: {
            const int index = event.cdevice.which;
            if (!SDL_IsGameController(index)) {
                return true;
            }
            const SDL_JoystickID instance = SDL_JoystickGetDeviceInstanceID(index);
            for (const Pad& pad : m_pads) {
                if (pad.instance == instance) {
                    return true;  // Already open.
                }
            }
            uint8_t player = 0;
            while (player < m_maxPlayers &&
                   std::any_of(
                       m_pads.begin(), m_pads.end(), [player](const Pad& p) { return p.player == player; })) {
                ++player;
            }
            if (player >= m_maxPlayers) {
                return true;  // Every player slot is taken.
            }
            SDL_GameController* controller = SDL_GameControllerOpen(index);
            if (controller != nullptr) {
                m_pads.push_back(Pad{controller, instance, player});
            }
            return true;
        }
        case SDL_CONTROLLERDEVICEREMOVED:
            for (size_t i = 0; i < m_pads.size(); ++i) {
                if (m_pads[i].instance == event.cdevice.which) {
                    SDL_GameControllerClose(m_pads[i].controller);
                    m_pads.erase(m_pads.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
            return true;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
        case SDL_CONTROLLERAXISMOTION: {
            const SDL_JoystickID which =
                event.type == SDL_CONTROLLERAXISMOTION ? event.caxis.which : event.cbutton.which;
            for (const Pad& pad : m_pads) {
                if (pad.instance != which) {
                    continue;
                }
                InputEvent input;
                if (translateSdlGamepadEvent(event, input)) {
                    input.player = pad.player;
                    focusWindow().m_context->pushInput(input);  // T-384b (23 §23.ad).
                }
                break;
            }
            return true;
        }
        default:
            return false;
    }
}

bool App::handleTouchEvent(const SDL_Event& event, Window& window) {
    if (event.type != SDL_FINGERDOWN && event.type != SDL_FINGERMOTION && event.type != SDL_FINGERUP) {
        return false;
    }
    // SDL finger coordinates are 0..1 of the window; the recognizer works in document pixels.
    // T-384b: `window` is the one the finger event names (its own recognizer).
    int w = 0;
    int h = 0;
    SDL_GetWindowSize(window.m_window, &w, &h);
    const Vec2 pos = window.windowToDesign(
        Vec2{event.tfinger.x * static_cast<float>(w), event.tfinger.y * static_cast<float>(h)});
    const float time = static_cast<float>(event.tfinger.timestamp) / 1000.0f;
    const uint64_t finger = static_cast<uint64_t>(event.tfinger.fingerId);
    if (event.type == SDL_FINGERDOWN) {
        window.m_gestures.touchDown(finger, pos, time);
    } else if (event.type == SDL_FINGERMOTION) {
        window.m_gestures.touchMove(finger, pos, time);
    } else {
        window.m_gestures.touchUp(finger, pos, time);
    }
    return true;
}

uint32_t App::addFontDir(std::string_view dir, uint32_t maxFiles) {
#ifndef XT_GL_NO_TEXT
    return m_textShaper.addFontDir(dir, maxFiles);
#else
    (void)dir;
    (void)maxFiles;
    return 0;
#endif
}

void App::addSystemFonts() {
#ifndef XT_GL_NO_TEXT
    // The directory table and the budget live in FtTextShaper now, so every host -- xtframe-pack's
    // thumbnails, engine_integration -- gets the same fallback (Docs/tasks/T-97.md).
    m_textShaper.addSystemFonts();
#endif
}

void App::setAssetRoot(std::string dir) {
    m_assets = ShellAssetResolver(std::move(dir), sw::decodeImageBytes);
}

void App::setAssetRoot(std::vector<std::string> dirs) {
    m_assets = ShellAssetResolver(std::move(dirs), sw::decodeImageBytes);
}

// T-384a: the per-window methods forward to window 0.
void App::setTitle(const char* title) { mainWindow().setTitle(title); }

float App::devicePixelRatio() const { return m_windows[0]->devicePixelRatio(); }

void App::syncViewport() { mainWindow().syncViewport(); }

void App::setFit(FitMode mode, int designWidth, int designHeight) {
    mainWindow().setFit(mode, designWidth, designHeight);
}

void App::setLogicalHeight(float logicalHeight, UiScaleEdge edge) {
    mainWindow().setLogicalHeight(logicalHeight, edge);
}

void App::setScene(const uint8_t* rgba, int width, int height) { mainWindow().setScene(rgba, width, height); }

bool App::readPixels(std::vector<uint8_t>& rgba, int& width, int& height) {
    return mainWindow().readPixels(rgba, width, height);
}

void App::setHitTest(std::function<HitRegion(int x, int y)> hitTest) {
    mainWindow().setHitTest(std::move(hitTest));
}

void App::minimize() { mainWindow().minimize(); }

void App::maximize() { mainWindow().maximize(); }

void App::restore() { mainWindow().restore(); }

void App::toggleMaximize() { mainWindow().toggleMaximize(); }

bool App::isMaximized() const { return m_windows[0]->isMaximized(); }

void App::close() { mainWindow().close(); }

bool App::openUrl(std::string_view url) {
    const std::string text(url);  // SDL_OpenURL takes a NUL-terminated string.
    if (SDL_OpenURL(text.c_str()) != 0) {
        SDL_Log("xt_shell: openUrl(%s) failed: %s", text.c_str(), SDL_GetError());
        return false;
    }
    return true;
}

uint8_t App::modifierState() const { return sdlModifiers(static_cast<uint16_t>(SDL_GetModState())); }

HitRegion hitRegionFor(int x, int y, int windowW, int windowH, int resizeBorder, bool maximized,
                       const std::function<AppRegion(int x, int y)>& appRegionAt) {
    if (!maximized && resizeBorder > 0) {
        const bool left = x < resizeBorder;
        const bool right = x >= windowW - resizeBorder;
        const bool top = y < resizeBorder;
        const bool bottom = y >= windowH - resizeBorder;
        if (top && left) {
            return HitRegion::ResizeTopLeft;
        }
        if (top && right) {
            return HitRegion::ResizeTopRight;
        }
        if (bottom && right) {
            return HitRegion::ResizeBottomRight;
        }
        if (bottom && left) {
            return HitRegion::ResizeBottomLeft;
        }
        if (top) {
            return HitRegion::ResizeTop;
        }
        if (right) {
            return HitRegion::ResizeRight;
        }
        if (bottom) {
            return HitRegion::ResizeBottom;
        }
        if (left) {
            return HitRegion::ResizeLeft;
        }
    }
    const AppRegion region = appRegionAt ? appRegionAt(x, y) : AppRegion::None;
    return region == AppRegion::Drag ? HitRegion::Draggable : HitRegion::Normal;
}

// -------------------------------------------------------------------------------------------
// T-384b (23 §23.ad): windows.
// -------------------------------------------------------------------------------------------

WindowId App::createWindow(const WindowConfig& config) {
    if (!m_ok) {
        return kInvalidWindow;  // m_error already says why the App itself failed.
    }
    std::unique_ptr<Window> window(new Window(*this, config, m_nextWindowId));
    if (!window->m_created) {
        m_error = window->m_error;
        return kInvalidWindow;
    }
    const WindowId id = m_nextWindowId++;
    m_windows.push_back(std::move(window));
    return id;
}

Window* App::findWindow(WindowId id) {
    for (const std::unique_ptr<Window>& window : m_windows) {
        if (window->m_id == id && !window->m_closing) {
            return window.get();
        }
    }
    return nullptr;
}

Window& App::window(WindowId id) {
    Window* window = findWindow(id);
    if (window == nullptr) {
        SDL_Log("xt_shell: App::window(%u): no such window", id);
        std::abort();
    }
    return *window;
}

bool App::requestClose(Window& window, bool pushQuit) {
    if (window.m_closing) {
        return true;  // Already on its way out.
    }
    if (m_closeRequest && !m_closeRequest(window.m_id)) {
        return false;  // Vetoed: the window stays (the main window keeps run() going).
    }
    if (window.m_id == 0) {
        if (pushQuit) {
            SDL_Event event{};
            event.type = SDL_QUIT;
            SDL_PushEvent(&event);  // Window::close(): ends run() / runFrame() as before.
        } else {
            m_quit = true;
        }
        return true;
    }
    window.m_closing = true;
    if (m_windowClosed) {
        m_windowClosed(window.m_id);
    }
    return true;
}

void App::closeWindow(WindowId id) {
    Window* window = findWindow(id);
    if (window == nullptr) {
        return;
    }
    if (!requestClose(*window, /*pushQuit=*/false)) {
        return;
    }
    if (id != 0 && !m_inFrame) {
        destroyClosedWindows();
    }
}

std::vector<WindowId> App::windows() const {
    std::vector<WindowId> ids;
    ids.reserve(m_windows.size());
    for (const std::unique_ptr<Window>& window : m_windows) {
        if (!window->m_closing) {
            ids.push_back(window->m_id);
        }
    }
    return ids;
}

void App::destroyClosedWindows() {
    // Window 0 is never marked (closing it quits), so m_windows[0] stays the main window.
    if (Window* focused = m_focusedWindow != kInvalidWindow ? findWindow(m_focusedWindow) : nullptr;
        focused == nullptr) {
        m_focusedWindow = kInvalidWindow;  // T-398: the focused window is closing (or gone).
    }
    m_windows.erase(std::remove_if(m_windows.begin(), m_windows.end(),
                                   [](const std::unique_ptr<Window>& window) { return window->m_closing; }),
                    m_windows.end());
}

Window* App::windowForSdlId(uint32_t sdlId) {
    if (m_windows.empty()) {
        return nullptr;
    }
    if (sdlId == 0) {
        return m_windows[0].get();  // Synthetic events, or a key event without a focus window.
    }
    for (const std::unique_ptr<Window>& window : m_windows) {
        if (window->m_window != nullptr && SDL_GetWindowID(window->m_window) == sdlId) {
            return window->m_closing ? nullptr : window.get();
        }
    }
    return nullptr;
}

Window* App::eventWindow(const SDL_Event& event) {
    switch (event.type) {
        case SDL_WINDOWEVENT:
            return windowForSdlId(event.window.windowID);
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            return windowForSdlId(event.key.windowID);
        case SDL_TEXTEDITING:
            return windowForSdlId(event.edit.windowID);
        case SDL_TEXTINPUT:
            return windowForSdlId(event.text.windowID);
        case SDL_MOUSEMOTION:
            return windowForSdlId(event.motion.windowID);
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            return windowForSdlId(event.button.windowID);
        case SDL_MOUSEWHEEL:
            return windowForSdlId(event.wheel.windowID);
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION:
        case SDL_FINGERUP:
            return windowForSdlId(event.tfinger.windowID);
        default:
            return windowForSdlId(0);
    }
}

Window& App::focusWindow() {
    for (const std::unique_ptr<Window>& window : m_windows) {
        if (!window->m_closing && window->m_window != nullptr &&
            (SDL_GetWindowFlags(window->m_window) & SDL_WINDOW_INPUT_FOCUS) != 0) {
            return *window;
        }
    }
    return mainWindow();
}

int App::displayCount() const {
    const int n = SDL_GetNumVideoDisplays();
    return n > 0 ? n : 0;
}

Rect App::displayBounds(int index) const {
    SDL_Rect r{};
    if (SDL_GetDisplayBounds(index, &r) != 0) {
        return Rect{0, 0, 0, 0};
    }
    return Rect{static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w),
                static_cast<float>(r.h)};
}

std::string App::displayName(int index) const {
    const char* name = SDL_GetDisplayName(index);
    return name != nullptr ? std::string(name) : std::string();
}

WindowId App::focusedWindow() const {
    for (const std::unique_ptr<Window>& window : m_windows) {
        if (window->m_id == m_focusedWindow && !window->m_closing) {
            return m_focusedWindow;
        }
    }
    return kInvalidWindow;
}

bool App::setTargetFps(int fps) {
    if (fps <= 0) {
        m_targetFps = 0;
        return true;
    }
    if (m_windows.empty() || m_windows[0]->m_vsync) {
        SDL_Log("xt_shell: setTargetFps(%d) refused: the main window has vsync on", fps);
        return false;
    }
    m_targetFps = fps;
    m_targetFpsWarned = false;
    return true;
}

bool App::targetFpsActive() const {
    if (m_targetFps <= 0) {
        return false;
    }
    for (const std::unique_ptr<Window>& window : m_windows) {
        if (window->m_vsync && !window->m_closing) {
            return false;
        }
    }
    return true;
}

Rect App::displayUsableBounds(int index) const {
    SDL_Rect r{};
    if (SDL_GetDisplayUsableBounds(index, &r) != 0) {
        return Rect{0, 0, 0, 0};
    }
    return Rect{static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w),
                static_cast<float>(r.h)};
}

void App::pollEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event) != 0) {
        if (m_eventHook && m_eventHook(event)) {  // T-240.
            continue;
        }
        if (event.type == SDL_QUIT) {
            m_quit = true;
            continue;
        }
#ifdef XT_SHELL_SW
        if (event.type == SDL_RENDER_DEVICE_RESET || event.type == SDL_RENDER_TARGETS_RESET) {
            // T-240: the renderer lost its textures (Android pause/resume); recreate and re-upload.
            for (const std::unique_ptr<Window>& w : m_windows) {
                if (w->m_closing) {
                    continue;
                }
                if (w->m_texture != nullptr) {
                    SDL_DestroyTexture(w->m_texture);
                    w->m_texture = nullptr;
                }
                w->m_texW = w->m_texH = 0;
                w->syncViewport();
            }
            continue;
        }
#endif
        if (event.type == SDL_DISPLAYEVENT) {
            // T-398 (23 §23.ad.1): a display came, went or turned; the host queries again.
            const Uint8 kind = event.display.event;
            if (m_displayChanged &&
                (kind == SDL_DISPLAYEVENT_CONNECTED || kind == SDL_DISPLAYEVENT_DISCONNECTED ||
                 kind == SDL_DISPLAYEVENT_ORIENTATION)) {
                m_displayChanged();
            }
            continue;
        }
        if (handleGamepadEvent(event)) {  // T-194; T-384b: to the focus window.
            continue;
        }
        if (event.type == SDL_DROPBEGIN || event.type == SDL_DROPFILE || event.type == SDL_DROPTEXT ||
            event.type == SDL_DROPCOMPLETE) {
            handleDropEvent(event);  // T-393 (08.14): the non-Windows fallback.
            continue;
        }
        // T-384b: every other event goes to the window its windowID names (window 0 without one);
        // an event of a window that is gone or closing is dropped.
        Window* target = eventWindow(event);
        if (target == nullptr) {
            continue;
        }
        Window& window = *target;
        if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE) {
            // T-384b: the main window ends run(); another one is reported, then destroyed after
            // this loop (never while the windows are being iterated). T-398: unless the
            // close-request handler vetoes it.
            requestClose(window, /*pushQuit=*/false);
            continue;
        }
        if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            m_focusedWindow = window.m_id;  // T-398: App::focusedWindow().
        } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST &&
                   m_focusedWindow == window.m_id) {
            m_focusedWindow = kInvalidWindow;
        }
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_AC_BACK) {
            // T-240 (13.8 system events): the Android back key; a UI answers on system.back.
            if (event.key.repeat == 0) {
                window.m_context->postSystemEvent("back");
            }
            continue;
        }
        if (event.type == SDL_KEYUP && event.key.keysym.sym == SDLK_AC_BACK) {
            continue;
        }
        if (event.type == SDL_WINDOWEVENT) {
            if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
                event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                window.syncViewport();
            }
        }
        if (event.type == SDL_TEXTINPUT) {
            InputEvent chunks[8];
            size_t n = translateSdlTextInput(event, chunks, 8);
            for (size_t i = 0; i < n; ++i) {
                window.m_context->pushInput(chunks[i]);
            }
            continue;
        }
        if (event.type == SDL_TEXTEDITING) {
            // §8.8: CompositionStart/End derived from TEXTEDITING's presence/emptiness.
            std::string_view text(event.edit.text);
            if (!text.empty() && !window.m_composing) {
                InputEvent start{};
                start.kind = InputEvent::CompositionStart;
                window.m_context->pushInput(start);
                window.m_composing = true;
            }
            if (!text.empty()) {
                uint32_t start = event.edit.start < 0 ? 0u : static_cast<uint32_t>(event.edit.start);
                uint32_t length = event.edit.length < 0 ? 0u : static_cast<uint32_t>(event.edit.length);
                uint32_t cursor = utf8ByteOffset(text, start);
                InputEvent update{};
                update.kind = InputEvent::CompositionUpdate;
                update.composition = text;
                update.compositionCursor = cursor;
                update.compositionSelLen = utf8ByteOffset(text, start + length) - cursor;
                window.m_context->pushInput(update);
            } else if (window.m_composing) {
                InputEvent end{};
                end.kind = InputEvent::CompositionEnd;
                window.m_context->pushInput(end);
                window.m_composing = false;
            }
            continue;
        }

        if (handleTouchEvent(event, window)) {  // T-194: SDL still synthesizes mouse events from touch.
            continue;
        }
        if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_LEAVE &&
            SDL_GetMouseState(nullptr, nullptr) != 0) {
            // T-386 (08.11): the pointer left the window with a button held (the OS took the
            // capture away): cancel the press before the PointerLeave.
            window.m_context->cancelPointer();
        }
        InputEvent input;
        if (translateSdlEvent(event, window.devicePixelRatio(), input)) {
            if (input.kind == InputEvent::PointerMove || input.kind == InputEvent::PointerDown ||
                input.kind == InputEvent::PointerUp || input.kind == InputEvent::Wheel) {
                input.pos = window.windowToDesign(input.pos);  // T-156: identity under Native.
            }
            window.m_context->pushInput(input);
        }
    }
    // T-194 (27.7): a held finger becomes a Hold without any new event (per window, T-384b).
    const float now = static_cast<float>(SDL_GetTicks()) / 1000.0f;
    for (const std::unique_ptr<Window>& window : m_windows) {
        if (window->m_closing) {
            continue;
        }
        window->m_gestures.update(now);
        InputEvent gesture;
        while (window->m_gestures.poll(gesture)) {
            gesture.modifiers = modifierState();  // T-387 (08.12).
            window->m_context->pushInput(gesture);
        }
    }
}

void App::handleDropEvent(const SDL_Event& event) {
    // T-393 (08.14): SDL's file drop (Windows turns it off and uses its OLE drop target). The
    // event's file is always freed by translateSdlDrop, also for a window that takes no drops.
    Window* window = windowForSdlId(event.drop.windowID);
    std::vector<std::string> discard;
    std::vector<std::string_view> views;
    InputEvent input;
    const bool accept = window != nullptr && window->m_acceptFileDrop;
    if (!translateSdlDrop(event, accept ? window->m_dropPaths : discard, views, input) || !accept) {
        return;
    }
    // The drop position: SDL_GetMouseState when the pointer is over this window, else the global
    // position relative to the window.
    int x = 0;
    int y = 0;
    if (SDL_GetMouseFocus() == window->m_window) {
        SDL_GetMouseState(&x, &y);
    } else {
        int wx = 0;
        int wy = 0;
        SDL_GetGlobalMouseState(&x, &y);
        SDL_GetWindowPosition(window->m_window, &wx, &wy);
        x -= wx;
        y -= wy;
    }
    window->externalDrag(InputEvent::ExternalDrop, Vec2{static_cast<float>(x), static_cast<float>(y)},
                         input.paths);
    window->m_dropPaths.clear();
}

void App::syncTextInput() {
    // 08.8: the host starts/stops platform text input on the wantsTextInput() edges and keeps
    // the IME candidate window positioned at the caret every frame. T-384b: SDL has one text
    // input state, so it follows the focus window.
    Window& focus = focusWindow();
    const bool wants = focus.m_context->wantsTextInput();
    if (m_textInputActive && (!wants || focus.m_id != m_textInputWindow)) {
        if (Window* previous = findWindow(m_textInputWindow); previous != nullptr && previous->m_composing) {
            InputEvent end{};
            end.kind = InputEvent::CompositionEnd;
            previous->m_context->pushInput(end);
            previous->m_composing = false;
        }
        SDL_StopTextInput();
        m_textInputActive = false;
    }
    if (wants && !m_textInputActive) {
        SDL_StartTextInput();
        m_textInputActive = true;
    }
    m_textInputWindow = focus.m_id;
    SDL_Rect rect;
    if (wants && focus.caretRect(rect)) {
        SDL_SetTextInputRect(&rect);
    }
}

namespace {

// T-377: sets a flag for a scope and restores its previous value (runFrame() nests from the watch).
class FlagScope {
public:
    explicit FlagScope(bool& flag) : m_flag(flag), m_saved(flag) { m_flag = true; }
    ~FlagScope() { m_flag = m_saved; }
    FlagScope(const FlagScope&) = delete;
    FlagScope& operator=(const FlagScope&) = delete;

private:
    bool& m_flag;
    bool m_saved;
};

}  // namespace

int SDLCALL App::liveResizeWatch(void* userdata, SDL_Event* event) {
    App* app = static_cast<App*>(userdata);
    if (app == nullptr || event == nullptr || event->type != SDL_WINDOWEVENT ||
        (event->window.event != SDL_WINDOWEVENT_SIZE_CHANGED &&
         event->window.event != SDL_WINDOWEVENT_EXPOSED)) {
        return 1;
    }
    // Only during run(), never re-entered, never inside a frame's update / render; a frame that
    // is inside pollEvents() is at a safe point (the OS modal size / move loop runs in there).
    if (!app->m_inRun || app->m_windows.empty()) {
        return 1;
    }
    if (app->m_inWatch || (app->m_inFrame && !app->m_inPoll)) {
        return 1;
    }
    if (SDL_ThreadID() != app->m_mainThread || event->window.windowID == 0) {
        return 1;  // An event pushed from another thread (no GL context there).
    }
    // T-384b: the window the event names; its viewport follows, then every window draws a frame.
    Window* window = app->windowForSdlId(event->window.windowID);
    if (window == nullptr) {
        return 1;  // Not one of this App's windows.
    }
    const FlagScope watch(app->m_inWatch);
    int drawableW = 0, drawableH = 0;
#ifdef XT_SHELL_SW
    SDL_GetRendererOutputSize(window->m_sdlRenderer, &drawableW, &drawableH);
#else
    SDL_GL_GetDrawableSize(window->m_window, &drawableW, &drawableH);
#endif
    // An EXPOSED at an unchanged size (SDL's modal-loop timer sends one every few ms) only
    // redraws: syncViewport() would force a full repaint.
    if (event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED || drawableW != window->m_drawableW ||
        drawableH != window->m_drawableH) {
        window->syncViewport();  // Updates m_drawableW/H and, under SW, ensureSwTarget().
    }
    const std::chrono::duration<float> elapsed =
        std::chrono::steady_clock::now() - app->mainWindow().m_lastFrameTime;
    app->runFrameAll(app->m_lastOnFrame, std::clamp(elapsed.count(), 0.0f, 0.1f), /*swap=*/true);
    return 1;
}

bool App::runFrameAll(const WindowFrameFn& onFrame, float dt, bool swap) {
    const FlagScope frame(m_inFrame);  // T-377.
    const auto now = std::chrono::steady_clock::now();
    for (const std::unique_ptr<Window>& window : m_windows) {
        window->m_lastFrameTime = now;
    }
    const Uint64 frameStart = SDL_GetPerformanceCounter();
    if (!m_inWatch) {
        // T-377: a frame run from the watch is already inside SDL_PollEvent (the OS modal loop);
        // pumping again there would steal that loop's mouse messages.
        {
            const FlagScope poll(m_inPoll);
            pollEvents();
        }
        destroyClosedWindows();  // T-384b: windows the user closed during the poll.
    }
    if (m_quit) {
        return false;
    }
    // T-384b: window 0 first, then the others in creation order. By index: onFrame may create a
    // window (appended; it renders this frame too) but never removes one (closing is deferred).
    for (size_t i = 0; i < m_windows.size(); ++i) {
        Window& window = *m_windows[i];
        if (window.m_closing) {
            continue;
        }
        // Window 0's stats include the poll (as before T-384b); the others time their own part.
        const Uint64 start = i == 0 ? frameStart : SDL_GetPerformanceCounter();
        if (onFrame) {
            onFrame(dt, window.m_id);
        }
        if (window.m_closing) {
            continue;
        }
        window.renderFrame(dt, swap, start);
    }
    syncTextInput();
    if (!m_inWatch) {
        destroyClosedWindows();  // Closed by onFrame / a listener during this frame.
    }
    return !m_quit;
}

bool App::runFrame(const std::function<void(float dt)>& onFrame, float dt, bool swap) {
    if (!onFrame) {
        return runFrameAll(nullptr, dt, swap);
    }
    return runFrameAll(
        [&onFrame](float frameDt, WindowId id) {
            if (id == 0) {
                onFrame(frameDt);
            }
        },
        dt, swap);
}

bool App::runFrame(const WindowFrameFn& onFrame, float dt, bool swap) {
    return runFrameAll(onFrame, dt, swap);
}

void App::paceFrame(uint64_t start) {
    if (m_targetFps <= 0) {
        return;
    }
    for (const std::unique_ptr<Window>& window : m_windows) {
        if (window->m_vsync && !window->m_closing) {
            if (!m_targetFpsWarned) {
                SDL_Log("xt_shell: setTargetFps(%d) ignored: a window has vsync on", m_targetFps);
                m_targetFpsWarned = true;
            }
            return;
        }
    }
    const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
    const double elapsedMs = static_cast<double>(SDL_GetPerformanceCounter() - start) * 1000.0 / freq;
    const double remainingMs = 1000.0 / static_cast<double>(m_targetFps) - elapsedMs;
    if (remainingMs >= 1.0) {
        SDL_Delay(static_cast<Uint32>(remainingMs));
    }
}

int App::run(const WindowFrameFn& onFrame) {
    m_quit = false;
    // T-377: the watch reruns this onFrame; dt is measured from the last frame (m_lastFrameTime,
    // set by every runFrame(), the watch's included), so time is never counted twice.
    const FlagScope inRun(m_inRun);
    m_lastOnFrame = onFrame;
    mainWindow().m_lastFrameTime = std::chrono::steady_clock::now();
    while (!m_quit) {
        const Uint64 start = SDL_GetPerformanceCounter();
        const std::chrono::duration<float> elapsed =
            std::chrono::steady_clock::now() - mainWindow().m_lastFrameTime;
        if (!runFrameAll(onFrame, std::clamp(elapsed.count(), 0.0f, 0.1f), /*swap=*/true)) {
            break;
        }
        paceFrame(start);
    }
    m_lastOnFrame = nullptr;
    return 0;
}

int App::run(const std::function<void(float dt)>& onFrame) {
    if (!onFrame) {
        return run(WindowFrameFn{});
    }
    return run(WindowFrameFn{[onFrame](float dt, WindowId id) {
        if (id == 0) {
            onFrame(dt);
        }
    }});
}

}  // namespace xt::shell

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "xt/asset_resolver.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/gesture.h"
#include "xt/shell/sdl_rw_asset_resolver.h"
#include "xt/shell/window.h"
#ifdef XT_SHELL_SW
#include "xt/sw/sw_renderer.h"
#else
#include "xt/gl/gl_renderer.h"
#endif

#ifndef XT_GL_NO_TEXT
#include "xt/text/ft_text_shaper.h"
#ifndef XT_SHELL_SW
#include "xt/text/glyph_atlas.h"
#endif
#endif

// SDL2 is a real third-party dependency of xt_shell (see cmake/xt_sdl2.cmake); included as
// SYSTEM by shell/CMakeLists.txt so its warnings don't count against xt_shell.
#include <SDL.h>

/// xt_shell: SDL2 window + GL 3.3 context + main loop wrapping Context/GlRenderer. See
/// docs/tasks/T-20.md "app.h", docs/design/13-api.md §13.5.
///
/// T-240 (docs/design/23-host-integration.md §23.11.1): built with XT_SHELL_GL=OFF (XT_SHELL_SW
/// defined; always on Android) App does not use xt_gl. Each frame xt_sw renders the CommandList
/// into a premultiplied RGBA buffer -- only the damage rectangles are cleared and redrawn, as in
/// the editor's SW preview (T-239) -- only those rectangles are uploaded to an SDL_Renderer
/// streaming texture, and the texture is copied to the window. A frame without damage renders and
/// uploads nothing. Fit modes work the same way (the texture is the design-size image, copied to
/// fitPlacement()).

namespace xt::shell {

/// Parses "native", "stretch", "contain", "cover" or "center" into `out`; false for anything else
/// (`out` is then left untouched).
bool parseFitMode(std::string_view text, FitMode& out);

/// T-240: the asset resolver App reads through -- SdlRwAssetResolver (SDL_RWops, APK assets on
/// Android) on the SW path, FileAssetResolver as before on the GL path.
#ifdef XT_SHELL_SW
using ShellAssetResolver = SdlRwAssetResolver;
#else
using ShellAssetResolver = FileAssetResolver;
#endif

/// T-176 (docs/design/13-api.md §13.8.2 `setDevice`): the `--device`/`--safe`/`--platform`/
/// `--input` option values of a host tool (xtframe-view). Each returns false for anything else and
/// then leaves `out` untouched.
/// `--device W:H` (e.g. `21:9`) or a plain ratio (`2.39`) -> width / height.
bool parseDeviceAspect(std::string_view text, float& out);
/// `--safe F` (all four sides) or `--safe T,R,B,L`: fractions of the viewport in [0, 0.5].
bool parseSafeArea(std::string_view text, Insets& out);
/// `--platform desktop|console|handheld`.
bool parseDeviceKind(std::string_view text, DeviceKind& out);
/// `--input mouse|gamepad|touch`.
bool parseInputKind(std::string_view text, InputKind& out);

/// T-376 (23 §23.ab): the borderless hit-test decision, without any window. (1) Within
/// `resizeBorder` window-logical pixels of an edge of the `windowW` x `windowH` window -> the
/// matching Resize* (corners win; skipped while `maximized` or when `resizeBorder` <= 0); (2) else
/// `appRegionAt(x, y)`: Drag -> Draggable, NoDrag / None -> Normal. An empty `appRegionAt` counts
/// as None.
HitRegion hitRegionFor(int x, int y, int windowW, int windowH, int resizeBorder, bool maximized,
                       const std::function<AppRegion(int x, int y)>& appRegionAt);

/// Where a `designW` x `designH` image lands in a `windowW` x `windowH` window under `mode`, in the
/// window's units (top-left origin). Native (like a degenerate size) returns the whole window.
Rect fitPlacement(FitMode mode, float designW, float designH, float windowW, float windowH);

/// SDL2 window + OpenGL 3.3 context + main loop, wrapping a Context and GlRenderer into a
/// ready-to-run desktop host.
class App {
public:
    /// Initializes SDL2, creates the window and GL 3.3 context, and constructs the GlRenderer and
    /// Context. On failure, ok() returns false and lastError() explains why; the object is
    /// otherwise safe to destroy.
    explicit App(const WindowConfig& config);
    /// Shuts down the renderer, destroys the GL context and window, and quits SDL2.
    ~App();
    /// Non-copyable (owns the SDL window/GL context and GlRenderer).
    App(const App&) = delete;
    /// Non-copyable (owns the SDL window/GL context and GlRenderer).
    App& operator=(const App&) = delete;

    /// True if the constructor successfully created the window, GL context, and renderer.
    bool ok() const { return m_ok; }
    /// Empty string if ok() is true; otherwise a description of what failed. T-384b: also set by
    /// a failed createWindow() (ok() stays true).
    const char* lastError() const { return m_error.c_str(); }

    /// The Context this App drives each frame; owns loaded Documents.
    Context& context() { return mainWindow().context(); }

    /// Rebuilds the asset resolver rooted at `dir` (must be called before/between loadDocument
    /// calls that reference relative asset paths; existing Documents keep referring to the same
    /// AssetResolver object, whose root directory this changes in place).
    void setAssetRoot(std::string dir);
    /// Multi-root form (docs/design/22-bundle.md §22.2, docs/tasks/T-138.md): the resolver tries
    /// `dirs[0]`, `dirs[1]`, ... in order, nearer roots first.
    void setAssetRoot(std::vector<std::string> dirs);

    /// The Document App renders each frame (docs/tasks/T-20.md "app.h": "ir 指標：Document 的
    /// IrView（App 持有目前載入文件的 handle）"). 0 (kInvalidHandle-equivalent) means "nothing to
    /// render". Not covered by the task sheet for the multi-document case: this task's only tool
    /// (xtframe-view) ever has one document loaded, so App renders exactly the CommandList produced
    /// by Context::render() (every loaded Document, in load order) against this single handle's
    /// IrView. True multi-document rendering (a distinct IrView per Document, each needing its own
    /// CommandList) is left for whichever later task first needs more than one simultaneously
    /// displayed Document.
    void setCurrentDocument(DocumentHandle handle) { mainWindow().setCurrentDocument(handle); }
    /// The handle set by the most recent setCurrentDocument() call (0 if never called).
    DocumentHandle currentDocument() const { return m_windows[0]->currentDocument(); }

#ifndef XT_GL_NO_TEXT
    /// The shaper this App's Context shapes text with (docs/tasks/T-30.md goal 5). Exposed so a
    /// host can register fonts -- e.g. xtframe-view scanning the system font directories
    /// (docs/design/12-compiler.md §12.6) -- before loading a document.
    text::FtTextShaper& textShaper() { return m_textShaper; }
#endif

    /// Host-settable window title (not in the task sheet's App skeleton, but needed by xtframe-view
    /// to show "XTFrame - <filename>"; a small, conservative addition).
    /// Registers every .ttf/.otf directly inside `dir` with the shell's text shaper (at most
    /// `maxFiles`); returns the number of fonts added. No-op (returns 0) without xt_text.
    uint32_t addFontDir(std::string_view dir, uint32_t maxFiles = 200);
    /// Registers the platform's system font directories (docs/design/12-compiler.md §12.6) by
    /// forwarding to FtTextShaper::addSystemFonts(), which owns the directory table and the
    /// budget (docs/design/23-host-integration.md §23.4). No-op without xt_text.
    void addSystemFonts();
    /// The window title. T-261f: `title` is UTF-8 (SDL takes UTF-8 and sets it through the
    /// platform's wide-character API on Windows).
    void setTitle(const char* title);

    /// Main loop: runs until quit() is called or the window is closed, returns 0. T-377: with
    /// WindowConfig::liveResize, a SIZE_CHANGED / EXPOSED window event that arrives while the
    /// loop is waiting in the OS (the modal size / move loop entered from SDL_PollEvent) runs one
    /// extra frame from the event watch, so `onFrame` may be called from inside SDL_PollEvent.
    int run(const std::function<void(float dt)>& onFrame);
    /// Requests that run()'s loop stop after the current iteration.
    void quit() { m_quit = true; }

    /// Runs exactly one iteration of run()'s per-frame pipeline (poll SDL events -> translate ->
    /// Context::pushInput -> resize -> onFrame(dt) -> Context::update(dt) -> Context::render ->
    /// GlRenderer::render), optionally swapping the window afterwards. Returns false once a QUIT
    /// event was observed (the caller should stop calling runFrame). Exposed (beyond the task
    /// sheet's App skeleton) so xtframe-view's --screenshot can render exactly one frame without
    /// entering run()'s loop and without swapping before glReadPixels. T-384b: every open window
    /// is updated and rendered (window 0 first); `onFrame` is called for window 0 only.
    bool runFrame(const std::function<void(float dt)>& onFrame, float dt, bool swap);

    /// drawable (framebuffer) size / window (logical) size.
    float devicePixelRatio() const;
    /// Framebuffer width in pixels (may exceed window `width` on a high-DPI display).
    int drawableWidth() const { return m_windows[0]->drawableWidth(); }
    /// Framebuffer height in pixels (may exceed window `height` on a high-DPI display).
    int drawableHeight() const { return m_windows[0]->drawableHeight(); }

    /// Re-applies the current window size to Context::setViewport(). Context::setViewport()
    /// (docs/tasks/T-18.md) only walks Documents already loaded at the time it is called, so a
    /// Document loaded after App's constructor (or after a reload) never gets its contain-fit
    /// transform computed unless the host calls this again; not covered by the task sheet's App
    /// skeleton, added so xtframe-view can call it right after Context::loadDocument /
    /// loadDocumentFile.
    void syncViewport();

    /// Selects how the document is placed in the window (docs/tasks/T-156.md); WindowConfig::fit
    /// sets the initial one. For every mode but
    /// Native the Context viewport becomes `designWidth` x `designHeight` logical pixels, frames
    /// render into an offscreen target of that size (times the DPR) which is then blitted to
    /// fitPlacement() in the window, and pointer positions are mapped back into design
    /// coordinates. Native (or a non-positive design size) restores the window-sized viewport.
    /// Calls syncViewport(). Switching away from Native at run time on a window that was created
    /// multisampled (WindowConfig::fit Native with msaa > 0) relies on the driver accepting a blit
    /// into a multisampled default framebuffer.
    void setFit(FitMode mode, int designWidth, int designHeight);
    /// The mode the latest setFit() selected (Native by default).
    FitMode fit() const { return m_windows[0]->fit(); }

    /// T-210 (docs/design/25-editor-blueprint.md §25.4.1 item 2): the design height of a UI
    /// Project (`spec.logicalHeight`). When positive, every syncViewport() also calls
    /// Context::setUiScale(viewport height / logicalHeight); 0 (the default) leaves uiscale alone.
    /// T-241: `edge` is the UI Project's `spec.uiscaleRule` -- the viewport edge divided (the
    /// width, or the shorter edge for portrait phones). Calls syncViewport().
    void setLogicalHeight(float logicalHeight, UiScaleEdge edge = UiScaleEdge::Height);

    /// T-240: called first for every SDL event pollEvents() takes; returning true consumes it (App
    /// then does nothing more with it). Hosts use it for platform events App does not interpret
    /// (app lifecycle, display changes). An empty function (the default) consumes nothing.
    void setEventHook(std::function<bool(const SDL_Event&)> hook) { m_eventHook = std::move(hook); }

    /// T-343 (docs/design/23-host-integration.md §23.aa.5, `xtframe-view --scene`): `rgba`
    /// (straight alpha, row 0 = top, `width x height`; copied) becomes the host scene: every
    /// frame it is drawn under the UI, stretched to the render target, the Context is switched to
    /// HostTargetMode::Direct and the backend draws the UI onto it -- `mix-blend-mode` and
    /// `backdrop-blur` then see the scene. A null `rgba` (or an empty size) restores Layer mode
    /// and the plain clear.
    void setScene(const uint8_t* rgba, int width, int height);
    /// T-343 (`--scene-animate`): the scene is shifted left by `pxPerFrame` scene pixels every
    /// frame, wrapping around, so every frame's backdrop differs. 0 (the default) holds it still.
    void setSceneScroll(int pxPerFrame) { mainWindow().setSceneScroll(pxPerFrame); }
    /// True once setScene() installed a scene.
    bool hasScene() const { return m_windows[0]->hasScene(); }

    /// T-240: copies the window's current image (what runFrame() last drew, before any swap) into
    /// `rgba` as top-down, straight-alpha RGBA8 rows of `width` x `height` pixels. False when the
    /// backend cannot read it back.
    bool readPixels(std::vector<uint8_t>& rgba, int& width, int& height);

    /// T-240 (item 7): when on, App logs (SDL_Log) once per second the average milliseconds per
    /// frame spent in runFrame() (events + update + render + upload), the average render time of
    /// the frames that rendered, and the average damage area ratio. Off by default.
    void setStatsLog(bool on) { mainWindow().setStatsLog(on); }

    /// The resolver this App's Context reads assets through (the one the latest setAssetRoot()
    /// built). Exposed so a host can bind external textures on it
    /// (AssetResolver::setExternalTexture, docs/tasks/T-150.md) -- after setAssetRoot(), which
    /// replaces the resolver, and before the document's first update().
    ShellAssetResolver& assetResolver() { return m_assets; }

    /// T-194 (docs/design/08-events.md §8.9): opens every SDL game controller (and later hot-
    /// plugged ones) and forwards their buttons and axes as GamepadButtonDown/Up and
    /// GamepadAxisMotion InputEvents. Controllers get player slots 0, 1, ... in connection order
    /// (up to `maxPlayers`, at most kMaxPlayers); a slot freed by an unplugged controller is
    /// reused. Returns false when SDL's game controller subsystem cannot start.
    bool enableGamepad(uint8_t maxPlayers = kMaxPlayers);
    /// T-194 (27.7): the touch gesture thresholds (SDL finger events -> GestureRecognizer ->
    /// InputEvent::Gesture); the defaults of GestureConfig until called.
    void setGestureConfig(const GestureConfig& config) { mainWindow().setGestureConfig(config); }

    /// T-376 (23 §23.ab): replaces the window's OS hit-test (SDL_SetWindowHitTest) with
    /// `hitTest(x, y)` (window-logical coordinates). An empty function restores the default
    /// implementation: hitRegionFor() with WindowConfig::resizeBorder, isMaximized() and the
    /// `app-region` (Document::appRegionAt) of the currentDocument() element Document::hitTest()
    /// finds at windowToDesign(x, y) -- None without a document. The default is installed by the
    /// constructor when WindowConfig::borderless is set, never otherwise.
    /// The callback may be called from inside the platform's message loop (e.g. while the OS
    /// drags or resizes the window): it must only read, never change App, Context or Document
    /// state.
    void setHitTest(std::function<HitRegion(int x, int y)> hitTest);
    /// T-376: window controls for a borderless host (UI `emit(...)` -> host -> these).
    void minimize();
    void maximize();
    void restore();
    /// restore() when isMaximized(), else maximize().
    void toggleMaximize();
    /// True while the window is maximized (SDL_WINDOW_MAXIMIZED).
    bool isMaximized() const;
    /// Pushes an SDL_QUIT event: run() / runFrame() end through the usual quit path. T-398: after
    /// the close-request handler allowed it (Window::close() of the main window).
    void close();

    /// T-387 (13 §13.9): opens `url` with the system's handler (browser, mail client, file
    /// manager; SDL_OpenURL). False (logged) when the platform cannot open it.
    bool openUrl(std::string_view url);
    /// T-387 (13 §13.9): the modifier keys held right now (SDL_GetModState), as
    /// InputEvent::modifiers bits (kShift / kCtrl / kAlt / kSuper).
    uint8_t modifierState() const;

    /// T-384a (23 §23.ad): the window App created at construction (window 0). Valid whenever
    /// ok() is true.
    Window& mainWindow() { return *m_windows[0]; }

    /// T-384b (23 §23.ad, 13 §13.5 T-384): opens another window with its own renderer (GL: its
    /// own GL context, GlRenderer and GlyphAtlas; SW: its own SDL_Renderer) and Context, sharing
    /// this App's text shaper and asset resolver. Returns its id (1, 2, ...), or kInvalidWindow
    /// with lastError() set when the window or its renderer cannot be created.
    WindowId createWindow(const WindowConfig& config);
    /// T-384b: the open window `id`; `id` must name one (findWindow() checks; aborts otherwise).
    Window& window(WindowId id);
    /// T-384b: the open window `id`, or null (never created, or closed).
    Window* findWindow(WindowId id);
    /// T-384b: closes window `id`. T-398 (23 §23.ad.1): through the same path as the user closing
    /// it -- the close-request handler first (false = vetoed, nothing happens), then the
    /// window-closed handler, then the window is destroyed. Window 0: the request handler, then
    /// quit(). Called during a frame (from onFrame, an event hook or a listener) the window is
    /// destroyed once the frame loop is done; otherwise at once.
    void closeWindow(WindowId id);
    /// T-384b: the ids of the open windows, window 0 first.
    std::vector<WindowId> windows() const;
    /// T-384b: called with the id of a window (never 0) that is closing -- T-398: closed by the
    /// user (SDL_WINDOWEVENT_CLOSE, during pollEvents()), Window::close() or closeWindow(), once
    /// the close-request handler let it -- before the window is destroyed (after the poll loop /
    /// frame loop when it happens inside one).
    void setWindowClosedHandler(std::function<void(WindowId)> handler) {
        m_windowClosed = std::move(handler);
    }
    /// T-398 (23 §23.ad.1): asked before any window closes -- the OS close request
    /// (SDL_WINDOWEVENT_CLOSE), Window::close() and closeWindow() alike, the main window
    /// included; returning false vetoes it (the window stays, the main window's run() goes on).
    /// An empty function (the default) allows every close. SDL_QUIT (a signal, the platform's
    /// quit command) still ends run() without asking; App turns off SDL's quit-on-last-window
    /// so a vetoed close of the last window does not quit anyway.
    void setWindowCloseRequestHandler(std::function<bool(WindowId)> handler) {
        m_closeRequest = std::move(handler);
    }
    /// T-384b: run() waits (SDL_Delay) at the end of each iteration until 1/fps seconds have
    /// passed since it began; 0 = unpaced. T-398 (23 §23.ad.1): returns false and changes nothing
    /// when `fps` > 0 and the main window has vsync on; true otherwise (`fps` <= 0 always turns
    /// pacing off). Pacing still waits while another window has vsync on (targetFpsActive()).
    bool setTargetFps(int fps);
    /// T-398: true when run() paces frames right now: a target is set and no open window has
    /// vsync on.
    bool targetFpsActive() const;
    /// T-398 (23 §23.ad.1): the window with the keyboard focus (tracked from
    /// SDL_WINDOWEVENT_FOCUS_GAINED / FOCUS_LOST), kInvalidWindow when none of this App's has it.
    WindowId focusedWindow() const;
    /// T-398 (23 §23.ad.1): display `index`'s name (SDL_GetDisplayName; empty on failure).
    std::string displayName(int index) const;
    /// T-398 (23 §23.ad.1): called during pollEvents() when a display is connected, disconnected
    /// or changes orientation (SDL_DISPLAYEVENT); the host queries displayCount() / displayBounds()
    /// again. An empty function (the default) removes it.
    void setDisplayChangedHandler(std::function<void()> handler) { m_displayChanged = std::move(handler); }

    /// T-384b: the number of displays (SDL_GetNumVideoDisplays; 0 on failure).
    int displayCount() const;
    /// T-384b: display `index`'s rectangle in SDL's global desktop coordinates (empty on failure).
    Rect displayBounds(int index) const;
    /// T-384b: as displayBounds() without the taskbar / dock / menu bar.
    Rect displayUsableBounds(int index) const;

    /// T-384b: run() whose `onFrame` is called once per window per frame (window 0 first) with
    /// that window's id. The single-argument run() calls its `onFrame` for window 0 only; the
    /// other windows are updated and rendered all the same.
    int run(const std::function<void(float dt, WindowId window)>& onFrame);
    /// T-384b: runFrame() with the per-window `onFrame` of run(float, WindowId).
    bool runFrame(const std::function<void(float dt, WindowId window)>& onFrame, float dt, bool swap);

private:
    friend class Window;

    using WindowFrameFn = std::function<void(float dt, WindowId window)>;
    /// T-384b: one frame for every window: poll (unless run from the live-resize watch) -> for
    /// each window: onFrame(dt, id) -> Window::renderFrame() -> text input for the focused window.
    bool runFrameAll(const WindowFrameFn& onFrame, float dt, bool swap);
    /// T-384b: the open window whose SDL window id is `sdlId` (0 = the main window); null when it
    /// is not one of ours (or is closing).
    Window* windowForSdlId(uint32_t sdlId);
    /// T-384b: the window `event` belongs to (its windowID field; window 0 for events without
    /// one); null for an event of a window that is gone or closing.
    Window* eventWindow(const SDL_Event& event);
    /// T-384b: the window with SDL_WINDOW_INPUT_FOCUS, else window 0 (gamepad and text input).
    Window& focusWindow();
    /// T-384b: destroys the windows closeWindow() / a CLOSE event marked (never window 0).
    void destroyClosedWindows();
    /// T-398: the one close path (OS request, Window::close(), closeWindow()): the request
    /// handler, then window 0 -> m_quit (or, `pushQuit`, an SDL_QUIT event as Window::close()
    /// always did), any other -> marked closing + the window-closed handler. False when vetoed.
    bool requestClose(Window& window, bool pushQuit);
    /// T-384b (T-33b moved here): SDL_StartTextInput / SDL_StopTextInput on the focus window's
    /// Context::wantsTextInput() edges (one global SDL text input state), plus
    /// SDL_SetTextInputRect at its caret. Called once per frame, after the windows rendered.
    void syncTextInput();
    /// T-384b: setTargetFps() pacing at the end of a run() iteration that began at `start`.
    void paceFrame(uint64_t start);

    /// T-377: the SDL event watch (SDL_AddEventWatch, installed with WindowConfig::liveResize).
    /// On SIZE_CHANGED / EXPOSED for the main window, on the main thread, during run() and outside
    /// any frame -- or while a frame is inside pollEvents(), which is where the OS modal size /
    /// move loop runs -- it syncs the viewport and runs one frame without polling. Returns 1.
    static int SDLCALL liveResizeWatch(void* userdata, SDL_Event* event);
    /// T-194: controller hot-plug and button/axis events; touch fingers into the recognizer.
    bool handleGamepadEvent(const SDL_Event& event);
    bool handleTouchEvent(const SDL_Event& event, Window& window);
    /// T-393 (08.14): SDL_DROPBEGIN / DROPFILE / DROPTEXT / DROPCOMPLETE (the non-Windows file
    /// drop) into the window the event names.
    void handleDropEvent(const SDL_Event& event);
    void pollEvents();

    bool m_ok = false;
    std::string m_error;
#ifndef XT_GL_NO_TEXT
    // Shared by every window (23 §23.ad): its Context shapes with it and its GlyphAtlas
    // rasterizes from it. Declared before m_windows so it outlives them.
    text::FtTextShaper m_textShaper;
#endif
    ShellAssetResolver m_assets;
    // T-384a: the windows; m_windows[0] is created by the constructor (none when SDL_Init failed).
    std::vector<std::unique_ptr<Window>> m_windows;
    bool m_quit = false;
    bool m_platformDrop = false;  // T-393: initPlatformFileDrop() ran (shut down in ~App).
    std::function<bool(const SDL_Event&)> m_eventHook;  // T-240.
    // T-194: open controllers (SDL instance id -> player slot).
    struct Pad {
        SDL_GameController* controller = nullptr;
        int32_t instance = -1;
        uint8_t player = 0;
    };
    std::vector<Pad> m_pads;
    bool m_gamepad = false;
    uint8_t m_maxPlayers = kMaxPlayers;
    // T-377: live resize. m_inRun: inside run(). T-384b: m_inFrame: inside a frame (poll and
    // every window's update / render); m_inPoll: that frame is inside pollEvents(); m_inWatch:
    // the frame was started by liveResizeWatch().
    bool m_liveResize = false;
    bool m_inRun = false;
    bool m_inFrame = false;
    bool m_inPoll = false;
    bool m_inWatch = false;
    SDL_threadID m_mainThread = 0;
    WindowFrameFn m_lastOnFrame;  // run()'s onFrame (a copy; the single-argument one wrapped).
    // T-384b: multi-window state.
    WindowId m_nextWindowId = 1;
    std::function<void(WindowId)> m_windowClosed;
    std::function<bool(WindowId)> m_closeRequest;  // T-398.
    std::function<void()> m_displayChanged;        // T-398.
    WindowId m_focusedWindow = kInvalidWindow;     // T-398: from FOCUS_GAINED / FOCUS_LOST.
    int m_targetFps = 0;
    bool m_targetFpsWarned = false;
    bool m_textInputActive = false;  // T-33b: the last SDL text input state App set.
    WindowId m_textInputWindow = 0;  // The focus window that state was set for.
};

}  // namespace xt::shell

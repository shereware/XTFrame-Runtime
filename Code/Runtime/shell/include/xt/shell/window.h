#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "xt/context.h"
#include "xt/document.h"
#include "xt/gesture.h"
#ifdef XT_SHELL_SW
#include "xt/sw/sw_renderer.h"
#else
#include "xt/gl/gl_renderer.h"
#endif

#if !defined(XT_GL_NO_TEXT) && !defined(XT_SHELL_SW)
#include "xt/text/glyph_atlas.h"
#endif

// SDL2 is a real third-party dependency of xt_shell (see Tools/CMake/xt_sdl2.cmake); included as
// SYSTEM by Code/Runtime/shell/CMakeLists.txt so its warnings don't count against xt_shell.
#include <SDL.h>

/// T-384a (Docs/design/23-host-integration.md §23.ad, 13-api.md §13.5 T-384): the per-window part
/// of xt_shell. A Window owns one SDL window, its renderer (GL: its own GL context, GlRenderer and
/// GlyphAtlas; SW: its own SDL_Renderer and streaming texture), its Context, fit targets, host
/// scene, text input state, hit-test and gesture recognizer. App creates and owns the Windows
/// (window 0 at construction) and keeps what is shared: SDL itself, the text shaper, the asset
/// resolver, gamepads and the event loop.

namespace xt::shell {

class App;

/// T-384b (13 §13.5 T-384): a window of an App. 0 is the main window (created by App's
/// constructor); App::createWindow() hands out 1, 2, ... (never reused).
using WindowId = uint32_t;
/// What App::createWindow() returns on failure.
inline constexpr WindowId kInvalidWindow = 0xFFFFFFFFu;

/// How App places the document in its window (Docs/tasks/T-156.md; the manifest's per-UI `fit`,
/// Docs/design/22-bundle.md §22.3). Native lays the document out at the window size (web-style
/// responsive layout); every other mode lays it out at a fixed design size, renders it offscreen
/// and places that image in the window.
enum class FitMode {
    /// The Context viewport follows the window (the behaviour before T-156).
    Native,
    /// The design-size image fills the whole window, ignoring its aspect ratio.
    Stretch,
    /// The design-size image is scaled uniformly to fit inside the window and centred.
    Contain,
    /// The design-size image is scaled uniformly to cover the window and centred (cropped).
    Cover,
    /// The design-size image is shown unscaled and centred (cropped when the window is smaller).
    Center,
};

/// T-241 (Docs/design/23-host-integration.md §23.11): the viewport edge App::setLogicalHeight()
/// divides by the design height, a UI Project's `spec.uiscaleRule` (height | width | shortEdge).
enum class UiScaleEdge {
    /// uiscale = viewport height / design height (T-210, the default).
    Height,
    /// uiscale = viewport width / design height.
    Width,
    /// uiscale = the shorter viewport edge / design height (portrait phones).
    ShortEdge,
    /// T-345 (25 §25.4.2 item 1): uiscale = min(viewport width / (design height x 16/9),
    /// viewport height / design height) -- bundle::uiScaleFor(UiScaleRule::Fit).
    Fit,
};

/// T-376 (Docs/design/23-host-integration.md §23.ab): what a borderless window's OS hit-test
/// answers for a point; one to one with SDL_HitTestResult.
enum class HitRegion {
    /// A normal client area (UI input).
    Normal,
    /// The OS drags the window (an `app-region: drag` element).
    Draggable,
    ResizeTopLeft,
    ResizeTop,
    ResizeTopRight,
    ResizeRight,
    ResizeBottomRight,
    ResizeBottom,
    ResizeBottomLeft,
    ResizeLeft,
};

/// T-398 (23 §23.ad.1): how a window covers its display.
enum class FullscreenMode {
    /// A normal window.
    Off,
    /// Exclusive fullscreen at the window size (SDL_WINDOW_FULLSCREEN; may change the display mode).
    Exclusive,
    /// Borderless at the desktop resolution (SDL_WINDOW_FULLSCREEN_DESKTOP; no mode change).
    Desktop,
};

/// T-398: WindowConfig::fullscreen's type -- a FullscreenMode that also takes the `bool` of the
/// field's earlier form (`true` = Exclusive, `false` = Off), so `config.fullscreen = true` and
/// `if (config.fullscreen)` keep working.
struct FullscreenOption {
    FullscreenMode mode = FullscreenMode::Off;
    constexpr FullscreenOption() = default;
    constexpr FullscreenOption(FullscreenMode m) : mode(m) {}  // NOLINT(google-explicit-constructor)
    constexpr FullscreenOption(bool on)                        // NOLINT(google-explicit-constructor)
        : mode(on ? FullscreenMode::Exclusive : FullscreenMode::Off) {}
    constexpr operator FullscreenMode() const { return mode; }  // NOLINT(google-explicit-constructor)
    constexpr explicit operator bool() const { return mode != FullscreenMode::Off; }
};

/// Parameters for the SDL2 window and GL context App creates.
struct WindowConfig {
    /// Initial window width, in logical (not necessarily pixel) units.
    int width = 1280;
    /// Initial window height, in logical (not necessarily pixel) units.
    int height = 720;
    /// Window title, shown in the OS title bar. UTF-8 (T-261f).
    const char* title = "XTFrame";
    /// T-240: create the window fullscreen (Android: immersive, the whole screen). T-398: a
    /// FullscreenMode (Desktop = SDL_WINDOW_FULLSCREEN_DESKTOP); `true` still means Exclusive.
    FullscreenOption fullscreen = FullscreenMode::Off;
    /// T-240: create the window hidden (SDL_WINDOW_HIDDEN), for a screenshot from automation.
    bool hidden = false;

    /// Requests a vsync-locked swap interval when true.
    bool vsync = true;
    /// Requested MSAA sample count for the GL context (0 disables multisampling).
    int msaa = 4;
    /// Requests a high-DPI-aware window (drawable size may then exceed `width`/`height`).
    bool highDpi = true;
    /// Initial placement (Docs/tasks/T-156.md); see App::setFit(). When it is not Native (and the
    /// design size is positive) the window itself is created without multisampling and `msaa`
    /// applies to the offscreen design-size target instead, because a blit into a multisampled
    /// window is not portable.
    FitMode fit = FitMode::Native;
    /// Design size used with a non-Native `fit`, in logical pixels.
    int designWidth = 0;
    /// Design size used with a non-Native `fit`, in logical pixels.
    int designHeight = 0;
    /// T-376 (23 §23.ab): create the window without OS decorations (SDL_WINDOW_BORDERLESS); App
    /// then installs its default hit-test (see App::setHitTest()).
    bool borderless = false;
    /// T-376: with `borderless`, Windows only: extend the DWM frame 1 px into the client area so
    /// the window keeps its drop shadow. No-op on other platforms.
    bool borderlessShadow = true;
    /// T-376: with `borderless`, the resize band along the window edges in window-logical
    /// pixels; 0 = no resizing from the edges.
    int resizeBorder = 8;
    /// T-377 (23 §23.ab, 13 §13.5): keep drawing while the OS runs its modal size / move loop
    /// (Windows, macOS), through an SDL event watch; see App::run(). The host's `onFrame` may then
    /// be called from inside SDL_PollEvent (still on the main thread, never re-entered).
    bool liveResize = true;
    /// T-384b (23 §23.ad): the initial window position in SDL's global desktop coordinates (see
    /// App::displayBounds()); SDL_WINDOWPOS_UNDEFINED lets the OS choose, and
    /// SDL_WINDOWPOS_CENTERED_DISPLAY(n) etc. work too.
    int x = SDL_WINDOWPOS_UNDEFINED;
    int y = SDL_WINDOWPOS_UNDEFINED;
    /// T-393 (08.14): files dragged in from the OS reach the window's Context as
    /// InputEvent::ExternalDragEnter / Move / Leave / ExternalDrop. Windows: an OLE IDropTarget
    /// (CF_HDROP) registered for the window -- hover included, the drag shows "copy" over a
    /// `drop-target` of the current document and "none" elsewhere; App calls OleInitialize once
    /// before creating its first window, so the host must not initialize COM / OLE on that thread
    /// itself in another apartment mode. Elsewhere: SDL's drop events, ExternalDrop only (no hover).
    bool acceptFileDrop = true;
    /// T-398 (23 §23.ad.1): whether a fullscreen window minimizes when it loses the focus
    /// (SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, set when the window is created; the hint is global,
    /// so the most recently created window's value applies to all).
    bool minimizeOnFocusLoss = false;
};

/// One SDL window with its renderer and Context (T-384a). Created only by App; App's per-window
/// methods forward to its window 0 (App::mainWindow()).
class Window {
public:
    /// Shuts down the renderer, destroys the GL context (or SDL_Renderer) and the window.
    ~Window();
    /// Non-copyable (owns the SDL window and its renderer).
    Window(const Window&) = delete;
    /// Non-copyable (owns the SDL window and its renderer).
    Window& operator=(const Window&) = delete;

    /// The Context this window drives each frame; owns the Documents loaded into it.
    Context& context() { return *m_context; }

    /// See App::setCurrentDocument().
    void setCurrentDocument(DocumentHandle handle) { m_currentDocument = handle; }
    /// The handle set by the most recent setCurrentDocument() call (0 if never called).
    DocumentHandle currentDocument() const { return m_currentDocument; }

    /// The window title (UTF-8).
    void setTitle(const char* title);

    /// drawable (framebuffer) size / window (logical) size.
    float devicePixelRatio() const;
    /// Framebuffer width in pixels.
    int drawableWidth() const { return m_drawableW; }
    /// Framebuffer height in pixels.
    int drawableHeight() const { return m_drawableH; }

    /// See App::syncViewport().
    void syncViewport();
    /// See App::setFit().
    void setFit(FitMode mode, int designWidth, int designHeight);
    /// The mode the latest setFit() selected (Native by default).
    FitMode fit() const { return m_fit; }
    /// See App::setLogicalHeight().
    void setLogicalHeight(float logicalHeight, UiScaleEdge edge = UiScaleEdge::Height);

    /// See App::setScene().
    void setScene(const uint8_t* rgba, int width, int height);
    /// See App::setSceneScroll().
    void setSceneScroll(int pxPerFrame) { m_sceneScroll = pxPerFrame; }
    /// True once setScene() installed a scene.
    bool hasScene() const { return !m_scene.empty(); }

    /// See App::readPixels().
    bool readPixels(std::vector<uint8_t>& rgba, int& width, int& height);

    /// See App::setStatsLog().
    void setStatsLog(bool on) { m_statsLog = on; }
    /// See App::setGestureConfig().
    void setGestureConfig(const GestureConfig& config) { m_gestures.setConfig(config); }

    /// See App::setHitTest().
    void setHitTest(std::function<HitRegion(int x, int y)> hitTest);
    /// See App::minimize() and the other window controls.
    void minimize();
    void maximize();
    void restore();
    void toggleMaximize();
    bool isMaximized() const;
    /// T-398 (23 §23.ad.1): asks App's close-request handler first (a veto = no-op). The main
    /// window then pushes an SDL_QUIT event (as App::close()); any other window is closed as by
    /// App::closeWindow(id()) (window-closed handler, then destroyed).
    void close();

    /// T-384b: this window's id in its App (0 = the main window).
    WindowId id() const { return m_id; }
    /// T-384b: the SDL window (for host code App does not wrap).
    SDL_Window* sdlWindow() const { return m_window; }
    /// T-384b: the window's position and size in SDL's global desktop coordinates (logical units).
    Rect bounds() const;
    /// T-384b: moves and resizes the window (SDL_SetWindowPosition + SDL_SetWindowSize).
    void setBounds(int x, int y, int width, int height);
    /// T-384b: the display the window is on (SDL_GetWindowDisplayIndex; -1 on failure).
    int displayIndex() const;
    /// T-384b: the DamageList the latest frame of this window produced (Context::render()).
    const DamageList& damage() const { return m_damage; }
    /// T-398 (23 §23.ad.1): switches fullscreen (SDL_SetWindowFullscreen); false (logged) when SDL
    /// refuses. The viewport follows through the resize events of the next frame.
    bool setFullscreen(FullscreenMode mode);
    /// T-398: the window's current fullscreen mode (from SDL_GetWindowFlags).
    FullscreenMode fullscreen() const;
    /// T-398 (23 §23.ad.1): brings the window to the front and asks for the input focus
    /// (SDL_RaiseWindow; the OS may only flash it in the taskbar).
    void raise();
    /// T-398: true while this window has the keyboard focus (App::focusedWindow() == id()).
    bool hasInputFocus() const;

private:
    /// T-393 (08.14): pushes an external drag event (`windowPos` in window-logical units, mapped
    /// through the fit) into the Context; true when the current document has a `drop-target`
    /// there (the OS drag's copy / none feedback). Called by the platform drop target.
    bool externalDrag(InputEvent::Kind kind, Vec2 windowPos, std::span<const std::string_view> paths);
    friend class App;

    /// Creates the SDL window, its renderer and its Context (called by App only). On failure
    /// m_error explains why; the object is then still safe to destroy.
    Window(App& app, const WindowConfig& config, WindowId id);
    /// T-384b: makes this window's GL context current (no-op when it already is, and under SW).
    /// Every method that touches GL calls it first, since each window has its own GL context.
    void makeCurrent();

    /// T-376: the SDL_HitTest callback; `data` is the Window.
    static SDL_HitTestResult sdlHitTest(SDL_Window* window, const SDL_Point* area, void* data);
    /// T-376: the default hit-test (setHitTest() with an empty function).
    HitRegion defaultHitTest(int x, int y) const;
    /// True while a non-Native fit with a positive design size is active.
    bool fitActive() const;
    /// (Re)creates the offscreen targets for a `width` x `height` pixel design image.
    bool ensureFitTargets(int width, int height);
    /// T-210: Context::setUiScale(viewport edge / m_logicalHeight) when a design height is set
    /// (T-241: the edge m_uiScaleEdge names).
    void applyLogicalHeight(float viewportWidth, float viewportHeight);
    /// Deletes the offscreen targets.
    void releaseFitTargets();
#ifdef XT_SHELL_SW
    /// T-240: renders m_list with xt_sw (damage-limited) and copies it to the window.
    void presentSw(bool swap);
    /// T-240: (re)creates the streaming texture and pixel buffer at `width` x `height` pixels.
    bool ensureSwTarget(int width, int height);
#endif
    /// T-240: accumulates one frame for setStatsLog() and logs once per second.
    void accumulateStats(double frameMs, double renderMs, double damageFraction, bool rendered);
    /// Maps a window-logical position into design coordinates under the active fit.
    Vec2 windowToDesign(Vec2 pos) const;
    /// T-33b: the caret rectangle for SDL_SetTextInputRect (window pixels); false when empty.
    /// T-384b: App starts / stops text input for the focused window (App::syncTextInput()).
    bool caretRect(SDL_Rect& out) const;
    // T-343: the scene image resampled to `w x h` (bilinear) into m_sceneScaled, once per size.
    void scaleScene(int w, int h);
#ifndef XT_SHELL_SW
    /// Draws the scene (wrapped by the scroll offset) into the bound draw framebuffer of
    /// `w x h` pixels, through a single-sample FBO the scene texture is attached to.
    void drawSceneGl(int w, int h);
    void releaseSceneGl();
#endif
    /// The per-window half of App::runFrame(), after onFrame: Context::update(dt) ->
    /// Context::render -> present (optionally swapping) -> stats. `frameStart` is the
    /// SDL_GetPerformanceCounter() value the frame started at (for setStatsLog()).
    void renderFrame(float dt, bool swap, uint64_t frameStart);

    App& m_app;
    WindowId m_id = 0;
    bool m_vsync = true;     // T-384b: WindowConfig::vsync (App::setTargetFps() needs it).
    bool m_closing = false;  // T-384b: closed; App destroys it outside the frame loop.
    std::string m_error;
    bool m_created = false;  // The constructor completed (App::ok() mirrors it).
    SDL_Window* m_window = nullptr;
#ifdef XT_SHELL_SW
    // T-240: the SW presentation path.
    SDL_Renderer* m_sdlRenderer = nullptr;
    SDL_Texture* m_texture = nullptr;
    sw::SwRenderer m_swRenderer;
    std::vector<uint8_t> m_pixels;  // Premultiplied RGBA8, m_texW x m_texH.
    int m_texW = 0;
    int m_texH = 0;
    bool m_uploadFull = true;  // The texture lost its content (recreated / device reset).
    DamageList m_swDamage;     // The rectangles redrawn this frame (texture pixels).
    double m_lastRenderMs = 0;
    double m_lastDamage = 0;
    bool m_lastRendered = false;
#else
    SDL_GLContext m_glContext = nullptr;
    gl::GlRenderer m_renderer;
#endif
    /// T-162: the frame's CommandList and DamageList persist across frames so Context::render()
    /// can diff against the previous list and GlRenderer can resubmit an unchanged frame.
    CommandList m_list;
    DamageList m_damage;
#if !defined(XT_GL_NO_TEXT) && !defined(XT_SHELL_SW)
    // GlyphAtlas injection (Docs/tasks/T-31.md): rasterizes from App's shared FtTextShaper and
    // feeds GlRenderer's DrawGlyphs atlas path; one per window (23 §23.ad: no shared GL context).
    text::GlyphAtlas m_glyphAtlas;
#endif
    std::unique_ptr<Context> m_context;
    DocumentHandle m_currentDocument = 0;
    bool m_composing = false;  // TEXTEDITING -> CompositionStart/End bookkeeping (§8.8).
    // T-393 (08.14): WindowConfig::acceptFileDrop; the platform drop target (Windows: the OLE
    // IDropTarget, nullptr elsewhere or when not registered); the SDL_DROPFILE paths collected
    // until SDL_DROPCOMPLETE (the non-Windows fallback).
    bool m_acceptFileDrop = true;
    void* m_fileDrop = nullptr;
    std::vector<std::string> m_dropPaths;
    int m_drawableW = 0;
    int m_drawableH = 0;
    // T-156 fit placement: the requested MSAA sample count (the offscreen target mirrors the
    // window's), the design size, the placement rect in window-logical units and the offscreen
    // targets (a resolve FBO, plus a multisampled one when m_msaa > 0).
    int m_msaa = 0;
    FitMode m_fit = FitMode::Native;
    int m_designW = 0;
    int m_designH = 0;
    float m_logicalHeight = 0;                        // T-210: setLogicalHeight().
    UiScaleEdge m_uiScaleEdge = UiScaleEdge::Height;  // T-241
    // T-240 (item 7): setStatsLog() accumulators.
    bool m_statsLog = false;
    uint32_t m_statsFrames = 0;
    uint32_t m_statsRendered = 0;
    double m_statsFrameMs = 0;
    double m_statsRenderMs = 0;
    double m_statsDamage = 0;
    double m_statsSkinned = 0;  // T-244: FrameStats::skinnedPoints summed (perf tracing on).
    double m_statsBones = 0;    // T-244: FrameStats::rigBones summed.
    uint32_t m_statsStart = 0;

    // T-343: the host scene (setScene), its resampled copy and the scroll state.
    std::vector<uint8_t> m_scene;  // straight RGBA8, m_sceneW x m_sceneH
    int m_sceneW = 0;
    int m_sceneH = 0;
    std::vector<uint8_t> m_sceneScaled;  // m_sceneScaledW x m_sceneScaledH
    int m_sceneScaledW = 0;
    int m_sceneScaledH = 0;
    int m_sceneScroll = 0;
    int m_sceneOffset = 0;
#ifndef XT_SHELL_SW
    unsigned m_sceneTex = 0;
    unsigned m_sceneFbo = 0;
    int m_sceneTexW = 0;
    int m_sceneTexH = 0;
#endif

    Rect m_place{0, 0, 0, 0};
    unsigned m_fitFbo = 0;
    unsigned m_fitColor = 0;
    unsigned m_fitDepthStencil = 0;
    unsigned m_fitMsaaFbo = 0;
    unsigned m_fitMsaaColor = 0;
    unsigned m_fitMsaaDepthStencil = 0;
    int m_fitW = 0;
    int m_fitH = 0;
    // T-194: the touch gesture recognizer (per window, 23 §23.ad).
    GestureRecognizer m_gestures;
    // T-376: the borderless window's resize band and the host's hit-test (empty = default).
    int m_resizeBorder = 8;
    std::function<HitRegion(int, int)> m_hitTest;
    // T-377: when this window's latest frame started. T-384b: the m_inFrame / m_inPoll / m_inWatch
    // flags moved to App (one poll and one frame loop for all windows).
    std::chrono::steady_clock::time_point m_lastFrameTime{};
};

}  // namespace xt::shell

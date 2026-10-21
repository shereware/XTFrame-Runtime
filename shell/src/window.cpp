#include "xt/shell/window.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <span>
#include <string>

#ifndef XT_SHELL_SW
#include "gl_loader.h"  // Private header (gl/src, see shell/CMakeLists.txt); shares the process-
                        // wide xt::gl::glClear/glViewport/... globals GlRenderer::init() loads.
#endif
#include "xt/shell/app.h"
#include "xt/sw/sw_png.h"

/// Window: one SDL window, its renderer and its Context (T-384a, docs/design/23-host-integration.md
/// §23.ad). The code moved here from app.cpp unchanged; App keeps SDL, the shared text shaper and
/// asset resolver, gamepads and the event loop.

namespace xt::shell {

// T-376: defined in platform_win.cpp (Windows) or platform_other.cpp (a no-op); no header.
void applyBorderlessShadow(SDL_Window* window);
void prepareBorderlessWindow();                    // Before SDL_CreateWindow.
void keepMaximizedInWorkArea(SDL_Window* window);  // Maximized = the monitor's work area.
// T-393 (08.14): the platform file drop target (platform_win.cpp: an OLE IDropTarget for the
// window's HWND; platform_other.cpp: nothing, nullptr -- SDL's drop events are the fallback).
// `drag` gets the event kind, the window-logical position and (ExternalDrop) the paths, and
// answers whether a drop target is there. revokeFileDrop(nullptr) is a no-op.
using ExternalDragFn = std::function<bool(InputEvent::Kind, Vec2, std::span<const std::string_view>)>;
void* registerFileDrop(SDL_Window* window, ExternalDragFn drag);
void revokeFileDrop(void* handle);

namespace {

#ifndef XT_SHELL_SW
void* sdlGetProcAddress(const char* name) { return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name)); }

/// A renderbuffer of `format`, multisampled when `samples` > 0.
gl::GLuint makeRenderbuffer(gl::GLenum format, int samples, int width, int height) {
    gl::GLuint rb = 0;
    gl::glGenRenderbuffers(1, &rb);
    gl::glBindRenderbuffer(gl::GL_RENDERBUFFER, rb);
    if (samples > 0) {
        gl::glRenderbufferStorageMultisample(gl::GL_RENDERBUFFER, samples, format, width, height);
    } else {
        gl::glRenderbufferStorage(gl::GL_RENDERBUFFER, format, width, height);
    }
    gl::glBindRenderbuffer(gl::GL_RENDERBUFFER, 0);
    return rb;
}

/// An FBO with `color` and `depthStencil` attached; 0 when it is incomplete.
gl::GLuint makeFramebuffer(gl::GLuint color, gl::GLuint depthStencil) {
    gl::GLuint fbo = 0;
    gl::glGenFramebuffers(1, &fbo);
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, fbo);
    gl::glFramebufferRenderbuffer(gl::GL_FRAMEBUFFER, gl::GL_COLOR_ATTACHMENT0, gl::GL_RENDERBUFFER, color);
    gl::glFramebufferRenderbuffer(gl::GL_FRAMEBUFFER, gl::GL_DEPTH_STENCIL_ATTACHMENT, gl::GL_RENDERBUFFER,
                                  depthStencil);
    const bool complete = gl::glCheckFramebufferStatus(gl::GL_FRAMEBUFFER) == gl::GL_FRAMEBUFFER_COMPLETE;
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, 0);
    if (!complete) {
        gl::glDeleteFramebuffers(1, &fbo);
        return 0;
    }
    return fbo;
}
#endif

/// The background every frame is cleared to (both presentation paths).
[[maybe_unused]] constexpr float kClearGray = 0.1f;  // Read only under XT_SHELL_SW.

/// T-398 (23 §23.ad.1): the SDL window flags of WindowConfig::fullscreen; also sets the global
/// SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS from WindowConfig::minimizeOnFocusLoss.
Uint32 prepareFullscreen(const WindowConfig& config) {
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, config.minimizeOnFocusLoss ? "1" : "0");
    switch (static_cast<FullscreenMode>(config.fullscreen)) {
        case FullscreenMode::Exclusive:
            return SDL_WINDOW_FULLSCREEN;
        case FullscreenMode::Desktop:
            return SDL_WINDOW_FULLSCREEN_DESKTOP;
        case FullscreenMode::Off:
            break;
    }
    return 0;
}

/// T-240: the fraction of `viewportPixels` the rectangles of `damage` cover (1 when full).
double damageFraction(const DamageList& damage, double viewportPixels) {
    if (damage.full) {
        return 1.0;
    }
    if (viewportPixels <= 0.0) {
        return 0.0;
    }
    double area = 0.0;
    for (const Rect& r : damage.rects) {
        area += static_cast<double>(std::max(0.0f, r.w)) * static_cast<double>(std::max(0.0f, r.h));
    }
    return std::min(1.0, area / viewportPixels);
}

}  // namespace

Window::Window(App& app, const WindowConfig& config, WindowId id)
    : m_app(app),
      m_id(id),
      m_vsync(config.vsync)
#if !defined(XT_GL_NO_TEXT) && !defined(XT_SHELL_SW)
      ,
      m_glyphAtlas(app.m_textShaper)
#endif
{
#ifdef XT_SHELL_SW
    // T-240: the SW presentation path -- a plain window and an SDL_Renderer; no GL context.
    m_msaa = 0;
    m_fit = config.fit;
    m_designW = config.designWidth;
    m_designH = config.designHeight;
    Uint32 flags = SDL_WINDOW_RESIZABLE;
    if (config.highDpi) {
        flags |= SDL_WINDOW_ALLOW_HIGHDPI;
    }
    flags |= prepareFullscreen(config);  // T-240; T-398: Exclusive or Desktop.
    if (config.hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    if (config.borderless) {
        flags |= SDL_WINDOW_BORDERLESS;  // T-376 (23 §23.ab).
        prepareBorderlessWindow();
    }
    m_window = SDL_CreateWindow(config.title, config.x, config.y, config.width, config.height, flags);
    if (m_window == nullptr) {
        m_error = SDL_GetError();
        return;
    }
    m_sdlRenderer =
        SDL_CreateRenderer(m_window, -1,
                           static_cast<Uint32>(SDL_RENDERER_ACCELERATED) |
                               (config.vsync ? static_cast<Uint32>(SDL_RENDERER_PRESENTVSYNC) : 0u));
    if (m_sdlRenderer == nullptr) {
        // No accelerated renderer (a headless desktop): SDL's own software renderer still presents.
        m_sdlRenderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (m_sdlRenderer == nullptr) {
        m_error = SDL_GetError();
        return;
    }
    SDL_GetRendererOutputSize(m_sdlRenderer, &m_drawableW, &m_drawableH);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    m_msaa = std::max(0, config.msaa);
    m_fit = config.fit;
    m_designW = config.designWidth;
    m_designH = config.designHeight;
    // T-156: under a fit the window only receives a blit; the offscreen target is multisampled.
    if (config.msaa > 0 && !fitActive()) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, config.msaa);
    } else {
        // T-384b: the attributes are global; an earlier window must not leave MSAA on.
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
    }

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
    if (config.highDpi) {
        flags |= SDL_WINDOW_ALLOW_HIGHDPI;
    }
    flags |= prepareFullscreen(config);  // T-398 (23 §23.ad.1).
    if (config.hidden) {
        flags |= SDL_WINDOW_HIDDEN;  // T-240.
    }
    if (config.borderless) {
        flags |= SDL_WINDOW_BORDERLESS;  // T-376 (23 §23.ab).
        prepareBorderlessWindow();
    }
    m_window = SDL_CreateWindow(config.title, config.x, config.y, config.width, config.height, flags);
    if (m_window == nullptr) {
        m_error = SDL_GetError();
        return;
    }

    m_glContext = SDL_GL_CreateContext(m_window);
    if (m_glContext == nullptr) {
        m_error = SDL_GetError();
        return;
    }
    if (SDL_GL_MakeCurrent(m_window, m_glContext) != 0) {
        m_error = SDL_GetError();
        return;
    }
    SDL_GL_SetSwapInterval(config.vsync ? 1 : 0);

    if (!m_renderer.init(&sdlGetProcAddress)) {
        m_error = m_renderer.lastError();
        return;
    }

    SDL_GL_GetDrawableSize(m_window, &m_drawableW, &m_drawableH);
    m_renderer.resize(m_drawableW, m_drawableH);
#ifndef XT_GL_NO_TEXT
    m_renderer.setGlyphAtlas(&m_glyphAtlas);
#endif
#endif  // XT_SHELL_SW

    ContextConfig ctxConfig;
#ifndef XT_GL_NO_TEXT
    // docs/tasks/T-30.md goal 5: the shaper Text nodes shape/measure with is the same
    // FtTextShaper the GL glyph atlas rasterizes from, and the one Document registers
    // `@font-face` assets into.
    ctxConfig.shaper = &m_app.m_textShaper;
#else
    ctxConfig.shaper = nullptr;  // Built without xt_text: no text.
#endif
    ctxConfig.assets = &m_app.m_assets;
    m_context = std::make_unique<Context>(ctxConfig);
    // T-191 (25.3.5b): xt_shell carries no video decoder, so an unbound <video>/<external> shows a
    // placeholder (checkerboard, name, duration); image sequences play as usual.
    m_context->setExternalPlaceholders(true);

    int logicalW = 0, logicalH = 0;
    SDL_GetWindowSize(m_window, &logicalW, &logicalH);
    m_context->setViewport(Rect{0, 0, static_cast<float>(logicalW), static_cast<float>(logicalH)},
                           devicePixelRatio());

#ifdef XT_SHELL_SW
    syncViewport();  // Creates the streaming texture (and the fit placement).
#else
    if (fitActive()) {
        syncViewport();
    }
#endif
    // T-376 (23 §23.ab): a borderless window gets the default hit-test (drag / resize regions)
    // and, on Windows, the DWM shadow.
    m_resizeBorder = std::max(0, config.resizeBorder);
    if (config.borderless) {
        setHitTest({});
        keepMaximizedInWorkArea(m_window);
        if (config.borderlessShadow) {
            applyBorderlessShadow(m_window);
        }
    }
    // T-393 (08.14): files dragged in from the OS.
    m_acceptFileDrop = config.acceptFileDrop;
    if (m_acceptFileDrop) {
        m_fileDrop = registerFileDrop(
            m_window, [this](InputEvent::Kind kind, Vec2 pos, std::span<const std::string_view> paths) {
                return externalDrag(kind, pos, paths);
            });
    }
    m_statsStart = SDL_GetTicks();
    m_created = true;
}

bool Window::externalDrag(InputEvent::Kind kind, Vec2 windowPos, std::span<const std::string_view> paths) {
    if (m_context == nullptr || m_closing) {
        return false;
    }
    InputEvent event;
    event.kind = kind;
    event.pos = windowToDesign(windowPos);  // T-156: identity under Native.
    event.paths = paths;                    // Copied by pushInput.
    m_context->pushInput(event);
    if (kind == InputEvent::ExternalDragLeave) {
        return false;
    }
    Document* doc = m_context->document(m_currentDocument);
    return doc != nullptr && doc->hasDropTargetAt(event.pos);
}

Window::~Window() {
#ifdef XT_SHELL_SW
    if (m_texture != nullptr) {
        SDL_DestroyTexture(m_texture);
    }
    if (m_sdlRenderer != nullptr) {
        SDL_DestroyRenderer(m_sdlRenderer);
    }
#else
    if (m_glContext != nullptr) {
        makeCurrent();  // T-384b: this window's GL objects live in its own context.
        releaseFitTargets();
        releaseSceneGl();
        m_renderer.shutdown();
    }
    if (m_glContext != nullptr) {
        SDL_GL_DeleteContext(m_glContext);
    }
#endif
    revokeFileDrop(m_fileDrop);  // T-393: before the HWND goes.
    m_fileDrop = nullptr;
    if (m_window != nullptr) {
        SDL_DestroyWindow(m_window);
    }
}

void Window::makeCurrent() {
#ifndef XT_SHELL_SW
    if (m_glContext != nullptr &&
        (SDL_GL_GetCurrentContext() != m_glContext || SDL_GL_GetCurrentWindow() != m_window)) {
        SDL_GL_MakeCurrent(m_window, m_glContext);
    }
#endif
}

Rect Window::bounds() const {
    int x = 0, y = 0, w = 0, h = 0;
    SDL_GetWindowPosition(m_window, &x, &y);
    SDL_GetWindowSize(m_window, &w, &h);
    return Rect{static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)};
}

void Window::setBounds(int x, int y, int width, int height) {
    if (m_window == nullptr) {
        return;
    }
    SDL_SetWindowPosition(m_window, x, y);
    SDL_SetWindowSize(m_window, width, height);
}

int Window::displayIndex() const { return m_window != nullptr ? SDL_GetWindowDisplayIndex(m_window) : -1; }

void Window::setTitle(const char* title) {
    if (m_window != nullptr) {
        SDL_SetWindowTitle(m_window, title);
    }
}

float Window::devicePixelRatio() const {
    int logicalW = 0, logicalH = 0;
    SDL_GetWindowSize(m_window, &logicalW, &logicalH);
    if (logicalW <= 0) {
        return 1.0f;
    }
    return static_cast<float>(m_drawableW) / static_cast<float>(logicalW);
}

void Window::syncViewport() {
    makeCurrent();
#ifdef XT_SHELL_SW
    SDL_GetRendererOutputSize(m_sdlRenderer, &m_drawableW, &m_drawableH);
#else
    SDL_GL_GetDrawableSize(m_window, &m_drawableW, &m_drawableH);
#endif
    int logicalW = 0, logicalH = 0;
    SDL_GetWindowSize(m_window, &logicalW, &logicalH);
    const float dpr = devicePixelRatio();
    if (fitActive()) {
        const int width = std::max(1, static_cast<int>(static_cast<float>(m_designW) * dpr + 0.5f));
        const int height = std::max(1, static_cast<int>(static_cast<float>(m_designH) * dpr + 0.5f));
#ifdef XT_SHELL_SW
        const bool targets = ensureSwTarget(width, height);
#else
        const bool targets = ensureFitTargets(width, height);
#endif
        if (targets) {
            m_place = fitPlacement(m_fit, static_cast<float>(m_designW), static_cast<float>(m_designH),
                                   static_cast<float>(logicalW), static_cast<float>(logicalH));
#ifndef XT_SHELL_SW
            m_renderer.resize(width, height);
#endif
            m_context->setViewport(Rect{0, 0, static_cast<float>(m_designW), static_cast<float>(m_designH)},
                                   dpr);
            applyLogicalHeight(static_cast<float>(m_designW), static_cast<float>(m_designH));
            return;
        }
        // No offscreen target (incomplete FBO): fall back to the window-sized viewport.
        m_fit = FitMode::Native;
    }
#ifdef XT_SHELL_SW
    ensureSwTarget(std::max(1, m_drawableW), std::max(1, m_drawableH));
#else
    m_renderer.resize(m_drawableW, m_drawableH);
#endif
    m_context->setViewport(Rect{0, 0, static_cast<float>(logicalW), static_cast<float>(logicalH)}, dpr);
    applyLogicalHeight(static_cast<float>(logicalW), static_cast<float>(logicalH));
}

void Window::setLogicalHeight(float logicalHeight, UiScaleEdge edge) {
    m_logicalHeight = logicalHeight > 0.0f ? logicalHeight : 0.0f;
    m_uiScaleEdge = edge;
    syncViewport();
}

void Window::applyLogicalHeight(float viewportWidth, float viewportHeight) {
    // T-210: output height / design height; T-241: or the width / the short edge.
    // T-345: Fit compares the width against the 16:9 design width (bundle::kFitDesignAspect).
    const float edge = m_uiScaleEdge == UiScaleEdge::Width       ? viewportWidth
                       : m_uiScaleEdge == UiScaleEdge::ShortEdge ? std::min(viewportWidth, viewportHeight)
                       : m_uiScaleEdge == UiScaleEdge::Fit
                           ? std::min(viewportWidth * 9.0f / 16.0f, viewportHeight)
                           : viewportHeight;
    if (m_logicalHeight > 0.0f && edge > 0.0f) {
        m_context->setUiScale(edge / m_logicalHeight);
    }
}

void Window::setFit(FitMode mode, int designWidth, int designHeight) {
    makeCurrent();
    m_fit = mode;
    m_designW = designWidth;
    m_designH = designHeight;
#ifndef XT_SHELL_SW
    if (!fitActive()) {
        releaseFitTargets();
    }
#endif
    syncViewport();
}

bool Window::fitActive() const { return m_fit != FitMode::Native && m_designW > 0 && m_designH > 0; }

#ifdef XT_SHELL_SW
bool Window::ensureSwTarget(int width, int height) {
    if (m_texture != nullptr && m_texW == width && m_texH == height) {
        return true;
    }
    if (m_texture != nullptr) {
        SDL_DestroyTexture(m_texture);
        m_texture = nullptr;
    }
    m_texture =
        SDL_CreateTexture(m_sdlRenderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (m_texture == nullptr) {
        m_texW = m_texH = 0;
        return false;
    }
    // The buffer is opaque (cleared to kClearGray), so the copy needs no blending; a fit scales it.
    SDL_SetTextureBlendMode(m_texture, SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(m_texture, SDL_ScaleModeLinear);
    if (m_texW != width || m_texH != height) {
        m_pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
        sw::SwTarget target;
        target.rgba = m_pixels.data();
        target.width = width;
        target.height = height;
        target.strideBytes = width * 4;
        target.premultiplied = true;  // T-239: the cheapest SW target; the texture takes it as is.
        m_swRenderer.setTarget(target);
        m_texW = width;
        m_texH = height;
    }
    m_uploadFull = true;
    return true;
}

void Window::presentSw(bool swap) {
    const Color background{kClearGray, kClearGray, kClearGray, 1.0f};
    Document* doc = m_context->document(m_currentDocument);
    const IrView* ir = doc != nullptr ? &doc->ir() : nullptr;
#ifndef XT_GL_NO_TEXT
    TextShaper* shaper = &m_app.m_textShaper;
#else
    TextShaper* shaper = nullptr;
#endif
    // T-343: a scene changes every frame (the scroll), so Direct mode always draws the whole
    // target: the scene is copied in, the renderer pastes its bands and draws the scene readers.
    const bool full = m_uploadFull || m_damage.full || hasScene();
    const double pixels = static_cast<double>(m_texW) * static_cast<double>(m_texH);
    m_lastRendered = false;
    m_lastRenderMs = 0.0;
    m_lastDamage = 0.0;
    if (m_texture != nullptr && (full || !m_damage.rects.empty())) {
        const Uint64 t0 = SDL_GetPerformanceCounter();
        if (full) {
            if (hasScene()) {
                scaleScene(m_texW, m_texH);
                const int w = m_sceneScaledW;
                for (int y = 0; y < m_texH && y < m_sceneScaledH; ++y) {
                    const uint8_t* src =
                        m_sceneScaled.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u;
                    uint8_t* dst =
                        m_pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(m_texW) * 4u;
                    for (int x = 0; x < m_texW; ++x) {
                        const int sx = (x + m_sceneOffset) % std::max(1, w);
                        const uint8_t* p = src + static_cast<size_t>(sx) * 4u;
                        // Premultiplied target: the scene is drawn opaque.
                        dst[static_cast<size_t>(x) * 4u + 0] = p[0];
                        dst[static_cast<size_t>(x) * 4u + 1] = p[1];
                        dst[static_cast<size_t>(x) * 4u + 2] = p[2];
                        dst[static_cast<size_t>(x) * 4u + 3] = 255;
                    }
                }
                m_sceneOffset = (m_sceneOffset + m_sceneScroll) % std::max(1, w);
                m_swRenderer.render(m_list, shaper, &m_app.m_assets, ir, &m_context->perf(), &m_damage);
            } else {
                m_swRenderer.clear(background);
                m_swRenderer.render(m_list, shaper, &m_app.m_assets, ir, &m_context->perf(), nullptr);
            }
            SDL_UpdateTexture(m_texture, nullptr, m_pixels.data(), m_texW * 4);
            m_lastDamage = 1.0;
        } else {
            // T-239 (mirrored): only the damaged pixels are cleared and redrawn, everything else
            // keeps the previous frame; then only those rectangles are uploaded.
            m_swDamage.full = false;
            m_swDamage.rects = m_damage.rects;
            m_swDamage.viewport = Rect{0, 0, static_cast<float>(m_texW), static_cast<float>(m_texH)};
            m_swRenderer.clearRects(background, m_swDamage.rects);
            m_swRenderer.render(m_list, shaper, &m_app.m_assets, ir, &m_context->perf(), &m_swDamage);
            for (const Rect& r : m_swDamage.rects) {
                // Snapped outwards and clamped like clearRects() does.
                const int x0 = std::clamp(static_cast<int>(std::floor(r.x)), 0, m_texW);
                const int y0 = std::clamp(static_cast<int>(std::floor(r.y)), 0, m_texH);
                const int x1 = std::clamp(static_cast<int>(std::ceil(r.x + r.w)), 0, m_texW);
                const int y1 = std::clamp(static_cast<int>(std::ceil(r.y + r.h)), 0, m_texH);
                if (x1 <= x0 || y1 <= y0) {
                    continue;
                }
                const SDL_Rect area{x0, y0, x1 - x0, y1 - y0};
                const uint8_t* src =
                    m_pixels.data() +
                    (static_cast<size_t>(y0) * static_cast<size_t>(m_texW) + static_cast<size_t>(x0)) * 4u;
                SDL_UpdateTexture(m_texture, &area, src, m_texW * 4);
            }
            m_lastDamage = damageFraction(m_swDamage, pixels);
        }
        m_uploadFull = false;
        m_lastRendered = true;
        m_lastRenderMs = static_cast<double>(SDL_GetPerformanceCounter() - t0) * 1000.0 /
                         static_cast<double>(SDL_GetPerformanceFrequency());
    }

    // The window is redrawn from the texture every frame (cheap on the GPU; a back buffer's
    // content is undefined after a present), which also keeps the loop paced by vsync.
    SDL_SetRenderDrawColor(m_sdlRenderer, 0, 0, 0, 255);
    SDL_RenderClear(m_sdlRenderer);
    if (m_texture != nullptr) {
        if (fitActive()) {
            const float dpr = devicePixelRatio();
            const SDL_Rect dst{static_cast<int>(std::lround(m_place.x * dpr)),
                               static_cast<int>(std::lround(m_place.y * dpr)),
                               static_cast<int>(std::lround(m_place.w * dpr)),
                               static_cast<int>(std::lround(m_place.h * dpr))};
            SDL_RenderCopy(m_sdlRenderer, m_texture, nullptr, &dst);
        } else {
            SDL_RenderCopy(m_sdlRenderer, m_texture, nullptr, nullptr);
        }
    }
    if (swap) {
        SDL_RenderPresent(m_sdlRenderer);
    }
}
#else
bool Window::ensureFitTargets(int width, int height) {
    if (m_fitFbo != 0 && m_fitW == width && m_fitH == height) {
        return true;
    }
    releaseFitTargets();
    m_fitColor = makeRenderbuffer(gl::GL_RGBA8, 0, width, height);
    m_fitDepthStencil = makeRenderbuffer(gl::GL_DEPTH24_STENCIL8, 0, width, height);
    m_fitFbo = makeFramebuffer(m_fitColor, m_fitDepthStencil);
    if (m_msaa > 0) {
        m_fitMsaaColor = makeRenderbuffer(gl::GL_RGBA8, m_msaa, width, height);
        m_fitMsaaDepthStencil = makeRenderbuffer(gl::GL_DEPTH24_STENCIL8, m_msaa, width, height);
        m_fitMsaaFbo = makeFramebuffer(m_fitMsaaColor, m_fitMsaaDepthStencil);
    }
    if (m_fitFbo == 0 || (m_msaa > 0 && m_fitMsaaFbo == 0)) {
        releaseFitTargets();
        return false;
    }
    m_fitW = width;
    m_fitH = height;
    return true;
}

void Window::releaseFitTargets() {
    const gl::GLuint fbos[] = {m_fitFbo, m_fitMsaaFbo};
    for (gl::GLuint fbo : fbos) {
        if (fbo != 0) {
            gl::glDeleteFramebuffers(1, &fbo);
        }
    }
    const gl::GLuint rbs[] = {m_fitColor, m_fitDepthStencil, m_fitMsaaColor, m_fitMsaaDepthStencil};
    for (gl::GLuint rb : rbs) {
        if (rb != 0) {
            gl::glDeleteRenderbuffers(1, &rb);
        }
    }
    m_fitFbo = m_fitColor = m_fitDepthStencil = 0;
    m_fitMsaaFbo = m_fitMsaaColor = m_fitMsaaDepthStencil = 0;
    m_fitW = m_fitH = 0;
}
#endif  // XT_SHELL_SW

// -------------------------------------------------------------------------------------------
// T-343: the host scene (docs/design/23-host-integration.md §23.aa.5).
// -------------------------------------------------------------------------------------------

void Window::setScene(const uint8_t* rgba, int width, int height) {
    makeCurrent();
    m_scene.clear();
    m_sceneW = m_sceneH = 0;
    m_sceneScaled.clear();
    m_sceneScaledW = m_sceneScaledH = 0;
    m_sceneOffset = 0;
    if (rgba != nullptr && width > 0 && height > 0) {
        m_scene.assign(rgba, rgba + static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
        m_sceneW = width;
        m_sceneH = height;
    }
    m_context->setHostTargetMode(hasScene() ? HostTargetMode::Direct : HostTargetMode::Layer);
#ifdef XT_SHELL_SW
    m_uploadFull = true;
#else
    releaseSceneGl();
#endif
}

void Window::scaleScene(int w, int h) {
    if (m_scene.empty() || w <= 0 || h <= 0) {
        return;
    }
    if (m_sceneScaledW == w && m_sceneScaledH == h) {
        return;
    }
    m_sceneScaled.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
    for (int y = 0; y < h; ++y) {
        const float fy =
            (static_cast<float>(y) + 0.5f) * static_cast<float>(m_sceneH) / static_cast<float>(h) - 0.5f;
        const int sy0 = std::clamp(static_cast<int>(std::floor(fy)), 0, m_sceneH - 1);
        const int sy1 = std::clamp(sy0 + 1, 0, m_sceneH - 1);
        const float ty = std::clamp(fy - static_cast<float>(sy0), 0.0f, 1.0f);
        for (int x = 0; x < w; ++x) {
            const float fx =
                (static_cast<float>(x) + 0.5f) * static_cast<float>(m_sceneW) / static_cast<float>(w) - 0.5f;
            const int sx0 = std::clamp(static_cast<int>(std::floor(fx)), 0, m_sceneW - 1);
            const int sx1 = std::clamp(sx0 + 1, 0, m_sceneW - 1);
            const float tx = std::clamp(fx - static_cast<float>(sx0), 0.0f, 1.0f);
            uint8_t* o = m_sceneScaled.data() +
                         (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
            for (int c = 0; c < 4; ++c) {
                auto at = [&](int sx, int sy) {
                    return static_cast<float>(
                        m_scene[(static_cast<size_t>(sy) * static_cast<size_t>(m_sceneW) +
                                 static_cast<size_t>(sx)) *
                                    4u +
                                static_cast<size_t>(c)]);
                };
                const float top = at(sx0, sy0) + (at(sx1, sy0) - at(sx0, sy0)) * tx;
                const float bottom = at(sx0, sy1) + (at(sx1, sy1) - at(sx0, sy1)) * tx;
                o[c] = static_cast<uint8_t>(std::clamp(top + (bottom - top) * ty + 0.5f, 0.0f, 255.0f));
            }
        }
    }
    m_sceneScaledW = w;
    m_sceneScaledH = h;
}

#ifndef XT_SHELL_SW
void Window::releaseSceneGl() {
    if (m_sceneFbo != 0) {
        gl::GLuint fbo = m_sceneFbo;
        gl::glDeleteFramebuffers(1, &fbo);
    }
    if (m_sceneTex != 0) {
        gl::GLuint tex = m_sceneTex;
        gl::glDeleteTextures(1, &tex);
    }
    m_sceneFbo = m_sceneTex = 0;
    m_sceneTexW = m_sceneTexH = 0;
}

void Window::drawSceneGl(int w, int h) {
    if (w <= 0 || h <= 0) {
        return;
    }
    if (m_sceneTex == 0 || m_sceneTexW != w || m_sceneTexH != h) {
        releaseSceneGl();
        scaleScene(w, h);
        gl::GLuint tex = 0;
        gl::glGenTextures(1, &tex);
        gl::glBindTexture(gl::GL_TEXTURE_2D, tex);
        gl::glTexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MIN_FILTER,
                            static_cast<gl::GLint>(gl::GL_NEAREST));
        gl::glTexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MAG_FILTER,
                            static_cast<gl::GLint>(gl::GL_NEAREST));
        gl::glPixelStorei(gl::GL_UNPACK_ALIGNMENT, 1);
        // Row 0 of the image is the top; GL's row 0 is the bottom: upload flipped so a 1:1 blit
        // lands the scene the right way up.
        std::vector<uint8_t> flipped(m_sceneScaled.size());
        const size_t pitch = static_cast<size_t>(w) * 4u;
        for (int y = 0; y < h; ++y) {
            std::memcpy(flipped.data() + static_cast<size_t>(y) * pitch,
                        m_sceneScaled.data() + static_cast<size_t>(h - 1 - y) * pitch, pitch);
        }
        gl::glTexImage2D(gl::GL_TEXTURE_2D, 0, static_cast<gl::GLint>(gl::GL_RGBA8), w, h, 0, gl::GL_RGBA,
                         gl::GL_UNSIGNED_BYTE, flipped.data());
        gl::glPixelStorei(gl::GL_UNPACK_ALIGNMENT, 4);
        gl::GLuint fbo = 0;
        gl::glGenFramebuffers(1, &fbo);
        gl::glBindFramebuffer(gl::GL_READ_FRAMEBUFFER, fbo);
        gl::glFramebufferTexture2D(gl::GL_READ_FRAMEBUFFER, gl::GL_COLOR_ATTACHMENT0, gl::GL_TEXTURE_2D, tex,
                                   0);
        m_sceneTex = tex;
        m_sceneFbo = fbo;
        m_sceneTexW = w;
        m_sceneTexH = h;
    }
    // The draw framebuffer (the window, or the fit target) receives the scene; the read binding
    // is put back on it afterwards, since readPixels() and the host read through it.
    gl::GLint drawFbo = 0;
    gl::glGetIntegerv(gl::GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
    gl::glBindFramebuffer(gl::GL_READ_FRAMEBUFFER, m_sceneFbo);
    gl::glDisable(gl::GL_SCISSOR_TEST);
    const int ox = w > 0 ? ((m_sceneOffset % w) + w) % w : 0;
    // Wrapped: [ox, w) lands at the left, [0, ox) at the right (1:1 blits, so a multisampled
    // window accepts them).
    gl::glBlitFramebuffer(ox, 0, w, h, 0, 0, w - ox, h, gl::GL_COLOR_BUFFER_BIT, gl::GL_NEAREST);
    if (ox > 0) {
        gl::glBlitFramebuffer(0, 0, ox, h, w - ox, 0, w, h, gl::GL_COLOR_BUFFER_BIT, gl::GL_NEAREST);
    }
    gl::glBindFramebuffer(gl::GL_READ_FRAMEBUFFER, static_cast<gl::GLuint>(drawFbo));
    gl::glClearStencil(0);
    gl::glClear(gl::GL_STENCIL_BUFFER_BIT);
    m_sceneOffset = (m_sceneOffset + m_sceneScroll) % std::max(1, w);
}
#endif

bool Window::readPixels(std::vector<uint8_t>& rgba, int& width, int& height) {
    makeCurrent();
#ifdef XT_SHELL_SW
    // The buffer is opaque (cleared to kClearGray), so premultiplied and straight are the same.
    if (!fitActive() && !m_pixels.empty()) {
        // Native: the window shows the buffer 1:1; reading the CPU copy also works hidden.
        rgba = m_pixels;
        width = m_texW;
        height = m_texH;
        return true;
    }
    int w = 0;

    int h = 0;
    if (SDL_GetRendererOutputSize(m_sdlRenderer, &w, &h) != 0 || w <= 0 || h <= 0) {
        return false;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    if (SDL_RenderReadPixels(m_sdlRenderer, nullptr, SDL_PIXELFORMAT_RGBA32, pixels.data(), w * 4) != 0) {
        return false;
    }
    rgba = std::move(pixels);
    width = w;
    height = h;
    return true;
#else
    // glReadPixels reads bottom-up; flip rows into top-down order.
    const int w = m_drawableW;
    const int h = m_drawableH;
    if (w <= 0 || h <= 0) {
        return false;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    gl::glPixelStorei(gl::GL_PACK_ALIGNMENT, 1);
    gl::glReadPixels(0, 0, w, h, gl::GL_RGBA, gl::GL_UNSIGNED_BYTE, pixels.data());
    const size_t rowBytes = static_cast<size_t>(w) * 4u;
    rgba.resize(pixels.size());
    for (int y = 0; y < h; ++y) {
        std::memcpy(rgba.data() + static_cast<size_t>(y) * rowBytes,
                    pixels.data() + static_cast<size_t>(h - 1 - y) * rowBytes, rowBytes);
    }
    width = w;
    height = h;
    return true;
#endif
}

void Window::accumulateStats(double frameMs, double renderMs, double damage, bool rendered) {
    if (!m_statsLog) {
        return;
    }
    ++m_statsFrames;
    m_statsFrameMs += frameMs;
    if (m_context && m_context->perf().enabled()) {  // T-244: the rig workload.
        m_statsSkinned += static_cast<double>(m_context->perf().current().skinnedPoints);
        m_statsBones += static_cast<double>(m_context->perf().current().rigBones);
    }
    if (rendered) {
        ++m_statsRendered;
        m_statsRenderMs += renderMs;
        m_statsDamage += damage;
    }
    const uint32_t now = SDL_GetTicks();
    if (now - m_statsStart < 1000u) {
        return;
    }
    const double frames = static_cast<double>(m_statsFrames);
    const double rendered_ = static_cast<double>(m_statsRendered);
    // T-384b: window 0 keeps the plain prefix; another window's lines name it (xt-stats[1]: ...).
    char prefix[32];
    if (m_id == 0) {
        std::snprintf(prefix, sizeof(prefix), "xt-stats");
    } else {
        std::snprintf(prefix, sizeof(prefix), "xt-stats[%u]", m_id);
    }
    char line[256];
    std::snprintf(
        line, sizeof(line),
        "%s: %u frames in %.2f s, %.2f ms/frame, %u rendered (%.2f ms render, damage %.1f%% of %dx%d)",
        prefix, m_statsFrames, static_cast<double>(now - m_statsStart) / 1000.0, m_statsFrameMs / frames,
        m_statsRendered, rendered_ > 0 ? m_statsRenderMs / rendered_ : 0.0,
        rendered_ > 0 ? 100.0 * m_statsDamage / rendered_ : 0.0, m_drawableW, m_drawableH);
#ifdef __ANDROID__
    SDL_Log("%s", line);  // logcat.
#else
    std::fprintf(stderr, "%s\n", line);  // SDL_Log does not reach a redirected Windows console.
#endif
    if (m_statsBones > 0.0) {  // T-244 (32 §32.3): bones evaluated and points re-skinned.
        std::snprintf(line, sizeof(line), "%s: rig %.1f bones/frame, %.1f skinned points/frame", prefix,
                      m_statsBones / frames, m_statsSkinned / frames);
#ifdef __ANDROID__
        SDL_Log("%s", line);
#else
        std::fprintf(stderr, "%s\n", line);
#endif
    }
    m_statsFrames = m_statsRendered = 0;
    m_statsFrameMs = m_statsRenderMs = m_statsDamage = 0.0;
    m_statsSkinned = m_statsBones = 0.0;
    m_statsStart = now;
}
void Window::setHitTest(std::function<HitRegion(int x, int y)> hitTest) {
    m_hitTest = std::move(hitTest);
    if (m_window != nullptr && SDL_SetWindowHitTest(m_window, &Window::sdlHitTest, this) != 0) {
        SDL_Log("xt_shell: SDL_SetWindowHitTest failed: %s", SDL_GetError());
    }
}

SDL_HitTestResult Window::sdlHitTest(SDL_Window* /*window*/, const SDL_Point* area, void* data) {
    // Called from inside the platform's message handling: read only (see setHitTest()).
    const Window* self = static_cast<const Window*>(data);
    const HitRegion region =
        self->m_hitTest ? self->m_hitTest(area->x, area->y) : self->defaultHitTest(area->x, area->y);
    switch (region) {
        case HitRegion::Normal:
            return SDL_HITTEST_NORMAL;
        case HitRegion::Draggable:
            return SDL_HITTEST_DRAGGABLE;
        case HitRegion::ResizeTopLeft:
            return SDL_HITTEST_RESIZE_TOPLEFT;
        case HitRegion::ResizeTop:
            return SDL_HITTEST_RESIZE_TOP;
        case HitRegion::ResizeTopRight:
            return SDL_HITTEST_RESIZE_TOPRIGHT;
        case HitRegion::ResizeRight:
            return SDL_HITTEST_RESIZE_RIGHT;
        case HitRegion::ResizeBottomRight:
            return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        case HitRegion::ResizeBottom:
            return SDL_HITTEST_RESIZE_BOTTOM;
        case HitRegion::ResizeBottomLeft:
            return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        case HitRegion::ResizeLeft:
            return SDL_HITTEST_RESIZE_LEFT;
    }
    return SDL_HITTEST_NORMAL;
}

HitRegion Window::defaultHitTest(int x, int y) const {
    int w = 0, h = 0;
    SDL_GetWindowSize(m_window, &w, &h);
    return hitRegionFor(x, y, w, h, m_resizeBorder, isMaximized(), [this](int px, int py) {
        const Document* doc = m_context ? m_context->document(m_currentDocument) : nullptr;
        if (doc == nullptr) {
            return AppRegion::None;
        }
        const Vec2 pos = windowToDesign(Vec2{static_cast<float>(px), static_cast<float>(py)});
        return doc->appRegionAt(doc->hitTest(pos));
    });
}

void Window::minimize() {
    if (m_window != nullptr) {
        SDL_MinimizeWindow(m_window);
    }
}

void Window::maximize() {
    if (m_window != nullptr) {
        SDL_MaximizeWindow(m_window);
    }
}

void Window::restore() {
    if (m_window != nullptr) {
        SDL_RestoreWindow(m_window);
    }
}

void Window::toggleMaximize() {
    if (isMaximized()) {
        restore();
    } else {
        maximize();
    }
}

bool Window::isMaximized() const {
    return m_window != nullptr && (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MAXIMIZED) != 0;
}

void Window::close() {
    if (m_id != 0) {
        m_app.closeWindow(m_id);  // T-384b; T-398: through the close-request handler.
        return;
    }
    m_app.requestClose(*this, /*pushQuit=*/true);  // T-398: the handler, then SDL_QUIT.
}

bool Window::setFullscreen(FullscreenMode mode) {
    if (m_window == nullptr) {
        return false;
    }
    const Uint32 flags = mode == FullscreenMode::Exclusive ? static_cast<Uint32>(SDL_WINDOW_FULLSCREEN)
                         : mode == FullscreenMode::Desktop
                             ? static_cast<Uint32>(SDL_WINDOW_FULLSCREEN_DESKTOP)
                             : 0u;
    if (SDL_SetWindowFullscreen(m_window, flags) != 0) {
        SDL_Log("xt_shell: setFullscreen failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

FullscreenMode Window::fullscreen() const {
    const Uint32 flags = m_window != nullptr ? SDL_GetWindowFlags(m_window) : 0u;
    if ((flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP) {
        return FullscreenMode::Desktop;
    }
    return (flags & SDL_WINDOW_FULLSCREEN) != 0 ? FullscreenMode::Exclusive : FullscreenMode::Off;
}

void Window::raise() {
    if (m_window != nullptr) {
        SDL_RaiseWindow(m_window);
    }
}

bool Window::hasInputFocus() const { return !m_closing && m_app.focusedWindow() == m_id; }

Vec2 Window::windowToDesign(Vec2 pos) const {
    if (!fitActive() || m_place.w <= 0.0f || m_place.h <= 0.0f) {
        return pos;
    }
    return Vec2{(pos.x - m_place.x) * static_cast<float>(m_designW) / m_place.w,
                (pos.y - m_place.y) * static_cast<float>(m_designH) / m_place.h};
}

void Window::renderFrame(float dt, bool swap, uint64_t frameStart) {
    makeCurrent();
    m_context->update(dt);
    // T-162: the same list every frame (docs/design/23-host-integration.md §23.2), so the damage
    // diff can say "nothing changed" and the renderer can reuse the previous frame.
    m_context->render(m_list, &m_damage);
#ifdef XT_SHELL_SW
    presentSw(swap);
    if (m_statsLog) {
        accumulateStats(static_cast<double>(SDL_GetPerformanceCounter() - frameStart) * 1000.0 /
                            static_cast<double>(SDL_GetPerformanceFrequency()),
                        m_lastRenderMs, m_lastDamage, m_lastRendered);
    }
#else
    const CommandList& list = m_list;

    // T-156: a non-Native fit renders into the design-size offscreen target, then places it.
    const bool offscreen = fitActive() && m_fitFbo != 0;
    if (offscreen) {
        gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, m_fitMsaaFbo != 0 ? m_fitMsaaFbo : m_fitFbo);
    }

    if (hasScene()) {
        // T-343: the scene under the UI (Direct mode: the renderer leaves it in place).
        drawSceneGl(offscreen ? m_fitW : m_drawableW, offscreen ? m_fitH : m_drawableH);
    } else {
        gl::glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        gl::glClear(gl::GL_COLOR_BUFFER_BIT | gl::GL_STENCIL_BUFFER_BIT);
    }

    Document* doc = m_context->document(m_currentDocument);
    const IrView* ir = doc != nullptr ? &doc->ir() : nullptr;
    // docs/design/21-perf-tracing.md §21.2: the backend reports renderMs/effectsMs/textMs into the
    // Context's tracker. Passing it unconditionally costs one branch per frame while it is off.
    const Uint64 renderStart = SDL_GetPerformanceCounter();
    m_renderer.render(list, /*shaper=*/nullptr, &m_app.m_assets, ir, &m_context->perf(), &m_damage);
    const double renderMs = static_cast<double>(SDL_GetPerformanceCounter() - renderStart) * 1000.0 /
                            static_cast<double>(SDL_GetPerformanceFrequency());

    if (offscreen) {
        gl::glDisable(gl::GL_SCISSOR_TEST);
        if (m_fitMsaaFbo != 0) {
            // Resolve first: a multisampled source cannot be blitted to a differently sized target.
            gl::glBindFramebuffer(gl::GL_READ_FRAMEBUFFER, m_fitMsaaFbo);
            gl::glBindFramebuffer(gl::GL_DRAW_FRAMEBUFFER, m_fitFbo);
            gl::glBlitFramebuffer(0, 0, m_fitW, m_fitH, 0, 0, m_fitW, m_fitH, gl::GL_COLOR_BUFFER_BIT,
                                  gl::GL_NEAREST);
        }
        gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, 0);
        gl::glViewport(0, 0, m_drawableW, m_drawableH);
        gl::glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        gl::glClear(gl::GL_COLOR_BUFFER_BIT);
        const float dpr = devicePixelRatio();
        const int x0 = static_cast<int>(m_place.x * dpr);
        const int x1 = static_cast<int>((m_place.x + m_place.w) * dpr);
        // GL's origin is bottom-left; m_place is top-left.
        const int y0 = m_drawableH - static_cast<int>((m_place.y + m_place.h) * dpr);
        const int y1 = m_drawableH - static_cast<int>(m_place.y * dpr);
        gl::glBindFramebuffer(gl::GL_READ_FRAMEBUFFER, m_fitFbo);
        gl::glBindFramebuffer(gl::GL_DRAW_FRAMEBUFFER, 0);
        gl::glBlitFramebuffer(0, 0, m_fitW, m_fitH, x0, y0, x1, y1, gl::GL_COLOR_BUFFER_BIT, gl::GL_LINEAR);
        gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, 0);
    }

    if (swap) {
        SDL_GL_SwapWindow(m_window);
    }
    if (m_statsLog) {
        accumulateStats(
            static_cast<double>(SDL_GetPerformanceCounter() - frameStart) * 1000.0 /
                static_cast<double>(SDL_GetPerformanceFrequency()),
            renderMs,
            damageFraction(m_damage, static_cast<double>(m_drawableW) * static_cast<double>(m_drawableH)),
            true);
    }
#endif
}

bool Window::caretRect(SDL_Rect& out) const {
    Rect caret = m_context->textInputRect();
    if (caret.w <= 0.0f || caret.h <= 0.0f) {
        return false;
    }
    if (fitActive() && m_designW > 0 && m_designH > 0) {
        // T-156: design coordinates back to the window's.
        const float sx = m_place.w / static_cast<float>(m_designW);
        const float sy = m_place.h / static_cast<float>(m_designH);
        caret = Rect{m_place.x + caret.x * sx, m_place.y + caret.y * sy, caret.w * sx, caret.h * sy};
    }
    const float dpr = devicePixelRatio();
    out.x = static_cast<int>(caret.x * dpr);
    out.y = static_cast<int>(caret.y * dpr);
    out.w = static_cast<int>(caret.w * dpr);
    out.h = static_cast<int>(caret.h * dpr);
    return true;
}

}  // namespace xt::shell

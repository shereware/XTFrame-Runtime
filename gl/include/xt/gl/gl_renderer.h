#pragma once

#include <cstdint>
#include <memory>

#include "xt/command_list.h"
#include "xt/damage.h"
#include "xt/ir.h"
#include "xt/perf.h"

/// OpenGL 3.3 backend: consumes a CommandList and draws it into whatever FBO is currently bound.
/// See docs/tasks/T-19.md, docs/tasks/T-31.md (DrawGlyphs, §10.3.4), docs/design/10-backends.md
/// §10.3, §10.1, docs/design/09-command-list.md.

namespace xt {
class TextShaper;
class AssetResolver;
}  // namespace xt

#ifndef XT_GL_NO_TEXT
namespace xt::text {
class GlyphAtlas;
}  // namespace xt::text
#endif

namespace xt::gl {

/// Function pointer type matching SDL_GL_GetProcAddress / wglGetProcAddress / etc. Matches
/// gl_loader.h's GlLoadProc (kept as a separate alias here so callers of the public header don't
/// need to include the private gl_loader.h).
using GlLoadProc = void* (*)(const char* name);

/// What the last `GlRenderer::render` actually sent to GL (T-159): the calls that survived the
/// device's state de-duplication, plus the transient resolves and the vertex/index bytes
/// uploaded. For tools (xtframe-gpu-probe `--gl`) and perf work; not a stable API.
struct GlFrameStats {
    uint32_t drawCalls = 0;
    uint32_t programBinds = 0;
    uint32_t fboBinds = 0;
    uint32_t textureBinds = 0;
    uint32_t uniformSets = 0;
    uint32_t resolves = 0;  ///< multisample transient -> single-sample resolve draws
    uint32_t blits = 0;     ///< every glBlitFramebuffer (copy passes)
    uint32_t clears = 0;
    uint32_t passes = 0;
    uint32_t uploadRanges = 0;      ///< glBufferData/glBufferSubData calls
    uint64_t uploadBytes = 0;       ///< bytes those calls carried
    uint32_t transientCreates = 0;  ///< pooled offscreen targets created this frame (not recycled)
    uint32_t cacheStores = 0;       ///< effect results copied into the cache this frame (T-162)
    uint32_t frameReused = 0;       ///< 1 when the previous GpuFrame was resubmitted without a build (T-162)
    // -- T-163: per-phase CPU time of the frame (ms) and GL object churn, for tail analysis. --
    float buildMs = 0;                  ///< FrameBuilder::build (0 when the frame was reused)
    float uploadMs = 0;                 ///< vertex/index upload
    float targetMs = 0;                 ///< transient target acquisition (pool lookups and creations)
    float drawMs = 0;                   ///< passes: clears, copies, commands, resolves, cache stores
    float endFrameMs = 0;               ///< pool release/eviction at the end of submit
    float stateMs = 0;                  ///< the host GL state save (glGet*) and restore around the frame
    uint32_t cachedTargetCreates = 0;   ///< effect-cache targets whose GL objects were created this frame
    uint32_t cachedTargetReleases = 0;  ///< effect-cache targets released this frame
    uint32_t transientDeletes = 0;      ///< pooled offscreen targets deleted this frame
    uint32_t poolEntries = 0;           ///< pooled offscreen targets alive after the frame
    uint64_t poolBytes = 0;             ///< their color + depth/stencil bytes (4 per sample each)
    uint32_t cachedTargets = 0;         ///< effect-cache targets alive after the frame
    uint64_t cachedBytes = 0;           ///< their RGBA8 bytes
};

/// OpenGL 3.3 core-profile backend for CommandList; owns its GL programs/VAOs/VBOs and draws into
/// whichever FBO is bound when render() is called.
class GlRenderer {
public:
    /// Constructs a renderer with no GL objects created yet; call init() before render().
    GlRenderer();
    /// Destroys the renderer, releasing any GL objects created by init() (equivalent to calling
    /// shutdown() first).
    ~GlRenderer();
    /// Non-copyable (owns GL object handles).
    GlRenderer(const GlRenderer&) = delete;
    /// Non-copyable (owns GL object handles).
    GlRenderer& operator=(const GlRenderer&) = delete;
    /// Transfers ownership of `other`'s GL objects; `other` is left uninitialized.
    GlRenderer(GlRenderer&&) noexcept;
    /// Transfers ownership of `other`'s GL objects; `other` is left uninitialized.
    GlRenderer& operator=(GlRenderer&&) noexcept;

    /// Loads every required GL function via `loadProc`, compiles the fill/image/composite
    /// programs and creates the geometry VAOs/VBOs. Returns false on failure (lastError() then
    /// explains why); safe to call again after a failure.
    bool init(GlLoadProc loadProc);

    /// Releases every GL object created by init(). Safe to call on an already-shutdown or
    /// never-initialized instance.
    void shutdown();

    /// Sets the pixel size of the currently bound draw FBO. Must be called at least once before
    /// render(), and again whenever that FBO's size changes.
    void resize(int fbWidth, int fbHeight);

    /// Renders `list` into the currently bound FBO. `shaper` is used only as the DrawGlyphs
    /// outline-fill fallback (no glyph atlas set, or a rotated/non-uniformly-scaled run's xf, or
    /// an individual glyph missing from the atlas); a null `shaper` makes such glyphs draw
    /// nothing, matching a missing font.
    ///
    /// `perf` (docs/design/21-perf-tracing.md §21.2) is additive and defaulted, so no existing call
    /// site changes: when it is non-null and enabled, this call adds its own duration to the
    /// current frame's `renderMs`, the PopEffect work to `effectsMs` and the DrawGlyphs work to
    /// `textMs`. These are **CPU submission times** -- this backend never calls glFinish, so the
    /// GPU may still be working when they stop (§21.2).
    ///
    /// `damage` (T-162, docs/design/23-host-integration.md §23.2) is optional: when it reports
    /// no visual change (`!full && rects.empty()`, which `Context::render(list, &damage)` gives
    /// for a list identical to the previous frame's) and the builder confirms nothing the
    /// frame depends on moved (`FrameBuilder::lastFrameReusable`), the previous frame is
    /// submitted again without rebuilding it. Null, `full`, or any rectangle: build as usual.
    /// This backend still draws the whole surface either way (§23.3). T-343: under
    /// `CommandList::hostTargetMode == Direct` the bound FBO is the host's scene (never cleared
    /// here) and `damage` also decides which cached bands are redrawn (23 §23.aa.2).
    void render(const CommandList& list, TextShaper* shaper, AssetResolver* assets,
                const IrView* ir = nullptr, PerfTracker* perf = nullptr, const DamageList* damage = nullptr);

#ifndef XT_GL_NO_TEXT
    /// Injects the glyph atlas DrawGlyphs samples glyph bitmaps from (docs/tasks/T-31.md). Not
    /// owned by GlRenderer; the caller must keep it alive (and call this again, or with nullptr)
    /// before destroying it. Null (the default) makes every DrawGlyphs command use the
    /// outline-fill fallback.
    void setGlyphAtlas(xt::text::GlyphAtlas* atlas);
#endif

    /// T-215: drops every cached image texture (`gpu::FrameBuilder::invalidateAllAssets()`), so the
    /// next render() uploads each image again. Call it with this renderer's GL context current
    /// whenever the `AssetId`s the command lists use may name other images than before -- a newly
    /// compiled or loaded IR (ids restart at 0 per IR) or a replaced `AssetResolver`. T-263b /
    /// T-263c: render() needs no such call for another `ir` (told apart by `IrView::loadSerial()`,
    /// so a document recompiled into the same `IrView` object counts as another one) or another
    /// `assets` object: textures are cached per pair, and the two most recently rendered pairs
    /// keep theirs. The call is still needed for a resolver whose images changed under the same
    /// IR, or one replaced by another object at the same address.
    void invalidateImages();

    /// Empty string if the last init()/render() call had no error.
    const char* lastError() const;

    /// T-203 (docs/design/18-effects.md §18.7): empty unless the last render() raised a one-time
    /// warning -- a `.xtfx` custom effect whose GLSL failed to compile. That effect's `fallback`
    /// is applied from then on (nothing when it is `none`); the warning is not repeated.
    const char* lastWarning() const;

    /// GL_SAMPLES of the currently bound draw FBO, queried by the most recent init() or resize().
    int sampleCount() const;

    /// GL call counts of the last `render` (T-159).
    GlFrameStats frameStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace xt::gl

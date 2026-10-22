#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "gl_effects.h"
#include "gl_fbo.h"
#include "gl_geometry.h"
#include "gl_loader.h"
#include "gl_state_cache.h"
#include "xt/gpu/gpu_device.h"
#include "xt/gpu/gpu_frame.h"

/// GlDevice: the OpenGL 3.3 implementation of `xt::gpu::GpuDevice` (T-147, Docs/design/
/// 10-backends.md §10.5). Textures are plain GL texture objects (the handle *is* the GL name);
/// transient targets come from the FboPool (multisampled at the surface's sample count when they
/// are drawn into, T-80; resolved for sampling by a `sampler2DMS` draw rather than a blit,
/// T-161; kept within a byte budget rather than deleted on an idle schedule, T-163); `submit` uploads the
/// frame's vertex/index buffers (incrementally, T-159) and replays its passes and commands with
/// the state mapping spelled out in gpu_frame.h (stencil ops, blend, scissor, samplers).
///
/// Pass windows (T-151, gpu_frame.h's `Pass::view*`): a transient is exactly its window's size,
/// so its own pixel (0,0) is the window's top-left surface pixel; the surface's origin is (0,0).
/// Per pass this device sets the viewport to the window (relative to the target's origin), builds
/// `uNdc` so surface-pixel vertices land in the window, and translates every command's
/// surface-pixel scissor by the target's origin. A copy pass copies the overlap of the two
/// windows, in surface coordinates.
///
/// State (T-159): every GL state change goes through `GlStateCache`, every uniform through the
/// owning program's `UniformShadow` (gl_state_cache.h), so a command only pays for what differs
/// from the previous one. The cache is reset per submit; `saveState`/`restoreState` in
/// GlRenderer are unchanged. `lastStats()` reports what the last submit actually issued.
///
/// Effect result cache (T-162, gpu_device.h): a cached target is a plain RGBA8 texture (its name
/// is the handle, so `Shading::Texture` samples it like an uploaded one) with its own
/// single-sample FBO. A `CacheStore` is a `glBlitFramebuffer` from the transient (resolved first
/// when multisampled) into that FBO with the rows flipped, which turns the bottom-up render
/// target into the top-down orientation uploaded textures have.
///
/// Object churn (T-163, Docs/tasks/T-163.md): creating or deleting a GL object costs the
/// driver's worker thread hundreds of microseconds that land in the next `glGet*` (the state
/// save at the start of the next render), so this device avoids both in steady state. Released
/// cached targets go to a free list (`m_cachedFree`, reused by exact size, trimmed only when it
/// exceeds `kCachedFreeBudgetBytes`), and the transient pool keeps its entries within a byte
/// budget instead of deleting them on an idle schedule (gl_fbo.h).

namespace xt::gl {

class GlDevice : public gpu::GpuDevice {
public:
    /// Compiles every program and creates the geometry buffers; the GL loader must already have
    /// run. Returns false on failure (`err` explains why). Leaves the current program binding as
    /// it found it.
    bool init(std::string& err);
    void shutdown();

    /// Surface size and sample count (GlRenderer::resize); pooled draw targets follow the
    /// surface's sample count when the driver can allocate it.
    void resize(int width, int height, int samples, int maxSamples);
    /// The FBO `submit` treats as `kSurfaceTarget` (bound at GlRenderer::render time).
    void setSurface(GLuint fbo) { m_surface = fbo; }

    gpu::TextureHandle createTexture(const char* debugName, uint16_t w, uint16_t h, gpu::TextureFormat fmt,
                                     const void* initial) override;
    void updateTexture(gpu::TextureHandle tex, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       const void* data, size_t rowPitch) override;
    void destroyTexture(gpu::TextureHandle tex) override;
    void submit(const gpu::GpuFrame& frame) override;
    gpu::TextureHandle acquireCachedTarget(uint16_t w, uint16_t h, gpu::TextureFormat fmt) override;
    void releaseCachedTarget(gpu::TextureHandle target) override;
    uint32_t prepareCustomEffect(std::string_view glsl, const char* names, const uint8_t* types,
                                 uint32_t count) override;
    /// T-343: target 0 is whatever FBO the host bound (`setSurface`); glBlitFramebuffer reads it
    /// back for the Direct mode's region copies (a multisampled surface resolves in the blit).
    bool supportsDirectTarget() const override { return true; }
    /// T-263d: GL_MAX_TEXTURE_SIZE as init() read it (0 before init or when the driver gave none).
    uint32_t maxTextureSize() const override {
        return m_maxTextureSize > 0 ? static_cast<uint32_t>(m_maxTextureSize) : 0;
    }
    /// T-203: the one-time warnings raised since the last call (a custom effect shader that
    /// failed to compile), then cleared.
    std::string takeWarning() {
        std::string w;
        w.swap(m_warning);
        return w;
    }

    /// Empty when the last submit had nothing to report.
    const std::string& lastError() const { return m_lastError; }
    /// GL calls the last frame issued (T-159). Counters start at `beginFrame` (T-163), so a
    /// cached target acquired during the build that precedes `submit` is counted too.
    const GlDeviceStats& lastStats() const { return m_cache.stats; }
    GlDeviceStats& stats() { return m_cache.stats; }
    /// Zeroes the counters; GlRenderer calls it before the frame's build.
    void beginFrame() { m_cache.stats = GlDeviceStats{}; }

private:
    GLuint m_surface = 0;
    int m_width = 0;
    int m_height = 0;
    int m_poolSamples = 0;
    bool m_poolMsaaFailed = false;
    /// GL_MAX_TEXTURE_SIZE, read once by init() (T-263b E10); 0 = unknown, nothing refused.
    GLint m_maxTextureSize = 0;
    std::string m_lastError;

    /// One of the main (kMainVert) programs with its uniform locations (-1 where absent) and
    /// shadow.
    struct Program {
        GLuint id = 0;
        GLint uNdc = -1, uKind = -1, uP0 = -1, uP1 = -1, uRadialAxes = -1, uPaintAlpha = -1, uGamma = -1,
              uMode = -1, uOpacity = -1, uSamples = -1;
        // T-178: kGlyphFrag's text-effect uniforms.
        GLint uEffect = -1, uRadius = -1, uRect = -1, uTexSize = -1;
        UniformShadow shadow;
    };
    Program m_solid, m_gradient, m_texture, m_textureStraight, m_glyph, m_composite, m_resolve;
    /// T-202: the instanced particle programs (kParticleVert) and their instance buffer.
    struct ParticleProgram {
        GLuint id = 0;
        GLint uNdc = -1, uXf = -1, uStraight = -1;
        UniformShadow shadow;
    };
    ParticleProgram m_particleTexture, m_particleSolid;
    GLuint m_particleVao = 0;
    GLuint m_particleVbo = 0;
    size_t m_particleCapacity = 0;  ///< bytes allocated in m_particleVbo
    bool linkParticle(ParticleProgram& p, const char* frag, std::string& err);
    /// Uploads `GpuFrame::particleInstances` (once per submit that has any).
    void uploadParticles(const gpu::GpuFrame& frame);
    /// One instanced draw of a DrawKind::Particles command (state already applied).
    void drawParticles(const gpu::DrawCmd& cmd);
    GlGeometry m_geometry;
    FboPool m_pool;
    EffectPrograms m_effects;
    CustomEffectPrograms m_customEffects;  ///< T-203
    std::string m_warning;                 ///< T-203: see takeWarning()
    GlStateCache m_cache;
    float m_ndc[9] = {};  ///< the open pass's surface-px -> NDC matrix

    /// A pass window in surface pixels (gpu_frame.h's `Pass::view*`).
    struct Window {
        int x = 0, y = 0, w = 0, h = 0;
    };

    /// A texture this device created: its format (for updateTexture's upload) and the filter
    /// its parameters currently hold (so setSampler only touches them on a change).
    struct Texture {
        GLuint name = 0;
        gpu::TextureFormat format = gpu::TextureFormat::Rgba8;
        GLenum filter = GL_LINEAR;
    };
    std::vector<Texture> m_textures;

    /// A persistent effect-result target (T-162): the texture (also listed in `m_textures`, so
    /// sampling it tracks its filter like any other) and the FBO the store blits into.
    struct CachedTarget {
        GLuint texture = 0;
        GLuint fbo = 0;
        int width = 0;
        int height = 0;
    };
    std::vector<CachedTarget> m_cached;
    /// Released cached targets kept for reuse (T-163); `lastUsed` is the release frame.
    struct FreeCachedTarget {
        CachedTarget target;
        uint32_t releasedFrame = 0;
    };
    std::vector<FreeCachedTarget> m_cachedFree;
    uint64_t m_cachedFreeBytes = 0;
    uint32_t m_frameCounter = 0;  ///< submits so far (stamps the free list)

    // -- Per-submit state --
    const gpu::GpuFrame* m_frame = nullptr;
    GLuint m_currentFbo = 0;
    std::vector<int> m_handles;     ///< pool handle per transient (or -1)
    std::vector<int> m_resolved;    ///< single-sample resolve of a multisampled transient (or -1)
    std::vector<Window> m_windows;  ///< where each transient sits on the surface (its last pass's window)
    Window m_passWindow;            ///< the open pass's window
    GLint m_passViewport[4] = {0, 0, 0, 0};  ///< the open pass's glViewport (re-applied after a resolve)
    int m_targetW = 0, m_targetH = 0;        ///< the open pass's target size in pixels
    int m_originX = 0, m_originY = 0;        ///< the open pass's target origin in surface pixels
    /// T-343: a scaled transient (gpu_frame.h `TransientTargetDesc`) is smaller than its window;
    /// the scissor is scaled by target / window (1 for every other pass).
    float m_scaleX = 1.0f, m_scaleY = 1.0f;

    bool linkMain(Program& p, const char* frag, std::string& err);

    /// `target`'s window on the surface: the whole surface for kSurfaceTarget, else the window of
    /// the last pass that drew it (or, before any pass, the full transient at (0,0)).
    Window windowOf(gpu::TargetHandle target) const;
    /// `target`'s own pixel size.
    void sizeOf(gpu::TargetHandle target, int& w, int& h) const;

    GLuint fboOf(gpu::TargetHandle target);
    /// Returns a sampleable texture for `target`. A multisampled transient is resolved first: a
    /// full-screen triangle through `m_resolve` into a single-sample pooled entry of the same
    /// size, which leaves the draw framebuffer, viewport, program, blend and scissor state on the
    /// resolve's values (the caller re-applies its own through the cache).
    GLuint textureOf(gpu::TargetHandle target);
    void invalidateResolve(gpu::TargetHandle target);
    /// A single-sample FBO holding `target`'s current content to read from (its own, or the
    /// resolve `textureOf` makes); 0 when unavailable.
    GLuint readFboOf(gpu::TargetHandle target);
    void executeCacheStore(const gpu::CacheStore& store);
    int acquireDrawTarget(const gpu::TransientTargetDesc& desc);
    /// FboPool::acquire plus the cache invalidation a newly created entry needs.
    int acquirePooled(int w, int h, int samples, GLenum internalFormat, bool stencil);
    /// Deletes a cached target's GL objects (its texture entry too).
    void destroyCachedTarget(const CachedTarget& c);
    /// Trims the free list to `kCachedFreeBudgetBytes`, oldest first.
    void trimCachedFree();

    void applyScissor(const gpu::DrawCmd& cmd);
    void applyStencil(const gpu::DrawCmd& cmd);
    void drawCmd(const gpu::DrawCmd& cmd);
    /// Binds `tex` on unit 0 with `sampler`'s filter. A texture this device did not create (a
    /// host's, T-150) whose filter had to change is noted in `m_hostFilter`: the caller draws,
    /// then calls restoreHostFilter().
    void bindSampled(GLuint tex, gpu::Sampler sampler);
    /// T-263b E5: gives the host texture bindSampled() changed its own min/mag filter back.
    void restoreHostFilter();
    struct HostFilter {
        GLuint texture = 0;  ///< 0 = nothing to restore
        GLint minFilter = 0;
        GLint magFilter = 0;
    };
    HostFilter m_hostFilter;
    const float* paramsAt(uint32_t index, size_t& available) const;
};

}  // namespace xt::gl

#include "xt/gl/gl_renderer.h"

#include <chrono>
#include <string>

#include "gl_device.h"
#include "gl_loader.h"
#include "gl_state.h"
#include "xt/gpu/frame_builder.h"
#include "xt/gpu/gpu_frame.h"

/// GlRenderer (T-147): `xt::gpu::FrameBuilder` builds the frame, `GlDevice` draws it. The public
/// header and behaviour (draw into the currently bound FBO, save/restore GL state, glyph atlas
/// injection, sample count) are unchanged from the pre-T-147 renderer; every CPU-side algorithm
/// now lives in xt_gpu (Docs/design/10-backends.md §10.5).

namespace xt::gl {

struct GlRenderer::Impl {
    GlDevice device;
    gpu::FrameBuilder builder;
    gpu::GpuFrame frame;
    bool initialized = false;
    int fbWidth = 0, fbHeight = 0;
    int samples = 0;
    bool frameReused = false;
    std::string lastError;
    std::string lastWarning;  // T-203

    bool init(GlLoadProc loadProc) {
        if (!loadGl(loadProc)) {
            lastError = std::string("failed to load GL function: ") + lastMissing();
            return false;
        }
        std::string err;
        if (!device.init(err)) {
            lastError = err;
            return false;
        }
        builder.init(device);
        initialized = true;
        lastError.clear();
        return true;
    }

    void shutdown() {
        if (!initialized) {
            return;
        }
        builder.shutdown();
        device.shutdown();
        initialized = false;
    }

    void resize(int w, int h) {
        fbWidth = w;
        fbHeight = h;
        GLint s = 0;
        glGetIntegerv(GL_SAMPLES, &s);
        samples = s;
        GLint maxSamples = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
        device.resize(w, h, s, maxSamples);
        builder.resize(w, h);
        builder.setEdgeFringe(s <= 1);  // a multisampled target resolves edges itself
    }

    void render(const CommandList& list, TextShaper* shaper, AssetResolver* assets, const IrView* ir,
                PerfTracker* perf, const DamageList* damage) {
        // Docs/design/21-perf-tracing.md §21.2: CPU submission time (no glFinish); the frame build
        // additionally lands in gpuBuildMs.
        FrameStats* stats = (perf != nullptr && perf->enabled()) ? &perf->current() : nullptr;
        PerfFieldScope renderScope(stats != nullptr ? &stats->renderMs : nullptr);
        lastError.clear();
        if (!initialized) {
            lastError = "GlRenderer::init was not called";
            return;
        }
        // T-163: the device's counters cover build (cached-target churn) and submit alike.
        device.beginFrame();
        auto s0 = std::chrono::steady_clock::now();
        GlSavedState saved = saveState();
        // T-263b E5: every texture this render creates or uploads is bound on unit 0, one of the
        // units whose binding is saved (the host's active unit may be any).
        glActiveTexture(GL_TEXTURE0);
        // Docs/tasks/T-263c.md ruling 7: a sampler object the host left bound on a unit xt_gl
        // samples through would replace the filter and wrap parameters of xt_gl's textures.
        unbindSamplers();
        // E9: target and texture allocations made during this render must not read through the
        // host's unpack state either (a bound pixel unpack buffer turns a null pointer into an
        // offset); the uploads set it again themselves.
        setUploadUnpackState();
        GLint boundFbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &boundFbo);
        device.setSurface(static_cast<GLuint>(boundFbo));
        device.stats().stateMs +=
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - s0).count();
        // T-162: an unchanged list whose frame is still valid is submitted as it is.
        frameReused = damage != nullptr && !damage->full && damage->rects.empty() &&
                      builder.lastFrameReusable(assets, ir);
        if (!frameReused) {
            const auto b0 = std::chrono::steady_clock::now();
            builder.build(list, shaper, assets, ir, frame, perf, damage);  // T-343: band damage
            device.stats().buildMs =
                std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - b0).count();
        }
        device.submit(frame);
        lastWarning = device.takeWarning();  // T-203: a custom shader that failed to compile.

        if (builder.lastError()[0] != '\0') {
            lastError = builder.lastError();
        } else if (!device.lastError().empty()) {
            lastError = device.lastError();
        }
        s0 = std::chrono::steady_clock::now();
        restoreState(saved);
        device.stats().stateMs +=
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - s0).count();
    }
};

GlRenderer::GlRenderer() : m_impl(std::make_unique<Impl>()) {}
GlRenderer::~GlRenderer() {
    if (m_impl) {
        m_impl->shutdown();
    }
}
GlRenderer::GlRenderer(GlRenderer&&) noexcept = default;
GlRenderer& GlRenderer::operator=(GlRenderer&&) noexcept = default;

bool GlRenderer::init(GlLoadProc loadProc) { return m_impl->init(loadProc); }

void GlRenderer::shutdown() { m_impl->shutdown(); }

void GlRenderer::resize(int fbWidth, int fbHeight) { m_impl->resize(fbWidth, fbHeight); }

void GlRenderer::render(const CommandList& list, TextShaper* shaper, AssetResolver* assets, const IrView* ir,
                        PerfTracker* perf, const DamageList* damage) {
    m_impl->render(list, shaper, assets, ir, perf, damage);
}

#ifndef XT_GL_NO_TEXT
void GlRenderer::setGlyphAtlas(xt::text::GlyphAtlas* atlas) { m_impl->builder.setGlyphAtlas(atlas); }
#endif

void GlRenderer::invalidateImages() { m_impl->builder.invalidateAllAssets(); }

const char* GlRenderer::lastError() const { return m_impl->lastError.c_str(); }

const char* GlRenderer::lastWarning() const { return m_impl->lastWarning.c_str(); }

int GlRenderer::sampleCount() const { return m_impl->samples; }

GlFrameStats GlRenderer::frameStats() const {
    const GlDeviceStats& d = m_impl->device.lastStats();
    GlFrameStats s;
    s.drawCalls = d.drawCalls;
    s.programBinds = d.programBinds;
    s.fboBinds = d.fboBinds;
    s.textureBinds = d.textureBinds;
    s.uniformSets = d.uniformSets;
    s.resolves = d.resolves;
    s.blits = d.blits;
    s.clears = d.clears;
    s.passes = d.passes;
    s.uploadRanges = d.uploadRanges;
    s.uploadBytes = d.uploadBytes;
    s.transientCreates = d.transientCreates;
    s.cacheStores = d.cacheStores;
    s.frameReused = m_impl->frameReused ? 1u : 0u;
    s.buildMs = d.buildMs;
    s.uploadMs = d.uploadMs;
    s.targetMs = d.targetMs;
    s.drawMs = d.drawMs;
    s.endFrameMs = d.endFrameMs;
    s.stateMs = d.stateMs;
    s.cachedTargetCreates = d.cachedTargetCreates;
    s.cachedTargetReleases = d.cachedTargetReleases;
    s.transientDeletes = d.transientDeletes;
    s.poolEntries = d.poolEntries;
    s.poolBytes = d.poolBytes;
    s.cachedTargets = d.cachedTargets;
    s.cachedBytes = d.cachedBytes;
    return s;
}

}  // namespace xt::gl

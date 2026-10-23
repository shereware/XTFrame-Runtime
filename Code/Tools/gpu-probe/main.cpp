// xtframe-gpu-probe: offline GPU-frame shape probe (Docs/tasks/T-151.md "量測"). Opens a packed
// bundle, lays a UI out at a given size and runs FrameBuilder::build over a counting GpuDevice for
// N frames, exactly as a host's GPU path does, without any GPU. Prints per frame: passes, commands
// by shading, transient targets (count and summed area), vertices, texture traffic, build ms and
// the fill the fullscreen-type work costs (transient clears + composite/effect quads, in Mpx).
//
//   xtframe-gpu-probe <bundle dir> <ui id> [width height] [frames] [--dump <file>]
//                   [--gl [--msaa <0|2|4|8>] [--no-finish] [--warmup <frames>]]
//                   [--wgpu [--msaa <0|4>] [--no-finish] [--warmup <frames>]]
//
// `--wgpu` (T-438; built when xt_wgpu is): the same frames through `xt::wgpu::WgpuRenderer` on a
// headless device (XT_WGPU_BACKEND / XT_WGPU_FALLBACK apply) into an offscreen RGBA8 target; prints
// the average / p99 / max CPU render time (Docs/design/21 FrameStats::renderMs) and, unless
// `--no-finish`, render + wait for the queue, over the steady frames.
//
// The idea comes from an earlier host-side probe; this copy measures the fill
// from the frame itself (pass windows and quad extents) instead of estimating it from boundsHint.
//
// `--gl` (T-159; built when xt_gl is): the same frames go through the real `xt::gl::GlRenderer`
// into a hidden-window GL 3.3 context (SDL2) and an offscreen target of the given size and sample
// count, and the tool prints what the device actually issued per frame (`GlFrameStats`: program
// and FBO binds, texture binds, uniform sets, resolves, clears, vertex/index upload bytes, pooled
// targets created) plus the CPU render time and the render + glFinish time.
//
// T-163 (tail latency): the `--gl` run also prints, per phase (paint = Context::render, build,
// vertex upload, transient acquisition, passes, pool end-of-frame, the whole render and render +
// glFinish), the average, p99 and max over the steady frames, and the worst frames with their
// phase split and GL object churn (transients created/deleted, cached targets created/released,
// cache stores). `--no-finish` skips the per-frame glFinish so the driver queue behaves as in a
// host that only swaps (the bench harness); `--warmup N` excludes the first N frames from the
// steady statistics (default 5).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../Common/console_utf8.h"
#include "xt/bundle/bundle.h"
#include "xt/command_list.h"
#include "xt/compile/utf8_path.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/gpu/frame_builder.h"
#include "xt/gpu/gpu_device.h"
#include "xt/gpu/gpu_frame.h"
#include "xt/sw/sw_png.h"
#include "xt/text/ft_text_shaper.h"
#include "xt/text/glyph_atlas.h"
#ifdef XT_PROBE_WGPU
#include <webgpu/wgpu.h>

#include "xt/wgpu/wgpu_renderer.h"
#endif

#ifdef XT_PROBE_GL
#include <SDL.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// psapi.h needs windows.h first (kept out of the sorted block above).
#include <psapi.h>
#endif

#include "gl_loader.h"
#include "xt/gl/gl_renderer.h"
#endif

namespace {

struct CountingDevice final : xt::gpu::GpuDevice {
    uint32_t next = 1;
    uint32_t alive = 0;
    uint32_t created = 0, destroyed = 0, updates = 0;
    uint64_t uploadBytes = 0;
    /// T-162 effect result cache: targets alive and their bytes.
    uint32_t cachedAlive = 0;
    uint64_t cachedBytes = 0;
    std::vector<std::pair<uint32_t, uint64_t>> cached;

    xt::gpu::TextureHandle createTexture(const char*, uint16_t w, uint16_t h, xt::gpu::TextureFormat fmt,
                                         const void* initial) override {
        ++created;
        ++alive;
        if (initial != nullptr) {
            uploadBytes += uint64_t(w) * uint64_t(h) * (fmt == xt::gpu::TextureFormat::R8 ? 1u : 4u);
        }
        return next++;
    }
    void updateTexture(xt::gpu::TextureHandle, uint16_t, uint16_t, uint16_t w, uint16_t h, const void*,
                       size_t) override {
        ++updates;
        uploadBytes += uint64_t(w) * uint64_t(h);
    }
    void destroyTexture(xt::gpu::TextureHandle tex) override {
        if (tex == 0) {
            return;
        }
        ++destroyed;
        if (alive > 0) {
            --alive;
        }
    }
    void submit(const xt::gpu::GpuFrame&) override {}
    xt::gpu::TextureHandle acquireCachedTarget(uint16_t w, uint16_t h, xt::gpu::TextureFormat) override {
        const uint64_t bytes = uint64_t(w) * uint64_t(h) * 4u;
        cached.emplace_back(next, bytes);
        cachedBytes += bytes;
        ++cachedAlive;
        return next++;
    }
    void releaseCachedTarget(xt::gpu::TextureHandle target) override {
        for (size_t i = 0; i < cached.size(); ++i) {
            if (cached[i].first != target) continue;
            cachedBytes -= cached[i].second;
            --cachedAlive;
            cached.erase(cached.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
    void resetFrame() {
        created = destroyed = updates = 0;
        uploadBytes = 0;
    }
};

/// Area (px) of a composite/effect command: its quad's bounding box clipped by its scissor.
double quadFillPx(const xt::gpu::GpuFrame& f, const xt::gpu::DrawCmd& c) {
    if (c.vertexCount == 0) {
        return 0.0;
    }
    float x0 = f.vertices[c.baseVertex].x, x1 = x0, y0 = f.vertices[c.baseVertex].y, y1 = y0;
    for (uint32_t i = 1; i < c.vertexCount; ++i) {
        const xt::gpu::Vertex& v = f.vertices[c.baseVertex + i];
        x0 = std::min(x0, v.x);
        x1 = std::max(x1, v.x);
        y0 = std::min(y0, v.y);
        y1 = std::max(y1, v.y);
    }
    const double cx0 = std::max(double(x0), double(c.clipX)), cy0 = std::max(double(y0), double(c.clipY));
    const double cx1 = std::min(double(x1), double(c.clipX + c.clipW));
    const double cy1 = std::min(double(y1), double(c.clipY + c.clipH));
    return (cx1 > cx0 && cy1 > cy0) ? (cx1 - cx0) * (cy1 - cy0) : 0.0;
}

struct FrameNumbers {
    double buildMs = 0, passes = 0, cmds = 0, transients = 0, transientMpx = 0, vertices = 0;
    double solid = 0, gradient = 0, texture = 0, glyph = 0, stencil = 0, composite = 0, effect = 0;
    double created = 0, destroyed = 0, updates = 0, uploadKb = 0;
    double fillMpx = 0;                  ///< transient clears + composite/effect quads
    double cacheStores = 0, reused = 0;  ///< T-162: results stored per frame; frames not rebuilt
    int n = 0;
    void add(const FrameNumbers& o) {
        buildMs += o.buildMs;
        passes += o.passes;
        cmds += o.cmds;
        transients += o.transients;
        transientMpx += o.transientMpx;
        vertices += o.vertices;
        solid += o.solid;
        gradient += o.gradient;
        texture += o.texture;
        glyph += o.glyph;
        stencil += o.stencil;
        composite += o.composite;
        effect += o.effect;
        created += o.created;
        destroyed += o.destroyed;
        updates += o.updates;
        uploadKb += o.uploadKb;
        fillMpx += o.fillMpx;
        cacheStores += o.cacheStores;
        reused += o.reused;
        ++n;
    }
    void print(const char* title) const {
        const double d = n > 0 ? double(n) : 1.0;
        std::printf("%s (%d frame(s), per frame):\n", title, n);
        std::printf(
            "  build %.2f ms | passes %.1f cmds %.1f [solid %.1f gradient %.1f texture %.1f glyph %.1f "
            "stencil %.1f composite %.1f effect %.1f] vertices %.0f | cache stores %.2f | frames reused "
            "%.0f/%d\n",
            buildMs / d, passes / d, cmds / d, solid / d, gradient / d, texture / d, glyph / d, stencil / d,
            composite / d, effect / d, vertices / d, cacheStores / d, reused, n);
        std::printf(
            "  transients %.1f (%.2f Mpx allocated) | fill (clears + composite/effect quads) %.1f Mpx\n",
            transients / d, transientMpx / d, fillMpx / d);
        std::printf("  textures created %.2f destroyed %.2f updated %.2f upload %.1f KB\n", created / d,
                    destroyed / d, updates / d, uploadKb / d);
    }
};

#ifdef XT_PROBE_GL
/// " | process private N MB" on Windows (what the bench harness reports), empty elsewhere.
std::string processMemoryNote() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc))) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), " | process private %.1f MB",
                      double(pmc.PrivateUsage) / (1024.0 * 1024.0));
        return buf;
    }
#endif
    return std::string();
}

/// Hidden SDL2 window + GL 3.3 core context + an offscreen color/depth-stencil target.
struct GlHarness {
    SDL_Window* window = nullptr;
    SDL_GLContext context = nullptr;
    xt::gl::GLuint fbo = 0, colorRb = 0, depthRb = 0;
    int samples = 0;

    bool init(int width, int height, int msaa, std::string& err) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            err = std::string("SDL_Init: ") + SDL_GetError();
            return false;
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        window = SDL_CreateWindow("xtframe-gpu-probe", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 64,
                                  64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        context = window != nullptr ? SDL_GL_CreateContext(window) : nullptr;
        if (context == nullptr || SDL_GL_MakeCurrent(window, context) != 0) {
            err = std::string("GL context: ") + SDL_GetError();
            return false;
        }
        if (!xt::gl::loadGl(
                [](const char* name) { return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name)); })) {
            err = std::string("GL 3.3 entry point missing: ") + xt::gl::lastMissing();
            return false;
        }
        using namespace xt::gl;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glGenRenderbuffers(1, &colorRb);
        glBindRenderbuffer(GL_RENDERBUFFER, colorRb);
        glGenRenderbuffers(1, &depthRb);
        if (msaa > 0) {
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaa, GL_RGBA8, width, height);
            glBindRenderbuffer(GL_RENDERBUFFER, depthRb);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaa, GL_DEPTH24_STENCIL8, width, height);
        } else {
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
            glBindRenderbuffer(GL_RENDERBUFFER, depthRb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        }
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorRb);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthRb);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            err = "offscreen framebuffer incomplete";
            return false;
        }
        GLint s = 0;
        glGetIntegerv(GL_SAMPLES, &s);
        samples = s;
        return true;
    }

    void shutdown() {
        using namespace xt::gl;
        if (fbo != 0) glDeleteFramebuffers(1, &fbo);
        if (colorRb != 0) glDeleteRenderbuffers(1, &colorRb);
        if (depthRb != 0) glDeleteRenderbuffers(1, &depthRb);
        if (context != nullptr) SDL_GL_DeleteContext(context);
        if (window != nullptr) SDL_DestroyWindow(window);
        SDL_Quit();
    }
};

/// One `--gl` frame's phase times (ms) and churn counters, for the tail statistics (T-163).
struct GlFrameRecord {
    int frame = 0;
    double paintMs = 0, renderMs = 0, finishMs = 0;
    xt::gl::GlFrameStats stats;
};

/// avg / p99 / max of `values` (p99 = the value at index ceil(0.99 n) - 1 of the sorted list).
struct Tail {
    double avg = 0, p99 = 0, max = 0;
};
Tail tailOf(std::vector<double> values) {
    Tail t;
    if (values.empty()) {
        return t;
    }
    std::sort(values.begin(), values.end());
    double sum = 0;
    for (double v : values) sum += v;
    t.avg = sum / double(values.size());
    const size_t k = std::min(values.size() - 1, size_t(std::ceil(0.99 * double(values.size()))) - 1);
    t.p99 = values[k];
    t.max = values.back();
    return t;
}

void printTails(const std::vector<GlFrameRecord>& recs) {
    struct Phase {
        const char* name;
        double (*get)(const GlFrameRecord&);
    };
    const Phase phases[] = {
        {"paint (Context::render)", [](const GlFrameRecord& r) { return r.paintMs; }},
        {"build", [](const GlFrameRecord& r) { return double(r.stats.buildMs); }},
        {"upload", [](const GlFrameRecord& r) { return double(r.stats.uploadMs); }},
        {"targets", [](const GlFrameRecord& r) { return double(r.stats.targetMs); }},
        {"passes", [](const GlFrameRecord& r) { return double(r.stats.drawMs); }},
        {"end of frame", [](const GlFrameRecord& r) { return double(r.stats.endFrameMs); }},
        {"state save/restore", [](const GlFrameRecord& r) { return double(r.stats.stateMs); }},
        {"render (build + submit)", [](const GlFrameRecord& r) { return r.renderMs; }},
        {"paint + render", [](const GlFrameRecord& r) { return r.paintMs + r.renderMs; }},
        {"render + glFinish", [](const GlFrameRecord& r) { return r.finishMs; }},
    };
    std::printf("  phase                      avg     p99     max  (ms, %d frames)\n", int(recs.size()));
    for (const Phase& ph : phases) {
        std::vector<double> v;
        v.reserve(recs.size());
        for (const GlFrameRecord& r : recs) v.push_back(ph.get(r));
        const Tail t = tailOf(v);
        std::printf("  %-24s %7.3f %7.3f %7.3f\n", ph.name, t.avg, t.p99, t.max);
    }
    // The worst frames by paint + render, with what they did.
    std::vector<const GlFrameRecord*> worst;
    for (const GlFrameRecord& r : recs) worst.push_back(&r);
    std::sort(worst.begin(), worst.end(), [](const GlFrameRecord* a, const GlFrameRecord* b) {
        return a->paintMs + a->renderMs > b->paintMs + b->renderMs;
    });
    const size_t n = std::min<size_t>(worst.size(), 8);
    std::printf(
        "  worst %d frame(s): frame | paint build upload targets passes end state render | draws fbo "
        "blits upKB | stores cachedNew/Rel transNew/Del\n",
        int(n));
    for (size_t i = 0; i < n; ++i) {
        const GlFrameRecord& r = *worst[i];
        const xt::gl::GlFrameStats& st = r.stats;
        std::printf(
            "    %5d | %5.2f %5.2f %5.2f %5.2f %5.2f %5.2f %5.2f %6.2f | %3u %3u %2u %5.1f | %u %u/%u "
            "%u/%u\n",
            r.frame, r.paintMs, double(st.buildMs), double(st.uploadMs), double(st.targetMs),
            double(st.drawMs), double(st.endFrameMs), double(st.stateMs), r.renderMs, st.drawCalls,
            st.fboBinds, st.blits, double(st.uploadBytes) / 1024.0, st.cacheStores, st.cachedTargetCreates,
            st.cachedTargetReleases, st.transientCreates, st.transientDeletes);
    }
}

struct GlNumbers {
    double renderMs = 0, finishMs = 0;
    double draws = 0, programBinds = 0, fboBinds = 0, textureBinds = 0, uniformSets = 0, resolves = 0,
           blits = 0, clears = 0, passes = 0, uploadRanges = 0, uploadKb = 0, transientCreates = 0;
    double cacheStores = 0, reused = 0;                                  ///< T-162
    double cachedCreates = 0, cachedReleases = 0, transientDeletes = 0;  ///< T-163
    int n = 0;
    void add(const xt::gl::GlFrameStats& s, double render, double finish) {
        renderMs += render;
        finishMs += finish;
        draws += s.drawCalls;
        programBinds += s.programBinds;
        fboBinds += s.fboBinds;
        textureBinds += s.textureBinds;
        uniformSets += s.uniformSets;
        resolves += s.resolves;
        blits += s.blits;
        clears += s.clears;
        passes += s.passes;
        uploadRanges += s.uploadRanges;
        uploadKb += double(s.uploadBytes) / 1024.0;
        transientCreates += s.transientCreates;
        cacheStores += s.cacheStores;
        reused += s.frameReused;
        cachedCreates += s.cachedTargetCreates;
        cachedReleases += s.cachedTargetReleases;
        transientDeletes += s.transientDeletes;
        ++n;
    }
    void print(const char* title) const {
        const double d = n > 0 ? double(n) : 1.0;
        std::printf("%s (%d frame(s), per frame):\n", title, n);
        std::printf("  render %.3f ms | render + glFinish %.3f ms\n", renderMs / d, finishMs / d);
        std::printf(
            "  passes %.1f draws %.1f | binds: program %.1f fbo %.1f texture %.1f | uniform sets %.1f\n",
            passes / d, draws / d, programBinds / d, fboBinds / d, textureBinds / d, uniformSets / d);
        std::printf(
            "  resolves %.1f blits %.1f clears %.1f | vertex/index upload %.1f KB in %.1f call(s) | "
            "transients created %.2f\n",
            resolves / d, blits / d, clears / d, uploadKb / d, uploadRanges / d, transientCreates / d);
        std::printf(
            "  cache stores %.2f | frames reused %.0f/%d | cached targets created %.3f released %.3f | "
            "transients deleted %.3f\n",
            cacheStores / d, reused, n, cachedCreates / d, cachedReleases / d, transientDeletes / d);
    }
};

/// The `--gl` run: frames through GlRenderer into `harness`'s target. Returns the exit code.
int runGl(xt::Context& ctx, xt::DocumentHandle doc, xt::text::FtTextShaper& shaper,
          xt::bundle::Bundle& bundle, int width, int height, int frames, int msaa, bool finish, int warmup) {
    GlHarness harness;
    std::string err;
    if (!harness.init(width, height, msaa, err)) {
        std::fprintf(stderr, "xtframe-gpu-probe --gl: %s\n", err.c_str());
        harness.shutdown();
        return 1;
    }
    std::printf("memory: after GL context + offscreen target%s\n", processMemoryNote().c_str());
    xt::gl::GlRenderer renderer;
    if (!renderer.init(
            [](const char* name) { return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name)); })) {
        std::fprintf(stderr, "GlRenderer::init: %s\n", renderer.lastError());
        harness.shutdown();
        return 1;
    }
    xt::text::GlyphAtlas atlas(shaper);
    renderer.setGlyphAtlas(&atlas);
    renderer.resize(width, height);
    std::printf("GL: %s | target %dx%d, %d sample(s)\n",
                reinterpret_cast<const char*>(xt::gl::glGetString(xt::gl::GL_RENDERER)), width, height,
                harness.samples);

    xt::CommandList list;
    xt::DamageList damage;  // T-162: lets an unchanged frame be resubmitted
    GlNumbers first, steady;
    std::vector<GlFrameRecord> records;
    records.reserve(size_t(std::max(frames - warmup, 0)));
    uint64_t peakPoolBytes = 0, peakCachedBytes = 0;
    for (int f = 0; f < frames; ++f) {
        ctx.update(1.0f / 60.0f);
        const auto p0 = std::chrono::steady_clock::now();
        ctx.render(list, &damage);
        const auto p1 = std::chrono::steady_clock::now();
        const xt::Document* pDoc = ctx.document(doc);
        using namespace xt::gl;
        glBindFramebuffer(GL_FRAMEBUFFER, harness.fbo);
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        const auto t0 = std::chrono::steady_clock::now();
        renderer.render(list, &shaper, &bundle.assets(), pDoc ? &pDoc->ir() : nullptr, nullptr, &damage);
        const auto t1 = std::chrono::steady_clock::now();
        if (finish) {
            glFinish();
        }
        const auto t2 = std::chrono::steady_clock::now();
        const double renderMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double finishMs = std::chrono::duration<double, std::milli>(t2 - t0).count();
        if (renderer.lastError()[0] != '\0') {
            std::fprintf(stderr, "frame %d: %s\n", f, renderer.lastError());
        }
        (f < warmup ? first : steady).add(renderer.frameStats(), renderMs, finishMs);
        peakPoolBytes = std::max(peakPoolBytes, renderer.frameStats().poolBytes);
        peakCachedBytes = std::max(peakCachedBytes, renderer.frameStats().cachedBytes);
        if (f == 0 || f == warmup - 1) {
            glFinish();
            std::printf("memory: after frame %d%s\n", f, processMemoryNote().c_str());
        }
        if (f >= warmup) {
            GlFrameRecord rec;
            rec.frame = f;
            rec.paintMs = std::chrono::duration<double, std::milli>(p1 - p0).count();
            rec.renderMs = renderMs;
            rec.finishMs = finishMs;
            rec.stats = renderer.frameStats();
            records.push_back(rec);
        }
    }
    const std::string firstTitle = "first " + std::to_string(warmup) + " frame(s) (decode + atlas warm-up)";
    first.print(firstTitle.c_str());
    steady.print("steady state");
    std::printf("steady-state tails (T-163)%s:\n", finish ? "" : ", no per-frame glFinish");
    printTails(records);
    const xt::gl::GlFrameStats last = renderer.frameStats();
    std::printf(
        "device memory after the last frame: pooled targets %u (%.1f MB) | cached effect results %u "
        "(%.1f MB) | peak over the run: pool %.1f MB, cached %.1f MB%s\n",
        last.poolEntries, double(last.poolBytes) / (1024.0 * 1024.0), last.cachedTargets,
        double(last.cachedBytes) / (1024.0 * 1024.0), double(peakPoolBytes) / (1024.0 * 1024.0),
        double(peakCachedBytes) / (1024.0 * 1024.0), processMemoryNote().c_str());
    renderer.setGlyphAtlas(nullptr);
    renderer.shutdown();
    harness.shutdown();
    return 0;
}
#endif

}  // namespace

#ifdef XT_PROBE_WGPU
namespace {

/// The `--wgpu` run (T-438): frames through WgpuRenderer into an offscreen target. Returns the
/// exit code.
int runWgpu(xt::Context& ctx, xt::DocumentHandle doc, xt::text::FtTextShaper& shaper,
            xt::bundle::Bundle& bundle, int width, int height, int frames, int msaa, bool finish,
            int warmup) {
    xt::wgpu::WgpuHeadlessOptions options;
    options.sampleCount = msaa >= 4 ? 4u : 1u;
    xt::wgpu::WgpuInit init = xt::wgpu::WgpuInit::headless(options);
    xt::wgpu::WgpuRenderer renderer;
    if (!renderer.init(init)) {
        std::fprintf(stderr, "xtframe-gpu-probe --wgpu: %s\n", renderer.lastError());
        init.release();
        return 1;
    }
    WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
    d.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    d.size = WGPUExtent3D{uint32_t(width), uint32_t(height), 1};
    d.format = WGPUTextureFormat_RGBA8Unorm;
    WGPUTexture target = wgpuDeviceCreateTexture(init.device, &d);
    WGPUTextureView view = wgpuTextureCreateView(target, nullptr);
    xt::text::GlyphAtlas atlas(shaper);
    renderer.setGlyphAtlas(&atlas);
    renderer.resize(width, height);
    renderer.setClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    std::printf("WGPU: %s | target %dx%d, %u sample(s)\n", renderer.adapterName(), width, height,
                options.sampleCount);
    xt::CommandList list;
    xt::DamageList damage;
    std::vector<double> render, total, build, plan, upload, encode, queue;
    double draws = 0, passes = 0, uploads = 0, reused = 0;
    for (int f = 0; f < frames; ++f) {
        ctx.update(1.0f / 60.0f);
        ctx.render(list, &damage);
        const xt::Document* pDoc = ctx.document(doc);
        const auto t0 = std::chrono::steady_clock::now();
        renderer.render(list, &shaper, &bundle.assets(), view, pDoc ? &pDoc->ir() : nullptr, nullptr,
                        &damage);
        const auto t1 = std::chrono::steady_clock::now();
        if (finish) {
            wgpuDevicePoll(init.device, 1, nullptr);
        }
        const auto t2 = std::chrono::steady_clock::now();
        if (renderer.lastError()[0] != '\0') {
            std::fprintf(stderr, "frame %d: %s\n", f, renderer.lastError());
        }
        if (f < warmup) {
            continue;
        }
        render.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        total.push_back(std::chrono::duration<double, std::milli>(t2 - t0).count());
        const xt::wgpu::WgpuStats s = renderer.stats();
        build.push_back(s.buildMs);
        plan.push_back(s.planMs);
        upload.push_back(s.uploadMs);
        encode.push_back(s.encodeMs);
        queue.push_back(s.queueMs);
        draws += s.drawCalls;
        passes += s.passes;
        uploads += double(s.uploadBytes);
        reused += s.frameReused;
    }
    auto report = [](const char* name, std::vector<double> v) {
        if (v.empty()) return;
        double sum = 0;
        for (double x : v) sum += x;
        std::sort(v.begin(), v.end());
        const size_t p99 = std::min(v.size() - 1, size_t(double(v.size()) * 0.99));
        std::printf("  %-22s avg %.3f ms  p99 %.3f ms  max %.3f ms\n", name, sum / double(v.size()), v[p99],
                    v.back());
    };
    const double n = render.empty() ? 1.0 : double(render.size());
    std::printf("steady state (%zu frames): draws %.1f, passes %.1f, upload %.0f B, reused %.0f%%\n",
                render.size(), draws / n, passes / n, uploads / n, 100.0 * reused / n);
    report("render (CPU)", render);
    report("  build", build);
    report("  submit: plan", plan);
    report("  submit: upload", upload);
    report("  submit: encode", encode);
    report("  submit: queue", queue);
    if (finish) report("render + queue wait", total);
    renderer.setGlyphAtlas(nullptr);
    renderer.shutdown();
    wgpuTextureViewRelease(view);
    wgpuTextureDestroy(target);
    wgpuTextureRelease(target);
    init.release();
    return 0;
}

}  // namespace
#endif

static int run(int argc, char** argv) {
    xt::tools::useUtf8Console();  // T-261e
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: xtframe-gpu-probe <bundle dir> <ui id> [width height] [frames] [--dump <file>]\n"
                     "                         [--gl [--msaa <0|2|4|8>] [--no-finish] [--warmup <frames>]]\n"
                     "                         [--wgpu [--msaa <0|4>] [--no-finish] [--warmup <frames>]]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const std::string uiId = argv[2];
    int width = 1920, height = 1080, frames = 120;
    std::string dumpPath;
    bool useGl = false;
    bool useWgpu = false;
    bool finish = true;
    int msaa = 0;
    int warmup = 5;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            dumpPath = argv[++i];
        } else if (std::strcmp(argv[i], "--wgpu") == 0) {
            useWgpu = true;
        } else if (std::strcmp(argv[i], "--gl") == 0) {
            useGl = true;
        } else if (std::strcmp(argv[i], "--no-finish") == 0) {
            finish = false;
        } else if (std::strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            warmup = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--msaa") == 0 && i + 1 < argc) {
            msaa = std::atoi(argv[++i]);
        } else if (i == 3 && i + 1 < argc && argv[i + 1][0] != '-') {
            width = std::atoi(argv[i]);
            height = std::atoi(argv[++i]);
        } else {
            frames = std::atoi(argv[i]);
        }
    }
    if (width <= 0 || height <= 0 || frames <= 0 || warmup < 0) {
        std::fprintf(stderr, "xtframe-gpu-probe: width, height and frames must be positive\n");
        return 2;
    }

#ifdef XT_PROBE_GL
    std::printf("memory: at start%s\n", processMemoryNote().c_str());
#endif
    std::string error;
    xt::Result<std::unique_ptr<xt::bundle::Bundle>> opened =
        xt::bundle::Bundle::open(dir, xt::sw::decodeImageBytes, &error);
    if (!opened.ok()) {
        std::fprintf(stderr, "cannot open bundle '%s': %s\n", dir.c_str(), error.c_str());
        return 1;
    }
    std::unique_ptr<xt::bundle::Bundle> bundle = std::move(opened.value());

    xt::text::FtTextShaper shaper;
    shaper.addSystemFonts();
    xt::Context ctx(xt::ContextConfig{&shaper, &bundle->assets()});
    xt::Result<xt::DocumentHandle> loaded = xt::bundle::loadUi(ctx, *bundle, uiId);
    if (!loaded.ok()) {
        std::fprintf(stderr, "cannot load ui '%s' (%s)\n", uiId.c_str(), xt::statusName(loaded.status()));
        return 1;
    }
    const xt::DocumentHandle doc = loaded.value();
    const xt::bundle::UiInfo* info = bundle->ui(uiId);
    std::printf("bundle %s ui %s design %.0fx%.0f, laid out at %dx%d, %d frames, %u fonts\n", dir.c_str(),
                uiId.c_str(), info ? double(info->size.x) : 0.0, info ? double(info->size.y) : 0.0, width,
                height, frames, unsigned(shaper.fontCount()));
    ctx.setViewport(xt::Rect{0.0f, 0.0f, float(width), float(height)}, 1.0f);
#ifdef XT_PROBE_GL
    std::printf("memory: after bundle, fonts and document load%s\n", processMemoryNote().c_str());
#endif

    if (useWgpu) {
#ifdef XT_PROBE_WGPU
        return runWgpu(ctx, doc, shaper, *bundle, width, height, frames, msaa, finish, warmup);
#else
        std::fprintf(stderr, "xtframe-gpu-probe: --wgpu needs a build with XT_BUILD_WGPU=ON\n");
        return 2;
#endif
    }
    if (useGl) {
#ifdef XT_PROBE_GL
        return runGl(ctx, doc, shaper, *bundle, width, height, frames, msaa, finish, warmup);
#else
        std::fprintf(stderr, "xtframe-gpu-probe: --gl needs a build with XT_BUILD_GL=ON\n");
        return 2;
#endif
    }
    (void)msaa;
    (void)finish;
    (void)warmup;

    CountingDevice device;
    xt::gpu::FrameBuilder builder;
    builder.init(device);
    builder.resize(width, height);
    xt::text::GlyphAtlas atlas(shaper);
    builder.setGlyphAtlas(&atlas);

    xt::CommandList list;
    xt::DamageList damage;  // T-162: an unchanged frame is not rebuilt (counted as reused)
    xt::gpu::GpuFrame frame;
    FrameNumbers first, steady;
    for (int f = 0; f < frames; ++f) {
        ctx.update(1.0f / 60.0f);
        ctx.render(list, &damage);
        const xt::Document* pDoc = ctx.document(doc);
        const xt::IrView* ir = pDoc ? &pDoc->ir() : nullptr;
        device.resetFrame();
        const auto t0 = std::chrono::steady_clock::now();
        const bool reused =
            !damage.full && damage.rects.empty() && builder.lastFrameReusable(&bundle->assets(), ir);
        if (!reused) {
            builder.build(list, &shaper, &bundle->assets(), ir, frame, nullptr);
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        FrameNumbers n;
        n.buildMs = ms;
        n.reused = reused ? 1 : 0;
        n.cacheStores = reused ? 0 : double(frame.cacheStores.size());
        n.passes = double(frame.passes.size());
        n.cmds = double(frame.cmds.size());
        n.transients = double(frame.transients.size());
        n.vertices = double(frame.vertices.size());
        for (const xt::gpu::TransientTargetDesc& d : frame.transients) {
            n.transientMpx += double(d.width) * double(d.height) / 1.0e6;
        }
        for (const xt::gpu::DrawCmd& c : frame.cmds) {
            switch (c.shading) {
                case xt::gpu::Shading::Solid:
                    n.solid += 1;
                    break;
                case xt::gpu::Shading::Gradient:
                    n.gradient += 1;
                    break;
                case xt::gpu::Shading::Texture:
                case xt::gpu::Shading::TextureStraight:
                case xt::gpu::Shading::ParticleTexture:  // T-202: one instanced draw each.
                case xt::gpu::Shading::ParticleSolid:
                    n.texture += 1;
                    break;
                case xt::gpu::Shading::Glyph:
                    n.glyph += 1;
                    break;
                case xt::gpu::Shading::StencilOnly:
                    n.stencil += 1;
                    break;
                case xt::gpu::Shading::Composite:
                    n.composite += 1;
                    n.fillMpx += quadFillPx(frame, c) / 1.0e6;
                    break;
                case xt::gpu::Shading::Effect:
                    n.effect += 1;
                    n.fillMpx += quadFillPx(frame, c) / 1.0e6;
                    break;
            }
        }
        for (const xt::gpu::Pass& p : frame.passes) {
            if (p.clear != 0) {
                n.fillMpx += double(p.viewW) * double(p.viewH) / 1.0e6;
            }
        }
        n.created = device.created;
        n.destroyed = device.destroyed;
        n.updates = device.updates;
        n.uploadKb = double(device.uploadBytes) / 1024.0;
        if (f < 5) {
            first.add(n);
        } else {
            steady.add(n);
        }
    }
    first.print("first 5 frames (decode + atlas warm-up)");
    steady.print("steady state");
    std::printf("textures alive at the end: %u | cached effect results: %u (%.1f KB)\n", device.alive,
                device.cachedAlive, double(device.cachedBytes) / 1024.0);

    if (!dumpPath.empty()) {
        std::string text;
        xt::gpu::dumpGpuFrame(frame, text);
        std::ofstream out(xt::compile::pathFromUtf8(dumpPath), std::ios::binary | std::ios::trunc);  // T-261f
        out << text;
        std::printf("last frame dumped to %s\n", dumpPath.c_str());
    }
    builder.shutdown();
    return 0;
}

// MSVC only: MinGW calls `wmain` only when linked with `-municode`, which this build does not pass,
// so there the narrow `main` below stays the entry point. With XT_PROBE_GL, <SDL.h> renames that
// `main` to SDL_main, and SDL2main's own `main` / `wmain` hands it the wide command line as UTF-8.
#if defined(_WIN32) && defined(_MSC_VER) && !defined(XT_PROBE_GL)
/// T-261f: the arguments as UTF-8 (xt::tools::Utf8Args); the narrow `argv` of `main` is in the ANSI
/// code page, which cannot carry every path.
int wmain(int argc, wchar_t** wargv) {
    xt::tools::Utf8Args args(argc, wargv);
    return run(args.argc(), args.argv());
}
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif

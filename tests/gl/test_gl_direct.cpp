#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "gl_loader.h"
#include "golden_common.h"
#include "xt/command_list.h"
#include "xt/damage.h"
#include "xt/effect_utils.h"
#include "xt/gl/gl_renderer.h"
#include "xt/node.h"
#include "xt/perf.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// T-343 (docs/design/23-host-integration.md §23.aa, docs/tasks/T-343.md item 10): Direct target
/// mode on the GL backend. `direct_glass_blend` renders a synthetic scene plus three glass panels
/// (sigma 4 / 8 / 16), a multiply vignette and a difference crosshair through xt::sw and xt_gl in
/// Direct mode and compares them (channel <= 16/255, <= 3 % differing pixels, like
/// test_gl_effects.cpp); the band test checks that a second frame without damage pastes the
/// bands and that a third is resubmitted; the perf case prints the §23.aa.6 numbers.

using namespace xt;

namespace {

void* sdlLoadProcDirect(const char* name) { return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name)); }

struct DirectGlTarget {
    gl::GLuint fbo = 0, color = 0, rbo = 0;
    gl::GLuint sceneFbo = 0, sceneTex = 0;  ///< the scene uploaded once, blitted in per frame
    int width = 0, height = 0;

    void init(int w, int h) {
        width = w;
        height = h;
        using namespace xt::gl;
        glGenTextures(1, &color);
        glBindTexture(GL_TEXTURE_2D, color);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_NEAREST));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_NEAREST));
        glGenRenderbuffers(1, &rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);
        REQUIRE(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    }

    /// Fills the target with `scene` (straight RGBA8, row 0 = top): the host's scene. The image
    /// is uploaded once into its own FBO and copied in with a blit on every call, the way a host
    /// hands over a freshly rendered frame.
    void bindWithScene(const std::vector<uint8_t>& scene) {
        using namespace xt::gl;
        if (sceneTex == 0) {
            std::vector<uint8_t> flipped(scene.size());
            const size_t pitch = static_cast<size_t>(width) * 4u;
            for (int y = 0; y < height; ++y) {
                std::memcpy(flipped.data() + static_cast<size_t>(y) * pitch,
                            scene.data() + static_cast<size_t>(height - 1 - y) * pitch, pitch);
            }
            glGenTextures(1, &sceneTex);
            glBindTexture(GL_TEXTURE_2D, sceneTex);
            glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), width, height, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_NEAREST));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_NEAREST));
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data());
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glGenFramebuffers(1, &sceneFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sceneTex, 0);
            REQUIRE(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        }
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sceneFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
    }

    std::vector<uint8_t> readbackTopDown() const {
        using namespace xt::gl;
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        std::vector<uint8_t> topDown(pixels.size());
        const size_t pitch = static_cast<size_t>(width) * 4u;
        for (int y = 0; y < height; ++y) {
            std::memcpy(topDown.data() + static_cast<size_t>(y) * pitch,
                        pixels.data() + static_cast<size_t>(height - 1 - y) * pitch, pitch);
        }
        return topDown;
    }

    void destroy() {
        using namespace xt::gl;
        if (fbo != 0) glDeleteFramebuffers(1, &fbo);
        if (sceneFbo != 0) glDeleteFramebuffers(1, &sceneFbo);
        if (rbo != 0) glDeleteRenderbuffers(1, &rbo);
        if (color != 0) glDeleteTextures(1, &color);
        if (sceneTex != 0) glDeleteTextures(1, &sceneTex);
        fbo = rbo = color = sceneFbo = sceneTex = 0;
    }
};

void ensureLoaded() {
    static bool loaded = false;
    if (!loaded) {
        REQUIRE(xt::gl::loadGl(sdlLoadProcDirect));
        loaded = true;
    }
}

/// A synthetic opaque scene: a hue sweep, a dark band, bright blocks and thin stripes, so blurs
/// and blend modes have structure to work on. Straight RGBA8, row 0 = top.
std::vector<uint8_t> makeScene(int w, int h) {
    std::vector<uint8_t> s(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(std::max(1, w - 1));
            const float v = static_cast<float>(y) / static_cast<float>(std::max(1, h - 1));
            float r = 0.2f + 0.7f * u;
            float g = 0.3f + 0.5f * (1.0f - v);
            float b = 0.6f - 0.4f * u + 0.3f * v;
            if (y % 16 < 2) {
                r *= 0.4f;
                g *= 0.4f;
                b *= 0.4f;
            }
            if ((x / 24 + y / 24) % 5 == 0) {
                r = std::min(1.0f, r + 0.35f);
                g = std::min(1.0f, g + 0.35f);
            }
            if (v > 0.6f && v < 0.7f) {
                r = 0.05f;
                g = 0.05f;
                b = 0.08f;
            }
            uint8_t* p =
                s.data() + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
            p[0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[3] = 255;
        }
    }
    return s;
}

PathRef addRectPath(CommandList& cl, float x, float y, float w, float h) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    cl.verbs.push_back(PathVerb::Move);
    cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Close);
    float pts[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
    cl.points.insert(cl.points.end(), pts, pts + 8);
    ref.verbCount = 5;
    ref.pointCount = 8;
    return ref;
}

void addFillRect(CommandList& cl, float x, float y, float w, float h, Color color) {
    FillCmd f{};
    f.path = addRectPath(cl, x, y, w, h);
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(cl.fills.size() - 1)});
}

void addScenePushLayer(CommandList& cl, BlendMode blend, Rect hint) {
    LayerCmd l{};
    l.opacity = 1.0f;
    l.blend = blend;
    l.boundsHint = hint;
    cl.layers.push_back(l);
    Command c{CmdKind::PushLayer, {}, static_cast<uint32_t>(cl.layers.size() - 1)};
    c.flags[0] = kCommandSceneRead;
    cl.commands.push_back(c);
}

void addPopLayer(CommandList& cl) { cl.commands.push_back(Command{CmdKind::PopLayer, {}, 0}); }

void addGlassPanel(CommandList& cl, Rect box, float radius, Color tint) {
    EffectInstance e{};
    e.kind = EffectKind::BackdropBlur;
    e.params[0] = radius;
    EffectCmd c{};
    c.effectOffset = static_cast<uint32_t>(cl.effects.size());
    c.effectCount = 1;
    cl.effects.push_back(e);
    c.boundsHint = box;
    c.contentBounds = box;
    cl.effectCmds.push_back(c);
    Command cmd{CmdKind::PushEffect, {}, static_cast<uint32_t>(cl.effectCmds.size() - 1)};
    cmd.flags[0] = kCommandSceneRead;
    cl.commands.push_back(cmd);
    addFillRect(cl, box.x, box.y, box.w, box.h, tint);
    cl.commands.push_back(Command{CmdKind::PopEffect, {}, 0});
}

/// The `direct_glass_blend` list on a `w x h` surface: static content, three glass panels
/// (sigma 4 / 8 / 16), a multiply vignette, a difference crosshair.
CommandList glassBlendList(int w, int h, int panels = 3) {
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    cl.hostTargetMode = HostTargetMode::Direct;
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    // Band 0: a status bar and a few icons.
    addFillRect(cl, 8, 8, fw * 0.3f, 14, Color{0, 0, 0, 0.5f});
    addFillRect(cl, 10, 10, fw * 0.2f, 10, Color{0.9f, 0.2f, 0.2f, 1});
    for (int i = 0; i < 6; ++i) {
        addFillRect(cl, 8.0f + 18.0f * static_cast<float>(i), 30, 14, 14, Color{0.2f, 0.8f, 0.3f, 1});
    }
    // Glass panels along the bottom.
    const float pw = fw / static_cast<float>(panels + 1);
    const float radii[3] = {8.0f, 16.0f, 32.0f};
    for (int i = 0; i < panels; ++i) {
        const float x = pw * 0.5f + pw * static_cast<float>(i);
        addGlassPanel(cl, Rect{x, fh * 0.55f, pw * 0.8f, fh * 0.3f}, radii[i % 3], Color{1, 1, 1, 0.25f});
    }
    // Band 1: text-like bars inside the panels' area (drawn above them).
    for (int i = 0; i < panels; ++i) {
        const float x = pw * 0.5f + pw * static_cast<float>(i) + 6.0f;
        addFillRect(cl, x, fh * 0.58f, pw * 0.5f, 6, Color{1, 1, 1, 0.9f});
        addFillRect(cl, x, fh * 0.63f, pw * 0.4f, 4, Color{1, 1, 1, 0.6f});
    }
    // Multiply vignette (a scene run) over everything.
    addScenePushLayer(cl, BlendMode::Multiply, Rect{0, 0, fw, fh});
    addFillRect(cl, 0, 0, fw, fh * 0.12f, Color{0.6f, 0.1f, 0.1f, 0.7f});
    addFillRect(cl, 0, fh * 0.88f, fw, fh * 0.12f, Color{0.6f, 0.1f, 0.1f, 0.7f});
    addPopLayer(cl);
    // Difference crosshair (a scene run) in the middle.
    addScenePushLayer(cl, BlendMode::Difference, Rect{fw * 0.5f - 16, fh * 0.5f - 16, 32, 32});
    addFillRect(cl, fw * 0.5f - 14, fh * 0.5f - 1, 28, 2, Color{1, 1, 1, 1});
    addFillRect(cl, fw * 0.5f - 1, fh * 0.5f - 14, 2, 28, Color{1, 1, 1, 1});
    addPopLayer(cl);
    // Band 2: a small badge on top.
    addFillRect(cl, fw - 40, 8, 32, 12, Color{0.2f, 0.3f, 0.9f, 1});
    return cl;
}

std::vector<uint8_t> renderSwDirect(const CommandList& cl, const std::vector<uint8_t>& scene, int w, int h) {
    std::vector<uint8_t> buf = scene;
    sw::SwRenderer renderer;
    sw::SwTarget target{buf.data(), w, h, w * 4};
    renderer.setTarget(target);
    renderer.render(cl, nullptr, nullptr);
    return buf;
}

std::vector<uint8_t> renderGlDirect(const CommandList& cl, const std::vector<uint8_t>& scene, int w, int h) {
    ensureLoaded();
    DirectGlTarget target;
    target.init(w, h);
    gl::GlRenderer renderer;
    REQUIRE(renderer.init(sdlLoadProcDirect));
    renderer.resize(w, h);
    target.bindWithScene(scene);
    renderer.render(cl, nullptr, nullptr, nullptr);
    CHECK(renderer.lastError()[0] == '\0');
    std::vector<uint8_t> out = target.readbackTopDown();
    renderer.shutdown();
    target.destroy();
    return out;
}

}  // namespace

TEST_CASE(
    "gl direct: T-343 direct_glass_blend matches SW (scene + glass sigma 4/8/16 + multiply + difference)") {
    constexpr int kW = 320, kH = 200;
    const std::vector<uint8_t> scene = makeScene(kW, kH);
    const CommandList cl = glassBlendList(kW, kH);
    const std::vector<uint8_t> swPixels = renderSwDirect(cl, scene, kW, kH);
    const std::vector<uint8_t> glPixels = renderGlDirect(cl, scene, kW, kH);
    golden::GoldenResult result = golden::compareRgba(glPixels.data(), swPixels.data(), kW, kH, 16, 0.03f);
    std::printf("direct_glass_blend: %d px differ, max channel diff %d\n",
                static_cast<int>(result.diffPixels), static_cast<int>(result.maxChannelDiff));
    CHECK(result.pass);
    // The scene survives where nothing was drawn (never cleared), and a panel is not the scene.
    const size_t corner = (static_cast<size_t>(kH / 2) * kW + 4) * 4u;
    CHECK(glPixels[corner] == scene[corner]);
    const size_t panel = (static_cast<size_t>(kH * 0.7f) * kW + static_cast<size_t>(kW * 0.3f)) * 4u;
    CHECK(glPixels[panel] != scene[panel]);
}

TEST_CASE("gl direct: T-343 bands are pasted on the second frame and the third is resubmitted") {
    constexpr int kW = 320, kH = 200;
    const std::vector<uint8_t> scene = makeScene(kW, kH);
    const CommandList cl = glassBlendList(kW, kH);
    ensureLoaded();
    DirectGlTarget target;
    target.init(kW, kH);
    gl::GlRenderer renderer;
    REQUIRE(renderer.init(sdlLoadProcDirect));
    renderer.resize(kW, kH);
    PerfTracker perf;
    perf.setEnabled(true);
    auto frame = [&](const DamageList* damage) {
        target.bindWithScene(scene);
        perf.beginFrame(0.0f);
        renderer.render(cl, nullptr, nullptr, nullptr, &perf, damage);
        CHECK(renderer.lastError()[0] == '\0');
        return target.readbackTopDown();
    };
    const std::vector<uint8_t> first = frame(nullptr);
    CHECK(perf.current().bands == 3);
    CHECK(perf.current().bandRedraws == 3);
    CHECK(renderer.frameStats().cacheStores == 3);
    DamageList none;
    none.full = false;
    const std::vector<uint8_t> second = frame(&none);
    CHECK(renderer.frameStats().frameReused == 0);
    CHECK(perf.current().bandRedraws == 0);
    CHECK(renderer.frameStats().cacheStores == 0);
    const uint32_t pastedPasses = renderer.frameStats().passes;
    const std::vector<uint8_t> third = frame(&none);
    CHECK(renderer.frameStats().frameReused == 1);
    CHECK(renderer.frameStats().passes == pastedPasses);
    // Pasting a band reproduces its pixels exactly.
    golden::GoldenResult r2 = golden::compareRgba(second.data(), first.data(), kW, kH, 1, 0.0f);
    CHECK(r2.pass);
    golden::GoldenResult r3 = golden::compareRgba(third.data(), first.data(), kW, kH, 1, 0.0f);
    CHECK(r3.pass);
    renderer.shutdown();
    target.destroy();
}

TEST_CASE("gl direct: T-343 perf numbers for 23 §23.aa.6 (1080p, printed)") {
    constexpr int kW = 1920, kH = 1080;
    ensureLoaded();
    std::printf("T-343 perf on %s\n",
                reinterpret_cast<const char*>(xt::gl::glGetString(xt::gl::GL_RENDERER)));
    const std::vector<uint8_t> scene = makeScene(kW, kH);
    DirectGlTarget target;
    target.init(kW, kH);
    using Clock = std::chrono::steady_clock;
    auto measure = [&](const char* label, CommandList cl, bool direct) {
        cl.hostTargetMode = direct ? HostTargetMode::Direct : HostTargetMode::Layer;
        gl::GlRenderer renderer;
        REQUIRE(renderer.init(sdlLoadProcDirect));
        renderer.resize(kW, kH);
        DamageList none;
        none.full = false;
        DamageList tiny;  // a rectangle outside every band: forces a build that pastes the bands
        tiny.full = false;
        tiny.rects.push_back(Rect{0, kH * 0.45f, 2, 2});
        // GPU time of the render alone (a GL_TIME_ELAPSED query around it, the scene blit outside),
        // the CPU time of render() (submission; a reused frame has no build) and the wall time
        // with glFinish.
        gl::GLuint query = 0;
        xt::gl::glGenQueries(1, &query);
        double gpuMs = 0.0, cpuMs = 0.0;
        auto run = [&](const DamageList* d, bool timed) {
            target.bindWithScene(scene);
            if (timed) {
                xt::gl::glBeginQuery(xt::gl::GL_TIME_ELAPSED, query);
            }
            const auto c0 = Clock::now();
            renderer.render(cl, nullptr, nullptr, nullptr, nullptr, d);
            const double cpu = std::chrono::duration<double, std::milli>(Clock::now() - c0).count();
            if (timed) {
                xt::gl::glEndQuery(xt::gl::GL_TIME_ELAPSED);
            }
            xt::gl::glFinish();
            if (timed) {
                gl::GLuint64 ns = 0;
                xt::gl::glGetQueryObjectui64v(query, xt::gl::GL_QUERY_RESULT, &ns);
                gpuMs += static_cast<double>(ns) * 1e-6;
                cpuMs += cpu;
            }
        };
        for (int i = 0; i < 4; ++i) {
            run(i == 0 ? nullptr : &none, false);  // warm up: stores, pastes, caches
        }
        constexpr int kFrames = 40;
        auto t0 = Clock::now();
        for (int i = 0; i < kFrames; ++i) {
            run(&none, true);
        }
        const double staticMs =
            std::chrono::duration<double, std::milli>(Clock::now() - t0).count() / kFrames;
        gpuMs /= kFrames;
        cpuMs /= kFrames;
        double buildMs = 0.0;
        for (int i = 0; i < kFrames; ++i) {
            run(&tiny, false);
            buildMs += renderer.frameStats().buildMs;
        }
        buildMs /= kFrames;
        // The scene blit (bindWithScene) is part of every frame above: measure it alone.
        t0 = Clock::now();
        for (int i = 0; i < kFrames; ++i) {
            target.bindWithScene(scene);
            xt::gl::glFinish();
        }
        const double sceneMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count() / kFrames;
        xt::gl::glDeleteQueries(1, &query);
        std::printf(
            "  %-18s static frame: GPU %.3f ms (timer query), CPU submit %.3f ms, wall render+glFinish "
            "%.3f ms (scene blit %.3f of it); CPU build %.3f ms; pasted-frame passes %u\n",
            label, gpuMs, cpuMs, staticMs, sceneMs, buildMs, renderer.frameStats().passes);
        renderer.shutdown();
        return gpuMs;
    };
    const double direct3 = measure("Direct, 3 panels", glassBlendList(kW, kH, 3), true);
    const double direct8 = measure("Direct, 8 panels", glassBlendList(kW, kH, 8), true);
    const double layer3 = measure("Layer, 3 panels", glassBlendList(kW, kH, 3), false);
    std::printf("  3 -> 8 panels: GPU +%.3f ms; Direct vs Layer static GPU: %.3f vs %.3f ms\n",
                direct8 - direct3, direct3, layer3);
    target.destroy();
    CHECK(direct3 >= 0.0);
}

#include <SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "doctest.h"
#include "gl_loader.h"
#include "golden_common.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/xtfx.h"
#include "xt/gl/gl_renderer.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// GlRenderer PushEffect/PopEffect GL-vs-SW cross-check (Docs/tasks/T-54.md "測試"): the same ten
/// CommandLists Tests/unit/test_sw_effects.cpp exercises, rendered through both xt::gl::GlRenderer
/// and xt::sw::SwRenderer at 64x64 and compared with Docs/design/15-testing.md §15.3's tolerance
/// (channel <= 16/255, <= 3% differing pixels); Outline (SDF-based) is relaxed to 5% per
/// Docs/tasks/T-54.md's own allowance for the multi-tap SDF approximation. Builder helpers are
/// duplicated from Tests/unit/test_sw_effects.cpp (file-local there, same pattern
/// Tests/gl/test_gl_3d.cpp already uses for Tests/unit/test_sw_3d.cpp's helpers). Skips (does not
/// fail) when no GL context is available, per test_gl_main.cpp's rule.

using namespace xt;

namespace {

// -------------------------------------------------------------------------------------------
// Offscreen render target + readback (mirrors test_gl_renderer.cpp/test_gl_3d.cpp).
// -------------------------------------------------------------------------------------------

struct GlTestTarget {
    gl::GLuint fbo = 0, color = 0, rbo = 0;
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

    void bindAndClear(Color c) {
        using namespace xt::gl;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(c.r, c.g, c.b, c.a);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }

    std::vector<uint8_t> readback() const {
        using namespace xt::gl;
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        return pixels;
    }

    void destroy() {
        using namespace xt::gl;
        if (fbo != 0) glDeleteFramebuffers(1, &fbo);
        if (rbo != 0) glDeleteRenderbuffers(1, &rbo);
        if (color != 0) glDeleteTextures(1, &color);
        fbo = rbo = color = 0;
    }
};

void* sdlLoadProc(const char* name) { return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name)); }

std::vector<uint8_t> renderGlTopDown(const CommandList& cmds, int w, int h) {
    static bool loaded = false;
    if (!loaded) {
        REQUIRE(xt::gl::loadGl(sdlLoadProc));
        loaded = true;
    }
    // Opaque backdrop (matches Tests/gl/test_gl_renderer.cpp's convention, e.g. "gl: semi-
    // transparent overlap"): xt_gl's offscreen buffers accumulate color via GL_SRC_ALPHA blending
    // (effectively premultiplied storage, see gl_shaders.h's file comment), while xt::sw::Surface
    // always stores straight (non-premultiplied) color; the two representations only coincide once
    // every backdrop an effect/layer composites onto is fully opaque. A transparent canvas would
    // make any partially-transparent effect output (shadow edges, glow, ColorAdjust opacity, ...)
    // disagree with SW by a large, non-tolerance-sized margin that has nothing to do with T-54's
    // own effect math -- not a per-test choice, per Docs/design/14-coding-standards.md §14.8's
    // "regard 規格未涵蓋的情況" (not covered by Docs/tasks/T-54.md, most conservative/consistent
    // choice: match the codebase's own established convention for this exact backend difference).
    GlTestTarget target;
    target.init(w, h);
    target.bindAndClear(Color{0, 0, 0, 1});

    gl::GlRenderer renderer;
    REQUIRE(renderer.init(sdlLoadProc));
    renderer.resize(w, h);
    target.bindAndClear(Color{0, 0, 0, 1});  // resize()/init() may have touched GL state; re-clear.
    renderer.render(cmds, nullptr, nullptr, nullptr);
    std::vector<uint8_t> pixels = target.readback();
    renderer.shutdown();
    target.destroy();

    std::vector<uint8_t> topDown(pixels.size());
    for (int y = 0; y < h; ++y) {
        size_t srcRow = static_cast<size_t>(h - 1 - y) * static_cast<size_t>(w) * 4;
        size_t dstRow = static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
        std::copy(pixels.begin() + static_cast<long>(srcRow),
                  pixels.begin() + static_cast<long>(srcRow) + w * 4,
                  topDown.begin() + static_cast<long>(dstRow));
    }
    return topDown;
}

std::vector<uint8_t> renderSw(const CommandList& cmds, int w, int h) {
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    sw::SwRenderer renderer;
    sw::SwTarget target{buf.data(), w, h, w * 4};
    renderer.setTarget(target);
    renderer.clear(Color{0, 0, 0, 1});  // opaque backdrop; see renderGlTopDown's comment.
    renderer.render(cmds, nullptr, nullptr);
    return buf;
}

/// Compares GL vs SW with Docs/design/15-testing.md §15.3's tolerance (channel <= 16/255, <=
/// `maxDiffRatio` differing pixels; 0.03 for every case except Outline/SDF, per Docs/tasks/
/// T-54.md's "Outline/SDF 案例放寬到 5%").
void checkGlMatchesSw(const CommandList& cl, int w, int h, float maxDiffRatio = 0.03f) {
    std::vector<uint8_t> sw = renderSw(cl, w, h);
    std::vector<uint8_t> gl = renderGlTopDown(cl, w, h);
    golden::GoldenResult result = golden::compareRgba(gl.data(), sw.data(), w, h, 16, maxDiffRatio);
    std::printf("XXDIFF diffPixels=%d maxChannelDiff=%d\n", (int)result.diffPixels,
                (int)result.maxChannelDiff);
    CHECK(result.pass);
}

// -------------------------------------------------------------------------------------------
// CommandList builder helpers, duplicated from Tests/unit/test_sw_effects.cpp (file-local there).
// -------------------------------------------------------------------------------------------

CommandList makeList() {
    CommandList cl;
    cl.viewport = Rect{0, 0, 64, 64};
    cl.devicePixelRatio = 1;
    return cl;
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
    PathRef path = addRectPath(cl, x, y, w, h);
    FillCmd f{};
    f.xf = Mat2x3{};
    f.path = path;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    Command cmd{};
    cmd.kind = CmdKind::FillPath;
    cmd.index = static_cast<uint32_t>(cl.fills.size() - 1);
    cl.commands.push_back(cmd);
}

void addPushClipRect(CommandList& cl, Rect rect) {
    ClipRectCmd c{};
    c.xf = Mat2x3{};
    c.rect = rect;
    cl.clipRects.push_back(c);
    Command cmd{};
    cmd.kind = CmdKind::PushClipRect;
    cmd.index = static_cast<uint32_t>(cl.clipRects.size() - 1);
    cl.commands.push_back(cmd);
}

void addPopClip(CommandList& cl) {
    Command cmd{};
    cmd.kind = CmdKind::PopClip;
    cl.commands.push_back(cmd);
}

void addPushLayer(CommandList& cl, float opacity, BlendMode blend, Rect boundsHint = Rect{0, 0, 64, 64}) {
    LayerCmd l{};
    l.opacity = opacity;
    l.blend = blend;
    // T-133: xt::sw sizes its layer buffer from the hint, so an empty rect would drop the whole
    // layer; the default is the full 64x64 viewport (same as Tests/unit/test_sw_effects.cpp's
    // helper).
    l.boundsHint = boundsHint;
    cl.layers.push_back(l);
    Command cmd{};
    cmd.kind = CmdKind::PushLayer;
    cmd.index = static_cast<uint32_t>(cl.layers.size() - 1);
    cl.commands.push_back(cmd);
}

void addPopLayer(CommandList& cl) {
    Command cmd{};
    cmd.kind = CmdKind::PopLayer;
    cl.commands.push_back(cmd);
}

void addPushProjection(CommandList& cl, Mat4 viewProj, Mat2x3 rootXf = Mat2x3{},
                       Rect box = Rect{0, 0, 64, 64}) {
    ProjectionCmd c{};
    c.viewProj = viewProj;
    c.rootXf = rootXf;
    c.boxRect = box;
    c.depthTest = 0;
    cl.projections.push_back(c);
    Command cmd{};
    cmd.kind = CmdKind::PushProjection;
    cmd.index = static_cast<uint32_t>(cl.projections.size() - 1);
    cl.commands.push_back(cmd);
}

void addPopProjection(CommandList& cl) {
    Command cmd{};
    cmd.kind = CmdKind::PopProjection;
    cl.commands.push_back(cmd);
}

void addPushTransform3D(CommandList& cl, Mat4 world3) {
    Transform3DCmd c{};
    c.world3 = world3;
    cl.transforms3.push_back(c);
    Command cmd{};
    cmd.kind = CmdKind::PushTransform3D;
    cmd.index = static_cast<uint32_t>(cl.transforms3.size() - 1);
    cl.commands.push_back(cmd);
}

void addPopTransform3D(CommandList& cl) {
    Command cmd{};
    cmd.kind = CmdKind::PopTransform3D;
    cl.commands.push_back(cmd);
}

void addPushEffect(CommandList& cl, const std::vector<EffectInstance>& effects, Mat2x3 xf, Rect boundsHint,
                   Rect contentBounds) {
    EffectCmd c{};
    c.effectOffset = static_cast<uint32_t>(cl.effects.size());
    c.effectCount = static_cast<uint32_t>(effects.size());
    for (const EffectInstance& e : effects) {
        cl.effects.push_back(e);
    }
    c.xf = xf;
    c.boundsHint = boundsHint;
    c.contentBounds = contentBounds;
    cl.effectCmds.push_back(c);
    Command cmd{};
    cmd.kind = CmdKind::PushEffect;
    cmd.index = static_cast<uint32_t>(cl.effectCmds.size() - 1);
    cl.commands.push_back(cmd);
}

void addPopEffect(CommandList& cl) {
    Command cmd{};
    cmd.kind = CmdKind::PopEffect;
    cl.commands.push_back(cmd);
}

std::pair<uint32_t, uint32_t> addStops(CommandList& cl, std::initializer_list<GradientStop> stops) {
    uint32_t offset = static_cast<uint32_t>(cl.stops.size());
    for (const GradientStop& s : stops) {
        cl.stops.push_back(s);
    }
    return {offset, static_cast<uint32_t>(stops.size())};
}

EffectInstance dropShadow(float ox, float oy, float blur, float spread, float opacity, Color color) {
    EffectInstance e{};
    e.kind = EffectKind::DropShadow;
    e.params[0] = ox;
    e.params[1] = oy;
    e.params[2] = blur;
    e.params[3] = spread;
    e.params[4] = opacity;
    e.colors[0] = color;
    return e;
}

EffectInstance dropShadowInset(float ox, float oy, float blur, float spread, float opacity, Color color) {
    EffectInstance e = dropShadow(ox, oy, blur, spread, opacity, color);
    e.params[5] = 1;  // inset (Docs/design/18-effects.md §18.3)
    return e;
}

EffectInstance glow(float radius, Color color, float intensity, bool inner) {
    EffectInstance e{};
    e.kind = EffectKind::Glow;
    e.params[0] = radius;
    e.params[1] = intensity;
    e.params[2] = inner ? 1.0f : 0.0f;
    e.colors[0] = color;
    return e;
}

EffectInstance blurFx(float radiusX, float radiusY) {
    EffectInstance e{};
    e.kind = EffectKind::Blur;
    e.params[0] = radiusX;
    e.params[1] = radiusY;
    return e;
}

EffectInstance colorAdjust(float brightness, float contrast, float saturation, float hueDeg, float opacity) {
    EffectInstance e{};
    e.kind = EffectKind::ColorAdjust;
    e.params[0] = brightness;
    e.params[1] = contrast;
    e.params[2] = saturation;
    e.params[3] = hueDeg;
    e.params[4] = opacity;
    return e;
}

EffectInstance gradientMask(Vec2 p0, Vec2 p1, bool radial, bool invert, uint32_t stopOffset,
                            uint32_t stopCount) {
    EffectInstance e{};
    e.kind = EffectKind::GradientMask;
    e.params[0] = p0.x;
    e.params[1] = p0.y;
    e.params[2] = p1.x;
    e.params[3] = p1.y;
    e.params[4] = radial ? 1.0f : 0.0f;
    e.params[5] = invert ? 1.0f : 0.0f;
    e.stopOffset = stopOffset;
    e.stopCount = stopCount;
    return e;
}

EffectInstance shine(float position, float width, float softness, float angleDeg, float intensity,
                     Color color) {
    EffectInstance e{};
    e.kind = EffectKind::Shine;
    e.params[0] = position;
    e.params[1] = width;
    e.params[2] = softness;
    e.params[3] = angleDeg;
    e.params[4] = intensity;
    e.colors[0] = color;
    return e;
}

EffectInstance backdropBlur(float radius, float saturation, float brightness) {
    EffectInstance e{};
    e.kind = EffectKind::BackdropBlur;
    e.params[0] = radius;
    e.params[1] = saturation;
    e.params[2] = brightness;
    return e;
}

EffectInstance outline(float radius, float softness, float position, Color color) {
    EffectInstance e{};
    e.kind = EffectKind::Outline;
    e.params[0] = radius;
    e.params[1] = softness;
    e.params[2] = position;
    e.colors[0] = color;
    return e;
}

Rect outsetClamped(Rect content, float margin) {
    float x0 = std::max(0.0f, content.x - margin);
    float y0 = std::max(0.0f, content.y - margin);
    float x1 = std::min(64.0f, content.x + content.w + margin);
    float y1 = std::min(64.0f, content.y + content.h + margin);
    return Rect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

// -------------------------------------------------------------------------------------------
// T-80: multisampled render target.
// -------------------------------------------------------------------------------------------

void addFillRectXf(CommandList& cl, Rect r, Mat2x3 xf, Color color) {
    PathRef path = addRectPath(cl, r.x, r.y, r.w, r.h);
    FillCmd f{};
    f.xf = xf;
    f.path = path;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    Command cmd{};
    cmd.kind = CmdKind::FillPath;
    cmd.index = static_cast<uint32_t>(cl.fills.size() - 1);
    cl.commands.push_back(cmd);
}

/// Multisampled offscreen target, like the window surface the shell and the editor's GL preview
/// render into (both ask for 4x). Everything is drawn into multisample renderbuffers and resolved
/// into a single-sample texture for readback -- which is exactly the comparison T-80 is about:
/// content inside a PushEffect/PushLayer must arrive here antialiased, not aliased.
struct GlMsaaTestTarget {
    gl::GLuint fbo = 0, colorRbo = 0, dsRbo = 0;
    gl::GLuint resolveFbo = 0, resolveTex = 0;
    int width = 0, height = 0;

    /// Returns false when the driver cannot give this exact sample count, in which case the test
    /// has nothing to check and skips (same spirit as test_gl_main.cpp's no-context skip).
    bool init(int w, int h, int samples) {
        using namespace xt::gl;
        width = w;
        height = h;
        GLint maxSamples = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
        if (maxSamples < samples) {
            return false;
        }

        glGenRenderbuffers(1, &colorRbo);
        glBindRenderbuffer(GL_RENDERBUFFER, colorRbo);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
        glGenRenderbuffers(1, &dsRbo);
        glBindRenderbuffer(GL_RENDERBUFFER, dsRbo);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, w, h);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorRbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, dsRbo);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            return false;
        }
        GLint actual = 0;
        glGetIntegerv(GL_SAMPLES, &actual);
        if (actual != samples) {
            return false;
        }

        glGenTextures(1, &resolveTex);
        glBindTexture(GL_TEXTURE_2D, resolveTex);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_NEAREST));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_NEAREST));
        glGenFramebuffers(1, &resolveFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, resolveFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolveTex, 0);
        return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    void bindAndClear(Color c) {
        using namespace xt::gl;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(c.r, c.g, c.b, c.a);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }

    std::vector<uint8_t> readback() const {
        using namespace xt::gl;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolveFbo);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, resolveFbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        return pixels;
    }

    void destroy() {
        using namespace xt::gl;
        if (fbo != 0) glDeleteFramebuffers(1, &fbo);
        if (resolveFbo != 0) glDeleteFramebuffers(1, &resolveFbo);
        if (colorRbo != 0) glDeleteRenderbuffers(1, &colorRbo);
        if (dsRbo != 0) glDeleteRenderbuffers(1, &dsRbo);
        if (resolveTex != 0) glDeleteTextures(1, &resolveTex);
        fbo = resolveFbo = colorRbo = dsRbo = resolveTex = 0;
    }
};

/// Renders `cmds` into a `samples`x multisampled 64x64 target and returns the resolved pixels,
/// top-down. `available` comes back false (and the buffer empty) when the driver cannot provide
/// that sample count.
std::vector<uint8_t> renderGlMsaaTopDown(const CommandList& cmds, int w, int h, int samples,
                                         bool& available) {
    static bool loaded = false;
    if (!loaded) {
        REQUIRE(xt::gl::loadGl(sdlLoadProc));
        loaded = true;
    }
    GlMsaaTestTarget target;
    if (!target.init(w, h, samples)) {
        target.destroy();
        available = false;
        return {};
    }
    available = true;
    target.bindAndClear(Color{0, 0, 0, 1});

    gl::GlRenderer renderer;
    REQUIRE(renderer.init(sdlLoadProc));
    target.bindAndClear(Color{0, 0, 0, 1});
    renderer.resize(w, h);  // must see the multisampled target bound (it reads GL_SAMPLES).
    CHECK(renderer.sampleCount() == samples);
    target.bindAndClear(Color{0, 0, 0, 1});  // resize()/init() may have touched GL state.
    renderer.render(cmds, nullptr, nullptr, nullptr);
    std::vector<uint8_t> pixels = target.readback();
    renderer.shutdown();
    target.destroy();

    std::vector<uint8_t> topDown(pixels.size());
    for (int y = 0; y < h; ++y) {
        size_t srcRow = static_cast<size_t>(h - 1 - y) * static_cast<size_t>(w) * 4;
        size_t dstRow = static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
        std::copy(pixels.begin() + static_cast<long>(srcRow),
                  pixels.begin() + static_cast<long>(srcRow) + w * 4,
                  topDown.begin() + static_cast<long>(dstRow));
    }
    return topDown;
}

/// Number of pixels whose red channel is neither fully off nor fully on -- i.e. antialiased edge
/// pixels of a white-on-black shape.
int countPartialRed(const std::vector<uint8_t>& rgba) {
    int count = 0;
    for (size_t i = 0; i < rgba.size(); i += 4) {
        if (rgba[i] > 24 && rgba[i] < 231) {
            ++count;
        }
    }
    return count;
}

/// A rotated square, whose two diagonal edges give MSAA something to antialias.
CommandList rotatedSquare(const std::function<void(CommandList&)>& open,
                          const std::function<void(CommandList&)>& close) {
    CommandList cl = makeList();
    Rect content{18, 18, 28, 28};
    Mat2x3 spin = mul(translation(Vec2{32, 32}), mul(rotationDeg(20.0f), translation(Vec2{-32, -32})));
    if (open) {
        open(cl);
    }
    addFillRectXf(cl, content, spin, Color{1, 1, 1, 1});
    if (close) {
        close(cl);
    }
    return cl;
}

}  // namespace

// 1. Blur.
TEST_CASE("gl effects: Blur matches SW") {
    CommandList cl = makeList();
    Rect content{22, 22, 20, 20};
    Rect bounds = outsetClamped(content, 16);
    addPushEffect(cl, {blurFx(4, 4)}, Mat2x3{}, bounds, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    checkGlMatchesSw(cl, 64, 64);
}

// 2. DropShadow (hard-edged).
TEST_CASE("gl effects: DropShadow (hard edge) matches SW") {
    CommandList cl = makeList();
    Rect content{20, 20, 20, 20};
    Rect bounds = outsetClamped(content, 14);
    addPushEffect(cl, {dropShadow(6, 6, 0, 0, 1, Color{0, 0, 0, 1})}, Mat2x3{}, bounds, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    checkGlMatchesSw(cl, 64, 64);
}

// 2b. DropShadow with blur.
TEST_CASE("gl effects: DropShadow (blurred) matches SW") {
    CommandList cl = makeList();
    Rect content{20, 20, 20, 20};
    Rect bounds = outsetClamped(content, 20);
    addPushEffect(cl, {dropShadow(6, 6, 4, 0, 1, Color{0, 0, 0, 1})}, Mat2x3{}, bounds, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    checkGlMatchesSw(cl, 64, 64);
}

// 2c. DropShadow inset (T-111): the bounds are exactly the content (inset expands nothing), so the
// pass also exercises the out-of-buffer value of the inverted alpha field.
TEST_CASE("gl effects: DropShadow inset matches SW") {
    CommandList cl = makeList();
    Rect content{20, 20, 24, 24};
    addPushEffect(cl, {dropShadowInset(4, 4, 6, 2, 1, Color{0, 0, 0, 1})}, Mat2x3{}, content, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    checkGlMatchesSw(cl, 64, 64);
}

// 3. Glow outer + inner.
TEST_CASE("gl effects: Glow outer matches SW") {
    CommandList cl = makeList();
    Rect content{22, 22, 20, 20};
    Rect bounds = outsetClamped(content, 16);
    addPushEffect(cl, {glow(6, Color{1, 1, 0, 1}, 1, false)}, Mat2x3{}, bounds, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    checkGlMatchesSw(cl, 64, 64);
}

TEST_CASE("gl effects: Glow inner matches SW") {
    CommandList cl = makeList();
    Rect content{22, 22, 20, 20};
    Rect innerBounds = outsetClamped(content, 10);
    addPushEffect(cl, {glow(6, Color{1, 1, 1, 1}, 2, true)}, Mat2x3{}, innerBounds, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    checkGlMatchesSw(cl, 64, 64);
}

// 4. ColorAdjust: saturate/hue-rotate/opacity.
TEST_CASE("gl effects: ColorAdjust matches SW") {
    Rect content{22, 22, 20, 20};

    CommandList clSat = makeList();
    addPushEffect(clSat, {colorAdjust(0, 0, -1, 0, 1)}, Mat2x3{}, content, content);
    addFillRect(clSat, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(clSat);
    checkGlMatchesSw(clSat, 64, 64);

    CommandList clHue = makeList();
    addPushEffect(clHue, {colorAdjust(0, 0, 0, 180, 1)}, Mat2x3{}, content, content);
    addFillRect(clHue, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(clHue);
    checkGlMatchesSw(clHue, 64, 64);

    CommandList clOp = makeList();
    addPushEffect(clOp, {colorAdjust(0, 0, 0, 0, 0.5f)}, Mat2x3{}, content, content);
    addFillRect(clOp, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(clOp);
    checkGlMatchesSw(clOp, 64, 64);
}

// 5. GradientMask: linear, invert, radial.
TEST_CASE("gl effects: GradientMask matches SW") {
    Rect content{12, 12, 40, 40};

    CommandList clLin = makeList();
    auto stopsLin = addStops(clLin, {GradientStop{0, Color{0, 0, 0, 1}}, GradientStop{1, Color{0, 0, 0, 0}}});
    addPushEffect(clLin,
                  {gradientMask(Vec2{12, 32}, Vec2{52, 32}, false, false, stopsLin.first, stopsLin.second)},
                  Mat2x3{}, content, content);
    addFillRect(clLin, content.x, content.y, content.w, content.h, Color{1, 1, 1, 1});
    addPopEffect(clLin);
    checkGlMatchesSw(clLin, 64, 64);

    CommandList clInv = makeList();
    auto stopsInv = addStops(clInv, {GradientStop{0, Color{0, 0, 0, 1}}, GradientStop{1, Color{0, 0, 0, 0}}});
    addPushEffect(clInv,
                  {gradientMask(Vec2{12, 32}, Vec2{52, 32}, false, true, stopsInv.first, stopsInv.second)},
                  Mat2x3{}, content, content);
    addFillRect(clInv, content.x, content.y, content.w, content.h, Color{1, 1, 1, 1});
    addPopEffect(clInv);
    checkGlMatchesSw(clInv, 64, 64);

    CommandList clRad = makeList();
    auto stopsRad = addStops(clRad, {GradientStop{0, Color{0, 0, 0, 1}}, GradientStop{1, Color{0, 0, 0, 0}}});
    addPushEffect(clRad,
                  {gradientMask(Vec2{32, 32}, Vec2{52, 32}, true, false, stopsRad.first, stopsRad.second)},
                  Mat2x3{}, content, content);
    addFillRect(clRad, content.x, content.y, content.w, content.h, Color{1, 1, 1, 1});
    addPopEffect(clRad);
    checkGlMatchesSw(clRad, 64, 64);
}

// 6. Shine.
TEST_CASE("gl effects: Shine matches SW") {
    CommandList cl = makeList();
    Rect content{12, 12, 40, 40};
    addPushEffect(cl, {shine(0.5f, 10, 0.5f, 0, 1, Color{1, 1, 1, 1})}, Mat2x3{}, content, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{0.2f, 0.2f, 0.2f, 1});
    addPopEffect(cl);
    checkGlMatchesSw(cl, 64, 64);
}

// 7. Outline outer/inner/center (relaxed to 5%, SDF edge approximation).
TEST_CASE("gl effects: Outline outer matches SW (5% tolerance)") {
    Rect content{22, 22, 20, 20};
    Color blue{0, 0, 1, 1};
    CommandList cl = makeList();
    Rect bounds = outsetClamped(content, 6);
    addPushEffect(cl, {outline(3, 0, 0, blue)}, Mat2x3{}, bounds, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    checkGlMatchesSw(cl, 64, 64, 0.05f);
}

TEST_CASE("gl effects: Outline inner matches SW (5% tolerance)") {
    Rect content{22, 22, 20, 20};
    Color blue{0, 0, 1, 1};
    CommandList cl = makeList();
    Rect innerBounds = outsetClamped(content, 6);
    addPushEffect(cl, {outline(3, 0, 2, blue)}, Mat2x3{}, innerBounds, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    checkGlMatchesSw(cl, 64, 64, 0.05f);
}

TEST_CASE("gl effects: Outline center matches SW (5% tolerance)") {
    Color blue{0, 0, 1, 1};
    Rect contentFrac{22, 22, 20.5f, 20};
    CommandList cl = makeList();
    Rect bounds = outsetClamped(contentFrac, 6);
    addPushEffect(cl, {outline(3, 0, 1, blue)}, Mat2x3{}, bounds, contentFrac);
    addFillRect(cl, contentFrac.x, contentFrac.y, contentFrac.w, contentFrac.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    checkGlMatchesSw(cl, 64, 64, 0.05f);
}

// 8. Stacking order (blur then shadow vs shadow then blur) -- GL vs SW for each order.
// T-130 (Docs/design/18-effects.md §18.3's kind 8, §18.6): a half-transparent panel straddling a
// hard colour edge, so the blurred backdrop the two backends read has to agree pixel for pixel.
TEST_CASE("gl effects: BackdropBlur matches SW") {
    CommandList cl = makeList();
    addFillRect(cl, 0, 0, 32, 64, Color{0.85f, 0.25f, 0.2f, 1});
    addFillRect(cl, 32, 0, 32, 64, Color{0.15f, 0.4f, 0.8f, 1});
    Rect content{18, 18, 28, 28};
    addPushEffect(cl, {backdropBlur(8, 0, 0)}, Mat2x3{}, content, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 1, 1, 0.5f});
    addPopEffect(cl);
    checkGlMatchesSw(cl, 64, 64);
}

// The saturation/brightness parameters go through ColorAdjust's own formulas in both backends.
TEST_CASE("gl effects: BackdropBlur with saturation/brightness matches SW") {
    CommandList cl = makeList();
    addFillRect(cl, 0, 0, 64, 32, Color{0.9f, 0.6f, 0.2f, 1});
    addFillRect(cl, 0, 32, 64, 32, Color{0.2f, 0.7f, 0.5f, 1});
    Rect content{16, 16, 32, 32};
    addPushEffect(cl, {backdropBlur(6, 0.5f, 0.1f)}, Mat2x3{}, content, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{0.9f, 0.9f, 1, 0.4f});
    addPopEffect(cl);
    checkGlMatchesSw(cl, 64, 64);
}

// BackdropBlur first, then an ordinary effect over the combined result (§18.6's ordering rule).
TEST_CASE("gl effects: BackdropBlur followed by ColorAdjust matches SW") {
    CommandList cl = makeList();
    addFillRect(cl, 0, 0, 32, 64, Color{0.85f, 0.25f, 0.2f, 1});
    addFillRect(cl, 32, 0, 32, 64, Color{0.15f, 0.4f, 0.8f, 1});
    Rect content{18, 18, 28, 28};
    addPushEffect(cl, {backdropBlur(8, 0, 0), colorAdjust(0, 0, -0.5f, 0, 1)}, Mat2x3{}, content, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 1, 1, 0.5f});
    addPopEffect(cl);
    checkGlMatchesSw(cl, 64, 64);
}

TEST_CASE("gl effects: stacked Blur+DropShadow matches SW (both orders)") {
    Rect content{20, 20, 20, 20};
    Rect bounds = outsetClamped(content, 20);
    Color shadowColor{0, 0, 0, 1};

    CommandList clA = makeList();
    addPushEffect(clA, {blurFx(8, 8), dropShadow(6, 6, 2, 0, 1, shadowColor)}, Mat2x3{}, bounds, content);
    addFillRect(clA, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(clA);
    checkGlMatchesSw(clA, 64, 64);

    CommandList clB = makeList();
    addPushEffect(clB, {dropShadow(6, 6, 2, 0, 1, shadowColor), blurFx(8, 8)}, Mat2x3{}, bounds, content);
    addFillRect(clB, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(clB);
    checkGlMatchesSw(clB, 64, 64);
}

// 9. Outer PushLayer + overflow clip inside the effect.
TEST_CASE("gl effects: outer PushLayer + overflow clip inside PushEffect matches SW") {
    CommandList cl = makeList();
    Rect fillRect{20, 20, 12, 12};
    Rect bounds{10, 10, 50, 50};

    addPushLayer(cl, 0.5f, BlendMode::Normal);
    addPushEffect(cl, {dropShadow(15, 15, 0, 0, 1, Color{0, 0, 0, 1})}, Mat2x3{}, bounds, fillRect);
    addPushClipRect(cl, fillRect);
    addFillRect(cl, fillRect.x, fillRect.y, fillRect.w, fillRect.h, Color{1, 0, 0, 1});
    addPopClip(cl);
    addPopEffect(cl);
    addPopLayer(cl);
    checkGlMatchesSw(cl, 64, 64);
}

// 10. Effect nested inside a 3D projection.
TEST_CASE("gl effects: effect nested inside a 3D projection matches SW") {
    CommandList cl = makeList();
    Rect content{22, 22, 20, 20};
    Rect bounds = outsetClamped(content, 20);
    Mat2x3 xf{0.5f, 0, 0, 1, 0, 0};

    addPushProjection(cl, mat4Identity());
    addPushTransform3D(cl, mat4Identity());
    addPushEffect(cl, {dropShadow(10, 0, 0, 0, 1, Color{0, 0, 0, 1})}, xf, bounds, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    addPopTransform3D(cl);
    addPopProjection(cl);
    checkGlMatchesSw(cl, 64, 64);
}

// 11. T-80: on a multisampled target, the pooled layer/effect FBOs are multisampled too, so a
// shape drawn inside a PushEffect or a PushLayer gets the same antialiased edges as the very same
// shape drawn straight into the target. Before T-80 the offscreen buffers were always
// single-sample and the two differed by a full pixel of coverage along every diagonal edge.
TEST_CASE("gl effects: layer/effect content is antialiased on a multisampled target") {
    const int kSize = 64;
    const int kSamples = 4;

    bool available = false;
    std::vector<uint8_t> plain =
        renderGlMsaaTopDown(rotatedSquare(nullptr, nullptr), kSize, kSize, kSamples, available);
    if (!available) {
        MESSAGE("no 4x MSAA target available; skipping");
        return;
    }
    // The reference itself must be antialiased, otherwise the comparisons below prove nothing.
    int plainEdge = countPartialRed(plain);
    CHECK(plainEdge > 40);

    // Identity ColorAdjust: exercises the full effect path (multisample layer -> resolve ->
    // effect pass -> composite) without changing the pixels it is given.
    std::vector<uint8_t> inEffect =
        renderGlMsaaTopDown(rotatedSquare(
                                [](CommandList& cl) {
                                    addPushEffect(cl, {colorAdjust(0, 0, 0, 0, 1)}, Mat2x3{},
                                                  Rect{0, 0, 64, 64}, Rect{18, 18, 28, 28});
                                },
                                [](CommandList& cl) { addPopEffect(cl); }),
                            kSize, kSize, kSamples, available);
    CHECK(countPartialRed(inEffect) > 40);
    golden::GoldenResult effectResult =
        golden::compareRgba(inEffect.data(), plain.data(), kSize, kSize, 16, 0.03f);
    CHECK(effectResult.pass);

    // Same for a plain group-opacity layer (opacity 1, Normal: a pure round trip through the
    // pooled layer FBO and the composite pass).
    std::vector<uint8_t> inLayer =
        renderGlMsaaTopDown(rotatedSquare([](CommandList& cl) { addPushLayer(cl, 1.0f, BlendMode::Normal); },
                                          [](CommandList& cl) { addPopLayer(cl); }),
                            kSize, kSize, kSamples, available);
    CHECK(countPartialRed(inLayer) > 40);
    golden::GoldenResult layerResult =
        golden::compareRgba(inLayer.data(), plain.data(), kSize, kSize, 16, 0.03f);
    CHECK(layerResult.pass);
}

// -------------------------------------------------------------------------------------------
// T-151: windowed transient targets. A 256x256 canvas with the content away from the origin, so
// every transient is a sub-rectangle of the surface (non-zero window origin): the device's
// window projection, scissor translation, composite quad placement, region copy and the
// origin-folded GradientMask/Shine parameters all have to agree with SW.
// -------------------------------------------------------------------------------------------

namespace {

CommandList makeListSized(int w, int h) {
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    return cl;
}

Rect outsetRect(Rect content, float margin) {
    return Rect{content.x - margin, content.y - margin, content.w + 2 * margin, content.h + 2 * margin};
}

}  // namespace

TEST_CASE("gl effects: T-151 windowed DropShadow (blur + spread) matches SW") {
    CommandList cl = makeListSized(256, 256);
    Rect content{150, 90, 40, 30};
    Rect bounds = outsetRect(content, 24);
    addPushEffect(cl, {dropShadow(6, 6, 8, 2, 1, Color{0, 0, 0, 1})}, Mat2x3{}, bounds, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    checkGlMatchesSw(cl, 256, 256);
}

TEST_CASE("gl effects: T-151 windowed hard DropShadow and Glow match SW") {
    CommandList cl = makeListSized(256, 256);
    Rect content{40, 170, 50, 40};
    Rect bounds = outsetRect(content, 16);
    addPushEffect(cl, {dropShadow(8, 8, 0, 0, 1, Color{0, 0, 1, 1})}, Mat2x3{}, bounds, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    Rect content2{180, 180, 40, 40};
    Rect bounds2 = outsetRect(content2, 16);
    addPushEffect(cl, {glow(6, Color{1, 1, 0, 1}, 1, false)}, Mat2x3{}, bounds2, content2);
    addFillRect(cl, content2.x, content2.y, content2.w, content2.h, Color{0, 1, 0, 1});
    addPopEffect(cl);
    checkGlMatchesSw(cl, 256, 256);
}

TEST_CASE("gl effects: T-151 windowed GradientMask (linear, radial) matches SW") {
    Rect content{120, 60, 80, 60};
    CommandList clLin = makeListSized(256, 256);
    auto stopsLin = addStops(clLin, {GradientStop{0, Color{0, 0, 0, 1}}, GradientStop{1, Color{0, 0, 0, 0}}});
    addPushEffect(clLin,
                  {gradientMask(Vec2{120, 90}, Vec2{200, 90}, false, false, stopsLin.first, stopsLin.second)},
                  Mat2x3{}, content, content);
    addFillRect(clLin, content.x, content.y, content.w, content.h, Color{1, 1, 1, 1});
    addPopEffect(clLin);
    checkGlMatchesSw(clLin, 256, 256);

    CommandList clRad = makeListSized(256, 256);
    auto stopsRad = addStops(clRad, {GradientStop{0, Color{0, 0, 0, 1}}, GradientStop{1, Color{0, 0, 0, 0}}});
    addPushEffect(clRad,
                  {gradientMask(Vec2{160, 90}, Vec2{200, 90}, true, false, stopsRad.first, stopsRad.second)},
                  Mat2x3{}, content, content);
    addFillRect(clRad, content.x, content.y, content.w, content.h, Color{1, 1, 1, 1});
    addPopEffect(clRad);
    checkGlMatchesSw(clRad, 256, 256);
}

TEST_CASE("gl effects: T-151 windowed Shine matches SW") {
    CommandList cl = makeListSized(256, 256);
    Rect content{130, 140, 80, 60};
    addPushEffect(cl, {shine(0.5f, 12, 0.5f, 30, 1, Color{1, 1, 1, 1})}, Mat2x3{}, content, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{0.2f, 0.2f, 0.2f, 1});
    addPopEffect(cl);
    checkGlMatchesSw(cl, 256, 256);
}

TEST_CASE("gl effects: T-151 windowed BackdropBlur matches SW") {
    CommandList cl = makeListSized(256, 256);
    addFillRect(cl, 0, 0, 160, 256, Color{0.85f, 0.25f, 0.2f, 1});
    addFillRect(cl, 160, 0, 96, 256, Color{0.15f, 0.4f, 0.8f, 1});
    addFillRect(cl, 0, 120, 256, 20, Color{0.1f, 0.9f, 0.3f, 1});
    Rect content{130, 100, 60, 60};
    addPushEffect(cl, {backdropBlur(8, 0, 0)}, Mat2x3{}, content, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 1, 1, 0.5f});
    addPopEffect(cl);
    checkGlMatchesSw(cl, 256, 256);
}

TEST_CASE("gl effects: T-151 windowed Multiply layer and clipped layer match SW") {
    // A blend-mode layer with a hint: the backdrop is copied for the window only.
    CommandList cl = makeListSized(256, 256);
    addFillRect(cl, 0, 0, 256, 256, Color{0.6f, 0.7f, 0.8f, 1});
    addFillRect(cl, 100, 100, 100, 20, Color{0.2f, 0.2f, 0.2f, 1});
    addPushLayer(cl, 1.0f, BlendMode::Multiply, Rect{90, 90, 70, 70});
    addFillRect(cl, 90, 90, 70, 70, Color{1, 0.5f, 0.2f, 1});
    addPopLayer(cl);
    // An opacity layer whose lazy full-viewport hint is cut down by the clip.
    addPushClipRect(cl, Rect{170, 30, 60, 60});
    addPushLayer(cl, 0.5f, BlendMode::Normal, Rect{0, 0, 256, 256});
    addFillRect(cl, 150, 10, 100, 100, Color{0, 0, 1, 1});
    addPopLayer(cl);
    addPopClip(cl);
    checkGlMatchesSw(cl, 256, 256);
}

TEST_CASE("gl effects: T-151 effect under a clip narrower than its hint matches SW") {
    CommandList cl = makeListSized(256, 256);
    addFillRect(cl, 0, 0, 256, 256, Color{0.9f, 0.9f, 0.9f, 1});
    addPushClipRect(cl, Rect{100, 100, 60, 80});
    Rect content{90, 110, 80, 40};
    Rect bounds = outsetRect(content, 20);
    addPushEffect(cl, {dropShadow(6, 6, 6, 0, 1, Color{0, 0, 0, 1})}, Mat2x3{}, bounds, content);
    addFillRect(cl, content.x, content.y, content.w, content.h, Color{1, 0, 0, 1});
    addPopEffect(cl);
    addPopClip(cl);
    checkGlMatchesSw(cl, 256, 256);
}

// -------------------------------------------------------------------------------------------
// T-162: effect result cache and static frame reuse through the real device.
// -------------------------------------------------------------------------------------------

TEST_CASE("gl effects: T-162 a cached effect result draws the same pixels as the effect chain") {
    constexpr int kSize = 64;
    CommandList cl = makeList();
    addFillRect(cl, 0, 0, 64, 64, Color{0.2f, 0.2f, 0.2f, 1});
    addPushEffect(cl, {blurFx(4, 4), dropShadow(3, 3, 2, 0, 0.8f, Color{0, 0, 0, 1})}, Mat2x3{},
                  Rect{0, 0, 64, 64}, Rect{16, 16, 32, 32});
    addFillRect(cl, 16, 16, 32, 32, Color{1, 0.5f, 0, 1});
    addPopEffect(cl);

    static bool loaded = false;
    if (!loaded) {
        REQUIRE(xt::gl::loadGl(sdlLoadProc));
        loaded = true;
    }
    GlTestTarget target;
    target.init(kSize, kSize);
    gl::GlRenderer renderer;
    REQUIRE(renderer.init(sdlLoadProc));
    renderer.resize(kSize, kSize);

    auto renderOnce = [&](const DamageList* damage) {
        target.bindAndClear(Color{0, 0, 0, 1});
        renderer.render(cl, nullptr, nullptr, nullptr, nullptr, damage);
        CHECK(renderer.lastError()[0] == '\0');
        return target.readback();
    };
    std::vector<uint8_t> first = renderOnce(nullptr);
    const uint32_t chainPasses = renderer.frameStats().passes;
    CHECK(renderer.frameStats().cacheStores == 0);
    CHECK(chainPasses > 1);

    std::vector<uint8_t> second = renderOnce(nullptr);  // stores
    CHECK(renderer.frameStats().cacheStores == 1);
    CHECK(renderer.frameStats().frameReused == 0);

    std::vector<uint8_t> third = renderOnce(nullptr);  // hit: surface pass only
    CHECK(renderer.frameStats().cacheStores == 0);
    CHECK(renderer.frameStats().passes == 1);
    CHECK(renderer.frameStats().resolves == 0);

    // The cached quad reproduces the composite texel for texel (a 1:1 window quad).
    golden::GoldenResult r2 = golden::compareRgba(second.data(), first.data(), kSize, kSize, 1, 0.0f);
    CHECK(r2.pass);
    golden::GoldenResult r3 = golden::compareRgba(third.data(), first.data(), kSize, kSize, 1, 0.0f);
    CHECK(r3.pass);
    // Something was actually drawn.
    bool anyOrange = false;
    for (size_t i = 0; i + 3 < third.size(); i += 4) {
        anyOrange = anyOrange || (third[i] > 200 && third[i + 2] < 60);
    }
    CHECK(anyOrange);

    // A frame the host reports unchanged is resubmitted without a build.
    DamageList none;
    none.full = false;
    std::vector<uint8_t> fourth = renderOnce(&none);
    CHECK(renderer.frameStats().frameReused == 1);
    CHECK(renderer.frameStats().passes == 1);
    golden::GoldenResult r4 = golden::compareRgba(fourth.data(), first.data(), kSize, kSize, 1, 0.0f);
    CHECK(r4.pass);

    // `full` damage builds again (still a hit).
    DamageList full;
    std::vector<uint8_t> fifth = renderOnce(&full);
    CHECK(renderer.frameStats().frameReused == 0);
    CHECK(renderer.frameStats().passes == 1);
    golden::GoldenResult r5 = golden::compareRgba(fifth.data(), first.data(), kSize, kSize, 1, 0.0f);
    CHECK(r5.pass);

    renderer.shutdown();
    target.destroy();
}

TEST_CASE("gl effects: T-162 the cache leaves the host's framebuffer bound and keeps MSAA transients") {
    constexpr int kSize = 64, kSamples = 4;
    static bool loaded = false;
    if (!loaded) {
        REQUIRE(xt::gl::loadGl(sdlLoadProc));
        loaded = true;
    }
    GlMsaaTestTarget target;
    if (!target.init(kSize, kSize, kSamples)) {
        target.destroy();
        MESSAGE("no 4x multisampled target available, skipping");
        return;
    }
    CommandList cl = makeList();
    addPushEffect(cl, {blurFx(4, 4)}, Mat2x3{}, Rect{0, 0, 64, 64}, Rect{16, 16, 32, 32});
    addFillRect(cl, 16, 16, 32, 32, Color{1, 0.5f, 0, 1});
    addPopEffect(cl);

    target.bindAndClear(Color{0, 0, 0, 1});
    gl::GlRenderer renderer;
    REQUIRE(renderer.init(sdlLoadProc));  // init() probes acquireCachedTarget: must not rebind
    renderer.resize(kSize, kSize);        // reads GL_SAMPLES of the bound FBO
    CHECK(renderer.sampleCount() == kSamples);

    std::vector<uint8_t> pixels[3];
    for (int i = 0; i < 3; ++i) {
        target.bindAndClear(Color{0, 0, 0, 1});
        renderer.render(cl, nullptr, nullptr, nullptr);
        CHECK(renderer.lastError()[0] == '\0');
        pixels[i] = target.readback();
        if (i < 2) {
            // The chain draws: the content layer is multisampled and resolved for the blur.
            CHECK(renderer.frameStats().resolves >= 1);
        }
    }
    // Frame 3 is the cached quad: same pixels, no resolve.
    CHECK(renderer.frameStats().resolves == 0);
    CHECK(renderer.frameStats().passes == 1);
    golden::GoldenResult r = golden::compareRgba(pixels[2].data(), pixels[0].data(), kSize, kSize, 1, 0.0f);
    CHECK(r.pass);
    renderer.shutdown();
    target.destroy();
}

// -------------------------------------------------------------------------------------------
// T-203 (Docs/design/18-effects.md §18.7): `.xtfx` custom effects on GL.
// -------------------------------------------------------------------------------------------

namespace {

/// Adds a ShaderRef for `asset` (GLSL `glsl`, one parameter per `names`/`types`) to `cl`.
void addShader(CommandList& cl, AssetId asset, const std::string& glsl, const std::vector<std::string>& names,
               const std::vector<uint8_t>& types, EffectInstance fallback) {
    ShaderRef ref{};
    ref.asset = asset;
    ref.sourceOffset = static_cast<uint32_t>(cl.shaderText.size());
    ref.sourceLength = 0;
    ref.glslOffset = static_cast<uint32_t>(cl.shaderText.size());
    ref.glslLength = static_cast<uint32_t>(glsl.size());
    cl.shaderText.insert(cl.shaderText.end(), glsl.begin(), glsl.end());
    ref.namesOffset = static_cast<uint32_t>(cl.shaderText.size());
    for (const std::string& n : names) {
        cl.shaderText.insert(cl.shaderText.end(), n.begin(), n.end());
        cl.shaderText.push_back('\0');
    }
    ref.namesLength = static_cast<uint32_t>(cl.shaderText.size()) - ref.namesOffset;
    ref.paramOffset = static_cast<uint32_t>(cl.shaderParams.size());
    ref.paramCount = static_cast<uint32_t>(types.size());
    for (uint8_t t : types) {
        ShaderParamDesc d{};
        d.type = t;
        cl.shaderParams.push_back(d);
    }
    ref.fallback = fallback;
    cl.shaders.push_back(ref);
}

EffectInstance customFx(AssetId asset, std::initializer_list<float> params,
                        Color color0 = Color{1, 1, 1, 1}) {
    EffectInstance e{};
    e.kind = EffectKind::Custom;
    e.shader = asset;
    int i = 0;
    for (float p : params) {
        e.params[i++] = p;
    }
    e.colors[0] = color0;
    return e;
}

std::string glslOf(const char* xtfx) {
    compile::XtfxResult r = compile::compileXtfx(xtfx);
    REQUIRE_MESSAGE(r.ok, r.error.message);
    return r.glsl;
}

/// One GlRenderer kept across renders (the custom program cache and its one-time warning live in
/// it); render() returns top-down pixels.
struct GlSession {
    GlTestTarget target;
    gl::GlRenderer renderer;
    int w, h;
    GlSession(int width, int height) : w(width), h(height) {
        static bool loaded = false;
        if (!loaded) {
            REQUIRE(xt::gl::loadGl(sdlLoadProc));
            loaded = true;
        }
        target.init(w, h);
        target.bindAndClear(Color{0, 0, 0, 1});
        REQUIRE(renderer.init(sdlLoadProc));
        renderer.resize(w, h);
    }
    ~GlSession() {
        renderer.shutdown();
        target.destroy();
    }
    std::vector<uint8_t> render(const CommandList& cl) {
        target.bindAndClear(Color{0, 0, 0, 1});
        renderer.render(cl, nullptr, nullptr, nullptr);
        std::vector<uint8_t> pixels = target.readback();
        std::vector<uint8_t> topDown(pixels.size());
        const size_t row = static_cast<size_t>(w) * 4;
        for (int y = 0; y < h; ++y) {
            std::copy(pixels.begin() + static_cast<long>(size_t(h - 1 - y) * row),
                      pixels.begin() + static_cast<long>(size_t(h - y) * row),
                      topDown.begin() + static_cast<long>(size_t(y) * row));
        }
        return topDown;
    }
};

const uint8_t* px(const std::vector<uint8_t>& rgba, int w, int x, int y) {
    return rgba.data() + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
}

int near(int value, int expected) { return std::abs(value - expected); }

const char* const kRippleXtfx =
    "name: Ripple\n"
    "param amplitude float 4 [0, 32]\n"
    "param speed float 1.5 [0, 10]\n"
    "param tint color #ffffff\n"
    "fallback: blur(2px)\n"
    "expand: 8px\n"
    "---\n"
    "float4 xtMain(XtInput i)\n"
    "{\n"
    "    float2 uv = i.uv + sin(i.uv.yx * 40 + xtTime * speed) * (amplitude * i.texelSize);\n"
    "    return xtSample(uv) * tint;\n"
    "}\n";

}  // namespace

TEST_CASE("gl custom: the 18.7 Ripple example compiles and runs") {
    CommandList cl = makeList();
    cl.time = 0.5f;
    addShader(cl, 0, glslOf(kRippleXtfx), {"amplitude", "speed", "tint"}, {0, 0, 2}, blurFx(2, 2));
    addPushEffect(cl, {customFx(0, {4.0f, 1.5f}, Color{1, 1, 1, 1})}, Mat2x3{}, Rect{0, 0, 64, 64},
                  Rect{16, 16, 32, 32});
    addFillRect(cl, 16, 16, 32, 32, Color{1, 0, 0, 1});
    addPopEffect(cl);
    GlSession gl(64, 64);
    std::vector<uint8_t> pixels = gl.render(cl);
    CHECK(std::string(gl.renderer.lastError()).empty());
    CHECK(std::string(gl.renderer.lastWarning()).empty());
    // The ripple displaces by at most 4 px: the square's middle stays red, far corners stay black.
    const uint8_t* mid = px(pixels, 64, 32, 32);
    CHECK(mid[0] > 200);
    CHECK(mid[1] < 40);
    CHECK(px(pixels, 64, 2, 2)[0] < 40);
}

TEST_CASE("gl custom: XtInput.uv is top-down, parameters reach their uniforms, output is premultiplied") {
    const std::string glsl = glslOf(
        "param k float 1\nparam tint color #ffffff\n---\n"
        "float4 xtMain(XtInput i) {\n"
        "    float4 c = xtSample(i.uv);\n"
        "    return float4(i.uv.x, i.uv.y * k, tint.b, c.a);\n"
        "}\n");
    auto listWith = [&](float k, float alpha) {
        CommandList cl = makeList();
        addShader(cl, 3, glsl, {"k", "tint"}, {0, 2}, EffectInstance{});
        addPushEffect(cl, {customFx(3, {k}, Color{0, 0, 1, 1})}, Mat2x3{}, Rect{0, 0, 64, 64},
                      Rect{0, 0, 64, 64});
        addFillRect(cl, 0, 0, 64, 64, Color{1, 1, 1, alpha});
        addPopEffect(cl);
        return cl;
    };
    GlSession gl(64, 64);
    std::vector<uint8_t> a = gl.render(listWith(1.0f, 1.0f));
    CHECK(std::string(gl.renderer.lastWarning()).empty());
    // uv = (x + 0.5) / 64 across the 64 x 64 window, y down.
    CHECK(near(px(a, 64, 8, 8)[0], 34) <= 6);
    CHECK(near(px(a, 64, 8, 8)[1], 34) <= 6);
    CHECK(near(px(a, 64, 8, 56)[1], 227) <= 6);
    CHECK(near(px(a, 64, 56, 8)[0], 227) <= 6);
    CHECK(px(a, 64, 8, 8)[2] > 245);  // tint.b
    std::vector<uint8_t> b = gl.render(listWith(0.5f, 1.0f));
    CHECK(near(px(b, 64, 8, 56)[1], 113) <= 6);
    // Half-transparent content over the opaque black backdrop: xtSample saw straight alpha 0.5
    // and the result was premultiplied before compositing (227 * 0.5).
    std::vector<uint8_t> c = gl.render(listWith(1.0f, 0.5f));
    CHECK(near(px(c, 64, 8, 56)[1], 113) <= 8);
}

TEST_CASE("gl custom: a shader that fails to compile warns once and applies its fallback") {
    CommandList cl = makeList();
    addShader(cl, 1, "this is not GLSL;", {}, {}, blurFx(4, 4));
    addPushEffect(cl, {customFx(1, {})}, Mat2x3{}, Rect{0, 0, 64, 64}, Rect{16, 16, 32, 32});
    addFillRect(cl, 16, 16, 32, 32, Color{1, 0.5f, 0, 1});
    addPopEffect(cl);

    GlSession gl(64, 64);
    std::vector<uint8_t> first = gl.render(cl);
    CHECK(std::string(gl.renderer.lastWarning()).find("failed to compile") != std::string::npos);
    CHECK(std::string(gl.renderer.lastError()).empty());
    std::vector<uint8_t> second = gl.render(cl);
    CHECK(std::string(gl.renderer.lastWarning()).empty());  // reported once
    // The fallback (blur 4px) draws what SW draws for the same list (SW always applies it).
    std::vector<uint8_t> sw = renderSw(cl, 64, 64);
    golden::GoldenResult r = golden::compareRgba(second.data(), sw.data(), 64, 64, 16, 0.03f);
    CHECK(r.pass);
    golden::GoldenResult same = golden::compareRgba(first.data(), second.data(), 64, 64, 0, 0.0f);
    CHECK(same.pass);
}

TEST_CASE("gl custom: a failed shader with fallback none leaves the content as drawn") {
    CommandList cl = makeList();
    addShader(cl, 2, "not glsl either", {}, {}, EffectInstance{});
    addPushEffect(cl, {customFx(2, {})}, Mat2x3{}, Rect{0, 0, 64, 64}, Rect{16, 16, 32, 32});
    addFillRect(cl, 16, 16, 32, 32, Color{0, 1, 0, 1});
    addPopEffect(cl);
    CommandList plain = makeList();
    addFillRect(plain, 16, 16, 32, 32, Color{0, 1, 0, 1});
    GlSession gl(64, 64);
    std::vector<uint8_t> a = gl.render(cl);
    std::vector<uint8_t> b = gl.render(plain);
    golden::GoldenResult r = golden::compareRgba(a.data(), b.data(), 64, 64, 2, 0.0f);
    CHECK(r.pass);
}

TEST_CASE("gl golden: gl-only UI fixtures (custom effects) match their expected images") {
    const std::string uiDir = XT_GOLDEN_UI_DIR;
    const std::vector<golden::UiFixture> fixtures = golden::loadUiManifest(uiDir + "/manifest.txt");
    REQUIRE(golden::manifestErrors() == 0);  // T-267a: no fixture dropped by a bad manifest line.
    int ran = 0;
    for (const golden::UiFixture& fx : fixtures) {
        if (!fx.glOnly) {
            continue;
        }
        CAPTURE(fx.name);
        ++ran;
        std::string warning = "?";
        std::vector<uint8_t> actual = golden::renderUiFixture(
            fx, uiDir,
            [&](const golden::UiFixture& f, const CommandList& cmds, TextShaper* shaper,
                AssetResolver* assets, const IrView* ir) {
                GlSession gl(f.width, f.height);
                gl.target.bindAndClear(f.clearColor);
                gl.renderer.render(cmds, shaper, assets, ir);
                warning = gl.renderer.lastWarning();
                std::vector<uint8_t> pixels = gl.target.readback();
                std::vector<uint8_t> topDown(pixels.size());
                const size_t row = static_cast<size_t>(f.width) * 4;
                for (int y = 0; y < f.height; ++y) {
                    std::copy(pixels.begin() + static_cast<long>(size_t(f.height - 1 - y) * row),
                              pixels.begin() + static_cast<long>(size_t(f.height - y) * row),
                              topDown.begin() + static_cast<long>(size_t(y) * row));
                }
                return topDown;
            });
        REQUIRE(!actual.empty());
        CHECK(warning.empty());  // every shader of the fixture compiled
        const std::string path = std::string(XT_GOLDEN_EXPECTED_DIR) + "/" + fx.name + ".png";
        std::vector<uint8_t> expected;
        int ew = 0, eh = 0;
        if (!sw::readPng(path, expected, ew, eh)) {
            // No expected image yet (xt_golden --update cannot make one: it is SW-only): write
            // this run's output for review and fail.
            CHECK(sw::writePng(path, actual.data(), fx.width, fx.height, fx.width * 4));
            FAIL_CHECK("expected image was missing; wrote " << path);
            continue;
        }
        REQUIRE(ew == fx.width);
        REQUIRE(eh == fx.height);
        golden::GoldenResult result =
            golden::compareRgba(actual.data(), expected.data(), fx.width, fx.height, 16, 0.03f);
        CHECK(result.pass);
    }
    CHECK(ran >= 1);
}
// T-325 (05 §5.9.8): the outline focus ring is a plain FillPath, so GL draws the same image xt_golden
// draws with SW (Tests/golden/expected/ui_outline_focus.png).
TEST_CASE("gl golden: ui_outline_focus matches the SW expected image") {
    const std::string uiDir = XT_GOLDEN_UI_DIR;
    const std::vector<golden::UiFixture> fixtures = golden::loadUiManifest(uiDir + "/manifest.txt");
    REQUIRE(golden::manifestErrors() == 0);
    const golden::UiFixture* found = nullptr;
    for (const golden::UiFixture& fx : fixtures) {
        if (fx.name == "ui_outline_focus") {
            found = &fx;
        }
    }
    REQUIRE(found != nullptr);
    std::vector<uint8_t> actual = golden::renderUiFixture(
        *found, uiDir,
        [&](const golden::UiFixture& f, const CommandList& cmds, TextShaper* shaper, AssetResolver* assets,
            const IrView* ir) {
            GlSession gl(f.width, f.height);
            gl.target.bindAndClear(f.clearColor);
            gl.renderer.render(cmds, shaper, assets, ir);
            std::vector<uint8_t> pixels = gl.target.readback();
            std::vector<uint8_t> topDown(pixels.size());
            const size_t row = static_cast<size_t>(f.width) * 4;
            for (int y = 0; y < f.height; ++y) {
                std::copy(pixels.begin() + static_cast<long>(size_t(f.height - 1 - y) * row),
                          pixels.begin() + static_cast<long>(size_t(f.height - y) * row),
                          topDown.begin() + static_cast<long>(size_t(y) * row));
            }
            return topDown;
        });
    REQUIRE(!actual.empty());
    std::vector<uint8_t> expected;
    int ew = 0, eh = 0;
    REQUIRE(sw::readPng(std::string(XT_GOLDEN_EXPECTED_DIR) + "/ui_outline_focus.png", expected, ew, eh));
    REQUIRE(ew == found->width);
    REQUIRE(eh == found->height);
    golden::GoldenResult result =
        golden::compareRgba(actual.data(), expected.data(), found->width, found->height, 16, 0.03f);
    CHECK(result.pass);
}
// T-324 (05 §5.9.8): text decorations are plain FillPaths after the glyphs, so GL draws the same image
// xt_golden draws with SW (Tests/golden/expected/ui_text_decoration.png).
// The decoration rows match exactly (the rectangles are pixel-snapped); the glyphs' own edge
// coverage differs between the GL atlas and the SW rasterizer by about 3.7% of this image (every
// differing row is a glyph row), hence 0.05 instead of ui_outline_focus's 0.03.
TEST_CASE("gl golden: ui_text_decoration matches the SW expected image") {
    const std::string uiDir = XT_GOLDEN_UI_DIR;
    const std::vector<golden::UiFixture> fixtures = golden::loadUiManifest(uiDir + "/manifest.txt");
    REQUIRE(golden::manifestErrors() == 0);
    const golden::UiFixture* found = nullptr;
    for (const golden::UiFixture& fx : fixtures) {
        if (fx.name == "ui_text_decoration") {
            found = &fx;
        }
    }
    REQUIRE(found != nullptr);
    std::vector<uint8_t> actual = golden::renderUiFixture(
        *found, uiDir,
        [&](const golden::UiFixture& f, const CommandList& cmds, TextShaper* shaper, AssetResolver* assets,
            const IrView* ir) {
            GlSession gl(f.width, f.height);
            gl.target.bindAndClear(f.clearColor);
            gl.renderer.render(cmds, shaper, assets, ir);
            std::vector<uint8_t> pixels = gl.target.readback();
            std::vector<uint8_t> topDown(pixels.size());
            const size_t row = static_cast<size_t>(f.width) * 4;
            for (int y = 0; y < f.height; ++y) {
                std::copy(pixels.begin() + static_cast<long>(size_t(f.height - 1 - y) * row),
                          pixels.begin() + static_cast<long>(size_t(f.height - y) * row),
                          topDown.begin() + static_cast<long>(size_t(y) * row));
            }
            return topDown;
        });
    REQUIRE(!actual.empty());
    std::vector<uint8_t> expected;
    int ew = 0, eh = 0;
    REQUIRE(sw::readPng(std::string(XT_GOLDEN_EXPECTED_DIR) + "/ui_text_decoration.png", expected, ew, eh));
    REQUIRE(ew == found->width);
    REQUIRE(eh == found->height);
    golden::GoldenResult result =
        golden::compareRgba(actual.data(), expected.data(), found->width, found->height, 16, 0.05f);
    CAPTURE(result.diffPixels);
    CAPTURE(result.maxChannelDiff);
    CHECK(result.pass);
}

// T-326 (18 §18.12): the 17 `mix-blend-mode` cells composite through the GL Layer path's
// Composite shader (every non-Normal mode), so GL must match xt_golden's SW image within the
// 15.3 tolerance (16/255, 3 % of the pixels).
TEST_CASE("gl golden: ui_blend_modes matches the SW expected image") {
    const std::string uiDir = XT_GOLDEN_UI_DIR;
    const std::vector<golden::UiFixture> fixtures = golden::loadUiManifest(uiDir + "/manifest.txt");
    REQUIRE(golden::manifestErrors() == 0);
    const golden::UiFixture* found = nullptr;
    for (const golden::UiFixture& fx : fixtures) {
        if (fx.name == "ui_blend_modes") {
            found = &fx;
        }
    }
    REQUIRE(found != nullptr);
    std::vector<uint8_t> actual = golden::renderUiFixture(
        *found, uiDir,
        [&](const golden::UiFixture& f, const CommandList& cmds, TextShaper* shaper, AssetResolver* assets,
            const IrView* ir) {
            GlSession gl(f.width, f.height);
            gl.target.bindAndClear(f.clearColor);
            gl.renderer.render(cmds, shaper, assets, ir);
            std::vector<uint8_t> pixels = gl.target.readback();
            std::vector<uint8_t> topDown(pixels.size());
            const size_t row = static_cast<size_t>(f.width) * 4;
            for (int y = 0; y < f.height; ++y) {
                std::copy(pixels.begin() + static_cast<long>(size_t(f.height - 1 - y) * row),
                          pixels.begin() + static_cast<long>(size_t(f.height - y) * row),
                          topDown.begin() + static_cast<long>(size_t(y) * row));
            }
            return topDown;
        });
    REQUIRE(!actual.empty());
    std::vector<uint8_t> expected;
    int ew = 0, eh = 0;
    REQUIRE(sw::readPng(std::string(XT_GOLDEN_EXPECTED_DIR) + "/ui_blend_modes.png", expected, ew, eh));
    REQUIRE(ew == found->width);
    REQUIRE(eh == found->height);
    golden::GoldenResult result =
        golden::compareRgba(actual.data(), expected.data(), found->width, found->height, 16, 0.03f);
    CHECK(result.pass);
}
// T-327 (05 §5.9.8): background-size / -position / -repeat are plain ImageCmd quads (the repeat
// split into one quad per tile, never a sampler wrap mode) and the rounded clip a PushClipPath, so
// GL draws the same image xt_golden draws with SW (Tests/golden/expected/ui_background_image.png).
// About 0.2% of the pixels differ (the rounded clip's antialiased edge and bilinear sampling at the
// cover / contain image edges), hence 0.01.
TEST_CASE("gl golden: ui_background_image matches the SW expected image") {
    const std::string uiDir = XT_GOLDEN_UI_DIR;
    const std::vector<golden::UiFixture> fixtures = golden::loadUiManifest(uiDir + "/manifest.txt");
    REQUIRE(golden::manifestErrors() == 0);
    const golden::UiFixture* found = nullptr;
    for (const golden::UiFixture& fx : fixtures) {
        if (fx.name == "ui_background_image") {
            found = &fx;
        }
    }
    REQUIRE(found != nullptr);
    std::vector<uint8_t> actual = golden::renderUiFixture(
        *found, uiDir,
        [&](const golden::UiFixture& f, const CommandList& cmds, TextShaper* shaper, AssetResolver* assets,
            const IrView* ir) {
            GlSession gl(f.width, f.height);
            gl.target.bindAndClear(f.clearColor);
            gl.renderer.render(cmds, shaper, assets, ir);
            std::vector<uint8_t> pixels = gl.target.readback();
            std::vector<uint8_t> topDown(pixels.size());
            const size_t row = static_cast<size_t>(f.width) * 4;
            for (int y = 0; y < f.height; ++y) {
                std::copy(pixels.begin() + static_cast<long>(size_t(f.height - 1 - y) * row),
                          pixels.begin() + static_cast<long>(size_t(f.height - y) * row),
                          topDown.begin() + static_cast<long>(size_t(y) * row));
            }
            return topDown;
        });
    REQUIRE(!actual.empty());
    std::vector<uint8_t> expected;
    int ew = 0, eh = 0;
    REQUIRE(sw::readPng(std::string(XT_GOLDEN_EXPECTED_DIR) + "/ui_background_image.png", expected, ew, eh));
    REQUIRE(ew == found->width);
    REQUIRE(eh == found->height);
    golden::GoldenResult result =
        golden::compareRgba(actual.data(), expected.data(), found->width, found->height, 16, 0.01f);
    CAPTURE(result.diffPixels);
    CAPTURE(result.maxChannelDiff);
    CHECK(result.pass);
}

// T-332 (05 §5.9.8): clip-path is a PushClipPath (a stencil level in GL, a coverage clip in SW),
// so GL draws the same image xt_golden draws with SW (Tests/golden/expected/ui_clip_path.png).
// Differences are confined to the antialiased shape edges, hence 16 / 0.03.
TEST_CASE("gl golden: ui_clip_path matches the SW expected image") {
    const std::string uiDir = XT_GOLDEN_UI_DIR;
    const std::vector<golden::UiFixture> fixtures = golden::loadUiManifest(uiDir + "/manifest.txt");
    REQUIRE(golden::manifestErrors() == 0);
    const golden::UiFixture* found = nullptr;
    for (const golden::UiFixture& fx : fixtures) {
        if (fx.name == "ui_clip_path") {
            found = &fx;
        }
    }
    REQUIRE(found != nullptr);
    std::vector<uint8_t> actual = golden::renderUiFixture(
        *found, uiDir,
        [&](const golden::UiFixture& f, const CommandList& cmds, TextShaper* shaper, AssetResolver* assets,
            const IrView* ir) {
            GlSession gl(f.width, f.height);
            gl.target.bindAndClear(f.clearColor);
            gl.renderer.render(cmds, shaper, assets, ir);
            std::vector<uint8_t> pixels = gl.target.readback();
            std::vector<uint8_t> topDown(pixels.size());
            const size_t row = static_cast<size_t>(f.width) * 4;
            for (int y = 0; y < f.height; ++y) {
                std::copy(pixels.begin() + static_cast<long>(size_t(f.height - 1 - y) * row),
                          pixels.begin() + static_cast<long>(size_t(f.height - y) * row),
                          topDown.begin() + static_cast<long>(size_t(y) * row));
            }
            return topDown;
        });
    REQUIRE(!actual.empty());
    std::vector<uint8_t> expected;
    int ew = 0, eh = 0;
    REQUIRE(sw::readPng(std::string(XT_GOLDEN_EXPECTED_DIR) + "/ui_clip_path.png", expected, ew, eh));
    REQUIRE(ew == found->width);
    REQUIRE(eh == found->height);
    golden::GoldenResult result =
        golden::compareRgba(actual.data(), expected.data(), found->width, found->height, 16, 0.03f);
    CAPTURE(result.diffPixels);
    CAPTURE(result.maxChannelDiff);
    CHECK(result.pass);
}

// T-334 (05 §5.9.8): color-adjust() invert()/sepia() (ColorAdjust slots 5/6) compose into the same
// 4x5 matrix on both backends, so GL must match xt_golden's SW image
// (Tests/golden/expected/fx_invert_sepia.png) within 1/255 on every channel of every pixel; the
// fixture's boxes are pixel-aligned, so there are no antialiased edges to excuse.
TEST_CASE("gl golden: fx_invert_sepia matches the SW expected image within 1/255") {
    const std::string uiDir = XT_GOLDEN_UI_DIR;
    const std::vector<golden::UiFixture> fixtures = golden::loadUiManifest(uiDir + "/manifest.txt");
    REQUIRE(golden::manifestErrors() == 0);
    const golden::UiFixture* found = nullptr;
    for (const golden::UiFixture& fx : fixtures) {
        if (fx.name == "fx_invert_sepia") {
            found = &fx;
        }
    }
    REQUIRE(found != nullptr);
    std::vector<uint8_t> actual = golden::renderUiFixture(
        *found, uiDir,
        [&](const golden::UiFixture& f, const CommandList& cmds, TextShaper* shaper, AssetResolver* assets,
            const IrView* ir) {
            GlSession gl(f.width, f.height);
            gl.target.bindAndClear(f.clearColor);
            gl.renderer.render(cmds, shaper, assets, ir);
            std::vector<uint8_t> pixels = gl.target.readback();
            std::vector<uint8_t> topDown(pixels.size());
            const size_t row = static_cast<size_t>(f.width) * 4;
            for (int y = 0; y < f.height; ++y) {
                std::copy(pixels.begin() + static_cast<long>(size_t(f.height - 1 - y) * row),
                          pixels.begin() + static_cast<long>(size_t(f.height - y) * row),
                          topDown.begin() + static_cast<long>(size_t(y) * row));
            }
            return topDown;
        });
    REQUIRE(!actual.empty());
    std::vector<uint8_t> expected;
    int ew = 0, eh = 0;
    REQUIRE(sw::readPng(std::string(XT_GOLDEN_EXPECTED_DIR) + "/fx_invert_sepia.png", expected, ew, eh));
    REQUIRE(ew == found->width);
    REQUIRE(eh == found->height);
    golden::GoldenResult result =
        golden::compareRgba(actual.data(), expected.data(), found->width, found->height, 1, 0.0f);
    MESSAGE("fx_invert_sepia SW/GL max channel diff: " << result.maxChannelDiff);
    CAPTURE(result.diffPixels);
    CAPTURE(result.maxChannelDiff);
    CHECK(result.pass);
    CHECK(result.maxChannelDiff <= 1);
}

// T-333 (05 §5.9.8): per-side borders, dashes and dots are plain FillPaths, so GL draws the same
// image xt_golden draws with SW (Tests/golden/expected/ui_border_styles.png). Differences are
// confined to the antialiased edges, hence 16 / 0.03.
TEST_CASE("gl golden: ui_border_styles matches the SW expected image") {
    const std::string uiDir = XT_GOLDEN_UI_DIR;
    const std::vector<golden::UiFixture> fixtures = golden::loadUiManifest(uiDir + "/manifest.txt");
    REQUIRE(golden::manifestErrors() == 0);
    const golden::UiFixture* found = nullptr;
    for (const golden::UiFixture& fx : fixtures) {
        if (fx.name == "ui_border_styles") {
            found = &fx;
        }
    }
    REQUIRE(found != nullptr);
    std::vector<uint8_t> actual = golden::renderUiFixture(
        *found, uiDir,
        [&](const golden::UiFixture& f, const CommandList& cmds, TextShaper* shaper, AssetResolver* assets,
            const IrView* ir) {
            GlSession gl(f.width, f.height);
            gl.target.bindAndClear(f.clearColor);
            gl.renderer.render(cmds, shaper, assets, ir);
            std::vector<uint8_t> pixels = gl.target.readback();
            std::vector<uint8_t> topDown(pixels.size());
            const size_t row = static_cast<size_t>(f.width) * 4;
            for (int y = 0; y < f.height; ++y) {
                std::copy(pixels.begin() + static_cast<long>(size_t(f.height - 1 - y) * row),
                          pixels.begin() + static_cast<long>(size_t(f.height - y) * row),
                          topDown.begin() + static_cast<long>(size_t(y) * row));
            }
            return topDown;
        });
    REQUIRE(!actual.empty());
    std::vector<uint8_t> expected;
    int ew = 0, eh = 0;
    REQUIRE(sw::readPng(std::string(XT_GOLDEN_EXPECTED_DIR) + "/ui_border_styles.png", expected, ew, eh));
    REQUIRE(ew == found->width);
    REQUIRE(eh == found->height);
    golden::GoldenResult result =
        golden::compareRgba(actual.data(), expected.data(), found->width, found->height, 16, 0.03f);
    CAPTURE(result.diffPixels);
    CAPTURE(result.maxChannelDiff);
    CHECK(result.pass);
}

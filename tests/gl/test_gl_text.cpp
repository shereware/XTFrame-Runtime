// Requires xt_text (XT_BUILD_TEXT); the whole file is excluded otherwise (docs/tasks/T-31.md
// "tests/gl/test_gl_text.cpp // 需要 xt_text（XT_BUILD_TEXT）；否則整檔以 #if 排除").
#ifndef XT_GL_NO_TEXT

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "gl_loader.h"
#include "golden_common.h"
#include "xt/command_list.h"
#include "xt/gl/gl_renderer.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text/ft_text_shaper.h"
#include "xt/text/glyph_atlas.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// DrawGlyphs GL backend tests (docs/tasks/T-31.md "測試"). Renders into its own 128x128 offscreen
/// FBO (RGBA8 + GL_DEPTH24_STENCIL8, no MSAA) exactly like test_gl_renderer.cpp; main()
/// (test_gl_main.cpp) guarantees a current GL context exists before any TEST_CASE runs.
///
/// The sheet asks to compare the atlas raster against "SW rendering of the same command"; SW's
/// own DrawGlyphs support does not exist in this tree (deferred to T-30, not yet landed here per
/// sw/src/sw_renderer.cpp's "Deferred to T-30" comment), so that comparison is instead done
/// against GlRenderer's own outline-fill fallback (setGlyphAtlas(nullptr)) for the identical
/// GlyphsCmd -- both are anti-aliased fills of the same outline data, just rasterized two
/// different ways, which is the same kind of comparison the sheet's tolerance (each channel <=
/// 48/255, <= 8% differing pixels) is sized for ("點陣 vs 外框抗鋸齒差異"). Not covered verbatim
/// by the task sheet; a conservative substitution given SW glyph rendering isn't available yet.

using namespace xt;

namespace {

// -------------------------------------------------------------------------------------------
// Offscreen render target + readback (mirrors test_gl_renderer.cpp; duplicated rather than
// shared since each xt_gl_tests translation unit keeps its own anonymous namespace).
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

struct Px {
    uint8_t r, g, b, a;
};
Px pixelAt(const std::vector<uint8_t>& rgba, int width, int height, int x, int y) {
    int flippedY = height - 1 - y;
    size_t idx = (static_cast<size_t>(flippedY) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
    return Px{rgba[idx], rgba[idx + 1], rgba[idx + 2], rgba[idx + 3]};
}

bool isDark(Px p, int threshold = 128) { return p.r < threshold && p.g < threshold && p.b < threshold; }

int countDark(const std::vector<uint8_t>& rgba, int width, int height, int threshold = 128) {
    int n = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (isDark(pixelAt(rgba, width, height, x, y), threshold)) ++n;
        }
    }
    return n;
}

std::string fontPath(const char* name) { return std::string(XT_TEST_FONTS_DIR) + "/" + name; }

struct Fixture {
    text::FtTextShaper shaper;
    Fixture() { REQUIRE(shaper.addFontFile(fontPath("NotoSans-Regular.subset.ttf"))); }
};

const std::vector<std::string_view>& notoFamily() {
    static const std::vector<std::string_view> families = {"Noto Sans"};
    return families;
}

/// Shapes `text` at `size` px and builds a single-run GlyphsCmd CommandList (baseline origin
/// `origin`, world transform `xf`), matching the (font, size, color, glyphs, positions) layout
/// Painter would produce for one GlyphRun (docs/design/09-command-list.md).
CommandList buildGlyphsList(text::FtTextShaper& shaper, const std::string& text, float size, Vec2 origin,
                            const Mat2x3& xf, Color color, int vpW, int vpH) {
    ShapeParams params;
    params.font = FontRequest{std::span<const std::string_view>(notoFamily()), 400, false};
    params.size = size;
    ShapeResult shaped;
    shaper.shape(text, params, shaped);
    REQUIRE(!shaped.glyphs.empty());
    REQUIRE(!shaped.runs.empty());

    CommandList list;
    list.viewport = Rect{0, 0, static_cast<float>(vpW), static_cast<float>(vpH)};
    list.devicePixelRatio = 1.0f;

    GlyphsCmd cmd{};
    cmd.xf = xf;
    cmd.font = shaped.runs[0].font;
    cmd.size = size;
    cmd.color = color;
    cmd.glyphOffset = static_cast<uint32_t>(list.glyphs.size());
    float penX = origin.x;
    for (const ShapedGlyph& g : shaped.glyphs) {
        list.glyphs.push_back(g.glyphId);
        list.glyphPos.push_back(Vec2{penX + g.xOffset, origin.y + g.yOffset});
        penX += g.xAdvance;
    }
    cmd.glyphCount = static_cast<uint32_t>(list.glyphs.size()) - cmd.glyphOffset;
    list.glyphRuns.push_back(cmd);
    list.commands.push_back(Command{CmdKind::DrawGlyphs, {}, 0});
    return list;
}

std::vector<uint8_t> renderGlyphs(const CommandList& list, int w, int h, Color clearColor, TextShaper* shaper,
                                  text::GlyphAtlas* atlas) {
    static bool loaded = false;
    if (!loaded) {
        REQUIRE(xt::gl::loadGl(sdlLoadProc));
        loaded = true;
    }
    GlTestTarget target;
    target.init(w, h);
    target.bindAndClear(clearColor);

    gl::GlRenderer renderer;
    REQUIRE(renderer.init(sdlLoadProc));
    renderer.resize(w, h);
    target.bindAndClear(clearColor);  // resize()/init() may have touched GL state; re-clear.
    renderer.setGlyphAtlas(atlas);
    renderer.render(list, shaper, nullptr, nullptr);
    std::vector<uint8_t> pixels = target.readback();
    renderer.shutdown();
    target.destroy();
    return pixels;
}

}  // namespace

// =================================================================================================
// Atlas raster vs outline fallback of the identical command.
// =================================================================================================

TEST_CASE("gl: DrawGlyphs atlas raster has dark pixels and roughly matches the outline fallback") {
    Fixture f;
    CommandList list =
        buildGlyphsList(f.shaper, "H", 48.0f, Vec2{40, 96}, Mat2x3{}, Color{0, 0, 0, 1}, 128, 128);

    text::GlyphAtlas atlas(f.shaper);
    std::vector<uint8_t> atlasPx = renderGlyphs(list, 128, 128, Color{1, 1, 1, 1}, &f.shaper, &atlas);

    // Central row of "H" (drawn near mid-height) should have a run of continuous dark pixels.
    int darkRun = 0, maxDarkRun = 0;
    for (int x = 0; x < 128; ++x) {
        if (isDark(pixelAt(atlasPx, 128, 128, x, 75))) {
            ++darkRun;
            maxDarkRun = std::max(maxDarkRun, darkRun);
        } else {
            darkRun = 0;
        }
    }
    CHECK(maxDarkRun >= 3);

    std::vector<uint8_t> outlinePx = renderGlyphs(list, 128, 128, Color{1, 1, 1, 1}, &f.shaper, nullptr);

    int diffPixels = 0;
    const int kChannelTol = 48;
    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 128; ++x) {
            Px a = pixelAt(atlasPx, 128, 128, x, y);
            Px b = pixelAt(outlinePx, 128, 128, x, y);
            bool close = std::abs(static_cast<int>(a.r) - b.r) <= kChannelTol &&
                         std::abs(static_cast<int>(a.g) - b.g) <= kChannelTol &&
                         std::abs(static_cast<int>(a.b) - b.b) <= kChannelTol;
            if (!close) ++diffPixels;
        }
    }
    CHECK(diffPixels <= (128 * 128 * 8) / 100);
}

// =================================================================================================
// Rotated xf -> outline fallback (atlas set, but the run's xf rotates).
// =================================================================================================

TEST_CASE("gl: DrawGlyphs rotated xf falls back to outline fill") {
    Fixture f;
    Mat2x3 xf = mul(translation(Vec2{64, 64}), rotationDeg(45.0f));
    CommandList list = buildGlyphsList(f.shaper, "H", 48.0f, Vec2{-24, 20}, xf, Color{0, 0, 0, 1}, 128, 128);

    text::GlyphAtlas atlas(f.shaper);
    std::vector<uint8_t> px = renderGlyphs(list, 128, 128, Color{1, 1, 1, 1}, &f.shaper, &atlas);

    CHECK(countDark(px, 128, 128) > 0);
    // A corner far from the rotated glyph's (rotated) bounding box stays background.
    CHECK_FALSE(isDark(pixelAt(px, 128, 128, 2, 2)));
    CHECK_FALSE(isDark(pixelAt(px, 128, 128, 125, 125)));
}

// =================================================================================================
// Atlas upload: a second identical frame uploads nothing new.
// =================================================================================================

TEST_CASE("gl: GlyphAtlas flushUploads does not re-upload on an unchanged second frame") {
    Fixture f;
    text::GlyphAtlas atlas(f.shaper);

    ShapeParams params;
    params.font = FontRequest{std::span<const std::string_view>(notoFamily()), 400, false};
    params.size = 32.0f;
    ShapeResult shaped;
    f.shaper.shape("H", params, shaped);
    REQUIRE(!shaped.glyphs.empty());
    FontId font = shaped.runs[0].font;
    uint32_t glyphId = shaped.glyphs[0].glyphId;

    // Frame 1: first lookup rasterizes and dirties the atlas; draining it now mirrors
    // GlRenderer::doGlyphs's "flush right after every successful lookup" invariant.
    text::AtlasEntry entry{};
    REQUIRE(atlas.lookup(font, glyphId, 32.0f, entry));
    int uploadsFrame1 = 0;
    atlas.flushUploads([&](uint16_t, int, int, int, int, const uint8_t*, int) { ++uploadsFrame1; });
    CHECK(uploadsFrame1 > 0);

    // Frame 2: same (font, glyph, size) key -> cache hit, no new pixels, nothing dirty.
    text::AtlasEntry entry2{};
    REQUIRE(atlas.lookup(font, glyphId, 32.0f, entry2));
    int uploadsFrame2 = 0;
    atlas.flushUploads([&](uint16_t, int, int, int, int, const uint8_t*, int) { ++uploadsFrame2; });
    CHECK(uploadsFrame2 == 0);
}

// =================================================================================================
// Atlas overflow (small texture) -> reset -> still renders correctly afterward.
// =================================================================================================

TEST_CASE("gl: DrawGlyphs renders correctly after an atlas reset") {
    Fixture f;
    // A small 256x256 atlas (docs/tasks/T-31.md's optional GlyphAtlas(shaper, textureSize)
    // constructor param) overflows quickly at large sizes.
    text::GlyphAtlas atlas(f.shaper, 256);

    CommandList list;
    list.viewport = Rect{0, 0, 128, 128};
    list.devicePixelRatio = 1.0f;

    // Junk glyphs at distinct large sizes (distinct cache keys, off-screen position so they don't
    // interfere visually) to fill and overflow the tiny atlas before the real, visible draw.
    ShapeParams junkParams;
    junkParams.font = FontRequest{std::span<const std::string_view>(notoFamily()), 400, false};
    for (int i = 0; i < 12; ++i) {
        char c = static_cast<char>('A' + (i % 26));
        std::string s(1, c);
        junkParams.size = 120.0f + static_cast<float>(i) * 10.0f;
        ShapeResult junkShaped;
        f.shaper.shape(s, junkParams, junkShaped);
        if (junkShaped.glyphs.empty()) continue;
        GlyphsCmd junk{};
        junk.xf = Mat2x3{};
        junk.font = junkShaped.runs[0].font;
        junk.size = junkParams.size;
        junk.color = Color{0, 0, 0, 1};
        junk.glyphOffset = static_cast<uint32_t>(list.glyphs.size());
        list.glyphs.push_back(junkShaped.glyphs[0].glyphId);
        list.glyphPos.push_back(Vec2{-500, -500});  // off-screen: never visible.
        junk.glyphCount = 1;
        list.glyphRuns.push_back(junk);
        list.commands.push_back(
            Command{CmdKind::DrawGlyphs, {}, static_cast<uint32_t>(list.glyphRuns.size() - 1)});
    }

    // The real, visible glyph, drawn last.
    ShapeParams realParams;
    realParams.font = FontRequest{std::span<const std::string_view>(notoFamily()), 400, false};
    realParams.size = 48.0f;
    ShapeResult realShaped;
    f.shaper.shape("H", realParams, realShaped);
    REQUIRE(!realShaped.glyphs.empty());
    GlyphsCmd real{};
    real.xf = Mat2x3{};
    real.font = realShaped.runs[0].font;
    real.size = 48.0f;
    real.color = Color{0, 0, 0, 1};
    real.glyphOffset = static_cast<uint32_t>(list.glyphs.size());
    list.glyphs.push_back(realShaped.glyphs[0].glyphId);
    list.glyphPos.push_back(Vec2{40, 96});
    real.glyphCount = 1;
    list.glyphRuns.push_back(real);
    list.commands.push_back(
        Command{CmdKind::DrawGlyphs, {}, static_cast<uint32_t>(list.glyphRuns.size() - 1)});

    std::vector<uint8_t> px = renderGlyphs(list, 128, 128, Color{1, 1, 1, 1}, &f.shaper, &atlas);
    CHECK(countDark(px, 128, 128) > 0);
}

// =================================================================================================
// setGlyphAtlas(nullptr) -> every glyph uses the outline fallback.
// =================================================================================================

TEST_CASE("gl: DrawGlyphs with no glyph atlas uses the outline fallback") {
    Fixture f;
    CommandList list =
        buildGlyphsList(f.shaper, "H", 48.0f, Vec2{40, 96}, Mat2x3{}, Color{0, 0, 0, 1}, 128, 128);

    std::vector<uint8_t> px = renderGlyphs(list, 128, 128, Color{1, 1, 1, 1}, &f.shaper, nullptr);
    CHECK(countDark(px, 128, 128) > 0);
}

// =================================================================================================
// T-178: glyph-level text effects (GlyphEffect) -- GL atlas second sampling vs SW bitmap path.
// =================================================================================================

namespace {

/// `base` (one plain run) plus effect commands sharing its glyphs, drawn first: a Blur (shadow)
/// offset by (3, 3), then a Dilate (outline).
CommandList buildEffectList(text::FtTextShaper& shaper, float blurRadius, float outlineWidth) {
    CommandList list =
        buildGlyphsList(shaper, "Hag", 40.0f, Vec2{12, 62}, Mat2x3{}, Color{1, 1, 1, 1}, 128, 96);
    const GlyphsCmd fill = list.glyphRuns[0];
    list.glyphRuns.clear();
    list.commands.clear();
    GlyphsCmd shadow = fill;
    shadow.effect = GlyphEffect::Blur;
    shadow.effectRadius = blurRadius;
    shadow.color = Color{1, 0.8f, 0, 1};
    shadow.xf = translation(Vec2{3, 3});
    GlyphsCmd outline = fill;
    outline.effect = GlyphEffect::Dilate;
    outline.effectRadius = outlineWidth;
    outline.color = Color{0, 0, 0, 1};
    for (const GlyphsCmd& cmd : {shadow, outline, fill}) {
        list.commands.push_back(
            Command{CmdKind::DrawGlyphs, {}, static_cast<uint32_t>(list.glyphRuns.size())});
        list.glyphRuns.push_back(cmd);
    }
    return list;
}

std::vector<uint8_t> renderSwTopDown(const CommandList& list, int w, int h, TextShaper* shaper) {
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    sw::SwRenderer renderer;
    sw::SwTarget target{buf.data(), w, h, w * 4};
    renderer.setTarget(target);
    renderer.clear(Color{0.2f, 0.3f, 0.4f, 1});
    renderer.render(list, shaper, nullptr);
    return buf;
}

std::vector<uint8_t> flipRows(const std::vector<uint8_t>& bottomUp, int w, int h) {
    std::vector<uint8_t> topDown(bottomUp.size());
    for (int y = 0; y < h; ++y) {
        std::copy_n(
            bottomUp.begin() + static_cast<long>(static_cast<size_t>(h - 1 - y) * static_cast<size_t>(w) * 4),
            w * 4, topDown.begin() + static_cast<long>(static_cast<size_t>(y) * static_cast<size_t>(w) * 4));
    }
    return topDown;
}

}  // namespace

TEST_CASE("gl: glyph text effects (outline + blurred shadow) match SW") {
    Fixture f;
    const int w = 128;
    const int h = 96;
    for (float radius : {0.0f, 2.0f, 6.0f}) {
        CAPTURE(radius);
        // One atlas per GlRenderer: a new renderer creates its atlas texture empty and uploads only
        // glyphs added since the atlas's last flush, so a shared atlas would leave it blank (Mesa
        // zero-fills new textures; other drivers happened to show the previous texture's memory).
        text::GlyphAtlas atlas(f.shaper);
        CommandList list = buildEffectList(f.shaper, radius, radius * 0.5f + 1.0f);
        std::vector<uint8_t> gl =
            flipRows(renderGlyphs(list, w, h, Color{0.2f, 0.3f, 0.4f, 1}, &f.shaper, &atlas), w, h);
        std::vector<uint8_t> sw = renderSwTopDown(list, w, h, &f.shaper);
        golden::GoldenResult result = golden::compareRgba(gl.data(), sw.data(), w, h, 16, 0.03f);
        CAPTURE(result.diffPixels);
        CAPTURE(result.maxChannelDiff);
        CHECK(result.pass);
        // The effects actually drew: many pixels differ from the plain fill alone.
        CommandList plain = list;
        plain.commands.erase(plain.commands.begin(), plain.commands.begin() + 2);
        std::vector<uint8_t> fillOnly = renderSwTopDown(plain, w, h, &f.shaper);
        int changed = 0;
        for (size_t i = 0; i < sw.size(); i += 4) {
            int d = std::abs(sw[i] - fillOnly[i]) + std::abs(sw[i + 1] - fillOnly[i + 1]) +
                    std::abs(sw[i + 2] - fillOnly[i + 2]);
            changed += d > 60 ? 1 : 0;
        }
        CHECK(changed > 150);
        // The effects alone (no fill, whose SW outline raster differs from the GL atlas raster at
        // this size) agree much more tightly: both backends compute them from the same bitmaps.
        CommandList effectsOnly = list;
        effectsOnly.commands.resize(2);
        text::GlyphAtlas fxAtlas(f.shaper);
        std::vector<uint8_t> glFx =
            flipRows(renderGlyphs(effectsOnly, w, h, Color{0.2f, 0.3f, 0.4f, 1}, &f.shaper, &fxAtlas), w, h);
        std::vector<uint8_t> swFx = renderSwTopDown(effectsOnly, w, h, &f.shaper);
        golden::GoldenResult fx = golden::compareRgba(glFx.data(), swFx.data(), w, h, 4, 0.002f);
        CAPTURE(fx.diffPixels);
        CAPTURE(fx.maxChannelDiff);
        CHECK(fx.pass);
    }
}

TEST_CASE("gl: a glyph effect on a rotated run draws nothing (no atlas coverage)") {
    Fixture f;
    text::GlyphAtlas atlas(f.shaper);
    CommandList list = buildEffectList(f.shaper, 2.0f, 2.0f);
    list.glyphRuns[1].xf = mul(translation(Vec2{64, 48}), mul(rotationDeg(30), translation(Vec2{-64, -48})));
    list.commands.resize(2);  // shadow + rotated outline, no fill
    list.commands[0] = list.commands[1];
    list.commands.resize(1);
    std::vector<uint8_t> px = renderGlyphs(list, 128, 96, Color{1, 1, 1, 1}, &f.shaper, &atlas);
    CHECK(countDark(px, 128, 96) == 0);
}

// T-263e ruling D: one glyph-effect run may occupy at most a quarter of an atlas page; a run over
// that is drawn from its outlines, without the effect.
TEST_CASE("gl: T-263e a glyph-effect run over the atlas bound is drawn as plain outlines") {
    Fixture f;
    const CommandList plain =
        buildGlyphsList(f.shaper, "H", 48.0f, Vec2{40, 96}, Mat2x3{}, Color{0, 0, 0, 1}, 128, 128);
    CommandList effect = plain;
    effect.glyphRuns[0].effect = GlyphEffect::Dilate;
    effect.glyphRuns[0].effectRadius = 3.0f;

    // A 64 px page: a quarter of it is 1024 px, less than the run's one 48 px em square.
    text::GlyphAtlas small(f.shaper, 64);
    const std::vector<uint8_t> bounded = renderGlyphs(effect, 128, 128, Color{1, 1, 1, 1}, &f.shaper, &small);
    const std::vector<uint8_t> outline = renderGlyphs(plain, 128, 128, Color{1, 1, 1, 1}, &f.shaper, nullptr);
    CHECK(countDark(outline, 128, 128) > 0);
    CHECK(bounded == outline);  // the same outline fill, no dilation

    // With room in the atlas the effect is drawn: the dilated glyph covers more pixels.
    text::GlyphAtlas large(f.shaper);
    const std::vector<uint8_t> dilated = renderGlyphs(effect, 128, 128, Color{1, 1, 1, 1}, &f.shaper, &large);
    CHECK(countDark(dilated, 128, 128) > countDark(outline, 128, 128));
}

TEST_CASE("gl: T-263b glyph atlas uploads do not depend on the host's unpack state") {
    using namespace xt::gl;
    Fixture f;
    const int w = 96;
    const int h = 32;
    CommandList list =
        buildGlyphsList(f.shaper, "Hello", 16.0f, Vec2{4, 22}, Mat2x3{}, Color{0, 0, 0, 1}, w, h);
    text::GlyphAtlas cleanAtlas(f.shaper);
    std::vector<uint8_t> clean = renderGlyphs(list, w, h, Color{1, 1, 1, 1}, &f.shaper, &cleanAtlas);
    REQUIRE(countDark(clean, w, h) > 50);

    // A streaming host's leftovers (small values: a loader honoring them reads other texels of
    // the atlas page, not memory outside it).
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 3);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 1);
    text::GlyphAtlas atlas(f.shaper);
    std::vector<uint8_t> px = renderGlyphs(list, w, h, Color{1, 1, 1, 1}, &f.shaper, &atlas);
    GLint v = -1;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &v);
    CHECK(v == 3);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &v);
    CHECK(v == 1);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &v);
    CHECK(v == 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    CHECK(px == clean);
}

TEST_CASE("gl: T-263b a scaled run's atlas glyphs are as large as SW's bitmaps") {
    Fixture f;
    const int w = 96;
    const int h = 32;
    // 9 px text under scale(2): 18 device px, which SW draws from hinted bitmaps (below 24 px).
    CommandList list =
        buildGlyphsList(f.shaper, "Hello", 9.0f, Vec2{3, 11}, scaling(Vec2{2, 2}), Color{0, 0, 0, 1}, w, h);
    text::GlyphAtlas atlas(f.shaper);
    std::vector<uint8_t> glBottomUp = renderGlyphs(list, w, h, Color{0.2f, 0.3f, 0.4f, 1}, &f.shaper, &atlas);
    std::vector<uint8_t> gl = flipRows(glBottomUp, w, h);
    std::vector<uint8_t> sw = renderSwTopDown(list, w, h, &f.shaper);
    // Dark (ink) pixels: GL at the unscaled size had about a quarter of SW's.
    auto ink = [&](const std::vector<uint8_t>& px) {
        int n = 0;
        for (size_t i = 0; i < px.size(); i += 4) {
            n += (px[i] < 26 && px[i + 1] < 38) ? 1 : 0;
        }
        return n;
    };
    const int swInk = ink(sw);
    const int glInk = ink(gl);
    CAPTURE(swInk);
    CAPTURE(glInk);
    CHECK(swInk > 100);
    CHECK(std::abs(glInk - swInk) * 10 <= swInk);
    golden::GoldenResult result = golden::compareRgba(gl.data(), sw.data(), w, h, 24, 0.03f);
    CAPTURE(result.diffPixels);
    CAPTURE(result.maxChannelDiff);
    CHECK(result.pass);
}

#endif  // XT_GL_NO_TEXT

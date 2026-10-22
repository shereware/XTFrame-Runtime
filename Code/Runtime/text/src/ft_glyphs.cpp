#include <ft2build.h>

#include <cmath>
#include <cstdint>
#include <vector>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include "xt/node.h"
#include "xt/text_shaper.h"

/// Glyph outline decomposition (FT_Outline_Decompose, conic -> cubic) and bitmap rendering. See
/// Docs/design/07-text.md §7.3 "字形外框"/"點陣". Stateless: caching lives in FtTextShaper
/// (ft_text_shaper.cpp), which is the only place that knows about FontId and holds the mutex
/// guarding repeated calls.

namespace xt::text {

namespace {

struct DecomposeCtx {
    std::vector<PathVerb>* verbs;
    std::vector<float>* points;
    float invUpem;
};

float xf(const DecomposeCtx& ctx, FT_Pos v) { return static_cast<float>(v) * ctx.invUpem; }
// y is flipped: FreeType outlines are y-up (font convention); xt_text's contract is y-down
// (Docs/design/07-text.md §7.3 "字形外框": "除以 unitsPerEm，y 翻轉").
float yf(const DecomposeCtx& ctx, FT_Pos v) { return -static_cast<float>(v) * ctx.invUpem; }

int moveTo(const FT_Vector* to, void* user) {
    auto* ctx = static_cast<DecomposeCtx*>(user);
    ctx->verbs->push_back(PathVerb::Move);
    ctx->points->push_back(xf(*ctx, to->x));
    ctx->points->push_back(yf(*ctx, to->y));
    return 0;
}

int lineTo(const FT_Vector* to, void* user) {
    auto* ctx = static_cast<DecomposeCtx*>(user);
    ctx->verbs->push_back(PathVerb::Line);
    ctx->points->push_back(xf(*ctx, to->x));
    ctx->points->push_back(yf(*ctx, to->y));
    return 0;
}

int conicTo(const FT_Vector* control, const FT_Vector* to, void* user) {
    auto* ctx = static_cast<DecomposeCtx*>(user);
    std::vector<float>& pts = *ctx->points;
    // Current point is the last point already emitted (Move/Line/Cubic all end with x,y).
    const float x0 = pts[pts.size() - 2];
    const float y0 = pts[pts.size() - 1];
    const float cx = xf(*ctx, control->x);
    const float cy = yf(*ctx, control->y);
    const float x1 = xf(*ctx, to->x);
    const float y1 = yf(*ctx, to->y);
    // Degree elevation, quadratic -> cubic (2/3 rule): CP1 = P0 + 2/3(C-P0), CP2 = P1 + 2/3(C-P1).
    const float cp1x = x0 + (2.0f / 3.0f) * (cx - x0);
    const float cp1y = y0 + (2.0f / 3.0f) * (cy - y0);
    const float cp2x = x1 + (2.0f / 3.0f) * (cx - x1);
    const float cp2y = y1 + (2.0f / 3.0f) * (cy - y1);
    ctx->verbs->push_back(PathVerb::Cubic);
    pts.push_back(cp1x);
    pts.push_back(cp1y);
    pts.push_back(cp2x);
    pts.push_back(cp2y);
    pts.push_back(x1);
    pts.push_back(y1);
    return 0;
}

int cubicTo(const FT_Vector* control1, const FT_Vector* control2, const FT_Vector* to, void* user) {
    auto* ctx = static_cast<DecomposeCtx*>(user);
    ctx->verbs->push_back(PathVerb::Cubic);
    ctx->points->push_back(xf(*ctx, control1->x));
    ctx->points->push_back(yf(*ctx, control1->y));
    ctx->points->push_back(xf(*ctx, control2->x));
    ctx->points->push_back(yf(*ctx, control2->y));
    ctx->points->push_back(xf(*ctx, to->x));
    ctx->points->push_back(yf(*ctx, to->y));
    return 0;
}

}  // namespace

bool ftDecomposeOutline(FT_Face face, uint32_t glyphId, std::vector<PathVerb>& outVerbs,
                        std::vector<float>& outPoints) {
    outVerbs.clear();
    outPoints.clear();
    if (face == nullptr) return false;
    if (FT_Load_Glyph(face, glyphId, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) != 0) return false;
    if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) return false;

    const float upem = face->units_per_EM != 0 ? static_cast<float>(face->units_per_EM) : 1000.0f;
    DecomposeCtx ctx{&outVerbs, &outPoints, 1.0f / upem};

    FT_Outline_Funcs funcs{};
    funcs.move_to = &moveTo;
    funcs.line_to = &lineTo;
    funcs.conic_to = &conicTo;
    funcs.cubic_to = &cubicTo;
    funcs.shift = 0;
    funcs.delta = 0;

    if (FT_Outline_Decompose(&face->glyph->outline, &funcs, &ctx) != 0) return false;

    // FT_Outline_Decompose does not emit a verb closing each contour; §3.4's verb format expects
    // an explicit PathVerb::Close per subpath. outline.contours[] gives per-contour end point
    // indices into FreeType's own point array, which no longer lines up 1:1 with our verb/point
    // stream once conics have been elevated to cubics, so instead a Close is inserted right
    // before every Move that is not the stream's first verb, and one final Close after the last
    // contour.
    std::vector<PathVerb> closedVerbs;
    std::vector<float> closedPoints;
    closedVerbs.reserve(outVerbs.size() + face->glyph->outline.n_contours);
    closedPoints.reserve(outPoints.size());
    size_t pi = 0;
    for (size_t vi = 0; vi < outVerbs.size(); ++vi) {
        const PathVerb v = outVerbs[vi];
        if (v == PathVerb::Move && vi != 0) {
            closedVerbs.push_back(PathVerb::Close);
        }
        closedVerbs.push_back(v);
        const int n = v == PathVerb::Cubic ? 6 : 2;
        for (int k = 0; k < n; ++k) closedPoints.push_back(outPoints[pi++]);
    }
    if (!outVerbs.empty()) closedVerbs.push_back(PathVerb::Close);

    outVerbs = std::move(closedVerbs);
    outPoints = std::move(closedPoints);
    return !outVerbs.empty();
}

bool ftRenderBitmap(FT_Face face, uint32_t glyphId, float sizePx, float subpixelX, GlyphBitmap& out) {
    if (face == nullptr || sizePx <= 0) return false;
    const FT_F26Dot6 charSize = static_cast<FT_F26Dot6>(std::lround(sizePx * 64.0f));
    if (FT_Set_Char_Size(face, 0, charSize, 0, 0) != 0) return false;
    // FT_LOAD_TARGET_LIGHT: vertical-only hinting (Docs/design/23-host-integration.md §23.4).
    // Stems keep their true horizontal position and width -- which is what makes the 1/4 px
    // subpixel variants below meaningful -- while baselines and x-heights snap to the pixel grid,
    // so small text stops looking blurry and unevenly weighted.
    if (FT_Load_Glyph(face, glyphId, FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT) != 0) return false;
    // Quantize to the 1/4 px grid the bitmap cache keys on, then shift the (hinted) outline by
    // that fraction of a pixel before rasterizing. 26.6 fixed point, so 1/4 px == 16 units.
    const float quantized = std::floor(subpixelX * 4.0f + 0.5f) / 4.0f;
    if (quantized != 0.0f && face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        FT_Outline_Translate(&face->glyph->outline, static_cast<FT_Pos>(std::lround(quantized * 64.0f)), 0);
    }
    if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) return false;

    const FT_Bitmap& bmp = face->glyph->bitmap;
    out.width = static_cast<int>(bmp.width);
    out.height = static_cast<int>(bmp.rows);
    out.bearingX = face->glyph->bitmap_left;
    out.bearingY = face->glyph->bitmap_top;
    out.originX = quantized;
    out.alpha.assign(static_cast<size_t>(out.width) * static_cast<size_t>(out.height), 0);

    if (bmp.buffer != nullptr && bmp.pixel_mode == FT_PIXEL_MODE_GRAY) {
        const int pitch = bmp.pitch;
        const int absPitch = pitch < 0 ? -pitch : pitch;
        for (int row = 0; row < out.height; ++row) {
            const unsigned char* src = bmp.buffer + (pitch < 0 ? static_cast<size_t>(out.height - 1 - row)
                                                               : static_cast<size_t>(row)) *
                                                        static_cast<size_t>(absPitch);
            for (int col = 0; col < out.width; ++col) {
                out.alpha[static_cast<size_t>(row) * static_cast<size_t>(out.width) +
                          static_cast<size_t>(col)] = src[col];
            }
        }
    }
    return true;
}

}  // namespace xt::text

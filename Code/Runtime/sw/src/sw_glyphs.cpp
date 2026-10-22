#include <algorithm>
#include <cmath>
#include <vector>

#include "sw_blend.h"
#include "sw_layer.h"
#include "xt/command_list.h"
#include "xt/geom.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// DrawGlyphs filling for the SW backend: the outline path of Docs/design/10-backends.md §10.2.6
/// ("for every glyph take glyphOutline, scale by `size`, translate to the position, then go
/// through the fill path"), plus the small-size bitmap path T-101 added
/// (Docs/design/23-host-integration.md §23.4). Glyph outline/bitmap caching is the shaper's job.
/// See Docs/tasks/T-30.md goal 4, Docs/tasks/T-101.md.
///
/// At device sizes below 24 px the outline path's unhinted, arbitrary-phase coverage makes stems
/// blurry and unevenly weighted, so `text-rendering: auto` runs FreeType's lightly hinted bitmap
/// rasterizer instead, at 1/4 px horizontal subpixel positions, and blends the coverage through a
/// gamma curve whose direction depends on the text/background contrast.
///
/// Not covered by the design docs (conservative choices made here):
///   - A glyph without an outline (space, or an unsupported font) is skipped, not replaced by a
///     .notdef box; a glyph the shaper has no *bitmap* for is skipped the same way.
///   - A DrawGlyphs command is all-bitmap or all-outline: the decision is made once per command,
///     from its size/transform/text-rendering, never per glyph.

namespace xt::sw {

bool buildGlyphContours(TextShaper& shaper, const CommandList& list, const GlyphsCmd& cmd,
                        const Mat2x3& deviceXf, std::vector<Vec2>& outPoints,
                        std::vector<uint32_t>& outStarts) {
    outPoints.clear();
    outStarts.clear();
    if (cmd.font == kInvalidFont || cmd.glyphCount == 0) {
        return false;
    }
    std::vector<PathVerb> verbs;
    std::vector<float> points;
    std::vector<uint8_t> closed;
    for (uint32_t i = 0; i < cmd.glyphCount; ++i) {
        uint32_t index = cmd.glyphOffset + i;
        if (index >= list.glyphs.size() || index >= list.glyphPos.size()) {
            break;  // Defensive: a well-formed CommandList always has both pools in step.
        }
        verbs.clear();
        points.clear();
        if (!shaper.glyphOutline(cmd.font, list.glyphs[index], verbs, points)) {
            continue;
        }
        // Outlines are in em units (y down), so the glyph's own transform is "scale by the font
        // size, then move to the glyph's origin".
        Mat2x3 local = mul(translation(list.glyphPos[index]), scaling(Vec2{cmd.size, cmd.size}));
        closed.clear();
        flattenPath(verbs, points, mul(deviceXf, local), kFlattenTolerance, outPoints, outStarts, closed);
    }
    return !outPoints.empty();
}

namespace {

/// Perceived brightness, §23.4's 0.299R + 0.587G + 0.114B, on the non-premultiplied colors the SW
/// backend stores.
float luminance(float r, float g, float b) { return 0.299f * r + 0.587f * g + 0.114f * b; }

/// §23.4's two coverage curves as 256-entry tables: `lighter` (gamma 1/1.2) applies when the text
/// is brighter than the pixel under it, `darker` (gamma 1.2) otherwise. Built once, on first use.
struct GammaTables {
    float lighter[256];
    float darker[256];

    GammaTables() {
        for (int i = 0; i < 256; ++i) {
            const float c = static_cast<float>(i) / 255.0f;
            lighter[i] = std::pow(c, 1.0f / 1.2f);
            darker[i] = std::pow(c, 1.2f);
        }
    }
};

const GammaTables& gammaTables() {
    static const GammaTables tables;
    return tables;
}

}  // namespace

float glyphBitmapDeviceSize(const GlyphsCmd& cmd, const Mat2x3& deviceXf) {
    // Only meaningful for an axis-aligned, uniformly scaled deviceXf (see glyphBitmapPathApplies),
    // where `a` alone is the device scale. The GPU atlas keys on the same value (T-263b E3).
    return glyphRunBitmapSize(cmd, deviceXf);
}

bool glyphBitmapPathApplies(const GlyphsCmd& cmd, const Mat2x3& deviceXf) {
    // §23.4: geometricPrecision means outlines, and no gamma; a rotated/skewed/mirrored/
    // non-uniformly-scaled run cannot be drawn by blitting axis-aligned bitmaps. The test is the
    // GPU atlas path's too (xt/command_list.h); the 24 px limit is this backend's own.
    return glyphRunBitmapApplies(cmd, deviceXf) && glyphBitmapDeviceSize(cmd, deviceXf) < 24.0f;
}

uint32_t drawGlyphBitmaps(TextShaper& shaper, const CommandList& list, const GlyphsCmd& cmd,
                          const Mat2x3& deviceXf, const Target& dst, const uint8_t* clipMask,
                          int clipStride) {
    if (cmd.font == kInvalidFont || cmd.glyphCount == 0 || cmd.color.a <= 0.0f) {
        return 0;
    }
    const float sizeDevice = glyphBitmapDeviceSize(cmd, deviceXf);
    if (sizeDevice <= 0.0f) {
        return 0;
    }
    const GammaTables& gamma = gammaTables();
    const float textLum = luminance(cmd.color.r, cmd.color.g, cmd.color.b);
    const int dstX0 = dst.originX;
    const int dstX1 = dst.originX + dst.w;
    const int dstY0 = dst.originY;
    const int dstY1 = dst.originY + dst.h;
    const bool dstPremul = dst.u8Premultiplied;

    uint32_t drawn = 0;
    GlyphBitmap bitmap;
    for (uint32_t i = 0; i < cmd.glyphCount; ++i) {
        const uint32_t index = cmd.glyphOffset + i;
        if (index >= list.glyphs.size() || index >= list.glyphPos.size()) {
            break;  // Defensive: a well-formed CommandList always has both pools in step.
        }
        const Vec2 device = apply(deviceXf, list.glyphPos[index]);
        // Split the device-space origin into a whole pixel and one of four 1/4 px phases; a phase
        // that rounds up to a whole pixel becomes phase 0 of the next pixel.
        float wholeX = std::floor(device.x);
        int phase = static_cast<int>(std::lround((device.x - wholeX) * 4.0f));
        if (phase >= 4) {
            phase = 0;
            wholeX += 1.0f;
        }
        if (!shaper.glyphBitmap(cmd.font, list.glyphs[index], sizeDevice, static_cast<float>(phase) * 0.25f,
                                bitmap)) {
            continue;
        }
        ++drawn;
        if (bitmap.width <= 0 || bitmap.height <= 0 || bitmap.alpha.empty()) {
            continue;  // A glyph with no pixels of its own (a space, say).
        }
        const int left = static_cast<int>(wholeX) + bitmap.bearingX;
        const int top = static_cast<int>(std::lround(device.y)) - bitmap.bearingY;

        for (int row = 0; row < bitmap.height; ++row) {
            const int y = top + row;
            if (y < dstY0 || y >= dstY1) {
                continue;
            }
            float* rowF = dst.rowF(y);
            uint8_t* rowU8 = rowF != nullptr ? nullptr : dst.rowU8(y);
            if (rowF == nullptr && rowU8 == nullptr) {
                continue;
            }
            const uint8_t* clipRow = clipMask != nullptr
                                         ? clipMask + static_cast<size_t>(y) * static_cast<size_t>(clipStride)
                                         : nullptr;
            const uint8_t* src =
                bitmap.alpha.data() + static_cast<size_t>(row) * static_cast<size_t>(bitmap.width);
            for (int col = 0; col < bitmap.width; ++col) {
                const uint8_t coverage = src[col];
                if (coverage == 0) {
                    continue;
                }
                const int x = left + col;
                if (x < dstX0 || x >= dstX1) {
                    continue;
                }
                uint8_t clip = 255;
                if (clipRow != nullptr) {
                    clip = clipRow[static_cast<size_t>(x)];
                }
                if (clip == 0) {
                    continue;
                }
                const size_t lx = static_cast<size_t>(x - dstX0);
                // The destination is read before blending: the gamma direction depends on it.
                // Float rows are premultiplied since T-102, so the luminance test un-premultiplies
                // first -- it is a comparison against the colour the viewer sees.
                Color d{};
                float* pf = nullptr;
                uint8_t* pu = nullptr;
                if (rowF != nullptr) {
                    pf = rowF + lx * 4;
                    d = Color{pf[0], pf[1], pf[2], pf[3]};
                } else {
                    pu = rowU8 + lx * 4;
                    d = Color{static_cast<float>(pu[0]) / 255.0f, static_cast<float>(pu[1]) / 255.0f,
                              static_cast<float>(pu[2]) / 255.0f, static_cast<float>(pu[3]) / 255.0f};
                    if (dstPremul) {
                        d = unpremultiply(d);
                    }
                }
                const Color plain = rowF != nullptr ? unpremultiply(d) : d;
                const float cov = textLum > luminance(plain.r, plain.g, plain.b) ? gamma.lighter[coverage]
                                                                                 : gamma.darker[coverage];
                const float a = cov * cmd.color.a * (static_cast<float>(clip) / 255.0f);
                if (a <= 0.0f) {
                    continue;
                }
                const Color tint{cmd.color.r * a, cmd.color.g * a, cmd.color.b * a, a};
                const Color out = blendSrcOverPremul(tint, rowF != nullptr ? d : premultiply(d));
                if (pf != nullptr) {
                    pf[0] = out.r;
                    pf[1] = out.g;
                    pf[2] = out.b;
                    pf[3] = out.a;
                } else {
                    const Color stored = dstPremul ? out : unpremultiply(out);
                    pu[0] = floatToByte(stored.r);
                    pu[1] = floatToByte(stored.g);
                    pu[2] = floatToByte(stored.b);
                    pu[3] = floatToByte(stored.a);
                }
            }
        }
    }
    return drawn;
}

namespace {

/// Source-over of `color` at coverage `a` (already times color.a and the clip) onto pixel `x` of
/// row `y`; the same premultiplied/straight handling as drawGlyphBitmaps above.
void blendCoverage(const Target& dst, int x, int y, const Color& color, float a) {
    float* rowF = dst.rowF(y);
    uint8_t* rowU8 = rowF != nullptr ? nullptr : dst.rowU8(y);
    if (rowF == nullptr && rowU8 == nullptr) {
        return;
    }
    const size_t lx = static_cast<size_t>(x - dst.originX);
    const Color tint{color.r * a, color.g * a, color.b * a, a};
    if (rowF != nullptr) {
        float* pf = rowF + lx * 4;
        const Color out = blendSrcOverPremul(tint, Color{pf[0], pf[1], pf[2], pf[3]});
        pf[0] = out.r;
        pf[1] = out.g;
        pf[2] = out.b;
        pf[3] = out.a;
        return;
    }
    uint8_t* pu = rowU8 + lx * 4;
    Color d{static_cast<float>(pu[0]) / 255.0f, static_cast<float>(pu[1]) / 255.0f,
            static_cast<float>(pu[2]) / 255.0f, static_cast<float>(pu[3]) / 255.0f};
    if (!dst.u8Premultiplied) {
        d = premultiply(d);
    }
    const Color out = blendSrcOverPremul(tint, d);
    const Color stored = dst.u8Premultiplied ? out : unpremultiply(out);
    pu[0] = floatToByte(stored.r);
    pu[1] = floatToByte(stored.g);
    pu[2] = floatToByte(stored.b);
    pu[3] = floatToByte(stored.a);
}

}  // namespace

float glyphEffectDeviceRadius(const GlyphsCmd& cmd, const Mat2x3& deviceXf) {
    return std::min(cmd.effectRadius * std::fabs(deviceXf.a), 8.0f);
}

uint32_t drawGlyphEffect(TextShaper& shaper, const CommandList& list, const GlyphsCmd& cmd,
                         const Mat2x3& deviceXf, const Target& dst, const uint8_t* clipMask, int clipStride) {
    if (cmd.font == kInvalidFont || cmd.glyphCount == 0 || cmd.color.a <= 0.0f) {
        return 0;
    }
    const float sizeDevice = glyphBitmapDeviceSize(cmd, deviceXf);
    if (sizeDevice <= 0.0f) {
        return 0;
    }
    // GlyphEffect's definitions (xt/command_list.h), per glyph, on the glyph's coverage bitmap --
    // the same bitmap, placement and formulas the GL atlas shader uses.
    const float radius = glyphEffectDeviceRadius(cmd, deviceXf);
    const bool dilate = cmd.effect == GlyphEffect::Dilate;
    const float sigma = radius * 0.5f;
    int k = 0;
    if (dilate) {
        k = radius > 0.0f ? static_cast<int>(std::ceil(radius)) : 0;
    } else if (sigma > 0.0f) {
        k = static_cast<int>(std::ceil(3.0f * sigma));
    }
    std::vector<float> weights;  // Blur: 1-D Gaussian, normalized.
    if (!dilate && k > 0) {
        weights.resize(static_cast<size_t>(2 * k + 1));
        float sum = 0;
        for (int i = -k; i <= k; ++i) {
            const float w = std::exp(-static_cast<float>(i * i) / (2.0f * sigma * sigma));
            weights[static_cast<size_t>(i + k)] = w;
            sum += w;
        }
        for (float& w : weights) {
            w /= sum;
        }
    }
    const int dstX0 = dst.originX;
    const int dstX1 = dst.originX + dst.w;
    const int dstY0 = dst.originY;
    const int dstY1 = dst.originY + dst.h;

    uint32_t drawn = 0;
    GlyphBitmap bitmap;
    std::vector<float> mask;
    std::vector<float> temp;
    for (uint32_t i = 0; i < cmd.glyphCount; ++i) {
        const uint32_t index = cmd.glyphOffset + i;
        if (index >= list.glyphs.size() || index >= list.glyphPos.size()) {
            break;
        }
        const Vec2 device = apply(deviceXf, list.glyphPos[index]);
        float wholeX = std::floor(device.x);
        int phase = static_cast<int>(std::lround((device.x - wholeX) * 4.0f));
        if (phase >= 4) {
            phase = 0;
            wholeX += 1.0f;
        }
        if (!shaper.glyphBitmap(cmd.font, list.glyphs[index], sizeDevice, static_cast<float>(phase) * 0.25f,
                                bitmap)) {
            continue;
        }
        ++drawn;
        if (bitmap.width <= 0 || bitmap.height <= 0 || bitmap.alpha.empty()) {
            continue;
        }
        const int bw = bitmap.width;
        const int bh = bitmap.height;
        const int mw = bw + 2 * k;
        const int mh = bh + 2 * k;
        auto cov = [&](int x, int y) -> float {  // bitmap coverage, 0 outside it
            if (x < 0 || y < 0 || x >= bw || y >= bh) {
                return 0.0f;
            }
            return static_cast<float>(bitmap.alpha[static_cast<size_t>(y) * static_cast<size_t>(bw) +
                                                   static_cast<size_t>(x)]) /
                   255.0f;
        };
        mask.assign(static_cast<size_t>(mw) * static_cast<size_t>(mh), 0.0f);
        if (k == 0) {
            for (int y = 0; y < bh; ++y) {
                for (int x = 0; x < bw; ++x) {
                    mask[static_cast<size_t>(y) * static_cast<size_t>(mw) + static_cast<size_t>(x)] =
                        cov(x, y);
                }
            }
        } else if (dilate) {
            for (int my = 0; my < mh; ++my) {
                for (int mx = 0; mx < mw; ++mx) {
                    float m = 0;
                    for (int dy = -k; dy <= k; ++dy) {
                        for (int dx = -k; dx <= k; ++dx) {
                            const float wd = std::clamp(
                                radius + 1.0f - std::sqrt(static_cast<float>(dx * dx + dy * dy)), 0.0f, 1.0f);
                            if (wd <= 0.0f) {
                                continue;
                            }
                            m = std::max(m, cov(mx - k + dx, my - k + dy) * wd);
                        }
                    }
                    mask[static_cast<size_t>(my) * static_cast<size_t>(mw) + static_cast<size_t>(mx)] = m;
                }
            }
        } else {
            // Separable Gaussian: rows of the padded bitmap, then columns.
            temp.assign(static_cast<size_t>(mw) * static_cast<size_t>(bh), 0.0f);
            for (int y = 0; y < bh; ++y) {
                for (int mx = 0; mx < mw; ++mx) {
                    float s = 0;
                    for (int d = -k; d <= k; ++d) {
                        s += weights[static_cast<size_t>(d + k)] * cov(mx - k + d, y);
                    }
                    temp[static_cast<size_t>(y) * static_cast<size_t>(mw) + static_cast<size_t>(mx)] = s;
                }
            }
            for (int my = 0; my < mh; ++my) {
                for (int mx = 0; mx < mw; ++mx) {
                    float s = 0;
                    for (int d = -k; d <= k; ++d) {
                        const int y = my - k + d;
                        if (y < 0 || y >= bh) {
                            continue;
                        }
                        s += weights[static_cast<size_t>(d + k)] *
                             temp[static_cast<size_t>(y) * static_cast<size_t>(mw) + static_cast<size_t>(mx)];
                    }
                    mask[static_cast<size_t>(my) * static_cast<size_t>(mw) + static_cast<size_t>(mx)] = s;
                }
            }
        }

        const int left = static_cast<int>(wholeX) + bitmap.bearingX - k;
        const int top = static_cast<int>(std::lround(device.y)) - bitmap.bearingY - k;
        for (int my = 0; my < mh; ++my) {
            const int y = top + my;
            if (y < dstY0 || y >= dstY1) {
                continue;
            }
            const uint8_t* clipRow = clipMask != nullptr
                                         ? clipMask + static_cast<size_t>(y) * static_cast<size_t>(clipStride)
                                         : nullptr;
            for (int mx = 0; mx < mw; ++mx) {
                const int x = left + mx;
                if (x < dstX0 || x >= dstX1) {
                    continue;
                }
                const float m = std::min(
                    mask[static_cast<size_t>(my) * static_cast<size_t>(mw) + static_cast<size_t>(mx)], 1.0f);
                if (m <= 0.0f) {
                    continue;
                }
                const float clip =
                    clipRow != nullptr ? static_cast<float>(clipRow[static_cast<size_t>(x)]) / 255.0f : 1.0f;
                const float a = m * cmd.color.a * clip;
                if (a <= 0.0f) {
                    continue;
                }
                blendCoverage(dst, x, y, cmd.color, a);
            }
        }
    }
    return drawn;
}

}  // namespace xt::sw

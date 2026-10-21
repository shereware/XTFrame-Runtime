#include "sw_effects.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "sw_blend.h"
#include "sw_effect_math.h"
#include "sw_simd.h"
#include "xt/assert.h"
#include "xt/perf.h"

/// See docs/design/18-effects.md §18.3, §18.6, docs/tasks/T-53b.md.

namespace xt::sw {

namespace {

constexpr float kDegToRad = 0.017453292519943295f;
constexpr float kMaxDevicePx = 64.0f;

/// docs/design/18-effects.md §18.3: "半徑類...換算到裝置 px 後上限 64...超過夾住並警告一次". One
/// warning for the whole process lifetime, same pattern as sw_renderer.cpp's PushEffect stub
/// warning.
float clampDevicePx64(float v) {
    if (v > kMaxDevicePx) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                         "xt::sw effects: a radius-like effect parameter exceeded the 64 device px "
                         "cap; clamped (docs/design/18-effects.md \xc2\xa7"
                         "18.3).\n");
        }
        return kMaxDevicePx;
    }
    return v;
}

/// `glsl`-style smoothstep, guarded against `edge1 <= edge0` (Shine `softness = 0` makes the two
/// edges coincide): degenerates to a hard step at `edge0` rather than dividing by zero.
float smoothstepGuarded(float edge0, float edge1, float x) {
    if (edge1 <= edge0) {
        return x < edge0 ? 0.0f : 1.0f;
    }
    float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// Samples a GradientStop list's alpha channel only (GradientMask only uses stop alpha, 18.3).
/// Same bracketing rule as sw_paint.cpp's sampleStops: before/after the stop range clamps to the
/// nearest end stop; an empty list conservatively returns 0 (fully masked).
float sampleStopAlpha(std::span<const GradientStop> stops, float t) {
    if (stops.empty()) {
        return 0.0f;
    }
    if (stops.size() == 1 || t <= stops.front().offset) {
        return stops.front().color.a;
    }
    if (t >= stops.back().offset) {
        return stops.back().color.a;
    }
    for (size_t i = 0; i + 1 < stops.size(); ++i) {
        const GradientStop& a = stops[i];
        const GradientStop& b = stops[i + 1];
        if (t >= a.offset && t <= b.offset) {
            float span = b.offset - a.offset;
            float f = span > 0.0f ? (t - a.offset) / span : 0.0f;
            return a.color.a + (b.color.a - a.color.a) * f;
        }
    }
    return stops.back().color.a;
}

/// Bilinear sample of `s` at pixel coordinates `(x, y)` (pixel `(0,0)`'s center is `(0.5, 0.5)`,
/// same convention `Target::get`'s integer callers use). Samples outside `[0,w) x [0,h)` are
/// treated as fully transparent black (not clamp-to-edge): DropShadow's offset is a translation
/// of a defined-support alpha field, so a shifted-out shadow should not smear the buffer edge.
Color sampleBilinearZero(const Surface& s, float x, float y) {
    float fx = x - 0.5f;
    float fy = y - 0.5f;
    int x0 = static_cast<int>(std::floor(fx));
    int y0 = static_cast<int>(std::floor(fy));
    float tx = fx - static_cast<float>(x0);
    float ty = fy - static_cast<float>(y0);
    auto at = [&](int xx, int yy) -> Color {
        if (xx < 0 || yy < 0 || xx >= s.w || yy >= s.h) {
            return Color{0, 0, 0, 0};
        }
        return s.get(xx, yy);
    };
    Color c00 = at(x0, y0);
    Color c10 = at(x0 + 1, y0);
    Color c01 = at(x0, y0 + 1);
    Color c11 = at(x0 + 1, y0 + 1);
    auto lerp4 = [](const Color& a, const Color& b, float f) {
        return Color{
            a.r + (b.r - a.r) * f,
            a.g + (b.g - a.g) * f,
            a.b + (b.b - a.b) * f,
            a.a + (b.a - a.a) * f,
        };
    };
    Color top = lerp4(c00, c10, tx);
    Color bottom = lerp4(c01, c11, tx);
    return lerp4(top, bottom, ty);
}

/// Resizes `s` to `like`'s dimensions. The contents are left undefined: every caller overwrites
/// each pixel before reading it back, and zero-filling a full-size float buffer that is about to
/// be overwritten was a measurable share of the effect cost (docs/tasks/T-82.md). Passes that
/// write only some channels (packAlpha) keep their own assign().
void resizeLike(Surface& s, const Surface& like) {
    s.w = like.w;
    s.h = like.h;
    s.rgba.resize(static_cast<size_t>(like.w) * static_cast<size_t>(like.h) * 4);
}

/// Copies `s`'s alpha channel into `out` (row-major, `s.w * s.h`), which is only ever grown
/// (docs/tasks/T-102b.md: the effect scratch buffers are owned by the caller and reused).
void alphaChannel(const Surface& s, std::vector<float>& out) {
    const size_t n = static_cast<size_t>(s.w) * static_cast<size_t>(s.h);
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = s.rgba[i * 4 + 3];
    }
}

/// Writes one solid effect colour (DropShadow/Glow/Outline read theirs non-premultiplied from the
/// command list) scaled to `alpha` and premultiplied at `p`, ready to composite onto a Surface.
void writeTinted(float* p, const Color& color, float alpha) {
    p[0] = color.r * alpha;
    p[1] = color.g * alpha;
    p[2] = color.b * alpha;
    p[3] = alpha;
}

/// Bilinear sample of a scalar field (`w x h`, row-major) at pixel coordinates `(x, y)`, same
/// half-pixel convention as sampleBilinearZero(); samples outside the field read `outside`.
float sampleScalarBilinear(const std::vector<float>& f, int w, int h, float x, float y, float outside) {
    float fx = x - 0.5f;
    float fy = y - 0.5f;
    int x0 = static_cast<int>(std::floor(fx));
    int y0 = static_cast<int>(std::floor(fy));
    float tx = fx - static_cast<float>(x0);
    float ty = fy - static_cast<float>(y0);
    auto at = [&](int xx, int yy) -> float {
        if (xx < 0 || yy < 0 || xx >= w || yy >= h) {
            return outside;
        }
        return f[static_cast<size_t>(yy) * static_cast<size_t>(w) + static_cast<size_t>(xx)];
    };
    float top = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * tx;
    float bottom = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
    return top + (bottom - top) * ty;
}

/// DropShadow with `inset` (docs/design/18-effects.md §18.3/§18.6, docs/tasks/T-111.md):
/// `h = gauss(dilate(1 - alpha(src), spread), sigma)`, shifted by `(offsetX, offsetY)` and
/// multiplied by `alpha(src)`; the shadow layer is composited *on top* of the content, so it only
/// darkens the shape's inside.
///
/// The effect buffer holds a layer that is transparent outside its own bounds, so the inverted
/// alpha field is 1 there. That matters here (unlike the outer shadow) because an inset shadow
/// expands nothing (18.3) and the shape therefore usually touches the buffer edge: an edge-clamped
/// blur would read 0 past the edge and drop the shadow along the whole side. The field is built on
/// a buffer padded with 1s, wide enough that the dilate and the blur never reach past the padding,
/// which makes the existing edge-clamped passes behave exactly like an infinite transparent
/// surround. `gl/src/gl_effects.cpp`'s inset path uses the same convention (its passes read the
/// out-of-texture value directly).
void applyDropShadowInset(const EffectInstance& p, const Surface& src, Surface& dst, EffectScratch& scratch,
                          const EffectTimers& timers) {
    int w = src.w;
    int h = src.h;
    float blur = clampDevicePx64(std::max(0.0f, p.params[2]));
    float spread = clampDevicePx64(std::max(0.0f, p.params[3]));
    float sigma = blur * 0.5f;
    int spreadR = static_cast<int>(std::lround(spread));
    int pad = spreadR + static_cast<int>(std::ceil(3.0f * sigma)) + 2;
    int pw = w + 2 * pad;
    int ph = h + 2 * pad;

    // The 1-filled padding is part of the algorithm (see above), so this buffer is filled whole
    // rather than only resized; only its capacity is reused across frames.
    std::vector<float>& inv = scratch.alpha;
    inv.assign(static_cast<size_t>(pw) * static_cast<size_t>(ph), 1.0f);
    for (int y = 0; y < h; ++y) {
        const float* srcRow = src.row(y);
        float* invRow =
            inv.data() + static_cast<size_t>(y + pad) * static_cast<size_t>(pw) + static_cast<size_t>(pad);
        for (int x = 0; x < w; ++x) {
            invRow[x] = 1.0f - srcRow[static_cast<size_t>(x) * 4 + 3];
        }
    }

    {
        PerfFieldScope dilateScope(timers.dilate);
        fx::dilateAlpha(inv, scratch.dilated, scratch.fieldA, pw, ph, spreadR);
    }
    {
        PerfFieldScope blurScope(timers.blur);
        fx::gaussianBlurAlphaField(scratch.dilated, pw, ph, scratch.blurred, scratch.fieldB, sigma);
    }

    PerfFieldScope pixScope(timers.pix);
    const std::vector<float>& blurred = scratch.blurred;
    Color color = p.colors[0];
    float opacity = p.params[4];
    float offsetX = p.params[0];
    float offsetY = p.params[1];
    resizeLike(dst, src);
    scratch.row.resize(static_cast<size_t>(w) * 4);
    const SwSimdOps& ops = swSimd();
    for (int y = 0; y < h; ++y) {
        const float* srcRow = src.row(y);
        float* dstRow = dst.row(y);
        float* shadowRow = scratch.row.data();
        float sy = static_cast<float>(y + pad) + 0.5f - offsetY;
        for (int x = 0; x < w; ++x) {
            float sx = static_cast<float>(x + pad) + 0.5f - offsetX;
            float hVal = sampleScalarBilinear(blurred, pw, ph, sx, sy, /*outside=*/1.0f);
            float* q = shadowRow + static_cast<size_t>(x) * 4;
            q[0] = color.r;
            q[1] = color.g;
            q[2] = color.b;
            q[3] = hVal * srcRow[static_cast<size_t>(x) * 4 + 3] * color.a * opacity;
        }
        // The shadow goes on top of the content, so `dst` starts as a copy of `src` and the span
        // kernel blends the shadow row over it (docs/tasks/T-102b.md step 2).
        std::memcpy(dstRow, srcRow, static_cast<size_t>(w) * 4 * sizeof(float));
        ops.spanSrcOverF(dstRow, shadowRow, w);
    }
}

void applyDropShadow(const EffectInstance& p, const Surface& src, Surface& shadowBuf, Surface& dst,
                     EffectScratch& scratch, const EffectTimers& timers) {
    int w = src.w;
    int h = src.h;
    float blur = clampDevicePx64(std::max(0.0f, p.params[2]));
    float spread = clampDevicePx64(std::max(0.0f, p.params[3]));

    alphaChannel(src, scratch.alpha);
    int spreadR = static_cast<int>(std::lround(spread));
    {
        PerfFieldScope dilateScope(timers.dilate);
        fx::dilateAlpha(scratch.alpha, scratch.dilated, scratch.fieldA, w, h, spreadR);
    }
    {
        PerfFieldScope blurScope(timers.blur);
        fx::gaussianBlurAlphaField(scratch.dilated, w, h, scratch.blurred, scratch.fieldB, blur * 0.5f);
    }

    PerfFieldScope pixScope(timers.pix);
    const std::vector<float>& blurred = scratch.blurred;
    Color color = p.colors[0];
    float opacity = p.params[4];
    resizeLike(shadowBuf, src);
    for (int y = 0; y < h; ++y) {
        const float* b = blurred.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
        float* shadowRow = shadowBuf.row(y);
        for (int x = 0; x < w; ++x) {
            writeTinted(shadowRow + static_cast<size_t>(x) * 4, color, b[x] * color.a * opacity);
        }
    }

    resizeLike(dst, src);
    float offsetX = p.params[0];
    float offsetY = p.params[1];
    const SwSimdOps& ops = swSimd();
    // The shadow is sampled at a pure translation of the pixel grid, so whether a pixel's 2x2
    // bilinear footprint lies fully inside the shadow buffer depends only on its row and on a
    // single column range. Those interior pixels take an unchecked path; the outer border keeps
    // `sampleBilinearZero` (which reads transparent black past the edge). `fx`/`fy` are still
    // computed exactly the way `sampleBilinearZero` computes them, so both paths agree bit for
    // bit (docs/tasks/T-102b.md step 2).
    auto fxAt = [&](int x) { return (static_cast<float>(x) + 0.5f - offsetX) - 0.5f; };
    auto fyAt = [&](int y) { return (static_cast<float>(y) + 0.5f - offsetY) - 0.5f; };
    int xLo = 0;
    while (xLo < w && static_cast<int>(std::floor(fxAt(xLo))) < 0) {
        ++xLo;
    }
    int xHi = w;
    while (xHi > xLo && static_cast<int>(std::floor(fxAt(xHi - 1))) + 1 >= w) {
        --xHi;
    }
    // Whole-pixel offsets (the common case: an unscaled `shadow(0 10px ...)`) make every
    // bilinear weight 0 or 1, so the shifted shadow row is a byte-for-byte copy of a shadow-buffer
    // row with transparent black past its ends (docs/tasks/T-102b.md step 2).
    if (offsetX == std::floor(offsetX) && offsetY == std::floor(offsetY)) {
        int ox = static_cast<int>(offsetX);
        int oy = static_cast<int>(offsetY);
        int lo = std::clamp(ox, 0, w);
        int hi = std::clamp(w + ox, lo, w);
        const size_t rowBytes = static_cast<size_t>(w) * 4 * sizeof(float);
        for (int y = 0; y < h; ++y) {
            const float* srcRow = src.row(y);
            float* dstRow = dst.row(y);
            int y0 = y - oy;
            if (y0 < 0 || y0 >= h) {
                std::memset(dstRow, 0, rowBytes);
            } else {
                const float* shadowRow = shadowBuf.row(y0);
                if (lo > 0) {
                    std::memset(dstRow, 0, static_cast<size_t>(lo) * 4 * sizeof(float));
                }
                if (hi > lo) {
                    std::memcpy(dstRow + static_cast<size_t>(lo) * 4,
                                shadowRow + static_cast<size_t>(lo - ox) * 4,
                                static_cast<size_t>(hi - lo) * 4 * sizeof(float));
                }
                if (hi < w) {
                    std::memset(dstRow + static_cast<size_t>(hi) * 4, 0,
                                static_cast<size_t>(w - hi) * 4 * sizeof(float));
                }
            }
            ops.spanSrcOverF(dstRow, srcRow, w);
        }
        return;
    }

    auto safeSample = [&](float* q, int x, int y) {
        Color c = sampleBilinearZero(shadowBuf, static_cast<float>(x) + 0.5f - offsetX,
                                     static_cast<float>(y) + 0.5f - offsetY);
        q[0] = c.r;
        q[1] = c.g;
        q[2] = c.b;
        q[3] = c.a;
    };
    for (int y = 0; y < h; ++y) {
        const float* srcRow = src.row(y);
        float* dstRow = dst.row(y);
        float fy = fyAt(y);
        int y0 = static_cast<int>(std::floor(fy));
        float ty = fy - static_cast<float>(y0);
        bool rowInside = y0 >= 0 && y0 + 1 < h;
        int fastLo = rowInside ? xLo : w;
        int fastHi = rowInside ? xHi : w;
        for (int x = 0; x < fastLo; ++x) {
            safeSample(dstRow + static_cast<size_t>(x) * 4, x, y);
        }
        if (fastLo < fastHi) {
            const float* r0 = shadowBuf.row(y0);
            const float* r1 = shadowBuf.row(y0 + 1);
            for (int x = fastLo; x < fastHi; ++x) {
                float fx = fxAt(x);
                int x0 = static_cast<int>(std::floor(fx));
                float tx = fx - static_cast<float>(x0);
                const float* a0 = r0 + static_cast<size_t>(x0) * 4;
                const float* a1 = r1 + static_cast<size_t>(x0) * 4;
                float* q = dstRow + static_cast<size_t>(x) * 4;
                for (int k = 0; k < 4; ++k) {
                    float top = a0[k] + (a0[k + 4] - a0[k]) * tx;
                    float bottom = a1[k] + (a1[k + 4] - a1[k]) * tx;
                    q[k] = top + (bottom - top) * ty;
                }
            }
        }
        for (int x = fastHi; x < w; ++x) {
            safeSample(dstRow + static_cast<size_t>(x) * 4, x, y);
        }
        // The content goes on top of the shifted shadow row just written into `dst`.
        ops.spanSrcOverF(dstRow, srcRow, w);
    }
}

void applyGlow(const EffectInstance& p, const Surface& src, Surface& dst, EffectScratch& scratch,
               const EffectTimers& timers) {
    int w = src.w;
    int h = src.h;
    float radius = clampDevicePx64(std::max(0.0f, p.params[0]));
    float intensity = p.params[1];
    bool inner = p.params[2] != 0.0f;
    Color color = p.colors[0];
    float sigma = radius * 0.5f;
    const SwSimdOps& ops = swSimd();

    resizeLike(dst, src);
    if (!inner) {
        {
            PerfFieldScope blurScope(timers.blur);
            fx::gaussianBlurAlpha(src, scratch.blurred, scratch.fieldB, sigma);
        }
        PerfFieldScope pixScope(timers.pix);
        const std::vector<float>& blurred = scratch.blurred;
        for (int y = 0; y < h; ++y) {
            const float* b = blurred.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
            const float* srcRow = src.row(y);
            float* dstRow = dst.row(y);
            for (int x = 0; x < w; ++x) {
                float g = std::clamp(b[x] * intensity, 0.0f, 1.0f);
                writeTinted(dstRow + static_cast<size_t>(x) * 4, color, g * color.a);
            }
            // The outer glow lies under the content.
            ops.spanSrcOverF(dstRow, srcRow, w);
        }
    } else {
        const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
        alphaChannel(src, scratch.alpha);
        std::vector<float>& inv = scratch.fieldA;
        inv.resize(n);
        for (size_t i = 0; i < n; ++i) {
            inv[i] = 1.0f - scratch.alpha[i];
        }
        {
            PerfFieldScope blurScope(timers.blur);
            fx::gaussianBlurAlphaField(inv, w, h, scratch.blurred, scratch.fieldB, sigma);
        }
        PerfFieldScope pixScope(timers.pix);
        const std::vector<float>& blurred = scratch.blurred;
        scratch.row.resize(static_cast<size_t>(w) * 4);
        for (int y = 0; y < h; ++y) {
            size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(w);
            const float* b = blurred.data() + rowBase;
            const float* srcAlpha = scratch.alpha.data() + rowBase;
            const float* srcRow = src.row(y);
            float* dstRow = dst.row(y);
            float* glowRow = scratch.row.data();
            for (int x = 0; x < w; ++x) {
                float hVal = std::clamp(b[x] * intensity, 0.0f, 1.0f) * srcAlpha[x];
                writeTinted(glowRow + static_cast<size_t>(x) * 4, color, hVal * color.a);
            }
            // The inner glow lies on top of the content.
            std::memcpy(dstRow, srcRow, static_cast<size_t>(w) * 4 * sizeof(float));
            ops.spanSrcOverF(dstRow, glowRow, w);
        }
    }
}

void applyBlur(const EffectInstance& p, const Surface& src, Surface& scratchA, Surface& dst,
               const EffectTimers& timers) {
    PerfFieldScope blurScope(timers.blur);
    float radiusX = clampDevicePx64(std::max(0.0f, p.params[0]));
    float radiusY = clampDevicePx64(std::max(0.0f, p.params[1]));
    fx::gaussianBlur(src, dst, scratchA, radiusX * 0.5f, radiusY * 0.5f, /*premultiply=*/true);
}

void applyColorAdjust(const EffectInstance& p, const Surface& src, Surface& dst, const EffectTimers& timers) {
    PerfFieldScope pixScope(timers.pix);
    dst = src;
    fx::ColorMatrix m =
        fx::colorAdjustMatrix(p.params[0], p.params[1], p.params[2], p.params[3], p.params[5], p.params[6]);
    fx::applyColorMatrix(dst, m, p.params[4]);
}

void applyGradientMask(const EffectInstance& p, std::span<const GradientStop> stopsPool,
                       const Mat2x3& invScreenXf, Vec2 bufferOriginPx, const Surface& src, Surface& dst,
                       const EffectTimers& timers) {
    PerfFieldScope pixScope(timers.pix);
    resizeLike(dst, src);
    std::span<const GradientStop> stops = stopsPool.subspan(p.stopOffset, p.stopCount);
    bool radial = p.params[4] != 0.0f;
    bool invertMask = p.params[5] != 0.0f;

    Vec2 p0Screen{p.params[0], p.params[1]};
    Vec2 p1Screen{p.params[2], p.params[3]};
    Vec2 p0 = apply(invScreenXf, p0Screen);
    Vec2 p1 = apply(invScreenXf, p1Screen);
    Vec2 d = p1 - p0;
    float lenSq = d.x * d.x + d.y * d.y;

    for (int y = 0; y < src.h; ++y) {
        const float* srcRow = src.row(y);
        float* dstRow = dst.row(y);
        for (int x = 0; x < src.w; ++x) {
            Vec2 screenPixel{static_cast<float>(x) + bufferOriginPx.x + 0.5f,
                             static_cast<float>(y) + bufferOriginPx.y + 0.5f};
            Vec2 q = apply(invScreenXf, screenPixel);
            Vec2 v = q - p0;
            float t;
            if (lenSq <= 0.0f) {
                t = 0.0f;
            } else if (radial) {
                t = std::sqrt(v.x * v.x + v.y * v.y) / std::sqrt(lenSq);
            } else {
                t = (v.x * d.x + v.y * d.y) / lenSq;
            }
            t = std::clamp(t, 0.0f, 1.0f);
            float m = sampleStopAlpha(stops, t);
            if (invertMask) {
                m = 1.0f - m;
            }
            // Premultiplied: scaling every channel by the mask coefficient is what scaling alpha
            // alone did before T-102.
            const float* sp = srcRow + static_cast<size_t>(x) * 4;
            float* dp = dstRow + static_cast<size_t>(x) * 4;
            dp[0] = sp[0] * m;
            dp[1] = sp[1] * m;
            dp[2] = sp[2] * m;
            dp[3] = sp[3] * m;
        }
    }
}

void applyShine(const EffectInstance& p, Vec2 bufferOriginPx, const Rect& contentBoundsPx, const Surface& src,
                Surface& dst, const EffectTimers& timers) {
    PerfFieldScope pixScope(timers.pix);
    resizeLike(dst, src);
    float position = p.params[0];
    float width = std::max(0.0f, p.params[1]);
    float softness = std::clamp(p.params[2], 0.0f, 1.0f);
    float angleDeg = p.params[3];
    float intensity = p.params[4];
    Color color = p.colors[0];

    float rad = angleDeg * kDegToRad;
    Vec2 d{std::cos(rad), std::sin(rad)};
    Vec2 corners[4] = {
        Vec2{contentBoundsPx.x, contentBoundsPx.y},
        Vec2{contentBoundsPx.x + contentBoundsPx.w, contentBoundsPx.y},
        Vec2{contentBoundsPx.x, contentBoundsPx.y + contentBoundsPx.h},
        Vec2{contentBoundsPx.x + contentBoundsPx.w, contentBoundsPx.y + contentBoundsPx.h},
    };
    float lo = corners[0].x * d.x + corners[0].y * d.y;
    float hi = lo;
    for (int i = 1; i < 4; ++i) {
        float proj = corners[i].x * d.x + corners[i].y * d.y;
        lo = std::min(lo, proj);
        hi = std::max(hi, proj);
    }
    float c = lo + (hi - lo) * position;
    float edge0 = width * (1.0f - softness) * 0.5f;
    float edge1 = width * 0.5f;

    for (int y = 0; y < src.h; ++y) {
        const float* srcRow = src.row(y);
        float* dstRow = dst.row(y);
        float sy = static_cast<float>(y) + bufferOriginPx.y + 0.5f;
        for (int x = 0; x < src.w; ++x) {
            float sx = static_cast<float>(x) + bufferOriginPx.x + 0.5f;
            float u = sx * d.x + sy * d.y;
            float band = 1.0f - smoothstepGuarded(edge0, edge1, std::fabs(u - c));
            // The additive band and its clamp are defined on the colour the viewer sees, so the
            // premultiplied sample is divided out around them (docs/tasks/T-102.md step 2).
            const float* sp = srcRow + static_cast<size_t>(x) * 4;
            Color s = unpremultiply(Color{sp[0], sp[1], sp[2], sp[3]});
            Color out{
                std::clamp(s.r + color.r * band * intensity * s.a, 0.0f, 1.0f),
                std::clamp(s.g + color.g * band * intensity * s.a, 0.0f, 1.0f),
                std::clamp(s.b + color.b * band * intensity * s.a, 0.0f, 1.0f),
                s.a,
            };
            Color pm = premultiply(out);
            float* dp = dstRow + static_cast<size_t>(x) * 4;
            dp[0] = pm.r;
            dp[1] = pm.g;
            dp[2] = pm.b;
            dp[3] = pm.a;
        }
    }
}

void applyOutline(const EffectInstance& p, const Surface& src, Surface& dst, EffectScratch& scratch,
                  const EffectTimers& timers) {
    int w = src.w;
    int h = src.h;
    float radius = clampDevicePx64(std::max(0.0f, p.params[0]));
    float softness = clampDevicePx64(std::max(0.0f, p.params[1]));
    // position: 0 outer, 1 center, 2 inner (docs/design/18-effects.md §18.3).
    float position = p.params[2];
    Color color = p.colors[0];

    alphaChannel(src, scratch.alpha);
    {
        PerfFieldScope sdfScope(timers.sdf);
        fx::signedDistance(scratch.alpha, scratch.blurred, w, h);
    }

    PerfFieldScope pixScope(timers.pix);
    const std::vector<float>& sdf = scratch.blurred;
    resizeLike(dst, src);
    const SwSimdOps& ops = swSimd();
    // `position` is constant over the pass, so the three-way choice (and with it whether the line
    // goes over or under the content) is hoisted out of the pixel loop.
    const int mode = position < 0.5f ? 0 : (position < 1.5f ? 1 : 2);
    const bool lineOnTop = mode == 2;
    if (lineOnTop) {
        scratch.row.resize(static_cast<size_t>(w) * 4);
    }
    for (int y = 0; y < h; ++y) {
        const float* sd = sdf.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
        const float* srcRow = src.row(y);
        float* dstRow = dst.row(y);
        float* lineRow = lineOnTop ? scratch.row.data() : dstRow;
        for (int x = 0; x < w; ++x) {
            float sdfV = sd[x];
            float m;
            if (mode == 0) {  // outer
                m = sdfV > 0.0f ? 1.0f - smoothstepGuarded(radius, radius + softness, sdfV) : 0.0f;
            } else if (mode == 1) {  // center
                m = 1.0f - smoothstepGuarded(radius * 0.5f, radius * 0.5f + softness, std::fabs(sdfV));
            } else {  // inner
                m = sdfV <= 0.0f ? 1.0f - smoothstepGuarded(radius, radius + softness, -sdfV) : 0.0f;
            }
            writeTinted(lineRow + static_cast<size_t>(x) * 4, color, m * color.a);
        }
        if (lineOnTop) {
            std::memcpy(dstRow, srcRow, static_cast<size_t>(w) * 4 * sizeof(float));
            ops.spanSrcOverF(dstRow, lineRow, w);
        } else {
            ops.spanSrcOverF(dstRow, srcRow, w);
        }
    }
}

/// The `EffectScratch` the overloads without one use: per thread, so two renderers on different
/// threads never share buffers, and never freed (the same lifetime the old per-call vectors had,
/// minus the allocation). `SwRenderer` passes its own `Impl`-owned scratch instead.
/// T-343: bilinear, edge-clamped sample of `s` at `(px, py)` in texel units (texel centers at
/// integer + 0.5), the rule gpu_frame.h's `LevelMix` spells out.
Color sampleLevel(const Surface& s, float px, float py) {
    const float fx0 = px - 0.5f;
    const float fy0 = py - 0.5f;
    const int ix = static_cast<int>(std::floor(fx0));
    const int iy = static_cast<int>(std::floor(fy0));
    const float tx = fx0 - static_cast<float>(ix);
    const float ty = fy0 - static_cast<float>(iy);
    const int x0 = std::clamp(ix, 0, s.w - 1), x1 = std::clamp(ix + 1, 0, s.w - 1);
    const int y0 = std::clamp(iy, 0, s.h - 1), y1 = std::clamp(iy + 1, 0, s.h - 1);
    const float* r0 = s.row(y0);
    const float* r1 = s.row(y1);
    const float w00 = (1.0f - tx) * (1.0f - ty), w10 = tx * (1.0f - ty), w01 = (1.0f - tx) * ty,
                w11 = tx * ty;
    Color c;
    const float* a = r0 + static_cast<size_t>(x0) * 4;
    const float* b = r0 + static_cast<size_t>(x1) * 4;
    const float* cc = r1 + static_cast<size_t>(x0) * 4;
    const float* d = r1 + static_cast<size_t>(x1) * 4;
    c.r = a[0] * w00 + b[0] * w10 + cc[0] * w01 + d[0] * w11;
    c.g = a[1] * w00 + b[1] * w10 + cc[1] * w01 + d[1] * w11;
    c.b = a[2] * w00 + b[2] * w10 + cc[2] * w01 + d[2] * w11;
    c.a = a[3] * w00 + b[3] * w10 + cc[3] * w01 + d[3] * w11;
    return c;
}

/// T-343: the Kawase pass (gpu_frame.h `KawaseParams`, offset 1.5 texels): each diagonal tap is
/// the mean of the 2 x 2 block whose near corner is 1 texel away.
void kawasePass(const Surface& src, Surface& dst) {
    dst.w = src.w;
    dst.h = src.h;
    dst.rgba.resize(static_cast<size_t>(src.w) * static_cast<size_t>(src.h) * 4);
    auto texel = [&](int x, int y) {
        x = std::clamp(x, 0, src.w - 1);
        y = std::clamp(y, 0, src.h - 1);
        return src.row(y) + static_cast<size_t>(x) * 4;
    };
    for (int y = 0; y < src.h; ++y) {
        float* out = dst.row(y);
        for (int x = 0; x < src.w; ++x) {
            const float* c = texel(x, y);
            float acc[4] = {c[0] * 4.0f, c[1] * 4.0f, c[2] * 4.0f, c[3] * 4.0f};
            const int xs[2] = {x + 1, x - 2};
            const int ys[2] = {y + 1, y - 2};
            for (int bx : xs) {
                for (int by : ys) {
                    const float* t00 = texel(bx, by);
                    const float* t10 = texel(bx + 1, by);
                    const float* t01 = texel(bx, by + 1);
                    const float* t11 = texel(bx + 1, by + 1);
                    for (int ch = 0; ch < 4; ++ch) {
                        acc[ch] += (t00[ch] + t10[ch] + t01[ch] + t11[ch]) * 0.25f;
                    }
                }
            }
            float* o = out + static_cast<size_t>(x) * 4;
            for (int ch = 0; ch < 4; ++ch) {
                o[ch] = acc[ch] * 0.125f;
            }
        }
    }
}

/// BackdropBlur (docs/design/18-effects.md §18.3 kind 8, §18.6; T-343 §18.15). See sw_effects.h.
void applyBackdropBlurImpl(const EffectInstance& p, const BackdropPyramid& pyr, Vec2 bufferOriginPx,
                           const Surface& src, Surface& dst, EffectScratch& scratch,
                           const EffectTimers& timers) {
    resizeLike(dst, src);
    const Surface& base = pyr.level[0];
    {
        PerfFieldScope blurScope(timers.blur);
        float radius = clampDevicePx64(std::max(0.0f, p.params[0]));
        float sigma = radius * 0.5f;
        int lower = 0, upper = 0;
        float t = 0.0f;
        backdropPyramidMix(sigma, pyr.levels, lower, upper, t);
        const Surface& la = pyr.level[lower];
        const Surface& lb = pyr.level[upper];
        const bool two = upper != lower && t > 0.0f;
        // The blurred backdrop over the content buffer: every content pixel's device position
        // mapped into the region's normalised space, sampled in each level (gpu_frame.h LevelMix).
        Surface& out = scratch.backdrop;
        out.w = src.w;
        out.h = src.h;
        out.rgba.assign(static_cast<size_t>(src.w) * static_cast<size_t>(src.h) * 4, 0.0f);
        const float rw = static_cast<float>(std::max(1, base.w));
        const float rh = static_cast<float>(std::max(1, base.h));
        for (int y = 0; y < src.h; ++y) {
            const float v =
                (bufferOriginPx.y + static_cast<float>(y) + 0.5f - static_cast<float>(pyr.y0)) / rh;
            float* row = out.row(y);
            for (int x = 0; x < src.w; ++x) {
                const float u =
                    (bufferOriginPx.x + static_cast<float>(x) + 0.5f - static_cast<float>(pyr.x0)) / rw;
                Color c = sampleLevel(la, u * static_cast<float>(la.w), v * static_cast<float>(la.h));
                if (two) {
                    Color c2 = sampleLevel(lb, u * static_cast<float>(lb.w), v * static_cast<float>(lb.h));
                    c.r += (c2.r - c.r) * t;
                    c.g += (c2.g - c.g) * t;
                    c.b += (c2.b - c.b) * t;
                    c.a += (c2.a - c.a) * t;
                }
                float* o = row + static_cast<size_t>(x) * 4;
                o[0] = c.r;
                o[1] = c.g;
                o[2] = c.b;
                o[3] = c.a;
            }
        }
    }
    PerfFieldScope pixScope(timers.pix);
    float saturation = p.params[1];
    float brightness = p.params[2];
    if (saturation != 0.0f || brightness != 0.0f) {
        // 18.3's BackdropBlur row reuses ColorAdjust's own Saturation/Brightness formulas; the
        // other three ColorAdjust parameters are identities here (contrast 0, hue 0, opacity 1).
        fx::ColorMatrix m = fx::colorAdjustMatrix(brightness, 0.0f, saturation, 0.0f);
        fx::applyColorMatrix(scratch.backdrop, m, 1.0f);
    }

    const Surface& blurred = scratch.backdrop;
    for (int y = 0; y < src.h; ++y) {
        const float* srcRow = src.row(y);
        float* dstRow = dst.row(y);
        const float* bdRow = blurred.row(y);
        for (int x = 0; x < src.w; ++x) {
            const float* sp = srcRow + static_cast<size_t>(x) * 4;
            float srcA = sp[3];
            // The blurred backdrop, masked by the content's own alpha, lies *under* the content.
            const float* bp = bdRow + static_cast<size_t>(x) * 4;
            Color bottom{bp[0] * srcA, bp[1] * srcA, bp[2] * srcA, bp[3] * srcA};
            float inv = 1.0f - srcA;
            float* dp = dstRow + static_cast<size_t>(x) * 4;
            dp[0] = sp[0] + bottom.r * inv;
            dp[1] = sp[1] + bottom.g * inv;
            dp[2] = sp[2] + bottom.b * inv;
            dp[3] = srcA + bottom.a * inv;
        }
    }
}

EffectScratch& fallbackScratch() {
    static thread_local EffectScratch scratch;
    return scratch;
}

}  // namespace

void applyEffect(const EffectInstance& screenParams, std::span<const GradientStop> stops,
                 const Mat2x3& invScreenXf, Vec2 bufferOriginPx, const Rect& contentBoundsPx,
                 const Surface& src, Surface& scratchA, Surface& scratchB, Surface& dst,
                 EffectScratch& scratch, const EffectTimers& timers) {
    if (screenParams.enabled == 0 || screenParams.kind == EffectKind::None) {
        dst = src;
        return;
    }
    switch (screenParams.kind) {
        case EffectKind::DropShadow:
            if (screenParams.params[5] != 0.0f) {
                applyDropShadowInset(screenParams, src, dst, scratch, timers);
            } else {
                applyDropShadow(screenParams, src, scratchB, dst, scratch, timers);
            }
            break;
        case EffectKind::Glow:
            applyGlow(screenParams, src, dst, scratch, timers);
            break;
        case EffectKind::Blur:
            applyBlur(screenParams, src, scratchA, dst, timers);
            break;
        case EffectKind::ColorAdjust:
            applyColorAdjust(screenParams, src, dst, timers);
            break;
        case EffectKind::GradientMask:
            applyGradientMask(screenParams, stops, invScreenXf, bufferOriginPx, src, dst, timers);
            break;
        case EffectKind::Shine:
            applyShine(screenParams, bufferOriginPx, contentBoundsPx, src, dst, timers);
            break;
        case EffectKind::Outline:
            applyOutline(screenParams, src, dst, scratch, timers);
            break;
        case EffectKind::BackdropBlur:
            // 18.6: BackdropBlur is the stack's first stage and is run by sw_renderer.cpp's
            // PopEffect (it needs the backdrop snapshot as a second input, which this entry point
            // has no room for). Reaching it here means a malformed command list put it elsewhere
            // in the stack; pass the content through, same as the Custom branch below.
            dst = src;
            break;
        case EffectKind::None:
        case EffectKind::Custom:
        default:
            // T-203: sw_renderer.cpp replaces a Custom effect by its fallback before it gets here
            // (18.7); anything else passes the content through unfiltered.
            XT_ASSERT(false && "unsupported: Custom/unknown effect kind");
            dst = src;
            break;
    }
}

void captureBackdrop(const Target& src, int x0, int y0, int w, int h, Surface& out) {
    out.w = std::max(0, w);
    out.h = std::max(0, h);
    out.rgba.resize(static_cast<size_t>(out.w) * static_cast<size_t>(out.h) * 4);
    for (int y = 0; y < out.h; ++y) {
        float* row = out.row(y);
        for (int x = 0; x < out.w; ++x) {
            Color c = src.get(x0 + x, y0 + y);
            float* p = row + static_cast<size_t>(x) * 4;
            p[0] = c.r;
            p[1] = c.g;
            p[2] = c.b;
            p[3] = c.a;
        }
    }
}

void buildBackdropPyramid(const Target& src, int x0, int y0, int w, int h, float maxSigma,
                          BackdropPyramid& out, const EffectTimers& timers) {
    out.x0 = x0;
    out.y0 = y0;
    out.levels = 0;
    captureBackdrop(src, x0, y0, w, h, out.level[0]);
    out.copiedPx = static_cast<uint32_t>(std::max(0, w)) * static_cast<uint32_t>(std::max(0, h));
    if (out.level[0].w <= 0 || out.level[0].h <= 0) {
        return;
    }
    PerfFieldScope blurScope(timers.blur);
    out.levels = backdropPyramidLevels(std::min(maxSigma, 32.0f));
    std::vector<float> box;
    Surface boxed;
    for (int l = 1; l <= out.levels; ++l) {
        const Surface& prev = out.level[l - 1];
        int bw = 0, bh = 0;
        fx::downsample2x2(prev.rgba.data(), prev.w, prev.h, 4, box, bw, bh);
        boxed.w = bw;
        boxed.h = bh;
        boxed.rgba.swap(box);
        kawasePass(boxed, out.level[l]);
        box.swap(boxed.rgba);
    }
}

void applyBackdropBlur(const EffectInstance& screenParams, const BackdropPyramid& pyramid,
                       Vec2 bufferOriginPx, const Surface& src, Surface& dst, EffectScratch& scratch,
                       const EffectTimers& timers) {
    if (src.w <= 0 || src.h <= 0 || pyramid.level[0].w <= 0 || pyramid.level[0].h <= 0 ||
        screenParams.enabled == 0) {
        dst = src;
        return;
    }
    applyBackdropBlurImpl(screenParams, pyramid, bufferOriginPx, src, dst, scratch, timers);
}

void applyEffect(const EffectInstance& screenParams, std::span<const GradientStop> stops,
                 const Mat2x3& invScreenXf, Vec2 bufferOriginPx, const Rect& contentBoundsPx,
                 const Surface& src, Surface& scratchA, Surface& scratchB, Surface& dst,
                 const EffectTimers& timers) {
    applyEffect(screenParams, stops, invScreenXf, bufferOriginPx, contentBoundsPx, src, scratchA, scratchB,
                dst, fallbackScratch(), timers);
}

void applyEffectStack(std::span<const EffectInstance> screenParams, std::span<const GradientStop> stops,
                      const Mat2x3& invScreenXf, Vec2 bufferOriginPx, const Rect& contentBoundsPx,
                      const Surface& src, Surface& scratchA, Surface& scratchB, Surface& dst,
                      EffectScratch& scratch, const EffectTimers& timers) {
    // An effect layer clipped entirely out of the target (an element with an effect that lies
    // outside the viewport) has an empty buffer; the blur/dilate/SDF kernels index `h - 1` /
    // `w - 1` and must never see one.
    if (screenParams.empty() || src.w <= 0 || src.h <= 0) {
        dst = src;
        return;
    }
    if (screenParams.size() == 1) {
        applyEffect(screenParams[0], stops, invScreenXf, bufferOriginPx, contentBoundsPx, src, scratchA,
                    scratchB, dst, scratch, timers);
        return;
    }

    const Surface* cur = &src;
    Surface* next = &scratch.stageA;
    for (size_t i = 0; i < screenParams.size(); ++i) {
        Surface* out = (i + 1 == screenParams.size()) ? &dst : next;
        applyEffect(screenParams[i], stops, invScreenXf, bufferOriginPx, contentBoundsPx, *cur, scratchA,
                    scratchB, *out, scratch, timers);
        cur = out;
        next = (next == &scratch.stageA) ? &scratch.stageB : &scratch.stageA;
    }
}

void applyEffectStack(std::span<const EffectInstance> screenParams, std::span<const GradientStop> stops,
                      const Mat2x3& invScreenXf, Vec2 bufferOriginPx, const Rect& contentBoundsPx,
                      const Surface& src, Surface& scratchA, Surface& scratchB, Surface& dst,
                      const EffectTimers& timers) {
    applyEffectStack(screenParams, stops, invScreenXf, bufferOriginPx, contentBoundsPx, src, scratchA,
                     scratchB, dst, fallbackScratch(), timers);
}

}  // namespace xt::sw

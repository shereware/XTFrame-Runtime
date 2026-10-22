#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "stb_image.h"  // NOLINT: implementation compiled once, in sw_png.cpp.
#include "sw_blend.h"
#include "sw_dds.h"
#include "sw_layer.h"
#include "sw_simd.h"
#include "xt/dds.h"
#include "xt/simd.h"
#include "xt/sw/sw_png.h"

/// DrawImage sampling and stb_image-backed decodeImageBytes. See Docs/design/10-backends.md
/// §10.2.5, Docs/tasks/T-12.md "描邊、Dash、ClipPath、圖片", "AssetResolver".
///
/// Docs/tasks/T-149.md: every covered span is first *sampled* into a premultiplied float scratch
/// row (coverage and opacity folded in, clip not yet) and then *blended* by the T-102 span kernels
/// over the runs the clip mask lets through, instead of one Target::get/set per pixel. Sampling
/// itself has two paths:
///  - axis-aligned (`xf` has no rotation/shear, i.e. translate + scale, negative scale included):
///    the source row is fixed per device row and the source column advances by a constant 32.32
///    fixed-point step, so the bilinear filter becomes one vertical row blend (two source rows
///    pre-mixed once, `ImageScratch::row`) plus one horizontal lerp per pixel;
///  - general (rotation, shear, perspective quads): the previous per-pixel inverse mapping, with
///    the four texel lerps done on `xt::simd::f32x4` (SSE2, scalar where absent).
/// The arithmetic is the same operators as before (non-premultiplied lerp, then `rgb * a`), only
/// reassociated where the row blend swaps the lerp order, so golden images stay within the
/// Docs/design/15-testing.md §15.3 tolerance.

namespace xt::sw {

bool decodeImageBytes(const uint8_t* bytes, size_t size, ImageData& out) {
    if (bytes == nullptr || size == 0) {
        return false;
    }
    if (isDds(bytes, size)) {
        return decodeDdsBytes(bytes, size, out);  // T-199: BC1/BC3/BC7 (or RGBA8) DDS.
    }
    int w = 0;
    int h = 0;
    int channels = 0;
    // T-258a: the header's size is checked before anything is allocated for the pixels.
    if (stbi_info_from_memory(bytes, static_cast<int>(size), &w, &h, &channels) == 0 || w <= 0 || h <= 0 ||
        w > kMaxImageDimension || h > kMaxImageDimension ||
        static_cast<int64_t>(w) * static_cast<int64_t>(h) > kMaxImagePixels) {
        return false;
    }
    unsigned char* data =
        stbi_load_from_memory(bytes, static_cast<int>(size), &w, &h, &channels, 4 /* force RGBA */);
    if (data == nullptr) {
        return false;
    }
    ImageData result;
    result.width = w;
    result.height = h;
    result.rgba.assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    stbi_image_free(data);
    out = std::move(result);
    return true;
}

namespace {

using simd::f32x4;

inline f32x4 vsplat(float s) { return simd::splat(static_cast<const f32x4*>(nullptr), s); }
inline f32x4 vload(const float* p) { return simd::load(static_cast<const f32x4*>(nullptr), p); }
inline f32x4 vloadU8(const uint8_t* p) { return simd::loadU8(static_cast<const f32x4*>(nullptr), p); }
/// `a + (b - a) * t`, the same form as the scalar `lerp` this file used before T-149.
inline f32x4 vlerp(f32x4 a, f32x4 b, f32x4 t) { return simd::fma(simd::sub(b, a), t, a); }
/// Mask selecting lane 3 (alpha) only.
inline f32x4 alphaLaneMask() {
    const float sel[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    return simd::cmpGt(vload(sel), vsplat(0.5f));
}
/// Non-premultiplied sample `s` -> premultiplied `(rgb * a, a)` with `a = s.a * k` (`k` is the
/// span's coverage * opacity). Same multiply order as the old `Target::blendSrcOver` call site.
inline f32x4 premulSample(f32x4 s, f32x4 k, f32x4 alphaMask) {
    const f32x4 a = simd::mul(simd::splatW(s), k);
    return simd::blend(simd::mul(s, a), a, alphaMask);
}

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int fastFloor(float f) {
    int i = static_cast<int>(f);
    return f < static_cast<float>(i) ? i - 1 : i;
}

inline const uint8_t* texelPtr(const ImageData& img, int x, int y) {
    x = clampi(x, 0, img.width - 1);
    y = clampi(y, 0, img.height - 1);
    return img.rgba.data() +
           (static_cast<size_t>(y) * static_cast<size_t>(img.width) + static_cast<size_t>(x)) * 4;
}

/// Nearest-neighbour texel at normalized (u, v), non-premultiplied, in [0, 1].
inline f32x4 sampleNearest(const ImageData& img, float u, float v) {
    int x = fastFloor(u * static_cast<float>(img.width));
    int y = fastFloor(v * static_cast<float>(img.height));
    return vloadU8(texelPtr(img, x, y));
}

/// Bilinear texel at normalized (u, v), non-premultiplied. Horizontal lerps first, then vertical,
/// exactly as before T-149.
inline f32x4 sampleBilinear(const ImageData& img, float u, float v) {
    float fx = u * static_cast<float>(img.width) - 0.5f;
    float fy = v * static_cast<float>(img.height) - 0.5f;
    int x0 = fastFloor(fx);
    int y0 = fastFloor(fy);
    const f32x4 tx = vsplat(fx - static_cast<float>(x0));
    const f32x4 ty = vsplat(fy - static_cast<float>(y0));
    const f32x4 c00 = vloadU8(texelPtr(img, x0, y0));
    const f32x4 c10 = vloadU8(texelPtr(img, x0 + 1, y0));
    const f32x4 c01 = vloadU8(texelPtr(img, x0, y0 + 1));
    const f32x4 c11 = vloadU8(texelPtr(img, x0 + 1, y0 + 1));
    return vlerp(vlerp(c00, c10, tx), vlerp(c01, c11, tx), ty);
}

/// One device row of the destination, looked up once per span.
struct DstRow {
    float* rowF = nullptr;
    uint8_t* rowU8 = nullptr;
    bool premul = false;  // Target::u8Premultiplied
    int originX = 0;
    /// Resolves row `y` and clamps `[x0, x1)` to the target's columns. False when nothing of the
    /// span lands inside the target (the row is outside, or the clamp empties the span).
    bool resolve(const Target& target, int y, int& x0, int& x1) {
        rowF = target.rowF(y);
        rowU8 = rowF != nullptr ? nullptr : target.rowU8(y);
        if (rowF == nullptr && rowU8 == nullptr) {
            return false;
        }
        premul = target.u8Premultiplied;
        originX = target.originX;
        if (x0 < target.originX) {
            x0 = target.originX;
        }
        if (x1 > target.originX + target.w) {
            x1 = target.originX + target.w;
        }
        return x0 < x1;
    }
};

/// Blends `src` (n premultiplied float samples for device pixels `[x, x + n)`, coverage and
/// opacity already applied) onto `row`. The clip mask splits the span into runs: 0 skips the pixel
/// (never written, so a partial redraw's damage mask leaves the pixel byte-identical), 255 goes to
/// one kernel call, anything else scales that pixel's sample in place first. A sample with alpha 0
/// is skipped too, matching the old `a <= 0 -> continue`. `allOpaque` says every sample has alpha
/// exactly 1, which reduces src-over to a store (Docs/tasks/T-128.md step 2).
void blendSpan(const DstRow& row, int x, int n, float* src, const uint8_t* clipRow, bool allOpaque) {
    const SwSimdOps& ops = swSimd();
    const int end = x + n;
    int i = x;
    while (i < end) {
        const size_t si = static_cast<size_t>(i - x) * 4;
        uint8_t clip = 255;
        if (clipRow != nullptr) {
            clip = clipRow[static_cast<size_t>(i)];
        }
        if (clip == 0 || src[si + 3] <= 0.0f) {
            ++i;
            continue;
        }
        const size_t lx = static_cast<size_t>(i - row.originX) * 4;
        if (clip == 255) {
            int e = i + 1;
            if (allOpaque) {
                while (e < end && (clipRow == nullptr || clipRow[static_cast<size_t>(e)] == 255)) {
                    ++e;
                }
            } else {
                while (e < end && (clipRow == nullptr || clipRow[static_cast<size_t>(e)] == 255) &&
                       src[static_cast<size_t>(e - x) * 4 + 3] > 0.0f) {
                    ++e;
                }
            }
            const int m = e - i;
            if (allOpaque) {
                if (row.rowF != nullptr) {
                    std::memcpy(row.rowF + lx, src + si, static_cast<size_t>(m) * 4 * sizeof(float));
                } else {
                    ops.rowToU8(src + si, row.rowU8 + lx, m, row.premul);
                }
            } else if (row.rowF != nullptr) {
                ops.spanSrcOverF(row.rowF + lx, src + si, m);
            } else {
                ops.spanSrcOverU8(row.rowU8 + lx, src + si, m, row.premul);
            }
            i = e;
            continue;
        }
        // Partial clip: fold it into this one sample, then blend it alone.
        const float c = static_cast<float>(clip) / 255.0f;
        simd::store(src + si, simd::mul(vload(src + si), vsplat(c)));
        if (row.rowF != nullptr) {
            ops.spanSrcOverF(row.rowF + lx, src + si, 1);
        } else {
            ops.spanSrcOverU8(row.rowU8 + lx, src + si, 1, row.premul);
        }
        ++i;
    }
}

/// 32.32 fixed point for the axis-aligned path's source coordinate: 1920 steps accumulate well
/// under 1e-6 texel of error, far inside the golden tolerance.
constexpr int kFixShift = 32;
constexpr double kFixOne = 4294967296.0;

inline int64_t toFixed(double v) { return static_cast<int64_t>(std::llround(v * kFixOne)); }
inline int fixedFloor(int64_t f) { return static_cast<int>(f >> kFixShift); }
inline float fixedFrac(int64_t f) {
    return static_cast<float>(static_cast<double>(f & ((static_cast<int64_t>(1) << kFixShift) - 1)) /
                              kFixOne);
}

/// Source coordinate along one axis of the axis-aligned path: `fixed(i) = base + i * step` for
/// the i-th device pixel of a span, clamped to `[lo, hi]` (the old `clamp(su, 0, 1)`).
struct AxisMap {
    int64_t base = 0;
    int64_t step = 0;
    int64_t lo = 0;
    int64_t hi = 0;
    int64_t at(int i) const {
        int64_t f = base + step * static_cast<int64_t>(i);
        return f < lo ? lo : (f > hi ? hi : f);
    }
};

/// Builds the map for device pixels `x0..` on one axis. `k` and `c` come from the inverse device
/// transform: local = k * (x + 0.5) + c; then u = (local - dstOrigin) / dstSize, src = uvOrigin +
/// u * uvSize (clamped to [0, 1]), and the texel coordinate is src * texels (minus 0.5 for the
/// bilinear filter's texel-centre convention).
AxisMap makeAxisMap(double k, double c, double dstOrigin, double dstSize, double uvOrigin, double uvSize,
                    double texels, double offset, int x0) {
    const double scale = uvSize * texels / dstSize;
    const double step = k * scale;
    const double base =
        (k * (static_cast<double>(x0) + 0.5) + c - dstOrigin) * scale + uvOrigin * texels + offset;
    AxisMap m;
    m.step = toFixed(step);
    m.base = toFixed(base);
    // clamp(src, 0, 1) -> texel coordinate in [offset, texels + offset].
    m.lo = toFixed(offset);
    m.hi = toFixed(texels + offset);
    return m;
}

/// The axis-aligned path's sampler for one span (shared by DrawImage and, T-245, the axis-aligned
/// triangles of DrawImageMesh): `n` device pixels whose source column is `xm.at(i)` and whose
/// source row is the fixed-point `fy`, written premultiplied (times `kv`) into `span`. Returns
/// true when every texel the span read is known to be opaque (the caller still folds coverage and
/// opacity into its `allOpaque`).
bool sampleAxisSpan(const ImageData& image, bool smooth, const AxisMap& xm, int64_t fy, int n, f32x4 kv,
                    f32x4 alphaMask, float* span, float* rowBlend) {
    const int W = image.width;
    const int H = image.height;
    const int sy0 = fixedFloor(fy);
    // Columns this span touches (the map is monotonic, so the two ends bound it).
    const int cA = fixedFloor(xm.at(0));
    const int cB = fixedFloor(xm.at(n - 1));
    const int cMin = clampi(std::min(cA, cB), 0, W - 1);
    const int cMax = clampi(std::max(cA, cB) + (smooth ? 1 : 0), 0, W - 1);
    const int cCount = cMax - cMin + 1;

    uint8_t alphaAnd = 255;
    bool alphaKnown = false;
    if (smooth) {
        const int y0c = clampi(sy0, 0, H - 1);
        const int y1c = clampi(sy0 + 1, 0, H - 1);
        const uint8_t* r0 = image.rgba.data() + static_cast<size_t>(y0c) * static_cast<size_t>(W) * 4;
        const uint8_t* r1 = image.rgba.data() + static_cast<size_t>(y1c) * static_cast<size_t>(W) * 4;
        const f32x4 ty = vsplat(fixedFrac(fy));
        if (cCount <= 2 * n) {
            // Pre-blend the two source rows once over the touched columns, then one
            // horizontal lerp per pixel.
            for (int c = cMin; c <= cMax; ++c) {
                const size_t o = static_cast<size_t>(c) * 4;
                alphaAnd = static_cast<uint8_t>(alphaAnd & r0[o + 3] & r1[o + 3]);
                simd::store(rowBlend + o, vlerp(vloadU8(r0 + o), vloadU8(r1 + o), ty));
            }
            alphaKnown = true;
            for (int i = 0; i < n; ++i) {
                const int64_t fx = xm.at(i);
                const int sx0 = fixedFloor(fx);
                const int xa = clampi(sx0, 0, W - 1);
                const int xb = clampi(sx0 + 1, 0, W - 1);
                const f32x4 s = vlerp(vload(rowBlend + static_cast<size_t>(xa) * 4),
                                      vload(rowBlend + static_cast<size_t>(xb) * 4), vsplat(fixedFrac(fx)));
                simd::store(span + static_cast<size_t>(i) * 4, premulSample(s, kv, alphaMask));
            }
        } else {
            // Strong minification: fewer texels are read by sampling four per pixel than by
            // pre-blending every column in between.
            f32x4 minv = vsplat(1.0f);
            for (int i = 0; i < n; ++i) {
                const int64_t fx = xm.at(i);
                const int sx0 = fixedFloor(fx);
                const size_t xa = static_cast<size_t>(clampi(sx0, 0, W - 1)) * 4;
                const size_t xb = static_cast<size_t>(clampi(sx0 + 1, 0, W - 1)) * 4;
                const f32x4 tx = vsplat(fixedFrac(fx));
                const f32x4 s = vlerp(vlerp(vloadU8(r0 + xa), vloadU8(r0 + xb), tx),
                                      vlerp(vloadU8(r1 + xa), vloadU8(r1 + xb), tx), ty);
                minv = simd::min(minv, s);
                simd::store(span + static_cast<size_t>(i) * 4, premulSample(s, kv, alphaMask));
            }
            float mins[4];
            simd::store(mins, minv);
            alphaAnd = mins[3] >= 1.0f ? 255 : 0;
            alphaKnown = true;
        }
    } else {
        const int syc = clampi(sy0, 0, H - 1);
        const uint8_t* r0 = image.rgba.data() + static_cast<size_t>(syc) * static_cast<size_t>(W) * 4;
        if (cCount <= 2 * n) {
            for (int c = cMin; c <= cMax; ++c) {
                alphaAnd = static_cast<uint8_t>(alphaAnd & r0[static_cast<size_t>(c) * 4 + 3]);
            }
            alphaKnown = true;
        }
        for (int i = 0; i < n; ++i) {
            const int sx = clampi(fixedFloor(xm.at(i)), 0, W - 1);
            const f32x4 s = vloadU8(r0 + static_cast<size_t>(sx) * 4);
            simd::store(span + static_cast<size_t>(i) * 4, premulSample(s, kv, alphaMask));
        }
    }
    return alphaKnown && alphaAnd == 255;
}

/// Axis-aligned DrawImage (deviceXf.b == deviceXf.c == 0). See the file comment.
void drawImageAxisAligned(Target& target, Rasterizer& raster, const Mat2x3& inverse, const ImageCmd& cmd,
                          const ImageData& image, const uint8_t* clipMask, int clipWidth,
                          ImageScratch& scratch) {
    const int W = image.width;
    const int H = image.height;
    const bool smooth = cmd.smooth != 0;
    const double offset = smooth ? -0.5 : 0.0;
    const float opacity = cmd.opacity;
    const f32x4 alphaMask = alphaLaneMask();
    scratch.span.resize(static_cast<size_t>(target.w) * 4);
    scratch.row.resize(static_cast<size_t>(W) * 4);
    float* span = scratch.span.data();
    float* rowBlend = scratch.row.data();

    raster.sweep(FillRule::NonZero, [&](int y, int x0, int x1, uint8_t coverage) {
        if (coverage == 0) {
            return;
        }
        DstRow row;
        if (!row.resolve(target, y, x0, x1)) {
            return;
        }
        const int n = x1 - x0;
        const uint8_t* clipRow = clipMask != nullptr
                                     ? clipMask + static_cast<size_t>(y) * static_cast<size_t>(clipWidth)
                                     : nullptr;
        const float k = (static_cast<float>(coverage) / 255.0f) * opacity;
        const f32x4 kv = vsplat(k);

        // Source row(s) for this device row.
        const AxisMap ym = makeAxisMap(inverse.d, inverse.f, cmd.dst.y, cmd.dst.h, cmd.srcUv.y, cmd.srcUv.h,
                                       static_cast<double>(H), offset, y);
        const int64_t fy = ym.at(0);
        const AxisMap xm = makeAxisMap(inverse.a, inverse.e, cmd.dst.x, cmd.dst.w, cmd.srcUv.x, cmd.srcUv.w,
                                       static_cast<double>(W), offset, x0);
        const bool opaqueSrc = sampleAxisSpan(image, smooth, xm, fy, n, kv, alphaMask, span, rowBlend);
        const bool allOpaque = opaqueSrc && coverage == 255 && opacity == 1.0f;
        blendSpan(row, x0, n, span, clipRow, allOpaque);
    });
}

/// General DrawImage (rotation/shear): per-pixel inverse mapping, SIMD lerps.
void drawImageGeneral(Target& target, Rasterizer& raster, const Mat2x3& inverse, const ImageCmd& cmd,
                      const ImageData& image, const uint8_t* clipMask, int clipWidth, ImageScratch& scratch) {
    const bool smooth = cmd.smooth != 0;
    const float opacity = cmd.opacity;
    const f32x4 alphaMask = alphaLaneMask();
    scratch.span.resize(static_cast<size_t>(target.w) * 4);
    float* span = scratch.span.data();

    raster.sweep(FillRule::NonZero, [&](int y, int x0, int x1, uint8_t coverage) {
        if (coverage == 0) {
            return;
        }
        DstRow row;
        if (!row.resolve(target, y, x0, x1)) {
            return;
        }
        const int n = x1 - x0;
        const uint8_t* clipRow = clipMask != nullptr
                                     ? clipMask + static_cast<size_t>(y) * static_cast<size_t>(clipWidth)
                                     : nullptr;
        const float k = (static_cast<float>(coverage) / 255.0f) * opacity;
        const f32x4 kv = vsplat(k);
        f32x4 minv = vsplat(1.0f);
        const float cy = static_cast<float>(y) + 0.5f;
        for (int i = 0; i < n; ++i) {
            const float cx = static_cast<float>(x0 + i) + 0.5f;
            const Vec2 local = apply(inverse, Vec2{cx, cy});
            const float u = (local.x - cmd.dst.x) / cmd.dst.w;
            const float v = (local.y - cmd.dst.y) / cmd.dst.h;
            const float su = std::clamp(cmd.srcUv.x + u * cmd.srcUv.w, 0.0f, 1.0f);
            const float sv = std::clamp(cmd.srcUv.y + v * cmd.srcUv.h, 0.0f, 1.0f);
            const f32x4 s = smooth ? sampleBilinear(image, su, sv) : sampleNearest(image, su, sv);
            minv = simd::min(minv, s);
            simd::store(span + static_cast<size_t>(i) * 4, premulSample(s, kv, alphaMask));
        }
        float mins[4];
        simd::store(mins, minv);
        const bool allOpaque = mins[3] >= 1.0f && coverage == 255 && opacity == 1.0f;
        blendSpan(row, x0, n, span, clipRow, allOpaque);
    });
}

/// T-202: `dst += src` (premultiplied, clamped to 1) over `n` samples for device pixels
/// `[x, x + n)`. Pixels the clip mask zeroes and zero samples are never touched.
void addSpan(const DstRow& row, int x, int n, const float* src, const uint8_t* clipRow) {
    const SwSimdOps& ops = swSimd();
    for (int i = 0; i < n; ++i) {
        const float* s = src + static_cast<size_t>(i) * 4;
        const float c =
            clipRow != nullptr ? static_cast<float>(clipRow[static_cast<size_t>(x + i)]) / 255.0f : 1.0f;
        if (c <= 0.0f || (s[0] <= 0.0f && s[1] <= 0.0f && s[2] <= 0.0f && s[3] <= 0.0f)) {
            continue;
        }
        const size_t lx = static_cast<size_t>(x + i - row.originX) * 4;
        float px[4];
        float* d = row.rowF != nullptr ? row.rowF + lx : px;
        if (row.rowF == nullptr) {
            ops.rowFromU8(row.rowU8 + lx, px, 1, row.premul);
        }
        for (int k = 0; k < 4; ++k) {
            d[k] = std::min(1.0f, d[k] + s[k] * c);
        }
        if (row.rowF == nullptr) {
            ops.rowToU8(px, row.rowU8 + lx, 1, row.premul);
        }
    }
}

}  // namespace

void drawParticles(Target& target, Rasterizer& raster, const Mat2x3& deviceXf,
                   const ParticleInstance* instances, uint32_t count, bool additive, const ImageData* image,
                   const uint8_t* clipMask, int clipWidth, ImageScratch& scratch) {
    if (image != nullptr && (image->width <= 0 || image->height <= 0 || image->rgba.empty())) {
        return;  // A texture that did not decode draws nothing (like DrawImage).
    }
    const f32x4 alphaMask = alphaLaneMask();
    const f32x4 one = vsplat(1.0f);
    scratch.span.resize(static_cast<size_t>(target.w) * 4);
    float* span = scratch.span.data();
    for (uint32_t pi = 0; pi < count; ++pi) {
        const ParticleInstance& p = instances[pi];
        if (!(p.w > 0.0f) || !(p.h > 0.0f) || !(p.rgba.a > 0.0f)) {
            continue;
        }
        // The quad's corners: top-left + u * ax + v * ay, (u, v) in the unit square.
        const float cs = std::cos(p.rot);
        const float sn = std::sin(p.rot);
        const Vec2 ax{cs * p.w, sn * p.w};
        const Vec2 ay{-sn * p.h, cs * p.h};
        const Vec2 tl{p.x - 0.5f * (ax.x + ay.x), p.y - 0.5f * (ax.y + ay.y)};
        const Mat2x3 unitToDevice = mul(deviceXf, Mat2x3{ax.x, ax.y, ay.x, ay.y, tl.x, tl.y});
        Mat2x3 deviceToUnit;
        if (!invert(unitToDevice, deviceToUnit)) {
            continue;
        }
        const Vec2 q[4] = {apply(unitToDevice, Vec2{0, 0}), apply(unitToDevice, Vec2{1, 0}),
                           apply(unitToDevice, Vec2{1, 1}), apply(unitToDevice, Vec2{0, 1})};
        raster.reset(target.originX + target.w, target.originY + target.h);
        for (int i = 0; i < 4; ++i) {
            raster.addLine(q[i], q[(i + 1) % 4]);
        }
        const float tint[4] = {p.rgba.r, p.rgba.g, p.rgba.b, p.rgba.a};
        const f32x4 tintV = vload(tint);
        raster.sweep(FillRule::NonZero, [&](int y, int x0, int x1, uint8_t coverage) {
            if (coverage == 0) {
                return;
            }
            DstRow row;
            if (!row.resolve(target, y, x0, x1)) {
                return;
            }
            const int n = x1 - x0;
            const uint8_t* clipRow = clipMask != nullptr
                                         ? clipMask + static_cast<size_t>(y) * static_cast<size_t>(clipWidth)
                                         : nullptr;
            const f32x4 kv = vsplat(static_cast<float>(coverage) / 255.0f);
            const float cy = static_cast<float>(y) + 0.5f;
            for (int i = 0; i < n; ++i) {
                float* out = span + static_cast<size_t>(i) * 4;
                f32x4 texel = one;
                if (image != nullptr) {
                    const Vec2 uv = apply(deviceToUnit, Vec2{static_cast<float>(x0 + i) + 0.5f, cy});
                    const float u = std::clamp(uv.x, 0.0f, 1.0f);
                    const float v = std::clamp(uv.y, 0.0f, 1.0f);
                    const float su = p.uv0.x + u * (p.uv1.x - p.uv0.x);
                    const float sv = p.uv0.y + v * (p.uv1.y - p.uv0.y);
                    texel = premulSample(sampleBilinear(*image, su, sv), one, alphaMask);
                }
                simd::store(out, simd::mul(simd::mul(texel, tintV), kv));
            }
            if (additive) {
                addSpan(row, x0, n, span, clipRow);
            } else {
                blendSpan(row, x0, n, span, clipRow, false);
            }
        });
    }
}

void drawImage(Target& target, Rasterizer& raster, const Mat2x3& deviceXf, const ImageCmd& cmd,
               const ImageData& image, const uint8_t* clipMask, int clipWidth, ImageScratch& scratch) {
    if (image.width <= 0 || image.height <= 0 || image.rgba.empty()) {
        return;
    }
    if (cmd.dst.w == 0.0f || cmd.dst.h == 0.0f) {
        return;
    }
    Mat2x3 inverse;
    if (!invert(deviceXf, inverse)) {
        // A degenerate (non-invertible) device transform is a known limitation, not covered by
        // Docs/tasks/T-12.md; skip the command rather than dividing by zero.
        return;
    }

    Vec2 corners[4] = {
        apply(deviceXf, Vec2{cmd.dst.x, cmd.dst.y}),
        apply(deviceXf, Vec2{cmd.dst.x + cmd.dst.w, cmd.dst.y}),
        apply(deviceXf, Vec2{cmd.dst.x + cmd.dst.w, cmd.dst.y + cmd.dst.h}),
        apply(deviceXf, Vec2{cmd.dst.x, cmd.dst.y + cmd.dst.h}),
    };
    // Device-space extent: `target` may be a bounded effect/layer buffer with an origin offset, but
    // the polygon corners are in device coordinates (the span blend subtracts the origin).
    raster.reset(target.originX + target.w, target.originY + target.h);
    for (int i = 0; i < 4; ++i) {
        raster.addLine(corners[i], corners[(i + 1) % 4]);
    }

    if (deviceXf.b == 0.0f && deviceXf.c == 0.0f) {
        drawImageAxisAligned(target, raster, inverse, cmd, image, clipMask, clipWidth, scratch);
    } else {
        drawImageGeneral(target, raster, inverse, cmd, image, clipMask, clipWidth, scratch);
    }
}

void drawImageQuad(Target& target, Rasterizer& raster, const Vec2 q[4], Rect srcUv, float opacity,
                   uint8_t smooth, const ImageData& image, const uint8_t* clipMask, int clipWidth,
                   ImageScratch& scratch) {
    if (image.width <= 0 || image.height <= 0 || image.rgba.empty()) {
        return;
    }
    Vec2 unitSquare[4] = {Vec2{0, 0}, Vec2{1, 0}, Vec2{1, 1}, Vec2{0, 1}};
    Mat3 hinv;
    if (!homographyFromQuad(q, unitSquare, hinv)) {
        // Degenerate quad (e.g. collinear/coincident corners): skip, per Docs/tasks/T-45.md
        // "退化（homographyFromQuad 失敗）→ 跳過".
        return;
    }
    // Docs/tasks/T-149.md: a quad that is still an axis-aligned rectangle (the projection stack
    // held only translation/scale, which is how a whole board scaled to the viewport arrives here)
    // is the axis-aligned DrawImage of the unit square under `translate(q[0]) * scale(w, h)`;
    // route it there instead of evaluating a homography per pixel. Corners come out of float
    // matrix products, so "equal" allows 1e-3 px and the rectangle snaps to q[0], q[1].x, q[3].y.
    constexpr float kAxisEps = 1e-3f;
    if (std::fabs(q[0].y - q[1].y) <= kAxisEps && std::fabs(q[2].y - q[3].y) <= kAxisEps &&
        std::fabs(q[0].x - q[3].x) <= kAxisEps && std::fabs(q[1].x - q[2].x) <= kAxisEps) {
        ImageCmd cmd{};
        cmd.xf = Mat2x3{q[1].x - q[0].x, 0.0f, 0.0f, q[3].y - q[0].y, q[0].x, q[0].y};
        cmd.dst = Rect{0.0f, 0.0f, 1.0f, 1.0f};
        cmd.srcUv = srcUv;
        cmd.opacity = opacity;
        cmd.smooth = smooth;
        drawImage(target, raster, cmd.xf, cmd, image, clipMask, clipWidth, scratch);
        return;
    }
    // Device-space extent: `target` may be a bounded effect/layer buffer with an origin offset, but
    // the polygon corners are in device coordinates (the span blend subtracts the origin).
    raster.reset(target.originX + target.w, target.originY + target.h);
    for (int i = 0; i < 4; ++i) {
        raster.addLine(q[i], q[(i + 1) % 4]);
    }
    const f32x4 alphaMask = alphaLaneMask();
    const f32x4 zero = vsplat(0.0f);
    scratch.span.resize(static_cast<size_t>(target.w) * 4);
    float* span = scratch.span.data();

    raster.sweep(FillRule::NonZero, [&](int y, int x0, int x1, uint8_t coverage) {
        if (coverage == 0) {
            return;
        }
        DstRow row;
        if (!row.resolve(target, y, x0, x1)) {
            return;
        }
        const int n = x1 - x0;
        const uint8_t* clipRow = clipMask != nullptr
                                     ? clipMask + static_cast<size_t>(y) * static_cast<size_t>(clipWidth)
                                     : nullptr;
        const float k = (static_cast<float>(coverage) / 255.0f) * opacity;
        const f32x4 kv = vsplat(k);
        f32x4 minv = vsplat(1.0f);
        const float cy = static_cast<float>(y) + 0.5f;
        for (int i = 0; i < n; ++i) {
            float* out = span + static_cast<size_t>(i) * 4;
            const Vec2 uv = mat3Apply(hinv, Vec2{static_cast<float>(x0 + i) + 0.5f, cy});
            if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) {
                // Outside the quad's own uv square: alpha 0, which blendSpan skips.
                simd::store(out, zero);
                minv = zero;
                continue;
            }
            const float su = std::clamp(srcUv.x + uv.x * srcUv.w, 0.0f, 1.0f);
            const float sv = std::clamp(srcUv.y + uv.y * srcUv.h, 0.0f, 1.0f);
            const f32x4 s = smooth != 0 ? sampleBilinear(image, su, sv) : sampleNearest(image, su, sv);
            minv = simd::min(minv, s);
            simd::store(out, premulSample(s, kv, alphaMask));
        }
        float mins[4];
        simd::store(mins, minv);
        const bool allOpaque = mins[3] >= 1.0f && coverage == 255 && opacity == 1.0f;
        blendSpan(row, x0, n, span, clipRow, allOpaque);
    });
}

namespace {

/// Sub-pixel precision of the triangle setup: device coordinates snap to 1/256 px, so every edge
/// function is an exact int64 and two triangles sharing an edge evaluate it identically (with
/// opposite signs) -- the precondition of the top-left rule below.
constexpr int kTriSubBits = 8;
constexpr int64_t kTriSub = int64_t{1} << kTriSubBits;
/// Vertices farther than this from the origin (device px) are rejected: it keeps every edge
/// function product below 2^62.
constexpr float kTriMaxCoord = 2097152.0f;  // 2^21

inline int64_t floorDiv(int64_t a, int64_t b) {  // b > 0
    int64_t q = a / b;
    if ((a % b) != 0 && a < 0) {
        --q;
    }
    return q;
}
inline int64_t ceilDiv(int64_t a, int64_t b) { return -floorDiv(-a, b); }  // b > 0

/// One edge `a -> b` of a positively oriented triangle (fixed point): inside means
/// `E(p) = dx * (p.y - a.y) - dy * (p.x - a.x) >= minE`, where `minE` is 0 for a top or left edge
/// (a pixel center exactly on it belongs to this triangle) and 1 otherwise.
struct TriEdge {
    int64_t ax, ay, dx, dy, minE;
};

TriEdge makeEdge(int64_t ax, int64_t ay, int64_t bx, int64_t by) {
    TriEdge e{ax, ay, bx - ax, by - ay, 1};
    // Top-left rule (y down): with E > 0 inside, a top edge runs +x (dy == 0, dx > 0) and a left
    // edge runs -y (dy < 0). A shared edge is traversed in opposite directions by its two
    // triangles, so exactly one of them owns the pixel centers lying on it.
    if (e.dy < 0 || (e.dy == 0 && e.dx > 0)) {
        e.minE = 0;
    }
    return e;
}

/// Narrows the inclusive pixel-column range [lo, hi] of device row `y` to the pixel centers that
/// pass `e`. Returns false when no column does.
bool clipRowToEdge(const TriEdge& e, int y, int64_t& lo, int64_t& hi) {
    const int64_t cy = static_cast<int64_t>(y) * kTriSub + kTriSub / 2;
    // E(px) = c - k * px with c = dx * (cy - ay) - dy * (kTriSub / 2 - ax), k = kTriSub * dy.
    const int64_t c = e.dx * (cy - e.ay) - e.dy * (kTriSub / 2 - e.ax);
    if (e.dy == 0) {
        return c >= e.minE;
    }
    if (e.dy < 0) {
        lo = std::max(lo, ceilDiv(e.minE - c, -kTriSub * e.dy));
    } else {
        hi = std::min(hi, floorDiv(c - e.minE, kTriSub * e.dy));
    }
    return lo <= hi;
}

/// Device -> source mapping of the axis-aligned triangle case along one axis: the device
/// coordinate where the normalized texture coordinate is 0 (`origin`) and the device length of a
/// whole texture (`size`), from the vertex pair farthest apart on that axis -- for a mesh rectangle
/// these are exactly DrawImage's `dst.x`/`dst.w` (or `y`/`h`), so both draw the same bytes.
void axisFromVertices(const double pos[3], const double uv[3], double& origin, double& size) {
    int bi = 0;
    int bj = 1;
    double best = -1.0;
    for (int i = 0; i < 3; ++i) {
        for (int j = i + 1; j < 3; ++j) {
            const double d = std::fabs(pos[j] - pos[i]);
            if (d > best) {
                best = d;
                bi = i;
                bj = j;
            }
        }
    }
    size = (pos[bj] - pos[bi]) / (uv[bj] - uv[bi]);
    origin = pos[bi] - uv[bi] * size;
}

/// T-245: draws one triangle of a DrawImageMesh, affinely textured (no perspective correction).
/// Coverage is binary at pixel centers with the top-left rule (no antialiasing), so the triangles
/// of a mesh tile without seams or double-blended pixels. A triangle whose texture mapping has no
/// rotation/shear (u depends on x only, v on y only) samples through the axis-aligned DrawImage
/// path (sampleAxisSpan); any other triangle maps every pixel center back to (u, v).
void drawAffineTriangle(Target& target, const MeshVertex& m0, const MeshVertex& m1, const MeshVertex& m2,
                        float opacity, bool smooth, const ImageData& image, const uint8_t* clipMask,
                        int clipWidth, ImageScratch& scratch) {
    const MeshVertex* v[3] = {&m0, &m1, &m2};
    for (const MeshVertex* p : v) {
        if (!(std::fabs(p->p.x) <= kTriMaxCoord) || !(std::fabs(p->p.y) <= kTriMaxCoord) ||
            !std::isfinite(p->uv.x) || !std::isfinite(p->uv.y)) {
            return;  // NaN / out-of-range vertex: the triangle is skipped.
        }
    }
    int64_t fx[3];
    int64_t fy[3];
    for (int i = 0; i < 3; ++i) {
        fx[i] = std::llround(static_cast<double>(v[i]->p.x) * static_cast<double>(kTriSub));
        fy[i] = std::llround(static_cast<double>(v[i]->p.y) * static_cast<double>(kTriSub));
    }
    const int64_t area = (fx[1] - fx[0]) * (fy[2] - fy[0]) - (fy[1] - fy[0]) * (fx[2] - fx[0]);
    if (area == 0) {
        return;  // Degenerate (zero area after snapping): nothing to draw.
    }
    if (area < 0) {
        std::swap(v[1], v[2]);
        std::swap(fx[1], fx[2]);
        std::swap(fy[1], fy[2]);
    }
    const TriEdge edges[3] = {makeEdge(fx[0], fy[0], fx[1], fy[1]), makeEdge(fx[1], fy[1], fx[2], fy[2]),
                              makeEdge(fx[2], fy[2], fx[0], fy[0])};

    // Device rows / columns that can hold a covered pixel center, clamped to the target.
    const int64_t minFx = std::min({fx[0], fx[1], fx[2]});
    const int64_t maxFx = std::max({fx[0], fx[1], fx[2]});
    const int64_t minFy = std::min({fy[0], fy[1], fy[2]});
    const int64_t maxFy = std::max({fy[0], fy[1], fy[2]});
    const int64_t colLo = std::max<int64_t>(floorDiv(minFx, kTriSub), target.originX);
    const int64_t colHi = std::min<int64_t>(floorDiv(maxFx, kTriSub), target.originX + target.w - 1);
    const int64_t rowLo = std::max<int64_t>(floorDiv(minFy, kTriSub), target.originY);
    const int64_t rowHi = std::min<int64_t>(floorDiv(maxFy, kTriSub), target.originY + target.h - 1);
    if (colLo > colHi || rowLo > rowHi) {
        return;
    }

    // Device -> normalized texture coordinate (affine), from the unsnapped positions.
    const double px[3] = {v[0]->p.x, v[1]->p.x, v[2]->p.x};
    const double py[3] = {v[0]->p.y, v[1]->p.y, v[2]->p.y};
    const double pu[3] = {v[0]->uv.x, v[1]->uv.x, v[2]->uv.x};
    const double pv[3] = {v[0]->uv.y, v[1]->uv.y, v[2]->uv.y};
    const double e1x = px[1] - px[0], e1y = py[1] - py[0];
    const double e2x = px[2] - px[0], e2y = py[2] - py[0];
    const double det = e1x * e2y - e1y * e2x;
    if (det == 0.0) {
        return;
    }
    const double du1 = pu[1] - pu[0], du2 = pu[2] - pu[0];
    const double dv1 = pv[1] - pv[0], dv2 = pv[2] - pv[0];
    const double dudx = (du1 * e2y - du2 * e1y) / det;
    const double dudy = (du2 * e1x - du1 * e2x) / det;
    const double dvdx = (dv1 * e2y - dv2 * e1y) / det;
    const double dvdy = (dv2 * e1x - dv1 * e2x) / det;
    const bool axisAligned = dudy == 0.0 && dvdx == 0.0 && dudx != 0.0 && dvdy != 0.0;

    const f32x4 alphaMask = alphaLaneMask();
    const f32x4 kv = vsplat(opacity);
    scratch.span.resize(static_cast<size_t>(target.w) * 4);
    float* span = scratch.span.data();

    AxisMap xmAnchor;
    double oy = 0.0;
    double sizeY = 1.0;
    const double offset = smooth ? -0.5 : 0.0;
    Mat2x3 toUv{};
    if (axisAligned) {
        scratch.row.resize(static_cast<size_t>(image.width) * 4);
        double ox = 0.0;
        double sizeX = 1.0;
        axisFromVertices(px, pu, ox, sizeX);
        axisFromVertices(py, pv, oy, sizeY);
        // Anchored at the first column so every span of the triangle steps from the same base
        // (as DrawImage's one span per row does).
        xmAnchor = makeAxisMap(1.0, 0.0, ox, sizeX, 0.0, 1.0, static_cast<double>(image.width), offset,
                               static_cast<int>(colLo));
    } else {
        toUv = Mat2x3{static_cast<float>(dudx),
                      static_cast<float>(dvdx),
                      static_cast<float>(dudy),
                      static_cast<float>(dvdy),
                      static_cast<float>(pu[0] - dudx * px[0] - dudy * py[0]),
                      static_cast<float>(pv[0] - dvdx * px[0] - dvdy * py[0])};
    }

    for (int64_t yy = rowLo; yy <= rowHi; ++yy) {
        const int y = static_cast<int>(yy);
        int64_t lo = colLo;
        int64_t hi = colHi;
        if (!clipRowToEdge(edges[0], y, lo, hi) || !clipRowToEdge(edges[1], y, lo, hi) ||
            !clipRowToEdge(edges[2], y, lo, hi)) {
            continue;
        }
        int x0 = static_cast<int>(lo);
        int x1 = static_cast<int>(hi) + 1;
        DstRow row;
        if (!row.resolve(target, y, x0, x1)) {
            continue;
        }
        const int n = x1 - x0;
        const uint8_t* clipRow = clipMask != nullptr
                                     ? clipMask + static_cast<size_t>(y) * static_cast<size_t>(clipWidth)
                                     : nullptr;
        bool opaqueSrc = false;
        if (axisAligned) {
            const AxisMap ym =
                makeAxisMap(1.0, 0.0, oy, sizeY, 0.0, 1.0, static_cast<double>(image.height), offset, y);
            AxisMap xm = xmAnchor;
            xm.base += xm.step * static_cast<int64_t>(x0 - static_cast<int>(colLo));
            opaqueSrc =
                sampleAxisSpan(image, smooth, xm, ym.at(0), n, kv, alphaMask, span, scratch.row.data());
        } else {
            f32x4 minv = vsplat(1.0f);
            const float cy = static_cast<float>(y) + 0.5f;
            for (int i = 0; i < n; ++i) {
                const Vec2 uv = apply(toUv, Vec2{static_cast<float>(x0 + i) + 0.5f, cy});
                const float su = std::clamp(uv.x, 0.0f, 1.0f);
                const float sv = std::clamp(uv.y, 0.0f, 1.0f);
                const f32x4 s = smooth ? sampleBilinear(image, su, sv) : sampleNearest(image, su, sv);
                minv = simd::min(minv, s);
                simd::store(span + static_cast<size_t>(i) * 4, premulSample(s, kv, alphaMask));
            }
            float mins[4];
            simd::store(mins, minv);
            opaqueSrc = mins[3] >= 1.0f;
        }
        blendSpan(row, x0, n, span, clipRow, opaqueSrc && opacity == 1.0f);
    }
}

}  // namespace

void drawImageMesh(Target& target, const MeshVertex* vertices, uint32_t vertexCount, const uint16_t* indices,
                   uint32_t indexCount, float opacity, uint8_t smooth, const ImageData& image,
                   const uint8_t* clipMask, int clipWidth, ImageScratch& scratch) {
    if (image.width <= 0 || image.height <= 0 || image.rgba.empty() || !(opacity > 0.0f)) {
        return;
    }
    for (uint32_t t = 0; t + 2 < indexCount; t += 3) {
        const uint16_t i0 = indices[t];
        const uint16_t i1 = indices[t + 1];
        const uint16_t i2 = indices[t + 2];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) {
            continue;  // Out-of-range index (never produced by the Painter): skipped.
        }
        drawAffineTriangle(target, vertices[i0], vertices[i1], vertices[i2], opacity, smooth != 0, image,
                           clipMask, clipWidth, scratch);
    }
}

}  // namespace xt::sw

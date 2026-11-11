#include "xt/image_mips.h"

#include <algorithm>
#include <cmath>

#include "xt/asset_resolver.h"

/// T-665: see xt/image_mips.h and Docs/tasks/T-665.md "規格" (with the main session's ruling 1 on
/// the level sizes).

namespace xt {

size_t ImageMipChain::byteSize() const {
    size_t bytes = 0;
    for (const ImageMipLevel& level : levels) {
        bytes += level.rgba.size();
    }
    return bytes;
}

int imageMipLevelCount(int w, int h) {
    if (w <= 0 || h <= 0) {
        return 0;
    }
    int levels = 1;
    while (w > 1 || h > 1) {
        w = w > 1 ? w >> 1 : 1;
        h = h > 1 ? h >> 1 : 1;
        ++levels;
    }
    return levels;
}

float imageMinifyLod(float a, float b, float c, float d, float sx, float sy) {
    const double da = a, db = b, dc = c, dd = d;
    const double det = da * dd - db * dc;
    if (!std::isfinite(det) || det == 0.0 || !std::isfinite(sx) || !std::isfinite(sy)) {
        return 0.0f;
    }
    // J = diag(sx, sy) * inverse([a c; b d]); inverse = [d -c; -b a] / det.
    const double ax = std::fabs(static_cast<double>(sx));
    const double ay = std::fabs(static_cast<double>(sy));
    const double j00 = ax * dd / det;   // d texel.x / d device.x
    const double j10 = -ay * db / det;  // d texel.y / d device.x
    const double j01 = -ax * dc / det;  // d texel.x / d device.y
    const double j11 = ay * da / det;   // d texel.y / d device.y
    const double rho = std::max(std::sqrt(j00 * j00 + j10 * j10), std::sqrt(j01 * j01 + j11 * j11));
    if (!std::isfinite(rho) || rho <= 1.0) {
        return 0.0f;
    }
    const double lod = std::log2(rho);
    return std::isfinite(lod) && lod > 0.0 ? static_cast<float>(lod) : 0.0f;
}

float imageDrawLod(const Mat2x3& deviceXf, const Rect& dst, const Rect& srcUv, int texW, int texH) {
    if (dst.w == 0.0f || dst.h == 0.0f || texW <= 0 || texH <= 0) {
        return 0.0f;
    }
    const float sx = srcUv.w * static_cast<float>(texW) / dst.w;
    const float sy = srcUv.h * static_cast<float>(texH) / dst.h;
    return imageMinifyLod(deviceXf.a, deviceXf.b, deviceXf.c, deviceXf.d, sx, sy);
}

bool imageQuadLod(const Vec2 q[4], const Rect& srcUv, int texW, int texH, float& lod) {
    constexpr float kAffineEps = 1e-3f;
    if (std::fabs(q[0].x + q[2].x - q[1].x - q[3].x) > kAffineEps ||
        std::fabs(q[0].y + q[2].y - q[1].y - q[3].y) > kAffineEps) {
        return false;
    }
    if (texW <= 0 || texH <= 0) {
        lod = 0.0f;
        return true;
    }
    lod = imageMinifyLod(q[1].x - q[0].x, q[1].y - q[0].y, q[3].x - q[0].x, q[3].y - q[0].y,
                         srcUv.w * static_cast<float>(texW), srcUv.h * static_cast<float>(texH));
    return true;
}

void premultiplyRgba8(const uint8_t* src, size_t count, uint8_t* dst) {
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* s = src + i * 4;
        uint8_t* d = dst + i * 4;
        const uint32_t a = s[3];
        const uint8_t r = static_cast<uint8_t>((s[0] * a + 127u) / 255u);
        const uint8_t g = static_cast<uint8_t>((s[1] * a + 127u) / 255u);
        const uint8_t b = static_cast<uint8_t>((s[2] * a + 127u) / 255u);
        d[0] = r;
        d[1] = g;
        d[2] = b;
        d[3] = static_cast<uint8_t>(a);
    }
}

void downsampleBox(const uint8_t* src, int w, int h, uint8_t* dst) {
    const int dw = w > 1 ? w >> 1 : 1;
    const int dh = h > 1 ? h >> 1 : 1;
    const size_t stride = static_cast<size_t>(w) * 4;
    for (int y = 0; y < dh; ++y) {
        // A 1-texel axis averages the texel with itself (the GPU's clamped fetch does the same).
        const int y0 = h > 1 ? 2 * y : 0;
        const int y1 = h > 1 ? 2 * y + 1 : 0;
        const uint8_t* r0 = src + static_cast<size_t>(y0) * stride;
        const uint8_t* r1 = src + static_cast<size_t>(y1) * stride;
        uint8_t* out = dst + static_cast<size_t>(y) * static_cast<size_t>(dw) * 4;
        for (int x = 0; x < dw; ++x) {
            const size_t x0 = static_cast<size_t>(w > 1 ? 2 * x : 0) * 4;
            const size_t x1 = static_cast<size_t>(w > 1 ? 2 * x + 1 : 0) * 4;
            for (size_t k = 0; k < 4; ++k) {
                const uint32_t s = uint32_t{r0[x0 + k]} + r0[x1 + k] + r1[x0 + k] + r1[x1 + k];
                // Round to nearest, ties to even: s / 4 as the GPU stores it into a unorm level.
                const uint32_t q = s >> 2, r = s & 3u;
                out[static_cast<size_t>(x) * 4 + k] =
                    static_cast<uint8_t>(q + ((r > 2u || (r == 2u && (q & 1u) != 0)) ? 1u : 0u));
            }
        }
    }
}

bool buildImageMipChain(const ImageData& image, ImageMipChain& out) {
    out.levels.clear();
    const int w0 = image.width;
    const int h0 = image.height;
    if (w0 <= 0 || h0 <= 0 || image.rgba.size() < static_cast<size_t>(w0) * static_cast<size_t>(h0) * 4) {
        return false;
    }
    const int count = imageMipLevelCount(w0, h0);
    if (count <= 1) {
        return true;
    }
    std::vector<uint8_t> level0(static_cast<size_t>(w0) * static_cast<size_t>(h0) * 4);
    premultiplyRgba8(image.rgba.data(), static_cast<size_t>(w0) * static_cast<size_t>(h0), level0.data());
    out.levels.resize(static_cast<size_t>(count - 1));
    const uint8_t* src = level0.data();
    int w = w0;
    int h = h0;
    for (int k = 1; k < count; ++k) {
        ImageMipLevel& level = out.levels[static_cast<size_t>(k - 1)];
        level.width = w > 1 ? w >> 1 : 1;
        level.height = h > 1 ? h >> 1 : 1;
        level.rgba.resize(static_cast<size_t>(level.width) * static_cast<size_t>(level.height) * 4);
        downsampleBox(src, w, h, level.rgba.data());
        src = level.rgba.data();
        w = level.width;
        h = level.height;
    }
    return true;
}

}  // namespace xt

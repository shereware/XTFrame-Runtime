#include "planar_params.h"

namespace xt::gpu::detail {

PlanarMatrix planarConversion(AssetResolver::ExternalMatrix matrix, AssetResolver::ExternalRange range) {
    const bool bt709 = matrix == AssetResolver::ExternalMatrix::Bt709;
    const double kr = bt709 ? 0.2126 : 0.299;
    const double kb = bt709 ? 0.0722 : 0.114;
    const double kg = 1.0 - kr - kb;
    // Y' = ys * y + yo, C = cs * c + co.
    const bool limited = range == AssetResolver::ExternalRange::Limited;
    const double ys = limited ? 255.0 / 219.0 : 1.0;
    const double yo = limited ? -16.0 / 219.0 : 0.0;
    const double cs = limited ? 255.0 / 224.0 : 1.0;
    const double co = limited ? -128.0 / 224.0 : -128.0 / 255.0;
    const double rv = 2.0 * (1.0 - kr);            // R from Cr
    const double bu = 2.0 * (1.0 - kb);            // B from Cb
    const double gv = 2.0 * kr * (1.0 - kr) / kg;  // G from Cr (subtracted)
    const double gu = 2.0 * kb * (1.0 - kb) / kg;  // G from Cb (subtracted)
    const double rows[3][4] = {
        {ys, 0.0, rv * cs, yo + rv * co},
        {ys, -gu * cs, -gv * cs, yo - (gu + gv) * co},
        {ys, bu * cs, 0.0, yo + bu * co},
    };
    PlanarMatrix out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            out.m[r][c] = static_cast<float>(rows[r][c]);
        }
    }
    return out;
}

ChromaOffset chromaSitingOffset(AssetResolver::ChromaSiting siting) {
    switch (siting) {
        case AssetResolver::ChromaSiting::Center:
            return ChromaOffset{0.0f, 0.0f};
        case AssetResolver::ChromaSiting::Left:
            return ChromaOffset{0.25f, 0.0f};
        case AssetResolver::ChromaSiting::TopLeft:
            return ChromaOffset{0.25f, 0.25f};
    }
    return ChromaOffset{0.25f, 0.0f};
}

namespace {

/// Plane `rect` sampled at luma position `uv * (lumaW, lumaH)`, `step` luma pixels per plane texel
/// (1, or 2 for chroma) and `off` plane texels added: (scaleU, scaleV, offU, offV) and the clamp.
void planeMapping(const AssetResolver::ExternalPlaneRect& rect, double lumaW, double lumaH, double step,
                  ChromaOffset off, double texW, double texH, float uv[4], float clampRect[4]) {
    uv[0] = static_cast<float>(lumaW / step / texW);
    uv[1] = static_cast<float>(lumaH / step / texH);
    uv[2] = static_cast<float>((rect.x + static_cast<double>(off.dx)) / texW);
    uv[3] = static_cast<float>((rect.y + static_cast<double>(off.dy)) / texH);
    clampRect[0] = static_cast<float>((rect.x + 0.5) / texW);
    clampRect[1] = static_cast<float>((rect.y + 0.5) / texH);
    clampRect[2] = static_cast<float>((rect.x + rect.w - 0.5) / texW);
    clampRect[3] = static_cast<float>((rect.y + rect.h - 0.5) / texH);
}

}  // namespace

PlanarParams planarParams(const AssetResolver::ExternalPlanes& planes) {
    PlanarParams p{};
    const double texW = planes.textureWidth > 0 ? planes.textureWidth : 1;
    const double texH = planes.textureHeight > 0 ? planes.textureHeight : 1;
    const double lumaW = planes.y.w, lumaH = planes.y.h;
    const ChromaOffset chroma = chromaSitingOffset(planes.siting);
    planeMapping(planes.y, lumaW, lumaH, 1.0, ChromaOffset{}, texW, texH, p.uv[0], p.clampRect[0]);
    planeMapping(planes.u, lumaW, lumaH, 2.0, chroma, texW, texH, p.uv[1], p.clampRect[1]);
    planeMapping(planes.v, lumaW, lumaH, 2.0, chroma, texW, texH, p.uv[2], p.clampRect[2]);
    const bool alpha = planes.layout == AssetResolver::ExternalLayout::PackedYuva420;
    if (alpha) {
        planeMapping(planes.a, lumaW, lumaH, 1.0, ChromaOffset{}, texW, texH, p.uv[3], p.clampRect[3]);
    }
    const PlanarMatrix m = planarConversion(planes.matrix, planes.range);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            p.matrix[r][c] = m.m[r][c];
        }
    }
    p.hasAlpha = alpha ? 1.0f : 0.0f;
    p.premultiplied = alpha && planes.premultipliedPlanes ? 1.0f : 0.0f;
    return p;
}

}  // namespace xt::gpu::detail

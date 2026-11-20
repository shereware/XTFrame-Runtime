#pragma once

// T-806 (Docs/design/23-host-integration.md §23.aj): the CPU reference of a planar external texture --
// straight RGBA8 pixels turned into the Y'CbCr 4:2:0 planes of one R8 texture, the way a host's video
// decoder would hand them over. Used by `xtframe-view --bind-planar` and by the tests (Tests/gpu,
// Tests/wgpu, Tests/gl); test and tool support, not part of any runtime module's API. Header only.
//
// Layout: Y (w x h) at the top left; U and V (each ceil(w/2) x ceil(h/2)) side by side below it,
// U first, with no gap; A (w x h) below them for PackedYuva420. The texture is max(w, 2 ceil(w/2))
// wide; texels no plane covers are 0.
//
// The conversion is the forward one of §23.aj, written independently of the runtime's matrix:
// Y' = Kr R + Kg G + Kb B, Cb = (B - Y') / (2 (1 - Kb)), Cr = (R - Y') / (2 (1 - Kr)); Limited codes
// 16 + 219 Y' and 128 + 224 C, Full codes 255 Y' and 128 + 255 C, rounded and clamped to 0..255.
// Chroma sample (i, j) takes the colour at the luma position its siting gives (§23.aj: X = 2 (i + 0.5
// - dx), Y = 2 (j + 0.5 - dy)), interpolated bilinearly between the luma pixels (clamped at the edges).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "xt/asset_resolver.h"

namespace xt::tools {

struct PackedPlanes {
    std::vector<uint8_t> texels;  ///< planes.textureWidth x planes.textureHeight R8, row 0 = top
    AssetResolver::ExternalPlanes planes;
};

/// Packs the straight-alpha RGBA8 picture `rgba` (`w` x `h`, rows top-down, tightly packed) as
/// described above. `premultipliedPlanes` (PackedYuva420 only): the colour planes hold the colour
/// multiplied by alpha. `layout` Rgba is taken as PackedYuv420.
inline PackedPlanes packPlanes(const uint8_t* rgba, int w, int h, AssetResolver::ExternalLayout layout,
                               AssetResolver::ExternalMatrix matrix, AssetResolver::ExternalRange range,
                               AssetResolver::ChromaSiting siting, bool premultipliedPlanes = false) {
    using Layout = AssetResolver::ExternalLayout;
    PackedPlanes out;
    if (rgba == nullptr || w <= 0 || h <= 0) {
        return out;
    }
    const bool alpha = layout == Layout::PackedYuva420;
    const bool premul = alpha && premultipliedPlanes;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    AssetResolver::ExternalPlanes& p = out.planes;
    p.layout = alpha ? Layout::PackedYuva420 : Layout::PackedYuv420;
    p.matrix = matrix;
    p.range = range;
    p.siting = siting;
    p.premultipliedPlanes = premul;
    p.textureWidth = std::max(w, 2 * cw);
    p.textureHeight = h + ch + (alpha ? h : 0);
    p.y = {0, 0, w, h};
    p.u = {0, h, cw, ch};
    p.v = {cw, h, cw, ch};
    if (alpha) {
        p.a = {0, h + ch, w, h};
    }
    out.texels.assign(static_cast<size_t>(p.textureWidth) * static_cast<size_t>(p.textureHeight), 0);

    const bool bt709 = matrix == AssetResolver::ExternalMatrix::Bt709;
    const double kr = bt709 ? 0.2126 : 0.299, kb = bt709 ? 0.0722 : 0.114, kg = 1.0 - kr - kb;
    const bool limited = range == AssetResolver::ExternalRange::Limited;
    auto code = [](double v) { return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L)); };
    // Per luma pixel: Y', Cb, Cr of the (possibly premultiplied) colour.
    std::vector<double> yf(static_cast<size_t>(w) * static_cast<size_t>(h));
    std::vector<double> cbf(yf.size()), crf(yf.size());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* px =
                rgba + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
            const double a = premul ? px[3] / 255.0 : 1.0;
            const double r = px[0] / 255.0 * a, g = px[1] / 255.0 * a, b = px[2] / 255.0 * a;
            const double luma = kr * r + kg * g + kb * b;
            const size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
            yf[i] = luma;
            cbf[i] = (b - luma) / (2.0 * (1.0 - kb));
            crf[i] = (r - luma) / (2.0 * (1.0 - kr));
            out.texels[static_cast<size_t>(y) * static_cast<size_t>(p.textureWidth) +
                       static_cast<size_t>(x)] = code(limited ? 16.0 + 219.0 * luma : 255.0 * luma);
            if (alpha) {
                out.texels[static_cast<size_t>(p.a.y + y) * static_cast<size_t>(p.textureWidth) +
                           static_cast<size_t>(x)] = px[3];
            }
        }
    }
    // Bilinear sample of a per-luma-pixel field at luma position (X, Y) (pixel k's centre at k + 0.5).
    auto field = [w, h](const std::vector<double>& f, double X, double Y) {
        const double fx = std::clamp(X - 0.5, 0.0, double(w - 1)),
                     fy = std::clamp(Y - 0.5, 0.0, double(h - 1));
        const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
        const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
        const double tx = fx - x0, ty = fy - y0;
        auto at = [&f, w](int x, int y) {
            return f[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
        };
        return (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) +
               (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
    };
    const double dx = siting == AssetResolver::ChromaSiting::Center ? 0.0 : 0.25;
    const double dy = siting == AssetResolver::ChromaSiting::TopLeft ? 0.25 : 0.0;
    for (int j = 0; j < ch; ++j) {
        for (int i = 0; i < cw; ++i) {
            const double X = 2.0 * (i + 0.5 - dx), Y = 2.0 * (j + 0.5 - dy);
            const double cb = field(cbf, X, Y), cr = field(crf, X, Y);
            const size_t row = static_cast<size_t>(h + j) * static_cast<size_t>(p.textureWidth);
            out.texels[row + static_cast<size_t>(i)] =
                code(limited ? 128.0 + 224.0 * cb : 128.0 + 255.0 * cb);
            out.texels[row + static_cast<size_t>(cw + i)] =
                code(limited ? 128.0 + 224.0 * cr : 128.0 + 255.0 * cr);
        }
    }
    return out;
}

}  // namespace xt::tools

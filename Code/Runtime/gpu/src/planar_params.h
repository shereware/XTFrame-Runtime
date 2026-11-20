#pragma once

#include "xt/asset_resolver.h"
#include "xt/gpu/gpu_frame.h"

/// T-806 (Docs/design/23-host-integration.md §23.aj): the CPU half of `Shading::TexturePlanar` -- the
/// colour conversion matrix of a Y'CbCr matrix and range, the chroma siting offsets and the whole
/// `PlanarParams` block of a plane description. Pure functions: the shaders never branch on the
/// matrix, the range or the siting.

namespace xt::gpu::detail {

/// Three rows (R, G, B) of (cY, cU, cV, offset): `rgb = row . (y, u, v, 1)` over sampled 0..1 plane
/// values, before the clamp to [0, 1]. Limited: Y' = (y - 16/255) * 255/219, C = (c - 128/255) *
/// 255/224; Full: Y' = y, C = c - 128/255. R = Y' + 2 (1 - Kr) Cr, B = Y' + 2 (1 - Kb) Cb,
/// G = (Y' - Kr R - Kb B) / (1 - Kr - Kb); BT.601: Kr 0.299, Kb 0.114; BT.709: Kr 0.2126, Kb 0.0722.
struct PlanarMatrix {
    float m[3][4];
};
PlanarMatrix planarConversion(AssetResolver::ExternalMatrix matrix, AssetResolver::ExternalRange range);

/// The chroma sample grid relative to the luma one, in chroma texels: the chroma continuous
/// coordinate of luma position (X, Y) (pixel k's centre at k + 0.5) is (X / 2 + dx, Y / 2 + dy).
/// Center: (0, 0); Left: (0.25, 0); TopLeft: (0.25, 0.25).
struct ChromaOffset {
    float dx = 0, dy = 0;
};
ChromaOffset chromaSitingOffset(AssetResolver::ChromaSiting siting);

/// The PlanarParams of `planes` (meant for a description externalPlanesValid() accepts): per plane
/// the picture uv -> texture uv mapping (luma size = the Y rectangle's; the chroma offsets folded
/// into U and V) and the plane's rectangle inset by half a texel; the matrix of planarConversion;
/// the two flags. Plane A is all zero without an alpha plane.
PlanarParams planarParams(const AssetResolver::ExternalPlanes& planes);

}  // namespace xt::gpu::detail

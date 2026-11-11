#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "xt/types.h"

/// T-665 (Docs/design/10-backends.md §10.11 item 7, Docs/tasks/T-665.md): the level of detail of a
/// minified image draw and the mip chain it samples, shared by the software renderer and the GPU frame
/// builder so both pick the same levels with the same weights, whatever the driver.
///
/// - LOD: `J` is the 2x2 linear part of the affine map "destination device pixel -> source texel"
///   (level-0 texels); `rho = max(|J column 0|, |J column 1|)`; `lod = max(0, log2(rho))`. A draw
///   with lod 0 samples exactly as before T-665. With lod > 0 the levels floor(lod) and ceil(lod)
///   (clamped to the last level) are each sampled bilinearly and mixed by the fraction (trilinear).
/// - Chain: level k is `max(1, w >> k) x max(1, h >> k)` (hardware mip sizes); level k+1 is the 2x2
///   box average, in premultiplied space, of the level-k texels (2x, 2y)..(2x+1, 2y+1) -- on an odd
///   edge the last row / column of level k is not sampled (main-session ruling 1 of T-665). Level 0
///   is premultiplied with the GPU upload's rounding (premultiplyRgba8) before level 1 is made.

namespace xt {

struct ImageData;

/// One level (k >= 1) of an image's mip chain: premultiplied RGBA8, tightly packed.
struct ImageMipLevel {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

/// The levels 1..n-1 of an image's mip chain (level 0 is the image itself). Empty for a 1x1 image.
struct ImageMipChain {
    /// `levels[k - 1]` is level k.
    std::vector<ImageMipLevel> levels;
    /// T-686 (Docs/design/10-backends.md §10.13 ruling 3): the levels a DDS stored, decoded -- straight
    /// alpha (sampled bilinearly, then premultiplied, as the GPU samples block-compressed levels) and
    /// at their stored size: the image's 0..1 uv covers 0..levelUvScaleX x the level's width (the
    /// logical / padded ratio of a DDS padded to a multiple of 4, ruling 2), likewise vertically.
    /// false / 1 / 1 for a chain built by buildImageMipChain().
    bool straight = false;
    float levelUvScaleX = 1.0f;
    float levelUvScaleY = 1.0f;
    /// The bytes the levels hold (what a cache counts against its budget).
    size_t byteSize() const;
};

/// The number of levels of a full chain of a `w` x `h` image (1 for 1x1; 0 for an empty size).
int imageMipLevelCount(int w, int h);

/// `lod = max(0, log2(rho))` for the map whose source-texel-per-device-pixel Jacobian is
/// `J = diag(sx, sy) * inverse([a c; b d])`, where `[a c; b d]` is the linear part of the
/// "image unit square or local space -> device" transform and `sx`, `sy` scale that space to level-0
/// texels. 0 for a singular or non-finite map. Computed in double precision.
float imageMinifyLod(float a, float b, float c, float d, float sx, float sy);

/// The lod of a DrawImage of `dst` (local space) sampling `srcUv` of a `texW` x `texH` texture under
/// the (non-projective) `deviceXf`.
float imageDrawLod(const Mat2x3& deviceXf, const Rect& dst, const Rect& srcUv, int texW, int texH);

/// The lod of an image quad `q` (device corners clockwise from the uv origin) sampling `srcUv` of a
/// `texW` x `texH` texture, when `q` is a parallelogram (within 1e-3 px); false (lod untouched) for a
/// projective quad, which keeps the single-level sampling.
bool imageQuadLod(const Vec2 q[4], const Rect& srcUv, int texW, int texH, float& lod);

/// Straight -> premultiplied RGBA8 exactly as the GPU frame builder uploads images:
/// `c' = (c * a + 127) / 255`, alpha kept. `count` pixels; `dst` may equal `src`.
void premultiplyRgba8(const uint8_t* src, size_t count, uint8_t* dst);

/// Builds levels 1..n-1 of `image` (straight RGBA8) into `out` (replacing its content). False, with
/// `out` empty, for an image without pixels.
bool buildImageMipChain(const ImageData& image, ImageMipChain& out);

/// One box step of the chain: `dst` (`max(1, w >> 1)` x `max(1, h >> 1)`) from `src` (`w` x `h`),
/// both premultiplied RGBA8, `s / 4` rounded to nearest with ties to even (as a GPU stores the mean into
/// a unorm level: the GPU chain then stays within one step of this one at every level).
void downsampleBox(const uint8_t* src, int w, int h, uint8_t* dst);

}  // namespace xt

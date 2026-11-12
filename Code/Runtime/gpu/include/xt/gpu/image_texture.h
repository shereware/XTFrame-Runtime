#pragma once

#include <cstddef>
#include <cstdint>

#include "xt/asset_resolver.h"
#include "xt/gpu/gpu_device.h"
#include "xt/ir.h"

/// T-688 (Docs/design/50-c-abi.md §50.ae): the image half of `FrameBuilder`'s texture cache -- an image
/// asset, its content key and the texture of that content on a `GpuDevice` (found among the device's
/// shared images, or created and uploaded) -- without a builder or a surface. `FrameBuilder` draws
/// through it (its own per-builder cache, retry and refusal bookkeeping wrap it), and the C ABI's
/// `xt_gpu_target_preload_ui` / `xt_gpu_target_preload_image` put images into a GPU target's shared
/// image cache with it, so a preloaded image is the very texture (same key, size and format) a later
/// draw looks for.

namespace xt::gpu {

/// T-648: the content key of `bytes` bytes at `data` (an image's straight RGBA8 pixels or its compressed
/// blocks) -- four xxh64-style lanes over 8-byte words, folded into two 64-bit halves.
ImageKey imageContentKey(const uint8_t* data, size_t bytes);

/// The draw path's own lookup before a key is computed (T-263c ruling 1: the cells of one atlas page share
/// the texture the builder already holds for that page). Not needed outside a builder.
class ImageTextureLookup {
public:
    /// The texture already held for the resolver's image `image` (`width` x `height`), or 0; `straight`
    /// receives whether it holds straight-alpha blocks.
    virtual TextureHandle find(const void* image, int width, int height, bool& straight) = 0;

protected:
    ~ImageTextureLookup() = default;
};

/// What `acquireImageTexture` is asked for: asset `asset` of `ir`, as `assets` resolves it at `scale` (the
/// variant choice of AssetResolver::imageView(asset, view, dpr): `@3x` at >= 2.5, `@2x` at >= 1.5).
struct ImageTextureRequest {
    GpuDevice* device = nullptr;
    AssetResolver* assets = nullptr;
    const IrView* ir = nullptr;
    AssetId asset = kInvalidAsset;
    float scale = 1.0f;
    /// GpuDevice::maxTextureSize() (0 = unknown, no limit).
    uint32_t maxTexture = 0;
    /// Optional (see ImageTextureLookup); asked with the compressed blocks, then with the decoded pixels.
    ImageTextureLookup* lookup = nullptr;
};

/// What `acquireImageTexture` found or made.
struct ImageTextureResult {
    enum class Outcome : uint8_t {
        Ok,            ///< `tex` is the image's texture
        DecodeFailed,  ///< the resolver has no pixels for it (missing or undecodable file)
        BadSize,       ///< the decoded image is empty or larger than 65535 px
        OverLimit,     ///< larger than `maxTexture` (`width` x `height` say how large)
        CreateFailed,  ///< the device could not create the texture
    };
    Outcome outcome = Outcome::DecodeFailed;
    /// A handle on the device: on a sharing device (GpuDevice::sharesImageTextures) one use of the shared
    /// texture, ended by releaseSharedImage; else a texture of its own. 0 unless Ok.
    TextureHandle tex = 0;
    /// The texture holds straight-alpha blocks (a compressed DDS; Shading::TextureStraight).
    bool straight = false;
    /// The texture was created by this call (an upload); false when it was found.
    bool created = false;
    /// `tex` came from ImageTextureRequest::lookup (it is the builder's, not a new use).
    bool fromLookup = false;
    /// The resolver's image pointer the texture was made from or found for (nullptr when it was found by
    /// the resolver's retained key without the pixels, T-657).
    const void* image = nullptr;
    /// The compressed image `tex` holds, when it holds one this call read (nullptr otherwise).
    const CompressedImage* compressed = nullptr;
    /// The texture's (stored) size, and T-686 the image's logical size (0 = the stored size).
    int width = 0;
    int height = 0;
    int logicalWidth = 0;
    int logicalHeight = 0;
};

/// The texture of `r.asset`, in the order the draw path has always taken: on a sharing device the key the
/// resolver retained (T-657) finds the shared texture without the pixels; else the compressed blocks
/// (T-199) -- `lookup`, then the shared texture of their key, else one created from them; a device that
/// cannot take them falls through -- then the decoded pixels (T-148) -- `lookup`, the shared texture of
/// their key, else premultiplied and created. On a sharing device the key of what was found or created is
/// retained with the resolver (T-657). No mip chain is made here (GpuDevice::requestMipChain is the
/// caller's, on the first minified draw).
ImageTextureResult acquireImageTexture(const ImageTextureRequest& r);

}  // namespace xt::gpu

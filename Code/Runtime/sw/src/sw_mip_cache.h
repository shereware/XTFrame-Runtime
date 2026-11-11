#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "xt/asset_resolver.h"
#include "xt/image_mips.h"

/// T-665 (Docs/design/10-backends.md §10.11 item 7; main-session ruling 2): the mip chains the
/// software renderer builds itself for images whose resolver returns none from
/// AssetResolver::imageMipChain(). Keyed and dropped the way gpu::FrameBuilder keys the textures of
/// borrowed images: by source (the resolver and the IR's IrView::loadSerial()), asset id, variant and
/// the borrowed image (address and size) under an epoch; entries go with invalidateAsset(),
/// invalidateAll(), clear() (the renderer's releaseScratch() and destruction) and when their source
/// falls out of the kMaxSources most recently rendered ones.

namespace xt::sw {

class SwMipCache {
public:
    /// Sources kept besides those the render in progress uses (gpu::FrameBuilder's kMaxAssetSources).
    static constexpr size_t kMaxSources = 2;

    /// The chain of `image` (what assets->imageView(asset, ir, scale) returned): the resolver's own,
    /// else one built and kept here; nullptr when the image must keep its single level (the asset's
    /// GPU form is block-compressed without a stored chain, so the GPU samples one level too).
    const ImageMipChain* chainFor(AssetResolver* assets, const IrView& ir, AssetId asset, float scale,
                                  const ImageData& image);
    /// Ends one SwRenderer::render(): sources beyond kMaxSources that it did not use go, with their
    /// chains.
    void endRender();
    /// Drops the chains of `asset` (under the IR `source` only, when non-null).
    void invalidateAsset(AssetId asset, const IrView* source);
    /// Drops every chain (a new IR or resolver may reuse an image address).
    void invalidateAll();
    /// Frees everything (releaseScratch, shutdown).
    void clear();
    /// Bytes of the chains held now (tests).
    size_t bytes() const;
    size_t entries() const { return m_entries.size(); }

private:
    struct Source {
        const AssetResolver* assets = nullptr;
        uint32_t irSerial = 0;
        uint64_t lastRender = 0;
    };
    struct Entry {
        const AssetResolver* assets = nullptr;
        uint32_t irSerial = 0;
        AssetId asset = kInvalidAsset;
        uint8_t variant = 0;
        uint32_t epoch = 0;
        const ImageData* image = nullptr;
        int width = 0;
        int height = 0;
        ImageMipChain chain;
    };
    std::vector<Source> m_sources;
    std::vector<Entry> m_entries;
    uint64_t m_render = 1;
    uint32_t m_epoch = 0;
};

}  // namespace xt::sw

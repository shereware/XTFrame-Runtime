#include "sw_mip_cache.h"

#include <algorithm>

#include "xt/sw/sw_png.h"

/// T-665, T-835: see sw_mip_cache.h.

namespace xt::sw {

namespace {

/// The variant key of a host's CPU pixels (images use 0..2, the @1x/@2x/@3x choice).
constexpr uint8_t kExternalVariant = 0xFF;

bool hasPixels(const ImageData& image) {
    return image.width > 0 && image.height > 0 &&
           image.rgba.size() >= static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4;
}

/// `out` = `image` premultiplied with the GPU upload's rounding (xt::premultiplyRgba8).
void premultiplyInto(const ImageData& image, ImageData& out) {
    const size_t count = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
    out.width = image.width;
    out.height = image.height;
    out.rgba.resize(count * 4);
    premultiplyRgba8(image.rgba.data(), count, out.rgba.data());
}

}  // namespace

SwMipCache::Entry& SwMipCache::entryFor(AssetResolver* assets, const IrView& ir, AssetId asset,
                                        uint8_t variant, const ImageData& image) {
    const uint32_t serial = ir.loadSerial();
    bool known = false;
    for (Source& s : m_sources) {
        if (s.assets == assets && s.irSerial == serial) {
            s.lastRender = m_render;
            known = true;
            break;
        }
    }
    if (!known) {
        m_sources.push_back(Source{assets, serial, m_render});
    }
    for (Entry& e : m_entries) {
        if (e.assets == assets && e.irSerial == serial && e.asset == asset && e.variant == variant &&
            e.epoch == m_epoch && e.image == &image && e.width == image.width && e.height == image.height) {
            return e;
        }
    }
    // A stale entry of the same asset (another decode, address or size) goes before the new one.
    std::erase_if(m_entries, [&](const Entry& e) {
        return e.assets == assets && e.irSerial == serial && e.asset == asset && e.variant == variant;
    });
    Entry e;
    e.assets = assets;
    e.irSerial = serial;
    e.asset = asset;
    e.variant = variant;
    e.epoch = m_epoch;
    e.image = &image;
    e.width = image.width;
    e.height = image.height;
    m_entries.push_back(std::move(e));
    return m_entries.back();
}

SwImageLevels SwMipCache::levelsFor(AssetResolver* assets, const IrView& ir, AssetId asset, float scale,
                                    const ImageData& image, bool wantChain) {
    // Ruling to interpretation 4: an asset whose GPU form is block-compressed without a stored chain
    // stays single-level on the GPU, so here too. T-686 (10 §10.13 ruling 3): one with a stored chain
    // samples those levels (decoded below when the resolver keeps none), never a chain built here.
    // T-835 (amendment item 3): either way the GPU filters a block-compressed level 0 straight, so
    // there is no premultiplied level 0.
    const CompressedImage* compressed = assets->compressedImage(asset, ir, scale);
    if (compressed != nullptr && compressed->levels <= 1) {
        return {};
    }
    const ImageMipChain* own = assets->imageMipChain(asset, ir, scale);
    if (compressed != nullptr && (own != nullptr || !wantChain)) {
        return SwImageLevels{wantChain ? own : nullptr, nullptr};
    }
    if (!hasPixels(image)) {
        return {};
    }
    const uint8_t variant = scale >= 2.5f ? 2 : scale >= 1.5f ? 1 : 0;
    Entry& e = entryFor(assets, ir, asset, variant, image);
    SwImageLevels out;
    if (compressed == nullptr) {
        if (e.premultiplied.rgba.empty()) {
            premultiplyInto(image, e.premultiplied);
        }
        out.premultiplied = &e.premultiplied;
    }
    if (!wantChain) {
        return out;
    }
    if (own != nullptr) {
        out.mips = own;
        return out;
    }
    if (e.chainState == 0) {
        const bool ok = compressed != nullptr
                            ? decodeStoredMipLevels(*compressed, decodeImageBytes, e.chain)
                            : buildImageMipChainPremultiplied(e.premultiplied.rgba.data(), image.width,
                                                              image.height, e.chain);
        e.chainState = ok ? 1 : 2;
        if (!ok) {
            e.chain = ImageMipChain{};
        }
    }
    out.mips = e.chainState == 1 ? &e.chain : nullptr;
    return out;
}

const ImageMipChain* SwMipCache::chainFor(AssetResolver* assets, const IrView& ir, AssetId asset, float scale,
                                          const ImageData& image) {
    return levelsFor(assets, ir, asset, scale, image, true).mips;
}

const ImageData* SwMipCache::externalPremultiplied(AssetResolver* assets, const IrView& ir, AssetId asset,
                                                   const AssetResolver::ExternalTexture& ext) {
    if (ext.pixels == nullptr || ext.straightAlpha || !hasPixels(*ext.pixels)) {
        return nullptr;
    }
    const ImageData& image = *ext.pixels;
    Entry& e = entryFor(assets, ir, asset, kExternalVariant, image);
    const uint64_t stamp = externalTextureStamp(&ext);
    // A versioned binding changes its pixels only with its stamp; an unversioned one may rewrite them
    // in place before any render, so its copy is made again once per render.
    if (e.premultiplied.rgba.empty() || e.stamp != stamp || (!ext.hasVersion && e.builtRender != m_render)) {
        premultiplyInto(image, e.premultiplied);
        e.stamp = stamp;
        e.builtRender = m_render;
    }
    return &e.premultiplied;
}

void SwMipCache::endRender() {
    size_t usedNow = 0;
    for (const Source& s : m_sources) {
        usedNow += s.lastRender == m_render ? 1 : 0;
    }
    while (m_sources.size() > std::max(kMaxSources, usedNow)) {
        auto oldest =
            std::min_element(m_sources.begin(), m_sources.end(),
                             [](const Source& a, const Source& b) { return a.lastRender < b.lastRender; });
        const Source gone = *oldest;
        m_sources.erase(oldest);
        std::erase_if(m_entries,
                      [&](const Entry& e) { return e.assets == gone.assets && e.irSerial == gone.irSerial; });
    }
    ++m_render;
}

void SwMipCache::invalidateAsset(AssetId asset, const IrView* source) {
    std::erase_if(m_entries, [&](const Entry& e) {
        return e.asset == asset && (source == nullptr || e.irSerial == source->loadSerial());
    });
}

void SwMipCache::invalidateAll() {
    m_entries.clear();
    ++m_epoch;
}

void SwMipCache::clear() {
    m_entries.clear();
    m_entries.shrink_to_fit();
    m_sources.clear();
}

size_t SwMipCache::bytes() const {
    size_t total = 0;
    for (const Entry& e : m_entries) {
        total += e.chain.byteSize() + e.premultiplied.rgba.size();
    }
    return total;
}

}  // namespace xt::sw

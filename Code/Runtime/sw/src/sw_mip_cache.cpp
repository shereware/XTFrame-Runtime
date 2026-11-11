#include "sw_mip_cache.h"

#include <algorithm>

/// T-665: see sw_mip_cache.h.

namespace xt::sw {

const ImageMipChain* SwMipCache::chainFor(AssetResolver* assets, const IrView& ir, AssetId asset, float scale,
                                          const ImageData& image) {
    // Ruling to interpretation 4: an asset whose GPU form is block-compressed without a stored chain
    // stays single-level on the GPU, so here too.
    if (const CompressedImage* compressed = assets->compressedImage(asset, ir, scale)) {
        if (compressed->levels <= 1) {
            return nullptr;
        }
    }
    if (const ImageMipChain* own = assets->imageMipChain(asset, ir, scale)) {
        return own;
    }
    const uint32_t serial = ir.loadSerial();
    const uint8_t variant = scale >= 2.5f ? 2 : scale >= 1.5f ? 1 : 0;
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
            return &e.chain;
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
    if (!buildImageMipChain(image, e.chain)) {
        return nullptr;
    }
    m_entries.push_back(std::move(e));
    return &m_entries.back().chain;
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
        total += e.chain.byteSize();
    }
    return total;
}

}  // namespace xt::sw

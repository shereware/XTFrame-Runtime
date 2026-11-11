#include "xt/wgpu/wgpu_shared.h"

#include <webgpu/webgpu.h>

#include <algorithm>
#include <cstddef>

#include "wgpu_internal.h"

/// WgpuSharedCache (T-648): see wgpu_shared.h. The device side (borrowing the pipeline set,
/// registering and releasing image uses) is in wgpu_device.cpp.

namespace xt::wgpu {

WgpuSharedCache::WgpuSharedCache() : m_impl(std::make_unique<Impl>()) {}

WgpuSharedCache::~WgpuSharedCache() { shutdown(); }

void WgpuSharedCache::shutdown() {
    Impl& c = *m_impl;
    (void)c.destroyPools();  // T-675: every device is shut down, so nothing is borrowed
    detail::releaseTexture(c.dummy, c.dummyView);
    c.frameBorrowPeak = 0;
    c.transientBudget = detail::kPoolBudgetHeadroomBytes;
    for (Impl::Image& image : c.images) {
        c.destroyImage(image);
    }
    c.images.clear();
    c.freeIds.clear();
    c.idle.clear();
    if (c.pipelinesReady) {
        c.pipelines.shutdown();
        c.pipelinesReady = false;
    }
    c.probed.clear();
    c.stats.imageTextures = 0;
    c.stats.imagesInUse = 0;
    c.stats.imageBytes = 0;
    c.stats.imageIdleTextures = 0;
    c.stats.imageIdleBytes = 0;
    c.releaseDeviceIfUnused();
}

WgpuSharedStats WgpuSharedCache::stats() const {
    WgpuSharedStats s = m_impl->stats;
    m_impl->poolStats(s);
    return s;
}

void WgpuSharedCache::setImageIdleBudget(uint64_t bytes) {
    m_impl->idleBudget = bytes;
    m_impl->applyIdleBudget();
}

uint64_t WgpuSharedCache::imageIdleBudget() const { return m_impl->idleBudget; }

uint32_t WgpuSharedCache::trimIdleImages() {
    Impl& c = *m_impl;
    uint32_t n = 0;
    while (!c.idle.empty()) {
        c.evictIdle(c.idle.front());
        ++n;
    }
    return n;
}

// -- T-675 (Docs/design/50-c-abi.md §50.ac item 2): the target-level pools. --

WGPUTexture WgpuSharedCache::acquireSurfaceTexture(int width, int height, WGPUTextureFormat format,
                                                   WGPUTextureView& view) {
    Impl& c = *m_impl;
    view = nullptr;
    if (c.device == nullptr || width <= 0 || height <= 0) {
        return nullptr;
    }
    const auto w = static_cast<uint32_t>(width), h = static_cast<uint32_t>(height);
    for (size_t i = 0; i < c.surfaceIdle.size(); ++i) {
        const Impl::SurfaceTexture& e = c.surfaceIdle[i];
        if (e.width == w && e.height == h && e.format == format) {
            WGPUTexture texture = e.texture;
            view = e.view;
            c.surfaceIdle.erase(c.surfaceIdle.begin() + static_cast<std::ptrdiff_t>(i));
            return texture;
        }
    }
    WGPUTexture texture = detail::makeTexture(
        c.device, "xt_capi surface", w, h, format, 1,
        WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_TextureBinding);
    if (texture == nullptr) {
        return nullptr;
    }
    ++c.stats.textureCreates;
    view = detail::makeView(texture);
    if (view == nullptr) {
        detail::releaseTexture(texture, view);
        return nullptr;
    }
    return texture;
}

void WgpuSharedCache::releaseSurfaceTexture(WGPUTexture texture, WGPUTextureView view) {
    if (texture == nullptr) {
        if (view != nullptr) wgpuTextureViewRelease(view);
        return;
    }
    Impl& c = *m_impl;
    Impl::SurfaceTexture e;
    e.width = wgpuTextureGetWidth(texture);
    e.height = wgpuTextureGetHeight(texture);
    e.format = wgpuTextureGetFormat(texture);
    e.texture = texture;
    e.view = view != nullptr ? view : detail::makeView(texture);
    e.idleSince = c.frame;
    e.bytes = uint64_t(e.width) * e.height * bytesPerTexel(e.format);
    c.surfaceIdle.push_back(e);
}

void WgpuSharedCache::endFrame() { m_impl->agePools(); }

uint32_t WgpuSharedCache::trimPools() { return m_impl->destroyPools(); }

bool WgpuSharedCache::Impl::ensureDummy(WGPUQueue queue) {
    if (dummyView != nullptr) {
        return true;
    }
    if (device == nullptr || queue == nullptr) {
        return false;
    }
    dummy = detail::makeTexture(device, "xt_wgpu dummy", 1, 1, WGPUTextureFormat_RGBA8Unorm, 1,
                                WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);
    if (dummy == nullptr) {
        return false;
    }
    ++stats.textureCreates;
    dummyView = detail::makeView(dummy);
    if (dummyView == nullptr) {
        detail::releaseTexture(dummy, dummyView);
        return false;
    }
    const uint8_t zero[4] = {0, 0, 0, 0};
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = dummy;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = 4;
    layout.rowsPerImage = 1;
    const WGPUExtent3D one{1, 1, 1};
    wgpuQueueWriteTexture(queue, &dst, zero, 4, &layout, &one);
    return true;
}

int WgpuSharedCache::Impl::borrowAttachments(uint32_t width, uint32_t height, uint32_t samples,
                                             WGPUTextureFormat format) {
    int empty = -1;
    for (size_t i = 0; i < attachments.size(); ++i) {
        AttachmentSet& a = attachments[i];
        if (a.stencil == nullptr) {
            if (empty < 0) empty = static_cast<int>(i);
            continue;
        }
        if (!a.borrowed && a.width == width && a.height == height && a.samples == samples &&
            a.format == format) {
            a.borrowed = true;
            a.lastUsed = frame;
            return static_cast<int>(i);
        }
    }
    if (device == nullptr || width == 0 || height == 0) {
        return -1;
    }
    AttachmentSet a;
    a.width = width;
    a.height = height;
    a.samples = samples;
    a.format = format;
    if (samples > 1) {
        a.msaa = detail::makeTexture(device, "xt_wgpu surface msaa", width, height, format, samples,
                                     WGPUTextureUsage_RenderAttachment);
        if (a.msaa != nullptr) ++stats.textureCreates;
        a.msaaView = detail::makeView(a.msaa);
    }
    a.stencil = detail::makeTexture(device, "xt_wgpu surface stencil", width, height,
                                    WGPUTextureFormat_Stencil8, samples, WGPUTextureUsage_RenderAttachment);
    if (a.stencil != nullptr) ++stats.textureCreates;
    a.stencilView = detail::makeView(a.stencil);
    if (a.stencilView == nullptr || (samples > 1 && a.msaaView == nullptr)) {
        detail::releaseTexture(a.msaa, a.msaaView);
        detail::releaseTexture(a.stencil, a.stencilView);
        return -1;
    }
    const uint64_t texels = uint64_t(width) * height * samples;
    // T-678: the stencil as the backend creates a Stencil8 (stencilTexelBytes).
    a.bytes = texels * stencilTexelBytes + (samples > 1 ? texels * bytesPerTexel(format) : 0u);
    a.borrowed = true;
    a.lastUsed = frame;
    if (empty >= 0) {
        attachments[static_cast<size_t>(empty)] = a;
        return empty;
    }
    attachments.push_back(a);
    return static_cast<int>(attachments.size() - 1);
}

void WgpuSharedCache::Impl::returnAttachments(int index) {
    if (index < 0 || static_cast<size_t>(index) >= attachments.size()) {
        return;
    }
    AttachmentSet& a = attachments[static_cast<size_t>(index)];
    a.borrowed = false;
    a.lastUsed = frame;
}

void WgpuSharedCache::Impl::destroyTransient(WgpuDevice::Pooled& p) {
    if (!p.live()) {
        return;
    }
    stats.poolFrees += 1u + (p.resolve != nullptr ? 1u : 0u) + (p.depthStencil != nullptr ? 1u : 0u);
    WgpuDevice::releasePooled(p);
    p = WgpuDevice::Pooled{};
    ++viewGeneration;
}

void WgpuSharedCache::Impl::trimCachedFree() {
    while (cachedFreeBytes > detail::kCachedFreeBudgetBytes && !cachedFree.empty()) {
        size_t oldest = 0;
        for (size_t i = 1; i < cachedFree.size(); ++i) {
            if (cachedFree[i].releasedFrame < cachedFree[oldest].releasedFrame) oldest = i;
        }
        WgpuDevice::Texture t = cachedFree[oldest].texture;
        cachedFree.erase(cachedFree.begin() + static_cast<std::ptrdiff_t>(oldest));
        cachedFreeBytes -= uint64_t(t.width) * t.height * 4u;
        WgpuDevice::releaseTextureEntry(t);
        ++stats.poolFrees;
        ++viewGeneration;
    }
}

void WgpuSharedCache::Impl::applyTransientBudget() {
    transientBudget = frameBorrowPeak + detail::kPoolBudgetHeadroomBytes;
    frameBorrowPeak = 0;
    uint64_t live = 0;
    size_t liveCount = 0;
    for (const WgpuDevice::Pooled& p : transients) {
        if (!p.live()) continue;
        live += p.bytes;
        ++liveCount;
    }
    while (live > transientBudget && liveCount > 1) {
        size_t lru = transients.size();
        for (size_t i = 0; i < transients.size(); ++i) {
            if (transients[i].live() &&
                (lru == transients.size() || transients[i].lastUsed < transients[lru].lastUsed))
                lru = i;
        }
        if (transients[lru].lastUsed == frame) break;  // everything left was used in the frame that ended
        live -= transients[lru].bytes;
        --liveCount;
        destroyTransient(transients[lru]);  // the devices flush their bind groups by viewGeneration
    }
}

void WgpuSharedCache::Impl::agePools() {
    applyTransientBudget();  // T-678: before the clock moves, so the frame that ended is "now"
    ++frame;
    for (size_t i = 0; i < surfaceIdle.size();) {
        if (frame - surfaceIdle[i].idleSince > detail::kSurfacePoolIdleFrames) {
            detail::releaseTexture(surfaceIdle[i].texture, surfaceIdle[i].view);
            surfaceIdle.erase(surfaceIdle.begin() + static_cast<std::ptrdiff_t>(i));
            ++stats.poolFrees;
        } else {
            ++i;
        }
    }
    for (AttachmentSet& a : attachments) {
        if (a.stencil != nullptr && !a.borrowed && frame - a.lastUsed > detail::kSurfacePoolIdleFrames) {
            stats.poolFrees += a.msaa != nullptr ? 2u : 1u;
            detail::releaseTexture(a.msaa, a.msaaView);
            detail::releaseTexture(a.stencil, a.stencilView);
            a = AttachmentSet{};
        }
    }
    // The device rule (gl_device.cpp, T-163): at most one idle transient goes per frame; the same for
    // the released cached targets.
    for (WgpuDevice::Pooled& p : transients) {
        if (p.live() && frame - p.lastUsed > detail::kTransientIdleFrames) {
            destroyTransient(p);
            break;
        }
    }
    for (size_t i = 0; i < cachedFree.size(); ++i) {
        if (frame - cachedFree[i].releasedFrame > detail::kTransientIdleFrames) {
            WgpuDevice::Texture t = cachedFree[i].texture;
            cachedFreeBytes -= uint64_t(t.width) * t.height * 4u;
            cachedFree.erase(cachedFree.begin() + static_cast<std::ptrdiff_t>(i));
            WgpuDevice::releaseTextureEntry(t);
            ++stats.poolFrees;
            ++viewGeneration;
            break;
        }
    }
}

uint32_t WgpuSharedCache::Impl::destroyPools() {
    const uint32_t before = stats.poolFrees;
    for (WgpuDevice::Pooled& p : transients) {
        destroyTransient(p);
    }
    transients.clear();
    for (WgpuDevice::FreeCached& f : cachedFree) {
        WgpuDevice::releaseTextureEntry(f.texture);
        ++stats.poolFrees;
    }
    if (!cachedFree.empty()) ++viewGeneration;
    cachedFree.clear();
    cachedFreeBytes = 0;
    for (AttachmentSet& a : attachments) {
        if (a.stencil == nullptr || a.borrowed) continue;
        stats.poolFrees += a.msaa != nullptr ? 2u : 1u;
        detail::releaseTexture(a.msaa, a.msaaView);
        detail::releaseTexture(a.stencil, a.stencilView);
        a = AttachmentSet{};
    }
    while (!attachments.empty() && attachments.back().stencil == nullptr) attachments.pop_back();
    for (SurfaceTexture& e : surfaceIdle) {
        detail::releaseTexture(e.texture, e.view);
        ++stats.poolFrees;
    }
    surfaceIdle.clear();
    return stats.poolFrees - before;
}

void WgpuSharedCache::Impl::poolStats(WgpuSharedStats& out) const {
    out.poolTextures = 0;
    out.poolBytes = 0;
    out.attachmentSets = 0;
    out.attachmentBytes = 0;
    out.surfaceTexturesIdle = static_cast<uint32_t>(surfaceIdle.size());
    for (const WgpuDevice::Pooled& p : transients) {
        if (!p.live()) continue;
        out.poolTextures += 1u + (p.resolve != nullptr ? 1u : 0u) + (p.depthStencil != nullptr ? 1u : 0u);
        out.poolBytes += p.bytes;
    }
    out.poolTextures += static_cast<uint32_t>(cachedFree.size());
    out.poolBytes += cachedFreeBytes;
    for (const AttachmentSet& a : attachments) {
        if (a.stencil == nullptr) continue;
        ++out.attachmentSets;
        out.attachmentBytes += a.bytes;
        out.poolTextures += a.msaa != nullptr ? 2u : 1u;
        out.poolBytes += a.bytes;
    }
    for (const SurfaceTexture& e : surfaceIdle) {
        ++out.poolTextures;
        out.poolBytes += e.bytes;
    }
}

bool WgpuSharedCache::Impl::adopt(WGPUDevice dev) {
    if (dev == nullptr) {
        return false;
    }
    if (device == nullptr) {
        wgpuDeviceAddRef(dev);
        device = dev;
    }
    return device == dev;
}

uint32_t WgpuSharedCache::Impl::findImage(const gpu::ImageKey& key, uint16_t w, uint16_t h,
                                          gpu::TextureFormat fmt) const {
    for (size_t i = 0; i < images.size(); ++i) {
        const Image& image = images[i];
        if (image.view != nullptr && image.key == key && image.width == w && image.height == h &&
            image.format == fmt) {
            return static_cast<uint32_t>(i + 1);
        }
    }
    return 0;
}

uint32_t WgpuSharedCache::Impl::addImage(const gpu::ImageKey& key, uint16_t w, uint16_t h,
                                         gpu::TextureFormat fmt, WGPUTexture texture, WGPUTextureView view,
                                         uint64_t bytes) {
    Image image;
    image.key = key;
    image.width = w;
    image.height = h;
    image.format = fmt;
    image.texture = texture;
    image.view = view;
    image.uses = 1;
    image.bytes = bytes;
    ++stats.imageCreates;
    ++stats.imageTextures;
    ++stats.imagesInUse;
    stats.imageBytes += bytes;
    if (!freeIds.empty()) {
        const uint32_t id = freeIds.back();
        freeIds.pop_back();
        images[id - 1] = image;
        return id;
    }
    images.push_back(image);
    return static_cast<uint32_t>(images.size());
}

void WgpuSharedCache::Impl::useImage(uint32_t id) {
    Image& image = images[id - 1];
    if (image.uses++ == 0) {
        ++stats.imagesInUse;
        leaveIdle(id);  // T-656: found again while idle (no upload)
    }
    ++stats.imageReuses;
}

void WgpuSharedCache::Impl::releaseImage(uint32_t id, bool keep) {
    if (id == 0 || id > images.size() || images[id - 1].view == nullptr) {
        return;
    }
    Image& image = images[id - 1];
    if (image.uses > 0) {
        if (--image.uses > 0) {
            return;
        }
        --stats.imagesInUse;
    }
    if (!keep) {
        destroyImage(image);  // also takes it off the idle list
        ++stats.imageFrees;
        freeIds.push_back(id);
        return;
    }
    if (!image.idle) {
        // T-656 (50 §50.ab item 1): kept, but only within the idle budget.
        image.idle = true;
        idle.push_back(id);
        ++stats.imageIdleTextures;
        stats.imageIdleBytes += image.bytes;
        applyIdleBudget();
    }
}

void WgpuSharedCache::Impl::leaveIdle(uint32_t id) {
    Image& image = images[id - 1];
    if (!image.idle) {
        return;
    }
    image.idle = false;
    const auto it = std::find(idle.begin(), idle.end(), id);
    if (it != idle.end()) {
        idle.erase(it);
    }
    if (stats.imageIdleTextures > 0) {
        --stats.imageIdleTextures;
    }
    stats.imageIdleBytes -= image.bytes <= stats.imageIdleBytes ? image.bytes : stats.imageIdleBytes;
}

void WgpuSharedCache::Impl::evictIdle(uint32_t id) {
    leaveIdle(id);
    destroyImage(images[id - 1]);
    ++stats.imageFrees;
    ++stats.imageEvictions;
    freeIds.push_back(id);
}

void WgpuSharedCache::Impl::applyIdleBudget() {
    while (stats.imageIdleBytes > idleBudget && !idle.empty()) {
        evictIdle(idle.front());
    }
}

void WgpuSharedCache::Impl::destroyImage(Image& image) {
    if (image.view == nullptr) {
        return;
    }
    if (image.idle) {
        leaveIdle(static_cast<uint32_t>(&image - images.data()) + 1u);
    }
    if (image.uses > 0 && stats.imagesInUse > 0) {
        --stats.imagesInUse;
    }
    if (stats.imageTextures > 0) {
        --stats.imageTextures;
    }
    stats.imageBytes -= image.bytes <= stats.imageBytes ? image.bytes : stats.imageBytes;
    wgpuTextureViewRelease(image.view);
    if (image.texture != nullptr) {
        wgpuTextureDestroy(image.texture);
        wgpuTextureRelease(image.texture);
    }
    image = Image{};
}

void WgpuSharedCache::Impl::releaseDeviceIfUnused() {
    if (device != nullptr && stats.devices == 0 && !pipelinesReady && stats.imageTextures == 0 &&
        dummy == nullptr && transients.empty() && cachedFree.empty() && attachments.empty() &&
        surfaceIdle.empty()) {
        wgpuDeviceRelease(device);
        device = nullptr;
    }
}

}  // namespace xt::wgpu

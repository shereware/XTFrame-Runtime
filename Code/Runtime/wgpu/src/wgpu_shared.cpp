#include "xt/wgpu/wgpu_shared.h"

#include <webgpu/webgpu.h>

#include "wgpu_internal.h"

/// WgpuSharedCache (T-648): see wgpu_shared.h. The device side (borrowing the pipeline set,
/// registering and releasing image uses) is in wgpu_device.cpp.

namespace xt::wgpu {

WgpuSharedCache::WgpuSharedCache() : m_impl(std::make_unique<Impl>()) {}

WgpuSharedCache::~WgpuSharedCache() { shutdown(); }

void WgpuSharedCache::shutdown() {
    Impl& c = *m_impl;
    for (Impl::Image& image : c.images) {
        c.destroyImage(image);
    }
    c.images.clear();
    c.freeIds.clear();
    if (c.pipelinesReady) {
        c.pipelines.shutdown();
        c.pipelinesReady = false;
    }
    c.probed.clear();
    c.stats.imageTextures = 0;
    c.stats.imagesInUse = 0;
    c.stats.imageBytes = 0;
    c.releaseDeviceIfUnused();
}

WgpuSharedStats WgpuSharedCache::stats() const { return m_impl->stats; }

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

void WgpuSharedCache::Impl::releaseImage(uint32_t id, bool keep) {
    if (id == 0 || id > images.size() || images[id - 1].view == nullptr) {
        return;
    }
    Image& image = images[id - 1];
    if (image.uses > 0 && --image.uses == 0) {
        --stats.imagesInUse;
    }
    if (image.uses == 0 && !keep) {
        destroyImage(image);
        ++stats.imageFrees;
        freeIds.push_back(id);
    }
}

void WgpuSharedCache::Impl::destroyImage(Image& image) {
    if (image.view == nullptr) {
        return;
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
    if (device != nullptr && stats.devices == 0 && !pipelinesReady && stats.imageTextures == 0) {
        wgpuDeviceRelease(device);
        device = nullptr;
    }
}

}  // namespace xt::wgpu

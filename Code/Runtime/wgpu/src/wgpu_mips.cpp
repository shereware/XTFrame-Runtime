#include <webgpu/webgpu.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include "wgpu_internal.h"
#include "xt/image_mips.h"
#include "xt/wgpu/wgpu_device.h"

/// T-665 (Docs/design/10-backends.md §10.11 item 7, Docs/tasks/T-665.md 規格 3): WgpuDevice's mip
/// chains for minified images -- a texture with every level replaces the one-level image texture.

namespace xt::wgpu {

using detail::makeTexture;
using detail::makeView;
using detail::releaseTexture;

// -- T-665 mip chains (Docs/design/10-backends.md §10.11 item 7, Docs/tasks/T-665.md 規格 3). --

namespace {

WGPUTextureView levelView(WGPUTexture texture, uint32_t level) {
    WGPUTextureViewDescriptor d = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    d.baseMipLevel = level;
    d.mipLevelCount = 1;
    d.baseArrayLayer = 0;
    d.arrayLayerCount = 1;
    return wgpuTextureCreateView(texture, &d);
}

}  // namespace

uint32_t WgpuDevice::requestMipChain(gpu::TextureHandle tex, const void* storedLevels,
                                     uint32_t storedLevelCount) {
    const auto start = std::chrono::steady_clock::now();
    const uint32_t made = m_mipChains;
    const uint32_t levels = makeMipChain(tex, storedLevels, storedLevelCount);
    if (m_mipChains != made) {
        m_mipMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    return levels;
}

uint32_t WgpuDevice::makeMipChain(gpu::TextureHandle tex, const void* storedLevels,
                                  uint32_t storedLevelCount) {
    const Texture* found = find(tex);
    if (m_device == nullptr || found == nullptr || found->cached) {
        return 1;
    }
    Texture& t = m_textures[tex - 1];
    WgpuSharedCache::Impl::Image* shared = nullptr;
    if (t.sharedId != 0) {
        if (m_sharing == nullptr || m_sharing->m_impl->images[t.sharedId - 1].texture == nullptr) {
            return 1;
        }
        shared = &m_sharing->m_impl->images[t.sharedId - 1];
    } else if (t.external || t.texture == nullptr) {
        return 1;  // a host view (registerExternalTexture)
    }
    if (shared != nullptr ? shared->levels > 1 : t.levels > 1) {
        if (shared != nullptr) {
            t.texture = shared->texture;  // this entry was made before another device made the chain
            t.view = shared->view;
            t.levels = shared->levels;
        }
        return t.levels;
    }
    uint32_t levels = static_cast<uint32_t>(imageMipLevelCount(t.width, t.height));
    const bool compressed = detail::isCompressedFormat(t.format);
    if (compressed) {
        if (storedLevels == nullptr || storedLevelCount < 2) {
            return 1;  // 規格 3: no chain is made for block-compressed images
        }
        levels = std::min(levels, storedLevelCount);
    } else if (t.format != gpu::TextureFormat::Rgba8) {
        return 1;
    }
    if (levels <= 1) {
        return 1;
    }
    const WGPUTextureUsage usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst |
                                   WGPUTextureUsage_CopySrc |
                                   (compressed ? WGPUTextureUsage_None : WGPUTextureUsage_RenderAttachment);
    WGPUTexture next = makeTexture(m_device, "xt image (mips)", t.width, t.height, detail::formatOf(t.format),
                                   1, usage, levels);
    if (next == nullptr) {
        return 1;
    }
    countTextureCreate();
    WGPUTexture old = shared != nullptr ? shared->texture : t.texture;
    WGPUCommandEncoderDescriptor ed = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(m_device, &ed);
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = old;
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = next;
    const WGPUExtent3D size0{t.width, t.height, 1};
    wgpuCommandEncoderCopyTextureToTexture(enc, &src, &dst, &size0);
    uint64_t extraBytes = 0;
    WGPUBuffer zeros = nullptr;
    std::vector<WGPUTextureView> views;
    std::vector<WGPUBindGroup> groups;
    if (compressed) {
        // The stored levels 1.., block rows back to back (DDS order), at their physical (block) size.
        const uint8_t* p = static_cast<const uint8_t*>(storedLevels);
        for (uint32_t k = 1; k < levels; ++k) {
            const uint32_t lw = std::max(1u, uint32_t(t.width) >> k),
                           lh = std::max(1u, uint32_t(t.height) >> k);
            const uint32_t bw = (lw + 3u) / 4u, bh = (lh + 3u) / 4u;
            WGPUTexelCopyTextureInfo lvl = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            lvl.texture = next;
            lvl.mipLevel = k;
            WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
            layout.bytesPerRow = bw * detail::formatUnitBytes(t.format);
            layout.rowsPerImage = bh;
            const WGPUExtent3D size{bw * 4u, bh * 4u, 1};
            const size_t bytes = size_t(layout.bytesPerRow) * bh;
            wgpuQueueWriteTexture(m_queue, &lvl, p, bytes, &layout, &size);
            p += bytes;
            extraBytes += bytes;
        }
    } else {
        detail::Pipelines& pipes = *m_gpu->pipelines;
        WGPURenderPipeline pipeline = pipes.mipBox();
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.size = 16;
        bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        zeros = wgpuDeviceCreateBuffer(m_device, &bd);
        if (pipeline == nullptr || zeros == nullptr) {
            wgpuCommandEncoderRelease(enc);
            if (zeros != nullptr) wgpuBufferRelease(zeros);
            WGPUTextureView none = nullptr;
            releaseTexture(next, none);
            return 1;
        }
        const float zero[4] = {0, 0, 0, 0};
        wgpuQueueWriteBuffer(m_queue, zeros, 0, zero, sizeof(zero));
        for (uint32_t k = 0; k < levels; ++k) {
            views.push_back(levelView(next, k));
        }
        for (uint32_t k = 1; k < levels; ++k) {
            WGPUBindGroupEntry e[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
            e[0].binding = 0;
            e[0].buffer = zeros;
            e[0].size = 16;
            e[1].binding = 1;
            e[1].textureView = views[k - 1];
            WGPUBindGroupDescriptor gd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            gd.layout = pipes.blitLayout();
            gd.entryCount = 2;
            gd.entries = e;
            groups.push_back(wgpuDeviceCreateBindGroup(m_device, &gd));
            WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
            ca.view = views[k];
            ca.loadOp = WGPULoadOp_Clear;
            ca.storeOp = WGPUStoreOp_Store;
            ca.clearValue = WGPUColor{0, 0, 0, 0};
            WGPURenderPassDescriptor pd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            pd.colorAttachmentCount = 1;
            pd.colorAttachments = &ca;
            WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &pd);
            wgpuRenderPassEncoderSetPipeline(pass, pipeline);
            const uint32_t noOffset = 0;  // the blit layout's uniform is dynamic
            wgpuRenderPassEncoderSetBindGroup(pass, 0, groups.back(), 1, &noOffset);
            wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(pass);
            wgpuRenderPassEncoderRelease(pass);
            const uint64_t lw = std::max(1u, uint32_t(t.width) >> k),
                           lh = std::max(1u, uint32_t(t.height) >> k);
            extraBytes += lw * lh * 4u;
        }
    }
    WGPUCommandBufferDescriptor cd = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    WGPUCommandBuffer cmds = wgpuCommandEncoderFinish(enc, &cd);
    wgpuQueueSubmit(m_queue, 1, &cmds);
    wgpuCommandBufferRelease(cmds);
    wgpuCommandEncoderRelease(enc);
    for (WGPUBindGroup g : groups) wgpuBindGroupRelease(g);
    for (WGPUTextureView v : views) wgpuTextureViewRelease(v);
    if (zeros != nullptr) wgpuBufferRelease(zeros);
    ++m_mipChains;
    m_mipBytes += extraBytes;

    // The new texture takes the old one's place behind the same handle(s).
    m_gpu->releaseBindGroups();  // this device's cached bind groups may name the old view
    if (shared != nullptr) {
        WgpuSharedCache::Impl& c = *m_sharing->m_impl;
        releaseTexture(shared->texture, shared->view);
        shared->texture = next;
        shared->view = makeView(next);
        shared->levels = levels;
        shared->bytes += extraBytes;
        c.stats.imageBytes += extraBytes;
        ++c.viewGeneration;  // the other devices flush their bind groups before their next submit
        m_seenViewGeneration = c.viewGeneration;
        t.texture = shared->texture;
        t.view = shared->view;
    } else {
        releaseTexture(t.texture, t.view);
        t.texture = next;
        t.view = makeView(next);
    }
    t.levels = levels;
    return levels;
}

}  // namespace xt::wgpu

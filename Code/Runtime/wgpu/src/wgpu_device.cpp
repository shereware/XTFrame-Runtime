#include "xt/wgpu/wgpu_device.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

#include "wgpu_internal.h"

/// WgpuDevice (T-437 skeleton, T-438 submit): see wgpu_device.h. Docs/design/10-backends.md
/// §10.10.2 for the mapping; gl_device.cpp is the behaviour reference (windows, scissors, copies,
/// pool budget, cached targets). Command encoding lives in wgpu_draw.cpp.

namespace xt::wgpu {

using detail::label;

namespace {

/// gl_device.cpp kTransientIdleFrames (T-163): an entry idle this long goes, at most one per frame.
constexpr uint32_t kTransientIdleFrames = 1800;
/// gl_device.cpp kPoolBudgetFloorBytes; resize raises it to two full-surface draw targets.
constexpr uint64_t kPoolBudgetFloorBytes = 64ull * 1024u * 1024u;
/// gl_device.cpp kCachedFreeBudgetBytes.
constexpr uint64_t kCachedFreeBudgetBytes = 16ull * 1024u * 1024u;
/// The zero block at the start of the uniform buffer (one parameter binding).
constexpr size_t kZeroBlockFloats = detail::kParamBlockBytes / sizeof(float);

bool isCompressed(gpu::TextureFormat fmt) {
    return fmt == gpu::TextureFormat::Bc1 || fmt == gpu::TextureFormat::Bc3 || fmt == gpu::TextureFormat::Bc7;
}

/// Bytes per texel (uncompressed) or per 4x4 block (compressed).
uint32_t unitBytes(gpu::TextureFormat fmt) {
    switch (fmt) {
        case gpu::TextureFormat::Rgba8:
        case gpu::TextureFormat::R32F:
            return 4;
        case gpu::TextureFormat::R8:
            return 1;
        case gpu::TextureFormat::Bc1:
            return 8;
        case gpu::TextureFormat::Bc3:
        case gpu::TextureFormat::Bc7:
            return 16;
    }
    return 0;
}

WGPUTexture makeTexture(WGPUDevice device, const char* name, uint32_t w, uint32_t h, WGPUTextureFormat format,
                        uint32_t samples, WGPUTextureUsage usage) {
    WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
    d.label = label(name);
    d.usage = usage;
    d.dimension = WGPUTextureDimension_2D;
    d.size = WGPUExtent3D{w, h, 1};
    d.format = format;
    d.mipLevelCount = 1;
    d.sampleCount = samples;
    return wgpuDeviceCreateTexture(device, &d);
}

WGPUTextureView makeView(WGPUTexture t) { return t != nullptr ? wgpuTextureCreateView(t, nullptr) : nullptr; }

void releaseTexture(WGPUTexture& t, WGPUTextureView& v) {
    if (v != nullptr) {
        wgpuTextureViewRelease(v);
        v = nullptr;
    }
    if (t != nullptr) {
        wgpuTextureDestroy(t);
        wgpuTextureRelease(t);
        t = nullptr;
    }
}

uint32_t roundUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

/// How many parameter floats a command's shader reads (gpu_frame.h's *Params sizes); 0 = none.
uint32_t paramFloats(const gpu::DrawCmd& cmd) {
    using gpu::EffectProgram;
    using gpu::Shading;
    if (cmd.kind == gpu::DrawKind::Particles) {
        return sizeof(gpu::ParticleParams) / sizeof(float);
    }
    switch (cmd.shading) {
        case Shading::Gradient:
            return sizeof(gpu::GradientParams) / sizeof(float);
        case Shading::Glyph:
            return sizeof(gpu::GlyphParams) / sizeof(float);
        case Shading::Composite:
            return sizeof(gpu::CompositeParams) / sizeof(float);
        case Shading::Effect:
            switch (cmd.program) {
                case EffectProgram::GaussScalar:
                case EffectProgram::GaussColor:
                    return sizeof(gpu::GaussParams) / sizeof(float);
                case EffectProgram::Dilate:
                    return sizeof(gpu::DilateParams) / sizeof(float);
                case EffectProgram::AlphaExtract:
                    return sizeof(gpu::AlphaExtractParams) / sizeof(float);
                case EffectProgram::Sdf:
                    return sizeof(gpu::SdfParams) / sizeof(float);
                case EffectProgram::ColorMatrix:
                    return sizeof(gpu::ColorMatrixParams) / sizeof(float);
                case EffectProgram::GradientMask:
                    return sizeof(gpu::GradientMaskParams) / sizeof(float);
                case EffectProgram::Shine:
                    return sizeof(gpu::ShineParams) / sizeof(float);
                case EffectProgram::ShadowGlow:
                    return sizeof(gpu::ShadowGlowParams) / sizeof(float);
                case EffectProgram::Outline:
                    return sizeof(gpu::OutlineParams) / sizeof(float);
                case EffectProgram::MaskApply:
                    return sizeof(gpu::MaskApplyParams) / sizeof(float);
                case EffectProgram::Custom:
                    return sizeof(gpu::CustomEffectParams) / sizeof(float);
                case EffectProgram::Downsample:
                    return sizeof(gpu::DownsampleParams) / sizeof(float);
                case EffectProgram::Kawase:
                    return sizeof(gpu::KawaseParams) / sizeof(float);
                case EffectProgram::LevelMix:
                    return sizeof(gpu::LevelMixParams) / sizeof(float);
                case EffectProgram::Backdrop:
                case EffectProgram::None:
                default:
                    return 0;
            }
        default:
            return 0;
    }
}

}  // namespace

uint32_t bytesPerTexel(WGPUTextureFormat format) {
    switch (format) {
        case WGPUTextureFormat_RGBA8Unorm:
        case WGPUTextureFormat_RGBA8UnormSrgb:
        case WGPUTextureFormat_BGRA8Unorm:
        case WGPUTextureFormat_BGRA8UnormSrgb:
        case WGPUTextureFormat_R32Float:
            return 4;
        case WGPUTextureFormat_R8Unorm:
            return 1;
        default:
            return 0;
    }
}

WgpuDevice::WgpuDevice() = default;

WgpuDevice::~WgpuDevice() { shutdown(); }

bool WgpuDevice::init(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat targetFormat,
                      uint32_t sampleCount, std::string& err) {
    shutdown();
    if (device == nullptr || queue == nullptr) {
        err = "WgpuDevice::init: no device or queue";
        return false;
    }
    if (sampleCount != 1 && sampleCount != 4) {
        err = "WgpuDevice::init: sample count " + std::to_string(sampleCount) + " (WebGPU allows 1 or 4)";
        return false;
    }
    if (bytesPerTexel(targetFormat) != 4 || targetFormat == WGPUTextureFormat_R32Float) {
        err = "WgpuDevice::init: the target format must be RGBA8 or BGRA8 (unorm or sRGB)";
        return false;
    }
    wgpuDeviceAddRef(device);
    wgpuQueueAddRef(queue);
    m_device = device;
    m_queue = queue;
    m_targetFormat = targetFormat;
    m_sampleCount = sampleCount;
    WGPULimits limits = WGPU_LIMITS_INIT;
    m_maxTextureSize =
        wgpuDeviceGetLimits(device, &limits) == WGPUStatus_Success ? limits.maxTextureDimension2D : 0;
    m_hasBc = wgpuDeviceHasFeature(device, WGPUFeatureName_TextureCompressionBC) != 0;
    m_gpu = std::make_unique<Gpu>();
    if (!m_gpu->pipelines.init(device, err)) {
        shutdown();
        return false;
    }
    // T-456: one minimal pipeline first, so a backend whose shader compiler fails (T-440: a D3Dcompiler_47
    // without ps_5_1) is an init failure the host can fall back from, not a panic at submit.
    if (!m_gpu->pipelines.probe(targetFormat, sampleCount, err)) {
        err = "WgpuDevice::init: " + err;
        shutdown();
        return false;
    }
    m_gpu->dummy = makeTexture(device, "xt_wgpu dummy", 1, 1, WGPUTextureFormat_RGBA8Unorm, 1,
                               WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);
    m_gpu->dummyView = makeView(m_gpu->dummy);
    if (m_gpu->dummyView == nullptr) {
        err = "WgpuDevice::init: texture creation failed";
        shutdown();
        return false;
    }
    const uint8_t zero[4] = {0, 0, 0, 0};
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = m_gpu->dummy;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = 4;
    layout.rowsPerImage = 1;
    const WGPUExtent3D one{1, 1, 1};
    wgpuQueueWriteTexture(queue, &dst, zero, 4, &layout, &one);
    m_poolBudget = kPoolBudgetFloorBytes;
    m_lastError.clear();
    return true;
}

void WgpuDevice::setProbeShaderForTesting(const char* wgsl) { detail::setProbeShaderOverride(wgsl); }

void WgpuDevice::shutdown() {
    if (m_gpu) {
        m_gpu->releaseBindGroups();
        if (m_gpu->passGroup != nullptr) wgpuBindGroupRelease(m_gpu->passGroup);
        if (m_gpu->paramGroup != nullptr) wgpuBindGroupRelease(m_gpu->paramGroup);
        m_gpu->uniforms.release();
        m_gpu->geometry.shutdown();
        m_gpu->pipelines.shutdown();
        releaseTexture(m_gpu->dummy, m_gpu->dummyView);
        m_gpu.reset();
    }
    for (Texture& t : m_textures) {
        releaseTextureEntry(t);
    }
    m_textures.clear();
    m_freeTextures.clear();
    for (FreeCached& f : m_cachedFree) {
        releaseTextureEntry(f.texture);
    }
    m_cachedFree.clear();
    m_cachedFreeBytes = 0;
    for (Pooled& p : m_pool) {
        releasePooled(p);
    }
    m_pool.clear();
    m_frameTargets.clear();
    releaseSurfaceAttachments();
    if (m_queue != nullptr) {
        wgpuQueueRelease(m_queue);
        m_queue = nullptr;
    }
    if (m_device != nullptr) {
        wgpuDeviceRelease(m_device);
        m_device = nullptr;
    }
    m_surface = nullptr;
}

void WgpuDevice::resize(int width, int height) {
    m_width = width > 0 ? width : 0;
    m_height = height > 0 ? height : 0;
    // gl_device.cpp resize: two full-surface draw targets (color and stencil per sample plus the
    // resolve), or the floor.
    const uint64_t s = m_sampleCount;
    const uint64_t entry = uint64_t(m_width) * uint64_t(m_height) * (s * 4u + s + (s > 1 ? 4u : 0u));
    m_poolBudget = std::max(kPoolBudgetFloorBytes, 2u * entry);
    // Cached targets are window-sized: a resize invalidates every size (the builder flushes).
    if (m_gpu && !m_cachedFree.empty()) m_gpu->releaseBindGroups();
    for (FreeCached& f : m_cachedFree) {
        releaseTextureEntry(f.texture);
    }
    m_cachedFree.clear();
    m_cachedFreeBytes = 0;
}

void WgpuDevice::setSurfaceClear(bool enabled, const WGPUColor& color) {
    m_clearSurface = enabled;
    m_clearColor = color;
}

// -------------------------------------------------------------------------------------------
// Textures.
// -------------------------------------------------------------------------------------------

const WgpuDevice::Texture* WgpuDevice::find(gpu::TextureHandle tex) const {
    if (tex == 0 || tex > m_textures.size()) {
        return nullptr;
    }
    const Texture& t = m_textures[tex - 1];
    return t.view != nullptr ? &t : nullptr;
}

WGPUTexture WgpuDevice::texture(gpu::TextureHandle tex) const {
    const Texture* t = find(tex);
    return t != nullptr ? t->texture : nullptr;
}

void WgpuDevice::releaseTextureEntry(Texture& t) {
    if (!t.external) {
        releaseTexture(t.texture, t.view);
    }
    t = Texture{};
}

gpu::TextureHandle WgpuDevice::addTexture(const Texture& t) {
    if (!m_freeTextures.empty()) {
        const uint32_t handle = m_freeTextures.back();
        m_freeTextures.pop_back();
        m_textures[handle - 1] = t;
        return handle;
    }
    m_textures.push_back(t);
    return static_cast<uint32_t>(m_textures.size());
}

gpu::TextureHandle WgpuDevice::createTexture(const char* debugName, uint16_t w, uint16_t h,
                                             gpu::TextureFormat fmt, const void* initial) {
    if (m_device == nullptr || w == 0 || h == 0 || fmt == gpu::TextureFormat::R32F) {
        return 0;  // R32F: transient targets only (gl_device.cpp)
    }
    if (m_maxTextureSize != 0 && (w > m_maxTextureSize || h > m_maxTextureSize)) {
        return 0;
    }
    const bool compressed = isCompressed(fmt);
    if (compressed && (!m_hasBc || (w % 4) != 0 || (h % 4) != 0)) {
        return 0;  // the builder falls back to the decoded Rgba8 image
    }
    WGPUTexture texture =
        makeTexture(m_device, debugName, w, h, detail::formatOf(fmt), 1,
                    WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc);
    if (texture == nullptr) {
        return 0;
    }
    Texture t;
    t.texture = texture;
    t.view = makeView(texture);
    t.format = fmt;
    t.width = w;
    t.height = h;
    const uint32_t handle = addTexture(t);
    if (initial != nullptr) {
        if (compressed) {
            const uint32_t bw = (w + 3u) / 4u, bh = (h + 3u) / 4u;
            WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            dst.texture = texture;
            WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
            layout.bytesPerRow = bw * unitBytes(fmt);
            layout.rowsPerImage = bh;
            const WGPUExtent3D size{w, h, 1};
            wgpuQueueWriteTexture(m_queue, &dst, initial, static_cast<size_t>(layout.bytesPerRow) * bh,
                                  &layout, &size);
        } else {
            updateTexture(handle, 0, 0, w, h, initial, static_cast<size_t>(w) * unitBytes(fmt));
        }
    }
    return handle;
}

void WgpuDevice::updateTexture(gpu::TextureHandle tex, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                               const void* data, size_t rowPitch) {
    const Texture* t = find(tex);
    if (t == nullptr || t->texture == nullptr || data == nullptr || w == 0 || h == 0 ||
        isCompressed(t->format)) {
        return;
    }
    if (static_cast<uint32_t>(x) + w > t->width || static_cast<uint32_t>(y) + h > t->height) {
        m_lastError = "updateTexture: region outside the texture";
        return;
    }
    const size_t rowBytes = static_cast<size_t>(w) * unitBytes(t->format);
    if (rowPitch < rowBytes) {
        m_lastError = "updateTexture: row pitch smaller than a row";
        return;
    }
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = t->texture;
    dst.origin = WGPUOrigin3D{x, y, 0};
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = static_cast<uint32_t>(rowPitch);
    layout.rowsPerImage = h;
    const WGPUExtent3D size{w, h, 1};
    wgpuQueueWriteTexture(m_queue, &dst, data, rowPitch * (h - 1u) + rowBytes, &layout, &size);
}

void WgpuDevice::destroyTexture(gpu::TextureHandle tex) {
    if (find(tex) == nullptr) {
        return;
    }
    m_gpu->releaseBindGroups();  // the cached bind groups may name its view
    releaseTextureEntry(m_textures[tex - 1]);
    m_freeTextures.push_back(tex);
}

gpu::TextureHandle WgpuDevice::registerExternalTexture(WGPUTextureView view, uint16_t width,
                                                       uint16_t height) {
    if (view == nullptr) {
        return 0;
    }
    Texture t;
    t.view = view;
    t.width = width;
    t.height = height;
    t.external = true;
    return addTexture(t);
}

void WgpuDevice::unregisterExternalTexture(gpu::TextureHandle tex) {
    const Texture* t = find(tex);
    if (t != nullptr && t->external) {
        destroyTexture(tex);
    }
}

gpu::TextureHandle WgpuDevice::acquireCachedTarget(uint16_t w, uint16_t h, gpu::TextureFormat fmt) {
    if (m_device == nullptr || fmt != gpu::TextureFormat::Rgba8 || w == 0 || h == 0) {
        return 0;
    }
    // T-163: a released target of the same size is reused as it is (the store overwrites it).
    for (size_t i = 0; i < m_cachedFree.size(); ++i) {
        if (m_cachedFree[i].texture.width != w || m_cachedFree[i].texture.height != h) continue;
        const Texture t = m_cachedFree[i].texture;
        m_cachedFreeBytes -= uint64_t(w) * h * 4u;
        m_cachedFree.erase(m_cachedFree.begin() + static_cast<std::ptrdiff_t>(i));
        return addTexture(t);
    }
    Texture t;
    t.texture =
        makeTexture(m_device, "xt_wgpu cached target", w, h, WGPUTextureFormat_RGBA8Unorm, 1,
                    WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc);
    t.view = makeView(t.texture);
    if (t.view == nullptr) {
        releaseTexture(t.texture, t.view);
        return 0;
    }
    t.width = w;
    t.height = h;
    t.cached = true;
    return addTexture(t);
}

void WgpuDevice::releaseCachedTarget(gpu::TextureHandle target) {
    const Texture* t = find(target);
    if (t == nullptr || !t->cached) {
        return;
    }
    m_cachedFree.push_back(FreeCached{*t, m_frameCounter});
    m_cachedFreeBytes += uint64_t(t->width) * t->height * 4u;
    m_textures[target - 1] = Texture{};
    m_freeTextures.push_back(target);
    trimCachedFree();
}

void WgpuDevice::trimCachedFree() {
    while (m_cachedFreeBytes > kCachedFreeBudgetBytes && !m_cachedFree.empty()) {
        size_t oldest = 0;
        for (size_t i = 1; i < m_cachedFree.size(); ++i) {
            if (m_cachedFree[i].releasedFrame < m_cachedFree[oldest].releasedFrame) oldest = i;
        }
        Texture t = m_cachedFree[oldest].texture;
        m_cachedFree.erase(m_cachedFree.begin() + static_cast<std::ptrdiff_t>(oldest));
        m_cachedFreeBytes -= uint64_t(t.width) * t.height * 4u;
        if (m_gpu) m_gpu->releaseBindGroups();
        releaseTextureEntry(t);
    }
}

uint32_t WgpuDevice::prepareCustomEffect(std::string_view glsl, const char* names, const uint8_t* types,
                                         uint32_t count) {
    if (!m_gpu) {
        return 0;
    }
    std::string warning;
    const uint32_t id = m_gpu->pipelines.prepareCustom(glsl, names, types, count, warning);
    if (!warning.empty()) {
        if (!m_warning.empty()) {
            m_warning += "\n";
        }
        m_warning += warning;
    }
    return id;
}

// -------------------------------------------------------------------------------------------
// Targets.
// -------------------------------------------------------------------------------------------

void WgpuDevice::releasePooled(Pooled& p) {
    releaseTexture(p.color, p.colorView);
    releaseTexture(p.resolve, p.resolveView);
    releaseTexture(p.depthStencil, p.depthStencilView);
}

void WgpuDevice::releaseSurfaceAttachments() {
    releaseTexture(m_surfaceMsaa, m_surfaceMsaaView);
    releaseTexture(m_surfaceStencil, m_surfaceStencilView);
    m_surfaceAttachW = m_surfaceAttachH = 0;
}

bool WgpuDevice::ensureSurfaceAttachments() {
    if (m_width <= 0 || m_height <= 0) {
        return false;
    }
    if (m_surfaceStencil != nullptr && m_surfaceAttachW == m_width && m_surfaceAttachH == m_height) {
        return true;
    }
    releaseSurfaceAttachments();
    const uint32_t w = static_cast<uint32_t>(m_width), h = static_cast<uint32_t>(m_height);
    if (m_sampleCount > 1) {
        m_surfaceMsaa = makeTexture(m_device, "xt_wgpu surface msaa", w, h, m_targetFormat, m_sampleCount,
                                    WGPUTextureUsage_RenderAttachment);
        m_surfaceMsaaView = makeView(m_surfaceMsaa);
    }
    m_surfaceStencil = makeTexture(m_device, "xt_wgpu surface stencil", w, h, WGPUTextureFormat_Stencil8,
                                   m_sampleCount, WGPUTextureUsage_RenderAttachment);
    m_surfaceStencilView = makeView(m_surfaceStencil);
    m_surfaceAttachW = m_width;
    m_surfaceAttachH = m_height;
    return m_surfaceStencilView != nullptr && (m_sampleCount == 1 || m_surfaceMsaaView != nullptr);
}

int WgpuDevice::acquirePooled(const gpu::TransientTargetDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        return -1;
    }
    // gl_device.cpp acquireDrawTarget: only an Rgba8 target that needs stencil is multisampled and
    // gets a stencil attachment (R32Float cannot be multisampled in WebGPU anyway).
    const bool stencil = desc.needStencil != 0 && desc.format == gpu::TextureFormat::Rgba8;
    const uint32_t samples = stencil ? m_sampleCount : 1u;
    for (size_t i = 0; i < m_pool.size(); ++i) {
        Pooled& p = m_pool[i];
        if (!p.inUse && p.width == desc.width && p.height == desc.height && p.format == desc.format &&
            p.samples == samples && p.stencil == stencil) {
            p.inUse = true;
            p.lastUsed = m_frameCounter;
            return static_cast<int>(i);
        }
    }
    Pooled p;
    p.width = desc.width;
    p.height = desc.height;
    p.format = desc.format;
    p.samples = samples;
    p.stencil = stencil;
    const WGPUTextureFormat format = detail::formatOf(desc.format);
    const WGPUTextureUsage sampled = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding |
                                     WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
    if (samples > 1) {
        p.color = makeTexture(m_device, "xt_wgpu transient msaa", desc.width, desc.height, format, samples,
                              WGPUTextureUsage_RenderAttachment);
        p.resolve = makeTexture(m_device, "xt_wgpu transient", desc.width, desc.height, format, 1, sampled);
        p.resolveView = makeView(p.resolve);
    } else {
        p.color = makeTexture(m_device, "xt_wgpu transient", desc.width, desc.height, format, 1, sampled);
    }
    p.colorView = makeView(p.color);
    if (stencil) {
        p.depthStencil = makeTexture(m_device, "xt_wgpu transient stencil", desc.width, desc.height,
                                     WGPUTextureFormat_Stencil8, samples, WGPUTextureUsage_RenderAttachment);
        p.depthStencilView = makeView(p.depthStencil);
    }
    if (p.colorView == nullptr || (samples > 1 && p.resolveView == nullptr) ||
        (stencil && p.depthStencilView == nullptr)) {
        releasePooled(p);
        return -1;
    }
    const uint64_t px = uint64_t(desc.width) * desc.height;
    p.bytes = px * (samples * 4u + (samples > 1 ? 4u : 0u) + (stencil ? samples : 0u));
    p.inUse = true;
    p.lastUsed = m_frameCounter;
    m_pool.push_back(p);
    ++m_stats.transientCreates;
    return static_cast<int>(m_pool.size() - 1);
}

void WgpuDevice::trimPool() {
    // T-163 (gl_fbo.h): over the byte budget, the least recently used entries go; under it, one
    // entry idle for kTransientIdleFrames per frame. Called after the frame's submit.
    auto erase = [this](size_t i) {
        m_gpu->releaseBindGroups();  // the cached bind groups may name its views
        releasePooled(m_pool[i]);
        m_pool.erase(m_pool.begin() + static_cast<std::ptrdiff_t>(i));
        for (int& t : m_frameTargets) {
            if (t == static_cast<int>(i)) {
                t = -1;
            } else if (t > static_cast<int>(i)) {
                --t;
            }
        }
        ++m_stats.transientDeletes;
    };
    uint64_t live = 0;
    for (const Pooled& p : m_pool) live += p.bytes;
    while (live > m_poolBudget && m_pool.size() > 1) {
        size_t lru = 0;
        for (size_t i = 1; i < m_pool.size(); ++i) {
            if (m_pool[i].lastUsed < m_pool[lru].lastUsed) lru = i;
        }
        if (m_pool[lru].lastUsed == m_frameCounter) break;  // everything left was used this frame
        live -= m_pool[lru].bytes;
        erase(lru);
    }
    for (size_t i = 0; i < m_pool.size(); ++i) {
        if (m_frameCounter - m_pool[i].lastUsed > kTransientIdleFrames) {
            live -= m_pool[i].bytes;
            erase(i);
            break;
        }
    }
    m_stats.poolEntries = static_cast<uint32_t>(m_pool.size());
    m_stats.poolBytes = live;
}

WGPUTexture WgpuDevice::transientTexture(gpu::TargetHandle target) const {
    if (target == 0 || target > m_frameTargets.size() || m_frameTargets[target - 1] < 0) {
        return nullptr;
    }
    const Pooled& p = m_pool[static_cast<size_t>(m_frameTargets[target - 1])];
    return p.resolve != nullptr ? p.resolve : p.color;
}

uint32_t WgpuDevice::transientSampleCount(gpu::TargetHandle target) const {
    if (target == 0 || target > m_frameTargets.size() || m_frameTargets[target - 1] < 0) {
        return 0;
    }
    return m_pool[static_cast<size_t>(m_frameTargets[target - 1])].samples;
}

WgpuDevice::Window WgpuDevice::windowOf(gpu::TargetHandle target) const {
    if (target == gpu::kSurfaceTarget || target > m_windows.size()) {
        return Window{0, 0, m_width, m_height};
    }
    return m_windows[target - 1];
}

WGPUTextureView WgpuDevice::sampleView(gpu::TargetHandle t) const {
    if (t == gpu::kSurfaceTarget) {
        return m_surface;
    }
    if (t > m_frameTargets.size() || m_frameTargets[t - 1] < 0) {
        return nullptr;
    }
    const Pooled& p = m_pool[static_cast<size_t>(m_frameTargets[t - 1])];
    return p.resolveView != nullptr ? p.resolveView : p.colorView;
}

// -------------------------------------------------------------------------------------------
// submit(): plan, upload, encode.
// -------------------------------------------------------------------------------------------

void WgpuDevice::planFrame(const gpu::GpuFrame& frame) {
    const size_t n = frame.transients.size();
    m_windows.assign(n, Window{});
    for (size_t i = 0; i < n; ++i) {
        m_windows[i] = Window{0, 0, frame.transients[i].width, frame.transients[i].height};
    }
    // Uniform buffer: [0, 512) zeros (the binding of every parameter-less command and of the
    // zero-offset blit), then 256-byte-aligned blocks per pass, per copy blit and per parameter
    // block.
    m_uniformData.assign(kZeroBlockFloats, 0.0f);
    auto alloc = [this](uint32_t floats) {
        const uint32_t offset = static_cast<uint32_t>(m_uniformData.size() * sizeof(float));
        m_uniformData.resize(m_uniformData.size() + roundUp(floats * 4u, detail::kUniformAlign) / 4u, 0.0f);
        return offset;
    };
    m_plans.assign(frame.passes.size(), PassPlan{});
    for (size_t i = 0; i < frame.passes.size(); ++i) {
        const gpu::Pass& pass = frame.passes[i];
        PassPlan& plan = m_plans[i];
        if (pass.target == gpu::kSurfaceTarget) {
            plan.targetW = m_width;
            plan.targetH = m_height;
        } else if (pass.target <= n) {
            plan.targetW = frame.transients[pass.target - 1].width;
            plan.targetH = frame.transients[pass.target - 1].height;
        }
        Window win{pass.viewX, pass.viewY, pass.viewW, pass.viewH};
        if (win.w <= 0 || win.h <= 0) {
            win = pass.target == gpu::kSurfaceTarget ? Window{0, 0, plan.targetW, plan.targetH}
                                                     : windowOf(pass.target);
        }
        if (pass.target != gpu::kSurfaceTarget && pass.target <= n) {
            m_windows[pass.target - 1] = win;
            plan.originX = win.x;
            plan.originY = win.y;
            if (plan.targetW != win.w || plan.targetH != win.h) {
                // T-343: a scaled transient covers its whole target with the window.
                plan.scaleX = static_cast<float>(plan.targetW) / static_cast<float>(std::max(1, win.w));
                plan.scaleY = static_cast<float>(plan.targetH) / static_cast<float>(std::max(1, win.h));
            }
        }
        plan.win = win;
        // gl_device.cpp ndcMatrix: NDC.x = 2 (px - x) / w - 1, NDC.y = 1 - 2 (py - y) / h.
        const float sx = win.w > 0 ? 2.0f / static_cast<float>(win.w) : 0.0f;
        const float sy = win.h > 0 ? 2.0f / static_cast<float>(win.h) : 0.0f;
        plan.constOffset = alloc(12);
        float* m = &m_uniformData[plan.constOffset / 4];
        m[0] = sx;
        m[5] = -sy;
        m[8] = -1.0f - sx * static_cast<float>(win.x);
        m[9] = 1.0f + sy * static_cast<float>(win.y);
        m[10] = 1.0f;
        if (pass.copy == 0) {
            continue;
        }
        // The overlap of the source's and this pass's windows (surface pixels), each side in its
        // own target's top-down pixel space.
        const Window sw = windowOf(pass.copySource);
        const int sox = pass.copySource == gpu::kSurfaceTarget ? 0 : sw.x;
        const int soy = pass.copySource == gpu::kSurfaceTarget ? 0 : sw.y;
        const int x0 = std::max(win.x, sw.x), y0 = std::max(win.y, sw.y);
        const int x1 = std::min(win.x + win.w, sw.x + sw.w), y1 = std::min(win.y + win.h, sw.y + sw.h);
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        plan.dstX = x0 - plan.originX;
        plan.dstY = y0 - plan.originY;
        plan.srcX = x0 - sox;
        plan.srcY = y0 - soy;
        plan.copyW = std::min(x1 - x0, plan.targetW - plan.dstX);
        plan.copyH = std::min(y1 - y0, plan.targetH - plan.dstY);
        bool ctt = false;
        if (pass.target != gpu::kSurfaceTarget && pass.copySource != gpu::kSurfaceTarget &&
            pass.target <= n && pass.copySource <= n && m_frameTargets[pass.target - 1] >= 0 &&
            m_frameTargets[pass.copySource - 1] >= 0) {
            const Pooled& dst = m_pool[static_cast<size_t>(m_frameTargets[pass.target - 1])];
            const Pooled& src = m_pool[static_cast<size_t>(m_frameTargets[pass.copySource - 1])];
            plan.copyW = std::min(plan.copyW, static_cast<int>(src.width) - plan.srcX);
            plan.copyH = std::min(plan.copyH, static_cast<int>(src.height) - plan.srcY);
            ctt = dst.samples == 1 && dst.format == src.format;
        }
        if (plan.copyW <= 0 || plan.copyH <= 0 || plan.dstX < 0 || plan.dstY < 0 || plan.srcX < 0 ||
            plan.srcY < 0) {
            continue;
        }
        plan.copyMode = ctt ? 1 : 2;
        if (!ctt) {
            plan.blitOffset = alloc(4);
            m_uniformData[plan.blitOffset / 4] = static_cast<float>(plan.srcX - plan.dstX);
            m_uniformData[plan.blitOffset / 4 + 1] = static_cast<float>(plan.srcY - plan.dstY);
        }
    }
    // Parameter blocks: one per distinct paramIndex (a block's content depends on nothing else).
    m_cmdParamOffsets.assign(frame.cmds.size(), 0);
    std::unordered_map<uint32_t, uint32_t> byIndex;
    for (size_t c = 0; c < frame.cmds.size(); ++c) {
        const gpu::DrawCmd& cmd = frame.cmds[c];
        const uint32_t floats = paramFloats(cmd);
        if (floats == 0) {
            continue;  // offset 0: the zero block
        }
        auto it = byIndex.find(cmd.paramIndex);
        if (it != byIndex.end()) {
            m_cmdParamOffsets[c] = it->second;
            continue;
        }
        const size_t available =
            cmd.paramIndex < frame.params.size() ? frame.params.size() - cmd.paramIndex : 0;
        const float* src = available > 0 ? frame.params.data() + cmd.paramIndex : nullptr;
        uint32_t offset = 0;
        if (cmd.shading == gpu::Shading::Effect && cmd.program == gpu::EffectProgram::Custom) {
            float block[kZeroBlockFloats] = {};
            uint16_t shader = 0;
            if (!m_gpu->pipelines.packCustom(src, available, block, shader)) {
                m_cmdParamOffsets[c] = kNoParams;
                continue;
            }
            offset = alloc(static_cast<uint32_t>(kZeroBlockFloats));
            std::memcpy(&m_uniformData[offset / 4], block, sizeof(block));
        } else if (available >= floats) {
            offset = alloc(floats);
            std::memcpy(&m_uniformData[offset / 4], src, floats * sizeof(float));
        } else if (cmd.shading == gpu::Shading::Glyph && cmd.kind == gpu::DrawKind::Triangles) {
            offset = alloc(floats);
            m_uniformData[offset / 4] = 1.0f;  // gl_device.cpp: GlyphParams{1.0f} when missing
        } else if (cmd.shading == gpu::Shading::Composite && cmd.kind == gpu::DrawKind::Triangles) {
            offset = alloc(floats);
            m_uniformData[offset / 4 + 1] = 1.0f;  // CompositeParams{0, 1}
        } else {
            m_cmdParamOffsets[c] = kNoParams;  // gl_device.cpp draws nothing without its parameters
            continue;
        }
        m_cmdParamOffsets[c] = offset;
        byIndex.emplace(cmd.paramIndex, offset);
    }
    // The last binding reads kParamBlockBytes from its offset.
    m_uniformData.resize(m_uniformData.size() + kZeroBlockFloats, 0.0f);
}

void WgpuDevice::submit(const gpu::GpuFrame& frame) {
    m_lastError.clear();
    m_stats = WgpuDeviceStats{};
    if (m_device == nullptr || !m_gpu) {
        m_lastError = "submit: WgpuDevice::init was not called";
        return;
    }
    using Clock = std::chrono::steady_clock;
    const auto msSince = [](Clock::time_point t0) {
        return std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
    };
    auto t0 = Clock::now();
    m_frame = &frame;
    ++m_frameCounter;
    for (Pooled& p : m_pool) {
        p.inUse = false;
    }
    m_frameTargets.assign(frame.transients.size(), -1);
    for (size_t i = 0; i < frame.transients.size(); ++i) {
        m_frameTargets[i] = acquirePooled(frame.transients[i]);
        if (m_frameTargets[i] < 0) {
            m_lastError = "submit: failed to create a transient target";
        }
    }
    planFrame(frame);
    m_stats.planMs = msSince(t0);
    t0 = Clock::now();
    Gpu& g = *m_gpu;
    m_stats.uploadBytes += g.geometry.upload(m_device, m_queue, frame, m_stats.uploadRanges);
    m_stats.uploadBytes += g.uniforms.upload(m_device, m_queue, WGPUBufferUsage_Uniform, m_uniformData.data(),
                                             m_uniformData.size() * sizeof(float), 16, m_stats.uploadRanges);
    if (g.uniforms.generation != g.groupsGeneration) {
        // The uniform buffer was (re)created: new group 0 / group 1 bind groups over it (and the
        // cached blit groups, which bind it too, go). T-457: compared by generation, not by handle:
        // the grown buffer's handle may reuse the address of the one just released, which left the
        // groups bound to the old, smaller buffer (Vulkan, challenge-menu: offset 65536 into 64 KB).
        g.releaseBindGroups();
        if (g.passGroup != nullptr) wgpuBindGroupRelease(g.passGroup);
        if (g.paramGroup != nullptr) wgpuBindGroupRelease(g.paramGroup);
        WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
        e.binding = 0;
        e.buffer = g.uniforms.buffer;
        e.size = 48;
        WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        d.layout = g.pipelines.passLayout();
        d.entryCount = 1;
        d.entries = &e;
        g.passGroup = wgpuDeviceCreateBindGroup(m_device, &d);
        e.size = detail::kParamBlockBytes;
        d.layout = g.pipelines.paramLayout();
        g.paramGroup = wgpuDeviceCreateBindGroup(m_device, &d);
        g.groupsGeneration = g.uniforms.generation;
    }
    m_stats.uploadMs = msSince(t0);
    t0 = Clock::now();
    const bool surfaceOk = m_surface != nullptr && ensureSurfaceAttachments();
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(m_device, nullptr);
    bool surfaceStarted = false;
    // T-457: every dynamic offset of this frame is below the uploaded size, so a buffer at least that
    // large keeps them all in range; anything else (a failed growth) skips the frame's draws.
    const bool groupsOk = g.uniforms.buffer != nullptr && g.passGroup != nullptr && g.paramGroup != nullptr &&
                          g.uniforms.capacity >= m_uniformData.size() * sizeof(float);
    for (size_t i = 0; i < frame.passes.size(); ++i) {
        const gpu::Pass& pass = frame.passes[i];
        if (!groupsOk || (pass.target == gpu::kSurfaceTarget && !surfaceOk)) {
            m_stats.skippedCmds += pass.cmdCount;
            continue;
        }
        if (pass.target != gpu::kSurfaceTarget &&
            (pass.target > m_frameTargets.size() || m_frameTargets[pass.target - 1] < 0)) {
            m_lastError = "submit: pass target " + std::to_string(pass.target) + " has no transient";
            m_stats.skippedCmds += pass.cmdCount;
            continue;
        }
        encodePass(enc, i, surfaceStarted);
    }
    if (!surfaceStarted && surfaceOk && m_clearSurface) {
        // Nothing drew the surface: a pass of its own for the requested clear.
        WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        ca.view = m_sampleCount > 1 ? m_surfaceMsaaView : m_surface;
        ca.resolveTarget = m_sampleCount > 1 ? m_surface : nullptr;
        ca.loadOp = WGPULoadOp_Clear;
        ca.storeOp = WGPUStoreOp_Store;
        ca.clearValue = m_clearColor;
        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &ca;
        WGPURenderPassEncoder p = wgpuCommandEncoderBeginRenderPass(enc, &rp);
        wgpuRenderPassEncoderEnd(p);
        wgpuRenderPassEncoderRelease(p);
        ++m_stats.passes;
        ++m_stats.clears;
    }
    m_stats.encodeMs = msSince(t0);
    t0 = Clock::now();
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(m_queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(enc);
    m_stats.queueMs = msSince(t0);
    g.releaseFrameBindGroups();

    trimPool();
    m_stats.cachedTargets = static_cast<uint32_t>(m_cachedFree.size());
    for (const Texture& t : m_textures) {
        if (t.cached && t.view != nullptr) ++m_stats.cachedTargets;
    }
    // T-456: pipelines that failed this submit (each key once; their draws were skipped).
    const std::string pipelineErrors = g.pipelines.takeError();
    if (!pipelineErrors.empty()) {
        m_lastError = m_lastError.empty() ? "submit: " + pipelineErrors : m_lastError + "; " + pipelineErrors;
    }
    if (m_surface == nullptr && !frame.passes.empty() && m_lastError.empty()) {
        m_lastError = "submit: no target view (setSurface)";
    }
    if (m_lastError.empty() && (m_stats.skippedCmds > 0 || m_stats.skippedCopies > 0)) {
        m_lastError = "submit: " + std::to_string(m_stats.skippedCmds) + " draw commands and " +
                      std::to_string(m_stats.skippedCopies) + " copies could not be executed";
    }
    m_frame = nullptr;
}

void WgpuDevice::encodePass(WGPUCommandEncoder enc, size_t index, bool& surfaceStarted) {
    const gpu::GpuFrame& frame = *m_frame;
    const gpu::Pass& pass = frame.passes[index];
    const PassPlan& plan = m_plans[index];
    Gpu& g = *m_gpu;
    const WGPUColor transparent{0, 0, 0, 0};

    WGPUTextureView colorView = nullptr, resolveView = nullptr, stencilView = nullptr;
    WGPUTexture colorTexture = nullptr;
    WGPUTextureFormat format = m_targetFormat;
    uint32_t samples = m_sampleCount;
    bool clearColor = pass.clear != 0, clearStencil = pass.clear != 0, loadHost = false;
    WGPUColor color = transparent;
    if (pass.target == gpu::kSurfaceTarget) {
        colorView = m_sampleCount > 1 ? m_surfaceMsaaView : m_surface;
        resolveView = m_sampleCount > 1 ? m_surface : nullptr;
        stencilView = m_surfaceStencilView;
        const bool first = !surfaceStarted;
        clearColor = (first && m_clearSurface) || pass.clear != 0;
        if (first && m_clearSurface && pass.clear == 0) {
            color = m_clearColor;
        }
        clearStencil = first || pass.clear != 0;
        // T-438 ruling: a 4x surface without a clear starts from the host's content.
        loadHost = first && m_sampleCount > 1 && !clearColor;
        surfaceStarted = true;
    } else {
        const Pooled& p = m_pool[static_cast<size_t>(m_frameTargets[pass.target - 1])];
        colorView = p.colorView;
        colorTexture = p.color;
        resolveView = p.resolveView;
        stencilView = p.depthStencilView;
        format = detail::formatOf(p.format);
        samples = p.samples;
    }
    const uint32_t tw = static_cast<uint32_t>(std::max(0, plan.targetW));
    const uint32_t th = static_cast<uint32_t>(std::max(0, plan.targetH));
    auto begin = [&](WGPUTextureView view, WGPUTextureView resolve, WGPUTextureView stencil, bool clear,
                     const WGPUColor& c, bool stencilClear) {
        WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        ca.view = view;
        ca.resolveTarget = resolve;
        ca.loadOp = clear ? WGPULoadOp_Clear : WGPULoadOp_Load;
        ca.storeOp = WGPUStoreOp_Store;
        ca.clearValue = c;
        WGPURenderPassDepthStencilAttachment ds = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rp.label = label("xt_wgpu pass");
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &ca;
        if (stencil != nullptr) {
            ds.view = stencil;
            ds.stencilLoadOp = stencilClear ? WGPULoadOp_Clear : WGPULoadOp_Load;
            ds.stencilStoreOp = WGPUStoreOp_Store;
            ds.stencilClearValue = 0;
            rp.depthStencilAttachment = &ds;
        }
        ++m_stats.passes;
        if (clear) ++m_stats.clears;
        return wgpuCommandEncoderBeginRenderPass(enc, &rp);
    };

    if (plan.copyMode == 1) {
        // copyTextureToTexture happens outside a render pass: a requested clear goes first.
        if (clearColor || clearStencil) {
            WGPURenderPassEncoder p =
                begin(colorView, resolveView, stencilView, clearColor, color, clearStencil);
            wgpuRenderPassEncoderEnd(p);
            wgpuRenderPassEncoderRelease(p);
            clearColor = clearStencil = false;
        }
        const Pooled& src = m_pool[static_cast<size_t>(m_frameTargets[pass.copySource - 1])];
        WGPUTexelCopyTextureInfo s = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        s.texture = src.resolve != nullptr ? src.resolve : src.color;
        s.origin = WGPUOrigin3D{static_cast<uint32_t>(plan.srcX), static_cast<uint32_t>(plan.srcY), 0};
        WGPUTexelCopyTextureInfo d = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        d.texture = colorTexture;
        d.origin = WGPUOrigin3D{static_cast<uint32_t>(plan.dstX), static_cast<uint32_t>(plan.dstY), 0};
        const WGPUExtent3D size{static_cast<uint32_t>(plan.copyW), static_cast<uint32_t>(plan.copyH), 1};
        wgpuCommandEncoderCopyTextureToTexture(enc, &s, &d, &size);
        ++m_stats.copies;
    }
    if (loadHost) {
        // Pre-pass: the host's surface into the 4x attachment (blit.wgsl, zero offset).
        WGPURenderPassEncoder p = begin(m_surfaceMsaaView, nullptr, nullptr, true, transparent, false);
        blit(p, m_surface, 0, 0, 0, static_cast<int>(tw), static_cast<int>(th), format, samples, false, tw,
             th);
        wgpuRenderPassEncoderEnd(p);
        wgpuRenderPassEncoderRelease(p);
        ++m_stats.blits;
    }
    WGPURenderPassEncoder rp = begin(colorView, resolveView, stencilView, clearColor, color, clearStencil);
    g.resetPassState();
    if (plan.copyMode == 2) {
        WGPUTextureView src = sampleView(pass.copySource);
        if (src != nullptr) {
            blit(rp, src, plan.blitOffset, plan.dstX, plan.dstY, plan.copyW, plan.copyH, format, samples,
                 stencilView != nullptr, tw, th);
            ++m_stats.blits;
        } else {
            ++m_stats.skippedCopies;
        }
        g.resetPassState();
    }
    // The pass viewport (gl_device.cpp submit): the window inside the target, or the whole target
    // for a scaled transient.
    int vx = 0, vy = 0, vw = static_cast<int>(tw), vh = static_cast<int>(th);
    if (plan.scaleX == 1.0f && plan.scaleY == 1.0f) {
        vx = plan.win.x - plan.originX;
        vy = plan.win.y - plan.originY;
        vw = std::min(plan.win.w, static_cast<int>(tw) - vx);
        vh = std::min(plan.win.h, static_cast<int>(th) - vy);
    }
    if (vw > 0 && vh > 0 && vx >= 0 && vy >= 0) {
        wgpuRenderPassEncoderSetViewport(rp, static_cast<float>(vx), static_cast<float>(vy),
                                         static_cast<float>(vw), static_cast<float>(vh), 0.0f, 1.0f);
        wgpuRenderPassEncoderSetBindGroup(rp, 0, g.passGroup, 1, &plan.constOffset);
        if (g.geometry.indexBytes() > 0 && g.geometry.indices() != nullptr) {
            wgpuRenderPassEncoderSetIndexBuffer(rp, g.geometry.indices(), WGPUIndexFormat_Uint32, 0,
                                                g.geometry.indexBytes());
        }
        for (uint32_t c = pass.firstCmd; c < pass.firstCmd + pass.cmdCount && c < frame.cmds.size(); ++c) {
            drawCmd(rp, frame.cmds[c], c, plan, pass.target);
        }
    }
    wgpuRenderPassEncoderEnd(rp);
    wgpuRenderPassEncoderRelease(rp);
    for (const gpu::CacheStore& store : frame.cacheStores) {
        if (store.pass == index) {
            executeCacheStore(enc, store);
        }
    }
}

void WgpuDevice::executeCacheStore(WGPUCommandEncoder enc, const gpu::CacheStore& store) {
    const Texture* cached = find(store.texture);
    if (cached == nullptr || !cached->cached || store.src == gpu::kSurfaceTarget ||
        store.src > m_frameTargets.size() || m_frameTargets[store.src - 1] < 0) {
        m_lastError = "cache store into a target this device did not acquire, or from no transient";
        ++m_stats.skippedCopies;
        return;
    }
    const Pooled& src = m_pool[static_cast<size_t>(m_frameTargets[store.src - 1])];
    if (src.format != gpu::TextureFormat::Rgba8) {
        ++m_stats.skippedCopies;
        return;
    }
    // Row 0 = top on both sides (WebGPU targets are top-down): a plain copy, no flip.
    WGPUTexelCopyTextureInfo s = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    s.texture = src.resolve != nullptr ? src.resolve : src.color;
    WGPUTexelCopyTextureInfo d = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    d.texture = cached->texture;
    const WGPUExtent3D size{std::min<uint32_t>(src.width, cached->width),
                            std::min<uint32_t>(src.height, cached->height), 1};
    wgpuCommandEncoderCopyTextureToTexture(enc, &s, &d, &size);
    ++m_stats.cacheStores;
}

}  // namespace xt::wgpu

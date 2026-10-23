#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "wgpu_internal.h"
#include "xt/wgpu/wgpu_device.h"

/// Command encoding of WgpuDevice::submit (T-438, Docs/design/10-backends.md §10.10.2): per draw
/// command the cached pipeline, the parameter offset (group 1), the texture bind group (group 2),
/// the scissor (gl_device.cpp applyScissor in top-down pixels) and the stencil reference
/// (gl_device.cpp applyStencil's `ref`).

namespace xt::wgpu {

WGPUBindGroup WgpuDevice::Gpu::bindGroup(WGPUDevice device, const detail::BindGroupKey& key,
                                         uint32_t& creates, bool perFrame) {
    if (!perFrame) {
        auto it = bindGroups.find(key);
        if (it != bindGroups.end()) {
            return it->second;
        }
        if (bindGroups.size() >= kMaxBindGroups) {
            for (auto& kv : bindGroups) {
                wgpuBindGroupRelease(kv.second);
            }
            bindGroups.clear();
        }
    }
    WGPUBindGroupEntry e[4] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
                               WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    size_t count = 0;
    if (key.layout == pipelines.blitLayout()) {
        e[0].binding = 0;
        e[0].buffer = uniforms.buffer;
        e[0].size = 16;
        e[1].binding = 1;
        e[1].textureView = key.v0;
        count = 2;
    } else {
        e[0].binding = 0;
        e[0].textureView = key.v0;
        e[1].binding = 1;
        e[1].textureView = key.v1;
        e[2].binding = 2;
        e[2].textureView = key.v2;
        e[3].binding = 3;
        e[3].sampler = key.sampler;
        count = 4;
    }
    WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    d.layout = key.layout;
    d.entryCount = count;
    d.entries = e;
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(device, &d);
    ++creates;
    if (perFrame) {
        frameBindGroups.push_back(bg);
    } else {
        bindGroups.emplace(key, bg);
    }
    return bg;
}

void WgpuDevice::blit(WGPURenderPassEncoder pass, WGPUTextureView src, uint32_t uniformOffset, int x, int y,
                      int w, int h, WGPUTextureFormat format, uint32_t samples, bool hasStencil,
                      uint32_t targetW, uint32_t targetH) {
    Gpu& g = *m_gpu;
    WGPURenderPipeline p = g.pipelines.blit(format, samples, hasStencil);
    x = std::max(0, x);
    y = std::max(0, y);
    w = std::min(w, static_cast<int>(targetW) - x);
    h = std::min(h, static_cast<int>(targetH) - y);
    if (p == nullptr || src == nullptr || w <= 0 || h <= 0) {
        ++m_stats.skippedCopies;
        return;
    }
    detail::BindGroupKey key;
    key.layout = g.pipelines.blitLayout();
    key.v0 = src;
    WGPUBindGroup bg = g.bindGroup(m_device, key, m_stats.bindGroups, src == m_surface);
    wgpuRenderPassEncoderSetPipeline(pass, p);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 1, &uniformOffset);
    wgpuRenderPassEncoderSetViewport(pass, 0.0f, 0.0f, static_cast<float>(targetW),
                                     static_cast<float>(targetH), 0.0f, 1.0f);
    wgpuRenderPassEncoderSetScissorRect(pass, static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                                        static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    ++m_stats.drawCalls;
    ++m_stats.pipelineBinds;
}

void WgpuDevice::drawCmd(WGPURenderPassEncoder pass, const gpu::DrawCmd& cmd, uint32_t cmdIndex,
                         const PassPlan& plan, gpu::TargetHandle target) {
    using gpu::Shading;
    using Clock = std::chrono::steady_clock;
    Gpu& g = *m_gpu;
    const gpu::GpuFrame& frame = *m_frame;
    const bool particles = cmd.kind == gpu::DrawKind::Particles;
    if (particles ? (cmd.instanceCount == 0 || static_cast<size_t>(cmd.firstInstance) + cmd.instanceCount >
                                                   frame.particleInstances.size())
                  : cmd.indexCount == 0) {
        return;
    }
    const bool timed = cmd.shading == Shading::Effect || cmd.shading == Shading::Glyph;
    const Clock::time_point t0 = timed ? Clock::now() : Clock::time_point{};
    const uint32_t paramOffset = m_cmdParamOffsets[cmdIndex];
    if (paramOffset == kNoParams) {
        ++m_stats.skippedCmds;
        return;
    }

    // Target attachments of the open pass.
    WGPUTextureFormat format = m_targetFormat;
    uint32_t samples = m_sampleCount;
    bool hasStencil = true;
    if (target != gpu::kSurfaceTarget) {
        const Pooled& p = m_pool[static_cast<size_t>(m_frameTargets[target - 1])];
        format = detail::formatOf(p.format);
        samples = p.samples;
        hasStencil = p.depthStencilView != nullptr;
    }

    // The fragment program.
    uint16_t shader = detail::kShSolid;
    switch (cmd.shading) {
        case Shading::Solid:
        case Shading::StencilOnly:
            shader = detail::kShSolid;
            break;
        case Shading::Gradient:
            shader = detail::kShGradient;
            break;
        case Shading::Texture:
            shader = detail::kShTexture;
            break;
        case Shading::TextureStraight:
            shader = detail::kShTextureStraight;
            break;
        case Shading::Glyph:
            shader = detail::kShGlyph;
            break;
        case Shading::Composite:
            shader = detail::kShComposite;
            break;
        case Shading::ParticleTexture:
            shader = detail::kShParticleTexture;
            break;
        case Shading::ParticleSolid:
            shader = detail::kShParticleSolid;
            break;
        case Shading::Effect:
            if (cmd.program == gpu::EffectProgram::Custom) {
                const float* p = &frame.params[cmd.paramIndex];  // packCustom checked the block
                const uint32_t id = static_cast<uint32_t>(p[0]);
                if (id == 0 || id > g.pipelines.customCount()) {
                    m_lastError = "unknown custom effect program";
                    ++m_stats.skippedCmds;
                    return;
                }
                shader = static_cast<uint16_t>(detail::kShaderCount + id - 1);
            } else {
                shader = detail::effectShader(cmd.program);
                if (shader == detail::kShaderCount) {
                    m_lastError = "unsupported effect program";
                    ++m_stats.skippedCmds;
                    return;
                }
            }
            break;
    }
    if (particles != (cmd.shading == Shading::ParticleTexture || cmd.shading == Shading::ParticleSolid)) {
        return;  // gl_device.cpp: particle shadings draw only as DrawKind::Particles and vice versa
    }

    detail::PipelineKey key;
    key.shader = shader;
    key.blend = cmd.blend;
    key.stencil = hasStencil ? cmd.stencilOp : gpu::StencilOp::None;
    key.kind = cmd.kind;
    key.colorWrite = cmd.shading != Shading::StencilOnly;
    key.format = format;
    key.samples = static_cast<uint8_t>(samples);
    key.hasStencil = hasStencil;
    WGPURenderPipeline pipeline = g.pipelines.get(key);
    if (pipeline == nullptr) {
        ++m_stats.skippedCmds;
        return;
    }

    // Scissor: surface pixels clipped to the pass window, then into the target's own pixels.
    const int x0 = std::max<int>(cmd.clipX, plan.win.x);
    const int y0 = std::max<int>(cmd.clipY, plan.win.y);
    const int x1 = std::min<int>(cmd.clipX + cmd.clipW, plan.win.x + plan.win.w);
    const int y1 = std::min<int>(cmd.clipY + cmd.clipH, plan.win.y + plan.win.h);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    int sx0, sy0, sx1, sy1;
    if (plan.scaleX != 1.0f || plan.scaleY != 1.0f) {
        // T-343: a scaled transient; the window maps onto the whole (smaller) target.
        sx0 = static_cast<int>(std::floor(static_cast<float>(x0 - plan.originX) * plan.scaleX));
        sx1 = static_cast<int>(std::ceil(static_cast<float>(x1 - plan.originX) * plan.scaleX));
        sy0 = static_cast<int>(std::floor(static_cast<float>(y0 - plan.originY) * plan.scaleY));
        sy1 = static_cast<int>(std::ceil(static_cast<float>(y1 - plan.originY) * plan.scaleY));
    } else {
        sx0 = x0 - plan.originX;
        sx1 = x1 - plan.originX;
        sy0 = y0 - plan.originY;
        sy1 = y1 - plan.originY;
    }
    sx0 = std::max(sx0, 0);
    sy0 = std::max(sy0, 0);
    sx1 = std::min(sx1, plan.targetW);
    sy1 = std::min(sy1, plan.targetH);
    if (sx1 <= sx0 || sy1 <= sy0) {
        return;
    }

    // Group 2: the textures and sampler of this shading.
    detail::BindGroupKey bk;
    bk.layout = g.pipelines.textureLayout(shader);
    bk.v0 = bk.v1 = bk.v2 = g.dummyView;
    bk.sampler = g.pipelines.sampler(gpu::Sampler::LinearClamp);
    auto textureView = [this, &g](gpu::TextureHandle h) {
        const Texture* t = find(h);
        return t != nullptr ? t->view : g.dummyView;
    };
    auto targetView = [this, &g](gpu::TargetHandle t) {
        WGPUTextureView v = sampleView(t);
        return (t != gpu::kSurfaceTarget && v != nullptr) ? v : g.dummyView;
    };
    switch (cmd.shading) {
        case Shading::Texture:
        case Shading::TextureStraight:
        case Shading::ParticleTexture:
            bk.v0 = textureView(cmd.texture);
            bk.sampler = g.pipelines.sampler(cmd.sampler);
            break;
        case Shading::Gradient:
        case Shading::Glyph:
            bk.v0 = textureView(cmd.texture);  // gl_device.cpp: always LinearClamp
            break;
        case Shading::Composite:
        case Shading::Effect:
            bk.v0 = targetView(cmd.src);
            if (cmd.aux != 0) bk.v1 = targetView(cmd.aux);
            if (cmd.texture != 0) bk.v2 = textureView(cmd.texture);
            break;
        default:
            break;
    }
    WGPUBindGroup textures = g.bindGroup(m_device, bk, m_stats.bindGroups);

    if (pipeline != g.pipeline) {
        wgpuRenderPassEncoderSetPipeline(pass, pipeline);
        g.pipeline = pipeline;
        ++m_stats.pipelineBinds;
    }
    if (paramOffset != g.paramOffset) {
        wgpuRenderPassEncoderSetBindGroup(pass, 1, g.paramGroup, 1, &paramOffset);
        g.paramOffset = paramOffset;
    }
    if (textures != g.textures) {
        wgpuRenderPassEncoderSetBindGroup(pass, 2, textures, 0, nullptr);
        g.textures = textures;
    }
    if (hasStencil) {
        // gl_device.cpp applyStencil: the depth ops compare against `ref << 4`; the cover test and
        // the winding clear use 0.
        const bool zeroRef =
            cmd.stencilOp == gpu::StencilOp::TestNotEqualZero || cmd.stencilOp == gpu::StencilOp::ClearRect;
        const uint32_t ref = zeroRef ? 0u : (static_cast<uint32_t>(cmd.stencilRef) << 4) & 0xFFu;
        if (ref != g.stencilRef) {
            wgpuRenderPassEncoderSetStencilReference(pass, ref);
            g.stencilRef = ref;
        }
    }
    wgpuRenderPassEncoderSetScissorRect(pass, static_cast<uint32_t>(sx0), static_cast<uint32_t>(sy0),
                                        static_cast<uint32_t>(sx1 - sx0), static_cast<uint32_t>(sy1 - sy0));
    if (particles) {
        if (g.vertexSource != 2) {
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, g.geometry.instances(), 0,
                                                 g.geometry.instanceBytes());
            g.vertexSource = 2;
        }
        wgpuRenderPassEncoderDraw(pass, 6, cmd.instanceCount, 0, cmd.firstInstance);
    } else {
        if (g.vertexSource != 1) {
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, g.geometry.vertices(), 0, g.geometry.vertexBytes());
            g.vertexSource = 1;
        }
        wgpuRenderPassEncoderDrawIndexed(pass, cmd.indexCount, 1, cmd.firstIndex,
                                         static_cast<int32_t>(cmd.baseVertex), 0);
    }
    ++m_stats.drawCalls;
    if (timed) {
        const float ms = std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
        (cmd.shading == Shading::Effect ? m_stats.effectsMs : m_stats.textMs) += ms;
    }
}

}  // namespace xt::wgpu

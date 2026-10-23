#pragma once

#include <webgpu/webgpu.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xt/gpu/gpu_frame.h"
#include "xt/wgpu/wgpu_device.h"

/// xt_wgpu internals (T-438, Docs/design/10-backends.md §10.10.2): the pipeline cache, the shader
/// table and the geometry/uniform buffers behind `WgpuDevice::submit`. Not installed.

namespace xt::wgpu::detail {

inline WGPUStringView label(const char* s) { return WGPUStringView{s, s != nullptr ? WGPU_STRLEN : 0}; }

WGPUTextureFormat formatOf(gpu::TextureFormat fmt);

/// Pops the validation error scope pushed before (waiting for its answer); empty when no error.
std::string popErrorScope(WGPUDevice device);

/// T-456: pushes a Validation and an OutOfMemory error scope, the two filters wgpu-native accepts
/// (the Internal filter panics with "invalid error filter") ...
void pushCreationScopes(WGPUDevice device);
/// ... and pops both; empty when neither caught an error.
std::string popCreationScopes(WGPUDevice device);

/// T-456 test hook behind `WgpuDevice::setProbeShaderForTesting` (null = the built-in probe).
void setProbeShaderOverride(const char* wgsl);

/// The fragment programs of Code/Runtime/wgpu/src/shaders (one WGSL file each, prelude.wgsl
/// prepended). Custom effects (`EffectProgram::Custom`) follow at `kShaderCount + index`.
enum ShaderId : uint16_t {
    kShSolid = 0,
    kShGradient,
    kShTexture,
    kShTextureStraight,
    kShGlyph,
    kShComposite,
    kShParticleTexture,
    kShParticleSolid,
    kShFxGaussScalar,
    kShFxGaussColor,
    kShFxDilate,
    kShFxAlphaExtract,
    kShFxSdf,
    kShFxColorMatrix,
    kShFxGradientMask,
    kShFxShine,
    kShFxShadowGlow,
    kShFxOutline,
    kShFxBackdrop,
    kShFxMaskApply,
    kShFxDownsample,
    kShFxKawase,
    kShFxLevelMix,
    kShaderCount
};

/// The shader of a built-in `EffectProgram` (kShaderCount for None/Custom/unknown).
ShaderId effectShader(gpu::EffectProgram program);

/// Everything a render pipeline depends on (§10.10.2 "管線快取").
struct PipelineKey {
    uint16_t shader = 0;  ///< ShaderId, or kShaderCount + custom index
    gpu::Blend blend = gpu::Blend::SrcOver;
    gpu::StencilOp stencil = gpu::StencilOp::None;
    gpu::DrawKind kind = gpu::DrawKind::Triangles;
    bool colorWrite = true;
    WGPUTextureFormat format = WGPUTextureFormat_RGBA8Unorm;
    uint8_t samples = 1;
    bool hasStencil = false;  ///< the pass has a Stencil8 attachment

    uint64_t packed() const {
        return uint64_t(shader) | uint64_t(blend) << 16 | uint64_t(stencil) << 20 | uint64_t(kind) << 24 |
               uint64_t(colorWrite) << 25 | uint64_t(hasStencil) << 26 | uint64_t(samples) << 28 |
               uint64_t(uint32_t(format)) << 32;
    }
};

/// Shader modules, bind group layouts, samplers and the render pipelines built from them.
class Pipelines {
public:
    bool init(WGPUDevice device, std::string& err);
    void shutdown();

    /// T-456: builds one minimal pipeline (a WGSL module of its own, no bindings) for `format` x
    /// `samples` under the creation scopes, then releases it -- proves the backend's shader compiler
    /// works before any frame is drawn. False (and `err`) when it does not.
    bool probe(WGPUTextureFormat format, uint32_t samples, std::string& err);

    /// The pipeline for `key` (created on first use under the creation scopes; null after a
    /// failure, which is cached so the key is never retried and reported once via `takeError`).
    WGPURenderPipeline get(const PipelineKey& key);
    /// The copy blit pipeline (blit.wgsl) for a target of `format` x `samples` (with or without a
    /// stencil attachment).
    WGPURenderPipeline blit(WGPUTextureFormat format, uint32_t samples, bool hasStencil);

    WGPUBindGroupLayout passLayout() const { return m_bglPass; }
    WGPUBindGroupLayout paramLayout() const { return m_bglParams; }
    /// group 2 for `shader`: the R32Float aux of Outline needs an unfilterable binding.
    WGPUBindGroupLayout textureLayout(uint16_t shader) const {
        return shader == kShFxOutline ? m_bglTexturesOutline : m_bglTextures;
    }
    WGPUBindGroupLayout blitLayout() const { return m_bglBlit; }
    WGPUSampler sampler(gpu::Sampler s) const { return s == gpu::Sampler::PointClamp ? m_point : m_linear; }

    /// T-203 custom effects (§10.10.2 last rows): `glsl` is the `.xtfx` GLSL 330 module; it is
    /// rewritten for wgpu-native's GLSL frontend (Naga) and compiled once per distinct text.
    /// Returns the program id (1-based) or 0 when it does not compile (`warning` says why, once).
    uint32_t prepareCustom(std::string_view glsl, const char* names, const uint8_t* types, uint32_t count,
                           std::string& warning);
    /// Packs `CustomEffectParams` into the program's uniform block (`out`, 128 floats). False for
    /// an unknown or failed program.
    bool packCustom(const float* params, size_t available, float* out, uint16_t& shader) const;
    size_t customCount() const { return m_custom.size(); }

    /// The pipeline creation failures since the last call (one entry per failed key), then cleared.
    std::string takeError() {
        std::string e;
        e.swap(m_lastError);
        return e;
    }

private:
    struct Custom {
        std::string glsl;
        WGPUShaderModule module = nullptr;  ///< null = failed
        std::vector<uint8_t> types;
        std::vector<uint8_t> slots;
    };

    WGPUDevice m_device = nullptr;
    WGPUShaderModule m_modules[kShaderCount] = {};
    WGPUShaderModule m_blitModule = nullptr;
    /// The vertex stage of custom programs: kQuadVert in GLSL through the same frontend, so its
    /// output interpolation matches what Naga gives the fragment input (wgpu_custom.cpp).
    WGPUShaderModule m_customVertex = nullptr;
    WGPUBindGroupLayout m_bglPass = nullptr, m_bglParams = nullptr, m_bglTextures = nullptr,
                        m_bglTexturesOutline = nullptr, m_bglBlit = nullptr;
    WGPUPipelineLayout m_layout = nullptr, m_layoutOutline = nullptr, m_layoutBlit = nullptr;
    WGPUSampler m_linear = nullptr, m_point = nullptr;
    std::unordered_map<uint64_t, WGPURenderPipeline> m_pipelines;
    std::vector<Custom> m_custom;
    std::string m_lastError;

    /// The raw creation call for `key` (the caller holds the error scopes).
    WGPURenderPipeline create(const PipelineKey& key, WGPUShaderModule fragment);
    /// Pops the creation scopes pushed before `p` was created: `p`, or null (released) when they
    /// caught an error, which is appended once to m_lastError with `what` and `key` named.
    WGPURenderPipeline checked(WGPURenderPipeline p, const char* what, const PipelineKey& key);
};

/// One of the frame's GPU buffers with a CPU shadow: `upload` writes only the byte ranges that
/// differ from what this buffer last held (T-159), growing it in 64 KB steps.
struct ShadowBuffer {
    WGPUBuffer buffer = nullptr;
    uint64_t capacity = 0;
    /// T-457: bumped each time `buffer` is (re)created. A new buffer's handle may reuse the address of
    /// the one just released, so whatever was made over the old buffer compares this, not the handle.
    uint64_t generation = 0;
    std::vector<uint8_t> shadow;

    /// Returns the number of bytes written (0 when nothing changed).
    size_t upload(WGPUDevice device, WGPUQueue queue, WGPUBufferUsage usage, const void* data, size_t bytes,
                  size_t elementSize, uint32_t& ranges);
    void release();
};

/// The frame's vertex, index and particle-instance buffers: three rotating sets (§10.10.2 first
/// row, the T-159 strategy), each uploaded against its own shadow.
class Geometry {
public:
    void shutdown();
    /// Uploads the frame into the next set. Returns the bytes written; `ranges` counts the writes.
    size_t upload(WGPUDevice device, WGPUQueue queue, const gpu::GpuFrame& frame, uint32_t& ranges);
    WGPUBuffer vertices() const { return m_sets[m_current].vertices.buffer; }
    WGPUBuffer indices() const { return m_sets[m_current].indices.buffer; }
    WGPUBuffer instances() const { return m_sets[m_current].instances.buffer; }
    uint64_t vertexBytes() const { return m_vertexBytes; }
    uint64_t indexBytes() const { return m_indexBytes; }
    uint64_t instanceBytes() const { return m_instanceBytes; }

private:
    static constexpr int kSets = 3;
    struct Set {
        ShadowBuffer vertices, indices, instances;
    };
    Set m_sets[kSets];
    int m_current = 0;
    uint64_t m_vertexBytes = 0, m_indexBytes = 0, m_instanceBytes = 0;
};

/// Size of one command's parameter binding (group 1): 128 floats.
inline constexpr uint32_t kParamBlockBytes = 512;
/// Uniform dynamic offsets are multiples of this.
inline constexpr uint32_t kUniformAlign = 256;

/// A group-2 bind group's contents (one per distinct combination per submit).
struct BindGroupKey {
    WGPUBindGroupLayout layout = nullptr;
    WGPUTextureView v0 = nullptr, v1 = nullptr, v2 = nullptr;
    WGPUSampler sampler = nullptr;
    bool operator==(const BindGroupKey& o) const {
        return layout == o.layout && v0 == o.v0 && v1 == o.v1 && v2 == o.v2 && sampler == o.sampler;
    }
};
struct BindGroupKeyHash {
    size_t operator()(const BindGroupKey& k) const {
        size_t h = std::hash<const void*>()(k.layout);
        for (const void* p : {static_cast<const void*>(k.v0), static_cast<const void*>(k.v1),
                              static_cast<const void*>(k.v2), static_cast<const void*>(k.sampler)}) {
            h = h * 1099511628211ull ^ std::hash<const void*>()(p);
        }
        return h;
    }
};

}  // namespace xt::wgpu::detail

namespace xt::wgpu {

/// WgpuDevice's GPU-side state (wgpu_device.cpp, wgpu_draw.cpp).
struct WgpuDevice::Gpu {
    detail::Pipelines pipelines;
    detail::Geometry geometry;
    detail::ShadowBuffer uniforms;
    /// Group 0 / group 1 bind groups over `uniforms` (re-created when that buffer is).
    WGPUBindGroup passGroup = nullptr;
    WGPUBindGroup paramGroup = nullptr;
    uint64_t groupsGeneration = 0;  ///< `uniforms.generation` the two groups were made for (T-457)
    /// 1x1 transparent RGBA8: the view bound wherever a command has no texture.
    WGPUTexture dummy = nullptr;
    WGPUTextureView dummyView = nullptr;
    /// Bind groups over this device's own views (and registered external views), kept across
    /// submits: every release of such a view flushes them all (`releaseBindGroups`), so a key never
    /// outlives the views it names. At most kMaxBindGroups.
    std::unordered_map<detail::BindGroupKey, WGPUBindGroup, detail::BindGroupKeyHash> bindGroups;
    /// Bind groups over the host's surface view (a blit source), released after each submit: the
    /// host may release that view and a new one may take its address.
    std::vector<WGPUBindGroup> frameBindGroups;
    static constexpr size_t kMaxBindGroups = 2048;

    // Encoder state of the open render pass.
    WGPURenderPipeline pipeline = nullptr;
    WGPUBindGroup textures = nullptr;
    uint32_t paramOffset = 0xFFFFFFFFu;
    uint32_t stencilRef = 0xFFFFFFFFu;
    int vertexSource = 0;  ///< 0 none, 1 geometry, 2 particle instances

    void resetPassState() {
        pipeline = nullptr;
        textures = nullptr;
        paramOffset = 0xFFFFFFFFu;
        stencilRef = 0xFFFFFFFFu;
        vertexSource = 0;
    }
    void releaseBindGroups() {
        for (auto& kv : bindGroups) {
            wgpuBindGroupRelease(kv.second);
        }
        bindGroups.clear();
        releaseFrameBindGroups();
    }
    void releaseFrameBindGroups() {
        for (WGPUBindGroup bg : frameBindGroups) {
            wgpuBindGroupRelease(bg);
        }
        frameBindGroups.clear();
    }
    /// The bind group of `key`, cached unless `perFrame` (see frameBindGroups).
    WGPUBindGroup bindGroup(WGPUDevice device, const detail::BindGroupKey& key, uint32_t& creates,
                            bool perFrame = false);
};

}  // namespace xt::wgpu

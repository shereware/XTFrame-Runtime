#pragma once

#include <webgpu/webgpu.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xt/gpu/gpu_device.h"
#include "xt/gpu/gpu_frame.h"
#include "xt/wgpu/wgpu_device.h"
#include "xt/wgpu/wgpu_shared.h"

/// xt_wgpu internals (T-438, Docs/design/10-backends.md §10.10.2): the pipeline cache, the shader
/// table and the geometry/uniform buffers behind `WgpuDevice::submit`. Not installed.

namespace xt::wgpu::detail {

inline WGPUStringView label(const char* s) { return WGPUStringView{s, s != nullptr ? WGPU_STRLEN : 0}; }

/// gl_device.cpp kTransientIdleFrames (T-163): a pooled transient idle this long goes, at most one per
/// frame. T-675: on a WgpuSharedCache the frames are the target's (WgpuSharedCache::endFrame).
inline constexpr uint32_t kTransientIdleFrames = 1800;
/// T-678 (50 §50.ac addendum item 1): the transient pool's byte budget is the most bytes of transients
/// borrowed at once in the last frame plus this much (so never below it): on a WgpuSharedCache the last
/// target frame's peak over its submits (set by WgpuSharedCache::endFrame), else the last submit's.
inline constexpr uint64_t kPoolBudgetHeadroomBytes = 16ull * 1024u * 1024u;
/// gl_device.cpp kCachedFreeBudgetBytes (released cached targets kept for reuse).
inline constexpr uint64_t kCachedFreeBudgetBytes = 16ull * 1024u * 1024u;
/// T-675 (50 §50.ac item 2): an idle surface texture of a WgpuSharedCache's pool, and an unborrowed
/// surface attachment set, go after this many target frames without use.
inline constexpr uint32_t kSurfacePoolIdleFrames = 300;

/// A 2D texture (one mip level; T-665: `levels` of them) and its default view; release* destroys and
/// releases both (T-675: shared by WgpuDevice and WgpuSharedCache).
WGPUTexture makeTexture(WGPUDevice device, const char* name, uint32_t w, uint32_t h, WGPUTextureFormat format,
                        uint32_t samples, WGPUTextureUsage usage, uint32_t levels = 1);
WGPUTextureView makeView(WGPUTexture t);
void releaseTexture(WGPUTexture& t, WGPUTextureView& v);

WGPUTextureFormat formatOf(gpu::TextureFormat fmt);
/// T-665 (wgpu_mips.cpp): whether `fmt` is block-compressed, and its bytes per texel or per 4x4 block.
bool isCompressedFormat(gpu::TextureFormat fmt);
uint32_t formatUnitBytes(gpu::TextureFormat fmt);

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
    kShTextureLod,  ///< T-665: Texture / TextureStraight with DrawCmd::mipLod
    kShGlyph,
    kShComposite,
    kShParticleTexture,
    kShParticleSolid,
    kShCoverageSolid,     ///< T-660b: Solid with DrawCmd::coverage
    kShCoverageGradient,  ///< T-660b: Gradient with DrawCmd::coverage
    kShBoxShadow,         ///< T-668: Shading::BoxShadow (T-683: one shape)
    kShBoxShadowMulti,    ///< T-683: Shading::BoxShadow with DrawCmd::boxShadowMulti (up to three shapes)
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
    /// T-665: the mip box step (blit.wgsl fs_mip_box, the blit layout) into an RGBA8Unorm level.
    WGPURenderPipeline mipBox();

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

    /// T-683 (Docs/design/50-c-abi.md §50.ad item 2): one representative render pipeline of every
    /// built-in program (kShaderCount of them; `.xtfx` custom programs are not known yet) plus the blit
    /// and the mip box step, so that no later draw waits for a shader compile -- wgpu keeps a compiled
    /// shader on its device, and another blend / stencil / sample count of the same program then costs
    /// only the pipeline. The content programs at `format` x `samples` with a stencil attachment,
    /// SrcOver and TestEqualRef (what nearly every draw of a surface or layer uses; the particle
    /// programs as DrawKind::Particles); Composite likewise with no stencil test (a Normal layer
    /// composite); the effect programs at RGBA8Unorm, one sample, no stencil, Replace (an effect pass
    /// into a transient); the blit at `format` x `samples` with stencil. Keys already built are not
    /// built again. Returns false when one of them failed (named by `takeError`, as a draw's would be).
    bool prewarm(WGPUTextureFormat format, uint32_t samples);
    /// T-683: shader compiles (the first pipeline of a program and vertex stage of this set, a T-456
    /// probe each time) and pipeline creates, counted apart while `prewarm` runs and after it.
    struct Counters {
        uint32_t prewarmShaderCompiles = 0;   ///< during the prewarms of this set (summed)
        uint32_t prewarmPipelineCreates = 0;  ///< likewise
        uint32_t shaderCompiles = 0;          ///< since the last prewarm ended (since init without one)
        uint32_t pipelineCreates = 0;         ///< likewise
    };
    const Counters& counters() const { return m_counters; }

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
    Counters m_counters;               ///< T-683
    bool m_prewarming = false;         ///< T-683: inside prewarm()
    std::vector<uint32_t> m_compiled;  ///< T-683: (shader id << 2 | vertex stage) of every pipeline made

    /// T-683: counts one pipeline create of `shader` with vertex stage `stage` (0 vs_main, 1 vs_quad,
    /// 2 vs_particle, 3 the custom GLSL stage) and, the first time that pair is made, a shader compile.
    void noteCreate(uint16_t shader, uint32_t stage);

    /// blit() and mipBox() (`mip`): the blit module's pipelines.
    WGPURenderPipeline blitModulePipeline(WGPUTextureFormat format, uint32_t samples, bool hasStencil,
                                          bool mip);
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

/// The state behind WgpuSharedCache (T-648; wgpu_shared.cpp, wgpu_device.cpp).
struct WgpuSharedCache::Impl {
    /// The device the pipeline set and the images were created on (one reference held while
    /// either lives); a WgpuDevice on another WGPUDevice does not share.
    WGPUDevice device = nullptr;
    detail::Pipelines pipelines;
    bool pipelinesReady = false;
    /// (format << 8 | samples) of every probe (T-456) already passed on `device`.
    std::vector<uint64_t> probed;
    struct Image {
        gpu::ImageKey key;
        uint16_t width = 0, height = 0;
        gpu::TextureFormat format = gpu::TextureFormat::Rgba8;
        WGPUTexture texture = nullptr;  ///< null = free slot
        WGPUTextureView view = nullptr;
        uint32_t levels = 1;  ///< T-665: mip levels of `texture` (WgpuDevice::requestMipChain)
        uint32_t uses = 0;    ///< device handles naming it
        uint64_t bytes = 0;
        bool idle = false;  ///< T-656: in `idle` (no use)
    };
    std::vector<Image> images;  ///< id i + 1 = images[i]
    std::vector<uint32_t> freeIds;
    /// T-656: ids of the unused images, oldest (first left unused) first.
    std::vector<uint32_t> idle;
    uint64_t idleBudget = WgpuSharedCache::kDefaultImageIdleBudget;
    WgpuSharedStats stats;

    /// True when a WgpuDevice on `dev` may borrow this cache (it adopts `dev` when it has none).
    bool adopt(WGPUDevice dev);
    /// The id of the live image of (`key`, size, format), or 0.
    uint32_t findImage(const gpu::ImageKey& key, uint16_t w, uint16_t h, gpu::TextureFormat fmt) const;
    /// Takes ownership of `texture` / `view` as the image of `key` with one use; returns its id.
    uint32_t addImage(const gpu::ImageKey& key, uint16_t w, uint16_t h, gpu::TextureFormat fmt,
                      WGPUTexture texture, WGPUTextureView view, uint64_t bytes);
    /// T-656: one more use of the live image `id` (found by findImage); takes it off the idle list.
    void useImage(uint32_t id);
    /// Ends one use of image `id` (see gpu::GpuDevice::releaseSharedImage for `keep`). T-656: with
    /// `keep`, an image left unused goes to the idle list (then the budget is applied).
    void releaseImage(uint32_t id, bool keep);
    void destroyImage(Image& image);
    /// T-656: takes image `id` off the idle list (no-op when it is not on it).
    void leaveIdle(uint32_t id);
    /// T-656: destroys idle image `id` (counted as an eviction).
    void evictIdle(uint32_t id);
    /// T-656: evicts the oldest idle images while the idle bytes exceed `idleBudget`.
    void applyIdleBudget();
    /// Releases the device reference once nothing created on it is left.
    void releaseDeviceIfUnused();

    // -- T-675 (Docs/design/50-c-abi.md §50.ac item 2): the target-level pools the devices borrow
    // from. Only the devices of one target use them, one at a time (50 §50.1), so a pooled texture is
    // either idle here or used by the one device drawing now. --

    /// Transient targets of every device (WgpuDevice::acquirePooled); slots keep their index.
    std::vector<WgpuDevice::Pooled> transients;
    /// T-678 (50 §50.ac addendum item 1): the most bytes of transients one submit of the current
    /// target frame borrowed (WgpuDevice::submit; the devices submit one after another, so this is
    /// the most borrowed at once) ...
    uint64_t frameBorrowPeak = 0;
    /// ... and the budget endFrame made of the frame that ended: frameBorrowPeak +
    /// kPoolBudgetHeadroomBytes. LRU entries over it go then (and after each submit, as before).
    uint64_t transientBudget = detail::kPoolBudgetHeadroomBytes;
    /// T-678 (50 §50.ac addendum item 2): bytes per texel of a Stencil8 texture as the backend of the
    /// cache's device creates it (stencil8TexelBytes; set by the first device that borrows it).
    uint32_t stencilTexelBytes = 1;
    /// Released effect-cache / band targets of every device, reused by exact size.
    std::vector<WgpuDevice::FreeCached> cachedFree;
    uint64_t cachedFreeBytes = 0;
    /// Surface attachments (a 4x colour attachment when samples > 1, and a Stencil8 of the same
    /// sample count), one set per (size, samples, format), borrowed by the device drawing its
    /// surface for one submit.
    struct AttachmentSet {
        uint32_t width = 0, height = 0, samples = 1;
        WGPUTextureFormat format = WGPUTextureFormat_Undefined;
        WGPUTexture msaa = nullptr;
        WGPUTextureView msaaView = nullptr;
        WGPUTexture stencil = nullptr;  ///< null = empty slot
        WGPUTextureView stencilView = nullptr;
        bool borrowed = false;
        uint32_t lastUsed = 0;
        uint64_t bytes = 0;
    };
    std::vector<AttachmentSet> attachments;
    /// Idle single-sample surface textures (WgpuSharedCache::releaseSurfaceTexture).
    struct SurfaceTexture {
        uint32_t width = 0, height = 0;
        WGPUTextureFormat format = WGPUTextureFormat_Undefined;
        WGPUTexture texture = nullptr;
        WGPUTextureView view = nullptr;
        uint32_t idleSince = 0;
        uint64_t bytes = 0;
    };
    std::vector<SurfaceTexture> surfaceIdle;
    /// The 1x1 transparent texture every device binds where a command has no texture.
    WGPUTexture dummy = nullptr;
    WGPUTextureView dummyView = nullptr;
    /// Target frames (WgpuSharedCache::endFrame): the clock of every pool above.
    uint32_t frame = 0;
    /// Bumped whenever a pooled view that a device's bind groups may name (a transient, a cached
    /// target) is destroyed; each device flushes its bind groups before its next submit when it
    /// differs from the value it saw (WgpuDevice::m_seenViewGeneration).
    uint64_t viewGeneration = 0;

    /// Creates the dummy on `device` once (uploaded through `queue`). False when it cannot be made.
    bool ensureDummy(WGPUQueue queue);
    /// An unborrowed set of the key, made when there is none; its index, or -1 when it cannot be made.
    int borrowAttachments(uint32_t width, uint32_t height, uint32_t samples, WGPUTextureFormat format);
    void returnAttachments(int index);
    /// Destroys pooled transient `p` (leaves an empty slot).
    void destroyTransient(WgpuDevice::Pooled& p);
    /// Destroys the oldest released cached targets while they exceed kCachedFreeBudgetBytes.
    void trimCachedFree();
    /// endFrame: sets the transient budget from the frame that ended and applies it
    /// (applyTransientBudget), then ages every pool (one more target frame first).
    void agePools();
    /// T-678: transientBudget = frameBorrowPeak + kPoolBudgetHeadroomBytes (the peak starts again
    /// at 0), then the least recently used transients go while the pool is over it -- never one used
    /// in the frame that ended, and never the last one (WgpuDevice::trimPool's rule).
    void applyTransientBudget();
    /// Destroys every pooled texture no device borrows now; returns how many textures.
    uint32_t destroyPools();
    /// Fills the T-675 pool fields of `out`.
    void poolStats(WgpuSharedStats& out) const;
};

/// WgpuDevice's GPU-side state (wgpu_device.cpp, wgpu_draw.cpp).
struct WgpuDevice::Gpu {
    /// The device's own pipeline set, unused while `pipelines` points into a WgpuSharedCache (T-648).
    detail::Pipelines ownPipelines;
    detail::Pipelines* pipelines = &ownPipelines;
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
    /// T-670 (50 §50.ab addendum 1 iii): live non-sampler bindings (texture views and buffers) of
    /// passGroup + paramGroup, of bindGroups and of frameBindGroups; the peak since init, and whether
    /// the one-time warning past kBindingsWarnThreshold (wgpu_util.h) was printed.
    uint32_t uniformBindings = 0;
    uint32_t cachedBindings = 0;
    uint32_t frameBindings = 0;
    uint32_t peakBindings = 0;
    bool bindingsWarned = false;
    uint32_t liveBindings() const { return uniformBindings + cachedBindings + frameBindings; }
    /// After a create: updates the peak and warns once per device past the threshold (wgpu_device.cpp).
    void noteBindings();

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
        cachedBindings = 0;
        releaseFrameBindGroups();
    }
    void releaseFrameBindGroups() {
        for (WGPUBindGroup bg : frameBindGroups) {
            wgpuBindGroupRelease(bg);
        }
        frameBindGroups.clear();
        frameBindings = 0;
    }
    /// The bind group of `key`, cached unless `perFrame` (see frameBindGroups).
    WGPUBindGroup bindGroup(WGPUDevice device, const detail::BindGroupKey& key, uint32_t& creates,
                            bool perFrame = false);
};

}  // namespace xt::wgpu

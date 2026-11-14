#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "xt/command_list.h"
#include "xt/gpu/gpu_device.h"
#include "xt/gpu/gpu_frame.h"
#include "xt/ir.h"
#include "xt/perf.h"
#include "xt/types.h"

/// World layers on the GPU (Docs/design/33-world-layers.md §33.3-§33.4, §33.8; Docs/tasks/T-701.md):
/// a CommandList painted in world-layer output (Context::setWorldLayerOutput) becomes one texture
/// per layer plus where each texture sits in the root box, for a host that draws them as quads in
/// its own 3D scene with its own camera.
///
/// `WorldLayerBuilder` keeps, per layer, a FrameBuilder of its own (so each layer has its own
/// effect result cache, coverage atlas and image textures, and "the previous build" always means
/// that layer's), a persistent device target (`GpuDevice::acquireLayerTargetWithStencil`) and the hash of
/// what was last drawn into it. `build` compares every layer's commands with that hash and builds a frame
/// only for the layers that changed; `submit` draws those frames (`GpuDevice::submitLayer`). A layer whose
/// commands, rectangle and density are what they were is neither rebuilt nor drawn: its target is not touched
/// and `WorldLayer::changed` is 0. Moving, turning or fading a layer changes `world3` / `opacity` only, so it
/// costs no drawing at all.

namespace xt {
class TextShaper;
class AssetResolver;
}  // namespace xt

#ifndef XT_GPU_NO_TEXT
namespace xt::text {
class GlyphAtlas;
}  // namespace xt::text
#endif

namespace xt::gpu {

/// One layer as the host gets it (33 §33.3). The quad: the four corners of `rect` (z = 0) through
/// `world3` are its corners in root-box coordinates (logical px; x right, y down, +z toward the
/// design-time viewer; the root box is the viewport); `texture`'s (0, 0) is `rect`'s top-left.
struct WorldLayer {
    /// The layer's element (its NodeId, stable across frames); 0 = the base layer.
    uint32_t id = 0;
    /// Paint order within its Document (0 = the base layer): the tie-break for coplanar layers.
    uint32_t order = 0;
    /// Layer plane -> root box.
    Mat4 world3 = mat4Identity();
    /// What the texture covers on the layer plane, logical px.
    Rect rect{};
    /// RGBA8, premultiplied alpha, row 0 = top. `texWidth` x `texHeight` = `rect` x `density`,
    /// rounded up.
    TextureHandle texture = 0;
    uint16_t texWidth = 0, texHeight = 0;
    /// CSS `backface-visibility: hidden`.
    uint8_t backfaceHidden = 0;
    /// 1 = this build drew the texture again (the host remakes its mips); 0 = untouched.
    uint8_t changed = 0;
    /// T-703 (33 §33.13 item 4): 1 = the layer's frame uses the stencil, so its target was acquired
    /// with a stencil attachment (`GpuDevice::acquireLayerTargetWithStencil`); 0 = without.
    uint8_t stencil = 0;
    /// Which Document of the list (CommandList::sources index) the layer belongs to.
    uint16_t source = 0;
    /// The opacity the host draws the whole layer with (33 §33.5 item 3).
    float opacity = 1.0f;
    /// Texels per logical px the texture was drawn at (the builder's density, lowered for a layer
    /// that would exceed the device's texture size).
    float density = 1.0f;
};

class WorldLayerBuilder {
public:
    /// T-703 (33 §33.13 item 3): what the last `build` came to, for a host to act on without
    /// reading `lastError` (whose text stays as it was).
    enum class BuildStatus : uint8_t {
        Ok = 0,          ///< built (`build` returned true)
        NotInitialized,  ///< `init` was not called
        NoLayers,        ///< the list has no layers (it was not painted in world-layer output)
        /// The device has no layer targets: `GpuDevice::acquireLayerTargetWithStencil` returned 0
        /// (the default device, or one that cannot allocate the target). The host draws the UI
        /// flat, with world-layer output off.
        Unsupported,
    };

    WorldLayerBuilder();
    ~WorldLayerBuilder();
    WorldLayerBuilder(const WorldLayerBuilder&) = delete;
    WorldLayerBuilder& operator=(const WorldLayerBuilder&) = delete;

    /// Binds the device the layers' targets and textures are created on. Must precede `build`.
    void init(GpuDevice& device);
    /// Releases every layer target and texture. Safe to call repeatedly.
    void shutdown();

#ifndef XT_GPU_NO_TEXT
    /// The glyph atlas the layers' text samples from. Not owned. It must be an atlas no other
    /// FrameBuilder uses (the layers share one set of device textures for it; see
    /// gpu::GlyphAtlasTextures). Null (the default): every glyph is drawn from its outline.
    void setGlyphAtlas(xt::text::GlyphAtlas* atlas);
#endif
    /// As FrameBuilder::setEdgeFringe (on by default: layer targets are single-sampled).
    void setEdgeFringe(bool enabled);
    /// Texels per logical px of the layers' textures (33 §33.4 item 3). 0 (the default): the
    /// list's own `devicePixelRatio`. A change redraws every layer.
    void setDensity(float density);
    float density() const;

    /// Builds the frames of the layers of `list` (painted in world-layer output) that changed
    /// since they were last drawn; `shaper`, `assets`, `ir` and `perf` as FrameBuilder::build.
    /// Layers that are gone give their target back. False when nothing could be built: `list` has
    /// no layers (it was not painted in world-layer output), `init` was not called, or the device
    /// has no layer targets (`GpuDevice::acquireLayerTargetWithStencil` returned 0) -- `status`
    /// says which (and `lastError` in words); on `BuildStatus::Unsupported` the host draws the UI
    /// flat, with world-layer output off. A layer's target is acquired once its frame is built,
    /// with a stencil attachment only when that frame uses the stencil (33 §33.13 item 4).
    bool build(const CommandList& list, TextShaper* shaper, AssetResolver* assets, const IrView* ir,
               PerfTracker* perf = nullptr);
    /// What the last `build` came to (`BuildStatus::Ok` before the first).
    BuildStatus status() const;
    /// Draws the frames the last `build` made into their layers' targets
    /// (`GpuDevice::submitLayer`), once; nothing when no layer changed.
    void submit();

    /// The layers of the last `build`, in paint order (the list's order). A layer without anything
    /// to draw (an empty base layer) is left out.
    std::span<const WorldLayer> layers() const;

    /// What the last `build` / `submit` did.
    struct Stats {
        uint32_t layers = 0;          ///< layers of the list that draw something
        uint32_t built = 0;           ///< of which got a new frame (changed)
        uint32_t submitted = 0;       ///< targets the last submit() drew into
        uint32_t drawCommands = 0;    ///< draw commands of the frames built
        uint32_t targetsCreated = 0;  ///< layer targets acquired by the build
        uint32_t stencilLayers = 0;   ///< layers whose target has a stencil attachment (T-703)
        uint64_t targetBytes = 0;     ///< the layer targets alive, 4 bytes per texel
    };
    Stats stats() const;
    /// Layer targets drawn into since `init` (every `submit` adds its `Stats::submitted`).
    uint64_t totalSubmitted() const;

    /// True when a layer was drawn with a coverage shape held back for the frame's rasterization
    /// budget (FrameBuilder::wantsAnotherFrame): the next `build` draws that layer again.
    bool wantsAnotherFrame() const;
    /// Forgets what every layer drew (the next `build` redraws them all) and drops the layers'
    /// cached image textures, as FrameBuilder::invalidateAllAssets.
    void invalidateAll();
    /// Empty string when the last `build` had nothing to report.
    const char* lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// A frame that draws `layers` as textured quads into target 0 -- the smallest possible host: a
/// viewer's or an editor's preview of the layers standing in space (33 §33.9), and, with an
/// orthographic `boxToSurface`, the layers put back on top of one another. `boxToSurface` maps a
/// root-box point (x, y, z, 1) to homogeneous surface pixels: (X, Y, _, W) with the pixel at
/// (X / W, Y / W). The quads are drawn from the farthest (largest W at the quad's centre) to the
/// nearest, equal depths in paint order, each with its layer's opacity; a layer with a corner at
/// W <= 0 (behind the eye), without texture, or `backfaceHidden` and seen from behind is left out.
/// No depth buffer: quads that cross each other are not split. `clear`: the pass clears the
/// surface to transparent first. `out` is cleared first.
void buildWorldLayerQuads(std::span<const WorldLayer> layers, const Mat4& boxToSurface, int surfaceWidth,
                          int surfaceHeight, bool clear, GpuFrame& out);

}  // namespace xt::gpu
